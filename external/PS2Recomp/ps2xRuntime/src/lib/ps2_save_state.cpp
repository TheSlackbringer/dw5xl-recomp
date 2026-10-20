// DW5XL (issue 027): save states. The whole running game — guest memory, EE threads and kernel, device state, the
// IOP, the renderer's VRAM and registers — written to one file at an EE quiet point and restored the same way.
// File: magic, format version, build ID, body size and checksum, then the body; a state that does not match this
// build or format, or whose body is damaged, is refused before anything changes.
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_device_queue.h"
#include "runtime/state_archive.h"
#include "runtime/stub_state.h"
#include "ps2x/iop/iop_subsystem.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>

namespace
{
    constexpr char kMagic[8] = {'D', 'W', '5', 'X', 'L', 'S', 'T', 'A'};
    constexpr uint32_t kFormatVersion = 1u;

    uint64_t fnv1a(const uint8_t *data, size_t size)
    {
        uint64_t hash = 0xCBF29CE484222325ull;
        for (size_t i = 0; i < size; ++i)
        {
            hash ^= data[i];
            hash *= 0x100000001B3ull;
        }
        return hash;
    }

    struct Header
    {
        char magic[8];
        uint32_t version;
        uint32_t reserved;
        uint64_t bodySize;
        uint64_t bodyHash;
    };

    std::vector<uint8_t> readFile(const std::string &path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            throw ps2x::StateError("cannot open " + path);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    void writeFileAtomically(const std::string &path, const std::vector<uint8_t> &bytes)
    {
        const std::filesystem::path target(path);
        if (target.has_parent_path())
            std::filesystem::create_directories(target.parent_path());
        const std::string tmp = path + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
            if (!out)
                throw ps2x::StateError("cannot write " + tmp);
        }
        std::filesystem::rename(tmp, target);
    }
} // namespace

void PS2Memory::serializeState(ps2x::StateArchive &a)
{
    auto transfers = [](ps2x::StateArchive &x, std::vector<PendingTransfer> &list)
    {
        x.sequence(list, [](ps2x::StateArchive &y, PendingTransfer &t)
        {
            y.value(t.fromScratchpad);
            y.value(t.srcAddr);
            y.value(t.qwc);
            y.vector(t.chainData);
        });
    };
    a.tag("EMEM");
    a.raw(m_rdram, PS2_RAM_SIZE);
    a.raw(m_scratchpad, PS2_SCRATCHPAD_SIZE);
    a.raw(iop_ram, 2u * 1024u * 1024u);
    a.raw(m_gsVRAM, PS2_GS_VRAM_SIZE); // the CPU-side copy; the renderer's own VRAM is its own section
    a.raw(m_vu0Code, PS2_VU0_CODE_SIZE);
    a.raw(m_vu0Data, PS2_VU0_DATA_SIZE);
    a.raw(m_vu1Code, PS2_VU1_CODE_SIZE);
    a.raw(m_vu1Data, PS2_VU1_DATA_SIZE);
    a.tag("EREG");
    for (uint64_t *reg : {&gs_regs.pmode, &gs_regs.smode1, &gs_regs.smode2, &gs_regs.srfsh, &gs_regs.synch1,
                          &gs_regs.synch2, &gs_regs.syncv, &gs_regs.dispfb1, &gs_regs.display1, &gs_regs.dispfb2,
                          &gs_regs.display2, &gs_regs.extbuf, &gs_regs.extdata, &gs_regs.extwrite, &gs_regs.bgcolor,
                          &gs_regs.imr, &gs_regs.busdir, &gs_regs.siglblid})
        a.value(*reg);
    for (std::atomic<uint64_t> *reg : {&gs_regs.csr, &gs_regs.vsyncTick})
    {
        uint64_t v = reg->load(std::memory_order_acquire);
        a.value(v);
        if (a.loading())
            reg->store(v, std::memory_order_release);
    }
    a.value(vif0_regs);
    a.value(vif1_regs);
    a.value(dma_regs);
    a.podMap(m_ioRegisters);
    a.vector(m_tlbEntries);
    a.value(m_seenGifCopy);
    a.value(m_path3Masked);
    a.value(m_vif1PendingPath2ImageQwc);
    a.value(m_vif1PendingPath2DirectHl);
    a.sequence(m_path3MaskedFifo, [](ps2x::StateArchive &x, std::vector<uint8_t> &packet) { x.vector(packet); });
    transfers(a, m_pendingGifTransfers);
    transfers(a, m_pendingVif0Transfers);
    transfers(a, m_pendingVif1Transfers);
    {
        std::lock_guard lock(m_completedDmacMutex);
        a.vector(m_completedDmacCauses);
    }
    a.sequence(m_codeRegions, [](ps2x::StateArchive &x, CodeRegion &r)
    {
        x.value(r.start);
        x.value(r.end);
        x.boolVector(r.modified);
    });
    a.value(m_eeTimers);
    a.value(m_ipuInputCredit);
    a.value(m_ipuInputTagLoaded);
    a.value(m_ipuInputEndAfterPayload);
    if (a.loading())
    {
        // VU code changed under any cache built from it: let lifted VU1 / interpreters look again.
        markVU0CodeModified();
        markVU1CodeModified();
    }
}

