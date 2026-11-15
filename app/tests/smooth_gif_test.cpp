// Issue 049 Part 2 (amendment v3.4, P2.7): GIF packets of two consecutive frames paired by place and blended at t.
// No game data: every packet here is made up. Checks reading a part, the structure rule, the blending of each
// register in both layouts, the end points (criterion 8d), the guards, the text rule and the pairing inside a run.
// `dw5xl_smooth_gif_test pairs <file> [limit in pixels]`: not a test: reads pairs of real parts from a local file
// (game data, written by notes' probe p27_unshared_packets.py) and reports what the same code makes of them.
#include "dw5xl_smooth_gif.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace dw5xl::smooth;

namespace {
int failures = 0;
void check(bool ok, const std::string &what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    failures += ok ? 0 : 1;
}

struct Bytes
{
    std::vector<uint8_t> data;
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) data.push_back(static_cast<uint8_t>(v >> (8 * i))); }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) data.push_back(static_cast<uint8_t>(v >> (8 * i))); }
    void f32(float v) { uint32_t bits; std::memcpy(&bits, &v, 4); u32(bits); }
    void vif(uint32_t cmd, uint32_t num = 0, uint32_t imm = 0) { u32((cmd << 24) | (num << 16) | imm); }
    // A GIF tag: flg 0 packed, 1 list, 2 image; regs: one register number a nibble, first register lowest.
    void tag(uint32_t nloop, uint32_t flg, uint32_t nreg, uint64_t regs, uint32_t prim = 0, bool pre = false)
    {
        u64(static_cast<uint64_t>(nloop) | (1ull << 15) | (static_cast<uint64_t>(pre) << 46) |
            (static_cast<uint64_t>(prim) << 47) | (static_cast<uint64_t>(flg) << 58) | (static_cast<uint64_t>(nreg) << 60));
        u64(regs);
    }
    // Packed quadwords.
    void rgbaq(uint8_t r, uint8_t g, uint8_t b, uint8_t a) { u32(r); u32(g); u32(b); u32(a); }
    void uv(uint32_t u, uint32_t v) { u32(u); u32(v); u64(0); }
    void st(float s, float t, float q) { f32(s); f32(t); f32(q); u32(0); }
    void xyz2(uint32_t x, uint32_t y, uint32_t z, bool adc = false) { u32(x); u32(y); u32(z); u32(adc ? 0x8000u : 0u); }
    void xyzf2(uint32_t x, uint32_t y, uint32_t z, uint32_t f) { u32(x); u32(y); u32(z << 4); u32(f << 4); }
    void ad(uint8_t reg, uint64_t value) { u64(value); u64(reg); }
};

constexpr uint32_t kNop = 0x00, kFlusha = 0x13, kDirect = 0x50, kUnpackV4_32 = 0x6C;
constexpr uint64_t kRgbaq = 1, kSt = 2, kUv = 3, kXyzf2 = 4, kXyz2 = 5, kAd = 14;

// A textured sprite in the packed layout: colour, then two corners with their texture coordinates.
Bytes sprite(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t z, uint8_t colour, uint32_t u0, uint32_t u1,
             bool adc = false)
{
    Bytes b;
    b.vif(kNop); b.vif(kNop); b.vif(kFlusha); b.vif(kDirect, 0, 6);
    b.tag(1, 0, 5, kRgbaq | (kUv << 4) | (kXyz2 << 8) | (kUv << 12) | (kXyz2 << 16), 6, true);
    b.rgbaq(colour, colour, colour, 0x80);
    b.uv(u0, 16); b.xyz2(x0, y0, z, adc);
    b.uv(u1, 48); b.xyz2(x1, y1, z);
    return b;
}

