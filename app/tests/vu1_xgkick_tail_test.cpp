// XGKICK tail (issue 025 spec v9, guard for C3a): an XGKICK still copying when a VU1 run ends is finished by
// flushPipelines. The packet bytes, the complete VU state afterwards (all registers incl. Q/P, flags, PC) and VU
// memory, and the final cycle count must stay exactly as the stepped implementation produced them. The in-game
// verifier cannot check this on its own: its interpreter shadow ends through the same tail (Codex review
// 2026-10-23). Authored microcode and GIF data only; no game code or data.
//   - packets without a store: the delivered bytes also equal the VU data they were copied from;
//   - every value is compared with one recorded from the stepped implementation on 2026-10-23 (golden;
//     DW5XL_PRINT_GOLDEN=1 prints the current values);
//   - cases: a multi-tag packet (PACKED, REGLIST padded, IMAGE + EOP) right after XGKICK, wrapping the end of VU data,
//     after an even and an odd instruction runway (both values of the transfer credit at the tail); a store into the
//     packet in the run's last pair; a DIV whose Q result commits during the tail (the one-step copy splits around
//     it); one-tag packets of 63, 64 and 65 payload quadwords (the one-step copy works in steps of 64).
#include "ps2_runtime.h"
#include "runtime/ps2_vu1.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x1000;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x1004;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 1;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[] = {nullptr};

namespace
{
    constexpr uint32_t kLowerNop = 0x8000033Cu, kUpperNop = 0x000002FFu, kEbit = 1u << 30;
    constexpr uint32_t kDataSize = 16384u;

    void require(bool ok, const std::string &what)
    {
        if (!ok)
            throw std::runtime_error(what);
    }

    struct Fnv
    {
        uint64_t h = 1469598103934665603ull;
        void add(const void *data, size_t size)
        {
            const auto *p = static_cast<const uint8_t *>(data);
            for (size_t i = 0; i < size; ++i)
                h = (h ^ p[i]) * 1099511628211ull;
        }
        template <typename T> void field(const T &v) { add(&v, sizeof(v)); }
    };

    // Every architectural field of the VU state, field by field (no struct padding).
    uint64_t stateHash(const VU1State &s)
    {
        Fnv f;
        f.add(s.vf, sizeof(s.vf));
        f.add(s.vi, sizeof(s.vi));
        f.add(s.acc, sizeof(s.acc));
        f.field(s.q); f.field(s.p); f.field(s.i); f.field(s.r); f.field(s.pc);
        f.field(s.mac); f.field(s.clip); f.field(s.status); f.field(s.cycles);
        f.field(s.ebit); f.field(s.haltAfterDelaySlot); f.field(s.dBitEnabled); f.field(s.tBitEnabled);
        f.field(s.stoppedByD); f.field(s.stoppedByT); f.field(s.top); f.field(s.itop);
        f.field(s.branchPending); f.field(s.branchTarget); f.field(s.branchDelay);
        return f.h;
    }

    std::vector<uint8_t> tagged(uint64_t nloop, bool eop, uint64_t flg, uint64_t nreg, uint64_t regs)
    {
        const uint64_t lo = nloop | (eop ? 1ull << 15 : 0ull) | (flg << 58) | (nreg << 60);
        std::vector<uint8_t> p(16);
        std::memcpy(p.data(), &lo, 8);
        std::memcpy(p.data() + 8, &regs, 8);
        return p;
    }

    void fill(std::vector<uint8_t> &p, size_t bytes, uint8_t seed)
    {
        for (size_t i = 0; i < bytes; ++i)
            p.push_back(static_cast<uint8_t>(seed + i * 7u));
    }

    // PACKED A+D x40 (no EOP), REGLIST 1 reg x13 (odd count: padded to 14 slots), IMAGE x50 with EOP.
    std::vector<uint8_t> multiTagPacket()
    {
        std::vector<uint8_t> p = tagged(40, false, 0, 1, 0xE);
        fill(p, 40 * 16, 0x11);
        auto t2 = tagged(13, false, 1, 1, 0x1);
        p.insert(p.end(), t2.begin(), t2.end());
        fill(p, 14 * 8, 0x22); // 13 registers -> 14 eight-byte slots (qword padded)
        auto t3 = tagged(50, true, 2, 0, 0);
        p.insert(p.end(), t3.begin(), t3.end());
        fill(p, 50 * 16, 0x33);
        return p;
    }

