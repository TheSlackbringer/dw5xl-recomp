#pragma once
// Setup wizard step "generate the game's C++" (issue 011): the cross-platform replacement for tools/regen.sh.
// Runs the recompiler on the main program and the three overlays from the player's own extracted disc.
#include <filesystem>
#include <functional>
#include <string>

namespace dw5xl::setup {

struct CodegenProfile
{
    const char *name;
    const char *config;    // recompiler configuration (in git), relative to the root
    const char *input;     // disc file it translates, relative to the root
    const char *ghidraCsv; // function list (in git), relative to the root
    const char *output;    // generated C++ folder (never in git), relative to the root
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
// (so an incremental build recompiles only what changed), removes files that are no longer generated.
bool syncDirectory(const std::filesystem::path &source, const std::filesystem::path &target, std::string &error);

// Runs the recompiler for every profile under `root`. Logs go to `logDir/<profile>.log`.
// `progress(profile, index, count)` is called before each profile; returning false cancels.
bool generateCode(const std::filesystem::path &root, const std::filesystem::path &recompiler,
                  const std::filesystem::path &logDir,
                  const std::function<bool(const char *, int, int)> &progress, std::string &error);

} // namespace dw5xl::setup
