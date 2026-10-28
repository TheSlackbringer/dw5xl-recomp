// DW5XL's linked SDK sets the current global context on allocation failure,
// including calls whose _r argument names a different context. Keep that game
// contract separate from generic runtime clients and their heap ownership.
#include "game_overrides.h"
#include "ps2_runtime.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <limits>

namespace
{
constexpr uint32_t kErrnoAccessor = 0x001066F0u;
constexpr uint32_t kCurrentContextPointer = 0x0030F000u;
constexpr uint32_t kOutOfMemory = 12u;
constexpr std::array<uint32_t, 4> kEntries{0x00105148u, 0x00107808u, 0x00107B58u, 0x00107E50u};

uint32_t readWord(const uint8_t *ram, uint32_t address)
{
    uint32_t value;
    std::memcpy(&value, ram + address, sizeof(value));
    return value;
}

uint32_t allocationSize(uint32_t size)
{
    // This SDK adds 19 in 32 bits before its minimum-chunk check. Zero
    // and additions that wrap select the minimum allocation. Other sizes
    // keep the existing runtime request; block metadata stays runtime-owned.
    return size == 0u || size > std::numeric_limits<uint32_t>::max() - 19u ? 16u : size;
}

void finishAllocation(uint8_t *ram, R5900Context *ctx, uint32_t result)
{
    if (result == 0u)
    {
        const uint32_t address = readWord(ram, kCurrentContextPointer);
        const bool mirror = Ps2IsUncachedRamMirrorAddress(address);
        const bool direct = address < PS2_RAM_SIZE || mirror || Ps2IsKseg01Address(address);
        const uint32_t physical =
            mirror ? address & PS2_RAM_MASK : Ps2DirectMappedPhysicalAddress(address);
        // Accept RAM and its KSEG aliases, never wrap an invalid context
        // into low RAM or write through a null/unaligned pointer.
        if (direct && address != 0u && (address & 3u) == 0u &&
            physical <= PS2_RAM_SIZE - sizeof(uint32_t))
            std::memcpy(ram + physical, &kOutOfMemory, sizeof(kOutOfMemory));
    }
    setReturnU32(ctx, result);
    ctx->pc = getRegU32(ctx, 31);
}

void mallocNative(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    finishAllocation(ram, ctx, runtime->guestMalloc(allocationSize(getRegU32(ctx, 4))));
}

void mallocReentrant(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    finishAllocation(ram, ctx, runtime->guestMalloc(allocationSize(getRegU32(ctx, 5))));
}

void callocReentrant(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    // The linked SDK uses the low 32-bit product, then calls its allocator.
    // Preserve that guest rule without weakening generic guestCalloc's
    // overflow checks for other games or native runtime clients.
    const uint32_t size = getRegU32(ctx, 5) * getRegU32(ctx, 6);
    finishAllocation(ram, ctx, runtime->guestCalloc(1u, allocationSize(size)));
}

void memalignReentrant(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    finishAllocation(ram, ctx,
                     runtime->guestMalloc(allocationSize(getRegU32(ctx, 6)), getRegU32(ctx, 5)));
}

void applyAllocationContract(PS2Runtime &runtime)
{
    // Verify the original accessor that establishes this context location.
    // Reject the whole adapter if any required entry is missing.
    if (!runtime.hasFunction(kErrnoAccessor) ||
        runtime.memory().read32(kErrnoAccessor) != 0x3C030031u ||
        runtime.memory().read32(kErrnoAccessor + 4u) != 0x03E00008u ||
        runtime.memory().read32(kErrnoAccessor + 8u) != 0x8C62F000u)
        return;
    for (uint32_t entry : kEntries)
        if (!runtime.hasFunction(entry))
            return;

    runtime.replaceFunction(kEntries[0], &callocReentrant);
    runtime.replaceFunction(kEntries[1], &memalignReentrant);
    runtime.replaceFunction(kEntries[2], &mallocNative);
    runtime.replaceFunction(kEntries[3], &mallocReentrant);
    std::fprintf(stderr, "[dw5xl:alloc] installed SDK allocation error/request adapter\n");
}
} // namespace

PS2_REGISTER_GAME_OVERRIDE("DW5XL allocator error state", "SLES_538.60", 0u, 0u,
                           &applyAllocationContract)