void GSRasterBackend::SerializeState(ps2x::StateArchive &)
{
    throw ps2x::StateError("this renderer does not support save states (use the Vulkan renderer)");
}

void GS::serializeState(ps2x::StateArchive &a)
{
    a.tag("GSFE");
    a.value(m_gifPathStates);
    a.value(m_localMemorySize);
    a.value(m_ctx);
    a.value(m_prim);
    a.value(m_primRegister);
    a.value(m_prmodeRegister);
    a.value(m_curR);
    a.value(m_curG);
    a.value(m_curB);
    a.value(m_curA);
    a.value(m_curU);
    a.value(m_curV);
    a.value(m_curFog);
    a.value(m_fogR);
    a.value(m_fogG);
    a.value(m_fogB);
    a.value(m_prmodecont);
    a.value(m_pabe);
    a.value(m_scanmsk);
    a.value(m_dimx);
    a.value(m_dthe);
    a.value(m_colclamp);
    a.value(m_texa);
    a.value(m_texclut);
    a.value(m_bitbltbuf);
    a.value(m_trxpos);
    a.value(m_trxreg);
    a.value(m_trxdir);
    a.value(m_vtxQueue);
    a.value(m_vtxCount);
    a.value(m_vtxIndex);
    a.value(m_lastDisplayBaseBytes);
    a.value(m_preferredDisplaySourceFrame);
    a.value(m_preferredDisplayDestFbp);
    a.value(m_hasPreferredDisplaySource);
    if (a.loading())
    {
        // Host presentation is rebuilt from the next fields.
        std::lock_guard lock(m_snapshotMutex);
        m_displaySnapshot.clear();
        m_latchedFrame = {};
        m_hasHostPresentationFrame = false;
        ++m_hostPresentationGeneration;
    }
    if (!m_backend)
        throw ps2x::StateError("no GS renderer");
    m_backend->SerializeState(a);
}

void VU1Interpreter::serializeState(ps2x::StateArchive &a)
{
    a.tag(m_unit == Unit::VU0 ? "VU0S" : "VU1S");
    a.value(m_state);
    a.value(m_flagPipeline);
    a.value(m_fdiv);
    a.value(m_efu);
    a.value(m_storePipeline);
    a.value(m_xgkick);
    a.value(m_ready);
    a.value(m_cycle);
    a.value(m_efuResourceReady);
    a.value(m_nextCommitCycle);
    a.value(m_writebackHorizon);
    a.value(m_flagMask);
    a.value(m_efuMask);
    a.value(m_storeMask);
    a.value(m_workingClip);
    a.value(m_currentUpperInstruction);
    a.value(m_fmacQ);
    a.value(m_fmacI);
    a.value(m_viBranchBackupValue);
    a.value(m_viBranchBackupReg);
    a.value(m_viBranchBackupValid);
    a.value(m_stopRequested);
    a.value(m_pendingHaltD);
    a.value(m_pendingHaltT);
    if (a.loading())
    {
        // Decoded code and lift timing are rebuilt from the restored VU code memory.
        m_decodedCodeCacheValid = false;
        m_liftTimingValid = false;
        m_cachedVuCode = nullptr;
        m_cachedMemory = nullptr;
        m_cachedCodeSize = 0;
        m_cachedCodeGeneration = 0;
    }
}

