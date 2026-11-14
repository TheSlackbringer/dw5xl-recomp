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

    // ---- A real display's clock from the driver's reports (issue 049 amendment v3.12, step 5) ----

    // What the graphics driver reports of one presented picture.
    struct ShownReport
    {
        uint64_t present = 0;  // the present's number (growing)
        double calledMs = 0.0; // when the window handed the picture over (its present call)
        double outMs = 0.0;    // when its first pixel left for the display
        double meantMs = 0.0;  // above 0: the hand-over was aimed at a refresh; that refresh's instant on the grid
    };

    // The display the window presents on, worked out from those reports and the refresh time the driver names.
    //  - Fixed: the reports lie on the driver's refresh grid. The grid handed out is the grid of hand-over
    //    deadlines: the instant of refresh n is the latest time a picture can be handed over and still be shown by
    //    refresh n. The refresh itself comes leadMs() later, the same for every picture.
    //  - Follows: the reports follow the hand-overs and not a grid (variable refresh in effect, or no vsync where
    //    the system shows a picture at once). The grid says `follows`, with the driver's refresh time as the
    //    shortest time between two pictures.
    //  - Unknown: no reports, or too few (a hidden window, a screen that is off, a driver that has none).
    // On a display that follows, pictures handed over on whole refresh times look like a grid and are taken for
    // one; the fixed-rate rule is right on both kinds (spec, change 3).
    class RealDisplay
    {
      public:
        enum class Kind { Unknown, Fixed, Follows };
        // The driver's refresh time (0: not known). Another one (more than half a hundredth off) starts the clock
        // anew; the driver's own small corrections of it do not (measured: 4.1666 to 4.1668 ms within one run).
        void refresh(double periodMs);
        void report(const ShownReport &report);
        Kind kind() const { return m_kind; }
        // False while the display is not known well enough to make pictures for it.
        bool grid(Grid &out) const;
        // Fixed: the time from a hand-over deadline to its refresh. A picture handed over at any time is shown
        // between this lead and one refresh more after its hand-over, never sooner, and later only when the system
        // was slow. So the lead is taken from the short end of the last such times: their 10th hundredth, less a
        // tenth of a refresh (measured on a 240 Hz display: the times lie evenly between 4.7 and 8.9 ms). It is
        // learned from pictures handed over at any time (the game's own, field by field). Pictures aimed at a
        // refresh all come at one place between two deadlines and say nothing new about the lead, so they leave it
        // alone, unless most of them are not shown by the refresh they were aimed at: then the lead is wrong (the
        // window is shown another way now) and is taken from them: the middle of their times from the deadline
        // they were aimed at to the refresh that showed them.
        double leadMs() const { return m_leadMs; }
        size_t reports() const { return m_latencies.size() + m_aimed.size(); }
        // Of the last pictures aimed at a refresh (at most 32): how many another refresh showed.
        size_t aimedMissed() const;
        // Whether a picture aimed at the refresh of deadline `meantMs` was shown by that refresh.
        bool madeItsRefresh(double meantMs, double outMs) const;
        void reset();

      private:
        double m_nominalMs = 0.0;
        Estimator m_line{1.0};
        std::deque<double> m_offs;      // the last reports' distances from a whole number of refreshes, in refreshes
        std::deque<double> m_latencies; // hand-over to refresh, of the last pictures handed over at any time
        std::deque<double> m_aimed;     // of the last pictures aimed at a refresh: that refresh's deadline to the refresh that showed it
        std::deque<bool> m_missed;      // ... and whether another refresh showed them
        bool m_haveLead = false;
        Kind m_kind = Kind::Unknown;
        bool m_have = false;
        uint64_t m_lastPresent = 0;
        double m_lastOutMs = 0.0;
        double m_leadMs = 0.0;
    };

    // ---- Shared between the window and the code that decides which pictures to make (thread-safe) ----

    // Milliseconds on the PC's steady clock, as every time in this file.
    double toMs(std::chrono::steady_clock::time_point time);
    std::chrono::steady_clock::time_point fromMs(double ms);
    double nowMs();

    // The display the window presents on (window thread); false until one is published.
    void publish(const Grid &grid);
    bool published(Grid &out);
    // The display is not known any more (a real display whose reports stopped): nothing is published.
    void withdraw();

    // Where between two hand-over deadlines of a real fixed-rate display a picture is handed over: this part of a
    // refresh before its own deadline, so a fifth of a refresh after the deadline before. After a hand-over the
    // graphics card and the system can only add time, never take any away, so the picture gets as much room as
    // the clock allows; the fifth is twice what the lead may be off (a tenth of a refresh, see leadMs).
    // Measured 2026-11-14 (240 Hz display, normal window, light battle): handed over in the middle between two
    // deadlines, 17 % of the in-between pictures were shown one refresh late.
    constexpr double kAimBeforeDeadline = 0.8;

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
        bool givenUp = false;   // it was never drawn (issue 049 v3.7: the pictures' device gave it up); then not shown
    };
    void report(const Outcome &outcome);
    void takeOutcomes(std::vector<Outcome> &out);

    // DW5XL-DEBUG PS2X_SMOOTH_TIMELINE=1: the time-line log lines the 049 test reads ([smooth:list], [smooth:shown],
    // [smooth:game], [smooth:rate], [smooth:missed]).
    bool logTimeline();
}
