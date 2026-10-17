#include "runtime/ps2_vu1_lift.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_device_queue.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <cfenv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace vl
{
    bool resetRequested() noexcept { return ps2x::device::workerShouldCancelVu1(); }
    namespace { std::atomic<uint32_t> g_declined{0}; }
    void LiftAccess::noteDeclined() { g_declined.fetch_add(1, std::memory_order_relaxed); }
    uint32_t declinedSegments() { return g_declined.load(std::memory_order_relaxed); }

    void LiftAccess::prepareActive(Run &c)
    {
        VU1Interpreter &v = vu(c);
        prepare(c, v.m_activeMemory->getVU1Code(), v.m_activeMemory);
    }
    namespace { std::atomic<int> g_enabledOverride{-1}; }
    void setEnabledOverride(int value) { g_enabledOverride.store(value, std::memory_order_relaxed); }
    int enabledOverride() { return g_enabledOverride.load(std::memory_order_relaxed); }
    void checkReset(Run &) { if (resetRequested()) throw ResetCanceled{}; }
    namespace
    {
        std::vector<Registration> &registry()
        {
            static std::vector<Registration> entries;
            return entries;
        }

        // Exact-result classification (normalizeFmacExactResult) of one lane.
        template <typename Real>
        uint32_t classifyExact(Real exact, float &value)
        {
            const bool negative = std::signbit(exact);
            const Real magnitude = std::fabs(exact);
            uint32_t flags = negative ? 0x2u : 0u;
            const uint32_t sign = negative ? 0x80000000u : 0u;
            if (magnitude == Real(0))
            {
                flags |= 0x1u;
                value = f32(sign);
            }
            else if (magnitude > static_cast<Real>(std::numeric_limits<float>::max()))
            {
                flags |= 0x8u;
                value = f32(sign | 0x7F7FFFFFu);
            }
            else if (magnitude < static_cast<Real>(std::numeric_limits<float>::min()))
            {
                flags |= 0x5u;
                value = f32(sign);
            }
            return flags;
        }

        template <typename Real>
        Real exactResult(FmacOp op, Real a, Real b, Real acc)
        {
            switch (op)
            {
            case Add: return a + b;
            case Sub: return a - b;
            case Mul: return a * b;
            case MAdd: return acc + a * b;
            case MSub: return acc - a * b;
            }
            return Real(0);
        }
    }

    Registrar::Registrar(const Registration *registrations, size_t count)
    {
        registry().insert(registry().end(), registrations, registrations + count);
    }

    Segment find(uint64_t hash, uint32_t pc, const uint8_t *vuCode)
    {
        for (const Registration &r : registry())
        {
            if (r.pc != pc)
                continue;
            if (r.subSize == 0)
            {
                if (r.hash == hash)
                    return r.fn;
            }
            else
            {
                const uint32_t start = r.subStart == ~0u ? pc : r.subStart;
                if (vuCode && start + r.subSize <= 0x4000u && hashCode(vuCode + start, r.subSize) == r.hash)
                    return r.fn;
            }
        }
        return nullptr;
    }

    uint64_t hashCode(const uint8_t *code, uint32_t size)
    {
        uint64_t h = 1469598103934665603ull;
        for (uint32_t i = 0; i < size; ++i)
            h = (h ^ code[i]) * 1099511628211ull;
        return h;
    }

    void kickCatchUpSlow(Run &c, uint64_t cycle)
    {
        while (*c.kickActive && c.kickCycle < cycle)
        {
            checkReset(c);
            ++c.kickCycle;
            c.kickProgress(c.vu);
        }
    }

    uint64_t kickWait(Run &c, uint64_t cycle)
    {
        kickCatchUp(c, cycle);
        while (*c.kickActive)
        {
            checkReset(c);
            ++c.kickCycle;
            c.kickProgress(c.vu);
        }
        return std::max(cycle, c.kickCycle);
    }

    V fmacSlow(FmacOp op, V r, V a, V b, V acc, int laneMask, Flags &fl)
    {
        alignas(16) float value[4], av[4], bv[4], cv[4];
        _mm_store_ps(value, r);
        _mm_store_ps(av, a);
        _mm_store_ps(bv, b);
        _mm_store_ps(cv, acc);
        int z = 0, s = 0, u = 0, o = 0;
        uint32_t productSticky = 0u;
        for (int lane = 0; lane < 4; ++lane)
        {
            if ((laneMask & (1 << lane)) == 0)
                continue;
            if (op == MAdd || op == MSub)
            {
                float ignored = 0.0f;
                productSticky |= classifyExact(static_cast<double>(av[lane]) * bv[lane], ignored);
            }
            const double exact = exactResult<double>(op, av[lane], bv[lane], cv[lane]);
            uint32_t flags;
            if (std::fabs(exact) == static_cast<double>(std::numeric_limits<float>::max()))
                flags = classifyExact(exactResult<long double>(op, av[lane], bv[lane], cv[lane]), value[lane]);
            else
                flags = classifyExact(exact, value[lane]);
            const int bit = 1 << lane;
            z |= (flags & 0x1u) ? bit : 0;
            s |= (flags & 0x2u) ? bit : 0;
            u |= (flags & 0x4u) ? bit : 0;
            o |= (flags & 0x8u) ? bit : 0;
        }
        fl.mac = macNibble(z) | (macNibble(s) << 4) | (macNibble(u) << 8) | (macNibble(o) << 12);
        fl.cur = (z ? 1u : 0u) | (s ? 2u : 0u) | (u ? 4u : 0u) | (o ? 8u : 0u);
        fl.sticky |= (fl.cur | productSticky) << 6;
        fl.extra = productSticky;
        fl.pending = true;
        return _mm_load_ps(value);
    }

    void trace(Run &c, uint32_t pc, uint64_t cycle)
    {
        std::fprintf(stderr, "[vu1lift] pc=%04x cycle=%llu\n", pc,
                     static_cast<unsigned long long>(cycle - c.cycle));
    }
}

