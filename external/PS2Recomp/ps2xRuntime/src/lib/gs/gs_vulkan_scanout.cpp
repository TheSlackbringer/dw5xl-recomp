// SPDX-License-Identifier: GPL-3.0-only
#include "gs_vulkan_scanout.h"

#include "gs_interface.hpp"
#include "device.hpp"
#include "buffer.hpp"
#include "fence.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace
{
constexpr unsigned hostWidth = 640, hostHeight = 512;
struct Circuit
{
    bool enabled = false;
    ParallelGS::DISPFBBits framebuffer{};
    ParallelGS::DISPLAYBits display{};
    unsigned width = 0, height = 0;
};

Circuit decodeCircuit(bool enabled, uint64_t framebuffer, uint64_t display, bool frameFields)
{
    Circuit result;
    result.framebuffer = ParallelGS::Reg64<ParallelGS::DISPFBBits>(framebuffer).desc;
    result.display = ParallelGS::Reg64<ParallelGS::DISPLAYBits>(display).desc;
    result.enabled = enabled && result.framebuffer.FBW != 0 && display != 0;
    if (!result.enabled) return result;
    result.width = (static_cast<unsigned>(result.display.DW) + 1u) /
                   (static_cast<unsigned>(result.display.MAGH) + 1u);
    result.height = (static_cast<unsigned>(result.display.DH) + 1u) /
                    (static_cast<unsigned>(result.display.MAGV) + 1u);
    if (frameFields) result.height = (result.height + 1u) / 2u;
    if (result.width < 64 || result.width > hostWidth || result.height < 64 || result.height > 448)
        throw VulkanGsScanoutUnsupported("GPU logical scanout supports widths 64..640 and source heights 64..448");
    return result;
}

// GSPresentationRequest describes a logical viewport but has no SMODE1 clock
// mode. Re-express its decoded source rectangle in a progressive NTSC-derived
// viewport. This avoids analog-mode cropping of tall FRAME buffers, while the
// actual sampling/merge remains ParallelGS GPU work. Relative CRT offsets are
// retained in source-pixel units; no game identity is consulted.
void normalizeCircuit(Circuit &c, unsigned minX, unsigned minY, bool frameFields)
{
    if (!c.enabled) return;
    const unsigned x = (static_cast<unsigned>(c.display.DX) - minX) /
                       (static_cast<unsigned>(c.display.MAGH) + 1u);
    unsigned y = (static_cast<unsigned>(c.display.DY) - minY) /
                 (static_cast<unsigned>(c.display.MAGV) + 1u);
    if (frameFields) y /= 2u;
    if (318u + x * 2u > 4095u || 50u + y > 2047u)
        throw VulkanGsScanoutUnsupported("GPU logical scanout CRT offsets exceed the viewport encoding");
    c.display.DX = 318u + x * 2u;
    c.display.DY = 50u + y;
    c.display.DW = c.width * 2u - 1u;
    c.display.DH = c.height - 1u;
    c.display.MAGH = 1;
    c.display.MAGV = 0;
}

bool noWeave()
{
    const char *value = std::getenv("PS2X_NO_WEAVE");
    return value && *value && *value != '0';
}
}

