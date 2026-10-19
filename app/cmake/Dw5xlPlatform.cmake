# Compiler and platform options shared by the dw5xl targets.
# With GCC-style compilers (the Linux build) these expand to exactly the flags
# the targets used before Windows support: -march=x86-64-v3 and, for lifted VU1
# code, -frounding-math -ffp-contract=off.

if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # clang-cl (recommended on Windows): same LLVM code generation as Linux.
    # Plain -f options are ignored by clang-cl, so pass them through /clang:.
    set(DW5XL_ARCH_FLAGS -march=x86-64-v3)
    set(DW5XL_STRICT_FP_FLAGS /clang:-frounding-math /clang:-ffp-contract=off)
elseif(MSVC)
    # cl.exe: nearest equivalents. Not bit-identical with the clang builds.
    set(DW5XL_ARCH_FLAGS /arch:AVX2)
    set(DW5XL_STRICT_FP_FLAGS /fp:strict)
else()
    set(DW5XL_ARCH_FLAGS -march=x86-64-v3)
    set(DW5XL_STRICT_FP_FLAGS -frounding-math -ffp-contract=off)
endif()

# Windows-only target settings; a no-op elsewhere.
function(dw5xl_windows_target_options target)
    if(NOT WIN32)
        return()
    endif()
    get_target_property(target_type ${target} TYPE)
    if(MSVC)
        # Unity batches of generated code exceed the default COFF section limit.
        target_compile_options(${target} PRIVATE /bigobj)
        if(target_type STREQUAL "EXECUTABLE")
            # Windows reserves 1 MiB of stack per thread by default (Linux: 8 MiB);
            # std::thread uses the executable's reserve. Same values as ps2EntryRunner.
            target_link_options(${target} PRIVATE /STACK:16777216 /FORCE:MULTIPLE /MANIFEST:NO)
        endif()
    elseif(target_type STREQUAL "EXECUTABLE") # MinGW (llvm-mingw): same stack reserve
        target_link_options(${target} PRIVATE -Wl,--stack,16777216)
    endif()
    if(target_type STREQUAL "EXECUTABLE")
        # UTF-8 process code page (Windows 10 1903+): argv and narrow file APIs take UTF-8 paths, so folders with
        # non-ASCII characters work (issue 011). MSVC: /MANIFEST:NO above, this resource is the manifest.
        target_sources(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../tools/windows/utf8.rc")
    endif()
    if(target_type STREQUAL "EXECUTABLE" AND COMMAND ps2x_stage_ffmpeg_runtime_dlls)
        # Copy the FFmpeg DLLs next to the executable.
        ps2x_stage_ffmpeg_runtime_dlls(${target})
    endif()
endfunction()
