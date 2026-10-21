// Setup wizard step "compile the game" (issue 011): Ninja progress parsing, the configure arguments, where the
// tools are found, and that a cancel stops a running program. The real build needs generated game code and is
// checked locally (see notes, issue 011).
#include "build_game.h"
#include "run_process.h"
#include "setup_layout.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

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

void touch(const fs::path &path)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path) << "x";
}

void setEnv(const char *name, const char *value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void layoutTests()
{
    const fs::path root = fs::temp_directory_path() / "dw5xl_setup_layout_test";
    fs::remove_all(root);
    touch(root / "app" / "CMakeLists.txt");
    fs::create_directories(root / "recomp");
    fs::create_directories(root / "game" / "dw5xl");
    require(findRoot(root / "game" / "dw5xl") == root, "root found from a subfolder");
    require(findRoot(root / "app") == root, "root found from app/");

    SetupLayout layout;
    std::string error;
    require(!findLayout(root, false, layout, error) && error.find("recompiler") != std::string::npos,
            "missing recompiler is reported");
    touch(root / "tools" / "ps2_recomp");
    require(findLayout(root, false, layout, error), "Linux layout with a bundled recompiler");
    require(layout.recompiler == root / "tools" / "ps2_recomp" && layout.tools.cmake == "cmake" &&
                layout.tools.ninja.empty() && layout.tools.llvmMingwRoot.empty(), "Linux: cmake/ninja from PATH");
    touch(root / "tools" / "cmake" / "bin" / "cmake");
    require(findLayout(root, false, layout, error) && layout.tools.cmake == root / "tools" / "cmake" / "bin" / "cmake",
            "bundled cmake preferred");

    touch(root / "BUILD_ID");
    require(!findLayout(root, false, layout, error) && error.find("bundled Linux") != std::string::npos,
            "Linux release refuses to fall back to tools on the player's PATH");
    touch(root / "tools" / "ninja");
    touch(root / "tools" / "clang" / "bin" / "clang");
    touch(root / "tools" / "clang" / "bin" / "clang++");
    touch(root / "tools" / "ffmpeg" / "include" / "libavcodec" / "avcodec.h");
    require(findLayout(root, false, layout, error), "self-contained Linux release layout");
    require(layout.tools.linuxToolchainRoot == root / "tools" / "clang" &&
                layout.tools.ffmpegRoot == root / "tools" / "ffmpeg" &&
                layout.tools.ninja == root / "tools" / "ninja", "Linux tools are selected from the release");

    touch(root / "tools" / "ps2_recomp.exe");
    require(!findLayout(root, true, layout, error) && error.find("compiler") != std::string::npos,
            "Windows without the bundled compiler is reported");
    fs::create_directories(root / "tools" / "llvm-mingw");
    touch(root / "tools" / "ninja.exe");
    touch(root / "tools" / "ffmpeg.zip");
    require(findLayout(root, true, layout, error), "Windows release layout");
    require(layout.recompiler == root / "tools" / "ps2_recomp.exe" && layout.tools.ninja == root / "tools" / "ninja.exe" &&
                layout.tools.llvmMingwRoot == root / "tools" / "llvm-mingw" &&
                layout.tools.ffmpegArchive == root / "tools" / "ffmpeg.zip", "Windows tools from tools/");
    fs::remove_all(root);
}

void linuxUpdateTests()
{
    const fs::path scratch = fs::temp_directory_path() / "dw5xl update sources Jürgen";
    fs::remove_all(scratch);
    const fs::path package = scratch / "first mount", data = scratch / "player";
    for (const char *folder : {"app", "external", "recomp", "tools"})
        touch(package / folder / "file");
    touch(package / "BUILD_ID");
#ifndef _WIN32
    fs::create_symlink("file", package / "tools" / "alias");
#endif
    std::string error;
    fs::path selected;
    require(prepareLinuxBuildRoot(package, data, selected, error), "prepare Linux build sources");
    require(selected == data / "builder" && fs::exists(package / "app" / "file"),
            "Linux sources use a stable data path without moving package files");
    const auto old = fs::file_time_type::clock::now() - std::chrono::hours(24);
    fs::last_write_time(selected / "app" / "file", old);
    touch(selected / "app" / "stale");
    const fs::path moved = scratch / "second mount";
    fs::rename(package, moved);
    std::ofstream(moved / "external" / "file") << "updated";
    require(prepareLinuxBuildRoot(moved, data, selected, error), "update from a different AppImage mount");
    require(selected == data / "builder" && fs::last_write_time(selected / "app" / "file") == old,
            "unchanged sources keep their path and timestamp across releases");
    require(!fs::exists(selected / "app" / "stale") && fs::file_size(selected / "external" / "file") == 7,
            "removed sources disappear and changed files update");
#ifndef _WIN32
    require(fs::is_symlink(selected / "tools" / "alias") && fs::read_symlink(selected / "tools" / "alias") == "file",
            "relative tool wrapper links survive source caching");
#endif
    fs::remove_all(scratch);
}

// A program that runs 30 s is stopped within a few seconds of a cancel.
void cancelTest(const char *self)
{
    const fs::path dir = fs::temp_directory_path() / "dw5xl_setup_cancel_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    setEnv("DW5XL_SLEEP_CHILD", "1"); // the child (this program) sleeps instead of testing; see main()
    std::atomic<bool> cancel{false};
    std::thread stopper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        cancel = true;
    });
    const auto start = std::chrono::steady_clock::now();
    const ProcessResult r = runProcess(fs::absolute(self), {}, dir, dir / "child.log", &cancel);
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    stopper.join();
    setEnv("DW5XL_SLEEP_CHILD", "");
    require(r.started && r.cancelled, "the child was started and reported as cancelled");
    require(seconds < 10.0, "cancel stopped the child well before its 30 s");
    fs::remove_all(dir);
}

