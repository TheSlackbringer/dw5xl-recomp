#pragma once
// Setup wizard step "compile the game" (issue 011): configures and builds app/ with CMake + Ninja from the
// generated code, with the same settings as the Linux build and CI. Windows: the bundled llvm-mingw toolchain.
#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace dw5xl::setup {

struct BuildTools
{
    std::filesystem::path cmake = "cmake";      // bundled cmake.exe on Windows
    std::filesystem::path ninja;                // empty: Ninja on PATH
    std::filesystem::path llvmMingwRoot;        // Windows: bundled llvm-mingw (empty on Linux)
    std::filesystem::path ffmpegArchive;        // Windows: bundled FFmpeg zip (empty: CMake downloads it)
};

struct BuildProgress
{
    int done = 0, total = 0; // Ninja's [done/total]; total 0 while configuring
    std::string phase;       // "configure" / "build"
};

// The CMake arguments used to configure the game build (also used by the test and CI).
std::vector<std::string> gameConfigureArgs(const std::filesystem::path &root, const std::filesystem::path &buildDir,
                                           const BuildTools &tools);

// Ninja's progress in a build log: the last "[done/total]" (0/0 if none yet).
BuildProgress parseNinjaProgress(const std::string &logText);

// Configures and builds the game (`dw5xl` target) in `buildDir`; the log goes to `logFile`. `progress` is called
// about twice a second while CMake runs. On success `executable` is the built game. Setting `cancel` stops the
// build (CMake, Ninja and the compilers).
bool buildGame(const std::filesystem::path &root, const std::filesystem::path &buildDir, const BuildTools &tools,
               const std::filesystem::path &logFile, const std::function<void(const BuildProgress &)> &progress,
               std::filesystem::path &executable, std::string &error, const std::atomic<bool> *cancel = nullptr);

} // namespace dw5xl::setup
