#include "ps2_runtime.h"
#include "ps2_syscalls.h"
#include "runtime/ee_scheduler.h"
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

// A minimal real dispatch table for the original loader completion routine.
extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x11f5a0u;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x11f5c0u;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 8u;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[8] = {
    ps2_syscalls::WaitSema, nullptr, nullptr, nullptr,
    ps2_syscalls::PollSema, nullptr, nullptr, nullptr
};
void FUN_00144c70_0x144c70(uint8_t*, R5900Context*, PS2Runtime*);

namespace {
void require(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}
uint32_t reg(const R5900Context& ctx, unsigned index) {
    uint32_t value; std::memcpy(&value, &ctx.r[index], sizeof(value)); return value;
}
void setReg(R5900Context& ctx, unsigned index, uint32_t value) {
    ctx.r[index] = _mm_setzero_si128();
    std::memcpy(&ctx.r[index], &value, sizeof(value));
}
uint32_t word(const std::vector<uint8_t>& ram, uint32_t address) {
    uint32_t value; std::memcpy(&value, ram.data() + address, sizeof(value)); return value;
}
void setWord(std::vector<uint8_t>& ram, uint32_t address, uint32_t value) {
    std::memcpy(ram.data() + address, &value, sizeof(value));
}
}

int main() {
    try {
        PS2Runtime runtime;
        R5900Context context{};
        std::vector<uint8_t> ram(0x2000000u);
        auto& kernel = runtime.eeScheduler();
        kernel.bindMainContextForSyscall(context, ram.data());
        const int sema = kernel.createSemaphore(0, 1, 0, 0);
        require(sema > 0, "create empty semaphore");

        // Direct scheduler contract: the same abstraction used by both syscalls.
        require(kernel.pollSemaphore(sema) == -1, "empty poll must return exactly -1");
        require(kernel.pollSemaphore(-123) == -1, "unknown ID must return exactly -1");

        // Actual numeric EE syscall dispatcher, including interrupt-safe alias.
        for (uint32_t syscall : {0x45u, static_cast<uint32_t>(-0x46)}) {
            setReg(context, 4, static_cast<uint32_t>(sema));
            require(ps2_syscalls::dispatchNumericSyscall(syscall, ram.data(), &context, &runtime),
                    "dispatch PollSema/iPollSema");
            require(reg(context, 2) == 0xffffffffu, "empty syscall result exactly -1");
            uint64_t low64; std::memcpy(&low64, &context.r[2], sizeof(low64));
            require(low64 == UINT64_MAX, "failure result sign-extended for original 64-bit comparison");
            require(kernel.signalSemaphore(sema, false) >= 0, "signal before successful poll");
            setReg(context, 4, static_cast<uint32_t>(sema));
            require(ps2_syscalls::dispatchNumericSyscall(syscall, ram.data(), &context, &runtime),
                    "dispatch successful poll");
            require(reg(context, 2) == static_cast<uint32_t>(sema), "successful poll returns ID");
            require(kernel.pollSemaphore(sema) == -1, "successful poll consumed exactly one token");
            setReg(context, 4, 0xffffff85u);
            require(ps2_syscalls::dispatchNumericSyscall(syscall, ram.data(), &context, &runtime),
                    "dispatch invalid ID poll");
            require(reg(context, 2) == 0xffffffffu, "invalid syscall result exactly -1");
        }

        // Exercise the original completion check against the actual runtime.
        // No game data is embedded: its locally generated function is a build input.
        setWord(ram, 0x35adc8u, static_cast<uint32_t>(sema)); // gp - 0x7ba8
        setWord(ram, 0x452310u, 2); // submitted batch
        setWord(ram, 0x452314u, 101);
        auto checkCompletion = [&] {
            context.pc = 0x144c70u;
            setReg(context, 4, 0); // poll, don't wait
            setReg(context, 28, 0x362970u);
            setReg(context, 29, 0x1fff000u);
            setReg(context, 31, 0x1000u);
            FUN_00144c70_0x144c70(ram.data(), &context, &runtime);
            require(context.pc == 0x1000u, "original completion routine returns to caller");
        };
        checkCompletion();
        require(reg(context, 2) == 0, "pending batch reported incomplete");
        require(word(ram, 0x452310u) == 2 && word(ram, 0x452314u) == 101,
                "empty semaphore must preserve all queued reads");
        require(kernel.signalSemaphore(sema, false) >= 0, "worker signals real completion");
        checkCompletion();
        require(reg(context, 2) == 1, "completed batch reported complete");
        require(word(ram, 0x452310u) == 0 && word(ram, 0x452314u) == 0,
                "real completion permits original queue cleanup");
        require(kernel.pollSemaphore(sema) == -1, "original check consumes completion token once");
        std::cout << "EE PollSema contract and original loader completion passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
