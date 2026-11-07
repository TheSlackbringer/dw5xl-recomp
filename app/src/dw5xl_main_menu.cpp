// Issue 058: a way back to the main menu, through the game's own reset.
// Select + Start held for 51 pad reads sets a flag (0x141E20 sets 0x35AD80, the pad code clears it at each read), and
// the battle loop and the menu screens ask for it through 0x141AA0: they end, the game frees what it allocated, and the
// scene loop goes to the main menu. So the in-play MENU's MAIN MENU row only has to answer that question "yes", and it
// works exactly where the buttons work: the game keeps asking, or it does not listen there.
#include "dw5xl_main_menu.h"
#include "game_overrides.h"
#include "ps2_runtime_macros.h"
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"

#include <atomic>
#include <cstdio>

namespace {
constexpr uint32_t kAskReset = 0x00141AA0u;    // returns the flag
constexpr uint32_t kFrameEnd = 0x001425D0u;    // every game frame ends here; it also reads the pads
constexpr uint32_t kMainMenu = 0x002605D0u;    // the main menu starts
PS2Runtime::RecompiledFunction originalAskReset = nullptr, originalFrameEnd = nullptr, originalMainMenu = nullptr;
std::atomic<bool> g_askedThisFrame{false}, g_request{false}, g_installed{false};
std::atomic<unsigned> g_askedRecently{0};      // one bit per finished frame: some screens end two frames per question
std::atomic<uint64_t> g_loadsAtRequest{0};

void askReset(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    originalAskReset(ram, ctx, runtime);
    g_askedThisFrame.store(true, std::memory_order_relaxed);
    if (!g_request.load(std::memory_order_acquire)) return;
    if (runtime->stateLoadCount() != g_loadsAtRequest.load(std::memory_order_relaxed)) {
        g_request.store(false, std::memory_order_release);
        std::fprintf(stderr, "[dw5xl:main-menu] request dropped: a state was loaded\n");
        return;
    }
    setReturnU32(ctx, 1u);
}

void frameEnd(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    const unsigned asked = g_askedThisFrame.exchange(false, std::memory_order_relaxed) ? 1u : 0u;
    g_askedRecently.store(((g_askedRecently.load(std::memory_order_relaxed) << 1) | asked) & 3u, std::memory_order_relaxed);
    originalFrameEnd(ram, ctx, runtime);
}

void mainMenu(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_request.store(false, std::memory_order_release);
    g_askedThisFrame.store(false, std::memory_order_relaxed);
    g_askedRecently.store(0u, std::memory_order_relaxed);
    std::fprintf(stderr, "[dw5xl:main-menu] main menu starts at field %llu\n",
                 static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()));
    originalMainMenu(ram, ctx, runtime);
}

void install(PS2Runtime &runtime)
{
    const struct { uint32_t address, word; } expected[] = {
        {kAskReset, 0x03E00008u}, {kAskReset + 4, 0x8F828410u}, {kFrameEnd, 0x27BDFFE0u}, {kMainMenu, 0x27BDFFA0u}};
    for (const auto &e : expected)
        if (runtime.memory().read32(e.address) != e.word) {
            std::fprintf(stderr, "[dw5xl:main-menu] declined: unexpected code at 0x%X\n", e.address);
            return;
        }
    originalAskReset = runtime.lookupFunction(kAskReset);
    originalFrameEnd = runtime.lookupFunction(kFrameEnd);
    originalMainMenu = runtime.lookupFunction(kMainMenu);
    runtime.replaceFunction(kAskReset, &askReset);
    runtime.replaceFunction(kFrameEnd, &frameEnd);
    runtime.replaceFunction(kMainMenu, &mainMenu);
    g_installed.store(true, std::memory_order_release);
}
} // namespace

namespace dw5xl::mainmenu {
bool gameListens()
{
    return g_installed.load(std::memory_order_acquire) &&
           (g_askedThisFrame.load(std::memory_order_relaxed) || g_askedRecently.load(std::memory_order_relaxed) != 0u);
}
void requestReturn(PS2Runtime &runtime)
{
    g_loadsAtRequest.store(runtime.stateLoadCount(), std::memory_order_relaxed);
    g_request.store(true, std::memory_order_release);
    std::fprintf(stderr, "[dw5xl:main-menu] return asked at field %llu\n",
                 static_cast<unsigned long long>(runtime.eeScheduler().currentVSyncTick()));
}
}

PS2_REGISTER_GAME_OVERRIDE("DW5XL main menu", "SLES_538.60", 0u, 0u, &install)
