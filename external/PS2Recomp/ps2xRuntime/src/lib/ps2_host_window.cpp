// SPDX-License-Identifier: GPL-3.0-only
#include "runtime/host_window.h"
#include "ps2_host_backend.h"

#include <atomic>

// DW5XL (issue 049, amendment v3.12 step 4): today's window, through raylib. Every call is the one the pages and
// the window loop made themselves before this interface existed. The overlays' frame is left to rlImGui (the four
// ui functions answer false).
namespace
{
    Color colourOf(ps2x::host::Colour c) { return Color{c.r, c.g, c.b, c.a}; }

    struct RaylibWindow final : ps2x::host::Window
    {
        uint64_t makeTexture(const uint8_t *rgba, int width, int height) override
        {
            if (!rgba || width <= 0 || height <= 0)
                return 0u;
            const ::Image image{const_cast<uint8_t *>(rgba), width, height, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
            const Texture2D texture = LoadTextureFromImage(image);
            SetTextureFilter(texture, TEXTURE_FILTER_BILINEAR);
            // Filtering at an edge must not reach the opposite edge (issue 027 v4: a dark line at a picture's top in
            // big windows).
            SetTextureWrap(texture, TEXTURE_WRAP_CLAMP);
            return texture.id;
        }

        void dropTexture(uint64_t texture) override
        {
            if (texture == 0u)
                return;
            Texture2D gone{};
            gone.id = static_cast<unsigned int>(texture);
            UnloadTexture(gone);
        }

        // The layers are the caller's order here: raylib draws at once, so what is drawn before the pages lies
        // under them and what is drawn after lies over them.
        void fillRect(Layer, int x, int y, int width, int height, ps2x::host::Colour colour) override
        {
            DrawRectangle(x, y, width, height, colourOf(colour));
        }

        void text(Layer, int x, int y, int size, ps2x::host::Colour colour, const char *text) override
        {
            DrawText(text, x, y, size, colourOf(colour));
        }

        int textWidth(int size, const char *text) override { return MeasureText(text, size); }

        bool hidden() const override { return IsWindowHidden(); }
        bool maximized() const override { return IsWindowMaximized(); }
        bool fullscreen() const override { return IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE); }
        void setFullscreen(bool on) override
        {
            if (on != IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE))
                ToggleBorderlessWindowed();
        }
        bool vsync() const override { return IsWindowState(FLAG_VSYNC_HINT); }
        void setVsync(bool on) override
        {
            if (on == IsWindowState(FLAG_VSYNC_HINT))
                return;
            if (on)
                SetWindowState(FLAG_VSYNC_HINT);
            else
                ClearWindowState(FLAG_VSYNC_HINT);
        }
        void screenSize(int &width, int &height) const override
        {
            width = GetScreenWidth();
            height = GetScreenHeight();
        }
        void renderSize(int &width, int &height) const override
        {
            width = GetRenderWidth();
            height = GetRenderHeight();
        }
        void setScreenSize(int width, int height) override { SetWindowSize(width, height); }
        void position(int &x, int &y) const override
        {
            const Vector2 place = GetWindowPosition();
            x = static_cast<int>(place.x);
            y = static_cast<int>(place.y);
        }
        void setPosition(int x, int y) override { SetWindowPosition(x, y); }
        int displayWidth() const override { return GetMonitorWidth(GetCurrentMonitor()); }
        std::vector<std::string> takeDroppedFiles() override
        {
            std::vector<std::string> paths;
            if (!IsFileDropped())
                return paths;
            const FilePathList files = LoadDroppedFiles();
            for (unsigned int i = 0; i < files.count; ++i)
                paths.emplace_back(files.paths[i]);
            UnloadDroppedFiles(files);
            return paths;
        }
    };

    RaylibWindow g_raylib;
    std::atomic<ps2x::host::Window *> g_current{nullptr};
} // namespace

namespace ps2x::host
{
    Window &window()
    {
        Window *current = g_current.load(std::memory_order_acquire);
        return current ? *current : g_raylib;
    }

    void setWindow(Window *replacement)
    {
        g_current.store(replacement, std::memory_order_release);
    }
} // namespace ps2x::host
