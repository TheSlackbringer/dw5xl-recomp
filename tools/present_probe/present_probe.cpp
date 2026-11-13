// DW5XL-DEBUG (issue 049, P2.9 step 1): a test window, not the game.
//
// It opens a window through the renderer library's own presenter (Granite's Vulkan::WSI) and shows a moving test
// pattern at chosen instants. It measures two things the port needs to know before its window is rebuilt:
//   1. How the system shows such a window: a plain Vulkan swap chain or (Windows) one through DXGI, vsync on or off,
//      normal window or borderless; and what the driver reports about when each picture was shown.
//   2. How a presenter and a renderer take turns on one graphics device. A second thread (the "producer") stands in
//      for the GS thread: it draws the pictures, keeps a command buffer open while it works, sends extra work, and
//      moves its frame contexts on by the port's rule (issue 049, amendment v3.10: only when that does not wait).
//      --devices 2 (default): the presenter has a device object of its own on the same graphics device.
//      --devices 1: one device object for both, the presenter moving the frame contexts on as the library does.
// Nothing here is game code or game data. See README.md for the options and the output.

#include "device.hpp"
#include "context.hpp"
#include "wsi.hpp"
#include "thread_id.hpp"
#include "timer.hpp"
#include "shader.hpp"
#include "shaders/slangmosh_iface.hpp"
#include "shaders/slangmosh.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <timeapi.h>
#include <dwmapi.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
int64_t nowNs()
{
    return Util::get_current_time_nsecs();
}

void sleepUntil(int64_t t)
{
    for (;;)
    {
        const int64_t left = t - nowNs();
        if (left <= 0)
            return;
        if (left > 2'000'000)
            std::this_thread::sleep_for(std::chrono::nanoseconds(left - 1'500'000));
        else
            std::this_thread::yield();
    }
}

struct Options
{
    double seconds = 10.0;
    bool hidden = false;
    bool borderless = false;
    int windowW = 1280, windowH = 960;
    Vulkan::PresentMode mode = Vulkan::PresentMode::SyncToVBlank;
    std::string modeName = "vsync";
    bool dxgi = false;
    double rate = 50.0;            // pictures a second
    unsigned pictureW = 2560, pictureH = 2048;
    int devices = 2;
    double openMs = 4.0;           // how long the producer keeps a command buffer open for each picture
    int load = 4;                  // extra pieces of work the producer sends for each picture
    double leadMs = 10.0;          // a picture is made this long before its instant
    double handMs = 1.0;           // a picture is handed to the presenter this long before its instant
    bool target = false;           // ask the driver for a target time for each picture
    int latency = -1;              // present wait latency; -1 = the library's default
    bool lowLatency = false;
    bool perPicture = false;       // print a line for every picture
    bool feedback = true;          // ask the driver when each picture was shown
};

[[noreturn]] void usage(const char *why)
{
    std::fprintf(stderr,
                 "%s\nusage: dw5xl_present_probe [--seconds N] [--hidden] [--borderless] [--size WxH]\n"
                 "  [--present vsync|unlocked|immediate|mailbox] [--dxgi] [--rate HZ] [--picture WxH]\n"
                 "  [--devices 1|2] [--open-ms X] [--load N] [--lead-ms X] [--hand-ms X] [--target]\n"
                 "  [--latency N] [--low-latency] [--per-picture] [--no-feedback]\n",
                 why);
    std::exit(2);
}

#ifdef _WIN32
// The display's own refreshes during a run (Windows): a thread waits for each vertical blank of the screen the window
// is on and notes the time. On a display that follows the pictures (G-Sync, FreeSync) the gaps are the pictures'
// gaps; on a display at a fixed rate they are that rate's period whatever is presented.
struct RefreshListener
{
    std::thread thread;
    std::atomic<bool> stop{false};
    std::vector<int64_t> times;
    const char *problem = nullptr;

