// DW5XL (issue 049, amendment v3.12 step 7; owner 2026-11-15): the game has one window, the one the renderer
// draws itself. Where it cannot be opened the game says why and ends; there is no second window to fall back on.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace ps2x::gs
{
    // Issue 049, amendment v3.13: the record of a wait for the graphics driver that ran into its limit. One line
    // with what is needed to find out why the driver gave no answer (the freeze of 2026-11-15 left none of it).
    struct NoAnswerFacts
    {
        int wait = 0;            // GameWindowNoAnswer::FreeImage (1) or ShownPicture (2)
        uint64_t present = 0u;   // the present it was about, counted from 1 on its swap chain
        uint64_t swapChain = 0u; // which of the window's swap chains
        double waitedMs = 0.0, limitMs = 0.0;
        bool renewed = false;    // a new swap chain is made because of it
        bool vsync = false;
        int width = 0, height = 0; // the window's content, in pixels
        int kind = 0;            // 0: a window, 1: maximised, 2: borderless over the whole screen, 3: hidden
        int screen = -1;         // what the system last said of the screen: 0 off, 1 on, 2 dimmed; -1: nothing yet
        bool reports = false;    // the driver's reports of shown pictures are asked for
        uint64_t field = 0u;     // the game's field
        bool paused = false;     // the port's MENU or one of its pages holds the game
        double ranSeconds = 0.0; // since the window's loop began
    };

    inline std::string noAnswerRecord(const NoAnswerFacts &f)
    {
        static const char *const kKinds[] = {"a window", "maximised", "borderless", "hidden"};
        static const char *const kScreens[] = {"off", "on", "dimmed"};
        char text[640];
        char what[96];
        if (f.wait == 1)
            std::snprintf(what, sizeof(what), "a free swap chain image (for present %llu)", static_cast<unsigned long long>(f.present));
        else
            std::snprintf(what, sizeof(what), "the answer that present %llu was shown", static_cast<unsigned long long>(f.present));
        std::snprintf(text, sizeof(text),
                      "[window] no answer from the graphics driver within %.2f s (waited %.0f ms) for %s, swap chain %llu; "
                      "vsync %s; window %dx%d, %s; screen %s; the driver's reports %s; game field %llu%s, running %.0f s; %s",
                      f.limitMs / 1000.0, f.waitedMs, what, static_cast<unsigned long long>(f.swapChain), f.vsync ? "on" : "off",
                      f.width, f.height, kKinds[f.kind >= 0 && f.kind <= 3 ? f.kind : 0],
                      f.screen >= 0 && f.screen <= 2 ? kScreens[f.screen] : "not known", f.reports ? "on" : "off",
                      static_cast<unsigned long long>(f.field), f.paused ? " (paused)" : "", f.ranSeconds,
                      f.renewed ? "the swap chain is made anew" : "no new swap chain (one was made for the wait before)");
        return text;
    }

    // What the player is told. `why`: the reason as the window gave it (GameWindow::open).
    inline std::string windowNotOpenedMessage(const std::string &why)
    {
        return "The game's window could not be opened: " + (why.empty() ? std::string("no reason was given") : why) +
               ".\nThe game draws with Vulkan. It needs a graphics card and a graphics driver with Vulkan support; "
               "installing the current driver for the graphics card usually provides it.";
    }
} // namespace ps2x::gs
