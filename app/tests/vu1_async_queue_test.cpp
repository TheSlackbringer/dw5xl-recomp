// Device command queue (VIF1 -> VU1 -> GIF -> GS) tests: legacy inline path,
// synchronous executor (PS2X_VU1_ASYNC=0) and async worker must produce the
// same GS packet stream (incl. per-packet path and FP rounding mode) and the same
// guest-visible DMAC/VIF1 state after a drain.
#include "ps2_runtime.h"
#include "runtime/ps2_device_queue.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <cfenv>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x1000;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x1004;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 1;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[] = {nullptr};

extern thread_local int g_gifArbiterCurrentPath;

namespace
{
    constexpr uint32_t kD1 = 0x10009000u;
    constexpr uint32_t kD2 = 0x1000A000u;
    constexpr uint32_t kDStat = 0x1000E010u;
    constexpr uint32_t kVif1Fbrst = 0x10003C10u;
    constexpr uint32_t kVif1Stat = 0x10003C00u;

    void require(bool condition, const std::string &message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

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

    struct Entry
    {
        int kind = 0; // GIF path 1..3, 100 = GS register write, 101 = GS call
        int rounding = 0;
        std::vector<uint8_t> bytes;
        bool operator==(const Entry &) const = default;
    };

    uint64_t fnv(const uint8_t *data, size_t size, uint64_t h = 1469598103934665603ull)
    {
        for (size_t i = 0; i < size; ++i)
            h = (h ^ data[i]) * 1099511628211ull;
        return h;
    }

    struct VifBuilder
    {
        std::vector<uint32_t> words;
        void nop() { words.push_back(0u); }
        void alignForPayload()
        {
            while ((words.size() + 1u) % 4u != 0u)
                nop();
        }
        void unpackV4(uint32_t vuAddr, const std::vector<uint32_t> &lanes)
        {
            const uint32_t vectors = static_cast<uint32_t>(lanes.size() / 4u);
            alignForPayload();
            words.push_back((0x6Cu << 24) | ((vectors & 0xFFu) << 16) | (vuAddr & 0x3FFu));
            words.insert(words.end(), lanes.begin(), lanes.end());
        }
        void mscal(uint32_t pc) { words.push_back((0x14u << 24) | ((pc / 8u) & 0xFFFFu)); }
        void mskpath3(bool mask) { words.push_back((0x06u << 24) | (mask ? 0x8000u : 0u)); }
        void mpg(uint32_t addr, const std::vector<uint32_t> &code) // code: pairs of words
        {
            const uint32_t instructions = static_cast<uint32_t>(code.size() / 2u);
            while ((words.size() + 1u) % 2u != 0u)
                nop();
            words.push_back((0x4Au << 24) | ((instructions & 0xFFu) << 16) | ((addr / 8u) & 0xFFFFu));
            words.insert(words.end(), code.begin(), code.end());
        }
        void direct(const std::vector<uint32_t> &qwords)
        {
            alignForPayload();
            words.push_back((0x50u << 24) | static_cast<uint32_t>(qwords.size() / 4u));
            words.insert(words.end(), qwords.begin(), qwords.end());
        }
        void finish()
        {
            while (words.size() % 4u != 0u)
                nop();
        }
    };

    // One GIFtag PACKED A+D packet carrying a single register write.
    std::vector<uint32_t> gifAd(uint8_t reg, uint64_t value)
    {
        const uint64_t tagLo = 1ull | (1ull << 15) | (1ull << 60);
        const uint64_t tagHi = 0xEull;
        return {static_cast<uint32_t>(tagLo), static_cast<uint32_t>(tagLo >> 32), static_cast<uint32_t>(tagHi),
                static_cast<uint32_t>(tagHi >> 32), static_cast<uint32_t>(value), static_cast<uint32_t>(value >> 32),
                reg, 0u};
    }

    struct Rig
    {
        explicit Rig(Mode m) : mode(m)
        {
            require(mem.initialize(), "memory initialization");
            arbiter.setProcessPacketFn([this](const uint8_t *data, uint32_t size)
                                       { stream.push_back({g_gifArbiterCurrentPath, std::fegetround(),
                                                           std::vector<uint8_t>(data, data + size)}); });
            mem.setGifArbiter(&arbiter);
            mem.setVu1MscalCallback([this](uint32_t pc, uint32_t top, uint32_t itop)
                                    { fakeVu1(pc, top, itop); });
            if (mode != Mode::Legacy)
            {
                ps2x::device::Config config;
                config.async = mode == Mode::Async;
                config.trace = ps2x::device::traceRequestedByEnv();
                config.gsWriteRegister = [this](uint8_t reg, uint64_t value)
                {
                    Entry e{100, std::fegetround(), std::vector<uint8_t>(9)};
                    e.bytes[0] = reg;
                    std::memcpy(e.bytes.data() + 1, &value, 8);
                    stream.push_back(std::move(e));
                };
                config.controlWrite = [this](uint32_t address, uint32_t value)
                {
                    require(ps2x::device::executingCommand(), "control write executes on device");
                    mem.writeIORegister(address, value);
                };
                config.controlQwordWrite = [this](uint32_t address, uint64_t lo, uint64_t hi)
                {
                    require(ps2x::device::executingCommand(), "control qword executes on device");
                    const uint64_t words[2] = {lo, hi};
                    __m128i value;
                    std::memcpy(&value, words, sizeof(value));
                    mem.write128(address, value);
                };
                config.currentVu0Fbrst = [this]
                { return eeFbrst; };
                config.applyVu1StopBits = [this](uint32_t bits)
                { eeVpuStat = (eeVpuStat & ~0x600u) | bits; };
                config.callerRa = []
                { return 0x12345678u; };
                ps2x::device::attach(mem, std::move(config));
            }
        }
        ~Rig() { ps2x::device::detach(mem); }

        // Stand-in for VU1: hashes the microcode at PC and the data at TOP and
        // XGKICKs one packet under round-toward-zero, like VU1Interpreter::run().
        void fakeVu1(uint32_t pc, uint32_t top, uint32_t itop)
        {
            if (vuOverride)
            {
                vuOverride(pc, top, itop);
                return;
            }
            if (slowVu1)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if (pc == throwAtPc)
                throw std::runtime_error("fake VU1 fault");
            // Like the runtime callback: never read EE-owned state on the worker.
            const uint32_t fbrst = ps2x::device::onDeviceWorker() ? ps2x::device::workerVu0Fbrst() : eeFbrst;
            uint64_t h = fnv(mem.getVU1Code() + (pc & 0x3FF8u), 64u);
            h = fnv(mem.getVU1Data() + (top & 0x3FCu) * 16u, 64u, h);
            h ^= (static_cast<uint64_t>(itop) << 48) ^ (static_cast<uint64_t>(fbrst) << 32);
            const auto packet = gifAd(0x4F, h); // reg 0x4F unused by the fake sink
            const int previous = std::fegetround();
            std::fesetround(FE_TOWARDZERO);
            mem.submitGifPacket(GifPathId::Path1, reinterpret_cast<const uint8_t *>(packet.data()), 32u);
            std::fesetround(previous);
            const uint32_t stopBits = (fbrst & 0x400u) ? 0x200u : 0u; // fake: D-enable -> D-stop
            if (ps2x::device::onDeviceWorker())
                ps2x::device::workerRecordVu1StopBits(stopBits);
            else
                eeVpuStat = (eeVpuStat & ~0x600u) | stopBits;
        }

        void writeRam(uint32_t addr, const std::vector<uint32_t> &words)
        {
            std::memcpy(mem.getRDRAM() + addr, words.data(), words.size() * 4u);
        }

        // VIF1 chain: one END tag (TTE: upper half = two VIF NOPs) + payload.
        void kickVif1Chain(uint32_t tagAddr, VifBuilder vif)
        {
            vif.finish();
            const uint32_t qwc = static_cast<uint32_t>(vif.words.size() / 4u);
            std::vector<uint32_t> tag = {qwc | (7u << 28), 0u, 0u, 0u};
            writeRam(tagAddr, tag);
            writeRam(tagAddr + 16u, vif.words);
            mem.writeIORegister(kD1 + 0x20u, 0u);
            mem.writeIORegister(kD1 + 0x10u, 0u);
            mem.writeIORegister(kD1 + 0x30u, tagAddr);
            mem.writeIORegister(kD1, 0x145u);
            mem.processPendingTransfers();
        }

        void kickVif1Normal(uint32_t addr, VifBuilder vif)
        {
            vif.finish();
            writeRam(addr, vif.words);
            mem.writeIORegister(kD1 + 0x20u, static_cast<uint32_t>(vif.words.size() / 4u));
            mem.writeIORegister(kD1 + 0x10u, addr);
            mem.writeIORegister(kD1, 0x101u);
        }

        void kickGifNormal(uint32_t addr, const std::vector<uint32_t> &qwords)
        {
            writeRam(addr, qwords);
            mem.writeIORegister(kD2 + 0x20u, static_cast<uint32_t>(qwords.size() / 4u));
            mem.writeIORegister(kD2 + 0x10u, addr);
            mem.writeIORegister(kD2, 0x101u);
        }

        void gsRegs(std::vector<ps2x::device::GsRegWrite> writes)
        {
            if (ps2x::device::submitGsRegWrites(mem, writes))
                return;
            for (const auto &w : writes) // legacy stub path: write the GS inline
            {
                Entry e{100, std::fegetround(), std::vector<uint8_t>(9)};
                e.bytes[0] = w.first;
                std::memcpy(e.bytes.data() + 1, &w.second, 8);
                stream.push_back(std::move(e));
            }
        }

        void gsCall(uint8_t marker)
        {
            auto fn = [this, marker]
            { stream.push_back({101, std::fegetround(), {marker}}); };
            if (!ps2x::device::submitGsCall(mem, fn))
                fn();
        }

        struct Observed
        {
            std::vector<Entry> stream;
            std::vector<uint32_t> regs;
            std::vector<uint32_t> causes;
            std::vector<uint8_t> vif1Regs;
            bool path3Masked = false;
            uint64_t vu1DataHash = 0;
            uint32_t vpuStat = 0;
            bool operator==(const Observed &) const = default;
        };

        // Guest-style observation: D_STAT read drains; then raw state.
        Observed observe()
        {
            Observed o;
            o.regs.push_back(mem.readIORegister(kDStat));
            for (uint32_t off : {0x00u, 0x10u, 0x20u, 0x30u})
            {
                o.regs.push_back(mem.m_ioRegisters[kD1 + off]);
                o.regs.push_back(mem.m_ioRegisters[kD2 + off]);
            }
            o.regs.push_back(mem.m_ioRegisters[0x10003020u]);
            o.causes = mem.consumeCompletedDmacCauses();
            o.vif1Regs.resize(sizeof(mem.vif1_regs));
            std::memcpy(o.vif1Regs.data(), &mem.vif1_regs, sizeof(mem.vif1_regs));
            o.path3Masked = mem.m_path3Masked;
            o.vu1DataHash = fnv(mem.getVU1Data(), PS2_VU1_DATA_SIZE);
            o.stream = stream;
            o.vpuStat = eeVpuStat;
            return o;
        }

        Mode mode;
        PS2Memory mem;
        GifArbiter arbiter;
        std::vector<Entry> stream;
        uint32_t eeFbrst = 0u;
        uint32_t eeVpuStat = 0u;
        std::function<void(uint32_t, uint32_t, uint32_t)> vuOverride;
        bool slowVu1 = false;
        uint32_t throwAtPc = 0xFFFFFFFFu;
    };

class WorkerGate {
public:
    void arriveAndWait() {
        std::unique_lock lock(mutex);
        entered = true;
        cv.notify_all();
        cv.wait(lock, [&] { return released; });
    }
    void waitEntered() {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return entered; });
    }
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        cv.notify_all();
    }
    ~WorkerGate() { release(); }
