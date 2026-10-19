#pragma once
// Starts a program with arguments and waits for it (issue 011 setup wizard). No shell is involved, so paths with
// spaces or non-ASCII characters are passed through unchanged on Windows and Linux.
#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

namespace dw5xl::setup {

struct ProcessResult
{
    bool started = false;
    int exitCode = -1;
    bool cancelled = false; // stopped because `cancel` was set
    std::string error;      // why it could not be started
};

// Runs `program` with `args` in `workingDirectory`; stdout and stderr go to `logFile` (appended).
// When `cancel` becomes true the program and everything it started (Ninja's compilers) are stopped.
ProcessResult runProcess(const std::filesystem::path &program, const std::vector<std::string> &args,
                         const std::filesystem::path &workingDirectory, const std::filesystem::path &logFile,
                         const std::atomic<bool> *cancel = nullptr);

// Starts `program` and returns at once (the game from the wizard's Play button); it keeps running after the
// caller exits. false + error if it could not be started.
bool launchDetached(const std::filesystem::path &program, const std::vector<std::string> &args,
                    const std::filesystem::path &workingDirectory, std::string &error);

} // namespace dw5xl::setup
