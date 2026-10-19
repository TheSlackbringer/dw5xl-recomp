#!/usr/bin/env bash
# Runs a Windows program from an llvm-mingw cross-build under Wine (issue 011), e.g. as CTest's
# CMAKE_CROSSCOMPILING_EMULATOR. Uses a separate Wine prefix so the normal Wine/Proton setup is never touched.
#   LLVM_MINGW_ROOT  unpacked llvm-mingw (for libc++.dll / libunwind.dll)
#   DW5XL_WINEPREFIX Wine prefix for tests (created on first use)
set -euo pipefail
root=${LLVM_MINGW_ROOT:-$HOME/.local/opt/llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64}
export WINEPREFIX=${DW5XL_WINEPREFIX:-$HOME/.local/opt/wine-dw5xl-tests}
export WINEDEBUG=${WINEDEBUG:--all}
export WINEPATH="$root/x86_64-w64-mingw32/bin"
exec wine "$@"
