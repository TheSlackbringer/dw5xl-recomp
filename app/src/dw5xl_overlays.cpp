// Native dispatch for the separately compiled SLES_538.60 overlays.
// Original loader/finalizer code owns RAM copying, BSS and constructors.
#include "game_overrides.h"
#include "ps2_runtime.h"

extern "C" {
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>

extern const RecompiledModule g_ps2Module_dw5xl_ovl00;

namespace {
constexpr uint32_t kFinalizer = 0x00100260u;
constexpr uint32_t kOverlayBase = 0x0061A800u;
constexpr uint32_t kOvl00Size = 0x0007F880u;
constexpr std::array<uint8_t, 32> kOvl00Sha256 = {
    0x1c,0xb3,0x03,0xe3,0x89,0xc2,0x15,0x41,0x2c,0x3a,0x10,0x4e,0x26,0xb8,0xe4,0x3d,
    0x91,0x05,0xf2,0x30,0xb3,0xb8,0xa7,0x65,0x64,0xed,0x1a,0x0e,0xb9,0x2d,0xa5,0x0f
};
PS2Runtime::RecompiledFunction originalFinalizer = nullptr;

uint32_t read32(const uint8_t* ram, uint32_t address) {
    uint32_t value;
    std::memcpy(&value, ram + address, sizeof(value));
    return value;
}

void fail(PS2Runtime* runtime, const char* reason, uint32_t size) {
    std::fprintf(stderr, "[dw5xl:overlay] refused activation: %s (loaded size=0x%x)\n", reason, size);
    runtime->deactivateCompiledModule();
    runtime->requestStop();
}

void finalizeOverlay(uint8_t* ram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t base = getRegU32(ctx, 4);
    const uint32_t size = getRegU32(ctx, 5);
    if (base != kOverlayBase) {
        originalFinalizer(ram, ctx, runtime);
        return;
    }
    runtime->deactivateCompiledModule();
    if (size != kOvl00Size || uint64_t(base) + size > PS2_RAM_SIZE) {
        fail(runtime, "unsupported module size", size);
        return;
    }
    auto sha = std::unique_ptr<AVSHA, decltype(&av_free)>(av_sha_alloc(), &av_free);
    std::array<uint8_t, 32> digest{};
    if (!sha || av_sha_init(sha.get(), 256) < 0) {
        fail(runtime, "SHA256 initialization failed", size);
        return;
    }
    av_sha_update(sha.get(), ram + base, size);
    av_sha_final(sha.get(), digest.data());
    if (digest != kOvl00Sha256) {
        fail(runtime, "loaded bytes do not match supported OVL00", size);
        return;
    }
    const uint32_t bssSize = read32(ram, base + 0x14);
    const uint32_t ctorBegin = read32(ram, base + 0x18);
    const uint32_t ctorEnd = read32(ram, base + 0x1c);
    if (uint64_t(base) + size + bssSize > PS2_RAM_SIZE || ctorBegin > ctorEnd ||
        (ctorBegin & 3u) || (ctorEnd & 3u) || ctorBegin < base || ctorEnd > base + size) {
        fail(runtime, "invalid BSS or constructor bounds", size);
        return;
    }
    if (!runtime->activateCompiledModule(g_ps2Module_dw5xl_ovl00.identity)) {
        fail(runtime, "compiled OVL00 table unavailable", size);
        return;
    }
    for (uint32_t pointer = ctorBegin; pointer < ctorEnd; pointer += 4) {
        if (!runtime->hasFunction(read32(ram, pointer))) {
            fail(runtime, "constructor has no compiled target", size);
            return;
        }
    }
    std::fprintf(stderr, "[dw5xl:overlay] activated %s before %u constructors; generation=%llu\n",
                 g_ps2Module_dw5xl_ovl00.identity, (ctorEnd - ctorBegin) / 4,
                 static_cast<unsigned long long>(runtime->compiledModuleGeneration()));
    // The original function can yield; its existing resume aliases remain intact.
    originalFinalizer(ram, ctx, runtime);
}

void installOverlays(PS2Runtime& runtime) {
    if (runtime.memory().read32(kFinalizer) != 0x27BDFFB0u || !runtime.hasFunction(kFinalizer)) {
        std::fprintf(stderr, "[dw5xl:overlay] declined: unexpected finalizer\n");
        return;
    }
    if (!runtime.registerCompiledModule(g_ps2Module_dw5xl_ovl00)) {
        std::fprintf(stderr, "[dw5xl:overlay] declined: invalid compiled module descriptor\n");
        return;
    }
    auto fn = runtime.lookupFunction(kFinalizer);
    if (fn != &finalizeOverlay) originalFinalizer = fn;
    if (!originalFinalizer || !runtime.replaceFunction(kFinalizer, &finalizeOverlay)) return;
    std::fprintf(stderr, "[dw5xl:overlay] installed verified OVL00 finalizer hook\n");
}
}
PS2_REGISTER_GAME_OVERRIDE("DW5XL native overlays", "SLES_538.60", 0u, 0u, &installOverlays)
