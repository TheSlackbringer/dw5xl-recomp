#pragma once
// Minimal ISO 9660 reader for the setup wizard (issue 011): lists the files of a PS2 DVD image and streams their
// contents. No mounting and no external tools; works the same on Windows and Linux.
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace dw5xl::setup {

struct IsoFile
{
    std::string path; // "SLES_538.60", "MODULES/LIBSD.IRX" (upper case, ";1" version removed)
    uint32_t lba = 0;  // first 2048-byte sector
    uint64_t size = 0; // bytes
};

class IsoImage
{
public:
    static constexpr uint32_t kSectorSize = 2048;

    // Reads the primary volume descriptor and walks every directory. false + error on a bad image.
    bool open(const std::filesystem::path &image, std::string &error);
    // Releases the image file (Windows cannot delete or move a file that is still open).
    void close()
    {
        m_stream.close();
        m_files.clear();
    }
    const std::vector<IsoFile> &files() const { return m_files; }
    const IsoFile *find(const std::string &path) const;

    // Streams a file's bytes to `sink` in chunks; sink returns false to cancel.
    bool read(const IsoFile &file, const std::function<bool(const uint8_t *, size_t)> &sink, std::string &error);

private:
    bool readSectors(uint32_t lba, uint32_t count, std::vector<uint8_t> &out, std::string &error);
    bool walk(uint32_t lba, uint32_t size, const std::string &prefix, int depth, std::string &error);

    std::ifstream m_stream;
    uint64_t m_imageSize = 0;
    std::vector<IsoFile> m_files;
};

} // namespace dw5xl::setup
