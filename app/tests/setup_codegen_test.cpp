// Setup wizard step "generate the game's C++" (issue 011): config rewriting, running the recompiler and syncing
// its output. The test runs itself as a fake recompiler: with DW5XL_FAKE_RECOMP set (inherited by the children it
// starts) it only translates the one config it is given and never runs the test again. No game data is used.
#include "codegen.h"
#include "run_process.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <regex>
#include <stdexcept>

using namespace dw5xl::setup;
namespace fs = std::filesystem;

namespace {
std::string lastError; // the driver's error message, shown when a check fails

void require(bool ok, const char *what)
{
    if (!ok)
        throw std::runtime_error(std::string(what) + (lastError.empty() ? "" : " (" + lastError + ")"));
}

std::string slurp(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

void spit(const fs::path &path, const std::string &text)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

std::string tomlValue(const std::string &config, const char *key)
{
    std::smatch m;
    const std::regex re(std::string("(^|\\n)") + key + " = \"([^\"]*)\"");
    return std::regex_search(config, m, re) ? m[2].str() : std::string();
}

fs::path fromUtf8(const std::string &text)
{
    return fs::path(std::u8string(text.begin(), text.end()));
}

// Fake recompiler: writes func.cpp (the input's text) and a fixed header into the configured output folder.
int fakeRecomp(const fs::path &config)
{
    const std::string text = slurp(config);
    const fs::path input = fromUtf8(tomlValue(text, "input"));
    const fs::path output = fromUtf8(tomlValue(text, "output"));
    if (!fs::exists(input))
        return 3;
    spit(output / "func.cpp", "// from " + slurp(input));
    spit(output / "ps2_recompiled_functions.h", "#pragma once\n");
    std::cout << "fake recompiler wrote " << output.string() << "\n";
    return 0;
}

const char *kConfig = "[general]\ninput = \"game/x\"\nghidra_output = \"recomp/y.csv\"\noutput = \"recomp/out/\"\n"
                      "single_file_output = false\n";
} // namespace

int main(int argc, char **argv)
{
    // Hard guard: a child started as the "recompiler" must never run the test (it would start itself again).
    if (std::getenv("DW5XL_FAKE_RECOMP"))
        return argc == 2 ? fakeRecomp(fromUtf8(argv[1])) : 2;
#ifdef _WIN32
    _putenv_s("DW5XL_FAKE_RECOMP", "1");
#else
    setenv("DW5XL_FAKE_RECOMP", "1", 1);
#endif
    try
    {
        const fs::path self = fs::absolute(argv[0]);
        const fs::path root = fs::temp_directory_path() / "dw5xl setup codegen Jürgen";
        fs::remove_all(root);

        // 1. The config rewrite keeps other lines and quotes paths with spaces and non-ASCII characters.
        const std::string rewritten =
            rewriteRecompConfig(kConfig, root / "game" / "a b.BIN", root / "f.csv", root / "out");
        require(rewritten.find("single_file_output = false") != std::string::npos, "other lines kept");
        require(fromUtf8(tomlValue(rewritten, "input")) == (root / "game" / "a b.BIN").generic_u8string(),
                "input path rewritten");
        require(tomlValue(rewritten, "output").back() == '/', "output folder ends with /");

        // 2. Starting a missing program fails cleanly.
        require(!runProcess(root / "no-such-program", {}, {}, root / "x.log").started, "missing program not started");

        // 3. Full run over the four profiles with the fake recompiler.
        for (const auto &profile : kCodegenProfiles)
        {
            spit(root / profile.config, kConfig);
            spit(root / profile.input, profile.name);
            spit(root / profile.ghidraCsv, "");
        }
        spit(root / kCodegenProfiles[1].output / "stale.cpp", "old");
        std::string &error = lastError;
        int calls = 0;
        require(generateCode(root, self, root / "logs", [&](const char *, int, int count) {
            ++calls;
            return count == 4;
        }, error), "generate all profiles");
        require(calls == 4, "progress for each profile");
        require(slurp(root / kCodegenProfiles[0].output / "func.cpp") == "// from main", "main output");
        require(slurp(root / kCodegenProfiles[3].output / "func.cpp") == "// from ovl02", "ovl02 output");
        require(!fs::exists(root / kCodegenProfiles[1].output / "stale.cpp"), "stale file removed");
        require(slurp(root / "logs" / "main.log").find("fake recompiler wrote") != std::string::npos, "log written");

        // 4. Re-run: unchanged files keep their time stamp, changed ones are rewritten.
        const fs::path header = root / kCodegenProfiles[0].output / "ps2_recompiled_functions.h";
        const auto old = fs::file_time_type::clock::now() - std::chrono::hours(24);
        fs::last_write_time(header, old);
        spit(root / kCodegenProfiles[0].input, "main v2");
        require(generateCode(root, self, root / "logs", nullptr, error), "re-generate");
        require(fs::last_write_time(header) == old, "unchanged file not rewritten");
        require(slurp(root / kCodegenProfiles[0].output / "func.cpp") == "// from main v2", "changed file updated");

        // 5. A failing recompiler stops with the profile and log in the message.
        fs::remove(root / kCodegenProfiles[2].input);
        require(!generateCode(root, self, root / "logs", nullptr, error) &&
                    error.find("failed on ovl01") != std::string::npos, "failure reported");

        fs::remove_all(root);
        std::cout << "setup codegen tests passed\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
