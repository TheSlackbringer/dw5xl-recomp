// Executes the player's translated routines against expectations from the original
// JR/LW and JR/SW instruction pair. No replacement setter is used by this test.
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include <ps2_recompiled_functions.h>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x1AC6C0;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x1AC6D8;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 6;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[6] = {};

namespace {
void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
void registersUnchanged(const R5900Context &before, const R5900Context &after, int except = -1) {
    for (int reg = 0; reg < 32; ++reg)
        if (reg != except) require(std::memcmp(&before.r[reg], &after.r[reg], sizeof(before.r[reg])) == 0,
                                   "unrelated caller register changed");
}
}
int main() {
    try {
        PS2Runtime runtime;
        require(runtime.memory().initialize(), "initialize RAM");
        auto *ram = runtime.memory().getRDRAM();
        constexpr uint32_t flag = 0x35AFFC, gp = 0x362970, returnPc = 0x123456;
        for (const uint32_t value : {0u, 1u, 0xF1234567u}) {
            R5900Context context{};
            for (int reg = 1; reg < 32; ++reg) context.r[reg] = _mm_set1_epi32(0x1000 + reg);
            SET_GPR_U64(&context, 28, gp); SET_GPR_U64(&context, 31, returnPc);
            SET_GPR_U64(&context, 4, value); context.pc = 0x1AC6C0;
            const auto before = context;
            std::memset(ram + flag - 8, 0xA5, 20);
            main_ovl02_import_001ac6c0_0x1ac6c0(ram, &context, &runtime);
            uint32_t stored = 0; std::memcpy(&stored, ram + flag, sizeof(stored));
            require(stored == value && context.pc == returnPc, "setter writes argument and returns through RA");
            registersUnchanged(before, context);
            for (int offset = -8; offset < 12; ++offset)
                if (offset < 0 || offset >= 4) require(ram[flag + offset] == 0xA5, "setter writes beyond its flag");
            const auto beforeRead = context;
            context.pc = 0x1AC6D0;
            FUN_001ac6d0_0x1ac6d0(ram, &context, &runtime);
            require(Ps2ExtractEpi64(context.r[2], 0) == static_cast<int32_t>(value) && context.pc == returnPc,
                    "reader sign-extends the original word and returns");
            registersUnchanged(beforeRead, context, 2);
            uint32_t afterRead = 0; std::memcpy(&afterRead, ram + flag, sizeof(afterRead));
            require(afterRead == value, "reader preserves flag");
        }
        std::cout << "Original translated Load Original reader/setter checks passed\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
