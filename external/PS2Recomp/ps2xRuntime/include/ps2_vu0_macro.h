#pragma once
// VU0 macro-mode FMAC instructions (COP2 upper ops in recompiled EE code). The recompiler emits one call per
// instruction, so the generated code and the tests run the same arithmetic (issue 017): the VU FMAC unit's exact
// result and its MAC/status flags (the lifted VU1 code's vl::fmac on normalized operands), committed at once.
// Dest uses the VU field bits: x = 8, y = 4, z = 2, w = 1.

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/vu_fmac.h"

#include <cmath>
#include <immintrin.h>
#include <limits>

namespace ps2_vu0
{
    enum Fmac
    {
        Add,
        Sub,
        Mul,
        MAdd,
        MSub
    };

    template <int Dest>
    inline __m128 blendDest(__m128 old, __m128 result)
    {
        constexpr int imm = ((Dest & 8) ? 1 : 0) | ((Dest & 4) ? 2 : 0) | ((Dest & 2) ? 4 : 0) | ((Dest & 1) ? 8 : 0);
        return _mm_blend_ps(old, result, imm);
    }

    constexpr vl::FmacOp liftOp(Fmac op)
    {
        return op == Add ? vl::Add : op == Sub ? vl::Sub : op == Mul ? vl::Mul : op == MAdd ? vl::MAdd : vl::MSub;
    }

    // a op b on normalized operands (denormals -> signed zero, exponent 255 -> signed max), as the VU reads them.
    // The MAC flags and the status Z S U O bits come from this result; the sticky bits (6-11) accumulate.
    template <Fmac Op, int Dest>
    inline __m128 compute(R5900Context *ctx, __m128 acc, __m128 a, __m128 b)
    {
        vl::Flags flags{0u, 0u, 0u};
        const __m128 result = vl::fmac<liftOp(Op), Dest>(vl::norm(a), vl::norm(b), vl::norm(acc), flags);
        const uint32_t current = flags.cur & 0xFu;
        ctx->vu0_mac_flags = flags.mac;
        ctx->vu0_status = static_cast<uint16_t>((ctx->vu0_status & 0xFF0u) | current | ((current | flags.extra) << 6));
        return result;
    }

    // fd = a op b (MADD/MSUB: ACC +- a * b), written to the Dest fields of VF[fd].
    template <Fmac Op, int Dest>
    inline void toVf(R5900Context *ctx, int fd, __m128 a, __m128 b)
    {
        ctx->vu0_vf[fd] = blendDest<Dest>(ctx->vu0_vf[fd], compute<Op, Dest>(ctx, ctx->vu0_acc, a, b));
    }

    // The ACC-destination forms (ADDA, MADDA, OPMULA, ...).
    template <Fmac Op, int Dest>
    inline void toAcc(R5900Context *ctx, __m128 a, __m128 b)
    {
        ctx->vu0_acc = blendDest<Dest>(ctx->vu0_acc, compute<Op, Dest>(ctx, ctx->vu0_acc, a, b));
    }

    // FDIV unit (DIV, SQRT, RSQRT): Q and the status D (0x20) / I (0x10) bits with their sticky copies, as the
    // VU interpreter commits them. Operands are single fields.
    inline void commitQ(R5900Context *ctx, float q, uint32_t di)
    {
        ctx->vu0_q = q;
        ctx->vu0_status = static_cast<uint16_t>((ctx->vu0_status & 0xFCFu) | di | (di << 6));
    }

    inline void div(R5900Context *ctx, float fs, float ft)
    {
        uint32_t di = 0u;
        const float q = vl::div(fs, ft, di);
        commitQ(ctx, q, di);
    }

    inline void sqrt(R5900Context *ctx, float ft)
    {
        const float value = vl::normf(ft);
        commitQ(ctx, vl::normalizeResult(std::sqrt(std::fabs(value))), value < 0.0f ? 0x10u : 0u);
    }

    // Q = fs / sqrt(|ft|). A zero radicand sets D; with a zero numerator too, also I and Q is a signed zero.
    inline void rsqrt(R5900Context *ctx, float fs, float ft)
    {
        const float num = vl::normf(fs);
        const float radicand = vl::normf(ft);
        const float den = std::sqrt(std::fabs(radicand));
        uint32_t di = radicand < 0.0f ? 0x10u : 0u;
        float q;
        if (den != 0.0f)
            q = num / den;
        else
        {
            const bool negative = std::signbit(num) != std::signbit(radicand);
            di = 0x20u;
            if (num != 0.0f)
                q = negative ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
            else
            {
                di |= 0x10u;
                q = negative ? -0.0f : 0.0f;
            }
        }
        commitQ(ctx, vl::normalizeResult(q), di);
    }

    // FTOIn: normalize, scale by 2^n, truncate; at or above 2^31 saturates to 0x7FFFFFFF (below -2^31 the
    // conversion already gives 0x80000000), as the VU (vuFloatToInt) does.
    template <int Shift, int Dest>
    inline void ftoi(R5900Context *ctx, int ft, __m128 src)
    {
        __m128 scaled = vl::norm(src);
        if constexpr (Shift != 0)
            scaled = _mm_mul_ps(scaled, _mm_set1_ps(static_cast<float>(1 << Shift)));
        __m128i ints = _mm_cvttps_epi32(scaled);
        const __m128 over = _mm_cmpge_ps(scaled, _mm_set1_ps(2147483648.0f));
        ints = _mm_blendv_epi8(ints, _mm_set1_epi32(0x7FFFFFFF), _mm_castps_si128(over));
        ctx->vu0_vf[ft] = blendDest<Dest>(ctx->vu0_vf[ft], _mm_castsi128_ps(ints));
    }
}
