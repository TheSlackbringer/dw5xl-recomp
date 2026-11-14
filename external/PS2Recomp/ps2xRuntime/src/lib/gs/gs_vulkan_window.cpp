// SPDX-License-Identifier: GPL-3.0-only
// DW5XL (issue 049, amendment v3.12 step 3 part 4): the game's window drawn by the renderer itself.
// See runtime/gs/gs_game_window.h. The window is GLFW's without OpenGL; the swap chain is the library's presenter
// (Granite's Vulkan::WSI) with a device object of its own on the renderer's context, as step 1's test window
// measured it (tools/present_probe): the presenter then never waits for the renderer's open command buffers.

#include "runtime/gs/gs_game_window.h"

#include "gs_vulkan_handover.h"
#include "gs_vulkan_window.h"
#include "runtime/host_devices.h"
#include "runtime/host_input_state.h"
#include "runtime/picture_rect.h"

#include "context.hpp"
#include "device.hpp"
#include "thread_id.hpp"
#include "wsi.hpp"
#include "shader.hpp"
#include "shaders/slangmosh_iface.hpp"
#include "shaders/slangmosh.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>

namespace ps2x::gs
{
    VulkanPictureDrawer::VulkanPictureDrawer(Vulkan::Device &device)
    {
        // The library's own two shaders for a picture on a quad: three corners that cover the viewport, and one
        // sample of the picture at each pixel.
        Vulkan::ResourceLayout layout;
        ParallelGS::Shaders<> suite(device, layout, 0);
        m_blit = device.request_program(suite.quad, suite.blit);
    }

