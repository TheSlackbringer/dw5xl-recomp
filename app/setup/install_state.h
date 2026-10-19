#pragma once
// Installed mode (issue 011 v4, the Unleashed model): where a player's game files, saves and settings live, and
// whether a verified install is there. The release carries the compiled game; the player's own disc supplies the
// game files, extracted once by the wizard into the player data folder.
#include "disc_setup.h"

#include <filesystem>
#include <string>

namespace dw5xl::setup {

// Windows: %LOCALAPPDATA%\DW5XL. Linux: $XDG_DATA_HOME/dw5xl, else ~/.local/share/dw5xl.
// DW5XL_DATA_DIR overrides it (tests, portable installs). Empty if none can be found.
std::filesystem::path playerDataDir();

// The folder a disc's files are extracted to: <data>/game/<disc.folder>.
std::filesystem::path discFolder(const std::filesystem::path &dataDir, const DiscSpec &disc);

enum class InstallState
{
    Installed,  // marker written after a verified extraction, every file present with its size
    Missing,    // no marker
    Incomplete, // marker, but a file is missing or has another size (repair)
};

// Fast check (sizes, no hashing): the full checksum check happened when the files were extracted.
InstallState installState(const std::filesystem::path &dataDir, const DiscSpec &disc, std::string *problem = nullptr);

// Written by the wizard after extractDisc succeeded (every checksum matched).
bool writeInstallMarker(const std::filesystem::path &dataDir, const DiscSpec &disc, std::string &error);

} // namespace dw5xl::setup
