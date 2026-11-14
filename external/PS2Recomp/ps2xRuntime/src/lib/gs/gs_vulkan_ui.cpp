// SPDX-License-Identifier: GPL-3.0-only
// DW5XL (issue 049, amendment v3.12 step 4): Dear ImGui's output drawn on a device object of the library. See
// gs_vulkan_ui.h. The drawing follows ImGui's own Vulkan backend (backends/imgui_impl_vulkan.cpp) step for step:
// the same two shaders, the same blending (colour by the source's alpha, alpha kept as coverage), no depth, no
// culling, one scissor per draw command, the picture sampled smoothly.

#include "gs_vulkan_ui.h"
#include "gs_vulkan_ui_shaders.h"

#include "command_buffer.hpp"
#include "shader.hpp"
#include "imgui.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

namespace ps2x::gs
{
    VulkanUiRenderer::VulkanUiRenderer(Vulkan::Device &device) : m_device(device)
    {
        // What the two shaders take in and give out, written down by hand: the library as built here does not read
        // it out of a shader's compiled form (its own shaders come with theirs). From the sources in
        // gs_vulkan_ui_shaders.h.
        Vulkan::ResourceLayout vertex, fragment;
        vertex.input_mask = 0x7u;        // aPos, aUV, aColor at locations 0, 1, 2
        vertex.output_mask = 0x1u;       // the colour and the picture's coordinate, one block at location 0
        vertex.push_constant_size = 16u; // uScale, uTranslate
        fragment.input_mask = 0x1u;
        fragment.output_mask = 0x1u;                // fColor
        fragment.sets[0].sampled_image_mask = 0x1u; // sTexture: a picture with its sampler at set 0, binding 0
        fragment.sets[0].fp_mask = 0x1u;            // sampled as floating point
        fragment.sets[0].meta[0].array_size = 1;
        Vulkan::Shader *vertexShader = device.request_shader(ui_shaders::kVertex, sizeof(ui_shaders::kVertex), &vertex);
        Vulkan::Shader *fragmentShader = device.request_shader(ui_shaders::kFragment, sizeof(ui_shaders::kFragment), &fragment);
        m_program = vertexShader && fragmentShader ? device.request_program(vertexShader, fragmentShader) : nullptr;
    }

    VulkanUiRenderer::~VulkanUiRenderer()
    {
        // ImGui's own textures no longer exist on the graphics card; a context that lives on asks for them again.
        if (ImGui::GetCurrentContext())
        {
            for (ImTextureData *texture : ImGui::GetPlatformIO().Textures)
            {
                if (texture->TexID != ImTextureID_Invalid && m_textures.count(texture->TexID))
                {
                    texture->SetTexID(ImTextureID_Invalid);
                    texture->SetStatus(ImTextureStatus_Destroyed);
                }
            }
        }
        m_textures.clear();
    }

    Vulkan::ImageHandle VulkanUiRenderer::image(int width, int height)
    {
        auto info = Vulkan::ImageCreateInfo::immutable_2d_image(static_cast<unsigned>(width), static_cast<unsigned>(height),
                                                                VK_FORMAT_R8G8B8A8_UNORM);
        info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        return m_device.create_image(info);
    }

    uint64_t VulkanUiRenderer::keep(Vulkan::ImageHandle image)
    {
        const uint64_t id = m_nextId++;
        m_textures.emplace(id, std::move(image));
        return id;
    }

