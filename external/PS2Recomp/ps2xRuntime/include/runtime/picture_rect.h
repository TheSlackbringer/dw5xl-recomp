// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <cstdint>

// DW5XL (issue 049, amendment v3.12 step 3): where the game's picture lies in the window. One function for every
// window that shows it: letterboxed to the picture's shape (issue 047) and centred.
namespace ps2x
{
    struct PictureRect
    {
        float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
    };

    // displayWidth, displayHeight: the PS2 display's logical size (not the stored picture's, which grows with the
    // output scale). shape: the picture's width over its height in the window, 0 = the display's own size.
    // windowWidth, windowHeight: the window's drawable size in pixels.
    // The operations and their order are those of the window's own code before this function existed (issue 047):
    // the result is bit for bit the same (app/tests/picture_rect_test.cpp).
    inline PictureRect pictureRectInWindow(uint32_t displayWidth, uint32_t displayHeight, float shape,
                                           float windowWidth, float windowHeight)
    {
        const float aspectHeight = static_cast<float>(std::max<uint32_t>(1u, displayHeight));
        const float aspectWidth = shape > 0.0f ? aspectHeight * shape
                                               : static_cast<float>(std::max<uint32_t>(1u, displayWidth));
        const float scale = std::min(windowWidth / aspectWidth, windowHeight / aspectHeight);
        const float width = aspectWidth * scale;
        const float height = aspectHeight * scale;
        return {(windowWidth - width) * 0.5f, (windowHeight - height) * 0.5f, width, height};
    }
} // namespace ps2x
