#pragma once
// DW5XL-DEBUG issue 023 / issue 049 (amendment v3.12 step 7a): the numbers of the picture trace (PS2X_WEAVE_TRACE=1),
// by which tests follow the pictures a window shows without saving them: a row profile (a picture that moves by a
// line shows as a shifted profile) and fingerprints. One place for them, used by the renderer's own window; today's
// window and the renderer's read-back path have the same sums written out where they are used.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

namespace ps2x::picture_trace
{
    // PS2X_WEAVE_TRACE_COLS=x0:x1: the columns the row profile covers (x0 up to, not including, x1), e.g. a part of
    // the HUD that never changes. No text, or one that is no range: the whole width.
    inline std::pair<uint32_t, uint32_t> columns(const char *text)
    {
        unsigned a = 0u, b = 0u;
        if (text && std::sscanf(text, "%u:%u", &a, &b) == 2 && b > a)
            return {a, b};
        return {0u, 0xFFFFFFFFu};
    }

    // The mean brightness of every row over the columns `range` (cut to the width), two hex digits a row. The
    // picture is RGBA, rows `strideBytes` apart; alpha is left out.
    inline std::string rowProfile(const uint8_t *rgba, size_t strideBytes, uint32_t width, uint32_t height, uint32_t first,
                                  uint32_t second)
    {
        static const char kHex[] = "0123456789abcdef";
        const uint32_t x0 = std::min(first, width), x1 = std::max(x0 + 1u, std::min(second, width));
        std::string rows;
        rows.reserve(static_cast<size_t>(height) * 2u);
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t *row = rgba + static_cast<size_t>(y) * strideBytes;
            uint32_t sum = 0u;
            for (uint32_t x = x0; x < x1 && x < width; ++x)
                sum += row[x * 4u] + row[x * 4u + 1u] + row[x * 4u + 2u];
            const uint32_t mean = sum / ((x1 - x0) * 3u);
            rows.push_back(kHex[(mean >> 4) & 15u]);
            rows.push_back(kHex[mean & 15u]);
        }
        return rows;
    }

    // FNV-1a over the rows y0 up to, not including, y1 (width * 4 bytes of each).
    inline uint64_t fingerprint(const uint8_t *rgba, size_t strideBytes, uint32_t width, uint32_t y0, uint32_t y1)
    {
        uint64_t h = 1469598103934665603ull;
        for (uint32_t y = y0; y < y1; ++y)
        {
            const uint8_t *row = rgba + static_cast<size_t>(y) * strideBytes;
            for (size_t i = 0; i < static_cast<size_t>(width) * 4u; ++i)
                h = (h ^ row[i]) * 1099511628211ull;
        }
        return h;
    }
} // namespace ps2x::picture_trace
