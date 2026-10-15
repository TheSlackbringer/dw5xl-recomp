// Authored instruction regression. No game code/data, GS or renderer is used.
#include "ps2_runtime_macros.h"
#include <array>
#include <atomic>
#include <bit>
#include <cfenv>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>

extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x1000;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x1004;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 1;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[] = {nullptr};

namespace {
float value(uint32_t b) { return std::bit_cast<float>(b); }
uint32_t bits(float v) { return std::bit_cast<uint32_t>(v); }
void require(bool ok, const char *what) { if (!ok) throw std::runtime_error(what); }
// Keep operands runtime values even with LTO/optimization.
float operand(uint32_t b) { volatile uint32_t u = b; return value(u); }
void expect(float actual, uint32_t wanted, const char *what)
{
    if (bits(actual) != wanted)
    {
        std::cerr << what << ": actual=0x" << std::hex << bits(actual)
                  << " expected=0x" << wanted << std::dec << '\n';
        throw std::runtime_error(what);
    }
}
void checkInstructionResults(int ambient)
{
    const Ps2ScopedHostRounding mode(ambient);
    expect(FPU_DIV_S(operand(0x3f800000), operand(0x40400000)), 0x3eaaaaab, "DIV 1/3 nearest");
    expect(FPU_DIV_S(operand(0xbf800000), operand(0x40400000)), 0xbeaaaaab, "DIV -1/3 nearest");
    expect(FPU_SQRT_S(operand(0x40a00000)), 0x400f1bbd, "SQRT 5 nearest");
    // Float-rounded sqrt(6), then division, differs from fused high-precision 1/sqrt(6).
    expect(FPU_RSQRT_S(operand(0x3f800000), operand(0x40c00000)), 0x3ed105eb, "RSQRT single-root boundary");
    require(std::fegetround() == ambient, "divide/root leaked rounding into caller");
#if !defined(USE_SSE2NEON) && (defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64))
    const unsigned wanted = ambient == FE_TONEAREST ? _MM_ROUND_NEAREST
        : ambient == FE_TOWARDZERO ? _MM_ROUND_TOWARD_ZERO
        : ambient == FE_UPWARD ? _MM_ROUND_UP : _MM_ROUND_DOWN;
    require((_mm_getcsr() & _MM_ROUND_MASK) == wanted, "SSE rounding not restored");
#endif
}
void checkSpecialOperands()
{
    const Ps2ScopedHostRounding mode(FE_TOWARDZERO);
    // Exponent255 is finite on EE; existing widen/saturation rules must survive.
    expect(FPU_DIV_S(operand(0x7f800000), operand(0x3f800000)), 0x7f800000, "extended exponent DIV");
    expect(FPU_SQRT_S(operand(0x7f800000)), 0x5f800000, "extended exponent SQRT");
    expect(FPU_RSQRT_S(operand(0x3f800000), operand(0x7f800000)), 0x1f800000, "extended exponent RSQRT");
    expect(FPU_DIV_S(operand(0x7fffffff), operand(0x7fffffff)), 0x3f800000, "largest EE / largest EE");
    expect(FPU_DIV_S(operand(0x3f800000), operand(0)), 0x7fffffff, "positive zero-divisor saturation");
    expect(FPU_DIV_S(operand(0xbf800000), operand(0)), 0xffffffff, "negative zero-divisor saturation");
    expect(FPU_DIV_S(operand(0x3f800000), operand(0x80000000)), 0xffffffff, "divisor sign saturation");
    expect(FPU_DIV_S(operand(1), operand(0x3f800000)), 0, "denormal numerator flushed");
    expect(FPU_SQRT_S(operand(1)), 0, "denormal sqrt flushed");
    expect(FPU_SQRT_S(operand(0x80000000)), 0, "sqrt negative zero is positive zero");
    expect(FPU_SQRT_S(operand(0xc0a00000)), 0x400f1bbd, "sqrt uses magnitude");
    expect(FPU_RSQRT_S(operand(0xbf800000), operand(0)), 0xffffffff, "RSQRT zero magnitude sign");
    expect(FPU_RSQRT_S(operand(0x3f800000), operand(0x80000000)), 0x7fffffff, "RSQRT ignores divisor sign");
    require(std::fegetround() == FE_TOWARDZERO, "special-case early return leaked mode");
}
void checkGeneralArithmetic()
{
    {
        const Ps2ScopedHostRounding mode(FE_TOWARDZERO);
        expect(FPU_ADD_S(operand(0x3f800000), operand(0x34400000)), 0x3f800001, "ordinary ADD chop");
        expect(FPU_SUB_S(operand(0x3f800000), operand(0x33000000)), 0x3f7fffff, "ordinary SUB chop");
    }
    {
        const Ps2ScopedHostRounding mode(FE_TONEAREST);
        expect(FPU_ADD_S(operand(0x3f800000), operand(0x34400000)), 0x3f800002, "nearest comparison ADD");
        expect(FPU_SUB_S(operand(0x3f800000), operand(0x33000000)), 0x3f800000, "nearest comparison SUB");
    }
}
void checkScopeAndThreadIsolation()
{
    const Ps2ScopedHostRounding outer(FE_TOWARDZERO);
    std::feraiseexcept(FE_INVALID);
    try { const Ps2ScopedHostRounding inner(FE_TONEAREST); throw 1; }
    catch (int) {}
    require(std::fegetround() == FE_TOWARDZERO, "exception unwind lost caller mode");
    require((std::fetestexcept(FE_INVALID) & FE_INVALID) != 0, "scope erased existing exception flags");
    std::atomic<bool> otherThreadCorrect{false};
    std::thread other([&] {
        const Ps2ScopedHostRounding otherMode(FE_UPWARD);
        const auto result = bits(FPU_DIV_S(operand(0x3f800000), operand(0x40400000)));
        otherThreadCorrect.store(result == 0x3eaaaaab && std::fegetround() == FE_UPWARD);
    });
    other.join();
    require(otherThreadCorrect.load(), "independent thread did not restore its mode");
    require(std::fegetround() == FE_TOWARDZERO, "other thread modified caller mode");
#if !defined(USE_SSE2NEON) && (defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64))
    const unsigned before = _mm_getcsr();
    _mm_setcsr((before & ~_MM_ROUND_MASK) | _MM_ROUND_NEAREST);
    expect(FPU_DIV_S(operand(0x3f800000), operand(0x40400000)), 0x3eaaaaab, "mismatched x87/SSE normalised inside scope");
    require(std::fegetround() == FE_TOWARDZERO, "mismatched scope changed x87 mode");
    require((_mm_getcsr() & _MM_ROUND_MASK) == _MM_ROUND_NEAREST, "mismatched scope did not restore SSE mode");
    _mm_setcsr(before);
#endif
}
}
int main()
{
    try
    {
        for (const int mode : {FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD})
            checkInstructionResults(mode);
        checkSpecialOperands();
        checkGeneralArithmetic();
        checkScopeAndThreadIsolation();
        std::cout << "EE instruction-specific rounding regression passed\n";
    }
    catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
