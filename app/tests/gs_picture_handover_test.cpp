// DW5XL-TEST issue 049 (amendment v3.12 step 3): the Vulkan renderer hands its finished pictures to a presenter on
// the graphics card (GSVulkanBackend::PresenterLink, src/lib/gs/gs_vulkan_handover.h). No window and no game data.
// The presenter here is a stand-in: a device object of its own on the renderer's context, with the link's lock for
// the queues, that samples a handed picture into a picture of its own and reads that one.
// At render scale 1 and 4:
//   1. the link: a context, a lock, a hand-over that is switched off; pictures are read back as before;
//   2. two drawings, A and B, read back the old way as the yardstick;
//   3. switched on: A's picture is handed over, is A pixel for pixel, carries its field and sizes, and is not read back;
//   4. a picture the presenter holds stays A while six pictures of B follow; the renderer is never refused a place
//      and makes no more than three images; the newest picture is B;
//   5. pixels wanted too: the picture is handed over and read back, both A;
//   6. a picture that needs the CPU's weave (interlaced field mode at 1x) is read back and not handed over, and the
//      read after handed-over pictures is woven as after a read picture of the same kind;
//   7. the output's scale changes: the held picture stays whole, new pictures come at the new size;
//   8. renderer and presenter on two threads, 120 fields of A and B in turn, the presenter holding the picture it
//      shows and for a moment the next one: every picture taken is whole and is the drawing of its field, and the
//      renderer is never refused a place;
//   9. switched off: pictures are read back again;
//  10. the renderer goes first: the presenter still samples the picture it holds, then lets go.
// Exit code 77: no Vulkan device (skipped).
#include "runtime/gs/gs_vulkan_backend.h"
#include "gs/gs_vulkan_handover.h"
#include "test_one_program.h"
#include "gs_test_pictures.h"

#include "device.hpp"
#include "context.hpp"
#include "thread_id.hpp"
#include "shader.hpp"
#include "shaders/slangmosh_iface.hpp"
#include "shaders/slangmosh.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
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

// The stand-in presenter.
struct Presenter
{
    std::shared_ptr<GSVulkanPresenterLink> link; // declared first: it goes last
    Vulkan::Device device;
    Vulkan::ResourceLayout layout;
    Vulkan::Program *blit = nullptr;
    HandedPictureViews views;
    static constexpr unsigned kFrameContexts = 4;

    explicit Presenter(std::shared_ptr<GSVulkanPresenterLink> from)
        : link(std::move(from)), views(device, kFrameContexts + 1u)
    {
        Util::register_thread_index(0);
        device.set_context(*link->context);
        device.init_frame_contexts(kFrameContexts);
        device.set_queue_lock([lock = link->queues] { lock->lock(); }, [lock = link->queues] { lock->unlock(); });
        ParallelGS::Shaders<> suite(device, layout, 0);
        blit = device.request_program(suite.quad, suite.blit);
    }
    ~Presenter()
    {
        device.wait_idle();
        views.clear();
    }

