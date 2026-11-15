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

    // Issue 049, amendment v3.13 (criterion G3): the record of a wait for the graphics driver that ran into its
    // limit names everything needed to find out why: which wait, which present of which swap chain, VSYNC, the
    // window's size and kind, whether the screen is on, whether the driver's reports are on, where the game was
    // and how long it has run, and what the window did about it.
    {
        ps2x::gs::NoAnswerFacts f;
        f.wait = 2;
        f.present = 2u;
        f.swapChain = 5u;
        f.waitedMs = 1000.4;
        f.limitMs = 1000.0;
        f.renewed = true;
        f.vsync = true;
        f.width = 2455;
        f.height = 1566;
        f.kind = 1;
        f.screen = 1;
        f.reports = true;
        f.field = 12645u;
        f.paused = true;
        f.ranSeconds = 962.0;
        const std::string line = ps2x::gs::noAnswerRecord(f);
        const auto has = [&line](const char *part) { return line.find(part) != std::string::npos; };
        check(line.rfind("[window] no answer from the graphics driver within 1.00 s (waited 1000 ms)", 0) == 0,
              "the record begins with what happened, the limit and how long was waited");
        check(has("for the answer that present 2 was shown, swap chain 5;"), "it names the wait, the present and the swap chain");
        check(has("vsync on;") && has("window 2455x1566, maximised;"), "VSYNC, the window's size and its kind");
        check(has("screen on;") && has("the driver's reports on;"), "the screen and the driver's reports");
        check(has("game field 12645 (paused), running 962 s;"), "where the game was and how long it has run");
        check(has("the swap chain is made anew") && line.find('\n') == std::string::npos, "what the window did, on one line");
        f.wait = 1;
        f.present = 1u;
        f.renewed = false;
        f.vsync = false;
        f.kind = 2;
        f.screen = 0;
        f.reports = false;
        f.paused = false;
        const std::string other = ps2x::gs::noAnswerRecord(f);
        const auto hasOther = [&other](const char *part) { return other.find(part) != std::string::npos; };
        check(hasOther("for a free swap chain image (for present 1), swap chain 5;") && hasOther("vsync off;") &&
                  hasOther(", borderless;") && hasOther("screen off;") && hasOther("the driver's reports off;") &&
                  hasOther("game field 12645, running") && hasOther("no new swap chain (one was made for the wait before)"),
              "the other wait, a borderless window, a dark screen and a wait in a row are told apart");
        f.screen = -1;
        f.kind = 3;
        check(ps2x::gs::noAnswerRecord(f).find("screen not known;") != std::string::npos &&
                  ps2x::gs::noAnswerRecord(f).find(", hidden;") != std::string::npos,
              "a screen the system has said nothing of yet, and a hidden window");
    }

    std::printf("%s: %d failures\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
