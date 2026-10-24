#pragma once

#include <atomic>
#include <cstdint>

// DW5XL-DEBUG issue 023: counters for PS2X_PRESENT_STATS (field reads copied for the window, and copies that took more
// than one field). Inline variables: the GPU scanout (GS) and the window loop (runtime) share them without either
// object having to be linked for the other (tools and tests link one without the other).
inline std::atomic<uint64_t> g_dw5xlReadsCopied{0};
inline std::atomic<uint64_t> g_dw5xlMultiReadCopies{0};
