#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

// Issue 023 criterion 3 (fix 2): which field the window shows and when it stops waiting for the next one. One field
// identity: the presentation tick (stored at VBlankEnd, the new vsync tick) announces the end of field tick - 1, and
// the window shows exactly the announced field. It gives up waiting only when the game is stalled: a whole field after
// the next field's scheduled end (or one field from now when that is already past), never one field after the window
// started waiting, which ordinary jitter of a few ms crosses (023 work.md 2026-10-31: 12 fields shown twice).
namespace ps2x::presentation
{
    inline constexpr uint64_t kNoField = UINT64_MAX;

    inline constexpr uint64_t announcedField(uint64_t presentationTick)
    {
        return presentationTick == 0u ? kNoField : presentationTick - 1u;
    }

    inline constexpr bool isNewer(uint64_t field, uint64_t lastShown)
    {
        return field != kNoField && (lastShown == kNoField || field > lastShown);
    }

    inline std::chrono::steady_clock::time_point giveUp(std::chrono::steady_clock::time_point scheduledEnd,
                                                        std::chrono::steady_clock::time_point now,
                                                        std::chrono::steady_clock::duration field)
    {
        return std::max(scheduledEnd, now) + field;
    }
}
