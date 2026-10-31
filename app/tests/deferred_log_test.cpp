// CTest deferred_log (issue 023 criterion 6): debug lines that are expensive to compute (row profiles and hashes of
// every window picture, PS2X_WEAVE_TRACE) run on one helper thread, so the window loop that posts them does not spend
// its frame on them. Posting returns at once; jobs run in the order they were posted; drain() runs every queued job
// before it returns (every exit path calls it); above the memory budget the poster waits and nothing is dropped.
#include "runtime/ps2_stdio_pump.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace
{
    int g_failures = 0;
    void check(bool ok, const char *what)
    {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok)
            ++g_failures;
    }

    double msSince(std::chrono::steady_clock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    void waitFor(const std::atomic<bool> &flag)
    {
        while (!flag.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

int main()
{
    namespace pump = ps2x::stdio_pump;

    // 1. Posting does not run the job: a job that cannot finish yet does not hold up the poster or later posts.
    std::atomic<bool> release{false};
    std::vector<int> order; // written only by the helper; read after drain()
    const auto start = std::chrono::steady_clock::now();
    pump::postDeferred([&] { waitFor(release); order.push_back(-1); }, 16);
    for (int i = 0; i < 1000; ++i)
        pump::postDeferred([&order, i] { order.push_back(i); }, 16);
    check(msSince(start) < 200.0, "posting returns while the helper is still busy with an earlier job");
    release = true;

    // 2. drain() runs everything queued, in the order it was posted.
    pump::drain();
    bool inOrder = order.size() == 1001u && order[0] == -1;
    for (int i = 0; inOrder && i < 1000; ++i)
        inOrder = order[static_cast<size_t>(i) + 1u] == i;
    check(inOrder, "drain() returns after every queued job ran, in post order");

    // 3. Above the budget the poster waits (nothing is dropped) and the wait is counted.
    const uint64_t waitsBefore = pump::deferredWaitCount();
    std::atomic<bool> releaseBig{false}, bigRan{false}, smallRan{false}, smallPosted{false};
    pump::postDeferred([&] { waitFor(releaseBig); bigRan = true; }, pump::kDeferredBytes - 1u);
    std::thread poster([&] {
        pump::postDeferred([&] { smallRan = true; }, 2u);
        smallPosted = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    check(!smallPosted.load(), "a post that would exceed the budget waits for the helper");
    releaseBig = true;
    poster.join();
    pump::drain();
    check(bigRan.load() && smallRan.load(), "after the wait both jobs ran (none dropped)");
    check(pump::deferredWaitCount() == waitsBefore + 1u, "the wait is counted");

    // 4. One job larger than the budget is accepted when nothing is queued (no deadlock).
    std::atomic<bool> hugeRan{false};
    pump::postDeferred([&] { hugeRan = true; }, pump::kDeferredBytes * 2u);
    pump::drain();
    check(hugeRan.load(), "a single job above the budget runs when the queue is empty");

    // 5. drain() with nothing queued returns at once.
    const auto idle = std::chrono::steady_clock::now();
    pump::drain();
    check(msSince(idle) < 200.0, "drain() with nothing queued returns at once");

    std::printf("%s\n", g_failures ? "deferred_log: FAIL" : "deferred_log: PASS");
    return g_failures ? 1 : 0;
}
