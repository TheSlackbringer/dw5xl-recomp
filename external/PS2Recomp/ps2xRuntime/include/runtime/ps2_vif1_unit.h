#ifndef PS2_VIF1_UNIT_H
#define PS2_VIF1_UNIT_H

#include <cstdint>

// PS2 VIF (VPU Interface) registers
struct VIFRegisters
{
    uint32_t stat;   // Status
    uint32_t fbrst;  // VIF Force Break
    uint32_t err;    // Error status
    uint32_t mark;   // Interrupt control
    uint32_t cycle;  // Transfer mode
    uint32_t mode;   // Mode control
    uint32_t num;    // Data amount counter
    uint32_t mask;   // Data mask
    uint32_t code;   // VIFcode
    uint32_t itops;  // ITOP save
    uint32_t base;   // Base address
    uint32_t ofst;   // Offset
    uint32_t tops;   // TOPS
    uint32_t itop;   // ITOP
    uint32_t top;    // TOP
    uint32_t row[4]; // Transfer row data
    uint32_t col[4]; // Transfer column data
};
static_assert(sizeof(VIFRegisters) == (23u * sizeof(uint32_t)), "VIFRegisters layout changed unexpectedly");

// DW5XL (issue 049, amendment v3.7): one VIF1 with its VU1 memory. Everything the VIF1 interpreter works on is in
// this object, so there can be more than one: the game's (PS2Memory holds it and keeps its own names for its
// parts), and one for in-between pictures. One implementation, any number of instances: process() is the only
// VIF1 interpreter.
struct Vif1Unit
{
    // What a unit reaches outside itself. The game's unit: the runtime memory object (GIF arbiter, the runtime's
    // VU1, the device worker's checkpoint). Called on the thread that calls process().
    struct Host
    {
        virtual ~Host() = default;
        // A PATH2 packet (DIRECT, DIRECTHL; also the rest of one that was cut off at a transfer's end).
        virtual void vif1Gif(const uint8_t *data, uint32_t sizeBytes, bool directHl) = 0;
        // MSCAL / MSCALF (`resume` false: VU1 starts at `startPC`) and MSCNT (`resume` true: it goes on).
        virtual void vif1Start(bool resume, uint32_t startPC, uint32_t top, uint32_t itop) = 0;
        // MSKPATH3 set the mask.
        virtual void vif1Path3Mask(bool masked) = 0;
        // MPG changed the VU1 program memory.
        virtual void vif1CodeChanged() = 0;
        // Before every VIFcode: `consumedBytes` of this transfer are done (the device worker may pause or drop
        // the rest there).
        virtual void vif1Checkpoint(uint32_t /*consumedBytes*/) {}
        // The field, for the debug traces (PS2X_GIF_PARSE_TRACE).
        virtual uint64_t vif1Tick() const { return 0u; }
    };

    VIFRegisters regs{};
    uint8_t *vu1Code = nullptr;             // PS2_VU1_CODE_SIZE bytes, not owned
    uint8_t *vu1Data = nullptr;             // PS2_VU1_DATA_SIZE bytes, not owned
    uint32_t pendingPath2ImageQwc = 0u;     // the rest of a DIRECT whose payload was cut off at a transfer's end
    bool pendingPath2DirectHl = false;

    void process(Host &host, const uint8_t *data, uint32_t sizeBytes);
};

#endif // PS2_VIF1_UNIT_H
