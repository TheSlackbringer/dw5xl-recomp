// Issue 049 Part 2 (amendment v3.1): in-between pictures from the game's finished command list.
// P2.3 prototype, DW5XL-DEBUG (no setting yet; MOTION SMOOTHING comes with P2.5):
//   PS2X_SMOOTH_PROTO=<n>[:after] — every time the display thread sends a new frame's list (sceDmaSend, called from
//   FUN_00142860 at 0x1428DC), the list is copied into a VIF1 stream of our own (the chain walked as the DMAC walks it,
//   the tags' VIF codes included when CHCR.TTE is set, the data of every reference copied) and sent n times through
//   the device as a detached stream (ps2x::device::submitDetachedVif1: no DMA channel, no completion the game could
//   see). Default: before the game's own list, as in-between pictures will be (the game's picture N stays the last
//   one drawn). ":after": after it, so the field shows our copy: with an unchanged copy the pictures must be
//   IDENTICAL to a run without the switch (the copy is right).
//   Logs "[smooth] ..." every 250 frames: list size, copy time.
//   The frame start is not in that list. Before it the display thread's FUN_001429e0 sends, on the GIF channel:
//   - with sceDmaSendN, the copy of the finished picture from the drawing buffer (page 64) to the display buffer
//     (page 0): 8 or 16 sprites, with their GS setup;
//   - through FUN_00111910 -> FUN_00111590 (which starts the GIF DMA itself), the field's draw environment: FRAME page
//     64, ZBUF, XYOFFSET for the field's parity, ..., and the sprite that clears the drawing buffer and its Z buffer.
//   Each copy is both packets (via VIF DIRECT), the copy's vertex writes into the display buffer turned into NOP
//   writes, then the list: it draws on a cleared buffer as the game does and leaves the shown picture alone. Before
//   the game's list both packets follow the copies once more, so the game's own list draws on a cleared buffer too
//   (P2.3, Linux, 2026-11-09: without the clear the HUD was blended twice).
#include "game_overrides.h"
#include "ps2_runtime_macros.h"
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_device_queue.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
constexpr uint32_t kDmaSend = 0x00112FE8u;          // sceDmaSend (runtime HLE stub)
constexpr uint32_t kSendNewFrame = 0x001428E4u;     // its return address in the display thread, new frame
constexpr uint32_t kDmaSendN = 0x00113050u;         // sceDmaSendN (runtime HLE stub)
constexpr uint32_t kStartSendA = 0x00142A9Cu, kStartSendB = 0x00142ABCu; // its return addresses in FUN_001429e0
constexpr uint32_t kSwapEnv = 0x00111910u;          // puts the draw environment of a field's parity
constexpr uint32_t kSwapEnvFromStart = 0x00142B74u; // its return address in FUN_001429e0
constexpr uint32_t kPutDrawEnv = 0x00111590u;       // starts the GIF DMA of one draw environment packet
constexpr uint32_t kVif1Chcr = 0x10009000u;
PS2Runtime::RecompiledFunction originalSend = nullptr, originalSendN = nullptr, originalSwapEnv = nullptr,
                               originalPutDrawEnv = nullptr;
unsigned g_copies = 0;
bool g_after = false;
std::vector<uint8_t> g_start;                       // the latest frame start, display-buffer kicks removed
std::vector<uint8_t> g_env;                         // the latest draw environment with the clear
bool g_startOk = false, g_startShown = false, g_envOk = false, g_inSwapEnv = false;
uint64_t g_frames = 0, g_bytes = 0, g_failed = 0, g_noStart = 0;
double g_buildMs = 0.0;

