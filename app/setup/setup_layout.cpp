#include "setup_layout.h"
#include "codegen.h"
#include "setup_cache.h"

#include <fstream>
#include <iterator>

namespace dw5xl::setup {
namespace fs = std::filesystem;

bool prepareLinuxBuildRoot(const fs::path &root, const fs::path &data, fs::path &buildRoot, std::string &error)
{
    return prepareBuildRoot(root,data,buildRoot,error);
}

bool prepareBuildRoot(const fs::path &root,const fs::path &data,fs::path &buildRoot,std::string &error,
                      const std::function<bool(uint64_t,uint64_t)> &progress)
{
    buildRoot = data / "builder";
    for (const char *folder : {"app", "external", "recomp", "tools"})
        if (!syncDirectory(root / folder, buildRoot / folder, error, true,progress))
            return false;
    const auto read = [](const fs::path &path) {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    const std::string version = read(root / "BUILD_ID");
    if (version != read(buildRoot / "BUILD_ID"))
    {
        if(!atomicWrite(buildRoot/"BUILD_ID",version,error)) return false;
    }
    return true;
}

fs::path findRoot(const fs::path &start)
{
    std::error_code ec;
    fs::path dir = fs::absolute(start, ec).lexically_normal();
    while (!dir.empty())
    {
        if (fs::exists(dir / "app" / "CMakeLists.txt", ec) && fs::is_directory(dir / "recomp", ec))
            return dir;
        const fs::path parent = dir.parent_path();
        if (parent == dir)
            break;
        dir = parent;
    }
    return {};
}

bool findLayout(const fs::path &root, bool windows, SetupLayout &layout, std::string &error)
{
    const std::string exe = windows ? ".exe" : "";
    const fs::path tools = root / "tools";
    std::error_code ec;
    layout = {};
    layout.root = root;

    const fs::path bundled = tools / ("ps2_recomp" + exe);
    const fs::path developer = root / "external" / "PS2Recomp" / "build" / "ps2xRecomp" / ("ps2_recomp" + exe);
    layout.recompiler = fs::exists(bundled, ec) ? bundled : developer;
    if (!fs::exists(layout.recompiler, ec))
    {
        error = "the recompiler is missing (" + bundled.string() + ")";
        return false;
    }

    const fs::path cmake = tools / "cmake" / "bin" / ("cmake" + exe);
    layout.tools.cmake = fs::exists(cmake, ec) ? cmake : fs::path("cmake");
    const fs::path ninja = tools / ("ninja" + exe);
    if (fs::exists(ninja, ec))
        layout.tools.ninja = ninja;
    if (windows)
    {
        layout.tools.llvmMingwRoot = tools / "llvm-mingw";
        if (!fs::is_directory(layout.tools.llvmMingwRoot, ec))
        {
            error = "the bundled compiler is missing (" + layout.tools.llvmMingwRoot.string() + ")";
            return false;
        }
        if (fs::exists(tools / "ffmpeg.zip", ec))
            layout.tools.ffmpegArchive = tools / "ffmpeg.zip";
    }
    else if (fs::exists(root / "BUILD_ID", ec))
    {
        layout.tools.linuxToolchainRoot = tools / "clang";
        layout.tools.ffmpegRoot = tools / "ffmpeg";
        for (const fs::path &required : {cmake, ninja, tools / "clang" / "bin" / "clang",
                                        tools / "clang" / "bin" / "clang++",
                                        tools / "ffmpeg" / "include" / "libavcodec" / "avcodec.h"})
        {
            if (!fs::exists(required, ec))
            {
                error = "a bundled Linux build tool is missing (" + required.string() + ")";
                return false;
            }
        }
    }
    return true;
}

} // namespace dw5xl::setup
