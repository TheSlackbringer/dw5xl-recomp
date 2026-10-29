#pragma once
// Where the setup wizard finds the project and its tools (issue 011).
// Release package: <root>/tools/ holds ps2_recomp(.exe), cmake/, ninja(.exe), and on Windows llvm-mingw/ and
// ffmpeg.zip. Developer tree: the recompiler from external/PS2Recomp/build, CMake and Ninja from PATH.
#include "build_game.h"

#include <filesystem>
#include <string>

namespace dw5xl::setup {

struct SetupLayout
{
    std::filesystem::path root;
    std::filesystem::path recompiler;
    BuildTools tools;
};

// The project root at or above `start`: the first folder that has app/CMakeLists.txt and recomp/. Empty if none.
std::filesystem::path findRoot(const std::filesystem::path &start);

// The tools for `root`; `windows` selects the Windows names and the bundled compiler. false + error when a tool
// the build needs is missing.
bool findLayout(const std::filesystem::path &root, bool windows, SetupLayout &layout, std::string &error);

// Linux AppImage mounts move between runs. Keep build inputs at one player-data
// path, preserving identical files so updates can reuse compiler output.
bool prepareLinuxBuildRoot(const std::filesystem::path &root, const std::filesystem::path &data,
                           std::filesystem::path &buildRoot, std::string &error);
bool prepareBuildRoot(const std::filesystem::path &root, const std::filesystem::path &data,
                      std::filesystem::path &buildRoot, std::string &error,
                      const std::function<bool(uint64_t,uint64_t)> &progress = {});

} // namespace dw5xl::setup
