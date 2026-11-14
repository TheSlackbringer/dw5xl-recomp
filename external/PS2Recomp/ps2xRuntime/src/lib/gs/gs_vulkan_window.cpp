// SPDX-License-Identifier: GPL-3.0-only
// DW5XL (issue 049, amendment v3.12 step 3 parts 4 and 5): the game's window drawn by the renderer itself.
// See runtime/gs/gs_game_window.h. The window is GLFW's without OpenGL; the swap chain is the library's presenter
// (Granite's Vulkan::WSI) with a device object of its own on the renderer's context, as step 1's test window
// measured it (tools/present_probe): the presenter then never waits for the renderer's open command buffers.

#include "runtime/gs/gs_game_window.h"

#include "gs_vulkan_handover.h"
#include "gs_vulkan_window.h"
#include "gs_vulkan_window_shaders.h"
#include "runtime/host_devices.h"
#include "runtime/host_input_state.h"
#include "runtime/host_window.h"
#include "runtime/picture_rect.h"
#ifdef PS2X_HAS_WINDOW_UI
#include "gs_vulkan_ui.h"
#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#endif

#include "context.hpp"
#include "device.hpp"
#include "thread_id.hpp"
#include "timer.hpp"
#include "wsi.hpp"
#include "shader.hpp"
#include "shaders/slangmosh_iface.hpp"
#include "shaders/slangmosh.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <string>
#include <utility>
#include <vector>

namespace ps2x::gs
{
    VulkanPictureDrawer::VulkanPictureDrawer(Vulkan::Device &device)
    {
        // The library's own two shaders for a picture on a quad: three corners that cover the viewport, and one
        // sample of the picture at each pixel.
        Vulkan::ResourceLayout layout;
        ParallelGS::Shaders<> suite(device, layout, 0);
        m_blit = device.request_program(suite.quad, suite.blit);
        // SHARPEN's fragment shader is ours (shaders/picture_sharpen.frag, compiled in gs_vulkan_window_shaders.h),
        // on the library's quad. The library is built without its reflection, so the shader's layout is written out
        // here: vUV in, the colour out, the picture with its sampler at set 0 binding 0, and eight bytes of push
        // constants (one picture pixel in vUV's units).
        Vulkan::ResourceLayout sharpen;
        sharpen.input_mask = 0x1u;
        sharpen.output_mask = 0x1u;
        sharpen.sets[0].sampled_image_mask = 0x1u;
        sharpen.sets[0].fp_mask = 0x1u;
        sharpen.sets[0].meta[0].array_size = 1;
        sharpen.push_constant_size = 8u;
        if (Vulkan::Shader *shader = device.request_shader(window_shaders::kPictureSharpen, sizeof(window_shaders::kPictureSharpen), &sharpen))
            m_sharpen = device.request_program(suite.quad, shader);
    }

    void VulkanPictureDrawer::draw(Vulkan::CommandBuffer &cmd, const Vulkan::ImageView &picture, float x, float y,
                                   float width, float height, bool smooth, bool sharpen) const
    {
        if (!m_blit || !(width > 0.0f) || !(height > 0.0f))
            return;
        const bool sharp = sharpen && smooth && m_sharpen;
        cmd.set_opaque_state();
        cmd.set_depth_test(false, false);
        cmd.set_cull_mode(VK_CULL_MODE_NONE);
        cmd.set_program(sharp ? m_sharpen : m_blit);
        cmd.set_texture(0, 0, picture, smooth ? Vulkan::StockSampler::LinearClamp : Vulkan::StockSampler::NearestClamp);
        if (sharp)
        {
            const float texel[2] = {1.0f / static_cast<float>(picture.get_image().get_width()),
                                    1.0f / static_cast<float>(picture.get_image().get_height())};
            cmd.push_constants(texel, 0, sizeof(texel));
        }
        // The quad covers the viewport and is cut at its edges, so the viewport is the picture's rectangle.
        cmd.set_viewport({x, y, width, height, 0.0f, 1.0f});
        cmd.draw(3);
    }
} // namespace ps2x::gs

namespace
{
    using ps2x::gs::GameWindowCounts;
    using ps2x::gs::GameWindowLook;
    using ps2x::gs::GameWindowOptions;
    using ps2x::gs::GameWindowPixels;
    using ps2x::gs::GameWindowShown;
    using ps2x::gs::GameWindowTimed;
    using ps2x::gs::GameWindowReport;

    // Milliseconds on the PC's steady clock (as ps2x::display_clock::nowMs).
    double steadyMs()
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // GLFW is started by the first window and ended by the last (the thread that owns the window events).
    int g_glfwUsers = 0;

    bool glfwUp(std::string &why)
    {
        if (g_glfwUsers == 0)
        {
            if (!glfwInit())
            {
                const char *text = nullptr;
                glfwGetError(&text);
                why = std::string("the window system did not start") + (text ? std::string(": ") + text : std::string());
                return false;
            }
            // As raylib does right after it starts GLFW: GLFW looks for joysticks now, not in the first poll.
            glfwSetJoystickCallback(nullptr);
        }
        ++g_glfwUsers;
        return true;
    }

    void glfwDown()
    {
        if (g_glfwUsers > 0 && --g_glfwUsers == 0)
            glfwTerminate();
    }

    // What the library's presenter asks of the window.
    struct Platform final : Vulkan::WSIPlatform
    {
        GLFWwindow *window = nullptr;
        uint64_t chains = 0u;

        VkSurfaceKHR create_surface(VkInstance instance, VkPhysicalDevice) override
        {
            VkSurfaceKHR surface = VK_NULL_HANDLE;
            if (glfwCreateWindowSurface(instance, window, nullptr, &surface) != VK_SUCCESS)
                return VK_NULL_HANDLE;
            return surface;
        }

        // Not asked: the renderer's context exists already and was made with the system's surface extensions.
        std::vector<const char *> get_instance_extensions() override
        {
            uint32_t count = 0;
            const char **names = glfwGetRequiredInstanceExtensions(&count);
            return names ? std::vector<const char *>(names, names + count) : std::vector<const char *>();
        }

        uint32_t get_surface_width() override
        {
            int w = 0, h = 0;
            glfwGetFramebufferSize(window, &w, &h);
            return static_cast<uint32_t>(std::max(w, 0));
        }

        uint32_t get_surface_height() override
        {
            int w = 0, h = 0;
            glfwGetFramebufferSize(window, &w, &h);
            return static_cast<uint32_t>(std::max(h, 0));
        }

        // The presenter loops on this while it has no surface to draw on. show() never asks it to present a window
        // without a size, so it has nothing to wait for here.
        bool alive(Vulkan::WSI &) override
        {
            return !glfwWindowShouldClose(window);
        }

        // The window's events are taken in GameWindow::poll, once a turn, not by the presenter.
        void poll_input() override
        {
        }

        void poll_input_async(Granite::InputTrackerHandler *) override
        {
        }

        // 0: the plain Vulkan swap chain (owner 2026-11-13, after step 1's session), not the one through DXGI.
        uintptr_t get_native_window() override
        {
            return 0;
        }

