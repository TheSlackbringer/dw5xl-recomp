#include "ps2_runtime.h"
#include "ps2_log.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "game_overrides.h"
#include "runtime/host_input.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_present_stats.h"
#include "gs/gs_trace.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_device_queue.h"
#include "runtime/ps2_settings.h"
#include "runtime/ps2_stdio_pump.h"
#include "runtime/ps2_profile_markers.h"
#include "vif1_capture.h"
#include "ThreadNaming.h"
#include "Kernel/Stubs/Audio.h"
#include "Kernel/Stubs/GS.h"
#include "Kernel/Stubs/MPEG.h"
#include "ps2_host_backend.h"
#include "rlgl.h"
#include "ps2_iop_host.h"
#include "ps2x/iop/iop_subsystem.h"

#include <iostream>
#include <stdexcept>
#include <fstream>
#include <algorithm>
#include <array>
#include <cstring>
#include <cfenv>
#include <initializer_list>
#include <limits>
#include <chrono>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <vector>
#include <string>
#include <cmath>
#include "runtime/hotkeys.h"
#include "runtime/ps2_present_feedback.h"

namespace ps2_stubs
{
    void resetSifState();
}

#define ELF_MAGIC 0x464C457F // "\x7FELF" in little endian
#define ET_EXEC 2            // Executable file
#define EM_MIPS 8            // MIPS architecture
#define PT_LOAD 1            // Loadable segment

static constexpr int FB_WIDTH = 640;
static constexpr int FB_HEIGHT = 512;
static constexpr int DEFAULT_DISPLAY_HEIGHT = 448;
static constexpr uint32_t DEFAULT_FB_SIZE = FB_WIDTH * FB_HEIGHT * 4;
static constexpr uint32_t DEFAULT_FB_ADDR = (PS2_RAM_SIZE - DEFAULT_FB_SIZE - 0x10000u);
#if defined(PLATFORM_VITA)
static constexpr int HOST_WINDOW_WIDTH = 960;
static constexpr int HOST_WINDOW_HEIGHT = 544;
#else
static constexpr int HOST_WINDOW_WIDTH = FB_WIDTH;
static constexpr int HOST_WINDOW_HEIGHT = DEFAULT_DISPLAY_HEIGHT;
#endif
struct ElfHeader
{
    uint32_t magic;
    uint8_t elf_class;
    uint8_t endianness;
    uint8_t version;
    uint8_t os_abi;
    uint8_t abi_version;
    uint8_t padding[7];
    uint16_t type;
    uint16_t machine;
    uint32_t version2;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct ProgramHeader
{
    uint32_t type;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
};

namespace
{
    constexpr uint32_t kGuestHeapDefaultBase = 0x00100000u;
    constexpr uint32_t kGuestHeapDefaultAlignment = 16u;
    constexpr uint32_t kGuestHeapSafetyPad = 0x1000u;
    constexpr uint32_t kGuestHeapHardLimit = 0x01F00000u;

    constexpr uint32_t COP0_CAUSE_EXCCODE_MASK = 0x0000007Cu;
    constexpr uint32_t COP0_CAUSE_BD = 0x80000000u;
    constexpr uint32_t COP0_STATUS_EXL = 0x00000002u;
    constexpr uint32_t COP0_STATUS_BEV = 0x00400000u;
    constexpr uint32_t EXCEPTION_VECTOR_GENERAL = 0x80000080u;
    constexpr uint32_t EXCEPTION_VECTOR_TLB_REFILL = 0x80000000u;
    constexpr uint32_t EXCEPTION_VECTOR_BOOT = 0xBFC00200u;

    struct DispatchHistory
    {
        std::array<uint32_t, 64> pcs{};
        uint32_t next = 0u;
        bool wrapped = false;
    };

    thread_local DispatchHistory g_dispatchHistory;
    // Counts EE checkpoints that unwind generated code back to the scheduler. A call during which one happened
    // did not return, whatever ctx->pc says (a recursive callee can unwind with pc == the caller's own entry).
    thread_local uint64_t g_unwindEpoch = 0;

    bool computeFileCrc32(const std::string &path, uint32_t &crcOut)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        static const std::array<uint32_t, 256> table = []
        {
            std::array<uint32_t, 256> values{};
            for (uint32_t i = 0; i < values.size(); ++i)
            {
                uint32_t value = i;
                for (uint32_t bit = 0; bit < 8; ++bit)
                {
                    value = (value & 1u) ? (0xEDB88320u ^ (value >> 1u)) : (value >> 1u);
                }
                values[i] = value;
            }
            return values;
        }();

        uint32_t crc = 0xFFFFFFFFu;
        std::array<uint8_t, 16 * 1024> buffer{};
        while (file.good())
        {
            file.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = file.gcount();
            for (std::streamsize i = 0; i < count; ++i)
            {
                crc = table[(crc ^ buffer[static_cast<size_t>(i)]) & 0xFFu] ^ (crc >> 8u);
            }
        }
        if (file.bad())
        {
            return false;
        }
        crcOut = ~crc;
        return true;
    }

    void pushDispatchPc(uint32_t pc)
    {
        DispatchHistory &h = g_dispatchHistory;
        h.pcs[h.next] = pc;
        h.next = (h.next + 1u) % static_cast<uint32_t>(h.pcs.size());
        if (h.next == 0u)
        {
            h.wrapped = true;
        }
    }

    std::string formatDispatchHistory()
    {
        const DispatchHistory &h = g_dispatchHistory;
        const uint32_t count = h.wrapped ? static_cast<uint32_t>(h.pcs.size()) : h.next;
        if (count == 0u)
        {
            return "(empty)";
        }

        std::ostringstream oss;
        bool first = true;
        for (uint32_t i = 0u; i < count; ++i)
        {
            const uint32_t idx = (h.next + h.pcs.size() - count + i) % static_cast<uint32_t>(h.pcs.size());
            if (!first)
            {
                oss << " -> ";
            }
            first = false;
            oss << "0x" << std::hex << h.pcs[idx];
        }
        return oss.str();
    }

    uint32_t selectExceptionVector(const R5900Context *ctx, bool tlbRefill)
    {
        if (ctx->cop0_status & COP0_STATUS_BEV)
        {
            return EXCEPTION_VECTOR_BOOT;
        }
        return tlbRefill ? EXCEPTION_VECTOR_TLB_REFILL : EXCEPTION_VECTOR_GENERAL;
    }

    void seedVu0IdleSuccess(R5900Context *ctx)
    {
        if (!ctx)
        {
            return;
        }

        ctx->vu0_clip_flags = 0;
        ctx->vu0_clip_flags2 = 0;
        ctx->vu0_mac_flags = 0;
        ctx->vu0_status = 0;
        ctx->vu0_q = 1.0f;
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));
        ctx->vu0_vpu_stat = 0;
        ctx->vu0_vpu_stat2 = 0;
    }

    void copyVu0ContextToState(const R5900Context *ctx, VU1State &state)
    {
        std::memset(&state, 0, sizeof(state));

        for (uint32_t i = 0; i < 32u; ++i)
        {
            _mm_storeu_ps(state.vf[i], ctx->vu0_vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            state.vi[i] = static_cast<int16_t>(ctx->vi[i]);
        }

        _mm_storeu_ps(state.acc, ctx->vu0_acc);
        state.q = ctx->vu0_q;
        state.p = ctx->vu0_p;
        state.i = ctx->vu0_i;
        alignas(16) uint32_t rWords[4]{};
        _mm_storeu_si128(reinterpret_cast<__m128i *>(rWords), _mm_castps_si128(ctx->vu0_r));
        state.r = 0x3F800000u | (rWords[0] & 0x007FFFFFu);
        state.pc = ctx->vu0_pc;
        state.mac = ctx->vu0_mac_flags;
        state.clip = ctx->vu0_clip_flags;
        state.status = ctx->vu0_status;
        state.itop = ctx->vu0_itop;
        state.dBitEnabled = (ctx->vu0_fbrst & (1u << 2)) != 0u;
        state.tBitEnabled = (ctx->vu0_fbrst & (1u << 3)) != 0u;

        state.vf[0][0] = 0.0f;
        state.vf[0][1] = 0.0f;
        state.vf[0][2] = 0.0f;
        state.vf[0][3] = 1.0f;
        state.vi[0] = 0;
    }

    void copyVu0StateToContext(const VU1State &state, R5900Context *ctx)
    {
        for (uint32_t i = 0; i < 32u; ++i)
        {
            ctx->vu0_vf[i] = _mm_loadu_ps(state.vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            ctx->vi[i] = static_cast<uint16_t>(state.vi[i]);
        }

        ctx->vu0_acc = _mm_loadu_ps(state.acc);
        ctx->vu0_q = state.q;
        ctx->vu0_p = state.p;
        ctx->vu0_i = state.i;
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(static_cast<int32_t>(state.r)));
        ctx->vu0_mac_flags = state.mac;
        ctx->vu0_clip_flags = state.clip;
        ctx->vu0_clip_flags2 = state.clip;
        ctx->vu0_status = static_cast<uint16_t>(state.status);
        ctx->vu0_itop = state.itop;
        ctx->vu0_pc = state.pc;
        ctx->vu0_tpc = state.pc;
        ctx->vu0_vpu_stat = (ctx->vu0_vpu_stat & 0xFF00u) | (state.stoppedByD ? (1u << 1) : 0u) | (state.stoppedByT ? (1u << 2) : 0u);
        ctx->vu0_vpu_stat2 = 0;

        ctx->vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
        ctx->vi[0] = 0;
    }

    void raiseCop0Exception(R5900Context *ctx, uint32_t exceptionCode, bool tlbRefill = false)
    {
        if (ctx->in_delay_slot)
        {
            ctx->cop0_epc = ctx->branch_pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~COP0_CAUSE_EXCCODE_MASK) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK) |
                              COP0_CAUSE_BD;
        }
        else
        {
            ctx->cop0_epc = ctx->pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~(COP0_CAUSE_EXCCODE_MASK | COP0_CAUSE_BD)) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK);
        }

        ctx->cop0_status |= COP0_STATUS_EXL;
        ctx->pc = selectExceptionVector(ctx, tlbRefill);
        ctx->in_delay_slot = false;
    }

    [[noreturn]] void raiseTlbAccessFault(R5900Context *ctx, const TlbFault &fault)
    {
        const bool nested = (ctx->cop0_status & COP0_STATUS_EXL) != 0u;
        const uint32_t code = fault.kind == TlbFault::Kind::Modified ? 1u : fault.store ? 3u : 2u;
        ctx->cop0_badvaddr = fault.address;
        ctx->cop0_context = (ctx->cop0_context & 0xFF800000u) | ((fault.address >> 9u) & 0x007FFFF0u);
        ctx->cop0_entryhi = (fault.address & 0xFFFFE000u) | (ctx->cop0_entryhi & 0xFFu);
        if (!nested)
        {
            ctx->cop0_epc = ctx->in_delay_slot ? ctx->branch_pc : ctx->pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~COP0_CAUSE_BD) | (ctx->in_delay_slot ? COP0_CAUSE_BD : 0u);
        }
        ctx->cop0_cause = (ctx->cop0_cause & ~COP0_CAUSE_EXCCODE_MASK) | (code << 2u);
        const bool refill = fault.kind == TlbFault::Kind::Refill && !nested;
        // EE Core v6 table4-1: refill at +0; invalid/modified/nested at +0x180.
        ctx->pc = ((ctx->cop0_status & COP0_STATUS_BEV) ? 0xBFC00200u : 0x80000000u) + (refill ? 0u : 0x180u);
        ctx->cop0_status |= COP0_STATUS_EXL;
        ctx->in_delay_slot = false;
        // Reuse the EE dispatcher's existing control transfer; no interpreter.
        throw EeDispatcherTransfer{};
    }

    std::filesystem::path normalizeAbsolutePath(const std::filesystem::path &path)
    {
        if (path.empty())
        {
            return {};
        }

#if defined(PLATFORM_VITA)
        const std::string generic = path.generic_string();
        const std::size_t colon = generic.find(':');
        if (colon != std::string::npos && colon != 0u)
        {
            const std::size_t slash = generic.find_first_of("/\\");
            if (slash == std::string::npos || colon < slash)
            {
                return path.lexically_normal();
            }
        }
#endif

        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
        if (ec)
        {
            return path.lexically_normal();
        }
        return absolute.lexically_normal();
    }

    PS2Runtime::IoPaths &runtimeIoPaths()
    {
        static PS2Runtime::IoPaths paths = []()
        {
            PS2Runtime::IoPaths defaults;
            std::error_code ec;
            const std::filesystem::path cwd = std::filesystem::current_path(ec);
            defaults.elfDirectory = ec ? std::filesystem::path(".") : cwd.lexically_normal();
            defaults.hostRoot = defaults.elfDirectory;
            defaults.cdRoot = defaults.elfDirectory;
            defaults.mcRoot = defaults.elfDirectory / "mc0";
            return defaults;
        }();

        return paths;
    }

    std::string readGuestPrintableString(const uint8_t *rdram, uint32_t addr, size_t maxLen)
    {
        std::string out;
        if (!rdram || maxLen == 0)
        {
            return out;
        }

        out.reserve(std::min<size_t>(maxLen, 64));
        for (size_t i = 0; i < maxLen; ++i)
        {
            const char ch = static_cast<char>(rdram[(addr + static_cast<uint32_t>(i)) & PS2_RAM_MASK]);
            if (ch == '\0')
            {
                break;
            }
            if (ch >= 0x20 && ch < 0x7F)
            {
                out.push_back(ch);
            }
            else
            {
                out.push_back('.');
            }
        }
        return out;
    }
}

// ---------------------------------------------------------------------------
// KFIV-PC dev harness (env-driven, no header changes):
//   PS2X_HIDDEN=1            create the window hidden (headless-ish runs)
//   PS2X_LATE_VBLANK=1       log fields emitted >3 ms after their wall-clock time ([late-vblank])
//   PS2X_HUD_LOG=1           log the F2 overlay numbers every 0.5 s ([hud] tick fps vsync frameMs maxFrameMs)
//   PS2X_DUMP_DIR=<dir>      where frame PNGs go (default: cwd)
//   PS2X_DUMP_EVERY=<n>      dump the presented frame every n vsync ticks
//   PS2X_EXIT_TICK=<n>       dump final.png and _Exit(0) at vsync tick n
//   PS2X_INPUT="t:BTN[+BTN]:dur,..."  scripted pad presses at vsync tick t
//   PS2X_CTRL=<file>         command file polled ~10x/s, consumed on read:
//       press BTN[+BTN] <ticks> | shot <path.png> | hostshot <path.png> | quit | tick <path.txt>
//       threads <path.txt>   (EE threads, semaphores, event flags)
//   PS2X_THREADS_AT_EXIT=1   print the thread dump to stderr at the exit tick
// Buttons: UP DOWN LEFT RIGHT START SELECT CROSS CIRCLE SQUARE TRIANGLE
//          L1 R1 L2 R2 L3 R3
// ---------------------------------------------------------------------------
namespace ps2x_dev
{
    std::atomic<uint32_t> g_padPressedMask{0u}; // read by ps2_pad.cpp

    struct PressEvent
    {
        uint64_t start;
        uint64_t duration;
        uint32_t mask;
    };

    static std::vector<uint8_t> s_lastFrame;
    static uint32_t s_lastFrameW = 0u;
    static uint32_t s_lastFrameH = 0u;
    static uint32_t s_lastFrameRows = 1u; // rows in s_lastFrame (FB_HEIGHT)

    static uint32_t buttonMask(const std::string &nameIn)
    {
        std::string n;
        for (char c : nameIn)
            n.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        static const std::unordered_map<std::string, uint32_t> k{
            {"LEFT", 0x0080u}, {"DOWN", 0x0040u}, {"RIGHT", 0x0020u}, {"UP", 0x0010u},
            {"START", 0x0008u}, {"R3", 0x0004u}, {"L3", 0x0002u}, {"SELECT", 0x0001u},
            {"SQUARE", 0x8000u}, {"CROSS", 0x4000u}, {"X", 0x4000u}, {"CIRCLE", 0x2000u},
            {"O", 0x2000u}, {"TRIANGLE", 0x1000u}, {"R1", 0x0800u}, {"L1", 0x0400u},
            {"R2", 0x0200u}, {"L2", 0x0100u}};
        const auto it = k.find(n);
        if (it == k.end())
        {
            std::cerr << "[dev] unknown button '" << nameIn << "'" << std::endl;
            return 0u;
        }
        return it->second;
    }

    static uint32_t parseButtons(const std::string &spec)
    {
        uint32_t mask = 0u;
        std::stringstream ss(spec);
        std::string part;
        while (std::getline(ss, part, '+'))
            mask |= buttonMask(part);
        return mask;
    }

    static uint64_t envU64(const char *name, uint64_t def)
    {
        const char *v = std::getenv(name);
        return (v && *v) ? std::strtoull(v, nullptr, 0) : def;
    }

    static bool writePng(const std::string &path)
    {
        if (s_lastFrame.empty() || s_lastFrameW == 0u || s_lastFrameH == 0u)
            return false;
        // s_lastFrame rows are FB_WIDTH (640) pixels apart
        const size_t stride = s_lastFrame.size() / std::max<size_t>(1u, s_lastFrameRows) ;
        std::vector<uint8_t> px(static_cast<size_t>(s_lastFrameW) * s_lastFrameH * 4u);
        for (uint32_t y = 0; y < s_lastFrameH; ++y)
            std::memcpy(px.data() + static_cast<size_t>(y) * s_lastFrameW * 4u,
                        s_lastFrame.data() + static_cast<size_t>(y) * stride,
                        static_cast<size_t>(s_lastFrameW) * 4u);
        for (size_t i = 3; i < px.size(); i += 4)
            px[i] = 0xFF;
        Image img{};
        img.data = px.data();
        img.width = static_cast<int>(s_lastFrameW);
        img.height = static_cast<int>(s_lastFrameH);
        img.mipmaps = 1;
        img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        const bool ok = ExportImage(img, path.c_str());
        std::cerr << "[dev] shot " << path << (ok ? " ok" : " FAILED") << std::endl;
        return ok;
    }

