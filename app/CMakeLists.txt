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
include(cmake/vu_sources.cmake)
set(DW5XL_VU1_LIFTED_SRC "${DW5XL_RECOMP_ROOT}/dw5xl/vu1/dw5xl_vu1_lifted.cpp")
dw5xl_vu_sources("${DW5XL_RECOMP_ROOT}/dw5xl/vu1" DW5XL_VU1_LIFTED_SOURCES)

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
include(cmake/generated_sources.cmake)
dw5xl_generated_sources(dw5xl "${DW5XL_GENERATED_DIR}"
    "${PS2RECOMP_DIR}/ps2xRuntime/src/main.cpp;${PS2RECOMP_DIR}/ps2xRuntime/src/lib/ps2_debug_panel.cpp"
    DW5XL_GENERATED_SOURCES DW5XL_BATCHED)

add_executable(dw5xl
    ${DW5XL_GENERATED_SOURCES}
    src/dw5xl_pad2.cpp
    src/dw5xl_images.cpp
    src/dw5xl_allocators.cpp
    src/ipu_idec.cpp
    src/dw5xl_overlays.cpp
    src/dw5xl_install.cpp
    src/dw5xl_load_original.cpp
    src/dw5xl_widescreen.cpp
    src/dw5xl_pc_settings.cpp
    src/dw5xl_main_menu.cpp
    src/dw5xl_frame_probe.cpp
    src/dw5xl_smoothing.cpp
    src/dw5xl_smooth_list.cpp
    src/dw5xl_smooth_time.cpp
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
if(DW5XL_BATCHED)
    set_target_properties(dw5xl PROPERTIES UNITY_BUILD OFF)
endif()
set_source_files_properties(src/dw5xl_allocators.cpp src/dw5xl_pad2.cpp src/dw5xl_images.cpp src/ipu_idec.cpp src/dw5xl_overlays.cpp src/dw5xl_install.cpp src/dw5xl_load_original.cpp src/dw5xl_widescreen.cpp src/dw5xl_pc_settings.cpp src/dw5xl_main_menu.cpp src/dw5xl_frame_probe.cpp src/dw5xl_smoothing.cpp setup/wizard_console.cpp PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON SKIP_PRECOMPILE_HEADERS ON)
# Lifted VU1 microprograms are translated from the game's own microcode (dw5xl_vu1lift, run by the launcher or
# tools/regen.sh) into the ignored recomp/dw5xl/vu1/ folder. Game-derived: never committed. Without it,
# VU1 microcode runs in the interpreter.
set(DW5XL_VU1_LIFTED_SRC "${DW5XL_RECOMP_ROOT}/dw5xl/vu1/dw5xl_vu1_lifted.cpp")
if(EXISTS "${DW5XL_VU1_LIFTED_SRC}")
    target_sources(dw5xl PRIVATE ${DW5XL_VU1_LIFTED_SOURCES})
    set_source_files_properties(${DW5XL_VU1_LIFTED_SOURCES} PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON SKIP_PRECOMPILE_HEADERS ON)
    set_property(SOURCE ${DW5XL_VU1_LIFTED_SOURCES} APPEND PROPERTY COMPILE_OPTIONS ${DW5XL_STRICT_FP_FLAGS})
else()
    message(STATUS "No lifted VU1 code (run tools/regen.sh); VU1 microcode will be interpreted")
endif()
# Lifted VU0 microprograms (issue 025): the programs the game starts with VCALLMS, translated the same way
# (dw5xl_vu1lift --vu0, also run by tools/regen.sh) into the ignored recomp/dw5xl/vu0/ folder. Game-derived: never
# committed. Without it, VU0 microprograms run in the interpreter.
set(DW5XL_VU0_LIFTED_SRC "${DW5XL_RECOMP_ROOT}/dw5xl/vu0/dw5xl_vu0_lifted.cpp" CACHE FILEPATH
    "Generated VU0 microprogram translations (game-derived)")
if(EXISTS "${DW5XL_VU0_LIFTED_SRC}")
    target_sources(dw5xl PRIVATE "${DW5XL_VU0_LIFTED_SRC}")
    set_source_files_properties("${DW5XL_VU0_LIFTED_SRC}" PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON SKIP_PRECOMPILE_HEADERS ON)
    set_property(SOURCE "${DW5XL_VU0_LIFTED_SRC}" APPEND PROPERTY COMPILE_OPTIONS ${DW5XL_STRICT_FP_FLAGS})
else()
    message(STATUS "No lifted VU0 code (run tools/regen.sh); VU0 microprograms will be interpreted")
endif()
# Issue 009 v4.5: the game's IOP modules translated to C++ (dw5xl_ioplift, run by tools/regen.sh) into the ignored
# recomp/dw5xl/iop/ folder. Game-derived: never committed. Without it, IOP code runs in the interpreter.
# The glob is re-checked on every build, so the folder appearing after configure is picked up too.
set(DW5XL_IOP_NATIVE_DIR "${DW5XL_RECOMP_ROOT}/dw5xl/iop")
file(GLOB DW5XL_IOP_NATIVE_SOURCES CONFIGURE_DEPENDS "${DW5XL_IOP_NATIVE_DIR}/*.cpp")
if(DW5XL_IOP_NATIVE_SOURCES)
    target_sources(dw5xl PRIVATE ${DW5XL_IOP_NATIVE_SOURCES})
    set_source_files_properties(${DW5XL_IOP_NATIVE_SOURCES} PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON SKIP_PRECOMPILE_HEADERS ON)
else()
    message(STATUS "No translated IOP modules (run tools/regen.sh); IOP code will be interpreted")
endif()
# Generated code uses SSE4.1/AVX intrinsics (PS2Recomp only enables them in its own directory).
target_compile_options(dw5xl PRIVATE ${DW5XL_ARCH_FLAGS} ${DW5XL_STRICT_FP_FLAGS})

# Overlay headers/symbols are isolated from the base executable's generated code.
set(DW5XL_OVL00_DIR "${DW5XL_RECOMP_ROOT}/dw5xl/overlays/ovl00/output")
dw5xl_generated_sources(dw5xl_ovl00 "${DW5XL_OVL00_DIR}" "" DW5XL_OVL00_SOURCES DW5XL_OVL00_BATCHED)
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
if(DW5XL_OVL00_BATCHED)
    set_target_properties(dw5xl_ovl00 PROPERTIES UNITY_BUILD OFF)
endif()
target_link_libraries(dw5xl PRIVATE dw5xl_ovl00)

# Overlay headers/symbols are isolated from the base executable's generated code.
set(DW5XL_OVL01_DIR "${DW5XL_RECOMP_ROOT}/dw5xl/overlays/ovl01/output")
dw5xl_generated_sources(dw5xl_ovl01 "${DW5XL_OVL01_DIR}" "" DW5XL_OVL01_SOURCES DW5XL_OVL01_BATCHED)
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
if(DW5XL_OVL01_BATCHED)
    set_target_properties(dw5xl_ovl01 PROPERTIES UNITY_BUILD OFF)
endif()
target_link_libraries(dw5xl PRIVATE dw5xl_ovl01)

set(DW5XL_OVL02_DIR "${DW5XL_RECOMP_ROOT}/dw5xl/overlays/ovl02/output")
dw5xl_generated_sources(dw5xl_ovl02 "${DW5XL_OVL02_DIR}" "" DW5XL_OVL02_SOURCES DW5XL_OVL02_BATCHED)
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
if(DW5XL_OVL02_BATCHED)
    set_target_properties(dw5xl_ovl02 PROPERTIES UNITY_BUILD OFF)
endif()
target_link_libraries(dw5xl PRIVATE dw5xl_ovl02)
endif() # DW5XL_BUILD_GAME

# Setup wizard core (issue 011): reads the player's disc image, checks and extracts it. Needs no game data.
find_package(Threads REQUIRED)
add_library(dw5xl_vu_generator OBJECT setup/vu1lift/vu1_timing.cpp setup/vu1lift/vu1_translator.cpp)
target_link_libraries(dw5xl_vu_generator PRIVATE ffmpeg Threads::Threads)
set(DW5XL_SETUP_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/setup-generated")
file(GENERATE OUTPUT "${DW5XL_SETUP_GENERATED}/vu-objects-$<CONFIG>.txt"
    CONTENT "$<JOIN:$<TARGET_OBJECTS:dw5xl_vu_generator>,\n>\n")
add_custom_command(OUTPUT "${DW5XL_SETUP_GENERATED}/vu_producer.h"
    COMMAND "${CMAKE_COMMAND}" "-DOBJECT_LIST=${DW5XL_SETUP_GENERATED}/vu-objects-$<CONFIG>.txt"
        "-DOUT=${DW5XL_SETUP_GENERATED}/vu_producer.h" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/vu_producer.cmake"
    DEPENDS dw5xl_vu_generator $<TARGET_OBJECTS:dw5xl_vu_generator> "${CMAKE_CURRENT_SOURCE_DIR}/cmake/vu_producer.cmake"
    VERBATIM)
add_custom_target(dw5xl_vu_identity DEPENDS "${DW5XL_SETUP_GENERATED}/vu_producer.h")
# Issue 009 v4.5: the IOP module translator (used by dw5xl_ioplift and the setup pipeline).
add_library(dw5xl_iop_translator OBJECT setup/ioplift/iop_translator.cpp setup/ioplift/iop_generate.cpp)
target_include_directories(dw5xl_iop_translator PUBLIC setup/ioplift "${PS2RECOMP_DIR}/ps2xIOP/include"
    PRIVATE "${PS2RECOMP_DIR}/ps2xIOP/src")
target_link_libraries(dw5xl_iop_translator PRIVATE ps2_iop)
add_library(dw5xl_setup_core STATIC setup/iso9660.cpp setup/disc_setup.cpp setup/run_process.cpp setup/codegen.cpp
    setup/build_game.cpp setup/install_state.cpp setup/load_original.cpp setup/setup_layout.cpp setup/setup_pipeline.cpp setup/setup_cache.cpp
    $<TARGET_OBJECTS:dw5xl_vu_generator> $<TARGET_OBJECTS:dw5xl_iop_translator>)
add_dependencies(dw5xl_setup_core dw5xl_vu_identity)
target_include_directories(dw5xl_setup_core PRIVATE "${DW5XL_SETUP_GENERATED}")
target_include_directories(dw5xl_setup_core PUBLIC setup)
target_link_libraries(dw5xl_setup_core PUBLIC ffmpeg Threads::Threads ps2_iop)
# VU1/VU0 translator, the only one (issue 025 Step 6): dw5xl_vu1lift [--vu0] <SLES_538.60> <output.cpp>;
# dw5xl_vu1lift --find-range <image> <pc> <name> [disc files] prints the vu1_inputs.h entries for a new program.
add_executable(dw5xl_vu1lift setup/vu1lift/vu1lift_main.cpp)
target_link_libraries(dw5xl_vu1lift PRIVATE dw5xl_setup_core)
# Issue 009 v4.5: IOP module translator: dw5xl_ioplift <MODULES folder> <output folder> (game-derived output);
# dw5xl_ioplift --raw <code.bin> <base hex> <name> <output folder> for the translator test.
add_executable(dw5xl_ioplift setup/ioplift/ioplift_main.cpp $<TARGET_OBJECTS:dw5xl_iop_translator>)
target_include_directories(dw5xl_ioplift PRIVATE setup/ioplift "${PS2RECOMP_DIR}/ps2xIOP/include")
target_link_libraries(dw5xl_ioplift PRIVATE ps2_iop)
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
    add_executable(dw5xl_setup_cache_test tests/setup_cache_test.cpp)
    target_link_libraries(dw5xl_setup_cache_test PRIVATE dw5xl_setup_core)
    add_test(NAME setup_cache COMMAND dw5xl_setup_cache_test)
    add_executable(dw5xl_setup_sync_test tests/setup_sync_test.cpp)
    target_link_libraries(dw5xl_setup_sync_test PRIVATE dw5xl_setup_core)
    add_test(NAME setup_sync COMMAND dw5xl_setup_sync_test)
    add_executable(dw5xl_setup_vu_layout_test tests/setup_vu_layout_test.cpp)
    target_link_libraries(dw5xl_setup_vu_layout_test PRIVATE dw5xl_setup_core)
    add_test(NAME setup_vu_layout COMMAND dw5xl_setup_vu_layout_test)
    add_executable(dw5xl_setup_timing_test tests/setup_timing_test.cpp)
    target_include_directories(dw5xl_setup_timing_test PRIVATE setup)
    add_test(NAME setup_timing COMMAND dw5xl_setup_timing_test)
    add_executable(dw5xl_setup_measure tests/setup_measure.cpp)
    target_link_libraries(dw5xl_setup_measure PRIVATE dw5xl_setup_core)
    add_executable(dw5xl_setup_disc_test tests/setup_disc_test.cpp)
    target_link_libraries(dw5xl_setup_disc_test PRIVATE dw5xl_setup_core)
    add_test(NAME setup_disc COMMAND dw5xl_setup_disc_test)
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
    add_executable(dw5xl_load_original_test tests/load_original_test.cpp)
    target_link_libraries(dw5xl_load_original_test PRIVATE dw5xl_setup_core)
    add_test(NAME load_original COMMAND dw5xl_load_original_test)
    # Function-key layout (owner 2026-10-20), unique keys, README key table, handlers read the table.
    add_executable(dw5xl_hotkeys_test tests/hotkeys_test.cpp)
    target_link_libraries(dw5xl_hotkeys_test PRIVATE ps2_runtime)
    target_compile_definitions(dw5xl_hotkeys_test PRIVATE DW5XL_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
    add_test(NAME hotkeys COMMAND dw5xl_hotkeys_test)
    # One controller sample per guest field (issue 023 5c): scripts by field, no mid-field changes.
    add_executable(dw5xl_host_input_field_sample_test tests/host_input_field_sample_test.cpp)
    target_link_libraries(dw5xl_host_input_field_sample_test PRIVATE ps2_runtime)
    add_test(NAME host_input_field_sample COMMAND dw5xl_host_input_field_sample_test)
    # Save-state archive (issue 027): save -> load -> save identity, refusal of damaged files.
    add_executable(dw5xl_state_archive_test tests/state_archive_test.cpp)
    target_link_libraries(dw5xl_state_archive_test PRIVATE ps2_runtime)
    add_test(NAME state_archive COMMAND dw5xl_state_archive_test)
    add_executable(dw5xl_allocator_contract_test tests/allocator_contract_test.cpp src/dw5xl_allocators.cpp)
    target_link_libraries(dw5xl_allocator_contract_test PRIVATE ps2_runtime)
    add_test(NAME allocator_contract COMMAND dw5xl_allocator_contract_test)
    add_executable(dw5xl_overlay_dispatch_test tests/overlay_dispatch_test.cpp)
    target_link_libraries(dw5xl_overlay_dispatch_test PRIVATE ps2_runtime)
    target_compile_options(dw5xl_overlay_dispatch_test PRIVATE ${DW5XL_ARCH_FLAGS})
    add_test(NAME overlay_dispatch COMMAND dw5xl_overlay_dispatch_test)
    add_executable(dw5xl_field_history_test tests/field_history_test.cpp)
    target_include_directories(dw5xl_field_history_test PRIVATE "${PS2RECOMP_DIR}/ps2xRuntime/include")
    add_test(NAME field_history COMMAND dw5xl_field_history_test)
    add_executable(dw5xl_menu_assets_test tests/menu_assets_test.cpp)
    add_test(NAME menu_assets COMMAND dw5xl_menu_assets_test)
    add_executable(dw5xl_spu2_render_test tests/spu2_render_test.cpp)
    target_link_libraries(dw5xl_spu2_render_test PRIVATE ps2x::iop)
    add_test(NAME spu2_render COMMAND dw5xl_spu2_render_test)
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
    if(DW5XL_BUILD_GAME) # these tests compile generated game functions
    dw5xl_generated_function("${DW5XL_GENERATED_DIR}" "FUN_00144c70_0x144c70.cpp" DW5XL_POLL_SEMA_SOURCE)
    dw5xl_generated_function("${DW5XL_GENERATED_DIR}" "FUN_00110c78_0x110c78.cpp" DW5XL_GS_HELPER_SOURCE)
    dw5xl_generated_function("${DW5XL_GENERATED_DIR}" "FUN_001111d8_0x1111d8.cpp" DW5XL_GS_HELPER2_SOURCE)
    add_executable(dw5xl_ee_poll_sema_test tests/ee_poll_sema_test.cpp
        "${DW5XL_POLL_SEMA_SOURCE}")
    target_link_libraries(dw5xl_ee_poll_sema_test PRIVATE ps2_runtime)
    target_include_directories(dw5xl_ee_poll_sema_test PRIVATE "${DW5XL_GENERATED_DIR}" "${PS2RECOMP_DIR}/ps2xRuntime/src/lib")
    target_compile_options(dw5xl_ee_poll_sema_test PRIVATE ${DW5XL_ARCH_FLAGS})
    add_test(NAME ee_poll_sema COMMAND dw5xl_ee_poll_sema_test)
    add_executable(dw5xl_original_gs_helper_test tests/original_gs_helper_test.cpp
        "${DW5XL_GS_HELPER_SOURCE}"
        "${DW5XL_GS_HELPER2_SOURCE}")
    target_link_libraries(dw5xl_original_gs_helper_test PRIVATE ps2_runtime)
    target_include_directories(dw5xl_original_gs_helper_test PRIVATE "${DW5XL_GENERATED_DIR}" "${PS2RECOMP_DIR}/ps2xRuntime/src/lib")
    target_compile_options(dw5xl_original_gs_helper_test PRIVATE ${DW5XL_ARCH_FLAGS})
    add_test(NAME original_gs_helper COMMAND dw5xl_original_gs_helper_test)
    dw5xl_generated_function("${DW5XL_GENERATED_DIR}" "main_ovl02_import_001ac6c0_0x1ac6c0.cpp" DW5XL_ORIGINAL_SETTER_SOURCE)
    dw5xl_generated_function("${DW5XL_GENERATED_DIR}" "FUN_001ac6d0_0x1ac6d0.cpp" DW5XL_ORIGINAL_READER_SOURCE)
    add_executable(dw5xl_original_load_original_test tests/original_load_original_test.cpp
        "${DW5XL_ORIGINAL_SETTER_SOURCE}" "${DW5XL_ORIGINAL_READER_SOURCE}")
    target_link_libraries(dw5xl_original_load_original_test PRIVATE ps2_runtime)
    target_include_directories(dw5xl_original_load_original_test PRIVATE "${DW5XL_GENERATED_DIR}" "${PS2RECOMP_DIR}/ps2xRuntime/src/lib")
    target_compile_options(dw5xl_original_load_original_test PRIVATE ${DW5XL_ARCH_FLAGS})
    add_test(NAME original_load_original COMMAND dw5xl_original_load_original_test)
    endif()
    add_executable(dw5xl_gif_squeeze_test tests/gif_squeeze_test.cpp)
    target_include_directories(dw5xl_gif_squeeze_test PRIVATE src)
    add_test(NAME gif_squeeze COMMAND dw5xl_gif_squeeze_test)
    add_executable(dw5xl_widescreen_settings_test tests/widescreen_settings_test.cpp)
    target_link_libraries(dw5xl_widescreen_settings_test PRIVATE ps2_runtime)
    target_include_directories(dw5xl_widescreen_settings_test PRIVATE src)
    target_compile_options(dw5xl_widescreen_settings_test PRIVATE ${DW5XL_ARCH_FLAGS})
    add_test(NAME widescreen_settings COMMAND dw5xl_widescreen_settings_test)
    add_executable(dw5xl_back_button_test tests/back_button_test.cpp)
    target_include_directories(dw5xl_back_button_test PRIVATE ../external/PS2Recomp/ps2xRuntime/include)
    target_compile_options(dw5xl_back_button_test PRIVATE ${DW5XL_ARCH_FLAGS})
    add_test(NAME back_button COMMAND dw5xl_back_button_test)
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
    add_executable(dw5xl_gs_worker_teardown_test tests/gs_worker_teardown_test.cpp)
    target_link_libraries(dw5xl_gs_worker_teardown_test PRIVATE ps2_runtime)
    target_compile_options(dw5xl_gs_worker_teardown_test PRIVATE ${DW5XL_ARCH_FLAGS})
    add_test(NAME gs_worker_teardown COMMAND dw5xl_gs_worker_teardown_test)
    set_tests_properties(gs_worker_teardown PROPERTIES TIMEOUT 20)
    add_executable(dw5xl_vu1_rounding_scope_test tests/vu1_rounding_scope_test.cpp)
    target_link_libraries(dw5xl_vu1_rounding_scope_test PRIVATE ps2_runtime)
    target_compile_options(dw5xl_vu1_rounding_scope_test PRIVATE ${DW5XL_ARCH_FLAGS} ${DW5XL_STRICT_FP_FLAGS})
    add_test(NAME vu1_rounding_scope COMMAND dw5xl_vu1_rounding_scope_test)
    add_executable(dw5xl_vu1_xgkick_tail_test tests/vu1_xgkick_tail_test.cpp)
    target_link_libraries(dw5xl_vu1_xgkick_tail_test PRIVATE ps2_runtime)
    target_compile_options(dw5xl_vu1_xgkick_tail_test PRIVATE ${DW5XL_ARCH_FLAGS})
    add_test(NAME vu1_xgkick_tail COMMAND dw5xl_vu1_xgkick_tail_test)
    # DW5XL-TEST issue 049 v3.7 (P2.6b-1): VIF1 and VU1 as one implementation with more than one instance.
    add_executable(dw5xl_vif1_units_test tests/vif1_units_test.cpp)
    target_link_libraries(dw5xl_vif1_units_test PRIVATE ps2_runtime)
    target_compile_options(dw5xl_vif1_units_test PRIVATE ${DW5XL_ARCH_FLAGS})
    add_test(NAME vif1_units COMMAND dw5xl_vif1_units_test)
    set_tests_properties(vif1_units PROPERTIES TIMEOUT 120)
    # DW5XL-TEST issue 049 v3.7 (P2.6b-2): the pictures' own VIF1/VU1 device.
    add_executable(dw5xl_picture_device_test tests/picture_device_test.cpp)
    target_link_libraries(dw5xl_picture_device_test PRIVATE ps2_runtime)
    target_compile_options(dw5xl_picture_device_test PRIVATE ${DW5XL_ARCH_FLAGS})
    add_test(NAME picture_device COMMAND dw5xl_picture_device_test)
    set_tests_properties(picture_device PROPERTIES TIMEOUT 120)
    add_executable(dw5xl_gif_arbiter_order_test tests/gif_arbiter_order_test.cpp)
    target_link_libraries(dw5xl_gif_arbiter_order_test PRIVATE ps2_runtime)
    add_test(NAME gif_arbiter_order COMMAND dw5xl_gif_arbiter_order_test)
    set_tests_properties(gif_arbiter_order PROPERTIES TIMEOUT 20)
    add_executable(dw5xl_iop_ascii_compare_test tests/iop_ascii_compare_test.cpp)
    target_link_libraries(dw5xl_iop_ascii_compare_test PRIVATE ps2_iop)
    add_test(NAME iop_ascii_compare COMMAND dw5xl_iop_ascii_compare_test)
    # Issue 025 v10 Step 5: a slow log destination must not stall the writing thread.
    add_executable(dw5xl_stdio_pump_test tests/stdio_pump_test.cpp
                   "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/ps2_stdio_pump.cpp")
    target_include_directories(dw5xl_stdio_pump_test PRIVATE "${PS2RECOMP_DIR}/ps2xRuntime/include")
    add_test(NAME stdio_pump COMMAND dw5xl_stdio_pump_test)
    set_tests_properties(stdio_pump PROPERTIES TIMEOUT 20)
    # Issue 023 step 7T: log lines from many threads arrive whole.
    add_executable(dw5xl_log_lines_whole_test tests/log_lines_whole_test.cpp
                   "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/ps2_stdio_pump.cpp")
    target_include_directories(dw5xl_log_lines_whole_test PRIVATE "${PS2RECOMP_DIR}/ps2xRuntime/include")
    add_test(NAME log_lines_whole COMMAND dw5xl_log_lines_whole_test)
    set_tests_properties(log_lines_whole PROPERTIES TIMEOUT 30)
    # Issue 023 criterion 3 (fix 2): the window shows each field once; it gives up waiting only when the game stalls.
    add_executable(dw5xl_presentation_wait_test tests/presentation_wait_test.cpp)
    target_include_directories(dw5xl_presentation_wait_test PRIVATE "${PS2RECOMP_DIR}/ps2xRuntime/include")
    add_test(NAME presentation_wait COMMAND dw5xl_presentation_wait_test)
    set_tests_properties(presentation_wait PROPERTIES TIMEOUT 20)
    # Issue 023 criterion 6: expensive debug lines are computed on a helper thread, in order, and drained at exit.
    add_executable(dw5xl_deferred_log_test tests/deferred_log_test.cpp
                   "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/ps2_stdio_pump.cpp")
    target_include_directories(dw5xl_deferred_log_test PRIVATE "${PS2RECOMP_DIR}/ps2xRuntime/include")
    add_test(NAME deferred_log COMMAND dw5xl_deferred_log_test)
    set_tests_properties(deferred_log PROPERTIES TIMEOUT 20)
    # Issue 009 v4.5: the IOP translator against the interpreter, on our own test program (translated at build time).
    add_executable(dw5xl_ioplift_testgen tests/ioplift_testgen.cpp)
    set(DW5XL_IOPLIFT_TEST_DIR "${CMAKE_CURRENT_BINARY_DIR}/ioplift-test")
    # Issue 054: a build machine of another system (Windows built on Linux: CI's windows-build) cannot run the
    # programs built here; it builds app/host_tools with its own compiler and runs those copies.
    if(CMAKE_CROSSCOMPILING AND NOT CMAKE_HOST_SYSTEM_NAME STREQUAL CMAKE_SYSTEM_NAME)
        include(ExternalProject)
        set(DW5XL_HOST_TOOLS_DIR "${CMAKE_CURRENT_BINARY_DIR}/host-tools")
        set(DW5XL_IOPLIFT_TESTGEN "${DW5XL_HOST_TOOLS_DIR}/dw5xl_ioplift_testgen${CMAKE_HOST_EXECUTABLE_SUFFIX}")
        set(DW5XL_IOPLIFT "${DW5XL_HOST_TOOLS_DIR}/dw5xl_ioplift${CMAKE_HOST_EXECUTABLE_SUFFIX}")
        ExternalProject_Add(dw5xl_host_tools
            SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/host_tools"
            BINARY_DIR "${DW5XL_HOST_TOOLS_DIR}"
            CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release
            BUILD_ALWAYS ON # rebuilt (usually a no-op) on every build, so source changes reach the host copies
            INSTALL_COMMAND ""
            BUILD_BYPRODUCTS "${DW5XL_IOPLIFT_TESTGEN}" "${DW5XL_IOPLIFT}")
        set(DW5XL_IOPLIFT_TOOLS dw5xl_host_tools "${DW5XL_IOPLIFT_TESTGEN}" "${DW5XL_IOPLIFT}")
    else()
        set(DW5XL_IOPLIFT_TESTGEN $<TARGET_FILE:dw5xl_ioplift_testgen>)
        set(DW5XL_IOPLIFT $<TARGET_FILE:dw5xl_ioplift>)
        set(DW5XL_IOPLIFT_TOOLS dw5xl_ioplift_testgen dw5xl_ioplift)
    endif()
    add_custom_command(OUTPUT "${DW5XL_IOPLIFT_TEST_DIR}/iop_IOPTEST.cpp"
        COMMAND "${DW5XL_IOPLIFT_TESTGEN}" "${DW5XL_IOPLIFT_TEST_DIR}/program.bin"
        COMMAND "${DW5XL_IOPLIFT}" --raw "${DW5XL_IOPLIFT_TEST_DIR}/program.bin" 80000 IOPTEST "${DW5XL_IOPLIFT_TEST_DIR}"
        DEPENDS ${DW5XL_IOPLIFT_TOOLS} "${CMAKE_CURRENT_SOURCE_DIR}/tests/ioplift_test_program.h"
        VERBATIM)
    add_executable(dw5xl_ioplift_test tests/ioplift_test.cpp "${DW5XL_IOPLIFT_TEST_DIR}/iop_IOPTEST.cpp")
    target_include_directories(dw5xl_ioplift_test PRIVATE tests "${PS2RECOMP_DIR}/ps2xIOP/src" "${PS2RECOMP_DIR}/ps2xIOP/include")
    target_link_libraries(dw5xl_ioplift_test PRIVATE ps2_iop)
    add_test(NAME ioplift COMMAND dw5xl_ioplift_test)
    set_tests_properties(ioplift PROPERTIES TIMEOUT 120)
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
        # Issue 049 v3.7 (P2.6b-1): two VIF1/VU1 units on two threads share no unguarded data. The VIF1
        # interpreter and the VU interpreter are recompiled instrumented as well.
        file(GLOB DW5XL_VU_SRC CONFIGURE_DEPENDS "${DW5XL_RUNTIME_SRC}/vu/*.cpp")
        add_executable(dw5xl_vif1_units_tsan_test
            tests/vif1_units_test.cpp
            "${DW5XL_RUNTIME_SRC}/ps2_memory.cpp"
            "${DW5XL_RUNTIME_SRC}/ps2_vif1_interpreter.cpp"
            "${DW5XL_RUNTIME_SRC}/gs/ps2_gif_arbiter.cpp"
            ${DW5XL_VU_SRC})
        target_link_libraries(dw5xl_vif1_units_tsan_test PRIVATE ps2_runtime)
        target_compile_options(dw5xl_vif1_units_tsan_test PRIVATE ${DW5XL_ARCH_FLAGS} -fsanitize=thread -g -O1)
        target_link_options(dw5xl_vif1_units_tsan_test PRIVATE -fsanitize=thread)
        add_test(NAME vif1_units_tsan COMMAND dw5xl_vif1_units_tsan_test)
        set_tests_properties(vif1_units_tsan PROPERTIES TIMEOUT 120 ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1")
        # Issue 049 v3.7 (P2.6b-2): the pictures' device is driven from three threads (the game's device, the
        # game's GS side, the game thread) beside its own.
        add_executable(dw5xl_picture_device_tsan_test
            tests/picture_device_test.cpp
            "${DW5XL_RUNTIME_SRC}/ps2_picture_device.cpp"
            "${DW5XL_RUNTIME_SRC}/ps2_memory.cpp"
            "${DW5XL_RUNTIME_SRC}/ps2_vif1_interpreter.cpp"
            "${DW5XL_RUNTIME_SRC}/gs/ps2_gif_arbiter.cpp"
            ${DW5XL_VU_SRC})
        target_link_libraries(dw5xl_picture_device_tsan_test PRIVATE ps2_runtime)
        target_compile_options(dw5xl_picture_device_tsan_test PRIVATE ${DW5XL_ARCH_FLAGS} -fsanitize=thread -g -O1)
        target_link_options(dw5xl_picture_device_tsan_test PRIVATE -fsanitize=thread)
        add_test(NAME picture_device_tsan COMMAND dw5xl_picture_device_tsan_test)
        set_tests_properties(picture_device_tsan PROPERTIES TIMEOUT 120 ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1")
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
    # DW5XL-TEST issue 049 v3.2: the GS rollback point on the Vulkan renderer (skipped without a Vulkan device).
    add_executable(dw5xl_gs_rollback_test tests/gs_rollback_test.cpp)
    target_link_libraries(dw5xl_gs_rollback_test PRIVATE ps2_runtime)
    target_include_directories(dw5xl_gs_rollback_test PRIVATE "${PS2RECOMP_DIR}/ps2xRuntime/src/lib" "${PS2RECOMP_DIR}/ps2xRuntime/include")
    add_test(NAME gs_rollback COMMAND dw5xl_gs_rollback_test)
    # DW5XL-TEST issue 049 (fact-finding): a second GS memory on the same Vulkan device (skipped without a device).
    add_executable(dw5xl_gs_second_memory_test tests/gs_second_memory_test.cpp)
    target_link_libraries(dw5xl_gs_second_memory_test PRIVATE ps2_runtime)
    target_include_directories(dw5xl_gs_second_memory_test PRIVATE "${PS2RECOMP_DIR}/ps2xRuntime/src/lib" "${PS2RECOMP_DIR}/ps2xRuntime/include")
    add_test(NAME gs_second_memory COMMAND dw5xl_gs_second_memory_test)
    # DW5XL-TEST issue 049 v3.7 (P2.6b-3): the two GS memories draw in turns and neither's work ends at a turn
    # (skipped without a Vulkan device).
    add_executable(dw5xl_gs_turns_test tests/gs_turns_test.cpp)
    target_link_libraries(dw5xl_gs_turns_test PRIVATE ps2_runtime)
    target_include_directories(dw5xl_gs_turns_test PRIVATE "${PS2RECOMP_DIR}/ps2xRuntime/src/lib" "${PS2RECOMP_DIR}/ps2xRuntime/include")
    add_test(NAME gs_turns COMMAND dw5xl_gs_turns_test)
    # DW5XL-TEST issue 049 v3.7: the picture GS follows the game's GS through a state load, a change of scale or
    # samples and a direct write (the GS front end's list of written pages; skipped without a Vulkan device).
    add_executable(dw5xl_gs_picture_sync_test tests/gs_picture_sync_test.cpp)
    target_link_libraries(dw5xl_gs_picture_sync_test PRIVATE ps2_runtime)
    target_include_directories(dw5xl_gs_picture_sync_test PRIVATE "${PS2RECOMP_DIR}/ps2xRuntime/src/lib" "${PS2RECOMP_DIR}/ps2xRuntime/include")
    add_test(NAME gs_picture_sync COMMAND dw5xl_gs_picture_sync_test)
    # DW5XL-TEST issue 049 P2.4: a frame's command list as our own data (stream, map, matrix tags); no game data.
    add_executable(dw5xl_smooth_list_test tests/smooth_list_test.cpp src/dw5xl_smooth_list.cpp)
    target_include_directories(dw5xl_smooth_list_test PRIVATE src)
    add_test(NAME smooth_list COMMAND dw5xl_smooth_list_test)
    add_executable(dw5xl_smooth_builder_test tests/smooth_builder_test.cpp src/dw5xl_smooth_list.cpp)
    target_include_directories(dw5xl_smooth_builder_test PRIVATE src)
    add_test(NAME smooth_builder COMMAND dw5xl_smooth_builder_test)
    # DW5XL-TEST issue 049 v3.5: the time line of the in-between pictures and the display's clock; no game data.
    add_executable(dw5xl_smooth_time_test tests/smooth_time_test.cpp src/dw5xl_smooth_time.cpp)
    target_include_directories(dw5xl_smooth_time_test PRIVATE src)
    add_test(NAME smooth_time COMMAND dw5xl_smooth_time_test)
    add_executable(dw5xl_display_clock_test tests/display_clock_test.cpp "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/ps2_display_clock.cpp")
    target_include_directories(dw5xl_display_clock_test PRIVATE "${PS2RECOMP_DIR}/ps2xRuntime/include")
    add_test(NAME display_clock COMMAND dw5xl_display_clock_test)
    set_tests_properties(gs_rollback PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
    set_tests_properties(gs_second_memory PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
    set_tests_properties(gs_picture_sync PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
    set_tests_properties(gs_turns PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
    if(NOT WIN32) # drives the tools through a POSIX shell (env -u, quoting, wait status)
    add_executable(dw5xl_gsreplay_test tests/gsreplay_test.cpp)
    target_link_libraries(dw5xl_gsreplay_test PRIVATE ps2_runtime)
    target_include_directories(dw5xl_gsreplay_test PRIVATE "${PS2RECOMP_DIR}/ps2xRuntime/src/lib" "${DW5XL_GSREPLAY_DIR}")
    target_compile_options(dw5xl_gsreplay_test PRIVATE ${DW5XL_ARCH_FLAGS})
    add_test(NAME gsreplay_synthetic COMMAND dw5xl_gsreplay_test $<TARGET_FILE:dw5xl_vif1_replay>
             $<TARGET_FILE:dw5xl_gsrec> "${CMAKE_CURRENT_BINARY_DIR}/gsreplay_test")
    endif()

    if(EXISTS "${DW5XL_VU1_LIFTED_SRC}")
        add_executable(dw5xl_vu1_lift_test tests/vu1_lift_test.cpp ${DW5XL_VU1_LIFTED_SOURCES})
        target_link_libraries(dw5xl_vu1_lift_test PRIVATE ps2_runtime dw5xl_setup_core)
        target_include_directories(dw5xl_vu1_lift_test PRIVATE "${PS2RECOMP_DIR}/ps2xRuntime/src/lib" "${PS2RECOMP_DIR}/ps2xRuntime/include"
            "${CMAKE_CURRENT_SOURCE_DIR}")
        target_compile_options(dw5xl_vu1_lift_test PRIVATE ${DW5XL_ARCH_FLAGS} ${DW5XL_STRICT_FP_FLAGS})
        add_test(NAME vu1_lift COMMAND dw5xl_vu1_lift_test)
        # Issue 036: the test rebuilds its VU1 images from the player's executable (the one the translated code
        # came from), like the translator; the game folder sits next to the generated code on Linux and Windows.
        set(DW5XL_TEST_GAME_ELF "${DW5XL_RECOMP_ROOT}/../game/dw5xl/SLES_538.60" CACHE FILEPATH
            "Player's PAL executable for the vu1_lift test")
        set_tests_properties(vu1_lift PROPERTIES ENVIRONMENT "DW5XL_TEST_ELF=${DW5XL_TEST_GAME_ELF}")
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
