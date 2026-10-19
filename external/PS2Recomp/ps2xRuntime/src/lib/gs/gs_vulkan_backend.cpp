// SPDX-License-Identifier: GPL-3.0-only
#include "runtime/gs/gs_vulkan_backend.h"
#include "gs_vulkan_scanout.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "gs_interface.hpp"
#include "context.hpp"
#include "thread_id.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>

namespace
{
constexpr uint32_t vramBytes = 4u << 20;
using Reg = ParallelGS::RegisterAddr;
uint64_t tex0Bits(const GSTex0Reg &t, bool load)
{
    return t.tbp0 | (uint64_t(t.tbw) << 14) | (uint64_t(t.psm) << 20) |
        (uint64_t(t.tw) << 26) | (uint64_t(t.th) << 30) | (uint64_t(t.tcc) << 34) |
        (uint64_t(t.tfx) << 35) | (uint64_t(t.cbp) << 37) | (uint64_t(t.cpsm) << 51) |
        (uint64_t(t.csm) << 55) | (uint64_t(t.csa) << 56) | (uint64_t(load ? t.cld : 0) << 61);
}
uint64_t texclutBits(const GSTexClutReg &t)
{
    return t.cbw | (uint64_t(t.cou) << 6) | (uint64_t(t.cov) << 12);
}
uint32_t transferBits(uint8_t psm)
{
    return uint32_t(GSMem::BitsPerPixel(GSMem::PixelStorageMode(psm)));
}
}

struct GSVulkanBackend::Impl
{
    // Declaration order ensures interface dies before device before context.
    Vulkan::Context context;
    Vulkan::Device device;
    ParallelGS::GSInterface gs;
    mutable std::mutex mutex;
    std::vector<uint8_t> snapshot = std::vector<uint8_t>(vramBytes);
    GSCpuBackend scanout;
    ReusableFieldWeaver fieldWeaver;
    VulkanGsScanoutState scanoutState;
    std::array<uint64_t, 128> state{};
    std::array<bool, 128> valid{};
    GSTransferCommand transfer{};
    GSTransferSnapshot transferState{};
    uint64_t uploadedBytes = 0;
    std::vector<uint8_t> uploadTail;
    std::vector<uint8_t> fifo;
    size_t fifoRead = 0;
    bool initialized = false;
    bool dirty = false;
    uint64_t draws = 0;
    uint64_t gpuPresentations = 0, convertedPresentations = 0;
    std::atomic<uint32_t> currentScale{1};
    std::atomic<uint32_t> aaSamples{1};
    // The player's anti-aliasing choice (F5): samples per output pixel, on top
    // of the samples the output scale needs to build the larger image.
    uint32_t requestedSamples = 1;
    uint32_t maxAASamples() const
    {
        // Same device capability predicate used by the pinned renderer.
        if (device.supports_subgroup_size_log2(true, 4, 6)) return 16;
        if (device.supports_subgroup_size_log2(true, 3, 6)) return 8;
        return 4;
    }
    // 2x output reads a 2x2 sample grid, 4x output a 4x4 grid.
    static uint32_t minSamplesFor(uint32_t scale) { return scale == 4u ? 16u : scale == 2u ? 4u : 1u; }
    uint32_t supportedScale(uint32_t scale) const
    {
        if (scale == 4u && maxAASamples() >= 16u) return 4u;
        return scale >= 2u ? 2u : 1u;
    }
    uint32_t effectiveSamples(uint32_t scale) const
    {
        return std::min(requestedSamples * minSamplesFor(scale), maxAASamples());
    }
    void flushIfDirty()
    {
        if (dirty)
        {
            gs.flush();
            dirty = false;
        }
    }
    // Caller holds access(). Retire pending readbacks before resetting
    // geometry/history; slots own image and fence until the copy completes.
    void applyOutput(uint32_t scale)
    {
        const uint32_t samples = effectiveSamples(scale);
        if (scale == currentScale.load(std::memory_order_relaxed) &&
            samples == aaSamples.load(std::memory_order_relaxed))
            return;
        gs.flush();
        dirty = false;
        for (auto &slot : scanoutState.slots)
            if (slot.pending && slot.fence) slot.fence->wait();
        scanoutState.reset();
        fieldWeaver.reset();
        gs.set_super_sampling_rate(static_cast<ParallelGS::SuperSampling>(samples), true, true);
        aaSamples.store(samples, std::memory_order_relaxed);
        currentScale.store(scale, std::memory_order_relaxed);
        std::fprintf(stderr, "[gs:vulkan] Output=%ux AA=%ux (%u samples)\n", scale, samples / minSamplesFor(scale), samples);
    }

