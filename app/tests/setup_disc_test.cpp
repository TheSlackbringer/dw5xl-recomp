// Setup wizard core (issue 011): ISO 9660 reading, disc recognition and verified extraction on a small
// synthetic image built here (no game data).
#include "disc_setup.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace dw5xl::setup;
namespace fs = std::filesystem;

namespace {
void require(bool ok, const char *what)
{
    if (!ok)
        throw std::runtime_error(what);
}

constexpr uint32_t S = IsoImage::kSectorSize;

void put32(std::vector<uint8_t> &img, size_t at, uint32_t v)
{
    for (int i = 0; i < 4; ++i) // both-endian field: little-endian half, then big-endian half
    {
        img[at + i] = uint8_t(v >> (8 * i));
        img[at + 4 + i] = uint8_t(v >> (8 * (3 - i)));
    }
}

// Appends one directory record at `at`; returns its length.
size_t record(std::vector<uint8_t> &img, size_t at, const std::string &name, uint32_t lba, uint32_t size, bool dir)
{
    const size_t length = (33 + name.size() + 1) & ~size_t(1);
    img[at] = uint8_t(length);
    put32(img, at + 2, lba);
    put32(img, at + 10, size);
    img[at + 25] = dir ? 0x02 : 0x00;
    img[at + 32] = uint8_t(name.size());
    std::memcpy(&img[at + 33], name.data(), name.size());
    return length;
}

std::vector<uint8_t> fileA()
{
    std::vector<uint8_t> a(3000);
    for (size_t i = 0; i < a.size(); ++i)
        a[i] = uint8_t(i * 7);
    return a;
}

// Sectors: 16 PVD, 17 terminator, 18 root dir, 19 SUB dir, 20-21 A.BIN (3000 bytes), 22 SUB/B.TXT ("hello").
std::vector<uint8_t> buildImage()
{
    std::vector<uint8_t> img(23 * S, 0);
    img[16 * S] = 1;
    std::memcpy(&img[16 * S + 1], "CD001", 5);
    record(img, 16 * S + 156, std::string(1, '\0'), 18, S, true);
    img[17 * S] = 255;
    std::memcpy(&img[17 * S + 1], "CD001", 5);
    size_t at = 18 * S;
    at += record(img, at, std::string(1, '\0'), 18, S, true);
    at += record(img, at, std::string(1, '\1'), 18, S, true);
    at += record(img, at, "A.BIN;1", 20, 3000, false);
    record(img, at, "SUB", 19, S, true);
    at = 19 * S;
    at += record(img, at, std::string(1, '\0'), 19, S, true);
    at += record(img, at, std::string(1, '\1'), 18, S, true);
    record(img, at, "B.TXT;1", 22, 5, false);
    const auto a = fileA();
    std::memcpy(&img[20 * S], a.data(), a.size());
    std::memcpy(&img[22 * S], "hello", 5);
    return img;
}

void write(const fs::path &path, const std::vector<uint8_t> &bytes)
{
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

std::vector<uint8_t> slurp(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

constexpr DiscFile kTestFiles[] = {
    {"A.BIN", 3000u, "7291514d2492fd7ff49e10ba7df95d19d31d199b89d74bcb62cebdee1bc1a498"},
    {"SUB/B.TXT", 5u, "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"},
};
constexpr DiscFile kWrongSize[] = {{"A.BIN", 2999u, "00"}};
constexpr DiscFile kWrongHash[] = {
    {"A.BIN", 3000u, "7291514d2492fd7ff49e10ba7df95d19d31d199b89d74bcb62cebdee1bc1a499"}};
} // namespace

int main()
{
    try
    {
        const fs::path dir = fs::temp_directory_path() / "dw5xl_setup_disc_test";
        fs::remove_all(dir);
        fs::create_directories(dir);
        const fs::path image = dir / "test.iso";
        write(image, buildImage());

        IsoImage iso;
        std::string error;
        require(iso.open(image, error), "open the synthetic image");
        require(iso.files().size() == 2, "two files listed");
        require(iso.find("A.BIN") && iso.find("SUB/B.TXT"), "paths without ;1, subfolder joined with /");
        std::vector<uint8_t> b;
        require(iso.read(*iso.find("SUB/B.TXT"), [&](const uint8_t *p, size_t n) {
            b.insert(b.end(), p, p + n);
            return true;
        }, error), "read B.TXT");
        require(b == std::vector<uint8_t>{'h', 'e', 'l', 'l', 'o'}, "B.TXT content");

        const DiscSpec good[] = {{"wrong-size disc", "x", kWrongSize}, {"test disc", "test", kTestFiles}};
        require(identifyDisc(iso, good, error) == &good[1], "recognised as the test disc, not the wrong-size one");
        const DiscSpec onlyWrong[] = {{"wrong-size disc", "x", kWrongSize}};
        require(!identifyDisc(iso, onlyWrong, error) && error.find("another size") != std::string::npos,
                "wrong size is rejected with a reason");

        const fs::path out = dir / "out";
        uint64_t last = 0;
        require(extractDisc(iso, good[1], out, [&](const ExtractProgress &p) {
            last = p.doneBytes;
            return p.totalBytes == 3005;
        }, error), "extract the test disc");
        require(last == 3005, "progress reaches the total");
        require(slurp(out / "A.BIN") == fileA() && slurp(out / "SUB" / "B.TXT").size() == 5, "extracted contents");
        require(extractDisc(iso, good[1], out, nullptr, error), "re-run keeps correct files");

        const DiscSpec damaged{"damaged", "d", kWrongHash};
        const fs::path out2 = dir / "out2";
        require(!extractDisc(iso, damaged, out2, nullptr, error) &&
                    error.find("wrong checksum") != std::string::npos, "wrong checksum is refused");
        require(!fs::exists(out2 / "A.BIN") && !fs::exists(out2 / "A.BIN.part"), "no partial file left behind");

        auto truncated = buildImage();
        truncated.resize(21 * S);
        write(dir / "truncated.iso", truncated);
        IsoImage cut;
        require(cut.open(dir / "truncated.iso", error), "directories are still readable");
        require(!cut.read(*cut.find("SUB/B.TXT"), [](const uint8_t *, size_t) { return true; }, error),
                "reading past the end of a truncated image fails cleanly");

        iso.close();
        cut.close();
        fs::remove_all(dir);
        std::cout << "setup disc tests passed\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
