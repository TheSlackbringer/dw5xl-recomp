#include "runtime/kfiv_input.h"

#include "game_overrides.h"
#include "ps2_runtime.h"
#include "runtime/host_input.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>

namespace
{
    constexpr uint32_t kStateUpdate = 0x001B5A48u;
    constexpr uint32_t kYawNormalizer = 0x0019B838u;
    constexpr uint32_t kPause = 0x00101F90u;
    constexpr uint32_t kInventory = 0x001D62C8u;
    constexpr uint32_t kMenuPrelude = 0x00101CE0u;
    constexpr uint32_t kMenuUpdate = 0x00206C40u;
    constexpr uint32_t kPlayerYawReturn = 0x001B8868u;
    constexpr uint32_t kPlayer = 0x00413F80u;
    constexpr uint32_t kPitch = kPlayer + 0x10u;
    constexpr uint32_t kYaw = kPlayer + 0x14u;
    constexpr uint32_t kPlayerFlags = kPlayer + 0x1C8u;
    constexpr uint64_t kPitchLocked = uint64_t{1} << 32;
    constexpr uint32_t kGameplayByte = 0x0038CC50u;
    constexpr uint32_t kPauseByte = 0x0038CC52u;
    constexpr uint32_t kGameplayFlags = 0x0038CC80u;
    constexpr uint32_t kPadHeld = 0x00413DA8u;
    constexpr uint32_t kPadEdge = 0x00413DAAu;
    constexpr uint32_t kStartConfig = 0x00446946u;

    struct HookState
    {
        PS2Runtime::RecompiledFunction originalStateUpdate = nullptr;
        PS2Runtime::RecompiledFunction originalYawNormalizer = nullptr;
        PS2Runtime::RecompiledFunction originalPause = nullptr;
        PS2Runtime::RecompiledFunction originalInventory = nullptr;
        PS2Runtime::RecompiledFunction originalMenuPrelude = nullptr;
        PS2Runtime::RecompiledFunction originalMenuUpdate = nullptr;
        uint64_t stateEntries = 0;
        uint64_t normalizerEntries = 0;
        uint64_t playerLookEntries = 0;
        uint64_t mouseApplications = 0;
        bool gameplayKnown = false;
        bool gameplayActive = false;
        bool pausePending = false;
        bool menuPending = false;
        bool inputTraceKnown = false;
        std::array<uint32_t, 23> inputTrace{};
        bool menuTraceKnown = false;
        std::array<uint32_t, 8> menuTrace{};
        bool worldTraceKnown = false;
        bool worldTraceReadFailed = false;
        uint64_t worldTraceSampleEntry = 0;
        std::array<uint32_t, 20> worldTrace{};
    };

    // Guest execution and these hooks run on the EE thread. Host input owns its
    // synchronized snapshot; no window or device API is called from here.
    HookState gState;

    bool traceEnabled()
    {
        static const bool enabled = []
        {
            const char *value = std::getenv("PS2X_INPUT_TRACE");
            return value && value[0] != '\0' && std::strcmp(value, "0") != 0;
        }();
        return enabled;
    }

