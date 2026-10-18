// Issue 017: VU0 macro-mode FMAC instructions in recompiled EE code (ps2_vu0::toVf / toAcc, the calls the
// recompiler emits) must compute what the VU FMAC unit computes: the result lanes, the MAC flags and the status
// flags (current Z S U O and the sticky bits). The reference is the lifted VU1 code's exact path
// (vl::fmacExact on normalized operands), which the in-game verifier and vu1_fmac_fast check against the VU
// interpreter. Every op (ADD SUB MUL MADD MSUB) x every dest mask x both destinations (VF, ACC), random and
// edge operands, under round-toward-zero (the game thread's EE rounding).
#include "ps2_vu0_macro.h"
#include "runtime/ps2_vu1_lift.h"
#include "runtime/ps2_vu1.h"

#include <cfenv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <utility>
#include <xmmintrin.h>

extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x1000;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x1004;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 1;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[] = {nullptr};

namespace
{
    using vl::V;
    std::mt19937_64 rng(0x0017017ull);
    uint64_t checked = 0, valueFailures = 0, flagFailures = 0;

    float fromBits(uint32_t b)
    {
        float f;
        std::memcpy(&f, &b, 4);
        return f;
    }

    uint32_t randomBits()
    {
        const uint32_t sign = (rng() & 1u) << 31;
        switch (rng() % 12u)
        {
        case 0: return sign;                                                             // +-0
        case 1: return sign | static_cast<uint32_t>(rng() & 0x7FFFFFu);                  // denormal
        case 2: return sign | (0x00800000u + static_cast<uint32_t>(rng() % 64u));       // near FLT_MIN
        case 3: return sign | (0x7F7FFFFFu - static_cast<uint32_t>(rng() % 64u));       // near FLT_MAX
        case 4: return sign | 0x7F800000u | static_cast<uint32_t>(rng() & 0x7FFFFFu);   // exponent 255
        case 5: return sign | ((static_cast<uint32_t>(rng() % 60u) + 1u) << 23) | static_cast<uint32_t>(rng() & 0x7FFFFFu);   // tiny
        case 6: return sign | ((static_cast<uint32_t>(rng() % 60u) + 194u) << 23) | static_cast<uint32_t>(rng() & 0x7FFFFFu); // huge
        default: return sign | ((static_cast<uint32_t>(rng() % 64u) + 95u) << 23) | static_cast<uint32_t>(rng() & 0x7FFFFFu); // ordinary
        }
    }

    V randomVector()
    {
        return _mm_setr_ps(fromBits(randomBits()), fromBits(randomBits()), fromBits(randomBits()), fromBits(randomBits()));
    }

    using ps2_vu0::liftOp;

    template <ps2_vu0::Fmac Op, int Dest, bool ToAcc>
    void checkOne(R5900Context &ctx, V a, V b, V acc, uint16_t status)
    {
        constexpr int fd = 7;
        const V oldVf = randomVector();
        ctx.vu0_vf[fd] = oldVf;
        ctx.vu0_acc = acc;
        ctx.vu0_status = status;
        ctx.vu0_mac_flags = 0xDEADu;
        if constexpr (ToAcc)
            ps2_vu0::toAcc<Op, Dest>(&ctx, a, b);
        else
            ps2_vu0::toVf<Op, Dest>(&ctx, fd, a, b);

        vl::Flags fl{0u, 0u, 0u};
        const V exact = vl::fmacExact<liftOp(Op), Dest>(vl::norm(a), vl::norm(b), vl::norm(acc), fl);
        const V expected = ps2_vu0::blendDest<Dest>(ToAcc ? acc : oldVf, exact);
        const uint32_t current = fl.cur & 0xFu;
        const uint32_t expectedStatus = (status & 0xFF0u) | current | ((current | fl.extra) << 6);
        const V got = ToAcc ? ctx.vu0_acc : ctx.vu0_vf[fd];
        ++checked;
        const bool valueOk = std::memcmp(&got, &expected, sizeof(V)) == 0;
        const bool flagsOk = ctx.vu0_mac_flags == fl.mac && ctx.vu0_status == expectedStatus;
        if (!valueOk)
            ++valueFailures;
        if (!flagsOk)
            ++flagFailures;
        if ((!valueOk || !flagsOk) && valueFailures + flagFailures <= 8)
        {
            uint32_t g[4], e[4];
            std::memcpy(g, &got, 16);
            std::memcpy(e, &expected, 16);
            std::fprintf(stderr, "op=%d dest=%d acc=%d value %08x %08x %08x %08x (want %08x %08x %08x %08x) mac %x (want %x) "
                         "status %x (want %x)\n", static_cast<int>(Op), Dest, int(ToAcc), g[0], g[1], g[2], g[3], e[0],
                         e[1], e[2], e[3], ctx.vu0_mac_flags, fl.mac, ctx.vu0_status, expectedStatus);
        }
    }

