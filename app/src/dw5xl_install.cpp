// Installed mode (issue 011 v4, the Unleashed model). The released game is started without arguments (a shortcut or
// a double-click): its game files, memory card and settings live in the player data folder, extracted once from the
// player's own disc by the wizard. Started with a guest ELF (developer runs, all tests) nothing here applies.
//   dw5xl                                   play; on first start the wizard asks for the disc image
//   dw5xl --install <DW5XL.iso> [--dw5-iso <DW5.iso>]
//                                           install without a window (tests), exit code 0 = installed
//   dw5xl --add-disc <image.iso>            add or repair one disc (what the in-game panel does), no window
// DW5XL_DATA_DIR overrides the player data folder.
// In the running game (M7, M10): when Load Original asks for the DW5 disc and it is not installed, an "Insert disc"
// window opens over the game and the drive reports "not ready" until the disc is installed — then Load Original
// continues by itself; Cancel lets the game find no disc once (its own prompt). F9 opens the Discs window.
#include "codegen.h"
#include "disc_wait.h"
#include "install_state.h"
#include "ps2_debug_panel.h"
#include "wizard_ui.h"

#include "imgui.h"
#include "raylib.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

namespace fs = std::filesystem;
using namespace dw5xl::setup;

void attachParentConsole(); // setup/wizard_console.cpp

namespace {
fs::path g_dataDir;                     // set when the game runs from the player data folder
DiscWait g_discWait;                      // the game's DW5 disc check while the Insert-disc window is open
std::atomic<bool> g_closeWindow{false};   // the requested disc is installed: close the Insert-disc window
bool g_discsWindow = false;             // F9
std::unique_ptr<DiscInstallPanel> g_panel;
std::string g_checkResult;

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

    fs::path xlIso, dw5Iso, addDisc;
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
        else if (arg == "--add-disc" && i + 1 < argc)
            addDisc = fromUtf8(argv[++i]);
    }
    if (!addDisc.empty())
    {
        attachParentConsole();
        IsoImage iso;
        std::string error;
        const DiscSpec *disc = nullptr;
        if (iso.open(addDisc, error))
            for (const auto &candidate : kSupportedDiscs)
            {
                const DiscSpec one[] = {candidate};
                if (identifyDisc(iso, one, error))
                    disc = &candidate;
            }
        iso.close();
        if (!disc)
        {
            std::fprintf(stderr, "%s is not a supported disc\n", utf8(addDisc).c_str());
            exitCode = 1;
            return false;
        }
        options.xlIso = addDisc;
        options.discs = std::span<const DiscSpec>(disc, 1);
        std::printf("adding %s into %s\n", disc->name, utf8(data).c_str());
        exitCode = runWizardConsole(options);
        return false;
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
    g_dataDir = data;
    elf = fs::path(".") / bootElfName();
    return true;
}

// The game's disc check (dw5xl_overlays.cpp, sceCdDiskReady) — game thread, once per frame while it waits.
// which: 0 = Dynasty Warriors 5 (Load Original). Installed mode only. true = report the drive "not ready": the
// requested disc is not installed yet and the Insert-disc window is open for it.
bool dw5xlDiscNotReady(uint32_t which)
{
    if (g_dataDir.empty() || which != 0u)
        return false;
    static auto lastCheck = std::chrono::steady_clock::time_point{};
    static bool installed = false;
    const auto now = std::chrono::steady_clock::now();
    if (!g_discWait.waiting() || now - lastCheck > std::chrono::seconds(1)) // the window's install, or --add-disc
    {
        installed = installState(g_dataDir, kSupportedDiscs[1]) == InstallState::Installed;
        lastCheck = now;
    }
    DiscWait::Event event;
    const bool notReady = g_discWait.notReady(installed, event);
    if (event == DiscWait::Event::WindowOpened)
        std::fprintf(stderr, "[install] the game asks for %s: Insert-disc window open, drive not ready\n",
                     kSupportedDiscs[1].name);
    else if (event == DiscWait::Event::DiscReady)
    {
        g_closeWindow = true;
        std::fprintf(stderr, "[install] %s installed: drive ready, the game continues\n", kSupportedDiscs[1].name);
    }
    return notReady;
}

