// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// DW5XL (issue 049, amendment v3.12 step 3 part 4): the drawing of the game's picture into a window, on its own so
// that it can be used and tested without a window. Internal: Granite's types.

#include "device.hpp"

namespace ps2x::gs
{
    // Draws one picture into a render pass that has begun and was cleared by the caller: the whole picture stretched
    // over the rectangle (x, y, width, height) in the target's pixels. Not smooth: every pixel of the target takes
    // the picture's nearest pixel (1x). Smooth: bilinear (above 1x, issue 047). Outside the picture nothing is
    // read: its edge pixels are repeated. Opaque: a picture's alpha is the PS2 frame buffer's and is not used.
    class VulkanPictureDrawer
    {
    public:
        // The device object the pictures are drawn with (the presenter's own).
        explicit VulkanPictureDrawer(Vulkan::Device &device);
        void draw(Vulkan::CommandBuffer &cmd, const Vulkan::ImageView &picture, float x, float y, float width,
                  float height, bool smooth) const;

    private:
        Vulkan::Program *m_blit = nullptr;
    };
} // namespace ps2x::gs
