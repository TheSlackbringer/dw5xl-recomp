// CTest menu_assets (issue 023 step 3S): readers for the game's menu pictures and the word/title building, on
// synthetic data only (no game files): LINKDATA.IDX, LZP2 (all three token kinds, overlapping copy, damaged input),
// TIM2 4-bit with a chosen palette and 8-bit in the PS2 palette order, the glyph table, letters split by shape and
// rebuilt into the same word, and the title glow learned and regenerated.
#include "../src/dw5xl_menu_assets.h"
#include <cstdio>
#include <string>

namespace {
int g_failures = 0;
void check(bool ok, const char *what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++g_failures;
}
void put32(std::vector<uint8_t> &v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); }

// A word image: letters as filled boxes (bright fill, dark 2-pixel outline), outlines of neighbours touching.
dw5xl::menu::Image boxWord(const std::vector<std::pair<int, int>> &fills, int width)
{
    dw5xl::menu::Image img(width, 24);
    for (auto [x0, x1] : fills)
        for (int y = 2; y < 22; ++y)
            for (int x = x0 - 2; x <= x1 + 2; ++x) {
                const bool fill = x >= x0 && x <= x1 && y >= 4 && y < 20;
                uint8_t *p = img.at(x, y);
                if (fill) { p[0] = p[1] = p[2] = 240; p[3] = 255; }
                else if (p[3] == 0) { p[0] = p[1] = p[2] = 30; p[3] = 200; }
            }
    return img;
}
}

