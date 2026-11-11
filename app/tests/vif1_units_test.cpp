// DW5XL-TEST issue 049, amendment v3.7 (P2.6b-1): VIF1 and VU1 as one implementation with more than one instance.
// The VIF1 interpreter works on a unit object (Vif1Unit: registers, VU1 memory, a cut-off DIRECT) and reaches outside
// it only through its host; a VU1 that is not the runtime memory object's own says through VU1Interpreter::Host which
// program memory it runs and where XGKICK's packets go. Authored VIF codes, microcode and GIF data only; no game data.
// One made-up stream in two transfers (set-up codes, STMASK/STROW/STCOL, MPG, a plain and a masked UNPACK with an
// offset mode, MSKPATH3, MSCAL of a program that XGKICKs a packet, a DIRECT, MARK, MSCNT, a DIRECT cut off at the
// transfer's end and continued in the next one, an UNPACK with skipping write cycles) is run
//   1. through the game's unit (PS2Memory::processVIF1Data with the runtime's VU1): the reference;
//   2. through a unit of its own with a VU1 of its own: registers, VU1 memory, VU1 registers and every GIF packet
//      byte must equal the reference;
//   3. through two units of their own on two threads at once, many times: each time equal to the reference;
//   4. through the game's unit on one thread and a unit of its own on another at once: each equal to the reference.
// What it does not prove: translated (lifted) VU1 programs on a second instance (the made-up program is interpreted).
#include "ps2_runtime.h"
#include "runtime/ps2_vif1_unit.h"
#include "runtime/ps2_vu1.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x1000;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x1004;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 1;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[] = {nullptr};