private:
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool released = false;
};

struct ReleaseBeforeDetach {
    std::vector<WorkerGate *> gates;
    ~ReleaseBeforeDetach() { for (auto *gate : gates) gate->release(); }
};

void emitMarker(Rig &rig, uint64_t value) {
    auto packet = gifAd(0x4f, value);
    rig.mem.submitGifPacket(GifPathId::Path1,
                           reinterpret_cast<const uint8_t *>(packet.data()),32u);
}

void testPauseLatchedReadAndStaleCompletion() {
    WorkerGate oldGate, newGate; // destruct after Rig has drained; explicit releases below
    Rig rig(Mode::Async);
    ReleaseBeforeDetach release{{&oldGate,&newGate}};
    rig.mem.writeIORegister(kDStat,0x00060000u);
    rig.vuOverride = [&](uint32_t pc,uint32_t,uint32_t) {
        if (pc == 0) {
            emitMarker(rig,0x1111);
            oldGate.arriveAndWait();
            require(!ps2x::device::workerShouldCancelVu1(), "DMA Pause preserves accepted VU execution");
            emitMarker(rig,0x1112);
        }
        else if (pc == 8) { newGate.arriveAndWait(); emitMarker(rig,0x2222); }
    };
    VifBuilder first; first.mscal(0);
    rig.kickVif1Normal(0x50000,first);
    oldGate.waitEntered();
    // This MUST return while worker is still blocked. A queue drain makes the
    // test deterministically deadlock; CTest TIMEOUT is the failure bound.
    const uint32_t before = rig.mem.readIORegister(kD1);
    require((before & 0x100u) != 0u,"active STR must be latched without waiting");
    uint32_t saved = 0;
    require(ps2x::device::pauseDmaChannel(rig.mem,kD1,saved),"pause attached channel");
    require(saved == before,"Pause returns original CHCR");
    require((rig.mem.readIORegister(kD1) & 0x100u) == 0u,"Pause clears STR immediately");
    VifBuilder next; next.mscal(8);
    rig.kickVif1Normal(0x51000,next); // must not drain old cancelled generation
    oldGate.release();
    newGate.waitEntered(); // old completion has retired; replacement is still active
    ps2x::device::pollCompletions(rig.mem);
    require((rig.mem.m_ioRegisters[kD1] & 0x100u) != 0u,"stale completion cannot clear new STR");
    require((rig.mem.m_ioRegisters[kDStat] & 2u) == 0u,"cancelled generation must not raise CIS1");
    require(rig.mem.consumeCompletedDmacCauses().empty(),"cancelled generation must not publish IRQ");
    newGate.release();
    ps2x::device::drainAll(rig.mem,ps2x::device::DrainReason::Explicit);
    const auto stats = ps2x::device::stats(rig.mem);
    require(stats.completed == stats.submitted && stats.applied == stats.submitted,
            "all cancelled/replacement sequences retire");
    require((rig.mem.m_ioRegisters[kD1] & 0x100u) == 0u,"new completion clears its STR");
    require(rig.mem.consumeCompletedDmacCauses() == std::vector<uint32_t>{1},
            "only replacement generation completes once");
    require(rig.stream.size() == 3u,"accepted VU finishes across Pause and replacement draw is emitted");
}

