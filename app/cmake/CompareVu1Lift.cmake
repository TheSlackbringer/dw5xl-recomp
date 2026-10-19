# CTest vu1lift_matches_python (issue 011): the C++ VU1 lifter must write exactly what tools/vu1_translator.py
# writes for the player's own executable. Needs the local game files; the outputs stay in the build folder.
# -DLIFTER=<dw5xl_vu1lift> -DPYTHON=<python3> -DREPO=<repo root> -DOUT=<folder>
file(MAKE_DIRECTORY "${OUT}")
execute_process(COMMAND "${LIFTER}" "${REPO}/game/dw5xl/SLES_538.60" "${OUT}/cpp_lifted.cpp"
                RESULT_VARIABLE cpp_rc OUTPUT_QUIET)
if(NOT cpp_rc EQUAL 0)
    message(FATAL_ERROR "dw5xl_vu1lift failed (${cpp_rc})")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "DW5XL_VU1_LIFTED_OUT=${OUT}/py_lifted.cpp"
                        "${PYTHON}" "${REPO}/tools/vu1_translator.py"
                WORKING_DIRECTORY "${REPO}" RESULT_VARIABLE py_rc OUTPUT_QUIET)
if(NOT py_rc EQUAL 0)
    message(FATAL_ERROR "tools/vu1_translator.py failed (${py_rc})")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${OUT}/cpp_lifted.cpp" "${OUT}/py_lifted.cpp"
                RESULT_VARIABLE same)
if(NOT same EQUAL 0)
    message(FATAL_ERROR "C++ and Python lifted VU1 code differ (${OUT})")
endif()
file(REMOVE "${OUT}/cpp_lifted.cpp" "${OUT}/py_lifted.cpp")
message(STATUS "C++ and Python lifted VU1 code are byte-identical")
