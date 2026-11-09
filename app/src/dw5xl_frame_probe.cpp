// DW5XL-DEBUG issue 049 P1.1: a measurement probe for the game's time step, not a fix.
// PS2X_FRAME_PROBE=<from field>[:<ms>] logs every game frame from that field on ("[frame-probe] frame N field F"), and
// with <ms> the game thread waits that long before each frame end, so a battle runs at fewer game frames than fields.
// A counter that follows the game frames then grows slower than the fields; one that follows the fields does not.
// Two frame ends: 0x1425D0 (battle and most loops; also wrapped by dw5xl_main_menu.cpp) and 0x142BC0 (other loops:
// marks a picture ready if asked, waits for the field, reads the pads); both share the frame count. Some scenes draw in
// the display thread's per-VBlank callback instead (gp-0x7BB4, set at 0x142BA4); every new picture, from either way,
// is logged at its flip ("[frame-probe] flip N field F": 0x1133B8, called only where the display thread flips).
// The real-time Opening (OVL_00) stops the display thread's flips and has a frame end of its own (0x67C8A0); every frame
// end, the overlay's included, first calls 0x147FE0, logged as "[frame-probe] draw N field F" (its own count).
// PS2X_FRAME_PROBE_RAM=<field> (049 P2.1): at the first game frame end from that field on, the game thread writes the
// 32 MB of EE RAM to <PS2X_DUMP_DIR>/ram_frame_<field>.bin before the frame end runs: game RAM at a defined point of
// the game's own loop, so two runs can be compared frame for frame.
#include "game_overrides.h"
#include "ps2_runtime_macros.h"
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace {
constexpr uint32_t kFrameEnd = 0x001425D0u, kFrameEndWait = 0x00142BC0u, kFlip = 0x001133B8u, kDraw = 0x00147FE0u;
PS2Runtime::RecompiledFunction originalFrameEnd = nullptr, originalFrameEndWait = nullptr, originalFlip = nullptr,
                               originalDraw = nullptr;
uint64_t g_from = 0, g_frames = 0, g_flips = 0, g_draws = 0;
unsigned g_waitMs = 0;
uint64_t g_ramField = 0;   // 0: no RAM dump

void dumpRam(PS2Runtime *runtime, uint64_t field)
{
    const char *dir = std::getenv("PS2X_DUMP_DIR");
    char name[64];
    std::snprintf(name, sizeof(name), "/ram_frame_%06llu.bin", static_cast<unsigned long long>(g_ramField));
    const std::string path = std::string(dir && *dir ? dir : ".") + name;
    if (FILE *f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(runtime->memory().getRDRAM(), 1, PS2_RAM_SIZE, f);
        std::fclose(f);
    }
    std::fprintf(stderr, "[frame-probe] ram at frame %llu field %llu -> %s\n", static_cast<unsigned long long>(g_frames),
                 static_cast<unsigned long long>(field), path.c_str());
    g_ramField = 0;
}

void beforeFrameEnd(PS2Runtime *runtime)
{
    const uint64_t field = runtime->eeScheduler().currentVSyncTick();
    if (field < g_from) return;
    std::fprintf(stderr, "[frame-probe] frame %llu field %llu\n", static_cast<unsigned long long>(++g_frames),
                 static_cast<unsigned long long>(field));
    if (g_ramField != 0u && field >= g_ramField) dumpRam(runtime, field);
    if (g_waitMs != 0u) std::this_thread::sleep_for(std::chrono::milliseconds(g_waitMs));
}

void frameEnd(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    beforeFrameEnd(runtime);
    originalFrameEnd(ram, ctx, runtime);
}

void frameEndWait(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    beforeFrameEnd(runtime);
    originalFrameEndWait(ram, ctx, runtime);
}

void flip(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint64_t field = runtime->eeScheduler().currentVSyncTick();
    if (field >= g_from)
        std::fprintf(stderr, "[frame-probe] flip %llu field %llu\n", static_cast<unsigned long long>(++g_flips),
                     static_cast<unsigned long long>(field));
    originalFlip(ram, ctx, runtime);
}

void draw(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint64_t field = runtime->eeScheduler().currentVSyncTick();
    if (field >= g_from)
        std::fprintf(stderr, "[frame-probe] draw %llu field %llu\n", static_cast<unsigned long long>(++g_draws),
                     static_cast<unsigned long long>(field));
    originalDraw(ram, ctx, runtime);
}

void install(PS2Runtime &runtime)
{
    const char *value = std::getenv("PS2X_FRAME_PROBE");
    if (!value || !*value) return;
    char *rest = nullptr;
    g_from = std::strtoull(value, &rest, 10);
    if (rest && *rest == ':') g_waitMs = static_cast<unsigned>(std::strtoul(rest + 1, nullptr, 10));
    if (const char *ram = std::getenv("PS2X_FRAME_PROBE_RAM"); ram && *ram)
        g_ramField = std::strtoull(ram, nullptr, 10);
    if (runtime.memory().read32(kFrameEnd) != 0x27BDFFE0u || runtime.memory().read32(kFrameEndWait) != 0x27BDFFF0u ||
        runtime.memory().read32(kFlip) != 0x8C820004u || runtime.memory().read32(kDraw) != 0x27BDFFE0u) {
        std::fprintf(stderr, "[frame-probe] declined: unexpected code at the frame ends, the flip or the draw\n");
        return;
    }
    originalFrameEnd = runtime.lookupFunction(kFrameEnd);
    originalFrameEndWait = runtime.lookupFunction(kFrameEndWait);
    originalFlip = runtime.lookupFunction(kFlip);
    originalDraw = runtime.lookupFunction(kDraw);
    runtime.replaceFunction(kFrameEnd, &frameEnd);
    runtime.replaceFunction(kFrameEndWait, &frameEndWait);
    runtime.replaceFunction(kFlip, &flip);
    runtime.replaceFunction(kDraw, &draw);
    std::fprintf(stderr, "[frame-probe] from field %llu, %u ms per game frame\n", static_cast<unsigned long long>(g_from),
                 g_waitMs);
}
} // namespace

PS2_REGISTER_GAME_OVERRIDE("DW5XL frame probe (DW5XL-DEBUG, issue 049)", "SLES_538.60", 0u, 0u, &install)
