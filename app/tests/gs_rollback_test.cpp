// DW5XL-TEST issue 049 (amendment v3.2): the GS rollback point on the Vulkan renderer
// (GSRasterBackend::SaveRollback / Rollback; paraLLEl-GS runtime patch 0007). No game data.
// At render scale 1 and 4:
//   1. a patterned GS memory; two buffers (a display buffer at page 0 and a second buffer at page 64) cleared and
//      drawn with a triangle;
//   2. the picture and the whole single-rate memory are kept;
//   3. SaveRollback for the two buffers' pages; both are drawn over with other colours and another triangle; the
//      picture and the memory must now differ;
//   4. Rollback: the picture and the whole single-rate memory must equal the kept ones, byte for byte; a second
//      Rollback has nothing to put back; the same draws again give the same picture.
// Picture reads (P2.3b, GSRasterBackend::SubmitPictureRead / CollectPictureReads): between 3 and 4 the drawn-over
// state is read out twice as an in-between picture. Both must come out, one by one and in order, with their tags
// and field, each equal to the field's read of the same state; the field reads after them are not disturbed.
// What it does not prove: the picture read here comes from the single-rate copy (control 2026-11-10: restoring only
// that copy also passed, restoring nothing failed). The reference and sample copies are proved by the game-level
// check at 4x (issue 049 criterion 6a, notes) and its control.
// Exit code 77: no Vulkan device (skipped).
#include "runtime/gs/gs_vulkan_backend.h"

#include <algorithm>
#include <bitset>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <thread>
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

GSContext buffer(uint32_t fbp, uint16_t lastRow, uint16_t ofx, uint16_t ofy)
{
    GSContext c{};
    c.frame.fbp = fbp;
    c.frame.fbw = 8u; // 512 pixels
    c.frame.psm = GS_PSM_CT32;
    c.scissor.x0 = 0u;
    c.scissor.x1 = 511u;
    c.scissor.y0 = 0u;
    c.scissor.y1 = lastRow;
    c.xyoffset.ofx = ofx;
    c.xyoffset.ofy = ofy;
    c.zbuf.zbp = 120u;
    c.zbuf.zmask = true;
    return c;
}

// The buffer cleared to `rgba`, then a triangle in the inverse colour with slanted edges at (x, y) + the corners.
void draw(GSVulkanBackend &backend, const GSContext &c, uint32_t rgba, float x, float y)
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
        b.vertices[i].x = x + corners[i][0] + c.xyoffset.ofx / 16.0f;
        b.vertices[i].y = y + corners[i][1] + c.xyoffset.ofy / 16.0f;
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

