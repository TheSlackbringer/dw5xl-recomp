#pragma once
// The setup wizard's window and its no-window run (issue 011), shared by dw5xl_setup (developer path: build from
// your own disc) and the game itself (player path, v4: install the game files from your own disc on first start).
#include "setup_pipeline.h"

#include <filesystem>
#include <memory>
#include <string>

namespace dw5xl::setup {

std::string utf8(const std::filesystem::path &path);
std::filesystem::path fromUtf8(const std::string &text);

struct WizardConfig
{
    SetupOptions base;       // base.installOnly: the player install (check + extract), else the developer build
    std::string layoutError; // shown instead of allowing a start (incomplete package)
    std::filesystem::path xlIso, dw5Iso; // prefilled fields
    bool discInstalled = false;     // the XL files are already in the data folder: Start works without an image
    bool closeWhenFinished = false; // the release launcher: close the window when done (the game starts next)
};

// Runs the wizard in its own window until the player closes it. Returns true if a run finished; in install mode
// the window then closes by itself so the game can start.
bool runWizardWindow(const WizardConfig &config);

// The same run without a window, printing the steps; returns 0 when it finished.
int runWizardConsole(const SetupOptions &options);

// "Insert disc" panel drawn inside an imgui frame that already exists (the running game, issue 011 M7): one disc
// image field (typed, Browse, or a file dropped on the window), Install (check + copy + marker into the player data
// folder), progress and errors.
class DiscInstallPanel
{
public:
    DiscInstallPanel(std::filesystem::path dataDir, const DiscSpec &disc);
    ~DiscInstallPanel();
    // Draws the panel's contents (the caller owns the imgui window). Returns true once the disc is installed.
    bool draw();
    void dropped(const std::filesystem::path &image);
    bool running() const;

private:
    struct State;
    std::unique_ptr<State> m;
};

} // namespace dw5xl::setup
