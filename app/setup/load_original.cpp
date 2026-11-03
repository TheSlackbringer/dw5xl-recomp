#include "load_original.h"
#include "setup_cache.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <set>
#include <string_view>
#include <vector>

namespace dw5xl::setup {
namespace fs = std::filesystem;
namespace {
std::string recordBytes(const DiscSpec &disc, const char *executable)
{
    const auto file = std::find_if(disc.files.begin(), disc.files.end(),
                                   [&](const DiscFile &f) { return std::string_view(f.path) == executable; });
    if (file == disc.files.end()) return {};
    return "dw5xl load original 1\n" + std::string(file->sha256) + "\n";
}
bool executableMatches(const fs::path &data, const DiscSpec &disc, const char *executable)
{
    const auto file = std::find_if(disc.files.begin(), disc.files.end(),
                                   [&](const DiscFile &f) { return std::string_view(f.path) == executable; });
    return file != disc.files.end() && sha256File(discFolder(data, disc) / executable) == file->sha256;
}
std::string pathKey(const fs::path &path)
{
    const auto bytes = path.generic_u8string();
    std::string key(reinterpret_cast<const char *>(bytes.data()), bytes.size());
#ifdef _WIN32
    // Manifest paths are ASCII; Windows resolves their case-insensitive aliases too.
    for (char &c : key) if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
#endif
    return key;
}
} // namespace

OriginalReadiness checkOriginal(const fs::path &data, const DiscSpec &disc, const char *executable)
{
    OriginalReadiness result;
    const auto start = std::chrono::steady_clock::now();
    try {
        std::error_code ec;
        result.record = fs::exists(data / "load_original", ec);
        result.installed = installState(data, disc);
        if (result.record) {
            result.check = OriginalCheck::unsupported;
            const std::string expected = recordBytes(disc, executable);
            // Bounded read, and exact format: malformed/old records never grant an unlock.
            std::ifstream in(data / "load_original", std::ios::binary);
            std::string bytes(expected.size() + 1, '\0');
            if (in) {
                in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                bytes.resize(static_cast<size_t>(in.gcount()));
                if (!expected.empty() && !in.bad() && bytes == expected) {
                    result.check = OriginalCheck::skipped;
                    if (result.installed == InstallState::Installed)
                        result.check = executableMatches(data, disc, executable) ? OriginalCheck::valid : OriginalCheck::unsupported;
                }
            }
        }
    } catch (const std::exception &) {
        result.check = OriginalCheck::unsupported;
    }
    result.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return result;
}

bool rememberOriginal(const fs::path &data, std::string &error, const DiscSpec &disc, const char *executable)
{
    try {
        const auto bytes = recordBytes(disc, executable);
        if (bytes.empty() || installState(data, disc) != InstallState::Installed || !executableMatches(data, disc, executable)) {
            error = "DW5 must be installed and its executable must match before Load Original can be remembered.";
            return false;
        }
        return atomicWrite(data / "load_original", bytes, error);
    } catch (const std::exception &) {
        error = "Could not remember Load Original.";
        return false;
    }
}

bool removeOriginal(const fs::path &data, std::string &error)
{
    if (data.empty()) { error = "No player data folder is open."; return false; }
    std::error_code ec;
    const auto failed = [&] { error = "Could not remove the installed DW5 files: " + ec.message(); return false; };
    try {
        // Invalidate first. Never recursively delete metadata paths or the disc folder:
        // players can have their own cards/settings or other files beside the disc assets.
        for (const auto &path : {data / "load_original", data / "game" / "dw5.verified"}) {
            fs::remove(path, ec);
            if (ec) return failed();
        }
        const auto root = discFolder(data, kSupportedDiscs[1]);
        const auto status = fs::symlink_status(root, ec);
        if (ec == std::errc::no_such_file_or_directory || (!ec && !fs::exists(status))) return true;
        if (ec) return failed();
        if (fs::is_symlink(status)) { fs::remove(root, ec); return ec ? failed() : true; }
        if (!fs::is_directory(status)) { error = "The DW5 folder is not a directory; its contents were kept."; return false; }

        std::set<std::string> files, directories;
        for (const auto &file : kSupportedDiscs[1].files) {
            const fs::path relative(file.path);
            files.insert(pathKey(relative));
            for (auto parent = relative.parent_path(); !parent.empty(); parent = parent.parent_path())
                directories.insert(pathKey(parent));
        }
        std::vector<fs::path> emptyCandidates{root};
        auto entry = fs::recursive_directory_iterator(root, fs::directory_options::none, ec);
        if (ec) return failed();
        const fs::recursive_directory_iterator end;
        while (entry != end) {
            const auto key = pathKey(entry->path().lexically_relative(root));
            const auto kind = entry->symlink_status(ec);
            if (ec) return failed();
            if (fs::is_directory(kind)) {
                if (directories.contains(key)) emptyCandidates.push_back(entry->path());
                else entry.disable_recursion_pending(); // no traversal of custom cards/settings
            } else if (files.contains(key) || (fs::is_symlink(kind) && directories.contains(key))) {
                fs::remove(entry->path(), ec); // remove a known link itself, never its target
                if (ec) return failed();
            }
            entry.increment(ec);
            if (ec) return failed();
        }
        std::sort(emptyCandidates.begin(), emptyCandidates.end(), std::greater<fs::path>());
        for (const auto &path : emptyCandidates) {
            const bool empty = fs::is_empty(path, ec);
            if (ec) return failed();
            if (empty) { fs::remove(path, ec); if (ec) return failed(); }
        }
        return true;
    } catch (const std::exception &) {
        error = "Could not remove all DW5 files. Saves, settings and unrecognized files were kept.";
        return false;
    }
}
} // namespace dw5xl::setup
