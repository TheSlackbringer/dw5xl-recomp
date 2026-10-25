// dw5xl_vu1lift [--vu0] <SLES_538.60> <output.cpp>: writes the lifted VU1 code (or with --vu0 the lifted VU0
// microprograms, issue 025) from the player's own executable. DW5XL_VU1_STATIC ('all' default, 'none', or a comma
// list) selects static-timing segments.
//
// dw5xl_vu1lift --find-range <census image .bin> <entry pc> <name> [disc file ...]: locates a VU1 program's reachable
// code in the player's own disc files (default game/dw5xl/SLES_538.60) and prints the entries to add to
// app/setup/vu1lift/vu1_inputs.h (kChunks/kRanges, kImageRegistrations, kTargets): offsets, sizes and a SHA256
// only, never instruction bytes. The image comes from a local PS2X_VU1_CENSUS run (never commit it). Each contiguous
// reachable run is matched greedily (longest prefix found) in the given files.
#include "vu1_translator.h"
#include "../sha256.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string_view>

namespace
{
    std::string readFile(const char *path)
    {
        std::ifstream in(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    uint64_t fnv1a64(const std::string &data)
    {
        uint64_t h = 1469598103934665603ull;
        for (unsigned char b : data)
            h = (h ^ b) * 1099511628211ull;
        return h;
    }

    int findRange(int argc, char **argv)
    {
        if (argc < 5)
        {
            std::fprintf(stderr, "usage: %s --find-range <census image .bin> <entry pc> <name> [disc file ...]\n",
                         argv[0]);
            return 2;
        }
        const std::string image = readFile(argv[2]);
        if (image.size() != 16384)
        {
            std::fprintf(stderr, "dw5xl_vu1lift: %s is not a 16 KiB VU1 image\n", argv[2]);
            return 1;
        }
        const uint32_t entry = static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 0));
        const std::string name = argv[4];
        struct Source { std::string label; std::string blob; }; // label empty: the executable
        std::vector<Source> sources;
        std::vector<std::string> files;
        for (int i = 5; i < argc; ++i)
            files.push_back(argv[i]);
        if (files.empty())
            files.push_back("game/dw5xl/SLES_538.60");
        for (const auto &f : files)
        {
            const std::string base = std::filesystem::path(f).filename().string();
            sources.push_back({base == "SLES_538.60" ? std::string() : base, readFile(f.c_str())});
        }
        const std::vector<uint8_t> code(image.begin(), image.end());
        const std::vector<uint32_t> reachable = dw5xl::vu1lift::findReachable(code, entry);
        std::vector<std::pair<uint32_t, uint32_t>> runs;
        for (uint32_t pc : reachable)
        {
            if (!runs.empty() && runs.back().second == pc)
                runs.back().second = pc + 8;
            else
                runs.push_back({pc, pc + 8});
        }
        struct Chunk { uint32_t pc, offset, size; std::string label; };
        std::vector<Chunk> chunks;
        for (const auto &[a, b] : runs)
            for (uint32_t pc = a; pc < b;)
            {
                bool found = false;
                Chunk best{};
                for (const auto &src : sources)
                {
                    int lo = 8, hi = int(b - pc);
                    while (lo <= hi)
                    {
                        int mid = (lo + hi) / 16 * 8;
                        if (mid == 0)
                            mid = 8;
                        const size_t at = src.blob.find(std::string_view(image).substr(pc, size_t(mid)));
                        if (at != std::string::npos)
                        {
                            if (!found || uint32_t(mid) > best.size)
                                best = {pc, uint32_t(at), uint32_t(mid), src.label};
                            found = true;
                            lo = mid + 8;
                        }
                        else
                            hi = mid - 8;
                    }
                }
                if (!found)
                {
                    std::fprintf(stderr, "dw5xl_vu1lift: %s: bytes at VU pc 0x%04x not found in the disc files\n",
                                 name.c_str(), pc);
                    return 1;
                }
                chunks.push_back(best);
                pc += best.size;
            }
        std::string rebuilt(16384, '\0');
        for (const auto &c : chunks)
            for (const auto &src : sources)
                if (src.label == c.label)
                    rebuilt.replace(c.pc, c.size, src.blob, c.offset, c.size);
        const uint32_t end = reachable.back();
        const uint64_t imageHash = fnv1a64(image);
        char key[17];
        std::snprintf(key, sizeof(key), "%016llx", static_cast<unsigned long long>(imageHash));
        const std::string digest = dw5xl::setup::sha256Hex(reinterpret_cast<const uint8_t *>(rebuilt.data()) + entry,
                                                           end + 8 - entry);
        std::printf("kChunks entry (number it after the last kChunksN in vu1_inputs.h):\n"
                    "inline constexpr Chunk kChunksN[] = {");
        for (size_t i = 0; i < chunks.size(); ++i)
            std::printf("%s{%uu, %uu, %uu, %s}", i ? ", " : "", chunks[i].pc, chunks[i].offset, chunks[i].size,
                        chunks[i].label.empty() ? "nullptr" : ("\"" + chunks[i].label + "\"").c_str());
        std::printf("};\nkRanges entry:\n    {\"%s\", %uu, %uu, \"%s\", kChunksN, %zuu, \"%s\"},\n", key, entry, end,
                    name.c_str(), chunks.size(), digest.c_str());
        std::printf("kImageRegistrations entry:\n    {0x%sull, %uu, \"%s\"},\n", key, entry, name.c_str());
        std::printf("kTargets entry:\n    {\"%s\", %uu, %uu, \"%s\"},\n", key, entry, end, name.c_str());
        return 0;
    }
}

int main(int argc, char **argv)
{
    if (argc >= 2 && std::strcmp(argv[1], "--find-range") == 0)
        return findRange(argc, argv);
    const bool vu0 = argc == 4 && std::strcmp(argv[1], "--vu0") == 0;
    if (vu0)
    {
        ++argv;
        --argc;
    }
    if (argc != 3)
    {
        std::fprintf(stderr, "usage: %s [--vu0] <SLES_538.60> <output.cpp>\n"
                             "       %s --find-range <census image .bin> <entry pc> <name> [disc file ...]\n",
                     argv[0], argv[0]);
        return 2;
    }
    const char *env = std::getenv("DW5XL_VU1_STATIC");
    const std::string selection = env ? env : "all";
    auto staticTiming = [&](const std::string &name) {
        if (selection == "all")
            return true;
        std::stringstream list(selection);
        for (std::string item; std::getline(list, item, ',');)
            if (item == name)
                return true;
        return false;
    };
    std::string text, error;
    const auto print = [](const std::string &line) { std::printf("%s\n", line.c_str()); };
    if (!(vu0 ? dw5xl::vu1lift::generateLiftedVu0(argv[1], text, error, print, staticTiming)
              : dw5xl::vu1lift::generateLiftedVu1(argv[1], text, error, print, staticTiming)))
    {
        std::fprintf(stderr, "dw5xl_vu1lift: %s\n", error.c_str());
        return 1;
    }
    std::ofstream out(argv[2], std::ios::binary);
    out << text;
    if (!out.flush())
    {
        std::fprintf(stderr, "dw5xl_vu1lift: cannot write %s\n", argv[2]);
        return 1;
    }
    std::printf("Wrote %s\n", argv[2]);
    return 0;
}
