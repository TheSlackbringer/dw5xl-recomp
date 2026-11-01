#pragma once
#include "ps2x/state_archive.h"

#include "ps2x/iop/iop_types.h"
#include "../core/iop_cpu.h" // issue 009 v4.4: a saved RPC thread state

#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>

namespace ps2x::iop
{
    class IopHost;
}

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopKernel;
    class IopMemory;

    class IopGuestExecutor
    {
    public:
        virtual ~IopGuestExecutor() = default;

        [[nodiscard]] virtual uint32_t executeGuestFunction(uint32_t address,
                                                            uint32_t a0,
                                                            uint32_t a1,
                                                            uint32_t a2,
                                                            uint32_t a3,
                                                            uint32_t gp) = 0;
        [[nodiscard]] virtual uint32_t executeGuestFunctionWithBudget(uint32_t address,
                                                                      uint32_t a0,
                                                                      uint32_t a1,
                                                                      uint32_t a2,
                                                                      uint32_t a3,
                                                                      uint32_t gp,
                                                                      uint32_t instructionBudget)
        {
            return executeGuestFunction(address, a0, a1, a2, a3, gp);
        }
    };

    class IopRpcBridge
    {
    public:
        // DW5XL (issue 027): save-state support.
        void serializeState(ps2x::StateArchive &archive);
        IopRpcBridge(IopHost &host, IopMemory &memory, IopKernel &kernel) noexcept;

        void reset();
        [[nodiscard]] bool dispatchSifManImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] bool dispatchSifCmdImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] RpcResult handleRpc(const RpcRequest &request, IopGuestExecutor &executor);
        void onSifTransfer(const SifTransfer &transfer);
        void removeServersInRange(uint32_t base, uint32_t size);

        [[nodiscard]] bool hasServer(uint32_t sid) const noexcept;
        [[nodiscard]] size_t serverCount() const noexcept { return m_servers.size(); }

        // Issue 009 v4.4 (design 1a): an EE request run by the module's own RPC thread, in IOP time, instead of at once
        // on the EE host thread (handleRpc). The send data goes into the server buffer at once (as the SIF transfer
        // does); the request waits for the thread that runs that server's sceSifRpcLoop, which runs the server
        // function at its own priority on its own stack. Returns a token, or 0 when that is not possible (no such
        // server, or no thread loops on its queue: the caller runs it with handleRpc). The end is reported through
        // IopHost::rpcCompleted(token) after the reply has been copied to the EE receive buffer.
        [[nodiscard]] uint64_t startRpc(const RpcRequest &request);
        // The IOP thread `threadId` reached kRpcServerReturnSentinel: finishes its request, puts it back to sleep in
        // its loop and starts its next queued request.
        void onServerReturned(int threadId);
        [[nodiscard]] bool rpcInFlight() const noexcept { return m_inFlight != 0u; }

    private:
        struct QueuedRpc
        {
            uint64_t token = 0;
            uint32_t sid = 0;
            uint32_t function = 0;
            uint32_t sendSize = 0;
            uint32_t receiveAddress = 0;
            uint32_t receiveSize = 0;
        };
        struct LoopThread
        {
            std::deque<QueuedRpc> queue;
            bool active = false;
            QueuedRpc current{};
            IopCpuState saved{}; // the thread asleep in sceSifRpcLoop, restored after each request
        };
        void copySendData(const RpcRequest &request, uint32_t serverBuffer);
        void copyReply(uint32_t returnPointer, uint32_t serverBuffer, uint32_t receiveAddress, uint32_t receiveSize);
        void startNext(int threadId);
        std::unordered_map<uint32_t, int> m_queueThreads; // queue (qd) -> the thread running sceSifRpcLoop on it
        std::unordered_map<int, LoopThread> m_loops;
        uint64_t m_nextToken = 0;
        size_t m_inFlight = 0;

        struct RpcServer
        {
            uint32_t sid = 0;
            uint32_t serverData = 0;
            uint32_t function = 0;
            uint32_t gp = 0;
            uint32_t buffer = 0;
            uint32_t callback = 0;
            uint32_t callbackBuffer = 0;
            uint32_t queue = 0;
        };

        IopHost &m_host;
        IopMemory &m_memory;
        IopKernel &m_kernel;
        std::unordered_map<uint32_t, RpcServer> m_servers;
        uint32_t m_nextDmaId = 1u;
        bool m_sifInitialized = true; // see reset(): SIFMAN is up before game modules load
    };
}