std::vector<uint8_t> picture(GSVulkanBackend &backend, uint64_t tick)
{
    // The field read as the game's fields are read (issue 023): started in order with the draws, then collected for
    // exactly this tick (Present alone can hand back the read before).
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

void run(uint32_t scale)
{
    GSVulkanBackend backend;
    std::vector<uint8_t> vram(4u << 20);
    uint32_t seed = 0x12345678u;
    for (uint8_t &b : vram)
    {
        seed = seed * 1664525u + 1013904223u;
        b = static_cast<uint8_t>(seed >> 24);
    }
    backend.Initialize(vram.data(), static_cast<uint32_t>(vram.size()));
    backend.SetResolutionScale(scale);
    if (backend.GetResolutionScale() != scale) // a GPU without 16 samples gives 2x for 4x: tested at what it gives
        std::printf("note  scale %ux: this GPU renders at %ux\n", scale, backend.GetResolutionScale());

    const GSContext display = buffer(0u, 255u, 0u, 0u), second = buffer(64u, 223u, 0u, 0u);
    draw(backend, display, 0x80204060u, 0.0f, 0.0f);
    draw(backend, second, 0x80806040u, 3.0f, 2.0f);
    backend.Flush();
    const std::vector<uint8_t> pictureBefore = picture(backend, 1u);
    std::vector<uint8_t> memoryBefore;
    backend.SnapshotVram(memoryBefore);
    check(!pictureBefore.empty(), "a picture is read", scale);
    {
        // The triangle must be in the picture: more than the clear colour.
        std::vector<uint32_t> colours;
        for (size_t i = 0; i + 4u <= pictureBefore.size(); i += 4u)
        {
            uint32_t v;
            std::memcpy(&v, &pictureBefore[i], 4);
            v &= 0x00FFFFFFu;
            if (std::find(colours.begin(), colours.end(), v) == colours.end())
            {
                colours.push_back(v);
                if (colours.size() > 64u)
                    break;
            }
        }
        std::printf("note  scale %ux: the picture has %zu%s different colours\n", scale, colours.size(),
                    colours.size() > 64u ? " or more" : "");
        check(colours.size() >= 2u, "the triangle is in the picture", scale);
    }

    std::bitset<512> pages;
    for (uint32_t page = 0; page < 120u; ++page) // the display buffer (64 pages) and the second buffer (56 pages)
        pages.set(page);
    check(backend.SaveRollback(pages), "SaveRollback accepts the pages", scale);
    check(backend.RollbackBytes() != 0u, "the rollback point holds bytes", scale);

    draw(backend, display, 0x80F0E0D0u, 150.4f, 20.2f);
    draw(backend, second, 0x80102030u, 90.9f, 11.3f);
    backend.Flush();
    const std::vector<uint8_t> pictureBetween = picture(backend, 2u);
    std::vector<uint8_t> memoryBetween;
    backend.SnapshotVram(memoryBetween);
    check(pictureBetween != pictureBefore, "the picture changed after drawing over the buffers", scale);
    check(memoryBetween != memoryBefore, "the memory changed after drawing over the buffers", scale);

    // P2.3b: the drawn-over state read out as two in-between pictures of the frame sent in field 8 (they belong to
    // field 9), before the rollback.
    {
        GSPresentationRequest r = request();
        r.vsyncTick = 9u;
        r.pictureTag = gsPictureTag(8u, 1u, 2u);
        const bool first = backend.SubmitPictureRead(r);
        r.pictureTag = gsPictureTag(8u, 2u, 2u);
        const bool second = backend.SubmitPictureRead(r);
        check(first && second, "two picture reads are accepted", scale);
        std::vector<GSPictureRead> pictures;
        for (int attempt = 0; attempt < 2000 && pictures.size() < 2u; ++attempt)
        {
            if (!backend.CollectPictureReads(pictures))
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check(pictures.size() == 2u, "both pictures come out, one by one", scale);
        if (pictures.size() == 2u)
        {
            check(pictures[0].tag == gsPictureTag(8u, 1u, 2u) && pictures[1].tag == gsPictureTag(8u, 2u, 2u),
                  "the pictures come out in order with their tags", scale);
            check(gsPictureTagFrameField(pictures[1].tag) == 8u && gsPictureTagIndex(pictures[1].tag) == 2u &&
                      gsPictureTagCount(pictures[1].tag) == 2u,
                  "a tag gives back its frame field, index and count", scale);
            check(pictures[0].field == 9u && pictures[1].field == 9u, "the pictures carry their field", scale);
            check(pictures[0].frame.pixels == pictureBetween && pictures[1].frame.pixels == pictureBetween,
                  "each picture equals the field's read of the same state", scale);
        }
        std::vector<GSPictureRead> none;
        check(!backend.CollectPictureReads(none) && none.empty(), "no picture is left afterwards", scale);
    }

    check(backend.Rollback(), "Rollback puts the point back", scale);
    const std::vector<uint8_t> pictureAfter = picture(backend, 3u);
    std::vector<uint8_t> memoryAfter;
    backend.SnapshotVram(memoryAfter);
    check(pictureAfter == pictureBefore, "the picture after the rollback equals the picture before", scale);
    check(memoryAfter == memoryBefore, "the whole single-rate memory after the rollback equals the memory before", scale);
    check(!backend.Rollback(), "a second Rollback has nothing to put back", scale);

    // Drawing goes on from the restored state: the same draws give the same picture as the first time.
    draw(backend, display, 0x80204060u, 0.0f, 0.0f);
    draw(backend, second, 0x80806040u, 3.0f, 2.0f);
    backend.Flush();
    check(picture(backend, 4u) == pictureBefore, "the same draws after the rollback give the same picture", scale);
}
} // namespace

int main()
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
