#include "runtime/kfiv_runtime.h"

#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "ps2_stubs.h"
#include "ps2_iop_transport.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <stdexcept>

namespace
{
    constexpr uint32_t kFreeIopHeap = 0x00228690u;
    constexpr uint32_t kRemoteInit = 0x00237E08u;
    constexpr uint32_t kRemote = 0x00237F48u;
    constexpr uint32_t kSifInit = 0x002240C0u;
    constexpr uint32_t kSifBind = 0x00224730u;
    constexpr uint32_t kSifCall = 0x00224900u;
    constexpr uint32_t kRemoteSid = 0x80000701u;
    constexpr uint32_t kRemotePacket = 0x01E55480u;
    constexpr uint32_t kRemoteClient = 0x01E554C0u;
    constexpr uint32_t kRemoteCallback = 0x0043AA20u;
    PS2Runtime *gRemoteBoundRuntime = nullptr;
    uint64_t gRemoteCalls = 0;

    bool audioTraceEnabled()
    {
        static const bool enabled = []
        {
            const char *value = std::getenv("PS2X_AUDIO_TRACE");
            return value && value[0] != '\0' && std::strcmp(value, "0") != 0;
        }();
        return enabled;
    }

    // Native SDK calls still use the existing SIF ABI, client registry and
    // physical IRX dispatcher. A separate guest allocation holds the ninth
    // SIF argument; the caller's live frame and incoming zero arguments survive.
    struct SdkCall
    {
        PS2Runtime &runtime;
        R5900Context context;
        uint32_t stack;
        SdkCall(PS2Runtime &owner, const R5900Context &caller)
            : runtime(owner), context(caller), stack(owner.guestMalloc(64u, 16u))
        {
            if (!stack)
                throw std::runtime_error("remote SDK guest stack allocation failed");
            try
            {
                SET_GPR_U32(&context, 29, stack);
                for (uint32_t offset = 0; offset < 64u; offset += 4u)
                    runtime.memory().write32(stack + offset, 0u);
            }
            catch (...)
            {
                runtime.guestFree(stack);
                throw;
            }
        }
        ~SdkCall() { runtime.guestFree(stack); }
        SdkCall(const SdkCall &) = delete;
        SdkCall &operator=(const SdkCall &) = delete;
        bool invoke(uint32_t address, uint8_t *rdram)
        {
            const auto function = runtime.lookupFunction(address);
            if (!function) return false;
            context.pc = address;
            function(rdram, &context, &runtime);
            return static_cast<int32_t>(getRegU32(&context, 2)) >= 0;
        }
    };

    bool bindRemote(uint8_t *rdram, const R5900Context &caller, PS2Runtime &runtime)
    {
        gRemoteBoundRuntime = nullptr;
        SdkCall call(runtime, caller);
        SET_GPR_U32(&call.context, 4, 0u); // Retail sceSifInitRpc(0).
        if (!call.invoke(kSifInit, rdram)) return false;
        // The generic libsd HLE route guesses sample addresses. It must not
        // substitute for SDRDRV's actual packet interpreter and SPU register IO.
        for (unsigned retry = 0; retry != 1024; ++retry)
        {
            if (PS2IopTransport::hasEmulatedRpcServer(&runtime, kRemoteSid))
            {
                SET_GPR_U32(&call.context, 4, kRemoteClient);
                SET_GPR_U32(&call.context, 5, kRemoteSid);
                SET_GPR_U32(&call.context, 6, 0u);
                if (!call.invoke(kSifBind, rdram)) return false;
                if (runtime.memory().read32(kRemoteClient + 0x24u))
                {
                    gRemoteBoundRuntime = &runtime;
                    if (audioTraceEnabled())
                        std::fprintf(stderr, "[kfiv-remote] bound physical SDRDRV sid=%08X client=%08X\n",
                                     kRemoteSid, kRemoteClient);
                    return true;
                }
            }
            // Equivalent to the retail client's 10,000-cycle retry delay;
            // advance IOP server startup without sleeping the host UI thread.
            runtime.eeScheduler().accountCycles(10000u);
        }
        std::fprintf(stderr, "[kfiv-remote] SDRDRV did not register sid=%08X; remote init failed\n", kRemoteSid);
        return false;
    }

