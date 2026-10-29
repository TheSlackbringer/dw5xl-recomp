#include "build_game.h"
#include "run_process.h"
#include "setup_cache.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iterator>
#include <regex>
#include <thread>
#include <charconv>

namespace dw5xl::setup {
namespace fs = std::filesystem;

namespace {
std::string utf8(const fs::path &path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

// Consume only new output; growing build logs are never reread on each UI poll.
ProcessResult runWithProgress(const fs::path &program, const std::vector<std::string> &args,
                              const fs::path &workingDirectory, const fs::path &logFile, const std::string &phase,
                              const std::function<void(const BuildProgress &)> &progress,
                              const std::atomic<bool> *cancel)
{
    if(progress) progress({0,0,phase});
    return runProcessObserved(program,args,workingDirectory,logFile,[&](const std::string &line) {
        BuildProgress p=parseNinjaProgress(line);
        if(phase=="configure" || p.phase.empty()) p.phase=phase;
        if(progress && (p.total>0 || phase=="configure")) progress(p);
        return true;
    },cancel);
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
    else if (!tools.linuxToolchainRoot.empty())
    {
        args.push_back("-DCMAKE_TOOLCHAIN_FILE=" + utf8(root / "tools" / "linux" / "clang.cmake"));
        args.push_back("-DDW5XL_LINUX_TOOLCHAIN_ROOT=" + utf8(tools.linuxToolchainRoot));
    }
    else
    {
        args.push_back("-DCMAKE_C_COMPILER=clang");
        args.push_back("-DCMAKE_CXX_COMPILER=clang++");
        args.push_back("-DCMAKE_LINKER_TYPE=LLD");
    }
    if (!tools.ffmpegArchive.empty())
        args.push_back("-DPS2X_FFMPEG_PREBUILT_URL=" + utf8(tools.ffmpegArchive));
    if (!tools.ffmpegRoot.empty())
        args.push_back("-DPS2X_FFMPEG_ROOT=" + utf8(tools.ffmpegRoot));
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

bool resetBuildIfMoved(const fs::path &buildDir, const fs::path &sourceDir)
{
    std::ifstream cache(buildDir / "CMakeCache.txt");
    if (!cache)
        return false;
    const std::string key = "CMAKE_HOME_DIRECTORY:INTERNAL=";
    std::string line, configured;
    while (std::getline(cache, line))
        if (line.rfind(key, 0) == 0)
            configured = line.substr(key.size());
    cache.close();
    while (!configured.empty() && (configured.back() == '\r' || configured.back() == ' '))
        configured.pop_back();
    auto normal = [](const fs::path &p) {
        std::string text = utf8(p.lexically_normal());
        while (text.size() > 1 && text.back() == '/')
            text.pop_back();
#ifdef _WIN32
        for (char &c : text)
            c = char(std::tolower(static_cast<unsigned char>(c))); // Windows paths ignore case
#endif
        return text;
    };
    if (configured.empty() || normal(fs::path(configured)) == normal(sourceDir))
        return false;
    std::error_code ec;
    fs::remove(buildDir / "CMakeCache.txt", ec);
    fs::remove_all(buildDir / "CMakeFiles", ec);
    return true;
}

BuildProgress parseNinjaProgress(const std::string &logText)
{
    BuildProgress p;
    static const std::regex re(R"(\[(\d+)/(\d+)\])");
    const size_t from = logText.size() > 4096 ? logText.size() - 4096 : 0; // only the last lines matter
    const std::string tail = logText.substr(from);
    for (std::sregex_iterator it(tail.begin(), tail.end(), re), end; it != end; ++it)
    {
        const auto done=(*it)[1].str(),total=(*it)[2].str();int n=0,t=0;
        if(std::from_chars(done.data(),done.data()+done.size(),n).ec==std::errc{} &&
           std::from_chars(total.data(),total.data()+total.size(),t).ec==std::errc{} && t>0 && n<=t)
        {
            p.done=n;p.total=t;
            const auto after=size_t(it->position()+it->length());
            p.phase=tail.compare(after,9," Linking ")==0?"link":"build";
        }
    }
    return p;
}

std::string toolchainIdentity(const fs::path &root,const BuildTools &tools,std::string &error)
{
    std::vector<CacheInput> inputs;
    const auto sdk=!tools.llvmMingwRoot.empty()?tools.llvmMingwRoot:tools.linuxToolchainRoot;
    if(sdk.empty()) return {}; // developer PATH tools retain CMake's existing detection path
    if(!treeInputs(sdk,"compiler-sdk",inputs,error)) return {};
    inputs.push_back({"toolchain",root/"tools"/(!tools.llvmMingwRoot.empty()?"windows/llvm-mingw.cmake":"linux/clang.cmake")});
    return cacheKey(inputs,{{"purpose","native-toolchain-1"}},error);
}

bool buildGame(const fs::path &root, const fs::path &buildDir, const BuildTools &tools, const fs::path &logFile,
               const std::function<void(const BuildProgress &)> &progress, fs::path &executable, std::string &error,
               const std::atomic<bool> *cancel, const fs::path &recompRoot,
               ps2recomp::timing::Session *timing, uint64_t timingParent)
{
    std::error_code ec;
    fs::create_directories(buildDir, ec);
    fs::create_directories(logFile.parent_path(), ec);
    fs::remove(logFile, ec);
    if (resetBuildIfMoved(buildDir, root / "app"))
        std::ofstream(logFile, std::ios::app) << "-- the release moved to " << utf8(root)
                                              << ": the build configuration was reset\n";
    ps2recomp::timing::Stage toolIdentityTime(timing,"compiler_identity",timingParent);
    const auto identity=toolchainIdentity(root,tools,error);
    toolIdentityTime.field("compiler_sdk_sha256",identity.empty()?"unavailable":identity);
    const auto identityFile=buildDir/"setup-toolchain.record";
    if(!tools.llvmMingwRoot.empty() || !tools.linuxToolchainRoot.empty())
    {
        if(identity.empty()) return false;
        std::ifstream old(identityFile);std::string previous;std::getline(old,previous);old.close();
        if(previous!=identity && fs::exists(buildDir/"CMakeCache.txt"))
        {
            // CMakeFiles owns compiler/PCH objects; downloaded dependency sources in _deps are preserved.
            fs::remove(buildDir/"CMakeCache.txt");fs::remove_all(buildDir/"CMakeFiles");
            std::ofstream(logFile,std::ios::app)<<"-- bundled compiler identity changed: resetting native configuration and objects\n";
        }
    }
    toolIdentityTime.finish();
    ps2recomp::timing::Stage configureTime(timing, "configure", timingParent);
    const ProcessResult configure =
        runWithProgress(tools.cmake, gameConfigureArgs(root, buildDir, tools, recompRoot), root, logFile, "configure", progress,
                        cancel);
    configureTime.finish(configure.started && configure.exitCode == 0 ? "success" : configure.cancelled ? "cancelled" : "failed");
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
    if(!identity.empty() && !atomicWrite(identityFile,identity+"\n",error)) return false;
    ps2recomp::timing::Stage ninjaTime(timing, "ninja", timingParent);
    const ProcessResult build =
        runWithProgress(tools.cmake, {"--build", utf8(buildDir), "--target", "dw5xl"}, root, logFile, "build", progress,
                        cancel);
    ninjaTime.finish(build.started && build.exitCode == 0 ? "success" : build.cancelled ? "cancelled" : "failed");
    if (timing)
    {
        ps2recomp::timing::Stage snapshot(timing, "ninja_log_snapshot", timingParent);
        const fs::path original = buildDir / ".ninja_log";
        if (fs::exists(original))
        {
            fs::copy_file(original, logFile.parent_path() / ("ninja-" + timing->run() + ".tsv"),
                          fs::copy_options::overwrite_existing, ec);
            snapshot.finish(ec ? "failed" : "success");
        }
        else snapshot.finish("unavailable");
    }
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
