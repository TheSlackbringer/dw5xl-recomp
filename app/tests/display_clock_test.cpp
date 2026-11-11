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
    std::printf("%s (%d failed)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