    template <ps2_vu0::Fmac Op, int Dest>
    void checkMany(R5900Context &ctx, int count)
    {
        for (int i = 0; i < count; ++i)
        {
            const V a = randomVector(), b = randomVector();
            const V acc = randomVector();
            const uint16_t status = static_cast<uint16_t>(rng() & 0xFC0u);
            checkOne<Op, Dest, false>(ctx, a, b, acc, status);
            checkOne<Op, Dest, true>(ctx, a, b, acc, status);
            // MADD/MSUB cancellation: ACC close to -+a*b.
            const V q = _mm_mul_ps(vl::norm(a), vl::norm(b));
            const V cancel = Op == ps2_vu0::MAdd ? _mm_sub_ps(_mm_setzero_ps(), q) : q;
            checkOne<Op, Dest, false>(ctx, a, b, cancel, status);
            checkOne<Op, Dest, true>(ctx, a, b, cancel, status);
        }
    }

    template <ps2_vu0::Fmac Op, int... Dest>
    void checkOp(R5900Context &ctx, int count, std::integer_sequence<int, Dest...>)
    {
        (checkMany<Op, Dest + 1>(ctx, count), ...);
    }
}

    // FDIV unit: the VU interpreter runs DIV/SQRT/RSQRT Q, fs.x, ft.y; WAITQ; end, and its Q and status are the
    // reference for ps2_vu0::div / sqrt / rsqrt.
    uint64_t fdivChecked = 0, fdivFailures = 0;

    void checkFdiv(PS2Runtime &rt, int which, float fs, float ft, uint16_t status)
    {
        constexpr uint32_t kUpperNop = 0x000002FFu, kLowerNop = 0x8000033Cu, kEBit = 0x40000000u;
        const uint32_t lowerOp = which == 0 ? 0x3BCu : which == 1 ? 0x3BDu : 0x3BEu;
        // ftf = y (1), fsf = x (0), ft = vf2, fs = vf1.
        const uint32_t fdiv = 0x80000000u | (1u << 23) | (0u << 21) | (2u << 16) | (1u << 11) | lowerOp;
        const uint32_t waitq = 0x80000000u | 0x3BFu;
        const uint32_t program[] = {fdiv, kUpperNop, waitq, kUpperNop | kEBit, kLowerNop, kUpperNop, kLowerNop, kUpperNop};
        uint8_t *code = rt.memory().getVU1Code();
        std::memset(code, 0, 64);
        std::memcpy(code, program, sizeof(program));
        auto &vu = rt.vu1();
        VU1State &st = vu.state();
        st = VU1State{};
        st.vf[0][3] = 1.0f;
        st.vf[1][0] = fs;
        st.vf[2][1] = ft;
        st.status = status;
        vl::setEnabledOverride(0);
        // No memory object: the interpreter decodes the rewritten program afresh instead of using its code cache.
        vu.execute(code, 16384, rt.memory().getVU1Data(), 16384, rt.gs(), nullptr, 0, 0, 0, 1000);
        vl::setEnabledOverride(-1);
        const float wantQ = vu.state().q;
        const uint32_t wantStatus = vu.state().status;

        auto ctx = std::make_unique<R5900Context>();
        ctx->vu0_status = status;
        if (which == 0)
            ps2_vu0::div(ctx.get(), fs, ft);
        else if (which == 1)
            ps2_vu0::sqrt(ctx.get(), ft);
        else
            ps2_vu0::rsqrt(ctx.get(), fs, ft);
        ++fdivChecked;
        if (std::memcmp(&ctx->vu0_q, &wantQ, 4) != 0 || ctx->vu0_status != wantStatus)
        {
            if (++fdivFailures <= 8)
            {
                uint32_t a, b, gq, wq;
                std::memcpy(&a, &fs, 4); std::memcpy(&b, &ft, 4); std::memcpy(&gq, &ctx->vu0_q, 4); std::memcpy(&wq, &wantQ, 4);
                std::fprintf(stderr, "fdiv op=%d fs=%08x ft=%08x q %08x (want %08x) status %x (want %x)\n", which, a, b, gq,
                             wq, ctx->vu0_status, wantStatus);
            }
        }
    }

    // FTOIn: ps2_vu0::ftoi against the lifted VU1 code's vl::ftoi on a normalized operand.
    uint64_t ftoiChecked = 0, ftoiFailures = 0;

    template <int Shift>
    void checkFtoi(int count)
    {
        auto ctx = std::make_unique<R5900Context>();
        for (int i = 0; i < count; ++i)
        {
            const V src = randomVector(), old = randomVector();
            ctx->vu0_vf[3] = old;
            ps2_vu0::ftoi<Shift, 15>(ctx.get(), 3, src);
            const V want = vl::ftoi<Shift, 15>(vl::norm(src));
            ++ftoiChecked;
            if (std::memcmp(&ctx->vu0_vf[3], &want, sizeof(V)) != 0 && ++ftoiFailures <= 4)
                std::fprintf(stderr, "ftoi%d mismatch\n", Shift);
        }
    }

