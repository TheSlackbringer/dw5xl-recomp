// Issue 049 (amendment v3.12 step 7a): the picture trace's numbers (runtime/picture_trace.h), which the renderer's
// own window writes for every picture it shows, as today's window does (PS2X_WEAVE_TRACE=1: [upload:rows],
// [upload:hash], [weave:out]). No window and no game data.
//  - the row profile: the mean brightness of every row over a range of columns, two hex digits a row, by the same
//    sum as the lines of today's window (copied here as the yardstick);
//  - the column range from PS2X_WEAVE_TRACE_COLS, cut to the picture's width;
//  - the fingerprint of a range of rows changes with any byte of those rows and with no byte outside them.
#include "runtime/picture_trace.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
int failures = 0;

void check(bool ok, const std::string &what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    failures += ok ? 0 : 1;
}

// The row profile as today's window computes it (ps2_runtime.cpp, UploadFrame, as it was on 2026-11-14).
std::string yardstick(const std::vector<uint8_t> &pixels, size_t rowBytes, uint32_t copyWidth, uint32_t copyHeight,
                      uint32_t first, uint32_t second)
{
    const uint32_t x0 = std::min(first, copyWidth), x1 = std::max(x0 + 1u, std::min(second, copyWidth));
    std::string rows;
    for (uint32_t y = 0; y < copyHeight; ++y)
    {
        const uint8_t *row = pixels.data() + static_cast<size_t>(y) * rowBytes;
        uint32_t sum = 0u;
        for (uint32_t x = x0; x < x1 && x < copyWidth; ++x)
            sum += row[x * 4u] + row[x * 4u + 1u] + row[x * 4u + 2u];
        const uint32_t mean = sum / ((x1 - x0) * 3u);
        static const char kHex[] = "0123456789abcdef";
        rows.push_back(kHex[(mean >> 4) & 15u]);
        rows.push_back(kHex[mean & 15u]);
    }
    return rows;
}
} // namespace

int main()
{
    using namespace ps2x::picture_trace;

    // A picture of 6 x 4 pixels, rows 32 bytes apart (two pixels of padding a row), every byte different.
    const uint32_t width = 6u, height = 4u;
    const size_t stride = 32u;
    std::vector<uint8_t> picture(stride * height);
    for (size_t i = 0; i < picture.size(); ++i)
        picture[i] = static_cast<uint8_t>((i * 37u + 11u) & 0xFFu);

    // Row profile, worked by hand for the first row over all columns: the mean of its 18 colour bytes.
    {
        uint32_t sum = 0u;
        for (uint32_t x = 0; x < width; ++x)
            sum += picture[x * 4u] + picture[x * 4u + 1u] + picture[x * 4u + 2u];
        const uint32_t mean = sum / (width * 3u);
        char expected[3];
        std::snprintf(expected, sizeof(expected), "%02x", mean);
        const std::string rows = rowProfile(picture.data(), stride, width, height, 0u, 0xFFFFFFFFu);
        check(rows.size() == height * 2u, "the row profile has two hex digits a row");
        check(rows.compare(0, 2, expected) == 0, "its first row is the mean of that row's colour bytes (alpha left out)");
    }
    // The same sum as today's window, for whole pictures, a range of columns, ranges cut by the width or outside it.
    {
        const std::pair<uint32_t, uint32_t> ranges[] = {{0u, 0xFFFFFFFFu}, {1u, 4u}, {2u, 3u}, {4u, 99u}, {6u, 9u}, {9u, 12u}, {3u, 3u}};
        bool same = true;
        for (const auto &[a, b] : ranges)
            same = same && rowProfile(picture.data(), stride, width, height, a, b) == yardstick(picture, stride, width, height, a, b);
        check(same, "the row profile equals today's window's sum for seven column ranges");
        check(rowProfile(picture.data(), stride, width, height, 1u, 4u) != rowProfile(picture.data(), stride, width, height, 0u, 0xFFFFFFFFu),
              "a range of columns gives another profile than the whole width");
        check(rowProfile(picture.data(), stride, width, height, 6u, 9u) == std::string(height * 2u, '0'),
              "a range outside the picture gives a profile of zeros");
    }
    // The padding between rows is not read.
    {
        std::vector<uint8_t> other = picture;
        for (uint32_t y = 0; y < height; ++y)
            for (size_t i = width * 4u; i < stride; ++i)
                other[y * stride + i] ^= 0xFFu;
        check(rowProfile(other.data(), stride, width, height, 0u, 0xFFFFFFFFu) == rowProfile(picture.data(), stride, width, height, 0u, 0xFFFFFFFFu) &&
                  fingerprint(other.data(), stride, width, 0u, height) == fingerprint(picture.data(), stride, width, 0u, height),
              "bytes between the rows change neither the profile nor the fingerprint");
    }
    // The column range's text.
    {
        check(columns("505:625") == std::make_pair(505u, 625u), "PS2X_WEAVE_TRACE_COLS=505:625 is columns 505 to 624");
        const auto whole = std::make_pair(0u, 0xFFFFFFFFu);
        check(columns(nullptr) == whole && columns("") == whole && columns("12") == whole && columns("9:9") == whole && columns("9:3") == whole,
              "no text, a text that is no range, and an empty range mean the whole width");
    }
    // Fingerprints.
    {
        const uint64_t all = fingerprint(picture.data(), stride, width, 0u, height);
        const uint64_t top = fingerprint(picture.data(), stride, width, 0u, height / 2u);
        const uint64_t bottom = fingerprint(picture.data(), stride, width, height / 2u, height);
        check(all != top && all != bottom && top != bottom, "the whole picture, its top half and its bottom half have three fingerprints");
        check(fingerprint(picture.data(), stride, width, 2u, 2u) == 1469598103934665603ull, "no rows: the fingerprint's starting value");
        std::vector<uint8_t> changed = picture;
        changed[3u * stride + 5u * 4u + 3u] ^= 1u; // the last pixel's alpha, in the bottom half
        check(fingerprint(changed.data(), stride, width, 0u, height) != all &&
                  fingerprint(changed.data(), stride, width, 0u, height / 2u) == top &&
                  fingerprint(changed.data(), stride, width, height / 2u, height) != bottom,
              "one changed byte in the bottom half changes the whole and the bottom half, not the top half");
    }

    std::printf("%s (%d failed)\n", failures == 0 ? "PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