    static std::string dumpDir()
    {
        const char *d = std::getenv("PS2X_DUMP_DIR");
        return (d && *d) ? std::string(d) : std::string(".");
    }

    static void writeThreads(std::ostream &o, PS2Runtime *rt)
    {
        if (!rt)
            return;
        static const char *kStatus[] = {"Running", "Ready", "Waiting", "WaitingSuspended", "Suspended", "Dormant"};
        static const char *kWait[] = {"None", "Sleep", "Semaphore", "EventFlag", "VSync", "External", "Mpeg"};
        const EeKernelSnapshot snap = rt->eeScheduler().snapshot();
        o << "running=" << snap.runningThreadId << " eeCycle=" << snap.eeCycle << "\n";
        for (const EeThreadSnapshot &t : snap.threads)
        {
            o << std::hex << "thread " << std::dec << t.id
              << " status=" << kStatus[std::min<size_t>(static_cast<size_t>(t.status), 5u)]
              << " wait=" << kWait[std::min<size_t>(static_cast<size_t>(t.waitReason), 6u)]
              << " waitId=" << t.waitId << " prio=" << t.currentPriority << std::hex
              << " pc=0x" << t.pc << " ra=0x" << t.ra << " sp=0x" << t.sp
              << " entry=0x" << t.entry << std::dec << " wakeups=" << t.wakeupCount << "\n";
        }
        for (const EeSemaphoreSnapshot &sem : snap.semaphores)
            o << "sema " << sem.id << " count=" << sem.count << "/" << sem.maxCount << " waiters=" << sem.waiters << "\n";
        for (const EeEventFlagSnapshot &ef : snap.eventFlags)
            o << std::hex << "evf " << std::dec << ef.id << std::hex << " bits=0x" << ef.bits << std::dec << " waiters=" << ef.waiters << "\n";
        o.flush();
    }

    struct Harness
    {
        PS2Runtime *runtime = nullptr;
        std::vector<PressEvent> events;
        uint64_t dumpEvery = 0u;
        uint64_t exitTick = 0u;
        uint64_t lastDumpBucket = std::numeric_limits<uint64_t>::max();
        std::string ctrlPath;
        std::vector<std::string> hostShots;
        std::chrono::steady_clock::time_point lastCtrlPoll{};
        bool stats = false;
        std::chrono::steady_clock::time_point statsStart{};
        uint64_t statsTick = 0u;
        uint64_t statsFlips = 0u;
        uint64_t lastDispfb = 0u;

        void init()
        {
            dumpEvery = envU64("PS2X_DUMP_EVERY", 0u);
            exitTick = envU64("PS2X_EXIT_TICK", 0u);
            if (const char *c = std::getenv("PS2X_CTRL"))
                ctrlPath = c;
            if (const char *st = std::getenv("PS2X_STATS"))
                stats = *st && *st != '0';
            if (const char *in = std::getenv("PS2X_INPUT"))
            {
                std::stringstream ss(in);
                std::string item;
                while (std::getline(ss, item, ','))
                {
                    const size_t a = item.find(':');
                    const size_t b = item.find(':', a == std::string::npos ? a : a + 1);
                    if (a == std::string::npos)
                        continue;
                    PressEvent ev{};
                    ev.start = std::strtoull(item.substr(0, a).c_str(), nullptr, 0);
                    ev.mask = parseButtons(item.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1));
                    ev.duration = (b == std::string::npos) ? 6u : std::strtoull(item.substr(b + 1).c_str(), nullptr, 0);
                    events.push_back(ev);
                }
            }
            std::cerr << "[dev] harness: dumpEvery=" << dumpEvery << " exitTick=" << exitTick
                      << " events=" << events.size() << " ctrl=" << ctrlPath << std::endl;
        }

        void pollCtrl(uint64_t tick)
        {
            if (ctrlPath.empty())
                return;
            const auto now = std::chrono::steady_clock::now();
            if (now - lastCtrlPoll < std::chrono::milliseconds(100))
                return;
            lastCtrlPoll = now;
            std::ifstream f(ctrlPath);
            if (!f)
                return;
            std::vector<std::string> lines;
            std::string line;
            while (std::getline(f, line))
                lines.push_back(line);
            f.close();
            std::remove(ctrlPath.c_str());
            uint64_t at = tick;
            for (const std::string &l : lines)
            {
                std::stringstream ls(l);
                std::string cmd;
                ls >> cmd;
                if (cmd == "press")
                {
                    std::string btns;
                    uint64_t dur = 6u;
                    ls >> btns >> dur;
                    events.push_back(PressEvent{at, dur, parseButtons(btns)});
                    at += dur + 4u; // queue sequential presses with a release gap
                }
                else if (cmd == "shot")
                {
                    std::string path;
                    ls >> path;
                    writePng(path);
                }
                else if (cmd == "hostshot")
                {
                    std::string path;
                    std::getline(ls >> std::ws, path);
                    if (!path.empty()) hostShots.push_back(path);
                }
                else if (cmd == "threads")
                {
                    std::string path;
                    ls >> path;
                    std::ofstream o(path);
                    writeThreads(o, runtime);
                }
                else if (cmd == "tick")
                {
                    std::string path;
                    ls >> path;
                    std::ofstream o(path);
                    o << tick << std::endl;
                }
                else if (cmd == "quit")
                {
                    std::cerr << "[dev] quit at tick " << tick << std::endl;
                    ps2x::device::quiesceAll(std::chrono::milliseconds(2000));
                    ps2prof::flush(); // DW5XL-DEBUG issue 025: _Exit skips atexit
                    GSTrace::shutdown();
                    if (runtime) runtime->audioBackend().stopPcmStream();
                    std::cout.flush();
                    std::cerr.flush();
                    ps2x::stdio_pump::drain(); // issue 025: _Exit skips atexit; write out the queued log
                    std::_Exit(0);
                }
            }
        }

        void captureHostFrame()
        {
            if (hostShots.empty()) return;
            // Flush the game and overlay into the backbuffer before EndDrawing
            // swaps it. ExportImage preserves the requested absolute path.
            rlDrawRenderBatchActive();
            Image image = LoadImageFromScreen();
            for (const std::string &path : hostShots)
            {
                const bool ok = image.data && ExportImage(image, path.c_str());
                const auto indicator = ps2_host_input::mouseCaptureIndicator();
                std::cerr << "[dev] hostshot " << path << (ok ? " ok" : " FAILED")
                          << " mouse=" << indicator.captured << " alpha=" << indicator.alpha << std::endl;
            }
            UnloadImage(image);
            hostShots.clear();
        }

        // PS2X_STATS=1: once per wall-clock second, print guest VSync ticks
        // and display flips (DISPFB1/DISPFB2 changes, sampled per host frame).
        void stepStats(uint64_t tick)
        {
            if (!stats || !runtime)
                return;
            const uint64_t dispfb = runtime->memory().gs().dispfb1 ^ (runtime->memory().gs().dispfb2 << 1);
            if (dispfb != lastDispfb)
            {
                lastDispfb = dispfb;
                ++statsFlips;
            }
            const auto now = std::chrono::steady_clock::now();
            if (statsStart == std::chrono::steady_clock::time_point{})
            {
                statsStart = now;
                statsTick = tick;
                statsFlips = 0u;
                return;
            }
            const double secs = std::chrono::duration<double>(now - statsStart).count();
            if (secs < 1.0)
                return;
            static const auto s_statsEpoch = now;
            std::fprintf(stderr, "[stats] t=%.2f tick=%llu vsync/s=%.1f flips/s=%.1f\n",
                         std::chrono::duration<double>(now - s_statsEpoch).count(),
                         static_cast<unsigned long long>(tick),
                         static_cast<double>(tick - statsTick) / secs,
                         static_cast<double>(statsFlips) / secs);
            statsStart = now;
            statsTick = tick;
            statsFlips = 0u;
        }

        // DW5XL-DEBUG: PS2X_RAM_DUMP_TICKS="t1,t2" writes ram_<tick>.bin (32 MiB RDRAM) to the dump dir;
        // PS2X_RAM_WATCH="addr:len" logs every tick on which that RDRAM range changed, with its new bytes.
        std::vector<uint64_t> ramDumpTicks;
        uint32_t ramWatchAddr = 0u, ramWatchLen = 0u;
        std::vector<uint8_t> ramWatchLast;
        bool ramDebugInit = false;

        void stepRamDebug(uint64_t tick)
        {
            if (!runtime)
                return;
            if (!ramDebugInit)
            {
                ramDebugInit = true;
                if (const char *t = std::getenv("PS2X_RAM_DUMP_TICKS"))
                {
                    std::stringstream ss(t);
                    std::string item;
                    while (std::getline(ss, item, ','))
                        ramDumpTicks.push_back(std::strtoull(item.c_str(), nullptr, 0));
                }
                if (const char *w = std::getenv("PS2X_RAM_WATCH"))
                {
                    char *end = nullptr;
                    ramWatchAddr = static_cast<uint32_t>(std::strtoul(w, &end, 0)) & PS2_RAM_MASK;
                    ramWatchLen = (end && *end == ':') ? static_cast<uint32_t>(std::strtoul(end + 1, nullptr, 0)) : 16u;
                    ramWatchLen = std::min<uint32_t>(ramWatchLen, PS2_RAM_SIZE - ramWatchAddr);
                }
            }
            const uint8_t *ram = runtime->memory().getRDRAM();
            for (uint64_t &t : ramDumpTicks)
            {
                if (t != 0u && tick >= t)
                {
                    char name[64];
                    std::snprintf(name, sizeof(name), "/ram_%06llu.bin", static_cast<unsigned long long>(tick));
                    if (FILE *f = std::fopen((dumpDir() + name).c_str(), "wb"))
                    {
                        std::fwrite(ram, 1, PS2_RAM_SIZE, f);
                        std::fclose(f);
                    }
                    t = 0u;
                }
            }
            if (ramWatchLen != 0u)
            {
                const uint8_t *cur = ram + ramWatchAddr;
                if (ramWatchLast.size() != ramWatchLen || std::memcmp(ramWatchLast.data(), cur, ramWatchLen) != 0)
                {
                    ramWatchLast.assign(cur, cur + ramWatchLen);
                    std::fprintf(stderr, "[ramwatch] tick=%llu addr=%08x:", static_cast<unsigned long long>(tick), ramWatchAddr);
                    for (uint32_t i = 0; i + 4u <= ramWatchLen; i += 4u)
                    {
                        uint32_t v;
                        std::memcpy(&v, cur + i, 4u);
                        std::fprintf(stderr, "%s%08x", (i % 16u) == 0u ? " | " : " ", v);
                    }
                    std::fprintf(stderr, "\n");
                }
            }
        }

        void step(uint64_t tick)
        {
            GSTrace::onTick(tick);
            stepRamDebug(tick);
            pollCtrl(tick);
            stepStats(tick);
            uint32_t mask = 0u;
            for (const PressEvent &ev : events)
                if (tick >= ev.start && tick < ev.start + ev.duration)
                    mask |= ev.mask;
            g_padPressedMask.store(mask, std::memory_order_relaxed);

            if (dumpEvery != 0u)
            {
                const uint64_t bucket = tick / dumpEvery;
                if (bucket != lastDumpBucket)
                {
                    lastDumpBucket = bucket;
                    char name[64];
                    std::snprintf(name, sizeof(name), "/frame_%06llu.png", static_cast<unsigned long long>(tick));
                    writePng(dumpDir() + name);
                }
            }
            if (exitTick != 0u && tick >= exitTick)
            {
                writePng(dumpDir() + "/final.png");
                if (const char *t = std::getenv("PS2X_THREADS_AT_EXIT"); t && *t && *t != '0')
                    writeThreads(std::cerr, runtime);
                std::cerr << "[dev] exit tick " << tick << " reached" << std::endl;
                ps2x::device::quiesceAll(std::chrono::milliseconds(2000));
                ps2prof::flush(); // DW5XL-DEBUG issue 025: _Exit skips atexit
                GSTrace::shutdown();
                if (runtime) runtime->audioBackend().stopPcmStream();
                std::cout.flush();
                std::cerr.flush();
                ps2x::stdio_pump::drain(); // issue 025: _Exit skips atexit; write out the queued log
                std::_Exit(0);
            }
        }
    };
}

