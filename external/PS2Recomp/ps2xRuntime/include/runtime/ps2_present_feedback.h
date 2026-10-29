#pragma once

#include <cstdint>

// DW5XL-DEBUG issue 023 (step 3S, criterion 3): PS2X_PRESENT_LOG=1 logs every window frame with the PS2 field it
// shows ([present:frame] n=<swap number> field=<field> new=<0|1> us=<monotonic time after the swap>) and, where the
// platform reports it, which display refresh showed that swap ([present:done] n=<swap number> msc=<refresh counter>
// ust=<monotonic time of that refresh> mode=copy|flip|skip|suboptimal; skip = replaced before it was shown).
// Reported on Linux/X11 from the X server's Present completion events, read on a connection of its own (libxcb
// loaded at run time, so the game needs no extra library). Off: no cost.
namespace ps2x::present_feedback
{
    bool enabled();
    // Main thread, after the window exists: starts listening for the window's presentation events.
    void start(void *glfwWindow);
    // Main thread, after each swap: the field this window frame showed and whether it carried a new picture.
    void frameSwapped(uint64_t field, bool newPicture);
}