    // One IMAGE tag with EOP and `qwords` payload quadwords.
    std::vector<uint8_t> imagePacket(uint32_t qwords)
    {
        std::vector<uint8_t> p = tagged(qwords, true, 2, 0, 0);
        fill(p, qwords * 16u, static_cast<uint8_t>(qwords));
        return p;
    }

    struct Result
    {
        std::vector<uint8_t> packet;
        uint64_t cycles = 0, state = 0, memory = 0;
    };

    // pc 0: XGKICK vi1; E bit at pc `endPc`; `extraLower` (when set) in the E bit's delay slot, the last pair of
    // the run: a store there lands one cycle later, a DIV commits its Q during the tail.
    Result run(uint32_t packetQword, uint32_t endPc, uint32_t extraLower, const std::vector<uint8_t> &packet,
               const float *storeValue)
    {
        PS2Runtime rt;
        require(rt.memory().initialize(), "memory initialization");
        auto &mem = rt.memory();
        std::vector<uint32_t> code(4096);
        for (size_t i = 0; i < code.size(); i += 2)
        {
            code[i] = kLowerNop;
            code[i + 1] = kUpperNop;
        }
        code[0] = 0x800006FCu | (1u << 11); // XGKICK vi1
        code[endPc / 4 + 1] |= kEbit;
        if (extraLower)
            code[(endPc + 8u) / 4u] = extraLower;
        std::memcpy(mem.getVU1Code(), code.data(), 16384);
        uint8_t *data = mem.getVU1Data();
        std::memset(data, 0, kDataSize);
        for (size_t i = 0; i < packet.size(); ++i)
            data[(packetQword * 16u + i) % kDataSize] = packet[i];
        auto &s = rt.vu1().state();
        s.vi[1] = static_cast<int32_t>(packetQword);
        if (storeValue)
            std::memcpy(s.vf[1], storeValue, 16);
        s.vf[2][0] = 3.0f; // DIV operands: Q = 0.75, distinct from Q's start value
        s.vf[3][0] = 4.0f;
        Result r;
        GifArbiter arbiter;
        arbiter.setProcessPacketFn([&](const uint8_t *bytes, uint32_t size) { r.packet.insert(r.packet.end(), bytes, bytes + size); });
        mem.setGifArbiter(&arbiter);
        rt.vu1().execute(mem.getVU1Code(), 16384, data, kDataSize, rt.gs(), &mem, 0, 0, 0, 100000);
        r.cycles = s.cycles;
        r.state = stateHash(s);
        Fnv m;
        m.add(data, kDataSize);
        r.memory = m.h;
        mem.setGifArbiter(nullptr);
        return r;
    }

    uint64_t hashBytes(const std::vector<uint8_t> &bytes)
    {
        Fnv f;
        f.add(bytes.data(), bytes.size());
        return f.h;
    }

    struct Golden
    {
        uint64_t cycles, packet, state, memory;
        size_t bytes;
    };
}