namespace
{
// The settings file: dw5xl.ini in the working directory. DW5XL-DEBUG: PS2X_SETTINGS_FILE=<path> uses another file
// (test runs, so they never change the player's settings).
static const char *iniPath()
{
    static const std::string path = [] { const char *v = std::getenv("PS2X_SETTINGS_FILE"); return std::string(v && *v ? v : "dw5xl.ini"); }();
    return path.c_str();
}

// The value of `wantedKey` in the settings file, trimmed; false if the file or the key is missing.
static bool loadIniString(const char *wantedKey, std::string &out)
{
    std::ifstream file(iniPath());
    if (!file.is_open())
        return false;
    std::string line;
    while (std::getline(file, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        size_t commentPos = line.find_first_of(";#");
        if (commentPos != std::string::npos)
            line.erase(commentPos);

        size_t eqPos = line.find('=');
        if (eqPos == std::string::npos)
            continue;

        std::string key = line.substr(0, eqPos);
        std::string val = line.substr(eqPos + 1);

        auto trim = [](std::string &s) {
            size_t first = s.find_first_not_of(" \t");
            if (first == std::string::npos) { s.clear(); return; }
            size_t last = s.find_last_not_of(" \t");
            s = s.substr(first, (last - first + 1));
        };
        trim(key);
        trim(val);

        if (key == wantedKey)
        {
            out = val;
            return true;
        }
    }
    return false;
}

// Unknown or missing values return fallback. Older SSAA-based scale values are
// treated as native output (anti-aliasing is configured separately).
static uint32_t loadIniValue(const char *wantedKey, std::initializer_list<uint32_t> allowed, uint32_t fallback)
{
    std::string val;
    if (!loadIniString(wantedKey, val))
        return fallback;
    try {
        const int value = std::stoi(val);
        for (uint32_t a : allowed)
            if (value >= 0 && static_cast<uint32_t>(value) == a)
                return a;
        return fallback;
    } catch (...) {
        return fallback;
    }
}

static uint32_t loadResolutionScaleFromIni() { return loadIniValue("resolution_scale", {1u, 2u, 4u}, 1u); }
static uint32_t loadAASamplesFromIni() { return loadIniValue("aa_samples", {1u, 2u, 4u, 8u, 16u}, 0u); }

static void saveIniText(const std::string &wantedKey, const std::string &value)
{
    std::vector<std::string> lines;
    bool found = false;
    {
        std::ifstream file(iniPath());
        if (file.is_open())
        {
            std::string line;
            while (std::getline(file, line))
            {
                std::string rawLine = line;
                if (!rawLine.empty() && rawLine.back() == '\r')
                    rawLine.pop_back();

                std::string trimmed = rawLine;
                std::string comment;
                size_t commentPos = trimmed.find_first_of(";#");
                if (commentPos != std::string::npos)
                {
                    comment = trimmed.substr(commentPos);
                    trimmed.erase(commentPos);
                }

                size_t eqPos = trimmed.find('=');
                if (eqPos != std::string::npos)
                {
                    std::string key = trimmed.substr(0, eqPos);
                    size_t first = key.find_first_not_of(" \t");
                    if (first != std::string::npos)
                    {
                        size_t last = key.find_last_not_of(" \t");
                        key = key.substr(first, (last - first + 1));
                    }
                    else
                    {
                        key.clear();
                    }

                    if (key == wantedKey)
                    {
                        std::string newLine = wantedKey + "=" + value;
                        if (!comment.empty())
                            newLine += " " + comment;
                        lines.push_back(newLine);
                        found = true;
                        continue;
                    }
                }
                lines.push_back(rawLine);
            }
        }
    }

    if (!found)
    {
        lines.push_back(wantedKey + "=" + value);
    }

    std::ofstream out(iniPath());
    if (out.is_open())
    {
        for (const auto &l : lines)
        {
            out << l << "\n";
        }
    }
}

static void saveIniValue(const std::string &wantedKey, uint32_t value) { saveIniText(wantedKey, std::to_string(value)); }

// The player's F3 anti-aliasing choice (kept when F4 changes the output scale).
static uint32_t s_aaChoice = 1u;

// Environment variables override the saved F4 settings for test runs.
static void applySavedOutputSettings(GS &gs)
{
    const char *scaleEnv = std::getenv("PS2X_GS_SCALE");
    if (!scaleEnv || !*scaleEnv)
    {
        const uint32_t iniScale = loadResolutionScaleFromIni();
        if (iniScale != gs.getResolutionScale())
            gs.setResolutionScale(iniScale);
    }
    const char *aaEnv = std::getenv("PS2X_GS_SSAA");
    if (!aaEnv || !*aaEnv)
    {
        if (const uint32_t iniSamples = loadAASamplesFromIni())
        {
            s_aaChoice = iniSamples;
            gs.setAASamples(iniSamples);
        }
    }
    // Always state the effective output so test logs show which scale a run used.
    std::fprintf(stderr, "[output] scale=%ux aa=%ux (source: scale %s, aa %s)\n", gs.getResolutionScale(), gs.getAASamples(),
                 (scaleEnv && *scaleEnv) ? "PS2X_GS_SCALE" : "dw5xl.ini", (aaEnv && *aaEnv) ? "PS2X_GS_SSAA" : "dw5xl.ini");
}

// Issue 023 step 3S: vsync and window mode are player settings, `vsync=on|off` (off by default for now) and
// `display=window|fullscreen` in dw5xl.ini; PS2X_VSYNC=on|off (or 1|0) overrides vsync. `source` names where the
// value came from (for the log).
static bool parseOnOff(const std::string &v, bool &on)
{
    if (v == "on" || v == "1" || v == "true") { on = true; return true; }
    if (v == "off" || v == "0" || v == "false") { on = false; return true; }
    return false;
}
static bool loadVsyncSetting(const char *&source)
{
    bool on = false;
    if (const char *env = std::getenv("PS2X_VSYNC"); env && parseOnOff(env, on)) { source = "PS2X_VSYNC"; return on; }
    std::string v;
    if (loadIniString("vsync", v) && parseOnOff(v, on)) { source = "dw5xl.ini"; return on; }
    source = "default";
    return false;
}
// Issue 023 3S: the F2 frame rate overlay is a player setting too, `show_fps=on|off` (off by default); PS2X_FPS=1
// turns it on for a run.
static bool loadShowFpsSetting()
{
    if (const char *env = std::getenv("PS2X_FPS"); env && *env && *env != '0') return true;
    std::string v;
    bool on = false;
    return loadIniString("show_fps", v) && parseOnOff(v, on) && on;
}
static bool loadFullscreenSetting()
{
    std::string v;
    return loadIniString("display", v) && v == "fullscreen";
}
} // namespace

namespace ps2x::settings
{
    bool loadText(const char *key, std::string &value) { return loadIniString(key, value); }
    void saveText(const std::string &key, const std::string &value) { saveIniText(key, value); }
    void saveNumber(const std::string &key, uint32_t value) { saveIniValue(key, value); }
    void setAntiAliasing(GS &gs, uint32_t samples)
    {
        s_aaChoice = samples;
        gs.setAASamples(samples);
        saveIniValue("aa_samples", samples);
    }
    void setResolutionScale(GS &gs, uint32_t scale)
    {
        gs.setResolutionScale(scale);
        saveIniValue("resolution_scale", gs.getResolutionScale());
    }
}



// Host window rate: field pacing by default, the window paced on every guest VBlankEnd (one host frame per field;
// issue 015; the default since issue 023, owner 2026-10-24, after its jump was fixed by reading each field at its end
// on the GS side). PS2X_HOST_FPS=<n> sets a fixed rate instead (60 = the earlier default); PS2X_HOST_FPS=field is
// the default spelled out. Returns 0 for field pacing.
static int configuredHostTargetFps()
{
    if (const char *env = std::getenv("PS2X_HOST_FPS"))
    {
        if (std::strcmp(env, "field") == 0)
            return 0;
        const int fps = std::atoi(env);
        if (fps > 0)
            return fps;
    }
    return 0;
}

// DW5XL-DEBUG issue 023 3S: the field the window frame being drawn shows and whether it carries a new picture
// (set by UploadFrame, logged after the swap with PS2X_PRESENT_LOG).
static uint64_t s_windowFrameField = 0;
static bool s_windowFrameNewPicture = false;

// The texture grows to the largest presented image (4x output); smaller frames
// occupy its top-left corner. Display dimensions keep the logical aspect ratio.
static void UploadFrame(Texture2D &tex, PS2Runtime *rt, uint32_t &outWidth, uint32_t &outHeight,
                        uint32_t &outDisplayWidth, uint32_t &outDisplayHeight)
{
    static uint64_t s_lastPresentationTick = std::numeric_limits<uint64_t>::max();
    static uint64_t s_lastUploadedGeneration = 0u;
    static bool s_hasLatchedInitialFrame = false;
    static uint32_t s_lastDisplayFbp = std::numeric_limits<uint32_t>::max();
    static uint32_t s_lastSourceFbp = std::numeric_limits<uint32_t>::max();
    static bool s_lastPreferred = false;
    static uint32_t s_lastWidth = 0u;
    static uint32_t s_lastHeight = 0u;
    static uint32_t s_lastDisplayWidth = 0u;
    static uint32_t s_lastDisplayHeight = 0u;
    static bool s_hasUploadedFrame = false;

    const uint64_t currentTick = rt->eeScheduler().currentVSyncTick();
    // DW5XL-DEBUG issue 023: PS2X_PRESENT_STATS=1 prints once a second the window frames, new pictures,
    // field reads copied, copies of more than one field (skipped fields), frames over 25 ms and the longest gap.
    {
        static const bool s_stats = [] { const char *v = std::getenv("PS2X_PRESENT_STATS"); return v && *v && *v != '0'; }();
        static uint64_t s_frames = 0, s_slow = 0, s_windowStartNs = 0, s_lastNs = 0, s_lastGen = 0, s_newPictures = 0;
        static double s_maxMs = 0.0;
        if (s_stats)
        {
            const uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
            if (s_lastNs != 0u)
            {
                const double ms = static_cast<double>(now - s_lastNs) / 1e6;
                s_maxMs = std::max(s_maxMs, ms);
                s_slow += ms > 25.0 ? 1u : 0u;
            }
            if (s_windowStartNs == 0u)
                s_windowStartNs = now;
            const uint64_t gen = rt->gs().hostPresentationGeneration();
            s_newPictures += gen != s_lastGen ? 1u : 0u;
            s_lastGen = gen;
            ++s_frames;
            s_lastNs = now;
            if (now - s_windowStartNs >= 1000000000ull)
            {
                std::fprintf(stderr, "[present:stats] tick=%llu frames=%llu newPictures=%llu readsCopied=%llu multiReadCopies=%llu slow=%llu maxMs=%.1f\n",
                             static_cast<unsigned long long>(currentTick), static_cast<unsigned long long>(s_frames),
                             static_cast<unsigned long long>(s_newPictures),
                             static_cast<unsigned long long>(g_dw5xlReadsCopied.exchange(0u)),
                             static_cast<unsigned long long>(g_dw5xlMultiReadCopies.exchange(0u)),
                             static_cast<unsigned long long>(s_slow), s_maxMs);
                s_frames = s_slow = s_newPictures = 0u;
                s_maxMs = 0.0;
                s_windowStartNs = now;
            }
        }
    }
    // Fields read on the GS side (issue 023) finish a little after their field ends: the window collects every frame,
    // so a field finished between two field changes is not skipped (a frame without a new field uploads nothing).
    const bool needsLatch = !s_hasLatchedInitialFrame || currentTick != s_lastPresentationTick ||
                            rt->gs().fieldScanoutsActive();
    if (needsLatch)
    {
        // Field pacing shows the field that has just ended (issue 023: read on the GS side at its end).
        static const bool s_fieldPaced = configuredHostTargetFps() == 0;
        rt->gs().latchHostPresentationFrame(s_fieldPaced && currentTick > 0u ? currentTick - 1u
                                                                               : std::numeric_limits<uint64_t>::max());
        s_windowFrameField = s_fieldPaced && currentTick > 0u ? currentTick - 1u : currentTick;
        s_lastPresentationTick = currentTick;
        s_hasLatchedInitialFrame = true;
    }

    const uint64_t generation = rt->gs().hostPresentationGeneration();
    // DW5XL-DEBUG issue 023: PS2X_WEAVE_TRACE=1 also logs every window frame: field it shows, whether it latched a new
    // picture and whether that picture was uploaded (a repeat shows the previous one again).
    static const bool s_presentTrace = [] { const char *v = std::getenv("PS2X_WEAVE_TRACE"); return v && *v && *v != '0'; }();
    if (s_presentTrace)
    {
        static uint64_t s_hostFrame = 0u;
        const auto nowNs = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now().time_since_epoch()).count();
        std::fprintf(stderr, "[present] host=%llu us=%lld tick=%llu latched=%d gen=%llu upload=%d\n",
                     static_cast<unsigned long long>(s_hostFrame++), static_cast<long long>(nowNs),
                     static_cast<unsigned long long>(currentTick), needsLatch ? 1 : 0,
                     static_cast<unsigned long long>(generation),
                     (!s_hasUploadedFrame || (needsLatch && generation != s_lastUploadedGeneration)) ? 1 : 0);
    }
    s_windowFrameNewPicture = false;
    if (s_hasUploadedFrame && (!needsLatch || generation == s_lastUploadedGeneration))
    {
        outWidth = (s_lastWidth != 0u) ? s_lastWidth : FB_WIDTH;
        outHeight = (s_lastHeight != 0u) ? s_lastHeight : DEFAULT_DISPLAY_HEIGHT;
        outDisplayWidth = (s_lastDisplayWidth != 0u) ? s_lastDisplayWidth : outWidth;
        outDisplayHeight = (s_lastDisplayHeight != 0u) ? s_lastDisplayHeight : outHeight;
        return;
    }

    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t displayFbp = 0u;
    uint32_t sourceFbp = 0u;
    bool usedPreferredDisplaySource = false;
    uint32_t displayWidth = 0u;
    uint32_t displayHeight = 0u;
    if (!rt->gs().copyLatchedHostPresentationFrame(ps2x_dev::s_lastFrame,
                                                   width,
                                                   height,
                                                   &displayFbp,
                                                   &sourceFbp,
                                                   &usedPreferredDisplaySource,
                                                   &displayWidth,
                                                   &displayHeight))
    {
        Image blank = GenImageColor(tex.width, tex.height, MAGENTA);
        UpdateTexture(tex, blank.data);
        UnloadImage(blank);
        outWidth = FB_WIDTH;
        outHeight = DEFAULT_DISPLAY_HEIGHT;
        outDisplayWidth = outWidth;
        outDisplayHeight = outHeight;
        s_lastWidth = outWidth;
        s_lastHeight = outHeight;
        s_lastDisplayWidth = outDisplayWidth;
        s_lastDisplayHeight = outDisplayHeight;
        s_hasUploadedFrame = true;
        return;
    }

    PS2_IF_AGRESSIVE_LOGS({
        static uint32_t s_uploadDebugCount = 0u;
        if (s_uploadDebugCount < 128u ||
            displayFbp != s_lastDisplayFbp ||
            sourceFbp != s_lastSourceFbp ||
            usedPreferredDisplaySource != s_lastPreferred ||
            width != s_lastWidth ||
            height != s_lastHeight)
        {
            std::cout << "[frame:upload] idx=" << s_uploadDebugCount
                      << " tick=" << currentTick
                      << " displayFbp=" << displayFbp
                      << " sourceFbp=" << sourceFbp
                      << " size=" << width << "x" << height
                      << " preferred=" << static_cast<uint32_t>(usedPreferredDisplaySource ? 1u : 0u)
                      << std::endl;
        }
        ++s_uploadDebugCount;
    });
    s_lastDisplayFbp = displayFbp;
    s_lastSourceFbp = sourceFbp;
    s_lastPreferred = usedPreferredDisplaySource;
    s_lastUploadedGeneration = generation;

    // 4x output is at most four times the native framebuffer in each direction.
    const uint32_t copyWidth = std::min<uint32_t>(width, FB_WIDTH * 4u);
    const uint32_t copyHeight = std::min<uint32_t>(height, FB_HEIGHT * 4u);
    const uint32_t texWidth = std::max<uint32_t>(FB_WIDTH, copyWidth);
    const uint32_t texHeight = std::max<uint32_t>(FB_HEIGHT, copyHeight);
    if (texWidth != static_cast<uint32_t>(tex.width) || texHeight != static_cast<uint32_t>(tex.height))
    {
        UnloadTexture(tex);
        Image blank = GenImageColor(static_cast<int>(texWidth), static_cast<int>(texHeight), BLANK);
        tex = LoadTextureFromImage(blank);
        UnloadImage(blank);
    }

    if (width == texWidth && height == texHeight)
    {
        UpdateTexture(tex, ps2x_dev::s_lastFrame.data());
    }
    else
    {
        static std::vector<uint8_t> s_paddedUploadBuffer;
        s_paddedUploadBuffer.assign(static_cast<size_t>(texWidth) * texHeight * 4u, 0u);
        const size_t srcRowBytes = static_cast<size_t>(width) * 4u;
        const size_t dstRowBytes = static_cast<size_t>(texWidth) * 4u;
        const size_t copyRowBytes = static_cast<size_t>(copyWidth) * 4u;
        for (uint32_t y = 0; y < copyHeight; ++y)
        {
            const size_t srcOffset = static_cast<size_t>(y) * srcRowBytes;
            const size_t dstOffset = static_cast<size_t>(y) * dstRowBytes;
            if (srcOffset + copyRowBytes > ps2x_dev::s_lastFrame.size() ||
                dstOffset + copyRowBytes > s_paddedUploadBuffer.size())
            {
                break;
            }
            std::memcpy(s_paddedUploadBuffer.data() + dstOffset, ps2x_dev::s_lastFrame.data() + srcOffset, copyRowBytes);
        }
        UpdateTexture(tex, s_paddedUploadBuffer.data());
        ps2x_dev::s_lastFrame.swap(s_paddedUploadBuffer);
    }
    s_windowFrameNewPicture = true;

    // DW5XL-DEBUG issue 023: PS2X_WEAVE_TRACE=1 logs a fingerprint of every uploaded picture (FNV-1a over the copied
    // rows) and of its top and bottom half, so a picture that alternates between two versions shows up.
    {
        static const bool s_hashTrace = [] { const char *v = std::getenv("PS2X_WEAVE_TRACE"); return v && *v && *v != '0'; }();
        if (s_hashTrace)
        {
            const size_t rowBytes = static_cast<size_t>(texWidth) * 4u;
            auto fnv = [&](uint32_t y0, uint32_t y1)
            {
                uint64_t h = 1469598103934665603ull;
                for (uint32_t y = y0; y < y1; ++y)
                {
                    const uint8_t *row = ps2x_dev::s_lastFrame.data() + static_cast<size_t>(y) * rowBytes;
                    for (size_t i = 0; i < static_cast<size_t>(copyWidth) * 4u; ++i)
                        h = (h ^ row[i]) * 1099511628211ull;
                }
                return h;
            };
            std::fprintf(stderr, "[upload:hash] tick=%llu all=%016llx top=%016llx bottom=%016llx h=%u\n",
                         static_cast<unsigned long long>(currentTick), static_cast<unsigned long long>(fnv(0, copyHeight)),
                         static_cast<unsigned long long>(fnv(0, copyHeight / 2u)),
                         static_cast<unsigned long long>(fnv(copyHeight / 2u, copyHeight)), copyHeight);
            // Row profile (mean brightness of every row, two hex digits per row): a picture that moves vertically shows
            // up as a shifted profile without writing whole frames (PNG dumps change the timing and hide the jump).
            // PS2X_WEAVE_TRACE_COLS=x0:x1 (DW5XL-DEBUG issue 023 2J): the row profile covers only these columns, e.g. a
            // part of the HUD that never changes, so a picture that moves by one line shows without the 3D scene's noise.
            static const std::pair<uint32_t, uint32_t> s_cols = []
            {
                unsigned a = 0u, b = 0u;
                const char *v = std::getenv("PS2X_WEAVE_TRACE_COLS");
                if (v && std::sscanf(v, "%u:%u", &a, &b) == 2 && b > a)
                    return std::pair<uint32_t, uint32_t>(a, b);
                return std::pair<uint32_t, uint32_t>(0u, 0xFFFFFFFFu);
            }();
            const uint32_t x0 = std::min(s_cols.first, copyWidth), x1 = std::max(x0 + 1u, std::min(s_cols.second, copyWidth));
            static std::string s_rows;
            s_rows.clear();
            for (uint32_t y = 0; y < copyHeight; ++y)
            {
                const uint8_t *row = ps2x_dev::s_lastFrame.data() + static_cast<size_t>(y) * rowBytes;
                uint32_t sum = 0u;
                for (uint32_t x = x0; x < x1 && x < copyWidth; ++x)
                    sum += row[x * 4u] + row[x * 4u + 1u] + row[x * 4u + 2u];
                const uint32_t mean = sum / ((x1 - x0) * 3u);
                static const char kHex[] = "0123456789abcdef";
                s_rows.push_back(kHex[(mean >> 4) & 15u]);
                s_rows.push_back(kHex[mean & 15u]);
            }
            std::fprintf(stderr, "[upload:rows] tick=%llu w=%u h=%u %s\n", static_cast<unsigned long long>(currentTick),
                         copyWidth, copyHeight, s_rows.c_str());
        }
    }
    ps2x_dev::s_lastFrameW = copyWidth;
    ps2x_dev::s_lastFrameH = copyHeight;
    ps2x_dev::s_lastFrameRows = texHeight;
    s_lastWidth = copyWidth;
    s_lastHeight = copyHeight;
    s_lastDisplayWidth = displayWidth ? displayWidth : copyWidth;
    s_lastDisplayHeight = displayHeight ? displayHeight : copyHeight;
    outWidth = copyWidth;
    outHeight = copyHeight;
    outDisplayWidth = s_lastDisplayWidth;
    outDisplayHeight = s_lastDisplayHeight;
    s_hasUploadedFrame = true;
    // DW5XL-DEBUG: PS2X_DRAW_TRACE=1 logs every upload whose image or display size differs from the previous one.
    static const bool s_drawTrace = [] { const char *e = std::getenv("PS2X_DRAW_TRACE"); return e && *e && *e != '0'; }();
    static uint32_t s_tw = 0, s_th = 0, s_tdw = 0, s_tdh = 0;
    if (s_drawTrace && (s_tw != outWidth || s_th != outHeight || s_tdw != outDisplayWidth || s_tdh != outDisplayHeight))
    {
        std::fprintf(stderr, "[draw] tick=%llu image=%ux%u display=%ux%u tex=%ux%u\n",
                     static_cast<unsigned long long>(currentTick), outWidth, outHeight, outDisplayWidth,
                     outDisplayHeight, texWidth, texHeight);
        s_tw = outWidth; s_th = outHeight; s_tdw = outDisplayWidth; s_tdh = outDisplayHeight;
    }
}

namespace
{
    std::vector<const RecompiledCodeImage *> &staticCodeImages()
    {
        static std::vector<const RecompiledCodeImage *> images;
        return images;
    }
}