int main(int argc, char **argv)
{
    // Child mode for cancelTest: sleep and exit, never test (and so never start another child).
    if (const char *child = std::getenv("DW5XL_SLEEP_CHILD"); child && *child == '1')
    {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        return 0;
    }
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
        require(!hasPrefix(lnx, "-DDW5XL_RECOMP_ROOT="), "developer tree: the generated code stays in recomp/");
        BuildTools linuxRelease;
        linuxRelease.linuxToolchainRoot = "/tmp/DW5XL package/tools/clang";
        linuxRelease.ffmpegRoot = "/tmp/DW5XL package/tools/ffmpeg";
        const auto linuxArgs = gameConfigureArgs("/tmp/DW5XL package", "/tmp/player build", linuxRelease);
        require(has(linuxArgs, "-DCMAKE_TOOLCHAIN_FILE=/tmp/DW5XL package/tools/linux/clang.cmake") &&
                    has(linuxArgs, "-DDW5XL_LINUX_TOOLCHAIN_ROOT=/tmp/DW5XL package/tools/clang"),
                "Linux release uses its relocatable compiler toolchain, including paths with spaces");
        require(has(linuxArgs, "-DPS2X_FFMPEG_ROOT=/tmp/DW5XL package/tools/ffmpeg") &&
                    !hasPrefix(linuxArgs, "-DCMAKE_CXX_COMPILER="), "Linux release needs no host compiler or FFmpeg");
        const auto rel = gameConfigureArgs("C:/DW5XL", "C:/Users/p/AppData/Local/DW5XL/build", windows,
                                           "C:/Users/p/AppData/Local/DW5XL/recomp");
        require(has(rel, "-DDW5XL_RECOMP_ROOT=C:/Users/p/AppData/Local/DW5XL/recomp"),
                "release: the generated code is read from the player data folder");
        { // a newer release in another folder: the old build configuration is reset, compiled files stay
            const fs::path b = fs::temp_directory_path() / "dw5xl_setup_moved_test";
            fs::remove_all(b);
            fs::create_directories(b / "CMakeFiles");
            std::ofstream(b / "CMakeCache.txt") << "FOO:BOOL=ON\nCMAKE_HOME_DIRECTORY:INTERNAL=C:/Games/DW5XL-old/app\n";
            std::ofstream(b / "kept.obj") << "x";
            require(!resetBuildIfMoved(b, "C:/Games/DW5XL-old/app") && fs::exists(b / "CMakeCache.txt"),
                    "same source folder: untouched");
            require(resetBuildIfMoved(b, "C:/Games/DW5XL-new/app"), "another source folder: reset");
            require(!fs::exists(b / "CMakeCache.txt") && !fs::exists(b / "CMakeFiles") && fs::exists(b / "kept.obj"),
                    "configuration gone, compiled files kept");
            require(!resetBuildIfMoved(b, "C:/Games/DW5XL-new/app"), "nothing configured: nothing to reset");
            fs::remove_all(b);
        }
        layoutTests();
        linuxUpdateTests();
        cancelTest(argc > 0 ? argv[0] : "");
        std::cout << "setup build tests passed\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
