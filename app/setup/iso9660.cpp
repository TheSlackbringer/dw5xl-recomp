#include "iso9660.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace dw5xl::setup {
namespace {
uint32_t le32(const uint8_t *p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

constexpr uint32_t kPrimaryVolumeDescriptorLba = 16;
constexpr int kMaxDirectoryDepth = 16; // guards against corrupt images that loop
} // namespace

bool IsoImage::readSectors(uint32_t lba, uint32_t count, std::vector<uint8_t> &out, std::string &error)
{
    const uint64_t offset = uint64_t(lba) * kSectorSize;
    const uint64_t bytes = uint64_t(count) * kSectorSize;
    if (offset + bytes > m_imageSize)
    {
        error = "image is truncated (sector " + std::to_string(lba) + " is past the end)";
        return false;
    }
    out.resize(static_cast<size_t>(bytes));
    m_stream.clear();
    m_stream.seekg(static_cast<std::streamoff>(offset));
    m_stream.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(bytes));
    if (!m_stream)
    {
        error = "read error at sector " + std::to_string(lba);
        return false;
    }
    return true;
}

bool IsoImage::open(const std::filesystem::path &image, std::string &error)
{
    m_files.clear();
    m_stream.close();
    m_stream.open(image, std::ios::binary);
    if (!m_stream)
    {
        error = "cannot open " + image.string();
        return false;
    }
    std::error_code ec;
    m_imageSize = std::filesystem::file_size(image, ec);
    if (ec)
    {
        error = "cannot read the size of " + image.string();
        return false;
    }
    std::vector<uint8_t> pvd;
    if (!readSectors(kPrimaryVolumeDescriptorLba, 1, pvd, error))
    {
        error = "not a disc image (" + error + ")";
        return false;
    }
    if (pvd[0] != 1 || std::memcmp(&pvd[1], "CD001", 5) != 0)
    {
        error = "not an ISO 9660 disc image (no primary volume descriptor)";
        return false;
    }
    const uint8_t *root = &pvd[156]; // root directory record
    if (!walk(le32(root + 2), le32(root + 10), "", 0, error))
        return false;
    std::sort(m_files.begin(), m_files.end(), [](const IsoFile &a, const IsoFile &b) { return a.path < b.path; });
    return true;
}

bool IsoImage::walk(uint32_t lba, uint32_t size, const std::string &prefix, int depth, std::string &error)
{
    if (depth > kMaxDirectoryDepth)
    {
        error = "directory tree too deep (corrupt image?)";
        return false;
    }
    std::vector<uint8_t> dir;
    if (!readSectors(lba, (size + kSectorSize - 1) / kSectorSize, dir, error))
        return false;
    size_t pos = 0;
    while (pos < size)
    {
        const uint8_t length = dir[pos];
        if (length == 0) // records never cross a sector boundary: continue in the next sector
        {
            pos = (pos / kSectorSize + 1) * kSectorSize;
            continue;
        }
        if (length < 34 || pos + length > dir.size())
        {
            error = "corrupt directory record";
            return false;
        }
        const uint8_t *rec = &dir[pos];
        const uint8_t nameLength = rec[32];
        if (33u + nameLength > length)
        {
            error = "corrupt directory record name";
            return false;
        }
        std::string name(reinterpret_cast<const char *>(rec + 33), nameLength);
        pos += length;
        if (name == std::string(1, '\0') || name == std::string(1, '\1')) // "." and ".."
            continue;
        if (const auto semicolon = name.find(';'); semicolon != std::string::npos)
            name.erase(semicolon);
        if (!name.empty() && name.back() == '.') // "NAME." for files without extension
            name.pop_back();
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::toupper(c)); });
        const std::string path = prefix.empty() ? name : prefix + "/" + name;
        if (rec[25] & 0x02) // directory
        {
            if (!walk(le32(rec + 2), le32(rec + 10), path, depth + 1, error))
                return false;
        }
        else
        {
            m_files.push_back({path, le32(rec + 2), le32(rec + 10)});
        }
    }
    return true;
}

const IsoFile *IsoImage::find(const std::string &path) const
{
    for (const auto &file : m_files)
        if (file.path == path)
            return &file;
    return nullptr;
}

bool IsoImage::read(const IsoFile &file, const std::function<bool(const uint8_t *, size_t)> &sink, std::string &error)
{
    constexpr uint32_t kChunkSectors = 512; // 1 MiB
    uint64_t remaining = file.size;
    uint32_t lba = file.lba;
    std::vector<uint8_t> chunk;
    while (remaining > 0)
    {
        const uint32_t sectors = static_cast<uint32_t>(
            std::min<uint64_t>(kChunkSectors, (remaining + kSectorSize - 1) / kSectorSize));
        if (!readSectors(lba, sectors, chunk, error))
            return false;
        const size_t bytes = static_cast<size_t>(std::min<uint64_t>(remaining, chunk.size()));
        if (!sink(chunk.data(), bytes))
        {
            error = "cancelled";
            return false;
        }
        remaining -= bytes;
        lba += sectors;
    }
    return true;
}

} // namespace dw5xl::setup
