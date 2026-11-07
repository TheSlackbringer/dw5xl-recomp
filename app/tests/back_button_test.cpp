// Issue 058: the pad's Back button. Alone it keeps its hold (the host action) and its short press (a short Select);
// with Start down it is the game's Select, so the game's own Select + Start reaches the game.
#include "runtime/back_button.h"

#include <cstdio>

namespace {
int failures = 0;
void check(bool ok, const char *what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}
constexpr double kHold = 0.75, kFrame = 0.02;
struct Totals { int selectFrames = 0, taps = 0, fires = 0; };
// Runs Back and Start from `backFrom` / `startFrom` to the given frames (frame numbers), 200 frames in all.
Totals play(int backFrom, int backTo, int startFrom, int startTo)
{
    ps2_host_input::BackButton button;
    Totals t;
    for (int f = 0; f < 200; ++f) {
        const auto r = button.step(f >= backFrom && f < backTo, f >= startFrom && f < startTo, f * kFrame, kHold);
        t.selectFrames += r.select; t.taps += r.tap; t.fires += r.fire;
    }
    return t;
}
}

int main()
{
    Totals t = play(10, 100, -1, -1);
    check(t.fires == 1 && t.taps == 0 && t.selectFrames == 0, "Back alone, held: the action once, no Select");
    t = play(10, 20, -1, -1);
    check(t.fires == 0 && t.taps == 1 && t.selectFrames == 0, "Back alone, short: one tap, no action");
    t = play(-1, -1, 10, 100);
    check(t.fires == 0 && t.taps == 0 && t.selectFrames == 0, "Start alone: nothing");
    t = play(10, 150, 20, 150);
    check(t.fires == 0 && t.taps == 0 && t.selectFrames == 130, "Back, then Start before the hold is up: Select from Start on, no action");
    t = play(20, 150, 10, 150);
    check(t.fires == 0 && t.taps == 0 && t.selectFrames == 130, "Start, then Back: Select at once, no action");
    t = play(10, 150, 10, 150);
    check(t.fires == 0 && t.taps == 0 && t.selectFrames == 140, "both in the same frame: Select at once, no action");
    t = play(10, 150, 20, 30);
    check(t.fires == 0 && t.taps == 0 && t.selectFrames == 130, "Start let go first: Back stays Select until it is let go, no action");
    t = play(10, 120, 80, 120);
    check(t.fires == 1 && t.taps == 0 && t.selectFrames == 0, "Start after the hold is up: only the action");
    t = play(10, 20, 30, 40);
    check(t.fires == 0 && t.taps == 1 && t.selectFrames == 0, "separate presses: Back is a short press");
    ps2_host_input::BackButton again;
    for (int f = 0; f < 60; ++f) again.step(f < 10 ? false : f < 30, f >= 15 && f < 30, f * kFrame, kHold);
    const auto later = again.step(true, false, 60 * kFrame, kHold);
    check(!later.select && !later.fire, "after both are let go, Back starts over");
    return failures ? 1 : 0;
}
