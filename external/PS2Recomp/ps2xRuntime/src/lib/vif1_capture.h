#ifndef PS2X_VIF1_CAPTURE_H
#define PS2X_VIF1_CAPTURE_H

// DW5XL-DEBUG: opt-in, bounded capture of VIF1 input for the offline replay
// harness (tools/gsreplay/vif1_replay.cpp). Default off; when the variable is
// unset the hooks cost one predictable branch.
//
//   PS2X_VIF1_CAPTURE=<file>        enable; writes a .v1cap file
//   PS2X_VIF1_CAPTURE_FROM=<tick>   first vsync tick to capture (default 0)
//   PS2X_VIF1_CAPTURE_TO=<tick>     last vsync tick to capture (default FROM+10)
//   PS2X_VIF1_CAPTURE_MAX_MB=<n>    stop after n MiB of kick payload (default 256)
//
// Captured: every buffer handed to PS2Memory::processVIF1Data (DMA chain
// snapshots and VIF1_FIFO writes, exactly as the interpreter receives them),
// EE writes to VIF1 registers (0x10003C00-0x10003DFF, e.g. FBRST aborts), and
// before the first captured kick: VU1 micro/data memory, VU1State, VIF1
// registers, PATH3 mask + masked FIFO and the PATH2 DIRECT continuation.
// Capture in synchronous mode (PS2X_VU1_ASYNC=0) so kicks and register writes
// are recorded in one program order.
//
// File:   "PS2XV1C1" | u32 version | u32 flags(0)
// Record: u8 type | u32 payloadBytes | payload
//   State  (1): u64 tick | u32 vu0Fbrst | VIFRegisters (23 x u32) | u8 path3Masked |
//               u32 pendingPath2Qwc | u8 pendingPath2DirectHl | u32 maskedPackets {u32 size, bytes}* |
//               u8 hasVu1State | VU1State (canonical, see writeVu1State) |
//               u32 codeSize, code | u32 dataSize, data
//   Kick   (2): u64 tick | u32 vu0Fbrst | u32 size | bytes
//   VifReg (3): u64 tick | u32 address | u32 value
//   End  (255): u32 kicks | u64 kickBytes | u8 truncatedByLimit

#include "runtime/ps2_vu1.h"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

class PS2Memory;
struct VIFRegisters;

namespace Vif1Capture
{
    inline constexpr char kMagic[8] = {'P', 'S', '2', 'X', 'V', '1', 'C', '1'};
    inline constexpr uint32_t kVersion = 1u;

    enum class Rec : uint8_t
    {
        State = 1,
        Kick = 2,
        VifReg = 3,
        End = 255,
    };

    // Set once from the environment at static initialisation.
    extern const bool g_requested;

    // Runtime hooks (no-ops unless PS2X_VIF1_CAPTURE is set).
    void onVif1Data(PS2Memory &memory, const uint8_t *data, uint32_t sizeBytes);
    void onVif1RegWrite(PS2Memory &memory, uint32_t address, uint32_t value);
    // The runtime attaches its VU1 core and a provider of the current EE
    // context's VU0 FBRST (its D/T-enable bits feed every MSCAL/MSCNT).
    void attach(VU1Interpreter *vu1, std::function<uint32_t()> fbrstProvider);

    inline void kick(PS2Memory &memory, const uint8_t *data, uint32_t sizeBytes)
    {
        if (g_requested)
            onVif1Data(memory, data, sizeBytes);
    }
    inline void regWrite(PS2Memory &memory, uint32_t address, uint32_t value)
    {
        if (g_requested)
            onVif1RegWrite(memory, address, value);
    }

    // ---- shared format helpers (runtime capture, replay harness, tests) ----
    struct StateSnapshot
    {
        uint64_t tick = 0;
        uint32_t vu0Fbrst = 0;
        uint32_t vif1Regs[23]{};
        bool path3Masked = false;
        uint32_t pendingPath2Qwc = 0;
        bool pendingPath2DirectHl = false;
        std::vector<std::vector<uint8_t>> maskedPath3Packets;
        bool hasVu1State = false;
        VU1State vu1{};
        std::vector<uint8_t> vu1Code;
        std::vector<uint8_t> vu1Data;
    };

    void writeVu1State(std::vector<uint8_t> &out, const VU1State &state);
    bool readVu1State(const uint8_t *&cur, const uint8_t *end, VU1State &state);
    void writeState(std::vector<uint8_t> &out, const StateSnapshot &state);
    bool readState(const uint8_t *cur, const uint8_t *end, StateSnapshot &state);
    // Snapshot the VIF1 side of PS2Memory (VU1 memory, VIF1 regs, PATH2/3 state).
    StateSnapshot snapshotMemory(const PS2Memory &memory);

    class Writer
    {
    public:
        Writer() = default;
        ~Writer() { close(); }
        Writer(const Writer &) = delete;
        Writer &operator=(const Writer &) = delete;

        bool open(const std::string &path);
        bool isOpen() const { return m_file != nullptr; }
        void writeState(const StateSnapshot &state);
        void writeKick(uint64_t tick, uint32_t vu0Fbrst, const uint8_t *data, uint32_t size);
        void writeVifReg(uint64_t tick, uint32_t address, uint32_t value);
        void writeEnd(bool truncatedByLimit);
        void close();
        uint32_t kicks() const { return m_kicks; }
        uint64_t kickBytes() const { return m_kickBytes; }

    private:
        void record(Rec type, const std::vector<uint8_t> &payload, const uint8_t *tail = nullptr, uint32_t tailSize = 0);

        std::FILE *m_file = nullptr;
        uint32_t m_kicks = 0;
        uint64_t m_kickBytes = 0;
        bool m_endWritten = false;
    };

    struct Record
    {
        Rec type{};
        std::vector<uint8_t> payload;
    };

    // Reads a whole .v1cap file. Returns false (with message) on a bad header;
    // a truncated trailing record is dropped with a warning in `message`.
    bool readFile(const std::string &path, std::vector<Record> &records, std::string &message);
}

#endif