    void VulkanPictureDrawer::draw(Vulkan::CommandBuffer &cmd, const Vulkan::ImageView &picture, float x, float y,
                                   float width, float height, bool smooth) const
    {
        if (!m_blit || !(width > 0.0f) || !(height > 0.0f))
            return;
        cmd.set_opaque_state();
        cmd.set_depth_test(false, false);
        cmd.set_cull_mode(VK_CULL_MODE_NONE);
        cmd.set_program(m_blit);
        cmd.set_texture(0, 0, picture, smooth ? Vulkan::StockSampler::LinearClamp : Vulkan::StockSampler::NearestClamp);
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
    using ps2x::gs::GameWindowShown;

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

    class Window final : public ps2x::gs::GameWindow, public ps2x::host::Devices
    {
    public:
        Window() = default;

        ~Window() override
        {
            if (m_started)
            {
                Util::register_thread_index(0);
                if (m_current)
                    m_link->fields->release(m_current);
                m_link->fields->stop();
                m_wsi.get_device().wait_idle();
                m_views.reset();
                m_drawer.reset();
                m_wsi.teardown();
            }
            if (m_platform.window)
                glfwDestroyWindow(m_platform.window);
            if (m_glfw)
                glfwDown();
        }

        bool start(const std::shared_ptr<GSVulkanPresenterLink> &link, const GameWindowOptions &options, std::string &why)
        {
            if (!link || !link->context || !link->queues || !link->fields)
            {
                why = "the renderer has nothing to present with";
                return false;
            }
            if (!link->windowSupport)
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

            // The presenter: a device object of its own on the renderer's context, the queues shared through the
            // renderer's lock.
            Util::register_thread_index(0);
            m_wsi.set_platform(&m_platform);
            m_wsi.set_present_mode(options.vsync ? Vulkan::PresentMode::SyncToVBlank : Vulkan::PresentMode::UnlockedMaybeTear);
            m_wsi.set_backbuffer_format(Vulkan::BackbufferFormat::UNORM); // the picture's bytes as they are
            if (!m_wsi.init_from_existing_context(link->context) || !m_wsi.init_device())
            {
                why = "the presenter did not start on the renderer's graphics device";
                return false;
            }
            m_started = true;
            Vulkan::Device &device = m_wsi.get_device();
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
            Util::register_thread_index(0);
            GameWindowShown shown;
            // The newest finished picture up to the field asked for. The one shown until now goes back to the
            // renderer: the last command buffer that used it was sent when the window was last presented.
            if (HandedPicture next = m_link->fields->takeNewest(maxField))
            {
                if (m_current)
                    m_link->fields->release(m_current);
                m_current = std::move(next);
                shown.newPicture = true;
                ++m_counts.newPictures;
            }
            describe(shown, look);

            uint32_t width = 0u, height = 0u;
            drawableSize(width, height);
            if (width == 0u || height == 0u || !m_wsi.begin_frame())
            {
                // No size (minimised) or no swap chain image: nothing is presented; the game goes on.
                ++m_counts.notPresented;
                return shown;
            }
            Vulkan::Device &device = m_wsi.get_device();
            auto cmd = device.request_command_buffer();
            Vulkan::RenderPassInfo pass = device.get_swapchain_render_pass(Vulkan::SwapchainRenderPass::ColorOnly);
            pass.clear_color[0].float32[0] = 0.0f;
            pass.clear_color[0].float32[1] = 0.0f;
            pass.clear_color[0].float32[2] = 0.0f;
            pass.clear_color[0].float32[3] = 1.0f;
            cmd->begin_render_pass(pass);
            const float targetWidth = static_cast<float>(device.get_swapchain_view().get_image().get_width());
            const float targetHeight = static_cast<float>(device.get_swapchain_view().get_image().get_height());
            drawPicture(*cmd, look, targetWidth, targetHeight, shown);
            cmd->end_render_pass();
            device.submit(cmd);
            shown.presented = m_wsi.end_frame();
            m_views->nextFrame();
            if (shown.presented)
                ++m_counts.presented;
            else
                ++m_counts.notPresented;
            m_counts.swapChains = m_platform.chains;
            return shown;
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

            auto cmd = device.request_command_buffer();
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
            describe(shown, look);
            drawPicture(*cmd, look, static_cast<float>(width), static_cast<float>(height), shown);
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

        GameWindowCounts counts() const override
        {
            GameWindowCounts counts = m_counts;
            counts.swapChains = m_platform.chains;
            return counts;
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
        static Window *self(GLFWwindow *window)
        {
            return static_cast<Window *>(glfwGetWindowUserPointer(window));
        }

        void describe(GameWindowShown &shown, const GameWindowLook &) const
        {
            shown.have = static_cast<bool>(m_current);
            if (!shown.have)
                return;
            shown.field = m_current.field;
            shown.width = m_current.width;
            shown.height = m_current.height;
            shown.displayWidth = m_current.displayWidth ? m_current.displayWidth : m_current.width;
            shown.displayHeight = m_current.displayHeight ? m_current.displayHeight : m_current.height;
        }

        // Inside a render pass cleared to black: the picture letterboxed to its shape, by the one function every
        // window uses (runtime/picture_rect.h).
        void drawPicture(Vulkan::CommandBuffer &cmd, const GameWindowLook &look, float targetWidth, float targetHeight,
                         GameWindowShown &shown)
        {
            if (!m_current)
                return;
            const Vulkan::ImageView *view = m_views->view(m_current);
            if (!view)
                return;
            const ps2x::PictureRect placed = ps2x::pictureRectInWindow(shown.displayWidth, shown.displayHeight, look.shape,
                                                                      targetWidth, targetHeight);
            shown.x = placed.x;
            shown.y = placed.y;
            shown.rectWidth = placed.width;
            shown.rectHeight = placed.height;
            m_drawer->draw(cmd, *view, placed.x, placed.y, placed.width, placed.height, look.smooth);
        }

        std::shared_ptr<GSVulkanPresenterLink> m_link; // declared first: it goes last
        bool m_glfw = false;
        bool m_started = false;
        bool m_hidden = false;
        Platform m_platform;
        Vulkan::WSI m_wsi;
        std::unique_ptr<ps2x::gs::VulkanPictureDrawer> m_drawer;
        std::unique_ptr<HandedPictureViews> m_views;
        HandedPicture m_current; // the picture the window shows
        ps2x::host::InputState m_input;
        GameWindowCounts m_counts;
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
