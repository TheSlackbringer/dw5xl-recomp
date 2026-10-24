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
}
