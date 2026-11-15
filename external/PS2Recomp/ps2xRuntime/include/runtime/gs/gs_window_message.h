// DW5XL (issue 049, amendment v3.12 step 7; owner 2026-11-15): the game has one window, the one the renderer
// draws itself. Where it cannot be opened the game says why and ends; there is no second window to fall back on.
#pragma once

#include <string>

namespace ps2x::gs
{
    // What the player is told. `why`: the reason as the window gave it (GameWindow::open).
    inline std::string windowNotOpenedMessage(const std::string &why)
    {
        return "The game's window could not be opened: " + (why.empty() ? std::string("no reason was given") : why) +
               ".\nThe game draws with Vulkan. It needs a graphics card and a graphics driver with Vulkan support; "
               "installing the current driver for the graphics card usually provides it.";
    }
} // namespace ps2x::gs
