// SPDX-License-Identifier: GPL-3.0-only
#include "runtime/host_window.h"

#include <atomic>

// DW5XL (issue 049, amendment v3.12): the window that shows the game is the one the renderer draws itself; it
// sets itself here (ps2_runtime.cpp). Until it exists, and after it is gone, there is no window: nothing is drawn,
// and it counts as hidden. (Until step 7 the raylib window stood here.)
namespace
{
    struct NoWindow final : ps2x::host::Window
    {
        uint64_t makeTexture(const uint8_t *, int, int) override { return 0u; }
        void dropTexture(uint64_t) override {}
        void fillRect(Layer, int, int, int, int, ps2x::host::Colour) override {}
        void text(Layer, int, int, int, ps2x::host::Colour, const char *) override {}
        int textWidth(int, const char *) override { return 0; }

        bool hidden() const override { return true; }
        bool maximized() const override { return false; }
        bool minimized() const override { return false; }
        bool fullscreen() const override { return false; }
        void setFullscreen(bool) override {}
        bool vsync() const override { return false; }
        void setVsync(bool) override {}
        void screenSize(int &width, int &height) const override
        {
            width = 0;
            height = 0;
        }
        void renderSize(int &width, int &height) const override
        {
            width = 0;
            height = 0;
        }
        void setScreenSize(int, int) override {}
        void position(int &x, int &y) const override
        {
            x = 0;
            y = 0;
        }
        void setPosition(int, int) override {}
        int displayWidth() const override { return 0; }
        std::vector<std::string> takeDroppedFiles() override { return {}; }
    };

    NoWindow g_none;
    std::atomic<ps2x::host::Window *> g_current{nullptr};
} // namespace

namespace ps2x::host
{
    Window &window()
    {
        Window *current = g_current.load(std::memory_order_acquire);
        return current ? *current : g_none;
    }

    void setWindow(Window *replacement)
    {
        g_current.store(replacement, std::memory_order_release);
    }
} // namespace ps2x::host
