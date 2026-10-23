// IOP library-name compare (issue 025 spec v8, guard for fix A): the ASCII case folding equals std::tolower in
// the C locale for all 256 byte values, and the name compare agrees with a std::tolower compare.
#include "ps2x/iop/iop_ascii.h"

#include <cctype>
#include <iostream>
#include <string>

int main()
{
    int failures = 0;
    for (int c = 0; c < 256; ++c)
    {
        const unsigned char b = static_cast<unsigned char>(c);
        if (ps2x::iop::asciiLower(b) != static_cast<unsigned char>(std::tolower(b)))
        {
            std::cout << "FAIL byte " << c << ": asciiLower " << int(ps2x::iop::asciiLower(b)) << " std::tolower "
                      << std::tolower(b) << "\n";
            ++failures;
        }
    }
    const auto toLowerEquals = [](const std::string &a, const std::string &b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        return true;
    };
    const std::string names[] = {"libsd", "LIBSD", "LibSd", "sysmem", "SYSMEM ", "cdvdman", "loadcore",
                                 "intrman", "ioman", "vblank", "timrman", "stdio", "sysclib", "heaplib",
                                 "thbase", "", "l", "libs\xC4", "LIBS\xE4", "lib@d", "lib`d", "LIB[D"};
    for (const auto &a : names)
        for (const auto &b : names)
            if (ps2x::iop::asciiEqualsIgnoreCase(a, b) != toLowerEquals(a, b))
            {
                std::cout << "FAIL compare '" << a << "' '" << b << "'\n";
                ++failures;
            }
    std::cout << (failures ? "FAIL" : "PASS") << " iop_ascii_compare\n";
    return failures ? 1 : 0;
}