// The game asked for the disc type (dw5xl_overlays.cpp): this disc check is over.
void dw5xlDiscChecked(uint32_t which)
{
    if (which == 0u)
        g_discWait.checked();
}

namespace {
const DiscSpec &dw5Disc() { return kSupportedDiscs[1]; }

bool overlayWanted()
{
    if (IsKeyPressed(KEY_F9))
        g_discsWindow = !g_discsWindow;
    return g_discWait.waiting() || g_discsWindow || (g_panel && g_panel->running());
}

void drawOverlay()
{
    if (g_closeWindow.exchange(false)) // the disc is in: the game goes on, the window goes away
    {
        g_panel.reset();
        return;
    }
    if (IsFileDropped())
    {
        FilePathList files = LoadDroppedFiles();
        if (files.count > 0 && g_panel)
            g_panel->dropped(fromUtf8(files.paths[0]));
        UnloadDroppedFiles(files);
    }
    const ImVec2 screen{float(GetScreenWidth()), float(GetScreenHeight())};
    ImGui::SetNextWindowPos(ImVec2(screen.x * 0.5f, screen.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(620.0f, 0.0f), ImGuiCond_Appearing);
    bool open = true;
    const bool requested = g_discWait.waiting();
    const char *title = requested ? "Insert disc###discs" : "Discs (F9)###discs";
    if (ImGui::Begin(title, &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (requested)
        {
            ImGui::TextWrapped("The game asks for the %s disc.", dw5Disc().name);
            if (!g_panel)
                g_panel = std::make_unique<DiscInstallPanel>(g_dataDir, dw5Disc());
            if (g_panel->draw())
                ImGui::TextWrapped("The game continues.");
            else if (!g_panel->running() && ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
                open = false; // the game then finds no disc and shows its own question
        }
        else
        {
            for (const auto &disc : kSupportedDiscs)
            {
                std::string problem;
                const InstallState state = installState(g_dataDir, disc, &problem);
                ImGui::Text("%s: %s", disc.name, state == InstallState::Installed  ? "installed"
                                                 : state == InstallState::Missing ? "not installed"
                                                                                  : "needs a repair");
            }
            if (!g_panel)
            {
                if (ImGui::Button("Add or repair a disc...", ImVec2(220.0f, 0.0f)))
                    g_panel = std::make_unique<DiscInstallPanel>(g_dataDir, dw5Disc());
                ImGui::SameLine();
                if (ImGui::Button("Check the files", ImVec2(160.0f, 0.0f)))
                {
                    g_checkResult.clear();
                    for (const auto &disc : kSupportedDiscs)
                    {
                        std::string problem;
                        if (installState(g_dataDir, disc, &problem) == InstallState::Incomplete)
                            g_checkResult += std::string(disc.name) + ": " + problem + "\n";
                    }
                    if (g_checkResult.empty())
                        g_checkResult = "All installed files are there with the right sizes.";
                }
                if (!g_checkResult.empty())
                    ImGui::TextWrapped("%s", g_checkResult.c_str());
            }
            else
            {
                ImGui::Separator();
                g_panel->draw();
            }
        }
    }
    ImGui::End();
    if (!open && !(g_panel && g_panel->running()))
    {
        if (requested)
        {
            g_discWait.cancel(); // the game finds no disc and shows its own question
            std::fprintf(stderr, "[install] Insert-disc window cancelled\n");
        }
        g_discsWindow = false;
        g_panel.reset();
        g_checkResult.clear();
    }
}
} // namespace

// Called by the runtime's main once the debug panel exists: the game overlay, and the F1 debugger only in developer
// runs (a player build has it off unless PS2X_DEBUG_UI=1, owner 2026-10-19).
void dw5xlConfigureDebugPanel(PS2DebugPanel &panel)
{
    if (!g_dataDir.empty())
    {
        const char *debug = std::getenv("PS2X_DEBUG_UI");
        const bool enabled = debug && *debug == '1';
        panel.setDebuggerEnabled(enabled);
        panel.setVisible(false);
        std::fprintf(stderr, "[install] F1 debugger %s\n", enabled ? "on (PS2X_DEBUG_UI=1)" : "off (player build)");
    }
    panel.setOverlay(overlayWanted, drawOverlay);
}