// The same sprite in the list layout (one doubleword a register), with ST for its texture coordinates.
Bytes listSprite(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t z, uint8_t colour, float s0, float s1)
{
    Bytes b;
    b.vif(kNop); b.vif(kNop); b.vif(kNop); b.vif(kDirect, 0, 4);
    b.tag(1, 1, 5, kRgbaq | (kSt << 4) | (kXyz2 << 8) | (kSt << 12) | (kXyz2 << 16), 6, true);
    const auto word = [&b](float s, float t) { b.f32(s); b.f32(t); };
    b.u64(static_cast<uint64_t>(colour) | (static_cast<uint64_t>(colour) << 8) | (static_cast<uint64_t>(colour) << 16) |
          (0x80ull << 24) | (0x3F800000ull << 32));
    word(s0, 0.25f); b.u64(x0 | (static_cast<uint64_t>(y0) << 16) | (static_cast<uint64_t>(z) << 32));
    word(s1, 0.75f); b.u64(x1 | (static_cast<uint64_t>(y1) << 16) | (static_cast<uint64_t>(z) << 32));
    b.u64(0); // a list of five doublewords is filled up to a quadword
    return b;
}

uint32_t u32At(const std::vector<uint8_t> &d, size_t at) { uint32_t v; std::memcpy(&v, d.data() + at, 4); return v; }
uint16_t u16At(const std::vector<uint8_t> &d, size_t at) { uint16_t v; std::memcpy(&v, d.data() + at, 2); return v; }
float f32At(const std::vector<uint8_t> &d, size_t at) { float v; std::memcpy(&v, d.data() + at, 4); return v; }

GifRefusal judge(const Bytes &before, const Bytes &now, float limit, GifPairStats *stats = nullptr)
{
    GifPart a, b;
    if (!readGifPart(before.data.data(), before.data.size(), a) || !readGifPart(now.data.data(), now.data.size(), b))
        return GifRefusal::Shape;
    return checkGifPair(before.data.data(), before.data.size(), a, now.data.data(), now.data.size(), b, limit, stats);
}

std::vector<uint8_t> blended(const Bytes &before, const Bytes &now, float t)
{
    GifPart shape;
    readGifPart(now.data.data(), now.data.size(), shape);
    std::vector<uint8_t> out(now.data.size(), 0xEEu);
    blendGifPair(before.data.data(), now.data.data(), now.data.size(), shape, t, out.data());
    return out;
}
} // namespace