PresentationFrame presentVulkanGs(ParallelGS::GSInterface &gs, Vulkan::Device &device,
                                  const GSPresentationRequest &request)
{
    using namespace ParallelGS;
    using namespace Vulkan;
    auto pmode = Reg64<PMODEBits>(request.pmode).desc;
    const bool interlaced = (request.smode2 & 1u) != 0;
    const bool frameMode = (request.smode2 & 2u) != 0;
    const bool frameFields = interlaced && frameMode;
    Circuit first = decodeCircuit(pmode.EN1 != 0, request.dispfb1, request.display1, frameFields);
    Circuit second = decodeCircuit(pmode.EN2 != 0, request.dispfb2, request.display2, frameFields);
    if (!first.enabled && !second.enabled) return {};
    pmode.EN1 = first.enabled; pmode.EN2 = second.enabled;
    const unsigned sourceHeight = std::max(first.height, second.height);
    const bool movieFields = frameFields && sourceHeight <= 256;
    if (movieFields && ((first.enabled && first.height != 224) || (second.enabled && second.height != 224)))
        throw VulkanGsScanoutUnsupported("GPU bounded FRAME-field presentation currently requires 224 source rows");

    auto &priv = gs.get_priv_register_state();
    priv.pmode = pmode;
    priv.bgcolor = Reg64<BGCOLORBits>(request.bgcolor).desc;
    priv.smode1 = {};
    priv.smode1.LC = SMODE1Bits::LC_ANALOG;
    priv.smode1.CMOD = movieFields ? SMODE1Bits::CMOD_NTSC : SMODE1Bits::CMOD_PROGRESSIVE;
    priv.smode2 = {};
    priv.smode2.INT = movieFields;
    priv.smode2.FFMD = movieFields;
    priv.smode2.DPMS = (request.smode2 >> 2u) & 3u;
    if (movieFields)
    {
        // Align the logical field viewport with NTSC's nominal non-overscan
        // origin. Original MAGH/MAGV still control GPU source sampling.
        for (auto *c : {&first, &second}) if (c->enabled)
        { c->display.DX = 636; c->display.DY = 50; }
    }
    else
    {
        const unsigned minX = first.enabled && second.enabled
            ? std::min<unsigned>(first.display.DX, second.display.DX)
            : static_cast<unsigned>(first.enabled ? first.display.DX : second.display.DX);
        const unsigned minY = first.enabled && second.enabled
            ? std::min<unsigned>(first.display.DY, second.display.DY)
            : static_cast<unsigned>(first.enabled ? first.display.DY : second.display.DY);
        normalizeCircuit(first, minX, minY, frameFields);
        normalizeCircuit(second, minX, minY, frameFields);
    }
    priv.dispfb1 = first.framebuffer; priv.display1 = first.display;
    priv.dispfb2 = second.framebuffer; priv.display2 = second.display;

    VSyncInfo info{};
    info.phase = static_cast<uint32_t>(request.vsyncTick & 1u);
    info.dst_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    const bool bobFields = movieFields && !noWeave();
    info.dst_stage = bobFields ? VK_PIPELINE_STAGE_2_BLIT_BIT : VK_PIPELINE_STAGE_2_COPY_BIT;
    info.dst_access = VK_ACCESS_2_TRANSFER_READ_BIT;
    info.force_progressive = !movieFields;
    info.adapt_to_internal_horizontal_resolution = true;
    info.raw_circuit_scanout = !movieFields;
    // Present calls need not arrive once per alternating field. Temporal weave
    // assumes that cadence and can combine repeated or stale fields, producing
    // displaced scanlines in text. Request the current field and bob it on GPU.
    // PS2X_NO_WEAVE keeps the unscaled field for diagnostics.
    info.skip_deinterlace = movieFields;
    gs.flush();
    auto output = gs.vsync(info);
    if (!output.image) throw std::runtime_error("ParallelGS GPU scanout returned no image");
    const auto width = output.image->get_width();
    auto height = output.image->get_height();
    if (!width || !height || width > hostWidth || height > hostHeight)
        throw VulkanGsScanoutUnsupported("ParallelGS image exceeds the fixed host viewport");
    if (output.image->get_create_info().format != VK_FORMAT_R8G8B8A8_UNORM)
        throw std::runtime_error("ParallelGS GPU scanout returned an unexpected pixel format");

    auto command = device.request_command_buffer();
    if (movieFields)
    {
        // With skipped deinterlacing, ParallelGS leaves the merged field in
        // READ_ONLY rather than honoring dst_layout. Make its writes visible
        // for our blit (or diagnostic raw-field copy) explicitly.
        command->image_barrier(*output.image, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                               info.dst_stage, VK_ACCESS_2_TRANSFER_READ_BIT);
    }
    if (bobFields)
    {
        if (height != 224)
            throw std::runtime_error("GPU FRAME-field scanout did not return 224 rows");
        auto imageInfo = ImageCreateInfo::immutable_2d_image(width, height * 2u, VK_FORMAT_R8G8B8A8_UNORM);
        imageInfo.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        auto bob = device.create_image(imageInfo);
        if (!bob) throw std::runtime_error("GPU bob image allocation failed");
        command->image_barrier(*bob, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               0, 0, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        command->blit_image(*bob, *output.image, {}, {int32_t(width), int32_t(height * 2u), 1},
                             {}, {int32_t(width), int32_t(height), 1}, 0, 0, 0, 0, 1, VK_FILTER_NEAREST);
        command->image_barrier(*bob, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        output.image = std::move(bob);
        height *= 2u;
    }

    BufferCreateInfo bufferInfo{};
    bufferInfo.size = static_cast<VkDeviceSize>(width) * height * 4u;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.domain = BufferDomain::CachedHost;
    auto readback = device.create_buffer(bufferInfo);
    if (!readback) throw std::runtime_error("GPU scanout readback allocation failed");
    // Progressive vsync or the field transition/blit above supplies COPY/read
    // visibility. Both operations use Granite's generic queue submission.
    command->copy_image_to_buffer(*readback, *output.image, 0, {}, {width, height, 1},
                                 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
    command->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    Fence fence;
    device.submit(command, &fence);
    if (!fence || !fence->wait_timeout(10'000'000'000ull))
        throw std::runtime_error("GPU scanout readback fence did not complete within 10 seconds");
    const auto *mapped = static_cast<const uint8_t *>(device.map_host_buffer(*readback, MEMORY_ACCESS_READ_BIT));
    if (!mapped) throw std::runtime_error("GPU scanout readback mapping failed");
    PresentationFrame result{};
    result.width = width; result.height = height;
    result.displayFbp = result.sourceFbp = first.enabled ? first.framebuffer.FBP : second.framebuffer.FBP;
    result.pixels.assign(hostWidth * hostHeight * 4u, 0);
    for (unsigned y = 0; y < height; ++y)
    {
        auto *row = result.pixels.data() + static_cast<size_t>(y) * hostWidth * 4u;
        std::memcpy(row, mapped + static_cast<size_t>(y) * width * 4u, width * 4u);
        for (unsigned x = 0; x < width; ++x) row[x * 4u + 3u] = 255;
    }
    device.unmap_host_buffer(*readback, MEMORY_ACCESS_READ_BIT);
    return result;
}
