// A scratch folder name of this test process's own.
// The setup tests used fixed folder names in the system's temp folder and remove the folder when they start. The
// same test started from two build folders at once (two sessions' unit tests, or CTest beside an issue test) then
// deleted each other's files: "source folder is missing" (found 2026-11-11). The given name stays the start of the
// folder's name, since some of them test spaces and non-ASCII characters in paths.
#pragma once

#include <string>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

inline std::string scratchName(const char *name)
{
#if defined(_WIN32)
    const long long pid = _getpid();
#else
    const long long pid = getpid();
#endif
    return std::string(name) + "-" + std::to_string(pid);
}
