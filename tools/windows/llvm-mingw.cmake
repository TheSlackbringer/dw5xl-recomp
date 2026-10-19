# CMake toolchain for Windows x86-64 builds with llvm-mingw (clang/lld + MinGW-w64, UCRT).
# The same toolchain works hosted on Linux (cross-compiling) and on Windows (the release bundles it).
# Pass -DLLVM_MINGW_ROOT=<unpacked llvm-mingw folder>, or put its bin/ on PATH.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(LLVM_MINGW_ROOT "$ENV{LLVM_MINGW_ROOT}" CACHE PATH "Unpacked llvm-mingw toolchain")
if(LLVM_MINGW_ROOT)
    set(_llvm_mingw_bin "${LLVM_MINGW_ROOT}/bin/")
else()
    set(_llvm_mingw_bin "")
endif()
if(CMAKE_HOST_WIN32)
    set(_exe ".exe")
else()
    set(_exe "")
endif()

set(CMAKE_C_COMPILER "${_llvm_mingw_bin}x86_64-w64-mingw32-clang${_exe}")
set(CMAKE_CXX_COMPILER "${_llvm_mingw_bin}x86_64-w64-mingw32-clang++${_exe}")
set(CMAKE_RC_COMPILER "${_llvm_mingw_bin}x86_64-w64-mingw32-windres${_exe}")
set(CMAKE_AR "${_llvm_mingw_bin}llvm-ar${_exe}" CACHE FILEPATH "")
set(CMAKE_RANLIB "${_llvm_mingw_bin}llvm-ranlib${_exe}" CACHE FILEPATH "")

if(LLVM_MINGW_ROOT)
    set(CMAKE_FIND_ROOT_PATH "${LLVM_MINGW_ROOT}/x86_64-w64-mingw32")
endif()
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
