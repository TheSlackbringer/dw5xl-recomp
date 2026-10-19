#pragma once
// Per-pair VU1 timing for the lifter (issue 011): a line-by-line port of tools/vu1_timing.py, itself a
// transcription of VU1Interpreter::decodeUpperUsage / decodeLowerUsage / makeLiftTiming. The lifted code embeds the
// result and checks it against the interpreter at run time, so a divergence costs speed, never correctness.
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dw5xl::vu1lift {

constexpr uint32_t FLAG_FDIV = 1, FLAG_EFU = 2, FLAG_WAITQ = 4, FLAG_WAITP = 8, FLAG_XGKICK = 16,
                   FLAG_BRANCH_DELAY = 32;
constexpr int READY_VI_BASE = 128, READY_ACC_BASE = 144;

enum Pipe { PIPE_NONE, PIPE_FMAC, PIPE_LSU, PIPE_FDIV, PIPE_EFU, PIPE_IALU, PIPE_BRANCH, PIPE_XGKICK };

struct Usage
{
    std::vector<std::pair<uint32_t, uint32_t>> vfRead; // (reg, lanes), at most 2
    uint32_t vfWrite[2] = {0, 0};                      // reg, lanes
    uint32_t viRead = 0, viWrite = 0, accRead = 0, accWrite = 0;
    uint32_t latency = 0, vfLatency = 0, viLatency = 0;
    Pipe pipeline = PIPE_NONE;
    bool waitQ = false, waitP = false, delaysBranch = false;
};

struct PairTiming
{
    std::array<uint32_t, 4> vfReg{}, vfLanes{};
    uint32_t viRead = 0, viWrite = 0, accRead = 0, accWrite = 0;
    std::array<uint32_t, 2> wReg{}, wLanes{}, wLat{};
    uint32_t viLat = 0, maxLat = 0, firstVi = 0, flags = 0;
    std::vector<int> reads;                       // ready-table slots read, sorted, unique
    std::vector<std::pair<int, uint32_t>> writes; // (slot, latency) in the interpreter's order
};

// Throw std::runtime_error on reserved instructions, like the Python ValueError.
Usage upperUsage(uint32_t upper);
Usage lowerUsage(uint32_t lower);
PairTiming pairTiming(uint32_t lower, uint32_t upper);
std::string timingInitializer(const PairTiming &t);

} // namespace dw5xl::vu1lift
