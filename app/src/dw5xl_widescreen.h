#pragma once
// Issue 047: aspect ratio. The window shows the game's picture at the chosen shape, on every screen. In battle, 16:9 and 21:9
// also draw a wider 3D view, squeezed by the factor, so the stretch gives it its proper shape; the HUD is squeezed too.
// At 4:3 (the default) every hook only calls the game's own function.
#include "runtime/ps2_settings.h"

#include <cstdint>
#include <cstdlib>
#include <string>

namespace dw5xl::widescreen {
enum Aspect { Aspect4x3, Aspect16x9, Aspect21x9 };
constexpr int kAspects = 3;
constexpr const char *kAspectNames[kAspects] = {"4:3", "16:9", "21:9"};
constexpr int kFovDefault = 100, kFovMin = 80, kFovMax = 130, kFovStep = 5;
Aspect aspect();
float shape();                          // width / height of the chosen aspect
void setAspect(Aspect aspect);          // takes effect with the game's next picture
int fov();                              // percent of the battle camera's view angle, 80..130
void setFov(int percent);

// In dw5xl.ini: keys "aspect" and "fov". A missing key or a value the menu cannot show gives the default.
inline Aspect savedAspect()
{
    std::string text;
    if (ps2x::settings::loadText("aspect", text))
        for (int i = 0; i < kAspects; ++i)
            if (text == kAspectNames[i]) return Aspect(i);
    return Aspect4x3;
}
inline int savedFov()
{
    std::string text;
    if (ps2x::settings::loadText("fov", text)) {
        const int percent = std::atoi(text.c_str());
        if (percent >= kFovMin && percent <= kFovMax && percent % kFovStep == 0) return percent;
    }
    return kFovDefault;
}
inline void saveAspect(Aspect aspect) { ps2x::settings::saveText("aspect", kAspectNames[aspect]); }
inline void saveFov(int percent) { ps2x::settings::saveNumber("fov", uint32_t(percent)); }
} // namespace dw5xl::widescreen
