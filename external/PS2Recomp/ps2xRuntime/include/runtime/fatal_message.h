// DW5XL (issue 049, amendment v3.12 step 7): a message for a failure the game cannot start past, for a player
// who may have no console to read the log in.
#pragma once

#include <string>

namespace ps2x
{
    // Shows `text` (UTF-8) in the system's own message box under `title` and returns when the box is closed.
    // Windows so far; elsewhere it does nothing. The caller writes the text to the log itself.
    void showFatalMessage(const std::string &title, const std::string &text);
} // namespace ps2x
