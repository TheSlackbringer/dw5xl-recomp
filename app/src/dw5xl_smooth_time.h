// Issue 049 Part 2, amendment v3.5 (approved 2026-11-10): the time line of the in-between pictures.
// "Every picture on screen is made for the instant it is seen": when a game frame's list arrives, this says which
// pictures to make for the stretch between the frame before and this one: for which instant on screen, and at which
// t between the two frames. Pure code: no game, no window, no clock of its own (all times are passed in, in
// milliseconds on the PC's steady clock).
//
//  - A frame's instant is the start of the field in which its list is sent (the PS2 field clock).
//  - The screen shows game time a fixed stretch behind: one game frame (blending needs the newer frame) plus the
//    head start (the time this PC needs from a list's arrival until its first picture can be on screen).
//  - Fixed-rate display: one picture per refresh, or per `step`-th refresh, each made for that refresh's instant.
//  - A display that follows the window: `perFrame` evenly spaced pictures per game frame; the last of them is
//    exactly the newer frame (t = 1), which is the game's own drawing.
//  - A single missed field: nothing is made for it (the last picture stays). Frames steadily longer than one
//    field: the pictures are spread over the length the last frames had. A frame that comes sooner than the line
//    expected: the line moves on to it (pictures still queued for later instants are void).
#pragma once

