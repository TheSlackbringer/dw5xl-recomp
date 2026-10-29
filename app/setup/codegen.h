#pragma once
// Setup wizard step "generate the game's C++" (issue 011): the cross-platform replacement for tools/regen.sh.
// Runs the recompiler on the main program and the three overlays from the player's own extracted disc.
#include <filesystem>
#include <functional>
#include <string>
#include <atomic>
#include "stage_timing.h"

namespace dw5xl::setup {

struct CodegenProfile
{
    const char *name;
    const char *config;    // recompiler configuration (in git), relative to the root
    const char *input;     // disc file it translates, relative to the root
    const char *ghidraCsv; // function list (in git), relative to the root
    const char *output;    // generated C++ folder (never in git), relative to the root
};

struct CodegenProgress
{
    std::string profile, stage;
    uint64_t done=0,total=0; // total==0 means activity only
};
struct CodegenOptions
{
    bool reuse=false, force=false;
    const std::atomic<bool> *cancel=nullptr;
    std::function<void(const CodegenProgress &)> activity;
};

inline constexpr CodegenProfile kCodegenProfiles[] = {
    {"main", "recomp/dw5xl/dw5xl.toml", "game/dw5xl/SLES_538.60", "recomp/dw5xl/ghidra_functions.code.csv",
     "recomp/dw5xl/output"},
    {"ovl00", "recomp/dw5xl/overlays/ovl00/ovl00.toml", "game/dw5xl/OVL_00.BIN",
     "recomp/dw5xl/overlays/ovl00/ghidra_functions.code.csv", "recomp/dw5xl/overlays/ovl00/output"},
    {"ovl01", "recomp/dw5xl/overlays/ovl01/ovl01.toml", "game/dw5xl/OVL_01.BIN",
     "recomp/dw5xl/overlays/ovl01/ghidra_functions.code.csv", "recomp/dw5xl/overlays/ovl01/output"},
    {"ovl02", "recomp/dw5xl/overlays/ovl02/ovl02.toml", "game/dw5xl/OVL_02.BIN",
     "recomp/dw5xl/overlays/ovl02/ghidra_functions.code.csv", "recomp/dw5xl/overlays/ovl02/output"},
};

// Rewrites the input / ghidra_output / output lines of a recompiler configuration (TOML text) to the given paths.
std::string rewriteRecompConfig(const std::string &config, const std::filesystem::path &input,
                                 const std::filesystem::path &ghidraCsv, const std::filesystem::path &output);

// Makes `target` hold exactly the files of `source`: copies new and changed files, leaves identical files untouched
// (so an incremental build recompiles only what changed), removes files that are no longer generated. When
// `target` does not exist yet, `source` is moved there in one step unless keepSource is true.
// Relative symlinks are preserved (the Linux compiler uses them for its tool wrappers).
bool syncDirectory(const std::filesystem::path &source, const std::filesystem::path &target, std::string &error,
                   bool keepSource = false, const std::function<bool(uint64_t,uint64_t)> &progress = {});

// Runs the recompiler for every profile. Configurations and function lists come from `root` (the project or the
// release package); the disc files are read from and the generated code is written to `dataDir` (empty: `root`;
// a release uses the player data folder). Logs go to `logDir/<profile>.log`.
// `progress(profile, index, count)` is called before each profile; returning false cancels.
bool generateCode(const std::filesystem::path &root, const std::filesystem::path &recompiler,
                  const std::filesystem::path &logDir,
                  const std::function<bool(const char *, int, int)> &progress, std::string &error,
                  const std::filesystem::path &dataDir = {}, ps2recomp::timing::Session *timing = nullptr,
                  uint64_t timingParent = 0, const CodegenOptions &options = {});

} // namespace dw5xl::setup
