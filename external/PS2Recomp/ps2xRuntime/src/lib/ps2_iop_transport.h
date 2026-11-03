#pragma once

#include "ps2_runtime.h"
#include "ps2x/iop/iop_subsystem.h"

#include <utility>

class PS2IopTransport
{
public:
    [[nodiscard]] static ps2x::iop::RpcAbi selectRpcAbi(
        const PS2Runtime *runtime,
        const ps2x::iop::RpcAbiRequest &request)
    {
        return runtime
                   ? runtime->selectIopRpcAbi(request)
                   : ps2x::iop::RpcAbi::RuntimeDefault;
    }

    [[nodiscard]] static ps2x::iop::RpcResult handleRpc(
        PS2Runtime *runtime,
        uint8_t *rdram,
        R5900Context *context,
        ps2x::iop::RpcRequest request)
    {
        return runtime
                   ? runtime->handleIopRpc(rdram, context, std::move(request))
                   : ps2x::iop::RpcResult{};
    }

    // Issue 009 v4.4 (design 1a): 0 = run it with handleRpc.
    [[nodiscard]] static uint64_t startRpc(
        PS2Runtime *runtime,
        uint8_t *rdram,
        R5900Context *context,
        ps2x::iop::RpcRequest request)
    {
        return runtime ? runtime->startIopRpc(rdram, context, std::move(request)) : 0u;
    }

    [[nodiscard]] static bool canBindRpc(const PS2Runtime *runtime, uint32_t sid)
    {
        return !runtime || runtime->canBindIopRpc(sid);
    }

    [[nodiscard]] static bool hasEmulatedRpcServer(const PS2Runtime *runtime, uint32_t sid) noexcept
    {
        return runtime && runtime->hasEmulatedIopRpcServer(sid);
    }

    static void notifyTransfer(
        PS2Runtime *runtime,
        uint8_t *rdram,
        const ps2x::iop::SifTransfer &transfer)
    {
        if (runtime)
        {
            runtime->notifyIopSifTransfer(rdram, transfer);
        }
    }

    static void reset(PS2Runtime *runtime)
    {
        if (runtime)
        {
            runtime->resetIop();
        }
    }
};
