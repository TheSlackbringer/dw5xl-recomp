// Issue 009 v4.5: the translated test program (ioplift_test_program.h, translated by dw5xl_ioplift at build time)
// against the IOP interpreter (IopCpuCore): from many random states and start addresses, a native run of up to N
// instructions must leave exactly the registers, load/branch delay state, pc, memory and written-byte map that the
// interpreter leaves after the same number of instructions, and must never run an instruction the interpreter would
// fault on.
#include "ioplift_test_program.h"

#include "ps2x/iop/iop_native.h"
#include "emulator/core/iop_cpu.h"
#include "emulator/core/iop_memory.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

uint32_t iopn_run_IOPTEST(ps2x::iop::IopNativeFrame &f);

namespace
{
    using namespace ps2x::iop;
    using namespace ps2x::iop::detail;
    using ioplift_test::kCodeBase;
    using ioplift_test::kDataBase;
    using ioplift_test::kDataSize;

    struct World
    {
        std::unique_ptr<IopMemory> interpreted = std::make_unique<IopMemory>();
        std::unique_ptr<IopMemory> native = std::make_unique<IopMemory>();
        std::vector<uint8_t> code;
    };

    void restore(World &w, ioplift_test::Rng &r)
    {
        std::vector<uint8_t> data(kDataSize);
        for (uint8_t &b : data)
            b = static_cast<uint8_t>(r.next());
        for (IopMemory *m : {w.interpreted.get(), w.native.get()})
        {
            std::memcpy(m->ramData() + kCodeBase, w.code.data(), w.code.size());
            std::memcpy(m->ramData() + kDataBase, data.data(), data.size());
            std::memset(m->ownedData() + kCodeBase, 0, w.code.size());
            std::memset(m->ownedData() + kDataBase, 0, data.size());
        }
    }

    IopCpuState randomState(ioplift_test::Rng &r, uint32_t words, uint32_t start)
    {
        IopCpuState s{};
        for (uint32_t i = 1; i < 32; ++i)
        {
            const uint32_t kind = r.below(4u);
            s.gpr[i] = kind == 0 ? r.next() : kind == 1 ? r.below(16u) : kind == 2 ? 0x80000000u - r.below(4u) : 0xFFFFFFFFu - r.below(4u);
        }
        for (uint32_t i = 8; i < 16; ++i)
            s.gpr[i] = kDataBase + 0x4000u + (r.below(0x8000u) & ~3u);
        s.gpr[20] = kCodeBase + r.below(words) * 4u;
        s.gpr[21] = kCodeBase + r.below(words) * 4u;
        s.hi = r.next();
        s.lo = r.next();
        if (r.below(3u) == 0u)
        {
            s.pendingLoad = true;
            s.pendingLoadReg = 1u + r.below(31u);
            s.pendingLoadValue = r.next();
        }
        s.branchTarget = r.below(2u) ? 0u : kCodeBase + r.below(words) * 4u;
        s.pc = kCodeBase + start * 4u;
        return s;
    }

    // Hardware callbacks for the native run: the native side's own IopMemory, as the runtime does it.
    struct HwHost
    {
        IopMemory *memory = nullptr;
        std::vector<uint32_t> addresses;
        std::vector<uint32_t> executed; // what the run said it had carried out before each access
    };

    uint32_t hwRead(void *host, uint32_t address, uint32_t size, uint32_t executed)
    {
        HwHost &h = *static_cast<HwHost *>(host);
        h.addresses.push_back(address);
        h.executed.push_back(executed);
        return size == 1u ? h.memory->read8(address) : size == 2u ? h.memory->read16(address) : h.memory->read32(address);
    }

    void hwWrite(void *host, uint32_t address, uint32_t size, uint32_t value, uint32_t executed)
    {
        HwHost &h = *static_cast<HwHost *>(host);
        h.addresses.push_back(address);
        h.executed.push_back(executed);
        if (size == 1u)
            h.memory->write8(address, static_cast<uint8_t>(value));
        else if (size == 2u)
            h.memory->write16(address, static_cast<uint16_t>(value));
        else
            h.memory->write32(address, value);
    }

