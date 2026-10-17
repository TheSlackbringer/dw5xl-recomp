// Lifted VU1 FMAC: the fast path (vl::fmac) must agree bit for bit with the exact
// reference path (vl::fmacExact) — result lanes, MAC, status, sticky, extra — for
// every op and destination mask, under round-toward-zero (VU1 run) and
// round-to-nearest, over random operands drawn to hit zeros, denormals, the
// FLT_MIN / FLT_MAX edges, overflow, cancellation and signed-zero lanes.
#include "runtime/ps2_vu1_lift.h"
#include "ps2_runtime.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
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
    std::mt19937_64 rng(0x5eed1234abcdull);
    uint64_t failures = 0, checked = 0;

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
        case 0: return sign;                                              // +-0
        case 1: return sign | static_cast<uint32_t>(rng() & 0x7FFFFFu);   // denormal
        case 2: return sign | (0x00800000u + static_cast<uint32_t>(rng() % 64u));       // near FLT_MIN
        case 3: return sign | (0x01000000u - 32u + static_cast<uint32_t>(rng() % 64u)); // near 2*FLT_MIN
        case 4: return sign | (0x7F7FFFFFu - static_cast<uint32_t>(rng() % 64u));      // near FLT_MAX
        case 5: return sign | (0x7F000000u - 32u + static_cast<uint32_t>(rng() % 64u)); // near FLT_MAX/2
        case 6: return sign | ((static_cast<uint32_t>(rng() % 60u) + 1u) << 23) | static_cast<uint32_t>(rng() & 0x7FFFFFu); // tiny
        case 7: return sign | ((static_cast<uint32_t>(rng() % 60u) + 194u) << 23) | static_cast<uint32_t>(rng() & 0x7FFFFFu); // huge
        default: return sign | ((static_cast<uint32_t>(rng() % 64u) + 95u) << 23) | static_cast<uint32_t>(rng() & 0x7FFFFFu); // ordinary
        }
    }

    V randomVector()
    {
        return _mm_setr_ps(fromBits(randomBits()), fromBits(randomBits()), fromBits(randomBits()), fromBits(randomBits()));
    }

    template <vl::FmacOp Op, int Dest>
    void checkOne(V a, V b, V acc, uint32_t sticky)
    {
        vl::Flags fast{0u, 0u, sticky}, exact{0u, 0u, sticky};
        const V rf = vl::fmac<Op, Dest>(a, b, acc, fast);
        const V re = vl::fmacExact<Op, Dest>(a, b, acc, exact);
        ++checked;
        if (std::memcmp(&rf, &re, sizeof(V)) != 0 || fast.mac != exact.mac || fast.cur != exact.cur ||
            fast.sticky != exact.sticky || fast.extra != exact.extra || fast.pending != exact.pending)
        {
            if (++failures <= 10)
            {
                float av[4], bv[4], cv[4];
                std::memcpy(av, &a, 16);
                std::memcpy(bv, &b, 16);
                std::memcpy(cv, &acc, 16);
                std::fprintf(stderr, "mismatch op=%d dest=%d a=%g,%g,%g,%g b=%g,%g,%g,%g acc=%g,%g,%g,%g mac %x/%x cur %x/%x sticky %x/%x extra %x/%x\n",
                             static_cast<int>(Op), Dest, av[0], av[1], av[2], av[3], bv[0], bv[1], bv[2], bv[3],
                             cv[0], cv[1], cv[2], cv[3], fast.mac, exact.mac, fast.cur, exact.cur, fast.sticky,
                             exact.sticky, fast.extra, exact.extra);
            }
        }
    }

    template <vl::FmacOp Op, int Dest>
    void checkMany(int count)
    {
        for (int i = 0; i < count; ++i)
        {
            const V a = randomVector(), b = randomVector();
            V acc = randomVector();
            const uint32_t sticky = static_cast<uint32_t>(rng()) & 0xFC0u;
            checkOne<Op, Dest>(a, b, acc, sticky);
            // Cancellation: acc close to -+a*b so the sum is tiny or exactly zero.
            const V q = _mm_mul_ps(a, b);
            acc = Op == vl::MAdd ? _mm_sub_ps(_mm_setzero_ps(), q) : q;
            checkOne<Op, Dest>(a, b, acc, sticky);
            acc = _mm_mul_ps(acc, _mm_set1_ps(1.0f + 1.0f / 4096.0f));
            checkOne<Op, Dest>(a, b, acc, sticky);
            // ADD/SUB cancellation.
            checkOne<Op, Dest>(a, Op == vl::Sub ? a : _mm_sub_ps(_mm_setzero_ps(), a), acc, sticky);
            // Zero lanes (the fast path's exact-zero rules): zero products,
            // zero ACC, both, signed zeros.
            const V signs = _mm_castsi128_ps(_mm_set_epi32(static_cast<int>(rng() & 0x80000000u),
                static_cast<int>(rng() & 0x80000000u), static_cast<int>(rng() & 0x80000000u), static_cast<int>(rng() & 0x80000000u)));
            const V zeroLanes = _mm_castsi128_ps(_mm_set_epi32(-(int)(rng() & 1u), -(int)(rng() & 1u), -(int)(rng() & 1u), -(int)(rng() & 1u)));
            const V za = _mm_or_ps(_mm_andnot_ps(zeroLanes, a), _mm_and_ps(zeroLanes, signs));
            const V zacc = _mm_or_ps(_mm_andnot_ps(zeroLanes, acc), _mm_and_ps(zeroLanes, _mm_xor_ps(signs, b)));
            checkOne<Op, Dest>(za, b, acc, sticky);
            checkOne<Op, Dest>(a, b, _mm_and_ps(signs, _mm_castsi128_ps(_mm_set1_epi32(static_cast<int>(0x80000000u)))), sticky);
            checkOne<Op, Dest>(za, b, zacc, sticky);
            checkOne<Op, Dest>(vl::norm(a), vl::norm(b), vl::norm(acc), sticky);
        }
    }

    template <vl::FmacOp Op, int... Dest>
    void checkOp(int count, std::integer_sequence<int, Dest...>)
    {
        (checkMany<Op, Dest>(count), ...);
    }

    void checkAll(int count)
    {
        const auto dests = std::make_integer_sequence<int, 16>{};
        checkOp<vl::Add>(count, dests);
        checkOp<vl::Sub>(count, dests);
        checkOp<vl::Mul>(count, dests);
        checkOp<vl::MAdd>(count, dests);
        checkOp<vl::MSub>(count, dests);
    }
}

int main()
{
    const unsigned int saved = _MM_GET_ROUNDING_MODE();
    _MM_SET_ROUNDING_MODE(_MM_ROUND_TOWARD_ZERO); // VU1 run()
    checkAll(40000);
    _MM_SET_ROUNDING_MODE(_MM_ROUND_NEAREST);
    checkAll(10000);
    _MM_SET_ROUNDING_MODE(saved);
    std::printf("vu1 fmac fast path: %llu checks, %llu mismatches\n", static_cast<unsigned long long>(checked),
                static_cast<unsigned long long>(failures));
    return failures == 0 ? 0 : 1;
}
