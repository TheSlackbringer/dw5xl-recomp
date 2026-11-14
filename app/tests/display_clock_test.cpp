// Issue 049 Part 2, amendment v3.5: the display's clock (runtime/ps2_display_clock.h). No display and no clock:
// refresh reports are made up here.
//  - the test display's description is read ("60", "follow", "follow:240"), nonsense is refused;
//  - the first refresh after a time;
//  - a fixed-rate display's period and refresh instants from late and missing reports;
//  - reports that name their refresh (X11 Present events) give the system's own refresh numbers.
#include "runtime/ps2_display_clock.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

using namespace ps2x::display_clock;

namespace
{
    int failures = 0;
    void check(bool ok, const std::string &what)
    {
        std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
        failures += ok ? 0 : 1;
    }

    // A fixed sequence of numbers in [0, 1) (the same on every machine).
    struct Sequence
    {
        uint32_t state = 12345u;
        double next()
        {
            state = state * 1664525u + 1013904223u;
            return static_cast<double>(state >> 8) / 16777216.0;
        }
    };
}

int main()
{
    {
        Grid a, b, c, d;
        const bool fixed = parseTestDisplay("60", 500.0, a);
        const bool follows = parseTestDisplay("follow", 500.0, b);
        const bool capped = parseTestDisplay("follow:240", 500.0, c);
        check(fixed && !a.follows && std::fabs(a.periodMs - 1000.0 / 60.0) < 1e-9 && a.originMs == 500.0,
              "test display \"60\": a fixed-rate display, 16.67 ms per refresh, refresh 0 now");
        check(follows && b.follows && b.periodMs == 0.0 && capped && c.follows && std::fabs(c.periodMs - 1000.0 / 240.0) < 1e-9,
              "test display \"follow\" and \"follow:240\": a display that follows the window, with or without a top rate");
        check(!parseTestDisplay("", 0.0, d) && !parseTestDisplay("abc", 0.0, d) && !parseTestDisplay("0", 0.0, d) &&
                  !parseTestDisplay("follow240", 0.0, d) && !parseTestDisplay("60hz", 0.0, d) && !parseTestDisplay(nullptr, 0.0, d),
              "a test display that is not understood is refused");
    }
    {
        Grid grid;
        grid.periodMs = 10.0;
        grid.originMs = 5.0;
        check(refreshAfter(grid, 5.0) == 1 && refreshAfter(grid, 14.9) == 1 && refreshAfter(grid, 15.0) == 2 &&
                  refreshAfter(grid, -7.0) == -1 && instantOf(grid, 2) == 25.0,
              "the first refresh strictly after a time (a time exactly on a refresh gives the next one)");
    }
    {
        // 240 Hz as Windows names it (4.16670 ms); each report up to 0.3 ms late, one in twenty missing.
        const double period = 4.16670, origin = 123.4;
        Estimator clock(1000.0 / 240.0);
        Sequence random;
        check(!clock.settled(), "no reports yet: the clock is not settled");
        int64_t last = 0;
        for (int64_t n = 0; n < 400; ++n)
        {
            const double late = random.next() * 0.3;
            if (random.next() < 0.05)
                continue;
            clock.report(origin + period * static_cast<double>(n) + late);
            last = n;
        }
        const Grid grid = clock.grid();
        // The estimator counts from its first report; find the refresh number it gives the last one.
        const int64_t own = refreshAfter(grid, origin + period * static_cast<double>(last) - period / 2.0);
        const double next = instantOf(grid, own + 1), truth = origin + period * static_cast<double>(last + 1) + 0.15;
        check(clock.settled() && std::fabs(grid.periodMs - period) < 5e-4,
              "late and missing reports: the period is found to better than 0.0005 ms");
        check(std::fabs(next - truth) < 0.05 && clock.spreadMs() > 0.05 && clock.spreadMs() < 0.12,
              "and the next refresh is foreseen within 0.05 ms of where the reports put it (their usual lateness included)");
    }
    {
        // A nominal period that is 1 % off still finds the real one.
        const double period = 16.6833;
        Estimator clock(16.85);
        for (int n = 0; n < 240; ++n)
            clock.report(50.0 + period * n);
        check(clock.settled() && std::fabs(clock.grid().periodMs - period) < 1e-6,
              "a nominal period 1 % off: the real period is found");
    }
    {
        // Reports that name their refresh (X11 Present events): exact, with gaps.
        const double period = 1000.0 / 144.0, origin = -2000.0;
        Estimator clock(period);
        for (int64_t msc = 700000; msc < 700300; msc += (msc % 7 == 0 ? 2 : 1))
            clock.report(msc, origin + period * static_cast<double>(msc));
        clock.report(int64_t(700100), 0.0); // an old report is left out
        const Grid grid = clock.grid();
        check(clock.settled() && std::fabs(instantOf(grid, 701000) - (origin + period * 701000.0)) < 1e-4 &&
                  clock.spreadMs() < 1e-6,
              "named refreshes: the grid uses the system's own refresh numbers");
    }
    // ---- A real display's clock from the driver's reports (amendment v3.12, step 5) ----
    // A fixed-rate display as the driver reports it: refresh k is at origin + k * period, and a picture handed over
    // at `called` is shown by the first refresh whose deadline (its instant less `lead`) has not passed.
    struct FixedDisplay
    {
        double period, origin, lead;
        double shownAt(double called) const
        {
            return origin + std::ceil((called + lead - origin) / period) * period;
        }
    };
    {
        // The game's own pictures, about 50 a second at any time between two refreshes.
        const FixedDisplay display{4.1668, 1000.0, 4.9};
        RealDisplay clock;
        Grid grid;
        clock.report({1, 1.0, 5.0});
        check(!clock.grid(grid) && clock.kind() == RealDisplay::Kind::Unknown,
              "a real display: nothing is known without the driver's refresh time");
        clock.refresh(display.period);
        Sequence noise;
        double called = 1003.3;
        uint64_t present = 0;
        for (int n = 0; n < 11; ++n)
        {
            called += 19.0 + 2.0 * noise.next();
            clock.report({++present, called, display.shownAt(called) + (noise.next() - 0.5) * 0.1});
        }
        check(!clock.grid(grid), "... and nothing from fewer than twelve reports");
        for (int n = 0; n < 200; ++n)
        {
            called += 19.0 + 2.0 * noise.next();
            clock.report({++present, called, display.shownAt(called) + (noise.next() - 0.5) * 0.1});
        }
        const bool known = clock.grid(grid);
        check(known && clock.kind() == RealDisplay::Kind::Fixed && !grid.follows,
              "a real fixed-rate display is recognised from the reports");
        check(known && std::fabs(grid.periodMs - display.period) < 0.002, "... its refresh time is found");
        check(std::fabs(clock.leadMs() - display.lead) < 0.3, "... and the time from a hand-over deadline to its refresh");
        bool meant = known;
        const int64_t first = refreshAfter(grid, called) + 1;
        for (int64_t n = first; meant && n < first + 50; ++n)
            meant = std::fabs(display.shownAt(instantOf(grid, n) - kAimBeforeDeadline * grid.periodMs) - (instantOf(grid, n) + clock.leadMs())) < 0.3;
        check(meant, "... a picture handed over soon after the deadline before its own is shown by its own deadline's refresh");
        clock.refresh(display.period + 0.0002);
        check(clock.grid(grid) && clock.kind() == RealDisplay::Kind::Fixed,
              "... the driver's small corrections of its refresh time do not start the clock anew");

        // Then pictures aimed with the clock's own grid, every second refresh, 4000 in a row (half a minute): each
        // is shown by its refresh, and the lead stays where it was (aimed pictures say nothing new about it).
        const double leadBefore = clock.leadMs();
        int wrong = 0, made = 0;
        double moved = 0.0;
        int64_t refresh = refreshAfter(grid, called) + 2;
        for (; made < 4000 && clock.grid(grid); ++made, refresh += 2)
        {
            const double at = instantOf(grid, refresh);
            called = at - kAimBeforeDeadline * grid.periodMs + noise.next() * 0.4; // never early: the window waits for the time
            const double out = display.shownAt(called) + (noise.next() - 0.5) * 0.1;
            wrong += std::fabs(out - (at + clock.leadMs())) > 0.5 * grid.periodMs ? 1 : 0;
            clock.report({++present, called, out, at});
            moved = std::fmax(moved, std::fabs(clock.leadMs() - leadBefore));
        }
        check(made == 4000 && wrong == 0, "pictures aimed with the clock's own grid: each of 4000 is shown by its refresh");
        check(moved < 1e-9 && clock.aimedMissed() == 0, "... and the lead does not wander while they are shown");
    }
    {
        // The system shows some pictures a refresh late (a busy compositor): the lead holds.
        const FixedDisplay display{4.1668, 500.0, 4.9};
        RealDisplay clock;
        clock.refresh(display.period);
        Sequence noise;
        double called = 503.0;
        for (uint64_t n = 1; n <= 300; ++n)
        {
            called += 19.0 + 2.0 * noise.next();
            const double late = n % 20 == 0 ? display.period : 0.0;
            clock.report({n, called, display.shownAt(called) + late + (noise.next() - 0.5) * 0.1});
        }
        Grid grid;
        check(clock.grid(grid) && std::fabs(clock.leadMs() - display.lead) < 0.4,
              "one picture in twenty shown a refresh late: the lead holds");
    }
    {
        // The window is shown another way from now on (the lead is much shorter, as when a window goes straight to
        // the display, or much longer): the aimed pictures miss their refresh, the lead is learned from them, and
        // they are shown by their refresh again. (A lead that grows by less than the room a hand-over has is not
        // seen this way: some pictures come a refresh late until the clock starts anew; work log 2026-11-14.)
        for (const double newLead : {1.0, 9.5})
        {
        FixedDisplay display{4.1668, 0.0, 4.9};
        RealDisplay clock;
        clock.refresh(display.period);
        Sequence noise;
        double called = 3.0;
        uint64_t present = 0;
        for (int n = 0; n < 200; ++n)
        {
            called += 19.0 + 2.0 * noise.next();
            clock.report({++present, called, display.shownAt(called) + (noise.next() - 0.5) * 0.1});
        }
        Grid grid;
        bool known = clock.grid(grid);
        display.lead = newLead;
        int64_t refresh = known ? refreshAfter(grid, called) + 2 : 0;
        int wrongLate = 0;
        for (int n = 0; n < 400 && known; ++n, refresh += 2)
        {
            const double at = instantOf(grid, refresh);
            called = at - kAimBeforeDeadline * grid.periodMs + noise.next() * 0.4;
            const double out = display.shownAt(called) + (noise.next() - 0.5) * 0.1;
            if (n >= 200)
                wrongLate += std::fabs(out - (at + clock.leadMs())) > 0.5 * grid.periodMs ? 1 : 0;
            clock.report({++present, called, out, at});
            known = clock.grid(grid);
        }
        check(known && wrongLate == 0 && clock.aimedMissed() == 0,
              std::string("the lead changed to ") + (newLead < 4.9 ? "a shorter" : "a longer") +
                  " one: after the aimed pictures missed, it is learned again and they are shown by their refresh");
        }
    }
    {
        // A display that follows the window: a picture is on screen when it is handed over (the reports name a
        // time a little before or after the hand-over), 50 a second.
        RealDisplay clock;
        clock.refresh(1000.0 / 240.0);
        Sequence noise;
        double called = 100.0;
        for (uint64_t n = 1; n <= 60; ++n)
        {
            called += 20.0 + (noise.next() - 0.5) * 0.3;
            clock.report({n, called, called - 2.0 + noise.next()});
        }
        Grid grid;
        check(clock.grid(grid) && clock.kind() == RealDisplay::Kind::Follows && grid.follows &&
                  std::fabs(grid.periodMs - 1000.0 / 240.0) < 1e-9,
              "a display that follows the window is recognised, with the driver's refresh time as its shortest");
        // A long silence (the window was minimised): what was known is dropped until new reports are in.
        called += 3000.0;
        clock.report({61, called, called - 2.0});
        check(!clock.grid(grid) && clock.kind() == RealDisplay::Kind::Unknown, "after a long silence the display is unknown again");
    }
    {
        // Reports that lie on a grid of another refresh time than the driver's are not taken for its grid.
        RealDisplay clock;
        clock.refresh(1000.0 / 240.0);
        for (uint64_t n = 1; n <= 100; ++n)
            clock.report({n, 4.2 * 5.0 * static_cast<double>(n), 4.2 * 5.0 * static_cast<double>(n) + 6.0});
        Grid grid;
        check(!clock.grid(grid) || grid.follows || std::fabs(grid.periodMs - 1000.0 / 240.0) < 0.005 * 1000.0 / 240.0,
              "a grid with another refresh time than the driver's is never handed out as fixed");
    }
    {
        Grid grid;
        publish(Grid{false, 10.0, 0.0});
        const bool was = published(grid);
        withdraw();
        check(was && !published(grid), "a published display can be withdrawn");
    }
    std::printf("%s (%d failed)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
