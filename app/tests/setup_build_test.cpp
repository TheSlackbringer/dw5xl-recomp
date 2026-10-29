// Setup wizard step "compile the game" (issue 011): Ninja progress parsing, the configure arguments, where the
// tools are found, and that a cancel stops a running program. The real build needs generated game code and is
// checked locally (see notes, issue 011).
#include "build_game.h"
#include "run_process.h"
#include "setup_layout.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <mutex>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#include <signal.h>
#include <cerrno>
#include <sys/wait.h>
#include <fcntl.h>
#endif

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
    require(prepareBuildRoot(moved,data,selected,error),"the same stable source cache is used on Windows");
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
    if(argc==2 && std::string(argv[1])=="--exit-seven") return 7;
#ifdef __linux__
    if(argc==2 && std::string(argv[1])=="--check-closed-input")
        return fcntl(STDIN_FILENO,F_GETFD)<0 && errno==EBADF ? 0 : 6;
    if(argc==2 && std::string(argv[1])=="--closed-input-owner")
    {
        close(STDIN_FILENO);
        const auto r=runProcess(fs::absolute(argv[0]),{"--check-closed-input"},fs::current_path(),"input.log");
        return r.started && r.exitCode==0 && r.error.empty() ? 0 : 6;
    }
#endif
    if(argc==2 && std::string(argv[1])=="--leaf")
    {
#ifdef _WIN32
        const auto pid=GetCurrentProcessId();
#else
        const auto pid=getpid();
#endif
        std::cout<<"leaf "<<pid<<'\n'<<std::flush;
        std::this_thread::sleep_for(std::chrono::seconds(30));return 0;
    }
    if(argc==3 && std::string(argv[1])=="--tree")
        return runProcess(fs::absolute(argv[0]),{"--leaf"},fs::current_path(),fs::path(argv[2])/"tree.log").exitCode;
    if(argc==3 && std::string(argv[1])=="--observed-tree")
        return runProcessObserved(fs::absolute(argv[0]),{"--tree",argv[2]},fs::current_path(),
            fs::path(argv[2])/"tree.log",[](const std::string &) {return true;}).exitCode;
#ifdef __linux__
    if(argc==2 && std::string(argv[1])=="--escaped-tree")
    {
        const pid_t leaf=fork();
        if(leaf<0) return 3;
        if(leaf==0)
        {
            if(setsid()<0) _exit(4);
            std::cout<<"leaf "<<getpid()<<'\n'<<std::flush;
            std::this_thread::sleep_for(std::chrono::seconds(30));_exit(0);
        }
        int status=0;
        return waitpid(leaf,&status,0)==leaf ? 0 : 5;
    }
#endif
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
        require(parseNinjaProgress("[9/10] Linking CXX executable game").phase=="link",
                "linking has distinct activity without inventing a separate percentage");
        const auto bad=parseNinjaProgress("[9999999999999999999999/9] partial [8/2] [1/");
        require(bad.done==0 && bad.total==0,"malformed, overflowing and impossible counters are ignored");

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
        { // 042: cancel only after the real grandchild has started, then verify it terminated.
            const auto dir=fs::temp_directory_path()/"dw5xl_setup_tree_test";
            fs::remove_all(dir);fs::create_directories(dir);unsigned long leaf=0;
#ifdef _WIN32
            HANDLE process=nullptr;
#endif
            const auto begin=std::chrono::steady_clock::now();
            const auto result=runProcessObserved(fs::absolute(argv[0]),{"--tree",dir.string()},dir,dir/"tree.log",
                [&](const std::string &line) {
                    if(line.rfind("leaf ",0)!=0) return true;
                    leaf=std::stoul(line.substr(5));
#ifdef _WIN32
                    process=OpenProcess(SYNCHRONIZE,FALSE,DWORD(leaf));
#endif
                    return false;
                });
            require(result.started && result.cancelled && leaf,"running grandchild observed before cancellation");
            require(std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count()<2,
                    "actual child/tree cancellation completes within two seconds");
#ifdef _WIN32
            require(process && WaitForSingleObject(process,0)==WAIT_OBJECT_0,"grandchild is terminated");
            CloseHandle(process);
#else
            const bool absent=kill(pid_t(leaf),0)==-1 && errno==ESRCH;
            std::ifstream state("/proc/"+std::to_string(leaf)+"/stat");std::string stat;std::getline(state,stat);
            const bool zombie=stat.find(") Z ")!=std::string::npos;
            require(absent || zombie,"grandchild stopped executing");
#endif
            fs::remove_all(dir);
        }
