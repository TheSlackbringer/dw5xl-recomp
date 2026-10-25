#pragma once
// Exact VU FMAC and FDIV arithmetic (values, MAC/status/sticky flags, Q with the D/I bits) shared by the lifted VU1 code (ps2_vu1_lift.h) and
// the VU0 macro instructions in recompiled EE code (ps2_vu0_macro.h). Moved unchanged from ps2_vu1_lift.h.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <immintrin.h>
#include <smmintrin.h>

namespace vl
{
    using V = __m128;

    struct Flags
    {
        uint32_t cur;    // status bits 0-3 (Z S U O) of the latest FMAC result
        uint32_t mac;    // MAC flags of the latest FMAC result
        uint32_t sticky; // status bits 6-11
        uint32_t extra = 0;    // product sticky bits of the latest FMAC op (MADD/MSUB)
        bool pending = false;  // the latest op produced flags not yet queued
    };

    // Value helpers ------------------------------------------------------------
    inline V load(const float *p) { return _mm_loadu_ps(p); }
    inline void store(float *p, V v) { _mm_storeu_ps(p, v); }

    inline float f32(uint32_t bits)
    {
        float f;
        std::memcpy(&f, &bits, sizeof(f));
        return f;
    }

    inline uint32_t bitsOf(float f)
    {
        uint32_t b;
        std::memcpy(&b, &f, sizeof(b));
        return b;
    }

