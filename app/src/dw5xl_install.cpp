// Installed mode (issue 011 v4, the Unleashed model). The released game is started without arguments (a shortcut or
// a double-click): its game files, memory card and settings live in the player data folder, extracted once from the
// player's own disc by the wizard. Started with a guest ELF (developer runs, all tests) nothing here applies.
//   dw5xl                                   play; on first start the wizard asks for the disc image
//   dw5xl --install <DW5XL.iso> [--dw5-iso <DW5.iso>]
//                                           install without a window (tests), exit code 0 = installed
// DW5XL_DATA_DIR overrides the player data folder.
#include "codegen.h"
#include "install_state.h"
#include "wizard_ui.h"

#include <cstdio>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace dw5xl::setup;

void attachParentConsole(); // setup/wizard_console.cpp

namespace {
fs::path bootElfName() { return fs::path(kCodegenProfiles[0].input).filename(); }

// Without a console (a Windows window program, or started from a desktop shortcut) the log goes to a file.
void logToFile(const fs::path &dataDir)
{
    std::error_code ec;
    fs::create_directories(dataDir / "logs", ec);
    const fs::path log = dataDir / "logs" / "game.log";
    std::fprintf(stderr, "[install] log: %s\n", utf8(log).c_str());
    std::fflush(stderr);
    if (std::FILE *f = std::freopen(log.string().c_str(), "w", stderr))
    {
        (void)f;
        std::freopen(log.string().c_str(), "a", stdout);
    }
}
} // namespace

// Called by the runtime's main when no guest ELF was given. Returns true with `elf` set (and the working folder
// changed to the game files) when the game should boot; false with `exitCode` when the process should end.
bool dw5xlInstalledBoot(int argc, char **argv, fs::path &elf, int &exitCode)
{
    const fs::path data = playerDataDir();
    if (data.empty())
    {
        std::fprintf(stderr, "[install] no player data folder (set DW5XL_DATA_DIR)\n");
        exitCode = 1;
        return false;
    }

    SetupOptions options;
    options.root = data;
    options.installOnly = true;
    options.logDir = data / "logs";

    fs::path xlIso, dw5Iso;
    bool install = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--install" && i + 1 < argc)
        {
            install = true;
            xlIso = fromUtf8(argv[++i]);
        }
        else if (arg == "--dw5-iso" && i + 1 < argc)
            dw5Iso = fromUtf8(argv[++i]);
    }
    if (install)
    {
        attachParentConsole();
        options.xlIso = xlIso;
        options.dw5Iso = dw5Iso;
        std::printf("installing into %s\n", utf8(data).c_str());
        exitCode = runWizardConsole(options);
        return false;
    }

    std::string problem;
    const InstallState state = installState(data, kSupportedDiscs[0], &problem);
    if (state != InstallState::Installed)
    {
        if (state == InstallState::Incomplete)
            std::fprintf(stderr, "[install] the installed game files need a repair: %s\n", problem.c_str());
        if (!runWizardWindow(WizardConfig{options, {}, {}, {}}))
        {
            exitCode = 0; // the player closed the wizard
            return false;
        }
    }

    logToFile(data);
    std::error_code ec;
    fs::current_path(discFolder(data, kSupportedDiscs[0]), ec);
    if (ec)
    {
        std::fprintf(stderr, "[install] cannot open the game folder: %s\n", ec.message().c_str());
        exitCode = 1;
        return false;
    }
    std::fprintf(stderr, "[install] playing from %s\n", utf8(fs::current_path()).c_str());
    elf = fs::path(".") / bootElfName();
    return true;
}
