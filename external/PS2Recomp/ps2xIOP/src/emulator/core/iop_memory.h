#pragma once
#include "ps2x/state_archive.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "iop_spu2.h"

namespace ps2x::iop::detail
{
    class IopMemory
    {
    public:
        // DW5XL (issue 027): save-state support.
        void serializeState(ps2x::StateArchive &archive);
        static constexpr uint32_t RamSize = 2u * 1024u * 1024u;
        static constexpr uint32_t ScratchBase = 0x1F800000u;
        static constexpr uint32_t ScratchSize = 0x400u;
        static constexpr uint32_t HardwareBase = 0x1F801000u;
        static constexpr uint32_t HardwareEnd = 0x1F900000u;
        static constexpr uint32_t Spu2Base = 0x1F900000u;
        static constexpr uint32_t Spu2End = 0x1FA00000u;
        static constexpr uint32_t SifBase = 0x1D000000u;
        static constexpr uint32_t SifEnd = 0x1D001000u;
        // IRX modules load from 0x10000 upward (kModuleLoadBase); KF4's nine
        // modules end below 0x60000. The heap starts above that and runs up to
        // the emulator's call-stack area.
        static constexpr uint32_t HeapBase = 0x00080000u;
        static constexpr uint32_t HeapLimit = 0x001F0000u;

        struct Allocation
        {
            uint32_t address = 0;
            uint32_t size = 0;
        };

        struct DmaStart
        {
            int irq = 0;
            uint64_t delayCycles = 0;
        };

        IopMemory();

        void reset();

        [[nodiscard]] uint8_t read8(uint32_t address) const;
        [[nodiscard]] uint16_t read16(uint32_t address) const;
        [[nodiscard]] uint32_t read32(uint32_t address) const;
        void write8(uint32_t address, uint8_t value);
        void write16(uint32_t address, uint16_t value);
        void write32(uint32_t address, uint32_t value);

        [[nodiscard]] bool readRam(uint32_t address, void *destination, size_t size) const;
        [[nodiscard]] bool writeRam(uint32_t address, const void *source, size_t size);
        [[nodiscard]] bool zeroRam(uint32_t address, size_t size);
        [[nodiscard]] bool ownsRamRange(uint32_t address, size_t size) const;
        [[nodiscard]] bool isHardwareAddress(uint32_t address) const;
        [[nodiscard]] std::string readString(uint32_t address, size_t limit = 1024u) const;

        [[nodiscard]] uint32_t allocate(uint32_t size, uint32_t alignment = 16u, std::optional<uint32_t> fixed = std::nullopt);
        [[nodiscard]] bool freeAllocation(uint32_t address);
        [[nodiscard]] uint32_t maxFreeMemory() const;
        [[nodiscard]] std::optional<Allocation> allocationContaining(uint32_t address) const;

        [[nodiscard]] uint32_t interruptStatus() const noexcept { return m_interruptStatus; }
        [[nodiscard]] uint32_t interruptMask() const noexcept { return m_interruptMask; }
        [[nodiscard]] uint32_t interruptControl() const noexcept { return m_interruptControl; }
        void setInterruptStatus(uint32_t value) noexcept { m_interruptStatus = value; }
        void setInterruptMask(uint32_t value) noexcept { m_interruptMask = value; }
        void setInterruptControl(uint32_t value) noexcept { m_interruptControl = value & 1u; }

        [[nodiscard]] std::optional<DmaStart> takeDmaStart() noexcept;
        [[nodiscard]] std::span<const uint8_t> ram() const noexcept { return m_ram; }
        [[nodiscard]] IopSpu2 &spu2() noexcept { return m_spu2; }
        void completeSoundDma(int irq);
        [[nodiscard]] bool soundDmaActive(int irq) const;

        [[nodiscard]] static uint32_t physicalAddress(uint32_t address) noexcept;