    Impl()
    {
        if (!Vulkan::Context::init_loader(nullptr))
            throw std::runtime_error("Vulkan loader is unavailable");
        context.set_num_thread_indices(1);
        if (!context.init_instance_and_device(nullptr, 0, nullptr, 0,
            Vulkan::CONTEXT_CREATION_ENABLE_PUSH_DESCRIPTOR_BIT |
            Vulkan::CONTEXT_CREATION_ENABLE_DESCRIPTOR_HEAP_BIT |
            Vulkan::CONTEXT_CREATION_ENABLE_DESCRIPTOR_BUFFER_BIT))
            throw std::runtime_error("Vulkan device creation failed");
        device.set_context(context);
        // Keep enough Granite PerFrame contexts that a multi-flush field + 3-slot
        // async readback ring never wraps and blocks inside per_frame->begin() while
        // holding the GS device mutex.
        device.init_frame_contexts(16);

        ParallelGS::GSOptions opts{};
        opts.dynamic_super_sampling = true;
        const char *scaleEnv = std::getenv("PS2X_GS_SCALE");
        if (const char *aa = std::getenv("PS2X_GS_SSAA"))
        {
            const uint32_t requested = std::strtoul(aa, nullptr, 10);
            if (requested == 1 || requested == 2 || requested == 4 || requested == 8 || requested == 16)
                requestedSamples = requested;
        }
        const uint32_t outputScale = supportedScale(scaleEnv ? std::strtoul(scaleEnv, nullptr, 10) : 1u);
        currentScale.store(outputScale, std::memory_order_relaxed);
        const uint32_t samples = effectiveSamples(outputScale);
        opts.super_sampling = static_cast<ParallelGS::SuperSampling>(samples);
        // Preserve supported sample-aware texture copies, including the game's
        // render-to-display blit; no backbuffer-promotion heuristic is enabled.
        opts.super_sampled_textures = true;
        aaSamples.store(samples, std::memory_order_relaxed);

        if (!gs.init(&device, opts))
            throw std::runtime_error("Vulkan device lacks paraLLEl-GS features");
        scanout.Initialize(snapshot.data(), vramBytes);
        const auto &gpu = device.get_gpu_properties();
        if (gpu.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
            throw std::runtime_error("software Vulkan devices are not a GPU renderer");
        std::fprintf(stderr, "[gs:vulkan] device=%s vendor=0x%04x device=0x%04x; GPU compute rasterization (AA=%u samples)\n",
            gpu.deviceName, gpu.vendorID, gpu.deviceID, static_cast<unsigned>(opts.super_sampling));
    }
    ~Impl()
    {
        Util::register_thread_index(0);
        gs.flush(); device.wait_idle();
        const char *stats = std::getenv("PS2X_STATS");
        if (stats && *stats && *stats != '0')
            std::fprintf(stderr, "[gs:vulkan] draws=%llu GPU scanouts=%llu CPU display conversions=%llu\n",
                static_cast<unsigned long long>(draws), static_cast<unsigned long long>(gpuPresentations),
                static_cast<unsigned long long>(convertedPresentations));
    }

    std::unique_lock<std::mutex> access()
    {
        // EE, VU and presentation callers share this serialized device slot.
        // Granite requires a thread-local index even with one command pool.
        std::unique_lock lock(mutex);
        Util::register_thread_index(0);
        return lock;
    }

    void reg(Reg id, uint64_t bits, bool force = false)
    {
        const auto index = static_cast<unsigned>(id);
        if (force || !valid[index] || state[index] != bits)
        {
            gs.write_register(id, bits);
            state[index] = bits;
            valid[index] = true;
            dirty = true;
        }
    }
    void readSnapshot()
    {
        const void *mapped = gs.map_vram_read(0, vramBytes);
        if (!mapped) throw std::runtime_error("Vulkan GS VRAM readback failed");
        std::memcpy(snapshot.data(), mapped, vramBytes);
        dirty = false;
    }
    void submit(const GSPrimitiveBatch &b)
    {
        const auto &s = b.state; const auto &c = s.context;
        unsigned type = s.prim.type;
        if (type == GS_PRIM_TRISTRIP || type == GS_PRIM_TRIFAN) type = GS_PRIM_TRIANGLE;
        if (type == GS_PRIM_LINESTRIP) type = GS_PRIM_LINE;
        reg(Reg::PRMODECONT, 1);
        reg(Reg::FRAME_1, c.frame.fbp | (uint64_t(c.frame.fbw) << 16) |
            (uint64_t(c.frame.psm) << 24) | (uint64_t(c.frame.fbmsk) << 32));
        reg(Reg::ZBUF_1, c.zbuf.zbp | (uint64_t(c.zbuf.psm & 15) << 24) | (uint64_t(c.zbuf.zmask) << 32));
        reg(Reg::SCISSOR_1, c.scissor.x0 | (uint64_t(c.scissor.x1) << 16) |
            (uint64_t(c.scissor.y0) << 32) | (uint64_t(c.scissor.y1) << 48));
        reg(Reg::XYOFFSET_1, c.xyoffset.ofx | (uint64_t(c.xyoffset.ofy) << 32));
        // Palette events already arrive separately via LoadClut.
        const bool autoMip = (c.tex1 & (1ull << 9)) != 0;
        const bool enteringAutoMip = autoMip && (!valid[unsigned(Reg::TEX1_1)] || !(state[unsigned(Reg::TEX1_1)] & (1ull << 9)));
        reg(Reg::TEX1_1, c.tex1);
        reg(Reg::TEX0_1, tex0Bits(c.tex0, false), enteringAutoMip);
        reg(Reg::CLAMP_1, c.clamp);
        // The current frontend stores explicit MIPTBP writes, but does not
        // materialize MTBA-generated values. Preserve the library's generated
        // mip addresses rather than overwriting them with an absent zero value.
        if (!autoMip || c.miptbp1 != 0) reg(Reg::MIPTBP1_1, c.miptbp1);
        reg(Reg::MIPTBP2_1, c.miptbp2);
        reg(Reg::ALPHA_1, c.alpha); reg(Reg::TEST_1, c.test); reg(Reg::FBA_1, c.fba);
        reg(Reg::TEXCLUT, texclutBits(s.texclut));
        reg(Reg::TEXA, s.texa.ta0 | (uint64_t(s.texa.aem) << 15) | (uint64_t(s.texa.ta1) << 32));
        reg(Reg::FOGCOL, s.fogR | (uint64_t(s.fogG) << 8) | (uint64_t(s.fogB) << 16));
        reg(Reg::PABE, s.pabe); reg(Reg::SCANMSK, s.scanmsk);
        reg(Reg::DIMX, s.dimx); reg(Reg::DTHE, s.dthe); reg(Reg::COLCLAMP, s.colclamp);
        const auto &p = s.prim;
        // Independent frontend batches use list primitives in context zero.
        reg(Reg::PRIM, type | (uint64_t(p.iip) << 3) | (uint64_t(p.tme) << 4) |
            (uint64_t(p.fge) << 5) | (uint64_t(p.abe) << 6) | (uint64_t(p.aa1) << 7) |
            (uint64_t(p.fst) << 8) | (uint64_t(p.fix) << 10), true);
        for (unsigned i = 0; i < b.vertexCount && i < b.vertices.size(); ++i)
        {
            const auto &v = b.vertices[i];
            reg(Reg::RGBAQ, v.r | (uint64_t(v.g) << 8) | (uint64_t(v.b) << 16) |
                (uint64_t(v.a) << 24) | (uint64_t(std::bit_cast<uint32_t>(v.q)) << 32), true);
            reg(Reg::ST, std::bit_cast<uint32_t>(v.s) | (uint64_t(std::bit_cast<uint32_t>(v.t)) << 32), true);
            reg(Reg::UV, v.u | (uint64_t(v.v) << 16), true);
            reg(Reg::FOG, uint64_t(v.fog) << 56, true);
            const auto x = uint16_t(int32_t(std::nearbyint(v.x * 16.0f)));
            const auto y = uint16_t(int32_t(std::nearbyint(v.y * 16.0f)));
            reg(Reg::XYZ2, x | (uint64_t(y) << 16) | (uint64_t(uint32_t(v.z)) << 32), true);
        }
        ++draws;
    }

    void imagePacket(const uint8_t *data, size_t count)
    {
        while (count)
        {
            const size_t bytes = std::min<size_t>(count, 0x7fff * 16u);
            const uint64_t qwords = (bytes + 15u) / 16u;
            std::vector<uint8_t> packet(size_t(qwords) * 16u + 16u);
            const uint64_t tag = qwords | (1ull << 15) | (2ull << 58);
            std::memcpy(packet.data(), &tag, 8);
            std::memcpy(packet.data() + 16, data, bytes);
            gs.gif_transfer(2, packet.data(), packet.size());
            dirty = true;
            count -= bytes; data += bytes;
        }
    }
};

GSVulkanBackend::GSVulkanBackend() : m_impl(std::make_unique<Impl>()) {}
GSVulkanBackend::~GSVulkanBackend() = default;
void GSVulkanBackend::Initialize(uint8_t *vram, uint32_t size)
{
    if (!vram || size < vramBytes) throw std::invalid_argument("Vulkan GS needs 4 MiB VRAM");
    auto lock = m_impl->access();
    auto *dst = m_impl->gs.map_vram_write(0, vramBytes);
    if (!dst) throw std::runtime_error("Vulkan GS initial VRAM mapping failed");
    std::memcpy(dst, vram, vramBytes);
    m_impl->gs.end_vram_write(0, vramBytes);
    m_impl->gs.reset_context_state(); m_impl->valid.fill(false);
    m_impl->transfer = {}; m_impl->uploadedBytes = 0;
    m_impl->uploadTail.clear(); m_impl->fifo.clear(); m_impl->fifoRead = 0;
    m_impl->transferState = {}; m_impl->transferState.direction = 3;
    m_impl->dirty = true;
    m_impl->initialized = true;
}
void GSVulkanBackend::Reset()
{
    auto lock = m_impl->access();
    m_impl->gs.flush(); m_impl->gs.reset_context_state(); m_impl->valid.fill(false);
    m_impl->dirty = false;
    m_impl->scanoutState.reset();
    m_impl->transferState = {}; m_impl->transferState.direction = 3;
    m_impl->fifo.clear(); m_impl->fifoRead = 0; m_impl->uploadTail.clear();
}
void GSVulkanBackend::Submit(const GSPrimitiveBatch &b) { auto lock = m_impl->access(); m_impl->submit(b); }
void GSVulkanBackend::LoadClut(const GSTex0Reg &t, const GSTexClutReg &c)
{
    auto lock = m_impl->access();
    m_impl->reg(Reg::TEXCLUT, texclutBits(c)); m_impl->reg(Reg::TEX0_1, tex0Bits(t, true), true);
}
void GSVulkanBackend::BeginTransfer(const GSTransferCommand &c)
{
    auto lock = m_impl->access();
    auto &p = *m_impl;
    p.transfer = c; p.transferState = {}; p.transferState.direction = c.direction;
    p.transferState.totalPixels = uint32_t(c.trxreg.rrw) * c.trxreg.rrh;
    p.transferState.x = c.trxpos.dsax; p.transferState.y = c.trxpos.dsay;
    p.uploadedBytes = 0; p.uploadTail.clear(); p.fifo.clear(); p.fifoRead = 0;
    const auto &b = c.bitbltbuf; const auto &pos = c.trxpos;
    p.reg(Reg::BITBLTBUF, b.sbp | (uint64_t(b.sbw) << 16) | (uint64_t(b.spsm) << 24) |
        (uint64_t(b.dbp) << 32) | (uint64_t(b.dbw) << 48) | (uint64_t(b.dpsm) << 56), true);
    p.reg(Reg::TRXPOS, pos.ssax | (uint64_t(pos.ssay) << 16) | (uint64_t(pos.dsax) << 32) |
        (uint64_t(pos.dsay) << 48) | (uint64_t(pos.dir) << 59), true);
    p.reg(Reg::TRXREG, c.trxreg.rrw | (uint64_t(c.trxreg.rrh) << 32), true);
    p.reg(Reg::TRXDIR, c.direction, true);
    if (c.direction == 1)
    {
        const size_t bytes = (size_t(p.transferState.totalPixels) * transferBits(b.spsm) + 7u) / 8u;
        p.fifo.resize((bytes + 15u) & ~size_t(15u));
        if (!p.fifo.empty()) p.gs.read_transfer_fifo(p.fifo.data(), uint32_t(p.fifo.size() / 16));
        p.fifo.resize(bytes); p.transferState.localToHostPendingBytes = bytes;
        p.transferState.copiedPixels = p.transferState.totalPixels;
        p.transferState.direction = 3;
    }
    else if (c.direction == 2) { p.transferState.copiedPixels = p.transferState.totalPixels; p.transferState.direction = 3; }
}
void GSVulkanBackend::UploadImage(const uint8_t *data, uint32_t bytes)
{
    if (!data || !bytes) return;
    auto lock = m_impl->access(); auto &p = *m_impl;
    if (p.transferState.direction != 0 || !p.transferState.totalPixels) return;
    const uint32_t bits = transferBits(p.transfer.bitbltbuf.dpsm);
    if (!bits || !p.transfer.trxreg.rrw) return;
    p.uploadTail.insert(p.uploadTail.end(), data, data + bytes); p.uploadedBytes += bytes;
    p.transferState.copiedPixels = uint32_t(std::min<uint64_t>(p.transferState.totalPixels, p.uploadedBytes * 8u / bits));
    const bool complete = p.transferState.copiedPixels == p.transferState.totalPixels;
    const size_t emit = complete ? p.uploadTail.size() : p.uploadTail.size() & ~size_t(15);
    if (emit) { p.imagePacket(p.uploadTail.data(), emit); p.uploadTail.erase(p.uploadTail.begin(), p.uploadTail.begin() + emit); }
    const uint32_t position = complete ? ((p.transferState.copiedPixels - 1u) / (bits == 4 ? 2u : 1u)) * (bits == 4 ? 2u : 1u) : p.transferState.copiedPixels;
    p.transferState.x = p.transfer.trxpos.dsax + position % p.transfer.trxreg.rrw;
    p.transferState.y = p.transfer.trxpos.dsay + position / p.transfer.trxreg.rrw;
    if (complete) { p.transferState.direction = 3; p.transferState.totalPixels = 0; }
}
void GSVulkanBackend::Flush() { auto lock = m_impl->access(); m_impl->flushIfDirty(); }
void GSVulkanBackend::TextureFlush() { auto lock = m_impl->access(); m_impl->reg(Reg::TEXFLUSH, 0, true); }
void GSVulkanBackend::Sync(GSSyncReason reason)
{
    auto lock = m_impl->access(); m_impl->flushIfDirty();
    if (reason == GSSyncReason::Reset) m_impl->device.wait_idle();
}
void GSVulkanBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    auto lock = m_impl->access(); m_impl->readSnapshot(); out = m_impl->snapshot;
}
uint32_t GSVulkanBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    auto lock = m_impl->access(); m_impl->readSnapshot(); return m_impl->scanout.ReadVram(psm, base, bw, x, y);
}
void GSVulkanBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    auto lock = m_impl->access(); auto &p = *m_impl; p.readSnapshot();
    p.scanout.WriteVram(psm, base, bw, x, y, value);
    auto *mapped = p.gs.map_vram_write(0, vramBytes); std::memcpy(mapped, p.snapshot.data(), vramBytes);
    p.gs.end_vram_write(0, vramBytes);
    p.dirty = true;
}
GSTransferSnapshot GSVulkanBackend::GetTransferSnapshot() const
{
    auto lock = m_impl->access(); return m_impl->transferState;
}
uint32_t GSVulkanBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    if (!dst || !maxBytes) return 0;
    auto lock = m_impl->access(); auto &p = *m_impl;
    const auto count = uint32_t(std::min<size_t>(maxBytes, p.fifo.size() - p.fifoRead));
    if (dst && count) std::memcpy(dst, p.fifo.data() + p.fifoRead, count);
    p.fifoRead += count; p.transferState.localToHostPendingBytes = p.fifo.size() - p.fifoRead; return count;
}
bool GSVulkanBackend::ClearFramebuffer(const GSContext &c, uint32_t rgba)
{
    if (!c.frame.fbw || (c.frame.psm != GS_PSM_CT32 && c.frame.psm != GS_PSM_CT24 &&
        c.frame.psm != GS_PSM_CT16 && c.frame.psm != GS_PSM_CT16S)) return false;
    GSPrimitiveBatch b{}; b.state.context = c;
    b.state.context.test = 0; b.state.context.zbuf.zmask = true; b.state.prim.type = GS_PRIM_SPRITE;
    b.state.colclamp = 1; b.vertexCount = 2;
    b.vertices[0].x = float(c.scissor.x0) + c.xyoffset.ofx / 16.0f;
    b.vertices[0].y = float(c.scissor.y0) + c.xyoffset.ofy / 16.0f;
    b.vertices[1].x = float(c.scissor.x1 + 1u) + c.xyoffset.ofx / 16.0f;
    b.vertices[1].y = float(c.scissor.y1 + 1u) + c.xyoffset.ofy / 16.0f;
    for (auto &v : b.vertices) { v.r = uint8_t(rgba); v.g = uint8_t(rgba >> 8);
        v.b = uint8_t(rgba >> 16); v.a = uint8_t(rgba >> 24); }
    Submit(b); return true;
}
bool GSVulkanBackend::PresentInto(const GSPresentationRequest &request, PresentationFrame &out)
{
    auto lock = m_impl->access(); auto &p = *m_impl;
    bool gpuScanout = false;
    bool updated = false;
    // The previous presentation's copied reads release their buffers here, under the device lock.
    p.scanoutState.retired.clear();
    try
    {
        presentVulkanGs(p.gs, p.device, p.scanoutState, request,
                        p.currentScale.load(std::memory_order_relaxed), p.dirty);
        p.dirty = false;
        gpuScanout = true;
        ++p.gpuPresentations;
    }
    catch (const std::exception &error)
    {
        // Preserve the fixed host viewport contract for display modes outside
        // the bounded GPU scanout bridge (native size). Rasterization remains Vulkan.
        std::fprintf(stderr, "[gs:vulkan] CPU display conversion: %s\n", error.what());
        p.readSnapshot();
        out = p.scanout.PresentFromLocalMemory(request, &p.fieldWeaver);
        updated = static_cast<bool>(out);
        ++p.convertedPresentations;
        p.device.next_frame_context();
    }
    const char *stats = std::getenv("PS2X_STATS");
    if (stats && *stats && *stats != '0' && (p.gpuPresentations + p.convertedPresentations) % 300u == 0)
        std::fprintf(stderr, "[gs:vulkan] draws=%llu GPU scanouts=%llu CPU display conversions=%llu\n",
            static_cast<unsigned long long>(p.draws), static_cast<unsigned long long>(p.gpuPresentations),
            static_cast<unsigned long long>(p.convertedPresentations));
    // Copy the completed readbacks without the device lock: the GS worker can keep drawing meanwhile.
    // Each read's slot buffer is only reused by a later presentation (this thread), after the copy.
    std::vector<VulkanGsScanoutState::StagedRead> reads = std::move(p.scanoutState.staged);
    p.scanoutState.staged.clear();
    lock.unlock();
    if (gpuScanout)
    {
        updated = finishVulkanGsPresentInto(p.device, p.scanoutState, reads, &p.fieldWeaver, out);
    }
    else if (!reads.empty())
    {
        PresentationFrame discarded;
        finishVulkanGsPresentInto(p.device, p.scanoutState, reads, &p.fieldWeaver, discarded);
    }
    lock.lock();
    p.scanoutState.retired = std::move(reads);
    return updated;
}
PresentationFrame GSVulkanBackend::Present(const GSPresentationRequest &request)
{
    auto &p = *m_impl;
    PresentInto(request, p.scanoutState.lastFrame);
    std::lock_guard<std::mutex> lock(p.scanoutState.frameMutex);
    return p.scanoutState.lastFrame;
}

void GSVulkanBackend::SetResolutionScale(uint32_t scale)
{
    auto lock = m_impl->access();
    m_impl->applyOutput(m_impl->supportedScale(scale));
}

uint32_t GSVulkanBackend::GetResolutionScale() const
{
    return m_impl->currentScale.load(std::memory_order_relaxed);
}

void GSVulkanBackend::SetAASamples(uint32_t samples)
{
    if (samples != 1 && samples != 2 && samples != 4 && samples != 8 && samples != 16) return;
    auto lock = m_impl->access();
    auto &p = *m_impl;
    p.requestedSamples = samples;
    p.applyOutput(p.currentScale.load(std::memory_order_relaxed));
}

uint32_t GSVulkanBackend::GetAASamples() const
{
    // Effective AA per output pixel (samples beyond what the output scale needs).
    return std::max(1u, m_impl->aaSamples.load(std::memory_order_relaxed) /
                            Impl::minSamplesFor(m_impl->currentScale.load(std::memory_order_relaxed)));
}
