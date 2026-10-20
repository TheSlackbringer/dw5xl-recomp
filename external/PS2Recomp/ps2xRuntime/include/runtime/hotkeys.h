#pragma once
// DW5XL: the function keys of the game window, in one table so no key gets two jobs (owner layout, 2026-10-20).
// The handlers (ps2_runtime.cpp, ps2_debug_panel.cpp, the app's Discs window) read their key from here; the CTest
// hotkeys_test checks the layout, that keys are unique, and that README.md's key table lists the same keys.
#include "raylib.h"

namespace ps2x::hotkeys
{
    enum class Action
    {
        Debugger,     // F1: debugger panel (developer builds, PS2X_DEBUG_UI=1 in the player build)
        AntiAliasing, // F2: anti-aliasing choice, saved in dw5xl.ini
        FpsOverlay,   // F3: frame rate overlay
        OutputScale,  // F4: output resolution 1x / 2x / 4x, saved in dw5xl.ini
        SaveState,    // F5: save state (issue 027, not available yet)
        BuildOverlay, // F8: build ID and game tick overlay (issue 012)
        LoadState,    // F9: load state (issue 027, not available yet)
        Discs,        // F10: Discs window (installed mode)
    };

    struct Binding
    {
        Action action;
        int key;          // raylib KeyboardKey
        const char *name; // as written in README.md
    };

    inline constexpr Binding kBindings[] = {
        {Action::Debugger, KEY_F1, "F1"},       {Action::AntiAliasing, KEY_F2, "F2"},
        {Action::FpsOverlay, KEY_F3, "F3"},     {Action::OutputScale, KEY_F4, "F4"},
        {Action::SaveState, KEY_F5, "F5"},      {Action::BuildOverlay, KEY_F8, "F8"},
        {Action::LoadState, KEY_F9, "F9"},      {Action::Discs, KEY_F10, "F10"},
    };

    constexpr const Binding &binding(Action action)
    {
        for (const Binding &b : kBindings)
            if (b.action == action)
                return b;
        return kBindings[0]; // unreachable: every Action has a binding (hotkeys_test)
    }
    constexpr int key(Action action) { return binding(action).key; }
    constexpr const char *name(Action action) { return binding(action).name; }
} // namespace ps2x::hotkeys