        void event_swapchain_created(Vulkan::Device *device, VkSwapchainKHR swapchain, unsigned width, unsigned height,
                                     float aspect, size_t images, VkFormat format, VkColorSpaceKHR space,
                                     VkSurfaceTransformFlagBitsKHR rotate) override
        {
            Vulkan::WSIPlatform::event_swapchain_created(device, swapchain, width, height, aspect, images, format, space,
                                                         rotate);
            ++chains;
        }

        void sizeChanged()
        {
            resize = true;
        }
    };

    class Window final : public ps2x::gs::GameWindow, public ps2x::host::Devices, public ps2x::host::Window
    {
    public:
        Window() = default;

        ~Window() override
        {
            if (m_started)
            {
                Util::register_thread_index(0);
                attachTimedPictures(nullptr);
                {
                    std::lock_guard<std::mutex> lock(m_finishMutex);
                    m_finishStop = true;
                    m_onTimedFinished = nullptr;
                }
                m_finishWake.notify_one();
                if (m_finishThread.joinable())
                    m_finishThread.join();
                if (m_link)
                {
                    if (m_current)
                        m_link->fields->release(m_current);
                    m_link->fields->stop();
                }
                closeUi();
                m_wsi.get_device().wait_idle();
                m_views.reset();
                m_drawer.reset();
                m_memoryImage.reset();
                m_wsi.teardown();
            }
            if (m_platform.window)
                glfwDestroyWindow(m_platform.window);
            if (m_glfw)
                glfwDown();
        }

        bool start(const std::shared_ptr<GSVulkanPresenterLink> &link, const GameWindowOptions &options, std::string &why)
        {
            if (link && (!link->context || !link->queues || !link->fields))
            {
                why = "the renderer has nothing to present with";
                return false;
            }
            if (link && !link->windowSupport)
            {
                why = "the renderer's graphics device was made without window support";
                return false;
            }
            m_link = link;
            if (!glfwUp(why))
                return false;
            m_glfw = true;
            if (!glfwVulkanSupported())
            {
                why = "the window system has no Vulkan windows";
                return false;
            }
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            glfwWindowHint(GLFW_VISIBLE, options.hidden ? GLFW_FALSE : GLFW_TRUE);
            glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
            m_hidden = options.hidden;
            m_vsync = options.vsync;
            m_platform.window = glfwCreateWindow(std::max(options.width, 1), std::max(options.height, 1),
                                                 options.title.c_str(), nullptr, nullptr);
            glfwDefaultWindowHints();
            if (!m_platform.window)
            {
                const char *text = nullptr;
                glfwGetError(&text);
                why = std::string("the window could not be made") + (text ? std::string(": ") + text : std::string());
                return false;
            }
            glfwSetWindowUserPointer(m_platform.window, this);
            glfwSetFramebufferSizeCallback(m_platform.window, [](GLFWwindow *w, int, int) { self(w)->m_platform.sizeChanged(); });
            // The lock keys' state comes with every key event (raylib keeps Caps Lock and Num Lock down while on).
            glfwSetInputMode(m_platform.window, GLFW_LOCK_KEY_MODS, GLFW_TRUE);
            glfwSetKeyCallback(m_platform.window, [](GLFWwindow *w, int key, int, int action, int mods) { self(w)->m_input.key(key, action, mods); });
            glfwSetMouseButtonCallback(m_platform.window, [](GLFWwindow *w, int button, int action, int) { self(w)->m_input.mouseButton(button, action); });
            glfwSetCursorPosCallback(m_platform.window, [](GLFWwindow *w, double x, double y) { self(w)->m_input.mouseMoved(x, y); });
            glfwSetScrollCallback(m_platform.window, [](GLFWwindow *w, double x, double y) { self(w)->m_input.scrolled(x, y); });
            glfwSetWindowFocusCallback(m_platform.window, [](GLFWwindow *w, int focused) { self(w)->m_input.focus(focused != 0); });
            glfwSetDropCallback(m_platform.window, [](GLFWwindow *w, int count, const char **paths) {
                for (int i = 0; i < count; ++i)
                    self(w)->m_dropped.emplace_back(paths[i]);
            });

            // The presenter: a device object of its own on the renderer's context, the queues shared through the
            // renderer's lock.
            Util::register_thread_index(0);
            m_wsi.set_platform(&m_platform);
            m_wsi.set_present_mode(options.vsync ? Vulkan::PresentMode::SyncToVBlank : Vulkan::PresentMode::UnlockedMaybeTear);
            m_wsi.set_backbuffer_format(Vulkan::BackbufferFormat::UNORM); // the picture's bytes as they are
            if (link)
            {
                if (!m_wsi.init_from_existing_context(link->context) || !m_wsi.init_device())
                {
                    why = "the presenter did not start on the renderer's graphics device";
                    return false;
                }
            }
            else
            {
                // No Vulkan renderer (the CPU renderer draws): a Vulkan device of the window's own, to show the
                // pictures it is given from main memory.
                if (!Vulkan::Context::init_loader(nullptr))
                {
                    why = "Vulkan is not available on this system";
                    return false;
                }
                if (!m_wsi.init_context_from_platform(1, {}) || !m_wsi.init_device())
                {
                    why = "no Vulkan graphics device could be started for the window";
                    return false;
                }
            }
            m_started = true;
            Vulkan::Device &device = m_wsi.get_device();
            if (link)
                device.set_queue_lock([lock = link->queues] { lock->lock(); }, [lock = link->queues] { lock->unlock(); });
            if (!m_wsi.init_surface_swapchain())
            {
                why = "the window's swap chain could not be made";
                return false;
            }
            // Shown-time reports are not asked for: a hidden window's never come and block the presenter (step 1,
            // finding 3). They come with the display clock (step 5).
            m_wsi.set_enable_timing_feedback(false);
            m_drawer = std::make_unique<ps2x::gs::VulkanPictureDrawer>(device);
            m_views = std::make_unique<HandedPictureViews>(device, device.get_num_frame_contexts() + 1u);
            if (link)
                link->fields->start();
            return true;
        }

        // --- GameWindow
        void poll() override
        {
            // raylib's PollInputEvents, step for step and in its order (runtime/host_input_state.h): the turn's
            // start, the pads in the first four joystick places (one GLFW has no pad layout for reads as a pad with
            // nothing pressed), then the system's events.
            m_input.beginPoll();
            for (int index = 0; index < ps2x::host::input_numbers::kMaxPads; ++index)
            {
                const bool present = glfwJoystickPresent(index) == GLFW_TRUE;
                GLFWgamepadstate state{};
                ps2x::host::RawPad raw{};
                if (present)
                {
                    glfwGetGamepadState(index, &state);
                    std::memcpy(raw.buttons, state.buttons, sizeof(raw.buttons));
                    std::memcpy(raw.axes, state.axes, sizeof(raw.axes));
                }
                m_input.pad(index, present, present ? &raw : nullptr);
            }
            glfwPollEvents();
        }

        bool closeAsked() const override
        {
            return glfwWindowShouldClose(m_platform.window) != 0;
        }

        ps2x::host::Devices &devices() override
        {
            return *this;
        }

