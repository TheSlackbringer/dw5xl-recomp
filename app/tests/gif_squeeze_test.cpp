// Issue 047: GIF packet walking and squeezing, on synthetic packets.
#include "dw5xl_gif_squeeze.h"
#include <cstdio>
#include <vector>

using namespace dw5xl::gifsqueeze;
namespace {
int failures = 0;
void require(bool ok, const char *what) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; } }
uint64_t tag(uint64_t nloop, uint64_t flg, uint64_t nreg) { return nloop | flg << 58 | nreg << 60; }
void put(std::vector<uint8_t> &p, size_t qw, uint64_t low, uint64_t high)
{
    std::memcpy(&p[qw * 16], &low, 8);
    std::memcpy(&p[qw * 16 + 8], &high, 8);
}
uint16_t x16(const std::vector<uint8_t> &p, size_t bytes) { uint16_t v; std::memcpy(&v, &p[bytes], 2); return v; }
}

int main()
{
    // REGLIST, as FUN_00140B10: PRIM, TEX0, CLAMP, RGBAQ, UV, XYZ2, UV, XYZ2. XYZ2 at qw 3 high and qw 4 high.
    // Across the middle: scaled about the centre.
    std::vector<uint8_t> list(5 * 16);
    put(list, 0, tag(1, 1, 8), 0x53531860);
    put(list, 3, 0x1234, 0x7400);            // UV, XYZ2
    put(list, 4, 0x1234, 0x8400);
    require(squeeze(list.data(), 5, 0.75f), "reglist squeezed");
    require(x16(list, 3 * 16) == 0x1234, "reglist: UV untouched");
    require(x16(list, 3 * 16 + 8) == 0x7700, "reglist: left x moves to the centre");
    require(x16(list, 4 * 16 + 8) == 0x8300, "reglist: right x moves to the centre");

    // In one half: stays at its side of the picture, in its shape. Life gauge left, K.O. count right.
    std::vector<uint8_t> left(3 * 16);
    put(left, 0, tag(2, 0, 1), 0x5);
    put(left, 1, 0x7400, 0);
    put(left, 2, 0x7800, 0);
    require(squeeze(left.data(), 3, 0.75f) && x16(left, 16) == 0x7300 && x16(left, 32) == 0x7600, "left half: about the left edge");
    std::vector<uint8_t> right(3 * 16);
    put(right, 0, tag(2, 0, 1), 0x5);
    put(right, 1, 0x8800, 0);
    put(right, 2, 0x8C00, 0);
    require(squeeze(right.data(), 3, 0.75f) && x16(right, 16) == 0x8A00 && x16(right, 32) == 0x8D00, "right half: about the right edge");

    // PACKED XYZ2, and A+D: XYZ2 and a part-frame scissor; across the middle.
    std::vector<uint8_t> packed(6 * 16);
    put(packed, 0, tag(1, 0, 1), 0x5);
    put(packed, 1, 0x7800, 0x1111);
    put(packed, 2, tag(3, 0, 1), 0xE);
    put(packed, 3, 0x8800, 0x05);
    put(packed, 4, uint64_t(156u) | uint64_t(356) << 16, 0x40);
    put(packed, 5, 0, 0x47);                 // another register stays
    require(squeeze(packed.data(), 6, 0.5f), "packed squeezed");
    require(x16(packed, 1 * 16) == 0x7C00, "packed x");
    require(x16(packed, 3 * 16) == 0x8400, "A+D x");
    uint64_t scissor; std::memcpy(&scissor, &packed[4 * 16], 8);
    require((scissor & 0x7FF) == 206 && ((scissor >> 16) & 0x7FF) == 306, "part-frame scissor");

    // A whole-frame draw is left alone; a whole-frame scissor stays.
    std::vector<uint8_t> whole(3 * 16), before;
    put(whole, 0, tag(2, 0, 1), 0x5);
    put(whole, 1, kLeft, 0);
    put(whole, 2, kRight, 0);
    before = whole;
    require(!squeeze(whole.data(), 3, 0.75f) && whole == before, "whole-frame draw unchanged");
    std::vector<uint8_t> full(3 * 16);
    put(full, 0, tag(2, 0, 1), 0xE);
    put(full, 1, 511ull << 16, 0x40);
    put(full, 2, 0x7400, 0x05);
    require(squeeze(full.data(), 3, 0.75f), "whole-frame scissor packet squeezed");
    std::memcpy(&scissor, &full[16], 8);
    require(scissor == 511ull << 16, "whole-frame scissor unchanged");

    // A band across the whole picture stays, the words on it in the same packet narrow about the centre.
    std::vector<uint8_t> band(6 * 16);
    put(band, 0, tag(2, 0, 1), 0x5);
    put(band, 1, kLeft, 0);
    put(band, 2, kRight, 0);
    put(band, 3, tag(2, 0, 1), 0x5);
    put(band, 4, 0x7400, 0);
    put(band, 5, 0x8400, 0);
    require(squeeze(band.data(), 6, 0.75f) && x16(band, 16) == kLeft && x16(band, 32) == kRight && x16(band, 64) == 0x7700 && x16(band, 80) == 0x8300, "band and words");

    // Parked off screen: not counted, not moved.
    std::vector<uint8_t> parked(3 * 16);
    put(parked, 0, tag(2, 0, 1), 0x5);
    put(parked, 1, 0, 0);
    put(parked, 2, kRight, 0);
    require(squeeze(parked.data(), 3, 0.75f) && x16(parked, 16) == 0 && x16(parked, 32) == kRight, "parked draw");

    // IMAGE data is skipped, and a tag running past the end stops the walk.
    std::vector<uint8_t> image(4 * 16);
    put(image, 0, tag(1, 2, 0), 0);
    put(image, 1, 0x7400, 0);
    put(image, 2, tag(1, 0, 1), 0x5);
    put(image, 3, 0x7400, 0);
    require(squeeze(image.data(), 4, 0.75f) && x16(image, 16) == 0x7400 && x16(image, 48) == 0x7300, "image skipped");
    std::vector<uint8_t> cut(2 * 16);
    put(cut, 0, tag(9, 0, 1), 0x5);
    put(cut, 1, 0x7400, 0);
    require(squeeze(cut.data(), 2, 0.75f) && x16(cut, 16) == 0x7300, "cut packet");

    if (!failures) std::puts("gif squeeze checks passed");
    return failures ? 1 : 0;
}