        // Issue 009 v4.5: native IOP code (ps2x/iop/iop_native.h) works on IOP RAM directly. The code map has one bit
        // per 32-bit word of RAM translated into native code; any RAM write over such a word is noted (taken by the
        // emulator, which then drops the module whose code changed).
        [[nodiscard]] uint8_t *ramData() noexcept { return m_ram.data(); }
        [[nodiscard]] uint8_t *ownedData() noexcept { return m_owned.data(); }
        [[nodiscard]] const uint32_t *nativeCodeWords() const noexcept { return m_nativeCodeWords.data(); }
        void markNativeCode(uint32_t address, uint32_t size, bool native) noexcept;
        void clearNativeCode() noexcept;
        [[nodiscard]] bool takeNativeCodeWrite() noexcept
        {
            const bool written = m_nativeCodeWritten;
            m_nativeCodeWritten = false;
            return written;
        }
        // Check mode (PS2X_IOP_LIFT_VERIFY): every RAM write's (address, size) while a log is set.
        void setWriteLog(std::vector<std::pair<uint32_t, uint32_t>> *log) noexcept { m_writeLog = log; }
        [[nodiscard]] std::vector<std::pair<uint32_t, uint32_t>> *writeLog() const noexcept { return m_writeLog; }
        // Check mode: while a replay is set, accesses outside IOP RAM (hardware, scratchpad) take the recorded values
        // of the native run and touch nothing; any difference from the record is noted.
        struct HwAccess
        {
            uint32_t address;
            uint32_t size;
            uint32_t value;
            bool write;
            uint64_t cycle; // the IOP cycle of the access
        };
        struct HwReplay
        {
            const std::vector<HwAccess> *records = nullptr;
            const uint64_t *cycle = nullptr; // the emulator's IOP cycle count during the replay
            size_t next = 0u;
            bool mismatch = false;
        };
        void setHwReplay(HwReplay *replay) noexcept { m_hwReplay = replay; }
        // Check mode: while set, every RAM write (and every change of the written-byte map) first saves the bytes and
        // flags it replaces, so a kernel call's effects can be undone and re-applied.
        struct RamUndo
        {
            uint32_t address;
            std::vector<uint8_t> data, owned;
        };
        void setRamUndo(std::vector<RamUndo> *undo) noexcept { m_ramUndo = undo; }
        [[nodiscard]] std::vector<RamUndo> *ramUndo() const noexcept { return m_ramUndo; }

    private:
        [[nodiscard]] uint32_t readHardware32(uint32_t address) const;
        void writeHardware32(uint32_t address, uint32_t value);
        void markOwned(uint32_t address, size_t size);

        std::vector<uint8_t> m_ram;
        std::vector<uint8_t> m_owned;
        std::vector<uint32_t> m_nativeCodeWords = std::vector<uint32_t>(RamSize / 128u); // issue 009 v4.5
        bool m_nativeCodeWritten = false;
        std::vector<std::pair<uint32_t, uint32_t>> *m_writeLog = nullptr;
        HwReplay *m_hwReplay = nullptr;
        std::vector<RamUndo> *m_ramUndo = nullptr;
        void noteRamWrite(uint32_t phys, size_t size)
        {
            if (m_ramUndo && size != 0u && phys < RamSize && size <= RamSize - phys)
                m_ramUndo->push_back({phys, std::vector<uint8_t>(m_ram.begin() + phys, m_ram.begin() + phys + size),
                                      std::vector<uint8_t>(m_owned.begin() + phys, m_owned.begin() + phys + size)});
        }
        [[nodiscard]] uint32_t replayRead(uint32_t address, uint32_t size) const noexcept;
        void replayWrite(uint32_t address, uint32_t size, uint32_t value) noexcept;
        std::vector<uint8_t> m_scratch;
        std::unordered_map<uint32_t, uint32_t> m_hardware;
        std::vector<Allocation> m_allocations;
        uint32_t m_heapCursor = HeapBase;
        uint32_t m_interruptStatus = 0;
        uint32_t m_interruptMask = 0;
        uint32_t m_interruptControl = 1;
        std::optional<DmaStart> m_dmaStart;
        IopSpu2 m_spu2;
    };
}