void testPauseRestartResumesUnacceptedTail() {
    WorkerGate gate;
    Rig rig(Mode::Async);
    ReleaseBeforeDetach release{{&gate}};
    std::vector<uint32_t> calls; // worker owned; read only after queue drain
    rig.vuOverride = [&](uint32_t pc,uint32_t,uint32_t) {
        calls.push_back(pc);
        emitMarker(rig,pc);
        if (pc == 0) gate.arriveAndWait();
    };
    VifBuilder stream; stream.mscal(0); stream.mscal(8); stream.mscal(16);
    rig.kickVif1Normal(0x50000,stream);
    gate.waitEntered();
    uint32_t saved=0;
    require(ps2x::device::pauseDmaChannel(rig.mem,kD1,saved),"tail Pause");
    gate.release();
    ps2x::device::drainAll(rig.mem,ps2x::device::DrainReason::Explicit);
    require(calls == std::vector<uint32_t>{0},"unaccepted VIF tail must not execute while paused");
    require(rig.stream.size()==1u,"already emitted PATH1 packet remains");
    uint32_t previous=~0u;
    require(ps2x::device::restartDmaChannel(rig.mem,kD1,saved,previous),"tail Restart");
    require(previous == 0u,"Restart returns previous STR bit");
    ps2x::device::drainAll(rig.mem,ps2x::device::DrainReason::Explicit);
    require(calls == std::vector<uint32_t>({0,8,16}),"Restart resumes exact tail without repeating accepted prefix");
    require(rig.stream.size()==3u,"each accepted packet emitted exactly once");
}

