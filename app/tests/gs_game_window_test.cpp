// DW5XL-TEST issue 049 (amendment v3.12 step 3 parts 4 and 5): the game's window drawn by the renderer itself
// (runtime/gs/gs_game_window.h). The window is made hidden: nothing appears on the screen. No game data.
// At render scale 1 and 4, with two drawings A and B read back the old way as the yardstick:
//   The drawing alone (VulkanPictureDrawer, into a picture of a device object of the test's own):
//   1. a handed picture drawn one to one is the read picture pixel for pixel;
//   2. drawn with room left and right: black there, the picture between pixel for pixel;
//   3. drawn to twice its size by the nearest pixel: every pixel four times;
//   4. drawn smooth to twice and to half its size: the bilinear value, within one step of 255.
//   The window:
//   5. it opens hidden on the renderer's link, has the size asked for, a swap chain, and devices that answer;
//   6. with no picture yet it presents black;
//   7. a field of A: the window presents it as a new picture with its field and sizes, the renderer does not
//      read it back, and what the window draws is A letterboxed by the one function every window uses
//      (at 1x by the nearest pixel, above 1x smooth);
//   8. shown again without a new field: presented, not new, the same picture;
//   9. 60 fields of A and B in turn: every one presented as a new picture of its field, none refused a place;
//  10. two fields finished, the older one asked for: that one is shown, then the newer;
//  11. another window size: a new swap chain, the picture letterboxed anew (bars left and right);
//  12. the output's scale changes: the window shows the picture at its new size;
//  13. the renderer goes first: the window still presents the picture it holds; then the window goes.
//  14. the window goes first while it holds a picture: the renderer reads pictures back again.
//   Part 5 (between 12 and 13, and 20 before everything):
//  15. the shown picture's pixels, asked for, are the read picture pixel for pixel, and asking makes no read-back;
//  16. a picture from main memory, finished last, is shown, letterboxed, and its pixels come back as given;
//  17. a field finished after it is shown instead;
//  18. a picture from main memory that arrives late, finished before the shown one, is not shown;
//  19. a field and a picture from main memory in one turn: the one finished later is shown, both ways round
//      (the one from main memory with half the rows and rows further apart than their pixels);
//  20. a window without a Vulkan renderer: a device of its own, pictures from main memory only.
//   The renderer numbers its pictures in the order it finished them, read back or handed over.
//   Step 4 (after 19):
//  21. the overlays: rectangles under and over the pages, a line of text and a picture of the pages are drawn
//      over the game's picture in the turn they were asked for, and gone in the next;
//  22. the window's settings: vsync on and off, its size and place, borderless fullscreen and back.
//   Step 5 (after 22):
//  23. in-between pictures: handed over to the window oldest first with their tags; the one the caller chooses
//      is presented, in any order; a dropped one cannot be chosen; the field's own picture is untouched; a
//      field's turn follows; the renderer is never refused a place and every place comes back.
// Exit code 77: no Vulkan device, or no Vulkan windows on this system (skipped).
#include "runtime/gs/gs_game_window.h"
#include "runtime/gs/gs_vulkan_backend.h"
#include "runtime/host_devices.h"
#include "runtime/host_window.h"
#include "runtime/picture_rect.h"
#include "gs/gs_vulkan_handover.h"
#include "gs/gs_vulkan_window.h"
#include "gs_test_pictures.h"
#include "test_one_program.h"

#include "device.hpp"
#include "context.hpp"
#include "thread_id.hpp"
#ifdef DW5XL_TEST_HAS_IMGUI
#include "imgui.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace gs_test;

