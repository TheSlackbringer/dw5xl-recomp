#include "ps2x/state_archive.h"
#include "ps2x/iop/iop_ascii.h"
#include <cstdio>
#include "iop_emulator.h"
#include "ps2x/iop/iop_subsystem.h" // kIopHostClockPollEeCycles
#include "imports/iop_cdvd.h"
#include "core/iop_cpu.h"
#include "imports/iop_heaplib.h"
#include "imports/iop_imports.h"
#include "imports/iop_intrman.h"
#include "imports/iop_ioman.h"
#include "core/iop_kernel.h"
#include "imports/iop_loadcore.h"
#include "core/iop_memory.h"
#include "services/iop_module_loader.h"
#include "services/iop_rpc.h"
#include "imports/iop_stdio.h"
#include "imports/iop_sysclib.h"
#include "imports/iop_sysmem.h"
#include "imports/iop_timrman.h"
#include "imports/iop_vblank.h"
#include "iop_emulator_const.h"
#include "ps2x/iop/iop_native.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <utility>
#include <vector>
#include <string>
#include <cstring>

// Issue 009 v4.5: the translated IOP modules registered by the generated iop_registry.cpp.
namespace ps2x::iop
{
    namespace detail
    {
        std::vector<IopNativeModule> &nativeRegistry()
        {
            static std::vector<IopNativeModule> modules;
            return modules;
        }
    }