// The frame start's GIF packets with every vertex kick (XYZ2/XYZF2/XYZ3/XYZF3) that draws into the display buffer
// (FRAME fbp 0 in the drawing context) removed: an A+D kick becomes an A+D write to NOP; in a PACKED or REGLIST tag
// whose drawing context targets the display buffer, the kick registers in the tag's register list become NOP (FRAME
// cannot change inside such a tag). Returns the kicks removed, or -1 for a packet it cannot read. `shape` describes
// the GIF tags (nloop/flg/nreg/regs).
int stripDisplayCopy(std::vector<uint8_t> &p, std::string &shape)
{
    uint32_t fbp[2] = {~0u, ~0u}, ctxt = 0;
    int removed = 0;
    const size_t count = p.size() / 16u;
    auto kick = [](uint32_t reg) { return reg == 0x04u || reg == 0x05u || reg == 0x0Cu || reg == 0x0Du; };
    for (size_t q = 0; q < count;) {
        uint64_t lo, hi;
        std::memcpy(&lo, &p[q * 16u], 8);
        std::memcpy(&hi, &p[q * 16u + 8u], 8);
        const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu), flg = static_cast<uint32_t>((lo >> 58) & 3u);
        const uint32_t nreg = static_cast<uint32_t>((lo >> 60) & 0xFu) ? static_cast<uint32_t>((lo >> 60) & 0xFu) : 16u;
        if ((lo >> 46) & 1u) ctxt = static_cast<uint32_t>((lo >> 56) & 1u);   // PRE: PRIM's CTXT (bit 9 of PRIM)
        char tag[48];
        std::snprintf(tag, sizeof(tag), "%s%u/%u/%u/%llx", shape.empty() ? "" : " ", nloop, flg, nreg,
                      static_cast<unsigned long long>(hi));
        shape += tag;
        const size_t tagAt = q++;
        if (flg >= 2u) {                                                       // IMAGE
            q += nloop;
            continue;
        }
        const size_t words = flg == 0u ? 2u : 1u;                              // 64-bit words per register
        if (static_cast<size_t>(nloop) * nreg * words > (count - q) * 2u) return -1;
        bool hasAd = false, hasKick = false;
        for (uint32_t r = 0; r < nreg; ++r) {
            const uint32_t reg = static_cast<uint32_t>((hi >> (4u * r)) & 0xFu);
            hasAd |= reg == 0x0Eu;
            hasKick |= kick(reg);
            if (reg == 0x00u && nloop > 0u) {                                  // PRIM in the list: its first value
                uint64_t data;
                std::memcpy(&data, &p[q * 16u + r * words * 8u], 8);
                ctxt = static_cast<uint32_t>((data >> 9) & 1u);
            }
        }
        if (hasKick && fbp[ctxt] == 0u) {                                      // vertex registers → NOP
            for (uint32_t r = 0; r < nreg; ++r)
                if (kick(static_cast<uint32_t>((hi >> (4u * r)) & 0xFu))) {
                    hi |= 0xFull << (4u * r);
                    removed += static_cast<int>(nloop);
                }
            std::memcpy(&p[tagAt * 16u + 8u], &hi, 8);
        }
        if (flg == 0u && hasAd) {                                              // PACKED with A+D
            for (uint32_t l = 0; l < nloop; ++l)
                for (uint32_t r = 0; r < nreg; ++r) {
                    const size_t at = q + static_cast<size_t>(l) * nreg + r;
                    if (((hi >> (4u * r)) & 0xFu) != 0x0Eu) continue;
                    uint64_t data;
                    std::memcpy(&data, &p[at * 16u], 8);
                    const uint32_t addr = p[at * 16u + 8u];
                    if (addr == 0x4Cu || addr == 0x4Du) fbp[addr - 0x4Cu] = static_cast<uint32_t>(data & 0x1FFu);
                    if (addr == 0x00u) ctxt = static_cast<uint32_t>((data >> 9) & 1u);
                    if (kick(addr) && fbp[ctxt] == 0u) {
                        p[at * 16u + 8u] = 0x7Fu;                              // NOP
                        ++removed;
                    }
                }
        }
        q += flg == 0u ? static_cast<size_t>(nloop) * nreg : (static_cast<size_t>(nloop) * nreg + 1u) / 2u;
    }
    return removed;
}