namespace
{
int g_failures = 0;

void check(bool ok, const std::string &what, uint32_t scale)
{
    std::printf("%s  scale %ux: %s\n", ok ? "ok  " : "FAIL", scale, what.c_str());
    if (!ok)
        ++g_failures;
}

struct Picture
{
    std::vector<uint8_t> rgba;
    uint32_t width = 0u, height = 0u;
};

// What a window of width x height shows of `source` placed at the rectangle, computed here: black outside the
// rectangle (by pixel centres), inside the nearest pixel or the bilinear value with the edge pixels repeated.
Picture expected(const Picture &source, uint32_t width, uint32_t height, const ps2x::PictureRect &rect, bool smooth)
{
    Picture out;
    out.width = width;
    out.height = height;
    out.rgba.assign(static_cast<size_t>(width) * height * 4u, 0u);
    const auto texel = [&](int x, int y, int channel) {
        x = std::clamp(x, 0, static_cast<int>(source.width) - 1);
        y = std::clamp(y, 0, static_cast<int>(source.height) - 1);
        return static_cast<double>(source.rgba[(static_cast<size_t>(y) * source.width + static_cast<size_t>(x)) * 4u + static_cast<size_t>(channel)]);
    };
    for (uint32_t py = 0; py < height; ++py)
        for (uint32_t px = 0; px < width; ++px)
        {
            uint8_t *o = &out.rgba[(static_cast<size_t>(py) * width + px) * 4u];
            o[3] = 0xFFu;
            const double cx = px + 0.5, cy = py + 0.5;
            if (cx < rect.x || cx >= rect.x + rect.width || cy < rect.y || cy >= rect.y + rect.height)
                continue;
            const double u = (cx - rect.x) / rect.width * source.width, v = (cy - rect.y) / rect.height * source.height;
            for (int c = 0; c < 3; ++c)
            {
                if (!smooth)
                {
                    o[c] = static_cast<uint8_t>(texel(static_cast<int>(std::floor(u)), static_cast<int>(std::floor(v)), c));
                    continue;
                }
                const double tu = u - 0.5, tv = v - 0.5;
                const int x0 = static_cast<int>(std::floor(tu)), y0 = static_cast<int>(std::floor(tv));
                const double fx = tu - x0, fy = tv - y0;
                const double top = texel(x0, y0, c) * (1.0 - fx) + texel(x0 + 1, y0, c) * fx;
                const double bottom = texel(x0, y0 + 1, c) * (1.0 - fx) + texel(x0 + 1, y0 + 1, c) * fx;
                o[c] = static_cast<uint8_t>(std::lround(top * (1.0 - fy) + bottom * fy));
            }
        }
    return out;
}

// The largest difference of a colour between two pictures of one size; 256 when they are not of one size.
int largestDifference(const Picture &a, const Picture &b)
{
    if (a.width != b.width || a.height != b.height || a.rgba.size() != b.rgba.size() || a.rgba.empty())
        return 256;
    int largest = 0;
    for (size_t i = 0; i + 4u <= a.rgba.size(); i += 4u)
        for (size_t c = 0; c < 3u; ++c)
            largest = std::max(largest, std::abs(static_cast<int>(a.rgba[i + c]) - static_cast<int>(b.rgba[i + c])));
    return largest;
}

Picture asPicture(const PresentationFrame &frame)
{
    Picture p;
    p.width = frame.width;
    p.height = frame.height;
    p.rgba = frame.pixels;
    return p;
}

// A picture of the test's own: every pixel another colour, its alpha not opaque (a window draws it opaque).
Picture patterned(uint32_t width, uint32_t height)
{
    Picture p;
    p.width = width;
    p.height = height;
    p.rgba.resize(static_cast<size_t>(width) * height * 4u);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
        {
            uint8_t *o = &p.rgba[(static_cast<size_t>(y) * width + x) * 4u];
            o[0] = static_cast<uint8_t>(x * 7u + y * 3u);
            o[1] = static_cast<uint8_t>((x ^ y) * 5u + 11u);
            o[2] = static_cast<uint8_t>(y * 13u + (x >> 2));
            o[3] = 0x12u;
        }
    return p;
}

ps2x::gs::GameWindowPixels asPixels(const Picture &picture, uint32_t displayWidth, uint32_t displayHeight, uint64_t field, uint64_t order)
{
    ps2x::gs::GameWindowPixels pixels;
    pixels.rgba = picture.rgba.data();
    pixels.width = picture.width;
    pixels.height = picture.height;
    pixels.displayWidth = displayWidth;
    pixels.displayHeight = displayHeight;
    pixels.field = field;
    pixels.order = order;
    return pixels;
}

bool opaque(const Picture &picture)
{
    if (picture.rgba.empty())
        return false;
    for (size_t i = 3u; i < picture.rgba.size(); i += 4u)
        if (picture.rgba[i] != 0xFFu)
            return false;
    return true;
}

// A device object of the test's own on the renderer's context, to draw with the drawer alone.
struct OwnDevice
{
    std::shared_ptr<GSVulkanPresenterLink> link; // declared first: it goes last
    Vulkan::Device device;
    std::unique_ptr<ps2x::gs::VulkanPictureDrawer> drawer;
    HandedPictureViews views;
    static constexpr unsigned kFrameContexts = 4;

    explicit OwnDevice(std::shared_ptr<GSVulkanPresenterLink> from) : link(std::move(from)), views(device, kFrameContexts + 1u)
    {
        Util::register_thread_index(0);
        device.set_context(*link->context);
        device.init_frame_contexts(kFrameContexts);
        device.set_queue_lock([lock = link->queues] { lock->lock(); }, [lock = link->queues] { lock->unlock(); });
        drawer = std::make_unique<ps2x::gs::VulkanPictureDrawer>(device);
    }
    ~OwnDevice()
    {
        device.wait_idle();
        views.clear();
    }