void testResetBeforeNewInputAndGifIndependence() {
    WorkerGate gate;
    Rig rig(Mode::Async);
    ReleaseBeforeDetach release{{&gate}};
    std::vector<uint32_t> calls;
    rig.vuOverride = [&](uint32_t pc,uint32_t,uint32_t) {
        calls.push_back(pc);
        if(pc==0) gate.arriveAndWait();
        if(pc==16) require(rig.mem.vif1_regs.mark==0x77u,"ordered reset then new MARK before MSCAL");
        emitMarker(rig,pc);
    };
    VifBuilder old; old.mscal(0); old.mscal(8);
    rig.kickVif1Normal(0x50000,old);
    gate.waitEntered();
    uint32_t saved=0;
    require(ps2x::device::pauseDmaChannel(rig.mem,kD1,saved),"reset Pause");
    require(ps2x::device::submitControlWrite(rig.mem,kVif1Fbrst,1u),"reset ordered on worker");
    rig.kickGifNormal(0x52000,gifAd(0x22,0xBEEF));
    VifBuilder fresh; fresh.words.push_back((0x07u<<24)|0x77u); fresh.mscal(16);
    rig.kickVif1Normal(0x53000,fresh);
    gate.release();
    ps2x::device::drainAll(rig.mem,ps2x::device::DrainReason::Explicit);
    require(calls==std::vector<uint32_t>({0,16}),"reset discards paused old VIF tail");
    require(rig.stream.size()==3u && rig.stream[0].kind==1 && rig.stream[1].kind==3 && rig.stream[2].kind==1,
            "VIF cancellation keeps independent GIF kick and queue order");
    const auto causes=rig.mem.consumeCompletedDmacCauses();
    require(causes==std::vector<uint32_t>({2,1}),"independent GIF and new VIF complete only once");
}