void captureStart(const uint8_t *ram, uint32_t addr, uint32_t qwc)
{
    g_startOk = false;
    if ((addr & 0x70000000u) == 0x70000000u) return;                         // scratchpad: not copied here
    addr &= 0x01FFFFF0u;
    if (qwc == 0u || qwc > 0xFFFFu || addr + qwc * 16u > PS2_RAM_SIZE) return;
    std::vector<uint8_t> packet(ram + addr, ram + addr + qwc * 16u);
    std::string shape;
    const int removed = stripDisplayCopy(packet, shape);
    if (!g_startShown) {
        g_startShown = true;
        std::fprintf(stderr, "[smooth] frame start: %u qw at 0x%X, GIF tags (nloop/flg/nreg/regs) %s; %d kicks into the "
                     "display buffer removed\n", qwc, addr, shape.c_str(), removed);
    }
    if (removed < 0) return;
    // VIF1: three NOPs, DIRECT (the packet goes to the GIF through PATH2), so the packet starts on a quadword.
    g_start.assign(16u, 0u);
    const uint32_t direct = 0x50000000u | qwc;
    std::memcpy(&g_start[12], &direct, 4);
    g_start.insert(g_start.end(), packet.begin(), packet.end());
    g_startOk = true;
}

void captureEnv(const uint8_t *ram, uint32_t addr)
{
    g_envOk = false;
    if ((addr & 0x70000000u) == 0x70000000u) return;                         // scratchpad: not copied here
    addr &= 0x01FFFFF0u;
    if (addr + 16u > PS2_RAM_SIZE) return;
    uint64_t tag;
    std::memcpy(&tag, ram + addr, 8);
    const uint32_t qwc = static_cast<uint32_t>(tag & 0x7FFFu) + 1u;          // as FUN_00111590 sets the DMA's QWC
    if (addr + qwc * 16u > PS2_RAM_SIZE) return;
    g_env.assign(16u, 0u);
    const uint32_t direct = 0x50000000u | qwc;
    std::memcpy(&g_env[12], &direct, 4);
    g_env.insert(g_env.end(), ram + addr, ram + addr + qwc * 16u);
    g_envOk = true;
}

// The VIF1 stream the DMAC makes of a source chain in RDRAM (refe/cnt/next/ref/refs/call/ret/end). Returns false for a
// chain it cannot copy whole (scratchpad, out of range, too long): the caller then sends nothing.
bool buildStream(const uint8_t *ram, uint32_t start, bool tte, std::vector<uint8_t> &out)
{
    out.clear();
    uint32_t tadr = start & 0x01FFFFF0u, stack[2] = {};
    int sp = 0;
    for (uint32_t steps = 0; steps < 100000u; ++steps) {
        if (tadr + 16u > PS2_RAM_SIZE) return false;
        uint64_t tag;
        std::memcpy(&tag, ram + tadr, 8);
        const uint32_t qwc = static_cast<uint32_t>(tag & 0xFFFFu), id = static_cast<uint32_t>((tag >> 28) & 7u);
        const uint32_t addr = static_cast<uint32_t>((tag >> 32) & 0x7FFFFFF0u);
        if ((tag >> 63) != 0u) return false;                       // scratchpad data: not copied here
        if (tte) out.insert(out.end(), ram + tadr + 8u, ram + tadr + 16u);
        uint32_t data = tadr + 16u;
        bool end = false;
        switch (id) {
        case 0: data = addr; end = true; break;                    // refe
        case 1: tadr = data + qwc * 16u; break;                    // cnt
        case 2: tadr = addr; break;                                // next
        case 3: case 4: data = addr; tadr += 16u; break;           // ref, refs
        case 5: if (sp < 2) stack[sp++] = data + qwc * 16u; tadr = addr; break;   // call
        case 6: if (sp == 0) end = true; else tadr = stack[--sp]; break;          // ret
        default: end = true; break;                                // end
        }
        data &= 0x01FFFFF0u;
        if (data + qwc * 16u > PS2_RAM_SIZE) return false;
        out.insert(out.end(), ram + data, ram + data + qwc * 16u);
        if (end) return true;
        tadr &= 0x01FFFFF0u;
    }
    return false;
}

