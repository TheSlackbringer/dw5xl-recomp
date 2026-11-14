// DW5XL-TEST issue 049: what the renderer's unit tests of the picture hand-over and of the new window share: two
// drawings, a read circuit for them, and a field's picture read the old way as the yardstick. No game data.
#pragma once

#include "runtime/gs/gs_vulkan_backend.h"

#include <cstdint>
#include <vector>

namespace gs_test
{
inline GSContext buffer()
{
    GSContext c{};
    c.frame.fbp = 0u;
    c.frame.fbw = 8u; // 512 pixels
    c.frame.psm = GS_PSM_CT32;
    c.scissor.x0 = 0u;
    c.scissor.x1 = 511u;
    c.scissor.y0 = 0u;
    c.scissor.y1 = 255u;
    c.zbuf.zbp = 120u;
    c.zbuf.zmask = true;
    return c;
}

// The buffer cleared to `rgba`, then a triangle in the inverse colour with slanted edges at (x, y) + the corners.
inline void draw(GSVulkanBackend &backend, uint32_t rgba, float x, float y)
{
    const GSContext c = buffer();
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
    backend.Flush();
}
inline void drawA(GSVulkanBackend &backend) { draw(backend, 0x80204060u, 0.0f, 0.0f); }
inline void drawB(GSVulkanBackend &backend) { draw(backend, 0x80806040u, 90.0f, 30.0f); }

// A read circuit on the buffer at page 0, 512 x 256: progressive, or interlaced in field mode.
inline GSPresentationRequest request(uint64_t tick, bool fieldMode = false)
{
    GSPresentationRequest r{};
    r.pmode = 0x8001u;                                            // EN1, alpha 0x80
    r.smode2 = fieldMode ? 1u : 0u;                               // INT without FFMD: field mode
    r.dispfb1 = 0u | (8ull << 9);                                 // FBP 0, FBW 8, PSMCT32
    r.display1 = (4ull << 23) | (2559ull << 32) | (255ull << 44); // MAGH 4 (512 pixels), DW 2560, DH 256
    r.vsyncTick = tick;
    return r;
}

// The field's picture the old way: read back into main memory. Empty when no read arrives.
inline PresentationFrame readBack(GSVulkanBackend &backend, uint64_t tick, bool fieldMode = false)
{
    PresentationFrame frame;
    if (!backend.SubmitFieldScanout(request(tick, fieldMode)))
        return {};
    for (int attempt = 0; attempt < 100 && !backend.CollectFieldScanouts(frame, tick, tick); ++attempt)
    {
    }
    return frame;
}

// Whether a read of field `tick` arrives (asked a few times; the read's wait is bounded by one field).
inline bool readArrives(GSVulkanBackend &backend, uint64_t tick)
{
    PresentationFrame frame;
    for (int attempt = 0; attempt < 3; ++attempt)
        if (backend.CollectFieldScanouts(frame, tick, tick))
            return true;
    return false;
}

// Colours alike; alpha is left out (a read forces it opaque, a handed picture keeps the frame buffer's).
inline bool sameColours(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b)
{
    if (a.size() != b.size() || a.empty())
        return false;
    for (size_t i = 0; i + 4u <= a.size(); i += 4u)
        if (a[i] != b[i] || a[i + 1u] != b[i + 1u] || a[i + 2u] != b[i + 2u])
            return false;
    return true;
}
} // namespace gs_test
