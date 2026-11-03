#pragma once
#include "ps2x/state_archive.h"

#include "ps2x/iop/iop_host.h"
#include "ps2x/iop/iop_types.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ps2x::iop
{
    // The host-clocked IOP catches up with the wall clock once per this many EE cycles (~14 us of PS2 time); the EE
    // scheduler uses the same step while an IOP request is in flight (issue 009 v4.4).
    inline constexpr uint64_t kIopHostClockPollEeCycles = 4096u;

    class IopSubsystem
    {
    public:
        // DW5XL (issue 027): the IOP for save states; service routes are rebuilt on load.
        void serializeState(ps2x::StateArchive &archive);
        explicit IopSubsystem(IopHost &host);
        ~IopSubsystem();

        IopSubsystem(const IopSubsystem &) = delete;
        IopSubsystem &operator=(const IopSubsystem &) = delete;
        IopSubsystem(IopSubsystem &&) noexcept;
        IopSubsystem &operator=(IopSubsystem &&) noexcept;

        void reset();

        [[nodiscard]] ModuleLoadResult loadModule(std::string_view path, const void *arguments = nullptr, uint32_t argumentSize = 0);
        [[nodiscard]] ModuleLoadResult loadModuleBuffer(uint32_t guestAddress, const void *arguments = nullptr, uint32_t argumentSize = 0);
        [[nodiscard]] bool stopModule(int32_t moduleId, int32_t *result = nullptr);
        void runEeCycles(uint64_t eeCycles) noexcept;

        [[nodiscard]] RpcAbi selectRpcAbi(const RpcAbiRequest &request) const;
        [[nodiscard]] bool canBindRpc(uint32_t sid) const noexcept;
        [[nodiscard]] bool hasEmulatedRpcServer(uint32_t sid) const noexcept;
        [[nodiscard]] RpcResult handleRpc(const RpcRequest &request);
        // Issue 009 v4.4 (design 1a): the request run by the emulated module's RPC thread in IOP time; its end comes
        // through IopHost::rpcCompleted(token). 0 = not possible (a host-implemented server, or no RPC thread):
        // use handleRpc.
        [[nodiscard]] uint64_t startRpc(const RpcRequest &request);
        [[nodiscard]] bool rpcInFlight() const noexcept;
        // Issue 009 v4.5: the IOP has work now (a request in flight or a runnable IOP thread).
        [[nodiscard]] bool busy() const noexcept;
        void runToHostClock() noexcept;
        void onSifTransfer(const SifTransfer &transfer);

        // Physical IOP RAM access shared by the emulator, SIF DMA, and HLE services. Addresses are IOP addresses.
        [[nodiscard]] uint32_t allocateMemory(uint32_t size, uint32_t alignment = 16u);
        [[nodiscard]] bool freeMemory(uint32_t address);
        [[nodiscard]] bool readMemory(uint32_t address, void *destination, size_t size) const;
        [[nodiscard]] bool writeMemory(uint32_t address, const void *source, size_t size);
        [[nodiscard]] bool zeroMemory(uint32_t address, size_t size);
        [[nodiscard]] bool isMemoryRange(uint32_t address, size_t size) const;

        [[nodiscard]] DebugSnapshot debugSnapshot() const;

    private:
        class Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