RecompiledCodeImageRegistration::RecompiledCodeImageRegistration(const RecompiledCodeImage &image)
{
    staticCodeImages().push_back(&image);
}

PS2Runtime::PS2Runtime()
{
    for (const auto *image : staticCodeImages())
        if (!registerCompiledCodeImage(*image))
            throw std::runtime_error("Invalid generated copied-code metadata");
    m_iopHost = std::make_unique<PS2IopHostAdapter>(*this);
    m_iopSubsystem = std::make_unique<ps2x::iop::IopSubsystem>(*m_iopHost);

    m_eeScheduler = std::make_unique<EeScheduler>(*this);

    // Assign rather than memset: R5900Context's constructor zeroes itself and
    // then applies the COP0 reset values, which a memset here would discard.
    m_cpuContext = R5900Context{};

    // R0 is always zero in MIPS
    m_cpuContext.r[0] = _mm_set1_epi32(0);
    m_cpuContext.vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    m_cpuContext.vu0_q = 1.0f;
    m_cpuContext.vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));

    // Stack pointer (SP) and global pointer (GP) will be set by the loaded ELF

    m_loadedModules.clear();
    m_guestHeapBlocks.clear();
    m_guestHeapBase = kGuestHeapDefaultBase;
    m_guestHeapEnd = kGuestHeapDefaultBase;
    m_guestHeapLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    m_guestHeapSuggestedBase = kGuestHeapDefaultBase;
    m_guestHeapConfigured = false;
    m_asyncCallbackStackFloor = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    m_asyncCallbackStackTop = PS2_RAM_SIZE;
}

void PS2Runtime::setDebugUiCallbacks(DebugUiCallback initCallback,
                                     DebugUiCallback drawCallback,
                                     DebugUiCallback shutdownCallback,
                                     void *userData)
{
    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }

    m_debugUiInitCallback = initCallback;
    m_debugUiDrawCallback = drawCallback;
    m_debugUiShutdownCallback = shutdownCallback;
    m_debugUiUserData = userData;
}

PS2Runtime::~PS2Runtime()
{
    try
    {
        requestStop();
        // Join the device worker before the GS/VU1/arbiter members it drives go away.
        ps2x::device::detach(m_memory);
        m_iopSubsystem.reset();
        m_iopHost.reset();
        m_audioBackend.stopPcmStream();
        m_audioBackend.stopAll();
#if defined(PLATFORM_VITA)
        m_audioBackend.setAudioReady(false);
#else
        if (IsAudioDeviceReady())
        {
            CloseAudioDevice();
            m_audioBackend.setAudioReady(false);
        }
#endif
        if (m_debugUiInitialized && m_debugUiShutdownCallback)
        {
            m_debugUiShutdownCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = false;
        }

        if (IsWindowReady())
        {
            CloseWindow();
        }

        m_loadedModules.clear();
    }
    catch (const std::exception &e)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: unknown" << std::endl;
    }
}

ps2x::iop::ModuleLoadResult PS2Runtime::loadIopModule(std::string_view path, const void *arguments, uint32_t argumentSize)
{
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->loadModule(path, arguments, argumentSize);
}

ps2x::iop::ModuleLoadResult PS2Runtime::loadIopModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
{
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->loadModuleBuffer(guestAddress, arguments, argumentSize);
}

bool PS2Runtime::stopIopModule(int32_t moduleId, int32_t *result)
{
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->stopModule(moduleId, result);
}

ps2x::iop::RpcAbi PS2Runtime::selectIopRpcAbi(const ps2x::iop::RpcAbiRequest &request) const
{
    return m_iopSubsystem->selectRpcAbi(request);
}

bool PS2Runtime::canBindIopRpc(uint32_t sid) const noexcept
{
    return m_iopSubsystem->canBindRpc(sid);
}

bool PS2Runtime::hasEmulatedIopRpcServer(uint32_t sid) const noexcept
{
    return m_iopSubsystem->hasEmulatedRpcServer(sid);
}

ps2x::iop::RpcResult PS2Runtime::handleIopRpc(uint8_t *rdram, R5900Context *ctx, ps2x::iop::RpcRequest request)
{
    auto scope = m_iopHost->enterCall(ctx, rdram);
    request.callToken = scope.token();
    return m_iopSubsystem->handleRpc(request);
}

void PS2Runtime::notifyIopSifTransfer(uint8_t *rdram, const ps2x::iop::SifTransfer &transfer)
{
    auto scope = m_iopHost->enterCall(nullptr, rdram);
    m_iopSubsystem->onSifTransfer(transfer);
}

void PS2Runtime::advanceIopEeCycles(uint64_t eeCycles) noexcept
{
    m_iopSubsystem->runEeCycles(eeCycles);
}

void PS2Runtime::setHostPaused(bool paused)
{
    const uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    {
        std::lock_guard<std::mutex> lock(m_hostPauseMutex);
        if (paused == (m_hostPauseStartNs != 0u))
            return;
        if (paused)
            m_hostPauseStartNs = now;
        else
        {
            m_hostPausedTotalNs += now - m_hostPauseStartNs;
            m_hostPauseStartNs = 0u;
        }
    }
    if (m_eeScheduler)
        m_eeScheduler->setHostPaused(paused);
    m_audioBackend.setPcmPaused(paused);
}

uint64_t PS2Runtime::hostPausedNanoseconds(uint64_t steadyNowNs) const
{
    std::lock_guard<std::mutex> lock(m_hostPauseMutex);
    const uint64_t current = m_hostPauseStartNs != 0u && steadyNowNs > m_hostPauseStartNs ? steadyNowNs - m_hostPauseStartNs : 0u;
    return m_hostPausedTotalNs + current;
}

void PS2Runtime::resetIop()
{
    m_iopSubsystem->reset();
}

ps2x::iop::DebugSnapshot PS2Runtime::iopDebugSnapshot() const
{
    return m_iopSubsystem->debugSnapshot();
}

uint32_t PS2Runtime::allocateIopMemory(uint32_t size, uint32_t alignment)
{
    return m_iopSubsystem ? m_iopSubsystem->allocateMemory(size, alignment) : 0u;
}

bool PS2Runtime::freeIopMemory(uint32_t address)
{
    return m_iopSubsystem && m_iopSubsystem->freeMemory(address);
}

bool PS2Runtime::readIopMemory(uint32_t address, void *destination, size_t size) const
{
    return m_iopSubsystem && m_iopSubsystem->readMemory(address, destination, size);
}

bool PS2Runtime::writeIopMemory(uint32_t address, const void *source, size_t size)
{
    return m_iopSubsystem && m_iopSubsystem->writeMemory(address, source, size);
}

bool PS2Runtime::zeroIopMemory(uint32_t address, size_t size)
{
    return m_iopSubsystem && m_iopSubsystem->zeroMemory(address, size);
}

bool PS2Runtime::isIopMemoryRange(uint32_t address, size_t size) const
{
    return m_iopSubsystem && m_iopSubsystem->isMemoryRange(address, size);
}

bool PS2Runtime::syncCoreSubsystems()
{
    uint8_t *const rdram = m_memory.getRDRAM();
    uint8_t *const gsVram = m_memory.getGSVRAM();
    if (!rdram || !gsVram)
    {
        return false;
    }

    if (m_boundRdram == rdram && m_boundGSVram == gsVram)
    {
        return true;
    }

    // Re-binding replaces the GS/VU1 state the device worker drives: stop it first.
    ps2x::device::detach(m_memory);
    m_gs.init(gsVram, static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &m_memory.gs());
    m_gifArbiter.setProcessPacketFn([this](const uint8_t *data, uint32_t size)
                                    { m_gs.processGIFPacket(data, size); });
    m_gifArbiter.setWorkerEnabled(GifArbiter::workerRequestedByEnv());
    m_memory.setGifArbiter(&m_gifArbiter);
    // VIF1 MSCAL/MSCNT. On the async device worker the VU1 D/T enables come from
    // the kick-time FBRST snapshot and the D/T stop bits go to the command's
    // completion record (VPU-STAT is device state; the EE applies it after the
    // drain). Elsewhere (PS2X_VU1_ASYNC=0, VIF1_FIFO) the legacy path is used:
    // the current guest context is read and written directly.
    auto runVu1 = [this](bool resume, uint32_t startPC, uint32_t top, uint32_t itop)
    {
        R5900Context *cpuContext = nullptr;
        uint32_t fbrst = 0u;
        if (ps2x::device::onDeviceWorker())
        {
            fbrst = ps2x::device::workerVu0Fbrst();
        }
        else
        {
            cpuContext = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
            if (!cpuContext)
            {
                cpuContext = &m_cpuContext;
            }
            fbrst = cpuContext->vu0_fbrst;
        }
        m_vu1.state().dBitEnabled = (fbrst & (1u << 10)) != 0u;
        m_vu1.state().tBitEnabled = (fbrst & (1u << 11)) != 0u;
        ps2x::device::ProfileScope profile(ps2x::device::ProfilePart::Vu1);
        if (resume)
        {
            m_vu1.resume(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                         m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                         m_gs, &m_memory, top, itop, 65536);
        }
        else
        {
            m_vu1.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                          m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                          m_gs, &m_memory, startPC, top, itop, 65536);
        }
        const uint32_t stopBits = (m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                                  (m_vu1.state().stoppedByT ? 0x0400u : 0u);
        if (cpuContext)
        {
            cpuContext->vu0_vpu_stat = (cpuContext->vu0_vpu_stat & ~0x0600u) | stopBits;
        }
        else
        {
            ps2x::device::workerRecordVu1StopBits(stopBits);
        }
    };
    m_memory.setVu1MscalCallback([runVu1](uint32_t startPC, uint32_t top, uint32_t itop)
                                 { runVu1(false, startPC, top, itop); });
    m_memory.setVu1MscntCallback([runVu1](uint32_t top, uint32_t itop)
                                 { runVu1(true, 0u, top, itop); });
    // DW5XL-DEBUG: PS2X_VIF1_CAPTURE records VU1 state and the D/T enable bits.
    Vif1Capture::attach(&m_vu1, [this]() -> uint32_t
                        {
                            const R5900Context *context = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                            return (context ? context : &m_cpuContext)->vu0_fbrst; });
    resetIop();
    m_vu0.reset();
    m_vu1.reset();

    m_boundRdram = rdram;
    m_boundGSVram = gsVram;

    {
        auto currentContext = [this]() -> R5900Context *
        {
            R5900Context *ctx = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
            return ctx ? ctx : &m_cpuContext;
        };
        ps2x::device::Config deviceConfig;
        deviceConfig.async = ps2x::device::asyncRequestedByEnv();
        deviceConfig.trace = ps2x::device::traceRequestedByEnv();
        deviceConfig.gsWriteRegister = [this](uint8_t reg, uint64_t value)
        { m_gs.writeRegister(reg, value); };
        deviceConfig.controlWrite = [this](uint32_t address, uint32_t value)
        {
            m_memory.writeIORegister(address, value);
            if (address == 0x10003000u && (value & 1u))
            {
                m_gifArbiter.reset();
                m_gs.resetGifInput();
            }
        };
        deviceConfig.controlQwordWrite = [this](uint32_t address, uint64_t lo, uint64_t hi)
        { m_memory.write128(address, _mm_set_epi64x(hi, lo)); };
        deviceConfig.resetVu1 = [this] { m_vu1.resetMicroExecution(); };
        deviceConfig.currentVu0Fbrst = [currentContext]() -> uint32_t
        { return currentContext()->vu0_fbrst; };
        deviceConfig.applyVu1StopBits = [currentContext](uint32_t stopBits)
        {
            R5900Context *ctx = currentContext();
            ctx->vu0_vpu_stat = (ctx->vu0_vpu_stat & ~0x0600u) | (stopBits & 0x0600u);
        };
        deviceConfig.localToHost = [this](uint8_t *dst, uint32_t maxBytes) -> uint32_t
        { return m_gs.consumeLocalToHostBytes(dst, maxBytes); };
        deviceConfig.workerIdle = [this]()
        {
            if (m_eeScheduler)
                m_eeScheduler->postEvent(EeEvent{EeEventType::ExternalWake, ps2x::device::kDeviceIdleWaitType,
                                                 ps2x::device::kDeviceIdleWaitToken});
        };
        deviceConfig.callerRa = [currentContext]() -> uint32_t
        { return getRegU32(currentContext(), 31); };
        ps2x::device::attach(m_memory, std::move(deviceConfig));
    }
    return true;
}

bool PS2Runtime::initialize(const char *title)
{
    try
    {
        ps2prof::openFromEnv(); // DW5XL-DEBUG issue 025: PS2X_PROFILE_MARKERS=<file> (diagnostic builds only)
        if (!m_memory.initialize())
        {
            std::cerr << "Failed to initialize PS2 memory" << std::endl;
            return false;
        }

        if (!syncCoreSubsystems())
        {
            std::cerr << "Failed to bind runtime core subsystems" << std::endl;
            return false;
        }

        applySavedOutputSettings(m_gs);
#if defined(PLATFORM_VITA)
        InitWindow(HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT, title); // raylib vita does not support audio
#else
        {
            const char *hidden = std::getenv("PS2X_HIDDEN");
            // Issue 023 step 3S: with vsync on the window swaps on a display refresh (swap interval 1) instead of
            // immediately; with field pacing (the default) that is one picture per PS2 field, shown on a refresh.
            const char *vsyncSource = "default";
            const bool vsyncOn = loadVsyncSetting(vsyncSource);
            SetConfigFlags(FLAG_WINDOW_RESIZABLE |
                           ((hidden && *hidden && *hidden != '0') ? FLAG_WINDOW_HIDDEN : 0) |
                           (vsyncOn ? FLAG_VSYNC_HINT : 0));
            std::fprintf(stderr, "[window] vsync %s (%s)\n", vsyncOn ? "on" : "off", vsyncSource);
        }
        InitWindow(HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT, title);
        ps2x::present_feedback::start(GetWindowHandle()); // DW5XL-DEBUG issue 023 3S: PS2X_PRESENT_LOG
        if (loadFullscreenSetting() && !IsWindowHidden())   // issue 023 3S: display=fullscreen (borderless)
            ToggleBorderlessWindowed();
        InitAudioDevice();
        // DW5XL-DEBUG (issue 025): PS2X_MUTE=1 silences the output for test runs where no silent sink exists
        // (Windows); the audio device and stream keep running exactly as in normal play, only the volume is 0.
        if (const char *mute = std::getenv("PS2X_MUTE"); mute && *mute == '1')
            SetMasterVolume(0.0f);
        m_audioBackend.setAudioReady(IsAudioDeviceReady());
        m_audioBackend.startPcmStream();
#endif
        SetTargetFPS(configuredHostTargetFps());
        if (m_debugUiInitCallback)
        {
            m_debugUiInitCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = true;
        }

        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Failed to initialize PS2 runtime: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "Failed to initialize PS2 runtime: unknown exception" << std::endl;
    }

    return false;
}

