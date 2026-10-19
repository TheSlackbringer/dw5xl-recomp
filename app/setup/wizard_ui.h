#pragma once
// The setup wizard's window and its no-window run (issue 011), shared by dw5xl_setup (developer path: build from
// your own disc) and the game itself (player path, v4: install the game files from your own disc on first start).
#include "setup_pipeline.h"

#include <filesystem>
#include <string>

namespace dw5xl::setup {

std::string utf8(const std::filesystem::path &path);
std::filesystem::path fromUtf8(const std::string &text);

struct WizardConfig
{
    SetupOptions base;       // base.installOnly: the player install (check + extract), else the developer build
    std::string layoutError; // shown instead of allowing a start (incomplete package)
    std::filesystem::path xlIso, dw5Iso; // prefilled fields
};

// Runs the wizard in its own window until the player closes it. Returns true if a run finished; in install mode
// the window then closes by itself so the game can start.
bool runWizardWindow(const WizardConfig &config);

// The same run without a window, printing the steps; returns 0 when it finished.
int runWizardConsole(const SetupOptions &options);

} // namespace dw5xl::setup
