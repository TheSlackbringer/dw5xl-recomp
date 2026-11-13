// Issue 049 (amendment v3.12 step 3): keys and pads reach the game through one interface (runtime/host_devices.h),
// whichever window shows the game. No window, no real device and no game data: the devices are made up here.
//  - the interface can be replaced and put back;
//  - every key of the keyboard layout reaches the game as its PS2 button, and only while the window has the focus;
//  - a window that is not ready is not read;
//  - every pad button reaches the game as its PS2 button; the first pad that is there is used; the sticks arrive as
//    bytes (128 = centre);
//  - keys and pad together add up.
// The time is made up too: a second passes between two samples, so that a short press held on for the game
// (0.1 s) has ended by the next one.
#include "ps2_runtime.h"
#include "ps2_host_backend.h" // raylib's key and pad button numbers
#include "runtime/host_devices.h"
#include "runtime/host_input.h"
#include "runtime/ps2_pad.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <utility>

// The runtime references the recompiled-function table; this test has no generated code (as the other app tests).
extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x1000;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x1004;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 1;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[] = {nullptr};

namespace
{
int failures = 0;
void check(bool ok, const std::string &what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    failures += ok ? 0 : 1;
}

constexpr uint16_t kSelect = 0x0001u, kL3 = 0x0002u, kR3 = 0x0004u, kStart = 0x0008u, kUp = 0x0010u, kRight = 0x0020u,
                   kDown = 0x0040u, kLeft = 0x0080u, kL2 = 0x0100u, kR2 = 0x0200u, kL1 = 0x0400u, kR1 = 0x0800u,
                   kTriangle = 0x1000u, kCircle = 0x2000u, kCross = 0x4000u, kSquare = 0x8000u;

struct MadeUp final : ps2x::host::Devices
{
    bool isReady = true, hasFocus = true;
    double now = 100.0;
    std::set<int> keys;
    std::set<int> pads;
    std::set<std::pair<int, int>> buttons;
    std::map<std::pair<int, int>, float> axes;

    bool ready() const override { return isReady; }
    bool focused() const override { return hasFocus; }
    double seconds() const override { return now; }
    bool keyDown(int key) const override { return keys.count(key) != 0; }
    bool keyPressed(int key) const override { return keys.count(key) != 0; }
    bool keyPressedRepeat(int) const override { return false; }
    bool padAvailable(int pad) const override { return pads.count(pad) != 0; }
    bool padButtonDown(int pad, int button) const override { return buttons.count({pad, button}) != 0; }
    bool padButtonPressed(int pad, int button) const override { return buttons.count({pad, button}) != 0; }
    float padAxis(int pad, int axis) const override
    {
        const auto found = axes.find({pad, axis});
        return found == axes.end() ? 0.0f : found->second;
    }
    bool mouseButtonDown(int) const override { return false; }
    float mouseWheel() const override { return 0.0f; }
    void mouseDelta(float &x, float &y) const override { x = y = 0.0f; }
    void captureMouse(bool) override {}
};

struct Sample
{
    uint16_t pressed = 0xFFFFu;
    uint8_t sticks[4] = {0, 0, 0, 0};
};

// One reading of the devices by the window, then the game's pad read of a new field.
Sample sample(MadeUp &devices, PSPadBackend &pad)
{
    static uint64_t field = 1;
    devices.now += 1.0;
    ps2_host_input::poll(field);
    uint8_t data[32]{};
    Sample s;
    if (pad.readState(0, 0, data, sizeof(data), field))
    {
        s.pressed = static_cast<uint16_t>(~(data[2] | (data[3] << 8)));
        for (int i = 0; i < 4; ++i)
            s.sticks[i] = data[4 + i];
    }
    ++field;
    return s;
}
} // namespace