    void remoteInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ctx->pc = getRegU32(ctx, 31);
        try
        {
            // The retail USA function returns one after a successful bind.
            setReturnS32(ctx, runtime && bindRemote(rdram, *ctx, *runtime) ? 1 : -1);
        }
        catch (const std::exception &error)
        {
            std::fprintf(stderr, "[kfiv-remote] init failed: %s\n", error.what());
            setReturnS32(ctx, -1);
        }
    }

    void remote(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t blocking = getRegU32(ctx, 4);
        const uint32_t command = getRegU32(ctx, 5);
        const uint32_t arguments[6] = {
            getRegU32(ctx, 6), getRegU32(ctx, 7), getRegU32(ctx, 8),
            getRegU32(ctx, 9), getRegU32(ctx, 10), getRegU32(ctx, 11),
        };
        ctx->pc = getRegU32(ctx, 31);
        try
        {
            if (!runtime || !PS2IopTransport::hasEmulatedRpcServer(runtime, kRemoteSid) ||
                (gRemoteBoundRuntime != runtime && !bindRemote(rdram, *ctx, *runtime)))
            {
                setReturnS32(ctx, -1);
                return;
            }
            // KFIV movie calls are blocking. Callback registration commands
            // need the EE callback server, which this game never requests here.
            const uint32_t callback = blocking ? 0u : runtime->memory().read32(kRemoteCallback);
            if (callback || command == 0x8110u || command == 0x8120u ||
                command == 0x8160u || command == 0x8170u)
            {
                std::fprintf(stderr, "[kfiv-remote] unsupported callback command=%04X\n", command);
                setReturnS32(ctx, -1);
                return;
            }
            runtime->memory().write32(kRemotePacket, kRemotePacket);
            for (unsigned i = 0; i != 6; ++i)
                runtime->memory().write32(kRemotePacket + 4u + i * 4u, arguments[i]);
            SdkCall call(*runtime, *ctx);
            uint32_t rpcCommand = command;
            uint32_t send = kRemotePacket, receive = kRemotePacket, receiveSize = 16u;
            uint32_t endParameter = kRemotePacket;
            if (command == 0x8130u || command == 0x8140u)
            {
                rpcCommand |= arguments[0];
                if (command == 0x8130u) { send = arguments[1]; receive = 0u; receiveSize = 0u; }
                else { receive = arguments[1]; receiveSize = 64u; endParameter = arguments[1]; }
            }
            SET_GPR_U32(&call.context, 4, kRemoteClient);
            SET_GPR_U32(&call.context, 5, rpcCommand);
            SET_GPR_U32(&call.context, 6, blocking ? 0u : 1u);
            SET_GPR_U32(&call.context, 7, send);
            SET_GPR_U32(&call.context, 8, 64u);
            SET_GPR_U32(&call.context, 9, receive);
            SET_GPR_U32(&call.context, 10, receiveSize);
            SET_GPR_U32(&call.context, 11, 0u);
            runtime->memory().write32(call.stack, endParameter);
            if (!call.invoke(kSifCall, rdram))
            {
                setReturnS32(ctx, -1);
                return;
            }
            // Retail 8140's separate return branch preserves incoming S1;
            // ordinary commands return the first response word instead.
            const uint32_t result = command == 0x8140u ? getRegU32(ctx, 17) : runtime->memory().read32(kRemotePacket);
            setReturnS32(ctx, static_cast<int32_t>(result));
            const uint64_t count = ++gRemoteCalls;
            if (audioTraceEnabled() && (count <= 32 || (count & (count - 1)) == 0))
                std::fprintf(stderr, "[kfiv-remote] cmd=%04X blocking=%u args=%08X,%08X,%08X,%08X,%08X,%08X result=%08X count=%llu\n",
                             command, blocking, arguments[0], arguments[1], arguments[2], arguments[3], arguments[4], arguments[5],
                             result, static_cast<unsigned long long>(count));
        }
        catch (const std::exception &error)
        {
            std::fprintf(stderr, "[kfiv-remote] command=%04X failed: %s\n", command, error.what());
            setReturnS32(ctx, -1);
        }
    }

    void applyKfivRemote(PS2Runtime &runtime)
    {
        constexpr uint32_t expected[][2] = {
            {kRemoteInit, 0x27BDFFC0u}, {0x237E20u, 0x261254C0u},
            {0x237E30u, 0x3C058000u}, {0x237E3Cu, 0x0C0891CCu},
            {0x237E94u, 0x8E420024u}, {kRemote, 0x27BDFF30u},
            {0x237F58u, 0xFFA50098u}, {0x237F60u, 0xFFA600A0u},
            {0x237F90u, 0xAE025480u}, {0x237FA4u, 0xACC20000u},
            {0x23811Cu, 0x24080040u}, {0x238124u, 0x240A0010u},
            {0x238128u, 0x0C089240u}, {0x238130u, 0x8E115480u},
            {kSifInit, 0x27BDFFC0u}, {kSifBind, 0x27BDFF90u}, {kSifCall, 0x27BDFF40u},
        };
        try
        {
            for (const auto &item : expected)
                if (runtime.memory().read32(item[0]) != item[1])
                {
                    if (audioTraceEnabled())
                        std::fprintf(stderr, "[kfiv-remote] declined binding: opcode mismatch=%08X\n", item[0]);
                    return;
                }
        }
        catch (const std::exception &) { return; }
        const auto originalInit = runtime.lookupFunction(kRemoteInit);
        const auto originalRemote = runtime.lookupFunction(kRemote);
        if (!originalInit || !originalRemote || !runtime.hasFunction(kSifInit) ||
            !runtime.hasFunction(kSifBind) || !runtime.hasFunction(kSifCall)) return;
        if (!runtime.replaceFunction(kRemoteInit, &remoteInit)) return;
        if (!runtime.replaceFunction(kRemote, &remote))
        {
            (void)runtime.replaceFunction(kRemoteInit, originalInit);
            return;
        }
        gRemoteBoundRuntime = nullptr;
        gRemoteCalls = 0;
        if (audioTraceEnabled())
            std::fprintf(stderr, "[kfiv-remote] installed guarded USA SDRDRV SDK bridge\n");
    }

    bool heapTraceEnabled()
    {
        static const bool enabled = []
        {
            const char *value = std::getenv("PS2X_IOP_HEAP_TRACE");
            return value && value[0] != '\0' && std::strcmp(value, "0") != 0;
        }();
        return enabled;
    }

    void freeIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // Match generated SDK stubs: finish this guest call at its saved return
        // address, without executing the retail RPC client or changing its data.
        ctx->pc = getRegU32(ctx, 31);
        ps2_stubs::sceSifFreeIopHeap(rdram, ctx, runtime);
    }

    void applyKfivRuntime(PS2Runtime &runtime)
    {
        struct ExpectedInstruction
        {
            uint32_t address;
            uint32_t instruction;
        };
        constexpr ExpectedInstruction expected[] = {
            {kFreeIopHeap, 0x3C030044u},
            {0x00228698u, 0x8C6282FCu},
            {0x0022869Cu, 0x0080282Du},
            {0x002286A4u, 0x04410003u},
            {0x002286B0u, 0x0000102Du},
            {0x002286D0u, 0x24050002u},
            {0x002286E4u, 0x0C089240u},
            {0x00228700u, 0x03E00008u},
        };
        try
        {
            for (const ExpectedInstruction &item : expected)
            {
                if (runtime.memory().read32(item.address) != item.instruction)
                {
                    std::fprintf(stderr,
                                 "[kfiv-runtime] declined IOP heap free binding: "
                                 "instruction mismatch at 0x%08x\n", item.address);
                    return;
                }
            }
        }
        catch (const std::exception &)
        {
            std::fprintf(stderr,
                         "[kfiv-runtime] declined IOP heap free binding: game RAM unavailable\n");
            return;
        }
        if (!runtime.hasFunction(kFreeIopHeap))
        {
            std::fprintf(stderr,
                         "[kfiv-runtime] declined IOP heap free binding: compiled function missing\n");
            return;
        }
        // Only the SDK entry is replaced; retail continuations remain intact.
        if (runtime.replaceFunction(kFreeIopHeap, &freeIopHeap) && heapTraceEnabled())
        {
            std::fprintf(stderr,
                         "[kfiv-runtime] installed USA sceSifFreeIopHeap=0x%08x "
                         "crc=0xbda82d37\n", kFreeIopHeap);
        }
    }
}

namespace ps2_kfiv_runtime
{
    void registerOverrides()
    {
        static std::once_flag registered;
        std::call_once(registered, []
        {
            ps2_game_overrides::registerDescriptor({
                "KFIV native IOP heap SDK binding", "SLUS_203.18",
                0x00100008u, 0xBDA82D37u, &applyKfivRuntime});
            ps2_game_overrides::registerDescriptor({
                "KFIV native SDRDRV remote SDK bridge", "SLUS_203.18",
                0x00100008u, 0xBDA82D37u, &applyKfivRemote});
        });
    }
}
