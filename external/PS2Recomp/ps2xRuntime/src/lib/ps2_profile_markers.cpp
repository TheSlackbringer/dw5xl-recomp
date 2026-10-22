// DW5XL-DEBUG profiling markers (issue 025), see runtime/ps2_profile_markers.h. Compiled only into diagnostic
// builds (DW5XL_DEVELOPER_DIAGNOSTICS=1).
#include "runtime/ps2_profile_markers.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#endif

namespace ps2prof
{
    namespace
    {
        struct State
        {
            std::atomic<bool> enabled{false};
            std::atomic<uint32_t> writers{0};
            std::atomic<uint64_t> next{0};
            Record *buffer = nullptr;
            uint64_t capacity = 0;
            ClockAnchor atOpen{};
            std::mutex mutex;
            bool flushed = false;
            bool flushOk = false;
            char path[4096] = {};
        };

        // Never destroyed: threads may still record while static destructors run at exit.
        State &state()
        {
            static State *s = new State();
            return *s;
        }

        uint64_t steadyNs()
        {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now().time_since_epoch())
                                             .count());
        }

        uint64_t osTicks()
        {
#ifdef _WIN32
            LARGE_INTEGER t;
            QueryPerformanceCounter(&t);
            return static_cast<uint64_t>(t.QuadPart);
#else
            timespec ts{};
            clock_gettime(CLOCK_MONOTONIC, &ts);
            return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
#endif
        }

        uint64_t osFreq()
        {
#ifdef _WIN32
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            return static_cast<uint64_t>(f.QuadPart);
#else
            return 1000000000ull;
#endif
        }

        // The steady_clock reading taken between two OS clock readings.
        ClockAnchor anchor()
        {
            const uint64_t before = osTicks();
            const uint64_t steady = steadyNs();
            const uint64_t after = osTicks();
            return ClockAnchor{steady, before + (after - before) / 2u, osFreq()};
        }

        uint32_t threadId()
        {
#ifdef _WIN32
            thread_local const uint32_t id = static_cast<uint32_t>(GetCurrentThreadId());
#else
            thread_local const uint32_t id = static_cast<uint32_t>(syscall(SYS_gettid));
#endif
            return id;
        }

        thread_local Vu1Totals t_vu1{};

        void flushAtExit() { flush(); }
    }

    bool open(const char *path, size_t capacity)
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.buffer || !path || !*path || capacity == 0u || std::strlen(path) >= sizeof(s.path))
            return false;
        FILE *f = std::fopen(path, "wb");
        if (!f)
            return false;
        std::fclose(f);
        s.buffer = new (std::nothrow) Record[capacity];
        if (!s.buffer)
            return false;
        s.capacity = capacity;
        std::memcpy(s.path, path, std::strlen(path) + 1u);
        s.atOpen = anchor();
        static const bool registered = (std::atexit(flushAtExit), true);
        (void)registered;
        s.enabled.store(true);
        return true;
    }

    bool openFromEnv()
    {
        const char *path = std::getenv("PS2X_PROFILE_MARKERS");
        if (!path || !*path)
            return false;
        size_t capacity = 1u << 20;
        if (const char *c = std::getenv("PS2X_PROFILE_MARKERS_CAPACITY"); c && *c)
            capacity = static_cast<size_t>(std::strtoull(c, nullptr, 10));
        const bool ok = open(path, capacity);
        std::fprintf(stderr, "[profile-markers] %s file=%s capacity=%zu recordBytes=%zu\n", ok ? "enabled" : "FAILED",
                     path, capacity, sizeof(Record));
        return ok;
    }

    bool enabled() { return state().enabled.load(); }

    void record(Event event, uint64_t seq, uint16_t kind, uint32_t program, uint32_t vu1Cycles)
    {
        State &s = state();
        if (!s.enabled.load())
            return;
        // flush() turns `enabled` off, then waits for `writers` to reach zero: a writer that registered before
        // that sees `enabled` again below and either finishes its slot or skips it.
        s.writers.fetch_add(1u);
        if (s.enabled.load())
        {
            const uint64_t i = s.next.fetch_add(1u, std::memory_order_relaxed);
            if (i < s.capacity)
                s.buffer[i] = Record{steadyNs(), seq, threadId(), program, vu1Cycles, static_cast<uint16_t>(event), kind};
        }
        s.writers.fetch_sub(1u);
    }

    uint64_t recorded()
    {
        State &s = state();
        return std::min(s.next.load(), s.capacity);
    }

    uint64_t dropped()
    {
        State &s = state();
        const uint64_t n = s.next.load();
        return n > s.capacity ? n - s.capacity : 0u;
    }

    bool flush()
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.buffer)
            return false;
        if (s.flushed)
            return s.flushOk;
        s.enabled.store(false);
        while (s.writers.load() != 0u)
            std::this_thread::yield();
        FileHeader header{};
        std::memcpy(header.magic, "DW5XLPM1", 8);
        header.version = 1u;
        header.recordSize = sizeof(Record);
        header.count = recorded();
        header.dropped = dropped();
        header.atOpen = s.atOpen;
        header.atFlush = anchor();
        bool ok = false;
        if (FILE *f = std::fopen(s.path, "wb"))
        {
            ok = std::fwrite(&header, sizeof(header), 1, f) == 1 &&
                 (header.count == 0u || std::fwrite(s.buffer, sizeof(Record), header.count, f) == header.count);
            ok = std::fclose(f) == 0 && ok;
        }
        s.flushed = true;
        s.flushOk = ok;
        std::fprintf(stderr, "[profile-markers] flushed records=%llu dropped=%llu ok=%d\n",
                     static_cast<unsigned long long>(header.count), static_cast<unsigned long long>(header.dropped), ok);
        return ok;
    }

    void addVu1(uint32_t entryPc, uint64_t cycles)
    {
        if (!state().enabled.load(std::memory_order_relaxed))
            return;
        Vu1Totals &t = t_vu1;
        t.cycles += cycles;
        if (cycles > t.longestCycles)
        {
            t.longestCycles = cycles;
            t.longestPc = entryPc;
        }
    }

    Vu1Totals takeVu1()
    {
        const Vu1Totals totals = t_vu1;
        t_vu1 = Vu1Totals{};
        return totals;
    }
}
