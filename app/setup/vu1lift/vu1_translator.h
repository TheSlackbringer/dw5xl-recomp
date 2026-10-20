#pragma once
// VU1 lifter (issue 011): translates the VU1 microprograms in the player's own PAL executable into C++, a port of
// tools/vu1_translator.py + disc_vu_inputs.py + native_call_analysis.py (disc mode). The output must be
// byte-identical to the Python tool's (checked by the vu1lift test against tools/vu1_translator.py).
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace dw5xl::vu1lift {

// Translates one VU1 program segment starting at entryPc in a 16 KiB microcode image (translate_segment).
std::string translateSegment(const std::vector<uint8_t> &code, uint32_t entryPc, const std::string &name,
                             bool staticTiming);

// Pairs reachable from entryPc (find_reachable, including the BAL/JR proof when the code has calls).
std::vector<uint32_t> findReachable(const std::vector<uint8_t> &code, uint32_t entryPc);

// The whole lifted file from the executable at `elf` (OVL_00.BIN is read from the same folder), exactly what
// `python3 tools/vu1_translator.py` writes. `log` gets the "Translating ..." progress lines.
// staticTiming(name) mirrors DW5XL_VU1_STATIC (default: all segments).
bool generateLiftedVu1(const std::filesystem::path &elf, std::string &text, std::string &error,
                       const std::function<void(const std::string &)> &log = {},
                       const std::function<bool(const std::string &)> &staticTiming = {});

// The VU0 microprograms the game starts with VCALLMS (issue 025), exactly what `python3 tools/vu0_translator.py`
// writes: same translator with the VU0 target (4 KB micro memory), inputs from VU0_RANGES.
bool generateLiftedVu0(const std::filesystem::path &elf, std::string &text, std::string &error,
                       const std::function<void(const std::string &)> &log = {},
                       const std::function<bool(const std::string &)> &staticTiming = {});

} // namespace dw5xl::vu1lift