// The report on real pairs (see the top of this file).
int reportPairs(const char *path, float limit)
{
    std::ifstream in(path, std::ios::binary);
    const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    uint32_t pairs = 0, gifPairs = 0, refused[5] = {}, differing = 0, held = 0, endsWrong = 0, atZeroIsBefore = 0;
    size_t blendedBytes = 0;
    std::string moves;
    for (size_t at = 0; at + 8u <= file.size();)
    {
        uint32_t beforeBytes, nowBytes;
        std::memcpy(&beforeBytes, file.data() + at, 4);
        std::memcpy(&nowBytes, file.data() + at + 4, 4);
        const uint8_t *before = file.data() + at + 8, *now = before + beforeBytes;
        at += 8u + beforeBytes + nowBytes;
        if (at > file.size())
            break;
        ++pairs;
        GifPart a, b;
        if (!readGifPart(now, nowBytes, b))
            continue;
        ++gifPairs;
        GifPairStats stats;
        const GifRefusal why = readGifPart(before, beforeBytes, a)
                                   ? checkGifPair(before, beforeBytes, a, now, nowBytes, b, limit, &stats)
                                   : GifRefusal::Shape;
        ++refused[static_cast<int>(why)];
        if (why != GifRefusal::None)
            continue;
        differing += stats.differing;
        held += stats.held;
        blendedBytes += nowBytes;
        char text[32];
        std::snprintf(text, sizeof(text), " %.1f", stats.largestMove);
        moves += text;
        std::vector<uint8_t> out(nowBytes);
        blendGifPair(before, now, nowBytes, b, 1.0f, out.data());
        endsWrong += std::memcmp(out.data(), now, nowBytes) != 0 ? 1u : 0u;
        blendGifPair(before, now, nowBytes, b, 0.0f, out.data());
        atZeroIsBefore += std::memcmp(out.data(), before, nowBytes) == 0 ? 1u : 0u;
    }
    std::printf("%u pairs by place, %u of them GIF parts\n", pairs, gifPairs);
    std::printf("accepted %u (%zu bytes); refused: size %u, shape %u, state %u, moved further than %.0f pixels %u\n",
                refused[0], blendedBytes, refused[1], refused[2], refused[3], limit, refused[4]);
    std::printf("in the accepted pairs: %u vertex writes differ, %u of them held by the text rule\n", differing, held);
    std::printf("largest vertex move of each accepted pair, in PS2 pixels:%s\n", moves.c_str());
    std::printf("t = 1 gives the newer part byte for byte in %u of %u; t = 0 gives the part before byte for byte in %u "
                "(the others hold texture coordinates at the newer frame)\n", refused[0] - endsWrong, refused[0], atZeroIsBefore);
    return endsWrong ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && std::strcmp(argv[1], "pairs") == 0)
        return reportPairs(argv[2], argc >= 4 ? static_cast<float>(std::atof(argv[3])) : 64.0f);
    // --- Reading a part.
    {
        const Bytes s = sprite(1600, 3200, 2000, 3600, 1000, 100, 16, 48);
        GifPart part;
        check(readGifPart(s.data.data(), s.data.size(), part), "a part of VIF codes and one DIRECT with a whole GIF packet is read");
        check(part.writes.size() == 5u && part.images.empty(), "its five register writes are found");
        check(part.writes.size() == 5u && part.writes[2].reg == kXyz2 && part.writes[2].offset == 16u + 16u + 32u &&
                  part.writes[2].form == GifWrite::Form::Packed,
              "a write knows its register, its place and its layout");
        check(part.shape.size() == 4u + 2u, "the shape holds the four VIF codes and the tag's two doublewords");

        Bytes unpack;
        unpack.vif(kNop); unpack.vif(kNop); unpack.vif(kNop); unpack.vif(kUnpackV4_32, 1, 0);
        unpack.u64(0); unpack.u64(0);
        check(!readGifPart(unpack.data.data(), unpack.data.size(), part), "a part with UNPACK data is no GIF part");

        Bytes codes;
        codes.vif(kNop); codes.vif(kNop); codes.vif(kFlusha); codes.vif(kNop);
        check(!readGifPart(codes.data.data(), codes.data.size(), part), "a part without DIRECT data is no GIF part");

        Bytes cut = s;
        cut.data.resize(cut.data.size() - 16u);
        check(!readGifPart(cut.data.data(), cut.data.size(), part), "a part whose DIRECT data is cut short is no GIF part");

        Bytes shortTag = s;
        shortTag.data[16] = 2; // NLOOP 2 in a packet that holds one loop
        check(!readGifPart(shortTag.data.data(), shortTag.data.size(), part), "a GIF packet longer than its DIRECT data is no GIF part");
    }

    // --- The structure rule.
    {
        const Bytes a = sprite(1600, 3200, 2000, 3600, 1000, 100, 16, 48);
        Bytes otherPrim = a;
        otherPrim.data[16 + 5] ^= 0x80u; // a bit of PRIM in the tag
        check(judge(a, otherPrim, 1000.0f) == GifRefusal::Shape, "another primitive kind in the tag: not the same shape");
        Bytes longer = a;
        longer.u64(0); longer.u64(0);
        check(judge(a, longer, 1000.0f) == GifRefusal::Size || judge(a, longer, 1000.0f) == GifRefusal::Shape,
              "another size: not blended");
        Bytes otherCode = a;
        otherCode.data[8 + 3] = 0x11; // FLUSH in place of FLUSHA
        check(judge(a, otherCode, 1000.0f) == GifRefusal::Shape, "another VIF code: not the same shape");
        check(judge(a, a, 1000.0f) == GifRefusal::None, "the same part twice: a pair");
    }

    // --- Blending, the packed layout: a sprite that moves 10 pixels right and 5 down, gets nearer, brighter, and
    // whose texture coordinates move with it.
    {
        const Bytes before = sprite(1600, 3200, 2000, 3600, 1000, 100, 16, 48);
        const Bytes now = sprite(1760, 3280, 2160, 3680, 2000, 200, 32, 64);
        GifPairStats stats;
        check(judge(before, now, 64.0f, &stats) == GifRefusal::None, "a moving sprite: a pair");
        check(stats.largestMove > 9.99f && stats.largestMove < 10.01f, "its largest move is 10 pixels");
        check(stats.differing == 5u && stats.held == 0u, "five vertex writes differ (a colour, two coordinates, two positions), none is held");
        const std::vector<uint8_t> half = blended(before, now, 0.5f);
        const size_t q = 32; // the first register quadword
        check(half[q] == 150 && half[q + 4] == 150 && half[q + 8] == 150 && half[q + 12] == 0x80, "packed colour: half way, alpha untouched");
        check(u16At(half, q + 16) == 24, "packed texture coordinate: half way");
        check(u16At(half, q + 32) == 1680 && u16At(half, q + 36) == 3240, "packed position: half way");
        check(u32At(half, q + 40) == 1500u, "packed depth: half way");
        check(u16At(half, q + 64) == 2080 && u16At(half, q + 68) == 3640, "the second corner too");
        check(std::memcmp(half.data(), now.data.data(), 32) == 0, "the VIF codes and the tag are the newer frame's");
        // Criterion 8d.
        check(blended(before, now, 1.0f) == now.data, "t = 1: the newer frame's part, byte for byte");
        check(blended(before, now, 0.0f) == before.data, "t = 0: the frame before's value in every blended value");
        check(blended(before, now, 2.0f) == now.data && blended(before, now, -1.0f) == before.data, "t outside 0 to 1 is held at the ends");
        const std::vector<uint8_t> quarter = blended(before, now, 0.25f);
        check(u16At(quarter, q + 32) == 1640 && quarter[q] == 125, "t = 0.25: a quarter of the way");
    }

    // --- Blending, the list layout and float texture coordinates.
    {
        const Bytes before = listSprite(1600, 3200, 2000, 3600, 1000, 100, 0.25f, 0.5f);
        const Bytes now = listSprite(1760, 3280, 2160, 3680, 3000, 200, 0.75f, 1.0f);
        check(judge(before, now, 64.0f) == GifRefusal::None, "a moving sprite in the list layout: a pair");
        const std::vector<uint8_t> half = blended(before, now, 0.5f);
        const size_t d = 32; // the first register doubleword
        check(half[d] == 150 && half[d + 1] == 150 && half[d + 2] == 150 && half[d + 3] == 0x80 && f32At(half, d + 4) == 1.0f,
              "list colour: half way, alpha and Q untouched");
        check(f32At(half, d + 8) == 0.5f && f32At(half, d + 12) == 0.25f, "list texture coordinate S: half way; T, equal in both, untouched");
        check(u16At(half, d + 16) == 1680 && u16At(half, d + 18) == 3240 && u32At(half, d + 20) == 2000u, "list position and depth: half way");
        check(blended(before, now, 1.0f) == now.data && blended(before, now, 0.0f) == before.data, "list layout: the end points (8d)");
    }

    // --- Fog in a packed XYZF2, and a vertex written through A+D.
    {
        Bytes before, now;
        for (Bytes *b : {&before, &now})
        {
            const bool later = b == &now;
            b->vif(kNop); b->vif(kNop); b->vif(kNop); b->vif(kDirect, 0, 3);
            b->tag(1, 0, 2, kXyzf2 | (kAd << 4), 0, false);
            b->xyzf2(later ? 1760 : 1600, 3200, later ? 0x400 : 0x200, later ? 200 : 100);
            b->ad(0x05, (later ? 1920ull : 1600ull) | (3200ull << 16) | (5000ull << 32)); // XYZ2 by A+D
        }
        check(judge(before, now, 64.0f) == GifRefusal::None, "XYZF2 and a position by A+D: a pair");
        const std::vector<uint8_t> half = blended(before, now, 0.5f);
        check(u16At(half, 32) == 1680 && (u32At(half, 40) >> 4) == 0x300u && ((u32At(half, 44) >> 4) & 0xFFu) == 150u,
              "packed XYZF2: position, depth and fog half way");
        check(u16At(half, 48) == 1760 && u16At(half, 50) == 3200 && half[56] == 0x05, "a position by A+D: half way in the register's own layout");
    }

    // --- Guards.
    {
        const Bytes before = sprite(1600, 3200, 2000, 3600, 1000, 100, 16, 48);
        const Bytes far = sprite(1600 + 100 * 16, 3200, 2000 + 100 * 16, 3600, 1000, 100, 16, 48);
        GifPairStats stats;
        check(judge(before, far, 64.0f, &stats) == GifRefusal::Moved && stats.largestMove > 99.9f, "a vertex 100 pixels away with a limit of 64: not blended");
        check(judge(before, far, 128.0f) == GifRefusal::None, "the same with a limit of 128: a pair");

        const Bytes hidden = sprite(1600, 3200, 2000, 3600, 1000, 100, 16, 48, true);
        check(judge(before, hidden, 64.0f) == GifRefusal::State, "a vertex drawn in one frame and not in the other: not blended");

        Bytes stateA, stateB;
        for (Bytes *b : {&stateA, &stateB})
        {
            b->vif(kNop); b->vif(kNop); b->vif(kNop); b->vif(kDirect, 0, 3);
            b->tag(1, 0, 2, kAd | (kXyz2 << 4), 0, false);
            b->ad(0x06, b == &stateB ? 0x1234ull : 0x1230ull); // TEX0_1
            b->xyz2(1600, 3200, 1000);
        }
        check(judge(stateA, stateB, 64.0f) == GifRefusal::State, "another texture (TEX0 by A+D): not blended");
        Bytes otherRegister = stateA;
        otherRegister.data[32 + 8] = 0x42; // the same A+D quadword names ALPHA_1
        check(judge(stateA, otherRegister, 64.0f) == GifRefusal::State, "an A+D write that names another register: not blended");

        Bytes imageA, imageB;
        for (Bytes *b : {&imageA, &imageB})
        {
            b->vif(kNop); b->vif(kNop); b->vif(kNop); b->vif(kDirect, 0, 2);
            b->tag(1, 2, 0, 0);
            b->u64(b == &imageB ? 7ull : 5ull); b->u64(0);
        }
        check(judge(imageA, imageA, 64.0f) == GifRefusal::None && judge(imageA, imageB, 64.0f) == GifRefusal::State,
              "texture data: equal is a pair, different is not blended");
    }

    // --- The text rule: a vertex that does not move keeps the newer frame's texture coordinates.
    {
        const Bytes before = sprite(1600, 3200, 2000, 3600, 1000, 100, 16, 48);
        const Bytes glyph = sprite(1600, 3200, 2000, 3600, 1000, 100, 80, 112); // same place, another glyph
        GifPairStats stats;
        check(judge(before, glyph, 64.0f, &stats) == GifRefusal::None && stats.held == 2u && stats.differing == 2u,
              "a sprite in place with other texture coordinates: a pair, both coordinates held");
        check(blended(before, glyph, 0.5f) == glyph.data && blended(before, glyph, 0.0f) == glyph.data,
              "its texture coordinates are the newer frame's at every t");
        // One corner moves, the other stays: only the moving corner's coordinate is blended.
        const Bytes mixed = sprite(1600, 3200, 2160, 3600, 1000, 100, 80, 112);
        const std::vector<uint8_t> half = blended(before, mixed, 0.5f);
        check(u16At(half, 32 + 16) == 80 && u16At(half, 32 + 48) == 80 && u16At(half, 32 + 64) == 2080,
              "of two corners the one in place keeps its coordinate, the moving one's is blended with its position");
        // Colours are blended wherever they differ.
        const Bytes brighter = sprite(1600, 3200, 2000, 3600, 1000, 200, 16, 48);
        check(blended(before, brighter, 0.5f)[32] == 150, "a colour change in place is blended");
    }

    // --- Pairing inside a run.
    {
        using Pairs = std::vector<std::pair<uint32_t, uint32_t>>;
        const Pairs shared = {{0, 0}, {3, 3}, {4, 5}};
        check(pairUnshared(shared, 6, 7) == Pairs{{1, 1}, {2, 2}, {5, 6}},
              "unshared parts pair first with first inside a run; a run with another count on one side pairs nothing");
        check(pairUnshared({}, 2, 2) == Pairs{{0, 0}, {1, 1}}, "two lists that share nothing and have the same count: one run");
        check(pairUnshared({}, 2, 3).empty(), "another count: no pairs");
        check(pairUnshared({{0, 0}, {1, 1}}, 2, 2).empty(), "everything shared: no pairs");
        check(pairUnshared({{1, 2}}, 3, 4) == Pairs{{2, 3}}, "before the first shared part and behind the last");
    }

    std::printf("%s: %d failures\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