bool PS2Runtime::loadELF(const std::string &elfPath)
{
    // Even a failed reload must not retain dispatch for the previous game data.
    deactivateCompiledModule();
    configureIoPathsFromElf(elfPath);

    std::ifstream file(elfPath, std::ios::binary);
    if (!file)
    {
        std::cerr << "Failed to open ELF file: " << elfPath << std::endl;
        return false;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    if (fileSize < static_cast<std::streamoff>(sizeof(ElfHeader)))
    {
        std::cerr << "ELF file is too small: " << elfPath << std::endl;
        return false;
    }
    file.seekg(0, std::ios::beg);

    ElfHeader header{};
    if (!file.read(reinterpret_cast<char *>(&header), sizeof(header)))
    {
        std::cerr << "Failed to read ELF header from: " << elfPath << std::endl;
        return false;
    }

    if (header.magic != ELF_MAGIC)
    {
        std::cerr << "Invalid ELF magic number" << std::endl;
        return false;
    }

    if (header.elf_class != 1u || header.endianness != 1u)
    {
        std::cerr << "Unsupported ELF format (expected 32-bit little-endian)." << std::endl;
        return false;
    }

    if (header.machine != EM_MIPS || header.type != ET_EXEC)
    {
        std::cerr << "Not a MIPS executable ELF file" << std::endl;
        return false;
    }

    if (header.phnum != 0u && header.phentsize < sizeof(ProgramHeader))
    {
        std::cerr << "Unsupported ELF program-header entry size: " << header.phentsize << std::endl;
        return false;
    }

    const uint64_t programHeaderTableEnd =
        static_cast<uint64_t>(header.phoff) +
        static_cast<uint64_t>(header.phnum) * static_cast<uint64_t>(header.phentsize);
    if (programHeaderTableEnd > static_cast<uint64_t>(fileSize))
    {
        std::cerr << "ELF program-header table is out of range." << std::endl;
        return false;
    }

    m_cpuContext.pc = header.entry;
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);

    uint32_t maxLoadedRdramEnd = kGuestHeapDefaultBase;
    uint32_t moduleBase = std::numeric_limits<uint32_t>::max();
    uint32_t moduleEnd = 0u;
    bool loadedAnySegment = false;

    for (uint16_t i = 0; i < header.phnum; i++)
    {
        const uint64_t phOffset =
            static_cast<uint64_t>(header.phoff) +
            static_cast<uint64_t>(i) * static_cast<uint64_t>(header.phentsize);
        if (phOffset + sizeof(ProgramHeader) > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF program header " << i << " is out of range." << std::endl;
            return false;
        }

        ProgramHeader ph{};
        file.seekg(static_cast<std::streamoff>(phOffset), std::ios::beg);
        if (!file.read(reinterpret_cast<char *>(&ph), sizeof(ph)))
        {
            std::cerr << "Failed to read ELF program header " << i << std::endl;
            return false;
        }

        if (ph.type != PT_LOAD || ph.memsz == 0u)
        {
            continue;
        }

        if (ph.filesz > ph.memsz)
        {
            std::cerr << "ELF segment " << i << " has filesz > memsz." << std::endl;
            return false;
        }

        const uint64_t segmentFileEnd = static_cast<uint64_t>(ph.offset) + static_cast<uint64_t>(ph.filesz);
        if (segmentFileEnd > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF segment " << i << " exceeds file bounds." << std::endl;
            return false;
        }

        const bool scratch =
            ph.vaddr >= PS2_SCRATCHPAD_BASE &&
            ph.vaddr < (PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE);

        uint32_t physAddr = 0u;
        try
        {
            physAddr = m_memory.translateAddress(ph.vaddr);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Failed to translate ELF segment " << i
                      << " virtual address 0x" << std::hex << ph.vaddr
                      << std::dec << ": " << e.what() << std::endl;
            return false;
        }
        const uint64_t regionSize = scratch ? static_cast<uint64_t>(PS2_SCRATCHPAD_SIZE)
                                            : static_cast<uint64_t>(PS2_RAM_SIZE);
        const uint64_t segmentMemEnd = static_cast<uint64_t>(physAddr) + static_cast<uint64_t>(ph.memsz);
        if (segmentMemEnd > regionSize)
        {
            std::cerr << "ELF segment " << i << " exceeds "
                      << (scratch ? "scratchpad" : "RDRAM")
                      << " bounds (vaddr=0x" << std::hex << ph.vaddr
                      << " memsz=0x" << ph.memsz << std::dec << ")." << std::endl;
            return false;
        }

        uint8_t *destBase = scratch ? m_memory.getScratchpad() : m_memory.getRDRAM();
        if (!destBase)
        {
            std::cerr << "ELF segment " << i << " has no destination memory backing." << std::endl;
            return false;
        }

        uint8_t *dest = destBase + physAddr;
        if (ph.filesz > 0u)
        {
            file.seekg(static_cast<std::streamoff>(ph.offset), std::ios::beg);
            if (!file.read(reinterpret_cast<char *>(dest), ph.filesz))
            {
                std::cerr << "Failed to read ELF segment " << i << " payload." << std::endl;
                return false;
            }
        }

        if (ph.memsz > ph.filesz)
        {
            std::memset(dest + ph.filesz, 0, ph.memsz - ph.filesz);
        }

        RUNTIME_LOG("Loading segment: 0x" << std::hex << ph.vaddr
                                          << " - 0x" << (static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz))
                                          << " (filesz: 0x" << ph.filesz
                                          << ", memsz: 0x" << ph.memsz << ")"
                                          << std::dec << std::endl);

        if (!scratch)
        {
            maxLoadedRdramEnd = std::max(maxLoadedRdramEnd, static_cast<uint32_t>(segmentMemEnd));
        }

        if (ph.flags & 0x1u) // PF_X
        {
            const uint64_t execEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.filesz);
            if (execEnd <= std::numeric_limits<uint32_t>::max())
            {
                m_memory.registerCodeRegion(ph.vaddr, static_cast<uint32_t>(execEnd));
            }
        }

        loadedAnySegment = true;
        moduleBase = std::min(moduleBase, ph.vaddr);
        const uint64_t segmentVirtualEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz);
        const uint32_t clampedVirtualEnd =
            (segmentVirtualEnd > std::numeric_limits<uint32_t>::max())
                ? std::numeric_limits<uint32_t>::max()
                : static_cast<uint32_t>(segmentVirtualEnd);
        moduleEnd = std::max(moduleEnd, clampedVirtualEnd);
    }

    if (!loadedAnySegment)
    {
        std::cerr << "ELF contains no loadable PT_LOAD segments." << std::endl;
        return false;
    }

    if (maxLoadedRdramEnd > PS2_RAM_SIZE)
    {
        maxLoadedRdramEnd = PS2_RAM_SIZE;
    }

    const uint32_t paddedEnd = (maxLoadedRdramEnd > (PS2_RAM_SIZE - kGuestHeapSafetyPad))
                                   ? PS2_RAM_SIZE
                                   : (maxLoadedRdramEnd + kGuestHeapSafetyPad);
    const uint32_t suggestedHeapBase = alignGuestHeapValue(paddedEnd, kGuestHeapDefaultAlignment);
    {
        std::lock_guard<std::mutex> lock(m_guestHeapMutex);
        if (!m_guestHeapConfigured)
        {
            const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
            m_guestHeapSuggestedBase = std::min(suggestedHeapBase, hardLimit);
            m_guestHeapBase = m_guestHeapSuggestedBase;
            m_guestHeapEnd = m_guestHeapSuggestedBase;
            m_guestHeapLimit = hardLimit;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
        const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
        m_asyncCallbackStackFloor = std::min(std::max(hardLimit, suggestedHeapBase), PS2_RAM_SIZE);
        m_asyncCallbackStackTop = PS2_RAM_SIZE;
    }

    LoadedModule module;
    module.name = elfPath.substr(elfPath.find_last_of("/\\") + 1);
    module.baseAddress = (moduleBase == std::numeric_limits<uint32_t>::max()) ? 0x00100000u : moduleBase;
    module.size = (moduleEnd > module.baseAddress) ? static_cast<size_t>(moduleEnd - module.baseAddress) : 0u;
    module.active = true;

    m_loadedModules.push_back(module);

    uint32_t elfCrc32 = 0u;
    const bool elfCrc32Valid = computeFileCrc32(elfPath, elfCrc32);
    if (!elfCrc32Valid)
    {
        std::cerr << "[ps2xIOP] failed to compute ELF CRC32 for '" << elfPath << "'" << std::endl;
    }
    ps2x::iop::GameIdentity identity;
    identity.elfName = module.name;
    identity.entryPoint = m_cpuContext.pc;
    identity.crc32 = elfCrc32;
    std::string romError;
    if (!m_romDevice.configure(identity, &romError))
    {
        std::cerr << "[ROM0] failed to configure profile: " << romError << std::endl;
        return false;
    }

    m_iopSubsystem->reset();

    ps2_game_overrides::applyMatching(*this,
                                      elfPath,
                                      m_cpuContext.pc,
                                      elfCrc32,
                                      elfCrc32Valid);

    RUNTIME_LOG("ELF file loaded successfully. Entry point: 0x" << std::hex << m_cpuContext.pc << std::dec);
    return true;
}

const PS2Runtime::IoPaths &PS2Runtime::getIoPaths()
{
    return runtimeIoPaths();
}

void PS2Runtime::setIoPaths(const IoPaths &paths)
{
    IoPaths normalized = paths;
    normalized.elfPath = normalizeAbsolutePath(normalized.elfPath);
    normalized.elfDirectory = normalizeAbsolutePath(normalized.elfDirectory);
    normalized.hostRoot = normalizeAbsolutePath(normalized.hostRoot);
    normalized.cdRoot = normalizeAbsolutePath(normalized.cdRoot);
    normalized.mcRoot = normalizeAbsolutePath(normalized.mcRoot);
    normalized.cdImage = normalizeAbsolutePath(normalized.cdImage);

    if (normalized.elfDirectory.empty() && !normalized.elfPath.empty())
    {
        normalized.elfDirectory = normalized.elfPath.parent_path();
    }

    if (normalized.hostRoot.empty())
    {
        normalized.hostRoot = normalized.elfDirectory;
    }
    if (normalized.cdRoot.empty())
    {
        normalized.cdRoot = normalized.elfDirectory;
    }
    if (normalized.mcRoot.empty())
    {
        normalized.mcRoot = normalized.elfDirectory / "mc0";
    }

    runtimeIoPaths() = normalized;
}

void PS2Runtime::configureIoPathsFromElf(const std::string &elfPath)
{
    IoPaths paths = runtimeIoPaths();
    paths.elfPath = normalizeAbsolutePath(std::filesystem::path(elfPath));
    if (!paths.elfPath.empty())
    {
        paths.elfDirectory = paths.elfPath.parent_path();
    }

    if (!paths.elfDirectory.empty())
    {
        paths.hostRoot = paths.elfDirectory;
        paths.cdRoot = paths.elfDirectory;
        paths.mcRoot = paths.elfDirectory / "mc0";
    }
    // PS2X_MC_ROOT=<dir> uses another folder as memory card slot 1 (the tests run on their own copy of a save,
    // the player's mc0 next to the game stays untouched).
    if (const char *mc = std::getenv("PS2X_MC_ROOT"); mc && *mc)
    {
        paths.mcRoot = std::filesystem::path(mc);
    }

    setIoPaths(paths);
    std::fprintf(stderr, "[io] memory card slot 1: %s\n", runtimeIoPaths().mcRoot.string().c_str());
}

namespace
{
    bool generatedFunctionTableSlot(uint32_t address, uint32_t &slot)
    {
        if ((address & 3u) != 0u || g_ps2RecompiledFunctionTableSlotCount == 0u)
        {
            return false;
        }

        if (address < g_ps2RecompiledFunctionTableBase || address >= g_ps2RecompiledFunctionTableEnd)
        {
            return false;
        }

        const uint32_t offset = address - g_ps2RecompiledFunctionTableBase;
        slot = offset >> 2;
        return slot < g_ps2RecompiledFunctionTableSlotCount;
    }
}

bool PS2Runtime::registerCompiledCodeImage(const RecompiledCodeImage &image)
{
    const auto &module = image.module;
    const uint64_t end = uint64_t(module.guestBase) + module.fileSize;
    const uint32_t physical = module.guestBase & 0x1fffffffu;
    if (!module.identity || !*module.identity || (module.guestBase & 3u) || !module.fileSize ||
        end > 0x100000000ull || uint64_t(physical) + module.fileSize > PS2_RAM_SIZE ||
        !(module.guestBase < PS2_RAM_SIZE || (module.guestBase >= 0x80000000u && module.guestBase < 0xc0000000u)) ||
        !module.entries || !module.entryCount || module.entryCount > module.fileSize/4u ||
        !image.words || !image.wordCount || image.wordCount > module.fileSize/4u ||
        (module.guestBase < g_ps2RecompiledFunctionTableEnd && end > g_ps2RecompiledFunctionTableBase))
        return false;
    bool nonzero = false;
    for (size_t i = 0; i < image.wordCount; ++i)
    {
        const auto &word = image.words[i];
        if ((word.address & 3u) || word.address < module.guestBase || uint64_t(word.address)+4u > end ||
            (i && word.address <= image.words[i-1].address)) return false;
        nonzero |= word.word != 0;
    }
    if (!nonzero) return false; // empty RAM cannot stand in for a copied all-zero image
    for (size_t i = 0; i < module.entryCount; ++i)
    {
        const auto &entry = module.entries[i];
        if (!entry.function || (entry.address & 3u) || entry.address < module.guestBase || uint64_t(entry.address)+4u > end ||
            (i && entry.address <= module.entries[i-1].address)) return false;
        const auto *finish = image.words + image.wordCount;
        const auto *word = std::lower_bound(image.words, finish, entry.address,
            [](const RecompiledCodeWord &candidate,uint32_t pc) { return candidate.address < pc; });
        if (word == finish || word->address != entry.address) return false;
    }
    for (const auto *other : m_compiledCodeImages)
    {
        if (std::string_view(other->module.identity) == module.identity) return other == &image;
        const uint32_t otherPhysical = other->module.guestBase & 0x1fffffffu;
        if (physical < uint64_t(otherPhysical)+other->module.fileSize && uint64_t(physical)+module.fileSize > otherPhysical)
            return false;
    }
    for (const auto *other : m_compiledModules)
        if (physical < uint64_t(other->guestBase)+other->fileSize && uint64_t(physical)+module.fileSize > other->guestBase)
            return false;
    m_compiledCodeImages.push_back(&image);
    return true;
}

bool PS2Runtime::registerCompiledModule(const RecompiledModule &module)
{
    const uint64_t end = static_cast<uint64_t>(module.guestBase) + module.fileSize;
    auto reject = [&]() {
        std::cerr << "[compiled-module] invalid descriptor for "
                  << (module.identity ? module.identity : "<null>") << std::endl;
        return false;
    };
    if (!module.identity || !*module.identity || (module.guestBase & 3u) != 0u ||
        module.fileSize < sizeof(uint32_t) || end > PS2_RAM_SIZE ||
        !module.entries || module.entryCount == 0u ||
        module.entryCount > module.fileSize / sizeof(uint32_t))
    {
        return reject();
    }
    // A module owns its loaded file envelope. Main ELF dispatch must never be
    // shadowed, even by addresses without an existing dense-table entry.
    if (g_ps2RecompiledFunctionTableSlotCount != 0u &&
        module.guestBase < g_ps2RecompiledFunctionTableEnd &&
        end > g_ps2RecompiledFunctionTableBase)
    {
        return reject();
    }
    for (const auto *copy : m_compiledCodeImages)
    {
        const uint32_t physical = copy->module.guestBase & 0x1fffffffu;
        if (module.guestBase < uint64_t(physical)+copy->module.fileSize && end > physical) return reject();
    }
    uint32_t previous = 0u;
    for (size_t i = 0; i < module.entryCount; ++i)
    {
        const RecompiledModuleEntry &entry = module.entries[i];
        if (!entry.function || (entry.address & 3u) != 0u ||
            entry.address < module.guestBase ||
            static_cast<uint64_t>(entry.address) + sizeof(uint32_t) > end ||
            (i != 0u && entry.address <= previous))
        {
            return reject();
        }
        previous = entry.address;
    }
    for (const RecompiledModule *existing : m_compiledModules)
    {
        if (std::string_view(existing->identity) == module.identity)
        {
            // Reapplying game overrides to the same static descriptor is safe.
            return existing == &module;
        }
    }
    m_compiledModules.push_back(&module);
    return true;
}

bool PS2Runtime::activateCompiledModule(std::string_view identity)
{
    // An unknown identity must not leave an earlier module callable.
    deactivateCompiledModule();
    for (const RecompiledModule *module : m_compiledModules)
    {
        if (identity == module->identity)
        {
            m_activeCompiledModule = module;
            return true;
        }
    }
    std::cerr << "[compiled-module] unknown identity '" << identity << "'" << std::endl;
    return false;
}

void PS2Runtime::deactivateCompiledModule()
{
    m_activeCompiledModule = nullptr;
    ++m_compiledModuleGeneration;
}

PS2Runtime::RecompiledFunction PS2Runtime::resolveFunctionExact(uint32_t address) const
{
    if ((address & 3u) != 0u)
    {
        return nullptr;
    }
    // Both module kinds are forbidden to shadow the dense main envelope.
    // Preserve its hot path without copied-image scans or byte checks.
    uint32_t mainSlot = 0u;
    if (generatedFunctionTableSlot(address,mainSlot)) return g_ps2RecompiledFunctionTable[mainSlot];
    for (const auto *image : m_compiledCodeImages)
    {
        const auto &module = image->module;
        if (address < module.guestBase || uint64_t(address) >= uint64_t(module.guestBase)+module.fileSize) continue;
        const auto *finish = module.entries + module.entryCount;
        const auto *entry = std::lower_bound(module.entries, finish, address,
            [](const RecompiledModuleEntry &candidate,uint32_t pc) { return candidate.address < pc; });
        if (entry == finish || entry->address != address) return nullptr;
        const auto *ram = m_memory.getRDRAM();
        if (!ram) return nullptr;
        for (size_t i = 0; i < image->wordCount; ++i)
        {
            uint32_t word;
            std::memcpy(&word,ram+(image->words[i].address & 0x1fffffffu),sizeof(word));
            if (word != image->words[i].word) return nullptr;
        }
        return entry->function;
    }
    // All registered module envelopes are reserved, including currently inactive
    // modules and tails that are longer than the active module's file.
    bool moduleAddress = false;
    for (const RecompiledModule *module : m_compiledModules)
    {
        if (address >= module->guestBase &&
            static_cast<uint64_t>(address) < static_cast<uint64_t>(module->guestBase) + module->fileSize)
        {
            moduleAddress = true;
            break;
        }
    }
    if (moduleAddress)
    {
        const RecompiledModule *module = m_activeCompiledModule;
        if (!module || address < module->guestBase ||
            static_cast<uint64_t>(address) >= static_cast<uint64_t>(module->guestBase) + module->fileSize)
        {
            return nullptr;
        }
        const RecompiledModuleEntry *const end = module->entries + module->entryCount;
        const RecompiledModuleEntry *entry = std::lower_bound(
            module->entries, end, address,
            [](const RecompiledModuleEntry &candidate, uint32_t pc) { return candidate.address < pc; });
        return entry != end && entry->address == address ? entry->function : nullptr;
    }
    uint32_t slot = 0u;
    return generatedFunctionTableSlot(address, slot) ? g_ps2RecompiledFunctionTable[slot] : nullptr;
}

bool PS2Runtime::replaceFunction(uint32_t address, RecompiledFunction func)
{
    uint32_t slot = 0u;
    if (!generatedFunctionTableSlot(address, slot))
    {
        std::cerr << "[function-table] cannot replace guest PC 0x" << std::hex << address
                  << ": outside generated dense table [0x" << g_ps2RecompiledFunctionTableBase
                  << ", 0x" << g_ps2RecompiledFunctionTableEnd << ")"
                  << std::dec << std::endl;
        return false;
    }

    g_ps2RecompiledFunctionTable[slot] = func;
    return true;
}

bool PS2Runtime::registerFunction(uint32_t address, RecompiledFunction func)
{
    return replaceFunction(address, func);
}

bool PS2Runtime::hasFunction(uint32_t address) const
{
    return resolveFunctionExact(address) != nullptr;
}

const char *describeGuestBranchKind(PS2Runtime::GuestBranchKind kind)
{
    switch (kind)
    {
    case PS2Runtime::GuestBranchKind::DirectJump:
        return "DirectJump";
    case PS2Runtime::GuestBranchKind::DirectCall:
        return "DirectCall";
    case PS2Runtime::GuestBranchKind::IndirectJump:
        return "IndirectJump";
    case PS2Runtime::GuestBranchKind::IndirectCall:
        return "IndirectCall";
    case PS2Runtime::GuestBranchKind::Return:
        return "Return";
    default:
        return "Unknown";
    }
}

