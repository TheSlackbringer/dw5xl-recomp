// Profiling test program (fixture): a small native program whose behaviour is known exactly, so the recorders,
// converters and report can be checked against ground truth. Part of tools/profile (a debugging tool), not game code.
//
// Each cycle: "fixture-hot" runs fixture_hot_outer -> fixture_hot_spin for --hot-ms; then "fixture-waiter"
// blocks in fixture_wait_point while "fixture-waker" runs fixture_wake_work for --wait-ms and then wakes it in
// fixture_wake_signal. Ground truth (steady_clock nanoseconds, the clock the game markers use) goes to
// <out>/truth.json. The same moments are also written as profiling markers (Event::User) when the marker code is
// compiled in, so the report can show that markers and recorder samples line up.
// --work-only: only the hot phase, once, for --hot-ms; truth.json then holds the work rate for the overhead gate.
//
// Build (Windows, llvm-mingw): x86_64-w64-mingw32-clang++ -std=c++20 -O2 -g -gcodeview -fno-omit-frame-pointer
//   -static -DDW5XL_DEVELOPER_DIAGNOSTICS=1 -I<repo>/external/PS2Recomp/ps2xRuntime/include profile_fixture.cpp
//   <repo>/external/PS2Recomp/ps2xRuntime/src/lib/ps2_profile_markers.cpp -o dw5xl_profile_fixture.exe -Wl,--pdb=
// Build (Linux): c++ -std=c++20 -O2 -g -fno-omit-frame-pointer -pthread (same -D/-I/sources) -o dw5xl_profile_fixture
#include "runtime/ps2_profile_markers.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

// No inlining and no tail calls: each named function keeps its own stack frame, so call stacks show it
// (fixture_wake_signal ends in notify_one, which would otherwise become a jump and vanish from the waker's stack).
// GCC (Linux `c++`) ignores clang's disable_tail_calls; its equivalent is the per-function optimize option.
#if defined(__clang__)
#define FIXTURE_NOINLINE __attribute__((noinline, disable_tail_calls))
#else
#define FIXTURE_NOINLINE __attribute__((noinline, optimize("no-optimize-sibling-calls")))
#endif

namespace {

uint64_t nowNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void nameThread(const char *name) {
#ifdef _WIN32
    wchar_t wide[64] = {};
    for (size_t i = 0; name[i] && i + 1 < 64; ++i) wide[i] = static_cast<wchar_t>(name[i]);
    SetThreadDescription(GetCurrentThread(), wide);
#else
    pthread_setname_np(pthread_self(), name);
#endif
}

uint64_t osThreadId() {
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return static_cast<uint64_t>(gettid());
#endif
}

volatile uint64_t g_sink;

// Pure CPU work with no calls inside the loop, so samples land in this function itself.
FIXTURE_NOINLINE uint64_t fixture_hot_spin(uint64_t untilNs) {
    uint64_t x = 0x9E3779B97F4A7C15ull, iterations = 0;
    while (nowNs() < untilNs) {
        for (int i = 0; i < 4096; ++i) {
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        }
        iterations += 4096;
    }
    g_sink = x;
    return iterations;
}

FIXTURE_NOINLINE uint64_t fixture_hot_outer(uint64_t untilNs) {
    const uint64_t n = fixture_hot_spin(untilNs);
    g_sink = g_sink + 1;
    return n;
}

FIXTURE_NOINLINE uint64_t fixture_wake_work(uint64_t untilNs) {
    return fixture_hot_spin(untilNs);
}

std::mutex g_mutex;
std::condition_variable g_cv;
uint64_t g_wakeGeneration = 0;

FIXTURE_NOINLINE void fixture_wake_signal() {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ++g_wakeGeneration;
    }
    g_cv.notify_one();
}

FIXTURE_NOINLINE void fixture_wait_point(uint64_t generation) {
    std::unique_lock<std::mutex> lock(g_mutex);
    g_cv.wait(lock, [&] { return g_wakeGeneration >= generation; });
}

struct Cycle {
    uint64_t hotStart = 0, hotEnd = 0, waitStart = 0, signal = 0, woken = 0;
};

struct Options {
    std::string out = ".";
    int cycles = 5, hotMs = 300, waitMs = 50, gapMs = 50;
    bool workOnly = false;
};

Options parse(int argc, char **argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](int &v) { if (i + 1 < argc) v = std::atoi(argv[++i]); };
        if (a == "--out" && i + 1 < argc) o.out = argv[++i];
        else if (a == "--cycles") next(o.cycles);
        else if (a == "--hot-ms") next(o.hotMs);
        else if (a == "--wait-ms") next(o.waitMs);
        else if (a == "--gap-ms") next(o.gapMs);
        else if (a == "--work-only") o.workOnly = true;
        else { std::fprintf(stderr, "unknown argument %s\n", a.c_str()); std::exit(2); }
    }
    return o;
}

