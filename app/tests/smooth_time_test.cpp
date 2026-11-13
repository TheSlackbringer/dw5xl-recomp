// Issue 049 Part 2, amendment v3.5: the time line of the in-between pictures (app/src/dw5xl_smooth_time.h).
// No game data and no clock: frame instants and display refreshes are made up here.
//  - fixed-rate displays (60, 120, 144, 240 Hz): every refresh of the step gets exactly one picture, made for that
//    refresh; the screen shows game time one frame plus the head start behind, on one straight line;
//  - a refresh exactly on a game frame is the game's own drawing (t = 1);
//  - a display that follows the window: an even cadence of our own, not faster than its top rate;
//  - a missed field, frames steadily two fields long, and the way back;
//  - the head start moves by at most 0.5 % of elapsed time.
#include "dw5xl_smooth_time.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace dw5xl::smooth;

namespace
{
    int failures = 0;
    void check(bool ok, const std::string &what)
    {
        std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
        failures += ok ? 0 : 1;
    }

    constexpr double kField = 20.0;

    struct Run
    {
        std::map<uint32_t, double> instants; // frame -> instant
        std::vector<Picture> pictures;
        std::vector<double> voidFrom;        // per frame() call
        double content(const Picture &p) const
        {
            const double before = instants.at(p.frame - 1u), now = instants.at(p.frame);
            return before + p.t * (now - before);
        }
    };

    // Frames 0..count-1; `fieldsOf(n)` = how many fields frame n came after frame n-1.
    template <typename Fields>
    Run play(TimeLine &line, const Display &display, uint32_t count, Fields fieldsOf, double first = 1000.0)
    {
        Run run;
        double instant = first;
        for (uint32_t n = 0; n < count; ++n)
        {
            if (n > 0)
                instant += kField * fieldsOf(n);
            run.instants[n] = instant;
            run.voidFrom.push_back(line.frame(n, instant, kField, display, run.pictures));
        }
        return run;
    }

    struct Line { double delayLow = 1e9, delayHigh = -1e9, stepError = 0.0; bool tOk = true; };
    Line measure(const Run &run, size_t from = 0)
    {
        Line m;
        for (size_t i = from; i < run.pictures.size(); ++i)
        {
            const Picture &p = run.pictures[i];
            const double delay = p.at - run.content(p);
            m.delayLow = std::min(m.delayLow, delay);
            m.delayHigh = std::max(m.delayHigh, delay);
            m.tOk = m.tOk && p.t > 0.0 && p.t <= 1.0;
            if (i > from)
            {
                const Picture &q = run.pictures[i - 1];
                m.stepError = std::max(m.stepError, std::fabs((run.content(p) - run.content(q)) - (p.at - q.at)));
            }
        }
        return m;
    }
}