void testCancelledWorkerErrorStillSurfaces() {
    WorkerGate gate;
    Rig rig(Mode::Async);
    ReleaseBeforeDetach release{{&gate}};
    rig.vuOverride=[&](uint32_t,uint32_t,uint32_t) {
        gate.arriveAndWait();
        throw std::runtime_error("cancelled worker fault");
    };
    VifBuilder input;input.mscal(0);
    rig.kickVif1Normal(0x50000,input);
    gate.waitEntered();
    uint32_t saved=0;
    require(ps2x::device::pauseDmaChannel(rig.mem,kD1,saved),"error Pause");
    gate.release();
    bool thrown=false;
    try { ps2x::device::drainAll(rig.mem,ps2x::device::DrainReason::Explicit); }
    catch(const std::runtime_error &e) { thrown=std::string(e.what())=="cancelled worker fault"; }
    require(thrown,"cancellation must not hide an independent worker error");
    const auto stats=ps2x::device::stats(rig.mem);
    require(stats.completed==stats.submitted && stats.applied==stats.submitted,"faulted cancelled sequence retires");
    require(rig.mem.consumeCompletedDmacCauses().empty(),"faulted cancelled sequence has no completion IRQ");
}

void testVu1ResetRetainsEmittedDraws() {
    WorkerGate gate;
    Rig rig(Mode::Async);
    ReleaseBeforeDetach release{{&gate}};
    bool resetApplied=false; // worker-only until explicit drain
    rig.vuOverride=[&](uint32_t pc,uint32_t,uint32_t) {
        if(pc==0) {
            emitMarker(rig,0x1111);
            gate.arriveAndWait();
            // Stand-in safe pair boundary. Differential interpreter/lift fixtures
            // below must separately verify the real execution loops use it.
            if(!ps2x::device::workerShouldCancelVu1()) emitMarker(rig,0xBAD);
        } else {
            require(resetApplied,"VU reset applies before replacement program");
            emitMarker(rig,0x2222);
        }
    };
    VifBuilder old;old.mscal(0);
    rig.kickVif1Normal(0x50000,old);
    gate.waitEntered();
    require(ps2x::device::submitControlWrite(rig.mem,kVif1Fbrst,1u),"hard reset discards old VIF input");
    require(ps2x::device::submitVu1Reset(rig.mem,[&]{resetApplied=true;}),"queue hard VU reset");
    VifBuilder next;next.mscal(8);
    rig.kickVif1Normal(0x51000,next);
    gate.release();
    ps2x::device::drainAll(rig.mem,ps2x::device::DrainReason::Explicit);
    require(resetApplied,"ordered VU reset processed");
    require(rig.stream.size()==2u,"hard cancel retains emitted draw and suppresses later draw");
    const auto stats=ps2x::device::stats(rig.mem);
    require(stats.completed==stats.submitted && stats.applied==stats.submitted,
            "hard-reset old/new/control sequences retire");
}

