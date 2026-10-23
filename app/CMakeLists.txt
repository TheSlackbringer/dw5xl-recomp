# English PAL DW5 Xtreme Legends (SLES_538.60), built on PS2Recomp's runtime.
# Generated game code stays local under ../recomp/dw5xl/output.
cmake_minimum_required(VERSION 3.20)
project(dw5xl LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Dw5xlPlatform.cmake")

set(PS2RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../external/PS2Recomp")
# Generated game code (game-derived, never in git): recomp/ in a developer tree; the player data folder when the
# release's launcher builds the game (issue 011 v5).
set(DW5XL_RECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../recomp" CACHE PATH "Folder holding the generated game code")
set(DW5XL_GENERATED_DIR "${DW5XL_RECOMP_ROOT}/dw5xl/output")

# Only the runtime (and the IOP module it needs) from PS2Recomp.
set(PS2X_BUILD_RECOMP OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_RUNTIME ON CACHE BOOL "" FORCE)
set(PS2X_BUILD_ANALYZER OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_STUDIO OFF CACHE BOOL "" FORCE)
# MSVC/clang-cl only: keep the runtime's floating point like the Linux build.
set(PS2X_MSVC_FAST_FLOAT OFF CACHE BOOL "" FORCE)
# Issue 025 (DW5XL-DEBUG): profiling markers, debug info and frame pointers for OS recorders (WPR, perf). Use a
# separate build directory (app/build-diagnostics); never for timing acceptance or releases.
option(DW5XL_DEVELOPER_DIAGNOSTICS "Developer diagnostics build: profiling markers (issue 025)" OFF)
if(DW5XL_DEVELOPER_DIAGNOSTICS)
    add_compile_options(-g -fno-omit-frame-pointer)
    if(WIN32)
        add_compile_options(-gcodeview)
        add_link_options(-Wl,--pdb=)
    endif()
endif()
add_subdirectory("${PS2RECOMP_DIR}" ps2recomp EXCLUDE_FROM_ALL)
# Runtime stubs also instantiate guest arithmetic helpers.
target_compile_options(ps2_runtime PRIVATE ${DW5XL_STRICT_FP_FLAGS})
if(DW5XL_DEVELOPER_DIAGNOSTICS)
    target_sources(ps2_runtime PRIVATE "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/ps2_profile_markers.cpp")
    target_compile_definitions(ps2_runtime PUBLIC DW5XL_DEVELOPER_DIAGNOSTICS=1)
endif()

# The game itself needs the code generated from the player's own disc (tools/regen.sh). OFF builds only what needs
# no game data: the runtime, the tools and the tests below that use no generated code (CI, issue 011).
option(DW5XL_BUILD_GAME "Build the game (needs locally generated code)" ON)
if(DW5XL_BUILD_GAME) # --- game: generated main program, lifted VU1, overlays ---
file(GLOB DW5XL_GENERATED_SOURCES CONFIGURE_DEPENDS "${DW5XL_GENERATED_DIR}/*.cpp")

add_executable(dw5xl
    ${DW5XL_GENERATED_SOURCES}
    "${PS2RECOMP_DIR}/ps2xRuntime/src/main.cpp"
    "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/ps2_debug_panel.cpp"
    src/dw5xl_pad2.cpp
    src/dw5xl_images.cpp
    src/ipu_idec.cpp
    src/dw5xl_overlays.cpp
    src/dw5xl_install.cpp
    setup/wizard_console.cpp
)
target_include_directories(dw5xl PRIVATE "${DW5XL_GENERATED_DIR}" "${PS2RECOMP_DIR}/ps2xRuntime/src/lib")
# Build ID (git describe --always --dirty) for the startup log and the F8 overlay (issue 012).
set(DW5XL_BUILD_ID_DIR "${CMAKE_CURRENT_BINARY_DIR}/build_id")
add_custom_target(dw5xl_build_id
    COMMAND "${CMAKE_COMMAND}" "-DSRC=${CMAKE_CURRENT_SOURCE_DIR}/.." "-DOUT=${DW5XL_BUILD_ID_DIR}/dw5xl_build_id.h"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_id.cmake"
    BYPRODUCTS "${DW5XL_BUILD_ID_DIR}/dw5xl_build_id.h"
    VERBATIM)
add_dependencies(dw5xl dw5xl_build_id)
target_include_directories(dw5xl PRIVATE "${DW5XL_BUILD_ID_DIR}")
target_link_libraries(dw5xl PRIVATE ps2_runtime rlImGui imgui dw5xl_setup_ui)
target_compile_definitions(dw5xl PRIVATE PS2X_ENABLE_DEBUG_UI=1 DW5XL_INSTALLED_MODE=1)
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
set_source_files_properties(src/dw5xl_pad2.cpp src/dw5xl_images.cpp src/ipu_idec.cpp src/dw5xl_overlays.cpp src/dw5xl_install.cpp setup/wizard_console.cpp PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON SKIP_PRECOMPILE_HEADERS ON)
# Lifted VU1 microprograms are translated from the game's own microcode (tools/vu1_translator.py, run by
# tools/regen.sh) into the ignored recomp/dw5xl/vu1/ folder. Game-derived: never committed. Without it,
# VU1 microcode runs in the interpreter.
set(DW5XL_VU1_LIFTED_SRC "${DW5XL_RECOMP_ROOT}/dw5xl/vu1/dw5xl_vu1_lifted.cpp")
if(EXISTS "${DW5XL_VU1_LIFTED_SRC}")
    target_sources(dw5xl PRIVATE "${DW5XL_VU1_LIFTED_SRC}")
    set_source_files_properties("${DW5XL_VU1_LIFTED_SRC}" PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON SKIP_PRECOMPILE_HEADERS ON)
    set_property(SOURCE "${DW5XL_VU1_LIFTED_SRC}" APPEND PROPERTY COMPILE_OPTIONS ${DW5XL_STRICT_FP_FLAGS})
else()
    message(STATUS "No lifted VU1 code (run tools/regen.sh); VU1 microcode will be interpreted")
endif()
# Lifted VU0 microprograms (issue 025): the programs the game starts with VCALLMS, translated the same way
# (tools/vu0_translator.py) into the ignored recomp/dw5xl/vu0/ folder. Game-derived: never committed. Without it,
# VU0 microprograms run in the interpreter.
set(DW5XL_VU0_LIFTED_SRC "${DW5XL_RECOMP_ROOT}/dw5xl/vu0/dw5xl_vu0_lifted.cpp" CACHE FILEPATH
    "Generated VU0 microprogram translations (game-derived)")
if(EXISTS "${DW5XL_VU0_LIFTED_SRC}")
    target_sources(dw5xl PRIVATE "${DW5XL_VU0_LIFTED_SRC}")
    set_source_files_properties("${DW5XL_VU0_LIFTED_SRC}" PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON SKIP_PRECOMPILE_HEADERS ON)
    set_property(SOURCE "${DW5XL_VU0_LIFTED_SRC}" APPEND PROPERTY COMPILE_OPTIONS ${DW5XL_STRICT_FP_FLAGS})
else()
    message(STATUS "No lifted VU0 code (run tools/vu0_translator.py); VU0 microprograms will be interpreted")
endif()
# Generated code uses SSE4.1/AVX intrinsics (PS2Recomp only enables them in its own directory).
target_compile_options(dw5xl PRIVATE ${DW5XL_ARCH_FLAGS} ${DW5XL_STRICT_FP_FLAGS})

# Overlay headers/symbols are isolated from the base executable's generated code.
set(DW5XL_OVL00_DIR "${DW5XL_RECOMP_ROOT}/dw5xl/overlays/ovl00/output")
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
set(DW5XL_OVL01_DIR "${DW5XL_RECOMP_ROOT}/dw5xl/overlays/ovl01/output")
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

set(DW5XL_OVL02_DIR "${DW5XL_RECOMP_ROOT}/dw5xl/overlays/ovl02/output")
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
endif() # DW5XL_BUILD_GAME

# Setup wizard core (issue 011): reads the player's disc image, checks and extracts it. Needs no game data.
find_package(Threads REQUIRED)
add_library(dw5xl_setup_core STATIC setup/iso9660.cpp setup/disc_setup.cpp setup/run_process.cpp setup/codegen.cpp
    setup/build_game.cpp setup/install_state.cpp setup/setup_layout.cpp setup/setup_pipeline.cpp setup/vu1lift/vu1_timing.cpp setup/vu1lift/vu1_translator.cpp)
target_include_directories(dw5xl_setup_core PUBLIC setup)
target_link_libraries(dw5xl_setup_core PUBLIC ffmpeg Threads::Threads)
# VU1 lifter (port of tools/vu1_translator.py, disc mode): dw5xl_vu1lift <SLES_538.60> <output.cpp>
add_executable(dw5xl_vu1lift setup/vu1lift/vu1lift_main.cpp)
target_link_libraries(dw5xl_vu1lift PRIVATE dw5xl_setup_core)
# The wizard's window (raylib + imgui) and no-window run, shared by dw5xl_setup and the game (installed mode).
add_library(dw5xl_setup_ui STATIC setup/wizard_ui.cpp setup/file_dialog.cpp)
# raylib before dw5xl_setup_core: the core pulls in user32 (via FFmpeg), whose CloseWindow must not win over raylib's.
target_link_libraries(dw5xl_setup_ui PUBLIC rlImGui imgui raylib dw5xl_setup_core)
if(WIN32)
    target_link_libraries(dw5xl_setup_ui PUBLIC comdlg32)
endif()
# Developer path: build the game from your own disc (window or --no-window).
add_executable(dw5xl_setup WIN32 setup/wizard_main.cpp setup/wizard_console.cpp)
target_link_libraries(dw5xl_setup PRIVATE dw5xl_setup_ui)
dw5xl_windows_target_options(dw5xl_setup)

option(DW5XL_BUILD_TESTS "Build runtime and locally generated helper tests" OFF)
if(DW5XL_BUILD_TESTS)
    enable_testing()
    add_executable(dw5xl_setup_disc_test tests/setup_disc_test.cpp)
    target_link_libraries(dw5xl_setup_disc_test PRIVATE dw5xl_setup_core)
    add_test(NAME setup_disc COMMAND dw5xl_setup_disc_test)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../game/dw5xl/SLES_538.60" AND NOT CMAKE_CROSSCOMPILING)
        # Needs the player's own executable (local only, never CI): C++ lifter == tools/vu1_translator.py.
        add_test(NAME vu1lift_matches_python COMMAND "${CMAKE_COMMAND}" -DLIFTER=$<TARGET_FILE:dw5xl_vu1lift>
                 -DPYTHON=${Python3_EXECUTABLE} "-DREPO=${CMAKE_CURRENT_SOURCE_DIR}/.."
                 "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/vu1lift-compare" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CompareVu1Lift.cmake")
        set_tests_properties(vu1lift_matches_python PROPERTIES TIMEOUT 300)
        # Issue 025: the same for the VU0 microprograms (C++ --vu0 == tools/vu0_translator.py).
        add_test(NAME vu0lift_matches_python COMMAND "${CMAKE_COMMAND}" -DLIFTER=$<TARGET_FILE:dw5xl_vu1lift>
                 -DPYTHON=${Python3_EXECUTABLE} "-DREPO=${CMAKE_CURRENT_SOURCE_DIR}/.." -DUNIT=vu0
                 "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/vu0lift-compare" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CompareVu1Lift.cmake")
        set_tests_properties(vu0lift_matches_python PROPERTIES TIMEOUT 120)
    endif()
    add_executable(dw5xl_setup_build_test tests/setup_build_test.cpp)
    target_link_libraries(dw5xl_setup_build_test PRIVATE dw5xl_setup_core)
    add_test(NAME setup_build COMMAND dw5xl_setup_build_test)
    add_executable(dw5xl_setup_codegen_test tests/setup_codegen_test.cpp)
    target_link_libraries(dw5xl_setup_codegen_test PRIVATE dw5xl_setup_core)
    add_test(NAME setup_codegen COMMAND dw5xl_setup_codegen_test)
    add_executable(dw5xl_setup_pipeline_test tests/setup_pipeline_test.cpp)
    target_link_libraries(dw5xl_setup_pipeline_test PRIVATE dw5xl_setup_core)
    add_test(NAME setup_pipeline COMMAND dw5xl_setup_pipeline_test)
    add_executable(dw5xl_setup_install_test tests/setup_install_test.cpp)
    target_link_libraries(dw5xl_setup_install_test PRIVATE dw5xl_setup_core)
    add_test(NAME setup_install COMMAND dw5xl_setup_install_test)
    # Function-key layout (owner 2026-10-20), unique keys, README key table, handlers read the table.
    add_executable(dw5xl_hotkeys_test tests/hotkeys_test.cpp)
    target_link_libraries(dw5xl_hotkeys_test PRIVATE ps2_runtime)
    target_compile_definitions(dw5xl_hotkeys_test PRIVATE DW5XL_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
    add_test(NAME hotkeys COMMAND dw5xl_hotkeys_test)
    # Save-state archive (issue 027): save -> load -> save identity, refusal of damaged files.
    add_executable(dw5xl_state_archive_test tests/state_archive_test.cpp)
    target_link_libraries(dw5xl_state_archive_test PRIVATE ps2_runtime)
    add_test(NAME state_archive COMMAND dw5xl_state_archive_test)
    add_executable(dw5xl_overlay_dispatch_test tests/overlay_dispatch_test.cpp)
    target_link_libraries(dw5xl_overlay_dispatch_test PRIVATE ps2_runtime)
    target_compile_options(dw5xl_overlay_dispatch_test PRIVATE ${DW5XL_ARCH_FLAGS})
    add_test(NAME overlay_dispatch COMMAND dw5xl_overlay_dispatch_test)
    add_executable(dw5xl_field_history_test tests/field_history_test.cpp)
    target_include_directories(dw5xl_field_history_test PRIVATE "${PS2RECOMP_DIR}/ps2xRuntime/include")
    add_test(NAME field_history COMMAND dw5xl_field_history_test)
    # Issue 025 profiling markers (a debugging tool, owner 2026-10-22: not part of CI or normal test runs): built and
    # run only in diagnostic builds. The first test compiles them in itself; the _off test checks that a build without
    # DW5XL_DEVELOPER_DIAGNOSTICS only gets empty inline functions.
    if(DW5XL_DEVELOPER_DIAGNOSTICS)
    add_executable(dw5xl_profile_markers_test tests/profile_markers_test.cpp
        "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/ps2_profile_markers.cpp")
    target_include_directories(dw5xl_profile_markers_test PRIVATE "${PS2RECOMP_DIR}/ps2xRuntime/include")
    target_compile_definitions(dw5xl_profile_markers_test PRIVATE DW5XL_DEVELOPER_DIAGNOSTICS=1)
    target_link_libraries(dw5xl_profile_markers_test PRIVATE Threads::Threads)
    add_test(NAME profile_markers COMMAND dw5xl_profile_markers_test)
    add_executable(dw5xl_profile_markers_off_test tests/profile_markers_off_test.cpp)
    target_include_directories(dw5xl_profile_markers_off_test PRIVATE "${PS2RECOMP_DIR}/ps2xRuntime/include")
    add_test(NAME profile_markers_off COMMAND dw5xl_profile_markers_off_test)
    endif()
    add_executable(dw5xl_cd_iop_read_test tests/cd_iop_read_test.cpp)
    target_link_libraries(dw5xl_cd_iop_read_test PRIVATE ps2_runtime)
    target_compile_options(dw5xl_cd_iop_read_test PRIVATE ${DW5XL_ARCH_FLAGS})
    add_test(NAME cd_iop_read COMMAND dw5xl_cd_iop_read_test)
    add_executable(dw5xl_cd_stream_rate_test tests/cd_stream_rate_test.cpp)
    target_link_libraries(dw5xl_cd_stream_rate_test PRIVATE ps2_runtime)
    target_compile_options(dw5xl_cd_stream_rate_test PRIVATE ${DW5XL_ARCH_FLAGS})
    add_test(NAME cd_stream_rate COMMAND dw5xl_cd_stream_rate_test)
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
    if(DW5XL_BUILD_GAME) # these two compile generated game functions
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
    endif()
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
    add_executable(dw5xl_dma_chain_snapshot_test tests/dma_chain_snapshot_test.cpp)
    target_link_libraries(dw5xl_dma_chain_snapshot_test PRIVATE ps2_runtime)
    target_compile_options(dw5xl_dma_chain_snapshot_test PRIVATE ${DW5XL_ARCH_FLAGS})
    add_test(NAME dma_chain_snapshot COMMAND dw5xl_dma_chain_snapshot_test)
    set_tests_properties(dma_chain_snapshot PROPERTIES TIMEOUT 20)
    add_executable(dw5xl_vu1_rounding_scope_test tests/vu1_rounding_scope_test.cpp)
    target_link_libraries(dw5xl_vu1_rounding_scope_test PRIVATE ps2_runtime)
    target_compile_options(dw5xl_vu1_rounding_scope_test PRIVATE ${DW5XL_ARCH_FLAGS} ${DW5XL_STRICT_FP_FLAGS})
    add_test(NAME vu1_rounding_scope COMMAND dw5xl_vu1_rounding_scope_test)
    add_executable(dw5xl_iop_ascii_compare_test tests/iop_ascii_compare_test.cpp)
    target_link_libraries(dw5xl_iop_ascii_compare_test PRIVATE ps2_iop)
    add_test(NAME iop_ascii_compare COMMAND dw5xl_iop_ascii_compare_test)
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