namespace
{
constexpr uint32_t kLowerNop = 0x8000033Cu, kUpperNop = 0x000002FFu, kEbit = 1u << 30;
int g_failures = 0;

void check(bool ok, const std::string &what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    g_failures += ok ? 0 : 1;
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

uint64_t vuStateHash(const VU1State &s)
{
    Fnv f;
    f.add(s.vf, sizeof(s.vf));
    f.add(s.vi, sizeof(s.vi));
    f.add(s.acc, sizeof(s.acc));
    f.field(s.q); f.field(s.p); f.field(s.i); f.field(s.r); f.field(s.pc);
    f.field(s.mac); f.field(s.clip); f.field(s.status); f.field(s.cycles);
    f.field(s.ebit); f.field(s.top); f.field(s.itop);
    return f.h;
}

struct Stream
{
    std::vector<uint32_t> words;
    void code(uint32_t opcode, uint32_t num, uint32_t imm) { words.push_back((opcode << 24) | (num << 16) | imm); }
    void word(uint32_t w) { words.push_back(w); }
    // NOPs until the next code is the last word of a quadword (as a DIRECT must be).
    void alignForDirect()
    {
        while (words.size() % 4u != 3u)
            words.push_back(0u);
    }
    void quad(uint64_t lo, uint64_t hi)
    {
        words.push_back(static_cast<uint32_t>(lo));
        words.push_back(static_cast<uint32_t>(lo >> 32));
        words.push_back(static_cast<uint32_t>(hi));
        words.push_back(static_cast<uint32_t>(hi >> 32));
    }
    const uint8_t *data() const { return reinterpret_cast<const uint8_t *>(words.data()); }
    uint32_t size() const { return static_cast<uint32_t>(words.size() * 4u); }
};

uint64_t gifTag(uint64_t nloop, bool eop, uint64_t flg, uint64_t nreg)
{
    return nloop | (eop ? 1ull << 15 : 0ull) | (flg << 58) | (nreg << 60);
}

// The two transfers of the made-up stream.
void buildStream(Stream &first, Stream &second)
{
    first.code(0x01, 0, 0x0101);                 // STCYCL cl 1, wl 1
    first.code(0x03, 0, 0x010);                  // BASE
    first.code(0x02, 0, 0x020);                  // OFFSET
    first.code(0x04, 0, 0x005);                  // ITOP
    first.code(0x20, 0, 0);                      // STMASK: every selection (data, row, column, protect) in turn
    first.word(0xE4E4E41Bu);
    first.code(0x30, 0, 0);                      // STROW
    for (uint32_t i = 0; i < 4u; ++i) first.word(0x1000u * (i + 1u));
    first.code(0x31, 0, 0);                      // STCOL
    for (uint32_t i = 0; i < 4u; ++i) first.word(0xC0000000u + i);
    first.code(0x4A, 6, 0);                      // MPG: 6 pairs at 0
    const uint32_t program[12] = {
        0x800006FCu | (1u << 11), kUpperNop,     // XGKICK vi1
        kLowerNop, kUpperNop | kEbit,            // end of the first run
        kLowerNop, kUpperNop,
        kLowerNop, kUpperNop | kEbit,            // end of the run MSCNT continues
        kLowerNop, kUpperNop,
        kLowerNop, kUpperNop};
    for (uint32_t w : program) first.word(w);
    first.code(0x6C, 4, 16);                     // UNPACK V4-32, 4 vectors at 16: the packet XGKICK sends
    first.quad(gifTag(3, true, 2, 0), 0u);       // IMAGE, 3 quadwords, EOP
    for (uint32_t i = 0; i < 12u; ++i) first.word(0x11110000u + i * 0x0101u);
    first.code(0x05, 0, 1);                      // STMOD: offset mode (data + ROW)
    first.code(0x7D, 3, 0x8000u | 4u);           // UNPACK V4-16 with the mask, 3 vectors at TOPS + 4
    for (uint32_t i = 0; i < 6u; ++i) first.word(0x7FFF0001u + i * 0x00030005u);
    first.code(0x05, 0, 0);                      // STMOD: back to plain
    first.code(0x06, 0, 0x8000);                 // MSKPATH3: masked
    first.code(0x14, 0, 0);                      // MSCAL 0
    first.code(0x06, 0, 0);                      // MSKPATH3: released
    first.alignForDirect();
    first.code(0x50, 0, 2);                      // DIRECT, 2 quadwords
    first.quad(gifTag(1, true, 0, 1), 0xEull);   // PACKED, A+D
    first.quad(0x0000000012345678ull, 0x3Full);  // TEXFLUSH
    first.code(0x07, 0, 0x77);                   // MARK
    first.code(0x17, 0, 0);                      // MSCNT
    first.alignForDirect();
    first.code(0x51, 0, 4);                      // DIRECTHL, 4 quadwords, of which 2 are in this transfer
    first.quad(gifTag(3, true, 2, 0), 0u);
    first.quad(0xAAAAAAAA00000001ull, 0xBBBBBBBB00000002ull);

    second.quad(0xCCCCCCCC00000003ull, 0xDDDDDDDD00000004ull);   // the cut-off DIRECTHL's other two quadwords
    second.quad(0xEEEEEEEE00000005ull, 0xFFFFFFFF00000006ull);
    second.code(0x01, 0, 0x0104);                // STCYCL cl 4, wl 1: skipping write
    second.code(0x64, 5, 40);                    // UNPACK V2-32, 5 vectors from 40, every fourth address
    for (uint32_t i = 0; i < 10u; ++i) second.word(0x22220000u + i);
    second.code(0x07, 0, 0x99);                  // MARK
}

struct Result
{
    std::vector<uint8_t> regs, gif;
    uint64_t code = 0, data = 0, vu = 0;
    uint32_t pendingQwc = 0, starts = 0;
    bool pendingHl = false, path3 = false;
    bool operator==(const Result &o) const
    {
        return regs == o.regs && gif == o.gif && code == o.code && data == o.data && vu == o.vu &&
               pendingQwc == o.pendingQwc && starts == o.starts && pendingHl == o.pendingHl && path3 == o.path3;
    }
};

uint64_t hashBytes(const uint8_t *bytes, size_t size)
{
    Fnv f;
    f.add(bytes, size);
    return f.h;
}

// The game's unit: the runtime's memory object and its VU1, with the VU1 starts wired as the runtime wires them.
struct Game
{
    PS2Runtime rt;
    GifArbiter arbiter;
    std::vector<uint8_t> gif;
    uint32_t starts = 0;
    Game()
    {
        if (!rt.memory().initialize())
            throw std::runtime_error("memory initialization");
        PS2Memory &mem = rt.memory();
        arbiter.setProcessPacketFn([this](const uint8_t *bytes, uint32_t size) { gif.insert(gif.end(), bytes, bytes + size); });
        mem.setGifArbiter(&arbiter);
        mem.setVu1MscalCallback([this](uint32_t startPC, uint32_t top, uint32_t itop) {
            ++starts;
            PS2Memory &m = rt.memory();
            rt.vu1().execute(m.getVU1Code(), PS2_VU1_CODE_SIZE, m.getVU1Data(), PS2_VU1_DATA_SIZE, rt.gs(), &m, startPC, top, itop, 65536);
        });
        mem.setVu1MscntCallback([this](uint32_t top, uint32_t itop) {
            ++starts;
            PS2Memory &m = rt.memory();
            rt.vu1().resume(m.getVU1Code(), PS2_VU1_CODE_SIZE, m.getVU1Data(), PS2_VU1_DATA_SIZE, rt.gs(), &m, top, itop, 65536);
        });
    }
    ~Game() { rt.memory().setGifArbiter(nullptr); }
    Result run(const Stream &first, const Stream &second)
    {
        PS2Memory &mem = rt.memory();
        std::memset(mem.getVU1Code(), 0, PS2_VU1_CODE_SIZE);
        std::memset(mem.getVU1Data(), 0, PS2_VU1_DATA_SIZE);
        std::memset(&mem.vif1_regs, 0, sizeof(mem.vif1_regs));
        gif.clear();
        starts = 0;
        rt.vu1().reset();
        rt.vu1().state().vi[1] = 16;
        mem.processVIF1Data(first.data(), first.size());
        Result r;
        r.pendingQwc = mem.m_vif1PendingPath2ImageQwc;      // the DIRECTHL was cut off: two quadwords to come
        r.pendingHl = mem.m_vif1PendingPath2DirectHl;
        mem.processVIF1Data(second.data(), second.size());
        r.regs.assign(reinterpret_cast<const uint8_t *>(&mem.vif1_regs),
                      reinterpret_cast<const uint8_t *>(&mem.vif1_regs) + sizeof(mem.vif1_regs));
        r.gif = gif;
        r.code = hashBytes(mem.getVU1Code(), PS2_VU1_CODE_SIZE);
        r.data = hashBytes(mem.getVU1Data(), PS2_VU1_DATA_SIZE);
        r.vu = vuStateHash(rt.vu1().state());
        r.starts = starts;
        r.path3 = mem.isPath3Masked();
        return r;
    }
};

// A unit of its own: registers and VU1 memory of its own, a VU1 of its own, its GIF packets collected here.
struct Own final : Vif1Unit::Host
{
    Vif1Unit unit;
    std::vector<uint8_t> code = std::vector<uint8_t>(PS2_VU1_CODE_SIZE), data = std::vector<uint8_t>(PS2_VU1_DATA_SIZE);
    std::atomic<uint64_t> generation{PS2Memory::nextCodeGeneration()};
    VU1Interpreter vu{VU1Interpreter::Unit::VU1};
    VU1Interpreter::Host vuHost;
    GS &gs;
    std::vector<uint8_t> gif;
    uint32_t starts = 0;
    bool path3 = false;

    explicit Own(GS &anyGs) : gs(anyGs)
    {
        unit.vu1Code = code.data();
        unit.vu1Data = data.data();
        vuHost.code = code.data();
        vuHost.generation = &generation;
        vuHost.gif = [this](const uint8_t *bytes, uint32_t size) { gif.insert(gif.end(), bytes, bytes + size); };
        vu.setHost(&vuHost);
    }
    void vif1Gif(const uint8_t *bytes, uint32_t size, bool) override { gif.insert(gif.end(), bytes, bytes + size); }
    void vif1Start(bool resume, uint32_t startPC, uint32_t top, uint32_t itop) override
    {
        ++starts;
        if (resume)
            vu.resume(unit.vu1Code, PS2_VU1_CODE_SIZE, unit.vu1Data, PS2_VU1_DATA_SIZE, gs, nullptr, top, itop, 65536);
        else
            vu.execute(unit.vu1Code, PS2_VU1_CODE_SIZE, unit.vu1Data, PS2_VU1_DATA_SIZE, gs, nullptr, startPC, top, itop, 65536);
    }
    void vif1Path3Mask(bool masked) override { path3 = masked; }
    void vif1CodeChanged() override { generation.store(PS2Memory::nextCodeGeneration(), std::memory_order_relaxed); }

    Result run(const Stream &first, const Stream &second)
    {
        std::memset(unit.vu1Code, 0, PS2_VU1_CODE_SIZE);
        std::memset(unit.vu1Data, 0, PS2_VU1_DATA_SIZE);
        unit.regs = VIFRegisters{};
        unit.pendingPath2ImageQwc = 0u;
        unit.pendingPath2DirectHl = false;
        generation.store(PS2Memory::nextCodeGeneration(), std::memory_order_relaxed);
        gif.clear();
        starts = 0;
        path3 = false;
        vu.reset();
        vu.state().vi[1] = 16;
        unit.process(*this, first.data(), first.size());
        Result r;
        r.pendingQwc = unit.pendingPath2ImageQwc;
        r.pendingHl = unit.pendingPath2DirectHl;
        unit.process(*this, second.data(), second.size());
        r.regs.assign(reinterpret_cast<const uint8_t *>(&unit.regs), reinterpret_cast<const uint8_t *>(&unit.regs) + sizeof(unit.regs));
        r.gif = gif;
        r.code = hashBytes(unit.vu1Code, PS2_VU1_CODE_SIZE);
        r.data = hashBytes(unit.vu1Data, PS2_VU1_DATA_SIZE);
        r.vu = vuStateHash(vu.state());
        r.starts = starts;
        r.path3 = path3;
        return r;
    }
};
} // namespace

int main()
{
#ifdef _WIN32
    _putenv_s("PS2X_GS_BACKEND", "cpu");
#else
    setenv("PS2X_GS_BACKEND", "cpu", 1);   // no GS work is done here; the VU1 calls only want a GS to name
#endif
    try
    {
        Stream first, second;
        buildStream(first, second);
        Game game;
        const Result reference = game.run(first, second);
        // The reference is what the stream should do, not just something repeatable.
        check(reference.starts == 2u, "the reference ran VU1 twice (MSCAL, MSCNT)");
        check(reference.pendingQwc == 2u && reference.pendingHl, "the reference kept the cut-off DIRECTHL's two quadwords for the next transfer");
        check(reference.gif.size() == (4u + 2u + 4u) * 16u, "the reference sent the XGKICK packet, the DIRECT and the whole DIRECTHL (160 bytes)");
        check(!reference.path3, "the reference released PATH3 again");
        check(game.run(first, second) == reference, "the game's unit gives the same result a second time");

        Own own(game.rt.gs());
        const Result alone = own.run(first, second);
        check(alone.regs == reference.regs, "a unit of its own: the VIF1 registers equal the game's unit's");
        check(alone.code == reference.code && alone.data == reference.data, "a unit of its own: VU1 program and data memory equal");
        check(alone.gif == reference.gif, "a unit of its own: every GIF packet byte equal (XGKICK, DIRECT, the continued DIRECTHL)");
        check(alone.vu == reference.vu && alone.starts == reference.starts, "a unit of its own: the VU1 registers and the count of VU1 starts equal");
        check(alone.pendingQwc == reference.pendingQwc && alone.pendingHl == reference.pendingHl && alone.path3 == reference.path3,
              "a unit of its own: the cut-off DIRECT and the PATH3 mask as the game's unit's");

        // Two units of their own on two threads at once.
        {
            Own a(game.rt.gs()), b(game.rt.gs());
            std::atomic<int> wrong{0};
            const auto many = [&](Own &unit) {
                for (int i = 0; i < 200; ++i)
                    if (!(unit.run(first, second) == reference))
                        wrong.fetch_add(1);
            };
            std::thread ta([&] { many(a); }), tb([&] { many(b); });
            ta.join();
            tb.join();
            check(wrong.load() == 0, "two units of their own on two threads at once, 200 times each: every result equals the reference");
        }

        // The game's unit on one thread and a unit of its own on another: what the pictures' device will be.
        {
            Own other(game.rt.gs());
            std::atomic<int> wrong{0};
            std::thread tg([&] {
                for (int i = 0; i < 100; ++i)
                    if (!(game.run(first, second) == reference))
                        wrong.fetch_add(1);
            });
            std::thread to([&] {
                for (int i = 0; i < 100; ++i)
                    if (!(other.run(first, second) == reference))
                        wrong.fetch_add(1);
            });
            tg.join();
            to.join();
            check(wrong.load() == 0, "the game's unit and a unit of its own on two threads at once, 100 times each: every result equals the reference");
        }
    }
    catch (const std::exception &error)
    {
        std::printf("FAIL  exception: %s\n", error.what());
        return 1;
    }
    std::printf("%s (%d failed)\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