PS2Runtime::RecompiledFunction PS2Runtime::lookupFunction(uint32_t address)
{
    pushDispatchPc(address);

    if (RecompiledFunction fn = resolveFunctionExact(address))
    {
        return fn;
    }

    std::cerr << "Error: No exact recompiled function for guest PC 0x" << std::hex << address
              << " tableBase=0x" << g_ps2RecompiledFunctionTableBase
              << " tableEnd=0x" << g_ps2RecompiledFunctionTableEnd
              << " module=" << (m_activeCompiledModule ? m_activeCompiledModule->identity : "<none>")
              << " generation=" << std::dec << m_compiledModuleGeneration
              << " codeRegion=" << (m_memory.isCodeAddress(address) ? "yes" : "no")
              << " trace=" << formatDispatchHistory()
              << std::dec << std::endl;

    static RecompiledFunction missingFunction = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t badPc = ctx->pc;
        runtime->reportMissingFunction(rdram,
                                       ctx,
                                       badPc,
                                       0u,
                                       PS2Runtime::GuestBranchKind::IndirectJump,
                                       "dispatch");
    };

    return missingFunction;
}

void PS2Runtime::setMissingFunctionPolicy(MissingFunctionPolicy policy)
{
    m_missingFunctionPolicy.store(static_cast<uint32_t>(policy), std::memory_order_release);
}

PS2Runtime::MissingFunctionPolicy PS2Runtime::missingFunctionPolicy() const
{
    return static_cast<MissingFunctionPolicy>(m_missingFunctionPolicy.load(std::memory_order_acquire));
}

void PS2Runtime::resetMissingFunctionReportOnce()
{
    m_missingFunctionReported.store(false, std::memory_order_release);
}

void PS2Runtime::reportMissingFunction(uint8_t *rdram,
                                       R5900Context *ctx,
                                       uint32_t targetPc,
                                       uint32_t sourcePc,
                                       GuestBranchKind kind,
                                       const char *debugName)
{
    const MissingFunctionPolicy policy = missingFunctionPolicy();
    const bool firstReport = !m_missingFunctionReported.exchange(true, std::memory_order_acq_rel);

    const uint32_t pc = ctx->pc;
    const uint32_t ra = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
    const uint32_t sp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0));
    const uint32_t gp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0));
    const uint32_t a0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0));
    const uint32_t a1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[5], 0));
    const uint32_t a2 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[6], 0));
    const uint32_t a3 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[7], 0));
    const uint32_t s0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[16], 0));
    const uint32_t s1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[17], 0));
    const uint32_t v0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[2], 0));
    const uint32_t v1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[3], 0));

    auto readGuestU32At = [rdram](uint32_t addr, uint32_t &out) -> bool
    {
        // TODO this !rdram exist only because of test fix those test later
        if (!rdram || addr > PS2_RAM_SIZE - sizeof(uint32_t))
        {
            out = 0u;
            return false;
        }

        std::memcpy(&out, rdram + addr, sizeof(uint32_t));
        return true;
    };

    auto readGuestU32Offset = [&readGuestU32At](uint32_t base, uint32_t offset, uint32_t &out) -> bool
    {
        if (base > PS2_RAM_SIZE - sizeof(uint32_t) || offset > PS2_RAM_SIZE - sizeof(uint32_t) - base)
        {
            out = 0u;
            return false;
        }

        return readGuestU32At(base + offset, out);
    };

    uint32_t a0Word0 = 0u;
    uint32_t a0Word4 = 0u;
    uint32_t a0Word8 = 0u;
    uint32_t a0WordC = 0u;
    const bool a0Readable =
        readGuestU32Offset(a0, 0x00u, a0Word0) &&
        readGuestU32Offset(a0, 0x04u, a0Word4) &&
        readGuestU32Offset(a0, 0x08u, a0Word8) &&
        readGuestU32Offset(a0, 0x0cu, a0WordC);

    uint32_t s0Word0 = 0u;
    uint32_t s0Word4 = 0u;
    uint32_t s0Word8 = 0u;
    uint32_t s0WordC = 0u;
    const bool s0Readable =
        readGuestU32Offset(s0, 0x00u, s0Word0) &&
        readGuestU32Offset(s0, 0x04u, s0Word4) &&
        readGuestU32Offset(s0, 0x08u, s0Word8) &&
        readGuestU32Offset(s0, 0x0cu, s0WordC);

    uint32_t recordWord0 = 0u;
    uint32_t recordWord4 = 0u;
    uint32_t recordWord8 = 0u;
    uint32_t recordWordC = 0u;
    const bool recordReadable =
        s0Readable && s0Word4 != 0u &&
        readGuestU32Offset(s0Word4, 0x00u, recordWord0) &&
        readGuestU32Offset(s0Word4, 0x04u, recordWord4) &&
        readGuestU32Offset(s0Word4, 0x08u, recordWord8) &&
        readGuestU32Offset(s0Word4, 0x0cu, recordWordC);

    uint32_t vtableSlot0 = 0u;
    uint32_t vtableSlot4 = 0u;
    uint32_t vtableSlot8 = 0u;
    uint32_t vtableSlotC = 0u;
    const bool vtableReadable =
        a0Readable && a0Word0 != 0u &&
        readGuestU32Offset(a0Word0, 0x00u, vtableSlot0) &&
        readGuestU32Offset(a0Word0, 0x04u, vtableSlot4) &&
        readGuestU32Offset(a0Word0, 0x08u, vtableSlot8) &&
        readGuestU32Offset(a0Word0, 0x0cu, vtableSlotC);

    if (firstReport)
    {
        std::ostringstream oss;
        oss << "[guest-branch:missing-target] kind=" << describeGuestBranchKind(kind)
            << " op=" << (debugName ? debugName : "<unknown>")
            << " source=0x" << std::hex << sourcePc
            << " target=0x" << targetPc
            << " pc=0x" << pc
            << " ra=0x" << ra
            << " sp=0x" << sp
            << " gp=0x" << gp
            << " module=" << (m_activeCompiledModule ? m_activeCompiledModule->identity : "<none>")
            << " generation=" << std::dec << m_compiledModuleGeneration << std::hex
            << " a0=0x" << a0
            << " a1=0x" << a1
            << " a2=0x" << a2
            << " a3=0x" << a3
            << " s0=0x" << s0
            << " s1=0x" << s1
            << " v0=0x" << v0
            << " v1=0x" << v1
            << " a0Readable=" << (a0Readable ? "yes" : "no")
            << " a0[0]=0x" << a0Word0
            << " a0[4]=0x" << a0Word4
            << " a0[8]=0x" << a0Word8
            << " a0[c]=0x" << a0WordC
            << " s0Readable=" << (s0Readable ? "yes" : "no")
            << " s0[0]=0x" << s0Word0
            << " s0[4]=0x" << s0Word4
            << " s0[8]=0x" << s0Word8
            << " s0[c]=0x" << s0WordC
            << " recordReadable=" << (recordReadable ? "yes" : "no")
            << " record[0]=0x" << recordWord0
            << " record[4]=0x" << recordWord4
            << " record[8]=0x" << recordWord8
            << " record[c]=0x" << recordWordC
            << " vtableReadable=" << (vtableReadable ? "yes" : "no")
            << " vtbl[0]=0x" << vtableSlot0
            << " vtbl[4]=0x" << vtableSlot4
            << " vtbl[8]=0x" << vtableSlot8
            << " vtbl[c]=0x" << vtableSlotC
            << " codeRegion=" << (m_memory.isCodeAddress(targetPc) ? "yes" : "no")
            << " policy=" << static_cast<uint32_t>(policy)
            << " trace=" << formatDispatchHistory()
            << std::dec;

        static std::mutex s_missingFunctionLogMutex;
        {
            std::lock_guard<std::mutex> lock(s_missingFunctionLogMutex);
            std::cerr << oss.str() << std::endl;
        }
    }

    if (firstReport && policy == MissingFunctionPolicy::BreakOnce)
    {
#if defined(_MSC_VER)
        __debugbreak();
#endif // TODO others breakpoints
    }

    if (ctx)
    {
        ctx->pc = targetPc;
    }

    if (policy == MissingFunctionPolicy::Stop)
    {
        requestStop();
    }
}

bool PS2Runtime::dispatchGuestBranch(uint8_t *rdram,
                                     R5900Context *ctx,
                                     uint32_t targetPc,
                                     uint32_t sourcePc,
                                     uint32_t fallthroughPc,
                                     GuestBranchKind kind,
                                     const char *debugName)
{
    ctx->pc = targetPc;
    const bool isCall = (kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall);

    // Every inter-function transfer is also a deterministic EE safe point.
    // Backward edges inside generated functions use eeCheckpointDue(), while
    // this charge bounds straight-line call chains that have no local loop.
    if (m_eeScheduler && m_eeScheduler->checkpointDue(EeScheduler::kGuestDispatchCycles))
    {
        ++g_unwindEpoch;
        return false;
    }

    if (!isCall)
    {
        if (!hasFunction(targetPc))
        {
            reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);
        }

        ctx->pc = targetPc;
        return false;
    }

    if (!hasFunction(targetPc))
    {
        reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);

        const MissingFunctionPolicy policy = missingFunctionPolicy();

        if (policy == MissingFunctionPolicy::SkipCallDebug && isCall)
        {
            ctx->pc = fallthroughPc;
            return true;
        }

        if (policy == MissingFunctionPolicy::ContinueToTarget)
        {
            ctx->pc = targetPc;
            // if you need the app to keep open to open debug pannel change this to false
            return false;
        }

        return false;
    }

    RecompiledFunction targetFn = lookupFunction(targetPc);
    const uint32_t entryPc = ctx->pc;
    const uint64_t unwindEpoch = g_unwindEpoch;
    targetFn(rdram, ctx, this);

    if (isStopRequested() || ctx->pc == 0u || g_unwindEpoch != unwindEpoch)
    {
        return false;
    }

    // A stub that never set pc returned normally.
    if (ctx->pc == entryPc)
    {
        ctx->pc = fallthroughPc;
    }

    return ctx->pc == fallthroughPc;
}

void PS2Runtime::SignalException(R5900Context *ctx, PS2Exception exception)
{
    if (exception == EXCEPTION_INTEGER_OVERFLOW)
    {
        HandleIntegerOverflow(ctx);
        return;
    }

    raiseCop0Exception(ctx, static_cast<uint32_t>(exception),
                       exception == EXCEPTION_TLB_REFILL);
}

void PS2Runtime::executeVU0Microprogram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    (void)rdram;

    uint8_t *const vu0Code = m_memory.getVU0Code();
    uint8_t *const vu0Data = m_memory.getVU0Data();
    const uint32_t startPC = address & ~0x7u;

    if (!vu0Code || !vu0Data || startPC + 8u > PS2_VU0_CODE_SIZE)
    {
        seedVu0IdleSuccess(ctx);
        return;
    }

    m_vu0.reset();
    copyVu0ContextToState(ctx, m_vu0.state());
    constexpr uint32_t kVu0MicroprogramCycles = 4096u;
    m_vu0.execute(vu0Code, PS2_VU0_CODE_SIZE,
                  vu0Data, PS2_VU0_DATA_SIZE,
                  m_gs, &m_memory,
                  startPC, 0u, ctx->vu0_itop, kVu0MicroprogramCycles);
    // Hardware runs a microprogram to completion; one cut off by the budget
    // leaves partial results. Report each entry point once.
    if (m_vu0.state().cycles >= kVu0MicroprogramCycles)
    {
        static std::unordered_set<uint32_t> s_reported;
        if (s_reported.insert(startPC).second)
            std::cerr << "[vu0] microprogram at 0x" << std::hex << startPC << " stopped at pc 0x"
                      << m_vu0.state().pc << std::dec << " after the cycle budget" << std::endl;
    }
    copyVu0StateToContext(m_vu0.state(), ctx);
}

void PS2Runtime::vu0StartMicroProgram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    // VCALLMS and VCALLMSR both route here.
    executeVU0Microprogram(rdram, ctx, address);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx)
{
    handleSyscall(rdram, ctx, 0);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx, uint32_t encodedSyscallId)
{
    if (ctx->in_delay_slot)
    {
        throw std::runtime_error("Attempted to execute a syscall inside a branch delay slot! "
                                 "This breaks the atomic basic block model and is structurally unsupported by the emulator.");
    }

    const uint32_t syscallId = (encodedSyscallId != 0u)
                                   ? encodedSyscallId
                                   : getRegU32(ctx, 3); // $v1 / $3 is the EE kernel syscall number

    if (ps2_syscalls::dispatchNumericSyscall(syscallId, rdram, ctx, this))
    {
        return;
    }

    // God help you
    ps2_syscalls::TODO(rdram, ctx, this, encodedSyscallId);
}

void PS2Runtime::handleBreak(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_BREAKPOINT);
}

void PS2Runtime::drainCompletedDmacHandlers(uint8_t *rdram)
{
    for (uint32_t cause : m_memory.consumeCompletedDmacCauses())
    {
        ps2_syscalls::dispatchDmacHandlersForCause(rdram, this, cause);
    }
}

void PS2Runtime::handleTrap(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_TRAP);
}

void PS2Runtime::handleTLBR(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t index = ctx->cop0_index & 0x3Fu;
    if (!m_memory.tlbReadRegisters(index, ctx->cop0_pagemask, ctx->cop0_entryhi,
                                  ctx->cop0_entrylo0, ctx->cop0_entrylo1))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
    }
}

void PS2Runtime::handleTLBWI(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t index = ctx->cop0_index & 0x3Fu;
    if (!m_memory.tlbWriteRegisters(index, ctx->cop0_pagemask, ctx->cop0_entryhi,
                                   ctx->cop0_entrylo0, ctx->cop0_entrylo1))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
    }
}

void PS2Runtime::handleTLBWR(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t entryCount = static_cast<uint32_t>(m_memory.tlbEntryCount());
    if (entryCount == 0)
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    const uint32_t wired = std::min(ctx->cop0_wired, entryCount - 1);
    uint32_t random = ctx->cop0_random % entryCount;
    if (random < wired)
    {
        random = wired;
    }

    if (!m_memory.tlbWriteRegisters(random, ctx->cop0_pagemask, ctx->cop0_entryhi,
                                   ctx->cop0_entrylo0, ctx->cop0_entrylo1))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Keep COP0 bookkeeping in sync with the selected slot.
    ctx->cop0_index = (ctx->cop0_index & ~0x3Fu) | (random & 0x3Fu);
    ctx->cop0_random = (random <= wired) ? (entryCount - 1) : (random - 1);
}

void PS2Runtime::handleTLBP(uint8_t *rdram, R5900Context *ctx)
{
    const int32_t index = m_memory.tlbProbe(ctx->cop0_entryhi);
    if (index >= 0)
    {
        ctx->cop0_index = (ctx->cop0_index & ~0x8000003Fu) |
                          (static_cast<uint32_t>(index) & 0x3Fu);
    }
    else
    {
        // MIPS sets probe failure bit (P) in Index[31].
        ctx->cop0_index |= 0x80000000u;
    }
}

void PS2Runtime::clearLLBit(R5900Context *ctx)
{
    // LL/SC reservation is tracked separately from COP0 Status.
    ctx->llbit = 0;
    ctx->lladdr = 0;
}

uint32_t PS2Runtime::alignGuestHeapValue(uint32_t value, uint32_t alignment)
{
    if (alignment == 0)
    {
        return value;
    }

    const uint32_t mask = alignment - 1u;
    if (value > (std::numeric_limits<uint32_t>::max() - mask))
    {
        return std::numeric_limits<uint32_t>::max();
    }
    return (value + mask) & ~mask;
}

bool PS2Runtime::isGuestHeapAlignmentValid(uint32_t alignment)
{
    return alignment != 0u && (alignment & (alignment - 1u)) == 0u;
}

uint32_t PS2Runtime::normalizeGuestHeapAlignment(uint32_t alignment)
{
    if (!isGuestHeapAlignmentValid(alignment))
    {
        return kGuestHeapDefaultAlignment;
    }
    return std::max(alignment, kGuestHeapDefaultAlignment);
}

uint32_t PS2Runtime::clampGuestHeapBase(uint32_t guestBase) const
{
    uint32_t normalized = guestBase;
    if (normalized >= PS2_RAM_SIZE)
    {
        normalized &= PS2_RAM_MASK;
    }
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    return std::min(normalized, hardLimit);
}

uint32_t PS2Runtime::clampGuestHeapLimit(uint32_t guestLimit) const
{
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    if (guestLimit == 0u || guestLimit > hardLimit)
    {
        return hardLimit;
    }
    return guestLimit;
}

void PS2Runtime::resetGuestHeapLocked(uint32_t guestBase, uint32_t guestLimit)
{
    uint32_t base = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    uint32_t limit = clampGuestHeapLimit(guestLimit);
    if (base == 0u)
    {
        const uint32_t fallbackBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
        base = alignGuestHeapValue(clampGuestHeapBase(fallbackBase), kGuestHeapDefaultAlignment);
    }

    if (limit <= base)
    {
        base = alignGuestHeapValue(clampGuestHeapBase(m_guestHeapSuggestedBase), kGuestHeapDefaultAlignment);
        limit = clampGuestHeapLimit(0u);
    }

    if (limit <= base)
    {
        base = 0u;
        limit = 0u;
    }

    m_guestHeapBlocks.clear();
    if (limit > base)
    {
        m_guestHeapBlocks.push_back({base, limit - base, true});
    }

    m_guestHeapBase = base;
    m_guestHeapEnd = base;
    m_guestHeapLimit = limit;
    m_guestHeapConfigured = true;
}

