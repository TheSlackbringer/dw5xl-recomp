// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// DW5XL (issue 049, amendment v3.12 step 3 parts 4 and 5): the game's window drawn by the renderer itself. A window
// of the system without OpenGL (GLFW), a Vulkan swap chain on the renderer's graphics device, and the game's
// finished pictures drawn into it straight from the graphics card (GSVulkanBackend::PresenterLink): no picture is
// read back to be shown. A picture that only exists in main memory (two fields put together by the CPU at 1x, the
// CPU renderer) is uploaded (memoryPicture). Whatever needs the shown picture in main memory (a frame dump, a
// saved state's picture) asks for it when it needs it (shownPixels). Behind a switch so far; today's window stays
// the default.
//
// One thread makes the window and calls everything here (the thread that owns the system's window events).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct GSVulkanPresenterLink;

namespace ps2x::host
{
    struct Devices;
}

namespace ps2x::gs
{
    struct GameWindowOptions
    {
        int width = 640;
        int height = 480;
        std::string title;
        bool hidden = false; // made, never shown (test runs)
        bool vsync = true;   // a picture is handed to the display on its refresh; off: at once
    };

    // How the picture is drawn, asked every time (the settings can change while the game runs).
    struct GameWindowLook
    {
        float shape = 0.0f;   // the picture's width over its height in the window; 0: the display's own size
        bool smooth = false;  // bilinear (above 1x, issue 047); off: the nearest pixel
    };

    // A picture in main memory: RGBA, 4 bytes a pixel, rows `strideBytes` apart.
    struct GameWindowPixels
    {
        const uint8_t *rgba = nullptr;
        size_t strideBytes = 0u;                        // 0: width * 4
        uint32_t width = 0u, height = 0u;
        uint32_t displayWidth = 0u, displayHeight = 0u; // the PS2 display's logical size; 0: the picture's own
        uint64_t field = 0u;
        uint64_t order = 0u; // ps2x::gs::nextPictureOrder() when it was finished (GS::hostPresentationOrder)
    };

    struct GameWindowShown
    {
        bool presented = false;  // the window's content went to the swap chain this time
        bool newPicture = false; // a picture of the game the window had not shown before
        bool have = false;       // the window has a picture of the game
        bool fromMemory = false; // it was given from main memory (memoryPicture), not taken on the graphics card
        uint64_t field = 0u;     // that picture's field
        uint64_t order = 0u;     // its place among all finished pictures (ps2x::gs::nextPictureOrder)
        uint32_t width = 0u, height = 0u;               // its size as stored (grows with the output scale)
        uint32_t displayWidth = 0u, displayHeight = 0u; // the PS2 display's logical size
        float x = 0.0f, y = 0.0f, rectWidth = 0.0f, rectHeight = 0.0f; // where it lies in the window
        // What this call spent, in milliseconds: until the swap chain gave an image to draw on, drawing (an
        // upload included) up to the send to the graphics card, and handing the image to the display.
        float acquireMs = 0.0f, drawMs = 0.0f, presentMs = 0.0f;
    };

    struct GameWindowCounts
    {
        uint64_t presented = 0u;   // times the window's content went to the swap chain
        uint64_t newPictures = 0u; // pictures of the game the window took to show
        uint64_t fromMemory = 0u;  // of them, given from main memory and uploaded
        uint64_t olderNotShown = 0u; // pictures not shown: a picture finished later was shown already
        uint64_t notPresented = 0u; // times nothing could be presented (no size, no swap chain image)
        uint64_t swapChains = 0u;  // swap chains made (the first one, and one for every change of size)
    };

    class GameWindow
    {
    public:
        // Nothing, with the reason in `why`, when the renderer cannot present (no window support), the system has
        // no Vulkan windows, or the window or its swap chain cannot be made. Starts the hand-over of the game's
        // fields; the destructor stops it.
        // Without a link (a renderer that does not draw with Vulkan: the CPU renderer) the window makes a Vulkan
        // device of its own and shows only pictures from main memory.
        static std::unique_ptr<GameWindow> open(const std::shared_ptr<GSVulkanPresenterLink> &link,
                                                const GameWindowOptions &options, std::string &why);
        virtual ~GameWindow() = default;

        // The system's events since the last call; the keys, the mouse and the pads as they are now.
        virtual void poll() = 0;
        virtual bool closeAsked() const = 0;
        virtual host::Devices &devices() = 0;

        // A picture of the game that only exists in main memory. The next show() shows it unless a picture finished
        // later (a larger `order`) is there. The pixels are copied; the caller's can go.
        virtual void memoryPicture(const GameWindowPixels &picture) = 0;

        // Takes the newest picture of the game's fields up to `maxField` (if there is a new one), or the picture
        // given from main memory if that one was finished later, draws the window (black, the picture letterboxed
        // to its shape) and presents it.
        virtual GameWindowShown show(uint64_t maxField, const GameWindowLook &look) = 0;

        // The picture the window shows, as the renderer finished it (its own size, no window scaling), in main
        // memory: RGBA, rows width * 4 bytes apart, opaque. A picture on the graphics card is read from there,
        // which waits for the card: only on request (a frame dump, a saved state's picture), never to show.
        virtual bool shownPixels(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height) = 0;

        // What show() draws now, drawn once more into a picture of the window's size and read into main memory
        // (RGBA, 4 bytes a pixel, opaque). For window pictures on request; never part of showing.
        virtual bool picture(const GameWindowLook &look, std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height) = 0;

        virtual void drawableSize(uint32_t &width, uint32_t &height) const = 0;
        virtual void resize(int width, int height) = 0; // the window's content size, as the system allows
        virtual bool hidden() const = 0;
        virtual GameWindowCounts counts() const = 0;
    };
} // namespace ps2x::gs
