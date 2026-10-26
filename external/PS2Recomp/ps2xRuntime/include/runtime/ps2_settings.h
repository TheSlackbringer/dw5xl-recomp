#ifndef PS2_SETTINGS_H
#define PS2_SETTINGS_H
// The player's settings file dw5xl.ini ("key=value" lines; comments after ';' or '#'), shared by the runtime's
// hotkeys and a game's settings screen (issue 023 step 3S, DW5XL's PC SETTINGS).
#include <cstdint>
#include <string>

class GS;
namespace ps2x::settings
{
    bool loadText(const char *key, std::string &value);          // false if the file or the key is missing
    void saveText(const std::string &key, const std::string &value);
    void saveNumber(const std::string &key, uint32_t value);
    // Anti-aliasing samples per output pixel (1 = off, 2, 4, 8, 16): applied, remembered for F3 and saved.
    void setAntiAliasing(GS &gs, uint32_t samples);
    // Internal resolution 1x/2x/4x: applied and saved (as F4 does).
    void setResolutionScale(GS &gs, uint32_t scale);
}

#endif
