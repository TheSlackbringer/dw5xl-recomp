// Issue 067: the game asks the system to keep the screen on while it is played (runtime/ps2_screen_on.h).
//  - the rule (option A): asked for while the game runs in a shown window; not while the port's MENU holds it,
//    not hidden, not minimised, not at exit;
//  - the system is called once per change of the rule's answer, never once per turn of the window loop, and a
//    refusal is not asked again until the answer changes;
//  - Windows: while the request is held the system's own state reads "display required", and afterwards it reads
//    what it read before the test. Read with CallNtPowerInformation, which needs no extra rights.
#include "runtime/ps2_screen_on.h"

#include <cstdint>
#include <cstdio>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <powrprof.h>
#endif

using namespace ps2x::screen_on;

namespace
{
    int failures = 0;
    void check(bool ok, const std::string &what)
    {
        std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
        failures += ok ? 0 : 1;
    }

    struct FakeSystem final : System
    {
        bool takes = true;
        bool held = false;
        int holds = 0, letGoes = 0;
        bool hold() override
        {
            ++holds;
            held = takes;
            return takes;
        }
        void letGo() override
        {
            ++letGoes;
            held = false;
        }
        const char *refused() const override { return "the fake refuses"; }
    };

    State state(bool paused, bool hidden, bool minimized, bool exiting)
    {
        State s;
        s.paused = paused;
        s.hidden = hidden;
        s.minimized = minimized;
        s.exiting = exiting;
        return s;
    }

#ifdef _WIN32
    bool displayRequired(bool &ok)
    {
        ULONG value = 0;
        ok = CallNtPowerInformation(SystemExecutionState, nullptr, 0, &value, sizeof(value)) == 0;
        return (value & ES_DISPLAY_REQUIRED) != 0;
    }
#endif
} // namespace

int main()
{
    {
        // The rule, every state.
        bool ruleHolds = true;
        for (int bits = 0; bits < 16; ++bits)
        {
            const State s = state((bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0, (bits & 8) != 0);
            Why why = Why::Exit;
            const bool want = wanted(s, why);
            ruleHolds = ruleHolds && want == (bits == 0) && (why == Why::Playing) == want;
        }
        check(ruleHolds, "the rule: the screen is asked for only while the game runs in a shown window (16 states)");
        Why why = Why::Playing;
        wanted(state(true, false, false, false), why);
        const bool paused = why == Why::Paused;
        wanted(state(true, true, true, false), why);
        const bool hidden = why == Why::Hidden;
        wanted(state(true, false, true, false), why);
        const bool minimized = why == Why::Minimized;
        wanted(state(true, true, true, true), why);
        check(paused && hidden && minimized && why == Why::Exit,
              "the reason said: exit before hidden before minimised before paused");
        check(std::string(said(Why::Playing)) == "playing" && std::string(said(Why::Paused)) == "paused" &&
                  std::string(said(Why::Hidden)) == "hidden" && std::string(said(Why::Minimized)) == "minimised" &&
                  std::string(said(Why::Exit)) == "exit",
              "each reason has its word for the log");
    }
    {
        // One call to the system per change, however many turns the window loop makes.
        FakeSystem system;
        {
            Keeper keeper(system, false);
            check(!keeper.held() && keeper.calls() == 0u, "before the first turn nothing is asked");
            for (int turn = 0; turn < 1000; ++turn)
                keeper.update(state(false, false, false, false));
            check(keeper.held() && system.held && system.holds == 1 && system.letGoes == 0 && keeper.calls() == 1u,
                  "1,000 turns of play: the system is asked once");
            for (int turn = 0; turn < 1000; ++turn)
                keeper.update(state(true, false, false, false));
            check(!keeper.held() && !system.held && system.holds == 1 && system.letGoes == 1 && keeper.calls() == 2u,
                  "1,000 turns in the MENU: let go once");
            keeper.update(state(true, false, true, false));
            keeper.update(state(false, false, true, false));
            check(!keeper.held() && keeper.calls() == 2u, "paused, then minimised too, then only minimised: no call, still let go");
            keeper.update(state(false, false, false, false));
            check(keeper.held() && system.holds == 2 && keeper.calls() == 3u, "the window is back and the game runs: asked again, once");
            keeper.update(state(false, true, false, false));
            check(!keeper.held() && system.letGoes == 2, "the window is hidden: let go");
            keeper.update(state(false, false, false, false));
            keeper.update(state(false, false, false, true));
            check(!keeper.held() && system.holds == 3 && system.letGoes == 3, "at exit: let go");
            keeper.update(state(false, false, false, false));
            check(keeper.held() && system.holds == 4, "held again (to see the keeper let go when it ends)");
        }
        check(!system.held && system.letGoes == 4, "a keeper that ends lets go of what it holds");
    }
    {
        // A hidden window from the start (a test run): nothing is ever asked.
        FakeSystem system;
        {
            Keeper keeper(system, false);
            for (int turn = 0; turn < 100; ++turn)
                keeper.update(state(false, true, false, false));
            check(!keeper.held() && system.holds == 0 && system.letGoes == 0 && keeper.calls() == 0u,
                  "a window that is hidden from the start: the system is never called");
        }
        check(system.letGoes == 0, "and a keeper that holds nothing lets go of nothing when it ends");
    }
    {
        // A system that refuses: the game goes on, and it is not asked every turn.
        FakeSystem system;
        system.takes = false;
        Keeper keeper(system, false);
        for (int turn = 0; turn < 1000; ++turn)
            keeper.update(state(false, false, false, false));
        check(!keeper.held() && system.holds == 1, "a system that refuses is asked once, not every turn");
        keeper.update(state(true, false, false, false));
        check(system.letGoes == 0, "nothing is let go that was never held");
        system.takes = true;
        keeper.update(state(false, false, false, false));
        check(keeper.held() && system.holds == 2, "at the next change it is asked again");
    }
#ifdef _WIN32
    {
        // The system call itself, read back from the system.
        bool readBefore = false, readHeld = false, readAfter = false;
        const bool before = displayRequired(readBefore);
        std::unique_ptr<System> system = makeSystem("DW5XL unit test");
        const bool taken = system->hold();
        const bool held = displayRequired(readHeld);
        const bool again = system->hold(); // held twice is held once
        system->letGo();
        system->letGo(); // let go twice is let go once
        const bool after = displayRequired(readAfter);
        check(readBefore && readHeld && readAfter, "the system's state can be read (CallNtPowerInformation)");
        check(taken && again, std::string("the system takes the request") + (taken ? "" : std::string(": ") + system->refused()));
        check(held, "while the request is held the system's state reads \"display required\"");
        check(after == before, "after it is let go the state is what it was before the test");
        if (before)
            std::printf("note  another program kept the screen on during this test: \"while held\" shows nothing here\n");
        // A request that is still held when its owner ends goes with it.
        {
            std::unique_ptr<System> second = makeSystem("DW5XL unit test");
            second->hold();
        }
        bool readEnd = false;
        check(displayRequired(readEnd) == before && readEnd, "a request still held when its owner ends is let go");
    }
#else
    {
        std::unique_ptr<System> system = makeSystem("DW5XL unit test");
        check(!system->hold() && std::string(system->refused()).find("not built") != std::string::npos,
              "on this system the request is not built yet, and it says so (issue 067, step 2)");
    }
#endif
    std::printf("%s (%d failed)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
