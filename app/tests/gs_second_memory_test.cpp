// DW5XL-TEST issue 049 (fact-finding 2026-11-11, "a pipeline of the pictures' own"): a second GS memory inside the
// same Vulkan renderer, on the same graphics device (GSRasterBackend::MakeSecondMemory / CopyPagesFrom; paraLLEl-GS
// runtime patch 0007). No game data. At render scale 1 and 4:
//   1. the first memory (patterned, a display buffer at page 0 and a second buffer at page 64, each cleared and
//      drawn with a triangle); its picture and its whole single-rate memory are kept;
//   2. a second memory is made and gets every page of the first, copied on the graphics card: its memory and its
//      picture must equal the first's;
//   3. both buffers are drawn over on the second memory: its picture changes, and the first memory and its picture
//      stay as they were, byte for byte;
//   4. the first memory's second buffer is drawn anew; only the pages that changed on either side are copied: the
//      second memory and its picture equal the first's again;
//   5. the two are used in turn many times (draws, flushes, reads): nothing hangs.
// It prints what a copy takes (all pages, and the pages of two buffers).
// What it does not prove: the picture read here comes from the single-rate copy; the reference and sample copies of
// a page are proved by game pictures at 4x (notes, issue 049).
// Exit code 77: no Vulkan device (skipped).
#include "runtime/gs/gs_vulkan_backend.h"
#include "test_one_program.h"

#include <algorithm>
#include <bitset>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

// The buffer cleared to `rgba`, then a triangle in the inverse colour with slanted edges at (x, y) + the corners.
void draw(GSRasterBackend &backend, const GSContext &c, uint32_t rgba, float x, float y)
{
    backend.ClearFramebuffer(c, rgba);
    GSPrimitiveBatch b{};
    b.state.context = c;
    b.state.context.test = 0;
    b.state.context.zbuf.zmask = true;
    b.state.prim.type = GS_PRIM_TRIANGLE;
    b.state.colclamp = 1;
    b.vertexCount = 3;
    const float corners[3][2] = {{10.3f, 5.7f}, {300.2f, 40.1f}, {120.6f, 190.9f}};
    for (int i = 0; i < 3; ++i)
    {
        b.vertices[i].x = x + corners[i][0];
        b.vertices[i].y = y + corners[i][1];
        b.vertices[i].r = uint8_t(~rgba);
        b.vertices[i].g = uint8_t(~(rgba >> 8));
        b.vertices[i].b = uint8_t(~(rgba >> 16));
        b.vertices[i].a = 0x80u;
    }
    backend.Submit(b);
}

// A progressive read circuit on the buffer at page 0: 512 x 256.
GSPresentationRequest request()
{
    GSPresentationRequest r{};
    r.pmode = 0x8001u;                                          // EN1, alpha 0x80
    r.smode2 = 0u;                                              // non-interlaced
    r.dispfb1 = 0u | (8ull << 9);                               // FBP 0, FBW 8, PSMCT32
    r.display1 = (4ull << 23) | (2559ull << 32) | (255ull << 44); // MAGH 4 (512 pixels), DW 2560, DH 256
    return r;
}

std::vector<uint8_t> picture(GSRasterBackend &backend, uint64_t tick)
{
    GSPresentationRequest r = request();
    r.vsyncTick = tick;
    if (!backend.SubmitFieldScanout(r))
        return {};
    PresentationFrame frame;
    for (int attempt = 0; attempt < 100 && !backend.CollectFieldScanouts(frame, tick, tick); ++attempt)
    {
    }
    return frame ? frame.pixels : std::vector<uint8_t>{};
}

std::vector<uint8_t> memory(GSRasterBackend &backend)
{
    std::vector<uint8_t> out;
    backend.Flush();
    backend.SnapshotVram(out);
    return out;
}

// The pages of a 512-pixel-wide 32-bit buffer of `rows` rows at page `fbp` (8 KB pages: 4 rows of 512 pixels each).
void addBuffer(std::bitset<512> &pages, uint32_t fbp, uint32_t rows)
{
    for (uint32_t page = fbp; page < fbp + (rows + 3u) / 4u && page < 512u; ++page)
        pages.set(page);
}

