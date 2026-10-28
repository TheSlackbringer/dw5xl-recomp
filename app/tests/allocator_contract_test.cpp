// Authored, disc-free DW5XL allocator contract and installation regression.
#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_stubs.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x105148;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x107e54;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = (0x107e54 - 0x105148) / 4;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[(0x107e54 - 0x105148) / 4] = {};
namespace
{
constexpr uint32_t kPointer = 0x30f000, kCurrent = 0x4000, kOther = 0x5000, kBase = 0x180000,
                   kLimit = 0x190000, kReturn = 0x90000;
unsigned checks = 0, failures = 0;
void check(bool good, const std::string &text)
{
    ++checks;
    if (!good)
    {
        ++failures;
        std::cerr << "FAIL " << text << '\n';
    }
}
void put(uint8_t *ram, uint32_t a, uint32_t v)
{
    std::memcpy(ram + a, &v, 4);
}
uint32_t word(uint8_t *ram, uint32_t a)
{
    uint32_t v;
    std::memcpy(&v, ram + a, 4);
    return v;
}
void set(R5900Context &c, unsigned r, uint32_t v)
{
    c.r[r] = _mm_cvtsi32_si128(v);
}
uint32_t reg(const R5900Context &c, unsigned r)
{
    return _mm_cvtsi128_si32(c.r[r]);
}
void mallocEntry(uint8_t *r, R5900Context *c, PS2Runtime *rt)
{
    c->pc = reg(*c, 31);
    ps2_stubs::malloc(r, c, rt);
}
void mallocREntry(uint8_t *r, R5900Context *c, PS2Runtime *rt)
{
    c->pc = reg(*c, 31);
    ps2_stubs::malloc_r(r, c, rt);
}
void callocEntry(uint8_t *r, R5900Context *c, PS2Runtime *rt)
{
    c->pc = reg(*c, 31);
    ps2_stubs::calloc_r(r, c, rt);
}
void memalignEntry(uint8_t *r, R5900Context *c, PS2Runtime *rt)
{
    c->pc = reg(*c, 31);
    ps2_stubs::memalign_r(r, c, rt);
}
void errnoEntry(uint8_t *, R5900Context *, PS2Runtime *)
{
}
void table(uint32_t a, PS2Runtime::RecompiledFunction f)
{
    g_ps2RecompiledFunctionTable[(a - g_ps2RecompiledFunctionTableBase) / 4] = f;
}
struct Env
{
    PS2Runtime runtime;
    R5900Context cpu{};
    uint8_t *ram;
    Env()
    {
        if (!runtime.memory().initialize())
            throw std::runtime_error("memory initialization");
        ram = runtime.memory().getRDRAM();
        runtime.configureGuestHeap(kBase, kLimit);
        std::fill(std::begin(g_ps2RecompiledFunctionTable), std::end(g_ps2RecompiledFunctionTable),
                  nullptr);
        table(0x105148, callocEntry);
        table(0x107808, memalignEntry);
        table(0x107b58, mallocEntry);
        table(0x107e50, mallocREntry);
        table(0x1066f0, errnoEntry);
        // Independently authored MIPS accessor fixture: LUI v1, JR ra, LW v0.
        put(ram, 0x1066f0, 0x3c030031);
        put(ram, 0x1066f4, 0x03e00008);
        put(ram, 0x1066f8, 0x8c62f000);
        put(ram, kPointer, kCurrent);
        put(ram, kCurrent, 0x13579bdf);
        put(ram, kOther, 0x2468ace0);
        put(ram, kCurrent + 4, 0x11223344);
        put(ram, kOther + 4, 0x55667788);
    }
    void install(const char *name = "SLES_538.60")
    {
        ps2_game_overrides::applyMatching(runtime, name, 0, 0, false);
    }
    uint32_t call(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2 = 0)
    {
        cpu = {};
        cpu.pc = fn;
        set(cpu, 4, a0);
        set(cpu, 5, a1);
        set(cpu, 6, a2);
        set(cpu, 16, 0xabcdef12);
        set(cpu, 29, 0x100000);
        set(cpu, 31, kReturn);
        runtime.lookupFunction(fn)(ram, &cpu, &runtime);
        check(cpu.pc == kReturn, "allocation returns to caller");
        check(reg(cpu, 16) == 0xabcdef12 && reg(cpu, 29) == 0x100000,
              "callee-saved state preserved");
        return reg(cpu, 2);
    }
    void sentinels(uint32_t error)
    {
        check(word(ram, kCurrent) == error, "current context error");
        check(word(ram, kOther) == 0x2468ace0, "supplied non-current context preserved");
        check(word(ram, kCurrent + 4) == 0x11223344 && word(ram, kOther + 4) == 0x55667788,
              "neighbor context words preserved");
    }
};
} // namespace
int main()
{
    try
    {
        // Expectations are specified independently from the adapter implementation.
        for (uint32_t fn : {0x107b58u, 0x107e50u, 0x105148u, 0x107808u})
            for (bool other : {false, true})
            {
                Env e;
                e.install();
                uint32_t context = other ? kOther : kCurrent;
                uint32_t result = fn == 0x107b58   ? e.call(fn, 0x1000000, 0)
                                  : fn == 0x105148 ? e.call(fn, context, 1, 0x1000000)
                                  : fn == 0x107808 ? e.call(fn, context, 64, 0x1000000)
                                                   : e.call(fn, context, 0x1000000);
                check(result == 0, "impossible allocation fails");
                e.sentinels(12);
            }
        struct Case
        {
            uint32_t fn, a0, a1, a2;
            bool zeroed;
        };
        const Case successes[] = {{0x107b58, 96, 0, 0, false},
                                  {0x107e50, kOther, 96, 0, false},
                                  {0x105148, kOther, 3, 32, true},
                                  {0x107808, kOther, 64, 96, false},
                                  {0x107b58, 0, 0, 0, false},
                                  {0x107e50, kOther, 0, 0, false},
                                  {0x105148, kOther, 0, 96, true},
                                  {0x105148, kOther, 96, 0, true},
                                  {0x107808, kOther, 64, 0, false},
                                  {0x107e50, kOther, 0xffffffff, 0, false},
                                  {0x105148, kOther, 0x10000, 0x10000, true},
                                  {0x105148, kOther, 0x10000, 0x10001, true}};
        for (auto c : successes)
        {
            Env e;
            e.install();
            uint32_t p = e.call(c.fn, c.a0, c.a1, c.a2);
            check(p >= kBase && p < kLimit,
                  "checked success/zero/wrap request returns guest allocation");
            e.sentinels(0x13579bdf);
            if (p && c.zeroed)
                check(word(e.ram, p) == 0, "calloc payload zeroed");
            e.runtime.guestFree(p);
            check(e.runtime.guestMalloc(kLimit - kBase) == kBase,
                  "allocation belongs to existing guest heap and frees completely");
        }
        for (uint32_t alignment : {0u, 3u, 16u, 17u, 24u, 64u})
        {
            Env e;
            e.install();
            auto p = e.call(0x107808, kOther, alignment, 96);
            check(p >= kBase && p < kLimit, "small/odd alignment succeeds without invented EINVAL");
            e.sentinels(0x13579bdf);
        }
        {
            Env e;
            e.install();
            auto p = e.call(0x107808, kOther, 64, 96);
            check((p & 63) == 0, "valid alignment honored");
            for (unsigned i = 0; i < 96; ++i)
                e.ram[p + i] = uint8_t(i ^ 0xa5);
            auto q = e.runtime.guestRealloc(p, 160);
            check(q != 0, "runtime-owned reallocation succeeds");
            if (q)
                for (unsigned i = 0; i < 96; ++i)
                    check(e.ram[q + i] == uint8_t(i ^ 0xa5), "reallocation payload preserved");
            e.runtime.guestFree(q);
            check(e.runtime.guestMalloc(kLimit - kBase) == kBase, "roundtrip releases full heap");
        }
        for (int mode = 0; mode < 3; ++mode)
        {
            Env e;
            if (mode == 1)
                put(e.ram, 0x1066f8, 0);
            if (mode == 2)
                table(0x107e50, nullptr);
            e.install(mode == 0 ? "OTHER.ELF" : "SLES_538.60");
            auto result = e.call(0x107b58, 0x1000000, 0);
            check(result == 0, "guard case still fails allocation");
            e.sentinels(0x13579bdf);
            check(e.runtime.lookupFunction(0x107b58) == mallocEntry,
                  "installation guard declines all replacements");
        }
        {
            Env e;
            e.install();
            put(e.ram, kPointer, kOther);
            check(e.call(0x107e50, kCurrent, 0x1000000) == 0,
                  "changed current pointer failure result");
            check(word(e.ram, kCurrent) == 0x13579bdf && word(e.ram, kOther) == 12,
                  "current context is read on every failure");
        }
        for (uint32_t pointer : {0u, 0x10004000u, 0x40004000u, 0xc0004000u, 0x01fffffeu})
        {
            Env e;
            e.install();
            put(e.ram, kPointer, pointer);
            check(e.call(0x107e50, kOther, 0x1000000) == 0,
                  "invalid current pointer retains failure result");
            e.sentinels(0x13579bdf);
            check(word(e.ram, 0) == 0, "invalid pointer does not write through RAM alias");
        }
        for (uint32_t alias : {0x20000000u, 0x80000000u, 0xa0000000u})
        {
            Env e;
            e.install();
            put(e.ram, kPointer, alias | kCurrent);
            check(e.call(0x107e50, kOther, 0x1000000) == 0, "RAM alias failure result");
            e.sentinels(12);
        }
        std::cout << checks << " allocator contract checks, " << failures << " failures\n";
        return failures ? 1 : 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
        return 2;
    }
}