bool VU1Interpreter::runLifted(const uint8_t *vuCode, uint32_t codeSize, uint32_t dataSize, PS2Memory *memory)
{
    // PS2X_VU1_LIFT: enabled by default, unset or != "0" -> true. PS2X_VU1_LIFT=0 -> false.
    static const bool enabled = []
    {
        const char *env = std::getenv("PS2X_VU1_LIFT");
        const bool on = !(env && env[0] == '0' && env[1] == '\0');
        if (on)
            std::fprintf(stderr, "[vu1lift] enabled\n");
        return on;
    }();
    static uint64_t s_totalCalls = 0;
    static uint64_t s_guardBail = 0;
    static uint64_t s_findMiss = 0;
    static uint64_t s_findHit = 0;

    ++s_totalCalls;

    const int forced = vl::enabledOverride();
    if (forced == 0 || (forced < 0 && !enabled) || m_unit != Unit::VU1 || !memory || codeSize != 0x4000u || dataSize != 0x4000u ||
        vuCode != memory->getVU1Code() || !m_activeVuData)
        return false;

    // Lifted schedules start from idle pipelines.
    if (m_stopRequested || m_state.branchPending || m_state.ebit || m_state.haltAfterDelaySlot ||
        m_viBranchBackupValid || m_xgkick.active ||
        m_nextCommitCycle != std::numeric_limits<uint64_t>::max() || m_writebackHorizon > m_cycle)
    {
        ++s_guardBail;
        return false;
    }

    thread_local const uint8_t *cachedCode = nullptr;
    thread_local uint64_t cachedGeneration = ~0ull;
    thread_local uint64_t cachedHash = 0;
    thread_local std::array<vl::Segment, 2048> s_pcCache{};
    thread_local std::array<bool, 2048> s_pcCacheValid{};
    const uint64_t generation = memory->getVU1CodeGeneration();
    if (vuCode != cachedCode || generation != cachedGeneration)
    {
        cachedHash = vl::hashCode(vuCode, codeSize);
        cachedCode = vuCode;
        cachedGeneration = generation;
        s_pcCacheValid.fill(false);
    }
    const uint32_t pcSlot = (m_state.pc >> 3) & 0x7FFu;
    vl::Segment segment = nullptr;
    if (s_pcCacheValid[pcSlot])
    {
        segment = s_pcCache[pcSlot];
    }
    else
    {
        segment = vl::find(cachedHash, m_state.pc, vuCode);
        s_pcCache[pcSlot] = segment;
        s_pcCacheValid[pcSlot] = true;
    }
    if (!segment)
    {
        ++s_findMiss;
        if ((s_findMiss & 0x3FFF) == 1)
        {
            std::fprintf(stderr, "[vu1lift:miss] calls=%llu bail=%llu miss=%llu hit=%llu hash=0x%016llx pc=0x%04x\n",
                         static_cast<unsigned long long>(s_totalCalls),
                         static_cast<unsigned long long>(s_guardBail),
                         static_cast<unsigned long long>(s_findMiss),
                         static_cast<unsigned long long>(s_findHit),
                         static_cast<unsigned long long>(cachedHash),
                         m_state.pc);
        }
        return false;
    }

    ++s_findHit;
    if ((s_findHit & 0x3FFF) == 1)
    {
        std::fprintf(stderr, "[vu1lift:hit] calls=%llu bail=%llu miss=%llu hit=%llu hash=0x%016llx pc=0x%04x\n",
                     static_cast<unsigned long long>(s_totalCalls),
                     static_cast<unsigned long long>(s_guardBail),
                     static_cast<unsigned long long>(s_findMiss),
                     static_cast<unsigned long long>(s_findHit),
                     static_cast<unsigned long long>(cachedHash),
                     m_state.pc);
    }

    vl::Run c{m_state, m_activeVuData, m_cycle, m_cycle, m_efuResourceReady, this,
              [](void *vu) { static_cast<VU1Interpreter *>(vu)->progressXgkick(); },
              [](void *vu, uint32_t qwordAddress, uint64_t cycle)
              {
                  auto *self = static_cast<VU1Interpreter *>(vu);
                  self->m_cycle = cycle; // startXgkick records its issue cycle
                  self->startXgkick(qwordAddress);
              },
              &m_xgkick.active, m_cycle};
    try
    {
        vl::checkReset(c);
        // The segment keeps time on this interpreter's scheduler (m_cycle,
        // register ready times, flag/Q/PATH1 pipelines); see vl::LiftAccess.
        // Generated segments carry constant timing; the decode table is only
        // built on demand (timing validation, hand-written segments).
        segment(c);
        if (c.declined)
            return false; // embedded timing differs: interpret this program
    }
    catch (const vl::ResetCanceled &)
    {
        // The ordered reset command clears pending pipelines next. Do not
        // interpret/replay this already partially accepted microprogram.
        m_state.cycles = m_cycle;
        m_liftCanceled = true;
        return true;
    }

    // As at the end of an interpreted program: every write lands and the
    // PATH1 transfer finishes.
    flushPipelines();
    m_state.cycles = m_cycle;
    m_state.ebit = false;
    m_state.haltAfterDelaySlot = false;
    m_pendingHaltD = false;
    m_pendingHaltT = false;
    return true;
}

