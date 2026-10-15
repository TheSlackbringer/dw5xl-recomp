// Authored scheduler regression: VU0 (COP2) registers are one hardware unit
// shared by every EE thread. A constant loaded on one thread must be visible
// to another thread's macro instructions and VU0 microprograms.
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include <bit>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
constexpr uint32_t kMainPc = 0x1000u;
constexpr uint32_t kWorkerPc = 0x1004u;
constexpr uint32_t kResultAddress = 0x2000u;
constexpr uint32_t kTwoPiBits = 0x40c90fdbu;

void mainThread(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
{
    // Load a constant into VU0 the way game setup code does, then hand over.
    ctx->vu0_vf[24] = _mm_set_ps(0.0f, 0.0f, 0.0f, std::bit_cast<float>(kTwoPiBits));
    ctx->vi[5] = 77;
    EeThreadCreateParams params{};
    params.entry = kWorkerPc;
    params.stack = 0x100000u;
    params.stackSize = 0x1000u;
    params.priority = 1;
    auto &kernel = runtime->eeScheduler();
    const int worker = kernel.createThread(params);
    if (worker <= 0 || kernel.startThread(worker, 0u, *ctx, false) < 0)
        throw std::runtime_error("worker thread not started");
    ctx->pc = 0u;
}

void workerThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    // A new thread's own context starts zeroed; the shared VU0 state must win.
    const uint32_t x = std::bit_cast<uint32_t>(_mm_cvtss_f32(ctx->vu0_vf[24]));
    const uint32_t vi = ctx->vi[5];
    std::memcpy(rdram + kResultAddress, &x, sizeof(x));
    std::memcpy(rdram + kResultAddress + 4u, &vi, sizeof(vi));
    runtime->eeScheduler().requestStop();
    ctx->pc = 0u;
}
}

extern const uint32_t g_ps2RecompiledFunctionTableBase = kMainPc;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = kMainPc + 8u;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 2u;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[2] = {mainThread, workerThread};

int main()
{
    try
    {
        PS2Runtime runtime;
        std::vector<uint8_t> ram(0x2000000u);
        R5900Context main{};
        main.pc = kMainPc;
        auto &kernel = runtime.eeScheduler();
        kernel.reset(ram.data(), main);
        kernel.run();
        uint32_t x = 0, vi = 0;
        std::memcpy(&x, ram.data() + kResultAddress, sizeof(x));
        std::memcpy(&vi, ram.data() + kResultAddress + 4u, sizeof(vi));
        if (x != kTwoPiBits || vi != 77u)
        {
            std::cerr << "worker saw vf24.x=0x" << std::hex << x << " vi5=" << std::dec << vi << '\n';
            return 1;
        }
        std::cout << "Shared VU0 register test passed\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