namespace
{
    std::map<std::string, ps2x::StubStateFn> &stubStates()
    {
        static std::map<std::string, ps2x::StubStateFn> registry;
        return registry;
    }
} // namespace

bool ps2x::registerStubState(const char *name, StubStateFn fn)
{
    // The file name only: the same build always gives the same names, whatever folder it was built in.
    std::string key = name;
    const size_t slash = key.find_last_of("/\\");
    if (slash != std::string::npos)
        key = key.substr(slash + 1);
    if (!stubStates().emplace(key, fn).second)
        std::cerr << "[state] stub state '" << key << "' registered twice" << std::endl;
    return true;
}

namespace
{
    uint8_t *g_stateGuestRdram = nullptr;
}

uint8_t *ps2x::stateGuestRdram()
{
    return g_stateGuestRdram;
}

void ps2x::serializeStubStates(StateArchive &a, uint8_t *rdram)
{
    g_stateGuestRdram = rdram;
    a.tag("STUB");
    for (auto &[name, fn] : stubStates())
    {
        std::string named = name;
        a.string(named);
        if (a.loading() && named != name)
            throw StateError("stub state '" + named + "' expected '" + name + "' (another build?)");
        fn(a);
    }
    g_stateGuestRdram = nullptr;
}

void PS2Runtime::registerStateHook(std::string name, StateHook hook)
{
    m_stateHooks.emplace_back(std::move(name), std::move(hook));
}

void PS2Runtime::serializeState(ps2x::StateArchive &a)
{
    a.tag("RTIM");
    std::string build = m_buildId;
    a.string(build);
    if (a.loading() && build != m_buildId)
        throw ps2x::StateError("state is from build " + build + ", this is " + m_buildId);

    m_memory.serializeState(a);
    a.tag("GIFA"); // drained before saving: nothing in flight is part of a state
    if (a.saving() && !m_gifArbiter.empty())
        throw ps2x::StateError("GIF packets still queued after the drain");
    if (a.loading())
        m_gifArbiter.reset();
    m_gs.serializeState(a);
    m_vu0.serializeState(a);
    m_vu1.serializeState(a);

    a.tag("ECPU");
    a.value(m_cpuContext);
    m_eeScheduler->serializeState(a);

    a.tag("RTHL");
    {
        std::lock_guard heapLock(m_guestHeapMutex);
        a.vector(m_guestHeapBlocks);
        a.value(m_guestHeapBase);
        a.value(m_guestHeapEnd);
        a.value(m_guestHeapLimit);
        a.value(m_guestHeapSuggestedBase);
        a.value(m_guestHeapConfigured);
    }
    {
        std::lock_guard stackLock(m_asyncCallbackStackMutex);
        a.value(m_asyncCallbackStackFloor);
        a.value(m_asyncCallbackStackTop);
    }
    {
        std::lock_guard kernelLock(m_eeKernelStateMutex);
        a.map(m_eeExitHandlers, [](ps2x::StateArchive &x, std::vector<EeExitHandlerRegistration> &list) { x.vector(list); });
        a.podMap(m_eeSyscallOverrides);
        a.podSet(m_eeSyscallMirrorAddresses);
    }
    a.sequence(m_loadedModules, [](ps2x::StateArchive &x, LoadedModule &m)
    {
        x.string(m.name);
        x.value(m.baseAddress);
        x.value(m.size);
        x.value(m.active);
    });
    std::string module = m_activeCompiledModule ? m_activeCompiledModule->identity : "";
    a.string(module);
    if (a.loading())
    {
        if (module.empty())
            deactivateCompiledModule();
        else if (!activateCompiledModule(module))
            throw ps2x::StateError("state needs compiled module " + module + ", which this build does not have");
    }

    ps2x::serializeStubStates(a, m_memory.getRDRAM());
    m_iopSubsystem->serializeState(a);

    for (auto &[name, hook] : m_stateHooks)
    {
        char tag[5] = {};
        std::snprintf(tag, sizeof(tag), "%-4.4s", name.c_str());
        a.tag(reinterpret_cast<const char(&)[5]>(tag));
        hook(a);
    }
    a.tag("DONE");
}

