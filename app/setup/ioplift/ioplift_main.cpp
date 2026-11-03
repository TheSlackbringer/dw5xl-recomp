// Issue 009 v4.5: dw5xl_ioplift — the game's IOP modules as C++ (game-derived output: never commit it).
//   dw5xl_ioplift <MODULES folder> <output folder>
//       every module of iop_inputs.h (iop_generate.h): iop_<name>.cpp per module plus iop_registry.cpp.
//   dw5xl_ioplift --raw <code.bin> <base hex> <name> <output folder>
//       a raw little-endian code image (translator tests; no game data).
// Files whose content did not change are not rewritten (no needless rebuilds).
#include "iop_generate.h"
#include "iop_translator.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    using namespace dw5xl::ioplift;

    bool readFile(const fs::path &path, std::vector<uint8_t> &bytes)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return false;
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return true;
    }

    bool writeIfChanged(const fs::path &path, const std::string &text)
    {
        std::vector<uint8_t> old;
        if (readFile(path, old) && old.size() == text.size() && std::memcmp(old.data(), text.data(), text.size()) == 0)
            return true;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        return static_cast<bool>(out);
    }

    int writeAll(const fs::path &outDir, const std::map<std::string, std::string> &files)
    {
        for (const auto &[name, text] : files)
            if (!writeIfChanged(outDir / name, text))
            {
                std::fprintf(stderr, "dw5xl_ioplift: cannot write %s\n", (outDir / name).string().c_str());
                return 1;
            }
        return 0;
    }

    int translateRaw(const fs::path &code, uint32_t base, const std::string &name, const fs::path &outDir)
    {
        ModuleText module;
        module.name = name;
        module.base = base;
        if (!readFile(code, module.bytes) || module.bytes.empty() || (module.bytes.size() & 3u))
        {
            std::fprintf(stderr, "dw5xl_ioplift: cannot read %s (whole words)\n", code.string().c_str());
            return 1;
        }
        const std::vector<ModuleText> modules{module};
        return writeAll(outDir, {{"iop_" + moduleIdentifier(name) + ".cpp", translateModule(module)},
                                 {"iop_registry.cpp", registrySource(modules)}});
    }
}

int main(int argc, char **argv)
{
    if (argc == 6 && std::strcmp(argv[1], "--raw") == 0)
    {
        fs::create_directories(argv[5]);
        return translateRaw(argv[2], static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 16)), argv[4], argv[5]);
    }
    if (argc != 3)
    {
        std::fprintf(stderr, "usage: dw5xl_ioplift <MODULES folder> <output folder>\n"
                             "       dw5xl_ioplift --raw <code.bin> <base hex> <name> <output folder>\n");
        return 2;
    }
    std::map<std::string, std::string> files;
    std::string error;
    if (!generateIopModules(argv[1], files, error, [](const std::string &line) { std::printf("dw5xl_ioplift: %s\n", line.c_str()); }))
    {
        std::fprintf(stderr, "dw5xl_ioplift: %s\n", error.c_str());
        return 1;
    }
    fs::create_directories(argv[2]);
    return writeAll(argv[2], files);
}
