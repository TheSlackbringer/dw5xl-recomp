// SPDX-License-Identifier: GPL-3.0-only
#include "gs_vulkan_scanout.h"

#include "runtime/gs/field_history.h"
#include "gs_interface.hpp"
#include "device.hpp"
#include "buffer.hpp"
#include "fence.hpp"
#include "image.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>


namespace
{
constexpr unsigned hostWidth = 640, hostHeight = 512;
struct Circuit
{
    bool enabled = false;
    ParallelGS::DISPFBBits framebuffer{};
    ParallelGS::DISPLAYBits display{};
    unsigned width = 0, height = 0;
};

Circuit decodeCircuit(bool enabled, uint64_t framebuffer, uint64_t display, bool frameFields)
{
    Circuit result;
    result.framebuffer = ParallelGS::Reg64<ParallelGS::DISPFBBits>(framebuffer).desc;
    result.display = ParallelGS::Reg64<ParallelGS::DISPLAYBits>(display).desc;
    result.enabled = enabled && result.framebuffer.FBW != 0 && display != 0;
    if (!result.enabled) return result;
    result.width = (static_cast<unsigned>(result.display.DW) + 1u) /
                   (static_cast<unsigned>(result.display.MAGH) + 1u);
    result.height = (static_cast<unsigned>(result.display.DH) + 1u) /
                    (static_cast<unsigned>(result.display.MAGV) + 1u);
    if (frameFields) result.height = (result.height + 1u) / 2u;
    if (result.width < 64 || result.width > hostWidth || result.height < 64 || result.height > hostHeight)
    {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "GPU logical scanout supports 64..%u x 64..%u, got %ux%u",
                      hostWidth, hostHeight, result.width, result.height);
        throw VulkanGsScanoutUnsupported(msg);
    }
    return result;
}

bool noWeave()
{
    const char *value = std::getenv("PS2X_NO_WEAVE");
    return value && *value && *value != '0';
}
}