#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace dw5xl::smooth
{
    // What the display's clock says.
    struct Display
    {
        bool follows = false; // it shows a picture when it is presented (variable refresh in effect, or VSYNC OFF)
        double period = 0.0;  // fixed rate: the time between two refreshes. Follows: the shortest time between two
                              // pictures (its top rate); 0 = not known.
        double origin = 0.0;  // fixed rate: the instant of refresh 0; refresh n is on screen at origin + n x period
    };

    // One picture to make.
    struct Picture
    {
        double at = 0.0;      // the instant it is on screen
        int64_t refresh = -1; // fixed rate: the refresh it is made for; follows: -1
        uint32_t frame = 0;   // it lies between game frames frame-1 and frame ...
        double t = 0.0;       // ... at t, 0 < t <= 1
        bool own = false;     // t is exactly 1: the game's own drawing of `frame`
    };

    class TimeLine
    {
      public:
        // The head start moves by at most this share of elapsed time (v3.5, change 4): motion runs between 0.995
        // and 1.005 of its speed while it adjusts.
        static constexpr double kHeadStartRate = 0.005;

        void reset();
        // The head start wanted (measured: HeadStartMeter). The line moves towards it at kHeadStartRate; the first
        // frame after a reset takes it at once, and so does every frame while `atOnce` is set (the first
        // measurements of a session). It may be below zero (v3.6): pictures ready before their frame's field begins.
        void setHeadStart(double wanted, bool atOnce = false)
        {
            m_wanted = wanted;
            if (atOnce)
                m_haveHead = false;
        }
        double headStart() const { return m_headStart; }
        // Fixed rate: a new picture every `step`-th refresh. Follows: `count` pictures per game frame.
        void setStep(int step) { m_step = step < 1 ? 1 : step; }
        void setPerFrame(int count) { m_perFrame = count < 1 ? 1 : count; }
        int step() const { return m_step; }
        int perFrame() const { return m_perFrame; }

        // Game frame `number`: its list was sent in the field that started at `instant`; `field` is the length of a
        // PS2 field. Appends to `out` the pictures for the stretch between the frame before and this one (none for
        // the first frame after a reset, or when the numbers do not follow one another).
        // Returns the instant from which pictures made for earlier frames are void because the line moved on, or a
        // negative value when none are.
        double frame(uint32_t number, double instant, double field, const Display &display, std::vector<Picture> &out);

        // The stretch of screen time the last call covered (for tests and logs): (start, end].
        double shownFrom() const { return m_start; }
        double shownUntil() const { return m_end; }

      private:
        bool m_have = false;      // a frame before exists
        uint32_t m_number = 0;
        double m_instant = 0.0;
        double m_length = 0.0;    // the length of the stretch before this one (0: none)
        bool m_haveEnd = false;
        double m_start = 0.0, m_end = 0.0;
        bool m_haveHead = false;
        double m_headStart = 0.0, m_wanted = 0.0;
        int m_step = 1, m_perFrame = 1;
        bool m_haveRefresh = false;
        int64_t m_lastRefresh = 0;
        int m_lastStep = 0;
    };

    // The head start this PC needs (v3.5, change 4): measured all the time, never a number in the code. Every
    // picture says what head start would have put it on screen exactly in time; the answer is the largest of the
    // last kWindowMs.
    class HeadStartMeter
    {
      public:
        static constexpr double kWindowMs = 5000.0;
        // A picture: `used` = the head start in force when its instant was set; `spare` = how long before the
        // moment it had to be handed over it was ready (below zero: too late by that much).
        void add(double nowMs, double used, double spare);
        bool measured() const { return !m_needs.empty(); }
        // Measurements cover less than half a second so far: the line should take wanted() at once.
        bool warmingUp() const { return m_needs.empty() || m_lastMs - m_firstMs < 500.0; }
        double wanted() const { return m_needs.empty() ? 0.0 : m_needs.front().second; }

      private:
        std::deque<std::pair<double, double>> m_needs; // (time, need), needs falling: the front is the largest in the window
        double m_firstMs = 0.0, m_lastMs = 0.0;
        bool m_any = false;
    };

    // "Never slower" (v3.5, change 6): how many pictures per game frame this PC makes without holding the game up.
    // The choice is one of a few levels, from the fewest pictures (a fixed-rate display: the largest step that
    // still shows at least the game's own rate; a following display: one picture per game frame) to the most
    // (every refresh; as many per frame as the display's top rate and the caller allow).
    // The measure of harm is the game's own spare time: how long before the field in which it sends a list the
    // game had finished building it. A level fits while the game keeps its reserve. Measured 2026-11-11: with no
    // pictures the game has 13-16 ms to spare in the light battle and never comes near its reserve; pictures that
    // run a few milliseconds past the field's start do not hurt it; a single short moment can be a hiccup of the
    // PC. So only repeated trouble counts.
    //  - It starts at the fewest.
    //  - Up one level: after kWindow frames without the game held up, and only when the pictures the next level
    //    adds to a frame's largest batch fit before the next field's start in the tightest frame since this level
    //    began. (Measured 2026-11-11: a guess from the game's spare time let the rate climb to three pictures in a
    //    frame, which then cost the game fields. A level that only makes the largest batch more frequent needs
    //    no more room.)
    //  - Down one level: when the game was held up in kHeldUpMoments separate moments, or in kHeldUpFrames
    //    frames, of the last kWindow. Below the fewest: off (no pictures; the window shows the game's own).
    //    A frame with a picture that did not fit before the game's field (v3.6: PictureBudget kept it out, so
    //    nobody was harmed) counts among the kHeldUpFrames, not as a moment.
    //  - After every change the next kSettle frames are not judged (the game needs a few frames to recover).
    //  - A level that did not fit is tried again only after a wait that doubles each time.
    class RateChoice
    {
      public:
        // The numbers of the rule (frames are game frames):
        static constexpr int kWindow = 100;        // the frames looked at (2 s at 50 a second)
        static constexpr int kHeldUpMoments = 2;   // "down": the game was held up in this many separate moments of them
        static constexpr int kHeldUpFrames = 10;   // ... or in this many of them
        static constexpr int kSettle = 25;         // frames not judged after a change
        static constexpr int kFirstWait = 1000;    // a level that did not fit waits 1000 frames (20 s), then twice
        static constexpr int kLongestWait = 16000; // that each time, at most 16000 (about five minutes)

        // One game frame as measured.
        struct Frame
        {
            bool heldUp = false; // the game finished its next frame with less than its reserve to spare, or missed its field
            bool noRoom = false; // a picture the level asked for was not started: it did not fit before the game's field
            double spare = 0.0;  // the game's spare time in that frame
            double reserve = 0.0; // the spare time the game is to keep
            double room = 0.0;   // how long before the next field began the frame's last picture left the device
                                 // (below zero: after it); a frame without pictures: the length of a field
            double cost = 0.0;   // the device time of one picture (0: not known)
        };

        // The display and the game: a change starts the choice afresh. `mostPerFrame`: the caller's own limit.
        void configure(bool follows, double displayPeriod, double fieldPeriod, int mostPerFrame);
        void frame(const Frame &frame);

        bool on() const { return m_level >= 0; }
        int step() const;     // fixed rate: a new picture every step-th refresh
        int perFrame() const; // following display: pictures per game frame
        int level() const { return m_level; }
        int levels() const { return m_levels; }
        double picturesPerFrame(int level) const;
        // The most pictures one frame gets at a level (a frame's share of the refreshes is not a whole number).
        int largestBatch(int level) const;

      private:
        void changed();
        bool m_configured = false, m_follows = false;
        double m_displayPeriod = 0.0, m_fieldPeriod = 0.0;
        int m_most = 1, m_levels = 1, m_level = 0;
        std::deque<Frame> m_frames;      // since the last change, newest last, at most kWindow
        int m_settle = 0;                // frames still not judged after a change
        double m_leastRoom = 1e18;       // the tightest frame since this level began
        double m_leastSpare = 1e18;      // the game's least spare time since this level began
        int64_t m_count = 0;             // frames seen
        std::vector<int64_t> m_notBefore; // per level: not tried again before this frame count
        std::vector<int> m_wait;         // per level: the wait its next failure gets
    };

    // Amendment v3.6, change 3: a frame's pictures are drawn before the field in which the game sends its own list
    // ("never slower"). RateChoice finds the rate that fits by what happened; this keeps a single frame from
    // holding the game up before that is known. (Measured 2026-11-11 without it: at 4x the game lost about 110
    // fields in the seven seconds before the rate choice reacted.)
    //  - A picture is started only when it will have left the device in time, by the longest a picture took in
    //    the last kWindowMs (from the moment the device could start it until it had left the device). A frame's
    //    first picture is measured apart from those after it: it may have to wait for the device, they do not.
    //  - "In time" is the start of the game's field plus what the game can spare: its own list may wait as long
    //    as the game still finishes its next frame with its reserve to spare, by the longest the game took lately
    //    from its field's start to the end of its next frame. Not known, or a heavy moment: nothing may wait.
    //  - While a picture's cost is not known, one picture is let through to learn it, and no other until that one
    //    is reported (or kProbeMs have passed).
    class PictureBudget
    {
      public:
        static constexpr double kWindowMs = 5000.0;
        static constexpr double kProbeMs = 500.0;
        // A picture: how long it took from the moment the device could start it (it was queued, or the picture
        // before it had left) until it had left the device. `first`: it was its frame's first picture.
        void measured(double nowMs, double pictureMs, bool first = true);
        // The game: how long it took from the moment the device was free for it at its field (the field's start,
        // or later when pictures ran past it) to the end of its next frame.
        void gameTook(double nowMs, double ms);
        bool known() const { return !m_costs.empty(); }
        double cost() const { return m_costs.empty() ? 0.0 : m_costs.front().second; }               // a frame's first picture
        double nextCost() const { return m_next.empty() ? cost() : m_next.front().second; }         // one after it
        // A frame's pictures may use the device from now on; the game's field begins at `instantMs`, a field is
        // `fieldMs` long and the game is to keep `reserveMs` to spare.
        void frame(double nowMs, double instantMs, double fieldMs, double reserveMs);
        // How long the game's own list may wait in this frame (0: not at all).
        double allowance() const { return m_allowance; }
        // One more picture of this frame: true when it fits, and it then counts as started.
        bool admit(double nowMs);

      private:
        using Window = std::deque<std::pair<double, double>>; // (time, value), values falling: the front is the largest
        static void add(Window &window, double nowMs, double value);
        static void prune(Window &window, double nowMs);
        Window m_costs, m_next, m_game;
        double m_latest = 0.0, m_busyUntil = 0.0, m_allowance = 0.0;
        int m_admitted = 0;
        bool m_probing = false;
        double m_probeUntil = 0.0;
    };

    // Amendment v3.6 (approved 2026-11-10): in-between pictures are made when the game has finished a frame, one
    // field before it sends the list. The field in which it will send it is the next one of the PS2 field clock,
    // and that field's start is the frame's instant: known in advance. This numbers the game's frames (one number
    // per list sent as a new frame), keeps the frame that is finished and not sent yet, and says at the send
    // whether what was known in advance held.
    class FinishedFrames
    {
      public:
        struct Coming
        {
            uint32_t number = 0; // the frame's number
            uint64_t field = 0;  // the field in which the game will send its list ...
            double instant = 0.0; // ... and that field's start: the frame's instant
            bool again = false;  // this list was reported finished before and is not sent yet: nothing new
        };
        // The game finished a frame: its list starts at `list` (`hash`: of the list as taken, 0 = not taken). The
        // field clock: field `lastField` began at `lastStart`, a field is `period` long. The list is sent at the
        // next field boundary, also when that boundary has just passed and its field has not been announced yet.
        Coming finished(uint32_t list, uint64_t hash, uint64_t lastField, double lastStart, double period);

        struct Sent
        {
            uint32_t number = 0;   // the frame's number (the one given when it was finished, or a new one)
            bool finished = false; // it was reported finished before
            bool sameField = false; // ... and is sent in the field that was known in advance
            bool sameList = false; // ... and its list is the one taken then (true when either hash is 0)
            double instant = 0.0;  // the instant that was known in advance (finished only)
        };
        // The game sends the list at `list` as a new frame, in field `field`.
        Sent sent(uint32_t list, uint64_t hash, uint64_t field);

        void reset() { *this = FinishedFrames(); }
        uint32_t frames() const { return m_count; }

      private:
        uint32_t m_count = 0;
        bool m_waiting = false;
        uint32_t m_list = 0;
        uint64_t m_hash = 0;
        Coming m_coming;
    };
}
