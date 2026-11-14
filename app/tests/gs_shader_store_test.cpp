// DW5XL-TEST issue 066: the file of the game's own compiled shader programs (runtime/gs/gs_shader_store.h). No
// graphics card and no game: the content is made up here.
//  - what was written is read back whole, and only for the shaders it was written for;
//  - no file, a file of something else, a damaged one, a cut one and an empty one are refused, each by its name;
//  - a write that cannot be made leaves the old file as it was, and a left-over part file does no harm.
#include "runtime/gs/gs_shader_store.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace ps2x::gs;

namespace
{
    int failures = 0;
    void check(bool ok, const std::string &what)
    {
        std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
        failures += ok ? 0 : 1;
    }

    std::vector<uint8_t> fileBytes(const std::filesystem::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    void writeBytes(const std::filesystem::path &path, const std::vector<uint8_t> &bytes)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
}

int main(int argc, char **argv)
{
    // A folder of this run's own under the build folder (given by CTest), with a name no earlier run had, removed
    // at the run's end. Nothing is removed before use: a file or folder just deleted can linger on Windows, and
    // nothing of its name can be made until it is gone.
    const std::filesystem::path all = std::filesystem::path(argc > 1 ? argv[1] : ".") / "gs_shader_store_test_files";
    std::error_code ignored;
    const std::filesystem::path folder =
        all / ("run-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(folder);
    struct Tidy
    {
        std::filesystem::path folder;
        ~Tidy()
        {
            std::error_code failed;
            std::filesystem::remove_all(folder, failed);
        }
    } tidy{folder};
    const std::string path = (folder / "shaders.bin").string();
    const uint64_t mark = 0x1122334455667788ull;

    std::vector<uint8_t> content(100000);
    for (size_t i = 0; i < content.size(); ++i)
        content[i] = static_cast<uint8_t>((i * 31u + (i >> 8)) & 0xFFu);

    std::vector<uint8_t> got;
    check(shader_store::read(path, mark, got) == shader_store::Read::NoFile && got.empty(), "no file: nothing is read, and it says so");
    check(shader_store::write(path, mark, content.data(), content.size()), "the store is written");
    check(!std::filesystem::exists(path + ".part"), "... and no part file is left beside it");
    check(shader_store::read(path, mark, got) == shader_store::Read::Taken && got == content, "read back: the same content, whole");

    got.assign(3, 7u);
    check(shader_store::read(path, mark + 1u, got) == shader_store::Read::OtherShaders && got.empty(),
          "written for other shaders: refused, nothing is handed out");

    const std::vector<uint8_t> whole = fileBytes(path);
    {
        std::vector<uint8_t> damaged = whole;
        damaged[damaged.size() / 2] ^= 0x40u;
        writeBytes(path, damaged);
        check(shader_store::read(path, mark, got) == shader_store::Read::Damaged && got.empty(), "one bit of the content changed: refused as damaged");
        std::vector<uint8_t> cut(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(whole.size() - 1000));
        writeBytes(path, cut);
        check(shader_store::read(path, mark, got) == shader_store::Read::Damaged && got.empty(), "the file cut short: refused as damaged");
        std::vector<uint8_t> longer = whole;
        longer.resize(whole.size() + 16, 0u);
        writeBytes(path, longer);
        check(shader_store::read(path, mark, got) == shader_store::Read::Damaged && got.empty(), "bytes added at its end: refused as damaged");
        writeBytes(path, {});
        check(shader_store::read(path, mark, got) == shader_store::Read::NotOurs && got.empty(), "an empty file: refused as not a store");
        writeBytes(path, std::vector<uint8_t>(5000, 0x55u));
        check(shader_store::read(path, mark, got) == shader_store::Read::NotOurs && got.empty(), "a file of something else: refused as not a store");
        std::vector<uint8_t> newer = whole;
        newer[8] = static_cast<uint8_t>(newer[8] + 1u); // the format's number, right after the eight letters
        writeBytes(path, newer);
        check(shader_store::read(path, mark, got) == shader_store::Read::NotOurs && got.empty(), "a store of another format number: refused");
    }

    // A second write replaces the first whole; a part file left by a run that died is no obstacle.
    writeBytes(path, whole);
    writeBytes(path + ".part", std::vector<uint8_t>(77, 1u));
    std::vector<uint8_t> second(content.begin(), content.begin() + 5000);
    check(shader_store::write(path, mark, second.data(), second.size()) && shader_store::read(path, mark, got) == shader_store::Read::Taken &&
              got == second && !std::filesystem::exists(path + ".part"),
          "written again over a left-over part file: the new content, whole, and no part file");

    // A write that cannot be made (its folder is not there) says so and changes nothing.
    const std::string nowhere = (folder / "not-there" / "shaders.bin").string();
    check(!shader_store::write(nowhere, mark, content.data(), content.size()) && !std::filesystem::exists(nowhere),
          "a write into a folder that is not there fails and makes nothing");
    check(shader_store::read(path, mark, got) == shader_store::Read::Taken && got == second, "... and the store written before is untouched");

    check(std::string(shader_store::said(shader_store::Read::Damaged)) != shader_store::said(shader_store::Read::OtherShaders),
          "each refusal has words of its own for the log");

    std::printf("%s (%d failed)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
