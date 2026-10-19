#pragma once
// SHA-256 for the setup wizard (issue 011), through FFmpeg's libavutil (already a dependency; the game's own disc
// check uses it too).
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

extern "C" {
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}

namespace dw5xl::setup {

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

inline std::string sha256Hex(const uint8_t *data, size_t size)
{
    Sha256 sha;
    sha.update(data, size);
    return sha.hex();
}

} // namespace dw5xl::setup
