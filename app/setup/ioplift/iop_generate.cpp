// Issue 009 v4.5: the disc's IOP modules as C++ (see iop_generate.h).
#include "iop_generate.h"

#include "iop_inputs.h"
#include "iop_translator.h"

#include "ps2x/iop/iop_native.h"
#include "emulator/core/iop_memory.h"
#include "emulator/services/iop_module_loader.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <vector>

namespace dw5xl::ioplift
{
    namespace
    {
        bool readFile(const std::filesystem::path &path, std::vector<uint8_t> &bytes)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return false;
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            return true;
        }

        template <typename T>
        T readAt(const std::vector<uint8_t> &bytes, size_t offset)
        {
            T value{};
            if (offset + sizeof(T) <= bytes.size())
                std::memcpy(&value, bytes.data() + offset, sizeof(T));
            return value;
        }

        // The .text section's address and size inside the loaded image, from the IRX's ELF section headers.
        bool textSection(const std::vector<uint8_t> &image, uint32_t &address, uint32_t &size, uint32_t &lowestVaddr)
        {
            const uint32_t phoff = readAt<uint32_t>(image, 0x1C), shoff = readAt<uint32_t>(image, 0x20);
            const uint16_t phentsize = readAt<uint16_t>(image, 0x2A), phnum = readAt<uint16_t>(image, 0x2C);
            const uint16_t shentsize = readAt<uint16_t>(image, 0x2E), shnum = readAt<uint16_t>(image, 0x30);
            const uint16_t shstrndx = readAt<uint16_t>(image, 0x32);
            lowestVaddr = UINT32_MAX;
            for (uint16_t i = 0; i < phnum; ++i)
            {
                const size_t ph = phoff + static_cast<size_t>(i) * phentsize;
                if (readAt<uint32_t>(image, ph) == 1u) // PT_LOAD
                    lowestVaddr = std::min(lowestVaddr, readAt<uint32_t>(image, ph + 8u));
            }
            if (shnum == 0 || shstrndx >= shnum || lowestVaddr == UINT32_MAX)
                return false;
            const uint32_t strtab = readAt<uint32_t>(image, shoff + static_cast<size_t>(shstrndx) * shentsize + 16u);
            for (uint16_t i = 0; i < shnum; ++i)
            {
                const size_t sh = shoff + static_cast<size_t>(i) * shentsize;
                const uint32_t nameOffset = strtab + readAt<uint32_t>(image, sh);
                if (nameOffset + 6u <= image.size() && std::memcmp(image.data() + nameOffset, ".text", 6) == 0)
                {
                    address = readAt<uint32_t>(image, sh + 12u);
                    size = readAt<uint32_t>(image, sh + 20u);
                    return size != 0u && address >= lowestVaddr;
                }
            }
            return false;
        }
    }

    bool generateIopModules(const std::filesystem::path &modulesDir, std::map<std::string, std::string> &files,
                            std::string &error, const std::function<void(const std::string &)> &progress)
    {
        std::vector<ModuleText> modules;
        for (const IopModuleInput &input : kIopModules)
        {
            std::vector<uint8_t> image;
            if (!readFile(modulesDir / input.file, image))
            {
                error = "cannot read " + (modulesDir / input.file).string();
                return false;
            }
            const uint64_t hash = ps2x::iop::iopNativeHash(image.data(), image.size());
            if (image.size() != input.fileSize || hash != input.fileHash)
            {
                error = std::string(input.file) + " is not the PAL disc's module";
                return false;
            }
            uint32_t textAddress = 0, textSize = 0, lowestVaddr = 0;
            if (!textSection(image, textAddress, textSize, lowestVaddr))
            {
                error = std::string(input.file) + ": no .text section";
                return false;
            }
            auto memory = std::make_unique<ps2x::iop::detail::IopMemory>();
            const auto loaded = ps2x::iop::detail::IopModuleLoader::load(image, *memory, input.base);
            if (!loaded || loaded.base != input.base || !loaded.relocationsComplete)
            {
                error = std::string(input.file) + " did not load at its address";
                return false;
            }
            ModuleText module;
            module.name = input.name;
            module.base = loaded.base + (textAddress - lowestVaddr);
            module.bytes.resize(textSize & ~3u);
            if (!memory->readRam(module.base, module.bytes.data(), module.bytes.size()))
            {
                error = std::string(input.file) + ": cannot read the loaded code";
                return false;
            }
            TranslationStats stats;
            files["iop_" + moduleIdentifier(module.name) + ".cpp"] = translateModule(module, &stats);
            if (progress)
            {
                char line[160];
                std::snprintf(line, sizeof(line), "%-8s code 0x%05x-0x%05x  %u words, %u native, %u import stubs", input.name,
                              module.base, module.base + static_cast<uint32_t>(module.bytes.size()), stats.words,
                              stats.nativeWords, stats.importStubs);
                progress(line);
            }
            modules.push_back(std::move(module));
        }
        files["iop_registry.cpp"] = registrySource(modules);
        return true;
    }
}
