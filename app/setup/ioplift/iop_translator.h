#pragma once
// Issue 009 v4.5: translates an IOP module's code (R3000A, as relocated in IOP RAM) into C++ for
// ps2x::iop::IopNativeFrame (ps2xIOP/include/ps2x/iop/iop_native.h). Pure: no file access.

#include <cstdint>
#include <string>
#include <vector>

namespace dw5xl::ioplift
{
    struct ModuleText
    {
        std::string name;           // module name, e.g. "KOEISND"
        uint32_t base = 0;          // IOP RAM address of the first byte of `bytes`
        std::vector<uint8_t> bytes; // the module's .text after relocation
    };

    struct TranslationStats
    {
        uint32_t words = 0;
        uint32_t nativeWords = 0;   // instructions with a native form (the rest stop for the interpreter)
        uint32_t importStubs = 0;
    };

    // C identifier for a module name ("KOEISND" -> "KOEISND", "sio2man" -> "sio2man").
    [[nodiscard]] std::string moduleIdentifier(const std::string &name);
    // One .cpp per module, defining `uint32_t iopn_run_<identifier>(ps2x::iop::IopNativeFrame &)`.
    [[nodiscard]] std::string translateModule(const ModuleText &module, TranslationStats *stats = nullptr);
    // The registry .cpp: a static ps2x::iop::IopNativeRegistrar for the modules.
    [[nodiscard]] std::string registrySource(const std::vector<ModuleText> &modules);
}
