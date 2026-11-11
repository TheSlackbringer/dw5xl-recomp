// DW5XL-TEST issue 049, amendment v3.7 (P2.6a): the picture GS stays the game's GS through everything that changes the
// game's GS memory. The pictures' GS memory gets the pages the game's GS wrote since the last copy
// (GS::takeWrittenPages, GS::copyPagesFrom). A change that is not a draw, a copy or an upload cannot be named page by
// page, so the front end must then say "all pages": a state load, a change of the output scale or of the samples, a
// direct write into GS memory. No game data. On the Vulkan renderer, the game's GS and a second memory of it:
//   1. the first call says all pages; after the copy the two memories are equal;
//   2. one buffer cleared: its pages and not all of them; after copying only those the two are equal;
//   3. nothing written: no pages;
//   4. a state saved, both buffers drawn over and copied, the state loaded: all pages; equal after the copy;
//   5. the output scale changed, then the samples: all pages each time; equal after the copy;
//   6. a direct write into GS memory: all pages; equal after the copy.
// "Equal" compares the single-rate GS memory (4 MB) of both, read back from the renderer.
// Exit code 77: no Vulkan device (skipped).
#include "ps2_runtime.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "runtime/state_archive.h"

#include <bitset>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <vector>

// The GS's state functions come from an object file that also holds the runtime's (found on the Windows build,
// 2026-11-11: the linker takes the runtime with it there): the table the generated code would bring, as in the
// other runtime tests.
extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x1000;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x1004;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 1;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[] = {nullptr};

namespace
{
int g_failures = 0;

void check(bool ok, const char *what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++g_failures;
}

// Context 1 draws into the 512-pixel-wide 32-bit buffer at page `fbp`, rows 0 to `lastRow`, without a depth buffer.
void setBuffer(GS &gs, uint32_t fbp, uint32_t lastRow)
{
    gs.writeRegister(0x4Cu, uint64_t(fbp) | (8ull << 16));                         // FRAME_1: FBW 8, PSMCT32
    gs.writeRegister(0x4Eu, 120ull | (1ull << 32));                                // ZBUF_1: masked
    gs.writeRegister(0x40u, (511ull << 16) | (uint64_t(lastRow) << 48));           // SCISSOR_1
}

std::bitset<512> written(GS &game, GS &picture)
{
    return game.takeWrittenPages() | picture.takeWrittenPages();
}

bool equalAfterCopy(GS &game, GS &picture, const std::bitset<512> &pages)
{
    if (!picture.copyPagesFrom(game, pages))
        return false;
    std::vector<uint8_t> a, b;
    game.snapshotVram(a);
    picture.snapshotVram(b);
    return !a.empty() && a == b;
}

void run()
{
    std::vector<uint8_t> gameVram(4u << 20), pictureVram(4u << 20, 0u);
    uint32_t seed = 0x2468ACE1u;
    for (uint8_t &b : gameVram)
    {
        seed = seed * 1664525u + 1013904223u;
        b = static_cast<uint8_t>(seed >> 24);
    }
    GSRegisters priv{};
    GS game;
    game.init(gameVram.data(), static_cast<uint32_t>(gameVram.size()), &priv);
    GS picture;
    if (!picture.initAsSecondMemory(game, pictureVram.data(), static_cast<uint32_t>(pictureVram.size()), &priv))
    {
        check(false, "the renderer makes a second GS memory");
        return;
    }

    // 1
    std::bitset<512> pages = written(game, picture);
    check(pages.all(), "the first call says all pages");
    check(equalAfterCopy(game, picture, pages), "after the first copy the two memories are equal");

    // 2
    setBuffer(game, 64u, 223u);
    game.clearActiveFramebuffer(0xFF336699u);
    pages = written(game, picture);
    check(pages.test(64) && !pages.test(0) && !pages.test(200) && pages.count() <= 64u,
          "one buffer cleared: its pages, and not all of them");
    check(equalAfterCopy(game, picture, pages), "after copying only those pages the two memories are equal");

    // 3
    check(written(game, picture).none(), "nothing written: no pages");

    // 4
    ps2x::StateArchive saved = ps2x::StateArchive::writer();
    game.serializeState(saved);
    setBuffer(game, 0u, 255u);
    game.clearActiveFramebuffer(0xFF102030u);
    setBuffer(game, 64u, 223u);
    game.clearActiveFramebuffer(0xFFC0B0A0u);
    check(equalAfterCopy(game, picture, written(game, picture)), "both buffers drawn over and copied: equal");
    ps2x::StateArchive loading = ps2x::StateArchive::reader(saved.data());
    game.serializeState(loading);
    pages = written(game, picture);
    check(pages.all(), "a state loaded into the game's GS: all pages");
    check(equalAfterCopy(game, picture, pages), "after a state load and the copy the two memories are equal");

    // 5
    game.setResolutionScale(2u);
    pages = written(game, picture);
    check(pages.all(), "the output scale changed: all pages");
    check(equalAfterCopy(game, picture, pages) && picture.getResolutionScale() == game.getResolutionScale(),
          "after a change of scale and the copy the two are equal and at the same scale");
    game.setResolutionScale(game.getResolutionScale());
    check(written(game, picture).none(), "the same scale set again: no pages");
    game.setAASamples(2u);
    pages = written(game, picture);
    check(pages.all(), "the samples changed: all pages");
    check(equalAfterCopy(game, picture, pages), "after a change of samples and the copy the two memories are equal");

    // 6
    game.WriteVram(0u, 0u, 8u, 5u, 5u, 0x11223344u);
    pages = written(game, picture);
    check(pages.all(), "a direct write into GS memory: all pages");
    check(equalAfterCopy(game, picture, pages), "after a direct write and the copy the two memories are equal");
}
} // namespace

int main()
{
#ifdef _WIN32
    _putenv_s("PS2X_GS_BACKEND", "vulkan");
#else
    setenv("PS2X_GS_BACKEND", "vulkan", 1);
#endif
    try
    {
        GS probe; // throws without a Vulkan device
    }
    catch (const std::exception &error)
    {
        std::printf("SKIPPED: no Vulkan GS renderer here (%s)\n", error.what());
        return 77;
    }
    try
    {
        run();
    }
    catch (const std::exception &error)
    {
        std::printf("FAIL  exception: %s\n", error.what());
        return 1;
    }
    std::printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
