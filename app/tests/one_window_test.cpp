// Issue 049, amendment v3.12 step 7 (owner 2026-11-15): the game has one window, the one the renderer draws
// itself. This test reads the sources: the runtime no longer holds the raylib/OpenGL window (its switch, its
// loop's drawing calls, the OpenGL upload, the X11 presentation log), the build refuses to be made without the
// Vulkan renderer, and a PC on which the window cannot be opened gets a message that names what is missing.
#include "runtime/gs/gs_window_message.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {
int failures = 0;
void check(bool ok, const std::string &what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    failures += ok ? 0 : 1;
}

bool readFile(const std::string &path, std::string &out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream text;
    text << in.rdbuf();
    out = text.str();
    return true;
}
} // namespace

int main()
{
    const std::string runtimeDir = std::string(DW5XL_SOURCE_DIR) + "/external/PS2Recomp/ps2xRuntime/";

    // CI's Windows test job runs the programs without the source tree at the path they were built from (they are
    // cross-built on Linux): then the checks that read sources are skipped, as in hotkeys_test. The Linux job and
    // every local build run them.
    std::string runtime, readme;
    if (!readFile(std::string(DW5XL_SOURCE_DIR) + "/README.md", readme))
        std::printf("SKIP  the sources are not available here: the checks that read them are not made\n");
    else
    {
    check(readFile(runtimeDir + "src/lib/ps2_runtime.cpp", runtime) && runtime.size() > 100000u, "the runtime's source is read");
    // The switch between two windows, and what only the raylib window used.
    const char *const gone[] = {
        "getenv(\"PS2X_WINDOW\")",   // the switch
        "getenv(\"PS2X_HOST_FPS\")", // the raylib window's own frame loop
        "InitWindow(", "WindowShouldClose(", "BeginDrawing(", "EndDrawing(", "SetTargetFPS(",
        "LoadTextureFromImage(", "UpdateTexture(", "DrawTexturePro(", // the OpenGL upload and draw
        "LoadShaderFromMemory(",                                     // SHARPEN's OpenGL shader
        "LoadImageFromScreen(",                                      // window pictures read from OpenGL
        "present_feedback",                                          // the X11 presentation log
    };
    for (const char *call : gone)
        check(runtime.find(call) == std::string::npos, std::string("the runtime no longer has ") + call);

    std::string unused;
    check(!readFile(runtimeDir + "src/lib/ps2_present_feedback.cpp", unused), "the X11 presentation log's source is gone");

    // The game's build, not the runtime's own: the PS2Recomp tree is also built by itself without the renderer
    // (its recompiler and its tests), and there the runtime simply has no window.
    std::string cmake;
    check(readFile(std::string(DW5XL_SOURCE_DIR) + "/app/CMakeLists.txt", cmake) &&
              cmake.find("message(FATAL_ERROR \"The game's window is drawn by the Vulkan renderer") != std::string::npos,
          "the game's build refuses to be made without the Vulkan renderer (app/CMakeLists.txt)");
    }

    // The message of a PC on which the window cannot be opened: the reason as the window gave it, and what to do.
    const std::string message = ps2x::gs::windowNotOpenedMessage("no Vulkan driver was found");
    check(message.find("no Vulkan driver was found") != std::string::npos, "the message names what is missing");
    check(message.find("Vulkan") != std::string::npos && message.find("graphics driver") != std::string::npos,
          "the message says what the game needs (a graphics driver with Vulkan)");
    check(message.find("today's window") == std::string::npos && message.find("OpenGL") == std::string::npos,
          "the message promises no second window");

    std::printf("%s: %d failures\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