int main()
{
    const auto oneField = [](uint32_t) { return 1; };

    // Fixed-rate displays, the game at 50 frames a second, a head start of 5 ms.
    for (const double hz : {60.0, 120.0, 144.0, 240.0})
    {
        TimeLine line;
        line.setHeadStart(5.0);
        Display display;
        display.period = 1000.0 / hz;
        display.origin = 3.0;
        const Run run = play(line, display, 150, oneField);
        bool everyRefresh = !run.pictures.empty();
        for (size_t i = 1; i < run.pictures.size(); ++i)
            everyRefresh = everyRefresh && run.pictures[i].refresh == run.pictures[i - 1].refresh + 1;
        bool onRefresh = true;
        for (const Picture &p : run.pictures)
            onRefresh = onRefresh && std::fabs(p.at - (display.origin + static_cast<double>(p.refresh) * display.period)) < 1e-9;
        const Line m = measure(run);
        const double expected = 149.0 * kField / display.period;
        char name[160];
        std::snprintf(name, sizeof(name), "%.0f Hz: every refresh gets exactly one picture, made for its instant (%zu pictures)", hz,
                      run.pictures.size());
        check(everyRefresh && onRefresh && std::fabs(static_cast<double>(run.pictures.size()) - expected) <= 1.0, name);
        std::snprintf(name, sizeof(name), "%.0f Hz: the screen is one frame plus the head start behind (25 ms), on a straight line", hz);
        check(m.tOk && std::fabs(m.delayLow - 25.0) < 1e-6 && std::fabs(m.delayHigh - 25.0) < 1e-6 && m.stepError < 1e-6, name);
    }

    // Every second refresh of a 240 Hz display: evenly spaced, the same refreshes throughout.
    {
        TimeLine line;
        line.setHeadStart(5.0);
        line.setStep(2);
        Display display;
        display.period = 1000.0 / 240.0;
        const Run run = play(line, display, 150, oneField);
        bool even = run.pictures.size() > 300;
        for (size_t i = 1; i < run.pictures.size(); ++i)
            even = even && run.pictures[i].refresh == run.pictures[i - 1].refresh + 2;
        const Line m = measure(run);
        check(even && std::fabs(m.delayLow - 25.0) < 1e-6 && std::fabs(m.delayHigh - 25.0) < 1e-6,
              "240 Hz, every second refresh: 120 evenly spaced pictures a second on the same line");
    }

    // A refresh exactly on a game frame: the game's own drawing.
    {
        TimeLine line;
        line.setHeadStart(5.0);
        Display display;
        display.period = 10.0;
        display.origin = 1005.0; // refreshes at frame instant + 5, + 15, + 25 ...
        const Run run = play(line, display, 50, oneField);
        size_t own = 0, half = 0;
        for (const Picture &p : run.pictures)
        {
            own += p.own && p.t == 1.0 ? 1u : 0u;
            half += !p.own && std::fabs(p.t - 0.5) < 1e-9 ? 1u : 0u;
        }
        check(run.pictures.size() == 98u && own == 49u && half == 49u,
              "100 Hz in step with the game: per frame one picture half way and one that is the game's own drawing");
    }

    // A display that follows the window.
    {
        TimeLine line;
        line.setHeadStart(5.0);
        line.setPerFrame(2);
        Display display;
        display.follows = true;
        display.period = 1000.0 / 240.0;
        const Run run = play(line, display, 50, oneField);
        bool cadence = run.pictures.size() == 98u;
        for (size_t i = 1; i < run.pictures.size(); ++i)
            cadence = cadence && std::fabs(run.pictures[i].at - run.pictures[i - 1].at - 10.0) < 1e-6;
        size_t own = 0;
        for (const Picture &p : run.pictures)
            own += p.own ? 1u : 0u;
        const Line m = measure(run);
        check(cadence && own == 49u && run.pictures.front().refresh == -1 && std::fabs(m.delayLow - 25.0) < 1e-6 &&
                  std::fabs(m.delayHigh - 25.0) < 1e-6,
              "a following display, 2 per frame: pictures 10 ms apart, every second one the game's own drawing");

        TimeLine capped;
        capped.setPerFrame(3);
        display.period = 8.0; // a top rate of 125 Hz: three pictures in 20 ms do not fit
        const Run slow = play(capped, display, 20, oneField);
        check(slow.pictures.size() == 38u, "a following display is not asked for more pictures than its top rate shows");
    }

    // The game misses one field (frame 10 comes after two fields).
    {
        TimeLine line;
        line.setHeadStart(5.0);
        Display display;
        display.period = 1000.0 / 60.0;
        const Run run = play(line, display, 40, [](uint32_t n) { return n == 10 ? 2 : 1; });
        const double gapFrom = run.instants.at(9) + 25.0, gapTo = run.instants.at(10) + 5.0;
        bool gapEmpty = true, pattern = true;
        for (size_t i = 0; i < run.pictures.size(); ++i)
        {
            const Picture &p = run.pictures[i];
            gapEmpty = gapEmpty && !(p.at > gapFrom + 1e-6 && p.at <= gapTo + 1e-6);
            if (i > 0)
                pattern = pattern && p.refresh > run.pictures[i - 1].refresh;
        }
        size_t after = 0;
        while (after < run.pictures.size() && run.pictures[after].frame < 11u)
            ++after;
        const Line m = measure(run, after);
        bool noVoid = true;
        for (double v : run.voidFrom)
            noVoid = noVoid && v < 0.0;
        check(gapEmpty && pattern && noVoid, "a missed field: nothing is made for it (the last picture stays)");
        check(std::fabs(m.delayLow - 25.0) < 1e-6 && std::fabs(m.delayHigh - 25.0) < 1e-6 && m.stepError < 1e-6,
              "after the missed field the line goes on as before");
    }

    // The game runs steadily at two fields per frame from frame 10, and at one again from frame 30.
    {
        TimeLine line;
        line.setHeadStart(5.0);
        Display display;
        display.period = 1000.0 / 60.0;
        const Run run = play(line, display, 60, [](uint32_t n) { return n >= 10 && n < 30 ? 2 : 1; });
        size_t from = 0, to = 0;
        for (size_t i = 0; i < run.pictures.size(); ++i)
        {
            if (run.pictures[i].frame < 12u)
                from = i + 1;
            if (run.pictures[i].frame < 30u)
                to = i + 1;
        }
        bool everyRefresh = to > from + 20;
        double low = 1e9, high = -1e9;
        for (size_t i = from; i < to; ++i)
        {
            if (i > from)
                everyRefresh = everyRefresh && run.pictures[i].refresh == run.pictures[i - 1].refresh + 1;
            const double delay = run.pictures[i].at - run.content(run.pictures[i]);
            low = std::min(low, delay);
            high = std::max(high, delay);
        }
        check(everyRefresh && std::fabs(low - 45.0) < 1e-6 && std::fabs(high - 45.0) < 1e-6,
              "frames steadily two fields long: their pictures are spread over two fields, every refresh used");
        size_t moved = 0;
        for (uint32_t n = 0; n < 60; ++n)
            moved += run.voidFrom[n] >= 0.0 ? 1u : 0u;
        check(moved == 1u && std::fabs(run.voidFrom[30] - (run.instants.at(30) + 5.0)) < 1e-9,
              "the first frame that comes sooner again: the line moves on to it (earlier pictures are void from there)");
        size_t tail = 0;
        while (tail < run.pictures.size() && run.pictures[tail].frame < 32u)
            ++tail;
        const Line m = measure(run, tail);
        check(std::fabs(m.delayLow - 25.0) < 1e-6 && std::fabs(m.delayHigh - 25.0) < 1e-6 && m.stepError < 1e-6,
              "and from then on the line is one field plus the head start behind again");
    }

    // The head start changes: slowly.
    {
        TimeLine line;
        line.setHeadStart(5.0);
        Display display;
        display.period = 1000.0 / 240.0;
        Run run;
        double instant = 1000.0, largest = 0.0, last = 5.0;
        for (uint32_t n = 0; n < 120; ++n)
        {
            if (n == 50)
                line.setHeadStart(8.0);
            if (n > 0)
                instant += kField;
            run.instants[n] = instant;
            line.frame(n, instant, kField, display, run.pictures);
            if (n > 0)
            {
                largest = std::max(largest, std::fabs(line.headStart() - last));
                last = line.headStart();
            }
        }
        const Line m = measure(run);
        check(std::fabs(line.headStart() - 8.0) < 1e-9 && largest <= TimeLine::kHeadStartRate * kField + 1e-9,
              "the head start moves by at most 0.5 % of elapsed time (0.1 ms per 20 ms frame)");
        bool everyRefresh = true;
        for (size_t i = 1; i < run.pictures.size(); ++i)
            everyRefresh = everyRefresh && run.pictures[i].refresh == run.pictures[i - 1].refresh + 1;
        check(everyRefresh && m.stepError < 0.2 && std::fabs(m.delayLow - 25.0) < 1e-6 && std::fabs(m.delayHigh - 28.0) < 1e-6,
              "while it moves every refresh still gets its picture and the line bends by less than 0.2 ms per step");
    }

    // A break in the frames: the line starts again.
    {
        TimeLine line;
        line.setHeadStart(5.0);
        Display display;
        display.period = 1000.0 / 60.0;
        std::vector<Picture> pictures;
        line.frame(0, 1000.0, kField, display, pictures);
        line.frame(1, 1020.0, kField, display, pictures);
        const size_t before = pictures.size();
        const double moved = line.frame(5, 1100.0, kField, display, pictures);
        const size_t atBreak = pictures.size();
        line.frame(6, 1120.0, kField, display, pictures);
        check(before > 0u && atBreak == before && moved < 0.0 && pictures.size() > atBreak && pictures.back().frame == 6u,
              "frames that do not follow one another: no picture across the break, the line starts again");
    }

    // A head start below zero (amendment v3.6): pictures ready before their frame's field begins.
    {
        TimeLine line;
        line.setHeadStart(-5.0);
        Display display;
        display.period = 1000.0 / 60.0;
        const Run run = play(line, display, 100, oneField);
        const Line m = measure(run);
        bool everyRefresh = !run.pictures.empty();
        for (size_t i = 1; i < run.pictures.size(); ++i)
            everyRefresh = everyRefresh && run.pictures[i].refresh == run.pictures[i - 1].refresh + 1;
        check(everyRefresh && std::fabs(m.delayLow - 15.0) < 1e-6 && std::fabs(m.delayHigh - 15.0) < 1e-6 && m.stepError < 1e-6,
              "a head start of -5 ms: the screen is 15 ms behind the game, less than one game frame");

        TimeLine atOnce;
        atOnce.setHeadStart(2.0);
        std::vector<Picture> pictures;
        atOnce.frame(0, 1000.0, kField, display, pictures);
        atOnce.frame(1, 1020.0, kField, display, pictures);
        atOnce.setHeadStart(12.0, true);
        atOnce.frame(2, 1040.0, kField, display, pictures);
        check(std::fabs(atOnce.headStart() - 12.0) < 1e-9, "the first measurements of a session are taken at once");
    }

    // The head start this PC needs (amendment v3.8, change 2, approved 2026-11-12): what pictures needed in two
    // separate moments, or in ten frames, of the last five seconds. What only one moment needed is not followed.
    // (Until v3.8: the largest need of the last five seconds. The checks of that rule are replaced by these.)
    {
        HeadStartMeter meter;
        check(!meter.measured() && meter.warmingUp() && meter.wanted() == 0.0, "no picture yet: nothing measured");
        meter.add(1000.0, 1u, 8.0, -6.0);  // too late by 6 ms with a head start of 8: needs 14
        check(meter.measured() && meter.warmingUp() && std::fabs(meter.wanted() - 14.0) < 1e-9,
              "the first picture was 6 ms late with a head start of 8 ms: it asks for 14 ms, and nothing else is known yet");
        meter.add(1020.0, 2u, 14.0, 2.0);  // the frame right after it: needs 12
        check(meter.warmingUp() && std::fabs(meter.wanted() - 14.0) < 1e-9,
              "frames in a row are one moment: while nothing was needed twice, the largest need counts");
        meter.add(1600.0, 3u, 14.0, 1.0);
        check(!meter.warmingUp(), "after half a second of measurements the warm-up is over");
    }
    {
        // One picture per game frame, a frame every 20 ms. The steady case: every fifth frame's picture falls just
        // after its frame's start and needs 3 ms, the others 1 ms.
        struct Feed
        {
            HeadStartMeter meter;
            double ms = 1000.0;
            uint32_t frame = 0;
            void picture(double need)
            {
                meter.add(ms, frame, need + 1.0, 1.0);
            }
            void frames(int count, double need = -1.0)
            {
                for (int i = 0; i < count; ++i)
                {
                    ++frame;
                    ms += kField;
                    picture(need >= 0.0 ? need : frame % 5u == 0u ? 3.0 : 1.0);
                }
            }
            bool wants(double head) const { return std::fabs(meter.wanted() - head) < 1e-9; }
        };
        Feed a;
        a.frames(100);
        check(a.wants(3.0), "what pictures need again and again is followed (every fifth frame needs 3 ms: 3 ms)");
        a.frames(3, 8.0);                  // one slow moment, three frames long: 8 ms
        const bool atOnce = a.wants(3.0);
        a.frames(22);
        check(atOnce && a.wants(3.0), "one slow moment does not move the head start (three frames needed 8 ms: still 3 ms)");
        a.frames(1, 7.0);                  // a second moment, half a second later: 7 ms
        check(a.wants(7.0), "what two separate moments needed is followed (8 ms, then 7 ms: 7 ms)");
        a.frames(230);                     // 4.6 s on: the first moment is older than five seconds, the second is not
        const bool oneLeft = a.wants(3.0);
        a.frames(40);
        check(oneLeft && a.wants(3.0), "when a moment is older than five seconds the head start comes back (3 ms again)");

        Feed b;
        b.frames(100);
        b.frames(9, 6.0);                  // the scene got heavier: frame after frame needs 6 ms
        const bool nine = b.wants(3.0);
        b.frames(1, 6.0);
        check(nine && b.wants(6.0), "ten frames that needed more are followed, also in one moment (nine: not yet; the tenth: 6 ms)");
        b.frames(260);
        check(b.wants(3.0), "and five seconds after the heavier stretch the head start is back at 3 ms");

        // A frame's need is the largest of its pictures'.
        Feed c;
        c.frames(1, 2.0);
        c.picture(5.0);                    // a second picture of the same frame needed 5 ms
        c.frames(3, 1.0);
        c.frames(1, 4.0);
        c.picture(1.0);
        check(c.wants(4.0), "a frame with several pictures counts once, with the largest need (5 ms, later 4 ms: 4 ms)");
    }

    // "Never slower": the rate choice.
    {
        RateChoice a, b, c, d, e, f;
        a.configure(false, 1000.0 / 240.0, kField, 5);
        b.configure(false, 1000.0 / 60.0, kField, 5);
        c.configure(false, 1000.0 / 120.0, kField, 3);
        d.configure(false, 1000.0 / 120.0, kField, 2);
        e.configure(true, 1000.0 / 240.0, kField, 3);
        f.configure(true, 8.0, kField, 3);
        check(a.levels() == 4 && a.step() == 4 && b.levels() == 1 && b.step() == 1 && c.levels() == 2 && c.step() == 2 &&
                  d.levels() == 1 && d.step() == 2 && e.levels() == 3 && e.perFrame() == 1 && f.levels() == 2,
              "the levels: never fewer pictures than game frames, never more per frame than the caller or the display allows");
        check(std::fabs(a.picturesPerFrame(0) - 1.2) < 1e-9 && std::fabs(a.picturesPerFrame(3) - 4.8) < 1e-9 &&
                  std::fabs(e.picturesPerFrame(2) - 3.0) < 1e-9,
              "pictures per game frame at each level (240 Hz: 1.2 at every fourth refresh, 4.8 at every refresh)");
    }
    {
        // A PC on a 240 Hz display: a frame's pictures leave the device 12 ms before the next field less 4.4 ms each:
        // two fit; three run 1.2 ms past the field's start and take the game's spare time below its reserve.
        const auto run = [](double reported, int frames, int &heldUpFrames, int &attempts, int &lastStep) {
            RateChoice choice;
            choice.configure(false, 1000.0 / 240.0, kField, 5);
            heldUpFrames = attempts = 0;
            int before = choice.level();
            for (int i = 0; i < frames; ++i)
            {
                RateChoice::Frame frame;
                frame.room = 12.0 - 4.4 * choice.largestBatch(choice.level());
                frame.spare = frame.room < 0.0 ? 2.0 : 14.0;
                frame.reserve = 5.0;
                frame.heldUp = frame.spare < frame.reserve;
                frame.cost = reported;
                heldUpFrames += frame.heldUp ? 1 : 0;
                choice.frame(frame);
                if (choice.level() == 2 && before != 2)
                    ++attempts;
                before = choice.level();
            }
            lastStep = choice.step();
        };
        int heldUp = 0, attempts = 0, step = 0;
        run(4.4, 3000, heldUp, attempts, step);
        check(step == 3 && heldUp == 0 && attempts == 0,
              "it goes up to every third refresh (1.6 pictures a frame, two at most) and stops: a third in a frame would not fit");
        run(1.0, 20000, heldUp, attempts, step);
        check(step == 3 && attempts >= 2 && attempts <= 6 && heldUp == attempts * (RateChoice::kSettle + RateChoice::kHeldUpFrames),
              "a level that holds the game up is left after ten frames and tried again only after a wait that doubles");
    }
    {
        // The game is held up: one moment may be a hiccup of the PC and changes nothing; a second one within the
        // window does. The frames right after a change are not judged.
        RateChoice choice;
        choice.configure(true, 0.0, kField, 3);
        RateChoice::Frame fine;
        fine.room = 3.0; // no room for a second picture: it stays at one per frame
        fine.spare = 8.0;
        fine.reserve = 5.0;
        fine.cost = 4.4;
        RateChoice::Frame hurt = fine;
        hurt.heldUp = true;
        hurt.spare = 1.0;
        for (int i = 0; i < 30; ++i)
            choice.frame(fine);
        for (int i = 0; i < 4; ++i)
            choice.frame(hurt); // one moment, four frames long
        for (int i = 0; i < 10; ++i)
            choice.frame(fine);
        const bool stillOn = choice.on();
        choice.frame(hurt); // the second moment
        const bool off = !choice.on();
        int onAgain = -1;
        for (int i = 0; i < 1200 && onAgain < 0; ++i)
        {
            choice.frame(hurt); // while it is off nothing is judged
            if (choice.on())
                onAgain = i;
        }
        check(stillOn && off && onAgain >= 990 && onAgain <= 1000,
              "held up once: nothing changes; a second moment at the fewest pictures: off, and on again after the wait");
        for (int i = 0; i < RateChoice::kSettle; ++i)
            choice.frame(hurt);
        const bool settled = choice.on();
        choice.frame(hurt);
        choice.frame(fine);
        const bool oneMoment = choice.on();
        choice.frame(hurt);
        check(settled && oneMoment && !choice.on(), "the frames right after a change are not judged; those after them are");
    }
    {
        // A following display with room for two more pictures a frame: up one level at a time.
        RateChoice choice;
        choice.configure(true, 1000.0 / 240.0, kField, 4);
        int ups = 0, before = choice.level();
        for (int i = 0; i < 2000; ++i)
        {
            RateChoice::Frame frame;
            frame.room = 14.0 - 4.4 * choice.perFrame();
            frame.spare = 12.0;
            frame.reserve = 5.0;
            frame.heldUp = frame.spare < frame.reserve;
            frame.cost = 4.4;
            choice.frame(frame);
            ups += choice.level() > before ? 1 : 0;
            before = choice.level();
        }
        const int perFrame = choice.perFrame();
        choice.configure(true, 1000.0 / 240.0, kField, 4);
        const bool kept = choice.perFrame() == perFrame;
        choice.configure(false, 1000.0 / 60.0, kField, 3);
        check(perFrame == 3 && ups == 2 && kept && choice.on() && choice.step() == 1,
              "a following display: up to three pictures a frame, where a fourth would not fit; the same display keeps the "
              "choice, another starts afresh");
    }

    // Amendment v3.6: a frame's instant is known when the game has finished the frame, one field before the send.
    {
        FinishedFrames frames;
        // Field 50 began at 1000 ms; the game finishes a frame 6 ms into it: sent in field 51, which starts at 1020.
        const FinishedFrames::Coming a = frames.finished(0x00370000u, 0x1111u, 50u, 1000.0, kField);
        const FinishedFrames::Sent sa = frames.sent(0x00370000u, 0x1111u, 51u);
        check(a.number == 1u && a.field == 51u && std::fabs(a.instant - 1020.0) < 1e-9 && !a.again && sa.number == 1u &&
              sa.finished && sa.sameField && sa.sameList && std::fabs(sa.instant - 1020.0) < 1e-9,
              "a finished frame: its instant is the start of the next field, and the send finds it there");

        // The next frame is built in the other list. The boundary has just passed and field 52 is not announced
        // yet: the clock still says field 51, and the list is sent in field 52 all the same.
        const FinishedFrames::Coming b = frames.finished(0x003E0000u, 0x2222u, 51u, 1020.0, kField);
        const FinishedFrames::Coming again = frames.finished(0x003E0000u, 0x2222u, 51u, 1020.0, kField);
        const FinishedFrames::Sent sb = frames.sent(0x003E0000u, 0x2222u, 52u);
        check(b.number == 2u && b.field == 52u && std::fabs(b.instant - 1040.0) < 1e-9 && again.again && again.number == 2u &&
              sb.number == 2u && sb.sameField && frames.frames() == 2u,
              "the same list reported finished twice is one frame");

        // What was known in advance did not hold: the list was sent a field later, or it is not the list taken.
        frames.finished(0x00370000u, 0x3333u, 52u, 1040.0, kField);
        const FinishedFrames::Sent late = frames.sent(0x00370000u, 0x3333u, 54u);
        frames.finished(0x003E0000u, 0x4444u, 54u, 1080.0, kField);
        const FinishedFrames::Sent changed = frames.sent(0x003E0000u, 0x4445u, 55u);
        check(late.number == 3u && late.finished && !late.sameField && late.sameList && changed.number == 4u &&
              changed.sameField && !changed.sameList,
              "a list sent in another field than expected, or changed after it was taken, is told at the send");

        // A list sent without a frame end (another scene's kind of frame) gets a number of its own; a frame that
        // was finished and never sent leaves no gap in what follows.
        const FinishedFrames::Sent other = frames.sent(0x00370000u, 0u, 56u);
        const FinishedFrames::Coming lost = frames.finished(0x003E0000u, 0u, 56u, 1120.0, kField);
        const FinishedFrames::Coming next = frames.finished(0x00370000u, 0u, 57u, 1140.0, kField);
        const FinishedFrames::Sent sn = frames.sent(0x00370000u, 0x5555u, 58u);
        check(other.number == 5u && !other.finished && lost.number == 6u && next.number == 7u && sn.number == 7u &&
              sn.finished && sn.sameList,
              "frames are numbered in the order they are finished or sent, with or without a frame end");
    }

    // Amendment v3.8, change 3 (approved 2026-11-12): on the pictures' own device "did not fit" is a frame with a
    // picture that was cut off (still being drawn when the next frame's pictures needed the device). The rate goes
    // down one level when that happened in two separate moments, or in ten frames, of the last hundred; at the
    // fewest pictures they pause and are tried again after the wait. Nothing foretold counts.
    {
        RateChoice choice;
        choice.configure(false, 1000.0 / 240.0, kField, 5);    // a 240 Hz display: four levels
        RateChoice::Frame fine;
        fine.room = 12.0;
        fine.spare = 14.0;
        fine.reserve = 5.0;
        fine.cost = 1.0;
        RateChoice::Frame cut = fine;
        cut.cutOff = true;
        const auto feed = [&](const RateChoice::Frame &frame, int count) {
            for (int i = 0; i < count; ++i)
                choice.frame(frame);
        };
        for (int i = 0; i < 1000 && choice.level() < 2; ++i)
            choice.frame(fine);
        const bool atTwo = choice.level() == 2;
        feed(fine, 30);
        feed(cut, 3);                      // one moment, three frames long
        feed(fine, 10);
        const bool oneMoment = choice.level() == 2;
        feed(cut, 1);                      // the second moment
        check(atTwo && oneMoment && choice.level() == 1 && choice.on(),
              "pictures cut off in one moment change nothing; in a second moment the rate goes down one level, not off");
        feed(fine, 30);
        feed(cut, 9);                      // one moment, nine frames long
        const bool nine = choice.level() == 1;
        feed(cut, 1);
        check(nine && choice.level() == 0 && choice.on(),
              "pictures cut off in ten frames of a hundred: down one level (nine in one moment: not yet)");
        // At the fewest pictures: they pause, and are tried again after 1000 frames, then after twice that.
        const auto offAndBack = [&]() {
            feed(fine, 30);
            feed(cut, 1);
            feed(fine, 5);
            const bool stillOn = choice.on();
            feed(cut, 1);
            if (!stillOn || choice.on())
                return -1;
            for (int i = 0; i < 5000; ++i)
            {
                choice.frame(cut);         // while they pause nothing is judged
                if (choice.on())
                    return i;
            }
            return -1;
        };
        const int first = offAndBack(), second = offAndBack();
        check(first >= 990 && first <= 1000 && second >= 1990 && second <= 2000,
              "at the fewest pictures, cut off in two moments: the pictures pause and are tried again after the wait, then twice that");
    }
    {
        // Nothing foretold counts: frames whose last picture left the device late, with nothing cut off and the
        // game not held up, do not bring the rate down.
        RateChoice choice;
        choice.configure(false, 1000.0 / 240.0, kField, 5);
        RateChoice::Frame late;
        late.room = -3.0;
        late.spare = 14.0;
        late.reserve = 5.0;
        late.cost = 6.0;
        bool down = false;
        int before = choice.level();
        for (int i = 0; i < 600; ++i)
        {
            choice.frame(late);
            down = down || choice.level() < before;
            before = choice.level();
        }
        check(!down && choice.on(), "pictures that ran to their end, however late, do not bring the rate down");
    }

    // (Amendment v3.7, change 4, the forecast for the pictures' own device: replaced by amendment v3.8, change 1,
    // approved 2026-11-12. Its functions and their checks are gone: nothing is foretold there.)

    {
        // Amendment v3.7 (P2.6a): a break in the game's own clock between two frames, read from the PS2 field clock:
        // the field's number and its start no longer move together. The cases are the ones measured on 2026-11-11
        // (a quick save, the MENU, a state load back, the first frame after a state load ahead).
        check(!fieldClockBroke(3599u, 1000.0, 3600u, 1020.0, kField), "frames one field apart: the field clock ran evenly");
        check(!fieldClockBroke(3599u, 1000.0, 3600u, 1020.4, kField) && !fieldClockBroke(3599u, 1000.0, 3600u, 1019.6, kField),
              "a field that starts a little late or early is no break");
        check(!fieldClockBroke(3599u, 1000.0, 3601u, 1040.0, kField) && !fieldClockBroke(3599u, 1000.0, 3604u, 1100.0, kField),
              "the game missed fields (more fields, as much more time): no break");
        check(fieldClockBroke(3600u, 1000.0, 3601u, 1095.2, kField), "a quick save: one field on, 95 ms later: a break");
        check(fieldClockBroke(3700u, 1000.0, 3701u, 4956.5, kField), "the MENU: one field on, four seconds later: a break");
        check(fieldClockBroke(3800u, 1000.0, 3601u, 1281.5, kField), "a state load: the fields go back: a break");
        check(fieldClockBroke(29u, 1000.0, 3151u, 1237.4, kField), "a state load ahead: many fields on in a quarter second: a break");
        check(fieldClockBroke(3600u, 1000.0, 3600u, 1020.0, kField), "the same field twice: a break");
        check(!fieldClockBroke(3600u, 1000.0, 3601u, 1500.0, 0.0), "without a field length nothing is judged");
    }

    std::printf("%s (%d failed)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
