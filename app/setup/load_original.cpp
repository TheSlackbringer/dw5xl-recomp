#include "load_original.h"
#include "setup_cache.h"

#include <algorithm>
#include <chrono>
#include <string_view>

namespace dw5xl::setup {
namespace fs = std::filesystem;

OriginalReadiness checkOriginal(const fs::path &data, const DiscSpec &disc, const char *executable)
{
    OriginalReadiness result;
    const auto start = std::chrono::steady_clock::now();
    try {
        result.installed = installState(data, disc);
        if (result.installed == InstallState::Installed) {
            const auto file = std::find_if(disc.files.begin(), disc.files.end(),
                                           [&](const DiscFile &f) { return std::string_view(f.path) == executable; });
            const bool matches = file != disc.files.end() &&
                                 sha256File(discFolder(data, disc) / executable) == file->sha256;
            result.check = matches ? OriginalCheck::valid : OriginalCheck::unsupported;
        }
    } catch (const std::exception &) {
        result.check = OriginalCheck::unsupported;
    }
    result.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return result;
}
} // namespace dw5xl::setup
