// DW5XL-TEST issue 049, amendment v3.7 (P2.6b-3): two GS memories of one graphics device draw in turns, and neither's
// open work may end where the other takes its turn. Found 2026-11-11: a turn ended the work (the render pass) of
// the memory used last, and what paraLLEl-GS draws is not the same for every place where its work ends, so game
// pictures differed by a few pixels wherever the game's GS work came between two packets of an in-between picture.
// No game data. On the Vulkan renderer, at render scale 1 and 4, a first memory and a second one:
//   1. a scene on the second memory alone (a cleared buffer, triangles with slanted edges, see-through ones over
//      them), flushed once at its end: its render passes are counted and its memory is kept;
//   2. the same start and the same scene, with the first memory drawing between every two of its draws: as many
//      render passes as alone, and the same memory byte for byte;
//   3. the same the other way round (the first memory's scene, the second drawing in between);
//   4. both with open work at the same time, then a flush of each, a picture of each, a change of the output scale,
//      a reset of the second and the second memory's end while the first has open work: nothing hangs.
// Exit code 77: no Vulkan device (skipped).
#include "runtime/gs/gs_vulkan_backend.h"
#include "test_one_program.h"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>

namespace
{
int g_failures = 0;

void check(bool ok, const char *what, uint32_t scale)
{
    std::printf("%s  scale %ux: %s\n", ok ? "ok  " : "FAIL", scale, what);
    if (!ok)
        ++g_failures;
}

GSContext buffer(uint32_t fbp, uint16_t lastRow)
{
    GSContext c{};
    c.frame.fbp = fbp;
    c.frame.fbw = 8u; // 512 pixels
    c.frame.psm = GS_PSM_CT32;
    c.scissor.x0 = 0u;
    c.scissor.x1 = 511u;
    c.scissor.y0 = 0u;
    c.scissor.y1 = lastRow;
    c.zbuf.zbp = 120u;
    c.zbuf.zmask = true;
    return c;
}

// A triangle with slanted edges; `alpha` below 0x80 makes it see-through ((Cs - Cd) * As + Cd).
void triangle(GSRasterBackend &backend, const GSContext &c, uint32_t rgb, uint8_t alpha, float x, float y)
{
    GSPrimitiveBatch b{};
    b.state.context = c;
    b.state.context.test = 0;
    b.state.context.alpha = 0x44u;
    b.state.prim.type = GS_PRIM_TRIANGLE;
    b.state.prim.abe = alpha < 0x80u;
    b.state.colclamp = 1;
    b.vertexCount = 3;
    const float corners[3][2] = {{10.3f, 5.7f}, {300.2f, 40.1f}, {120.6f, 190.9f}};
    for (int i = 0; i < 3; ++i)
    {
        b.vertices[i].x = x + corners[i][0];
        b.vertices[i].y = y + corners[i][1];
        b.vertices[i].r = uint8_t(rgb);
        b.vertices[i].g = uint8_t(rgb >> 8);
        b.vertices[i].b = uint8_t(rgb >> 16);
        b.vertices[i].a = alpha;
    }
    backend.Submit(b);
}

// The scene: 19 draws into the buffer at page 64, `between` after each, one flush at the end.
void scene(GSRasterBackend &backend, const std::function<void()> &between)
{
    const GSContext c = buffer(64u, 223u);
    backend.ClearFramebuffer(c, 0x80302418u);
    between();
    for (uint32_t i = 0; i < 12u; ++i)
    {
        triangle(backend, c, 0x204060u + i * 0x0B1307u, 0x80u, float(i) * 13.25f, float(i % 4u) * 6.5f);
        between();
    }
    for (uint32_t i = 0; i < 6u; ++i)
    {
        triangle(backend, c, 0xC0A080u - i * 0x110907u, 0x45u, 20.0f + float(i) * 21.5f, 3.0f + float(i) * 2.25f);
        between();
    }
    backend.Flush();
}

GSPresentationRequest request()
{
    GSPresentationRequest r{};
    r.pmode = 0x8001u;                                            // EN1, alpha 0x80
    r.smode2 = 0u;                                                // non-interlaced
    r.dispfb1 = 0u | (8ull << 9);                                 // FBP 0, FBW 8, PSMCT32
    r.display1 = (4ull << 23) | (2559ull << 32) | (255ull << 44); // MAGH 4 (512 pixels), DW 2560, DH 256
    return r;
}

bool picture(GSRasterBackend &backend, uint64_t tick)
{
    GSPresentationRequest r = request();
    r.vsyncTick = tick;
    if (!backend.SubmitFieldScanout(r))
        return false;
    PresentationFrame frame;
    for (int attempt = 0; attempt < 100 && !backend.CollectFieldScanouts(frame, tick, tick); ++attempt)
    {
    }
    return static_cast<bool>(frame);
}

std::vector<uint8_t> memory(GSRasterBackend &backend)
{
    std::vector<uint8_t> out;
    backend.Flush();
    backend.SnapshotVram(out);
    return out;
}

// The scene on `mine` from the same start, once alone and once with `other` drawing between every two draws.
void inTurns(GSVulkanBackend &mine, GSRasterBackend &other, std::vector<uint8_t> &start, const char *whose, uint32_t scale)
{
    char text[160];
    mine.Initialize(start.data(), static_cast<uint32_t>(start.size()));
    (void)mine.TakeRenderPasses();
    scene(mine, [] {});
    const uint32_t alonePasses = mine.TakeRenderPasses();
    const std::vector<uint8_t> alone = memory(mine);

    mine.Initialize(start.data(), static_cast<uint32_t>(start.size()));
    (void)mine.TakeRenderPasses();
    const GSContext elsewhere = buffer(0u, 255u);
    uint32_t turns = 0;
    scene(mine, [&]
    {
        triangle(other, elsewhere, 0x305070u + turns * 0x050301u, 0x80u, float(turns), 2.0f);
        ++turns;
    });
    const uint32_t passes = mine.TakeRenderPasses();
    const std::vector<uint8_t> inTurn = memory(mine);
    other.Flush();

    std::snprintf(text, sizeof(text), "%s scene, the other memory drawing %u times in between: %u render passes, as alone (%u)",
                  whose, turns, passes, alonePasses);
    check(alonePasses != 0u && passes == alonePasses, text, scale);
    std::snprintf(text, sizeof(text), "%s scene in turns: the same memory as alone", whose);
    check(!alone.empty() && inTurn == alone, text, scale);
}

void run(uint32_t scale)
{
    GSVulkanBackend first;
    std::vector<uint8_t> vram(4u << 20);
    uint32_t seed = 0x0BADF00Du;
    for (uint8_t &b : vram)
    {
        seed = seed * 1664525u + 1013904223u;
        b = static_cast<uint8_t>(seed >> 24);
    }
    first.Initialize(vram.data(), static_cast<uint32_t>(vram.size()));
    first.SetResolutionScale(scale);
    if (first.GetResolutionScale() != scale)
        std::printf("note  scale %ux: this GPU renders at %ux\n", scale, first.GetResolutionScale());
    std::unique_ptr<GSRasterBackend> made = first.MakeSecondMemory();
    auto *second = dynamic_cast<GSVulkanBackend *>(made.get());
    check(second != nullptr, "a second memory is made on the same graphics device", scale);
    if (!second)
        return;
    second->Initialize(vram.data(), static_cast<uint32_t>(vram.size()));

    // 1, 2
    inTurns(*second, first, vram, "the second memory's", scale);
    // 3
    inTurns(first, *second, vram, "the first memory's", scale);

    // 4. Both with open work, and everything that waits for the graphics device.
    const GSContext a = buffer(64u, 223u), b = buffer(0u, 255u);
    bool alive = true;
    for (uint32_t i = 0; i < 12u && alive; ++i)
    {
        triangle(first, a, 0x102030u + i, 0x80u, float(i), 1.0f);
        triangle(*second, b, 0x302010u + i, 0x45u, 2.0f, float(i));
        if (i % 3u == 1u)
            first.Flush();          // the second has open work
        if (i % 4u == 2u)
            second->Flush();        // the first has open work
        if (i % 5u == 3u)
            alive = picture(first, 100u + i) && picture(*second, 100u + i);
    }
    check(alive, "both with open work: flushes and pictures of each", scale);
    triangle(first, a, 0x405060u, 0x80u, 3.0f, 3.0f);
    triangle(*second, b, 0x605040u, 0x80u, 4.0f, 4.0f);
    first.SetResolutionScale(scale == 1u ? 2u : 1u); // waits for the device while the second has open work
    triangle(first, a, 0x708090u, 0x80u, 5.0f, 5.0f);
    triangle(*second, b, 0x908070u, 0x80u, 6.0f, 6.0f);
    second->Sync(GSSyncReason::Reset);               // waits for the device while the first has open work
    triangle(*second, b, 0x112233u, 0x80u, 7.0f, 7.0f);
    triangle(first, a, 0x332211u, 0x80u, 8.0f, 8.0f);
    made.reset();                                    // the second memory goes while the first has open work
    triangle(first, a, 0x445566u, 0x80u, 9.0f, 9.0f);
    first.Flush();
    check(picture(first, 200u), "a change of scale, a reset and the second memory's end with open work elsewhere: no hang", scale);
}
} // namespace

int DW5XL_TEST_MAIN(gs_turns)()
{
    try
    {
        GSVulkanBackend probe; // throws without a Vulkan device
    }
    catch (const std::exception &error)
    {
        std::printf("SKIPPED: no Vulkan GS renderer here (%s)\n", error.what());
        return 77;
    }
    try
    {
        run(1u);
        run(4u);
    }
    catch (const std::exception &error)
    {
        std::printf("FAIL  exception: %s\n", error.what());
        return 1;
    }
    std::printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
