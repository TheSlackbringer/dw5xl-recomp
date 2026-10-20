// DW5XL (PAL SLES_538.60) libpad2 bridge.
//
// The game uses libpad2 (scePad2*), which talks to DBCMAN.IRX over SIF RPC and
// expects the IOP to DMA a double-buffered controller report into a 0x100-byte
// socket buffer every frame. Nothing in the emulated IOP drives the SIO2 port, so
// the game never sees a controller. The retail libpad2 code stays in place; only
// the libdbc RPC client calls and the side-buffer getters are replaced, and the
// side buffer is filled from the host pad in the layout the IOP would produce.
//
// Socket buffer half (0x80 bytes, newer half picked by the counter at +0x7C):
//   +0x00 u8  state (1 = stable)      +0x02 u8  data bytes   +0x03 u8 profile bytes
//   +0x04 u32 connected               +0x1C     data, then the button profile
// DualShock2 analog report (18 bytes, profile 0xFFFFFFFF): buttons (active low),
// RX RY LX LY, then pressures R L U D Tri Cir Cross Sq L1 R1 L2 R2.
#include "runtime/state_archive.h"
#include "game_overrides.h"
#include "ps2_runtime.h"
#include "runtime/ps2_pad.h"

#include <array>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace
{
    constexpr uint32_t kDbcCreateSocket = 0x0012F678u; // libdbc: RPC 0x80001301
    constexpr uint32_t kDbcLink = 0x0012F868u;         // libdbc: RPC 0x80001303
    constexpr uint32_t kDbcSendData2 = 0x00130060u;    // libdbc: vibration data
    constexpr uint32_t kDbcGetInfo = 0x00130368u;      // libdbc: RPC 0x8000131A
    constexpr uint32_t kPad2GetSideA = 0x00130B58u;
    constexpr uint32_t kPad2GetSideB = 0x00130BC0u;
    constexpr uint32_t kSocketTable = 0x00367688u;     // SLES_538.60: 16 entries of 0x334
    constexpr uint32_t kSocketStride = 0x334u;
    constexpr uint32_t kSocketCount = 16u;
    constexpr uint32_t kConnectedPorts = 1u;           // controller in port 1 only

    constexpr uint32_t kInfoState = 0x0101800Cu;
    constexpr uint32_t kInfoVibProfile = 0x01038002u;

    std::array<int32_t, kSocketCount> g_socketPort{};
    uint32_t g_nextSocket = 0u;
    uint32_t g_counter = 0u;

    bool traceEnabled()
    {
        static const bool enabled = [] {
            const char *v = std::getenv("PS2X_PAD2_TRACE");
            return v && *v && *v != '0';
        }();
        return enabled;
    }

    bool socketConnected(uint32_t socket)
    {
        return socket < kSocketCount && g_socketPort[socket] >= 0 &&
               static_cast<uint32_t>(g_socketPort[socket]) < kConnectedPorts;
    }

    void finish(R5900Context *ctx, int32_t value)
    {
        setReturnS32(ctx, value);
        ctx->pc = getRegU32(ctx, 31);
    }

    void dbcCreateSocket(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        // a0 = request {type|1, 1, port, slot, ...}
        const uint32_t req = getRegU32(ctx, 4);
        const int32_t port = static_cast<int32_t>(*reinterpret_cast<uint32_t *>(getMemPtr(rdram, req + 8u)));
        if (g_nextSocket >= kSocketCount)
        {
            finish(ctx, -1);
            return;
        }
        const uint32_t socket = g_nextSocket++;
        g_socketPort[socket] = port;
        if (traceEnabled())
            std::fprintf(stderr, "[dw5xl:pad2] create socket=%u port=%d buf=0x%08x\n", socket, port, getRegU32(ctx, 5));
        finish(ctx, static_cast<int32_t>(socket));
    }

    void dbcLink(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t socket = getRegU32(ctx, 4);
        finish(ctx, socketConnected(socket) ? 1 : -12);
    }

    void dbcSendData2(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        finish(ctx, 0); // vibration: accepted and dropped
    }

    void dbcGetInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t socket = getRegU32(ctx, 4);
        const uint32_t cmd = getRegU32(ctx, 5);
        auto *size = reinterpret_cast<int32_t *>(getMemPtr(rdram, getRegU32(ctx, 6)));
        uint8_t *out = getMemPtr(rdram, getRegU32(ctx, 7));
        if (!socketConnected(socket))
        {
            finish(ctx, -1);
            return;
        }
        if (cmd == kInfoState)
        {
            *size = 1;
            out[0] = 1;
            finish(ctx, 1);
        }
        else if (cmd == kInfoVibProfile)
        {
            *size = 1;
            out[0] = 0; // no actuators: the game never sends vibration
            finish(ctx, 1);
        }
        else
        {
            *size = 0;
            finish(ctx, 0);
        }
    }

    void fillReport(uint8_t *half, bool connected)
    {
        std::memset(half, 0, 0x7Cu);
        if (!connected)
            return;
        std::array<uint8_t, 32> pad{};
        static PSPadBackend backend;
        backend.readState(0, 0, pad.data(), pad.size());
        const uint16_t pressed = static_cast<uint16_t>(~(pad[2] | (pad[3] << 8)));
        uint8_t *data = half + 0x1Cu;
        data[0] = pad[2];
        data[1] = pad[3];
        std::memcpy(data + 2, pad.data() + 4, 4); // RX RY LX LY
        constexpr uint16_t kPressureOrder[12] = {
            0x0020u, 0x0080u, 0x0010u, 0x0040u, // right left up down
            0x1000u, 0x2000u, 0x4000u, 0x8000u, // triangle circle cross square
            0x0400u, 0x0800u, 0x0100u, 0x0200u, // L1 R1 L2 R2
        };
        for (int i = 0; i < 12; ++i)
            data[6 + i] = (pressed & kPressureOrder[i]) ? 0xFFu : 0x00u;
        std::memset(data + 18, 0xFF, 4); // button profile: all 32 DS2 inputs
        half[0] = 1;                     // stable
        half[2] = 18;
        half[3] = 4;
        const uint32_t one = 1u;
        std::memcpy(half + 4, &one, 4);
    }

    void pad2GetSide(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t socket = getRegU32(ctx, 4);
        const uint32_t buf = *reinterpret_cast<uint32_t *>(getMemPtr(rdram, kSocketTable + socket * kSocketStride + 0x10u));
        const bool connected = socketConnected(socket);
        const uint32_t counter = ++g_counter == 0u ? ++g_counter : g_counter;
        for (uint32_t half = 0; half < 2u; ++half)
        {
            uint8_t *p = getMemPtr(rdram, buf + half * 0x80u);
            fillReport(p, connected);
            std::memcpy(p + 0x7Cu, &counter, 4);
        }
        finish(ctx, static_cast<int32_t>(buf));
    }

    void applyPad2Bridge(PS2Runtime &runtime)
    {
        // DW5XL (issue 027): the libpad2 sockets the game opened survive a save state.
        runtime.registerStateHook("PAD2", [](ps2x::StateArchive &a)
        {
            a.value(g_socketPort);
            a.value(g_nextSocket);
            a.value(g_counter);
        });
        constexpr uint32_t expected[][2] = {
            {kDbcCreateSocket, 0x27BDFF90u}, {kDbcLink, 0x27BDFFB0u},
            {kDbcGetInfo, 0x27BDFF80u},      {kDbcSendData2, 0x27BDFF70u},
            {kPad2GetSideA, 0x24030334u},    {kPad2GetSideB, 0x24030334u},
        };
        for (const auto &item : expected)
            if (runtime.memory().read32(item[0]) != item[1] || !runtime.hasFunction(item[0]))
            {
                std::fprintf(stderr, "[dw5xl:pad2] declined: mismatch at 0x%08x\n", item[0]);
                return;
            }
        g_socketPort.fill(-1);
        runtime.replaceFunction(kDbcCreateSocket, &dbcCreateSocket);
        runtime.replaceFunction(kDbcLink, &dbcLink);
        runtime.replaceFunction(kDbcGetInfo, &dbcGetInfo);
        runtime.replaceFunction(kDbcSendData2, &dbcSendData2);
        runtime.replaceFunction(kPad2GetSideA, &pad2GetSide);
        runtime.replaceFunction(kPad2GetSideB, &pad2GetSide);
        std::fprintf(stderr, "[dw5xl:pad2] installed libpad2 host bridge\n");
    }
}

PS2_REGISTER_GAME_OVERRIDE("DW5XL libpad2 host bridge", "SLES_538.60", 0u, 0u, &applyPad2Bridge)
