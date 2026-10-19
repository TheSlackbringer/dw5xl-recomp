#include "disc_setup.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <vector>

extern "C" {
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}

namespace dw5xl::setup {
namespace {
// Incremental SHA-256 through FFmpeg's libavutil (already a dependency; the game's own disc check uses it too).
class Sha256
{
public:
    Sha256() : m_sha(av_sha_alloc()) { m_ok = m_sha && av_sha_init(m_sha, 256) >= 0; }
    ~Sha256() { av_free(m_sha); }
    Sha256(const Sha256 &) = delete;
    Sha256 &operator=(const Sha256 &) = delete;
    bool ok() const { return m_ok; }
    void update(const uint8_t *data, size_t size)
    {
        // av_sha_update takes an unsigned int length on older FFmpeg; feed large buffers in pieces.
        constexpr size_t kPiece = 1u << 30;
        while (size > 0)
        {
            const size_t n = std::min(size, kPiece);
            av_sha_update(m_sha, data, static_cast<unsigned>(n));
            data += n;
            size -= n;
        }
    }
    std::string hex()
    {
        std::array<uint8_t, 32> digest{};
        av_sha_final(m_sha, digest.data());
        std::string out;
        char byte[3];
        for (uint8_t b : digest)
        {
            std::snprintf(byte, sizeof(byte), "%02x", b);
            out += byte;
        }
        return out;
    }

private:
    AVSHA *m_sha;
    bool m_ok = false;
};
} // namespace

const DiscSpec *identifyDisc(const IsoImage &iso, std::span<const DiscSpec> candidates, std::string &error)
{
    std::string reasons;
    for (const auto &disc : candidates)
    {
        const DiscFile *bad = nullptr;
        for (const auto &file : disc.files)
        {
            const IsoFile *found = iso.find(file.path);
            if (!found || found->size != file.size)
            {
                bad = &file;
                break;
            }
        }
        if (!bad)
            return &disc;
        const IsoFile *found = iso.find(bad->path);
        reasons += std::string("\n- not ") + disc.name + ": " + bad->path +
                   (found ? " has another size" : " is missing");
    }
    error = "this disc image is not a supported disc (wrong game, region or a damaged image):" + reasons;
    return nullptr;
}

std::string sha256File(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary); // fs::path: wide (Unicode) paths work on Windows too
    if (!file)
        return {};
    Sha256 sha;
    std::vector<uint8_t> buffer(1u << 20);
    while (file)
    {
        file.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        if (file.gcount() > 0)
            sha.update(buffer.data(), static_cast<size_t>(file.gcount()));
    }
    return file.eof() && sha.ok() ? sha.hex() : std::string();
}

bool extractDisc(IsoImage &iso, const DiscSpec &disc, const std::filesystem::path &destination,
                 const std::function<bool(const ExtractProgress &)> &progress, std::string &error)
{
    namespace fs = std::filesystem;
    ExtractProgress state;
    for (const auto &file : disc.files)
        state.totalBytes += file.size;
    std::error_code ec;
    for (const auto &file : disc.files)
    {
        const IsoFile *source = iso.find(file.path);
        if (!source || source->size != file.size)
        {
            error = std::string(file.path) + " is missing from the disc image";
            return false;
        }
        const fs::path target = destination / fs::path(file.path);
        state.file = file.path;
        if (fs::exists(target, ec) && fs::file_size(target, ec) == file.size && sha256File(target) == file.sha256)
        {
            state.doneBytes += file.size; // already extracted and correct: keep it
            if (progress && !progress(state))
            {
                error = "cancelled";
                return false;
            }
            continue;
        }
        fs::create_directories(target.parent_path(), ec);
        const fs::path partial = target.string() + ".part";
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            error = "cannot write " + partial.string();
            return false;
        }
        Sha256 sha;
        bool writeOk = sha.ok();
        const bool readOk = writeOk && iso.read(*source, [&](const uint8_t *data, size_t size) {
            sha.update(data, size);
            if (!out.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size)))
            {
                writeOk = false;
                return false;
            }
            state.doneBytes += size;
            return !progress || progress(state);
        }, error);
        out.close();
        writeOk = writeOk && !out.fail();
        if (!readOk || !writeOk)
        {
            fs::remove(partial, ec);
            if (!writeOk)
                error = "cannot write " + partial.string() + " (disk full?)";
            return false;
        }
        if (sha.hex() != file.sha256)
        {
            fs::remove(partial, ec);
            error = std::string(file.path) + " has the wrong checksum: the disc image is damaged or modified";
            return false;
        }
        fs::rename(partial, target, ec);
        if (ec)
        {
            error = "cannot rename " + partial.string() + ": " + ec.message();
            return false;
        }
    }
    return true;
}

} // namespace dw5xl::setup
