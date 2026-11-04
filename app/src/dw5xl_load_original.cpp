// Installed-player convenience: keep the game's translated reader/setter and manual overlay flow.
#include "dw5xl_load_original.h"
#include "load_original.h"
#include "game_overrides.h"
#include "ps2_runtime_macros.h"

#include <cstdio>
#include <future>

namespace {
using namespace dw5xl::setup;
constexpr uint32_t kSetter = 0x001AC6C0u, kReader = 0x001AC6D0u;
constexpr uint32_t kMenuReturn = 0x00260688u;
std::filesystem::path dataDir;
std::future<OriginalReadiness> check;
OriginalReadiness readiness;
PS2Runtime::RecompiledFunction originalReader = nullptr, originalSetter = nullptr;
bool firstMenu = true;

void reader(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    const bool menu = getRegU32(ctx, 31) == kMenuReturn;
    // Every menu read: loading a save state can put the flag back to 0.
    if (menu && readiness.allowed()) {
        // The original setter is immediate (JR RA; SW A0,-0x7974(GP)).
        // Run it with a private register context; the real caller sees only its ordinary reader result.
        auto setterContext = *ctx;
        SET_GPR_U64(&setterContext, 4, 1);
        setterContext.pc = kSetter;
        originalSetter(ram, &setterContext, runtime);
    }
    if (firstMenu && menu) {
        firstMenu = false;
        const char *installed = readiness.installed == InstallState::Installed ? "installed" :
                                readiness.installed == InstallState::Incomplete ? "incomplete" : "missing";
        const char *checked = readiness.check == OriginalCheck::valid ? "valid" :
                              readiness.check == OriginalCheck::unsupported ? "unsupported" : "skipped";
        std::fprintf(stderr, "[dw5xl:disc] load original on loading: dw5=%s check=%s unlock=%d check_ms=%.3f\n",
                     installed, checked, readiness.allowed(), readiness.milliseconds);
    }
    originalReader(ram, ctx, runtime);
}
void install(PS2Runtime &runtime)
{
    if (dataDir.empty()) return;
    // Complete the background disk check before runtime.run starts any game fields.
    // The menu reader never waits for host I/O, even on a slow disk.
    try { if (check.valid()) readiness = check.get(); }
    catch (const std::exception &) { readiness = {}; }
    if (runtime.memory().read32(kSetter) != 0x03E00008u ||
        runtime.memory().read32(kSetter + 4) != 0xAF84868Cu ||
        runtime.memory().read32(kReader) != 0x03E00008u ||
        runtime.memory().read32(kReader + 4) != 0x8F82868Cu ||
        !runtime.hasFunction(kSetter) || !runtime.hasFunction(kReader)) {
        std::fprintf(stderr, "[dw5xl:disc] automatic Load Original declined: unexpected reader/setter\n");
        return;
    }
    const auto currentReader = runtime.lookupFunction(kReader);
    if (currentReader != &reader) originalReader = currentReader;
    originalSetter = runtime.lookupFunction(kSetter);
    runtime.replaceFunction(kReader, &reader);
}
} // namespace

void dw5xlStartOriginalCheck(const std::filesystem::path &data)
{
    dataDir = data;
    firstMenu = true;
    try { check = std::async(std::launch::async, [data] { return checkOriginal(data); }); }
    catch (const std::exception &) {
        std::fprintf(stderr, "[dw5xl:disc] could not start the DW5 check; manual Load Original remains available\n");
    }
}

PS2_REGISTER_GAME_OVERRIDE("DW5XL automatic Load Original", "SLES_538.60", 0u, 0u, &install)
