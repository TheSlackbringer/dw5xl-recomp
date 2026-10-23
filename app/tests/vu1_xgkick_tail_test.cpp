// XGKICK tail (issue 025 spec v9, guard for C3a): an XGKICK still copying when a VU1 run ends is finished by
// flushPipelines. The packet bytes, the moment a store lands relative to the copy, and the final VU cycle count
// must stay exactly as they are. Authored microcode and GIF data only; no game code or data.
//   - packets without a store: the delivered bytes equal the VU data they were copied from (multi-tag packet with
//     PACKED, REGLIST and IMAGE tags; the same packet wrapping around the end of VU data memory);
//   - a store into the packet issued in the last pair of the run (it lands before the tail is copied: stores commit
//     one cycle after issue, before the run returns), and every final cycle count: values recorded from the stepped
//     implementation on 2026-10-23 (golden; DW5XL_PRINT_GOLDEN=1 prints the current values);
//   - a DIV whose result commits during the tail, so the one-step copy must split around that commit.
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

    uint64_t fnv(const std::vector<uint8_t> &bytes)
    {
        uint64_t h = 1469598103934665603ull;
        for (uint8_t b : bytes)
            h = (h ^ b) * 1099511628211ull;
        return h;
    }

    // GIF packet: PACKED A+D x40 (no EOP), REGLIST 1 reg x13 (odd count: padded to 14 slots), IMAGE x50 with EOP.
    std::vector<uint8_t> makePacket()
    {
        std::vector<uint8_t> p;
        auto tag = [&](uint64_t nloop, bool eop, uint64_t flg, uint64_t nreg, uint64_t regs)
        {
            const uint64_t lo = nloop | (eop ? 1ull << 15 : 0ull) | (flg << 58) | (nreg << 60);
            p.insert(p.end(), reinterpret_cast<const uint8_t *>(&lo), reinterpret_cast<const uint8_t *>(&lo) + 8);
            p.insert(p.end(), reinterpret_cast<const uint8_t *>(&regs), reinterpret_cast<const uint8_t *>(&regs) + 8);
        };
        auto filler = [&](size_t bytes, uint8_t seed)
        {
            for (size_t i = 0; i < bytes; ++i)
                p.push_back(static_cast<uint8_t>(seed + i * 7u));
        };
        tag(40, false, 0, 1, 0xE);
        filler(40 * 16, 0x11);
        tag(13, false, 1, 1, 0x1);
        filler(14 * 8, 0x22); // 13 registers -> 14 eight-byte slots (qword padded)
        tag(50, true, 2, 0, 0);
        filler(50 * 16, 0x33);
        return p;
    }

    struct Result
    {
        std::vector<uint8_t> packet;
        uint64_t cycles = 0;
    };

    // pc 0: XGKICK vi1; E bit at pc `endPc`; `extraLower` (when set) in the E bit's delay slot, the last pair of
    // the run: a store there lands one cycle later, i.e. while flushPipelines copies the tail.
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
        if (extraLower)
            code[(endPc + 8u) / 4u] = extraLower;
        code[endPc / 4 + 1] |= kEbit;
        std::memcpy(mem.getVU1Code(), code.data(), 16384);
        uint8_t *data = mem.getVU1Data();
        std::memset(data, 0, kDataSize);
        for (size_t i = 0; i < packet.size(); ++i)
            data[(packetQword * 16u + i) % kDataSize] = packet[i];
        auto &s = rt.vu1().state();
        s.vi[1] = static_cast<uint16_t>(packetQword);
        if (storeValue)
            std::memcpy(s.vf[1], storeValue, 16);
        Result r;
        GifArbiter arbiter;
        arbiter.setProcessPacketFn([&](const uint8_t *bytes, uint32_t size) { r.packet.insert(r.packet.end(), bytes, bytes + size); });
        mem.setGifArbiter(&arbiter);
        rt.vu1().execute(mem.getVU1Code(), 16384, data, kDataSize, rt.gs(), &mem, 0, 0, 0, 100000);
        r.cycles = s.cycles;
        mem.setGifArbiter(nullptr);
        return r;
    }

    struct Golden
    {
        const char *name;
        uint64_t cycles;
        uint64_t hash; // 0: bytes checked against VU data instead
    };
}

int main()
{
    const bool print = std::getenv("DW5XL_PRINT_GOLDEN") != nullptr;
    const auto packet = makePacket();
    const float store[4] = {1.5f, -2.25f, 3.0f, 4.0f};
    const uint32_t divQ = 0x800003BCu | (3u << 23) | (3u << 21); // DIV Q, vf0.w, vf0.w (7-cycle result)
    const uint32_t sqIntoPacket = (1u << 25) | (15u << 21) | (1u << 11) | (16u + 20u); // SQ.xyzw vf1, 36(vi0): packet qword 20, inside the first payload
    struct Case
    {
        Golden g;
        uint32_t qword, endPc, extraLower;
        const float *store;
    } cases[] = {
        {{"multi-tag packet, E right after XGKICK", 0, 0}, 16u, 8u, 0u, nullptr},
        {{"multi-tag packet wrapping the end of VU data", 0, 0}, 0x3F0u, 8u, 0u, nullptr},
        {{"multi-tag packet, 40 instruction runway before E", 0, 0}, 16u, 320u, 0u, nullptr},
        {{"store into the packet in the last pair of the run", 0, 0}, 16u, 8u, sqIntoPacket, store},
        {{"DIV result commits during the tail", 0, 0}, 16u, 8u, divQ, nullptr},
    };
    // Recorded from the stepped implementation (2026-10-23).
    const Golden golden[] = {
        {"multi-tag packet, E right after XGKICK", 199u, 0x9a8b22d85d27515full},
        {"multi-tag packet wrapping the end of VU data", 199u, 0x9a8b22d85d27515full},
        {"multi-tag packet, 40 instruction runway before E", 199u, 0x9a8b22d85d27515full},
        {"store into the packet in the last pair of the run", 199u, 0xa5326013a9687292ull},
        {"DIV result commits during the tail", 199u, 0x9a8b22d85d27515full},
    };
    int failures = 0;
    for (size_t i = 0; i < std::size(cases); ++i)
    {
        const auto &c = cases[i];
        try
        {
            const Result r = run(c.qword, c.endPc, c.extraLower, packet, c.store);
            if (print)
            {
                std::cout << "{\"" << c.g.name << "\", " << r.cycles << "u, 0x" << std::hex << fnv(r.packet) << std::dec
                          << "ull}, // " << r.packet.size() << " bytes\n";
                continue;
            }
            require(r.packet.size() == packet.size(), "packet length " + std::to_string(r.packet.size()));
            if (!c.store)
                require(r.packet == packet, "packet bytes differ from the VU data they were copied from");
            require(r.cycles == golden[i].cycles, "final cycle " + std::to_string(r.cycles) + ", recorded " +
                                                      std::to_string(golden[i].cycles));
            if (c.store)
            {
                require(fnv(r.packet) == golden[i].hash, "packet with the store differs from the recorded one");
                require(r.packet != packet, "the store must reach the packet (test premise)");
            }
            std::cout << "PASS vu1_xgkick_tail " << c.g.name << "\n";
        }
        catch (const std::exception &e)
        {
            ++failures;
            std::cout << "FAIL vu1_xgkick_tail " << c.g.name << ": " << e.what() << "\n";
        }
    }
    return failures ? 1 : 0;
}