void marker(uint64_t value) { ps2prof::record(ps2prof::Event::User, value); }

} // namespace

int main(int argc, char **argv) {
    const Options o = parse(argc, argv);
    std::error_code dirError;
    std::filesystem::create_directories(o.out, dirError);
    const std::string markerPath = o.out + "/markers.bin";
    const bool markersOpen = ps2prof::open(markerPath.c_str(), 4096);
    const uint64_t startNs = nowNs();
    std::vector<Cycle> cycles(o.workOnly ? 1 : o.cycles);
    uint64_t workIterations = 0, workNs = 0;
    std::atomic<uint64_t> hotTid{0}, waiterTid{0}, wakerTid{0};

    if (o.workOnly) {
        std::thread hot([&] {
            nameThread("fixture-hot");
            hotTid = osThreadId();
            const uint64_t begin = nowNs();
            cycles[0].hotStart = begin;
            marker(1);
            workIterations = fixture_hot_outer(begin + uint64_t(o.hotMs) * 1000000u);
            cycles[0].hotEnd = nowNs();
            marker(2);
            workNs = cycles[0].hotEnd - begin;
        });
        hot.join();
    } else {
        for (size_t c = 0; c < cycles.size(); ++c) {
            Cycle &cy = cycles[c];
            std::thread hot([&] {
                nameThread("fixture-hot");
                hotTid = osThreadId();
                cy.hotStart = nowNs();
                marker(10 * (c + 1) + 1);
                fixture_hot_outer(cy.hotStart + uint64_t(o.hotMs) * 1000000u);
                cy.hotEnd = nowNs();
                marker(10 * (c + 1) + 2);
            });
            hot.join();
            const uint64_t generation = c + 1;
            std::atomic<bool> waiting{false};
            std::thread waiter([&] {
                nameThread("fixture-waiter");
                waiterTid = osThreadId();
                cy.waitStart = nowNs();
                marker(10 * (c + 1) + 3);
                waiting = true;
                fixture_wait_point(generation);
                cy.woken = nowNs();
                marker(10 * (c + 1) + 5);
            });
            std::thread waker([&] {
                nameThread("fixture-waker");
                wakerTid = osThreadId();
                while (!waiting) std::this_thread::yield();
                fixture_wake_work(nowNs() + uint64_t(o.waitMs) * 1000000u);
                cy.signal = nowNs();
                marker(10 * (c + 1) + 4);
                fixture_wake_signal();
            });
            waker.join();
            waiter.join();
            std::this_thread::sleep_for(std::chrono::milliseconds(o.gapMs));
        }
    }
    const bool markersFlushed = markersOpen && ps2prof::flush();

    const std::string truthPath = o.out + "/truth.json";
    FILE *f = std::fopen(truthPath.c_str(), "w");
    if (!f) { std::fprintf(stderr, "cannot write %s\n", truthPath.c_str()); return 1; }
#ifdef _WIN32
    const unsigned long pid = GetCurrentProcessId();
#else
    const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
    std::fprintf(f, "{\"schema\": 1, \"pid\": %lu, \"start_ns\": %llu, \"markers\": %s, \"marker_file\": \"markers.bin\",\n",
                 pid, (unsigned long long)startNs, markersFlushed ? "true" : "false");
    std::fprintf(f, " \"threads\": {\"fixture-hot\": %llu, \"fixture-waiter\": %llu, \"fixture-waker\": %llu},\n",
                 (unsigned long long)hotTid.load(), (unsigned long long)waiterTid.load(), (unsigned long long)wakerTid.load());
    std::fprintf(f, " \"hot_ms\": %d, \"wait_ms\": %d, \"work_only\": %s, \"work_iterations\": %llu, \"work_ns\": %llu,\n",
                 o.hotMs, o.waitMs, o.workOnly ? "true" : "false", (unsigned long long)workIterations, (unsigned long long)workNs);
    std::fprintf(f, " \"cycles\": [");
    for (size_t c = 0; c < cycles.size(); ++c) {
        const Cycle &cy = cycles[c];
        std::fprintf(f, "%s\n  {\"hot_start_ns\": %llu, \"hot_end_ns\": %llu, \"wait_start_ns\": %llu, \"signal_ns\": %llu, \"woken_ns\": %llu}",
                     c ? "," : "", (unsigned long long)cy.hotStart, (unsigned long long)cy.hotEnd,
                     (unsigned long long)cy.waitStart, (unsigned long long)cy.signal, (unsigned long long)cy.woken);
    }
    std::fprintf(f, "\n ]}\n");
    std::fclose(f);
    return 0;
}
