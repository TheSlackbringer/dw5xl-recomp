#pragma once
// Issue 023 step 3S: the PC SETTINGS screen's state, shared by the game-thread hooks (dw5xl_pc_settings.cpp: the
// game's SCREEN ADJUST routine drives it) and the window thread (draws the rows, applies and saves the values).
#include <cstdint>

namespace dw5xl::pcsettings {
// Rows: VSYNC (ON, OFF), RESOLUTION (1x, 2x, 4x), ANTI-ALIASING (OFF, 2x, 4x, 8x, 16x), DISPLAY (WINDOW, FULLSCREEN),
// DEFAULT (an empty row before it on screen). Values are indices into those lists.
constexpr int kRows = 5, kDefaultRow = 4;
struct Values { int vsync = 1, resolution = 0, antiAliasing = 0, display = 0; };   // the defaults (vsync off for now)
struct Snapshot {
    bool open = false, needsInit = false, changed = false;
    int row = 0;
    Values values;
    uint64_t version = 0;
};
enum class Input { Up, Down, Left, Right, Default };
void setOpen(bool open);
bool isOpen();
void input(Input in);
Snapshot snapshot();
void setValues(const Values &values);   // window thread: the current settings when the screen opens
void clearChanged();                    // window thread: after applying and saving
}