    float readFloat(PS2Runtime &runtime, uint32_t address)
    {
        const uint32_t bits = runtime.memory().read32(address);
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    bool worldTraceEnabled()
    {
        static const bool enabled = []
        {
            const char *value = std::getenv("PS2X_WORLD_TRACE");
            return value && value[0] != '\0' && std::strcmp(value, "0") != 0;
        }();
        return enabled;
    }

    bool isGameplayActive(PS2Runtime &runtime);

    void traceWorldState(PS2Runtime &runtime, const char *reason)
    {
        if (!worldTraceEnabled()) return;
        try
        {
            // USA retail collision code at 001BDDC0 examines two 0x60-byte
            // world banks. Observe their actual ready/header/object fields;
            // never change a bank, player position or collision result here.
            std::array<uint32_t, 20> snapshot{};
            for (uint32_t index = 0; index != 2u; ++index)
            {
                const uint32_t bank = 0x0038CD20u + index * 0x60u;
                const uint32_t slot = index * 6u;
                snapshot[slot] = runtime.memory().read8(bank);
                snapshot[slot + 1u] = runtime.memory().read8(bank + 1u);
                const uint32_t header = runtime.memory().read32(bank + 8u);
                snapshot[slot + 2u] = header;
                snapshot[slot + 3u] = runtime.memory().read32(bank + 0x14u);
                // Cached/uncached aliases may name ordinary EE RAM. Reject
                // null, MMIO or overflowing pointers before following them.
                const uint32_t physical = header & 0x1FFFFFFFu;
                const bool valid = header != 0u && physical <= 0x01FFFFC2u;
                snapshot[slot + 4u] = valid
                    ? runtime.memory().read16(physical + 0x3Cu) : 0u;
                snapshot[slot + 5u] = valid ? 1u : 0u;
            }
            snapshot[12] = runtime.memory().read32(0x00446770u);
            snapshot[13] = runtime.memory().read32(0x004467E0u);
            snapshot[14] = runtime.memory().read32(0x0044685Cu);
            snapshot[15] = runtime.memory().read32(0x00446858u);
            snapshot[16] = runtime.memory().read32(0x0038A130u);
            snapshot[17] = runtime.memory().read32(0x004469E8u);
            snapshot[18] = runtime.memory().read32(0x004471D4u);
            snapshot[19] = isGameplayActive(runtime) ? 1u : 0u;
            const bool changed = !gState.worldTraceKnown || snapshot != gState.worldTrace;
            const bool sample = gState.stateEntries != gState.worldTraceSampleEntry &&
                                (gState.stateEntries <= 4u ||
                                 gState.stateEntries % 30u == 0u);
            if (!changed && !sample) return;
            gState.worldTraceKnown = true;
            gState.worldTrace = snapshot;
            gState.worldTraceSampleEntry = gState.stateEntries;
            std::fprintf(stderr,
                "[kfiv-world] %s state=%llu active=%u pos=(%.3f,%.3f,%.3f) "
                "yaw=%.6f vy=%.6f flags=%016llx ground=%08x type=%u/%u "
                "selector=%u dynamic=%08x "
                "bank0=(ready=%u byte1=%u header=%08x objects=%08x count=%u valid=%u) "
                "bank1=(ready=%u byte1=%u header=%08x objects=%08x count=%u valid=%u) "
                "loader=(ready=%u step=%u request=%08x bank-step=%u bank-info=%08x)\n",
                reason, static_cast<unsigned long long>(gState.stateEntries), snapshot[19],
                readFloat(runtime, kPlayer), readFloat(runtime, kPlayer + 4u),
                readFloat(runtime, kPlayer + 8u), readFloat(runtime, kYaw),
                readFloat(runtime, kPlayer + 0x1ACu),
                static_cast<unsigned long long>(runtime.memory().read64(kPlayerFlags)),
                runtime.memory().read32(kPlayer + 0x208u),
                runtime.memory().read8(kPlayer + 0x210u),
                runtime.memory().read8(kPlayer + 0x211u),
                snapshot[12], snapshot[13],
                snapshot[0], snapshot[1], snapshot[2], snapshot[3], snapshot[4], snapshot[5],
                snapshot[6], snapshot[7], snapshot[8], snapshot[9], snapshot[10], snapshot[11],
                snapshot[14], snapshot[15], snapshot[16], snapshot[17], snapshot[18]);
        }
        catch (const std::exception &error)
        {
            if (!gState.worldTraceReadFailed)
                std::fprintf(stderr, "[kfiv-world] observation unavailable: %s\n", error.what());
            gState.worldTraceReadFailed = true;
        }
    }

    void writeFloat(PS2Runtime &runtime, uint32_t address, float value)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        // Both addresses are verified ordinary game RAM. Store32's DMA handler
        // drain is unnecessary here and could disturb the guest call context.
        runtime.memory().write32(address, bits);
    }

    bool isGameplayActive(PS2Runtime &runtime)
    {
        return runtime.memory().read8(kGameplayByte) != 0u &&
               runtime.memory().read8(kPauseByte) == 0u && !gState.pausePending &&
               !gState.menuPending &&
               (runtime.memory().read32(kGameplayFlags) & 1u) == 0u;
    }

