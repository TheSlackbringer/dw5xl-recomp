#include "ps2_runtime.h"
#include "ps2_stubs.h"
#include "runtime/ee_scheduler.h"
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x1000;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x1004;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 1;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[] = {nullptr};

namespace
{
void require(bool ok, const char *why)
{
    if (!ok)
    {
        std::cerr << "FAILED: " << why << std::endl;
        throw std::runtime_error(why);
    }
}

void reg(R5900Context &c, unsigned n, uint32_t v)
{
    c.r[n] = _mm_cvtsi32_si128(v);
}

uint32_t result(R5900Context &c)
{
    return _mm_cvtsi128_si32(c.r[2]);
}
} // namespace

int main()
{
    try
    {
        PS2Runtime runtime;
        R5900Context ctx{};
        std::vector<uint8_t> rdram(0x2000000u, 0);

        // 1. PAL Video Mode (mode 3): 50 fields / second.
        runtime.eeScheduler().setCrtMode(3);
        require(runtime.eeScheduler().palVideoMode(), "PAL video mode active");

        // Init stream buffer: capacity 4096 sectors, 4096 banks -> bank size = 1 sector.
        reg(ctx, 4, 4096);
        reg(ctx, 5, 4096);
        reg(ctx, 6, 0x10000);
        ps2_stubs::sceCdStInit(rdram.data(), &ctx, &runtime);
        require(result(ctx) == 1, "sceCdStInit PAL");

        // Start 4x speed stream (mode[1] = 4 -> 2700 sectors/s).
        rdram[0x20001] = 4;
        reg(ctx, 4, 0x1000);
        reg(ctx, 5, 0x20000);
        ps2_stubs::sceCdStStart(rdram.data(), &ctx, &runtime);
        require(result(ctx) == 1, "sceCdStStart PAL");

        // Step exactly 50 PAL ticks (1 second).
        runtime.eeScheduler().stepVSyncTicksForTesting(50);
        ps2_stubs::sceCdStStat(rdram.data(), &ctx, &runtime);
        const uint32_t palSectors1s = result(ctx);
        std::cout << "[PAL 1s / 50 ticks] produced " << palSectors1s << " sectors (expected 2700)\n";
        require(palSectors1s == 2700, "50 PAL ticks at 4x spindle speed must produce exactly 2700 sectors (54 sectors/tick)");

        // Step 1 more PAL tick (54 sectors/tick).
        runtime.eeScheduler().stepVSyncTicksForTesting(1);
        ps2_stubs::sceCdStStat(rdram.data(), &ctx, &runtime);
        const uint32_t palSectorsPlus1 = result(ctx);
        std::cout << "[PAL 51 ticks] produced " << palSectorsPlus1 << " sectors (expected 2754)\n";
        require(palSectorsPlus1 == 2754, "1 additional PAL tick must produce 54 sectors");

        // 2. NTSC Video Mode (mode 2): ~59.94 fields / second (60000/1001).
        runtime.eeScheduler().setCrtMode(2);
        require(!runtime.eeScheduler().palVideoMode(), "NTSC video mode active");

        reg(ctx, 4, 0x2000);
        reg(ctx, 5, 0x20000);
        ps2_stubs::sceCdStStart(rdram.data(), &ctx, &runtime);
        require(result(ctx) == 1, "sceCdStStart NTSC");

        // Step 60 ticks: 60 * 2700 * 1001 / 60000 = 2702 sectors.
        runtime.eeScheduler().stepVSyncTicksForTesting(60);
        ps2_stubs::sceCdStStat(rdram.data(), &ctx, &runtime);
        const uint32_t ntscSectors60 = result(ctx);
        std::cout << "[NTSC 60 ticks] produced " << ntscSectors60 << " sectors (expected 2702)\n";
        require(ntscSectors60 == 2702, "60 NTSC ticks at 4x speed must produce 2702 sectors");

        std::cout << "All CD stream field rate tests PASSED!\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
