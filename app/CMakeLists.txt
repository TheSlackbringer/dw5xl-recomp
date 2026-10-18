# English PAL DW5 Xtreme Legends (SLES_538.60), built on PS2Recomp's runtime.
# Generated game code stays local under ../recomp/dw5xl/output.
cmake_minimum_required(VERSION 3.20)
project(dw5xl LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Dw5xlPlatform.cmake")

set(PS2RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../external/PS2Recomp")
set(DW5XL_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/dw5xl/output")

# Only the runtime (and the IOP module it needs) from PS2Recomp.
set(PS2X_BUILD_RECOMP OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_RUNTIME ON CACHE BOOL "" FORCE)
set(PS2X_BUILD_ANALYZER OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_STUDIO OFF CACHE BOOL "" FORCE)
# MSVC/clang-cl only: keep the runtime's floating point like the Linux build.
set(PS2X_MSVC_FAST_FLOAT OFF CACHE BOOL "" FORCE)
add_subdirectory("${PS2RECOMP_DIR}" ps2recomp EXCLUDE_FROM_ALL)
# Runtime stubs also instantiate guest arithmetic helpers.
target_compile_options(ps2_runtime PRIVATE ${DW5XL_STRICT_FP_FLAGS})

file(GLOB DW5XL_GENERATED_SOURCES CONFIGURE_DEPENDS "${DW5XL_GENERATED_DIR}/*.cpp")

add_executable(dw5xl
    ${DW5XL_GENERATED_SOURCES}
    "${PS2RECOMP_DIR}/ps2xRuntime/src/main.cpp"
    "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/ps2_debug_panel.cpp"
    src/dw5xl_pad2.cpp
    src/dw5xl_images.cpp
    src/ipu_idec.cpp
    src/dw5xl_overlays.cpp
)
target_include_directories(dw5xl PRIVATE "${DW5XL_GENERATED_DIR}" "${PS2RECOMP_DIR}/ps2xRuntime/src/lib")
# Build ID (git describe --always --dirty) for the startup log and the F2 overlay (issue 012).
set(DW5XL_BUILD_ID_DIR "${CMAKE_CURRENT_BINARY_DIR}/build_id")
add_custom_target(dw5xl_build_id
    COMMAND "${CMAKE_COMMAND}" "-DSRC=${CMAKE_CURRENT_SOURCE_DIR}/.." "-DOUT=${DW5XL_BUILD_ID_DIR}/dw5xl_build_id.h"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_id.cmake"
    BYPRODUCTS "${DW5XL_BUILD_ID_DIR}/dw5xl_build_id.h"
    VERBATIM)
add_dependencies(dw5xl dw5xl_build_id)
target_include_directories(dw5xl PRIVATE "${DW5XL_BUILD_ID_DIR}")
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
set_source_files_properties(src/dw5xl_pad2.cpp src/dw5xl_images.cpp src/ipu_idec.cpp src/dw5xl_overlays.cpp PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON SKIP_PRECOMPILE_HEADERS ON)
# Lifted VU1 microprograms are translated from the game's own microcode (tools/vu1_translator.py, run by
# tools/regen.sh) into the ignored recomp/dw5xl/vu1/ folder. Game-derived: never committed. Without it,
# VU1 microcode runs in the interpreter.
set(DW5XL_VU1_LIFTED_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/dw5xl/vu1/dw5xl_vu1_lifted.cpp")
if(EXISTS "${DW5XL_VU1_LIFTED_SRC}")
    target_sources(dw5xl PRIVATE "${DW5XL_VU1_LIFTED_SRC}")
    set_source_files_properties("${DW5XL_VU1_LIFTED_SRC}" PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON SKIP_PRECOMPILE_HEADERS ON)
    set_property(SOURCE "${DW5XL_VU1_LIFTED_SRC}" APPEND PROPERTY COMPILE_OPTIONS ${DW5XL_STRICT_FP_FLAGS})
else()
    message(STATUS "No lifted VU1 code (run tools/regen.sh); VU1 microcode will be interpreted")
endif()
# Generated code uses SSE4.1/AVX intrinsics (PS2Recomp only enables them in its own directory).
target_compile_options(dw5xl PRIVATE ${DW5XL_ARCH_FLAGS} ${DW5XL_STRICT_FP_FLAGS})

# Overlay headers/symbols are isolated from the base executable's generated code.
set(DW5XL_OVL00_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/dw5xl/overlays/ovl00/output")
file(GLOB DW5XL_OVL00_SOURCES CONFIGURE_DEPENDS "${DW5XL_OVL00_DIR}/*.cpp")
if(NOT DW5XL_OVL00_SOURCES)
    message(FATAL_ERROR "Missing generated OVL00 code; run tools/regen.sh ovl00")
endif()
add_library(dw5xl_ovl00 STATIC ${DW5XL_OVL00_SOURCES})
target_link_libraries(dw5xl_ovl00 PRIVATE ps2_runtime)
target_include_directories(dw5xl_ovl00 PRIVATE "${DW5XL_OVL00_DIR}" "${PS2RECOMP_DIR}/ps2xRuntime/src/lib")
target_compile_options(dw5xl_ovl00 PRIVATE ${DW5XL_ARCH_FLAGS} ${DW5XL_STRICT_FP_FLAGS})
target_precompile_headers(dw5xl_ovl00 PRIVATE
    "${PS2RECOMP_DIR}/ps2xRuntime/include/ps2_runtime_macros.h"
    "${PS2RECOMP_DIR}/ps2xRuntime/include/ps2_runtime.h"
    "${DW5XL_OVL00_DIR}/ps2_recompiled_functions.h"
    "${DW5XL_OVL00_DIR}/ps2_recompiled_stubs.h"
    "${PS2RECOMP_DIR}/ps2xRuntime/include/ps2_syscalls.h"
    "${PS2RECOMP_DIR}/ps2xRuntime/include/ps2_stubs.h"
)
set_target_properties(dw5xl_ovl00 PROPERTIES UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE 64)
target_link_libraries(dw5xl PRIVATE dw5xl_ovl00)

# Overlay headers/symbols are isolated from the base executable's generated code.
set(DW5XL_OVL01_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/dw5xl/overlays/ovl01/output")
file(GLOB DW5XL_OVL01_SOURCES CONFIGURE_DEPENDS "${DW5XL_OVL01_DIR}/*.cpp")
if(NOT DW5XL_OVL01_SOURCES)
    message(FATAL_ERROR "Missing generated OVL01 code; run tools/regen.sh ovl01")
endif()
add_library(dw5xl_ovl01 STATIC ${DW5XL_OVL01_SOURCES})
target_link_libraries(dw5xl_ovl01 PRIVATE ps2_runtime)
target_include_directories(dw5xl_ovl01 PRIVATE "${DW5XL_OVL01_DIR}" "${PS2RECOMP_DIR}/ps2xRuntime/src/lib")
target_compile_options(dw5xl_ovl01 PRIVATE ${DW5XL_ARCH_FLAGS} ${DW5XL_STRICT_FP_FLAGS})
target_precompile_headers(dw5xl_ovl01 PRIVATE
    "${PS2RECOMP_DIR}/ps2xRuntime/include/ps2_runtime_macros.h"
    "${PS2RECOMP_DIR}/ps2xRuntime/include/ps2_runtime.h"
    "${DW5XL_OVL01_DIR}/ps2_recompiled_functions.h"
    "${DW5XL_OVL01_DIR}/ps2_recompiled_stubs.h"
    "${PS2RECOMP_DIR}/ps2xRuntime/include/ps2_syscalls.h"
    "${PS2RECOMP_DIR}/ps2xRuntime/include/ps2_stubs.h"
)
set_target_properties(dw5xl_ovl01 PROPERTIES UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE 64)
target_link_libraries(dw5xl PRIVATE dw5xl_ovl01)

set(DW5XL_OVL02_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/dw5xl/overlays/ovl02/output")
file(GLOB DW5XL_OVL02_SOURCES CONFIGURE_DEPENDS "${DW5XL_OVL02_DIR}/*.cpp")
if(NOT DW5XL_OVL02_SOURCES)
    message(FATAL_ERROR "Missing generated OVL02 code; run tools/regen.sh ovl02")
endif()
add_library(dw5xl_ovl02 STATIC ${DW5XL_OVL02_SOURCES})
target_link_libraries(dw5xl_ovl02 PRIVATE ps2_runtime)
target_include_directories(dw5xl_ovl02 PRIVATE "${DW5XL_OVL02_DIR}" "${PS2RECOMP_DIR}/ps2xRuntime/src/lib")
target_compile_options(dw5xl_ovl02 PRIVATE ${DW5XL_ARCH_FLAGS} ${DW5XL_STRICT_FP_FLAGS})
target_precompile_headers(dw5xl_ovl02 PRIVATE
    "${PS2RECOMP_DIR}/ps2xRuntime/include/ps2_runtime_macros.h"
    "${PS2RECOMP_DIR}/ps2xRuntime/include/ps2_runtime.h"
    "${DW5XL_OVL02_DIR}/ps2_recompiled_functions.h"
    "${DW5XL_OVL02_DIR}/ps2_recompiled_stubs.h"
    "${PS2RECOMP_DIR}/ps2xRuntime/include/ps2_syscalls.h"
    "${PS2RECOMP_DIR}/ps2xRuntime/include/ps2_stubs.h"
)
set_target_properties(dw5xl_ovl02 PROPERTIES UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE 64)
target_link_libraries(dw5xl PRIVATE dw5xl_ovl02)

option(DW5XL_BUILD_TESTS "Build runtime and locally generated helper tests" OFF)
if(DW5XL_BUILD_TESTS)
    enable_testing()
    add_executable(dw5xl_overlay_dispatch_test tests/overlay_dispatch_test.cpp)
    target_link_libraries(dw5xl_overlay_dispatch_test PRIVATE ps2_runtime)
    target_compile_options(dw5xl_overlay_dispatch_test PRIVATE ${DW5XL_ARCH_FLAGS})
    add_test(NAME overlay_dispatch COMMAND dw5xl_overlay_dispatch_test)
    add_executable(dw5xl_field_history_test tests/field_history_test.cpp)
    target_include_directories(dw5xl_field_history_test PRIVATE "${PS2RECOMP_DIR}/ps2xRuntime/include")
    add_test(NAME field_history COMMAND dw5xl_field_history_test)
    add_executable(dw5xl_cd_iop_read_test tests/cd_iop_read_test.cpp)
    target_link_libraries(dw5xl_cd_iop_read_test PRIVATE ps2_runtime)
    target_compile_options(dw5xl_cd_iop_read_test PRIVATE ${DW5XL_ARCH_FLAGS})
    add_test(NAME cd_iop_read COMMAND dw5xl_cd_iop_read_test)
    add_executable(dw5xl_presentation_stride_test tests/presentation_stride_test.cpp)
    target_link_libraries(dw5xl_presentation_stride_test PRIVATE ps2_runtime)
    target_compile_options(dw5xl_presentation_stride_test PRIVATE ${DW5XL_ARCH_FLAGS})
    add_test(NAME presentation_stride COMMAND dw5xl_presentation_stride_test)
    add_executable(dw5xl_vif_unpack_boundary_test tests/vif_unpack_boundary_test.cpp)
    target_link_libraries(dw5xl_vif_unpack_boundary_test PRIVATE ps2_runtime)
    target_compile_options(dw5xl_vif_unpack_boundary_test PRIVATE ${DW5XL_ARCH_FLAGS})
    add_test(NAME vif_unpack_boundary COMMAND dw5xl_vif_unpack_boundary_test)
    add_executable(dw5xl_ee_shared_vu0_test tests/ee_shared_vu0_test.cpp)
    target_link_libraries(dw5xl_ee_shared_vu0_test PRIVATE ps2_runtime)
    target_compile_options(dw5xl_ee_shared_vu0_test PRIVATE ${DW5XL_ARCH_FLAGS})
    add_test(NAME ee_shared_vu0 COMMAND dw5xl_ee_shared_vu0_test)
    set_tests_properties(ee_shared_vu0 PROPERTIES TIMEOUT 20)
    add_executable(dw5xl_ee_rounding_test tests/ee_rounding_test.cpp)
    target_link_libraries(dw5xl_ee_rounding_test PRIVATE ps2_runtime)
    target_compile_options(dw5xl_ee_rounding_test PRIVATE ${DW5XL_ARCH_FLAGS} ${DW5XL_STRICT_FP_FLAGS})
    add_test(NAME ee_rounding COMMAND dw5xl_ee_rounding_test)
    add_executable(dw5xl_vu_minmax_test tests/vu_minmax_test.cpp)
    target_link_libraries(dw5xl_vu_minmax_test PRIVATE ps2_runtime)
    target_compile_options(dw5xl_vu_minmax_test PRIVATE ${DW5XL_ARCH_FLAGS})
    add_test(NAME vu_minmax COMMAND dw5xl_vu_minmax_test)
    add_executable(dw5xl_vif_unpack_fast_test tests/vif_unpack_fast_test.cpp)
    target_link_libraries(dw5xl_vif_unpack_fast_test PRIVATE ps2_runtime)
    target_compile_options(dw5xl_vif_unpack_fast_test PRIVATE ${DW5XL_ARCH_FLAGS})
    add_test(NAME vif_unpack_fast COMMAND dw5xl_vif_unpack_fast_test)
    add_executable(dw5xl_vu1_fmac_fast_test tests/vu1_fmac_fast_test.cpp)
    target_link_libraries(dw5xl_vu1_fmac_fast_test PRIVATE ps2_runtime)
    target_compile_options(dw5xl_vu1_fmac_fast_test PRIVATE ${DW5XL_ARCH_FLAGS} ${DW5XL_STRICT_FP_FLAGS})
    add_test(NAME vu1_fmac_fast COMMAND dw5xl_vu1_fmac_fast_test)
    add_executable(dw5xl_vu0_macro_test tests/vu0_macro_test.cpp)
    target_link_libraries(dw5xl_vu0_macro_test PRIVATE ps2_runtime)
    target_compile_options(dw5xl_vu0_macro_test PRIVATE ${DW5XL_ARCH_FLAGS} ${DW5XL_STRICT_FP_FLAGS})
    add_test(NAME vu0_macro COMMAND dw5xl_vu0_macro_test)
    add_executable(dw5xl_ee_poll_sema_test tests/ee_poll_sema_test.cpp
        "${DW5XL_GENERATED_DIR}/FUN_00144c70_0x144c70.cpp")
    target_link_libraries(dw5xl_ee_poll_sema_test PRIVATE ps2_runtime)
    target_include_directories(dw5xl_ee_poll_sema_test PRIVATE "${DW5XL_GENERATED_DIR}" "${PS2RECOMP_DIR}/ps2xRuntime/src/lib")
    target_compile_options(dw5xl_ee_poll_sema_test PRIVATE ${DW5XL_ARCH_FLAGS})
    add_test(NAME ee_poll_sema COMMAND dw5xl_ee_poll_sema_test)
    add_executable(dw5xl_original_gs_helper_test tests/original_gs_helper_test.cpp
        "${DW5XL_GENERATED_DIR}/FUN_00110c78_0x110c78.cpp"
        "${DW5XL_GENERATED_DIR}/FUN_001111d8_0x1111d8.cpp")
    target_link_libraries(dw5xl_original_gs_helper_test PRIVATE ps2_runtime)
    target_include_directories(dw5xl_original_gs_helper_test PRIVATE "${DW5XL_GENERATED_DIR}" "${PS2RECOMP_DIR}/ps2xRuntime/src/lib")
    target_compile_options(dw5xl_original_gs_helper_test PRIVATE ${DW5XL_ARCH_FLAGS})
    add_test(NAME original_gs_helper COMMAND dw5xl_original_gs_helper_test)
    add_executable(dw5xl_disc_validation_test tests/disc_validation_test.cpp)
    target_link_libraries(dw5xl_disc_validation_test PRIVATE ps2_runtime)
    target_include_directories(dw5xl_disc_validation_test PRIVATE "${PS2RECOMP_DIR}/ps2xRuntime/src/lib")
    target_compile_options(dw5xl_disc_validation_test PRIVATE ${DW5XL_ARCH_FLAGS})
    add_test(NAME disc_validation COMMAND dw5xl_disc_validation_test)
    add_executable(dw5xl_overlay_lifecycle_test tests/overlay_lifecycle_test.cpp)
    target_link_libraries(dw5xl_overlay_lifecycle_test PRIVATE ps2_runtime)
    target_compile_options(dw5xl_overlay_lifecycle_test PRIVATE ${DW5XL_ARCH_FLAGS})
    add_test(NAME overlay_lifecycle COMMAND dw5xl_overlay_lifecycle_test)

    add_executable(dw5xl_vu1_async_queue_test tests/vu1_async_queue_test.cpp)
    target_link_libraries(dw5xl_vu1_async_queue_test PRIVATE ps2_runtime)
    target_compile_options(dw5xl_vu1_async_queue_test PRIVATE ${DW5XL_ARCH_FLAGS})
    add_test(NAME vu1_async_queue COMMAND dw5xl_vu1_async_queue_test)
    set_tests_properties(vu1_async_queue PROPERTIES TIMEOUT 20)
    # ThreadSanitizer variant: the queue, PS2Memory, VIF1 parser and GIF arbiter
    # are recompiled instrumented; their archive copies are then never pulled in.
    option(DW5XL_TSAN_TESTS "Build the ThreadSanitizer variant of the VU1 async queue test" ON)
    if(DW5XL_TSAN_TESTS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT WIN32)
        set(DW5XL_RUNTIME_SRC "${PS2RECOMP_DIR}/ps2xRuntime/src/lib")
        add_executable(dw5xl_vu1_async_queue_tsan_test
            tests/vu1_async_queue_test.cpp
            "${DW5XL_RUNTIME_SRC}/ps2_device_queue.cpp"
            "${DW5XL_RUNTIME_SRC}/ps2_memory.cpp"
            "${DW5XL_RUNTIME_SRC}/ps2_vif1_interpreter.cpp"
            "${DW5XL_RUNTIME_SRC}/gs/ps2_gif_arbiter.cpp")
        target_link_libraries(dw5xl_vu1_async_queue_tsan_test PRIVATE ps2_runtime)
        target_compile_options(dw5xl_vu1_async_queue_tsan_test PRIVATE ${DW5XL_ARCH_FLAGS} -fsanitize=thread -g -O1)
        target_link_options(dw5xl_vu1_async_queue_tsan_test PRIVATE -fsanitize=thread)
        add_test(NAME vu1_async_queue_tsan COMMAND dw5xl_vu1_async_queue_tsan_test)
        set_tests_properties(vu1_async_queue_tsan PROPERTIES TIMEOUT 20 ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1")
    endif()

    # DW5XL-TEST: GS stream recording comparison and offline VIF1 replay (tools/gsreplay).
    set(DW5XL_GSREPLAY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../tools/gsreplay")
    add_executable(dw5xl_gsrec "${DW5XL_GSREPLAY_DIR}/gsrec.cpp")
    target_include_directories(dw5xl_gsrec PRIVATE "${PS2RECOMP_DIR}/ps2xRuntime/src/lib" "${PS2RECOMP_DIR}/ps2xRuntime/include")
    add_executable(dw5xl_gsrender "${DW5XL_GSREPLAY_DIR}/gsrender.cpp")
    target_link_libraries(dw5xl_gsrender PRIVATE ps2_runtime)
    target_include_directories(dw5xl_gsrender PRIVATE "${PS2RECOMP_DIR}/ps2xRuntime/src/lib" "${PS2RECOMP_DIR}/ps2xRuntime/include")
    add_executable(dw5xl_vif1_replay "${DW5XL_GSREPLAY_DIR}/vif1_replay.cpp")
    target_link_libraries(dw5xl_vif1_replay PRIVATE ps2_runtime)
    target_include_directories(dw5xl_vif1_replay PRIVATE "${PS2RECOMP_DIR}/ps2xRuntime/src/lib" "${DW5XL_GSREPLAY_DIR}")
    target_compile_options(dw5xl_vif1_replay PRIVATE ${DW5XL_ARCH_FLAGS})
    if(NOT WIN32) # drives the tools through a POSIX shell (env -u, quoting, wait status)
    add_executable(dw5xl_gsreplay_test tests/gsreplay_test.cpp)
    target_link_libraries(dw5xl_gsreplay_test PRIVATE ps2_runtime)
    target_include_directories(dw5xl_gsreplay_test PRIVATE "${PS2RECOMP_DIR}/ps2xRuntime/src/lib" "${DW5XL_GSREPLAY_DIR}")
    target_compile_options(dw5xl_gsreplay_test PRIVATE ${DW5XL_ARCH_FLAGS})
    add_test(NAME gsreplay_synthetic COMMAND dw5xl_gsreplay_test $<TARGET_FILE:dw5xl_vif1_replay>
             $<TARGET_FILE:dw5xl_gsrec> "${CMAKE_CURRENT_BINARY_DIR}/gsreplay_test")
    endif()

    if(EXISTS "${DW5XL_VU1_LIFTED_SRC}")
        add_executable(dw5xl_vu1_lift_test tests/vu1_lift_test.cpp "${DW5XL_VU1_LIFTED_SRC}")
        target_link_libraries(dw5xl_vu1_lift_test PRIVATE ps2_runtime)
        target_include_directories(dw5xl_vu1_lift_test PRIVATE "${PS2RECOMP_DIR}/ps2xRuntime/src/lib" "${PS2RECOMP_DIR}/ps2xRuntime/include")
        target_compile_options(dw5xl_vu1_lift_test PRIVATE ${DW5XL_ARCH_FLAGS} ${DW5XL_STRICT_FP_FLAGS})
        add_test(NAME vu1_lift COMMAND dw5xl_vu1_lift_test)
        add_test(NAME vu1_dma_reset_interpreter COMMAND dw5xl_vu1_lift_test dma-cancel-interpreter)
        add_test(NAME vu1_dma_reset_lift COMMAND dw5xl_vu1_lift_test dma-cancel-lift)
        set_tests_properties(vu1_dma_reset_interpreter vu1_dma_reset_lift PROPERTIES TIMEOUT 20)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../notes/issues/018-vu1-verify-cycle-mismatch/evidence/mm12.0.state")
            add_test(NAME vu1_lift_replay_mm12 COMMAND dw5xl_vu1_lift_test replaycap "${CMAKE_CURRENT_SOURCE_DIR}/../notes/issues/018-vu1-verify-cycle-mismatch/evidence/mm12.0")
            set_tests_properties(vu1_lift_replay_mm12 PROPERTIES TIMEOUT 20)
        endif()
    endif()
endif()

# Windows: /bigobj, stack reserve, FFmpeg DLL staging (no-op on other platforms).
get_property(DW5XL_TARGETS DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(dw5xl_target IN LISTS DW5XL_TARGETS)
    dw5xl_windows_target_options(${dw5xl_target})
endforeach()
