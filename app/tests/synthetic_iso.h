#pragma once
// A tiny ISO 9660 image built in memory for the setup wizard tests (issue 011): no game data.
// Sectors: 16 PVD, 17 terminator, 18 root dir, 19 SUB dir, 20-21 A.BIN (3000 bytes), 22 SUB/B.TXT ("hello").
#include "disc_manifest.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace dw5xl::setup::test {

inline constexpr uint32_t kSector = 2048;

inline void put32(std::vector<uint8_t> &img, size_t at, uint32_t v)
{
    for (int i = 0; i < 4; ++i) // both-endian field: little-endian half, then big-endian half
    {
        img[at + i] = uint8_t(v >> (8 * i));
        img[at + 4 + i] = uint8_t(v >> (8 * (3 - i)));
    }
}

// Appends one directory record at `at`; returns its length.
inline size_t record(std::vector<uint8_t> &img, size_t at, const std::string &name, uint32_t lba, uint32_t size,
                     bool dir)
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

inline std::vector<uint8_t> fileA()
{
    std::vector<uint8_t> a(3000);
    for (size_t i = 0; i < a.size(); ++i)
        a[i] = uint8_t(i * 7);
    return a;
}

inline std::vector<uint8_t> buildImage()
{
    constexpr uint32_t S = kSector;
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

inline void write(const std::filesystem::path &path, const std::vector<uint8_t> &bytes)
{
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

inline std::vector<uint8_t> slurp(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

// The two files of buildImage() with their sizes and SHA-256.
inline constexpr DiscFile kTestFiles[] = {
    {"A.BIN", 3000u, "7291514d2492fd7ff49e10ba7df95d19d31d199b89d74bcb62cebdee1bc1a498"},
    {"SUB/B.TXT", 5u, "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"},
};

} // namespace dw5xl::setup::test