        GameWindowShown show(uint64_t maxField, const GameWindowLook &look) override
        {
            GameWindowShown shown = take(maxField, look);
            present(shown, look);
            return shown;
        }

        GameWindowShown take(uint64_t maxField, const GameWindowLook &look) override
        {
            Util::register_thread_index(0);
            GameWindowShown shown;
            m_uiDrawn = false; // a new turn: the overlays of the turn before are no longer the window's content
            // A field's turn: an in-between picture chosen in a turn that could not present goes back unshown.
            if (m_timedNext && m_pictureLink)
                m_pictureLink->pictures->release(m_timedNext);
            m_timedNext = {};
            m_presentTimed = false;
            // The newest finished picture up to the field asked for, on the graphics card or given from main memory:
            // whichever was finished later, if it was finished after the one the window shows. A handed picture
            // shown until now goes back to the renderer: the last command buffer that used it was sent when the
            // window was last presented.
            HandedPicture next;
            if (m_link)
                next = m_link->fields->takeNewest(maxField);
            if (next && (next.order <= m_shownOrder || (m_nextMemory.have && next.order < m_nextMemory.order)))
            {
                m_link->fields->release(next); // empties it
                ++m_counts.olderNotShown;
            }
            if (next)
            {
                if (m_nextMemory.have)
                {
                    m_nextMemory.have = false;
                    ++m_counts.olderNotShown;
                }
                if (m_current)
                    m_link->fields->release(m_current);
                m_current = std::move(next);
                m_showMemory = false;
                m_shownOrder = m_current.order;
                shown.newPicture = true;
                ++m_counts.newPictures;
            }
            else if (m_nextMemory.have)
            {
                if (m_current)
                    m_link->fields->release(m_current);
                std::swap(m_memory, m_nextMemory); // the picture shown before keeps its buffer for the next one
                m_nextMemory.have = false;
                m_memoryToUpload = true;
                m_showMemory = true;
                m_shownOrder = m_memory.order;
                shown.newPicture = true;
                ++m_counts.newPictures;
                ++m_counts.fromMemory;
            }
            describe(shown, look);
            place(shown, look);
            // Once the overlays exist they get a frame in every turn: ImGui takes the window's events as they come,
            // and a frame is what uses them up.
            if (m_uiReady)
                beginUiFrame();
            return shown;
        }

        void present(GameWindowShown &shown, const GameWindowLook &look) override
        {
            Util::register_thread_index(0);
            // The overlays drawn since take() are complete now, whether or not the window can be presented.
            ImDrawData *overlays = endUiFrame();

            uint32_t width = 0u, height = 0u;
            drawableSize(width, height);
            const auto started = std::chrono::steady_clock::now();
            const auto msSince = [](std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to) {
                return std::chrono::duration<float, std::milli>(to - from).count();
            };
            if (width == 0u || height == 0u || !m_wsi.begin_frame())
            {
                // No size (minimised) or no swap chain image: nothing is presented; the game goes on.
                ++m_counts.notPresented;
                return;
            }
            if (m_reportsOn)
                collectReports(); // the presenter looked for new ones while it took the image
            const auto acquired = std::chrono::steady_clock::now();
            Vulkan::Device &device = m_wsi.get_device();
            auto cmd = device.request_command_buffer();
            uploadMemory(*cmd);
            prepareOverlays(*cmd, overlays);
            Vulkan::RenderPassInfo pass = device.get_swapchain_render_pass(Vulkan::SwapchainRenderPass::ColorOnly);
            pass.clear_color[0].float32[0] = 0.0f;
            pass.clear_color[0].float32[1] = 0.0f;
            pass.clear_color[0].float32[2] = 0.0f;
            pass.clear_color[0].float32[3] = 1.0f;
            cmd->begin_render_pass(pass);
            const float targetWidth = static_cast<float>(device.get_swapchain_view().get_image().get_width());
            const float targetHeight = static_cast<float>(device.get_swapchain_view().get_image().get_height());
            drawPicture(*cmd, look, targetWidth, targetHeight, shown, m_presentTimed ? &m_timedNext : nullptr);
            drawOverlays(*cmd, overlays);
            cmd->end_render_pass();
            device.submit(cmd);
            // The in-between picture presented before this one goes back to the renderer: the last command buffer
            // that drew it was sent in an earlier turn.
            if (m_timedShown && m_pictureLink)
                m_pictureLink->pictures->release(m_timedShown);
            m_timedShown = {};
            if (m_presentTimed)
                m_timedShown = std::move(m_timedNext);
            m_timedNext = {};
            m_lastPresentedTimed = m_presentTimed;
            m_presentTimed = false;
            const auto drawn = std::chrono::steady_clock::now();
            const double calledMs = steadyMs();
            shown.presented = m_wsi.end_frame();
            if (shown.presented)
            {
                shown.present = m_wsi.get_last_submitted_present_id();
                if (m_reportsOn)
                {
                    m_called.push_back({shown.present, calledMs, 0.0, 0.0});
                    while (m_called.size() > 64u)
                        m_called.pop_front();
                }
            }
            shown.acquireMs = msSince(started, acquired);
            shown.drawMs = msSince(acquired, drawn);
            shown.presentMs = msSince(drawn, std::chrono::steady_clock::now());
            m_views->nextFrame();
            if (shown.presented)
                ++m_counts.presented;
            else
                ++m_counts.notPresented;
            m_counts.swapChains = m_platform.chains;
        }

