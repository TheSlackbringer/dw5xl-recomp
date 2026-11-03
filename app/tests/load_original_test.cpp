// Synthetic files only: independently known SHA-256 of "abc".
#include "load_original.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace dw5xl::setup;
namespace fs = std::filesystem;
namespace {
constexpr DiscFile files[] = {
    {"SLES_533.39", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
    {"LINKDATA.IDX", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}
};
const DiscSpec fixture{"synthetic original", "dw5", files};
void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
void write(const fs::path &path, const std::string &bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary); out << bytes;
    require(bool(out), "write fixture");
}
}
int main() {
    const auto data = fs::temp_directory_path() / ("dw5xl_load_original_test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        require(fs::create_directory(data), "create unique fixture directory");
        std::string error;
        const auto check = [&] { return checkOriginal(data, fixture, "SLES_533.39"); };
        require(!check().allowed() && check().installed == InstallState::Missing && check().check == OriginalCheck::skipped,
                "nothing installed: locked, no hash check");
        write(data / "game/dw5/SLES_533.39", "abc"); write(data / "game/dw5/LINKDATA.IDX", "abc");
        require(!check().allowed() && check().installed == InstallState::Missing, "files without the marker: locked");
        // Any verified extraction leaves this marker, also one made before 051 (v3.1 criterion 11).
        require(writeInstallMarker(data, fixture, error), "verified fixture install marker");
        require(check().allowed() && check().check == OriginalCheck::valid, "verified install unlocks");
        write(data / "game/dw5/SLES_533.39", "abd");
        require(!check().allowed() && check().check == OriginalCheck::unsupported, "same-size executable corruption rejected");
        write(data / "game/dw5/SLES_533.39", "abc");
        fs::remove(data / "game/dw5/LINKDATA.IDX");
        require(!check().allowed() && check().installed == InstallState::Incomplete && check().check == OriginalCheck::skipped,
                "missing asset rejected");
        write(data / "game/dw5/LINKDATA.IDX", "ab");
        require(!check().allowed(), "wrong-size asset rejected");
        write(data / "game/dw5/LINKDATA.IDX", "abc");
        require(check().allowed(), "repaired install unlocks again");
        require(!checkOriginal(data, fixture, "MISSING.ELF").allowed(), "executable outside the manifest rejected");
        fs::remove(data / "game/dw5.verified");
        require(!check().allowed() && check().installed == InstallState::Missing, "marker removed: locked");
        fs::remove_all(data);
        std::cout << "Load Original install checks passed\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
