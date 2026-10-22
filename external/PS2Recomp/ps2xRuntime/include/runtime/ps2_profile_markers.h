#pragma once
// DW5XL-DEBUG profiling markers (issue 025): field/job/VU1-cycle events stamped with steady_clock, written to a file
// so OS recorder samples (WPR, perf) can be lined up with what the game was doing. Compiled in only when
// DW5XL_DEVELOPER_DIAGNOSTICS=1 (separate diagnostic build, src/lib/ps2_profile_markers.cpp); otherwise every call
// is an empty inline function. At run time the game enables them with PS2X_PROFILE_MARKERS=<file>
// (PS2X_PROFILE_MARKERS_CAPACITY=<records>, default 1048576 = 32 MiB). Recording never allocates or locks.
//
// File: FileHeader, then `count` Records. Records are fixed size and written as-is (little endian).

#include <cstddef>
#include <cstdint>

namespace ps2prof
{
    enum class Event : uint16_t
    {
        FieldStart = 1,      // seq = field (vsync) number
        JobSubmit = 2,       // seq = device job sequence, kind = command kind
        JobStart = 3,
        JobEnd = 4,          // program = entry PC of the job's longest VU1 run, vu1Cycles = all its VU1 cycles
        DeviceWaitStart = 5, // the game thread blocks until the device finished job `seq`; kind = drain reason
        DeviceWaitEnd = 6,
        GameFrame = 7,       // seq = field in which a game frame was counted
        User = 100,          // tests and fixtures
    };

    struct Record
    {
        uint64_t ns;        // steady_clock nanoseconds
        uint64_t seq;
        uint32_t tid;       // OS thread id of the writer
        uint32_t program;
        uint32_t vu1Cycles; // saturates at 0xFFFFFFFF
        uint16_t event;
        uint16_t kind;
    };
    static_assert(sizeof(Record) == 32, "records are written as-is");

    struct ClockAnchor
    {
        uint64_t steadyNs;  // steady_clock nanoseconds
        uint64_t osTicks;   // QueryPerformanceCounter on Windows, CLOCK_MONOTONIC nanoseconds elsewhere
        uint64_t osFreq;    // ticks per second
    };

    struct FileHeader
    {
        char magic[8];      // "DW5XLPM1"
        uint32_t version;   // 1
        uint32_t recordSize;
        uint64_t count;
        uint64_t dropped;   // records lost because the buffer was full
        ClockAnchor atOpen;
        ClockAnchor atFlush;
    };
    static_assert(sizeof(FileHeader) == 80, "header is written as-is");

    // VU1 work done by the calling thread since the last takeVu1() (one device job).
    struct Vu1Totals
    {
        uint64_t cycles;
        uint64_t longestCycles;
        uint32_t longestPc;
    };

#if defined(DW5XL_DEVELOPER_DIAGNOSTICS) && DW5XL_DEVELOPER_DIAGNOSTICS
    inline constexpr bool kCompiledIn = true;

    bool open(const char *path, size_t capacity); // preallocates; one session per process; flushes at exit
    bool openFromEnv();
    bool enabled();
    void record(Event event, uint64_t seq, uint16_t kind = 0, uint32_t program = 0, uint32_t vu1Cycles = 0);
    uint64_t recorded();
    uint64_t dropped();
    bool flush(); // stops recording and writes the file; later calls return the first result
    void addVu1(uint32_t entryPc, uint64_t cycles);
    Vu1Totals takeVu1();
#else
    inline constexpr bool kCompiledIn = false;

    inline bool open(const char *, size_t) { return false; }
    inline bool openFromEnv() { return false; }
    inline bool enabled() { return false; }
    inline void record(Event, uint64_t, uint16_t = 0, uint32_t = 0, uint32_t = 0) {}
    inline uint64_t recorded() { return 0; }
    inline uint64_t dropped() { return 0; }
    inline bool flush() { return false; }
    inline void addVu1(uint32_t, uint64_t) {}
    inline Vu1Totals takeVu1() { return {}; }
#endif
}
