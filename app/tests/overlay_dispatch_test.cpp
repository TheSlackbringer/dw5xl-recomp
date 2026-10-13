#include "ps2_runtime.h"
#include <iostream>
#include <stdexcept>

namespace {
int result = 0;
void a(uint8_t*, R5900Context*, PS2Runtime*) { result = 1; }
void b(uint8_t*, R5900Context*, PS2Runtime*) { result = 2; }
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}
extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x1000;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x1004;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 1;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[] = {a};

int main() {
    try {
        PS2Runtime runtime;
        R5900Context context{};
        static const RecompiledModuleEntry ae[] = {{0x610004,a},{0x61000c,a},{0x610030,a}};
        static const RecompiledModuleEntry be[] = {{0x610004,b},{0x61000c,b}};
        static const RecompiledModule A = {"A",0x610000,0x40,ae,3};
        static const RecompiledModule B = {"B",0x610000,0x20,be,2};
        require(runtime.registerCompiledModule(A), "register A");
        require(runtime.registerCompiledModule(B), "register B");
        require(!runtime.hasFunction(0x610004), "inactive module must not resolve");
        require(runtime.activateCompiledModule("A"), "activate A");
        runtime.lookupFunction(0x610004)(nullptr,&context,&runtime);
        require(result==1 && runtime.hasFunction(0x61000c), "A and continuation lookup");
        require(runtime.activateCompiledModule("B"), "activate B");
        runtime.lookupFunction(0x610004)(nullptr,&context,&runtime);
        require(result==2 && !runtime.hasFunction(0x610030), "same PC switches; stale tail gone");
        require(!runtime.hasFunction(0x610005), "unaligned target rejected");
        require(runtime.hasFunction(0x1000), "main ELF lookup preserved");
        require(!runtime.activateCompiledModule("unknown") && !runtime.hasFunction(0x610004), "unknown activation clears active module");
        static const RecompiledModuleEntry unordered[] = {{0x620008,a},{0x620004,a}};
        static const RecompiledModule bad = {"bad",0x620000,0x20,unordered,2};
        require(!runtime.registerCompiledModule(bad), "unsorted table rejected");
        static const RecompiledModuleEntry outside[] = {{0x620020,a}};
        static const RecompiledModule badRange = {"outside",0x620000,0x20,outside,1};
        require(!runtime.registerCompiledModule(badRange), "out-of-range target rejected");
        static const RecompiledModule duplicate = {"A",0x610000,0x40,ae,3};
        require(!runtime.registerCompiledModule(duplicate), "duplicate identity rejected");
        require(runtime.activateCompiledModule("A"), "reactivate A");
        auto generation = runtime.compiledModuleGeneration();
        runtime.deactivateCompiledModule();
        require(runtime.compiledModuleGeneration()>generation && !runtime.hasFunction(0x61000c), "deactivation clears continuation entries");
        std::cout << "Overlay dispatch tests passed\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