    // The picture drawn at the rectangle into a black picture of width x height, and that one read.
    Picture drawn(const HandedPicture &picture, uint32_t width, uint32_t height, const ps2x::PictureRect &rect, bool smooth)
    {
        Util::register_thread_index(0);
        Picture out;
        const Vulkan::ImageView *view = views.view(picture);
        if (!view)
            return out;
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
            return out;
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
        drawer->draw(*cmd, *view, rect.x, rect.y, rect.width, rect.height, smooth);
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
            return out;
        out.width = width;
        out.height = height;
        out.rgba.resize(static_cast<size_t>(width) * height * 4u);
        std::memcpy(out.rgba.data(), mapped, out.rgba.size());
        device.unmap_host_buffer(*read, Vulkan::MEMORY_ACCESS_READ_BIT);
        device.next_frame_context();
        views.nextFrame();
        return out;
    }
};

Picture windowPicture(ps2x::gs::GameWindow &window, const ps2x::gs::GameWindowLook &look)
{
    Picture p;
    if (!window.picture(look, p.rgba, p.width, p.height))
        p = {};
    return p;
}

// 0: skipped (no renderer); 1: ran; 2: skipped (no Vulkan windows).
int run(uint32_t wantedScale)
{
    std::unique_ptr<GSVulkanBackend> backend;
    try
    {
        backend = std::make_unique<GSVulkanBackend>(); // throws without a Vulkan device
    }
    catch (const std::exception &error)
    {
        std::printf("SKIPPED: no Vulkan GS renderer here (%s)\n", error.what());
        return 0;
    }
    std::vector<uint8_t> vram(4u << 20, 0u);
    backend->Initialize(vram.data(), static_cast<uint32_t>(vram.size()));
    backend->SetResolutionScale(wantedScale);
    const uint32_t scale = backend->GetResolutionScale();
    if (scale != wantedScale) // a GPU without 16 samples gives 2x for 4x: tested at what it gives
        std::printf("note  scale %ux: this GPU renders at %ux\n", wantedScale, scale);
    const bool smooth = scale > 1u; // as the game's window asks: by the nearest pixel at 1x, smooth above
    uint64_t tick = 10u;

    std::shared_ptr<GSVulkanPresenterLink> link = backend->PresenterLink();
    if (!link || !link->windowSupport)
    {
        std::printf("SKIPPED: the renderer's graphics device has no window support here\n");
        return 2;
    }
    VulkanGsHandover &fields = *link->fields;

    // The yardstick: A and B read back the old way (the hand-over is off).
    drawA(*backend);
    const PresentationFrame readA = readBack(*backend, ++tick);
    drawB(*backend);
    const PresentationFrame readB = readBack(*backend, ++tick);
    check(readA && readB && readA.pixels != readB.pixels && readA.pixels.size() == static_cast<size_t>(readA.width) * readA.height * 4u,
          "the yardstick: A and B are read back, differ, and are whole pictures", scale);
    if (!readA || !readB)
        return 1;
    check(readA.order != 0u && readB.order > readA.order, "the read pictures are numbered in the order they were finished", scale);
    const Picture a = asPicture(readA), b = asPicture(readB);
    const uint32_t displayWidth = readA.displayWidth ? readA.displayWidth : readA.width;
    const uint32_t displayHeight = readA.displayHeight ? readA.displayHeight : readA.height;

    // --- The drawing alone.
    {
        OwnDevice own(link);
        fields.start();
        drawA(*backend);
        check(backend->SubmitFieldScanout(request(++tick)), "a field of A is scanned out", scale);
        HandedPicture held = fields.takeNewest();
        check(static_cast<bool>(held) && held.width == a.width && held.height == a.height, "A's picture is handed over", scale);
        check(held.order > readB.order, "and numbered after the pictures read before it", scale);
        if (held)
        {
            const uint32_t w = a.width, h = a.height;
            // 1. One to one.
            const ps2x::PictureRect whole{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)};
            check(largestDifference(own.drawn(held, w, h, whole, false), a) == 0, "drawn one to one it is the read picture pixel for pixel", scale);
            // 2. Room left and right.
            const ps2x::PictureRect middle{94.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)};
            check(largestDifference(own.drawn(held, w + 188u, h, middle, false), expected(a, w + 188u, h, middle, false)) == 0,
                  "drawn with room left and right: black there, the picture between pixel for pixel", scale);
            if (scale == 1u)
            {
                // 3. Twice the size by the nearest pixel.
                const ps2x::PictureRect twice{0.0f, 0.0f, static_cast<float>(w * 2u), static_cast<float>(h * 2u)};
                check(largestDifference(own.drawn(held, w * 2u, h * 2u, twice, false), expected(a, w * 2u, h * 2u, twice, false)) == 0,
                      "drawn to twice its size by the nearest pixel: every pixel four times", scale);
                // 4. Smooth to twice the size.
                const int up = largestDifference(own.drawn(held, w * 2u, h * 2u, twice, true), expected(a, w * 2u, h * 2u, twice, true));
                std::printf("note  scale %ux: smooth to twice the size, largest difference from the bilinear value: %d of 255\n", scale, up);
                check(up <= 1, "drawn smooth to twice its size: the bilinear value within one step of 255", scale);
            }
            // 4. Smooth to half the size.
            const ps2x::PictureRect half{0.0f, 0.0f, static_cast<float>(w / 2u), static_cast<float>(h / 2u)};
            const int down = largestDifference(own.drawn(held, w / 2u, h / 2u, half, true), expected(a, w / 2u, h / 2u, half, true));
            std::printf("note  scale %ux: smooth to half the size, largest difference from the bilinear value: %d of 255\n", scale, down);
            check(down <= 1, "drawn smooth to half its size: the bilinear value within one step of 255", scale);
            fields.release(held);
        }
        fields.stop();
    }

    // --- The window. Twice the display's width and room above and below: the picture lies at a whole rectangle.
    ps2x::gs::GameWindowOptions options;
    options.width = static_cast<int>(displayWidth * 2u);
    options.height = static_cast<int>(displayHeight * 2u + 88u);
    options.title = "DW5XL test window (hidden)";
    options.hidden = true;
    options.vsync = false;
    std::string why;
    std::unique_ptr<ps2x::gs::GameWindow> window = ps2x::gs::GameWindow::open(link, options, why);
    if (!window)
    {
        std::printf("SKIPPED: no window here (%s)\n", why.c_str());
        return 2;
    }
    ps2x::gs::GameWindowLook look;
    look.shape = 0.0f;
    look.smooth = smooth;

    // 5. Opened.
    uint32_t winW = 0u, winH = 0u;
    window->drawableSize(winW, winH);
    check(window->hidden() && winW == static_cast<uint32_t>(options.width) && winH == static_cast<uint32_t>(options.height),
          "the window is hidden and has the size asked for", scale);
    check(fields.started(), "opening the window switches the hand-over on", scale);
    window->poll();
    {
        ps2x::host::Devices &devices = window->devices();
        float dx = 1.0f, dy = 1.0f;
        devices.mouseDelta(dx, dy);
        check(devices.ready() && !devices.keyDown(32) && !devices.keyPressed(257) && !devices.mouseButtonDown(0) && dx == 0.0f && dy == 0.0f &&
                  devices.seconds() >= 0.0,
              "its devices answer: ready, no key or mouse button down, the mouse not moved", scale);
    }
    const ps2x::PictureRect placed = ps2x::pictureRectInWindow(displayWidth, displayHeight, look.shape, static_cast<float>(winW), static_cast<float>(winH));

    // 6. No picture yet.
    {
        const ps2x::gs::GameWindowShown shown = window->show(tick, look);
        check(shown.presented && !shown.have && !shown.newPicture, "with no picture yet the window presents", scale);
        const Picture p = windowPicture(*window, look);
        bool black = !p.rgba.empty();
        for (size_t i = 0; i + 4u <= p.rgba.size(); i += 4u)
            black = black && p.rgba[i] == 0u && p.rgba[i + 1u] == 0u && p.rgba[i + 2u] == 0u;
        check(black && p.width == winW && p.height == winH, "and what it draws is black", scale);
        Picture none;
        check(!window->shownPixels(none.rgba, none.width, none.height), "it has no picture's pixels to give", scale);
        check(window->counts().swapChains == 1u, "one swap chain was made", scale);
    }

    // 7. A field of A.
    drawA(*backend);
    check(backend->SubmitFieldScanout(request(++tick)), "a field of A is scanned out", scale);
    {
        const ps2x::gs::GameWindowShown shown = window->show(tick, look);
        check(shown.presented && shown.have && shown.newPicture && shown.field == tick, "the window presents it as a new picture of its field", scale);
        check(shown.width == a.width && shown.height == a.height && shown.displayWidth == displayWidth && shown.displayHeight == displayHeight,
              "with the read picture's sizes", scale);
        check(shown.x == placed.x && shown.y == placed.y && shown.rectWidth == placed.width && shown.rectHeight == placed.height,
              "placed by the one function every window uses", scale);
        check(!readArrives(*backend, tick), "the renderer did not read it back", scale);
        const int difference = largestDifference(windowPicture(*window, look), expected(a, winW, winH, placed, smooth));
        std::printf("note  scale %ux: the window's picture of A, largest difference from the expected one: %d of 255\n", scale, difference);
        check(difference <= (smooth ? 1 : 0), smooth ? "what the window draws is A letterboxed, smooth, within one step of 255"
                                                     : "what the window draws is A letterboxed, pixel for pixel",
              scale);
    }

    // 8. Shown again without a new field.
    {
        const ps2x::gs::GameWindowShown shown = window->show(tick, look);
        check(shown.presented && shown.have && !shown.newPicture && shown.field == tick, "shown again without a new field: presented, not new, the same field", scale);
    }

    // 9. 60 fields of A and B in turn.
    {
        const ps2x::gs::GameWindowCounts before = window->counts();
        const VulkanGsHandover::Counts handedBefore = fields.counts();
        bool allNew = true, allPresented = true, allScanned = true;
        for (int i = 0; i < 60; ++i)
        {
            if (i % 2 == 0)
                drawB(*backend);
            else
                drawA(*backend);
            allScanned = backend->SubmitFieldScanout(request(++tick)) && allScanned;
            window->poll();
            const ps2x::gs::GameWindowShown shown = window->show(tick, look);
            allPresented = shown.presented && allPresented;
            allNew = shown.newPicture && shown.field == tick && allNew;
        }
        const ps2x::gs::GameWindowCounts after = window->counts();
        const VulkanGsHandover::Counts handedAfter = fields.counts();
        check(allScanned && allPresented && allNew, "60 fields of A and B in turn: every one presented as a new picture of its field", scale);
        check(after.presented - before.presented == 60u && after.newPictures - before.newPictures == 60u && after.notPresented == before.notPresented,
              "the window counted 60 presents, 60 new pictures and none that failed", scale);
        check(handedAfter.noPlace == handedBefore.noPlace && handedAfter.givenUp == handedBefore.givenUp && handedAfter.images <= 3u,
              "the renderer was never refused a place, gave up no picture and made no more than three images", scale);
        const int difference = largestDifference(windowPicture(*window, look), expected(a, winW, winH, placed, smooth)); // the last field was A
        check(difference <= (smooth ? 1 : 0), "the last field's picture is what the window draws", scale);
    }

    // 10. Two fields finished, the older asked for first.
    {
        drawB(*backend);
        check(backend->SubmitFieldScanout(request(++tick)), "a field of B is scanned out", scale);
        const uint64_t older = tick;
        drawA(*backend);
        check(backend->SubmitFieldScanout(request(++tick)), "and a field of A after it", scale);
        ps2x::gs::GameWindowShown shown = window->show(older, look);
        check(shown.newPicture && shown.field == older, "asked for the older field, the window shows that one", scale);
        check(largestDifference(windowPicture(*window, look), expected(b, winW, winH, placed, smooth)) <= (smooth ? 1 : 0), "which is B", scale);
        shown = window->show(tick, look);
        check(shown.newPicture && shown.field == tick, "then the newer one", scale);
    }

    // 11. Another window size: bars left and right.
    {
        const uint64_t chains = window->counts().swapChains;
        window->resize(static_cast<int>(displayWidth * 2u + 188u), static_cast<int>(displayHeight * 2u));
        window->poll();
        uint32_t w = 0u, h = 0u;
        window->drawableSize(w, h);
        check(w == displayWidth * 2u + 188u && h == displayHeight * 2u, "the window takes another size", scale);
        const ps2x::gs::GameWindowShown shown = window->show(tick, look);
        const ps2x::PictureRect now = ps2x::pictureRectInWindow(displayWidth, displayHeight, look.shape, static_cast<float>(w), static_cast<float>(h));
        check(shown.presented && window->counts().swapChains == chains + 1u, "and presents through a new swap chain", scale);
        check(shown.x == now.x && shown.rectWidth == now.width && now.x == 94.0f, "the picture is letterboxed anew, bars left and right", scale);
        check(largestDifference(windowPicture(*window, look), expected(a, w, h, now, smooth)) <= (smooth ? 1 : 0), "and is still A", scale);
    }

    // 12. The output's scale changes.
    {
        const uint32_t other = scale == 1u ? 2u : 1u;
        backend->SetResolutionScale(other);
        const uint32_t got = backend->GetResolutionScale();
        drawA(*backend);
        check(backend->SubmitFieldScanout(request(++tick)), "a field of A at another output scale is scanned out", scale);
        const ps2x::gs::GameWindowShown shown = window->show(tick, look);
        check(shown.presented && shown.newPicture && shown.width == a.width / scale * got && shown.displayWidth == displayWidth,
              "the window shows the picture at its new size, in the same place", scale);
        backend->SetResolutionScale(scale);
        drawB(*backend);
        check(backend->SubmitFieldScanout(request(++tick)), "and a field of B at the first scale again", scale);
        const ps2x::gs::GameWindowShown back = window->show(tick, look);
        check(back.presented && back.newPicture && back.width == a.width, "which the window shows", scale);
    }

    // --- Part 5: the shown picture's pixels on request, pictures from main memory, and which picture is the newer.
    {
        uint32_t w = 0u, h = 0u;
        window->drawableSize(w, h);
        const ps2x::PictureRect now = ps2x::pictureRectInWindow(displayWidth, displayHeight, look.shape, static_cast<float>(w), static_cast<float>(h));
        const int allowed = smooth ? 1 : 0;

        // 15. The pixels of a picture on the graphics card (the window shows B, step 12's last field).
        {
            Picture got;
            const bool have = window->shownPixels(got.rgba, got.width, got.height);
            check(have && got.width == b.width && got.height == b.height && sameColours(got.rgba, b.rgba),
                  "the shown picture's pixels, asked for: the read picture pixel for pixel", scale);
            check(have && opaque(got), "and opaque", scale);
            check(!readArrives(*backend, tick), "asking did not make the renderer read a picture back", scale);
            const ps2x::gs::GameWindowShown shown = window->show(tick, look);
            check(shown.presented && shown.have && !shown.newPicture, "the window still shows that picture afterwards", scale);
            check(largestDifference(windowPicture(*window, look), expected(b, w, h, now, smooth)) <= allowed, "which is still B", scale);
        }

        // 16. A picture from main memory, finished after everything so far.
        const Picture m = patterned(displayWidth, displayHeight);
        ps2x::gs::GameWindowPixels given = asPixels(m, displayWidth, displayHeight, tick, ps2x::gs::nextPictureOrder());
        {
            const ps2x::gs::GameWindowCounts before = window->counts();
            window->memoryPicture(given);
            ps2x::gs::GameWindowShown shown = window->show(tick, look);
            check(shown.presented && shown.have && shown.newPicture && shown.fromMemory && shown.order == given.order && shown.field == tick,
                  "a picture from main memory, finished last: the window shows it as a new picture", scale);
            check(shown.width == m.width && shown.height == m.height && shown.displayWidth == displayWidth && shown.displayHeight == displayHeight &&
                      shown.x == now.x && shown.rectWidth == now.width,
                  "with its sizes, in the picture's place", scale);
            const int difference = largestDifference(windowPicture(*window, look), expected(m, w, h, now, smooth));
            std::printf("note  scale %ux: the window's picture of the one from main memory, largest difference: %d of 255\n", scale, difference);
            check(difference <= allowed, "what the window draws is that picture, letterboxed", scale);
            const ps2x::gs::GameWindowCounts after = window->counts();
            check(after.fromMemory == before.fromMemory + 1u && after.newPictures == before.newPictures + 1u, "counted as one new picture from main memory", scale);
            Picture got;
            check(window->shownPixels(got.rgba, got.width, got.height) && got.width == m.width && got.height == m.height && sameColours(got.rgba, m.rgba) && opaque(got),
                  "its pixels, asked for, are the ones given, opaque", scale);
            shown = window->show(tick, look);
            check(shown.presented && shown.have && !shown.newPicture && shown.fromMemory, "shown again without anything new: presented, not new", scale);
        }

        // 17. A field finished after it.
        {
            drawA(*backend);
            check(backend->SubmitFieldScanout(request(++tick)), "a field of A is scanned out", scale);
            const ps2x::gs::GameWindowShown shown = window->show(tick, look);
            check(shown.newPicture && !shown.fromMemory && shown.field == tick && shown.order > given.order,
                  "a field finished after the picture from main memory: the window shows the field's picture", scale);
            check(largestDifference(windowPicture(*window, look), expected(a, w, h, now, smooth)) <= allowed, "which is A", scale);
        }

        // 18. A picture from main memory that arrives late: finished before the picture the window shows.
        {
            const uint64_t early = ps2x::gs::nextPictureOrder();
            drawB(*backend);
            check(backend->SubmitFieldScanout(request(++tick)), "a field of B is scanned out", scale);
            ps2x::gs::GameWindowShown shown = window->show(tick, look);
            check(shown.newPicture && !shown.fromMemory && shown.order > early, "and shown", scale);
            const ps2x::gs::GameWindowCounts before = window->counts();
            window->memoryPicture(asPixels(m, displayWidth, displayHeight, tick - 1u, early));
            shown = window->show(tick, look);
            check(shown.presented && !shown.newPicture && !shown.fromMemory && shown.field == tick,
                  "a picture from main memory finished before the shown one arrives late: not shown", scale);
            check(window->counts().olderNotShown == before.olderNotShown + 1u && window->counts().newPictures == before.newPictures, "and counted as not shown", scale);
            check(largestDifference(windowPicture(*window, look), expected(b, w, h, now, smooth)) <= allowed, "the window still draws B", scale);
        }

        // 19. A field and a picture from main memory in one turn: the one finished later is shown.
        {
            // Half the rows for the same display (two fields would fill it), and rows further apart than their pixels.
            const Picture half = patterned(displayWidth, displayHeight / 2u);
            const size_t stride = static_cast<size_t>(half.width) * 4u + 24u;
            std::vector<uint8_t> apart(stride * half.height, 0xEEu);
            for (uint32_t y = 0; y < half.height; ++y)
                std::memcpy(apart.data() + stride * y, half.rgba.data() + static_cast<size_t>(y) * half.width * 4u, static_cast<size_t>(half.width) * 4u);
            drawA(*backend);
            check(backend->SubmitFieldScanout(request(++tick)), "a field of A is scanned out", scale);
            ps2x::gs::GameWindowPixels later = asPixels(half, displayWidth, displayHeight, tick, ps2x::gs::nextPictureOrder());
            later.rgba = apart.data();
            later.strideBytes = stride;
            window->memoryPicture(later);
            ps2x::gs::GameWindowShown shown = window->show(tick, look);
            check(shown.newPicture && shown.fromMemory && shown.order == later.order && shown.height == half.height && shown.displayHeight == displayHeight,
                  "a field and then a picture from main memory in one turn: the one from main memory is shown", scale);
            const int difference = largestDifference(windowPicture(*window, look), expected(half, w, h, now, smooth));
            std::printf("note  scale %ux: half the rows drawn to the display's shape, largest difference: %d of 255\n", scale, difference);
            check(difference <= allowed, "drawn to the display's shape from half the rows, its rows taken by their stride", scale);

            const ps2x::gs::GameWindowPixels earlier = asPixels(m, displayWidth, displayHeight, tick + 1u, ps2x::gs::nextPictureOrder());
            drawB(*backend);
            check(backend->SubmitFieldScanout(request(++tick)), "a field of B is scanned out", scale);
            window->memoryPicture(earlier);
            shown = window->show(tick, look);
            check(shown.newPicture && !shown.fromMemory && shown.field == tick && shown.order > earlier.order,
                  "a picture from main memory and then a field in one turn: the field's picture is shown", scale);
            check(largestDifference(windowPicture(*window, look), expected(b, w, h, now, smooth)) <= allowed, "which is B", scale);
        }
    }

    // --- Step 4: the overlays drawn by the window, and the window's settings. The window shows B, with bars of 94
    // pixels left and right.
    {
        ps2x::host::Window &ui = window->control();
        using Layer = ps2x::host::Window::Layer;
        uint32_t w = 0u, h = 0u;
        window->drawableSize(w, h);
        const ps2x::PictureRect now = ps2x::pictureRectInWindow(displayWidth, displayHeight, look.shape, static_cast<float>(w), static_cast<float>(h));
        const int allowed = smooth ? 1 : 0;

        // 21. Overlays over the picture.
        check(ui.uiSetup(true), "the window draws the overlays itself", scale);
#ifdef DW5XL_TEST_HAS_IMGUI
        ImGui::GetIO().IniFilename = nullptr; // a test leaves no settings file behind
#endif
        Picture stamp = patterned(8u, 8u);
        for (size_t i = 3u; i < stamp.rgba.size(); i += 4u)
            stamp.rgba[i] = 0xFFu;
        const uint64_t texture = ui.makeTexture(stamp.rgba.data(), 8, 8);
        check(texture != 0u, "a picture for the overlays is made", scale);
        ps2x::gs::GameWindowShown shown = window->take(tick, look);
        check(shown.have && !shown.newPicture && shown.x == now.x && shown.y == now.y && shown.rectWidth == now.width && shown.rectHeight == now.height,
              "the window's turn in two halves: the picture has its place before anything is drawn", scale);
        const int inPicture = static_cast<int>(now.x) + 50;
        ui.fillRect(Layer::OverPages, 10, 20, 30, 40, {255, 0, 0, 255});
        ui.fillRect(Layer::UnderPages, 40, 100, 30, 40, {0, 0, 255, 128});
        ui.fillRect(Layer::OverPages, inPicture, 60, 20, 20, {0, 255, 0, 255});
        const int textWidth = ui.textWidth(18, "DW5XL 049");
        ui.text(Layer::OverPages, 10, 300, 18, {255, 255, 255, 255}, "DW5XL 049");
#ifdef DW5XL_TEST_HAS_IMGUI
        ui.uiBegin();
        ImGui::GetForegroundDrawList()->AddImage(ImTextureID(texture), ImVec2(10.0f, 200.0f), ImVec2(18.0f, 208.0f));
        ui.uiEnd();
#endif
        window->present(shown, look);
        check(shown.presented, "and is presented with the overlays", scale);
        const Picture p = windowPicture(*window, look);
        const auto is = [&](int x, int y, int r, int g, int b, int within) {
            const uint8_t *c = &p.rgba[(static_cast<size_t>(y) * p.width + static_cast<size_t>(x)) * 4u];
            return std::abs(c[0] - r) <= within && std::abs(c[1] - g) <= within && std::abs(c[2] - b) <= within;
        };
        const auto all = [&](int x0, int y0, int x1, int y1, int r, int g, int b, int within) {
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x)
                    if (!is(x, y, r, g, b, within))
                        return false;
            return true;
        };
        const bool sized = p.width == w && p.height == h;
        check(sized && all(10, 20, 40, 60, 255, 0, 0, 0), "a rectangle over the bar: its colour in every pixel", scale);
        check(sized && all(0, 20, 10, 60, 0, 0, 0, 0) && all(10, 10, 40, 20, 0, 0, 0, 0) && all(10, 60, 40, 70, 0, 0, 0, 0) && is(40, 30, 0, 0, 0, 0),
              "and black right beside it on every side", scale);
        check(sized && all(40, 100, 70, 140, 0, 0, 128, 1), "a half transparent rectangle under the pages: half its colour over black", scale);
        check(sized && all(inPicture, 60, inPicture + 20, 80, 0, 255, 0, 0), "a rectangle over the picture covers it", scale);
        {
            // The picture itself is untouched right of the overlays.
            const Picture want = expected(b, w, h, now, smooth);
            int largest = 256;
            if (sized)
            {
                largest = 0;
                for (uint32_t y = 0; y < h; ++y)
                    for (uint32_t x = static_cast<uint32_t>(inPicture) + 40u; x < w; ++x)
                        for (size_t c = 0; c < 3u; ++c)
                            largest = std::max(largest, std::abs(static_cast<int>(p.rgba[(static_cast<size_t>(y) * w + x) * 4u + c]) -
                                                                 static_cast<int>(want.rgba[(static_cast<size_t>(y) * w + x) * 4u + c])));
            }
            check(largest <= allowed, "the picture beside the overlays is as without them", scale);
        }
        {
            int bright = 0, astray = 0;
            for (int y = 296; sized && y < 326; ++y)
                for (int x = 10; x < 94; ++x)
                    bright += is(x, y, 255, 255, 255, 60) ? 1 : 0;
            for (int y = 340; sized && y < 360; ++y)
                for (int x = 0; x < 94; ++x)
                    astray += is(x, y, 0, 0, 0, 0) ? 0 : 1;
            std::printf("note  scale %ux: a line of text 18 pixels high: %d pixels wide by the window's measure, %d bright pixels drawn\n", scale, textWidth, bright);
            check(textWidth > 40 && bright >= 40 && astray == 0, "a line of text: it has a width, its letters are drawn at its place and nowhere below", scale);
        }