#ifdef __linux__
        { // Independent concurrent launches must not share cancellation ownership.
            const auto dir=fs::temp_directory_path()/"dw5xl_setup_independent_tree_test";
            fs::remove_all(dir);fs::create_directories(dir);
            std::atomic<bool> stop{false},finished{false};
            std::mutex readyMutex;std::condition_variable ready;pid_t sentinel=0;
            ProcessResult survivor;
            std::thread independent([&] {
                survivor=runProcessObserved(fs::absolute(argv[0]),{"--leaf"},dir,dir/"independent.log",
                    [&](const std::string &line) {
                        if(line.rfind("leaf ",0)==0) {
                            {std::lock_guard lock(readyMutex);sentinel=pid_t(std::stoul(line.substr(5)));}
                            ready.notify_one();
                        }
                        return true;
                    },&stop);
                finished=true;
            });
            try
            {
                {std::unique_lock lock(readyMutex);
                 require(ready.wait_for(lock,std::chrono::seconds(2),[&] {return sentinel!=0;}),
                         "independent process starts within two seconds");}
                for(const auto &mode : {std::string("--observed-tree"),std::string("--escaped-tree")})
                {
                    unsigned long leaf=0;
                    const auto start=std::chrono::steady_clock::now();
                    std::vector<std::string> args{mode};
                    if(mode=="--observed-tree") args.push_back(dir.string());
                    const auto r=runProcessObserved(fs::absolute(argv[0]),args,dir,dir/"tree.log",
                        [&](const std::string &line) {
                            if(line.rfind("leaf ",0)!=0) return true;
                            leaf=std::stoul(line.substr(5));return false;
                        });
                    require(r.started && r.cancelled && r.error.empty() && leaf,
                            "nested observer or new-session descendant is observed and cancelled");
                    require(std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()<2,
                            "nested/new-session cancellation retains the two-second bound");
                    require(kill(pid_t(leaf),0)<0 && errno==ESRCH,"owned escaped descendant is reaped");
                    require(!finished && kill(sentinel,0)==0,"independent concurrent process survives cancellation");
                }
            }
            catch(...) {stop=true;independent.join();throw;}
            stop=true;independent.join();
            require(survivor.started && survivor.cancelled && survivor.error.empty(),
                    "independent process can be cancelled separately");
            const auto input=runProcess(fs::absolute(argv[0]),{"--closed-input-owner"},dir,dir/"input-owner.log");
            require(input.started && input.exitCode==0 && input.error.empty(),
                    "closed stdin is not replaced by an inherited ownership socket");
            const auto nonzero=runProcess(fs::absolute(argv[0]),{"--exit-seven"},dir,dir/"exit.log");
            require(nonzero.started && nonzero.exitCode==7 && nonzero.error.empty(),
                    "supervision preserves the actual command exit code");
            const auto missing=runProcess(dir/"absent-program",{},dir,dir/"missing.log");
            require(!missing.started && !missing.error.empty(),"exec failure is not reported as a started command");
            fs::remove_all(dir);
        }
#endif
#ifdef _WIN32
        { // Cancelling one owned tree must complete without stopping an independent launch.
            const auto dir=fs::temp_directory_path()/"dw5xl_setup_windows_independent_test";
            fs::remove_all(dir);fs::create_directories(dir);
            std::atomic<bool> stop{false},finished{false};
            std::mutex readyMutex;std::condition_variable ready;HANDLE sentinel=nullptr;
            ProcessResult survivor;
            std::thread independent([&] {
                survivor=runProcessObserved(fs::absolute(argv[0]),{"--leaf"},dir,dir/"independent.log",
                    [&](const std::string &line) {
                        if(line.rfind("leaf ",0)==0) {
                            HANDLE child=OpenProcess(SYNCHRONIZE,FALSE,DWORD(std::stoul(line.substr(5))));
                            {std::lock_guard lock(readyMutex);sentinel=child;}
                            ready.notify_one();
                        }
                        return true;
                    },&stop);
                finished=true;
            });
            try
            {
                {std::unique_lock lock(readyMutex);
                 require(ready.wait_for(lock,std::chrono::seconds(2),[&] {return sentinel!=nullptr;}),
                         "independent Windows process starts within two seconds");}
                HANDLE leaf=nullptr;
                const auto start=std::chrono::steady_clock::now();
                const auto result=runProcessObserved(fs::absolute(argv[0]),{"--tree",dir.string()},dir,dir/"tree.log",
                    [&](const std::string &line) {
                        if(line.rfind("leaf ",0)!=0) return true;
                        leaf=OpenProcess(SYNCHRONIZE,FALSE,DWORD(std::stoul(line.substr(5))));return false;
                    });
                const bool terminated=leaf && WaitForSingleObject(leaf,0)==WAIT_OBJECT_0;
                if(leaf) CloseHandle(leaf);
                require(result.started && result.cancelled && result.error.empty(),"owned Windows tree cancellation succeeds");
                require(std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()<2,
                        "independent-tree cancellation retains the two-second bound");
                require(terminated,"nested Windows descendant is terminated before return");
                require(!finished && WaitForSingleObject(sentinel,0)==WAIT_TIMEOUT,
                        "independent Windows process survives other tree cancellation");
            }
            catch(...) {stop=true;independent.join();if(sentinel) CloseHandle(sentinel);throw;}
            stop=true;independent.join();
            require(survivor.started && survivor.cancelled && survivor.error.empty() &&
                        WaitForSingleObject(sentinel,0)==WAIT_OBJECT_0,"independent Windows process can be cancelled separately");
            CloseHandle(sentinel);fs::remove_all(dir);
        }
#endif
        std::cout << "setup build tests passed\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
