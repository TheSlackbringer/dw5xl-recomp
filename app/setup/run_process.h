#pragma once
// Starts a program with arguments and waits for it (issue 011 setup wizard). No shell is involved, so paths with
// spaces or non-ASCII characters are passed through unchanged on Windows and Linux.
#include <filesystem>
#include <string>
#include <vector>

namespace dw5xl::setup {

struct ProcessResult
{
    bool started = false;
    int exitCode = -1;
    std::string error; // why it could not be started
};

// Runs `program` with `args` in `workingDirectory`; stdout and stderr go to `logFile` (appended).
ProcessResult runProcess(const std::filesystem::path &program, const std::vector<std::string> &args,
                         const std::filesystem::path &workingDirectory, const std::filesystem::path &logFile);

} // namespace dw5xl::setup
