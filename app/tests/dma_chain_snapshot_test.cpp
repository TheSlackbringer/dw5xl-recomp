// DMA chain snapshot (issue 025 spec v8, guard for fix B): a VIF1 source chain kick must hand exactly the
// tag-data and payload bytes of the chain to the device, in chain order, and leave TADR/ASR0/ASR1/CHCR as the
// chain walk computes them. Every payload is a VIF DIRECT carrying one GIF A+D packet with its own value, so the
// GIF output shows which bytes arrived. Runs the legacy inline path, the synchronous device and the async device.
// The register values are the walk's current results (a regression guard, not a hardware reference).
#include "ps2_runtime.h"
#include "runtime/ps2_device_queue.h"

#include <cfenv>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x1000;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x1004;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 1;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[] = {nullptr};

extern thread_local int g_gifArbiterCurrentPath;

namespace
{
    constexpr uint32_t kD1 = 0x10009000u;
    constexpr uint32_t kSpr = 0x70000000u;
    constexpr uint32_t kChainTte = 0x145u;   // STR, TTE, chain mode, from memory
    constexpr uint32_t kChainNoTte = 0x105u; // STR, chain mode, from memory
    constexpr uint32_t kChainTie = 0x1C5u;   // as kChainTte plus TIE

    enum class Mode
    {
        Legacy,
        Sync,
        Async
    };

    const char *modeName(Mode mode)
    {
        return mode == Mode::Legacy ? "legacy" : (mode == Mode::Sync ? "sync" : "async");
    }