#ifdef DW5XL_TEST_HAS_IMGUI
        {
            bool same = sized;
            for (int y = 0; same && y < 8; ++y)
                for (int x = 0; same && x < 8; ++x)
                {
                    const uint8_t *s = &stamp.rgba[(static_cast<size_t>(y) * 8u + static_cast<size_t>(x)) * 4u];
                    same = is(10 + x, 200 + y, s[0], s[1], s[2], 0);
                }
            check(same, "a picture of the pages drawn one to one: pixel for pixel", scale);
        }
#endif
        shown = window->show(tick, look);
        check(shown.presented && largestDifference(windowPicture(*window, look), expected(b, w, h, now, smooth)) <= allowed,
              "in the next turn, with nothing drawn over it, the window shows the picture alone", scale);
        ui.dropTexture(texture);

        // 22. The window's settings.
        check(ui.hidden() && !ui.maximized() && !ui.fullscreen() && !ui.vsync(), "the window says what it is: hidden, not maximized, not fullscreen, vsync off", scale);
        ui.setVsync(true);
        const bool on = ui.vsync();
        ui.setVsync(false);
        shown = window->show(tick, look);
        check(on && !ui.vsync() && shown.presented, "vsync switched on and off again: the window presents", scale);
        int screenW = 0, screenH = 0, renderW = 0, renderH = 0;
        ui.screenSize(screenW, screenH);
        ui.renderSize(renderW, renderH);
        check(renderW == static_cast<int>(w) && renderH == static_cast<int>(h) && screenW > 0 && screenH > 0 && ui.displayWidth() > 0,
              "it knows its size and its display's width", scale);
        int placeX = 0, placeY = 0, movedX = 0, movedY = 0;
        ui.position(placeX, placeY);
        ui.setPosition(placeX + 7, placeY + 5);
        window->poll();
        ui.position(movedX, movedY);
        check(movedX == placeX + 7 && movedY == placeY + 5, "it takes another place", scale);
        ui.setPosition(placeX, placeY);
        window->poll();
        const uint64_t chains = window->counts().swapChains;
        ui.setFullscreen(true);
        window->poll();
        int fullW = 0, fullH = 0;
        ui.screenSize(fullW, fullH);
        shown = window->show(tick, look);
        check(ui.fullscreen() && fullW == ui.displayWidth() && shown.presented && window->counts().swapChains > chains,
              "borderless fullscreen: the display's width, presented through a new swap chain", scale);
        ui.setFullscreen(false);
        window->poll();
        ui.screenSize(fullW, fullH);
        ui.position(movedX, movedY);
        shown = window->show(tick, look);
        check(!ui.fullscreen() && fullW == screenW && fullH == screenH && movedX == placeX && movedY == placeY && shown.presented,
              "and back: the size and the place before", scale);
        check(ui.takeDroppedFiles().empty(), "no file was dropped on it", scale);
        check(largestDifference(windowPicture(*window, look), expected(b, w, h, now, smooth)) <= allowed, "the window still draws B", scale);
    }

    // --- Step 5: in-between pictures, handed over on the graphics card and presented when the caller asks.
    // 23. The window shows B (a field's own picture).
    {
        uint32_t w = 0u, h = 0u;
        window->drawableSize(w, h);
        const ps2x::PictureRect now = ps2x::pictureRectInWindow(displayWidth, displayHeight, look.shape, static_cast<float>(w), static_cast<float>(h));
        const int allowed = smooth ? 1 : 0;
        VulkanGsHandover &pictures = *link->pictures;
        const VulkanGsHandover::Counts before = pictures.counts();
        window->attachTimedPictures(link);
        check(pictures.started(), "attached: the in-between pictures' hand-over is switched on", scale);
        const uint64_t tags[3] = {gsPictureTag(tick, 1u, 3u), gsPictureTag(tick, 2u, 3u), gsPictureTag(tick, 3u, 3u)};
        bool submitted = true;
        for (int i = 0; i < 3; ++i)
        {
            if (i == 1)
                drawB(*backend);
            else
                drawA(*backend);
            GSPresentationRequest r = request(tick);
            r.pictureTag = tags[i];
            submitted = backend->SubmitPictureRead(r) && submitted;
        }
        std::vector<ps2x::gs::GameWindowTimed> fresh;
        window->takeTimed(fresh);
        check(submitted && fresh.size() == 3u, "three in-between pictures come to the window", scale);
        if (fresh.size() == 3u)
        {
            check(fresh[0].tag == tags[0] && fresh[1].tag == tags[1] && fresh[2].tag == tags[2] && fresh[0].order < fresh[1].order &&
                      fresh[1].order < fresh[2].order && fresh[0].id != fresh[1].id && fresh[0].width == a.width && fresh[0].displayWidth == displayWidth,
                  "oldest first, each with its tag, a number of its own and its sizes", scale);
            std::vector<ps2x::gs::GameWindowTimed> again;
            window->takeTimed(again);
            check(again.empty(), "asked again, there is none new", scale);

            // The second one (B) is presented.
            const ps2x::gs::GameWindowCounts countsBefore = window->counts();
            ps2x::gs::GameWindowShown shown = window->takeTimedPicture(fresh[1].id, look);
            check(shown.have && shown.timed && shown.newPicture && shown.tag == tags[1] && shown.x == now.x && shown.rectWidth == now.width,
                  "one of them chosen for the turn: it has its place before it is drawn", scale);
            window->present(shown, look);
            check(shown.presented && largestDifference(windowPicture(*window, look), expected(b, w, h, now, smooth)) <= allowed,
                  "presented: the window draws that picture (B)", scale);
            check(window->counts().timed == countsBefore.timed + 1u, "counted as an in-between picture", scale);
            // The field's own picture the window holds is still the one from before (B, the last field).
            Picture field;
            check(window->shownPixels(field.rgba, field.width, field.height) && sameColours(field.rgba, b.rgba),
                  "the field's own picture the window holds is untouched", scale);
            // The first one (A) after it: the caller decides the order.
            shown = window->takeTimedPicture(fresh[0].id, look);
            window->present(shown, look);
            check(shown.have && shown.presented && largestDifference(windowPicture(*window, look), expected(a, w, h, now, smooth)) <= allowed,
                  "then an older one: the window draws it (A)", scale);
            // The third is dropped; it cannot be chosen afterwards.
            window->dropTimed(fresh[2].id);
            shown = window->takeTimedPicture(fresh[2].id, look);
            check(!shown.have && !shown.timed, "a dropped one cannot be chosen", scale);
            window->present(shown, look);
            // A field's turn after in-between pictures.
            drawB(*backend);
            check(backend->SubmitFieldScanout(request(++tick)), "a field of B is scanned out", scale);
            shown = window->show(tick, look);
            check(shown.presented && shown.newPicture && !shown.timed && largestDifference(windowPicture(*window, look), expected(b, w, h, now, smooth)) <= allowed,
                  "a field's turn after them: the field's picture (B)", scale);
        }
        const VulkanGsHandover::Counts after = pictures.counts();
        check(after.noPlace == before.noPlace && after.givenUp == before.givenUp && after.handed == before.handed + 3u,
              "the renderer handed all three over, was never refused a place and gave none up", scale);
        std::vector<GSPictureRead> read;
        check(!backend->CollectPictureReads(read) && read.empty(), "none was read back", scale);
        window->attachTimedPictures(nullptr);
        check(!pictures.started(), "detached: the hand-over is switched off", scale);
        // Every place is free again: twelve more pictures find one each.
        pictures.start();
        bool all = true;
        for (int i = 0; i < 12; ++i)
        {
            GSPresentationRequest r = request(tick);
            r.pictureTag = gsPictureTag(tick, static_cast<uint32_t>(i + 1), 12u);
            all = backend->SubmitPictureRead(r) && all;
        }
        const VulkanGsHandover::Counts filled = pictures.counts();
        check(all && filled.noPlace == after.noPlace && filled.givenUp == after.givenUp, "and the window gave every place back (twelve more find one each)", scale);
        while (HandedPicture left = pictures.takeOldest())
            pictures.release(left);
        pictures.stop();
    }

    // 13. The renderer goes first.
    {
        uint32_t w = 0u, h = 0u;
        window->drawableSize(w, h);
        const ps2x::PictureRect now = ps2x::pictureRectInWindow(displayWidth, displayHeight, look.shape, static_cast<float>(w), static_cast<float>(h));
        backend.reset();
        const ps2x::gs::GameWindowShown shown = window->show(tick + 100u, look);
        check(shown.presented && shown.have && !shown.newPicture, "the renderer is gone: the window still presents the picture it holds", scale);
        check(largestDifference(windowPicture(*window, look), expected(b, w, h, now, smooth)) <= (smooth ? 1 : 0), "which is still B", scale);
        window.reset();
        check(true, "then the window goes", scale);
    }
    return 1;
}

