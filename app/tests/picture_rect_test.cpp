// Issue 049 (amendment v3.12 step 3): where the game's picture lies in the window (runtime/picture_rect.h). One
// function for every window that shows the picture. No window and no game data.
//  - worked examples: a window of the picture's own shape, a wider one (bars left and right), a taller one (bars
//    above and below), the display's own size when no shape is set, sizes of zero;
//  - the rectangle is centred, lies inside the window and touches two of its sides;
//  - for a sweep of window sizes and shapes it is bit for bit what the window computed before this function existed
//    (the lines of PS2Runtime::run as they were, copied here).
#include "runtime/picture_rect.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace
{
int failures = 0;

void check(bool ok, const std::string &what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    failures += ok ? 0 : 1;
}

bool near(float a, float b)
{
    return std::fabs(a - b) <= 0.01f;
}

bool is(const ps2x::PictureRect &r, float x, float y, float w, float h)
{
    return near(r.x, x) && near(r.y, y) && near(r.width, w) && near(r.height, h);
}

// The window's lines before the function existed (ps2_runtime.cpp, issue 047), kept here as the yardstick.
ps2x::PictureRect asBefore(uint32_t displayWidth, uint32_t displayHeight, float pictureShape, float screenWidth,
                           float screenHeight)
{
    const float aspectHeight = static_cast<float>(std::max<uint32_t>(1u, displayHeight));
    const float aspectWidth = pictureShape > 0.0f ? aspectHeight * pictureShape
                                                  : static_cast<float>(std::max<uint32_t>(1u, displayWidth));
    const float scale = std::min(screenWidth / aspectWidth, screenHeight / aspectHeight);
    const float dstWidth = aspectWidth * scale;
    const float dstHeight = aspectHeight * scale;
    return {(screenWidth - dstWidth) * 0.5f, (screenHeight - dstHeight) * 0.5f, dstWidth, dstHeight};
}
} // namespace

int main()
{
    using ps2x::pictureRectInWindow;
    // Worked examples (PAL display 640x448).
    check(is(pictureRectInWindow(640, 448, 4.0f / 3.0f, 1280.0f, 960.0f), 0.0f, 0.0f, 1280.0f, 960.0f),
          "4:3 picture in a 4:3 window fills it");
    check(is(pictureRectInWindow(640, 448, 4.0f / 3.0f, 1920.0f, 1080.0f), 240.0f, 0.0f, 1440.0f, 1080.0f),
          "4:3 picture in a 16:9 window: 1440 wide, bars of 240 left and right");
    check(is(pictureRectInWindow(640, 448, 16.0f / 9.0f, 1920.0f, 1080.0f), 0.0f, 0.0f, 1920.0f, 1080.0f),
          "16:9 picture in a 16:9 window fills it");
    check(is(pictureRectInWindow(640, 448, 16.0f / 9.0f, 1280.0f, 960.0f), 0.0f, 120.0f, 1280.0f, 720.0f),
          "16:9 picture in a 4:3 window: 720 high, bars of 120 above and below");
    check(is(pictureRectInWindow(640, 448, 0.0f, 1280.0f, 960.0f), 0.0f, 32.0f, 1280.0f, 896.0f),
          "no shape set: the display's own 640x448, doubled, bars of 32 above and below");
    check(is(pictureRectInWindow(640, 448, 0.0f, 640.0f, 448.0f), 0.0f, 0.0f, 640.0f, 448.0f),
          "no shape set, window of the display's size: one to one");
    check(is(pictureRectInWindow(0, 0, 0.0f, 300.0f, 200.0f), 50.0f, 0.0f, 200.0f, 200.0f),
          "a display of size zero counts as 1x1: a square");
    check(is(pictureRectInWindow(640, 448, 4.0f / 3.0f, 0.0f, 0.0f), 0.0f, 0.0f, 0.0f, 0.0f),
          "a window of size zero gives an empty rectangle");

    // Centred, inside, touching two sides; and the same as before, bit for bit.
    bool centred = true, inside = true, touches = true, same = true;
    const float shapes[] = {0.0f, 4.0f / 3.0f, 16.0f / 9.0f, 16.0f / 10.0f, 21.0f / 9.0f, 1.0f};
    const uint32_t displays[][2] = {{640, 448}, {640, 512}, {512, 448}, {640, 480}};
    int cases = 0;
    for (float shape : shapes)
        for (const auto &display : displays)
            for (int w = 320; w <= 3840; w += 173)
                for (int h = 200; h <= 2160; h += 131)
                {
                    const float ww = static_cast<float>(w), wh = static_cast<float>(h);
                    const ps2x::PictureRect r = pictureRectInWindow(display[0], display[1], shape, ww, wh);
                    const ps2x::PictureRect b = asBefore(display[0], display[1], shape, ww, wh);
                    ++cases;
                    same = same && r.x == b.x && r.y == b.y && r.width == b.width && r.height == b.height;
                    centred = centred && near(r.x * 2.0f + r.width, ww) && near(r.y * 2.0f + r.height, wh);
                    inside = inside && r.x >= -0.01f && r.y >= -0.01f && r.width <= ww + 0.01f && r.height <= wh + 0.01f;
                    touches = touches && (near(r.width, ww) || near(r.height, wh));
                }
    check(cases > 5000, "the sweep has " + std::to_string(cases) + " cases");
    check(centred, "the rectangle is centred in every case");
    check(inside, "the rectangle lies inside the window in every case");
    check(touches, "the rectangle touches two sides of the window in every case");
    check(same, "bit for bit what the window computed before, in every case");

    std::printf(failures ? "FAILED: %d check(s)\n" : "PASSED\n", failures);
    return failures ? 1 : 0;
}
