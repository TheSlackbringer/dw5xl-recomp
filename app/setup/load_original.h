#pragma once
#include "install_state.h"

namespace dw5xl::setup {
enum class OriginalCheck { skipped, valid, unsupported };
struct OriginalReadiness {
    bool record = false;
    InstallState installed = InstallState::Missing;
    OriginalCheck check = OriginalCheck::skipped;
    double milliseconds = 0;
    bool allowed() const { return record && installed == InstallState::Installed && check == OriginalCheck::valid; }
};

// The optional manifest/executable arguments let disc-free tests exercise the same checks.
// Player entry points always use the supported PAL DW5 manifest and executable below.
OriginalReadiness checkOriginal(const std::filesystem::path &data,
                                const DiscSpec &disc = kSupportedDiscs[1], const char *executable = "SLES_533.39");
// Call only after verified extraction, or the original Load Original has succeeded.
// Rechecks the installed marker, sizes and executable hash before publishing the record atomically.
bool rememberOriginal(const std::filesystem::path &data, std::string &error,
                      const DiscSpec &disc = kSupportedDiscs[1], const char *executable = "SLES_533.39");
// Only DW5's installed files/marker and this record; never XL, cards or settings.
bool removeOriginal(const std::filesystem::path &data, std::string &error);
} // namespace dw5xl::setup
