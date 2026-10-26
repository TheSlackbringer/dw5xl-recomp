#pragma once
// Issue 023 step 3S: the PC SETTINGS screen's state, shared by the game-thread hooks (dw5xl_pc_settings.cpp: the
// game's SCREEN ADJUST routine drives it in the Options menu) and the window thread (the doors during play, the rows,
// applying and saving the values).
#include <cstdint>

namespace dw5xl::pcsettings {
// Rows: VSYNC (ON, OFF), RESOLUTION (1x, 2x, 4x), ANTI-ALIASING (OFF, 2x, 4x, 8x, 16x), DISPLAY (WINDOW, FULLSCREEN),
// DEFAULT (an empty row before it on screen). Values are indices into those lists.
constexpr int kRows = 5, kDefaultRow = 4;
struct Values { int vsync = 1, resolution = 0, antiAliasing = 0, display = 0; };   // the defaults (vsync off for now)
// Options: opened from the game's Options menu (the game runs the screen). InPlay: opened with Esc or Back held
// during play; the game is paused and its input blocked, the window thread runs the screen.
enum class Mode { Closed, Options, InPlay };
struct Snapshot {
    Mode mode = Mode::Closed;
    bool needsInit = false, changed = false;
    int row = 0;
    Values values;
    uint64_t version = 0;
};
enum class Input { Up, Down, Left, Right, Default };
void setOpen(bool open);                // game thread: the Options menu's screen opens / closes
bool isOpen();                          // the Options menu's screen is open (the game hooks act only then)
void input(Input in);
Snapshot snapshot();
void setValues(const Values &values);   // window thread: the current settings when the screen opens
void clearChanged();                    // window thread: after applying and saving
// Window thread, every frame (the overlay's check): the doors and the in-play input; true while the screen is shown.
// allowOpen is false while another host window (the disc prompt) is up.
bool frame(bool allowOpen);
// Window thread, inside the overlay's imgui frame: draws the screen over the game.
void drawOverlay();
}
