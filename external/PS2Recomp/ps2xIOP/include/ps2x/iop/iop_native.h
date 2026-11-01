#pragma once
// DW5XL (issue 009 v4.5): IOP module code translated ahead of time into C++ (app/setup/ioplift) and run in place of
// the R3000A interpreter. The generated code only carries out plain instructions (arithmetic, branches, IOP RAM
// loads and stores); it stops before anything else (hardware registers, COP0, syscall/break, import stubs, faults)
// and when its instruction budget is used up, so the interpreter carries out those instructions itself and every
// event (interrupts, DMA, timers, sound, thread slices) happens at the same instruction as when interpreted.

#include <cstddef>
#include <cstdint>

namespace ps2x::iop
{
    inline constexpr uint32_t kIopNativeRamSize = 2u * 1024u * 1024u;

    // One native run: the interpreter's thread state in, the same state out (as if interpreted).
    struct IopNativeFrame
    {
        uint32_t gpr[32];
        uint32_t hi;
        uint32_t lo;
        uint32_t pc;
        uint32_t pendingLoadReg;   // the R3000A load delay: a load's value arrives one instruction later
        uint32_t pendingLoadValue;
        bool pendingLoad;
        bool branchPending;        // set when the run stops between a taken branch and its delay slot
        uint32_t branchTarget;
        uint32_t budget;           // instructions the run may carry out (each is one IOP cycle)
        uint32_t executed;         // instructions the run carried out in earlier chunks (kept by the module's run loop)
        bool codeWritten;          // a store hit translated code (the run stops; the caller drops that module)
        bool importDone;           // ... after an import: the call left the thread state and counters final
        bool ended;                // the run ended after a hardware access or an import (the checks between
                                   // instructions are due)
        uint8_t *ram;              // IOP RAM (kIopNativeRamSize bytes)
        uint8_t *owned;            // IopMemory's per-byte "written by the IOP" map
        const uint32_t *codeWords; // one bit per 32-bit word of RAM: set where native code was made from it
        // Check mode (PS2X_IOP_LIFT_VERIFY): every store's address, size and previous bytes, so the run can be undone
        // and repeated by the interpreter. Null in normal runs.
        struct UndoEntry
        {
            uint32_t address;
            uint32_t size;
            uint8_t oldBytes[4];
            uint8_t oldOwned[4];
        };
        UndoEntry *undo;
        size_t undoCount;
        size_t undoCapacity;       // a run stops before a store that would not fit
        // Hardware registers, scratchpad and import stubs: the runtime carries the instruction out as the
        // interpreter would at this point of the run (`executed` = instructions the run carried out before it) and the
        // run ends after it. With a null callback the run stops before such an instruction.
        uint32_t (*hwRead)(void *host, uint32_t address, uint32_t size, uint32_t executed);
        void (*hwWrite)(void *host, uint32_t address, uint32_t size, uint32_t value, uint32_t executed);
        // The import at pc: the frame (stored by the run) goes in, the state after the call comes out, with the
        // emulator's counters already final. False: pc is no import (the run stops before it).
        bool (*importCall)(void *host, IopNativeFrame &frame, uint32_t executed);
        void *host;
    };

    // Returns the number of instructions carried out (0: the interpreter must carry out the instruction at pc).
    using IopNativeRun = uint32_t (*)(IopNativeFrame &);

    // A translated module: valid only while the module's code in IOP RAM is exactly the bytes it was made from.
    struct IopNativeModule
    {
        const char *name;
        uint32_t textBase;  // IOP RAM address of the translated code
        uint32_t textSize;  // bytes
        uint64_t textHash;  // FNV-1a 64 of those bytes
        IopNativeRun run;
    };

    // The generated registry (iop_registry.cpp) holds a static registrar; the IOP emulator uses the registered
    // modules when they load (unless PS2X_IOP_LIFT=0).
    struct IopNativeRegistrar
    {
        IopNativeRegistrar(const IopNativeModule *modules, size_t count) noexcept;
    };

    [[nodiscard]] inline bool iopNativeCodeWord(const uint32_t *codeWords, uint32_t physical) noexcept
    {
        return (codeWords[physical >> 7u] >> ((physical >> 2u) & 31u)) & 1u;
    }

    [[nodiscard]] inline uint64_t iopNativeHash(const uint8_t *bytes, size_t size) noexcept
    {
        uint64_t hash = 0xCBF29CE484222325ull;
        for (size_t i = 0; i < size; ++i)
            hash = (hash ^ bytes[i]) * 0x100000001B3ull;
        return hash;
    }
}
