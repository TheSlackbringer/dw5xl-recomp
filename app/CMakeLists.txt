# English PAL DW5 Xtreme Legends (SLES_538.60), built on PS2Recomp's runtime.
# Generated game code stays local under ../recomp/dw5xl/output.
cmake_minimum_required(VERSION 3.20)
project(dw5xl LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PS2RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../external/PS2Recomp")
set(DW5XL_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/dw5xl/output")

# Only the runtime (and the IOP module it needs) from PS2Recomp.
set(PS2X_BUILD_RECOMP OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_RUNTIME ON CACHE BOOL "" FORCE)
set(PS2X_BUILD_ANALYZER OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_STUDIO OFF CACHE BOOL "" FORCE)
add_subdirectory("${PS2RECOMP_DIR}" ps2recomp EXCLUDE_FROM_ALL)

file(GLOB DW5XL_GENERATED_SOURCES CONFIGURE_DEPENDS "${DW5XL_GENERATED_DIR}/*.cpp")

add_executable(dw5xl
    ${DW5XL_GENERATED_SOURCES}
    "${PS2RECOMP_DIR}/ps2xRuntime/src/main.cpp"
    "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/ps2_debug_panel.cpp"
    src/dw5xl_pad2.cpp
    src/dw5xl_images.cpp
    src/ipu_idec.cpp
)
target_include_directories(dw5xl PRIVATE "${DW5XL_GENERATED_DIR}" "${PS2RECOMP_DIR}/ps2xRuntime/src/lib")
target_link_libraries(dw5xl PRIVATE ps2_runtime rlImGui imgui)
target_compile_definitions(dw5xl PRIVATE PS2X_ENABLE_DEBUG_UI=1)
target_precompile_headers(dw5xl PRIVATE
    "${PS2RECOMP_DIR}/ps2xRuntime/include/ps2_runtime_macros.h"
    "${PS2RECOMP_DIR}/ps2xRuntime/include/ps2_runtime.h"
    "${DW5XL_GENERATED_DIR}/ps2_recompiled_functions.h"
    "${DW5XL_GENERATED_DIR}/ps2_recompiled_stubs.h"
    "${PS2RECOMP_DIR}/ps2xRuntime/include/ps2_syscalls.h"
    "${PS2RECOMP_DIR}/ps2xRuntime/include/ps2_stubs.h"
)
# 42k small generated files: batch them so the compiler isn't started 42k times.
set_target_properties(dw5xl PROPERTIES UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE 64)
set_source_files_properties(src/dw5xl_pad2.cpp src/dw5xl_images.cpp src/ipu_idec.cpp PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON SKIP_PRECOMPILE_HEADERS ON)
# Generated code uses SSE4.1/AVX intrinsics (PS2Recomp only enables them in its own directory).
target_compile_options(dw5xl PRIVATE -march=x86-64-v3)