    void traceInputState(PS2Runtime &runtime, const char *reason)
    {
        if (!traceEnabled())
        {
            return;
        }
        const std::array<uint32_t, 23> snapshot = {
            runtime.memory().read16(kPadHeld),
            runtime.memory().read16(kPadEdge),
            runtime.memory().read16(0x00413DB8u),
            runtime.memory().read16(0x00413DBAu),
            runtime.memory().read16(0x00446942u),
            runtime.memory().read16(kStartConfig),
            runtime.memory().read32(0x0038CC74u),
            runtime.memory().read32(kGameplayFlags),
            runtime.memory().read8(kPauseByte),
            runtime.memory().read8(kPlayer + 0x1A8u),
            runtime.memory().read32(0x00446F78u),
            runtime.memory().read32(0x0038A13Cu),
            runtime.memory().read32(0x0038A148u),
            runtime.memory().read32(0x0038A134u),
            runtime.memory().read32(0x0038D620u),
            gState.pausePending ? 1u : 0u,
            gState.menuPending ? 1u : 0u,
            runtime.memory().read32(0x0038CC94u),
            runtime.memory().read32(0x0044685Cu),
            runtime.memory().read32(0x00446858u),
            runtime.memory().read32(0x0038A130u),
            runtime.memory().read32(0x004469E8u),
            runtime.memory().read32(0x004471D4u),
        };
        if (gState.inputTraceKnown && snapshot == gState.inputTrace)
        {
            return;
        }
        gState.inputTraceKnown = true;
        gState.inputTrace = snapshot;
        std::fprintf(stderr,
                     "[kfiv-input] input-state %s held=%04x edge=%04x "
                     "raw-held=%04x raw-edge=%04x circle=%04x start=%04x "
                     "commands=%08x flags=%08x pause=%u player-block=%u "
                     "buffer-selector=%u event=(%08x,%08x,%08x) "
                     "mode=%u pause-pending=%u menu-pending=%u input-lock-count=%d "
                     "loader-ready=%u loader-step=%u loader-request=%08x "
                     "bank-step=%u bank-info=%08x\n",
                     reason, snapshot[0], snapshot[1], snapshot[2], snapshot[3],
                     snapshot[4], snapshot[5], snapshot[6], snapshot[7], snapshot[8],
                     snapshot[9], snapshot[10], snapshot[11], snapshot[12],
                     snapshot[13], snapshot[14], snapshot[15], snapshot[16],
                     static_cast<int32_t>(snapshot[17]), snapshot[18], snapshot[19], snapshot[20],
                     snapshot[21], snapshot[22]);
    }

    void traceState(PS2Runtime &runtime, const char *reason)
    {
        if (!traceEnabled())
        {
            return;
        }
        std::fprintf(stderr,
                     "[kfiv-input] %s active=%u state=%llu normalizer=%llu "
                     "player-look=%llu mouse=%llu yaw=%.6f pitch=%.6f "
                     "pos=(%.3f,%.3f,%.3f)\n",
                     reason, gState.gameplayActive ? 1u : 0u,
                     static_cast<unsigned long long>(gState.stateEntries),
                     static_cast<unsigned long long>(gState.normalizerEntries),
                     static_cast<unsigned long long>(gState.playerLookEntries),
                     static_cast<unsigned long long>(gState.mouseApplications),
                     readFloat(runtime, kYaw), readFloat(runtime, kPitch),
                     readFloat(runtime, kPlayer), readFloat(runtime, kPlayer + 4u),
                     readFloat(runtime, kPlayer + 8u));
    }

    bool publishGameplayGate(PS2Runtime &runtime)
    {
        const bool active = isGameplayActive(runtime);
        const bool changed = !gState.gameplayKnown || active != gState.gameplayActive;
        gState.gameplayKnown = true;
        gState.gameplayActive = active;
        ps2_host_input::setGameplayActive(active);
        if (!active)
        {
            ps2_host_input::discardMouseLook();
        }
        if (changed)
        {
            traceState(runtime, "gameplay-transition");
        }
        return active;
    }