    // The picture sampled one to one into a picture of this device object, and that one read. Empty on failure.
    std::vector<uint8_t> pixels(const HandedPicture &picture)
    {
        Util::register_thread_index(0);
        const Vulkan::ImageView *view = views.view(picture);
        if (!view)
            return {};
        const uint32_t w = picture.width, h = picture.height;
        auto info = Vulkan::ImageCreateInfo::render_target(w, h, VK_FORMAT_R8G8B8A8_UNORM);
        info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        Vulkan::ImageHandle target = device.create_image(info);
        Vulkan::BufferCreateInfo bufferInfo{};
        bufferInfo.size = static_cast<VkDeviceSize>(w) * h * 4u;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.domain = Vulkan::BufferDomain::CachedHost;
        Vulkan::BufferHandle read = device.create_buffer(bufferInfo);
        if (!target || !read)
            return {};

        auto cmd = device.request_command_buffer();
        cmd->image_barrier(*target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        Vulkan::RenderPassInfo rp;
        rp.num_color_attachments = 1;
        rp.color_attachments[0] = &target->get_view();
        rp.clear_attachments = 1u;
        rp.store_attachments = 1u;
        cmd->begin_render_pass(rp);
        cmd->set_opaque_state();
        cmd->set_depth_test(false, false);
        cmd->set_cull_mode(VK_CULL_MODE_NONE);
        cmd->set_program(blit);
        cmd->set_texture(0, 0, *view, Vulkan::StockSampler::NearestClamp);
        cmd->set_viewport({0.0f, 0.0f, float(w), float(h), 0.0f, 1.0f});
        cmd->draw(3);
        cmd->end_render_pass();
        cmd->image_barrier(*target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        cmd->copy_image_to_buffer(*read, *target, 0, {}, {w, h, 1}, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
        cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                     VK_ACCESS_2_HOST_READ_BIT);
        Vulkan::Fence fence;
        device.submit(cmd, &fence);
        fence->wait();
        std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4u);
        const void *mapped = device.map_host_buffer(*read, Vulkan::MEMORY_ACCESS_READ_BIT);
        if (!mapped)
            return {};
        std::memcpy(out.data(), mapped, out.size());
        device.unmap_host_buffer(*read, Vulkan::MEMORY_ACCESS_READ_BIT);
        device.next_frame_context();
        views.nextFrame();
        return out;
    }
};

// False: no Vulkan renderer here.
bool run(uint32_t wantedScale)
{
    std::unique_ptr<GSVulkanBackend> backend;
    try
    {
        backend = std::make_unique<GSVulkanBackend>(); // throws without a Vulkan device
    }
    catch (const std::exception &error)
    {
        std::printf("SKIPPED: no Vulkan GS renderer here (%s)\n", error.what());
        return false;
    }
    std::vector<uint8_t> vram(4u << 20, 0u);
    backend->Initialize(vram.data(), static_cast<uint32_t>(vram.size()));
    backend->SetResolutionScale(wantedScale);
    const uint32_t scale = backend->GetResolutionScale();
    if (scale != wantedScale) // a GPU without 16 samples gives 2x for 4x: tested at what it gives
        std::printf("note  scale %ux: this GPU renders at %ux\n", wantedScale, scale);
    uint64_t tick = 10u;

    // 1. The link.
    std::shared_ptr<GSVulkanPresenterLink> link = backend->PresenterLink();
    check(link && link->context && link->queues && link->fields, "the link has a context, a lock and a hand-over", scale);
    if (!link || !link->context || !link->queues || !link->fields)
        return true;
    check(backend->PresenterLink() == link, "the same link for every caller", scale);
    check(link->windowSupport == backend->WindowSupport(), "the link says whether a swap chain can be made", scale);
    check(!link->fields->started(), "the hand-over is switched off until the presenter starts it", scale);
    VulkanGsHandover &fields = *link->fields;
    Presenter presenter(link);
    check(presenter.blit != nullptr, "the presenter has a device object of its own on the renderer's context", scale);

    // 2. The yardstick: A and B read back.
    drawA(*backend);
    const PresentationFrame readA = readBack(*backend, ++tick);
    drawB(*backend);
    const PresentationFrame readB = readBack(*backend, ++tick);
    check(readA && readB, "A and B are read back", scale);
    if (!readA || !readB)
        return true;
    check(!sameColours(readA.pixels, readB.pixels), "A and B differ", scale);
    check(!fields.takeNewest(), "switched off: nothing was handed over", scale);

    // 3. Switched on: A is handed over.
    fields.start();
    drawA(*backend);
    const uint64_t fieldA = ++tick;
    check(backend->SubmitFieldScanout(request(fieldA)), "switched on: the field is scanned out", scale);
    HandedPicture held = fields.takeNewest();
    check(static_cast<bool>(held), "the picture is handed over", scale);
    if (!held)
    {
        std::printf("      (the checks that need a handed picture are left out)\n");
        return true;
    }
    check(held.field == fieldA, "it carries its field", scale);
    check(held.width == readA.width && held.height == readA.height, "it has the read picture's size", scale);
    check(held.displayWidth == readA.displayWidth && held.displayHeight == readA.displayHeight,
          "it stands for the same display size", scale);
    check(sameColours(presenter.pixels(held), readA.pixels), "sampled by the presenter it is A, pixel for pixel", scale);
    check(!readArrives(*backend, fieldA) && fields.counts().alsoRead == 0u, "it was not read back", scale);

    // 4. The held picture stays A while six pictures of B follow.
    drawB(*backend);
    bool allScanned = true;
    for (int i = 0; i < 6; ++i)
        allScanned = backend->SubmitFieldScanout(request(++tick)) && allScanned;
    const uint64_t lastB = tick;
    check(allScanned, "six more fields are scanned out while the presenter takes nothing", scale);
    check(fields.counts().noPlace == 0u, "the renderer was never refused a place", scale);
    check(fields.counts().givenUp >= 4u, "older untaken pictures were given up", scale);
    check(fields.counts().images <= 3u, "no more than three images were made", scale);
    check(sameColours(presenter.pixels(held), readA.pixels), "the held picture is still A", scale);
    HandedPicture newest = fields.takeNewest();
    check(newest && newest.field == lastB, "the newest picture is the last field's", scale);
    check(newest && sameColours(presenter.pixels(newest), readB.pixels), "and it is B", scale);
    check(sameColours(presenter.pixels(held), readA.pixels), "the held picture is A beside it", scale);
    fields.release(newest);
    check(!newest, "a released picture is empty", scale);

    // 5. Pixels wanted too.
    fields.wantPixels(true);
    drawA(*backend);
    const PresentationFrame both = readBack(*backend, ++tick);
    HandedPicture alsoHanded = fields.takeNewest();
    check(both && sameColours(both.pixels, readA.pixels), "pixels wanted: the picture is read back, A", scale);
    check(alsoHanded && alsoHanded.field == tick && sameColours(presenter.pixels(alsoHanded), readA.pixels),
          "pixels wanted: it is handed over too, A", scale);
    check(fields.counts().alsoRead == 1u, "one picture went both ways", scale);
    fields.release(alsoHanded);
    fields.wantPixels(false);

    // 6. A picture that needs the CPU's weave is read back, and the weave after handed pictures is as after read ones.
    if (scale == 1u)
    {
        // The old way: a field-mode picture of A, a progressive picture, a field-mode picture of B.
        fields.stop();
        const uint64_t base = (tick | 1u) + 2u; // odd
        drawA(*backend);
        const PresentationFrame wovenA = readBack(*backend, base, true);
        drawB(*backend);
        readBack(*backend, base);
        const PresentationFrame wovenAfterRead = readBack(*backend, base + 1u, true);
        // The same with the hand-over on: the progressive picture is handed over, the field-mode ones are read.
        fields.start();
        const uint64_t handedBefore = fields.counts().handed;
        drawA(*backend);
        const PresentationFrame wovenA2 = readBack(*backend, base + 2u, true);
        check(wovenA && wovenA2 && fields.counts().handed == handedBefore && !fields.takeNewest(),
              "a picture that needs the weave is read back and not handed over", scale);
        drawB(*backend);
        check(backend->SubmitFieldScanout(request(base + 2u)), "a progressive picture between them is scanned out", scale);
        HandedPicture between = fields.takeNewest();
        check(static_cast<bool>(between) && fields.counts().handed == handedBefore + 1u, "and handed over", scale);
        fields.release(between);
        const PresentationFrame wovenAfterHanded = readBack(*backend, base + 3u, true);
        check(wovenAfterRead && wovenAfterHanded && wovenAfterRead.pixels == wovenAfterHanded.pixels,
              "the weave after a handed picture is the weave after a read picture", scale);
        tick = base + 3u;
    }

    // 7. The output's scale changes while the presenter holds A.
    {
        const uint32_t other = scale == 1u ? 2u : 1u;
        backend->SetResolutionScale(other);
        check(sameColours(presenter.pixels(held), readA.pixels), "after a change of scale the held picture is still A", scale);
        fields.stop();
        drawB(*backend);
        const PresentationFrame readOther = readBack(*backend, ++tick);
        fields.start();
        check(backend->SubmitFieldScanout(request(++tick)), "a field is scanned out at the other scale", scale);
        HandedPicture resized = fields.takeNewest();
        check(readOther && resized && resized.width == readOther.width && resized.height == readOther.height &&
                  (resized.width != held.width || resized.height != held.height),
              "its picture has the other scale's size", scale);
        check(resized && sameColours(presenter.pixels(resized), readOther.pixels), "and is B at that scale", scale);
        fields.release(resized);
        backend->SetResolutionScale(scale);
        check(sameColours(presenter.pixels(held), readA.pixels), "back at the first scale the held picture is still A", scale);
    }

    // 8. Two threads: this one is the renderer, the other the presenter. The presenter holds what a presenter holds:
    // the picture it shows and, for a moment, the next one. (The link's three places are counted for that; A, held
    // since step 3, is let go first.)
    fields.release(held);
    {
        std::atomic<bool> done{false};
        std::atomic<uint32_t> checked{0}, wrong{0}, torn{0}, backwards{0};
        const uint64_t first = (tick | 1u) + 1u; // even: A on even fields, B on odd ones
        std::thread other([&]
        {
            Util::register_thread_index(0);
            HandedPicture shown;
            uint64_t lastField = 0;
            while (true)
            {
                const bool last = done.load(std::memory_order_acquire);
                HandedPicture next = fields.takeNewest();
                if (next)
                {
                    const std::vector<uint8_t> got = presenter.pixels(next);
                    const bool isA = sameColours(got, readA.pixels), isB = sameColours(got, readB.pixels);
                    if (!isA && !isB)
                        torn.fetch_add(1u);
                    else if (isA != ((next.field & 1u) == 0u))
                        wrong.fetch_add(1u);
                    if (next.field <= lastField)
                        backwards.fetch_add(1u);
                    lastField = next.field;
                    checked.fetch_add(1u);
                    fields.release(shown); // the picture before, after the new one was sent
                    shown = std::move(next);
                }
                else if (last)
                    break;
                else
                    std::this_thread::yield();
            }
            fields.release(shown);
        });
        bool scanned = true;
        for (uint64_t field = first; field < first + 120u; ++field)
        {
            if ((field & 1u) == 0u)
                drawA(*backend);
            else
                drawB(*backend);
            scanned = backend->SubmitFieldScanout(request(field)) && scanned;
        }
        tick = first + 120u;
        done.store(true, std::memory_order_release);
        other.join();
        std::printf("note  scale %ux: two threads: 120 fields, %u pictures taken and checked\n", scale, checked.load());
        check(scanned, "two threads: every field was scanned out", scale);
        check(checked.load() >= 5u, "two threads: the presenter took pictures", scale);
        check(torn.load() == 0u, "two threads: every picture taken was whole (A or B)", scale);
        check(wrong.load() == 0u, "two threads: every picture was the drawing of its field", scale);
        check(backwards.load() == 0u, "two threads: no picture came after a newer one", scale);
        check(fields.counts().noPlace == 0u, "two threads: the renderer was never refused a place", scale);
    }
    drawA(*backend);
    check(backend->SubmitFieldScanout(request(++tick)), "a field of A is scanned out for the presenter to keep", scale);
    held = fields.takeNewest();
    check(held && held.field == tick && sameColours(presenter.pixels(held), readA.pixels), "the presenter holds A again", scale);

    // 9. Switched off: read back again.
    fields.stop();
    const uint64_t handedBefore = fields.counts().handed;
    drawB(*backend);
    const PresentationFrame after = readBack(*backend, ++tick);
    check(after && sameColours(after.pixels, readB.pixels) && fields.counts().handed == handedBefore,
          "switched off: the picture is read back and not handed over", scale);

    // 10. The renderer goes first.
    const VulkanGsHandover::Counts counts = fields.counts();
    std::printf("note  scale %ux: handed %llu, taken %llu, superseded %llu, given up %llu, images %llu\n", scale,
                static_cast<unsigned long long>(counts.handed), static_cast<unsigned long long>(counts.taken),
                static_cast<unsigned long long>(counts.superseded), static_cast<unsigned long long>(counts.givenUp),
                static_cast<unsigned long long>(counts.images));
    backend.reset();
    check(sameColours(presenter.pixels(held), readA.pixels), "the renderer is gone: the held picture is still A", scale);
    fields.release(held);
    return true;
}
} // namespace

int DW5XL_TEST_MAIN(gs_picture_handover)()
{
    if (!run(1u))
        return 77;
    run(4u);
    std::printf(g_failures ? "FAILED: %d check(s)\n" : "PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
