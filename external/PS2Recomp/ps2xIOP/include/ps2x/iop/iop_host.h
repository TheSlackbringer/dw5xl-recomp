#pragma once

#include "ps2x/iop/iop_types.h"
#include "ps2x/iop/spu2_sound.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ps2x::iop
{
    enum class HostPathKind : uint32_t
    {
        ElfDirectory = 0,
        CdRoot = 1,
        CdImage = 2,
        HostRoot = 3,
        MemoryCardRoot = 4,
    };

    enum class LogLevel : uint32_t
    {
        Debug = 0,
        Info = 1,
        Warning = 2,
        Error = 3,
    };

    enum class MemoryCardOperation : uint32_t
    {
        Init,
        GetInfo,
        Open,
        Close,
        Seek,
        Read,
        Write,
        Flush,
        Chdir,
        GetDir,
        SetFileInfo,
        Delete,
        Format,
        Unformat,
        Mkdir,
    };

    struct MemoryCardRequest
    {
        MemoryCardOperation operation = MemoryCardOperation::Init;
        // The fifth argument is carried in $t0 by the EE n32 ABI
        std::array<uint32_t, 5> arguments{};
    };

    class IopHost
    {
    public:
        virtual ~IopHost() = default;

        virtual bool readGuest(uint32_t address, void *destination, size_t size) const = 0;
        virtual bool writeGuest(uint32_t address, const void *source, size_t size) = 0;
        virtual bool zeroGuest(uint32_t address, size_t size) = 0;
        virtual bool normalizeGuestAddress(uint32_t address, uint32_t &normalized) const = 0;

        // IOP RAM is a distinct address space from the EE guest. TODO remove this later
        virtual bool readIopMemory(uint32_t address, void *destination, size_t size) const
        {
            (void)address;
            (void)destination;
            (void)size;
            return false;
        }
        virtual bool writeIopMemory(uint32_t address, const void *source, size_t size)
        {
            (void)address;
            (void)source;
            (void)size;
            return false;
        }
        virtual bool zeroIopMemory(uint32_t address, size_t size)
        {
            (void)address;
            (void)size;
            return false;
        }
        virtual bool normalizeIopAddress(uint32_t address, uint32_t &normalized) const
        {
            (void)address;
            normalized = 0u;
            return false;
        }
        virtual uint32_t allocateIopHandle(IopHandleKind kind) = 0;
        virtual uint32_t allocateGuest(uint32_t size, uint32_t alignment) = 0;
        virtual void freeGuest(uint32_t address) = 0;

        virtual void audioCommand(uint32_t sid, uint32_t function, GuestBuffer send, GuestBuffer receive) = 0;
        // Optional native SPU2 mixer output. Existing test and service hosts may
        // omit playback while the IOP still runs its sound hardware normally.
        virtual void audioSamples(std::span<const int16_t> stereo48k)
        {
            (void)stereo48k;
        }

        // Native hosts may clock the IOP independently of slow EE rendering.
        // nullopt retains deterministic EE/8 cycle accounting for other hosts.
        // DW5XL (issue 023 3S): SPU2 key-ons with their samples, while the host wants them (spu2_sound.h).
        virtual bool wantsSpu2KeyOns() const { return false; }
        virtual void spu2KeyOn(const Spu2KeyOn &) {}

        virtual std::optional<uint64_t> monotonicNanoseconds() const noexcept
        {
            return std::nullopt;
        }

        // EE SearchFile can return mapped sectors for extracted game files.
        // nullopt means unmapped; false means mapped but failed (no fallback).
        virtual std::optional<bool> readMappedCdSectors(uint32_t lsn, uint32_t sectors,
                                                       void *destination, size_t size)
        {
            (void)lsn;
            (void)sectors;
            (void)destination;
            (void)size;
            return std::nullopt;
        }

        virtual std::string hostPath(HostPathKind kind) const = 0;
        virtual std::string translateGuestPath(std::string_view path) const = 0;
        virtual uint64_t openHostFile(std::string_view path) = 0;
        virtual bool hostFileSize(uint64_t handle, uint64_t &size) const = 0;
        virtual bool readHostFile(uint64_t handle,
                                  uint64_t offset,
                                  void *destination,
                                  size_t size,
                                  size_t &bytesRead) = 0;
        virtual void closeHostFile(uint64_t handle) = 0;

        virtual int32_t memoryCard(const MemoryCardRequest &request) = 0;

        virtual bool hasGuestFunction(uint32_t address) const = 0;
        virtual bool invokeGuestFunction(uint64_t callToken,
                                         uint32_t address,
                                         uint32_t a0,
                                         uint32_t a1,
                                         uint32_t a2,
                                         uint32_t a3,
                                         uint32_t *resultAddress) = 0;

        // Deliver an IOP -> EE SIF command packet. The default keeps hosts
        // which do not emulate the EE command dispatcher source-compatible.
        virtual bool sendSifCommand(uint32_t commandId,
                                    const void *packet,
                                    size_t packetSize)
        {
            (void)commandId;
            (void)packet;
            (void)packetSize;
            return false;
        }

        virtual void log(LogLevel level, std::string_view message) = 0;
    };
}
