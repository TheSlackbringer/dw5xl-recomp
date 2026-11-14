// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// DW5XL (issue 049, amendment v3.12 step 3): what a presenter needs from the Vulkan renderer to show its pictures
// without reading them back into main memory.
//
// The presenter draws on a device object of its own (Vulkan::Device) made on the renderer's context: it then never
// waits for the renderer's open command buffers, and the renderer never for the presenter's (measured with the test
// window program, work log "M4-1"). Both device objects send to the same queues, so both take one lock around every
// queue call (GSVulkanPresenterLink::queues).
//
// A finished picture is copied on the graphics card into a picture of the hand-over's own (VulkanGsHandover) and is
// the presenter's until it gives it back. The renderer's own scan-out pictures cannot be lent: paraLLEl-GS draws
// into each again four scan-outs later (runtime patch 0003). The copy stays on the card; it takes the place of the
// copy into a read buffer.
//
// Lifetimes are counted: a picture keeps its image and the renderer's device alive, so nothing here depends on who
// goes first.

#include "runtime/gs/gs_handover_places.h"
#include "context.hpp"
#include "device.hpp"
#include "image.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

// A finished picture on the graphics card, handed from the renderer to a presenter.
struct HandedPicture
{
    // Keeps the renderer's device object alive while the picture is (declared first: it goes last).
    std::shared_ptr<void> renderer;
    // The picture, an image of the renderer's device object: R8G8B8A8_UNORM, in SHADER_READ_ONLY_OPTIMAL, complete
    // for everything sent to the queue after it was handed over. Its alpha is the PS2 frame buffer's and means
    // nothing on a display: draw it opaque. To sample it on another device object use HandedPictureViews.
    Vulkan::ImageHandle image;
    // One number per image. The same image comes back with later pictures (the hand-over has a few and reuses them).
    uint64_t imageId = 0;
    uint32_t place = ps2x::gs::HandoverPlaces::kNone;
    uint32_t width = 0, height = 0;               // the image's pixels
    uint32_t displayWidth = 0, displayHeight = 0; // the PS2 display's size it stands for (PresentationFrame's)
    uint32_t displayFbp = 0, sourceFbp = 0;
    uint64_t field = 0;      // the PS2 field it was scanned out for
    uint64_t order = 0;      // ps2x::gs::nextPictureOrder() at the scan-out: which of two pictures is the newer
    uint64_t tag = 0;        // an in-between picture's tag, 0 for a field's picture
    int8_t fieldTarget = -1; // the field the picture was drawn for, -1 if not known
    // Signalled when the graphics card has finished the picture: the copy into its image, which comes after
    // everything that drew it. It can be waited for on any thread. (Issue 049 v3.12 step 5: a picture sent to the
    // card is not yet a picture the card has drawn; found on the real display 2026-11-14.)
    Vulkan::Fence done;

    explicit operator bool() const { return static_cast<bool>(image); }
};

// The pictures handed over from one scan-out of the renderer (the game's fields).
class VulkanGsHandover
{
public:
    struct Counts
    {
        uint64_t handed = 0;     // pictures copied for the presenter
        uint64_t taken = 0;      // of them, taken by the presenter
        uint64_t superseded = 0; // not taken: a newer one was there when the presenter came
        uint64_t givenUp = 0;    // not taken: the renderer needed the place, or the output changed
        uint64_t noPlace = 0;    // not handed over: the presenter held every place
        uint64_t alsoRead = 0;   // handed over and read back into main memory too (pixels were wanted)
        uint64_t images = 0;     // images made
    };

    explicit VulkanGsHandover(std::shared_ptr<void> renderer, uint32_t places = 3u);

    // --- The presenter's side. Any thread; none of it waits for the renderer. ---
    // From the next scan-out on, a picture that needs no work in main memory is handed over and not read back.
    void start() { m_started.store(true, std::memory_order_release); }
    // From the next scan-out on, pictures are read back as without a presenter.
    void stop() { m_started.store(false, std::memory_order_release); }
    bool started() const { return m_started.load(std::memory_order_acquire); }
    // Handed-over pictures are read back into main memory as well (a frame dump, a thumbnail, a trace).
    void wantPixels(bool wanted) { m_pixels.store(wanted, std::memory_order_release); }
    bool pixelsWanted() const { return m_pixels.load(std::memory_order_acquire); }
    // The newest picture of a field up to `maxField`, or an empty one. Older untaken pictures are given up.
    HandedPicture takeNewest(uint64_t maxField = UINT64_MAX);
    // For a presenter that needs every picture (the in-between pictures): the oldest picture not taken yet, or an
    // empty one. No picture is given up by being passed over.
    HandedPicture takeOldest();
    // Done with a picture. Call it after the last command buffer that uses the picture was sent to the queue: the
    // renderer may copy the next picture into the image from then on.
    void release(HandedPicture &picture);
    Counts counts() const;

