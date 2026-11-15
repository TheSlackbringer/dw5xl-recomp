// Issue 049 Part 2 (amendment v3.4, P2.7): see dw5xl_smooth_gif.h.
#include "dw5xl_smooth_gif.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dw5xl::smooth {
namespace {

// GS registers whose values belong to a vertex.
constexpr uint8_t kRgbaq = 0x01, kSt = 0x02, kUv = 0x03, kXyzf2 = 0x04, kXyz2 = 0x05, kFog = 0x0A, kXyzf3 = 0x0C,
                  kXyz3 = 0x0D, kAd = 0x0E, kGifNop = 0x0F;

bool isPosition(uint8_t reg) { return reg == kXyzf2 || reg == kXyz2 || reg == kXyzf3 || reg == kXyz3; }
bool hasFog(uint8_t reg) { return reg == kXyzf2 || reg == kXyzf3; }
bool isTexture(uint8_t reg) { return reg == kSt || reg == kUv; }
bool isVertex(uint8_t reg) { return isPosition(reg) || isTexture(reg) || reg == kRgbaq || reg == kFog; }

uint16_t u16(const uint8_t *p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
uint32_t u32(const uint8_t *p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
uint64_t u64(const uint8_t *p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
float f32(const uint8_t *p) { float v; std::memcpy(&v, p, 4); return v; }
void put16(uint8_t *p, uint16_t v) { std::memcpy(p, &v, 2); }
void put32(uint8_t *p, uint32_t v) { std::memcpy(p, &v, 4); }
void putF(uint8_t *p, float v) { std::memcpy(p, &v, 4); }

// The bytes of a write that hold its value: a packed quadword, or the doubleword of the register's own layout (of
// an A+D quadword the address behind it is not part of the value).
uint32_t valueBytes(const GifWrite &w) { return w.form == GifWrite::Form::Packed ? 16u : 8u; }

struct Position
{
    uint32_t x = 0, y = 0, z = 0, fog = 0;
    bool noDraw = false; // the packed layout's ADC bit
};

Position position(const uint8_t *p, const GifWrite &w)
{
    Position out;
    out.x = u16(p);
    if (w.form == GifWrite::Form::Packed)
    {
        out.y = u16(p + 4);
        if (hasFog(w.reg))
        {
            out.z = (u32(p + 8) >> 4) & 0xFFFFFFu;
            out.fog = (u32(p + 12) >> 4) & 0xFFu;
        }
        else
        {
            out.z = u32(p + 8);
        }
        out.noDraw = (u32(p + 12) & 0x8000u) != 0u;
    }
    else
    {
        out.y = u16(p + 2);
        if (hasFog(w.reg))
        {
            out.z = u32(p + 4) & 0xFFFFFFu;
            out.fog = p[7];
        }
        else
        {
            out.z = u32(p + 4);
        }
    }
    return out;
}

uint32_t between(uint32_t a, uint32_t b, float t)
{
    const double value = static_cast<double>(a) + (static_cast<double>(b) - static_cast<double>(a)) * static_cast<double>(t);
    return static_cast<uint32_t>(std::llround(value));
}

float betweenF(float a, float b, float t) { return a + (b - a) * t; }

// One vertex write of `out` (a copy of the newer frame's part) at t between the two frames' values, 0 < t < 1.
void blendWrite(const uint8_t *before, uint8_t *out, const GifWrite &w, float t)
{
    const bool packed = w.form == GifWrite::Form::Packed;
    if (isPosition(w.reg))
    {
        const Position a = position(before, w), b = position(out, w);
        put16(out, static_cast<uint16_t>(between(a.x, b.x, t)));
        put16(out + (packed ? 4 : 2), static_cast<uint16_t>(between(a.y, b.y, t)));
        const uint32_t z = between(a.z, b.z, t), fog = between(a.fog, b.fog, t);
        if (packed && hasFog(w.reg))
        {
            put32(out + 8, (u32(out + 8) & ~0x0FFFFFF0u) | ((z & 0xFFFFFFu) << 4));
            put32(out + 12, (u32(out + 12) & ~0x00000FF0u) | ((fog & 0xFFu) << 4));
        }
        else if (packed)
        {
            put32(out + 8, z);
        }
        else if (hasFog(w.reg))
        {
            put32(out + 4, (z & 0xFFFFFFu) | ((fog & 0xFFu) << 24));
        }
        else
        {
            put32(out + 4, z);
        }
    }
    else if (w.reg == kRgbaq)
    {
        for (uint32_t c = 0; c < 4u; ++c)
        {
            const uint32_t at = packed ? c * 4u : c;
            out[at] = static_cast<uint8_t>(between(before[at], out[at], t));
        }
        if (!packed)
            putF(out + 4, betweenF(f32(before + 4), f32(out + 4), t)); // Q
    }
    else if (w.reg == kSt)
    {
        for (uint32_t c = 0; c < (packed ? 3u : 2u); ++c) // S, T, and Q in the packed layout
            putF(out + c * 4u, betweenF(f32(before + c * 4u), f32(out + c * 4u), t));
    }
    else if (w.reg == kUv)
    {
        put16(out, static_cast<uint16_t>(between(u16(before), u16(out), t)));
        const uint32_t v = packed ? 4u : 2u;
        put16(out + v, static_cast<uint16_t>(between(u16(before + v), u16(out + v), t)));
    }
    else if (w.reg == kFog)
    {
        if (packed)
            put32(out + 12, (u32(out + 12) & ~0x00000FF0u) | ((between((u32(before + 12) >> 4) & 0xFFu, (u32(out + 12) >> 4) & 0xFFu, t) & 0xFFu) << 4));
        else
            out[7] = static_cast<uint8_t>(between(before[7], out[7], t));
    }
}

// Calls `each(write, differs, blended)` for every vertex write of a pair, in order. `blended` is false for the
// texture coordinates of a vertex whose position is the same in both frames (the text rule) and for texture
// coordinates behind the part's last position.
template <typename Each>
void forVertexWrites(const uint8_t *before, const uint8_t *now, const GifPart &shape, Each each)
{
    std::vector<const GifWrite *> pending; // texture coordinates waiting for their vertex's position
    for (const GifWrite &w : shape.writes)
    {
        if (!isVertex(w.reg))
            continue;
        const bool differs = std::memcmp(before + w.offset, now + w.offset, valueBytes(w)) != 0;
        if (isTexture(w.reg))
        {
            pending.push_back(&w);
            continue;
        }
        if (isPosition(w.reg))
        {
            const Position a = position(before + w.offset, w), b = position(now + w.offset, w);
            const bool moved = a.x != b.x || a.y != b.y || a.z != b.z;
            for (const GifWrite *texture : pending)
                each(*texture, std::memcmp(before + texture->offset, now + texture->offset, valueBytes(*texture)) != 0, moved);
            pending.clear();
        }
        each(w, differs, true);
    }
    for (const GifWrite *texture : pending)
        each(*texture, std::memcmp(before + texture->offset, now + texture->offset, valueBytes(*texture)) != 0, false);
}

} // namespace

bool readGifPart(const uint8_t *part, size_t bytes, GifPart &out)
{
    out.shape.clear();
    out.writes.clear();
    out.images.clear();
    bool anyDirect = false;
    size_t at = 0;
    while (at + 4u <= bytes)
    {
        const uint32_t code = u32(part + at);
        const uint32_t cmd = (code >> 24) & 0x7Fu;
        out.shape.push_back(code);
        at += 4u;
        if (cmd == 0x00u || cmd == 0x07u || cmd == 0x10u || cmd == 0x11u || cmd == 0x13u) // NOP MARK FLUSHE FLUSH FLUSHA
            continue;
        if (cmd != 0x50u && cmd != 0x51u) // DIRECT, DIRECTHL
            return false;
        const size_t quadwords = (code & 0xFFFFu) ? (code & 0xFFFFu) : 65536u;
        const size_t end = at + quadwords * 16u;
        if (end > bytes)
            return false;
        anyDirect = true;
        while (at < end)
        {
            if (at + 16u > end)
                return false;
            const uint64_t lo = u64(part + at), hi = u64(part + at + 8u);
            out.shape.push_back(lo);
            out.shape.push_back(hi);
            at += 16u;
            const size_t loops = lo & 0x7FFFu;
            const uint32_t form = static_cast<uint32_t>((lo >> 58) & 3u);
            const size_t registers = ((lo >> 60) & 15u) ? ((lo >> 60) & 15u) : 16u;
            if (form == 0u) // packed: a quadword a register
            {
                if (at + loops * registers * 16u > end)
                    return false;
                for (size_t i = 0; i < loops * registers; ++i, at += 16u)
                {
                    const uint8_t reg = static_cast<uint8_t>((hi >> (4u * (i % registers))) & 15u);
                    if (reg == kGifNop)
                        continue;
                    GifWrite w;
                    w.offset = static_cast<uint32_t>(at);
                    w.viaAd = reg == kAd;
                    w.reg = w.viaAd ? part[at + 8u] : reg;
                    w.form = w.viaAd ? GifWrite::Form::Direct : GifWrite::Form::Packed;
                    out.writes.push_back(w);
                }
            }
            else if (form == 1u) // a list: a doubleword a register, filled up to a quadword
            {
                const size_t size = (loops * registers * 8u + 15u) / 16u * 16u;
                if (at + size > end)
                    return false;
                for (size_t i = 0; i < loops * registers; ++i)
                {
                    const uint8_t reg = static_cast<uint8_t>((hi >> (4u * (i % registers))) & 15u);
                    if (reg == kAd || reg == kGifNop) // no register of a list
                        continue;
                    GifWrite w;
                    w.offset = static_cast<uint32_t>(at + i * 8u);
                    w.reg = reg;
                    w.form = GifWrite::Form::Direct;
                    out.writes.push_back(w);
                }
                at += size;
            }
            else // texture data
            {
                if (at + loops * 16u > end)
                    return false;
                if (loops)
                    out.images.emplace_back(static_cast<uint32_t>(at), static_cast<uint32_t>(loops * 16u));
                at += loops * 16u;
            }
        }
    }
    return anyDirect && at == bytes;
}

GifRefusal checkGifPair(const uint8_t *before, size_t beforeBytes, const GifPart &beforeShape, const uint8_t *now,
                        size_t nowBytes, const GifPart &shape, float limitPixels, GifPairStats *stats)
{
    if (stats)
        *stats = GifPairStats{};
    if (beforeBytes != nowBytes)
        return GifRefusal::Size;
    if (beforeShape.shape != shape.shape || beforeShape.writes.size() != shape.writes.size())
        return GifRefusal::Shape;
    // Everything that is no vertex value must be equal. The newer part with the frame before's vertex values put in
    // is the frame before's part, or something else differs: a state register, texture data, the register an A+D
    // write names.
    std::vector<uint8_t> probe(now, now + nowBytes);
    for (const GifWrite &w : shape.writes)
        if (isVertex(w.reg))
            std::memcpy(probe.data() + w.offset, before + w.offset, valueBytes(w));
    if (std::memcmp(probe.data(), before, nowBytes) != 0)
        return GifRefusal::State;
    float largest = 0.0f;
    for (const GifWrite &w : shape.writes)
    {
        if (!isPosition(w.reg))
            continue;
        const Position a = position(before + w.offset, w), b = position(now + w.offset, w);
        if (a.noDraw != b.noDraw)
            return GifRefusal::State;
        const float dx = std::fabs(static_cast<float>(a.x) - static_cast<float>(b.x)) / 16.0f;
        const float dy = std::fabs(static_cast<float>(a.y) - static_cast<float>(b.y)) / 16.0f;
        largest = std::max(largest, std::max(dx, dy));
    }
    if (stats)
    {
        stats->largestMove = largest;
        forVertexWrites(before, now, shape, [stats](const GifWrite &, bool differs, bool blended) {
            stats->differing += differs ? 1u : 0u;
            stats->held += differs && !blended ? 1u : 0u;
        });
    }
    return largest > limitPixels ? GifRefusal::Moved : GifRefusal::None;
}

void blendGifPair(const uint8_t *before, const uint8_t *now, size_t bytes, const GifPart &shape, float t, uint8_t *out)
{
    std::memcpy(out, now, bytes);
    if (t >= 1.0f)
        return;
    forVertexWrites(before, now, shape, [before, out, t](const GifWrite &w, bool differs, bool blended) {
        if (!differs || !blended)
            return;
        if (t <= 0.0f)
            std::memcpy(out + w.offset, before + w.offset, valueBytes(w));
        else
            blendWrite(before + w.offset, out + w.offset, w, t);
    });
}

std::vector<std::pair<uint32_t, uint32_t>> pairUnshared(const std::vector<std::pair<uint32_t, uint32_t>> &shared,
                                                        uint32_t partsBefore, uint32_t partsNow)
{
    std::vector<std::pair<uint32_t, uint32_t>> pairs;
    uint32_t before = 0u, now = 0u;
    const auto run = [&pairs, &before, &now](uint32_t beforeEnd, uint32_t nowEnd) {
        if (beforeEnd - before == nowEnd - now)
            for (uint32_t i = 0; before + i < beforeEnd; ++i)
                pairs.emplace_back(before + i, now + i);
    };
    for (const auto &[sharedBefore, sharedNow] : shared)
    {
        run(sharedBefore, sharedNow);
        before = sharedBefore + 1u;
        now = sharedNow + 1u;
    }
    run(partsBefore, partsNow);
    return pairs;
}

} // namespace dw5xl::smooth
