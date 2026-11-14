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
} // namespace ps2x::screen_on
