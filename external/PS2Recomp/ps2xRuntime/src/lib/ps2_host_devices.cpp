// SPDX-License-Identifier: GPL-3.0-only
#include "runtime/host_devices.h"
#include "ps2_host_backend.h"

#include <atomic>

// DW5XL (issue 049, amendment v3.12 step 3): the devices of today's window, through raylib. Every call is the one
// the reading code made itself before this interface existed.
namespace
{
    struct RaylibDevices final : ps2x::host::Devices
    {
        bool ready() const override { return IsWindowReady(); }
        bool focused() const override { return IsWindowFocused(); }
        double seconds() const override { return GetTime(); }
        bool keyDown(int key) const override { return IsKeyDown(key); }
        bool keyPressed(int key) const override { return IsKeyPressed(key); }
        bool keyPressedRepeat(int key) const override { return IsKeyPressedRepeat(key); }
        bool padAvailable(int pad) const override { return IsGamepadAvailable(pad); }
        bool padButtonDown(int pad, int button) const override { return IsGamepadButtonDown(pad, button); }
        bool padButtonPressed(int pad, int button) const override { return IsGamepadButtonPressed(pad, button); }
        float padAxis(int pad, int axis) const override { return GetGamepadAxisMovement(pad, axis); }
        bool mouseButtonDown(int button) const override { return IsMouseButtonDown(button); }
        float mouseWheel() const override { return GetMouseWheelMove(); }
        void mouseDelta(float &x, float &y) const override
        {
            const Vector2 delta = GetMouseDelta();
            x = delta.x;
            y = delta.y;
        }
        void captureMouse(bool captured) override
        {
            if (captured)
                DisableCursor();
            else
                EnableCursor();
        }
    };

    RaylibDevices g_raylib;
    std::atomic<ps2x::host::Devices *> g_current{nullptr};
} // namespace

namespace ps2x::host
{
    Devices &devices()
    {
        Devices *current = g_current.load(std::memory_order_acquire);
        return current ? *current : g_raylib;
    }

    void setDevices(Devices *replacement)
    {
        g_current.store(replacement, std::memory_order_release);
    }
} // namespace ps2x::host
