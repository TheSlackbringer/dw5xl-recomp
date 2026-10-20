// The game window's function keys (owner layout, 2026-10-20): F1 debugger, F2 frame rate, F3 anti-aliasing,
// F4 resolution, F5 save state, F8 build ID, F9 load state, F10 Discs. Checks the layout, that no key has two jobs,
// and that README.md's key table lists exactly these keys.
#include "runtime/hotkeys.h"

#include <cstdio>
#include <fstream>
#include <regex>
#include <set>
#include <string>

using namespace ps2x::hotkeys;

namespace {
int failures = 0;
void check(bool ok, const std::string &what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    failures += ok ? 0 : 1;
}
} // namespace

int main()
{
    struct Want { Action action; int key; const char *what; };
    const Want layout[] = {
        {Action::Debugger, KEY_F1, "debugger on F1"},         {Action::FpsOverlay, KEY_F2, "frame rate on F2"},
        {Action::AntiAliasing, KEY_F3, "anti-aliasing on F3"},     {Action::OutputScale, KEY_F4, "resolution on F4"},
        {Action::SaveState, KEY_F5, "save state on F5"},      {Action::BuildOverlay, KEY_F8, "build ID on F8"},
        {Action::LoadState, KEY_F9, "load state on F9"},      {Action::Discs, KEY_F10, "Discs window on F10"},
    };
    for (const Want &w : layout)
    {
        int bindings = 0;
        for (const Binding &b : kBindings)
            bindings += b.action == w.action ? 1 : 0;
        check(bindings == 1 && key(w.action) == w.key, w.what);
    }

    std::set<int> keys;
    for (const Binding &b : kBindings)
        keys.insert(b.key);
    check(keys.size() == std::size(kBindings), "no key has two jobs");

    // README.md: every "| **Fn** |" row of the key table is a bound key, and every bound key has a row.
    // CI's Windows test job runs the programs without the source tree: then these checks are skipped (the Linux
    // job, which has the sources, runs them).
    std::ifstream readme(DW5XL_SOURCE_DIR "/README.md");
    if (!readme.good())
    {
        std::printf("SKIP  README.md and handler sources not available here\n");
        std::printf("%s\n", failures ? "HOTKEY TESTS FAILED" : "ALL HOTKEY TESTS PASSED");
        return failures ? 1 : 0;
    }
    std::set<std::string> documented;
    const std::regex row(R"(^\|\s*\*\*(F[0-9]{1,2})\*\*)");
    for (std::string line; std::getline(readme, line);)
        if (std::smatch m; std::regex_search(line, m, row))
            documented.insert(m[1]);
    std::set<std::string> bound;
    for (const Binding &b : kBindings)
        bound.insert(b.name);
    check(documented == bound, "README.md key table lists exactly the bound keys");

    // The handlers take their keys from the table: no hard-coded IsKeyPressed(KEY_Fn) left.
    for (const char *source : {"/external/PS2Recomp/ps2xRuntime/src/lib/ps2_runtime.cpp",
                               "/external/PS2Recomp/ps2xRuntime/src/lib/ps2_debug_panel.cpp",
                               "/app/src/dw5xl_install.cpp"})
    {
        std::ifstream file(std::string(DW5XL_SOURCE_DIR) + source);
        std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        check(!text.empty(), std::string(source) + " found");
        check(!std::regex_search(text, std::regex(R"(IsKeyPressed\(\s*KEY_F[0-9])")),
              std::string(source) + " reads its function keys from the table");
    }

    std::printf("%s\n", failures ? "HOTKEY TESTS FAILED" : "ALL HOTKEY TESTS PASSED");
    return failures ? 1 : 0;
}