int main()
{
    auto ctx = std::make_unique<R5900Context>();
    const unsigned int saved = _MM_GET_ROUNDING_MODE();
    _MM_SET_ROUNDING_MODE(_MM_ROUND_TOWARD_ZERO); // EE arithmetic on the game thread
    const auto dests = std::make_integer_sequence<int, 15>{};
    checkOp<ps2_vu0::Add>(*ctx, 4000, dests);
    checkOp<ps2_vu0::Sub>(*ctx, 4000, dests);
    checkOp<ps2_vu0::Mul>(*ctx, 4000, dests);
    checkOp<ps2_vu0::MAdd>(*ctx, 4000, dests);
    checkOp<ps2_vu0::MSub>(*ctx, 4000, dests);
    {
        PS2Runtime rt;
        if (!rt.memory().initialize())
            return 2;
        // Both x87 and SSE: VU1Interpreter::execute saves/restores with fegetround (x87 on glibc).
        std::fesetround(FE_TOWARDZERO);
        for (int i = 0; i < 20000; ++i)
        {
            const float fs = fromBits(randomBits()), ft = fromBits(randomBits());
            const uint16_t status = static_cast<uint16_t>(rng() & 0xFFFu);
            for (int which = 0; which < 3; ++which)
            {
                checkFdiv(rt, which, fs, ft, status);
                checkFdiv(rt, which, fs, 0.0f, status);
                checkFdiv(rt, which, 0.0f, ft, status);
            }
        }
    }
    checkFtoi<0>(50000);
    checkFtoi<4>(50000);
    checkFtoi<12>(50000);
    checkFtoi<15>(50000);
    _MM_SET_ROUNDING_MODE(saved);
    std::printf("vu0 macro ftoi: %llu checks, %llu mismatches\n", static_cast<unsigned long long>(ftoiChecked),
                static_cast<unsigned long long>(ftoiFailures));
    std::printf("vu0 macro fdiv: %llu checks, %llu mismatches\n", static_cast<unsigned long long>(fdivChecked),
                static_cast<unsigned long long>(fdivFailures));
    std::printf("vu0 macro fmac: %llu checks, %llu value mismatches, %llu flag mismatches\n",
                static_cast<unsigned long long>(checked), static_cast<unsigned long long>(valueFailures),
                static_cast<unsigned long long>(flagFailures));
    return valueFailures == 0 && flagFailures == 0 && fdivFailures == 0 && ftoiFailures == 0 ? 0 : 1;
}
