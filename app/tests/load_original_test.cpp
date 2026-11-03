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
std::string read(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
}
int main() {
    const auto data = fs::temp_directory_path() / ("dw5xl_load_original_test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        require(fs::create_directory(data), "create unique fixture directory");
        std::string error;
        const auto check = [&] { return checkOriginal(data, fixture, "SLES_533.39"); };
        const auto remember = [&] { return rememberOriginal(data, error, fixture, "SLES_533.39"); };
        require(!check().allowed() && check().check == OriginalCheck::skipped, "nothing installed: locked");
        require(!remember(), "cannot remember missing install");
        write(data / "game/dw5/SLES_533.39", "abc"); write(data / "game/dw5/LINKDATA.IDX", "abc");
        require(writeInstallMarker(data, fixture, error), "verified fixture install marker");
        require(!check().allowed(), "old installed marker alone must not earn record");
        require(remember() && check().allowed(), "verified installed original qualifies");
        const auto record = read(data / "load_original");
        require(record == "dw5xl load original 1\nba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n",
                "record holds independent expected executable hash");
        write(data / "game/dw5/SLES_533.39", "abd");
        require(!check().allowed() && check().check == OriginalCheck::unsupported, "same-size executable corruption rejected");
        require(!remember() && read(data / "load_original") == record, "failed verification preserves prior record");
        write(data / "game/dw5/SLES_533.39", "abc");
        fs::remove(data / "game/dw5/LINKDATA.IDX");
        require(!check().allowed() && check().installed == InstallState::Incomplete, "missing asset rejected");
        write(data / "game/dw5/LINKDATA.IDX", "ab");
        require(!check().allowed(), "wrong-size asset rejected");
        write(data / "game/dw5/LINKDATA.IDX", "abc");
        for (const auto &bad : {std::string(""), std::string("unrecognized record"), record + "extra", record.substr(0, record.size()-1)}) {
            write(data / "load_original", bad);
            require(!check().allowed() && check().check == OriginalCheck::unsupported, "malformed record rejected");
        }
        fs::remove(data / "load_original"); fs::create_directory(data / "load_original");
        write(data / "load_original/sentinel", "keep");
        require(!remember() && read(data / "load_original/sentinel") == "keep", "failed atomic record replacement preserves existing directory");
        fs::remove_all(data / "load_original"); require(remember(), "record can be repaired");
        fs::remove(data / "game/dw5.verified");
        require(!check().allowed() && check().installed == InstallState::Missing, "record without marker rejected");
        require(writeInstallMarker(data, fixture, error), "restore marker");
        write(data / "game/dw5xl/sentinel", "XL");
        write(data / "game/dw5xl/mc0/save", "card"); write(data / "settings.ini", "settings");
        require(removeOriginal(data, error), "remove original");
        require(!fs::exists(data / "load_original") && !fs::exists(data / "game/dw5.verified") &&
                !fs::exists(data / "game/dw5") && !check().allowed(), "remove invalidates marker, record and files");
        require(read(data / "game/dw5xl/sentinel") == "XL" && read(data / "game/dw5xl/mc0/save") == "card" &&
                read(data / "settings.ini") == "settings", "remove preserves XL, saves and settings");
        require(removeOriginal(data, error), "remove is idempotent");
        // A player may keep custom cards/settings beside extracted assets.
        // Removing a disc must delete only its manifest files, never that data.
        write(data / "game/dw5/SLES_533.39", "abc"); write(data / "game/dw5/LINKDATA.IDX", "abc");
        require(writeInstallMarker(data, fixture, error) && remember(), "restore original fixture");
        write(data / "game/dw5/mc0/save", "custom card");
        write(data / "game/dw5/settings.ini", "custom settings");
        write(data / "game/dw5/keep.txt", "player file");
        write(data / "game/dw5/custom/SLES_533.39", "not a disc asset");
        require(removeOriginal(data, error), "remove disc beside custom data");
        require(read(data / "game/dw5/mc0/save") == "custom card" &&
                read(data / "game/dw5/settings.ini") == "custom settings" &&
                read(data / "game/dw5/keep.txt") == "player file" &&
                read(data / "game/dw5/custom/SLES_533.39") == "not a disc asset", "remove preserves custom files inside DW5 folder");
        require(!fs::exists(data / "game/dw5/SLES_533.39") && !fs::exists(data / "game/dw5/LINKDATA.IDX") &&
                !fs::exists(data / "load_original") && !fs::exists(data / "game/dw5.verified"),
                "only installed disc files and entitlement removed");
        write(data / "game/dw5/sles_533.39", "case variant");
        require(removeOriginal(data, error), "remove case variants according to filesystem semantics");
#ifdef _WIN32
        require(!fs::exists(data / "game/dw5/sles_533.39"), "Windows manifest filename alias removed");
#else
        require(read(data / "game/dw5/sles_533.39") == "case variant", "case-sensitive unknown filename kept");
        // Never follow a linked installed root or a known module directory into other data.
        fs::remove_all(data / "game/dw5");
        write(data / "external/SLES_533.39", "external data");
        fs::create_directory_symlink(data / "external", data / "game/dw5");
        require(removeOriginal(data, error) && !fs::exists(data / "game/dw5") &&
                read(data / "external/SLES_533.39") == "external data", "linked root target kept");
        fs::create_directory(data / "game/dw5");
        fs::create_directory_symlink(data / "external", data / "game/dw5/MODULES");
        require(removeOriginal(data, error) && !fs::exists(data / "game/dw5") &&
                read(data / "external/SLES_533.39") == "external data", "linked module target kept");
#endif
        fs::remove_all(data);
        std::cout << "Load Original record/install checks passed\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
