# Stable archive filenames need content identity; fresh extracted dates invalidate actual consumers.
function(ps2x_local_archive_options archive result)
    set(options DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
    if(EXISTS "${archive}" AND NOT IS_DIRECTORY "${archive}")
        file(SHA256 "${archive}" digest)
        list(APPEND options URL_HASH "SHA256=${digest}")
    endif()
    set(${result} "${options}" PARENT_SCOPE)
endfunction()
