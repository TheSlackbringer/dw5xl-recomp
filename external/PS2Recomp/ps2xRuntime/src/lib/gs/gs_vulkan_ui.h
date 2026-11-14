// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// DW5XL (issue 049, amendment v3.12 step 4): Dear ImGui's output drawn on a device object of the library, for the
// game's window drawn by the renderer itself. It follows ImGui's renderer protocol of 1.92 (the textures ImGui asks
// for, its font picture among them, are made, updated and let go here) and draws with ImGui's own two shaders
// (gs_vulkan_ui_shaders.h). It also keeps the pictures the pages make for themselves.
//
// One thread, the window's. A picture's number is what ImGui's draw calls carry as their texture (ImTextureID).

#include "device.hpp"
#include "image.hpp"

#include <cstdint>
#include <unordered_map>

struct ImDrawData;

namespace ps2x::gs
{
    class VulkanUiRenderer
    {
    public:
        explicit VulkanUiRenderer(Vulkan::Device &device);
        // Lets go of every picture. Only after the device object is idle; ImGui's own textures are told so if its
        // context still exists.
        ~VulkanUiRenderer();

        // A picture of the caller's: RGBA, 4 bytes a pixel, sampled smoothly, its edges clamped. 0: none was made.
        uint64_t makeTexture(const uint8_t *rgba, int width, int height);
        void dropTexture(uint64_t texture);

        // Before the render pass, in the command buffer that will draw `data`: what ImGui wants made, updated or let
        // go of its textures.
        void prepare(Vulkan::CommandBuffer &cmd, ImDrawData *data);
        // Inside a render pass whose target is the size of ImGui's display in pixels.
        void draw(Vulkan::CommandBuffer &cmd, const ImDrawData *data);

        size_t pictures() const { return m_textures.size(); }

    private:
        uint64_t keep(Vulkan::ImageHandle image);
        Vulkan::ImageHandle image(int width, int height);
        void upload(Vulkan::CommandBuffer &cmd, const Vulkan::Image &image, const uint8_t *rgba, size_t bytes);

        Vulkan::Device &m_device;
        Vulkan::Program *m_program = nullptr;
        std::unordered_map<uint64_t, Vulkan::ImageHandle> m_textures;
        uint64_t m_nextId = 1u;
    };
} // namespace ps2x::gs
