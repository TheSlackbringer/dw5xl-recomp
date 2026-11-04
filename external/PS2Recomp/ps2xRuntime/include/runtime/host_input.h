#pragma once

#include <cstdint>

namespace ps2_host_input
{
    struct MouseLook
    {
        float yawRadians = 0.0f;
        float pitchRadians = 0.0f;
        bool captured = false;
    };

    struct MouseCaptureIndicator
    {
        bool captured = false;
        float alpha = 0.0f;
    };

    // Device APIs are sampled by the window thread. Guest pad reads take one
    // snapshot per guest field (a host-input script is evaluated by those reads
    // for their field, not by poll) and game hooks consume each mouse delta once.
    void enableVerditeControls();
    void poll(uint64_t vsyncTick = 0);
    void setGameplayActive(bool active);
    MouseLook takeMouseLook();
    void discardMouseLook();
    // Querying the indicator does not consume camera motion or call device APIs.
    MouseCaptureIndicator mouseCaptureIndicator();
    // Window-thread overlay, called after the game picture has been drawn.
    void drawMouseCaptureIndicator(float x, float y, float width, float height);
    // Issue 023 3S: while a host screen is open the game gets no buttons and centred sticks; buttons still held when
    // it closes stay hidden from the game until released.
    void setGameInputBlocked(bool blocked);
    // Issue 023 3S: holding the pad's Back for `seconds` runs `action` (window thread) instead of pressing Select; a
    // shorter press still reaches the game as Select, sent on release. nullptr: Back is Select again.
    void setBackHoldAction(double seconds, void (*action)());
    // Window thread: a short press of these PS2 pad buttons (pad bit mask) reaches the game, as a key tap would.
    void tapGameButtons(uint16_t mask);
    // The game does not see these buttons until they are let go (a host screen used the press).
    void hideGameButtonsUntilRelease(uint16_t mask);
    // Issue 027 v3: true once for each host-input script event called `name` (F5, F9, SHIFT+F9) whose field has come:
    // a script presses the save-state keys as a player would.
    bool scriptedKeyPress(const char *name, uint64_t vsyncTick);
    // True when a host-input script (PS2X_HOST_INPUT) drives the run: the real keyboard and pads are not read, by the
    // game and (issue 027 v4) by the host screens either.
    bool scripted();
}