    inline const V kVf0 = _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f);
    inline const V kZero = _mm_setzero_ps();

    // normalizeOperand: denormals flush to signed zero, Inf/NaN clamp to the
    // signed max.
    inline V norm(V v)
    {
        const __m128i bits = _mm_castps_si128(v);
        const __m128i exponent = _mm_and_si128(bits, _mm_set1_epi32(0x7F800000));
        const __m128i sign = _mm_and_si128(bits, _mm_set1_epi32(static_cast<int>(0x80000000u)));
        __m128i n = _mm_blendv_epi8(bits, sign, _mm_cmpeq_epi32(exponent, _mm_setzero_si128()));
        n = _mm_blendv_epi8(n, _mm_or_si128(sign, _mm_set1_epi32(0x7F7FFFFF)),
                            _mm_cmpeq_epi32(exponent, _mm_set1_epi32(0x7F800000)));
        return _mm_castsi128_ps(n);
    }

    inline float normf(float f)
    {
        uint32_t bits = bitsOf(f);
        const uint32_t exponent = (bits >> 23) & 0xFFu;
        if (exponent == 0u)
            bits &= 0x80000000u;
        else if (exponent == 0xFFu)
            bits = (bits & 0x80000000u) | 0x7F7FFFFFu;
        return f32(bits);
    }

    template <int Lane>
    inline V bc(V v) { return _mm_shuffle_ps(v, v, Lane * 0x55); }

    inline V splat(float f) { return _mm_set1_ps(f); }
    inline V splatBits(int32_t i) { return _mm_castsi128_ps(_mm_set1_epi32(i)); }

    // Lane i of the result comes from b when bit i of Imm is set (VU x = lane 0).
    template <int Imm>
    inline V blend(V a, V b) { return _mm_blend_ps(a, b, Imm); }

    template <int Lane>
    inline float lane(V v) { return _mm_cvtss_f32(_mm_shuffle_ps(v, v, Lane)); }

    template <int Lane>
    inline uint32_t bits(V v) { return static_cast<uint32_t>(_mm_extract_ps(v, Lane)); }

    inline V mr32(V v) { return _mm_shuffle_ps(v, v, _MM_SHUFFLE(0, 3, 2, 1)); }

    inline int32_t i16(int32_t v) { return static_cast<int32_t>(static_cast<int16_t>(v)); }
    inline int32_t u16(int32_t v) { return static_cast<int32_t>(static_cast<uint16_t>(v)); }

    // FMAC ----------------------------------------------------------------------
    enum FmacOp
    {
        Add,
        Sub,
        Mul,
        MAdd,
        MSub
    };

    // Per-lane classification of exact results (normalizeFmacExactResult):
    // bit i of each mask is lane i.
    struct Class
    {
        int sign, zero, under, over, edge;
    };

    inline Class classify(__m128d lo, __m128d hi)
    {
        const __m128d absMask = _mm_castsi128_pd(_mm_set1_epi64x(0x7FFFFFFFFFFFFFFFll));
        const __m128d maxF = _mm_set1_pd(static_cast<double>(std::numeric_limits<float>::max()));
        const __m128d minF = _mm_set1_pd(static_cast<double>(std::numeric_limits<float>::min()));
        const __m128d alo = _mm_and_pd(lo, absMask), ahi = _mm_and_pd(hi, absMask);
        const auto m = [](__m128d l, __m128d h)
        { return _mm_movemask_pd(l) | (_mm_movemask_pd(h) << 2); };
        Class k;
        k.sign = m(lo, hi);
        k.zero = m(_mm_cmpeq_pd(alo, _mm_setzero_pd()), _mm_cmpeq_pd(ahi, _mm_setzero_pd()));
        k.under = m(_mm_cmplt_pd(alo, minF), _mm_cmplt_pd(ahi, minF)) & ~k.zero;
        k.over = m(_mm_cmpgt_pd(alo, maxF), _mm_cmpgt_pd(ahi, maxF));
        k.edge = m(_mm_cmpeq_pd(alo, maxF), _mm_cmpeq_pd(ahi, maxF));
        return k;
    }

    // Lane mask (bit i = lane i) -> MAC nibble (x = bit 3).
    inline uint32_t macNibble(int m)
    {
        static constexpr uint8_t kRev[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};
        return kRev[m & 15];
    }

    // The interpreter's per-lane path, including the long double check for a
    // result that rounds to exactly FLT_MAX. Used when any lane hits that edge.
    V fmacSlow(FmacOp op, V r, V a, V b, V acc, int laneMask, Flags &fl);

    // An FMAC upper op with normalized operands: the result lanes, and the
    // MAC/status/sticky flags the op commits (applyFmacDest + updateFmacFlags).
    // Exact reference path: classifies the exact result in double precision.
    template <FmacOp Op, int Dest>
    inline V fmacExact(V a, V b, V acc, Flags &fl)
    {
        constexpr int laneMask = ((Dest & 8) ? 1 : 0) | ((Dest & 4) ? 2 : 0) | ((Dest & 2) ? 4 : 0) | ((Dest & 1) ? 8 : 0);
        V r;
        if constexpr (Op == Add)
            r = _mm_add_ps(a, b);
        else if constexpr (Op == Sub)
            r = _mm_sub_ps(a, b);
        else if constexpr (Op == Mul)
            r = _mm_mul_ps(a, b);
        else if constexpr (Op == MAdd)
            r = _mm_add_ps(acc, _mm_mul_ps(a, b));
        else
            r = _mm_sub_ps(acc, _mm_mul_ps(a, b));

        // The exact result in double, as normalizeFmacResult computes it: a
        // float product is exact in double, and zero, sign and the FLT_MIN /
        // FLT_MAX comparisons survive the (round toward zero) double sum.
        // No destination lanes: updateFmacFlags queues nothing.
        if constexpr (laneMask == 0)
            return r;
        const __m128d alo = _mm_cvtps_pd(a), ahi = _mm_cvtps_pd(_mm_movehl_ps(a, a));
        const __m128d blo = _mm_cvtps_pd(b), bhi = _mm_cvtps_pd(_mm_movehl_ps(b, b));
        __m128d elo, ehi;
        int productSticky = 0;
        if constexpr (Op == Add)
        {
            elo = _mm_add_pd(alo, blo);
            ehi = _mm_add_pd(ahi, bhi);
        }
        else if constexpr (Op == Sub)
        {
            elo = _mm_sub_pd(alo, blo);
            ehi = _mm_sub_pd(ahi, bhi);
        }
        else
        {
            const __m128d plo = _mm_mul_pd(alo, blo), phi = _mm_mul_pd(ahi, bhi);
            if constexpr (Op == Mul)
            {
                elo = plo;
                ehi = phi;
            }
            else
            {
                const __m128d clo = _mm_cvtps_pd(acc), chi = _mm_cvtps_pd(_mm_movehl_ps(acc, acc));
                elo = Op == MAdd ? _mm_add_pd(clo, plo) : _mm_sub_pd(clo, plo);
                ehi = Op == MAdd ? _mm_add_pd(chi, phi) : _mm_sub_pd(chi, phi);
                // Every product condition goes to the sticky flags.
                const Class p = classify(plo, phi);
                productSticky = ((p.zero | p.under) & laneMask ? 1 : 0) | (p.sign & laneMask ? 2 : 0) |
                                (p.under & laneMask ? 4 : 0) | (p.over & laneMask ? 8 : 0);
            }
        }
        const Class k = classify(elo, ehi);
        if ((k.edge & laneMask) != 0)
            return fmacSlow(Op, r, a, b, acc, laneMask, fl);

        const int z = (k.zero | k.under) & laneMask;
        const int s = k.sign & laneMask;
        const int u = k.under & laneMask;
        const int o = k.over & laneMask;
        if ((z | o) != 0)
        {
            // Zero / underflow -> signed zero, overflow -> signed max, with
            // the sign of the exact result.
            alignas(16) uint32_t lanes[4];
            _mm_store_si128(reinterpret_cast<__m128i *>(lanes), _mm_castps_si128(r));
            for (int i = 0; i < 4; ++i)
            {
                const uint32_t sign = (s >> i) & 1 ? 0x80000000u : 0u;
                if ((z >> i) & 1)
                    lanes[i] = sign;
                else if ((o >> i) & 1)
                    lanes[i] = sign | 0x7F7FFFFFu;
            }
            r = _mm_castsi128_ps(_mm_load_si128(reinterpret_cast<const __m128i *>(lanes)));
        }
        fl.mac = macNibble(z) | (macNibble(s) << 4) | (macNibble(u) << 8) | (macNibble(o) << 12);
        fl.cur = (z ? 1u : 0u) | (s ? 2u : 0u) | (u ? 4u : 0u) | (o ? 8u : 0u);
        fl.sticky |= (fl.cur | static_cast<uint32_t>(productSticky)) << 6;
        fl.extra = static_cast<uint32_t>(productSticky);
        fl.pending = true;
        return r;
    }

    // FMAC with a per-lane fast path that is exact by construction (issue 001).
    // A lane is "fast" when its exact flags and value follow from the float
    // result r alone; the op uses fmacExact unless every destination lane is.
    // Normal means FLT_MIN < |x| < FLT_MAX (strict, so it holds whether r was
    // rounded toward zero, as in a VU1 run, or to nearest):
    //  - ADD/SUB: exact zero iff a == -b (ADD) / a == b (SUB); r is then the
    //    same signed zero the exact path produces. Otherwise r must be normal:
    //    then the exact sum is nonzero, normal, not FLT_MAX, with r's sign.
    //  - MUL: exact zero iff a or b is +-0 (r is that signed zero), else r
    //    normal as above.
    //  - MADD/MSUB, q = float a*b:
    //      a or b zero: the product is an exact +-0, so r = acc +- 0 is exact:
    //        fast if r is normal, or r and acc are zero (a signed exact zero);
    //      acc zero: r = +-q, one rounding of the exact product: fast if r and
    //        q are normal;
    //      otherwise: q normal, 2*FLT_MIN < |r| < FLT_MAX/2 and |r| >= |q|*2^-20
    //        (both roundings together err by less than |r|/8, so the exact sum
    //        has r's sign and stays normal).
    //    The product's sticky class is then known too: zero where a or b is
    //    zero, sign = sign(a) ^ sign(b), never underflow/overflow (q normal).
    // app/tests/vu1_fmac_fast_test.cpp checks both paths agree bit for bit.
    // Always inlined into the translated VU code: the compiler otherwise calls it out of line, passing the vectors
    // through memory (issue 025 v9, C1: -17.9 % in the VU1 benchmark, all checked inputs identical).
    template <FmacOp Op, int Dest>
    [[gnu::always_inline]] inline V fmac(V a, V b, V acc, Flags &fl)
    {
        constexpr int laneMask = ((Dest & 8) ? 1 : 0) | ((Dest & 4) ? 2 : 0) | ((Dest & 2) ? 4 : 0) | ((Dest & 1) ? 8 : 0);
        if constexpr (laneMask == 0)
            return fmacExact<Op, Dest>(a, b, acc, fl);
        const __m128i absMask = _mm_set1_epi32(0x7FFFFFFF);
        const V zero = _mm_setzero_ps();
        const auto absBits = [&](V x) { return _mm_and_si128(_mm_castps_si128(x), absMask); };
        const auto inRange = [](__m128i x, int32_t lo, int32_t hi)
        { return _mm_and_si128(_mm_cmpgt_epi32(x, _mm_set1_epi32(lo)), _mm_cmplt_epi32(x, _mm_set1_epi32(hi))); };
        const auto normal = [&](V x) { return inRange(absBits(x), 0x00800000, 0x7F7FFFFF); };
        V r;
        __m128i ok;
        int productSticky = 0;
        if constexpr (Op == Add || Op == Sub || Op == Mul)
        {
            V exactZero;
            if constexpr (Op == Add)
            {
                r = _mm_add_ps(a, b);
                exactZero = _mm_cmpeq_ps(a, _mm_sub_ps(zero, b));
            }
            else if constexpr (Op == Sub)
            {
                r = _mm_sub_ps(a, b);
                exactZero = _mm_cmpeq_ps(a, b);
            }
            else
            {
                r = _mm_mul_ps(a, b);
                exactZero = _mm_or_ps(_mm_cmpeq_ps(a, zero), _mm_cmpeq_ps(b, zero));
            }
            ok = _mm_or_si128(_mm_castps_si128(exactZero), normal(r));
        }
        else
        {
            const V q = _mm_mul_ps(a, b);
            r = Op == MAdd ? _mm_add_ps(acc, q) : _mm_sub_ps(acc, q);
            const __m128i zeroP = _mm_castps_si128(_mm_or_ps(_mm_cmpeq_ps(a, zero), _mm_cmpeq_ps(b, zero)));
            const __m128i zeroAcc = _mm_castps_si128(_mm_cmpeq_ps(acc, zero));
            const __m128i zeroR = _mm_castps_si128(_mm_cmpeq_ps(r, zero));
            const __m128i rNormal = normal(r), qNormal = normal(q);
            const __m128i strict = _mm_and_si128(inRange(absBits(r), 0x01000000, 0x7F000000),
                _mm_castps_si128(_mm_cmpge_ps(_mm_mul_ps(_mm_castsi128_ps(absBits(r)), _mm_set1_ps(1048576.0f)),
                                              _mm_castsi128_ps(absBits(q)))));
            const __m128i withZeroProduct = _mm_and_si128(zeroP, _mm_or_si128(rNormal, _mm_and_si128(zeroAcc, zeroR)));
            const __m128i withProduct = _mm_andnot_si128(zeroP,
                _mm_and_si128(qNormal, _mm_or_si128(_mm_and_si128(zeroAcc, rNormal), _mm_andnot_si128(zeroAcc, strict))));
            ok = _mm_or_si128(withZeroProduct, withProduct);
            const int pz = _mm_movemask_ps(_mm_castsi128_ps(zeroP)) & laneMask;
            const int ps = _mm_movemask_ps(_mm_xor_ps(a, b)) & laneMask;
            productSticky = (pz ? 1 : 0) | (ps ? 2 : 0);
        }
        if ((_mm_movemask_ps(_mm_castsi128_ps(ok)) & laneMask) != laneMask)
            return fmacExact<Op, Dest>(a, b, acc, fl);
        const int z = _mm_movemask_ps(_mm_cmpeq_ps(r, zero)) & laneMask;
        const int s = _mm_movemask_ps(r) & laneMask;
        fl.mac = macNibble(z) | (macNibble(s) << 4);
        fl.cur = (z ? 1u : 0u) | (s ? 2u : 0u);
        fl.sticky |= (fl.cur | static_cast<uint32_t>(productSticky)) << 6;
        fl.extra = static_cast<uint32_t>(productSticky);
        fl.pending = true;
        return r;
    }

    // fmac kept out of line. Inlined into the very large segments that keep only
    // their last flags (e.g. actor skinning) the fast path made them ~37 % slower
    // (register pressure); called, it is ~18 % faster there than fmacExact
    // (issue 001 work.md, M12).
    template <FmacOp Op, int Dest>
    [[gnu::noinline]] V fmacOutOfLine(V a, V b, V acc, Flags &fl) { return fmac<Op, Dest>(a, b, acc, fl); }

    // FDIV / EFU ---------------------------------------------------------------
    inline float normalizeResult(float value)
    {
        uint32_t bits = bitsOf(value);
        const uint32_t exponent = (bits >> 23) & 0xFFu;
        if ((bits & 0x7FFFFFFFu) == 0u)
            return value;
        if (exponent == 0u)
            bits &= 0x80000000u;
        else if (exponent == 0xFFu)
            bits = (bits & 0x80000000u) | 0x7F7FFFFFu;
        return f32(bits);
    }

    inline float div(float numRaw, float denRaw, uint32_t &di)
    {
        const float num = normf(numRaw);
        const float den = normf(denRaw);
        float result;
        if (den == 0.0f)
        {
            di = num == 0.0f ? 0x10u : 0x20u;
            result = std::signbit(num) != std::signbit(den) ? -std::numeric_limits<float>::max()
                                                            : std::numeric_limits<float>::max();
        }
        else
        {
            di = 0u;
            result = num / den;
        }
        return normalizeResult(result);
    }

    inline float esadd(V v)
    {
        const float x = normf(lane<0>(v)), y = normf(lane<1>(v)), z = normf(lane<2>(v));
        return normalizeResult(x * x + y * y + z * z);
    }

}
