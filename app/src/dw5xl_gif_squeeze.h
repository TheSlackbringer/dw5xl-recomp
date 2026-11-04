// Issue 047: squeezes the screen x coordinates of a GIF packet about the picture's centre, for the HUD in a widescreen
// picture. Pure functions on a copy of the packet: tested without the game (CTest gif_squeeze).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace dw5xl::gifsqueeze {
// The game's 2D drawing: GS x = 0x7000 + 16 * x, for x in 0..512. Below kHidden: parked off screen (x = 0).
constexpr uint32_t kLeft = 0x7000, kCentre = 0x8000, kRight = 0x9000, kHidden = 0x6000;

inline bool isCoordinate(uint32_t reg) { return reg == 0x4 || reg == 0x5 || reg == 0xC || reg == 0xD; }   // XYZ(F)2, XYZ(F)3
inline bool isScissor(uint32_t address) { return address == 0x40 || address == 0x41; }

// Calls coordinate(x field) and scissor(register) for every one in the packet, tagEnd() after each GIF tag's data.
// A tag running past qwc ends the walk.
template <class Coordinate, class Scissor, class TagEnd>
void forEach(uint8_t *packet, uint32_t qwc, Coordinate &&coordinate, Scissor &&scissor, TagEnd &&tagEnd)
{
    uint32_t at = 0;
    while (at < qwc) {
        if (at) tagEnd();
        uint64_t low, regs;
        std::memcpy(&low, packet + size_t(at) * 16, 8);
        std::memcpy(&regs, packet + size_t(at) * 16 + 8, 8);
        ++at;
        const uint32_t nloop = uint32_t(low & 0x7FFF), flg = uint32_t(low >> 58) & 3;
        const uint32_t nreg = uint32_t(low >> 60) ? uint32_t(low >> 60) : 16;
        if (flg == 0) {                                   // PACKED: one quadword per register
            for (uint32_t n = 0; n < nloop; ++n)
                for (uint32_t r = 0; r < nreg; ++r, ++at) {
                    if (at >= qwc) { tagEnd(); return; }
                    uint8_t *data = packet + size_t(at) * 16;
                    const uint32_t reg = uint32_t(regs >> (r * 4)) & 15;
                    if (isCoordinate(reg)) coordinate(data);
                    else if (reg == 0xE) {                // A+D: the register's address in the high half
                        if (isCoordinate(data[8])) coordinate(data);
                        else if (isScissor(data[8])) scissor(data);
                    }
                }
        } else if (flg == 1) {                            // REGLIST: two registers per quadword, padded to a whole one
            const uint64_t count = uint64_t(nloop) * nreg;
            for (uint64_t i = 0; i < count; ++i) {
                const uint64_t q = at + i / 2;
                if (q >= qwc) { tagEnd(); return; }
                if (isCoordinate(uint32_t(regs >> ((i % nreg) * 4)) & 15)) coordinate(packet + size_t(q) * 16 + (i % 2) * 8);
            }
            at += uint32_t((count + 1) / 2);
        } else {
            at += nloop;                                  // IMAGE: no coordinates
        }
    }
    if (at) tagEnd();
}

inline uint16_t squeezedX(uint16_t x, float anchor, float factor)
{
    const float scaled = anchor + (float(x) - anchor) * factor;
    return uint16_t(std::clamp(std::lround(scaled), 0L, 0xFFFFL));
}

// Scales a packet in place, keeping what it draws at its side of the picture: a packet wholly in the left half is scaled
// about the left edge, one in the right half about the right edge, so the HUD stays at the screen's corners; one across
// the middle about the centre. A tag whose draws reach both picture edges keeps its x: fades, full-screen effects and
// bands must cover the stretched picture too. False, with the packet unchanged, if nothing was scaled.
inline bool squeeze(uint8_t *packet, uint32_t qwc, float factor)
{
    std::vector<std::vector<uint8_t *>> tags(1);
    std::vector<uint8_t *> scissors;
    forEach(packet, qwc, [&](uint8_t *field) { tags.back().push_back(field); }, [&](uint8_t *reg) { scissors.push_back(reg); },
            [&] { tags.emplace_back(); });
    const auto readX = [](const uint8_t *field) { uint16_t x; std::memcpy(&x, field, 2); return uint32_t(x); };
    uint32_t lowest = 0xFFFF, highest = 0;
    for (auto &fields : tags) {
        uint32_t low = 0xFFFF, high = 0;
        for (const uint8_t *field : fields) {
            const uint32_t x = readX(field);
            if (x >= kHidden) { low = std::min(low, x); high = std::max(high, x); }
        }
        if (low <= kLeft && high >= kRight) { fields.clear(); continue; }   // whole picture
        lowest = std::min(lowest, low);
        highest = std::max(highest, high);
    }
    if (lowest > highest) return false;                   // nothing on screen to scale
    const uint32_t anchor = highest <= kCentre ? kLeft : lowest >= kCentre ? kRight : kCentre;
    for (auto &fields : tags)
        for (uint8_t *field : fields) {
            if (readX(field) < kHidden) continue;
            const uint16_t x = squeezedX(uint16_t(readX(field)), float(anchor), factor);
            std::memcpy(field, &x, 2);
        }
    const float pixelAnchor = (float(anchor) - float(kLeft)) / 16.0f;
    for (uint8_t *reg : scissors) {
        uint64_t value; std::memcpy(&value, reg, 8);
        const uint32_t x0 = uint32_t(value) & 0x7FF, x1 = uint32_t(value >> 16) & 0x7FF;
        if (x0 == 0 && x1 >= 511) continue;               // the whole frame stays the whole frame
        const auto scale = [&](uint32_t x) { return uint64_t(std::clamp(std::lround(pixelAnchor + (float(x) - pixelAnchor) * factor), 0L, 0x7FFL)); };
        value = (value & ~uint64_t(0x07FF07FF)) | scale(x0) | scale(x1) << 16;
        std::memcpy(reg, &value, 8);
    }
    return true;
}
} // namespace dw5xl::gifsqueeze
