#ifndef PS2X_GS_RECORD_H
#define PS2X_GS_RECORD_H

#include "runtime/gs/gs_backend.h"

#include <memory>

// Opt-in GS backend command recording, for replaying a session's raster
// work outside the runtime (renderer regression tests and profiling).
//
//   PS2X_GS_RECORD=<file>  record every backend call of the main GS
//
// makeGsRasterBackend() returns the default CPU backend, wrapped in a
// recorder when the variable is set (only the first backend is recorded).

namespace GSRecord
{
    enum class Op : uint8_t
    {
        Initialize = 1, // u32 size, size bytes of initial VRAM
        Reset,
        Submit, // i32 FP rounding mode, GSPrimitiveBatch
        LoadClut, // GSTex0Reg, GSTexClutReg
        BeginTransfer, // GSTransferCommand
        UploadImage, // u32 size, bytes
        Flush,
        TextureFlush,
        Sync, // u8 GSSyncReason
        Present, // GSPresentationRequest
        ClearFramebuffer, // GSContext, u32 rgba
        ConsumeLocalToHost, // u32 maxBytes
        ReadVram, // u32 psm, base, bw, x, y
        WriteVram, // u32 psm, base, bw, x, y, value
        SnapshotVram,
    };
}

std::unique_ptr<GSRasterBackend> makeGsRasterBackend();

#endif