int main()
{
    ps2x::host::Devices *usual = &ps2x::host::devices();
    MadeUp made;
    ps2x::host::setDevices(&made);
    check(&ps2x::host::devices() == &made, "the devices can be replaced");
    ps2x::host::setDevices(nullptr);
    check(&ps2x::host::devices() == usual, "and put back");
    ps2x::host::setDevices(&made);

    PSPadBackend pad;
    check(sample(made, pad).pressed == 0, "nothing held: the game sees no button");

    const struct { int key; uint16_t button; const char *name; } keys[] = {
        {KEY_UP, kUp, "Up arrow is Up"}, {KEY_W, kUp, "W is Up"}, {KEY_DOWN, kDown, "Down arrow is Down"},
        {KEY_S, kDown, "S is Down"}, {KEY_LEFT, kLeft, "Left arrow is Left"}, {KEY_A, kLeft, "A is Left"},
        {KEY_RIGHT, kRight, "Right arrow is Right"}, {KEY_D, kRight, "D is Right"}, {KEY_ENTER, kStart, "Enter is Start"},
        {KEY_X, kCross, "X is Cross"}, {KEY_SPACE, kCross, "Space is Cross"}, {KEY_C, kCircle, "C is Circle"},
        {KEY_Z, kSquare, "Z is Square"}, {KEY_KP_0, kSquare, "keypad 0 is Square"}, {KEY_V, kTriangle, "V is Triangle"},
        {KEY_KP_1, kTriangle, "keypad 1 is Triangle"}, {KEY_Q, kL1, "Q is L1"}, {KEY_E, kR1, "E is R1"},
        {KEY_LEFT_SHIFT, kL2, "left Shift is L2"}, {KEY_RIGHT_SHIFT, kR2, "right Shift is R2"},
        {KEY_TAB, kSelect, "Tab is Select"}};
    for (const auto &k : keys)
    {
        made.keys = {k.key};
        const uint16_t held = sample(made, pad).pressed;
        made.keys.clear();
        const uint16_t after = sample(made, pad).pressed;
        check(held == k.button && after == 0, std::string(k.name) + ", and nothing when it is let go");
    }

    made.keys = {KEY_X};
    made.hasFocus = false;
    check(sample(made, pad).pressed == 0, "a key held while the window has no focus does not reach the game");
    made.hasFocus = true;
    check(sample(made, pad).pressed == kCross, "with the focus back it does");
    made.isReady = false;
    made.keys = {KEY_C};
    check(sample(made, pad).pressed == kCross, "a window that is not ready is not read: the last reading stands");
    made.isReady = true;
    made.keys.clear();
    check(sample(made, pad).pressed == 0, "ready again and nothing held: no button");

    const struct { int button; uint16_t ps2; const char *name; } buttons[] = {
        {GAMEPAD_BUTTON_LEFT_FACE_UP, kUp, "pad Up"}, {GAMEPAD_BUTTON_LEFT_FACE_DOWN, kDown, "pad Down"},
        {GAMEPAD_BUTTON_LEFT_FACE_LEFT, kLeft, "pad Left"}, {GAMEPAD_BUTTON_LEFT_FACE_RIGHT, kRight, "pad Right"},
        {GAMEPAD_BUTTON_RIGHT_FACE_DOWN, kCross, "pad bottom face button is Cross"},
        {GAMEPAD_BUTTON_RIGHT_FACE_RIGHT, kCircle, "pad right face button is Circle"},
        {GAMEPAD_BUTTON_RIGHT_FACE_LEFT, kSquare, "pad left face button is Square"},
        {GAMEPAD_BUTTON_RIGHT_FACE_UP, kTriangle, "pad top face button is Triangle"},
        {GAMEPAD_BUTTON_LEFT_TRIGGER_1, kL1, "pad L1"}, {GAMEPAD_BUTTON_RIGHT_TRIGGER_1, kR1, "pad R1"},
        {GAMEPAD_BUTTON_LEFT_TRIGGER_2, kL2, "pad L2"}, {GAMEPAD_BUTTON_RIGHT_TRIGGER_2, kR2, "pad R2"},
        {GAMEPAD_BUTTON_MIDDLE_RIGHT, kStart, "pad Start"}, {GAMEPAD_BUTTON_LEFT_THUMB, kL3, "pad L3"},
        {GAMEPAD_BUTTON_RIGHT_THUMB, kR3, "pad R3"}};
    made.pads = {0};
    for (const auto &b : buttons)
    {
        made.buttons = {{0, b.button}};
        const uint16_t held = sample(made, pad).pressed;
        made.buttons.clear();
        const uint16_t after = sample(made, pad).pressed;
        check(held == b.ps2 && after == 0, std::string(b.name) + ", and nothing when it is let go");
    }

    made.axes = {{{0, GAMEPAD_AXIS_RIGHT_X}, 1.0f}, {{0, GAMEPAD_AXIS_RIGHT_Y}, -1.0f}, {{0, GAMEPAD_AXIS_LEFT_X}, 0.5f}};
    Sample s = sample(made, pad);
    check(s.sticks[0] == 255 && s.sticks[1] == 1 && s.sticks[2] == 191 && s.sticks[3] == 128,
          "the sticks arrive as bytes: right 255 and 1, left 191 and 128 (centre)");
    made.axes.clear();
    s = sample(made, pad);
    check(s.sticks[0] == 128 && s.sticks[1] == 128 && s.sticks[2] == 128 && s.sticks[3] == 128, "sticks let go: all centred");

    made.pads = {2};
    made.buttons = {{2, GAMEPAD_BUTTON_RIGHT_FACE_DOWN}, {0, GAMEPAD_BUTTON_RIGHT_FACE_UP}};
    check(sample(made, pad).pressed == kCross, "the first pad that is there is used (pad 2), a missing pad 0 is not");
    made.buttons.clear();
    sample(made, pad);

    made.pads = {0};
    made.keys = {KEY_Z};
    made.buttons = {{0, GAMEPAD_BUTTON_RIGHT_TRIGGER_1}};
    check(sample(made, pad).pressed == (kSquare | kR1), "a key and a pad button together add up");
    made.pads.clear();
    made.buttons = {{0, GAMEPAD_BUTTON_RIGHT_TRIGGER_1}};
    made.keys.clear();
    sample(made, pad);
    check(sample(made, pad).pressed == 0, "a pad that is gone gives no button");

    ps2x::host::setDevices(nullptr);
    std::printf(failures ? "FAILED: %d check(s)\n" : "PASSED\n", failures);
    return failures ? 1 : 0;
}
