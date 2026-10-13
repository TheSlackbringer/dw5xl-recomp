#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace ps2x::iop::detail
{
    // The IOP-visible SPU2: register offsets are relative to 0x1f900000;
    // all sample addresses are 20-bit halfword addresses in shared 2 MiB RAM.
    class IopSpu2
    {
    public:
        static constexpr uint32_t CyclesPerFrame = 768;
        static constexpr uint32_t SampleRate = 48000;
        struct Statistics
        {
            uint64_t frames = 0;
            uint64_t nonzeroFrames = 0;
            uint64_t dmaWrites = 0;
            uint64_t dmaReads = 0;
            uint64_t dmaBytes = 0;
            uint64_t keyOns = 0;
            uint64_t decodedBlocks = 0;
            uint64_t irqAssertions = 0;
        };

        IopSpu2();
        ~IopSpu2();
        IopSpu2(const IopSpu2 &) = delete;
        IopSpu2 &operator=(const IopSpu2 &) = delete;

        void reset(); // Does not discard callbacks installed by the IOP host.
        [[nodiscard]] uint16_t read16(uint32_t registerOffset) const;
        void write16(uint32_t registerOffset, uint16_t value);
        void dmaTransfer(unsigned core, bool toSpu, uint8_t *iopData, uint32_t wordCount32);
        [[nodiscard]] uint64_t dmaCompletionCycles(unsigned core, uint32_t wordCount32) const;
        void completeDma(unsigned core);
        void cancelDma(unsigned core);
        void advance(uint64_t iopCycles);
        void flushPcm();
        void setPcmCallback(std::function<void(std::span<const int16_t>)> callback);
        void setIrqCallback(std::function<void()> callback);
        [[nodiscard]] Statistics statistics() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