    void start(HWND hwnd)
    {
        thread = std::thread([this, hwnd]
        {
            // The kernel's own wait for the next refresh of the screen the window is on (gdi32's D3DKMT calls, taken
            // by name so that no driver-kit header is needed).
            struct OpenAdapter { HDC hdc; UINT adapter; LUID luid; UINT source; };
            struct WaitRefresh { UINT adapter; UINT device; UINT source; };
            struct CloseAdapter { UINT adapter; };
            const HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
            auto open = reinterpret_cast<LONG(WINAPI *)(OpenAdapter *)>(
                reinterpret_cast<void *>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromHdc")));
            auto wait = reinterpret_cast<LONG(WINAPI *)(const WaitRefresh *)>(
                reinterpret_cast<void *>(GetProcAddress(gdi, "D3DKMTWaitForVerticalBlankEvent")));
            auto close = reinterpret_cast<LONG(WINAPI *)(const CloseAdapter *)>(
                reinterpret_cast<void *>(GetProcAddress(gdi, "D3DKMTCloseAdapter")));
            if (!open || !wait || !close)
            {
                problem = "the system has no refresh wait";
                return;
            }
            MONITORINFOEXW info = {};
            info.cbSize = sizeof(info);
            if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info))
            {
                problem = "no screen for the window";
                return;
            }
            const HDC hdc = CreateDCW(info.szDevice, info.szDevice, nullptr, nullptr);
            OpenAdapter opened = {};
            opened.hdc = hdc;
            if (!hdc || open(&opened) != 0)
            {
                problem = "the screen's adapter did not open";
                if (hdc)
                    DeleteDC(hdc);
                return;
            }
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            times.reserve(1 << 14);
            const WaitRefresh request = {opened.adapter, 0, opened.source};
            int tooFast = 0;
            int64_t last = nowNs();
            while (!stop.load())
            {
                if (wait(&request) != 0)
                {
                    problem = "the refresh wait was refused";
                    break;
                }
                const int64_t now = nowNs();
                // A wait that does not wait would spin a processor core: give up on it.
                tooFast = now - last < 50'000 ? tooFast + 1 : 0;
                if (tooFast > 200)
                {
                    problem = "the refresh wait does not wait";
                    times.clear();
                    break;
                }
                last = now;
                times.push_back(now);
            }
            const CloseAdapter closing = {opened.adapter};
            close(&closing);
            DeleteDC(hdc);
        });
    }

    void finish()
    {
        stop.store(true);
        if (thread.joinable())
            thread.join();
    }
};
#endif

