// DW5XL-TEST: offline VIF1 replay harness. No game run, no window, no GPU.
//
// Feeds a .v1cap capture (PS2X_VIF1_CAPTURE, or --write-synthetic) through
// the real runtime path: PS2Runtime wiring (syncCoreSubsystems), PS2Memory
// DMA completion (processPendingTransfers) -> VIF1 interpreter -> VU1 ->
// GIF arbiter -> GS frontend -> recording backend (null renderer). The GS
// command stream is written in the canonical PS2X_GS_RECORD format, so two
// replays can be compared with `dw5xl_gsrec diff`.
//
//   dw5xl_vif1_replay <in.v1cap> --out <gs.rec> [--mode sync|async]
//                     [--drain each|end] [--max-kicks N] [--state-out <file>] [--quiet]
//   dw5xl_vif1_replay --write-synthetic <out.v1cap>
//
// --mode sets PS2X_VU1_ASYNC=0/1 before the runtime is constructed, so an
// asynchronous VIF1/VU1/GS device (when present in the runtime) is used the
// same way the game uses it. --drain each performs a DMA-status observation
// (D1_CHCR + D_STAT reads, which must drain an async device) after every
// kick; --drain end only once at the end. Every replay ends with that
// observation, then prints a digest of the final VU1/VIF1 state; --state-out
// writes the final state in the canonical v1cap State layout for `cmp`.

#include "ps2_runtime.h"
#include "gs/gs_record.h"
#include "vif1_capture.h"
#include "synthetic_vif1.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#if defined(_WIN32)
#include <stdlib.h>
// MSVC CRT equivalents; an empty value removes the variable.
static int setenv(const char *name, const char *value, int) { return _putenv_s(name, value); }
static int unsetenv(const char *name) { return _putenv_s(name, ""); }
#endif
#include <string>
#include <vector>

extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x1000;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x1004;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 1;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[] = {nullptr};

namespace
{
    constexpr uint32_t kD1Chcr = 0x10009000u;
    constexpr uint32_t kDStat = 0x1000E010u;

    template <class T> bool take(const std::vector<uint8_t> &payload, size_t &offset, T &value)
    {
        if (offset > payload.size() || payload.size() - offset < sizeof(T))
            return false;
        std::memcpy(&value, payload.data() + offset, sizeof(T));
        offset += sizeof(T);
        return true;
    }

    void usage()
    {
        std::fprintf(stderr,
                     "usage: dw5xl_vif1_replay <in.v1cap> --out <gs.rec> [--mode sync|async] [--drain each|end]\n"
                     "                         [--max-kicks N] [--state-out <file>] [--quiet]\n"
                     "       dw5xl_vif1_replay --write-synthetic <out.v1cap>\n");
    }

    void applyState(PS2Runtime &rt, const Vif1Capture::StateSnapshot &s)
    {
        PS2Memory &mem = rt.memory();
        if (s.vu1Code.size() == PS2_VU1_CODE_SIZE)
            std::memcpy(mem.getVU1Code(), s.vu1Code.data(), PS2_VU1_CODE_SIZE);
        if (s.vu1Data.size() == PS2_VU1_DATA_SIZE)
            std::memcpy(mem.getVU1Data(), s.vu1Data.data(), PS2_VU1_DATA_SIZE);
        mem.markVU1CodeModified();
        std::memcpy(&mem.vif1_regs, s.vif1Regs, sizeof(s.vif1Regs));
        mem.m_path3Masked = s.path3Masked;
        mem.m_vif1PendingPath2ImageQwc = s.pendingPath2Qwc;
        mem.m_vif1PendingPath2DirectHl = s.pendingPath2DirectHl;
        mem.m_path3MaskedFifo = s.maskedPath3Packets;
        if (s.hasVu1State)
            rt.vu1().state() = s.vu1;
        rt.cpu().vu0_fbrst = s.vu0Fbrst;
        mem.gs().vsyncTick.store(s.tick, std::memory_order_release);
    }

    void observe(PS2Runtime &rt)
    {
        // DMA-status observation: what the guest polls (sceGsSyncPath,
        // sceDmaSync). An asynchronous device must be drained by these reads.
        (void)rt.memory().readIORegister(kD1Chcr);
        (void)rt.memory().readIORegister(kDStat);
    }
}

