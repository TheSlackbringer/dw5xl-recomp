// DW5XL (PAL SLES_538.60) native still-image decoding.
//
// The game decodes compressed still images (loading screens, backgrounds) with raw IPU IDEC
// through a small queue: FUN_001f0a40(slot, image) builds a DMA tag for the image inside an
// ipum archive, programs IPU/DMAC ch4/ch3 when the queue is idle, and the DMAC ch3 handler
// (0x1f0d30) marks the slot done (state 3) and starts the next queued job. Users then wait in
// FUN_001f0c80 for the queue to drain and pick up state 3.
//
// This replaces FUN_001f0a40 with a native decode that writes the slot's next output buffer
// and completes the job immediately, so the queue never holds IPU work.
//
// Slot layout (0x5442f0 + slot * 0x50):
//   +0x10 state (1 idle, 2 queued, 3 done)      +0x14 ipum archive (+8 w, +10 h, +0x10 {offset,size}[])
//   +0x18 width/height (u16)  +0x20 macroblocks  +0x28/+0x2c output buffers (mb * 1024 bytes RGBA)
//   +0x30 output buffer toggle                   +0x34 image index
// Archive frames start with the IPU_CTRL picture flags byte; IDEC data follows (QSC 1).
#include "game_overrides.h"
#include "ipu_idec.h"
#include "ps2_runtime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    constexpr uint32_t kQueueImage = 0x001F0A40u;
    constexpr uint32_t kSlots = 0x005442F0u;
    constexpr uint32_t kSlotStride = 0x50u;
    constexpr uint32_t kSlotCount = 16u;
    constexpr uint32_t kMaxMacroblocks = 64u * 64u; // 1024x1024

    uint32_t read32(uint8_t *rdram, uint32_t address)
    {
        uint32_t value;
        std::memcpy(&value, getMemPtr(rdram, address), sizeof(value));
        return value;
    }
    uint16_t read16(uint8_t *rdram, uint32_t address)
    {
        uint16_t value;
        std::memcpy(&value, getMemPtr(rdram, address), sizeof(value));
        return value;
    }
    void write32(uint8_t *rdram, uint32_t address, uint32_t value)
    {
        std::memcpy(getMemPtr(rdram, address), &value, sizeof(value));
    }

    const char *dumpDirectory()
    {
        static const char *dir = [] {
            const char *v = std::getenv("PS2X_IPU_DUMP");
            return (v && *v) ? v : nullptr;
        }();
        return dir;
    }

    void dumpFile(const std::string &path, const uint8_t *data, size_t size)
    {
        if (std::FILE *file = std::fopen(path.c_str(), "wb"))
        {
            std::fwrite(data, 1, size, file);
            std::fclose(file);
        }
    }

    void queueImage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t slot = getRegU32(ctx, 4);
        const uint32_t image = getRegU32(ctx, 5);
        ctx->pc = getRegU32(ctx, 31);
        if (slot >= kSlotCount)
            return;

        const uint32_t base = kSlots + slot * kSlotStride;
        const uint32_t archive = read32(rdram, base + 0x14u);
        const uint32_t macroblocks = read32(rdram, base + 0x20u) & 0x3FFFFFu;
        const uint16_t width = read16(rdram, base + 0x18u);
        const uint16_t height = read16(rdram, base + 0x1Au);
        const uint32_t entry = archive + 0x10u + image * 8u;
        const uint32_t offset = read32(rdram, entry);
        const uint32_t size = read32(rdram, entry + 4u);

        const uint32_t toggle = read32(rdram, base + 0x30u) ^ 1u;
        write32(rdram, base + 0x30u, toggle);
        write32(rdram, base + 0x34u, image);
        const uint32_t output = read32(rdram, base + 0x28u + toggle * 4u);

        if (archive == 0u || size < 2u || macroblocks == 0u || macroblocks > kMaxMacroblocks || output == 0u)
        {
            std::fprintf(stderr, "[dw5xl:ipu] slot %u image %u: bad job archive=0x%x size=%u mbs=%u out=0x%x\n",
                         slot, image, archive, size, macroblocks, output);
            write32(rdram, base + 0x10u, 3u);
            return;
        }

        std::vector<uint8_t> stream(size);
        for (uint32_t i = 0; i < size; ++i)
            stream[i] = *getMemPtr(rdram, archive + offset + i);
        std::vector<uint8_t> pixels(static_cast<size_t>(macroblocks) * 1024u, 0u);

        ipu::IdecConfig config;
        config.flags = stream[0];
        config.quantiserCode = 1;
        const ipu::IdecResult result = ipu::decodeIdec(stream.data(), stream.size(), 8u, config, macroblocks, pixels.data());
        if (!result.error.empty() || result.macroblocks != macroblocks)
            std::fprintf(stderr, "[dw5xl:ipu] slot %u image %u (%ux%u flags=0x%02x): decoded %u/%u macroblocks%s%s\n",
                         slot, image, width, height, config.flags, result.macroblocks, macroblocks,
                         result.error.empty() ? "" : ": ", result.error.c_str());

        for (size_t i = 0; i < pixels.size(); ++i)
            *getMemPtr(rdram, output + static_cast<uint32_t>(i)) = pixels[i];
        static const bool ipuTrace = std::getenv("PS2X_IPU_TRACE") != nullptr; // DW5XL-DEBUG (read once: issue 025)
        if (ipuTrace)
            std::fprintf(stderr, "[dw5xl:ipu] slot %u image %u %ux%u archive=0x%x data=0x%x+%u out=0x%x..0x%x\n", slot, image,
                         width, height, archive, archive + offset, size, output, output + macroblocks * 1024u);

        if (const char *dir = dumpDirectory())
        {
            static unsigned sequence = 0;
            char name[160];
            std::snprintf(name, sizeof(name), "%s/ipu_%04u_slot%u_img%u_%ux%u", dir, sequence++, slot, image, width, height);
            dumpFile(std::string(name) + ".bin", stream.data(), stream.size());
            dumpFile(std::string(name) + ".rgba", pixels.data(), pixels.size());
        }

        write32(rdram, base + 0x10u, 3u); // what the DMAC ch3 completion handler sets
    }

    void applyNativeImages(PS2Runtime &runtime)
    {
        if (runtime.memory().read32(kQueueImage) != 0x27BDFFC0u || !runtime.hasFunction(kQueueImage))
        {
            std::fprintf(stderr, "[dw5xl:ipu] declined: unexpected code at 0x%08x\n", kQueueImage);
            return;
        }
        if (const std::string error = ipu::selfTest(); !error.empty())
        {
            std::fprintf(stderr, "[dw5xl:ipu] declined: decoder self-test failed: %s\n", error.c_str());
            return;
        }
        runtime.replaceFunction(kQueueImage, &queueImage);
        std::fprintf(stderr, "[dw5xl:ipu] installed native IPU still-image decoder\n");
    }
}

PS2_REGISTER_GAME_OVERRIDE("DW5XL native IPU still images", "SLES_538.60", 0u, 0u, &applyNativeImages)
