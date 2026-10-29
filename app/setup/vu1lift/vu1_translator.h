#pragma once
// VU1/VU0 translator (issues 011, 025): translates the VU microprograms in the player's own PAL executable into C++.
// The only translator since issue 025 Step 6 (it was a port of the former Python tools/vu1_translator.py).
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace dw5xl::vu1lift {
// Exact disc files referenced by the selected generation ranges (for semantic cache keys).
std::vector<std::filesystem::path> inputFiles(const std::filesystem::path &elf, bool vu0);
// Whole segment bodies are copied byte for byte; only the owner emits registration data.
std::map<std::string,std::string> partitionVu1Sources(const std::string &prologue,
    const std::vector<std::pair<std::string,std::string>> &segments,const std::vector<std::string> &ownerData,
    const std::string &registration,size_t maxUnitBytes=512*1024);

// Translates one VU1 program segment starting at entryPc in a 16 KiB microcode image (translate_segment).
std::string translateSegment(const std::vector<uint8_t> &code, uint32_t entryPc, const std::string &name,
                             bool staticTiming);

// Pairs reachable from entryPc (find_reachable, including the BAL/JR proof when the code has calls).
std::vector<uint32_t> findReachable(const std::vector<uint8_t> &code, uint32_t entryPc);

// The whole lifted VU1 file from the executable at `elf` (OVL_00.BIN is read from the same folder). `log` gets the
// "Translating ..." progress lines.
// staticTiming(name) mirrors DW5XL_VU1_STATIC (default: all segments).
bool generateLiftedVu1(const std::filesystem::path &elf, std::string &text, std::string &error,
                       const std::function<void(const std::string &)> &log = {},
                       const std::function<bool(const std::string &)> &staticTiming = {},
                       std::map<std::string,std::string> *sources=nullptr);

// The VU0 microprograms the game starts with VCALLMS (issue 025): the same translator with the VU0 target (4 KB
// micro memory), inputs from kVu0Ranges.
bool generateLiftedVu0(const std::filesystem::path &elf, std::string &text, std::string &error,
                       const std::function<void(const std::string &)> &log = {},
                       const std::function<bool(const std::string &)> &staticTiming = {});

// The 16 KiB VU1 code images the translated programs come from, rebuilt from the executable at `elf` (OVL_00.BIN from
// the same folder) exactly as the translator reads them (load_disc_bins: executable, disc-file and range SHA-256
// checked), keyed by provenance key. Used by the vu1_lift test (issue 036). false + `error` if a file is missing or
// does not match.
bool loadVu1Images(const std::filesystem::path &elf, std::map<std::string, std::vector<uint8_t>> &images,
                   std::string &error);

} // namespace dw5xl::vu1lift