void run(uint32_t scale)
{
    GSVulkanBackend first;
    std::vector<uint8_t> vram(4u << 20);
    uint32_t seed = 0x12345678u;
    for (uint8_t &b : vram)
    {
        seed = seed * 1664525u + 1013904223u;
        b = static_cast<uint8_t>(seed >> 24);
    }
    first.Initialize(vram.data(), static_cast<uint32_t>(vram.size()));
    first.SetResolutionScale(scale);
    if (first.GetResolutionScale() != scale)
        std::printf("note  scale %ux: this GPU renders at %ux\n", scale, first.GetResolutionScale());

    const GSContext display = buffer(0u, 255u), second = buffer(64u, 223u);
    draw(first, display, 0x80204060u, 0.0f, 0.0f);
    draw(first, second, 0x80806040u, 3.0f, 2.0f);
    const std::vector<uint8_t> pictureFirst = picture(first, 1u);
    const std::vector<uint8_t> memoryFirst = memory(first);
    check(!pictureFirst.empty(), "the first memory's picture is read", scale);

    // 2. A second memory on the same graphics device, with every page of the first.
    std::vector<uint8_t> blank(4u << 20, 0u);
    std::unique_ptr<GSRasterBackend> other = first.MakeSecondMemory();
    check(other != nullptr, "a second memory is made on the same graphics device", scale);
    if (!other)
        return;
    other->Initialize(blank.data(), static_cast<uint32_t>(blank.size()));
    std::bitset<512> all;
    all.set();
    using Clock = std::chrono::steady_clock;
    auto t0 = Clock::now();
    const bool copied = other->CopyPagesFrom(first, all);
    other->Flush();
    const double allMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    check(copied, "every page is copied into it", scale);
    check(memory(*other) == memoryFirst, "its memory equals the first's", scale);
    check(picture(*other, 1u) == pictureFirst, "its picture equals the first's", scale);

    // 3. Drawn over on the second memory: the first is untouched.
    draw(*other, display, 0x80C01020u, 40.0f, 20.0f);
    draw(*other, second, 0x8010C0A0u, 60.0f, 10.0f);
    const std::vector<uint8_t> pictureOther = picture(*other, 2u);
    check(!pictureOther.empty() && pictureOther != pictureFirst, "drawn over, the second memory's picture changes", scale);
    check(memory(first) == memoryFirst, "the first memory is as it was", scale);
    check(picture(first, 2u) == pictureFirst, "the first memory's picture is as it was", scale);

    // 4. The first memory goes on (its second buffer and its display buffer are drawn anew); only the pages either
    // side wrote are copied.
    draw(first, second, 0x80405060u, 9.0f, 7.0f);
    draw(first, display, 0x80302010u, 5.0f, 1.0f);
    const std::vector<uint8_t> pictureNew = picture(first, 3u);
    const std::vector<uint8_t> memoryNew = memory(first);
    std::bitset<512> changed;
    addBuffer(changed, 0u, 256u);
    addBuffer(changed, 64u, 224u);
    t0 = Clock::now();
    const bool again = other->CopyPagesFrom(first, changed);
    other->Flush();
    const double someMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    check(again && pictureNew != pictureFirst, "the pages that changed are copied", scale);
    check(memory(*other) == memoryNew, "the second memory equals the first's again", scale);
    check(picture(*other, 3u) == pictureNew, "and so does its picture", scale);
    std::printf("note  scale %ux: a copy of all 512 pages took %.3f ms, of %zu pages %.3f ms (with the flush that follows)\n",
                first.GetResolutionScale(), allMs, changed.count(), someMs);

    // 5. In turn, many times.
    bool alive = true;
    for (uint32_t i = 0; i < 40u && alive; ++i)
    {
        draw(first, second, 0x80000000u | (i * 0x030507u), float(i), 1.0f);
        draw(*other, second, 0x80FFFFFFu ^ (i * 0x070503u), 2.0f, float(i));
        if (i % 3u == 0u)
            first.Flush();
        if (i % 4u == 0u)
            other->Flush();
        if (i % 10u == 0u)
            alive = !picture(first, 10u + i).empty() && !picture(*other, 10u + i).empty();
    }
    first.Flush();
    other->Flush();
    check(alive, "the two memories are used in turn without a hang", scale);
    other.reset(); // the second memory goes before the first
}
} // namespace

int DW5XL_TEST_MAIN(gs_second_memory)()
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