int main()
{
    using namespace dw5xl::menu;
    {   // LINKDATA.IDX
        std::vector<uint8_t> idx = {'S', 'M', '4', 'L'};
        put32(idx, 2); put32(idx, 0); put32(idx, 0);
        put32(idx, 0); put32(idx, 74); put32(idx, 150224); put32(idx, 0);
        put32(idx, 0x3312); put32(idx, 942); put32(idx, 1928384); put32(idx, 0);
        const auto e = parseLinkIndex(idx.data(), idx.size());
        check(e.size() == 2 && e[1].offset() == 26775552u && e[1].size == 1928384u, "LINKDATA.IDX entries");
        idx[0] = 'X';
        check(parseLinkIndex(idx.data(), idx.size()).empty(), "LINKDATA.IDX: wrong magic gives no entries");
    }
    {   // LZP2: literals "ABCD", fill 6 x 'z', copy 5 bytes from 4 back (overlapping: ABCD... -> zzzzz), literal "!"
        std::vector<uint8_t> s = {'L', 'Z', 'P', '2', 0xae, 0x47, 0x81, 0x3f};
        const std::string expect = "ABCDzzzzzzzzzzz!";
        put32(s, uint32_t(expect.size())); put32(s, 0);
        s.insert(s.end(), {0x04, 'A', 'B', 'C', 'D', 0x40, 0x02, 'z', uint8_t(0x80 | ((5 - 3) << 3)), 3, 0x01, '!'});
        std::vector<uint8_t> out;
        const bool ok = unpackLzp2(s.data(), s.size(), out);
        check(ok && std::string(out.begin(), out.end()) == expect, "LZP2: literal, fill and overlapping copy");
        std::vector<uint8_t> cut(s.begin(), s.end() - 3);
        check(!unpackLzp2(cut.data(), cut.size(), out), "LZP2: a truncated stream is refused, not read past its end");
        std::vector<uint8_t> bad = s; bad[16 + 5 + 3 + 1] = 200;  // copy distance beyond the output so far
        check(!unpackLzp2(bad.data(), bad.size(), out), "LZP2: a copy from before the start is refused");
    }
    {   // TIM2 4-bit, 2x2, two palettes; and 8-bit with the PS2 palette order
        auto tim = [](uint8_t imageType, int w, int h, const std::vector<uint8_t> &pixels, const std::vector<uint8_t> &clut, uint32_t colors) {
            std::vector<uint8_t> v = {'T', 'I', 'M', '2', 4, 0, 1, 0}; put32(v, 0); put32(v, 0);
            put32(v, uint32_t(48 + pixels.size() + clut.size())); put32(v, uint32_t(clut.size())); put32(v, uint32_t(pixels.size()));
            v.push_back(48); v.push_back(0); v.push_back(uint8_t(colors)); v.push_back(uint8_t(colors >> 8));
            v.insert(v.end(), {0, 1, 3, imageType}); v.push_back(uint8_t(w)); v.push_back(0); v.push_back(uint8_t(h)); v.push_back(0);
            v.resize(16 + 48, 0); v.insert(v.end(), pixels.begin(), pixels.end()); v.insert(v.end(), clut.begin(), clut.end());
            return v;
        };
        std::vector<uint8_t> clut(32 * 4, 0);
        clut[16 * 4 + 1 * 4 + 0] = 99; clut[16 * 4 + 1 * 4 + 3] = 0x80;      // palette 1, colour 1: red 99, opaque
        const auto t4 = tim(4, 2, 2, {0x10, 0x00}, clut, 32);
        const auto pics = findTim2(t4.data(), t4.size());
        bool ok = pics.size() == 1;
        if (ok) { const Image img = decodeTim2(t4.data(), t4.size(), pics[0], 1); ok = img.w == 2 && img.at(1, 0)[0] == 99 && img.at(1, 0)[3] == 255 && img.at(0, 0)[3] == 0; }
        check(ok, "TIM2 4-bit: pixel order and chosen palette");
        std::vector<uint8_t> clut8(256 * 4, 0); clut8[16 * 4] = 77; clut8[16 * 4 + 3] = 0x80;   // stored entry 16
        const auto t8 = tim(5, 1, 1, {8}, clut8, 256);                                           // index 8 -> entry 16
        const auto p8 = findTim2(t8.data(), t8.size());
        check(p8.size() == 1 && decodeTim2(t8.data(), t8.size(), p8[0]).at(0, 0)[0] == 77, "TIM2 8-bit: PS2 palette order");
    }
    {   // glyph table: 16x20 4-bit, palette alpha 0x80 -> 255
        std::vector<uint8_t> glyph(160, 0), pal(64, 0);
        glyph[0] = 0x20; pal[2 * 4 + 1] = 50; pal[2 * 4 + 3] = 0x40;
        const Image g = decodeGlyph(glyph.data(), pal.data());
        check(g.w == 16 && g.h == 20 && g.at(1, 0)[1] == 50 && g.at(1, 0)[3] == 128 && g.at(0, 0)[3] == 0, "glyph table");
    }
    {   // letters split by shape and rebuilt
        const Image word = boxWord({{4, 13}, {16, 21}, {24, 37}}, 44);     // fills 2 columns apart, outlines touching
        const auto letters = splitLetters(word, 3);
        bool ok = letters.size() == 3;
        if (ok) ok = letters[0].fill1 - letters[0].fill0 == 9 && letters[1].fill1 - letters[1].fill0 == 5 && letters[2].fill1 - letters[2].fill0 == 13;
        check(ok, "letters split by shape (outlines touching)");
        if (ok) {
            const Image rebuilt = buildWord({&letters[0], &letters[1], &letters[2]}, 9, 2, 4 - letters[0].fill0 + 2);
            int a = 0, b = 0;
            for (int x = 0; x < word.w; ++x) if (word.at(x, 12)[3]) { a = x; break; }
            for (int x = 0; x < rebuilt.w; ++x) if (rebuilt.at(x, 12)[3]) { b = x; break; }
            int diff = 0;
            for (int y = 0; y < 24; ++y) for (int x = 0; x + a < word.w && x + b < rebuilt.w; ++x)
                diff += std::abs(int(word.at(x + a, y)[3]) - int(rebuilt.at(x + b, y)[3]));
            check(diff == 0, "word rebuilt from its letters is identical");
            const Image spaced = buildWord({&letters[0], nullptr, &letters[2]}, 9, 2);
            check(spaced.w > letters[0].img.w + letters[2].img.w + 9, "a space widens the word");
        }
    }
    {   // title glow: learned from a core with a known glow, regenerated identically
        Image title(40, 20);
        for (int y = 0; y < 20; ++y) for (int x = 0; x < 40; ++x) {
            const float dx = float(std::max(0, std::abs(x - 20) - 6)), dy = float(std::max(0, std::abs(y - 10) - 4));
            const float d = std::sqrt(dx * dx + dy * dy);                       // distance to the solid core
            uint8_t *p = title.at(x, y);
            if (d == 0) { p[0] = 60; p[1] = 40; p[2] = 20; p[3] = 255; }
            else if (d < 6) { p[0] = p[1] = p[2] = 200; p[3] = uint8_t(200 - 30 * d); }
        }
        const GlowProfile prof = learnGlow(title);
        const Image regen = applyGlow(cores(title), prof);
        int maxErr = 0, sum = 0;
        for (size_t i = 0; i < title.rgba.size(); i += 4) {
            const int e = std::abs(int(title.rgba[i + 3]) - int(regen.rgba[i + 3]));
            maxErr = std::max(maxErr, e); sum += e;
        }
        // The profile averages one-pixel distance rings, so the synthetic glow's hard cut at distance 6 is softened
        // there (the game's glow fades out smoothly); everywhere else the glow comes back as it was.
        check(double(sum) / double(title.w * title.h) <= 2.0 && maxErr <= 16, "title glow learned and regenerated");
    }
    std::printf("%s\n", g_failures ? "menu_assets: FAIL" : "menu_assets: PASS");
    return g_failures ? 1 : 0;
}
