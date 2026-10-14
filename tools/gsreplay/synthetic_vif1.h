#ifndef DW5XL_GSREPLAY_SYNTHETIC_VIF1_H
#define DW5XL_GSREPLAY_SYNTHETIC_VIF1_H

// DW5XL-TEST: a small synthetic VIF1 capture for the replay harness test.
//
// Kick 1 (tick 100): MPG uploads a 3-pair VU1 program at micro address 0:
//     pair 0: XGKICK vi00            | NOP
//     pair 1: NOP                    | NOP [E]
//     pair 2: NOP                    | NOP      (E-bit delay slot)
//   UNPACK V4-32 places a GIF PACKED sprite packet (RGBAQ, XYZ2, XYZ2;
//   colour A) at VU1 data qword 0, MSCAL 0 runs the program (PATH1 sprite A),
//   then DIRECT sends sprite B (colour B) over PATH2.
// Kick 2 (tick 101): STROW/STMOD UNPACK (exercises row/mode), MSCAL again
//   (sprite A once more), and a DIRECT with an A+D packet writing
//   XYOFFSET_1 followed by sprite C, so the expected GS stream is
//   A, B, A, C (C offset by XYOFFSET_1).
// A VIF1_MARK register write between the kicks exercises VifReg records.

#include "vif1_capture.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace SyntheticVif1
{
    struct Sprite
    {
        uint8_t r, g, b, a;
        uint16_t x0, y0, x1, y1; // pixel coordinates (converted to 12.4)
        uint32_t z;
    };

    inline constexpr Sprite kSpriteA{0x10, 0x20, 0x30, 0x80, 10, 20, 50, 60, 0x1234};
    inline constexpr Sprite kSpriteB{0x40, 0x50, 0x60, 0x7F, 100, 110, 140, 150, 0x5678};
    inline constexpr Sprite kSpriteC{0x70, 0x08, 0x09, 0x40, 200, 210, 230, 240, 0x9ABC};
    inline constexpr uint16_t kXyOffsetX = 32; // pixels, written to XYOFFSET_1 before sprite C
    inline constexpr uint16_t kXyOffsetY = 16;

    inline void put32(std::vector<uint8_t> &out, uint32_t value)
    {
        const size_t offset = out.size();
        out.resize(offset + 4u);
        std::memcpy(out.data() + offset, &value, 4u);
    }
    inline void put64(std::vector<uint8_t> &out, uint64_t value)
    {
        put32(out, static_cast<uint32_t>(value));
        put32(out, static_cast<uint32_t>(value >> 32));
    }
    inline void vifcode(std::vector<uint8_t> &out, uint8_t cmd, uint8_t num, uint16_t imm)
    {
        put32(out, (static_cast<uint32_t>(cmd) << 24) | (static_cast<uint32_t>(num) << 16) | imm);
    }
    // Pads the stream with VIF NOPs so the next word starts on a quadword
    // boundary relative to the start of the kick (DIRECT payloads are QW aligned).
    inline void alignQw(std::vector<uint8_t> &out, size_t extraWordsBeforePayload = 0)
    {
        while (((out.size() / 4u) + extraWordsBeforePayload) % 4u != 0u)
            vifcode(out, 0x00, 0, 0);
    }

    // GIF PACKED sprite: tag (NLOOP=1 EOP PRE PRIM=SPRITE NREG=3) + RGBAQ + XYZ2 + XYZ2.
    inline std::vector<uint8_t> spritePacket(const Sprite &s, bool eop = true)
    {
        std::vector<uint8_t> p;
        const uint64_t tagLo = 1ull | (static_cast<uint64_t>(eop) << 15) | (1ull << 46) | (6ull << 47) |
                               (0ull << 58) | (3ull << 60);
        const uint64_t regs = 0x1ull | (0x5ull << 4) | (0x5ull << 8); // RGBAQ, XYZ2, XYZ2
        put64(p, tagLo);
        put64(p, regs);
        put32(p, s.r); put32(p, s.g); put32(p, s.b); put32(p, s.a);
        put32(p, static_cast<uint32_t>(s.x0) << 4); put32(p, static_cast<uint32_t>(s.y0) << 4); put32(p, s.z); put32(p, 0);
        put32(p, static_cast<uint32_t>(s.x1) << 4); put32(p, static_cast<uint32_t>(s.y1) << 4); put32(p, s.z); put32(p, 0);
        return p;
    }

    // GIF PACKED A+D packet setting XYOFFSET_1 (12.4 fixed point), not EOP.
    inline std::vector<uint8_t> xyoffsetPacket()
    {
        std::vector<uint8_t> p;
        put64(p, 1ull | (1ull << 60)); // NLOOP=1 NREG=1, no EOP
        put64(p, 0xEull);              // A+D
        put64(p, (static_cast<uint64_t>(kXyOffsetX) << 4) | (static_cast<uint64_t>(kXyOffsetY) << 4 << 32));
        put64(p, 0x18ull); // XYOFFSET_1
        return p;
    }

    inline std::vector<uint8_t> microprogram()
    {
        std::vector<uint8_t> code;
        auto pair = [&](uint32_t lower, uint32_t upper)
        {
            put32(code, lower);
            put32(code, upper);
        };
        constexpr uint32_t kLowerNop = 0x8000033Cu;
        constexpr uint32_t kUpperNop = 0x000002FFu;
        constexpr uint32_t kXgkickVi00 = 0x800006FCu; // lower special 0x6C, is = vi00
        pair(kXgkickVi00, kUpperNop);
        pair(kLowerNop, kUpperNop | 0x40000000u); // E-bit
        pair(kLowerNop, kUpperNop);
        return code;
    }

    inline void appendDirect(std::vector<uint8_t> &kick, const std::vector<uint8_t> &gif)
    {
        alignQw(kick, 1); // DIRECT word + payload must end QW aligned
        vifcode(kick, 0x50, 0, static_cast<uint16_t>(gif.size() / 16u));
        kick.insert(kick.end(), gif.begin(), gif.end());
    }

    inline std::vector<uint8_t> kick1()
    {
        std::vector<uint8_t> k;
        vifcode(k, 0x01, 0, 0x0404); // STCYCL cl=4 wl=4
        const auto code = microprogram();
        alignQw(k, 1); // MPG payload: keep data QW-aligned like real chains
        vifcode(k, 0x4A, static_cast<uint8_t>(code.size() / 8u), 0);
        k.insert(k.end(), code.begin(), code.end());
        const auto gif = spritePacket(kSpriteA);
        vifcode(k, 0x6C, static_cast<uint8_t>(gif.size() / 16u), 0x0000); // UNPACK V4-32 to qword 0
        k.insert(k.end(), gif.begin(), gif.end());
        vifcode(k, 0x14, 0, 0); // MSCAL 0
        appendDirect(k, spritePacket(kSpriteB));
        return k;
    }

    inline std::vector<uint8_t> kick2()
    {
        std::vector<uint8_t> k;
        vifcode(k, 0x05, 0, 0); // STMOD 0 (plain)
        vifcode(k, 0x30, 0, 0); // STROW
        for (uint32_t i = 0; i < 4; ++i)
            put32(k, 0x1000u + i);
        // Scratch UNPACK S-32 to qword 32 (not used by the program): row/mode path.
        vifcode(k, 0x60, 2, 32);
        put32(k, 7);
        put32(k, 9);
        vifcode(k, 0x14, 0, 0); // MSCAL 0 again -> sprite A
        auto gif = xyoffsetPacket();
        const auto sprite = spritePacket(kSpriteC);
        gif.insert(gif.end(), sprite.begin(), sprite.end());
        appendDirect(k, gif);
        return k;
    }

    inline Vif1Capture::StateSnapshot initialState()
    {
        Vif1Capture::StateSnapshot s;
        s.tick = 100;
        s.vu1Code.assign(16u * 1024u, 0u);
        s.vu1Data.assign(16u * 1024u, 0u);
        s.hasVu1State = true;
        s.vu1 = VU1State{};
        s.vu1.vf[0][3] = 1.0f;
        return s;
    }

    inline bool write(const std::string &path)
    {
        Vif1Capture::Writer writer;
        if (!writer.open(path))
            return false;
        writer.writeState(initialState());
        const auto k1 = kick1();
        writer.writeKick(100, 0, k1.data(), static_cast<uint32_t>(k1.size()));
        writer.writeVifReg(100, 0x10003C30u, 0x1234u); // VIF1_MARK
        const auto k2 = kick2();
        writer.writeKick(101, 0, k2.data(), static_cast<uint32_t>(k2.size()));
        writer.writeEnd(false);
        writer.close();
        return true;
    }
}

#endif