// 14. The window goes first while it holds a picture.
void windowGoesFirst()
{
    const uint32_t scale = 1u;
    std::unique_ptr<GSVulkanBackend> backend = std::make_unique<GSVulkanBackend>();
    std::vector<uint8_t> vram(4u << 20, 0u);
    backend->Initialize(vram.data(), static_cast<uint32_t>(vram.size()));
    backend->SetResolutionScale(scale);
    uint64_t tick = 10u;
    std::shared_ptr<GSVulkanPresenterLink> link = backend->PresenterLink();
    ps2x::gs::GameWindowOptions options;
    options.hidden = true;
    options.vsync = false;
    options.title = "DW5XL test window (hidden)";
    std::string why;
    std::unique_ptr<ps2x::gs::GameWindow> window = ps2x::gs::GameWindow::open(link, options, why);
    check(static_cast<bool>(window), "a second window opens after the first is gone", scale);
    if (!window)
        return;
    drawA(*backend);
    check(backend->SubmitFieldScanout(request(++tick)), "a field of A is scanned out", scale);
    const ps2x::gs::GameWindowShown shown = window->show(tick, {});
    check(shown.presented && shown.newPicture, "the window shows it", scale);
    window.reset();
    check(!link->fields->started(), "the window is gone: the hand-over is off", scale);
    drawB(*backend);
    const PresentationFrame read = readBack(*backend, ++tick);
    check(static_cast<bool>(read), "and the renderer reads pictures back again", scale);
}

