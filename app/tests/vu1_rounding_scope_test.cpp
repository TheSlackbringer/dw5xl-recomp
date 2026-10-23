// VU1 run rounding scope (issue 025 spec v9, guard for C2): a VU1 run computes with round-toward-zero whatever
// the caller's host rounding is, and afterwards the caller's SSE (MXCSR) and x87 rounding are exactly what they
// were. Authored two-instruction program, no game code or data.
#include "ps2_runtime.h"
#include "runtime/ps2_vu1.h"

#include <bit>
#include <cfenv>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x1000;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x1004;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 1;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[] = {nullptr};

namespace
{
    void require(bool ok, const std::string &what)
    {
        if (!ok)
            throw std::runtime_error(what);
    }

    unsigned sseRounding() { return _mm_getcsr() & _MM_ROUND_MASK; }

    unsigned x87Rounding()
    {
        uint16_t cw = 0;
        __asm__ volatile("fnstcw %0" : "=m"(cw));
        return (cw >> 10) & 3u; // 0 nearest, 1 down, 2 up, 3 toward zero
    }

    // 1.0 + 1.5 * 2^-24: nearest rounds up to 0x3F800001, toward zero gives 0x3F800000.
    constexpr uint32_t kOne = 0x3F800000u, kSmall = 0x33C00000u;

    void runOnce(PS2Runtime &rt, int ambient, const char *name)
    {
        uint8_t *code = rt.memory().getVU1Code();
        std::memset(code, 0, 16384);
        const uint32_t addUpper = (1u << 30) | (0xFu << 21) | (2u << 16) | (1u << 11) | (3u << 6) | 0x28u; // E, ADD.xyzw VF3, VF1, VF2
        const uint32_t nopUpper = 0x000002FFu, nopLower = 0x8000033Cu;
        const uint32_t program[4] = {nopLower, addUpper, nopLower, nopUpper};
        std::memcpy(code, program, sizeof(program));
        auto &s = rt.vu1().state();
        for (int i = 0; i < 4; ++i)
        {
            s.vf[1][i] = std::bit_cast<float>(kOne);
            s.vf[2][i] = std::bit_cast<float>(kSmall);
            s.vf[3][i] = 0.0f;
        }
        require(std::fesetround(ambient) == 0, std::string(name) + ": cannot set the caller's rounding");
        const unsigned sseBefore = sseRounding(), x87Before = x87Rounding();
        rt.vu1().execute(code, 16384, rt.memory().getVU1Data(), 16384, rt.gs(), &rt.memory(), 0, 0, 0, 1000);
        const unsigned sseAfter = sseRounding(), x87After = x87Rounding();
        std::fesetround(FE_TONEAREST);
        for (int i = 0; i < 4; ++i)
            require(std::bit_cast<uint32_t>(s.vf[3][i]) == kOne,
                    std::string(name) + ": VU1 ADD must round toward zero (lane " + std::to_string(i) + ")");
        require(sseAfter == sseBefore, std::string(name) + ": SSE rounding not restored after the VU1 run");
        require(x87After == x87Before, std::string(name) + ": x87 rounding not restored after the VU1 run");
    }
}

int main()
{
    int failures = 0;
    PS2Runtime rt;
    if (!rt.memory().initialize())
    {
        std::cout << "FAIL vu1_rounding_scope: memory initialization\n";
        return 1;
    }
    const struct { int mode; const char *name; } modes[] = {
        {FE_TONEAREST, "caller nearest"}, {FE_TOWARDZERO, "caller toward zero"}, {FE_UPWARD, "caller upward"}};
    for (const auto &m : modes)
    {
        try
        {
            runOnce(rt, m.mode, m.name);
            std::cout << "PASS vu1_rounding_scope " << m.name << "\n";
        }
        catch (const std::exception &e)
        {
            ++failures;
            std::cout << "FAIL vu1_rounding_scope " << e.what() << "\n";
        }
    }
    return failures ? 1 : 0;
}
