include(CheckIPOSupported)

check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_ERROR)

# Upstream default. Set OFF to keep MSVC/clang-cl release floating point like
# the Linux build (no /fp:fast, baseline x86-64 code generation).
option(PS2X_MSVC_FAST_FLOAT "MSVC release builds of the runtime use /fp:fast and /arch:AVX2" ON)

function(EnableFastReleaseMode TargetName)
    message("> Enabling optimization for: ${TargetName}")
    if(MSVC)
        set(_ps2x_msvc_release_options
            /O2 # speed
            /Ob2 # inline aggressively
            /Oi # intrinsics
            /GL # whole program opt
            /Gy # function-level linking
            /Gw # global data in COMDAT
            /GF # string pooling
            /Zc:inline # remove unreferenced inline
            /DNDEBUG
            /GS- # Disable Buffer Security Check (faster)
        )
        if(PS2X_MSVC_FAST_FLOAT)
            list(APPEND _ps2x_msvc_release_options
                /fp:fast # fast math (graphics friendly)
                /arch:AVX2 # Advanced Vector Extensions 2
            )
        endif()
        if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
            # clang-cl rejects /Qspectre- (it treats it as an input file).
            list(APPEND _ps2x_msvc_release_options /Qspectre-) # Disable Spectre mitigations (faster)
        endif()
        if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            # clang-cl ignores /GL with a warning; CMake IPO below selects ThinLTO.
            list(REMOVE_ITEM _ps2x_msvc_release_options /GL)
        endif()
        target_compile_options(${TargetName} PRIVATE
            "$<$<CONFIG:Release>:${_ps2x_msvc_release_options}>"
        )

        if(TARGET ${TargetName})
            target_link_options(${TargetName} PRIVATE
                $<$<CONFIG:Release>:
                    /LTCG # link-time code generation
                    /OPT:REF # remove unreferenced
                    /OPT:ICF # fold identical COMDATs
                >
            )
        endif()
    endif()

    if(IPO_SUPPORTED)
        set_property(TARGET ${TargetName} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    else()
        message(WARNING "Interprocedural optimization not supported: ${ipo_error}")
    endif()
endfunction()