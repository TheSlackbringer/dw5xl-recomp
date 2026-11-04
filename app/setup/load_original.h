#pragma once
#include "install_state.h"

namespace dw5xl::setup {
enum class OriginalCheck { skipped, valid, unsupported };
struct OriginalReadiness {
    InstallState installed = InstallState::Missing;
    OriginalCheck check = OriginalCheck::skipped;
    double milliseconds = 0;
    bool allowed() const { return installed == InstallState::Installed && check == OriginalCheck::valid; }
};

// Every way DW5 reaches the port (wizard, Insert-disc, F10, --add-disc) is the same verified extraction,
// which leaves the install marker. Marker, file sizes and the executable's hash earn the unlock.
// The optional manifest/executable arguments let disc-free tests exercise the same checks.
// Player entry points always use the supported PAL DW5 manifest and executable below.
OriginalReadiness checkOriginal(const std::filesystem::path &data,
                                const DiscSpec &disc = kSupportedDiscs[1], const char *executable = "SLES_533.39");
} // namespace dw5xl::setup