    void VulkanUiRenderer::upload(Vulkan::CommandBuffer &cmd, const Vulkan::Image &image, const uint8_t *rgba, size_t bytes)
    {
        // The whole picture again: what the image held is dropped; the barrier's first half waits for the last
        // sampling of it.
        cmd.image_barrier(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
                          VK_ACCESS_2_TRANSFER_WRITE_BIT);
        if (void *rows = cmd.update_image(image))
            std::memcpy(rows, rgba, bytes);
        cmd.image_barrier(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }

    uint64_t VulkanUiRenderer::makeTexture(const uint8_t *rgba, int width, int height)
    {
        if (!rgba || width <= 0 || height <= 0)
            return 0u;
        Vulkan::ImageHandle made = image(width, height);
        if (!made)
            return 0u;
        // Sent at once in a command buffer of its own: the pages make their pictures outside the window's turn.
        auto cmd = m_device.request_command_buffer();
        upload(*cmd, *made, rgba, static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
        m_device.submit(cmd);
        return keep(std::move(made));
    }

    void VulkanUiRenderer::dropTexture(uint64_t texture)
    {
        // The device object keeps the image until the frames that drew it are done on the graphics card.
        m_textures.erase(texture);
    }

    void VulkanUiRenderer::prepare(Vulkan::CommandBuffer &cmd, ImDrawData *data)
    {
        if (!data || !data->Textures)
            return;
        for (ImTextureData *texture : *data->Textures)
        {
            if (texture->Status == ImTextureStatus_OK)
                continue;
            if (texture->Status == ImTextureStatus_WantCreate || texture->Status == ImTextureStatus_WantUpdates)
            {
                Vulkan::ImageHandle target;
                if (texture->Status == ImTextureStatus_WantUpdates)
                {
                    const auto found = m_textures.find(texture->TexID);
                    if (found != m_textures.end())
                        target = found->second;
                }
                if (!target)
                {
                    target = image(texture->Width, texture->Height);
                    if (!target)
                        continue; // asked again in the next frame
                    texture->SetTexID(keep(target));
                }
                const size_t pixels = static_cast<size_t>(texture->Width) * static_cast<size_t>(texture->Height);
                if (texture->Format == ImTextureFormat_RGBA32)
                {
                    upload(cmd, *target, static_cast<const uint8_t *>(texture->GetPixels()), pixels * 4u);
                }
                else
                {
                    // Coverage alone: white with that alpha.
                    std::vector<uint8_t> rgba(pixels * 4u, 0xFFu);
                    const uint8_t *alpha = static_cast<const uint8_t *>(texture->GetPixels());
                    for (size_t i = 0; i < pixels; ++i)
                        rgba[i * 4u + 3u] = alpha[i];
                    upload(cmd, *target, rgba.data(), rgba.size());
                }
                texture->SetStatus(ImTextureStatus_OK);
            }
            else if (texture->Status == ImTextureStatus_WantDestroy && texture->UnusedFrames > 0)
            {
                m_textures.erase(texture->TexID);
                texture->SetTexID(ImTextureID_Invalid);
                texture->SetStatus(ImTextureStatus_Destroyed);
            }
        }
    }

    void VulkanUiRenderer::draw(Vulkan::CommandBuffer &cmd, const ImDrawData *data)
    {
        if (!data || !m_program || data->TotalVtxCount <= 0 || data->TotalIdxCount <= 0)
            return;
        const int width = static_cast<int>(data->DisplaySize.x * data->FramebufferScale.x);
        const int height = static_cast<int>(data->DisplaySize.y * data->FramebufferScale.y);
        if (width <= 0 || height <= 0)
            return;

        cmd.set_opaque_state();
        cmd.set_depth_test(false, false);
        cmd.set_cull_mode(VK_CULL_MODE_NONE);
        cmd.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        cmd.set_blend_enable(true);
        cmd.set_blend_factors(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                              VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
        cmd.set_blend_op(VK_BLEND_OP_ADD);
        cmd.set_program(m_program);

        // Every list's vertices and indices, one after the other, in buffers that live for this frame.
        auto *vertices = static_cast<ImDrawVert *>(
            cmd.allocate_vertex_data(0, static_cast<VkDeviceSize>(data->TotalVtxCount) * sizeof(ImDrawVert), sizeof(ImDrawVert)));
        auto *indices = static_cast<ImDrawIdx *>(
            cmd.allocate_index_data(static_cast<VkDeviceSize>(data->TotalIdxCount) * sizeof(ImDrawIdx),
                                    sizeof(ImDrawIdx) == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32));
        if (!vertices || !indices)
            return;
        for (const ImDrawList *list : data->CmdLists)
        {
            std::memcpy(vertices, list->VtxBuffer.Data, static_cast<size_t>(list->VtxBuffer.Size) * sizeof(ImDrawVert));
            std::memcpy(indices, list->IdxBuffer.Data, static_cast<size_t>(list->IdxBuffer.Size) * sizeof(ImDrawIdx));
            vertices += list->VtxBuffer.Size;
            indices += list->IdxBuffer.Size;
        }
        cmd.set_vertex_attrib(0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ImDrawVert, pos));
        cmd.set_vertex_attrib(1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ImDrawVert, uv));
        cmd.set_vertex_attrib(2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(ImDrawVert, col));

        // ImGui's display, in its own units, onto the whole target.
        const float scale[2] = {2.0f / data->DisplaySize.x, 2.0f / data->DisplaySize.y};
        const float constants[4] = {scale[0], scale[1], -1.0f - data->DisplayPos.x * scale[0], -1.0f - data->DisplayPos.y * scale[1]};
        cmd.push_constants(constants, 0, sizeof(constants));
        cmd.set_viewport({0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f});

        const ImVec2 clipOffset = data->DisplayPos, clipScale = data->FramebufferScale;
        int listVertex = 0, listIndex = 0;
        for (const ImDrawList *list : data->CmdLists)
        {
            for (const ImDrawCmd &command : list->CmdBuffer)
            {
                if (command.UserCallback)
                {
                    // The pages draw no callbacks; one that asks for the state to be set again needs nothing here
                    // (the state is not changed between commands).
                    if (command.UserCallback != ImDrawCallback_ResetRenderState)
                        command.UserCallback(list, &command);
                    continue;
                }
                float x0 = (command.ClipRect.x - clipOffset.x) * clipScale.x, y0 = (command.ClipRect.y - clipOffset.y) * clipScale.y;
                float x1 = (command.ClipRect.z - clipOffset.x) * clipScale.x, y1 = (command.ClipRect.w - clipOffset.y) * clipScale.y;
                x0 = std::max(x0, 0.0f);
                y0 = std::max(y0, 0.0f);
                x1 = std::min(x1, static_cast<float>(width));
                y1 = std::min(y1, static_cast<float>(height));
                if (x1 <= x0 || y1 <= y0)
                    continue;
                const auto found = m_textures.find(command.GetTexID());
                if (found == m_textures.end())
                    continue; // a picture that was let go: nothing to sample
                VkRect2D scissor;
                scissor.offset.x = static_cast<int32_t>(x0);
                scissor.offset.y = static_cast<int32_t>(y0);
                scissor.extent.width = static_cast<uint32_t>(x1 - x0);
                scissor.extent.height = static_cast<uint32_t>(y1 - y0);
                cmd.set_scissor(scissor);
                cmd.set_texture(0, 0, found->second->get_view(), Vulkan::StockSampler::LinearClamp);
                cmd.draw_indexed(command.ElemCount, 1, command.IdxOffset + static_cast<uint32_t>(listIndex),
                                 static_cast<int32_t>(command.VtxOffset) + listVertex, 0);
            }
            listVertex += list->VtxBuffer.Size;
            listIndex += list->IdxBuffer.Size;
        }
        cmd.set_scissor({{0, 0}, {static_cast<uint32_t>(width), static_cast<uint32_t>(height)}});
    }
} // namespace ps2x::gs