void presentVulkanGs(ParallelGS::GSInterface &gs, Vulkan::Device &device,
                     VulkanGsScanoutState &state,
                     const GSPresentationRequest &request,
                     uint32_t outputScale,
                     bool flushBeforeVsync)
{
    using namespace ParallelGS;
    using namespace Vulkan;
    static const bool metricsEnabled = [] {
        const char *value = std::getenv("PS2X_GS_FRAME_METRICS");
        return value && *value && *value != '0';
    }();
    auto nowNs = []() -> uint64_t {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    };
    auto &metrics = state.metrics;
    if (metricsEnabled) ++metrics.requests;
    auto pmode = Reg64<PMODEBits>(request.pmode).desc;
    const bool interlaced = (request.smode2 & 1u) != 0;
    const bool frameMode = (request.smode2 & 2u) != 0;
    const bool frameFields = interlaced && frameMode;
    const bool fieldMode = interlaced && !frameMode;
    Circuit first = decodeCircuit(pmode.EN1 != 0, request.dispfb1, request.display1, frameFields);
    Circuit second = decodeCircuit(pmode.EN2 != 0, request.dispfb2, request.display2, frameFields);
    if (!first.enabled && !second.enabled) return;
    pmode.EN1 = first.enabled; pmode.EN2 = second.enabled;

    const bool isPal = (first.enabled && (first.height > 224 || (first.display.DH + 1u) > 480)) ||
                       (second.enabled && (second.height > 224 || (second.display.DH + 1u) > 480)) ||
                       ((request.display1 >> 44) & 0x7FFu) >= 480 ||
                       ((request.display2 >> 44) & 0x7FFu) >= 480;

    static uint64_t s_lastLoggedTick = 0;
    if (request.vsyncTick == 0 || request.vsyncTick >= s_lastLoggedTick + 100u)
    {
        s_lastLoggedTick = request.vsyncTick;
        std::fprintf(stderr, "[vulkan:present] tick=%llu pmode=%llx smode2=%llx dispfb1=%llx display1=%llx dispfb2=%llx display2=%llx -> c1=(%u,%u) c2=(%u,%u) isPal=%d\n",
                     (unsigned long long)request.vsyncTick, (unsigned long long)request.pmode, (unsigned long long)request.smode2,
                     (unsigned long long)request.dispfb1, (unsigned long long)request.display1,
                     (unsigned long long)request.dispfb2, (unsigned long long)request.display2,
                     first.width, first.height, second.width, second.height, (int)isPal);
    }

    auto &priv = gs.get_priv_register_state();
    priv.pmode = pmode;
    priv.bgcolor = Reg64<BGCOLORBits>(request.bgcolor).desc;
    priv.smode1 = {};
    priv.smode1.LC = SMODE1Bits::LC_ANALOG;
    priv.smode1.CMOD = isPal ? SMODE1Bits::CMOD_PAL : SMODE1Bits::CMOD_NTSC;
    priv.smode2 = {};
    priv.smode2.INT = interlaced;
    priv.smode2.FFMD = frameMode;
    priv.smode2.DPMS = (request.smode2 >> 2u) & 3u;

    priv.dispfb1 = first.framebuffer; priv.display1 = first.display;
    priv.dispfb2 = second.framebuffer; priv.display2 = second.display;

    VSyncInfo info{};
    info.phase = static_cast<uint32_t>(request.vsyncTick & 1u);
    info.dst_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    info.dst_stage = VK_PIPELINE_STAGE_2_COPY_BIT;
    info.dst_access = VK_ACCESS_2_TRANSFER_READ_BIT;
    info.force_progressive = true;
    info.anti_blur = true;
    info.overscan = false;
    info.crtc_offsets = false;
    info.adapt_to_internal_horizontal_resolution = false;
    info.raw_circuit_scanout = true;
    info.skip_deinterlace = true;
    info.high_resolution_scanout = outputScale >= 2;
    info.high_resolution_scale_log2 = outputScale >= 4 ? 2u : 1u;
    // Native output averages every anti-aliasing sample into each pixel.
    info.resolve_super_samples = true;

    if (flushBeforeVsync)
        gs.flush();
    auto output = gs.vsync(info);
    if (!output.image) throw std::runtime_error("ParallelGS GPU scanout returned no image");
    const auto width = output.image->get_width();
    auto height = output.image->get_height();
    if (!width || !height || width > hostWidth * outputScale || height > hostHeight * outputScale)
    {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "ParallelGS image (%ux%u) exceeds the fixed host viewport (%ux%u)",
                      width, height, hostWidth * outputScale, hostHeight * outputScale);
        throw VulkanGsScanoutUnsupported(msg);
    }
    if (output.image->get_create_info().format != VK_FORMAT_R8G8B8A8_UNORM)
        throw std::runtime_error("ParallelGS GPU scanout returned an unexpected pixel format");

    static const bool s_sync = []() {
        const char *e = std::getenv("PS2X_GS_SYNCREADBACK");
        return e && *e && *e != '0';
    }();

    // Maps a completed readback for finishVulkanGsPresent; the copy happens after the device lock is released.
    auto consumeSlot = [&](VulkanGsScanoutState::RbSlot &slot) {
        const auto *mapped = static_cast<const uint8_t *>(device.map_host_buffer(*slot.buf, MEMORY_ACCESS_READ_BIT));
        if (!mapped)
        {
            slot.pending = false;
            slot.image.reset();
            slot.fence.reset();
            return;
        }

        if (metricsEnabled)
        {
            ++metrics.consumed;
            if (metrics.hasConsumed && slot.vsyncTick < metrics.consumedTick)
                ++metrics.consumeBackwards;
            metrics.consumedTick = slot.vsyncTick;
            metrics.hasConsumed = true;
        }
        VulkanGsScanoutState::StagedRead read;
        read.buf = slot.buf;
        read.mapped = mapped;
        read.w = slot.w;
        read.h = slot.h;
        read.displayFbp = slot.displayFbp;
        read.sourceFbp = slot.sourceFbp;
        read.vsyncTick = slot.vsyncTick;
        read.displayWidth = slot.displayWidth;
        read.displayHeight = slot.displayHeight;
        read.highResolution = slot.highResolution;
        read.frameFields = slot.frameFields;
        read.fieldMode = slot.fieldMode;
        state.staged.push_back(std::move(read));

        slot.pending = false;
        slot.image.reset();
        slot.fence.reset();
    };

    // 1. Prepare current slot: it is the oldest slot in the ring (submitted 3 frames ago)
    // and must be retired first to maintain chronological presentation order.
    auto &curSlot = state.slots[state.ringIndex];
    if (curSlot.pending)
    {
        if (curSlot.fence && curSlot.fence->wait_timeout(0))
        {
            consumeSlot(curSlot);
        }
        else
        {
            if (curSlot.fence)
            {
                const uint64_t before = metricsEnabled ? nowNs() : 0;
                const bool completed = curSlot.fence->wait_timeout(20'000'000ull);
                if (metricsEnabled)
                {
                    ++metrics.fullRingWaits;
                    if (!completed) ++metrics.waitTimeouts;
                    const uint64_t elapsed = nowNs() - before;
                    metrics.waitNs += elapsed;
                    metrics.maxWaitNs = std::max(metrics.maxWaitNs, elapsed);
                }
            }
            if (curSlot.pending)
                consumeSlot(curSlot);
        }
    }

    // 2. Consume newer pending slots in strict chronological order if already completed.
    // If an older slot is not ready, stop to preserve monotonic presentation order.
    for (uint32_t k = 1; k <= 2; ++k)
    {
        auto &nextSlot = state.slots[(state.ringIndex + k) % 3];
        if (nextSlot.pending && nextSlot.fence)
        {
            if (s_sync)
            {
                nextSlot.fence->wait();
                consumeSlot(nextSlot);
            }
            else if (nextSlot.fence->wait_timeout(0))
            {
                consumeSlot(nextSlot);
            }
            else
            {
                if (metricsEnabled) ++metrics.notReady;
                break;
            }
        }
    }

    if (!curSlot.buf || curSlot.w != width || curSlot.h != height)
    {
        BufferCreateInfo bufferInfo{};
        bufferInfo.size = static_cast<VkDeviceSize>(width) * height * 4u;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.domain = BufferDomain::CachedHost;
        curSlot.buf = device.create_buffer(bufferInfo);
        if (!curSlot.buf) throw std::runtime_error("GPU scanout readback allocation failed");
        curSlot.w = width;
        curSlot.h = height;
    }

    curSlot.displayFbp = first.enabled ? first.framebuffer.FBP : second.framebuffer.FBP;
    curSlot.sourceFbp = curSlot.displayFbp;
    curSlot.vsyncTick = request.vsyncTick;
    curSlot.highResolution = output.high_resolution_scanout;
    const uint32_t outputShift = output.high_resolution_scanout ? output.high_resolution_scale_log2 : 0u;
    curSlot.displayWidth = width >> outputShift;
    const auto &nativeCircuit = first.enabled ? first : second;
    curSlot.displayHeight = frameFields ? nativeCircuit.height * 2u :
                           fieldMode ? nativeCircuit.height :
                           height >> outputShift;
    curSlot.frameFields = frameFields;
    curSlot.fieldMode = fieldMode;

    auto command = device.request_command_buffer();
    command->copy_image_to_buffer(*curSlot.buf, *output.image, 0, {}, {width, height, 1},
                                 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
    command->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    Fence fence;
    device.submit(command, &fence);

    curSlot.fence = std::move(fence);
    curSlot.image = output.image;
    curSlot.pending = true;

    state.ringIndex = (state.ringIndex + 1u) % 3u;

    // For synchronous mode or initial startup frame: wait immediately
    if (s_sync || (!state.hasPresentedFrame && !state.lastFrame && state.staged.empty()))
    {
        if (curSlot.fence)
            curSlot.fence->wait();
        consumeSlot(curSlot);
    }

    static uint64_t s_lastLoggedTickVsync = 0;
    if (request.vsyncTick == 0 || request.vsyncTick >= s_lastLoggedTickVsync + 100u)
    {
        s_lastLoggedTickVsync = request.vsyncTick;
        const uint32_t resultWidth = state.staged.empty() ? state.lastFrame.width : state.staged.back().w;
        const uint32_t resultHeight = state.staged.empty() ? state.lastFrame.height : state.staged.back().h;
        std::fprintf(stderr, "[vulkan:vsync] tick=%llu output.img=(%u,%u) result=(%u,%u)\n",
                     (unsigned long long)request.vsyncTick, width, height, resultWidth, resultHeight);
    }

    if (metricsEnabled && metrics.hasConsumed)
    {
        if (metrics.hasReturned && metrics.consumedTick == metrics.returnedTick)
            ++metrics.duplicates;
        if (metrics.hasReturned && metrics.consumedTick < metrics.returnedTick)
            ++metrics.returnedBackwards;
        metrics.returnedTick = metrics.consumedTick;
        metrics.hasReturned = true;
        const uint64_t age = request.vsyncTick >= metrics.returnedTick ?
                             request.vsyncTick - metrics.returnedTick : 0;
        metrics.ageSum += age;
        metrics.ageMax = std::max(metrics.ageMax, age);
        const uint64_t now = nowNs();
        if (metrics.lastLogNs == 0) metrics.lastLogNs = now;
        if (now - metrics.lastLogNs >= 1'000'000'000ull)
        {
            std::fprintf(stderr,
                "[gs:frame-metrics] request=%llu source=%llu age=%llu requests=%llu "
                "consumed=%llu duplicate=%llu consumeBack=%llu returnBack=%llu "
                "notReady=%llu fullWait=%llu timeout=%llu waitMs=%.3f maxWaitMs=%.3f "
                "ageMean=%.3f ageMax=%llu\n",
                (unsigned long long)request.vsyncTick, (unsigned long long)metrics.returnedTick,
                (unsigned long long)age, (unsigned long long)metrics.requests,
                (unsigned long long)metrics.consumed, (unsigned long long)metrics.duplicates,
                (unsigned long long)metrics.consumeBackwards, (unsigned long long)metrics.returnedBackwards,
                (unsigned long long)metrics.notReady, (unsigned long long)metrics.fullRingWaits,
                (unsigned long long)metrics.waitTimeouts, metrics.waitNs / 1e6, metrics.maxWaitNs / 1e6,
                metrics.ageSum / static_cast<double>(metrics.requests), (unsigned long long)metrics.ageMax);
            metrics.lastLogNs = now;
        }
    }
}

bool finishVulkanGsPresentInto(Vulkan::Device &device, VulkanGsScanoutState &state,
                               std::vector<VulkanGsScanoutState::StagedRead> &reads,
                               ReusableFieldWeaver *weaver,
                               PresentationFrame &frame)
{
    if (reads.empty())
        return false;

    std::lock_guard<std::mutex> lock(state.frameMutex);
    // In high-resolution mode, paraLLEl-GS already reconstructs field-aware vertical samples on the GPU
    // and ReusableFieldWeaver runs in Progressive mode (a no-op on the pixel buffer). If multiple readbacks
    // completed together, earlier high-res reads would be immediately overwritten by the final read.
    const bool skipSupersededHighRes = reads.size() > 1u && reads.back().highResolution;
    for (size_t r = 0; r < reads.size(); ++r)
    {
        auto &read = reads[r];
        if (skipSupersededHighRes && r + 1u < reads.size())
        {
            device.unmap_host_buffer(*read.buf, Vulkan::MEMORY_ACCESS_READ_BIT);
            continue;
        }

        frame.width = read.w;
        frame.height = read.h;
        frame.displayFbp = read.displayFbp;
        frame.sourceFbp = read.sourceFbp;
        frame.strideBytes = read.w * 4u;
        frame.displayWidth = read.displayWidth;
        frame.displayHeight = read.displayHeight;
        // Rows are contiguous (stride = width): one pass copies every RGBA pixel and forces alpha (the top byte
        // of the little-endian word) opaque. Every byte is overwritten, so a same-size buffer is not cleared first.
        const size_t pixelCount = static_cast<size_t>(read.w) * read.h;
        frame.pixels.resize(pixelCount * 4u);
        uint8_t *dst = frame.pixels.data();
        const size_t pairCount = pixelCount / 2u;
        for (size_t i = 0; i < pairCount; ++i)
        {
            uint64_t rgba2;
            std::memcpy(&rgba2, read.mapped + i * 8u, sizeof(rgba2));
            rgba2 |= 0xFF000000FF000000ULL;
            std::memcpy(dst + i * 8u, &rgba2, sizeof(rgba2));
        }
        if (pixelCount & 1u)
        {
            const size_t last = pixelCount - 1u;
            uint32_t rgba;
            std::memcpy(&rgba, read.mapped + last * 4u, sizeof(rgba));
            rgba |= 0xFF000000u;
            std::memcpy(dst + last * 4u, &rgba, sizeof(rgba));
        }
        device.unmap_host_buffer(*read.buf, Vulkan::MEMORY_ACCESS_READ_BIT);

        if (weaver)
        {
            const bool oddField = (read.vsyncTick & 1ull) != 0ull;
            // paraLLEl high-res output already reconstructs field-aware vertical
            // samples. Applying our half-height weave would double it again.
            const auto weaveMode = read.highResolution ? ReusableFieldWeaver::Mode::Progressive :
                                   read.frameFields ? ReusableFieldWeaver::Mode::HalfHeightFrame :
                                   read.fieldMode   ? ReusableFieldWeaver::Mode::FullHeightField :
                                                      ReusableFieldWeaver::Mode::Progressive;
            if (!read.highResolution && read.frameFields && (noWeave() || uint64_t(frame.height) * 2u > hostHeight))
            {
                weaver->reset();
            }
            else
            {
                weaver->present(frame.pixels, frame.width, frame.height, read.w * 4u, read.vsyncTick, oddField,
                                weaveMode);
            }
        }
    }
    state.hasPresentedFrame = static_cast<bool>(frame);
    return true;
}

PresentationFrame finishVulkanGsPresent(Vulkan::Device &device, VulkanGsScanoutState &state,
                                        std::vector<VulkanGsScanoutState::StagedRead> &reads,
                                        ReusableFieldWeaver *weaver)
{
    finishVulkanGsPresentInto(device, state, reads, weaver, state.lastFrame);
    std::lock_guard<std::mutex> lock(state.frameMutex);
    return state.lastFrame;
}