        bool picture(const GameWindowLook &look, std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height) override
        {
            Util::register_thread_index(0);
            drawableSize(width, height);
            rgba.clear();
            if (width == 0u || height == 0u)
                return false;
            Vulkan::Device &device = m_wsi.get_device();
            auto info = Vulkan::ImageCreateInfo::render_target(width, height, VK_FORMAT_R8G8B8A8_UNORM);
            info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            Vulkan::ImageHandle target = device.create_image(info);
            Vulkan::BufferCreateInfo bufferInfo{};
            bufferInfo.size = static_cast<VkDeviceSize>(width) * height * 4u;
            bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            bufferInfo.domain = Vulkan::BufferDomain::CachedHost;
            Vulkan::BufferHandle read = device.create_buffer(bufferInfo);
            if (!target || !read)
                return false;

            ImDrawData *overlays = endUiFrame();
            auto cmd = device.request_command_buffer();
            uploadMemory(*cmd);
            prepareOverlays(*cmd, overlays);
            cmd->image_barrier(*target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            Vulkan::RenderPassInfo pass;
            pass.num_color_attachments = 1;
            pass.color_attachments[0] = &target->get_view();
            pass.clear_attachments = 1u;
            pass.store_attachments = 1u;
            pass.clear_color[0].float32[3] = 1.0f;
            cmd->begin_render_pass(pass);
            GameWindowShown shown;
            const HandedPicture *timed = m_lastPresentedTimed && m_timedShown ? &m_timedShown : nullptr;
            if (timed)
            {
                shown.have = true;
                shown.displayWidth = timed->displayWidth ? timed->displayWidth : timed->width;
                shown.displayHeight = timed->displayHeight ? timed->displayHeight : timed->height;
            }
            else
            {
                describe(shown, look);
            }
            drawPicture(*cmd, look, static_cast<float>(width), static_cast<float>(height), shown, timed);
            drawOverlays(*cmd, overlays);
            cmd->end_render_pass();
            cmd->image_barrier(*target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            cmd->copy_image_to_buffer(*read, *target, 0, {}, {width, height, 1}, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
            cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                         VK_ACCESS_2_HOST_READ_BIT);
            Vulkan::Fence fence;
            device.submit(cmd, &fence);
            fence->wait();
            const void *mapped = device.map_host_buffer(*read, Vulkan::MEMORY_ACCESS_READ_BIT);
            if (!mapped)
                return false;
            rgba.resize(static_cast<size_t>(width) * height * 4u);
            std::memcpy(rgba.data(), mapped, rgba.size());
            device.unmap_host_buffer(*read, Vulkan::MEMORY_ACCESS_READ_BIT);
            for (size_t i = 3u; i < rgba.size(); i += 4u)
                rgba[i] = 0xFFu;
            return true;
        }

        void memoryPicture(const GameWindowPixels &picture) override
        {
            if (!picture.rgba || picture.width == 0u || picture.height == 0u)
                return;
            const size_t rowBytes = static_cast<size_t>(picture.width) * 4u;
            const size_t stride = picture.strideBytes ? picture.strideBytes : rowBytes;
            if (stride < rowBytes)
                return;
            // Finished before the picture the window shows, or before one that is waiting to be shown.
            if (picture.order <= m_shownOrder || (m_nextMemory.have && picture.order <= m_nextMemory.order))
            {
                ++m_counts.olderNotShown;
                return;
            }
            if (m_nextMemory.have)
                ++m_counts.olderNotShown; // given, and replaced before the window was drawn again
            MemoryPicture &next = m_nextMemory;
            next.rgba.resize(rowBytes * picture.height);
            for (uint32_t y = 0; y < picture.height; ++y)
                std::memcpy(next.rgba.data() + rowBytes * y, picture.rgba + stride * y, rowBytes);
            next.width = picture.width;
            next.height = picture.height;
            next.displayWidth = picture.displayWidth ? picture.displayWidth : picture.width;
            next.displayHeight = picture.displayHeight ? picture.displayHeight : picture.height;
            next.field = picture.field;
            next.order = picture.order;
            next.have = true;
        }

        bool shownPixels(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height) override
        {
            rgba.clear();
            width = height = 0u;
            if (m_showMemory)
            {
                if (!m_memory.have)
                    return false;
                rgba = m_memory.rgba;
                width = m_memory.width;
                height = m_memory.height;
            }
            else
            {
                // A picture on the graphics card: copied out of its image by the presenter's device object. The
                // window holds the picture, so the renderer does not write the image meanwhile.
                if (!m_current)
                    return false;
                Util::register_thread_index(0);
                const Vulkan::ImageView *view = m_views->view(m_current);
                if (!view)
                    return false;
                const Vulkan::Image &image = view->get_image();
                Vulkan::Device &device = m_wsi.get_device();
                Vulkan::BufferCreateInfo bufferInfo{};
                bufferInfo.size = static_cast<VkDeviceSize>(m_current.width) * m_current.height * 4u;
                bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                bufferInfo.domain = Vulkan::BufferDomain::CachedHost;
                Vulkan::BufferHandle read = device.create_buffer(bufferInfo);
                if (!read)
                    return false;
                auto cmd = device.request_command_buffer();
                cmd->image_barrier(image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
                                   VK_ACCESS_2_TRANSFER_READ_BIT);
                cmd->copy_image_to_buffer(*read, image, 0, {}, {m_current.width, m_current.height, 1}, 0, 0,
                                          {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
                // Back to the layout the picture is sampled in, here and by whoever takes it next.
                cmd->image_barrier(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                   VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                             VK_ACCESS_2_HOST_READ_BIT);
                Vulkan::Fence fence;
                device.submit(cmd, &fence);
                fence->wait();
                const void *mapped = device.map_host_buffer(*read, Vulkan::MEMORY_ACCESS_READ_BIT);
                if (!mapped)
                    return false;
                rgba.resize(static_cast<size_t>(m_current.width) * m_current.height * 4u);
                std::memcpy(rgba.data(), mapped, rgba.size());
                device.unmap_host_buffer(*read, Vulkan::MEMORY_ACCESS_READ_BIT);
                width = m_current.width;
                height = m_current.height;
            }
            // Opaque, as a picture read back by the renderer is (a handed picture keeps the PS2 frame buffer's alpha).
            for (size_t i = 3u; i < rgba.size(); i += 4u)
                rgba[i] = 0xFFu;
            return true;
        }

        // --- In-between pictures (step 5)
        void attachTimedPictures(const std::shared_ptr<GSVulkanPresenterLink> &link) override
        {
            Util::register_thread_index(0);
            if (m_pictureLink && m_pictureLink->pictures)
            {
                m_pictureLink->pictures->stop();
                for (TimedHeld &held : m_timed)
                    m_pictureLink->pictures->release(held.picture);
                m_pictureLink->pictures->release(m_timedNext);
                m_pictureLink->pictures->release(m_timedShown);
            }
            if (m_pictureLink && m_pictureLink->pictures)
                for (TimedWaiting &waiting : m_timedWaiting)
                    m_pictureLink->pictures->release(waiting.picture);
            m_timedWaiting.clear();
            {
                // What the waiting thread still holds or finishes belongs to the hand-over before: not counted.
                std::lock_guard<std::mutex> lock(m_finishMutex);
                m_finishQueue.clear();
                m_finished.clear();
                ++m_finishEpoch;
            }
            m_timed.clear();
            m_timedNext = {};
            m_timedShown = {};
            m_presentTimed = false;
            m_lastPresentedTimed = false;
            m_pictureLink = link && link->pictures ? link : nullptr;
            if (m_pictureLink)
                m_pictureLink->pictures->start();
        }

        void onTimedFinished(std::function<void()> finished) override
        {
            std::lock_guard<std::mutex> lock(m_finishMutex);
            m_onTimedFinished = std::move(finished);
        }

        void takeTimed(std::vector<GameWindowTimed> &fresh) override
        {
            if (!m_pictureLink)
                return;
            // Handed over: the window holds it, and a thread of the window's own waits for the card to finish it.
            while (HandedPicture picture = m_pictureLink->pictures->takeOldest())
            {
                TimedWaiting waiting;
                waiting.timed.id = m_nextTimedId++;
                waiting.timed.tag = picture.tag;
                waiting.timed.order = picture.order;
                waiting.timed.width = picture.width;
                waiting.timed.height = picture.height;
                waiting.timed.displayWidth = picture.displayWidth ? picture.displayWidth : picture.width;
                waiting.timed.displayHeight = picture.displayHeight ? picture.displayHeight : picture.height;
                {
                    std::lock_guard<std::mutex> lock(m_finishMutex);
                    if (picture.done)
                        m_finishQueue.push_back(FinishWait{picture.renderer, waiting.timed.id, m_finishEpoch, picture.done});
                    else
                        m_finished.push_back({waiting.timed.id, steadyMs()}); // no signal: taken as finished
                    if (!m_finishThread.joinable())
                        m_finishThread = std::thread([this] { waitForFinished(); });
                }
                m_finishWake.notify_one();
                waiting.picture = std::move(picture);
                m_timedWaiting.push_back(std::move(waiting));
            }
            // Finished since the last call, oldest first: the caller's from here on.
            std::deque<std::pair<uint64_t, double>> finished;
            {
                std::lock_guard<std::mutex> lock(m_finishMutex);
                finished.swap(m_finished);
            }
            for (const auto &done : finished)
            {
                for (size_t i = 0; i < m_timedWaiting.size(); ++i)
                {
                    if (m_timedWaiting[i].timed.id != done.first)
                        continue;
                    m_timedWaiting[i].timed.finishedMs = done.second;
                    fresh.push_back(m_timedWaiting[i].timed);
                    m_timed.push_back(TimedHeld{done.first, std::move(m_timedWaiting[i].picture)});
                    m_timedWaiting.erase(m_timedWaiting.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
            }
        }

        void dropTimed(uint64_t id) override
        {
            for (size_t i = 0; i < m_timed.size(); ++i)
            {
                if (m_timed[i].id != id)
                    continue;
                m_pictureLink->pictures->release(m_timed[i].picture);
                m_timed.erase(m_timed.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
        }

        GameWindowShown takeTimedPicture(uint64_t id, const GameWindowLook &look) override
        {
            Util::register_thread_index(0);
            GameWindowShown shown;
            m_uiDrawn = false;
            // One chosen in a turn that could not present goes back unshown.
            if (m_timedNext && m_pictureLink)
                m_pictureLink->pictures->release(m_timedNext);
            m_timedNext = {};
            m_presentTimed = false;
            for (size_t i = 0; i < m_timed.size(); ++i)
            {
                if (m_timed[i].id != id)
                    continue;
                m_timedNext = std::move(m_timed[i].picture);
                m_timed.erase(m_timed.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
            if (m_timedNext)
            {
                m_presentTimed = true;
                shown.have = true;
                shown.newPicture = true;
                shown.timed = true;
                shown.tag = m_timedNext.tag;
                shown.field = m_timedNext.field;
                shown.order = m_timedNext.order;
                shown.width = m_timedNext.width;
                shown.height = m_timedNext.height;
                shown.displayWidth = m_timedNext.displayWidth ? m_timedNext.displayWidth : m_timedNext.width;
                shown.displayHeight = m_timedNext.displayHeight ? m_timedNext.displayHeight : m_timedNext.height;
                ++m_counts.newPictures;
                ++m_counts.timed;
            }
            place(shown, look);
            if (m_uiReady)
                beginUiFrame();
            return shown;
        }

        void drawableSize(uint32_t &width, uint32_t &height) const override
        {
            int w = 0, h = 0;
            glfwGetFramebufferSize(m_platform.window, &w, &h);
            width = static_cast<uint32_t>(std::max(w, 0));
            height = static_cast<uint32_t>(std::max(h, 0));
        }

        void resize(int width, int height) override
        {
            glfwSetWindowSize(m_platform.window, std::max(width, 1), std::max(height, 1));
        }

        bool hidden() const override
        {
            return m_hidden;
        }

        bool askShownReports(bool on) override
        {
            on = on && !m_hidden;
            if (on == m_reportsOn)
                return on;
            m_reportsOn = on;
            m_wsi.set_enable_timing_feedback(on);
            m_called.clear();
            m_reports.clear();
            if (on)
            {
                // The driver's times come on the library's clock: where that lies on the PC's steady clock (the
                // closest of five looks at both).
                double narrowest = DBL_MAX;
                for (int look = 0; look < 5; ++look)
                {
                    const double before = steadyMs();
                    const double library = static_cast<double>(Util::get_current_time_nsecs()) * 1e-6;
                    const double after = steadyMs();
                    if (after - before < narrowest)
                    {
                        narrowest = after - before;
                        m_libraryClockMs = (before + after) * 0.5 - library;
                    }
                }
            }
            return on;
        }

        void takeShownReports(std::vector<GameWindowReport> &out, double &refreshMs, bool &saysFollows) override
        {
            out.assign(m_reports.begin(), m_reports.end());
            m_reports.clear();
            refreshMs = m_refreshMs;
            saysFollows = m_saysFollows;
        }

        GameWindowCounts counts() const override
        {
            GameWindowCounts counts = m_counts;
            counts.swapChains = m_platform.chains;
            return counts;
        }

        // --- host::Window: what the pages and overlays ask of the window (runtime/host_window.h)
        ps2x::host::Window &control() override
        {
            return *this;
        }

        bool uiSetup(bool dark) override
        {
#ifdef PS2X_HAS_WINDOW_UI
            ensureUi();
            if (dark)
                ImGui::StyleColorsDark();
            else
                ImGui::StyleColorsLight();
#else
            (void)dark;
#endif
            return true;
        }

        bool uiShutdown() override
        {
            closeUi();
            return true;
        }

        bool uiBegin() override
        {
            beginUiFrame();
            return true;
        }

        // Nothing to do: the frame is closed and drawn when the window is presented.
        bool uiEnd() override
        {
            return true;
        }

        uint64_t makeTexture(const uint8_t *rgba, int width, int height) override
        {
#ifdef PS2X_HAS_WINDOW_UI
            ensureUi();
            Util::register_thread_index(0);
            return m_ui->makeTexture(rgba, width, height);
#else
            (void)rgba;
            (void)width;
            (void)height;
            return 0u;
#endif
        }

        void dropTexture(uint64_t texture) override
        {
#ifdef PS2X_HAS_WINDOW_UI
            if (m_ui)
                m_ui->dropTexture(texture);
#else
            (void)texture;
#endif
        }

        // The lines over the picture are ImGui's too: under the pages on its background list, over them on its
        // foreground list. Window pixels become ImGui's units by the display's scale.
        void fillRect(Layer layer, int x, int y, int width, int height, ps2x::host::Colour colour) override
        {
#ifdef PS2X_HAS_WINDOW_UI
            beginUiFrame();
            const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
            const float sx = scale.x > 0.0f ? 1.0f / scale.x : 1.0f, sy = scale.y > 0.0f ? 1.0f / scale.y : 1.0f;
            list(layer)->AddRectFilled(ImVec2(static_cast<float>(x) * sx, static_cast<float>(y) * sy),
                                       ImVec2(static_cast<float>(x + width) * sx, static_cast<float>(y + height) * sy),
                                       IM_COL32(colour.r, colour.g, colour.b, colour.a));
#else
            (void)layer; (void)x; (void)y; (void)width; (void)height; (void)colour;
#endif
        }

        void text(Layer layer, int x, int y, int size, ps2x::host::Colour colour, const char *text) override
        {
#ifdef PS2X_HAS_WINDOW_UI
            if (!text || !*text)
                return;
            beginUiFrame();
            const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
            const float sx = scale.x > 0.0f ? 1.0f / scale.x : 1.0f, sy = scale.y > 0.0f ? 1.0f / scale.y : 1.0f;
            list(layer)->AddText(ImGui::GetFont(), static_cast<float>(size) * sy,
                                 ImVec2(static_cast<float>(x) * sx, static_cast<float>(y) * sy),
                                 IM_COL32(colour.r, colour.g, colour.b, colour.a), text);
#else
            (void)layer; (void)x; (void)y; (void)size; (void)colour; (void)text;
#endif
        }

        int textWidth(int size, const char *text) override
        {
#ifdef PS2X_HAS_WINDOW_UI
            if (!text || !*text)
                return 0;
            beginUiFrame();
            const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
            const float sy = scale.y > 0.0f ? 1.0f / scale.y : 1.0f, toPixels = scale.x > 0.0f ? scale.x : 1.0f;
            const float units = ImGui::GetFont()->CalcTextSizeA(static_cast<float>(size) * sy, FLT_MAX, 0.0f, text).x;
            return static_cast<int>(std::ceil(units * toPixels));
#else
            (void)size; (void)text;
            return 0;
#endif
        }

        bool maximized() const override
        {
            return glfwGetWindowAttrib(m_platform.window, GLFW_MAXIMIZED) == GLFW_TRUE;
        }

        bool fullscreen() const override
        {
            return m_borderless;
        }

        // Borderless and the size of its display, by the steps raylib takes for today's window
        // (ToggleBorderlessWindowed): no decoration, on top, at the display's corner with the display's size; back:
        // decorated again, the size before, then the place before.
        void setFullscreen(bool on) override
        {
            if (on == m_borderless)
                return;
            GLFWmonitor *monitor = currentMonitor();
            const GLFWvidmode *mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
            if (!mode)
                return;
            if (on)
            {
                glfwGetWindowPos(m_platform.window, &m_before[0], &m_before[1]);
                glfwGetWindowSize(m_platform.window, &m_before[2], &m_before[3]);
                glfwSetWindowAttrib(m_platform.window, GLFW_DECORATED, GLFW_FALSE);
                glfwSetWindowAttrib(m_platform.window, GLFW_FLOATING, GLFW_TRUE);
                int x = 0, y = 0;
                glfwGetMonitorPos(monitor, &x, &y);
                glfwSetWindowPos(m_platform.window, x, y);
                glfwSetWindowSize(m_platform.window, mode->width, mode->height);
            }
            else
            {
                glfwSetWindowAttrib(m_platform.window, GLFW_FLOATING, GLFW_FALSE);
                glfwSetWindowAttrib(m_platform.window, GLFW_DECORATED, GLFW_TRUE);
                glfwSetWindowSize(m_platform.window, m_before[2], m_before[3]);
                glfwSetWindowPos(m_platform.window, m_before[0], m_before[1]);
            }
            if (!m_hidden) // a window nobody sees takes no focus from the one in use
                glfwFocusWindow(m_platform.window);
            m_borderless = on;
        }

        bool vsync() const override
        {
            return m_vsync;
        }

        // The swap chain is made again for the other way of presenting, between two frames.
        void setVsync(bool on) override
        {
            if (on == m_vsync)
                return;
            m_vsync = on;
            Util::register_thread_index(0);
            m_wsi.set_present_mode(on ? Vulkan::PresentMode::SyncToVBlank : Vulkan::PresentMode::UnlockedMaybeTear);
        }

        void screenSize(int &width, int &height) const override
        {
            glfwGetWindowSize(m_platform.window, &width, &height);
        }

        void renderSize(int &width, int &height) const override
        {
            glfwGetFramebufferSize(m_platform.window, &width, &height);
        }

        void setScreenSize(int width, int height) override
        {
            glfwSetWindowSize(m_platform.window, std::max(width, 1), std::max(height, 1));
        }

        void position(int &x, int &y) const override
        {
            glfwGetWindowPos(m_platform.window, &x, &y);
        }

        void setPosition(int x, int y) override
        {
            glfwSetWindowPos(m_platform.window, x, y);
        }

        int displayWidth() const override
        {
            GLFWmonitor *monitor = currentMonitor();
            const GLFWvidmode *mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
            return mode ? mode->width : 0;
        }

        std::vector<std::string> takeDroppedFiles() override
        {
            std::vector<std::string> files;
            files.swap(m_dropped);
            return files;
        }

        // --- host::Devices: the answers raylib gives for its window, from this window's events (InputState)
        bool ready() const override { return true; }
        bool focused() const override { return m_input.focused(); }
        double seconds() const override { return glfwGetTime(); }
        bool keyDown(int key) const override { return m_input.keyDown(key); }
        bool keyPressed(int key) const override { return m_input.keyPressed(key); }
        bool keyPressedRepeat(int key) const override { return m_input.keyPressedRepeat(key); }
        bool padAvailable(int pad) const override { return m_input.padAvailable(pad); }
        bool padButtonDown(int pad, int button) const override { return m_input.padButtonDown(pad, button); }
        bool padButtonPressed(int pad, int button) const override { return m_input.padButtonPressed(pad, button); }
        float padAxis(int pad, int axis) const override { return m_input.padAxis(pad, axis); }
        bool mouseButtonDown(int button) const override { return m_input.mouseButtonDown(button); }
        float mouseWheel() const override { return m_input.mouseWheel(); }
        void mouseDelta(float &x, float &y) const override { m_input.mouseDelta(x, y); }
        void captureMouse(bool captured) override
        {
            // The calls and their order are raylib's DisableCursor() and EnableCursor(): the cursor's mode, the
            // cursor put in the middle of the window, raw mouse movement where the system has it.
            glfwSetInputMode(m_platform.window, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
            int w = 0, h = 0;
            glfwGetWindowSize(m_platform.window, &w, &h);
            m_input.mouseCaptured(captured, w, h);
            glfwSetCursorPos(m_platform.window, static_cast<double>(m_input.mouseX()), static_cast<double>(m_input.mouseY()));
            if (glfwRawMouseMotionSupported())
                glfwSetInputMode(m_platform.window, GLFW_RAW_MOUSE_MOTION, captured ? GLFW_TRUE : GLFW_FALSE);
        }

    private:
        // The display the window's middle lies on (raylib's GetCurrentMonitor for a window that is not in exclusive
        // fullscreen); the first display when it lies on none.
        GLFWmonitor *currentMonitor() const
        {
            int count = 0;
            GLFWmonitor **monitors = glfwGetMonitors(&count);
            if (!monitors || count < 1)
                return nullptr;
            int x = 0, y = 0, w = 0, h = 0;
            glfwGetWindowPos(m_platform.window, &x, &y);
            glfwGetWindowSize(m_platform.window, &w, &h);
            const int cx = x + w / 2, cy = y + h / 2;
            for (int i = 0; i < count; ++i)
            {
                int mx = 0, my = 0;
                glfwGetMonitorPos(monitors[i], &mx, &my);
                const GLFWvidmode *mode = glfwGetVideoMode(monitors[i]);
                if (mode && cx >= mx && cx <= mx + mode->width - 1 && cy >= my && cy <= my + mode->height - 1)
                    return monitors[i];
            }
            return monitors[0];
        }

#ifdef PS2X_HAS_WINDOW_UI
        // The overlays' context, fonts and renderer, made when something first asks for them. The default font as
        // rlImGui sets it up for today's window: 13 pixels times the display's scale.
        void ensureUi()
        {
            if (m_uiReady)
                return;
            Util::register_thread_index(0);
            IMGUI_CHECKVERSION();
            if (!ImGui::GetCurrentContext())
            {
                ImGui::CreateContext();
                m_uiContextOwned = true;
            }
            ImGuiIO &io = ImGui::GetIO();
            float scaleX = 1.0f, scaleY = 1.0f;
            glfwGetWindowContentScale(m_platform.window, &scaleX, &scaleY);
            if (!(scaleY > 0.0f))
                scaleY = 1.0f;
            ImFontConfig font;
            font.SizePixels = std::ceil(13.0f * scaleY);
            font.RasterizerMultiply = scaleY;
            font.PixelSnapH = true;
            io.Fonts->AddFontDefault(&font);
            ImGui::StyleColorsDark();
            // ImGui's own GLFW input: its callbacks are put before this window's and call them on.
            ImGui_ImplGlfw_InitForVulkan(m_platform.window, true);
            io.BackendRendererName = "dw5xl_window";
            io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
            m_ui = std::make_unique<ps2x::gs::VulkanUiRenderer>(m_wsi.get_device());
            m_uiReady = true;
        }

        void beginUiFrame()
        {
            ensureUi();
            if (m_uiFrame)
                return;
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            m_uiFrame = true;
        }

        // Closes the frame if one is open. What to draw over the picture in this turn, or null.
        ImDrawData *endUiFrame()
        {
            if (m_uiFrame)
            {
                ImGui::Render();
                m_uiFrame = false;
                m_uiDrawn = true;
            }
            return m_uiReady && m_uiDrawn ? ImGui::GetDrawData() : nullptr;
        }

        void closeUi()
        {
            if (!m_uiReady)
                return;
            Util::register_thread_index(0);
            if (m_uiFrame)
            {
                ImGui::EndFrame();
                m_uiFrame = false;
            }
            m_uiDrawn = false;
            m_wsi.get_device().wait_idle();
            m_ui.reset();
            ImGui_ImplGlfw_Shutdown();
            if (m_uiContextOwned)
                ImGui::DestroyContext();
            m_uiContextOwned = false;
            m_uiReady = false;
        }

        static ImDrawList *list(Layer layer)
        {
            return layer == Layer::UnderPages ? ImGui::GetBackgroundDrawList() : ImGui::GetForegroundDrawList();
        }
#else
        struct ImDrawData;
        void beginUiFrame() {}
        ImDrawData *endUiFrame() { return nullptr; }
        void closeUi() {}
#endif

        void prepareOverlays(Vulkan::CommandBuffer &cmd, ImDrawData *overlays)
        {
#ifdef PS2X_HAS_WINDOW_UI
            if (overlays && m_ui)
                m_ui->prepare(cmd, overlays);
#else
            (void)cmd;
            (void)overlays;
#endif
        }

        void drawOverlays(Vulkan::CommandBuffer &cmd, ImDrawData *overlays)
        {
#ifdef PS2X_HAS_WINDOW_UI
            if (overlays && m_ui)
                m_ui->draw(cmd, overlays);
#else
            (void)cmd;
            (void)overlays;
#endif
        }

        static Window *self(GLFWwindow *window)
        {
            return static_cast<Window *>(glfwGetWindowUserPointer(window));
        }

        // A picture given from main memory: its pixels (rows width * 4 bytes apart) and what it stands for.
        struct MemoryPicture
        {
            std::vector<uint8_t> rgba;
            uint32_t width = 0u, height = 0u;
            uint32_t displayWidth = 0u, displayHeight = 0u;
            uint64_t field = 0u;
            uint64_t order = 0u;
            bool have = false;
        };

        // The shown picture from main memory into its image on the graphics card, in the command buffer that
        // draws it next (before the render pass). The image is the window's own and is written again for the next
        // picture of the same size: the queue runs that after every earlier draw of it.
        void uploadMemory(Vulkan::CommandBuffer &cmd)
        {
            if (!m_showMemory || !m_memoryToUpload || !m_memory.have)
                return;
            Vulkan::Device &device = m_wsi.get_device();
            if (!m_memoryImage || m_memoryImage->get_width() != m_memory.width || m_memoryImage->get_height() != m_memory.height)
            {
                auto info = Vulkan::ImageCreateInfo::immutable_2d_image(m_memory.width, m_memory.height, VK_FORMAT_R8G8B8A8_UNORM);
                info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
                info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
                m_memoryImage = device.create_image(info);
                if (!m_memoryImage)
                    return;
                device.set_name(*m_memoryImage, "Picture from main memory");
            }
            // What the image held is dropped; the barrier's first half waits for the last sampling of it.
            cmd.image_barrier(*m_memoryImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
                              VK_ACCESS_2_TRANSFER_WRITE_BIT);
            if (void *rows = cmd.update_image(*m_memoryImage))
                std::memcpy(rows, m_memory.rgba.data(), m_memory.rgba.size());
            cmd.image_barrier(*m_memoryImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            m_memoryToUpload = false;
        }

        void describe(GameWindowShown &shown, const GameWindowLook &) const
        {
            if (m_showMemory)
            {
                shown.have = m_memory.have;
                if (!shown.have)
                    return;
                shown.fromMemory = true;
                shown.field = m_memory.field;
                shown.order = m_memory.order;
                shown.width = m_memory.width;
                shown.height = m_memory.height;
                shown.displayWidth = m_memory.displayWidth;
                shown.displayHeight = m_memory.displayHeight;
                return;
            }
            shown.have = static_cast<bool>(m_current);
            if (!shown.have)
                return;
            shown.field = m_current.field;
            shown.order = m_current.order;
            shown.width = m_current.width;
            shown.height = m_current.height;
            shown.displayWidth = m_current.displayWidth ? m_current.displayWidth : m_current.width;
            shown.displayHeight = m_current.displayHeight ? m_current.displayHeight : m_current.height;
        }

        // Inside a render pass cleared to black: the picture letterboxed to its shape, by the one function every
        // window uses (runtime/picture_rect.h).
        // Where the picture `shown` describes will lie in the window, for overlays drawn before it is presented.
        void place(GameWindowShown &shown, const GameWindowLook &look) const
        {
            uint32_t drawableWidth = 0u, drawableHeight = 0u;
            drawableSize(drawableWidth, drawableHeight);
            if (!shown.have || drawableWidth == 0u || drawableHeight == 0u)
                return;
            const ps2x::PictureRect placed = ps2x::pictureRectInWindow(shown.displayWidth, shown.displayHeight, look.shape,
                                                                      static_cast<float>(drawableWidth),
                                                                      static_cast<float>(drawableHeight));
            shown.x = placed.x;
            shown.y = placed.y;
            shown.rectWidth = placed.width;
            shown.rectHeight = placed.height;
        }

        // `timed`: the in-between picture to draw; null: the field's own picture the window holds.
        void drawPicture(Vulkan::CommandBuffer &cmd, const GameWindowLook &look, float targetWidth, float targetHeight,
                         GameWindowShown &shown, const HandedPicture *timed = nullptr)
        {
            const Vulkan::ImageView *view = nullptr;
            if (timed)
                view = *timed ? m_views->view(*timed) : nullptr;
            else if (m_showMemory)
                view = m_memory.have && m_memoryImage && !m_memoryToUpload ? &m_memoryImage->get_view() : nullptr;
            else if (m_current)
                view = m_views->view(m_current);
            if (!view)
                return;
            const ps2x::PictureRect placed = ps2x::pictureRectInWindow(shown.displayWidth, shown.displayHeight, look.shape,
                                                                      targetWidth, targetHeight);
            shown.x = placed.x;
            shown.y = placed.y;
            shown.rectWidth = placed.width;
            shown.rectHeight = placed.height;
            m_drawer->draw(cmd, *view, placed.x, placed.y, placed.width, placed.height, look.smooth, look.sharpen);
        }

        // The presenter keeps the newest report it found; a second one that came in the same turn is not seen
        // (the clock is made for reports with gaps).
        void collectReports()
        {
            Vulkan::PresentationStats stats = {};
            if (m_wsi.get_presentation_stats(stats) && stats.present_done_ts != 0u && stats.feedback_present_id != m_lastReported)
            {
                m_lastReported = stats.feedback_present_id;
                while (!m_called.empty() && m_called.front().present < stats.feedback_present_id)
                    m_called.pop_front();
                if (!m_called.empty() && m_called.front().present == stats.feedback_present_id)
                {
                    GameWindowReport report = m_called.front();
                    report.outMs = static_cast<double>(stats.present_done_ts) * 1e-6 + m_libraryClockMs;
                    report.gpuDoneMs = stats.gpu_done_ts != 0u ? static_cast<double>(stats.gpu_done_ts) * 1e-6 + m_libraryClockMs : 0.0;
                    m_reports.push_back(report);
                    m_called.pop_front();
                }
            }
            Vulkan::RefreshRateInfo info = {};
            if (m_wsi.get_refresh_rate_info(info))
            {
                m_refreshMs = static_cast<double>(info.refresh_duration) * 1e-6;
                m_saysFollows = info.mode == Vulkan::RefreshMode::VRR;
            }
        }

        std::shared_ptr<GSVulkanPresenterLink> m_link; // declared first: it goes last
        bool m_glfw = false;
        bool m_started = false;
        bool m_hidden = false;
        Platform m_platform;
        Vulkan::WSI m_wsi;
        std::unique_ptr<ps2x::gs::VulkanPictureDrawer> m_drawer;
        std::unique_ptr<HandedPictureViews> m_views;
        HandedPicture m_current; // the picture the window shows, when it is one on the graphics card
        MemoryPicture m_memory;     // ... when it is one from main memory (m_showMemory)
        MemoryPicture m_nextMemory; // given since the window was last drawn, not shown yet
        Vulkan::ImageHandle m_memoryImage; // m_memory on the graphics card, an image of the presenter's device object
        bool m_showMemory = false;
        bool m_memoryToUpload = false;
        uint64_t m_shownOrder = 0u; // the shown picture's place among all finished pictures
        // In-between pictures (step 5): the link of the renderer memory that draws them; the ones handed over and
        // not presented yet; the one chosen for this turn; the one presented last.
        struct TimedHeld
        {
            uint64_t id = 0u;
            HandedPicture picture;
        };
        // Handed over, the graphics card not finished with it yet (takeTimed gives it out when it is).
        struct TimedWaiting
        {
            GameWindowTimed timed;
            HandedPicture picture;
        };
        std::vector<TimedWaiting> m_timedWaiting;
        // The thread that waits for the card, one picture after the other (they finish in the order they were
        // sent), notes the time and tells the caller (onTimedFinished).
        struct FinishWait
        {
            std::shared_ptr<void> renderer; // the device object the signal belongs to (declared first: it goes last)
            uint64_t id = 0u;
            uint64_t epoch = 0u;
            Vulkan::Fence done;
        };
        void waitForFinished()
        {
            std::unique_lock<std::mutex> lock(m_finishMutex);
            for (;;)
            {
                m_finishWake.wait(lock, [this] { return m_finishStop || !m_finishQueue.empty(); });
                if (m_finishStop)
                    return;
                FinishWait next = std::move(m_finishQueue.front());
                m_finishQueue.pop_front();
                lock.unlock();
                next.done->wait();
                const double when = steadyMs();
                next.done.reset();
                next.renderer.reset();
                lock.lock();
                if (next.epoch != m_finishEpoch)
                    continue; // of a hand-over the window has left
                m_finished.push_back({next.id, when});
                const std::function<void()> tell = m_onTimedFinished;
                lock.unlock();
                if (tell)
                    tell();
                lock.lock();
            }
        }
        std::mutex m_finishMutex;
        std::condition_variable m_finishWake;
        std::deque<FinishWait> m_finishQueue;
        std::deque<std::pair<uint64_t, double>> m_finished; // a picture's number and when the card had finished it
        std::function<void()> m_onTimedFinished;
        std::thread m_finishThread;
        uint64_t m_finishEpoch = 0u;
        bool m_finishStop = false;
        std::shared_ptr<GSVulkanPresenterLink> m_pictureLink;
        std::vector<TimedHeld> m_timed;
        HandedPicture m_timedNext, m_timedShown;
        bool m_presentTimed = false;       // this turn's present draws m_timedNext
        bool m_lastPresentedTimed = false; // the window's content is m_timedShown
        uint64_t m_nextTimedId = 1u;
        ps2x::host::InputState m_input;
        GameWindowCounts m_counts;
        // The driver's reports of shown pictures (askShownReports): the presents not reported yet with the time
        // of their hand-over, the reports not taken yet, and what the driver says of the display.
        bool m_reportsOn = false;
        std::deque<GameWindowReport> m_called;
        std::deque<GameWindowReport> m_reports;
        uint64_t m_lastReported = 0u;
        double m_libraryClockMs = 0.0;
        double m_refreshMs = 0.0;
        bool m_saysFollows = false;
        // What the pages and overlays asked of the window.
        bool m_vsync = true;
        bool m_borderless = false;
        int m_before[4] = {0, 0, 640, 480}; // the window's place and size before it went borderless
        std::vector<std::string> m_dropped;
        bool m_uiReady = false;        // the overlays' context and renderer exist
        bool m_uiContextOwned = false;
        bool m_uiFrame = false;        // a frame of overlays is open
        bool m_uiDrawn = false;        // a frame of overlays was closed in this turn: it is part of the window's content
#ifdef PS2X_HAS_WINDOW_UI
        std::unique_ptr<ps2x::gs::VulkanUiRenderer> m_ui;
#endif
    };
} // namespace

namespace ps2x::gs
{
    std::unique_ptr<GameWindow> GameWindow::open(const std::shared_ptr<GSVulkanPresenterLink> &link,
                                                 const GameWindowOptions &options, std::string &why)
    {
        why.clear();
        auto window = std::make_unique<Window>();
        if (!window->start(link, options, why))
            return nullptr;
        return window;
    }
} // namespace ps2x::gs
