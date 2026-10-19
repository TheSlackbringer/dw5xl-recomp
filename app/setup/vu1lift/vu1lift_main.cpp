// dw5xl_vu1lift <SLES_538.60> <output.cpp>: writes the lifted VU1 code, exactly like tools/vu1_translator.py
// (disc mode). DW5XL_VU1_STATIC ('all' default, 'none', or a comma list) selects static-timing segments, as there.
#include "vu1_translator.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        std::fprintf(stderr, "usage: %s <SLES_538.60> <output.cpp>\n", argv[0]);
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
    if (!dw5xl::vu1lift::generateLiftedVu1(argv[1], text, error, [](const std::string &line) {
            std::printf("%s\n", line.c_str());
        }, staticTiming))
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
