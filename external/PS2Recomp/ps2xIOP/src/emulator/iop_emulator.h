#pragma once
#include "ps2x/state_archive.h"

#include "ps2x/iop/iop_host.h"
#include "ps2x/iop/iop_types.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ps2x::iop::detail
{
    class IopEmulator
    {
    public:
        // DW5XL (issue 027): the whole IOP (memory, SPU2, kernel, CD, timers, files, modules) for save states.
        void serializeState(ps2x::StateArchive &archive);
        explicit IopEmulator(IopHost &host);
        ~IopEmulator();

        IopEmulator(const IopEmulator &) = delete;
        IopEmulator &operator=(const IopEmulator &) = delete;

        void reset();
        [[nodiscard]] ModuleLoadResult loadModule(std::string_view path, const void *arguments, uint32_t argumentSize);
        [[nodiscard]] ModuleLoadResult loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize);
        [[nodiscard]] bool stopModule(int32_t moduleId, int32_t *result);
        void runEeCycles(uint64_t eeCycles) noexcept;
        [[nodiscard]] RpcResult handleRpc(const RpcRequest &request);
        // Issue 009 v4.4 (design 1a): the request run by the module's RPC thread in IOP time (0 = not possible).
        [[nodiscard]] uint64_t startRpc(const RpcRequest &request);
        [[nodiscard]] bool rpcInFlight() const noexcept;
        // Issue 009 v4.5: the IOP has work now (a request in flight or a runnable thread).
        [[nodiscard]] bool busy() const noexcept;
        // Runs the host-clocked IOP up to the wall clock now (issue 009 v4.4: in steps while the EE waits).
        void runToHostClock() noexcept;
        [[nodiscard]] bool hasRpcServer(uint32_t sid) const noexcept;
        void onSifTransfer(const SifTransfer &transfer);

        [[nodiscard]] uint32_t allocateMemory(uint32_t size, uint32_t alignment = 16u);
        [[nodiscard]] bool freeMemory(uint32_t address);
        [[nodiscard]] bool readMemory(uint32_t address, void *destination, size_t size) const;
        [[nodiscard]] bool writeMemory(uint32_t address, const void *source, size_t size);
        [[nodiscard]] bool zeroMemory(uint32_t address, size_t size);
        [[nodiscard]] bool isMemoryRange(uint32_t address, size_t size) const;

        [[nodiscard]] uint64_t cycles() const noexcept;
        [[nodiscard]] uint64_t instructions() const noexcept;
        [[nodiscard]] uint32_t loadedModuleCount() const noexcept;
        [[nodiscard]] uint32_t threadCount() const noexcept;
        [[nodiscard]] uint32_t rpcServerCount() const noexcept;

    private:
        class Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