bool PS2Runtime::performStateRequest(bool save, const std::string &path)
{
    const auto started = std::chrono::steady_clock::now();
    const uint64_t tick = m_memory.gs().vsyncTick;
    try
    {
        // Nothing in flight: the VIF1/VU1/GIF device finishes what it was given and the renderer flushes.
        ps2x::device::drainAll(m_memory, ps2x::device::DrainReason::Explicit);
        m_gifArbiter.drain();
        m_gifArbiter.waitIdle();
        if (save)
        {
            ps2x::StateArchive body = ps2x::StateArchive::writer();
            serializeState(body);
            Header header{};
            std::memcpy(header.magic, kMagic, sizeof(kMagic));
            header.version = kFormatVersion;
            header.bodySize = body.data().size();
            header.bodyHash = fnv1a(body.data().data(), body.data().size());
            std::vector<uint8_t> file(sizeof(Header));
            std::memcpy(file.data(), &header, sizeof(Header));
            file.insert(file.end(), body.data().begin(), body.data().end());
            writeFileAtomically(path, file);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            std::cerr << "[state] saved " << path << " at tick " << tick << " (" << file.size() / (1024 * 1024)
                      << " MiB, " << ms << " ms)" << std::endl;
            return true;
        }

        std::vector<uint8_t> file = readFile(path);
        Header header{};
        if (file.size() < sizeof(Header))
            throw ps2x::StateError(path + " is not a save state");
        std::memcpy(&header, file.data(), sizeof(Header));
        if (std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0)
            throw ps2x::StateError(path + " is not a DW5XL save state");
        if (header.version != kFormatVersion)
            throw ps2x::StateError("state format " + std::to_string(header.version) + ", this build reads " +
                                   std::to_string(kFormatVersion));
        if (header.bodySize != file.size() - sizeof(Header) ||
            header.bodyHash != fnv1a(file.data() + sizeof(Header), file.size() - sizeof(Header)))
            throw ps2x::StateError(path + " is damaged (size or checksum)");
        std::vector<uint8_t> bodyBytes(file.begin() + sizeof(Header), file.end());
        {
            // The build ID is the first field of the body: check it before anything is overwritten.
            ps2x::StateArchive peek = ps2x::StateArchive::reader(bodyBytes);
            peek.tag("RTIM");
            std::string build;
            peek.string(build);
            if (build != m_buildId)
                throw ps2x::StateError("state is from build " + build + ", this is " + m_buildId);
        }
        ps2x::StateArchive body = ps2x::StateArchive::reader(std::move(bodyBytes));
        try
        {
            serializeState(body);
            if (!body.atEnd())
                throw ps2x::StateError("state has data this build does not read");
        }
        catch (const std::exception &e)
        {
            // Past the checks, a failure means the code and the file disagree: the game is half-loaded, stop it.
            std::cerr << "[state] load of " << path << " failed part-way: " << e.what() << " — stopping" << std::endl;
            requestStop();
            return false;
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        std::cerr << "[state] loaded " << path << " (saved at tick " << m_memory.gs().vsyncTick << ", " << ms
                  << " ms)" << std::endl;
        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "[state] " << (save ? "save" : "load") << " refused: " << e.what() << std::endl;
        return false;
    }
}
