#pragma once

namespace ps2_host_input
{
    // Issue 023 3S: the pad's Back button, when holding it runs a host action. Held for `holdSeconds` it runs the
    // action, a shorter press reaches the game as a short Select when it is let go.
    // Issue 058: with Start down it is the game's Select at once, until it is let go, so the game's own Select + Start
    // reset works as on a PS2. The order of the two buttons does not matter; after the action has run, Start changes nothing.
    struct BackButton
    {
        double downSince = -1.0;
        bool fired = false, withStart = false;

        struct Result
        {
            bool select = false;   // the game sees Select down now
            bool tap = false;      // a short press has ended: tap Select
            bool fire = false;     // run the host action
        };

        Result step(bool back, bool start, double now, double holdSeconds)
        {
            Result r;
            if (back) {
                if (downSince < 0.0) downSince = now;
                if (start && !fired) withStart = true;
                if (withStart) r.select = true;
                else if (!fired && now - downSince >= holdSeconds) { fired = true; r.fire = true; }
            } else if (downSince >= 0.0) {
                r.tap = !fired && !withStart;
                downSince = -1.0; fired = withStart = false;
            }
            return r;
        }
    };
}