void sendCopies(uint8_t *ram, PS2Runtime *runtime, uint32_t list)
{
    const auto t0 = std::chrono::steady_clock::now();
    const bool tte = (runtime->memory().readIORegister(kVif1Chcr) & 0x40u) != 0u;
    if (!g_startOk || !g_envOk) {
        ++g_noStart;
        return;
    }
    std::vector<uint8_t> start(g_start);
    start.insert(start.end(), g_env.begin(), g_env.end());
    std::vector<uint8_t> stream(start);
    std::vector<uint8_t> chain;
    if (!buildStream(ram, list, tte, chain)) {
        ++g_failed;
        return;
    }
    stream.insert(stream.end(), chain.begin(), chain.end());
    g_buildMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    g_bytes += stream.size();
    for (unsigned i = 0; i < g_copies; ++i)
        ps2x::device::submitDetachedVif1(runtime->memory(), i + 1u < g_copies ? stream : std::move(stream));
    if (!g_after)                                   // the game's own list then draws on a cleared buffer as well
        ps2x::device::submitDetachedVif1(runtime->memory(), std::move(start));
    if (++g_frames % 250u == 0u) {
        std::fprintf(stderr, "[smooth] %llu frames: list %.0f KB, copy %.3f ms (average), %u extra per frame %s, %llu not "
                     "copied, %llu without a frame start, field %llu\n", static_cast<unsigned long long>(g_frames),
                     g_bytes / 250.0 / 1024.0, g_buildMs / 250.0, g_copies,
                     g_after ? "after the game's list" : "before the game's list",
                     static_cast<unsigned long long>(g_failed), static_cast<unsigned long long>(g_noStart),
                     static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()));
        g_bytes = 0;
        g_buildMs = 0.0;
    }
}

void send(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t ra = getRegU32(ctx, 31), list = getRegU32(ctx, 5);
    const bool newFrame = ra == kSendNewFrame;
    if (newFrame && !g_after) sendCopies(ram, runtime, list);
    originalSend(ram, ctx, runtime);
    if (newFrame && g_after) sendCopies(ram, runtime, list);
}

void sendN(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t ra = getRegU32(ctx, 31);
    if (ra == kStartSendA || ra == kStartSendB) captureStart(ram, getRegU32(ctx, 5), getRegU32(ctx, 6));
    originalSendN(ram, ctx, runtime);
}

void swapEnv(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_inSwapEnv = getRegU32(ctx, 31) == kSwapEnvFromStart;
    originalSwapEnv(ram, ctx, runtime);
    g_inSwapEnv = false;
}

void putDrawEnv(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (g_inSwapEnv) captureEnv(ram, getRegU32(ctx, 4));
    originalPutDrawEnv(ram, ctx, runtime);
}

void install(PS2Runtime &runtime)
{
    const char *proto = std::getenv("PS2X_SMOOTH_PROTO");
    if (!proto || !*proto) return;
    char *rest = nullptr;
    g_copies = static_cast<unsigned>(std::strtoul(proto, &rest, 10));
    g_after = rest && std::strcmp(rest, ":after") == 0;
    if (g_copies == 0u) return;
    if (runtime.memory().read32(kDmaSend) != 0x27BDFFD0u || runtime.memory().read32(kDmaSendN) != 0x27BDFFC0u ||
        runtime.memory().read32(kSwapEnv) != 0x27BDFFD0u || runtime.memory().read32(kPutDrawEnv) != 0x27BDFFF0u) {
        std::fprintf(stderr, "[smooth] declined: unexpected code at a replaced function\n");
        return;
    }
    originalSend = runtime.lookupFunction(kDmaSend);
    runtime.replaceFunction(kDmaSend, &send);
    originalSendN = runtime.lookupFunction(kDmaSendN);
    runtime.replaceFunction(kDmaSendN, &sendN);
    originalSwapEnv = runtime.lookupFunction(kSwapEnv);
    runtime.replaceFunction(kSwapEnv, &swapEnv);
    originalPutDrawEnv = runtime.lookupFunction(kPutDrawEnv);
    runtime.replaceFunction(kPutDrawEnv, &putDrawEnv);
    std::fprintf(stderr, "[smooth] prototype: %u copies of each new frame's list, %s\n", g_copies,
                 g_after ? "after the game's list" : "before the game's list");
}
} // namespace

PS2_REGISTER_GAME_OVERRIDE("DW5XL in-between pictures (issue 049, P2.3 prototype)", "SLES_538.60", 0u, 0u, &install)
