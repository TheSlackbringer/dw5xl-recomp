// DW5XL (issue 067): the game asks the system to keep the screen on while it is being played.
//
// A system switches the screen off after its own time without a key or a mouse movement. A controller does not
// count, and a movie has no input at all, so a program that shows pictures has to ask. The window library asks
// only for a full-screen window that takes the display over, which the game never makes.
//
// The rule (option A, owner 2026-11-14): the screen is asked for while the game runs in a window that is shown.
// It is let go while the port's MENU or one of its pages holds the game, while the window is hidden or
// minimised, and at exit. A hidden test run therefore asks for nothing.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ps2x::screen_on
{
    // What the window loop knows of the game once a turn.
    struct State
    {
        bool paused = false;    // the port's MENU or one of its pages holds the game
        bool hidden = false;    // the window is not shown
        bool minimized = false; // the window is minimised
        bool exiting = false;   // the game is closing
    };

    enum class Why
    {
        Playing,
        Paused,
        Hidden,
        Minimized,
        Exit,
    };

    // The rule. True: the screen is to be kept on. `why` says which part of the state decided.
    bool wanted(const State &state, Why &why);
    const char *said(Why why);

    // The system's side: one request that is held or not.
    struct System
    {
        virtual ~System() = default;
        virtual bool hold() = 0;                 // true: the system took the request
        virtual void letGo() = 0;                // nothing happens when none is held
        virtual const char *refused() const = 0; // why the last hold() was not taken
    };

    // This system's way of asking. `program` names the game in the reason the system shows for the request.
    // Windows: a power request for the display and for the system (no sleep in a battle either). Other systems:
    // not built yet (the spec's step 2); hold() answers false there.
    std::unique_ptr<System> makeSystem(const std::string &program);

    // Holds the request by the rule. The system is called only when the rule's answer changes, and a refusal is
    // not asked again until then. Says each change in the log when `log` is set:
    //   [display] the screen is kept on (playing)
    //   [display] the screen may switch off (paused | hidden | minimised | exit)
    class Keeper
    {
    public:
        explicit Keeper(System &system, bool log = true);
        ~Keeper(); // lets go
        Keeper(const Keeper &) = delete;
        Keeper &operator=(const Keeper &) = delete;

        void update(const State &state);
        bool held() const { return m_held; }
        uint64_t calls() const { return m_calls; } // calls made to the system so far

    private:
        System &m_system;
        bool m_log;
        bool m_known = false; // update() has run once
        bool m_wanted = false;
        Why m_why = Why::Exit;
        bool m_held = false;
        uint64_t m_calls = 0u;
    };

    // ---- Part 2: what the system says of the screen.
    // A switch of the screen, off or on, stalls a running game once (0.12 to 0.32 s measured on the Windows test
    // PC, in a hidden window too). The game cannot prevent a switch from outside, but it can say when it was told
    // of one, so that a timing run knows it was disturbed (the go, issue 065 amendment v4.3).
    enum class Screen
    {
        Off,
        On,
        Dim,
    };
    const char *said(Screen screen);

    // The log's words for the system's reports. The first report is the screen's state when the game started (a
    // system tells it when it is asked to report changes); a later one that differs is a switch:
    //   [display] the screen at the start: on
    //   [display] the system switched the screen off (field 4081)
    // The field is the one the game was at when it was told, not the field of the stall (which comes before).
    class SwitchLog
    {
    public:
        // The line for this report (without a line end), or "" when there is nothing to say.
        std::string report(Screen screen, uint64_t field);
        uint64_t switches() const { return m_switches; }
        // What the system last said of the screen; `known` is false before its first report (issue 049 v3.13).
        bool known() const { return m_known; }
        Screen screen() const { return m_screen; }

    private:
        bool m_known = false;
        Screen m_screen = Screen::On;
        uint64_t m_switches = 0u;
    };

    // The system's reports of the screen, collected on a thread of their own.
    struct Watch
    {
        virtual ~Watch() = default;
        // Hands over the reports since the last call, oldest first (appended to `out`). Cheap when there are none.
        virtual void take(std::vector<Screen> &out) = 0;
    };
    // This system's reports; never null. Windows: the console display state (off, on, dim), asked for with a
    // window of the watch's own, which a hidden game gets too. Other systems: nothing is reported yet.
    std::unique_ptr<Watch> makeWatch();
} // namespace ps2x::screen_on
