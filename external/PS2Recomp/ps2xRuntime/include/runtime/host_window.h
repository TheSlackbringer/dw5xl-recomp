// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// DW5XL (issue 049, amendment v3.12 step 4): what the pages and overlays ask of the window that shows the game,
// behind one small interface, beside the one for its keys, mouse and pads (runtime/host_devices.h). The code that
// draws a page, a line of text over the picture or changes a window setting is then the same whichever window shows
// the game. Today's window answers through raylib, call for call as that code asked raylib before.
//
// One thread uses it: the one that owns the window.

#include <cstdint>
#include <string>
#include <vector>

namespace ps2x::host
{
    struct Colour
    {
        uint8_t r = 0, g = 0, b = 0, a = 255;
    };

    struct Window
    {
        virtual ~Window() = default;

        // --- The overlays' frame (Dear ImGui). A window that draws the overlays itself answers true and does the
        // work; today's window answers false and the caller uses rlImGui as before (it goes with that window).
        virtual bool uiSetup(bool dark) { (void)dark; return false; } // the ImGui context, its fonts and style
        virtual bool uiShutdown() { return false; }
        virtual bool uiBegin() { return false; } // a frame of overlays begins (or goes on, if one has begun)
        virtual bool uiEnd() { return false; }   // the caller's part of the frame is complete

        // --- Pictures for the overlays: RGBA, 4 bytes a pixel, sampled smoothly, the edges clamped. The number is
        // what ImGui's draw calls take as their texture (ImTextureID). 0: no picture.
        virtual uint64_t makeTexture(const uint8_t *rgba, int width, int height) = 0;
        virtual void dropTexture(uint64_t texture) = 0;

        // --- Plain drawing over the game's picture, in window pixels: the F2 and F8 lines, the state message (over
        // the pages) and the mouse-capture mark (under them).
        enum class Layer { UnderPages, OverPages };
        virtual void fillRect(Layer layer, int x, int y, int width, int height, Colour colour) = 0;
        virtual void text(Layer layer, int x, int y, int size, Colour colour, const char *text) = 0;
        virtual int textWidth(int size, const char *text) = 0;

        // --- The window itself.
        virtual bool hidden() const = 0;
        virtual bool maximized() const = 0;
        virtual bool minimized() const = 0;
        virtual bool fullscreen() const = 0; // borderless, the size of its display
        virtual void setFullscreen(bool on) = 0;
        virtual bool vsync() const = 0;
        virtual void setVsync(bool on) = 0;
        virtual void screenSize(int &width, int &height) const = 0; // the content in the window system's units
        virtual void renderSize(int &width, int &height) const = 0; // the content in pixels
        virtual void setScreenSize(int width, int height) = 0;
        virtual void position(int &x, int &y) const = 0;
        virtual void setPosition(int x, int y) = 0;
        virtual int displayWidth() const = 0; // of the display the window is on; 0: not known
        // Files dropped on the window since the last call (UTF-8 paths).
        virtual std::vector<std::string> takeDroppedFiles() = 0;
    };

    // The window that shows the game: today's raylib window unless another was set.
    Window &window();
    // nullptr: today's raylib window again. The object must outlive its use.
    void setWindow(Window *replacement);
} // namespace ps2x::host