void testPauseAtLastAcceptedCommandRestartsEmptyTail() {
    WorkerGate gate;
    Rig rig(Mode::Async);
    ReleaseBeforeDetach release{{&gate}};
    unsigned calls=0; // worker owned, inspect after drain only
    rig.vuOverride=[&](uint32_t,uint32_t,uint32_t) {
        ++calls;
        emitMarker(rig,0x1111);
        gate.arriveAndWait();
    };
    VifBuilder input;
    input.nop(); input.nop(); input.nop(); input.mscal(0); // last word: no NOP tail
    rig.kickVif1Normal(0x50000,input);
    gate.waitEntered();
    uint32_t saved=0;
    require(ps2x::device::pauseDmaChannel(rig.mem,kD1,saved),"last-command Pause");
    gate.release();
    ps2x::device::drainAll(rig.mem,ps2x::device::DrainReason::Explicit);
    require(calls==1u,"accepted final MSCAL completes once");
    require(rig.mem.consumeCompletedDmacCauses().empty(),"paused old generation cannot publish IRQ");
    uint32_t previous=~0u;
    require(ps2x::device::restartDmaChannel(rig.mem,kD1,saved,previous),"empty-tail Restart");
    ps2x::device::drainAll(rig.mem,ps2x::device::DrainReason::Explicit);
    require(calls==1u && rig.stream.size()==1u,"empty tail must not replay accepted final MSCAL");
    require((rig.mem.m_ioRegisters[kD1]&0x100u)==0u,"empty-tail Restart must complete STR");
    require(rig.mem.consumeCompletedDmacCauses()==std::vector<uint32_t>{1},"empty-tail Restart completes once");
}

    using Scenario = std::function<void(Rig &)>;

    Rig::Observed runScenario(Mode mode, const Scenario &scenario, bool slow = false)
    {
        Rig rig(mode);
        rig.slowVu1 = slow;
        scenario(rig);
        return rig.observe();
    }

    void requireSameAcrossModes(const char *name, const Scenario &scenario, bool slow = false)
    {
        const auto legacy = runScenario(Mode::Legacy, scenario, slow);
        for (Mode mode : {Mode::Sync, Mode::Async})
        {
            const auto other = runScenario(mode, scenario, slow);
            require(other.stream.size() == legacy.stream.size(),
                    std::string(name) + ": stream length differs in " + modeName(mode) + " (" +
                        std::to_string(other.stream.size()) + " vs " + std::to_string(legacy.stream.size()) + ")");
            for (size_t i = 0; i < legacy.stream.size(); ++i)
                require(other.stream[i] == legacy.stream[i], std::string(name) + ": GS entry " + std::to_string(i) +
                                                                 " differs in " + modeName(mode));
            require(other.regs == legacy.regs, std::string(name) + ": DMAC registers differ in " + modeName(mode));
            require(other.causes == legacy.causes, std::string(name) + ": DMAC causes differ in " + modeName(mode));
            require(other.vif1Regs == legacy.vif1Regs, std::string(name) + ": VIF1 registers differ in " + modeName(mode));
            require(other.path3Masked == legacy.path3Masked, std::string(name) + ": MSKPATH3 differs in " + modeName(mode));
            require(other.vu1DataHash == legacy.vu1DataHash, std::string(name) + ": VU1 data differs in " + modeName(mode));
            require(other.vpuStat == legacy.vpuStat, std::string(name) + ": VPU-STAT differs in " + modeName(mode));
        }
    }

    std::vector<uint32_t> lanes(uint32_t seed, uint32_t vectors)
    {
        std::vector<uint32_t> out(vectors * 4u);
        for (uint32_t i = 0; i < out.size(); ++i)
            out[i] = seed * 2654435761u + i * 40503u;
        return out;
    }

    void testOrdering()
    {
        // PATH3 and stub GS writes between two VIF1 streams, VU1 FBRST snapshot.
        const Scenario scenario = [](Rig &rig)
        {
            VifBuilder a;
            a.unpackV4(0, lanes(1, 4));
            a.mscal(0);
            a.direct(gifAd(0x10, 0xAAAAu));
            rig.eeFbrst = 0x400u; // VU1 D-bit enable at kick time
            rig.kickVif1Chain(0x10000u, a);
            const bool deferred = ps2x::device::hasDeferredChannel(rig.mem, kD1);
            require(deferred == (rig.mode == Mode::Async), "VIF1 completion deferral");
            if (rig.mode == Mode::Async)
                require((rig.mem.m_ioRegisters[kD1] & 0x100u) != 0u, "async kick keeps CHCR.STR until observed");
            rig.eeFbrst = 0u; // changed after the kick: VU1 must see the kick-time value
            rig.kickGifNormal(0x20000u, gifAd(0x20, 0xBBBBu));
            rig.gsRegs({{0x30, 0xCCCCu}, {0x31, 0xDDDDu}});
            rig.gsCall(7);
            VifBuilder b;
            b.unpackV4(16, lanes(2, 2));
            b.mscal(0);
            rig.kickVif1Normal(0x30000u, b);
        };
        requireSameAcrossModes("ordering", scenario, true);

        const auto observed = runScenario(Mode::Async, scenario, true);
        std::vector<int> kinds;
        for (const auto &e : observed.stream)
            kinds.push_back(e.kind);
        require(kinds == std::vector<int>({1, 2, 3, 100, 100, 101, 1}), "ordering: unexpected GS stream order");
        require(observed.stream[0].rounding == FE_TOWARDZERO && observed.stream[1].rounding == FE_TONEAREST &&
                    observed.stream[2].rounding == FE_TONEAREST,
                "ordering: per-path rounding mode");
        require(observed.vpuStat == 0u, "ordering: last MSCAL (FBRST 0) left no stop bits");
    }

    void testFbrstMidQueue()
    {
        const Scenario scenario = [](Rig &rig)
        {
            VifBuilder a;
            a.mskpath3(true);
            a.unpackV4(0, lanes(3, 2));
            a.mscal(0);
            a.words.push_back((0x04u << 24) | 0x12u); // ITOP
            a.words.push_back((0x03u << 24) | 0x40u); // BASE
            rig.kickVif1Chain(0x10000u, a);
            rig.kickGifNormal(0x20000u, gifAd(0x21, 0x1111u)); // masked behind MSKPATH3
            rig.kickGifNormal(0x20100u, gifAd(0x22, 0x2222u));
            rig.gsRegs({{0x32, 0x3333u}});
            // This fixture compares reset register/parser semantics after accepted
            // input. The dedicated reset-cancellation fixture covers active input.
            ps2x::device::drainAll(rig.mem, ps2x::device::DrainReason::Explicit);
            rig.mem.writeIORegister(kVif1Fbrst, 0x1u); // reset: unmasks PATH3 and flushes its FIFO
            VifBuilder b;
            b.mscal(8);
            rig.kickVif1Chain(0x40000u, b);
        };
        requireSameAcrossModes("fbrst", scenario, true);
        const auto observed = runScenario(Mode::Async, scenario, true);
        require(!observed.path3Masked, "fbrst: PATH3 unmasked");
        std::vector<int> kinds;
        for (const auto &e : observed.stream)
            kinds.push_back(e.kind);
        require(kinds == std::vector<int>({1, 100, 3, 3, 1}), "fbrst: masked PATH3 flushed by the reset in order");
    }

    void testMpgAfterQueuedMscal()
    {
        const Scenario scenario = [](Rig &rig)
        {
            VifBuilder a;
            a.mpg(0, {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u});
            a.mscal(0);
            rig.kickVif1Normal(0x50000u, a);
            // Overwrite the source in RAM right away: the queued kick runs on its snapshot.
            VifBuilder b;
            b.mpg(0, {0x55555555u, 0x66666666u, 0x77777777u, 0x88888888u});
            b.mscal(0);
            b.finish();
            rig.writeRam(0x50000u, b.words);
            rig.kickGifNormal(0x20000u, gifAd(0x23, 0x4444u)); // D2 does not wait for D1
            // EE write to VU1 micro memory: must land after the queued MSCAL.
            rig.mem.write32(PS2_VU1_CODE_BASE + 8u, 0x99999999u);
            rig.kickVif1Normal(0x50000u, b);
        };
        requireSameAcrossModes("mpg", scenario, true);
        const auto sync = runScenario(Mode::Sync, scenario, true);
        require(sync.stream.size() == 3u && sync.stream[0].bytes != sync.stream[2].bytes,
                "mpg: second MSCAL must see the new microcode");
    }

    void testCompletionState()
    {
        const Scenario scenario = [](Rig &rig)
        {
            rig.mem.writeIORegister(kDStat, 0x00060000u); // enable CIS1/CIS2 masks
            VifBuilder a;
            a.unpackV4(0, lanes(4, 1));
            a.mscal(0);
            rig.kickVif1Chain(0x10000u, a);
            rig.kickGifNormal(0x20000u, gifAd(0x24, 0x5555u));
        };
        requireSameAcrossModes("completion", scenario, true);

        Rig rig(Mode::Async);
        rig.slowVu1 = true;
        scenario(rig);
        require((rig.mem.m_ioRegisters[kDStat] & 0x2u) == 0u, "completion: CIS1 not set before observation");
        const uint32_t chcr = rig.mem.readIORegister(kD1);
        require((chcr & ~0x100u) == (rig.mem.m_ioRegisters[kD1] & ~0x100u),
                "completion: CHCR read returns latched channel mode");
        ps2x::device::drainAll(rig.mem, ps2x::device::DrainReason::Explicit);
        require((rig.mem.m_ioRegisters[kD1] & 0x100u) == 0u,
                "completion: CHCR.STR clear after explicit drain");
        require((rig.mem.m_ioRegisters[kDStat] & 0x80000002u) == 0x80000002u, "completion: CIS1 and IRQ after drain");
        require((rig.mem.readIORegister(kDStat) & 0x6u) == 0x6u, "completion: CIS1/CIS2 after D_STAT drain");
        const auto stats = ps2x::device::stats(rig.mem);
        require(stats.async && stats.submitted == 2u && stats.completed == stats.submitted, "completion: stats");
    }

    void testWorkerException()
    {
        Rig rig(Mode::Async);
        rig.throwAtPc = 0x100u;
        VifBuilder a;
        a.mscal(0x100u);
        rig.kickVif1Chain(0x10000u, a); // must not throw on the EE: the fault happens on the worker
        bool thrown = false;
        try
        {
            (void)rig.mem.readIORegister(kVif1Stat);
        }
        catch (const std::runtime_error &e)
        {
            thrown = std::string(e.what()) == "fake VU1 fault";
        }
        require(thrown, "exception: worker fault rethrown on the EE at the next observation");
        // The failed kick left CHCR.STR as the synchronous path would; the device keeps working.
        rig.throwAtPc = 0xFFFFFFFFu;
        VifBuilder b;
        b.mscal(0);
        rig.kickVif1Chain(0x20000u, b);
        (void)rig.mem.readIORegister(kDStat);
        require(rig.stream.size() == 1u && rig.stream[0].kind == 1, "exception: device continues after a fault");

        Rig syncRig(Mode::Sync);
        syncRig.throwAtPc = 0x100u;
        bool syncThrown = false;
        try
        {
            syncRig.kickVif1Chain(0x10000u, a);
        }
        catch (const std::runtime_error &)
        {
            syncThrown = true;
        }
        require(syncThrown, "exception: synchronous executor throws at the kick");
    }

    void testStress()
    {
        const Scenario scenario = [](Rig &rig)
        {
            std::mt19937 gen(1234u);
            auto rng = [&gen]() -> uint32_t { return static_cast<uint32_t>(gen()); };
            for (int i = 0; i < 3000; ++i)
            {
                const uint32_t pick = rng() % 10u;
                if (pick < 5u)
                {
                    VifBuilder v;
                    const uint32_t ops = 1u + rng() % 4u;
                    for (uint32_t k = 0; k < ops; ++k)
                    {
                        switch (rng() % 5u)
                        {
                        case 0: v.unpackV4(rng() % 1000u, lanes(rng(), 1u + rng() % 4u)); break;
                        case 1: v.mscal((rng() % 64u) * 8u); break;
                        case 2: v.direct(gifAd(0x40 + rng() % 8u, rng())); break;
                        case 3: v.mskpath3((rng() & 1u) != 0u); break;
                        default: v.mpg((rng() % 64u) * 8u, {rng(), rng()}); break;
                        }
                    }
                    if ((rng() & 1u) != 0u)
                        rig.kickVif1Chain(0x100000u + (i % 16u) * 0x1000u, v);
                    else
                        rig.kickVif1Normal(0x200000u + (i % 16u) * 0x1000u, v);
                }
                else if (pick < 7u)
                {
                    rig.kickGifNormal(0x300000u + (i % 16u) * 0x100u, gifAd(0x50 + rng() % 8u, rng()));
                }
                else if (pick == 7u)
                {
                    rig.gsRegs({{static_cast<uint8_t>(rng() & 0x3Fu), rng()}});
                }
                else if (pick == 8u)
                {
                    rig.gsCall(static_cast<uint8_t>(rng()));
                }
                else
                {
                    switch (rng() % 4u)
                    {
                    case 0: (void)rig.mem.readIORegister(kDStat); break;
                    case 1: (void)rig.mem.readIORegister(kVif1Stat); break;
                    case 2: (void)rig.mem.read32(PS2_VU1_DATA_BASE + (rng() % 1024u) * 16u); break;
                    default:
                        // Mode equivalence intentionally observes completion first;
                        // active cancellation has deterministic separate fixtures.
                        ps2x::device::drainAll(rig.mem, ps2x::device::DrainReason::Explicit);
                        rig.mem.writeIORegister(kVif1Fbrst, 0x1u);
                        break;
                    }
                }
            }
        };
        requireSameAcrossModes("stress", scenario);
        const auto observed = runScenario(Mode::Async, scenario);
        require(observed.stream.size() > 2000u, "stress: produced GS traffic");
    }
}

int main()
{
    try
    {
        testOrdering();
        testFbrstMidQueue();
        testMpgAfterQueuedMscal();
        testCompletionState();
        testWorkerException();
        testPauseLatchedReadAndStaleCompletion();
        testPauseRestartResumesUnacceptedTail();
        testResetBeforeNewInputAndGifIndependence();
        testCancelledWorkerErrorStillSurfaces();
        testVu1ResetRetainsEmittedDraws();
        testPauseAtLastAcceptedCommandRestartsEmptyTail();
        testStress();
        std::cout << "VU1 async device queue tests passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
