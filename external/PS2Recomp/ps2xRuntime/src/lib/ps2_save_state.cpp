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
    constexpr uint32_t kFormatVersion = 3u; // 3: complete COP0 TLB register images (issue 038)

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
    uint64_t g_stateVsyncTick = 0u;
}

uint8_t *ps2x::stateGuestRdram()
{
    return g_stateGuestRdram;
}

uint64_t ps2x::stateVsyncTick()
{
    return g_stateVsyncTick;
}

void ps2x::serializeStubStates(StateArchive &a, uint8_t *rdram, uint64_t vsyncTick)
{
    g_stateGuestRdram = rdram;
    g_stateVsyncTick = vsyncTick;
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
    g_stateVsyncTick = 0u;
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

    ps2x::serializeStubStates(a, m_memory.getRDRAM(), m_eeScheduler->currentVSyncTick());
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

bool PS2Runtime::performStateRequest(bool save, const std::string &path, bool &retry)
{
    const auto started = std::chrono::steady_clock::now();
    retry = false;
    try
    {
        // Nothing in flight: the VIF1/VU1/GIF device finishes what it was given and the renderer flushes. What that
        // completes (DMA interrupts, events) is handled by the scheduler first: then this request comes back.
        ps2x::device::drainAll(m_memory, ps2x::device::DrainReason::Explicit);
        m_gifArbiter.drain();
        m_gifArbiter.waitIdle();
        if (!m_eeScheduler->stateQuiet())
        {
            retry = true;
            return false;
        }
        const auto saveTo = [&](const std::string &target)
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
            writeFileAtomically(target, file);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            std::cerr << "[state] saved " << target << " at tick " << m_memory.gs().vsyncTick << " (" << file.size() / (1024 * 1024)
                      << " MiB, " << ms << " ms)" << std::endl;
        };
        if (save)
        {
            saveTo(path);
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
        m_stateLoads.fetch_add(1u, std::memory_order_release);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        std::cerr << "[state] loaded " << path << " (saved at tick " << m_memory.gs().vsyncTick << ", " << ms
                  << " ms)" << std::endl;
        // Test switch: save again at once, before the game runs a single step (save -> load -> save identity).
        if (const char *again = std::getenv("PS2X_STATE_RESAVE"); again && *again)
            saveTo(again);
        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "[state] " << (save ? "save" : "load") << " refused: " << e.what() << std::endl;
        return false;
    }
}
