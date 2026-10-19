// dw5xl_setup is a window program on Windows (no console of its own). With --no-window it prints to the console it
// was started from. Kept apart from wizard_main.cpp: <windows.h> and raylib.h define clashing names.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>

void attachParentConsole()
{
    if (AttachConsole(ATTACH_PARENT_PROCESS))
    {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
    }
}
#else
void attachParentConsole() {}
#endif