    void require(bool condition, const std::string &message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    enum TagId : uint32_t
    {
        REFE = 0,
        CNT = 1,
        NEXT = 2,
        REF = 3,
        REFS = 4,
        CALL = 5,
        RET = 6,
        END = 7
    };

    // One GIFtag PACKED A+D packet writing `value` to the unused register 0x4F (2 quadwords).
    std::vector<uint32_t> gifAd(uint64_t value)
    {
        const uint64_t tagLo = 1ull | (1ull << 15) | (1ull << 60);
        const uint64_t tagHi = 0xEull;
        return {static_cast<uint32_t>(tagLo), static_cast<uint32_t>(tagLo >> 32), static_cast<uint32_t>(tagHi),
                static_cast<uint32_t>(tagHi >> 32), static_cast<uint32_t>(value), static_cast<uint32_t>(value >> 32),
                0x4Fu, 0u};
    }

    // Payload segment: three VIF NOPs, DIRECT(2), the GIF packet (3 quadwords).
    std::vector<uint32_t> segment(uint64_t value)
    {
        std::vector<uint32_t> words = {0u, 0u, 0u, 0x50000002u};
        const auto packet = gifAd(value);
        words.insert(words.end(), packet.begin(), packet.end());
        return words;
    }

    struct Rig
    {
        explicit Rig(Mode m)
        {
            require(mem.initialize(), "memory initialization");
            arbiter.setProcessPacketFn([this](const uint8_t *data, uint32_t size)
                                       { if (g_gifArbiterCurrentPath == 2) bytes.insert(bytes.end(), data, data + size); });
            mem.setGifArbiter(&arbiter);
            if (m != Mode::Legacy)
            {
                ps2x::device::Config config;
                config.async = m == Mode::Async;
                config.gsWriteRegister = [](uint8_t, uint64_t) {};
                config.controlWrite = [this](uint32_t address, uint32_t value)
                { mem.writeIORegister(address, value); };
                config.controlQwordWrite = [this](uint32_t address, uint64_t lo, uint64_t hi)
                {
                    const uint64_t words[2] = {lo, hi};
                    __m128i value;
                    std::memcpy(&value, words, sizeof(value));
                    mem.write128(address, value);
                };
                config.currentVu0Fbrst = []
                { return 0u; };
                config.applyVu1StopBits = [](uint32_t) {};
                config.callerRa = []
                { return 0u; };
                ps2x::device::attach(mem, std::move(config));
            }
        }
        ~Rig() { ps2x::device::detach(mem); }

        uint8_t *at(uint32_t addr)
        {
            return ps2IsScratchpadAddress(addr) ? mem.getScratchpad() + (addr & (PS2_SCRATCHPAD_SIZE - 1u))
                                                : mem.getRDRAM() + (addr & (PS2_RAM_SIZE - 1u));
        }
        void write(uint32_t addr, const std::vector<uint32_t> &words)
        {
            for (size_t i = 0; i < words.size(); ++i) // word by word: a payload may wrap at the RAM end
                std::memcpy(at(addr + static_cast<uint32_t>(i * 4u)), &words[i], 4u);
        }
        // DMAtag: qwc, id, irq, addr; upper 64 bits = two VIF codes (sent first when TTE is on).
        void tag(uint32_t at, uint32_t qwc, uint32_t id, uint32_t addr = 0u, bool irq = false,
                 uint32_t vif0 = 0u, uint32_t vif1 = 0u)
        {
            write(at, {qwc | (id << 28) | (irq ? 0x80000000u : 0u), addr, vif0, vif1});
        }

        // Kick a chain at `tadr` and return the values that reached the GIF, in order.
        std::vector<uint64_t> kick(uint32_t tadr, uint32_t chcr)
        {
            bytes.clear();
            mem.writeIORegister(kD1 + 0x20u, 0u);
            mem.writeIORegister(kD1 + 0x10u, 0u);
            mem.writeIORegister(kD1 + 0x30u, tadr);
            mem.writeIORegister(kD1 + 0x40u, 0u);
            mem.writeIORegister(kD1 + 0x50u, 0u);
            mem.writeIORegister(kD1, chcr);
            mem.processPendingTransfers();
            ps2x::device::drainAll(mem, ps2x::device::DrainReason::Explicit);
            require(bytes.size() % 32u == 0u, "GIF output is whole A+D packets");
            std::vector<uint64_t> values;
            for (size_t i = 0; i + 32u <= bytes.size(); i += 32u)
            {
                uint64_t v = 0;
                std::memcpy(&v, bytes.data() + i + 16u, 8u);
                values.push_back(v);
            }
            return values;
        }

        uint32_t reg(uint32_t offset) { return mem.m_ioRegisters[kD1 + offset]; }

        PS2Memory mem;
        GifArbiter arbiter;
        std::vector<uint8_t> bytes;
    };

    uint64_t val(uint32_t scenario, uint32_t index) { return 0xC0DE000000000000ull | (uint64_t(scenario) << 16) | index; }

    void expectValues(const std::vector<uint64_t> &got, const std::vector<uint64_t> &want, const std::string &what)
    {
        if (got == want)
            return;
        std::string msg = what + ": got " + std::to_string(got.size()) + " packets, want " + std::to_string(want.size());
        for (size_t i = 0; i < got.size() && i < want.size(); ++i)
            if (got[i] != want[i])
            {
                msg += "; first difference at packet " + std::to_string(i);
                break;
            }
        throw std::runtime_error(msg);
    }

    // tagUpper: DMAtag bits 16-31 (PCE, ID, IRQ), which the walk leaves in CHCR bits 16-31.
    void expectRegs(Rig &r, uint32_t tadr, uint32_t asr0, uint32_t asr1, uint32_t asp, uint32_t tagUpper,
                    const std::string &what)
    {
        require(r.reg(0x30u) == tadr, what + ": TADR " + std::to_string(r.reg(0x30u)) + " want " + std::to_string(tadr));
        require(r.reg(0x40u) == asr0, what + ": ASR0");
        require(r.reg(0x50u) == asr1, what + ": ASR1");
        require(((r.reg(0x00u) >> 4) & 3u) == asp, what + ": CHCR.ASP");
        require((r.reg(0x00u) >> 16) == tagUpper, what + ": CHCR tag bits");
    }

    // Tag kinds that carry their payload after the tag: CNT -> NEXT -> END.
    void following(Rig &r)
    {
        const uint32_t t0 = 0x100000u, t2 = 0x180000u;
        r.tag(t0, 3, CNT);
        r.write(t0 + 16u, segment(val(1, 0)));
        const uint32_t t1 = t0 + 16u + 48u;
        r.tag(t1, 3, NEXT, t2);
        r.write(t1 + 16u, segment(val(1, 1)));
        r.tag(t2, 3, END);
        r.write(t2 + 16u, segment(val(1, 2)));
        expectValues(r.kick(t0, kChainTte), {val(1, 0), val(1, 1), val(1, 2)}, "CNT/NEXT/END");
        expectRegs(r, t2, 0u, 0u, 0u, END << 12, "CNT/NEXT/END registers");
    }

    // Tag kinds whose payload is elsewhere: REF, REFS, then REFE (payload in scratchpad).
    void referenced(Rig &r)
    {
        const uint32_t t0 = 0x200000u, d0 = 0x280000u, d1 = 0x290000u, d2 = kSpr + 0x100u;
        r.tag(t0, 3, REF, d0);
        r.write(d0, segment(val(2, 0)));
        r.tag(t0 + 16u, 3, REFS, d1);
        r.write(d1, segment(val(2, 1)));
        r.tag(t0 + 32u, 3, REFE, d2);
        r.write(d2, segment(val(2, 2)));
        expectValues(r.kick(t0, kChainTte), {val(2, 0), val(2, 1), val(2, 2)}, "REF/REFS/REFE");
        expectRegs(r, t0 + 48u, 0u, 0u, 0u, REFE << 12, "REF/REFS/REFE registers");
    }

    // CALL two levels deep, RET twice, END after the outer CALL's payload.
    void callReturn(Rig &r)
    {
        const uint32_t main = 0x300000u, sub1 = 0x340000u, sub2 = 0x380000u;
        r.tag(main, 3, CALL, sub1);
        r.write(main + 16u, segment(val(3, 0)));
        const uint32_t after = main + 16u + 48u;
        r.tag(after, 3, END);
        r.write(after + 16u, segment(val(3, 5)));
        r.tag(sub1, 3, CALL, sub2);
        r.write(sub1 + 16u, segment(val(3, 1)));
        const uint32_t sub1ret = sub1 + 16u + 48u;
        r.tag(sub1ret, 3, RET);
        r.write(sub1ret + 16u, segment(val(3, 4)));
        r.tag(sub2, 3, CNT);
        r.write(sub2 + 16u, segment(val(3, 2)));
        r.tag(sub2 + 64u, 3, RET);
        r.write(sub2 + 80u, segment(val(3, 3)));
        expectValues(r.kick(main, kChainTte), {val(3, 0), val(3, 1), val(3, 2), val(3, 3), val(3, 4), val(3, 5)},
                     "CALL/RET");
        expectRegs(r, after, after, sub1ret, 0u, END << 12, "CALL/RET registers");
    }

    // TTE off: the tag's upper half is not sent.
    void noTte(Rig &r)
    {
        const uint32_t t0 = 0x400000u;
        r.tag(t0, 3, END, 0u, false, 0x50000002u, 0u); // a DIRECT here must NOT reach the VIF
        r.write(t0 + 16u, segment(val(4, 0)));
        expectValues(r.kick(t0, kChainNoTte), {val(4, 0)}, "TTE off");
    }

    // TTE on with a DIRECT in the tag's upper half: the payload is just the GIF packet.
    void tteDirect(Rig &r)
    {
        const uint32_t t0 = 0x480000u;
        r.tag(t0, 2, END, 0u, false, 0u, 0x50000002u);
        r.write(t0 + 16u, gifAd(val(5, 0)));
        expectValues(r.kick(t0, kChainTte), {val(5, 0)}, "TTE with DIRECT in the tag");
    }

    // A REF payload that wraps from the last RAM quadword to address 0.
    void ramWrap(Rig &r)
    {
        const uint32_t t0 = 0x500000u, d0 = PS2_RAM_SIZE - 16u;
        r.tag(t0, 3, REFE, d0);
        r.write(d0, segment(val(6, 0)));
        expectValues(r.kick(t0, kChainTte), {val(6, 0)}, "payload across the RAM end");
    }

    // IRQ bit with TIE: the chain stops after that tag's payload; TADR points at the next tag.
    void irqStop(Rig &r)
    {
        const uint32_t t0 = 0x580000u;
        r.tag(t0, 3, CNT, 0u, true);
        r.write(t0 + 16u, segment(val(7, 0)));
        const uint32_t next = t0 + 64u;
        r.tag(next, 3, END);
        r.write(next + 16u, segment(val(7, 1)));
        expectValues(r.kick(t0, kChainTie), {val(7, 0)}, "IRQ + TIE stop");
        expectRegs(r, next, 0u, 0u, 0u, (CNT << 12) | 0x8000u, "IRQ + TIE registers");
    }

    // A long chain, then a short one: the short one must deliver only its own bytes (buffer reuse guard).
    void longThenShort(Rig &r)
    {
        const uint32_t t0 = 0x600000u, count = 400u;
        std::vector<uint64_t> want;
        uint32_t at = t0;
        for (uint32_t i = 0; i < count; ++i)
        {
            r.tag(at, 3, i + 1u == count ? END : CNT);
            r.write(at + 16u, segment(val(8, i)));
            want.push_back(val(8, i));
            at += 64u;
        }
        expectValues(r.kick(t0, kChainTte), want, "long chain");
        const uint32_t s0 = 0x700000u;
        r.tag(s0, 3, END);
        r.write(s0 + 16u, segment(val(9, 0)));
        expectValues(r.kick(s0, kChainTte), {val(9, 0)}, "short chain after a long one");
        expectValues(r.kick(t0, kChainTte), want, "long chain again");
    }

    void runAll(Mode mode)
    {
        Rig r(mode);
        following(r);
        referenced(r);
        callReturn(r);
        noTte(r);
        tteDirect(r);
        ramWrap(r);
        irqStop(r);
        longThenShort(r);
    }
}

int main()
{
    int failures = 0;
    for (Mode mode : {Mode::Legacy, Mode::Sync, Mode::Async})
    {
        try
        {
            runAll(mode);
            std::cout << "PASS dma_chain_snapshot " << modeName(mode) << "\n";
        }
        catch (const std::exception &e)
        {
            ++failures;
            std::cout << "FAIL dma_chain_snapshot " << modeName(mode) << ": " << e.what() << "\n";
        }
    }
    return failures ? 1 : 0;
}