void PS2Runtime::ensureGuestHeapInitializedLocked()
{
    if (m_guestHeapConfigured)
    {
        return;
    }

    const uint32_t suggested = (m_guestHeapSuggestedBase == 0u) ? kGuestHeapDefaultBase : m_guestHeapSuggestedBase;
    resetGuestHeapLocked(suggested, clampGuestHeapLimit(0u));
}

int32_t PS2Runtime::findGuestHeapBlockIndexLocked(uint32_t guestAddr) const
{
    const uint32_t normalizedAddr = guestAddr & PS2_RAM_MASK;
    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock &block = m_guestHeapBlocks[i];
        if (!block.free && block.addr == normalizedAddr)
        {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

uint32_t PS2Runtime::allocateGuestBlockLocked(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    if (size > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock block = m_guestHeapBlocks[i];
        if (!block.free)
        {
            continue;
        }

        const uint64_t blockStart = block.addr;
        const uint64_t blockEnd = blockStart + static_cast<uint64_t>(block.size);
        const uint32_t alignedAddr = alignGuestHeapValue(block.addr, normalizedAlignment);
        if (alignedAddr < block.addr)
        {
            continue;
        }

        const uint64_t alignedStart = alignedAddr;
        if (alignedStart > blockEnd)
        {
            continue;
        }

        const uint64_t allocEnd = alignedStart + static_cast<uint64_t>(allocSize);
        if (allocEnd > blockEnd)
        {
            continue;
        }

        const uint32_t prefixSize = static_cast<uint32_t>(alignedStart - blockStart);
        const uint32_t suffixSize = static_cast<uint32_t>(blockEnd - allocEnd);

        std::vector<GuestHeapBlock> replacement;
        replacement.reserve(3);
        if (prefixSize > 0u)
        {
            replacement.push_back({block.addr, prefixSize, true});
        }
        replacement.push_back({alignedAddr, allocSize, false});
        if (suffixSize > 0u)
        {
            replacement.push_back({static_cast<uint32_t>(allocEnd), suffixSize, true});
        }

        m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
        m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i),
                                 replacement.begin(),
                                 replacement.end());

        m_guestHeapEnd = std::max(m_guestHeapEnd, static_cast<uint32_t>(allocEnd));
        return alignedAddr;
    }

    return 0u;
}

void PS2Runtime::coalesceGuestHeapLocked()
{
    if (m_guestHeapBlocks.empty())
    {
        return;
    }

    size_t i = 1;
    while (i < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &prev = m_guestHeapBlocks[i - 1];
        GuestHeapBlock &curr = m_guestHeapBlocks[i];
        const uint64_t prevEnd = static_cast<uint64_t>(prev.addr) + static_cast<uint64_t>(prev.size);
        if (prev.free && curr.free && prevEnd == curr.addr)
        {
            prev.size += curr.size;
            m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
}

void PS2Runtime::freeGuestBlockLocked(uint32_t guestAddr)
{
    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return;
    }

    m_guestHeapBlocks[static_cast<size_t>(index)].free = true;
    coalesceGuestHeapLocked();
}

void PS2Runtime::configureGuestHeap(uint32_t guestBase, uint32_t guestLimit)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    uint32_t normalizedBase = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    if (normalizedBase == 0u)
    {
        normalizedBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
    }
    m_guestHeapSuggestedBase = normalizedBase;
    resetGuestHeapLocked(normalizedBase, guestLimit);
}

uint32_t PS2Runtime::guestMalloc(uint32_t size, uint32_t alignment)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    return allocateGuestBlockLocked(size, alignment);
}

uint32_t PS2Runtime::guestCalloc(uint32_t count, uint32_t size, uint32_t alignment)
{
    if (count == 0u || size == 0u)
    {
        return 0u;
    }
    if (count > (std::numeric_limits<uint32_t>::max() / size))
    {
        return 0u;
    }

    const uint32_t totalSize = count * size;
    const uint32_t guestAddr = guestMalloc(totalSize, alignment);
    if (guestAddr != 0u)
    {
        uint8_t *rdram = m_memory.getRDRAM();
        if (rdram)
        {
            uint32_t physAddr = guestAddr & PS2_RAM_MASK;
            if (physAddr + totalSize <= PS2_RAM_SIZE)
                std::memset(rdram + physAddr, 0, totalSize);
        }
    }

    return guestAddr;
}

uint32_t PS2Runtime::guestRealloc(uint32_t guestAddr, uint32_t newSize, uint32_t alignment)
{
    if (guestAddr == 0u)
    {
        return guestMalloc(newSize, alignment);
    }
    if (newSize == 0u)
    {
        guestFree(guestAddr);
        return 0u;
    }

    if (newSize > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t requestedSize = alignGuestHeapValue(newSize, kGuestHeapDefaultAlignment);

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();

    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return 0u;
    }

    const size_t blockIndex = static_cast<size_t>(index);
    const uint32_t oldAddr = m_guestHeapBlocks[blockIndex].addr;
    const uint32_t oldSize = m_guestHeapBlocks[blockIndex].size;

    if (requestedSize <= oldSize)
    {
        if (requestedSize < oldSize)
        {
            const uint32_t tailAddr = oldAddr + requestedSize;
            const uint32_t tailSize = oldSize - requestedSize;
            m_guestHeapBlocks[blockIndex].size = requestedSize;
            m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u),
                                     GuestHeapBlock{tailAddr, tailSize, true});
            coalesceGuestHeapLocked();
        }
        return oldAddr;
    }

    if (blockIndex + 1u < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &next = m_guestHeapBlocks[blockIndex + 1u];
        const uint64_t blockEnd = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].addr) +
                                  static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size);
        if (next.free && blockEnd == next.addr)
        {
            const uint64_t combined = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size) +
                                      static_cast<uint64_t>(next.size);
            if (combined >= requestedSize)
            {
                const uint32_t extraNeeded = requestedSize - m_guestHeapBlocks[blockIndex].size;
                m_guestHeapBlocks[blockIndex].size = requestedSize;
                if (next.size == extraNeeded)
                {
                    m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u));
                }
                else
                {
                    next.addr += extraNeeded;
                    next.size -= extraNeeded;
                }
                m_guestHeapEnd = std::max(m_guestHeapEnd, oldAddr + requestedSize);
                return oldAddr;
            }
        }
    }

    const uint32_t newAddr = allocateGuestBlockLocked(newSize, normalizedAlignment);
    if (newAddr == 0u)
    {
        return 0u;
    }

    uint8_t *rdram = m_memory.getRDRAM();
    if (rdram)
    {
        const uint32_t copyBytes = std::min(oldSize, newSize);
        uint32_t dstPhys = newAddr & PS2_RAM_MASK;
        uint32_t srcPhys = oldAddr & PS2_RAM_MASK;
        if (dstPhys + copyBytes <= PS2_RAM_SIZE && srcPhys + copyBytes <= PS2_RAM_SIZE)
            std::memmove(rdram + dstPhys, rdram + srcPhys, copyBytes);
    }

    freeGuestBlockLocked(oldAddr);
    return newAddr;
}

void PS2Runtime::guestFree(uint32_t guestAddr)
{
    if (guestAddr == 0u)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    freeGuestBlockLocked(guestAddr);
}

uint32_t PS2Runtime::guestHeapBase() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapBase : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapEnd() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapEnd : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapLimit() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapLimit : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::reserveAsyncCallbackStack(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);

    // Interrupt handlers and async callbacks run on the EE kernel's stacks,
    // in kernel RAM below 0x80000 that user programs (loaded at 0x100000)
    // never use. Carving them from the top of RAM put them on top of the
    // main thread's stack (which the kernel also places at the top of RAM),
    // so every VBlank/DMAC handler overwrote saved registers of the
    // interrupted frames.
    {
        constexpr uint32_t kKernelStackFloor = 0x00020000u;
        constexpr uint32_t kKernelStackCeiling = 0x00080000u;
        static uint32_t s_kernelStackTop = kKernelStackCeiling;
        const uint32_t kernelTop = s_kernelStackTop & ~(kGuestHeapDefaultAlignment - 1u);
        if (kernelTop > allocSize)
        {
            const uint32_t kernelBase = (kernelTop - allocSize) & ~(normalizedAlignment - 1u);
            if (kernelBase >= kKernelStackFloor && kernelBase < kernelTop)
            {
                s_kernelStackTop = kernelBase;
                return kernelTop - 0x10u;
            }
        }
    }

    uint32_t top = m_asyncCallbackStackTop;
    if (top > PS2_RAM_SIZE)
    {
        top = PS2_RAM_SIZE;
    }
    top &= ~(kGuestHeapDefaultAlignment - 1u);

    if (top <= allocSize)
    {
        return 0u;
    }

    uint32_t base = top - allocSize;
    base &= ~(normalizedAlignment - 1u);
    if (base < m_asyncCallbackStackFloor || base >= top)
    {
        return 0u;
    }

    m_asyncCallbackStackTop = base;
    return top - 0x10u;
}

uint8_t PS2Runtime::Load8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read8(vaddr, static_cast<uint8_t>(ctx->cop0_entryhi));
    }
    catch (const TlbFault &fault)
    {
        raiseTlbAccessFault(ctx, fault);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint16_t PS2Runtime::Load16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read16(vaddr, static_cast<uint8_t>(ctx->cop0_entryhi));
    }
    catch (const TlbFault &fault)
    {
        raiseTlbAccessFault(ctx, fault);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint32_t PS2Runtime::Load32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read32(vaddr, static_cast<uint8_t>(ctx->cop0_entryhi));
    }
    catch (const TlbFault &fault)
    {
        raiseTlbAccessFault(ctx, fault);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint64_t PS2Runtime::Load64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read64(vaddr, static_cast<uint8_t>(ctx->cop0_entryhi));
    }
    catch (const TlbFault &fault)
    {
        raiseTlbAccessFault(ctx, fault);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

__m128i PS2Runtime::Load128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read128(vaddr, static_cast<uint8_t>(ctx->cop0_entryhi));
    }
    catch (const TlbFault &fault)
    {
        raiseTlbAccessFault(ctx, fault);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return _mm_setzero_si128();
    }
}

