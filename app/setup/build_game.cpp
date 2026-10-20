#include "build_game.h"
#include "run_process.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iterator>
#include <regex>
#include <thread>

namespace dw5xl::setup {
namespace fs = std::filesystem;

namespace {
std::string utf8(const fs::path &path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

std::string readText(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

// Runs a step in a worker thread and reports Ninja's progress from its log while it runs.
ProcessResult runWithProgress(const fs::path &program, const std::vector<std::string> &args,
                              const fs::path &workingDirectory, const fs::path &logFile, const std::string &phase,
                              const std::function<void(const BuildProgress &)> &progress,
                              const std::atomic<bool> *cancel)
{
    ProcessResult result;
    std::atomic<bool> finished{false};
    std::thread worker([&] {
        result = runProcess(program, args, workingDirectory, logFile, cancel);
        finished = true;
    });
    while (!finished)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (progress)
        {
            BuildProgress p = parseNinjaProgress(readText(logFile));
            p.phase = phase;
            progress(p);
        }
    }
    worker.join();
    return result;
}
} // namespace

std::vector<std::string> gameConfigureArgs(const fs::path &root, const fs::path &buildDir, const BuildTools &tools,
                                           const fs::path &recompRoot)
{
    std::vector<std::string> args = {"-S", utf8(root / "app"), "-B", utf8(buildDir), "-G", "Ninja",
                                     "-DCMAKE_BUILD_TYPE=Release", "-DDW5XL_BUILD_GAME=ON", "-DDW5XL_BUILD_TESTS=OFF",
                                     "-DPS2X_ENABLE_SCCACHE=OFF", "-DPS2X_ENABLE_AGRESSIVE_LOGS=OFF",
                                     "-DPS2X_ENABLE_VULKAN_GS=ON",
                                     "-DPS2X_PARALLEL_GS_SOURCE_DIR=" + utf8(root / "app" / "deps" / "parallel-gs")};
    if (!recompRoot.empty())
        args.push_back("-DDW5XL_RECOMP_ROOT=" + utf8(recompRoot));
    if (!tools.ninja.empty())
        args.push_back("-DCMAKE_MAKE_PROGRAM=" + utf8(tools.ninja));
    if (!tools.llvmMingwRoot.empty())
    {
        args.push_back("-DCMAKE_TOOLCHAIN_FILE=" + utf8(root / "tools" / "windows" / "llvm-mingw.cmake"));
        args.push_back("-DLLVM_MINGW_ROOT=" + utf8(tools.llvmMingwRoot));
    }
    else
    {
        args.push_back("-DCMAKE_C_COMPILER=clang");
        args.push_back("-DCMAKE_CXX_COMPILER=clang++");
        args.push_back("-DCMAKE_LINKER_TYPE=LLD");
    }
    if (!tools.ffmpegArchive.empty())
        args.push_back("-DPS2X_FFMPEG_PREBUILT_URL=" + utf8(tools.ffmpegArchive));
    // The release ships the dependency sources next to app/ (no downloads while building).
    for (const char *dep : {"imgui", "raylib", "rlimgui"})
    {
        const fs::path src = root / "app" / "deps" / (std::string(dep) + "-src");
        if (fs::exists(src))
        {
            std::string var = dep;
            for (char &c : var)
                c = char(std::toupper(static_cast<unsigned char>(c)));
            args.push_back("-DFETCHCONTENT_SOURCE_DIR_" + var + "=" + utf8(src));
        }
    }
    if (fs::exists(root / "app" / "deps" / "raylib-src"))
        args.push_back("-DFETCHCONTENT_FULLY_DISCONNECTED=ON");
    return args;
}

BuildProgress parseNinjaProgress(const std::string &logText)
{
    BuildProgress p;
    static const std::regex re(R"(\[(\d+)/(\d+)\])");
    const size_t from = logText.size() > 4096 ? logText.size() - 4096 : 0; // only the last lines matter
    const std::string tail = logText.substr(from);
    for (std::sregex_iterator it(tail.begin(), tail.end(), re), end; it != end; ++it)
    {
        p.done = std::stoi((*it)[1].str());
        p.total = std::stoi((*it)[2].str());
    }
    return p;
}

bool buildGame(const fs::path &root, const fs::path &buildDir, const BuildTools &tools, const fs::path &logFile,
               const std::function<void(const BuildProgress &)> &progress, fs::path &executable, std::string &error,
               const std::atomic<bool> *cancel, const fs::path &recompRoot)
{
    std::error_code ec;
    fs::create_directories(buildDir, ec);
    fs::create_directories(logFile.parent_path(), ec);
    fs::remove(logFile, ec);
    const ProcessResult configure =
        runWithProgress(tools.cmake, gameConfigureArgs(root, buildDir, tools, recompRoot), root, logFile, "configure", progress,
                        cancel);
    if (configure.cancelled)
    {
        error = "cancelled";
        return false;
    }
    if (!configure.started)
    {
        error = configure.error;
        return false;
    }
    if (configure.exitCode != 0)
    {
        error = "configuring the build failed; see " + logFile.string();
        return false;
    }
    const ProcessResult build =
        runWithProgress(tools.cmake, {"--build", utf8(buildDir), "--target", "dw5xl"}, root, logFile, "build", progress,
                        cancel);
    if (build.cancelled)
    {
        error = "cancelled";
        return false;
    }
    if (!build.started)
    {
        error = build.error;
        return false;
    }
    if (build.exitCode != 0)
    {
        error = "compiling the game failed; see " + logFile.string();
        return false;
    }
    executable = buildDir / (tools.llvmMingwRoot.empty() ? "dw5xl" : "dw5xl.exe");
    if (!fs::exists(executable))
    {
        error = "the build finished but " + executable.string() + " is missing";
        return false;
    }
    return true;
}

} // namespace dw5xl::setup