    std::string compare(const IopCpuState &a, const IopNativeFrame &f, const World &w)
    {
        for (uint32_t i = 0; i < 32; ++i)
            if (a.gpr[i] != f.gpr[i])
                return "gpr " + std::to_string(i);
        if (a.hi != f.hi || a.lo != f.lo)
            return "hi/lo";
        if (a.pc != f.pc)
            return "pc";
        if (a.pendingLoad != f.pendingLoad || (a.pendingLoad && (a.pendingLoadReg != f.pendingLoadReg || a.pendingLoadValue != f.pendingLoadValue)))
            return "pending load";
        if (a.branchPending != f.branchPending || a.branchTarget != f.branchTarget)
            return "branch state";
        if (a.exception)
            return "the interpreter faulted inside the run";
        const uint8_t *ra = w.interpreted->ramData(), *rb = w.native->ramData();
        const uint8_t *oa = w.interpreted->ownedData(), *ob = w.native->ownedData();
        if (std::memcmp(ra + kCodeBase, rb + kCodeBase, w.code.size()) || std::memcmp(ra + kDataBase, rb + kDataBase, kDataSize))
            return "memory";
        if (std::memcmp(oa + kCodeBase, ob + kCodeBase, w.code.size()) || std::memcmp(oa + kDataBase, ob + kDataBase, kDataSize))
            return "written-byte map";
        return {};
    }
}

int main()
{
    World w;
    const std::vector<uint32_t> program = ioplift_test::program();
    for (const uint32_t word : program)
        for (int i = 0; i < 4; ++i)
            w.code.push_back(static_cast<uint8_t>(word >> (8 * i)));
    const uint32_t words = static_cast<uint32_t>(program.size());
    w.native->markNativeCode(kCodeBase, static_cast<uint32_t>(w.code.size()), true);
    IopCpuCore core(*w.interpreted);
    ioplift_test::Rng r{12345u};

    uint64_t cases = 0, failures = 0, instructions = 0, zeroRuns = 0, codeWrites = 0, delayStops = 0, hardware = 0;
    auto runCase = [&](uint32_t start, uint32_t budget, bool withHardware)
    {
        HwHost host{w.native.get(), {}, {}};
        restore(w, r);
        const IopCpuState initial = randomState(r, words, start);
        IopNativeFrame f{};
        std::memcpy(f.gpr, initial.gpr.data(), sizeof(f.gpr));
        f.hi = initial.hi;
        f.lo = initial.lo;
        f.pc = initial.pc;
        f.pendingLoad = initial.pendingLoad;
        f.pendingLoadReg = initial.pendingLoadReg;
        f.pendingLoadValue = initial.pendingLoadValue;
        f.branchTarget = initial.branchTarget;
        f.budget = budget;
        f.ram = w.native->ramData();
        f.owned = w.native->ownedData();
        f.codeWords = w.native->nativeCodeWords();
        if (withHardware)
        {
            f.hwRead = &hwRead;
            f.hwWrite = &hwWrite;
            f.host = &host;
        }
        const uint32_t ran = iopn_run_IOPTEST(f);
        IopCpuState s = initial;
        for (uint32_t i = 0; i < ran && !s.exception; ++i)
            (void)core.executeInstruction(s);
        ++cases;
        instructions += ran;
        zeroRuns += ran == 0u;
        codeWrites += f.codeWritten;
        delayStops += f.branchPending;
        std::string problem = ran > budget ? std::string("ran past its budget") : compare(s, f, w);
        hardware += host.addresses.size();
        if (problem.empty() && host.addresses.size() > 1u)
            problem = "more than one hardware access in a run";
        for (const uint32_t address : host.addresses)
            if (problem.empty() && w.interpreted->read32(address & ~3u) != w.native->read32(address & ~3u))
                problem = "hardware state";
        if (problem.empty() && !host.addresses.empty() && !f.ended)
            problem = "the run did not end after a hardware access";
        if (problem.empty() && !host.executed.empty() && host.executed[0] + 1u != ran)
            problem = "the access was reported at the wrong instruction count";
        if (!problem.empty() && ++failures <= 20u)
            std::fprintf(stderr, "FAIL start=0x%05x budget=%u ran=%u: %s\n", initial.pc, budget, ran, problem.c_str());
    };

    for (const bool withHardware : {false, true})
    {
        for (uint32_t start = 0; start < words; ++start)
            for (const uint32_t budget : {1u, 2u, 3u})
                runCase(start, budget, withHardware);
        for (int i = 0; i < 30000; ++i)
            runCase(r.below(words), 1u + r.below(400u), withHardware);
    }

    std::printf("ioplift: %llu cases, %llu native instructions (%llu runs stopped at once, %llu between a branch and its "
                "delay slot, %llu stores into code, %llu hardware accesses), %llu failures\n",
                static_cast<unsigned long long>(cases), static_cast<unsigned long long>(instructions),
                static_cast<unsigned long long>(zeroRuns), static_cast<unsigned long long>(delayStops),
                static_cast<unsigned long long>(codeWrites), static_cast<unsigned long long>(hardware),
                static_cast<unsigned long long>(failures));
    if (instructions == 0u || codeWrites == 0u || delayStops == 0u || hardware == 0u)
    {
        std::fprintf(stderr, "FAIL: the cases did not cover native runs, stores into code, delay-slot stops and hardware\n");
        return 1;
    }
    return failures == 0u ? 0 : 1;
}
