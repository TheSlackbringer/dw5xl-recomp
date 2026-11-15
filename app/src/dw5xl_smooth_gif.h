// Issue 049 Part 2 (amendment v3.4, P2.7): effects, gauges and HUD at the in-between rate. The game's own code
// projects these shapes and sends them as finished GIF packets (through VIF DIRECT); the list holds screen
// coordinates, colours and texture coordinates for them, no matrices. Two consecutive frames' packets of the same
// shape are paired by their place in the list, and every vertex value that differs is moved straight between its
// two values.
// No runtime or game access in this file's functions: they work on memory handed in, so they are unit-tested without
// a game (app/tests/smooth_gif_test.cpp).
#ifndef DW5XL_SMOOTH_GIF_H
#define DW5XL_SMOOTH_GIF_H

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace dw5xl::smooth {

// One register write of a GIF packet, where it lies in its part.
struct GifWrite
{
    enum class Form : uint8_t
    {
        Packed, // a quadword in the packed layout of its register (GIF tag FLG 0)
        Direct, // the register's own 64-bit layout: a doubleword of a list (FLG 1), or the data of an A+D quadword
    };
    uint32_t offset = 0; // of the write's first byte in the part
    uint8_t reg = 0;     // the GS register written (for A+D: the register its address names)
    Form form = Form::Packed;
    bool viaAd = false;  // an A+D quadword: the address byte lies 8 bytes behind `offset`
};

// A list part read as GIF packets.
struct GifPart
{
    // Every VIF code and every GIF tag of the part, in order (a tag as its two doublewords). Two parts of the same
    // size with equal shapes hold the same register writes in the same places; only the numbers differ.
    std::vector<uint64_t> shape;
    std::vector<GifWrite> writes;                      // in order
    std::vector<std::pair<uint32_t, uint32_t>> images; // offset and bytes of IMAGE data (texture data)
};

// Reads a part's VIF data (what follows the DMA tag's upper half). True when the part is VIF codes without data of
// their own (NOP, MARK, FLUSHE, FLUSH, FLUSHA) and at least one DIRECT code whose data is whole GIF packets. False
// for anything else (UNPACK data, a microprogram or its start, a packet cut short): such a part is never blended.
bool readGifPart(const uint8_t *part, size_t bytes, GifPart &out);

// Why a pair of parts is not blended.
enum class GifRefusal : uint8_t
{
    None,
    Size,  // the two parts differ in size
    Shape, // they differ in a VIF code or a GIF tag
    State, // a write that is no vertex value differs (texture, blend mode, scissor ..., texture data, an A+D
           // write naming another register), or a vertex's "do not draw" bit
    Moved, // a vertex moves further on the screen in one frame than the limit
};

struct GifPairStats
{
    uint32_t differing = 0;   // vertex writes (a position, a colour, a texture coordinate) that differ between the two parts
    uint32_t held = 0;        // of them: texture coordinates of a vertex that does not move (left at the newer frame)
    float largestMove = 0.0f; // the furthest a vertex moves, in PS2 pixels
};

// Whether `now` may be blended with `before`, the part in its place in the frame before. `shape` is `now` read by
// readGifPart; `beforeShape` the frame before's part read the same way. `limitPixels`: guard 3.
GifRefusal checkGifPair(const uint8_t *before, size_t beforeBytes, const GifPart &beforeShape, const uint8_t *now,
                        size_t nowBytes, const GifPart &shape, float limitPixels, GifPairStats *stats = nullptr);

// The part of an in-between picture at t between the frame before (t = 0) and this frame (t = 1), for a pair that
// checkGifPair accepted: `out` (nowBytes long) is `now` with every vertex value that differs moved straight between
// its two values: positions (X, Y, Z, fog), colours, texture coordinates. A vertex whose position is the same in
// both frames keeps the newer frame's texture coordinates (HUD numbers and text change glyphs that way; blending
// would smear one into the other). t >= 1 gives `now` byte for byte; t <= 0 gives the frame before's value in
// every value that is blended.
void blendGifPair(const uint8_t *before, const uint8_t *now, size_t bytes, const GifPart &shape, float t, uint8_t *out);

// The unshared parts of two lined-up lists, paired by place: `shared` holds the pairs of part numbers (before, now)
// the two lists share, in order. Between two shared parts (and before the first, behind the last) the parts that
// are not shared pair up first with first, second with second, but only where both lists hold the same number of
// them there. Returns the pairs (before, now), in order.
std::vector<std::pair<uint32_t, uint32_t>> pairUnshared(const std::vector<std::pair<uint32_t, uint32_t>> &shared,
                                                        uint32_t partsBefore, uint32_t partsNow);

} // namespace dw5xl::smooth

#endif
