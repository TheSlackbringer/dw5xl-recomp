# Writes OUT (a header defining DW5XL_BUILD_ID) from `git describe --always --dirty` in SRC.
# Runs on every build; the file is only rewritten when the ID changes (issue 012).
execute_process(COMMAND git describe --always --dirty
                WORKING_DIRECTORY "${SRC}"
                OUTPUT_VARIABLE id OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE rc ERROR_QUIET)
if(NOT rc EQUAL 0 OR id STREQUAL "")
    set(id "unknown")
endif()
set(content "#pragma once\n#define DW5XL_BUILD_ID \"${id}\"\n")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL content)
    file(WRITE "${OUT}" "${content}")
endif()
