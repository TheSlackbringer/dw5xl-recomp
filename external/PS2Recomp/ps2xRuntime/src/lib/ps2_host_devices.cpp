// SPDX-License-Identifier: GPL-3.0-only
#include "runtime/host_devices.h"

#include <atomic>

// DW5XL (issue 049, amendment v3.12): the keys, mouse and pads the game reads are those of the window that shows
// it (the renderer's own window sets them, ps2_runtime.cpp). Until a window exists, and after it is gone, there
// are none: nothing is ready, held or moved. (Until step 7 the devices of the raylib window stood here.)
namespace
{
    struct NoDevices final : ps2x::host::Devices
    {
        bool ready() const override { return false; }
        bool focused() const override { return false; }
        double seconds() const override { return 0.0; }
        bool keyDown(int) const override { return false; }
        bool keyPressed(int) const override { return false; }
        bool keyPressedRepeat(int) const override { return false; }
        bool padAvailable(int) const override { return false; }
        bool padButtonDown(int, int) const override { return false; }
        bool padButtonPressed(int, int) const override { return false; }
        float padAxis(int, int) const override { return 0.0f; }
        bool mouseButtonDown(int) const override { return false; }
        float mouseWheel() const override { return 0.0f; }
        void mouseDelta(float &x, float &y) const override
        {
            x = 0.0f;
            y = 0.0f;
        }
        void captureMouse(bool) override {}
    };

    NoDevices g_none;
    std::atomic<ps2x::host::Devices *> g_current{nullptr};
} // namespace

namespace ps2x::host
{
    Devices &devices()
    {
        Devices *current = g_current.load(std::memory_order_acquire);
        return current ? *current : g_none;
    }

    void setDevices(Devices *replacement)
    {
        g_current.store(replacement, std::memory_order_release);
    }
} // namespace ps2x::host
