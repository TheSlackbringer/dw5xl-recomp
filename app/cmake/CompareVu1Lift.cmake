# CTest vu1lift_matches_python (issue 011): the C++ VU1 lifter must write exactly what tools/vu1_translator.py
# writes for the player's own executable. Needs the local game files; the outputs stay in the build folder.
# -DLIFTER=<dw5xl_vu1lift> -DPYTHON=<python3> -DREPO=<repo root> -DOUT=<folder> [-DUNIT=vu0]
# UNIT=vu0 (issue 025, CTest vu0lift_matches_python): the same for the VU0 microprograms and tools/vu0_translator.py.
if(UNIT STREQUAL "vu0")
    set(lifter_args --vu0)
    set(script "${REPO}/tools/vu0_translator.py")
    set(out_var DW5XL_VU0_LIFTED_OUT)
    set(what "VU0")
else()
    set(lifter_args)
    set(script "${REPO}/tools/vu1_translator.py")
    set(out_var DW5XL_VU1_LIFTED_OUT)
    set(what "VU1")
endif()
file(MAKE_DIRECTORY "${OUT}")
execute_process(COMMAND "${LIFTER}" ${lifter_args} "${REPO}/game/dw5xl/SLES_538.60" "${OUT}/cpp_lifted.cpp"
                RESULT_VARIABLE cpp_rc OUTPUT_QUIET)
if(NOT cpp_rc EQUAL 0)
    message(FATAL_ERROR "dw5xl_vu1lift ${lifter_args} failed (${cpp_rc})")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "${out_var}=${OUT}/py_lifted.cpp"
                        "${PYTHON}" "${script}"
                WORKING_DIRECTORY "${REPO}" RESULT_VARIABLE py_rc OUTPUT_QUIET)
if(NOT py_rc EQUAL 0)
    message(FATAL_ERROR "${script} failed (${py_rc})")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${OUT}/cpp_lifted.cpp" "${OUT}/py_lifted.cpp"
                RESULT_VARIABLE same)
if(NOT same EQUAL 0)
    message(FATAL_ERROR "C++ and Python lifted ${what} code differ (${OUT})")
endif()
file(REMOVE "${OUT}/cpp_lifted.cpp" "${OUT}/py_lifted.cpp")
message(STATUS "C++ and Python lifted ${what} code are byte-identical")