    IopNativeRegistrar::IopNativeRegistrar(const IopNativeModule *modules, size_t count) noexcept
    {
        try
        {
            detail::nativeRegistry().insert(detail::nativeRegistry().end(), modules, modules + count);
        }
        catch (...)
        {
        }
    }
}

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kRamSize = IopMemory::RamSize;
        constexpr uint32_t kKernelHeapBase = IopMemory::HeapBase;
        constexpr uint32_t kKernelHeapLimit = IopMemory::HeapLimit;
        constexpr uint32_t kCallStackBase = kKernelHeapLimit;
        constexpr uint32_t kCallStackLimit = 0x001FFF00u;
        constexpr uint32_t kCallStackSize = 0x2000u;
        constexpr uint32_t kCallStackCapacity = (kCallStackLimit - kCallStackBase) / kCallStackSize;
        constexpr uint64_t kCdvdCompletionCycles = 128u;
        constexpr uint64_t kIopClockHz = 36'864'000u;
        // DW5XL: generated EE code accounts 32 cycles per checkpoint. Reading the host clock and
        // running an IOP scheduler pass at each one cost more than the IOP work itself, so the
        // host-clocked IOP catches up once per this many EE cycles (~14 us of PS2 time).
        constexpr uint64_t kHostClockPollEeCycles = kIopHostClockPollEeCycles;
        constexpr uint64_t kNanosecondsPerSecond = 1'000'000'000u;

        uint32_t physicalAddress(uint32_t address)
        {
            return IopMemory::physicalAddress(address);
        }

        int32_t sign16(uint32_t value)
        {
            return static_cast<int16_t>(value & 0xFFFFu);
        }

        bool iequals(std::string_view lhs, std::string_view rhs)
        {
            return asciiEqualsIgnoreCase(lhs, rhs); // no C-runtime call per character (issue 025)
        }

        bool libsdTrace()
        {
            static const bool on = std::getenv("PS2X_LIBSD_TRACE") != nullptr; // DW5XL-DEBUG (read once: issue 025)
            return on;
        }

    }

    class IopEmulator::Impl final : public IopGuestExecutor
    {
    public:
        using CpuState = IopCpuState;

        struct Module
        {
            int id = 0;
            std::string path;
            std::string name;
            uint32_t base = 0;
            uint32_t size = 0;
            uint32_t entry = 0;
            uint32_t gp = 0;
            bool resident = false;
        };

        struct GuestCallback
        {
            uint32_t function = 0;
            uint32_t gp = 0;
        };

        struct ScheduledGuestCallback
        {
            uint32_t function = 0u;
            uint32_t gp = 0u;
            uint32_t argument = 0u;
        };

        explicit Impl(IopHost &hostRef)
            : host(hostRef),
              sysmem(host, memory),
              kernel(memory),
              cdvd(host, memory, kernel),
              vblank(kernel),
              rpc(host, memory, kernel),
              sysclib(memory),
              stdio(host, memory),
              heaplib(memory),
              intrman(memory),
              timrman(),
              ioman(memory),
              cpuCore(memory),
              imports(memory),
              loadcore(memory, imports)
        {
            memory.spu2().setPcmCallback([this](std::span<const int16_t> samples)
            {
                host.audioSamples(samples);
            });
            memory.spu2().setIrqCallback([this] { pendingSoundInterrupt = true; });
            memory.spu2().setKeyOnCallback([this] { return host.wantsSpu2KeyOns(); },
                                           [this](const Spu2KeyOn &keyOn) { host.spu2KeyOn(keyOn); });
            kernel.setHostWaitPump([this]
            {
                // Only for a top-level host call (an RPC server function); never re-enter the scheduler.
                if (inRunCycles || callDepth != 1u)
                    return false;
                const uint64_t before = totalInstructions;
                runCycles(kDefaultSlice * 16u);
                return totalInstructions != before; // false: no IOP thread ran in this slice
            });
            reset();
        }

        void reset()
        {
            memory.reset();
            kernel.reset();
            modules.clear();
            activeNative.clear(); // issue 009 v4.5 (memory.reset() cleared the code map)
            lastNative = 0u;
            imports.reset();
            rpc.reset();
            cdvd.reset();
            intrman.reset();
            timrman.reset();
            ioman.reset();
            pendingDmaInterrupts.clear();
            pendingGuestCallbacks.clear();
            nextModuleId = 1;
            moduleCursor = kModuleLoadBase;
            totalCycles = 0;
            soundCycles = 0;
            pendingSoundInterrupt = false;
            servicingSoundInterrupt = false;
            totalInstructions = 0;
            eeCycleCarry = 0;
            hostClockPendingEe = 0;
            hostClockEpochNs = host.monotonicNanoseconds();
            hostClockLastNs = hostClockEpochNs.value_or(0u);
            hostClockBaseCycles = 0u;
            hostClockLastTraceNs = hostClockLastNs;
            const char *clockTrace = std::getenv("PS2X_IOP_CLOCK_TRACE");
            hostClockTrace = clockTrace && *clockTrace && std::string_view(clockTrace) != "0";
            activeCpu = nullptr;
            lastError.clear();
            servicingDmaInterrupts = false;
            servicingGuestCallbacks = false;
            callDepth = 0u;
            secrMcCommandHandler = {};
            secrMcDevIdHandler = {};
            checkKelfPathCallback = {};
        }

        uint8_t read8(uint32_t address) const
        {
            return memory.read8(address);
        }

        uint16_t read16(uint32_t address) const
        {
            return memory.read16(address);
        }

        uint32_t read32(uint32_t address) const
        {
            return memory.read32(address);
        }

        void write8(uint32_t address, uint8_t value)
        {
            memory.write8(address, value);
            schedulePendingDma();
        }

        void write16(uint32_t address, uint16_t value)
        {
            memory.write16(address, value);
            schedulePendingDma();
        }

        void write32(uint32_t address, uint32_t value)
        {
            memory.write32(address, value);
            schedulePendingDma();
        }

        void schedulePendingDma()
        {
            if (const auto dma = memory.takeDmaStart())
            {
                pendingDmaInterrupts[dma->irq] = totalCycles + dma->delayCycles;
                if (libsdTrace()) // DW5XL-DEBUG
                    std::fprintf(stderr, "[dw5xl:iopdma] start irq=0x%x at=%llu due=%llu\n", dma->irq,
                                 (unsigned long long)totalCycles, (unsigned long long)(totalCycles + dma->delayCycles));
            }
        }

        void advanceSound()
        {
            if (totalCycles > soundCycles)
            {
                const uint64_t elapsed = totalCycles - soundCycles;
                soundCycles = totalCycles;
                memory.spu2().advance(elapsed);
            }
        }

        void serviceSoundInterrupt()
        {
            if (memory.interruptControl() != 0u && pendingSoundInterrupt && !servicingSoundInterrupt)
            {
                pendingSoundInterrupt = false;
                servicingSoundInterrupt = true;
                struct SoundInterruptGuard
                {
                    bool &servicing;
                    ~SoundInterruptGuard() { servicing = false; }
                } guard{servicingSoundInterrupt};
                (void)intrman.dispatchInterrupt(9, *this);
            }
        }

        bool readRam(uint32_t address, void *destination, size_t size) const
        {
            return memory.readRam(address, destination, size);
        }

        bool writeRam(uint32_t address, const void *source, size_t size)
        {
            return memory.writeRam(address, source, size);
        }

        bool zeroRam(uint32_t address, size_t size)
        {
            return memory.zeroRam(address, size);
        }

        bool isHardwareAddress(uint32_t phys) const
        {
            return memory.isHardwareAddress(phys);
        }

        uint32_t allocate(uint32_t size, uint32_t alignment = 16u, std::optional<uint32_t> fixed = std::nullopt)
        {
            return memory.allocate(size, alignment, fixed);
        }

        bool freeAllocation(uint32_t address)
        {
            return memory.freeAllocation(address);
        }

        void log(LogLevel level, std::string_view text)
        {
            host.log(level, text);
        }

        bool checkInterrupt(CpuState &cpu)
        {
            const uint32_t status = cpu.cop0[12];
            if ((status & 1u) == 0u)
                return false;
            if ((status & 0x2u) != 0u)
                return false;
            const bool pending = memory.interruptControl() != 0u && (memory.interruptStatus() & memory.interruptMask()) != 0u;
            if (!pending)
                return false;
            cpu.cop0[13] |= 0x400u;
            cpuCore.raiseException(cpu, 0u, cpu.pc, false);
            return true;
        }

        enum class ImportDisposition
        {
            Handled,
            JumpToGuest,
            Missing,
        };

        ImportDisposition dispatchImport(const IopImportCall &call, CpuState &cpu)
        {
            const uint32_t a0 = cpu.gpr[4];
            auto setV0 = [&](uint32_t value)
            {
                cpu.gpr[2] = value;
            };

            if (iequals(call.library, "libsd") && libsdTrace()) // DW5XL-DEBUG
            {
                static unsigned traced = 0;
                const uint32_t ord = call.ordinal;
                if ((((ord >= 17u && ord <= 20u) && (a0 & 1u)) || ord == 21u || ord == 22u) && traced++ < 400)
                    std::fprintf(stderr, "[dw5xl:libsd] #%u a0=0x%x a1=0x%x a2=0x%x\n", ord, a0, cpu.gpr[5], cpu.gpr[6]);
                if (ord == 15u || ord == 16u)
                    for (uint32_t k = 0x40u; k >= 4u; k -= 4u)
                        std::fprintf(stderr, "[dw5xl:libsd]   %08x: %08x\n", cpu.gpr[31] - k, memory.read32(cpu.gpr[31] - k));
                if (ord == 15u || ord == 16u)
                    for (uint32_t i = 0; i < 16u; ++i)
                        std::fprintf(stderr, "[dw5xl:libsd]   batch[%u] func=%u entry=0x%04x value=0x%08x\n", i,
                                     memory.read16(a0 + i * 8u), memory.read16(a0 + i * 8u + 2u), memory.read32(a0 + i * 8u + 4u));
            }
            // DW5XL-HACK (temporary): libsd #23 sceSdSetEffectAttr never succeeds against the partial
            // SPU2 emulation (returns -210 forever) and DW5/DW5XL loop on it at boot. Report success
            // (no reverb) until SPU2 effect-area handling is emulated.
            if (iequals(call.library, "libsd") && call.ordinal == 23u)
            {
                static int logged = 0;
                if (logged++ < 4)
                {
                    const uint32_t core = a0 & 1u;
                    std::fprintf(stderr,
                                 "[dw5xl:iop] bypassing libsd sceSdSetEffectAttr(core=%u) ATTR=0x%04x CHCR=0x%08x ADMA=0x%04x\n",
                                 a0, memory.read16(0x1F90019Au + core * 0x400u),
                                 memory.read32(core ? 0x1F801508u : 0x1F8010C8u),
                                 memory.read16(0x1F9001B0u + core * 0x400u));
                }
                setV0(0u);
                return ImportDisposition::Handled;
            }

            if (iequals(call.library, "sysmem") && sysmem.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            if (iequals(call.library, "cdvdman") && cdvd.dispatchImport(call.ordinal, cpu))
            {
                if (const auto callback = cdvd.takeCompletionCallback())
                {
                    pendingGuestCallbacks.emplace(
                        totalCycles + kCdvdCompletionCycles,
                        ScheduledGuestCallback{
                            callback->address,
                            callback->gp,
                            callback->reason,
                        });
                }
                return ImportDisposition::Handled;
            }

            if (iequals(call.library, "loadcore") && loadcore.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            if (iequals(call.library, "thbase") || iequals(call.library, "threadman"))
            {
                return kernel.dispatchThreadImport(call.ordinal, cpu, totalCycles)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "thsemap"))
            {
                return kernel.dispatchSemaphoreImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "thevent"))
            {
                return kernel.dispatchEventImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "sifcmd"))
            {
                return rpc.dispatchSifCmdImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "intrman") && intrman.dispatchImport(call.ordinal, cpu, *this))
                return ImportDisposition::Handled;
            if (iequals(call.library, "secrman"))
            {
                switch (call.ordinal)
                {
                case 4: // SecrSetMcCommandHandler
                    secrMcCommandHandler = {a0, cpu.gpr[28]};
                    setV0(0);
                    return ImportDisposition::Handled;
                case 5: // SecrSetMcDevIDHandler
                    secrMcDevIdHandler = {a0, cpu.gpr[28]};
                    setV0(0);
                    return ImportDisposition::Handled;
                default:
                    break;
                }
            }
            if (iequals(call.library, "modload") && call.ordinal == 13u)
            {
                checkKelfPathCallback = {a0, cpu.gpr[28]};
                setV0(0);
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "ioman") && ioman.dispatchImport(call.ordinal, cpu, *this))
                return ImportDisposition::Handled;
            if (iequals(call.library, "sifman"))
            {
                return rpc.dispatchSifManImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "vblank") && vblank.dispatchImport(call.ordinal, cpu, totalCycles))
                return ImportDisposition::Handled;
            if (iequals(call.library, "timrman") && timrman.dispatchImport(call.ordinal, cpu, totalCycles))
                return ImportDisposition::Handled;
            if (iequals(call.library, "dmacman"))
            {
                setV0(0);
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "stdio") && stdio.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;
            if (iequals(call.library, "sysclib"))
            {
                return sysclib.dispatchImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "heaplib") && heaplib.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            const uint32_t target = imports.resolve(call.library, call.ordinal, call.version);
            if (target != 0u)
            {
                cpu.pc = target;
                cpu.branchPending = false;
                return ImportDisposition::JumpToGuest;
            }

            std::ostringstream out;
            out << "[IOP] unhandled import " << call.library << ':' << call.ordinal
                << " version=0x" << std::hex << call.version << " pc=0x" << cpu.pc;
            log(LogLevel::Warning, out.str());
            setV0(0);
            return ImportDisposition::Missing;
        }

        bool step(CpuState &cpu)
        {
            advanceSound();
            if (cpu.stopped)
                return false;
            if (cpu.pc == kRpcServerReturnSentinel)
            {
                rpc.onServerReturned(kernel.currentThreadId()); // issue 009 v4.4: back asleep in its RPC loop
                return false;
            }
            if (cpu.pc == kThreadReturnSentinel || cpu.pc == kCallReturnSentinel)
            {
                cpu.stopped = true;
                return false;
            }
            if (physicalAddress(cpu.pc) >= kRamSize)
            {
                std::ostringstream out;
                out << "[IOP] execution outside RAM pc=0x" << std::hex << cpu.pc;
                log(LogLevel::Error, out.str());
                cpu.stopped = true;
                return false;
            }
            if (checkInterrupt(cpu))
                return true;

            if (const auto import = imports.decode(cpu.pc))
            {
                if (replayImport && replayImport->valid && replayImport->stubPc == cpu.pc)
                {
                    // Issue 009 v4.5 check mode: the native run made this call; take its result instead of calling again.
                    replayImportAtSameCount = totalCycles == replayImport->beforeCycles &&
                                              totalInstructions == replayImport->beforeInstructions;
                    cpu = replayImport->after;
                    totalCycles = replayImport->cycles;
                    totalInstructions = replayImport->instructions;
                    for (const ImportRecord::Bytes &bytes : replayImport->writes)
                    {
                        std::memcpy(memory.ramData() + bytes.address, bytes.data.data(), bytes.data.size());
                        std::memcpy(memory.ownedData() + bytes.address, bytes.owned.data(), bytes.owned.size());
                    }
                    replayImport = nullptr;
                    replayImportUsed = true;
                    return !cpu.stopped;
                }
                const ImportDisposition disposition = dispatchImport(*import, cpu);
                ++totalInstructions;
                ++totalCycles;
                interpretedInstructions += !verifyReplaying; // issue 009 v4.5 statistics
                if (disposition == ImportDisposition::JumpToGuest)
                    return true;
                cpu.pc = cpu.gpr[31];
                cpu.branchPending = false;
                return !cpu.stopped;
            }

            const bool running = cpuCore.executeInstruction(cpu);
            schedulePendingDma();
            ++totalInstructions;
            ++totalCycles;
            interpretedInstructions += !verifyReplaying; // issue 009 v4.5 statistics
            return running;
        }

        // ---- Issue 009 v4.5: translated module code (ps2x/iop/iop_native.h) in place of the interpreter ----
        struct ActiveNative
        {
            uint32_t begin = 0u;
            uint32_t end = 0u;
            uint64_t hash = 0u;
            IopNativeRun run = nullptr;
            const char *name = "";
        };

        static bool nativeSwitch(const char *name, bool fallback)
        {
            const char *value = std::getenv(name);
            return value && *value ? std::string_view(value) != "0" : fallback;
        }

        bool nativeWanted(const char *name) const
        {
            if (nativeOnly.empty())
                return true;
            std::string_view list = nativeOnly;
            while (!list.empty())
            {
                const size_t comma = list.find(',');
                if (iequals(list.substr(0, comma), name))
                    return true;
                if (comma == std::string_view::npos)
                    break;
                list.remove_prefix(comma + 1u);
            }
            return false;
        }

        // A module image now at [base, base + size): its translated code is used if the RAM holds exactly the bytes
        // it was made from.
        void activateNative(uint32_t base, uint32_t size)
        {
            if (!nativeEnabled)
                return;
            for (const IopNativeModule &module : nativeRegistry())
            {
                if (module.textBase < base || module.textBase + module.textSize > base + size || !nativeWanted(module.name))
                    continue;
                if (module.textBase + module.textSize > IopMemory::RamSize ||
                    iopNativeHash(memory.ramData() + module.textBase, module.textSize) != module.textHash)
                {
                    std::fprintf(stderr, "[iopnative] %s: code differs from the translation, interpreted\n", module.name);
                    continue;
                }
                deactivateNative(module.textBase, module.textSize);
                activeNative.push_back({module.textBase, module.textBase + module.textSize, module.textHash, module.run, module.name});
                memory.markNativeCode(module.textBase, module.textSize, true);
                std::fprintf(stderr, "[iopnative] %s native (0x%05x-0x%05x)%s\n", module.name, module.textBase,
                             module.textBase + module.textSize, nativeVerify ? ", check mode" : "");
            }
            lastNative = 0u;
        }

        void deactivateNative(uint32_t base, uint32_t size)
        {
            for (size_t i = 0; i < activeNative.size();)
            {
                if (activeNative[i].begin < base + size && base < activeNative[i].end)
                {
                    memory.markNativeCode(activeNative[i].begin, activeNative[i].end - activeNative[i].begin, false);
                    activeNative.erase(activeNative.begin() + static_cast<std::ptrdiff_t>(i));
                }
                else
                    ++i;
            }
            lastNative = 0u;
        }

        // A RAM write hit translated code: modules whose code bytes changed run in the interpreter from now on.
        void dropChangedNative()
        {
            for (size_t i = 0; i < activeNative.size();)
            {
                const ActiveNative &active = activeNative[i];
                if (iopNativeHash(memory.ramData() + active.begin, active.end - active.begin) != active.hash)
                {
                    std::fprintf(stderr, "[iopnative] %s: its code was overwritten, interpreted from now on\n", active.name);
                    memory.markNativeCode(active.begin, active.end - active.begin, false);
                    activeNative.erase(activeNative.begin() + static_cast<std::ptrdiff_t>(i));
                }
                else
                    ++i;
            }
            lastNative = 0u;
        }

        void rebuildNative()
        {
            activeNative.clear();
            memory.clearNativeCode();
            for (const auto &[id, module] : modules)
                activateNative(module.base, module.size);
        }

        // The interpreter's check before an instruction (checkInterrupt) without its effect.
        bool interruptDue(const CpuState &cpu) const
        {
            const uint32_t status = cpu.cop0[12];
            return (status & 1u) != 0u && (status & 2u) == 0u && memory.interruptControl() != 0u &&
                   (memory.interruptStatus() & memory.interruptMask()) != 0u;
        }

        // How many instructions may run before the interpreter would do something between two of them (runCpu's
        // services, the timer check, a sound sample that can raise the sound interrupt). 0: step instead.
        uint64_t nativeLimit(uint64_t budgetLeft, uint64_t nextTimerCheck)
        {
            uint64_t limit = std::min<uint64_t>(budgetLeft, nextTimerCheck > totalCycles ? nextTimerCheck - totalCycles : 0u);
            if (memory.interruptControl() != 0u)
            {
                if (pendingSoundInterrupt && !servicingSoundInterrupt)
                    return 0u;
                if (!servicingDmaInterrupts)
                    for (const auto &[irq, due] : pendingDmaInterrupts)
                        limit = std::min<uint64_t>(limit, due > totalCycles ? due - totalCycles : 1u);
            }
            if (!servicingGuestCallbacks && !pendingGuestCallbacks.empty())
            {
                const uint64_t due = pendingGuestCallbacks.begin()->first;
                limit = std::min<uint64_t>(limit, due > totalCycles ? due - totalCycles : 1u);
            }
            return std::min<uint64_t>(limit, memory.spu2().cyclesUntilIrqCheck());
        }

        const ActiveNative *nativeAt(uint32_t pc)
        {
            if (lastNative < activeNative.size() && pc - activeNative[lastNative].begin < activeNative[lastNative].end - activeNative[lastNative].begin)
                return &activeNative[lastNative];
            for (size_t i = 0; i < activeNative.size(); ++i)
                if (pc - activeNative[i].begin < activeNative[i].end - activeNative[i].begin)
                {
                    lastNative = i;
                    return &activeNative[i];
                }
            return nullptr;
        }

        static void toFrame(const CpuState &cpu, IopNativeFrame &f)
        {
            std::memcpy(f.gpr, cpu.gpr.data(), sizeof(f.gpr));
            f.hi = cpu.hi;
            f.lo = cpu.lo;
            f.pc = cpu.pc;
            f.pendingLoadReg = cpu.pendingLoadReg;
            f.pendingLoadValue = cpu.pendingLoadValue;
            f.pendingLoad = cpu.pendingLoad;
            f.branchPending = false;
            f.branchTarget = cpu.branchTarget;
        }

        static void fromFrame(const IopNativeFrame &f, CpuState &cpu)
        {
            std::memcpy(cpu.gpr.data(), f.gpr, sizeof(f.gpr));
            cpu.hi = f.hi;
            cpu.lo = f.lo;
            cpu.pc = f.pc;
            cpu.pendingLoadReg = f.pendingLoadReg;
            cpu.pendingLoadValue = f.pendingLoadValue;
            cpu.pendingLoad = f.pendingLoad;
            cpu.branchPending = f.branchPending;
            cpu.branchTarget = f.branchTarget;
            cpu.exception = false; // as executeInstruction leaves them after an instruction
            cpu.yielded = false;
        }

        // Check mode: what an import call in a native run did, so the interpreter's repeat gets the same result.
        struct ImportRecord
        {
            struct Bytes
            {
                uint32_t address = 0u;
                std::vector<uint8_t> data, owned;
            };
            bool valid = false;
            uint32_t stubPc = 0u;
            uint64_t beforeCycles = 0u, beforeInstructions = 0u; // the counters as the call starts
            std::string call;                                    // library:ordinal (for reports)
            CpuState after{};
            uint64_t cycles = 0u, instructions = 0u;
            std::vector<Bytes> writes;              // IOP RAM the call wrote (final bytes)
            std::vector<IopMemory::RamUndo> undo;   // ... and what it replaced, in order
        };

        // Runs translated code at cpu.pc in place of interpreter steps; returns the instructions carried out (0: the
        // caller steps the interpreter).
        uint32_t tryNative(CpuState &cpu, uint64_t budgetLeft, uint64_t nextTimerCheck)
        {
            if (activeNative.empty() || cpu.branchPending || cpu.stopped)
                return 0u;
            if (memory.takeNativeCodeWrite())
                dropChangedNative();
            const ActiveNative *module = nativeAt(cpu.pc);
            if (!module)
                return 0u;
            advanceSound(); // as step() does first
            if (interruptDue(cpu))
                return 0u;
            const uint64_t limit = nativeLimit(budgetLeft, nextTimerCheck);
            if (limit == 0u)
                return 0u;
            IopNativeFrame f{};
            toFrame(cpu, f);
            f.budget = static_cast<uint32_t>(std::min<uint64_t>(limit, 1u << 24u));
            f.ram = memory.ramData();
            f.owned = memory.ownedData();
            f.codeWords = memory.nativeCodeWords();
            f.hwRead = &nativeHwRead;
            f.hwWrite = &nativeHwWrite;
            f.importCall = &nativeImport;
            f.host = this;
            const uint64_t startCycles = totalCycles, startInstructions = totalInstructions;
            nativeRunCycles = startCycles;
            nativeRunInstructions = startInstructions;
            nativeRunCpu = &cpu;
            struct Depth
            {
                uint32_t &depth;
                ~Depth() { --depth; }
            } depth{++nativeDepth};
            if (nativeVerify)
            {
                const uint32_t verified = verifyNative(*module, cpu, f, nextTimerCheck);
                --nativeDepth;
                nativeStats();
                ++nativeDepth;
                return verified;
            }
            const uint32_t ran = module->run(f);
            if (ran == 0u)
            {
                totalCycles = startCycles; // an import that turned out not to be one may have set them
                totalInstructions = startInstructions;
                return 0u;
            }
            if (!f.importDone)
            {
                fromFrame(f, cpu);
                totalCycles = startCycles + ran;
                totalInstructions = startInstructions + ran;
            } // else: the import call left the thread state and the counters final (as step() does)
            nativeInstructions += ran;
            ++nativeRuns;
            if (f.codeWritten)
                dropChangedNative();
            --nativeDepth;
            nativeStats(); // only for the outermost run: nested runs (inside an import call) see moving counters
            ++nativeDepth;
            return ran;
        }

        // A hardware register (or the scratchpad) at this instruction of the run, as step() reaches it: the sound
        // chip advanced to this cycle, the access, then a DMA the access started is scheduled from this cycle.
        void nativeSyncTo(uint32_t executed)
        {
            totalCycles = nativeRunCycles + executed;
            totalInstructions = nativeRunInstructions + executed;
            advanceSound();
        }

        static uint32_t nativeHwRead(void *host, uint32_t address, uint32_t size, uint32_t executed)
        {
            Impl &e = *static_cast<Impl *>(host);
            e.nativeSyncTo(executed);
            const uint32_t value = size == 1u ? e.memory.read8(address) : size == 2u ? e.memory.read16(address) : e.memory.read32(address);
            e.schedulePendingDma();
            if (e.nativeHwRecord)
                e.nativeHwRecord->push_back({address, size, value, false, e.totalCycles});
            return value;
        }

        static void nativeHwWrite(void *host, uint32_t address, uint32_t size, uint32_t value, uint32_t executed)
        {
            Impl &e = *static_cast<Impl *>(host);
            e.nativeSyncTo(executed);
            if (size == 1u)
                e.memory.write8(address, static_cast<uint8_t>(value));
            else if (size == 2u)
                e.memory.write16(address, static_cast<uint16_t>(value));
            else
                e.memory.write32(address, value);
            e.schedulePendingDma();
            if (e.nativeHwRecord)
                e.nativeHwRecord->push_back({address, size, value, true, e.totalCycles});
        }

        // The import at f.pc, exactly as step() makes it; the thread state and the counters are final afterwards.
        static bool nativeImport(void *host, IopNativeFrame &f, uint32_t executed)
        {
            Impl &e = *static_cast<Impl *>(host);
            const auto call = e.imports.decode(f.pc);
            if (!call)
                return false;
            CpuState &cpu = *e.nativeRunCpu;
            fromFrame(f, cpu);
            e.nativeSyncTo(executed);
            if (e.interruptDue(cpu))
                return false; // the interpreter takes the interrupt first (it cannot become due inside a run)
            ImportRecord *record = e.nativeImportRecord;
            const uint32_t stubPc = f.pc;
            const uint64_t beforeCycles = e.nativeRunCycles + executed, beforeInstructions = e.nativeRunInstructions + executed;
            auto *previousUndo = e.memory.ramUndo();
            std::vector<IopMemory::RamUndo> ramUndo;
            if (record)
                e.memory.setRamUndo(&ramUndo);
            const ImportDisposition disposition = e.dispatchImport(*call, cpu);
            e.memory.setRamUndo(previousUndo);
            if (previousUndo) // an enclosing check needs these too
                previousUndo->insert(previousUndo->end(), ramUndo.begin(), ramUndo.end());
            ++e.totalInstructions;
            ++e.totalCycles;
            if (disposition != ImportDisposition::JumpToGuest)
            {
                cpu.pc = cpu.gpr[31];
                cpu.branchPending = false;
            }
            toFrame(cpu, f);
            f.branchPending = cpu.branchPending;
            f.importDone = true;
            if (record)
            {
                record->valid = true;
                record->stubPc = stubPc;
                record->beforeCycles = beforeCycles;
                record->beforeInstructions = beforeInstructions;
                record->call = call->library + ":" + std::to_string(call->ordinal);
                record->after = cpu;
                record->cycles = e.totalCycles;
                record->instructions = e.totalInstructions;
                record->writes.clear();
                for (const IopMemory::RamUndo &undo : ramUndo)
                {
                    ImportRecord::Bytes bytes{undo.address, {}, {}};
                    bytes.data.assign(e.memory.ramData() + undo.address, e.memory.ramData() + undo.address + undo.data.size());
                    bytes.owned.assign(e.memory.ownedData() + undo.address, e.memory.ownedData() + undo.address + undo.data.size());
                    record->writes.push_back(std::move(bytes));
                }
                record->undo = std::move(ramUndo);
            }
            return true;
        }

        // Check mode: the native run (hardware accesses and an import carried out for real and recorded), its own RAM
        // stores undone, then the same instructions by the interpreter, given the recorded hardware values and import
        // result instead of touching hardware or the kernel again; registers, counters and memory compared.
        uint32_t verifyNative(const ActiveNative &module, CpuState &cpu, IopNativeFrame &f, uint64_t nextTimerCheck)
        {
            const uint32_t startPc = cpu.pc;
            const CpuState before = cpu;
            const uint64_t startCycles = totalCycles, startInstructions = totalInstructions;
            std::vector<IopMemory::HwAccess> hwRecord;
            ImportRecord importRecord;
            std::vector<IopNativeFrame::UndoEntry> undo(4096u);
            f.undo = undo.data();
            f.undoCount = 0u;
            f.undoCapacity = undo.size();
            auto *previousHw = std::exchange(nativeHwRecord, &hwRecord);
            auto *previousImport = std::exchange(nativeImportRecord, &importRecord);
            const uint32_t ran = module.run(f);
            nativeHwRecord = previousHw;
            nativeImportRecord = previousImport;
            if (ran == 0u)
            {
                totalCycles = startCycles;
                totalInstructions = startInstructions;
                return 0u;
            }
            CpuState native = before;
            uint64_t nativeCycles = startCycles + ran, nativeInstructionCount = startInstructions + ran;
            if (f.importDone)
            {
                native = importRecord.after;
                nativeCycles = importRecord.cycles;
                nativeInstructionCount = importRecord.instructions;
            }
            else
                fromFrame(f, native);
            struct Written { uint32_t address, size; uint8_t bytes[4], owned[4]; };
            std::vector<Written> written(f.undoCount);
            for (size_t i = 0; i < f.undoCount; ++i)
            {
                written[i].address = f.undo[i].address;
                written[i].size = f.undo[i].size;
                std::memcpy(written[i].bytes, memory.ramData() + f.undo[i].address, f.undo[i].size);
                std::memcpy(written[i].owned, memory.ownedData() + f.undo[i].address, f.undo[i].size);
            }
            if (f.importDone) // the import came last: undo its RAM writes first
                for (size_t i = importRecord.undo.size(); i-- > 0;)
                {
                    const IopMemory::RamUndo &u = importRecord.undo[i];
                    std::memcpy(memory.ramData() + u.address, u.data.data(), u.data.size());
                    std::memcpy(memory.ownedData() + u.address, u.owned.data(), u.owned.size());
                }
            for (size_t i = f.undoCount; i-- > 0;)
            {
                std::memcpy(memory.ramData() + f.undo[i].address, f.undo[i].oldBytes, f.undo[i].size);
                std::memcpy(memory.ownedData() + f.undo[i].address, f.undo[i].oldOwned, f.undo[i].size);
            }
            // The interpreter, with no services in between (the limit says none would act; checked here too).
            cpu = before;
            totalCycles = startCycles;
            totalInstructions = startInstructions;
            std::vector<std::pair<uint32_t, uint32_t>> interpreted;
            auto *previousLog = memory.writeLog();
            memory.setWriteLog(&interpreted);
            IopMemory::HwReplay hwReplay{&hwRecord, &totalCycles};
            memory.setHwReplay(&hwReplay);
            replayImport = f.importDone ? &importRecord : nullptr;
            std::string problem;
            const bool wasReplaying = std::exchange(verifyReplaying, true);
            for (uint32_t i = 0; i < ran && problem.empty(); ++i)
            {
                if (!step(cpu) && i + 1u < ran)
                    problem = "the interpreter stopped";
                else if (i + 1u < ran && memory.interruptControl() != 0u && pendingSoundInterrupt && !servicingSoundInterrupt)
                    problem = "a sound interrupt became due inside the run";
                else if (i + 1u < ran && memory.interruptControl() != 0u && !servicingDmaInterrupts &&
                         std::any_of(pendingDmaInterrupts.begin(), pendingDmaInterrupts.end(),
                                     [&](const auto &dma) { return dma.second <= totalCycles; }))
                    problem = "a DMA completion became due inside the run";
                else if (i + 1u < ran && !servicingGuestCallbacks && !pendingGuestCallbacks.empty() &&
                         pendingGuestCallbacks.begin()->first <= totalCycles)
                    problem = "a callback became due inside the run";
                else if (i + 1u < ran && totalCycles >= nextTimerCheck)
                    problem = "the timer check fell inside the run";
            }
            verifyReplaying = wasReplaying;
            replayImport = nullptr;
            memory.setHwReplay(nullptr);
            memory.setWriteLog(previousLog);
            if (problem.empty() && (hwReplay.mismatch || hwReplay.next != hwRecord.size()))
                problem = "hardware accesses differ";
            if (problem.empty() && f.importDone && !replayImportUsed)
                problem = "the interpreter did not reach the import";
            if (problem.empty() && f.importDone && !replayImportAtSameCount)
                problem = "the interpreter reached the import at another instruction count";
            replayImportUsed = false;
            if (problem.empty() && (totalInstructions != nativeInstructionCount || totalCycles != nativeCycles))
                problem = "the interpreter ran a different number of instructions";
            if (problem.empty())
            {
                const bool pendingSame = cpu.pendingLoad == native.pendingLoad &&
                                         (!cpu.pendingLoad || (cpu.pendingLoadReg == native.pendingLoadReg &&
                                                               cpu.pendingLoadValue == native.pendingLoadValue));
                if (cpu.gpr != native.gpr || cpu.hi != native.hi || cpu.lo != native.lo || cpu.pc != native.pc ||
                    !pendingSame || cpu.branchPending != native.branchPending || cpu.branchTarget != native.branchTarget)
                    problem = "registers differ";
            }
            for (size_t i = 0; problem.empty() && i < written.size(); ++i)
                if (std::memcmp(memory.ramData() + written[i].address, written[i].bytes, written[i].size) != 0 ||
                    std::memcmp(memory.ownedData() + written[i].address, written[i].owned, written[i].size) != 0)
                {
                    uint32_t nativeWord = 0u, interpretedWord = 0u;
                    std::memcpy(&nativeWord, written[i].bytes, written[i].size);
                    std::memcpy(&interpretedWord, memory.ramData() + written[i].address, written[i].size);
                    char text[320];
                    std::snprintf(text, sizeof(text),
                                  "memory differs at 0x%05x (%u bytes: native %08x, interpreted %08x; run ended by %s, %zu hardware accesses)",
                                  written[i].address, written[i].size, nativeWord, interpretedWord,
                                  f.importDone ? ("import " + importRecord.call + " (" + std::to_string(importRecord.writes.size()) +
                                                  " RAM writes)").c_str()
                                               : f.ended ? "a hardware access" : "its limit", hwRecord.size());
                    problem = text;
                }
            for (size_t i = 0; problem.empty() && i < interpreted.size(); ++i)
            {
                const auto [address, size] = interpreted[i];
                const bool covered = std::any_of(written.begin(), written.end(), [&](const Written &w)
                                                 { return w.address <= address && address + size <= w.address + w.size; });
                if (!covered)
                    problem = "the interpreter wrote memory the native run did not";
            }
            ++verifyRuns;
            nativeInstructions += ran;
            ++nativeRuns;
            if (!problem.empty())
            {
                ++verifyMismatches;
                if (verifyMismatches <= 20u)
                    std::fprintf(stderr, "[iopnative:verify] MISMATCH %s start=0x%05x instructions=%u: %s\n", module.name,
                                 startPc, ran, problem.c_str());
            }
            if (memory.takeNativeCodeWrite() || f.codeWritten)
                dropChangedNative();
            return ran;
        }

        void nativeStats()
        {
            if (!nativeStatsOn || nativeDepth != 0u || totalCycles - nativeStatsCycle < kIopClockHz)
                return;
            // Native = every instruction the interpreter did not step (native runs, imports they made, nested runs).
            const uint64_t instructions = totalInstructions - nativeStatsInstructions;
            const uint64_t interpreted = interpretedInstructions - nativeStatsInterpreted;
            const uint64_t native = instructions >= interpreted ? instructions - interpreted : 0u;
            const uint64_t runs = nativeRuns - nativeStatsRuns;
            std::fprintf(stderr, "[iopnative] second native=%.1f%% (%llu of %llu instructions) runs=%llu (%.1f per run) verify=%llu mismatches=%llu"
                                 " [counted in runs %llu; totals: instructions %llu, interpreted %llu, in runs %llu]\n",
                         instructions ? 100.0 * static_cast<double>(native) / static_cast<double>(instructions) : 0.0,
                         static_cast<unsigned long long>(native), static_cast<unsigned long long>(instructions),
                         static_cast<unsigned long long>(runs), runs ? static_cast<double>(native) / static_cast<double>(runs) : 0.0,
                         static_cast<unsigned long long>(verifyRuns), static_cast<unsigned long long>(verifyMismatches),
                         static_cast<unsigned long long>(nativeInstructions - nativeStatsNative),
                         static_cast<unsigned long long>(totalInstructions), static_cast<unsigned long long>(interpretedInstructions),
                         static_cast<unsigned long long>(nativeInstructions));
            nativeStatsInterpreted = interpretedInstructions;
            nativeStatsCycle = totalCycles;
            nativeStatsInstructions = totalInstructions;
            nativeStatsNative = nativeInstructions;
            nativeStatsRuns = nativeRuns;
        }

        std::vector<ActiveNative> activeNative;
        size_t lastNative = 0u;
        bool nativeEnabled = nativeSwitch("PS2X_IOP_LIFT", true);
        bool nativeVerify = nativeSwitch("PS2X_IOP_LIFT_VERIFY", false);
        bool nativeStatsOn = nativeVerify || nativeSwitch("PS2X_IOP_LIFT_STATS", false);
        std::string nativeOnly = std::getenv("PS2X_IOP_LIFT_MODULES") ? std::getenv("PS2X_IOP_LIFT_MODULES") : "";
        uint64_t nativeInstructions = 0u, nativeRuns = 0u, verifyRuns = 0u, verifyMismatches = 0u;
        uint64_t nativeStatsCycle = 0u, nativeStatsInstructions = 0u, nativeStatsNative = 0u, nativeStatsRuns = 0u;
        uint64_t nativeRunCycles = 0u, nativeRunInstructions = 0u; // where the current native run started
        uint32_t nativeDepth = 0u;                                  // native runs in progress (imports nest them)
        uint64_t interpretedInstructions = 0u, nativeStatsInterpreted = 0u; // interpreter steps (not check replays)
        bool verifyReplaying = false;
        CpuState *nativeRunCpu = nullptr;
        std::vector<IopMemory::HwAccess> *nativeHwRecord = nullptr; // check mode: the run's hardware accesses
        ImportRecord *nativeImportRecord = nullptr;                // check mode: the run's import call
        ImportRecord *replayImport = nullptr;                      // check mode: the interpreter's repeat uses it
        bool replayImportUsed = false;
        bool replayImportAtSameCount = false;
        // ---- end issue 009 v4.5 ----

        uint32_t runCpu(CpuState &cpu, uint32_t instructionBudget)
        {
            CpuState *previous = activeCpu;
            activeCpu = &cpu;
            const uint64_t start = totalInstructions;
            uint64_t nextTimerCheck = totalCycles + kDefaultSlice;
            while (!cpu.stopped && !cpu.yielded && totalInstructions - start < instructionBudget)
            {
                // Issue 009 v4.5: translated code for as many instructions as the interpreter would run without
                // anything happening in between; otherwise one interpreter step.
                if (tryNative(cpu, instructionBudget - (totalInstructions - start), nextTimerCheck) == 0u && !step(cpu))
                    break;
                serviceSoundInterrupt();
                if (!servicingDmaInterrupts && !pendingDmaInterrupts.empty())
                    servicePendingDmaInterrupts();
                if (!servicingGuestCallbacks && !pendingGuestCallbacks.empty())
                    servicePendingGuestCallbacks();
                if (totalCycles >= nextTimerCheck)
                {
                    if (memory.interruptControl() != 0u)
                        timrman.serviceDue(totalCycles, *this);
                    nextTimerCheck = totalCycles + kDefaultSlice;
                }
            }
            activeCpu = previous;
            return static_cast<uint32_t>(totalInstructions - start);
        }

        uint32_t callFunction(uint32_t address,
                              uint32_t a0,
                              uint32_t a1,
                              uint32_t a2,
                              uint32_t a3,
                              uint32_t gp,
                              uint32_t budget = kMaxCallInstructions)
        {
            struct CallDepthGuard
            {
                uint32_t &depth;
                ~CallDepthGuard() { --depth; }
            };

            const uint32_t depth = callDepth++;
            const CallDepthGuard depthGuard{callDepth};
            CpuState cpu{};
            cpu.pc = address;
            cpu.gpr[4] = a0;
            cpu.gpr[5] = a1;
            cpu.gpr[6] = a2;
            cpu.gpr[7] = a3;
            cpu.gpr[28] = gp;
            if (depth < kCallStackCapacity)
            {
                const uint32_t stackTop = kCallStackLimit - depth * kCallStackSize;
                cpu.gpr[29] = stackTop - 32u;
            }
            else if (activeCpu && activeCpu->gpr[29] > kCallStackBase + kStackGuardBytes)
            {
                // Extremely deep re-entrancy borrows unused space below the
                // suspended caller's live frame. Stack growth remains away
                // from the caller, so its saved registers stay intact.
                cpu.gpr[29] = (activeCpu->gpr[29] - kStackGuardBytes) & ~15u;
            }
            else
            {
                cpu.gpr[29] = kCallStackBase - 32u;
            }
            cpu.gpr[31] = kCallReturnSentinel;
            runCpu(cpu, budget);
            return cpu.gpr[2];
        }

        uint32_t executeGuestFunction(uint32_t address,
                                      uint32_t a0,
                                      uint32_t a1,
                                      uint32_t a2,
                                      uint32_t a3,
                                      uint32_t gp) override
        {
            return callFunction(address, a0, a1, a2, a3, gp);
        }

        uint32_t executeGuestFunctionWithBudget(uint32_t address,
                                                uint32_t a0,
                                                uint32_t a1,
                                                uint32_t a2,
                                                uint32_t a3,
                                                uint32_t gp,
                                                uint32_t instructionBudget) override
        {
            return callFunction(address, a0, a1, a2, a3, gp, instructionBudget);
        }

        // Not that good to use exception handling for control flow but will do for now
        void servicePendingDmaInterrupts()
        {
            if (memory.interruptControl() == 0u || servicingDmaInterrupts || pendingDmaInterrupts.empty())
                return;

            servicingDmaInterrupts = true;

            std::vector<int> completed;
            for (auto it = pendingDmaInterrupts.begin(); it != pendingDmaInterrupts.end();)
            {
                if (it->second > totalCycles)
                {
                    ++it;
                    continue;
                }
                completed.push_back(it->first);
                it = pendingDmaInterrupts.erase(it);
            }
            try
            {
                for (const int irq : completed)
                {
                    if (!memory.soundDmaActive(irq))
                    {
                        if (libsdTrace()) // DW5XL-DEBUG
                            std::fprintf(stderr, "[dw5xl:iopdma] skip inactive irq=0x%x\n", irq);
                        continue;
                    }
                    memory.completeSoundDma(irq);
                    const auto dispatched = intrman.dispatchInterrupt(irq, *this);
                    if (libsdTrace()) // DW5XL-DEBUG
                        std::fprintf(stderr, "[dw5xl:iopdma] complete irq=0x%x at=%llu dispatched=%d\n", irq,
                                     (unsigned long long)totalCycles, static_cast<int>(dispatched));
                }
            }
            catch (...)
            {
                servicingDmaInterrupts = false;
                throw;
            }
            servicingDmaInterrupts = false;
        }

        void servicePendingGuestCallbacks()
        {
            if (servicingGuestCallbacks || pendingGuestCallbacks.empty())
                return;

            std::vector<ScheduledGuestCallback> callbacks;
            for (auto it = pendingGuestCallbacks.begin(); it != pendingGuestCallbacks.end();)
            {
                if (it->first > totalCycles)
                    break;
                callbacks.push_back(it->second);
                it = pendingGuestCallbacks.erase(it);
            }
            if (callbacks.empty())
                return;

            servicingGuestCallbacks = true;
            try
            {
                for (const ScheduledGuestCallback &callback : callbacks)
                {
                    if (callback.function != 0u)
                    {
                        (void)callFunction(callback.function,
                                           callback.argument,
                                           0u,
                                           0u,
                                           0u,
                                           callback.gp,
                                           100000u);
                    }
                }
            }
            catch (...)
            {
                servicingGuestCallbacks = false;
                throw;
            }
            servicingGuestCallbacks = false;
        }

        void runCycles(uint64_t cycles) noexcept
        {
            struct RunGuard
            {
                bool &flag;
                bool previous;
                ~RunGuard() { flag = previous; }
            } const runGuard{inRunCycles, std::exchange(inRunCycles, true)};
            try
            {
                const uint64_t target = totalCycles + cycles;
                while (totalCycles < target)
                {
                    advanceSound();
                    serviceSoundInterrupt();
                    servicePendingDmaInterrupts();
                    servicePendingGuestCallbacks();
                    if (memory.interruptControl() != 0u)
                        timrman.serviceDue(totalCycles, *this);
                    IopThread *next = kernel.beginNextReady(totalCycles);
                    if (!next)
                    {
                        uint64_t nextWake = kernel.nextWakeCycle(target);
                        for (const auto &[irq, completionCycle] : pendingDmaInterrupts)
                            nextWake = std::min(nextWake, completionCycle);
                        if (!pendingGuestCallbacks.empty())
                            nextWake = std::min(nextWake, pendingGuestCallbacks.begin()->first);
                        if (memory.interruptControl() != 0u)
                            nextWake = timrman.nextEventCycle(nextWake);
                        totalCycles = std::max(totalCycles + 1u, std::min(target, nextWake));
                        continue;
                    }
                    const uint64_t before = totalCycles;
                    runCpu(next->cpu, static_cast<uint32_t>(std::min<uint64_t>(kDefaultSlice, target - totalCycles)));
                    kernel.endTimeslice(*next, kThreadReturnSentinel);
                    if (totalCycles == before)
                        ++totalCycles;
                }
                advanceSound();
            }
            catch (...)
            {
                // Runtime scheduling must never throw through EeScheduler::accountCycles().
            }
        }

        void runHostClock(uint64_t nowNs) noexcept
        {
            if (!hostClockEpochNs || nowNs < hostClockLastNs)
            {
                // Capability changes or a broken host clock cannot rewind IOP
                // state or create unsigned elapsed-time catch-up bursts.
                hostClockEpochNs = nowNs;
                hostClockBaseCycles = totalCycles;
                hostClockLastTraceNs = nowNs;
            }
            hostClockLastNs = nowNs;
            const uint64_t elapsed = nowNs - *hostClockEpochNs;
            const uint64_t wholeSeconds = elapsed / kNanosecondsPerSecond;
            const uint64_t fraction = (elapsed % kNanosecondsPerSecond) * kIopClockHz / kNanosecondsPerSecond;
            const uint64_t maximum = std::numeric_limits<uint64_t>::max();
            const uint64_t clockCycles = wholeSeconds > (maximum - fraction) / kIopClockHz
                                            ? maximum : wholeSeconds * kIopClockHz + fraction;
            const uint64_t target = clockCycles > maximum - hostClockBaseCycles
                                        ? maximum : hostClockBaseCycles + clockCycles;
            const uint64_t deficit = target > totalCycles ? target - totalCycles : 0u;
            if (deficit)
                runCycles(deficit);
            else
                advanceSound();
            if (hostClockTrace && nowNs - hostClockLastTraceNs >= kNanosecondsPerSecond)
            {
                std::fprintf(stderr, "[iop-clock] elapsed-ns=%llu target=%llu current=%llu catchup=%llu\n",
                             static_cast<unsigned long long>(elapsed),
                             static_cast<unsigned long long>(target),
                             static_cast<unsigned long long>(totalCycles),
                             static_cast<unsigned long long>(deficit));
                hostClockLastTraceNs = nowNs;
            }
        }

        ModuleLoadResult loadImage(std::string path, std::span<const uint8_t> image, const void *arguments, uint32_t argumentSize)
        {
            ModuleLoadResult result{true, -1, -1};
            const IopImageLoadResult loaded = IopModuleLoader::load(image, memory, moduleCursor);
            moduleCursor = loaded.nextModuleCursor;
            if (moduleCursor > IopMemory::HeapBase) // DW5XL: modules must stay below the sysmem heap
                std::fprintf(stderr, "[dw5xl:iop] WARNING module cursor 0x%x overlaps heap base 0x%x\n",
                             moduleCursor, IopMemory::HeapBase);
            if (!loaded)
            {
                if (loaded.error == IopImageLoadError::InvalidElf)
                    log(LogLevel::Error, "[IOP] rejected invalid/non-MIPS IRX ELF");
                else if (loaded.error == IopImageLoadError::ArenaExhausted)
                    log(LogLevel::Error, "[IOP] module arena exhausted");
                return result;
            }
            if (!loaded.relocationsComplete)
                log(LogLevel::Warning, "[IOP] one or more IRX relocations were unsupported");

            activateNative(loaded.base, loaded.size); // issue 009 v4.5: before its start code runs

            Module module;
            module.id = nextModuleId++;
            module.path = std::move(path);
            const size_t slash = module.path.find_last_of("/\\:");
            module.name = slash == std::string::npos ? module.path : module.path.substr(slash + 1u);
            module.base = loaded.base;
            module.size = loaded.size;
            module.entry = loaded.entry;
            module.gp = loaded.gp;

            uint32_t args = 0u;
            if (arguments && argumentSize)
            {
                args = allocate(argumentSize + 1u, 16u);
                if (args)
                {
                    writeRam(args, arguments, argumentSize);
                    write8(args + argumentSize, 0u);
                }
            }
            const uint32_t startResult = callFunction(module.entry, argumentSize, args, 0u, 0u, module.gp);
            if (args)
                freeAllocation(args);
            module.resident = startResult == 0u || startResult == 2u;
            result.moduleId = module.id;
            result.startResult = static_cast<int32_t>(startResult);
            modules[module.id] = std::move(module);

            std::ostringstream out;
            out << "[IOP] loaded IRX id=" << result.moduleId
                << " entry=0x" << std::hex << modules[result.moduleId].entry
                << " base=0x" << modules[result.moduleId].base
                << " start=" << std::dec << result.startResult;
            log(LogLevel::Info, out.str());
            return result;
        }

        ModuleLoadResult loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
        {
            std::vector<uint8_t> image;
            if (!IopModuleLoader::readWholeHostFile(host, path, image))
            {
                log(LogLevel::Warning, std::string("[IOP] failed to open IRX '") + std::string(path) + "'");
                return {true, -1, -1};
            }
            return loadImage(std::string(path), image, arguments, argumentSize);
        }

        ModuleLoadResult loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
        {
            std::vector<uint8_t> image;
            if (!IopModuleLoader::readElfFromGuest(host, guestAddress, image))
                return {true, -1, -1};
            std::ostringstream tag;
            tag << "buffer@0x" << std::hex << guestAddress;
            return loadImage(tag.str(), image, arguments, argumentSize);
        }

        bool stopModule(int32_t moduleId, int32_t *result)
        {
            auto it = modules.find(moduleId);
            if (it == modules.end())
                return false;
            // A removable IRX normally exposes a stop entry through module metadata. We do not guess it; terminate owned execution and release the image cleanly.
            deactivateNative(it->second.base, it->second.size); // issue 009 v4.5
            kernel.terminateThreadsInRange(it->second.base, it->second.size);
            rpc.removeServersInRange(it->second.base, it->second.size);
            imports.eraseRange(it->second.base, it->second.size);
            modules.erase(it);
            kernel.cleanupDeadThreads();
            if (result)
                *result = 0;
            return true;
        }

        IopHost &host;
        IopMemory memory;
        IopSysmem sysmem;
        IopKernel kernel;
        IopCdvd cdvd;
        IopVblank vblank;
        IopRpcBridge rpc;
        IopSysclib sysclib;
        IopStdio stdio;
        IopHeaplib heaplib;
        IopIntrman intrman;
        IopTimrman timrman;
        IopIoman ioman;
        IopCpuCore cpuCore;
        IopImportRegistry imports;
        IopLoadcore loadcore;
        std::map<int, Module> modules;
        std::map<int, uint64_t> pendingDmaInterrupts;
        std::multimap<uint64_t, ScheduledGuestCallback> pendingGuestCallbacks;
        uint32_t nextModuleId = 1;
        uint32_t moduleCursor = kModuleLoadBase;
        uint64_t totalCycles = 0;
        uint64_t soundCycles = 0;
        bool pendingSoundInterrupt = false;
        bool servicingSoundInterrupt = false;
        uint64_t totalInstructions = 0;
        uint64_t eeCycleCarry = 0;
        uint64_t hostClockPendingEe = 0;
        std::optional<uint64_t> hostClockEpochNs;
        uint64_t hostClockLastNs = 0u, hostClockBaseCycles = 0u, hostClockLastTraceNs = 0u;
        bool hostClockTrace = false;
        CpuState *activeCpu = nullptr;
        std::string lastError;
        bool servicingDmaInterrupts = false;
        bool servicingGuestCallbacks = false;
        uint32_t callDepth = 0u;
        bool inRunCycles = false;
        GuestCallback secrMcCommandHandler;
        GuestCallback secrMcDevIdHandler;
        GuestCallback checkKelfPathCallback;
    };

    IopEmulator::IopEmulator(IopHost &host)
        : m_impl(std::make_unique<Impl>(host))
    {
    }

    IopEmulator::~IopEmulator() = default;

    void IopEmulator::reset()
    {
        m_impl->reset();
    }

    ModuleLoadResult IopEmulator::loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
    {
        return m_impl->loadModule(path, arguments, argumentSize);
    }

    ModuleLoadResult IopEmulator::loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
    {
        return m_impl->loadModuleBuffer(guestAddress, arguments, argumentSize);
    }

    bool IopEmulator::stopModule(int32_t moduleId, int32_t *result)
    {
        return m_impl->stopModule(moduleId, result);
    }

    void IopEmulator::runEeCycles(uint64_t eeCycles) noexcept
    {
        if (m_impl->hostClockEpochNs)
        {
            m_impl->hostClockPendingEe += eeCycles;
            if (m_impl->hostClockPendingEe < kHostClockPollEeCycles)
                return;
        }
        m_impl->hostClockPendingEe = 0;
        if (const auto now = m_impl->host.monotonicNanoseconds())
        {
            m_impl->runHostClock(*now);
            return;
        }
        m_impl->hostClockEpochNs.reset();
        const uint64_t total = m_impl->eeCycleCarry + eeCycles;
        const uint64_t iopCycles = total / 8u;
        m_impl->eeCycleCarry = total % 8u;
        if (iopCycles)
            m_impl->runCycles(iopCycles);
    }

    RpcResult IopEmulator::handleRpc(const RpcRequest &request)
    {
        return m_impl->rpc.handleRpc(request, *m_impl);
    }

    uint64_t IopEmulator::startRpc(const RpcRequest &request)
    {
        return m_impl->rpc.startRpc(request);
    }

    bool IopEmulator::busy() const noexcept
    {
        return m_impl->rpc.rpcInFlight() || m_impl->kernel.hasRunnableThread(m_impl->totalCycles);
    }

    bool IopEmulator::rpcInFlight() const noexcept
    {
        return m_impl->rpc.rpcInFlight();
    }

    void IopEmulator::runToHostClock() noexcept
    {
        if (const auto now = m_impl->host.monotonicNanoseconds())
            m_impl->runHostClock(*now);
    }

    bool IopEmulator::hasRpcServer(uint32_t sid) const noexcept
    {
        return m_impl->rpc.hasServer(sid);
    }

    void IopEmulator::onSifTransfer(const SifTransfer &transfer)
    {
        m_impl->rpc.onSifTransfer(transfer);
    }

    uint32_t IopEmulator::allocateMemory(uint32_t size, uint32_t alignment)
    {
        return m_impl->memory.allocate(size, alignment);
    }

    bool IopEmulator::freeMemory(uint32_t address)
    {
        return m_impl->memory.freeAllocation(address);
    }

    bool IopEmulator::readMemory(uint32_t address, void *destination, size_t size) const
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.readRam(address, destination, size);
    }

    bool IopEmulator::writeMemory(uint32_t address, const void *source, size_t size)
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.writeRam(address, source, size);
    }

    bool IopEmulator::zeroMemory(uint32_t address, size_t size)
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.zeroRam(address, size);
    }

    bool IopEmulator::isMemoryRange(uint32_t address, size_t size) const
    {
        const bool physicalSegment = address < IopMemory::RamSize;
        const bool cachedSegment = address >= 0x80000000u && address < 0x80200000u;
        const bool uncachedSegment = address >= 0xA0000000u && address < 0xA0200000u;
        if (!physicalSegment && !cachedSegment && !uncachedSegment)
            return false;
        const uint32_t physical = IopMemory::physicalAddress(address);
        return physical <= IopMemory::RamSize && size <= IopMemory::RamSize - physical;
    }

    uint64_t IopEmulator::cycles() const noexcept
    {
        return m_impl->totalCycles;
    }

    uint64_t IopEmulator::instructions() const noexcept
    {
        return m_impl->totalInstructions;
    }

    uint32_t IopEmulator::loadedModuleCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->modules.size());
    }

    uint32_t IopEmulator::threadCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->kernel.threadCount());
    }

    uint32_t IopEmulator::rpcServerCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->rpc.serverCount());
    }

}

