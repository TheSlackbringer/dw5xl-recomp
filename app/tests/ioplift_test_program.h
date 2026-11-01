#pragma once
// Issue 009 v4.5: the fixed R3000A program the IOP translator test translates at build time (dw5xl_ioplift --raw)
// and then runs native and interpreted from many random states. Our own code, no game data.
#include <algorithm>
#include <cstdint>
#include <iterator>
#include <vector>

namespace ioplift_test
{
    inline constexpr uint32_t kCodeBase = 0x00080000u;
    inline constexpr uint32_t kDataBase = 0x00100000u; // r8..r15 point into [kDataBase, kDataBase + kDataSize)
    inline constexpr uint32_t kDataSize = 0x00010000u;

    struct Rng
    {
        uint64_t state;
        uint32_t next()
        {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            return static_cast<uint32_t>(state >> 33u);
        }
        uint32_t below(uint32_t n) { return next() % n; }
    };

    inline uint32_t rType(uint32_t rs, uint32_t rt, uint32_t rd, uint32_t sa, uint32_t fn)
    {
        return (rs << 21u) | (rt << 16u) | (rd << 11u) | (sa << 6u) | fn;
    }
    inline uint32_t iType(uint32_t op, uint32_t rs, uint32_t rt, uint32_t imm)
    {
        return (op << 26u) | (rs << 21u) | (rt << 16u) | (imm & 0xFFFFu);
    }