bool VU1Interpreter::runLiftedChecked(uint8_t *vuCode, uint32_t codeSize, uint8_t *vuData, uint32_t dataSize,
                                      GS &gs, PS2Memory *memory)
{
    // PS2X_VU1_LIFT_VERIFY=1 (testing only, slow): every lifted run is
    // repeated by the interpreter from a copy of the start state and VU
    // memory; registers, flags, Q/P, pc, cycles, VU memory and the XGKICK
    // packets must be identical. The game continues with the lifted result.
    static const bool verify = []
    {
        const char *env = std::getenv("PS2X_VU1_LIFT_VERIFY");
        const bool on = env && env[0] == '1';
        if (on)
            std::fprintf(stderr, "[vu1verify] enabled\n");
        return on;
    }();
    if (!verify)
        return runLifted(vuCode, codeSize, dataSize, memory);

    static uint64_t s_runs = 0, s_mismatches = 0, s_skipped = 0;
    auto shadow = std::make_unique<VU1Interpreter>(*this);
    std::vector<uint8_t> shadowData(vuData, vuData + dataSize);
    std::vector<uint8_t> liftedPackets, shadowPackets;
    const uint32_t entryPc = m_state.pc;
    const uint64_t entryCycle = m_cycle;

    m_liftCanceled = false;
    m_gifCapture = &liftedPackets;
    const bool lifted = runLifted(vuCode, codeSize, dataSize, memory);
    m_gifCapture = nullptr;
    if (!lifted)
        return false;
    if (m_liftCanceled)
    {
        ++s_skipped;
        return true;
    }

    shadow->m_verifyShadow = true;
    shadow->m_gifCapture = &shadowPackets;
    shadow->run(vuCode, codeSize, shadowData.data(), dataSize, gs, memory, std::numeric_limits<uint32_t>::max());
    if (ps2x::device::workerShouldCancelVu1())
    {
        ++s_skipped;
        return true;
    }

    ++s_runs;
    const VU1State &a = m_state;
    const VU1State &b = shadow->m_state;
    char what[160] = "";
    auto same = [](const void *x, const void *y, size_t n) { return std::memcmp(x, y, n) == 0; };
    for (int r = 0; r < 32 && !what[0]; ++r)
        if (!same(a.vf[r], b.vf[r], sizeof(a.vf[r])))
            std::snprintf(what, sizeof(what), "vf%d lifted=%g,%g,%g,%g interp=%g,%g,%g,%g", r,
                          a.vf[r][0], a.vf[r][1], a.vf[r][2], a.vf[r][3], b.vf[r][0], b.vf[r][1], b.vf[r][2], b.vf[r][3]);
    for (int r = 0; r < 16 && !what[0]; ++r)
        if (a.vi[r] != b.vi[r])
            std::snprintf(what, sizeof(what), "vi%d lifted=%04x interp=%04x", r, a.vi[r] & 0xFFFF, b.vi[r] & 0xFFFF);
    const struct { const char *name; const void *x, *y; size_t n; } fields[] = {
        {"acc", a.acc, b.acc, sizeof(a.acc)}, {"q", &a.q, &b.q, 4}, {"p", &a.p, &b.p, 4}, {"i", &a.i, &b.i, 4},
        {"r", &a.r, &b.r, 4}, {"pc", &a.pc, &b.pc, 4}, {"mac", &a.mac, &b.mac, 4}, {"clip", &a.clip, &b.clip, 4},
        {"status", &a.status, &b.status, 4}, {"cycles", &a.cycles, &b.cycles, 8},
        {"cycle", &m_cycle, &shadow->m_cycle, 8},
        {"ebit", &a.ebit, &b.ebit, 1}, {"haltAfterDelaySlot", &a.haltAfterDelaySlot, &b.haltAfterDelaySlot, 1},
        {"stoppedByD", &a.stoppedByD, &b.stoppedByD, 1}, {"stoppedByT", &a.stoppedByT, &b.stoppedByT, 1},
        {"branchPending", &a.branchPending, &b.branchPending, 1},
    };
    for (const auto &f : fields)
        if (!what[0] && !same(f.x, f.y, f.n))
        {
            uint64_t x = 0, y = 0;
            std::memcpy(&x, f.x, f.n < 8 ? f.n : 8);
            std::memcpy(&y, f.y, f.n < 8 ? f.n : 8);
            std::snprintf(what, sizeof(what), "%s lifted=%llx interp=%llx", f.name,
                          static_cast<unsigned long long>(x), static_cast<unsigned long long>(y));
        }
    if (!what[0] && !same(vuData, shadowData.data(), dataSize))
    {
        uint32_t at = 0;
        while (vuData[at] == shadowData[at])
            ++at;
        std::snprintf(what, sizeof(what), "VU memory from byte 0x%04x", at);
    }
    if (!what[0] && liftedPackets != shadowPackets)
        std::snprintf(what, sizeof(what), "XGKICK packets (lifted %zu bytes, interp %zu bytes)",
                      liftedPackets.size(), shadowPackets.size());

    if (what[0])
    {
        ++s_mismatches;
        if (s_mismatches <= 50u)
        {
            uint64_t h = 1469598103934665603ull;
            for (uint32_t n = 0; n < codeSize; ++n)
                h = (h ^ vuCode[n]) * 1099511628211ull;
            std::fprintf(stderr, "[vu1verify] MISMATCH hash=%016llx pc=%04x run=%llu cycles=%llu: %s\n",
                         static_cast<unsigned long long>(h), entryPc, static_cast<unsigned long long>(s_runs),
                         static_cast<unsigned long long>(m_cycle - entryCycle), what);
        }
    }
    if ((s_runs & 0x3FFFu) == 0u || (what[0] && s_mismatches <= 50u))
        std::fprintf(stderr, "[vu1verify] runs=%llu mismatches=%llu skipped=%llu\n",
                     static_cast<unsigned long long>(s_runs), static_cast<unsigned long long>(s_mismatches),
                     static_cast<unsigned long long>(s_skipped));
    return true;
}