// DW5XL (issue 027): save states.
namespace ps2x::iop::detail
{
    void IopEmulator::serializeState(ps2x::StateArchive &a)
    {
        Impl &e = *m_impl;
        if (a.saving() && (e.servicingSoundInterrupt || e.servicingDmaInterrupts || e.servicingGuestCallbacks ||
                           e.callDepth != 0u || e.inRunCycles))
            throw ps2x::StateError("IOP is inside a callback or a run slice");
        a.tag("IEMU");
        e.memory.serializeState(a);
        e.kernel.serializeState(a);
        e.cdvd.serializeState(a);
        e.rpc.serializeState(a);
        e.intrman.serializeState(a);
        e.timrman.serializeState(a);
        e.ioman.serializeState(a);
        e.imports.serializeState(a);
        a.map(e.modules, [](ps2x::StateArchive &x, Impl::Module &m)
        {
            x.value(m.id);
            x.string(m.path);
            x.string(m.name);
            x.value(m.base);
            x.value(m.size);
            x.value(m.entry);
            x.value(m.gp);
            x.value(m.resident);
        });
        a.podMap(e.pendingDmaInterrupts);
        struct PendingCallback { uint64_t cycle; Impl::ScheduledGuestCallback callback; };
        std::vector<PendingCallback> callbacks;
        for (const auto &[cycle, callback] : e.pendingGuestCallbacks)
            callbacks.push_back({cycle, callback});
        a.vector(callbacks);
        a.value(e.nextModuleId);
        a.value(e.moduleCursor);
        a.value(e.totalCycles);
        a.value(e.soundCycles);
        a.value(e.pendingSoundInterrupt);
        a.value(e.totalInstructions);
        a.value(e.eeCycleCarry);
        a.value(e.hostClockPendingEe);
        a.value(e.secrMcCommandHandler);
        a.value(e.secrMcDevIdHandler);
        a.value(e.checkKelfPathCallback);
        a.tag("IEND");
        if (a.loading())
        {
            e.pendingGuestCallbacks.clear();
            for (const PendingCallback &p : callbacks)
                e.pendingGuestCallbacks.emplace(p.cycle, p.callback);
            e.hostClockEpochNs.reset(); // the IOP clock restarts from the restored cycle count
            e.hostClockLastNs = 0u;
            e.rebuildNative(); // issue 009 v4.5: the restored RAM decides which modules run native
            e.activeCpu = nullptr;
        }
    }
}
