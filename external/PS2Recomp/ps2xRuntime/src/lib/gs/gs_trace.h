#ifndef PS2X_GS_TRACE_H
#define PS2X_GS_TRACE_H

// Optional, env-gated GS diagnostics.  Nothing here is active unless the
// process is launched with PS2X_GS_TRACE and/or PS2X_VRAM_DUMP set.
//
//   PS2X_GS_TRACE=<file>       append a text trace of GS activity
//   PS2X_GS_TRACE_FROM=<tick>  first vsync tick to record (default 0)
//   PS2X_GS_TRACE_TO=<tick>    last vsync tick to record (default infinite)
//   PS2X_VRAM_DUMP=<file>      raw 4 MiB GS local memory written on shutdown
//
// The trace file is buffered and only flushed/closed by shutdown().

#include "runtime/gs/gs_types.h"

#include <cstdint>
#include <functional>
#include <vector>

struct GSRegisters;

namespace GSTrace
{
    // Parse the env vars once.  Safe to call redundantly.
    void initialize();
    // Cheap (atomic) test used to keep the hot path free when tracing is off.
    bool enabled();

    // Registers the 4 MiB local memory + privileged register block backing the
    // active GS.  Called once from GS::init().
    void setLocalMemory(const uint8_t *vram, uint32_t size);
    void setPrivilegedRegisters(const GSRegisters *regs);
    void setSnapshotCallback(std::function<void(std::vector<uint8_t> &)> callback);

    // Current vsync tick, updated from the GS frontend so backend-side hooks
    // (image transfers) can tag their output without another header change.
    void setTick(uint64_t tick);

    // Emit a marker when a new in-range vsync tick starts.  Safe to call from
    // the dev harness every frame.
    void onTick(uint64_t tick);

    void registerWrite(uint8_t regAddr, uint64_t value, uint64_t tick);
    void imageTransfer(const GSBitBltBuf &buf,
                       const GSTrxPos &pos,
                       const GSTrxReg &reg,
                       uint32_t direction);
    void drawPrimitive(const GSPrimitiveBatch &batch, uint64_t tick);

    // Flush/close the trace file and write the VRAM dump if requested.  Safe to
    // call multiple times.
    void shutdown();
}

#endif
