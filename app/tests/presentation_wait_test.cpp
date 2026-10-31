// CTest presentation_wait (issue 023 criterion 3, fix 2): the window shows each field once. The window waits for the
// next field's end to be announced (VBlankEnd) and gives up only when the game is stalled. The give-up point decides
// whether an ordinary late field (a few ms of jitter) makes the window show the previous picture again: it must be a
// whole field after the next field's scheduled end, not one field after the window started waiting.
// A small model of the window loop runs over fields with the jitter measured on the Windows laptop (2026-10-31: field
// end announced 0.4-2.6 ms after its scheduled time, a window frame 0.5-4 ms), then over a stall.
#include "runtime/presentation_pacing.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
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

    using Clock = std::chrono::steady_clock;
    using us = std::chrono::microseconds;
    constexpr us kField{20000};

    struct Lcg
    {
        uint32_t s = 12345u;
        uint32_t next(uint32_t range) { s = s * 1664525u + 1013904223u; return (s >> 8) % range; }
    };

    struct Result
    {
        std::vector<uint64_t> shownFields; // field of every window frame (repeats included)
        std::vector<uint64_t> freshFields; // field of every frame that showed a new field
        std::vector<uint64_t> staleAfter;  // for every frame that showed the previous field again: that field
        uint64_t staleFrames = 0;
        us longestGap{0};                  // longest time between two window frames
    };

    // Fields 0..count-1 are scheduled every 20 ms (their end announced at k * 20 ms + 0.5 ms) and announced
    // `lateness` later; fields from stallFrom on arrive stallUs later (the game stalled). oldRule: give up one field
    // after the window started waiting (the code before fix 2).
    Result simulate(bool oldRule, uint64_t count, uint64_t stallFrom = UINT64_MAX, us stall = us{0})
    {
        namespace pp = ps2x::presentation;
        Lcg rng;
        const Clock::time_point base{};
        std::vector<Clock::time_point> scheduled(count), announced(count);
        for (uint64_t k = 0; k < count; ++k)
        {
            // The stalled field keeps its scheduled time and arrives late; the fields after it are scheduled anew
            // after the stall (the scheduler re-anchors when it is several fields behind).
            const Clock::time_point grid = base + kField * static_cast<int64_t>(k) + us{500};
            scheduled[k] = grid + (k > stallFrom && k != UINT64_MAX ? stall : us{0});
            announced[k] = grid + (k >= stallFrom ? stall : us{0}) + us{400 + static_cast<int64_t>(rng.next(2200))};
        }
        Result r;
        uint64_t lastShown = pp::kNoField;
        Clock::time_point now = announced[0], lastFrame = announced[0];
        uint64_t next = 0; // the next field to be announced
        while (next < count)
        {
            // Wait: the newest announced field if it is newer than the last shown, else until it comes or the give-up.
            const Clock::time_point giveUp = oldRule ? now + kField : pp::giveUp(scheduled[next], now, kField);
            uint64_t field = pp::kNoField;
            if (announced[next] <= now || announced[next] <= giveUp)
            {
                now = std::max(now, announced[next]);
                while (next + 1 < count && announced[next + 1] <= now)
                    ++next; // more than one announced meanwhile: the newest
                field = next++;
            }
            else
                now = giveUp;
            const bool fresh = pp::isNewer(field, lastShown);
            if (fresh)
            {
                lastShown = field;
                r.freshFields.push_back(field);
            }
            else
            {
                ++r.staleFrames;
                r.staleAfter.push_back(lastShown);
            }
            r.shownFields.push_back(lastShown);
            r.longestGap = std::max(r.longestGap, std::chrono::duration_cast<us>(now - lastFrame));
            lastFrame = now;
            now += us{500 + static_cast<int64_t>(rng.next(3500))}; // the frame itself: upload, draw, swap
        }
        return r;
    }

    // Every field shown new exactly once, in order.
    bool eachOnce(const Result &r, uint64_t count)
    {
        std::vector<int> seen(count, 0);
        for (uint64_t f : r.freshFields)
            if (f < count)
                ++seen[f];
        for (int n : seen)
            if (n != 1)
                return false;
        return true;
    }
}

int main()
{
    namespace pp = ps2x::presentation;
    check(pp::announcedField(0) == pp::kNoField && pp::announcedField(1) == 0 && pp::announcedField(7401) == 7400,
          "the presentation tick N announces the end of field N - 1 (none before the first)");
    check(pp::isNewer(0, pp::kNoField) && pp::isNewer(5, 4) && !pp::isNewer(4, 4) && !pp::isNewer(pp::kNoField, 4),
          "a field is fresh only when it is newer than the last one shown");

    const uint64_t kFields = 20000;
    const Result before = simulate(true, kFields);
    std::printf("  old give-up (one field after the wait starts): %llu frames re-showed the previous field\n",
                static_cast<unsigned long long>(before.staleFrames));
    check(before.staleFrames > 0, "the model reproduces the bug with the old give-up (fields shown again)");

    const Result after = simulate(false, kFields);
    check(after.staleFrames == 0 && eachOnce(after, kFields),
          "give-up a whole field after the next field's scheduled end: every field shown exactly once");

    // A stall of 210 ms after field 1000: the window keeps drawing (the last picture) every field period, and after the
    // stall every field is again shown once.
    const Result stalled = simulate(false, 2000, 1000, us{210000});
    bool staleOnlyInStall = true;
    for (uint64_t f : stalled.staleAfter)
        staleOnlyInStall = staleOnlyInStall && f == 999u; // the picture held is the last one before the stall
    std::printf("  stall of 210 ms: %llu frames showed the last picture again, longest gap %.1f ms\n",
                static_cast<unsigned long long>(stalled.staleFrames), stalled.longestGap.count() / 1000.0);
    check(stalled.staleFrames >= 8 && stalled.staleFrames <= 11,
          "during a stall the window still draws about every field period (the last picture)");
    check(stalled.longestGap <= kField + us{4000} + kField, "no window gap of more than two fields during the stall");
    check(eachOnce(stalled, 2000) && staleOnlyInStall,
          "every field shown new exactly once; the last picture is held only during the stall");

    std::printf("%s\n", g_failures ? "presentation_wait: FAIL" : "presentation_wait: PASS");
    return g_failures ? 1 : 0;
}