int main()
{
    const bool print = std::getenv("DW5XL_PRINT_GOLDEN") != nullptr;
    const auto multi = multiTagPacket();
    const float store[4] = {1.5f, -2.25f, 3.0f, 4.0f};
    const uint32_t divQ = 0x800003BCu | (3u << 16) | (2u << 11); // DIV Q, vf2.x, vf3.x = 3/4 (7-cycle result)
    const uint32_t sqIntoPacket = (1u << 25) | (15u << 21) | (1u << 11) | (16u + 20u); // SQ.xyzw vf1, 36(vi0)
    const auto img63 = imagePacket(63), img64 = imagePacket(64), img65 = imagePacket(65);
    struct Case
    {
        const char *name;
        uint32_t qword, endPc, extraLower;
        const std::vector<uint8_t> *packet;
        const float *store;
    } cases[] = {
        {"multi-tag packet, E right after XGKICK", 16u, 8u, 0u, &multi, nullptr},
        {"multi-tag packet wrapping the end of VU data", 0x3F0u, 8u, 0u, &multi, nullptr},
        {"multi-tag packet, 40 instruction runway before E", 16u, 320u, 0u, &multi, nullptr},
        {"multi-tag packet, 41 instruction runway before E (other credit)", 16u, 328u, 0u, &multi, nullptr},
        {"store into the packet in the last pair of the run", 16u, 8u, sqIntoPacket, &multi, store},
        {"DIV result commits during the tail", 16u, 8u, divQ, &multi, nullptr},
        {"one tag with 63 payload quadwords", 16u, 8u, 0u, &img63, nullptr},
        {"one tag with 64 payload quadwords", 16u, 8u, 0u, &img64, nullptr},
        {"one tag with 65 payload quadwords", 16u, 8u, 0u, &img65, nullptr},
    };
    // Recorded from the stepped implementation (2026-10-23): cycles, packet, state, memory, packet bytes.
    const Golden golden[] = {
        {199u, 0x9a8b22d85d27515full, 0x2b3e86fc92a60037ull, 0x747237eddf2e5a5full, 1600u}, // multi-tag packet, E right after XGKICK
        {199u, 0x9a8b22d85d27515full, 0x5e281b15026738a0ull, 0xdb75d18e4dad93f7ull, 1600u}, // multi-tag packet wrapping the end of VU data
        {199u, 0x9a8b22d85d27515full, 0x9d1c5e5345b5c3c6ull, 0x747237eddf2e5a5full, 1600u}, // multi-tag packet, 40 instruction runway before E
        {199u, 0x9a8b22d85d27515full, 0xaadfc0e8400af5eeull, 0x747237eddf2e5a5full, 1600u}, // multi-tag packet, 41 instruction runway before E (other credit)
        {199u, 0xa5326013a9687292ull, 0x48ec19c1c37c9854ull, 0xe3872fd2a77f7492ull, 1600u}, // store into the packet in the last pair of the run
        {199u, 0x9a8b22d85d27515full, 0xb736d1d07914d6f7ull, 0x747237eddf2e5a5full, 1600u}, // DIV result commits during the tail
        {127u, 0x50a8cec7c1ee3594ull, 0xe98a006778c98b7full, 0xc945ba4c4fd7b194ull, 1024u}, // one tag with 63 payload quadwords
        {129u, 0xebd7880251b7029bull, 0xb600bfd4487d1b81ull, 0x236873c27f9a73dbull, 1040u}, // one tag with 64 payload quadwords
        {131u, 0x85ee7c4344a2ee8aull, 0x4d9a075eb2d8f58bull, 0xf38f9cc0f6e1ed8aull, 1056u}, // one tag with 65 payload quadwords
    };
    static_assert(std::size(golden) == std::size(cases));
    int failures = 0;
    for (size_t i = 0; i < std::size(cases); ++i)
    {
        const auto &c = cases[i];
        try
        {
            const Result r = run(c.qword, c.endPc, c.extraLower, *c.packet, c.store);
            if (print)
            {
                std::cout << std::hex << "        {" << std::dec << r.cycles << "u, 0x" << std::hex << hashBytes(r.packet)
                          << "ull, 0x" << r.state << "ull, 0x" << r.memory << "ull, " << std::dec << r.packet.size()
                          << "u}, // " << c.name << "\n";
                continue;
            }
            const Golden &g = golden[i];
            require(r.packet.size() == g.bytes, "packet length " + std::to_string(r.packet.size()));
            if (!c.store)
                require(r.packet == *c.packet, "packet bytes differ from the VU data they were copied from");
            else
                require(r.packet != *c.packet, "the store must reach the packet (test premise)");
            require(r.cycles == g.cycles, "final cycle " + std::to_string(r.cycles) + ", recorded " + std::to_string(g.cycles));
            require(hashBytes(r.packet) == g.packet, "packet differs from the recorded one");
            require(r.state == g.state, "VU state (registers, Q/P, flags, PC) differs from the recorded one");
            require(r.memory == g.memory, "VU memory differs from the recorded one");
            std::cout << "PASS vu1_xgkick_tail " << c.name << "\n";
        }
        catch (const std::exception &e)
        {
            ++failures;
            std::cout << "FAIL vu1_xgkick_tail " << c.name << ": " << e.what() << "\n";
        }
    }
    return failures ? 1 : 0;
}