void PS2Runtime::Store8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint8_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 1u, value, 0u, "WRITE8", ctx);
    try
    {
        m_memory.write8(vaddr, value, static_cast<uint8_t>(ctx->cop0_entryhi));
    }
    catch (const TlbFault &fault)
    {
        raiseTlbAccessFault(ctx, fault);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint16_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 2u, value, 0u, "WRITE16", ctx);
    try
    {
        m_memory.write16(vaddr, value, static_cast<uint8_t>(ctx->cop0_entryhi));
    }
    catch (const TlbFault &fault)
    {
        raiseTlbAccessFault(ctx, fault);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint32_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 4u, value, 0u, "WRITE32", ctx);
    try
    {
        m_memory.write32(vaddr, value, static_cast<uint8_t>(ctx->cop0_entryhi));
        drainCompletedDmacHandlers(rdram);
    }
    catch (const TlbFault &fault)
    {
        raiseTlbAccessFault(ctx, fault);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint64_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 8u, value, 0u, "WRITE64", ctx);
    try
    {
        m_memory.write64(vaddr, value, static_cast<uint8_t>(ctx->cop0_entryhi));
    }
    catch (const TlbFault &fault)
    {
        raiseTlbAccessFault(ctx, fault);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, __m128i value)
{
    alignas(16) uint64_t _parts[2];
    _mm_storeu_si128(reinterpret_cast<__m128i *>(_parts), value);
    ps2TraceGuestWrite(rdram, vaddr, 16u, _parts[0], _parts[1], "WRITE128", ctx);
    try
    {
        m_memory.write128(vaddr, value, static_cast<uint8_t>(ctx->cop0_entryhi));
    }
    catch (const TlbFault &fault)
    {
        raiseTlbAccessFault(ctx, fault);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::kickGifDmaChainFromMMIO(uint8_t *rdram,
                                         R5900Context *ctx,
                                         uint32_t dPcrValue,
                                         uint32_t dStatValue,
                                         uint32_t tadr,
                                         uint32_t chcr)
{
    constexpr uint32_t D_PCR = 0x1000E020u;
    constexpr uint32_t D_STAT = 0x1000E010u;
    constexpr uint32_t GIF_TADR = 0x1000A030u;
    constexpr uint32_t GIF_CHCR = 0x1000A000u;

    ps2TraceGuestWrite(rdram, D_PCR, 4u, dPcrValue, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(D_PCR, dPcrValue);
    ps2TraceGuestWrite(rdram, D_STAT, 4u, dStatValue, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(D_STAT, dStatValue);
    ps2TraceGuestWrite(rdram, GIF_TADR, 4u, tadr, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(GIF_TADR, tadr);
    ps2TraceGuestWrite(rdram, GIF_CHCR, 4u, chcr, 0u, "WRITE32", ctx);
    if (m_memory.tryProcessNativeGifImageUploadChain(m_gs, tadr, chcr))
    {
        drainCompletedDmacHandlers(rdram);
        return;
    }
    if (m_memory.tryProcessNativeGifPackedChain(m_gs, tadr, chcr))
    {
        drainCompletedDmacHandlers(rdram);
        return;
    }
    m_memory.writeIORegister(GIF_CHCR, chcr);
    m_memory.processPendingTransfers();
    drainCompletedDmacHandlers(rdram);
}

void PS2Runtime::requestStop()
{
    m_stopRequested.store(true, std::memory_order_relaxed);
    if (m_eeScheduler)
    {
        m_eeScheduler->requestStop();
    }
}

bool PS2Runtime::isStopRequested() const
{
    return m_stopRequested.load(std::memory_order_relaxed);
}

EeScheduler &PS2Runtime::eeScheduler()
{
    return *m_eeScheduler;
}

const EeScheduler &PS2Runtime::eeScheduler() const
{
    return *m_eeScheduler;
}

void PS2Runtime::postEeEvent(EeEvent event)
{
    m_eeScheduler->postEvent(event);
}

bool PS2Runtime::eeCheckpointDue(uint32_t cycles) noexcept
{
    const bool due = m_eeScheduler->checkpointDue(cycles);
    g_unwindEpoch += due ? 1u : 0u;
    return due;
}

[[noreturn]] void PS2Runtime::eeWaitVSyncTicks(uint32_t ticks, uint32_t resumePc)
{
    const uint64_t currentTick = m_eeScheduler->currentVSyncTick();
    const uint64_t waitTicks = std::max<uint64_t>(1u, ticks);
    m_eeScheduler->waitVSync(currentTick + waitTicks - 1u,
                             0,
                             [resumePc](R5900Context &context)
                             {
                                 context.pc = resumePc;
                             });
}

void PS2Runtime::addEeExitHandler(int threadId, uint32_t function, uint32_t argument)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    m_eeExitHandlers[threadId].push_back({function, argument});
}

std::vector<PS2Runtime::EeExitHandlerRegistration> PS2Runtime::takeEeExitHandlers(int threadId)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    auto it = m_eeExitHandlers.find(threadId);
    if (it == m_eeExitHandlers.end())
    {
        return {};
    }
    auto handlers = std::move(it->second);
    m_eeExitHandlers.erase(it);
    return handlers;
}

void PS2Runtime::removeEeExitHandlers(int threadId)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    m_eeExitHandlers.erase(threadId);
}

bool PS2Runtime::findEeSyscallOverride(uint32_t syscallNumber, uint32_t &handler) const
{
    std::lock_guard lock(m_eeKernelStateMutex);
    const auto it = m_eeSyscallOverrides.find(syscallNumber);
    if (it == m_eeSyscallOverrides.end())
    {
        return false;
    }
    handler = it->second;
    return true;
}

void PS2Runtime::setEeSyscallOverride(uint8_t *rdram, uint32_t syscallNumber, uint32_t handler)
{
    constexpr uint32_t kTableBase = 0x80011F80u & 0x1FFFFFFFu;
    constexpr uint32_t kMirrorLimit = 0x00080000u;
    const int64_t offset = static_cast<int64_t>(static_cast<int32_t>(syscallNumber)) * 4;
    const int64_t address = static_cast<int64_t>(kTableBase) + offset;

    std::lock_guard lock(m_eeKernelStateMutex);
    if (handler == 0u)
    {
        m_eeSyscallOverrides.erase(syscallNumber);
    }
    else
    {
        m_eeSyscallOverrides[syscallNumber] = handler;
    }
    if (!rdram || address < 0 || address + 4 > kMirrorLimit)
    {
        return;
    }
    const uint32_t guestAddress = static_cast<uint32_t>(address);
    std::memcpy(rdram + guestAddress, &handler, sizeof(handler));
    if (handler == 0u)
    {
        m_eeSyscallMirrorAddresses.erase(guestAddress);
    }
    else
    {
        m_eeSyscallMirrorAddresses.insert(guestAddress);
    }
}

void PS2Runtime::initializeEeKernelState(uint8_t *rdram)
{
    if (!rdram)
    {
        return;
    }
    constexpr uint32_t kTableGuestBase = 0x80011F80u;
    constexpr uint32_t kTableBase = kTableGuestBase & 0x1FFFFFFFu;
    constexpr uint32_t kMirrorLimit = 0x00080000u;
    constexpr uint32_t kProbeBase = 0x000002F0u;

    std::lock_guard lock(m_eeKernelStateMutex);
    for (const uint32_t address : m_eeSyscallMirrorAddresses)
    {
        const uint32_t zero = 0u;
        std::memcpy(rdram + address, &zero, sizeof(zero));
    }
    m_eeSyscallMirrorAddresses.clear();
    const uint32_t high = kTableGuestBase >> 16;
    const uint32_t low = kTableGuestBase & 0xFFFFu;
    std::memcpy(rdram + kProbeBase, &high, sizeof(high));
    std::memcpy(rdram + kProbeBase + 8u, &low, sizeof(low));
    m_eeSyscallMirrorAddresses.insert(kProbeBase);
    m_eeSyscallMirrorAddresses.insert(kProbeBase + 8u);

    for (const auto &[syscallNumber, handler] : m_eeSyscallOverrides)
    {
        const int64_t offset = static_cast<int64_t>(static_cast<int32_t>(syscallNumber)) * 4;
        const int64_t address = static_cast<int64_t>(kTableBase) + offset;
        if (address < 0 || address + 4 > kMirrorLimit)
        {
            continue;
        }
        const uint32_t guestAddress = static_cast<uint32_t>(address);
        std::memcpy(rdram + guestAddress, &handler, sizeof(handler));
        m_eeSyscallMirrorAddresses.insert(guestAddress);
    }
}

void PS2Runtime::HandleIntegerOverflow(R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_INTEGER_OVERFLOW);
}

void PS2Runtime::run()
{
    m_stopRequested.store(false, std::memory_order_relaxed);
    ps2_stubs::resetSifState();
    resetIop();
    ps2_stubs::resetAudioStubState();
    ps2_stubs::resetMpegStubState();
    initializeEeKernelState(m_memory.getRDRAM());
    m_cpuContext.r[4] = _mm_setzero_si128();
    m_cpuContext.r[5] = _mm_setzero_si128();
    m_cpuContext.r[29] = _mm_set_epi64x(0, static_cast<int64_t>(PS2_RAM_SIZE - 0x10u));
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);
    m_debugRa.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)), std::memory_order_relaxed);
    m_debugSp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[29], 0)), std::memory_order_relaxed);
    m_debugGp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[28], 0)), std::memory_order_relaxed);

    RUNTIME_LOG("Starting execution at address 0x" << std::hex << m_cpuContext.pc << std::dec);

    // A blank image to use as a framebuffer
    Image blank = GenImageColor(FB_WIDTH, FB_HEIGHT, BLANK);
    Texture2D frameTex = LoadTextureFromImage(blank);
    UnloadImage(blank);

    // Escape belongs to the game's menu controls. Close the native window with
    // its close button or Alt+F4 instead of Raylib's default Escape shortcut.
    SetExitKey(KEY_NULL);
    ps2_host_input::poll();
    std::atomic<bool> gameThreadFinished{false};

    applySavedOutputSettings(m_gs);

    std::thread gameThread([&]()
                           {
        ThreadNaming::SetCurrentThreadName("GameThread");
        try
        {
            // General EE/VU0 arithmetic uses chop; COP1 DIV/SQRT/RSQRT helpers
            // select nearest locally. The comparison switch affects only the
            // general EE mode and never leaks to another host thread.
            const char *eeRound = std::getenv("PS2X_EE_ROUND");
            const bool compareNearest = eeRound && std::strcmp(eeRound, "nearest") == 0;
            const Ps2ScopedHostRounding guestRounding(compareNearest ? FE_TONEAREST : FE_TOWARDZERO);
            std::fprintf(stderr, "[ee:round] general EE arithmetic uses %s; COP1 divide/root uses nearest\n",
                compareNearest ? "nearest (comparison)" : "round-toward-zero");
            m_eeScheduler->reset(m_memory.getRDRAM(), m_cpuContext);
            m_eeScheduler->run();
            uint32_t pc = m_debugPc.load(std::memory_order_relaxed);
            RUNTIME_LOG("Game thread returned. PC=0x" << std::hex << pc
                      << " RA=0x" << static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)) << std::dec << std::endl);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error during program execution: " << e.what() << std::endl;
        }
        catch (...)
        {
            std::cerr << "Error during program execution: unknown exception" << std::endl;
        }
        gameThreadFinished.store(true, std::memory_order_release); });

    uint64_t tick = 0;
    const int hostFpsOverride = configuredHostTargetFps();
    uint64_t lastPresentationField = 0;
    ps2x_dev::Harness devHarness;
    devHarness.runtime = this;
    devHarness.init();
    while (!isStopRequested() && !gameThreadFinished.load(std::memory_order_acquire))
    {
        if (hostFpsOverride == 0 && m_eeScheduler)
        {
            m_eeScheduler->waitForNextPresentationField(lastPresentationField);
        }
        m_audioBackend.updatePcmStream();
        devHarness.step(m_eeScheduler->currentVSyncTick());
        ps2_host_input::poll(m_eeScheduler->currentVSyncTick());
        PS2_IF_AGRESSIVE_LOGS({
            tick++;
            if ((tick % 120) == 0)
            {
                uint64_t curDma = m_memory.dmaStartCount();
                uint64_t curGif = m_memory.gifCopyCount();
                uint64_t curGs = m_memory.gsWriteCount();
                uint64_t curVif = m_memory.vifWriteCount();
                const GSRegisters &gs = m_memory.gs();
                const uint32_t dbgPc = m_debugPc.load(std::memory_order_relaxed);
                const uint32_t dbgRa = m_debugRa.load(std::memory_order_relaxed);
                const uint32_t dbgSp = m_debugSp.load(std::memory_order_relaxed);
                const uint32_t dbgGp = m_debugGp.load(std::memory_order_relaxed);
                const auto eeSnapshot = m_eeScheduler->snapshot();

                RUNTIME_LOG("[run:tick] tick=" << tick
                                               << " pc=0x" << std::hex << dbgPc
                                               << " ra=0x" << dbgRa
                                               << " sp=0x" << dbgSp
                                               << " gp=0x" << dbgGp
                                               << " dispfb1=0x" << gs.dispfb1
                                               << " display1=0x" << gs.display1
                                               << std::dec
                                               << " activeThreads=" << eeSnapshot.threads.size()
                                               << " dma=" << curDma
                                               << " gif=" << curGif
                                               << " gsw=" << curGs
                                               << " vif=" << curVif
                                               << std::endl);

            }
        });
        uint32_t presentWidth = FB_WIDTH;
        uint32_t presentHeight = DEFAULT_DISPLAY_HEIGHT;
        uint32_t displayWidth = FB_WIDTH;
        uint32_t displayHeight = DEFAULT_DISPLAY_HEIGHT;
        UploadFrame(frameTex, this, presentWidth, presentHeight, displayWidth, displayHeight);

        BeginDrawing();
        ClearBackground(BLACK);
        const float srcWidth = static_cast<float>(std::max<uint32_t>(1u, presentWidth));
        const float srcHeight = static_cast<float>(std::max<uint32_t>(1u, presentHeight));
        // Letterbox by the logical display size, not the stored image size.
        const float aspectWidth = static_cast<float>(std::max<uint32_t>(1u, displayWidth));
        const float aspectHeight = static_cast<float>(std::max<uint32_t>(1u, displayHeight));
        const float screenWidth = static_cast<float>(GetScreenWidth());
        const float screenHeight = static_cast<float>(GetScreenHeight());
        const float scale = std::min(screenWidth / aspectWidth, screenHeight / aspectHeight);
        const float dstWidth = aspectWidth * scale;
        const float dstHeight = aspectHeight * scale;
        const Rectangle srcRect{0.0f, 0.0f, srcWidth, srcHeight};
        const Rectangle dstRect{
            (screenWidth - dstWidth) * 0.5f,
            (screenHeight - dstHeight) * 0.5f,
            dstWidth,
            dstHeight};
        // DW5XL-DEBUG issue 023: PS2X_WEAVE_TRACE=1 logs the window size and the picture rectangle whenever they change.
        {
            static const bool s_rectTrace = [] { const char *v = std::getenv("PS2X_WEAVE_TRACE"); return v && *v && *v != '0'; }();
            static float s_lastRect[6] = {};
            const float rect[6] = {screenWidth, screenHeight, dstRect.x, dstRect.y, dstRect.width, dstRect.height};
            if (s_rectTrace && std::memcmp(rect, s_lastRect, sizeof(rect)) != 0)
            {
                std::fprintf(stderr, "[draw:rect] tick=%llu screen=%.0fx%.0f dst=%.1f,%.1f %.1fx%.1f src=%.0fx%.0f\n",
                             static_cast<unsigned long long>(m_eeScheduler->currentVSyncTick()), screenWidth, screenHeight,
                             dstRect.x, dstRect.y, dstRect.width, dstRect.height, srcWidth, srcHeight);
                std::memcpy(s_lastRect, rect, sizeof(rect));
            }
        }
        DrawTexturePro(frameTex, srcRect, dstRect, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
        m_pictureRect[0] = dstRect.x; m_pictureRect[1] = dstRect.y;   // issue 023 3S: for overlays over the picture
        m_pictureRect[2] = dstRect.width; m_pictureRect[3] = dstRect.height;
        ps2_host_input::drawMouseCaptureIndicator(dstRect.x, dstRect.y, dstRect.width, dstRect.height);
        if (m_debugUiInitialized && m_debugUiDrawCallback)
        {
            m_debugUiDrawCallback(*this, m_debugUiUserData);
        }
        devHarness.captureHostFrame();

        // Function keys: one table, runtime/hotkeys.h (owner layout 2026-10-20: F2 FPS, F3 AA, F4 scale, F8 build ID).
        // On-screen FPS & VSync rate overlay: the show_fps setting (PC SETTINGS, issue 023 3S), toggled and saved
        // with F2 too; PS2X_FPS=1 turns it on for a run.
        static const bool s_showFpsInit = [this]() { m_showFps.store(loadShowFpsSetting(), std::memory_order_relaxed); return true; }();
        (void)s_showFpsInit;
        if (IsKeyPressed(ps2x::hotkeys::key(ps2x::hotkeys::Action::FpsOverlay)))
        {
            setShowFps(!showFps());
            ps2x::settings::saveText("show_fps", showFps() ? "on" : "off");
        }
        const bool s_showFpsOverlay = showFps();
        // F8: build ID and game tick, to match screenshots with logs (off by default).
        static bool s_showBuildOverlay = false;
        if (IsKeyPressed(ps2x::hotkeys::key(ps2x::hotkeys::Action::BuildOverlay)))
        {
            s_showBuildOverlay = !s_showBuildOverlay;
        }
        // F4 cycles native/2x/4x output; F3 cycles the anti-aliasing choice (1-16 samples
        // per output pixel). The two are independent: 2x/4x use 4/16 samples to build the
        // larger image and AA multiplies that, up to the 16-sample limit (4x: no extra AA).
        if (IsKeyPressed(ps2x::hotkeys::key(ps2x::hotkeys::Action::OutputScale)))
        {
            const uint32_t current = m_gs.getResolutionScale();
            m_gs.setResolutionScale(current == 1u ? 2u : current == 2u ? 4u : 1u);
            if (m_gs.getResolutionScale() == current)
                m_gs.setResolutionScale(1u);
            saveIniValue("resolution_scale", m_gs.getResolutionScale());
        }
        if (IsKeyPressed(ps2x::hotkeys::key(ps2x::hotkeys::Action::AntiAliasing)))
        {
            s_aaChoice = s_aaChoice >= 16u ? 1u : s_aaChoice * 2u;
            m_gs.setAASamples(s_aaChoice);
            saveIniValue("aa_samples", s_aaChoice);
        }

        // F2 overlay numbers (issue 014), about once a second. VSync = guest fields per second, timed by the fields'
        // own emission times (so a steady 50 reads 50.0, not 49/51 from where the window falls). FPS = the
        // game's frame rate over the same span: fields in which it finished a frame (ps2x::device::gameFrameCount)
        // or, during movies, the pictures the movie decoder handed to the game (its frame syncs do not always
        // have device work), whichever is higher. The host window loop itself is paced by VBlankEnd (50 Hz) and is not shown.
        static auto s_lastFpsTime = std::chrono::steady_clock::now();
        static uint64_t s_lastVsyncTick = 0;
        static int64_t s_lastVsyncNs = 0;
        static uint64_t s_lastGameFrames = 0;
        static uint64_t s_lastMoviePictures = 0;
        static float s_vsyncRate = 0.0f;
        static float s_gameFps = 0.0f;
        static float s_maxFrameMs = 0.0f; // worst host frame in the HUD window
        s_maxFrameMs = std::max(s_maxFrameMs, GetFrameTime() * 1000.0f);

        const auto nowFps = std::chrono::steady_clock::now();
        const float elapsedFps = std::chrono::duration<float>(nowFps - s_lastFpsTime).count();
        const uint64_t currentVsync = eeScheduler().currentVSyncTick();
        if (elapsedFps >= 1.0f)
        {
            auto &gsRegs = m_memory.gs();
            uint64_t tick = 0;
            int64_t tickNs = 0;
            do
            {
                tick = gsRegs.vsyncTick.load(std::memory_order_acquire);
                tickNs = m_memory.vsyncTickNs().load(std::memory_order_relaxed);
            } while (tick != gsRegs.vsyncTick.load(std::memory_order_acquire));
            const uint64_t gameFrames = ps2x::device::gameFrameCount();
            const uint64_t moviePictures = ps2_stubs::moviePictureCount();
            const double frames = static_cast<double>(std::max(gameFrames - s_lastGameFrames, moviePictures - s_lastMoviePictures));
            const int64_t spanNs = tickNs - s_lastVsyncNs;
            if (s_lastVsyncNs != 0 && tick > s_lastVsyncTick && spanNs > 0)
            {
                const double span = static_cast<double>(spanNs) * 1e-9;
                s_vsyncRate = static_cast<float>(static_cast<double>(tick - s_lastVsyncTick) / span);
                // A screen shows at most one frame per field: frames are counted at the sample moments, not
                // exactly on field boundaries, so clamp the +-1 slack.
                s_gameFps = std::min(s_vsyncRate, static_cast<float>(frames / span));
            }
            else
            {
                s_vsyncRate = 0.0f; // no new field: the guest clock is stalled
                s_gameFps = static_cast<float>(frames / elapsedFps);
            }
            // PS2X_HUD_LOG=1 logs the F2 overlay numbers (game FPS, guest VSync, worst host frame) for unattended runs.
            static const bool s_hudLog = [] { const char *v = std::getenv("PS2X_HUD_LOG"); return v && *v && *v != '0'; }();
            if (s_hudLog)
                std::fprintf(stderr, "[hud] tick=%llu fps=%.1f vsync=%.1f frameMs=%.1f maxFrameMs=%.1f\n",
                             static_cast<unsigned long long>(currentVsync), s_gameFps, s_vsyncRate, GetFrameTime() * 1000.0f, s_maxFrameMs);
            s_lastVsyncTick = tick;
            s_lastVsyncNs = tickNs;
            s_lastGameFrames = gameFrames;
            s_lastMoviePictures = moviePictures;
            s_lastFpsTime = nowFps;
            s_maxFrameMs = 0.0f;
        }

        if (s_showFpsOverlay && !IsWindowHidden())
        {
            const uint32_t aaNow = m_gs.getAASamples();
            const char *fpsText = TextFormat("FPS: %.0f  |  VSync: %.1f  |  Output: %ux (%ux%u)  |  AA: %ux%s",
                s_gameFps, s_vsyncRate, m_gs.getResolutionScale(), presentWidth, presentHeight, aaNow,
                aaNow < s_aaChoice ? TextFormat(" (set %ux, max at this output)", s_aaChoice) : "");
            constexpr int kFontSize = 18;
            const int textW = MeasureText(fpsText, kFontSize);
            DrawRectangle(8, 8, textW + 16, kFontSize + 10, Fade(BLACK, 0.75f));
            const Color statColor = (s_gameFps >= 45.0f && s_vsyncRate >= 45.0f) ? GREEN :
                                    (s_gameFps >= 25.0f && s_vsyncRate >= 25.0f) ? YELLOW : RED;
            DrawText(fpsText, 16, 13, kFontSize, statColor);
        }
        if (s_showBuildOverlay && !IsWindowHidden())
        {
            const char *buildText = TextFormat("Build: %s  |  Tick: %llu",
                m_buildId.empty() ? "unknown" : m_buildId.c_str(), static_cast<unsigned long long>(currentVsync));
            constexpr int kFontSize = 18;
            const int y = s_showFpsOverlay ? 44 : 8;
            DrawRectangle(8, y, MeasureText(buildText, kFontSize) + 16, kFontSize + 10, Fade(BLACK, 0.75f));
            DrawText(buildText, 16, y + 5, kFontSize, WHITE);
        }

        // DW5XL-DEBUG issue 023 3S: PS2X_WINDOW_SHOT=<field>:<path.png> saves the whole window (overlays included)
        // on the first frame at or after that field — the frame dumps hold only the game picture.
        {
            static uint64_t s_shotTick = 0; static std::string s_shotPath; static bool s_shotInit = false;
            if (!s_shotInit) {
                s_shotInit = true;
                if (const char *v = std::getenv("PS2X_WINDOW_SHOT"); v && std::strchr(v, ':')) {
                    s_shotTick = std::strtoull(v, nullptr, 10); s_shotPath = std::strchr(v, ':') + 1;
                }
            }
            if (!m_windowShotRequest.empty() && s_shotPath.empty()) { s_shotPath = m_windowShotRequest; s_shotTick = 0; m_windowShotRequest.clear(); }
            if (!s_shotPath.empty() && currentVsync >= s_shotTick) {
                ::Image shot = LoadImageFromScreen();
                ExportImage(shot, s_shotPath.c_str());
                UnloadImage(shot);
                std::fprintf(stderr, "[window:shot] field %llu -> %s\n", static_cast<unsigned long long>(currentVsync), s_shotPath.c_str());
                s_shotPath.clear();
            }
        }
        EndDrawing();
        ps2x::present_feedback::frameSwapped(s_windowFrameField, s_windowFrameNewPicture); // DW5XL-DEBUG issue 023 3S

        if (WindowShouldClose())
        {
            RUNTIME_LOG("[run] window close requested, breaking out of loop");
            requestStop();
            break;
        }
    }

    requestStop();
    if (gameThread.joinable())
    {
        gameThread.join();
    }
    // The EE is gone: let the device worker finish queued GS work, then stop it.
    ps2x::device::detach(m_memory);
    GSTrace::shutdown();

    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }
    UnloadTexture(frameTex);
    CloseWindow();

    RUNTIME_LOG("[run] exiting loop");
}

