# Writes OUT (a header defining DW5XL_BUILD_ID) from `git describe --always --dirty` in SRC, or, in a release package
# (no Git on the player's PC, issue 011), from the BUILD_ID file the package carries.
# Runs on every build; the file is only rewritten when the ID changes (issue 012).
set(id "")
if(EXISTS "${SRC}/BUILD_ID")
    file(STRINGS "${SRC}/BUILD_ID" id LIMIT_COUNT 1)
else()
    execute_process(COMMAND git describe --always --dirty
                    WORKING_DIRECTORY "${SRC}"
                    OUTPUT_VARIABLE id OUTPUT_STRIP_TRAILING_WHITESPACE
                    RESULT_VARIABLE rc ERROR_QUIET)
    if(NOT rc EQUAL 0)
        set(id "")
    endif()
endif()
if(id STREQUAL "")
    set(id "unknown")
endif()
set(content "#pragma once\n#define DW5XL_BUILD_ID \"${id}\"\n")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL content)
    file(WRITE "${OUT}" "${content}")
endif()
