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
}
