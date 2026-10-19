#pragma once
// Setup wizard core (issue 011): recognise a supported disc image and extract it with checksum verification.
#include "disc_manifest.h"
#include "iso9660.h"

#include <filesystem>
#include <functional>
#include <span>
#include <string>

namespace dw5xl::setup {

struct DiscSpec
{
    const char *name;
    const char *folder; // folder under game/ the runtime expects
    std::span<const DiscFile> files;
};

inline constexpr DiscSpec kSupportedDiscs[] = {
    {"Dynasty Warriors 5 Xtreme Legends (PAL)", "dw5xl", kDw5xlPal},
    {"Dynasty Warriors 5 (PAL, for Load Original)", "dw5", kDw5Pal},
};

// The supported disc this image is: every listed file present with the listed size. nullptr and a reason
// (first missing or wrong-sized file) otherwise.
const DiscSpec *identifyDisc(const IsoImage &iso, std::span<const DiscSpec> candidates, std::string &error);

struct ExtractProgress
{
    std::string file;
    uint64_t doneBytes = 0;
    uint64_t totalBytes = 0;
};

// Copies every file of `disc` into `destination`, checking each file's SHA-256 while it is written (a file
// already there with the right checksum is kept). A mismatch stops with an error and removes the partial file.
// `progress` is called per chunk; returning false cancels.
bool extractDisc(IsoImage &iso, const DiscSpec &disc, const std::filesystem::path &destination,
                 const std::function<bool(const ExtractProgress &)> &progress, std::string &error);

// SHA-256 of a file on disk as lower-case hex; empty on a read error.
std::string sha256File(const std::filesystem::path &path);

} // namespace dw5xl::setup