    // Registers: r8-r15 data pointers, r20/r21 code pointers (JR/JALR); results go to the others.
    inline uint32_t destReg(Rng &r)
    {
        static constexpr uint32_t regs[] = {0, 1, 2, 3, 4, 5, 6, 7, 16, 17, 18, 19, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
        return regs[r.below(sizeof(regs) / sizeof(regs[0]))];
    }
    inline uint32_t anyReg(Rng &r) { return r.below(32u); }
    inline uint32_t baseReg(Rng &r) { return 8u + r.below(8u); }

    inline std::vector<uint32_t> program()
    {
        std::vector<uint32_t> w;
        Rng r{0x009A45ull};
        const uint32_t randomWords = 3000u;
        auto codeAddress = [&](uint32_t index) { return kCodeBase + index * 4u; };
        for (uint32_t i = 0; i < randomWords; ++i)
        {
            const uint32_t kind = r.below(100u);
            const uint32_t rs = anyReg(r), rt = anyReg(r), rd = destReg(r), sa = r.below(32u);
            const uint32_t imm = r.next() & 0xFFFFu;
            if (kind < 30)
            {
                static constexpr uint32_t fns[] = {0x00, 0x02, 0x03, 0x04, 0x06, 0x07, 0x10, 0x11, 0x12, 0x13, 0x18, 0x19,
                                                   0x1A, 0x1B, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2A, 0x2B};
                const uint32_t fn = fns[r.below(sizeof(fns) / sizeof(fns[0]))];
                w.push_back(rType(rs, rt, rd, (fn <= 0x07) ? sa : 0u, fn));
            }
            else if (kind < 45)
            {
                static constexpr uint32_t ops[] = {0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};
                const uint32_t op = ops[r.below(sizeof(ops) / sizeof(ops[0]))];
                w.push_back(iType(op, op == 0x0F ? 0u : rs, rd, imm));
            }
            else if (kind < 50)
            {
                const uint32_t b = baseReg(r);
                w.push_back(iType(0x09, b, b, (r.below(17u) - 8u) * 4u)); // move a data pointer a little
            }
            else if (kind < 68)
            {
                static constexpr uint32_t ops[] = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26};
                const uint32_t off = (r.below(9u) < 8u ? (r.below(32u) * 4u) : r.below(128u)) - 64u; // mostly aligned
                w.push_back(iType(ops[r.below(sizeof(ops) / sizeof(ops[0]))], baseReg(r), destReg(r), off));
            }
            else if (kind < 80)
            {
                static constexpr uint32_t ops[] = {0x28, 0x29, 0x2A, 0x2B, 0x2E};
                const uint32_t off = (r.below(9u) < 8u ? (r.below(32u) * 4u) : r.below(128u)) - 64u;
                w.push_back(iType(ops[r.below(sizeof(ops) / sizeof(ops[0]))], baseReg(r), anyReg(r), off));
            }
            else if (kind < 92)
            {
                // conditional branches, near targets inside the random part
                const int32_t delta = static_cast<int32_t>(r.below(41u)) - 20;
                const int32_t target = std::max<int32_t>(0, std::min<int32_t>(static_cast<int32_t>(randomWords) - 1, static_cast<int32_t>(i) + 1 + delta));
                const uint32_t offset = static_cast<uint32_t>(target - static_cast<int32_t>(i) - 1) & 0xFFFFu;
                switch (r.below(6u))
                {
                case 0: w.push_back(iType(0x04, rs, rt, offset)); break;
                case 1: w.push_back(iType(0x05, rs, rt, offset)); break;
                case 2: w.push_back(iType(0x06, rs, 0, offset)); break;
                case 3: w.push_back(iType(0x07, rs, 0, offset)); break;
                case 4: w.push_back(iType(0x01, rs, r.below(2u), offset)); break;
                default: w.push_back(iType(0x01, rs, 0x10u + r.below(2u), offset)); break; // BLTZAL/BGEZAL
                }
            }
            else if (kind < 96)
            {
                const uint32_t target = codeAddress(r.below(randomWords));
                w.push_back(((2u + r.below(2u)) << 26u) | ((target >> 2u) & 0x03FFFFFFu)); // J/JAL
            }
            else
                w.push_back(r.below(2u) ? rType(20u + r.below(2u), 0, 0, 0, 0x08) : rType(20u + r.below(2u), 0, destReg(r), 0, 0x09));
        }
        // Hand-written edge cases (the random part's branches stay inside it).
        const uint32_t edges[] = {
            0x8D020000u, // lw v0,0(t0)
            0x00421021u, // addu v0,v0,v0      load delay: sees the old v0
            0x8D030004u, // lw v1,4(t0)
            0x8D030008u, // lw v1,8(t0)        a load right after a load to the same register
            0x24030001u, // addiu v1,zero,1    overwrites the pending v1: the load is dropped
            0x8D04000Cu, // lw a0,12(t0)
            0x1000000Au, // beq zero,zero,+10  taken branch, delay slot is a load
            0x8D050010u, // lw a1,16(t0)
            0x0000000Cu, // syscall            (skipped by the branch)
            0x04110002u, // bgezal zero,+2     link written before the condition
            0x00000000u, // nop
            0x0000000Cu, // syscall            -> interpreter
            0x0000000Du, // break              -> interpreter
            0x07F00001u, // bltzal ra,+1       the condition reads the new ra
            0x00000000u,
            0x0280A009u, // jalr s4,s4         the target is read after the link (= pc + 8)
            0x00000000u,
            0x03E00008u, // jr ra
            0x24000005u, // addiu zero,zero,5  (an import stub pattern: the interpreter decodes it)
            0x10000002u, // beq zero,zero,+2
            0x10000001u, // beq in the delay slot -> interpreter
            0x00000000u,
            0x3C011F80u, // lui at,0x1f80
            0x8C221070u, // lw v0,0x1070(at)   hardware register -> interpreter
            0x3C017FFFu, // lui at,0x7fff
            0x3421FFFFu, // ori at,at,0xffff
            0x20210001u, // addi at,at,1       overflow -> interpreter
            0x8D020001u, // lw v0,1(t0)        misaligned -> interpreter
            0x3C010008u, // lui at,0x0008      at = code base
            0xAC200000u, // sw zero,0(at)      a store into translated code
            0x00000000u,
            0x40026000u, // mfc0 v0,$12        COP0 -> interpreter
            0x3C011F80u, // lui at,0x1f80
            0x8C220000u, // lw v0,0(at)        scratchpad read (hardware callback)
            0xA4220002u, // sh v0,2(at)        scratchpad write
            0x80230001u, // lb v1,1(at)        scratchpad byte
            0xAC201074u, // sw zero,0x1074(at) I_MASK write
            0x8C241074u, // lw a0,0x1074(at)   I_MASK read
            0x3C011F90u, // lui at,0x1f90
            0x94220000u, // lhu v0,0(at)       sound chip register read
            0x10000002u, // beq zero,zero,+2
            0x84250002u, // lh a1,2(at)        hardware read in a delay slot
            0x00000000u,
        };
        w.insert(w.end(), std::begin(edges), std::end(edges));
        for (int i = 0; i < 16; ++i)
            w.push_back(0x00000000u); // tail
        return w;
    }
}