// 20. A window without a Vulkan renderer (the CPU renderer's case): a Vulkan device of its own, pictures from main
// memory only. Called before any renderer exists, so the window starts Vulkan itself. False: no window (the reason
// in `why`), nothing checked.
bool windowWithoutRenderer(std::string &why)
{
    const uint32_t scale = 1u;
    ps2x::gs::GameWindowOptions options;
    options.width = 640;
    options.height = 480;
    options.hidden = true;
    options.vsync = false;
    options.title = "DW5XL test window (hidden)";
    std::unique_ptr<ps2x::gs::GameWindow> window = ps2x::gs::GameWindow::open(nullptr, options, why);
    if (!window)
        return false;
    ps2x::gs::GameWindowLook look;
    uint32_t w = 0u, h = 0u;
    window->drawableSize(w, h);
    window->poll();
    ps2x::gs::GameWindowShown shown = window->show(1u, look);
    Picture got;
    check(shown.presented && !shown.have && !window->shownPixels(got.rgba, got.width, got.height),
          "without a renderer: the window presents, and has no picture to give", scale);

    const Picture m = patterned(640u, 448u);
    window->memoryPicture(asPixels(m, 640u, 448u, 2u, ps2x::gs::nextPictureOrder()));
    shown = window->show(2u, look);
    const ps2x::PictureRect rect = ps2x::pictureRectInWindow(640u, 448u, look.shape, static_cast<float>(w), static_cast<float>(h));
    check(shown.presented && shown.newPicture && shown.fromMemory && shown.field == 2u, "a picture from main memory is shown", scale);
    check(largestDifference(windowPicture(*window, look), expected(m, w, h, rect, false)) == 0, "letterboxed, pixel for pixel", scale);
    check(window->shownPixels(got.rgba, got.width, got.height) && got.width == m.width && got.height == m.height && sameColours(got.rgba, m.rgba),
          "and its pixels are the ones given", scale);

    // Another size for the same display: the window makes a new image for it.
    const Picture small = patterned(320u, 224u);
    window->memoryPicture(asPixels(small, 640u, 448u, 3u, ps2x::gs::nextPictureOrder()));
    shown = window->show(3u, look);
    check(shown.newPicture && shown.width == 320u && shown.displayWidth == 640u, "a picture of another size for the same display is shown", scale);
    check(largestDifference(windowPicture(*window, look), expected(small, w, h, rect, false)) == 0, "in the same place, every pixel four times", scale);
    for (int i = 0; i < 8; ++i) // more turns than the device has frame contexts: the pictures stay whole
        shown = window->show(3u, look);
    check(shown.presented && !shown.newPicture && largestDifference(windowPicture(*window, look), expected(small, w, h, rect, false)) == 0,
          "and still after more turns", scale);
    return true;
}
} // namespace

int DW5XL_TEST_MAIN(gs_game_window)()
{
    std::string aloneWhy;
    const bool alone = windowWithoutRenderer(aloneWhy);
    const int first = run(1u);
    if (first != 1)
        return 77;
    check(alone, "a window opens without a Vulkan renderer, on a device of its own" + (alone ? std::string() : " (" + aloneWhy + ")"), 1u);
    run(4u);
    windowGoesFirst();
    std::printf(g_failures ? "FAILED: %d check(s)\n" : "PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
