// Setup wizard step "compile the game" (issue 011): Ninja progress parsing and the configure arguments.
// The real build needs generated game code and is checked locally (see notes, issue 011).
#include "build_game.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace dw5xl::setup;
namespace fs = std::filesystem;

namespace {
void require(bool ok, const char *what)
{
    if (!ok)
        throw std::runtime_error(what);
}

bool has(const std::vector<std::string> &args, const std::string &arg)
{
    return std::find(args.begin(), args.end(), arg) != args.end();
}

bool hasPrefix(const std::vector<std::string> &args, const std::string &prefix)
{
    return std::any_of(args.begin(), args.end(), [&](const std::string &a) { return a.rfind(prefix, 0) == 0; });
}
} // namespace

int main()
{
    try
    {
        const BuildProgress none = parseNinjaProgress("-- Configuring done\n-- Generating done\n");
        require(none.done == 0 && none.total == 0, "no progress before Ninja runs");
        const BuildProgress p = parseNinjaProgress("[1/9] Building a\n[7/43210] Building CXX object x.o\n");
        require(p.done == 7 && p.total == 43210, "last [done/total] wins");

        BuildTools windows;
        windows.cmake = "C:/DW5XL/tools/cmake/bin/cmake.exe";
        windows.ninja = "C:/DW5XL/tools/ninja.exe";
        windows.llvmMingwRoot = "C:/DW5XL/tools/llvm-mingw";
        windows.ffmpegArchive = "C:/DW5XL/tools/ffmpeg.zip";
        const auto win = gameConfigureArgs("C:/DW5XL", "C:/DW5XL/build", windows);
        require(has(win, "-DDW5XL_BUILD_GAME=ON") && has(win, "-DPS2X_ENABLE_VULKAN_GS=ON"), "game + Vulkan");
        require(has(win, "-DPS2X_ENABLE_AGRESSIVE_LOGS=OFF"), "same log settings as Linux");
        require(has(win, "-DLLVM_MINGW_ROOT=C:/DW5XL/tools/llvm-mingw"), "bundled llvm-mingw");
        require(has(win, "-DCMAKE_TOOLCHAIN_FILE=C:/DW5XL/tools/windows/llvm-mingw.cmake"), "toolchain file");
        require(has(win, "-DCMAKE_MAKE_PROGRAM=C:/DW5XL/tools/ninja.exe"), "bundled Ninja");
        require(has(win, "-DPS2X_FFMPEG_PREBUILT_URL=C:/DW5XL/tools/ffmpeg.zip"), "bundled FFmpeg, no download");
        require(!hasPrefix(win, "-DCMAKE_CXX_COMPILER="), "no host compiler on Windows");

        const auto lnx = gameConfigureArgs("/home/p/dw5xl", "/home/p/dw5xl/build", BuildTools{});
        require(has(lnx, "-DCMAKE_CXX_COMPILER=clang++") && has(lnx, "-DCMAKE_LINKER_TYPE=LLD"), "clang + lld");
        require(!hasPrefix(lnx, "-DCMAKE_TOOLCHAIN_FILE="), "no toolchain file on Linux");
        std::cout << "setup build tests passed\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
