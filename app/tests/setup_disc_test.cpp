// Setup wizard core (issue 011): ISO 9660 reading, disc recognition and verified extraction on a small
// synthetic image built here (no game data).
#include "disc_setup.h"
#include "synthetic_iso.h"

#include <iostream>
#include <stdexcept>
#include <vector>

using namespace dw5xl::setup;
using namespace dw5xl::setup::test;
namespace fs = std::filesystem;

namespace {
void require(bool ok, const char *what)
{
    if (!ok)
        throw std::runtime_error(what);
}

constexpr uint32_t S = IsoImage::kSectorSize;
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