int main(int argc, char **argv)
{
    std::string input, out, stateOut, mode = "sync", drain = "end";
    uint64_t maxKicks = UINT64_MAX;
    bool quiet = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string
        {
            if (i + 1 >= argc)
            {
                usage();
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--write-synthetic")
        {
            const std::string path = value();
            if (!SyntheticVif1::write(path))
            {
                std::fprintf(stderr, "cannot write %s\n", path.c_str());
                return 2;
            }
            std::printf("wrote synthetic capture %s\n", path.c_str());
            return 0;
        }
        if (arg == "--out")
            out = value();
        else if (arg == "--mode")
            mode = value();
        else if (arg == "--drain")
            drain = value();
        else if (arg == "--max-kicks")
            maxKicks = std::strtoull(value().c_str(), nullptr, 0);
        else if (arg == "--state-out")
            stateOut = value();
        else if (arg == "--quiet")
            quiet = true;
        else if (!arg.empty() && arg[0] != '-' && input.empty())
            input = arg;
        else
        {
            usage();
            return 2;
        }
    }
    if (input.empty() || out.empty() || (mode != "sync" && mode != "async") || (drain != "each" && drain != "end"))
    {
        usage();
        return 2;
    }

    std::vector<Vif1Capture::Record> records;
    std::string message;
    if (!Vif1Capture::readFile(input, records, message))
    {
        std::fprintf(stderr, "dw5xl_vif1_replay: %s\n", message.c_str());
        return 2;
    }
    if (!message.empty())
        std::fprintf(stderr, "dw5xl_vif1_replay: warning: %s\n", message.c_str());

    // Mode switch for the runtime (read when the runtime/device is created).
    setenv("PS2X_VU1_ASYNC", mode == "async" ? "1" : "0", 1);
    // The GS constructor builds a default backend before we install the
    // recorder; keep it from opening a Vulkan device or a second recording.
    setenv("PS2X_GS_BACKEND", "cpu", 1);
    unsetenv("PS2X_GS_RECORD");

    auto rt = std::make_unique<PS2Runtime>();
    if (!rt->memory().initialize())
    {
        std::fprintf(stderr, "dw5xl_vif1_replay: memory initialization failed\n");
        return 2;
    }
    if (!rt->syncCoreSubsystems())
    {
        std::fprintf(stderr, "dw5xl_vif1_replay: core subsystem wiring failed\n");
        return 2;
    }
    {
        auto recorder = makeGsRecordingBackend(makeGsNullBackend(), out.c_str());
        if (!recorder)
        {
            std::fprintf(stderr, "dw5xl_vif1_replay: cannot open %s\n", out.c_str());
            return 2;
        }
        rt->gs().setRasterBackend(std::move(recorder));
    }

    uint64_t kicks = 0, kickBytes = 0, regWrites = 0, states = 0;
    bool stoppedEarly = false;
    for (const auto &rec : records)
    {
        size_t offset = 0;
        switch (rec.type)
        {
        case Vif1Capture::Rec::State:
        {
            Vif1Capture::StateSnapshot state;
            if (!Vif1Capture::readState(rec.payload.data(), rec.payload.data() + rec.payload.size(), state))
            {
                std::fprintf(stderr, "dw5xl_vif1_replay: bad State record\n");
                return 2;
            }
            if (states++ != 0u)
                std::fprintf(stderr, "dw5xl_vif1_replay: warning: extra State record applied mid-stream\n");
            applyState(*rt, state);
            break;
        }
        case Vif1Capture::Rec::Kick:
        {
            if (kicks >= maxKicks)
            {
                stoppedEarly = true;
                break;
            }
            uint64_t tick = 0;
            uint32_t fbrst = 0, size = 0;
            if (!take(rec.payload, offset, tick) || !take(rec.payload, offset, fbrst) ||
                !take(rec.payload, offset, size) || rec.payload.size() - offset != size)
            {
                std::fprintf(stderr, "dw5xl_vif1_replay: bad Kick record\n");
                return 2;
            }
            rt->memory().gs().vsyncTick.store(tick, std::memory_order_release);
            rt->cpu().vu0_fbrst = fbrst;
            // Same entry as a completed VIF1 DMA chain snapshot in the runtime.
            PS2Memory::PendingTransfer transfer;
            transfer.chainData.assign(rec.payload.begin() + static_cast<std::ptrdiff_t>(offset), rec.payload.end());
            transfer.qwc = size / 16u;
            rt->memory().m_pendingVif1Transfers.push_back(std::move(transfer));
            rt->memory().processPendingTransfers();
            if (drain == "each")
                observe(*rt);
            ++kicks;
            kickBytes += size;
            break;
        }
        case Vif1Capture::Rec::VifReg:
        {
            uint64_t tick = 0;
            uint32_t address = 0, value = 0;
            if (!take(rec.payload, offset, tick) || !take(rec.payload, offset, address) ||
                !take(rec.payload, offset, value))
            {
                std::fprintf(stderr, "dw5xl_vif1_replay: bad VifReg record\n");
                return 2;
            }
            if (kicks >= maxKicks)
                break;
            rt->memory().gs().vsyncTick.store(tick, std::memory_order_release);
            rt->memory().writeIORegister(address, value);
            ++regWrites;
            break;
        }
        case Vif1Capture::Rec::End:
            break;
        default:
            std::fprintf(stderr, "dw5xl_vif1_replay: warning: unknown record type %u skipped\n",
                         static_cast<unsigned>(rec.type));
            break;
        }
    }
    observe(*rt);

    // Final-state digest (VU1 memory/registers, VIF1 registers, PATH2/3 state).
    Vif1Capture::StateSnapshot finalState = Vif1Capture::snapshotMemory(rt->memory());
    finalState.hasVu1State = true;
    finalState.vu1 = rt->vu1().state();
    finalState.vu0Fbrst = rt->cpu().vu0_fbrst;
    finalState.tick = rt->memory().gs().vsyncTick.load();
    std::vector<uint8_t> vu1StateBytes;
    Vif1Capture::writeVu1State(vu1StateBytes, finalState.vu1);
    std::vector<uint8_t> finalBytes;
    Vif1Capture::writeState(finalBytes, finalState);
    if (!stateOut.empty())
    {
        if (std::FILE *file = std::fopen(stateOut.c_str(), "wb"))
        {
            std::fwrite(finalBytes.data(), 1u, finalBytes.size(), file);
            std::fclose(file);
        }
        else
        {
            std::fprintf(stderr, "dw5xl_vif1_replay: cannot write %s\n", stateOut.c_str());
            return 2;
        }
    }
    if (!quiet)
        std::printf("[replay] mode=%s drain=%s kicks=%llu bytes=%llu vifRegWrites=%llu%s\n"
                    "[replay] final vu1Code=%016llx vu1Data=%016llx vu1State=%016llx vif1Regs=%016llx state=%016llx\n"
                    "[replay] gs stream -> %s\n",
                    mode.c_str(), drain.c_str(), static_cast<unsigned long long>(kicks),
                    static_cast<unsigned long long>(kickBytes), static_cast<unsigned long long>(regWrites),
                    stoppedEarly ? " (stopped at --max-kicks)" : "",
                    static_cast<unsigned long long>(GSRecord::fnv1a(finalState.vu1Code.data(), finalState.vu1Code.size())),
                    static_cast<unsigned long long>(GSRecord::fnv1a(finalState.vu1Data.data(), finalState.vu1Data.size())),
                    static_cast<unsigned long long>(GSRecord::fnv1a(vu1StateBytes.data(), vu1StateBytes.size())),
                    static_cast<unsigned long long>(GSRecord::fnv1a(reinterpret_cast<const uint8_t *>(finalState.vif1Regs),
                                                                    sizeof(finalState.vif1Regs))),
                    static_cast<unsigned long long>(GSRecord::fnv1a(finalBytes.data(), finalBytes.size())),
                    out.c_str());
    std::fflush(stdout);
    // Closes the recording (backend destructor) and stops any device worker.
    rt.reset();
    return 0;
}