// Whether the window can be seen at all: a run whose window is hidden behind another one, minimised or on a screen
// that is off measures nothing about the display. Read-only questions to the system; Windows only.
void windowReport(GLFWwindow *window, const char *when)
{
#ifdef _WIN32
    const HWND hwnd = glfwGetWin32Window(window);
    RECT r = {};
    GetWindowRect(hwnd, &r);
    const POINT centre = {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
    const HWND at = WindowFromPoint(centre);
    const HWND root = at ? GetAncestor(at, GA_ROOT) : nullptr;
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    // The compositor's own pace: one DwmFlush returns per composed frame while the screen is on.
    int64_t t[10];
    for (auto &v : t)
    {
        DwmFlush();
        v = nowNs();
    }
    double gaps[9];
    for (int i = 0; i < 9; ++i)
        gaps[i] = double(t[i + 1] - t[i]) * 1e-6;
    std::sort(gaps, gaps + 9);
    std::printf("[probe:window] when=%s shown=%d minimised=%d foreground=%d cloaked=%d on_top_at_its_centre=%d "
                "at=%ld,%ld size=%ldx%ld compositor_frame_ms=%.3f (shortest %.3f, longest %.3f)\n",
                when, IsWindowVisible(hwnd) ? 1 : 0, IsIconic(hwnd) ? 1 : 0, GetForegroundWindow() == hwnd ? 1 : 0,
                cloaked ? 1 : 0, root == hwnd ? 1 : 0, long(r.left), long(r.top), long(r.right - r.left),
                long(r.bottom - r.top), gaps[4], gaps[0], gaps[8]);
#else
    (void)window;
    (void)when;
#endif
}

Options parse(int argc, char **argv)
{
    Options o;
    auto value = [&](int &i) -> const char *
    {
        if (i + 1 >= argc)
            usage("missing value");
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--seconds") o.seconds = std::atof(value(i));
        else if (a == "--hidden") o.hidden = true;
        else if (a == "--borderless") o.borderless = true;
        else if (a == "--size") { if (std::sscanf(value(i), "%dx%d", &o.windowW, &o.windowH) != 2) usage("--size WxH"); }
        else if (a == "--picture") { if (std::sscanf(value(i), "%ux%u", &o.pictureW, &o.pictureH) != 2) usage("--picture WxH"); }
        else if (a == "--present")
        {
            o.modeName = value(i);
            if (o.modeName == "vsync") o.mode = Vulkan::PresentMode::SyncToVBlank;
            else if (o.modeName == "unlocked") o.mode = Vulkan::PresentMode::UnlockedMaybeTear;
            else if (o.modeName == "immediate") o.mode = Vulkan::PresentMode::UnlockedForceTearing;
            else if (o.modeName == "mailbox") o.mode = Vulkan::PresentMode::UnlockedNoTearing;
            else usage("--present vsync|unlocked|immediate|mailbox");
        }
        else if (a == "--dxgi") o.dxgi = true;
        else if (a == "--rate") o.rate = std::atof(value(i));
        else if (a == "--devices") o.devices = std::atoi(value(i));
        else if (a == "--open-ms") o.openMs = std::atof(value(i));
        else if (a == "--load") o.load = std::atoi(value(i));
        else if (a == "--lead-ms") o.leadMs = std::atof(value(i));
        else if (a == "--hand-ms") o.handMs = std::atof(value(i));
        else if (a == "--target") o.target = true;
        else if (a == "--latency") o.latency = std::atoi(value(i));
        else if (a == "--low-latency") o.lowLatency = true;
        else if (a == "--per-picture") o.perPicture = true;
        else if (a == "--no-feedback") o.feedback = false;
        else usage(("unknown option " + a).c_str());
    }
    if (o.devices != 1 && o.devices != 2) usage("--devices 1|2");
    if (o.rate < 1.0 || o.rate > 1000.0 || o.seconds <= 0.0 || o.load < 0) usage("bad number");
#ifndef _WIN32
    if (o.dxgi) usage("--dxgi is for Windows");
#endif
    return o;
}

// The window as the presenter sees it.
struct Platform final : Vulkan::WSIPlatform
{
    GLFWwindow *window = nullptr;
    bool dxgi = false;
    int surfaceCalls = 0;
    // What the presenter made (the last one).
    unsigned chainW = 0, chainH = 0;
    size_t chainImages = 0;
    VkFormat chainFormat = VK_FORMAT_UNDEFINED;
    bool chainIsVulkan = false;
    int chainsMade = 0;

    VkSurfaceKHR create_surface(VkInstance instance, VkPhysicalDevice) override
    {
        ++surfaceCalls;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (glfwCreateWindowSurface(instance, window, nullptr, &surface) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        return surface;
    }

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
        return uint32_t(w);
    }

    uint32_t get_surface_height() override
    {
        int w = 0, h = 0;
        glfwGetFramebufferSize(window, &w, &h);
        return uint32_t(h);
    }

    bool alive(Vulkan::WSI &) override
    {
        return !glfwWindowShouldClose(window);
    }

    void poll_input() override
    {
        glfwPollEvents();
    }

    void poll_input_async(Granite::InputTrackerHandler *) override
    {
    }

    uintptr_t get_native_window() override
    {
#ifdef _WIN32
        return dxgi ? reinterpret_cast<uintptr_t>(glfwGetWin32Window(window)) : 0;
#else
        return 0;
#endif
    }

    void event_swapchain_created(Vulkan::Device *device, VkSwapchainKHR swapchain, unsigned width, unsigned height,
                                 float aspect, size_t images, VkFormat format, VkColorSpaceKHR space,
                                 VkSurfaceTransformFlagBitsKHR rotate) override
    {
        Vulkan::WSIPlatform::event_swapchain_created(device, swapchain, width, height, aspect, images, format, space,
                                                     rotate);
        chainW = width;
        chainH = height;
        chainImages = images;
        chainFormat = format;
        chainIsVulkan = swapchain != VK_NULL_HANDLE;
        ++chainsMade;
    }

    void sizeChanged()
    {
        resize = true;
    }
};

struct Series
{
    std::vector<double> ms;
    void add(int64_t ns) { ms.push_back(double(ns) * 1e-6); }
    void print(const char *name) const
    {
        if (ms.empty())
        {
            std::printf("[probe:sum] %-28s n=0\n", name);
            return;
        }
        std::vector<double> s = ms;
        std::sort(s.begin(), s.end());
        auto at = [&](double q) { return s[std::min(s.size() - 1, size_t(q * double(s.size())))]; };
        size_t over2 = 0, over05 = 0;
        for (double v : s)
        {
            over2 += v > 2.0;
            over05 += v > 0.5;
        }
        std::printf("[probe:sum] %-28s n=%zu p50=%.3f p99=%.3f max=%.3f over0.5ms=%zu over2ms=%zu\n", name, s.size(),
                    at(0.50), at(0.99), s.back(), over05, over2);
    }
};

struct Slot
{
    Vulkan::ImageHandle image;      // the producer's
    Vulkan::ImageHandle wrapped;    // the same picture as the presenter's device object sees it (two devices)
    Vulkan::ImageViewHandle wrappedView;
    std::atomic<bool> busy{false};
};

struct Handed
{
    int slot = -1;
    uint64_t n = 0;
    int64_t wanted = 0;   // the picture's instant
    int64_t made = 0;     // when the producer had sent it
};

struct Made
{
    uint64_t n;
    int64_t wanted, start, sent, moved;
    bool contextMoved;
    bool skipped;
};

struct Shown
{
    uint64_t n;
    int64_t wanted, made, drain0, begin0, begin1, sent, end0, end1;
    uint64_t id;
    bool targetOk;
};

struct Feedback
{
    uint64_t id;
    uint64_t gpuDone, presentDone;
    int64_t error;
};
} // namespace

