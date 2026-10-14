#ifndef PS2X_GS_RECORD_H
#define PS2X_GS_RECORD_H

#include "runtime/gs/gs_backend.h"
#include "gs_record_format.h"

#include <cstdint>
#include <memory>

// Opt-in GS backend command recording, for replaying a session's raster
// work outside the runtime and for comparing two runs (tools/gsreplay).
//
//   PS2X_GS_RECORD=<file>      record every backend call of the main GS
//   PS2X_GS_RECORD_FROM=<tick> start at the first Present with vsyncTick >= tick
//   PS2X_GS_RECORD_TO=<tick>   stop after the first Present with vsyncTick >= tick
//
// makeGsRasterBackend() returns the selected backend, wrapped in a recorder
// when the variable is set (only the first backend is recorded). The format
// is canonical (gs_record_format.h): identical GS work gives identical bytes.

std::unique_ptr<GSRasterBackend> makeGsRasterBackend();

// DW5XL-TEST: building blocks for the offline replay tools.
// A backend that accepts every call and draws nothing.
std::unique_ptr<GSRasterBackend> makeGsNullBackend();
// Wraps inner in a recorder writing to path; nullptr if path cannot be opened.
std::unique_ptr<GSRasterBackend> makeGsRecordingBackend(std::unique_ptr<GSRasterBackend> inner, const char *path,
                                                        uint64_t fromTick = 0, uint64_t toTick = UINT64_MAX);

#endif
