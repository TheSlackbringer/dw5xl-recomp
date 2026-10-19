#pragma once
// "Browse..." for the setup wizard (issue 011): the system's own file picker for a disc image.
// Windows: the standard Open dialog. Linux: zenity or kdialog when installed. Drag and drop and typing the path
// work everywhere, so a missing picker is never a dead end.
#include <filesystem>
#include <string>

namespace dw5xl::setup {

// true if this system has a picker the wizard can use.
bool fileDialogAvailable();

// Shows the picker; returns the chosen file, or empty when cancelled or unavailable.
std::filesystem::path pickDiscImage(const std::string &title);

} // namespace dw5xl::setup
