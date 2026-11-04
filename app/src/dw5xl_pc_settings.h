#pragma once
// Issue 023 step 3S: the PC SETTINGS screen's state, shared by the game-thread hooks (dw5xl_pc_settings.cpp: the
// game's SCREEN ADJUST routine drives it in the Options menu) and the window thread (the doors during play, the rows,
// applying and saving the values).
#include <cstdint>

namespace dw5xl::pcsettings {
// PC SETTINGS is a menu of two pages. Rows: VSYNC (ON, OFF), RENDER SCALE (1x, 2x, 4x), ANTI-ALIASING (OFF, 2x, 4x, 8x, 16x),
// DISPLAY (WINDOW, FULLSCREEN), SHOW FPS (ON, OFF), ASPECT RATIO (4:3, 16:9, 21:9), FIELD OF VIEW (80 %..130 % in 5 % steps),
// SHARPEN (ON, OFF), DEFAULT (its page only). Values are indices into those lists.
enum Row { RowVsync, RowScale, RowAntiAliasing, RowDisplay, RowShowFps, RowAspect, RowFov, RowSharpen, RowDefault };
constexpr int kRows = RowDefault + 1, kFovValues = 11, kFovDefault = 4;
enum Page { PageMenu, PageGraphics, PageDisplay };
constexpr int kPageRows = 5, kMenuChoices = 2;
constexpr Row kPages[2][kPageRows] = {{RowScale, RowAntiAliasing, RowSharpen, RowFov, RowDefault},
                                      {RowDisplay, RowAspect, RowVsync, RowShowFps, RowDefault}};
// defaults (vsync off for now)
struct Values { int vsync = 1, resolution = 0, antiAliasing = 0, display = 0, showFps = 1, aspect = 0, fov = kFovDefault, sharpen = 1; };
// Options: opened from the game's Options menu (the game runs the screen). InPlay: opened with Esc or Back held
// during play; the game is paused and its input blocked, the window thread runs the screen.
enum class Mode { Closed, Options, InPlay };
struct Snapshot {
    Mode mode = Mode::Closed;
    bool needsInit = false, changed = false;
    int page = PageMenu;
    int row = 0;                                       // on the page: a choice in the menu, an index into kPages otherwise
    Values values;
    uint64_t version = 0;
};
enum class Input { Up, Down, Left, Right, Default, Enter, Leave };
void setOpen(bool open);                // game thread: the Options menu's screen opens / closes
bool isOpen();                          // the Options menu's screen is open (the game hooks act only then)
bool input(Input in);                   // true when the row or a value changed (the game's menus sound only then)
Snapshot snapshot();
void setValues(const Values &values);   // window thread: the current settings when the screen opens
void clearChanged();                    // window thread: after applying and saving
// Window thread, every frame (the overlay's check): the doors and the in-play input; true while the screen is shown.
// allowOpen is false while another host window (the disc prompt) is up.
bool frame(bool allowOpen);
// Window thread, inside the overlay's imgui frame: draws the screen over the game.
void drawOverlay();
}
