// Issue 009 v4.5: writes the IOP translator test program (ioplift_test_program.h) as raw little-endian words.
#include "ioplift_test_program.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: dw5xl_ioplift_testgen <program.bin>\n");
        return 2;
    }
    const std::filesystem::path out(argv[1]);
    if (out.has_parent_path())
        std::filesystem::create_directories(out.parent_path());
    std::ofstream file(out, std::ios::binary | std::ios::trunc);
    for (const uint32_t word : ioplift_test::program())
    {
        const char bytes[4] = {static_cast<char>(word), static_cast<char>(word >> 8u), static_cast<char>(word >> 16u),
                               static_cast<char>(word >> 24u)};
        file.write(bytes, 4);
    }
    return file ? 0 : 1;
}