    void stateUpdateHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // Only the entry slot is replaced. All generated continuation slots
        // remain attached to the original function.
        if (ctx->pc == kStateUpdate)
        {
            // The blocking pause loop has returned before this next main-loop
            // update. Its generated continuations do not go through our entry.
            gState.pausePending = false;
            gState.menuPending = false;
            ++gState.stateEntries;
            publishGameplayGate(*runtime);
            traceInputState(*runtime, "state-entry");
            traceWorldState(*runtime, "state-entry");
        }
        gState.originalStateUpdate(rdram, ctx, runtime);
        publishGameplayGate(*runtime);
        traceInputState(*runtime, "state-return");
        traceWorldState(*runtime, "state-return");
        if (gState.stateEntries <= 4u || gState.stateEntries % 120u == 0u)
        {
            traceState(*runtime, "state-sample");
        }
    }

    void pauseHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kPause)
        {
            // This function is called every frame and enters its pause loop
            // only on the configured Start edge. The first nested call can
            // yield before the original sets the pause byte, so latch now.
            const uint16_t edge = runtime->memory().read16(kPadEdge);
            const uint16_t start = runtime->memory().read16(kStartConfig);
            if ((edge & start) != 0u)
            {
                gState.pausePending = true;
                publishGameplayGate(*runtime);
                traceInputState(*runtime, "pause-entry");
            }
        }
        gState.originalPause(rdram, ctx, runtime);
        publishGameplayGate(*runtime);
        traceInputState(*runtime, "pause-return");
    }

    void inventoryHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kInventory)
        {
            // Inventory executes its own pad/render loop, without the main
            // state updater. Keep host input in menu mode across its original
            // continuation slots until the main state updater resumes.
            gState.menuPending = true;
            publishGameplayGate(*runtime);
            traceInputState(*runtime, "inventory-entry");
        }
        gState.originalInventory(rdram, ctx, runtime);
        publishGameplayGate(*runtime);
        traceInputState(*runtime, "inventory-return");
    }

    void menuPreludeHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kMenuPrelude)
        {
            // The main-loop overlay dispatcher calls this prelude only after
            // its opening guards succeed, before inventory/shop/save loops.
            gState.menuPending = true;
            publishGameplayGate(*runtime);
            traceInputState(*runtime, "menu-prelude-entry");
        }
        gState.originalMenuPrelude(rdram, ctx, runtime);
        publishGameplayGate(*runtime);
        traceInputState(*runtime, "menu-prelude-return");
    }

    void menuUpdateHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        gState.originalMenuUpdate(rdram, ctx, runtime);
        if (!gState.menuPending) return;
        const std::array<uint32_t, 8> snapshot = {
            runtime->memory().read32(0x00446B20u),
            runtime->memory().read8(0x004472CDu),
            runtime->memory().read8(0x004472DAu),
            runtime->memory().read8(0x004472C4u),
            runtime->memory().read32(0x00446AC4u),
            runtime->memory().read32(0x00446AC8u),
            runtime->memory().read16(0x00446DE8u),
            runtime->memory().read32(0x00446C70u),
        };
        if (gState.menuTraceKnown && snapshot == gState.menuTrace) return;
        gState.menuTraceKnown = true;
        gState.menuTrace = snapshot;
        std::fprintf(stderr,
                     "[kfiv-input] menu-state handler=%08x selection=%u ready=%u "
                     "phase=%u lifecycle=(%u,%u) actions=%04x config-state=%u\n",
                     snapshot[0], snapshot[1], snapshot[2], snapshot[3],
                     snapshot[4], snapshot[5], snapshot[6], snapshot[7]);
    }

    void yawNormalizerHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ++gState.normalizerEntries;
        const bool playerLookCall = ctx->pc == kYawNormalizer &&
                                    getRegU32(ctx, 31) == kPlayerYawReturn &&
                                    getRegU32(ctx, 17) == kPlayer;
        if (!playerLookCall)
        {
            gState.originalYawNormalizer(rdram, ctx, runtime);
            return;
        }

        ++gState.playerLookEntries;
        const bool active = publishGameplayGate(*runtime);
        const ps2_host_input::MouseLook mouse = ps2_host_input::takeMouseLook();
        const float yawBefore = ctx->f[12];
        const float pitchBefore = readFloat(*runtime, kPitch);
        float yawDelta = 0.0f;
        float pitchDelta = 0.0f;
        if (active && mouse.captured)
        {
            if (std::isfinite(mouse.yawRadians) && std::isfinite(yawBefore))
            {
                yawDelta = mouse.yawRadians;
                // The caller has already added native controller velocity.
                // The original function wraps this sum into [-pi, pi].
                ctx->f[12] = yawBefore + yawDelta;
            }
            const bool pitchLocked =
                (runtime->memory().read64(kPlayerFlags) & kPitchLocked) != 0u;
            if (!pitchLocked && std::isfinite(mouse.pitchRadians) &&
                std::isfinite(pitchBefore))
            {
                pitchDelta = mouse.pitchRadians;
                if (pitchDelta != 0.0f)
                {
                    // After this call the original caller adds controller
                    // pitch velocity and clamps to the game's original limits.
                    writeFloat(*runtime, kPitch, pitchBefore + pitchDelta);
                }
            }
        }
        const bool applied = yawDelta != 0.0f || pitchDelta != 0.0f;
        if (applied)
        {
            ++gState.mouseApplications;
        }

        // No camera velocity or pad state is changed. A mouse displacement is
        // applied exactly once and cannot create artificial coast afterward.
        gState.originalYawNormalizer(rdram, ctx, runtime);

        if (traceEnabled() && (applied || gState.playerLookEntries <= 4u ||
                              gState.playerLookEntries % 120u == 0u))
        {
            std::fprintf(stderr,
                         "[kfiv-input] look=%llu active=%u captured=%u "
                         "dyaw=%.6f dpitch=%.6f yaw-in=%.6f yaw-out=%.6f "
                         "pitch-in=%.6f pitch-injected=%.6f mouse=%llu "
                         "pos=(%.3f,%.3f,%.3f)\n",
                         static_cast<unsigned long long>(gState.playerLookEntries),
                         active ? 1u : 0u, mouse.captured ? 1u : 0u,
                         yawDelta, pitchDelta, yawBefore, ctx->f[0], pitchBefore,
                         readFloat(*runtime, kPitch),
                         static_cast<unsigned long long>(gState.mouseApplications),
                         readFloat(*runtime, kPlayer),
                         readFloat(*runtime, kPlayer + 4u),
                         readFloat(*runtime, kPlayer + 8u));
        }
    }

    void applyKfivInput(PS2Runtime &runtime)
    {
        struct ExpectedInstruction
        {
            uint32_t address;
            uint32_t instruction;
        };
        constexpr ExpectedInstruction expected[] = {
            {kStateUpdate, 0x27BDFF10u},
            {kYawNormalizer, 0xC7839480u},
            {kPause, 0x27BDFFB0u},
            {0x00101FACu, 0x97849DD6u},
            {kInventory, 0x27BDFF80u},
            {kMenuPrelude, 0x27BDFFD0u},
            {0x001B6110u, 0x0C06DFB8u},
            {0x001B8858u, 0xC6200014u},
            {0x001B8860u, 0x0C066E0Eu},
            {0x001B8864u, 0x460C0300u},
            {0x001B8878u, 0xE6200014u},
            {0x001B8888u, 0xE6210010u},
        };

        try
        {
            for (const ExpectedInstruction &item : expected)
            {
                if (runtime.memory().read32(item.address) != item.instruction)
                {
                    std::fprintf(stderr,
                                 "[kfiv-input] declined hooks: instruction "
                                 "mismatch at 0x%08x\n", item.address);
                    return;
                }
            }
        }
        catch (const std::exception &)
        {
            std::fprintf(stderr, "[kfiv-input] declined hooks: game RAM unavailable\n");
            return;
        }

        if (!runtime.hasFunction(kStateUpdate) || !runtime.hasFunction(kYawNormalizer) ||
            !runtime.hasFunction(kPause) || !runtime.hasFunction(kInventory) ||
            !runtime.hasFunction(kMenuPrelude))
        {
            std::fprintf(stderr, "[kfiv-input] declined hooks: compiled functions missing\n");
            return;
        }
        const auto previousStateUpdate = runtime.lookupFunction(kStateUpdate);
        const auto previousYawNormalizer = runtime.lookupFunction(kYawNormalizer);
        const auto previousPause = runtime.lookupFunction(kPause);
        const auto previousInventory = runtime.lookupFunction(kInventory);
        const auto previousMenuPrelude = runtime.lookupFunction(kMenuPrelude);
        const auto originalStateUpdate = previousStateUpdate == &stateUpdateHook
                                             ? gState.originalStateUpdate
                                             : previousStateUpdate;
        const auto originalYawNormalizer = previousYawNormalizer == &yawNormalizerHook
                                               ? gState.originalYawNormalizer
                                               : previousYawNormalizer;
        const auto originalPause = previousPause == &pauseHook
                                       ? gState.originalPause
                                       : previousPause;
        const auto originalInventory = previousInventory == &inventoryHook
                                           ? gState.originalInventory
                                           : previousInventory;
        const auto originalMenuPrelude = previousMenuPrelude == &menuPreludeHook
                                             ? gState.originalMenuPrelude
                                             : previousMenuPrelude;
        if (!originalStateUpdate || !originalYawNormalizer || !originalPause ||
            !originalInventory || !originalMenuPrelude)
        {
            std::fprintf(stderr, "[kfiv-input] declined hooks: original functions missing\n");
            return;
        }

        if (!runtime.replaceFunction(kStateUpdate, &stateUpdateHook))
        {
            return;
        }
        if (!runtime.replaceFunction(kYawNormalizer, &yawNormalizerHook))
        {
            runtime.replaceFunction(kStateUpdate, previousStateUpdate);
            return;
        }
        if (!runtime.replaceFunction(kPause, &pauseHook))
        {
            runtime.replaceFunction(kYawNormalizer, previousYawNormalizer);
            runtime.replaceFunction(kStateUpdate, previousStateUpdate);
            return;
        }
        if (!runtime.replaceFunction(kInventory, &inventoryHook))
        {
            runtime.replaceFunction(kPause, previousPause);
            runtime.replaceFunction(kYawNormalizer, previousYawNormalizer);
            runtime.replaceFunction(kStateUpdate, previousStateUpdate);
            return;
        }
        if (!runtime.replaceFunction(kMenuPrelude, &menuPreludeHook))
        {
            runtime.replaceFunction(kInventory, previousInventory);
            runtime.replaceFunction(kPause, previousPause);
            runtime.replaceFunction(kYawNormalizer, previousYawNormalizer);
            runtime.replaceFunction(kStateUpdate, previousStateUpdate);
            return;
        }
        const auto previousMenuUpdateOriginal = gState.originalMenuUpdate;
        gState = {};
        gState.originalStateUpdate = originalStateUpdate;
        gState.originalYawNormalizer = originalYawNormalizer;
        gState.originalPause = originalPause;
        gState.originalInventory = originalInventory;
        gState.originalMenuPrelude = originalMenuPrelude;
        // Extra observation is enabled only for diagnostic runs. Production
        // menu execution keeps its original function registry entry.
        if (traceEnabled() && runtime.memory().read32(kMenuUpdate) == 0x27BDFFB0u &&
            runtime.hasFunction(kMenuUpdate))
        {
            const auto previous = runtime.lookupFunction(kMenuUpdate);
            const auto original = previous == &menuUpdateHook
                                      ? previousMenuUpdateOriginal : previous;
            if (original && runtime.replaceFunction(kMenuUpdate, &menuUpdateHook))
                gState.originalMenuUpdate = original;
        }
        ps2_host_input::enableVerditeControls();
        ps2_host_input::setGameplayActive(false);
        ps2_host_input::discardMouseLook();
        if (traceEnabled())
        {
            std::fprintf(stderr,
                         "[kfiv-input] installed USA SLUS_203.18 hooks "
                         "state=0x%08x yaw=0x%08x pause=0x%08x inventory=0x%08x "
                         "menu=0x%08x crc=0xbda82d37\n",
                         kStateUpdate, kYawNormalizer, kPause, kInventory, kMenuPrelude);
        }
    }
}

namespace ps2_kfiv_input
{
    void registerOverrides()
    {
        static std::once_flag registered;
        std::call_once(registered, []
        {
            ps2_game_overrides::registerDescriptor({
                "KFIV native mouse and keyboard input", "SLUS_203.18",
                0x00100008u, 0xBDA82D37u, &applyKfivInput});
        });
    }
}