    // --- The renderer's side. The caller holds the renderer's device lock. ---
    struct Target
    {
        uint32_t place = ps2x::gs::HandoverPlaces::kNone;
        Vulkan::ImageHandle image;
        uint64_t imageId = 0;
        explicit operator bool() const { return static_cast<bool>(image); }
    };
    // An image of width x height to copy the next picture into, or an empty target (the presenter holds every
    // place, or no image could be made). Every target is ended with finish() or abandon().
    Target begin(Vulkan::Device &device, uint32_t width, uint32_t height);
    // The copy into the target was sent to the queue: the picture is the presenter's to take. `picture` carries the
    // sizes and the field; image, place and renderer are filled in here.
    void finish(const Target &target, HandedPicture picture, bool alsoRead);
    void abandon(const Target &target);
    // The output changes or the renderer starts over: untaken pictures are given up and unused images let go.
    // A picture the presenter holds stays whole until it is released.
    void forget();

private:
    struct Place
    {
        Vulkan::ImageHandle image;
        uint64_t imageId = 0;
        HandedPicture picture; // while ready
    };
    const std::shared_ptr<void> m_renderer;
    mutable std::mutex m_mutex;
    ps2x::gs::HandoverPlaces m_rules;
    std::vector<Place> m_places;
    Counts m_counts;
    std::atomic<bool> m_started{false};
    std::atomic<bool> m_pixels{false};
};

// What GSVulkanBackend::PresenterLink() gives a presenter.
struct GSVulkanPresenterLink
{
    // Keeps the renderer's device object alive while the link is (declared first: it goes last).
    std::shared_ptr<void> renderer;
    // The renderer's Vulkan context: the presenter makes a device object of its own on it.
    Vulkan::ContextHandle context;
    // One lock for the queues of both device objects: give lock() and unlock() to Device::set_queue_lock of the
    // presenter's device object. The renderer's has it set.
    std::shared_ptr<std::mutex> queues;
    // The game's fields. Switched off until start().
    std::shared_ptr<VulkanGsHandover> fields;
    // The in-between pictures (picture reads, issue 049 v3.2), each with its tag, taken oldest first (takeOldest).
    // Switched off until start(); a picture it has no place for is given up or not handed over, never waited for.
    std::shared_ptr<VulkanGsHandover> pictures;
    // Whether the renderer's device was made with window support (a swap chain can be made on it).
    bool windowSupport = false;
};

// The presenter's side: handed pictures as views of the presenter's own device object. (A view made by the
// renderer's device object must not be bound there: the library numbers its resources per device object.)
// One thread, the presenter's.
class HandedPictureViews
{
public:
    // `keepFrames`: for how many frames after its last use a view and its image are kept, at least the number of
    // frame contexts of the presenter's device object plus one. The device object waits for a frame context's work
    // on the graphics card before it uses the context again; an image let go after that many frames is no longer
    // used by anything the presenter sent.
    HandedPictureViews(Vulkan::Device &presenter, uint32_t keepFrames);
    // The view to sample `picture` with in this frame, or null if none could be made.
    const Vulkan::ImageView *view(const HandedPicture &picture);
    // Once per frame context of the presenter's device object.
    void nextFrame();
    // Lets go of everything. Only after the presenter's device object is idle.
    void clear() { m_entries.clear(); }
    size_t size() const { return m_entries.size(); }

private:
    struct Entry
    {
        std::shared_ptr<void> renderer; // declared first: it goes last
        Vulkan::ImageHandle source;     // the renderer's image, kept alive
        Vulkan::ImageHandle wrapped;    // the same image as one of the presenter's device object (not its owner)
        Vulkan::ImageViewHandle view;
        uint64_t imageId = 0;
        uint64_t usedFrame = 0;
    };
    Vulkan::Device &m_device;
    const uint64_t m_keepFrames;
    uint64_t m_frame = 0;
    std::vector<Entry> m_entries;
};