int main(int argc, char **argv)
{
    const Options o = parse(argc, argv);
#ifdef _WIN32
    timeBeginPeriod(1);
#endif
    std::setvbuf(stdout, nullptr, _IOFBF, 1 << 16);

    if (!glfwInit())
    {
        std::fprintf(stderr, "GLFW did not start\n");
        return 1;
    }
    if (!glfwVulkanSupported())
    {
        std::fprintf(stderr, "GLFW finds no Vulkan\n");
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, o.hidden ? GLFW_FALSE : GLFW_TRUE);
    int winW = o.windowW, winH = o.windowH, posX = -1, posY = -1;
    if (o.borderless)
    {
        GLFWmonitor *monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode *vm = monitor ? glfwGetVideoMode(monitor) : nullptr;
        if (!vm)
        {
            std::fprintf(stderr, "no monitor for a borderless window\n");
            return 1;
        }
        glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
        winW = vm->width;
        winH = vm->height;
        glfwGetMonitorPos(monitor, &posX, &posY);
    }
    Platform platform;
    platform.dxgi = o.dxgi;
    platform.window = glfwCreateWindow(winW, winH, "DW5XL present probe", nullptr, nullptr);
    if (!platform.window)
    {
        std::fprintf(stderr, "no window\n");
        return 1;
    }
    if (o.borderless)
        glfwSetWindowPos(platform.window, posX, posY);
    glfwSetWindowUserPointer(platform.window, &platform);
    glfwSetFramebufferSizeCallback(platform.window, [](GLFWwindow *w, int, int)
    {
        static_cast<Platform *>(glfwGetWindowUserPointer(w))->sizeChanged();
    });

    if (!Vulkan::Context::init_loader(nullptr))
    {
        std::fprintf(stderr, "no Vulkan loader\n");
        return 1;
    }

    int result = 0;
    {
        Vulkan::WSI wsi;
        wsi.set_platform(&platform);
        wsi.set_present_mode(o.mode);
        wsi.set_backbuffer_format(Vulkan::BackbufferFormat::UNORM);
        if (o.latency >= 0)
            wsi.set_present_wait_latency(uint32_t(o.latency));
        if (o.lowLatency)
            wsi.set_present_low_latency_mode(true);
        if (!wsi.init_context_from_platform(2, {}) || !wsi.init_device() || !wsi.init_surface_swapchain())
        {
            std::fprintf(stderr, "the presenter did not start\n");
            return 1;
        }
        wsi.set_enable_timing_feedback(o.feedback);

        Vulkan::Device &pdev = wsi.get_device();   // the presenter's device object
        Vulkan::DeviceHandle own;                  // the producer's, with --devices 2
        Vulkan::Device *dev = &pdev;
        std::mutex queueLock;                      // both device objects send to the same queues
        Series producerLock, presenterLock;
        if (o.devices == 2)
        {
            own = Util::make_handle<Vulkan::Device>();
            own->set_context(wsi.get_context());
            own->init_frame_contexts(16);
            dev = own.get();
            dev->set_queue_lock([&] { const int64_t t = nowNs(); queueLock.lock(); producerLock.add(nowNs() - t); },
                                [&] { queueLock.unlock(); });
            pdev.set_queue_lock([&] { const int64_t t = nowNs(); queueLock.lock(); presenterLock.add(nowNs() - t); },
                                [&] { queueLock.unlock(); });
        }
        else
            pdev.init_frame_contexts(16);

        Vulkan::ResourceLayout layout;
        ParallelGS::Shaders<> suite(pdev, layout, 0);
        Vulkan::Program *blit = pdev.request_program(suite.quad, suite.blit);

        // The pictures: a ring the producer draws into and the presenter samples from.
        constexpr int kSlots = 8;
        Slot slots[kSlots];
        auto pictureInfo = Vulkan::ImageCreateInfo::render_target(o.pictureW, o.pictureH, VK_FORMAT_R8G8B8A8_UNORM);
        pictureInfo.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        pictureInfo.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
        for (auto &slot : slots)
        {
            slot.image = dev->create_image(pictureInfo);
            if (!slot.image)
            {
                std::fprintf(stderr, "no picture memory\n");
                return 1;
            }
            if (o.devices == 2)
            {
                slot.wrapped = pdev.wrap_image(pictureInfo, slot.image->get_image());
                Vulkan::ImageViewCreateInfo view;
                view.image = slot.wrapped.get();
                view.format = pictureInfo.format;
                view.view_type = VK_IMAGE_VIEW_TYPE_2D;
                view.levels = 1;
                view.layers = 1;
                slot.wrappedView = pdev.create_image_view(view);
                if (!slot.wrappedView)
                {
                    std::fprintf(stderr, "no view of a shared picture\n");
                    return 1;
                }
            }
        }
        Vulkan::ImageHandle scratch = dev->create_image(pictureInfo);

        std::mutex handMutex;
        std::condition_variable handCond;
        std::deque<Handed> handed;
        std::atomic<bool> stop{false};
        std::vector<Made> made;
        Series producerWork, producerContext;
        uint64_t producerHeld = 0, producerNoSlot = 0;

        for (int i = 0; i < 10; ++i)
        {
            glfwPollEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        windowReport(platform.window, "before");
#ifdef _WIN32
        RefreshListener refreshes;
        if (!o.hidden)
            refreshes.start(glfwGetWin32Window(platform.window));
#endif
        const int64_t start = nowNs() + 300'000'000;   // the time line begins 0.3 s from now
        const int64_t period = int64_t(1e9 / o.rate);
        const int64_t end = start + int64_t(o.seconds * 1e9);

        auto draw = [&](Vulkan::CommandBuffer &cmd, Vulkan::Image &image, uint64_t n, int64_t wanted, int bars)
        {
            cmd.image_barrier(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            Vulkan::RenderPassInfo rp;
            rp.num_color_attachments = 1;
            rp.color_attachments[0] = &image.get_view();
            rp.clear_attachments = 1u;
            rp.store_attachments = 1u;
            rp.clear_color[0].float32[0] = 0.05f;
            rp.clear_color[0].float32[1] = 0.05f;
            rp.clear_color[0].float32[2] = (n & 1u) ? 0.16f : 0.08f;
            rp.clear_color[0].float32[3] = 1.0f;
            cmd.begin_render_pass(rp);
            const unsigned w = image.get_width(), h = image.get_height();
            // A bar that crosses the picture once every two seconds, placed by the picture's instant, so that even
            // motion on screen means every picture was shown at its instant.
            const double phase = double((wanted - start) % 2'000'000'000) / 2e9;
            const unsigned barW = std::max(8u, w / 40u);
            VkClearValue white = {};
            white.color.float32[0] = white.color.float32[1] = white.color.float32[2] = white.color.float32[3] = 1.0f;
            for (int i = 0; i < bars; ++i)
            {
                const double p = phase + double(i) / double(bars);
                const unsigned x = unsigned((p - std::floor(p)) * double(w - barW));
                VkClearRect rect = {};
                rect.rect.offset = {int32_t(x), 0};
                rect.rect.extent = {barW, h};
                rect.layerCount = 1;
                cmd.clear_quad(0, rect, white);
            }
            // A square that changes sides with every picture: a picture shown twice or left out shows as a flicker.
            VkClearValue mark = {};
            mark.color.float32[0] = 1.0f;
            mark.color.float32[1] = 0.6f;
            mark.color.float32[3] = 1.0f;
            VkClearRect square = {};
            square.rect.offset = {int32_t((n & 1u) ? w / 2u : w / 2u - h / 8u), int32_t(h - h / 8u)};
            square.rect.extent = {h / 8u, h / 8u};
            square.layerCount = 1;
            cmd.clear_quad(0, square, mark);
            cmd.end_render_pass();
            cmd.image_barrier(image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        };

        // Where each thread is, for the watchdog: a run that hangs says where, instead of saying nothing.
        std::atomic<const char *> presenterAt{"waiting for a picture"}, producerAt{"waiting for its time"};
        std::atomic<uint64_t> shownSoFar{0}, madeSoFar{0};
        std::atomic<bool> finished{false};
        std::thread watchdog([&]
        {
            while (!finished.load() && nowNs() < end + 5'000'000'000ll)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (finished.load())
                return;
            std::printf("[probe:hang] five seconds after the end of the time line: presenter at \"%s\", producer at "
                        "\"%s\"; made=%llu shown=%llu\n",
                        presenterAt.load(), producerAt.load(), static_cast<unsigned long long>(madeSoFar.load()),
                        static_cast<unsigned long long>(shownSoFar.load()));
            std::fflush(stdout);
            std::_Exit(3);
        });

        std::thread producer([&]
        {
            Util::register_thread_index(o.devices == 1 ? 1u : 0u);
            for (uint64_t n = 0; !stop.load(); ++n)
            {
                const int64_t wanted = start + int64_t(n) * period;
                if (wanted >= end)
                    break;
                producerAt.store("waiting for its time");
                sleepUntil(wanted - int64_t(o.leadMs * 1e6));
                producerAt.store("drawing");
                int slot = -1;
                for (int i = 0; i < kSlots; ++i)
                {
                    const int candidate = int((n + uint64_t(i)) % kSlots);
                    bool expected = false;
                    if (slots[candidate].busy.compare_exchange_strong(expected, true))
                    {
                        slot = candidate;
                        break;
                    }
                }
                if (slot < 0)
                {
                    ++producerNoSlot;
                    made.push_back({n, wanted, 0, 0, 0, false, true});
                    continue;
                }
                const int64_t t0 = nowNs();
                auto cmd = dev->request_command_buffer();
                draw(*cmd, *slots[slot].image, n, wanted, 1);
                // The GS thread has commands open while it works on a list.
                const int64_t openFrom = nowNs();
                sleepUntil(openFrom + int64_t(o.openMs * 1e6));
                const int64_t openTo = nowNs();
                producerAt.store("sending");
                dev->submit(cmd);
                for (int i = 0; i < o.load; ++i)
                {
                    auto extra = dev->request_command_buffer();
                    draw(*extra, *scratch, n, wanted, 8);
                    dev->submit(extra);
                }
                dev->flush_frame();
                const int64_t t1 = nowNs();
                {
                    std::lock_guard<std::mutex> lock(handMutex);
                    handed.push_back({slot, n, wanted, t1});
                }
                handCond.notify_one();
                // The port's rule for its GS memories (amendment v3.10): move the frame context on only when that
                // does not wait for the graphics card.
                bool moved = false;
                producerAt.store("frame context");
                if (dev->next_frame_context_is_non_blocking())
                {
                    dev->next_frame_context();
                    moved = true;
                }
                else
                    ++producerHeld;
                const int64_t t2 = nowNs();
                producerWork.add((t1 - t0) - (openTo - openFrom));
                producerContext.add(t2 - t1);
                made.push_back({n, wanted, t0, t1, t2, moved, false});
                madeSoFar.fetch_add(1);
            }
            producerAt.store("finished");
            stop.store(true);
            handCond.notify_all();
        });

        std::vector<Shown> shown;
        std::vector<Feedback> feedback;
        std::vector<std::pair<int, Vulkan::Fence>> inFlight;
        Series drain, begin, record, present, handLate;
        uint64_t skipped = 0, beginFailed = 0, targetOk = 0, lastFeedback = 0;
        Vulkan::RefreshRateInfo refresh = {};
        bool haveRefresh = false;

        auto retire = [&](bool all)
        {
            for (auto it = inFlight.begin(); it != inFlight.end();)
            {
                if (all)
                    it->second->wait();
                if (all || it->second->wait_timeout(0))
                {
                    slots[it->first].busy.store(false);
                    it = inFlight.erase(it);
                }
                else
                    ++it;
            }
        };

        while (platform.alive(wsi))
        {
            glfwPollEvents();
            Handed pic;
            {
                std::unique_lock<std::mutex> lock(handMutex);
                handCond.wait_for(lock, std::chrono::milliseconds(20), [&] { return !handed.empty() || stop.load(); });
                if (handed.empty())
                {
                    if (stop.load())
                        break;
                    continue;
                }
                pic = handed.front();
                handed.pop_front();
                // A picture whose successor is due already is left out.
                while (!handed.empty() && handed.front().wanted - int64_t(o.handMs * 1e6) <= nowNs())
                {
                    slots[pic.slot].busy.store(false);
                    ++skipped;
                    pic = handed.front();
                    handed.pop_front();
                }
            }
            retire(false);
            presenterAt.store("waiting for the picture's time");
            sleepUntil(pic.wanted - int64_t(o.handMs * 1e6));

            Shown s = {};
            s.n = pic.n;
            s.wanted = pic.wanted;
            s.made = pic.made;
            s.drain0 = nowNs();
            presenterAt.store("waiting for open command buffers");
            // Waits until no command buffer is open on this device object, which the library's begin_frame does first.
            (void)pdev.next_frame_context_is_non_blocking();
            s.begin0 = nowNs();
            presenterAt.store("begin_frame");
            if (!wsi.begin_frame())
            {
                ++beginFailed;
                slots[pic.slot].busy.store(false);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            s.begin1 = nowNs();
            presenterAt.store("drawing");

            Vulkan::PresentationStats stats = {};
            if (wsi.get_presentation_stats(stats) && stats.feedback_present_id != lastFeedback)
            {
                lastFeedback = stats.feedback_present_id;
                feedback.push_back({stats.feedback_present_id, stats.gpu_done_ts, stats.present_done_ts, stats.error});
            }
            if (!haveRefresh)
                haveRefresh = wsi.get_refresh_rate_info(refresh);

            auto cmd = pdev.request_command_buffer();
            auto rp = pdev.get_swapchain_render_pass(Vulkan::SwapchainRenderPass::ColorOnly);
            rp.clear_color[0].float32[3] = 1.0f;
            const Vulkan::ImageView &view = o.devices == 2 ? *slots[pic.slot].wrappedView : slots[pic.slot].image->get_view();
            cmd->begin_render_pass(rp);
            const float cw = float(pdev.get_swapchain_view().get_image().get_width());
            const float ch = float(pdev.get_swapchain_view().get_image().get_height());
            float vw = cw, vh = cw * 3.0f / 4.0f;
            if (vh > ch)
            {
                vh = ch;
                vw = ch * 4.0f / 3.0f;
            }
            cmd->set_opaque_state();
            cmd->set_depth_test(false, false);
            cmd->set_cull_mode(VK_CULL_MODE_NONE);
            cmd->set_program(blit);
            cmd->set_texture(0, 0, view, Vulkan::StockSampler::LinearClamp);
            cmd->set_viewport({(cw - vw) * 0.5f, (ch - vh) * 0.5f, vw, vh, 0.0f, 1.0f});
            cmd->draw(3);
            cmd->end_render_pass();
            Vulkan::Fence fence;
            presenterAt.store("sending");
            pdev.submit(cmd, &fence);
            s.sent = nowNs();
            if (o.target)
            {
                s.targetOk = wsi.set_target_presentation_time(uint64_t(pic.wanted), 0, false);
                targetOk += s.targetOk;
            }
            s.end0 = nowNs();
            presenterAt.store("end_frame");
            const bool ended = wsi.end_frame();
            s.end1 = nowNs();
            s.id = wsi.get_last_submitted_present_id();
            inFlight.emplace_back(pic.slot, std::move(fence));
            if (!ended)
                ++beginFailed;
            drain.add(s.begin0 - s.drain0);
            begin.add(s.begin1 - s.begin0);
            record.add(s.sent - s.begin1);
            present.add(s.end1 - s.end0);
            handLate.add(s.end1 - s.wanted);
            shown.push_back(s);
            shownSoFar.fetch_add(1);
            presenterAt.store("waiting for a picture");
        }

#ifdef _WIN32
        refreshes.finish();
        {
            // The gaps between the display's refreshes while pictures were shown (from 0.5 s into the time line).
            Series gaps;
            for (size_t i = 1; i < refreshes.times.size(); ++i)
                if (refreshes.times[i - 1] >= start + 500'000'000 && refreshes.times[i] <= end)
                    gaps.add(refreshes.times[i] - refreshes.times[i - 1]);
            if (gaps.ms.empty())
                std::printf("[probe:display] refreshes=0%s%s\n", refreshes.problem ? " problem=" : "",
                            refreshes.problem ? refreshes.problem : "");
            else
            {
                std::vector<double> s = gaps.ms;
                std::sort(s.begin(), s.end());
                auto at = [&](double q) { return s[std::min(s.size() - 1, size_t(q * double(s.size())))]; };
                std::printf("[probe:display] refreshes=%zu gap_ms p5=%.3f p50=%.3f p95=%.3f longest=%.3f\n", s.size() + 1,
                            at(0.05), at(0.50), at(0.95), s.back());
            }
        }
#endif
        windowReport(platform.window, "after");
        stop.store(true);
        presenterAt.store("joining the producer");
        producer.join();
        presenterAt.store("waiting for the graphics card at the end");
        retire(true);
        dev->wait_idle();
        pdev.wait_idle();
        finished.store(true);
        watchdog.join();

        // The report.
        std::printf("[probe:setup] gpu=\"%s\" window=%dx%d%s%s present=%s asked=%s chain=%ux%u images=%zu format=%d "
                    "chains_made=%d surface_calls=%d\n",
                    wsi.get_context().get_gpu_props().deviceName, winW, winH, o.borderless ? " borderless" : "",
                    o.hidden ? " hidden" : "", platform.chainIsVulkan ? "vulkan" : "dxgi", o.dxgi ? "dxgi" : "vulkan",
                    platform.chainW, platform.chainH, platform.chainImages, int(platform.chainFormat),
                    platform.chainsMade, platform.surfaceCalls);
        std::printf("[probe:setup] mode=%s rate=%.3f picture=%ux%u devices=%d open_ms=%.2f load=%d lead_ms=%.2f "
                    "hand_ms=%.2f target=%d latency=%d low_latency=%d seconds=%.1f\n",
                    o.modeName.c_str(), o.rate, o.pictureW, o.pictureH, o.devices, o.openMs, o.load, o.leadMs, o.handMs,
                    o.target ? 1 : 0, o.latency, o.lowLatency ? 1 : 0, o.seconds);
        const auto &features = pdev.get_device_features();
        std::printf("[probe:driver] present_id=%d present_id2=%d refresh_known=%d refresh_mode=%s refresh_ns=%llu "
                    "interval_ns=%llu feedback=%zu\n",
                    features.present_id_features.presentId ? 1 : 0, features.present_id2_features.presentId2 ? 1 : 0,
                    haveRefresh ? 1 : 0,
                    !haveRefresh ? "-" : refresh.mode == Vulkan::RefreshMode::VRR ? "variable" :
                    refresh.mode == Vulkan::RefreshMode::FRR ? "fixed" : "unknown",
                    static_cast<unsigned long long>(haveRefresh ? refresh.refresh_duration : 0u),
                    static_cast<unsigned long long>(haveRefresh ? refresh.refresh_interval : 0u), feedback.size());

        size_t madeCount = 0;
        for (const auto &m : made)
            madeCount += !m.skipped;
        std::printf("[probe:count] made=%zu no_free_picture=%llu shown=%zu left_out=%llu presenter_failed=%llu "
                    "producer_context_held=%llu target_accepted=%llu\n",
                    madeCount, static_cast<unsigned long long>(producerNoSlot), shown.size(),
                    static_cast<unsigned long long>(skipped), static_cast<unsigned long long>(beginFailed),
                    static_cast<unsigned long long>(producerHeld), static_cast<unsigned long long>(targetOk));

        // When the driver says a picture left for the display, against its instant.
        Series shownAfterWanted, shownAfterPresent, driverError;
        for (const auto &f : feedback)
        {
            if (!f.presentDone)
                continue;
            for (const auto &s : shown)
                if (s.id == f.id)
                {
                    shownAfterWanted.add(int64_t(f.presentDone) - s.wanted);
                    shownAfterPresent.add(int64_t(f.presentDone) - s.end1);
                    break;
                }
            if (f.error)
                driverError.add(f.error);
        }

        std::printf("[probe:sum] times in ms\n");
        producerWork.print("producer: draw and send");
        producerContext.print("producer: frame context");
        producerLock.print("producer: queue lock wait");
        drain.print("presenter: open buffers wait");
        begin.print("presenter: begin_frame");
        record.print("presenter: draw and send");
        present.print("presenter: end_frame");
        presenterLock.print("presenter: queue lock wait");
        handLate.print("presented after instant");
        shownAfterWanted.print("driver: out after instant");
        shownAfterPresent.print("driver: out after present");
        driverError.print("driver: target error");

        if (o.perPicture)
        {
            for (const auto &m : made)
                if (!m.skipped)
                    std::printf("[probe:made] n=%llu wanted=%.3f start=%.3f sent=%.3f moved_at=%.3f context_moved=%d\n",
                                static_cast<unsigned long long>(m.n), double(m.wanted - start) * 1e-6,
                                double(m.start - start) * 1e-6, double(m.sent - start) * 1e-6,
                                double(m.moved - start) * 1e-6, m.contextMoved ? 1 : 0);
            for (const auto &s : shown)
                std::printf("[probe:shown] n=%llu id=%llu wanted=%.3f made=%.3f begin=%.3f acquired=%.3f sent=%.3f "
                            "presented=%.3f target=%d\n",
                            static_cast<unsigned long long>(s.n), static_cast<unsigned long long>(s.id),
                            double(s.wanted - start) * 1e-6, double(s.made - start) * 1e-6,
                            double(s.begin0 - start) * 1e-6, double(s.begin1 - start) * 1e-6,
                            double(s.sent - start) * 1e-6, double(s.end1 - start) * 1e-6, s.targetOk ? 1 : 0);
            for (const auto &f : feedback)
                std::printf("[probe:feedback] id=%llu gpu_done=%.3f out=%.3f error_ms=%.3f\n",
                            static_cast<unsigned long long>(f.id),
                            f.gpuDone ? double(int64_t(f.gpuDone) - start) * 1e-6 : -1.0,
                            f.presentDone ? double(int64_t(f.presentDone) - start) * 1e-6 : -1.0, double(f.error) * 1e-6);
        }
        std::printf("[probe:done] shown=%zu\n", shown.size());
        std::fflush(stdout);
        if (shown.empty())
            result = 1;

        // The pictures go before the device objects, the producer's device object before the presenter and its context.
        for (auto &slot : slots)
        {
            slot.wrappedView.reset();
            slot.wrapped.reset();
            slot.image.reset();
        }
        scratch.reset();
        own.reset();
        wsi.teardown();
    }

    glfwDestroyWindow(platform.window);
    glfwTerminate();
#ifdef _WIN32
    timeEndPeriod(1);
#endif
    return result;
}
