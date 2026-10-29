// dw5xl_setup — builds the game from your own disc (issue 011).
// In a release package (a BUILD_ID file next to app/) it is the launcher the player starts every time (v5):
//   - the game is built for this release and the disc is installed -> start the game, close;
//   - otherwise the wizard: disc image -> check, copy, translate, VU1, compile -> start the game.
//   Everything goes to the player data folder (%LOCALAPPDATA%\DW5XL, ~/.local/share/dw5xl, DW5XL_DATA_DIR).
// In the developer tree it builds into the project as before.
//   dw5xl_setup                         the window
//   dw5xl_setup --iso <DW5XL.iso> [--dw5-iso <DW5.iso>] [--root <dir>] --no-window
//                                       the same run without a window (prints the steps, exit code 0 = done)
//   dw5xl_setup --rebuild               release: build again even if the game is up to date
#include "codegen.h"
#include "install_state.h"
#include "run_process.h"
#include "setup_layout.h"
#include "setup_cache.h"
#include "wizard_ui.h"

#include <cstdio>
#include <fstream>
#include <string>

void attachParentConsole(); // wizard_console.cpp

using namespace dw5xl::setup;
namespace fs = std::filesystem;

namespace {
#ifdef _WIN32
constexpr bool kWindows = true;
#else
constexpr bool kWindows = false;
#endif

struct Args
{
    fs::path xlIso, dw5Iso, root;
    bool noWindow = false;
    bool rebuild = false;
    bool checkReady=false;
};

std::string firstLine(const fs::path &path)
{
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    return line;
}

Args parseArgs(int argc, char **argv)
{
    Args a;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--iso" && hasValue)
            a.xlIso = fromUtf8(argv[++i]);
        else if (arg == "--dw5-iso" && hasValue)
            a.dw5Iso = fromUtf8(argv[++i]);
        else if (arg == "--root" && hasValue)
            a.root = fromUtf8(argv[++i]);
        else if (arg == "--no-window")
            a.noWindow = true;
        else if(arg=="--check-ready") a.checkReady=true;
        else if (arg == "--rebuild")
            a.rebuild = true;
    }
    return a;
}
} // namespace

int main(int argc, char **argv)
{
    const Args args = parseArgs(argc, argv);
    if (args.noWindow)
        attachParentConsole();
    std::error_code ec;
    fs::path root = args.root;
    if (root.empty())
        root = findRoot(argc > 0 ? fs::absolute(fromUtf8(argv[0]), ec).parent_path() : fs::current_path(ec));
    if (root.empty())
        root = findRoot(fs::current_path(ec));

    SetupOptions options;
    std::string layoutError;
    SetupLayout layout;
    if (root.empty())
        layoutError = "cannot find the project folder (app/ and recomp/) next to this program";
    else if (findLayout(root, kWindows, layout, layoutError))
    {
        options.recompiler = layout.recompiler;
        options.tools = layout.tools;
    }
    options.root = root;

    // Release package: the launcher.
    const std::string release = root.empty() ? std::string() : firstLine(root / "BUILD_ID");
    fs::path game, stamp, data;
    if (!release.empty())
    {
        data = playerDataDir();
        if (data.empty())
            layoutError = "no player data folder (LOCALAPPDATA / HOME not set)";
        options.dataDir = data;
        options.releaseId=release;
        options.usePersistentBuilder=true;
        options.reuseGenerated=true;
        options.forceGenerate=args.rebuild;
        game = options.effectiveBuildDir() / (kWindows ? "dw5xl.exe" : "dw5xl");
        stamp = options.effectiveBuildDir() / "built-from.txt";
    }
    const auto installed = [&] { return installState(data, kSupportedDiscs[0]) == InstallState::Installed; };
    const auto startGame = [&] {
#ifndef _WIN32
        // Keep the AppImage mount alive while the game uses its bundled libraries.
        const ProcessResult result = runProcess(game, {}, data, data / "logs" / "game.log");
        if (result.started)
            return result.exitCode;
        std::fprintf(stderr, "dw5xl_setup: %s\n", result.error.c_str());
        return 1;
#else
        std::string error;
        if (!launchDetached(game, {}, data, error))
        {
            std::fprintf(stderr, "dw5xl_setup: %s\n", error.c_str());
            return 1;
        }
        return 0;
#endif
    };
    std::string readyError;
    const bool ready=!release.empty() && layoutError.empty() && !args.rebuild &&
        fs::exists(game) && installed() && buildReady(root,options.effectiveBuildDir(),release,readyError);
    if(args.checkReady) {std::printf("%s\n",ready?"ready":"setup required");return ready?0:3;}
    if(ready && !args.noWindow) return startGame();

    if (args.noWindow)
    {
        if (!layoutError.empty())
        {
            std::fprintf(stderr, "dw5xl_setup: %s\n", layoutError.c_str());
            return 2;
        }
        if (args.xlIso.empty() && !(!release.empty() && installed()))
        {
            std::fprintf(stderr, "usage: dw5xl_setup --iso <DW5XL.iso> [--dw5-iso <DW5.iso>] [--root <dir>] --no-window\n");
            return 2;
        }
        options.xlIso = args.xlIso;
        options.dw5Iso = args.dw5Iso;
        const int rc = runWizardConsole(options);
        return rc;
    }
    WizardConfig config{options, layoutError, args.xlIso, args.dw5Iso};
    if (!release.empty())
    {
        config.discInstalled = installed();
        config.dw5Installed = installState(data, kSupportedDiscs[1]) == InstallState::Installed;
        config.closeWhenFinished = true;
    }
    if (runWizardWindow(config) && !release.empty())
    {
        return startGame();
    }
    return 0;
}
