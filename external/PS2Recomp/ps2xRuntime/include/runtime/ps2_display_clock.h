// Issue 049 Part 2, amendment v3.5 (change 2 and 9): the display's clock.
// When is each coming refresh on screen? Pure code: every time is passed in (milliseconds on the PC's steady
// clock); the platform code that reads the system's reports feeds an Estimator, a test run uses a test display.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace ps2x::display_clock
{
    // A display as the window sees it.
    struct Grid
    {
        bool follows = false;  // it shows a picture when it is presented (variable refresh in effect, or VSYNC OFF)
        double periodMs = 0.0; // fixed rate: the time between two refreshes; follows: the shortest time between two
                               // pictures (its top rate), 0 = not known
        double originMs = 0.0; // fixed rate: the instant of refresh 0
    };

    inline double instantOf(const Grid &grid, int64_t refresh)
    {
        return grid.originMs + static_cast<double>(refresh) * grid.periodMs;
    }
    // The first refresh strictly after `ms` (fixed rate, periodMs > 0).
    int64_t refreshAfter(const Grid &grid, double ms);

    // A test display (hidden test runs have none): "60", "120", "144", "240" or any other rate in Hz: a fixed-rate
    // display whose refresh 0 is at `nowMs`; "follow" or "follow:<top rate in Hz>": a display that follows the
    // window. False when the text is not understood.
    bool parseTestDisplay(const char *text, double nowMs, Grid &out);

    // A fixed-rate display's clock from reports of single refreshes: each report is the time at which a program
    // saw a refresh (a swap that waited for it returned, or the system named it). Reports are late by a varying
    // amount and some refreshes are not reported at all. The estimate is the straight line through the last
    // `window` reports (least squares over refresh numbers): its slope is the period, and a report's usual
    // lateness is part of the line, which moves every picture alike.
    class Estimator
    {
      public:
        explicit Estimator(double nominalPeriodMs, size_t window = 240);
        // A refresh seen at `ms`; its number is worked out from the time since the report before.
        void report(double ms);
        // The same when the system names the refresh (X11: the msc of a Present event).
        void report(int64_t number, double ms);
        size_t reports() const { return m_reports.size(); }
        // At least 8 reports and a period within 5 % of the nominal one.
        bool settled() const;
        Grid grid() const;
        // How far the reports lie from the line (root mean square, ms).
        double spreadMs() const { return m_spreadMs; }
        void reset();

      private:
        void fit();
        struct Report { double number, ms; };
        double m_nominalMs;
        size_t m_window;
        std::deque<Report> m_reports;
        double m_periodMs, m_originMs = 0.0, m_spreadMs = 0.0;
        double m_firstNumber = 0.0;
    };

    // ---- Shared between the window and the code that decides which pictures to make (thread-safe) ----

    // Milliseconds on the PC's steady clock, as every time in this file.
    double toMs(std::chrono::steady_clock::time_point time);
    std::chrono::steady_clock::time_point fromMs(double ms);
    double nowMs();

    // The display the window presents on (window thread); false until one is published.
    void publish(const Grid &grid);
    bool published(Grid &out);

    // A timed picture: made for one instant on screen. The code that makes it announces it under the tag of its
    // picture read, before the read is started; the window takes it when the read arrives.
    struct Timed
    {
        double atMs = 0.0;    // the instant it is on screen
        int64_t refresh = -1; // fixed rate: the refresh it is made for
        uint32_t frame = 0;   // between game frames frame-1 and frame ...
        double t = 0.0;       // ... at t
        bool own = false;     // exactly the newer frame (t = 1)
        double instantMs = 0.0; // its newer frame's instant (the start of the field its list was sent in)
        double listMs = 0.0;  // when that list arrived (for the head start's measure)
        double queuedMs = 0.0; // when the picture was built and handed to the device
        double headStartMs = 0.0; // the head start in force when its instant was set
        bool isVoid = false;  // the time line moved on before it was shown
    };
    void announce(uint64_t tag, const Timed &timed);
    bool take(uint64_t tag, Timed &out);
    // Announced pictures for an instant at or after `atMs` are void (the time line moved on to a newer frame).
    void voidFrom(double atMs);
    // The latest instant any announced picture was made for (0: none yet).
    double latestAnnouncedMs();
    void forgetAnnounced();
    // How a timed picture fared (the window reports, the code that chooses head start and rate takes them).
    struct Outcome
    {
        Timed timed;
        double startedMs = 0.0; // its read was started: its drawing had left the device
        double readyMs = 0.0;   // its read was finished
        double handMs = 0.0;    // what the window needed from taking it to handing it over (a dropped one: the last such time)
        bool shown = false;     // false: it missed its instant
    };
    void report(const Outcome &outcome);
    void takeOutcomes(std::vector<Outcome> &out);

    // DW5XL-DEBUG PS2X_SMOOTH_TIMELINE=1: the time-line log lines the 049 test reads ([smooth:list], [smooth:shown],
    // [smooth:game], [smooth:rate], [smooth:missed]).
    bool logTimeline();
}
