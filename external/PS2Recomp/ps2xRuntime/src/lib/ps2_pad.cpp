#include "runtime/ps2_pad.h"
#include "runtime/host_input.h"
#include "ps2_host_backend.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace ps2x_dev
{
    extern std::atomic<uint32_t> g_padPressedMask;
}

namespace
{
    constexpr uint16_t PAD_SELECT = 0x0001u, PAD_L3 = 0x0002u, PAD_R3 = 0x0004u;
    constexpr uint16_t PAD_START = 0x0008u, PAD_UP = 0x0010u, PAD_RIGHT = 0x0020u;
    constexpr uint16_t PAD_DOWN = 0x0040u, PAD_LEFT = 0x0080u;
    constexpr uint16_t PAD_L2 = 0x0100u, PAD_R2 = 0x0200u;
    constexpr uint16_t PAD_L1 = 0x0400u, PAD_R1 = 0x0800u;
    constexpr uint16_t PAD_TRIANGLE = 0x1000u, PAD_CIRCLE = 0x2000u;
    constexpr uint16_t PAD_CROSS = 0x4000u, PAD_SQUARE = 0x8000u;
    constexpr double kRadiansPerPixel = 0.15 * 3.14159265358979323846 / 180.0;
    constexpr double kMaxMouseAngle = 3.14159265358979323846 / 2.0;
    constexpr uint16_t kTapButtons = PAD_SELECT | PAD_START | PAD_TRIANGLE |
                                     PAD_CIRCLE | PAD_CROSS | PAD_SQUARE;
    using Clock = std::chrono::steady_clock;

    struct State
    {
        uint16_t pressed = 0;
        std::array<uint8_t, 4> sticks{128, 128, 128, 128};
        bool verdite = false;
        bool gameplay = false;
        bool captured = false;
        bool focused = false;
        bool indicatorCaptured = false, indicatorShown = false;
        double inputTime = 0.0, indicatorTime = 0.0;
        double yaw = 0.0, pitch = 0.0;
        Clock::time_point gameplayTime{}, motionTime{};
    };
    State gState;
    std::mutex gMutex;

    // Only the window thread touches the following device and edge state.
    bool gPreviousFocus = false, gPreviousGameplay = false;
    bool gResumeCapture = false, gCursorDisabled = false;
    std::array<bool, 3> gPreviousMouse{}, gSwallowedMouse{};
    std::array<double, 16> gTapUntil{};
    uint16_t gPreviousPressed = 0;

    bool traceEnabled()
    {
        static const bool enabled = [] {
            const char *value = std::getenv("PS2X_INPUT_TRACE");
            return value && *value && std::strcmp(value, "0") != 0;
        }();
        return enabled;
    }

    // Optional host-side integration fixture. It exercises the same keyboard,
    // mouse, capture and pad mapping as real devices; it never sets guest masks.
    struct Event
    {
        uint64_t tick = 0, duration = 1;
        std::string name;
        float dx = 0.0f, dy = 0.0f;
        bool consumed = false;
    };
    std::vector<Event> gEvents;
    std::once_flag gScriptInit;
    bool gScripted = false;

    void initScript()
    {
        std::call_once(gScriptInit, [] {
            const char *script = std::getenv("PS2X_HOST_INPUT");
            if (!script || !*script) return;
            std::stringstream entries(script);
            std::string entry;
            while (std::getline(entries, entry, ','))
            {
                std::vector<std::string> parts;
                std::stringstream fields(entry);
                std::string part;
                while (std::getline(fields, part, ':')) parts.push_back(part);
                if (parts.size() < 3 || parts[1].empty()) continue;
                try
                {
                    Event event;
                    event.tick = std::stoull(parts[0]);
                    event.name = parts[1];
                    if (event.name == "MOUSE" && parts.size() == 4)
                    {
                        event.dx = std::stof(parts[2]);
                        event.dy = std::stof(parts[3]);
                        if (!std::isfinite(event.dx) || !std::isfinite(event.dy)) continue;
                    }
                    else event.duration = std::max<uint64_t>(1, std::stoull(parts[2]));
                    gEvents.push_back(event);
                }
                catch (const std::exception &) { }
            }
            gScripted = !gEvents.empty();
            if (traceEnabled()) std::fprintf(stderr, "[host-input] fixture events=%zu\n", gEvents.size());
        });
    }

    bool scriptedDown(const char *name, uint64_t tick)
    {
        for (const Event &event : gEvents)
            if (event.name == name && tick >= event.tick && tick - event.tick < event.duration)
                return true;
        return false;
    }

    uint8_t stickByte(float value)
    {
        if (!std::isfinite(value)) value = 0.0f;
        return static_cast<uint8_t>(128.0f + std::clamp(value, -1.0f, 1.0f) * 127.0f);
    }
}

namespace ps2_host_input
{
    void enableVerditeControls()
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gState.verdite = true;
    }

    void setGameplayActive(bool active)
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (gState.gameplay != active) gState.yaw = gState.pitch = 0.0;
        gState.gameplay = active;
        gState.gameplayTime = Clock::now();
    }

    void discardMouseLook()
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gState.yaw = gState.pitch = 0.0;
    }

    MouseLook takeMouseLook()
    {
        std::lock_guard<std::mutex> lock(gMutex);
        MouseLook result;
        result.captured = gState.captured && gState.gameplay;
        if (result.captured && Clock::now() - gState.motionTime < std::chrono::milliseconds(250))
        {
            result.yawRadians = static_cast<float>(std::clamp(gState.yaw, -kMaxMouseAngle, kMaxMouseAngle));
            result.pitchRadians = static_cast<float>(std::clamp(gState.pitch, -kMaxMouseAngle, kMaxMouseAngle));
        }
        gState.yaw = gState.pitch = 0.0;
        return result;
    }

    MouseCaptureIndicator mouseCaptureIndicator()
    {
        std::lock_guard<std::mutex> lock(gMutex);
        MouseCaptureIndicator result;
        result.captured = gState.verdite && gState.focused && gState.captured && gState.gameplay;
        if (!gState.verdite || !gState.indicatorShown) return result;
        // Verdite2's announcement: 140 ms in, 1100 ms held, 420 ms out.
        const double age = gState.inputTime - gState.indicatorTime;
        if (age < 0.0 || age >= 1.66) return result;
        if (age < 0.14) result.alpha = static_cast<float>(age / 0.14);
        else if (age < 1.24) result.alpha = 1.0f;
        else result.alpha = static_cast<float>((1.66 - age) / 0.42);
        return result;
    }

    void drawMouseCaptureIndicator(float x, float y, float width, float height)
    {
        const MouseCaptureIndicator indicator = mouseCaptureIndicator();
        if (indicator.alpha <= 0.001f || width < 64.0f || height < 64.0f) return;
        // Procedural glyph and released-state cut from Verdite2's
        // patches/MouseIndicator.cs (MIT, Voicedrew11/verdite2).
        static constexpr const char *glyph[] = {
            "....#....", "....#....", "...###...", "..#####..",
            ".##.#.##.", ".##.#.##.", ".#######.", ".#######.",
            ".#######.", ".#######.", ".#######.", "..#####..", "...###..."
        };
        const int cell = std::max(2, static_cast<int>(std::round(height / 180.0f)));
        const float margin = std::max(static_cast<float>(cell * 2), std::round(height * 0.03f));
        const int originX = static_cast<int>(std::round(x + width - margin - cell * 9));
        const int originY = static_cast<int>(std::round(y + margin));
        const Color shadow{0, 0, 0, static_cast<unsigned char>(255.0f * 0.55f * indicator.alpha)};
        const Color white{255, 255, 255, static_cast<unsigned char>(255.0f * indicator.alpha)};
        auto blit = [&](int offset, Color color) {
            for (int row = 0; row < 13; ++row)
                for (int column = 0; column < 9; ++column)
                {
                    if (glyph[row][column] != '#') continue;
                    if (!indicator.captured && (row - column == 4 || row - column == 5)) continue;
                    DrawRectangle(originX + (column + offset) * cell,
                                  originY + (row + offset) * cell, cell, cell, color);
                }
        };
        blit(1, shadow);
        blit(0, white);
    }

    void poll(uint64_t vsyncTick)
    {
        initScript();
        if (!IsWindowReady() && !gScripted) return;
        bool verdite, gameplay, captured;
        {
            std::lock_guard<std::mutex> lock(gMutex);
            verdite = gState.verdite;
            gameplay = gState.gameplay;
            captured = gState.captured;
        }
        const bool focused = gScripted || (IsWindowReady() && IsWindowFocused());
        const double now = IsWindowReady() ? GetTime() : 0.0;
        uint16_t pressed = 0;
        std::array<uint8_t, 4> sticks{128, 128, 128, 128};
        auto key = [&](int code, const char *name) {
            return focused && (gScripted ? scriptedDown(name, vsyncTick) : (IsWindowReady() && IsKeyDown(code)));
        };
        auto bind = [&](bool down, uint16_t mask) { if (down) pressed |= mask; };
        bind(key(KEY_UP, "UP") || key(KEY_W, "W"), PAD_UP);
        bind(key(KEY_DOWN, "DOWN") || key(KEY_S, "S"), PAD_DOWN);
        bind(key(KEY_LEFT, "LEFT"), PAD_LEFT);
        bind(key(KEY_RIGHT, "RIGHT"), PAD_RIGHT);
        bind(key(KEY_ENTER, "ENTER"), PAD_START);
        if (verdite)
        {
            bind(key(KEY_A, "A"), PAD_L1);
            bind(key(KEY_D, "D"), PAD_R1);
            bind(key(KEY_SPACE, "SPACE"), PAD_SQUARE);
            bind(key(KEY_F, "F"), PAD_CROSS);
            bind(key(KEY_Q, "Q"), PAD_TRIANGLE);
            bind(key(KEY_TAB, "TAB") || key(KEY_ESCAPE, "ESCAPE"), PAD_CIRCLE);
            bind(key(KEY_RIGHT_SHIFT, "RIGHTSHIFT"), PAD_SELECT);
        }
        else
        {
            bind(key(KEY_A, "A"), PAD_LEFT);
            bind(key(KEY_D, "D"), PAD_RIGHT);
            bind(key(KEY_X, "X") || key(KEY_SPACE, "SPACE"), PAD_CROSS);
            bind(key(KEY_C, "C") || key(KEY_ESCAPE, "ESCAPE"), PAD_CIRCLE);
            bind(key(KEY_Z, "Z") || key(KEY_KP_0, "KP0"), PAD_SQUARE);
            bind(key(KEY_V, "V") || key(KEY_KP_1, "KP1"), PAD_TRIANGLE);
            bind(key(KEY_Q, "Q"), PAD_L1);
            bind(key(KEY_E, "E"), PAD_R1);
            bind(key(KEY_LEFT_SHIFT, "LEFTSHIFT"), PAD_L2);
            bind(key(KEY_RIGHT_SHIFT, "RIGHTSHIFT"), PAD_R2);
            bind(key(KEY_TAB, "TAB"), PAD_SELECT);
        }

        int gamepad = -1;
        if (focused && !gScripted)
        {
            for (int index = 0; index < 4; ++index)
                if (IsGamepadAvailable(index)) { gamepad = index; break; }
        }
        if (gamepad >= 0)
        {
            const std::pair<int, uint16_t> buttons[] = {
                {GAMEPAD_BUTTON_LEFT_FACE_UP, PAD_UP}, {GAMEPAD_BUTTON_LEFT_FACE_DOWN, PAD_DOWN},
                {GAMEPAD_BUTTON_LEFT_FACE_LEFT, PAD_LEFT}, {GAMEPAD_BUTTON_LEFT_FACE_RIGHT, PAD_RIGHT},
                {GAMEPAD_BUTTON_RIGHT_FACE_DOWN, PAD_CROSS}, {GAMEPAD_BUTTON_RIGHT_FACE_RIGHT, PAD_CIRCLE},
                {GAMEPAD_BUTTON_RIGHT_FACE_LEFT, PAD_SQUARE}, {GAMEPAD_BUTTON_RIGHT_FACE_UP, PAD_TRIANGLE},
                {GAMEPAD_BUTTON_LEFT_TRIGGER_1, PAD_L1}, {GAMEPAD_BUTTON_RIGHT_TRIGGER_1, PAD_R1},
                {GAMEPAD_BUTTON_LEFT_TRIGGER_2, PAD_L2}, {GAMEPAD_BUTTON_RIGHT_TRIGGER_2, PAD_R2},
                {GAMEPAD_BUTTON_MIDDLE_RIGHT, PAD_START}, {GAMEPAD_BUTTON_MIDDLE_LEFT, PAD_SELECT},
                {GAMEPAD_BUTTON_LEFT_THUMB, PAD_L3}, {GAMEPAD_BUTTON_RIGHT_THUMB, PAD_R3}
            };
            for (const auto &button : buttons) bind(IsGamepadButtonDown(gamepad, button.first), button.second);
            sticks[0] = stickByte(GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_RIGHT_X));
            sticks[1] = stickByte(GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_RIGHT_Y));
            sticks[2] = stickByte(GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_LEFT_X));
            sticks[3] = stickByte(GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_LEFT_Y));
        }

        std::array<bool, 3> mouse{};
        const char *mouseNames[] = {"MOUSELEFT", "MOUSERIGHT", "MOUSEMIDDLE"};
        for (int index = 0; index < 3; ++index)
            mouse[index] = focused && (gScripted ? scriptedDown(mouseNames[index], vsyncTick) : IsMouseButtonDown(index));
        const bool wasCaptured = captured;
        bool discardMotion = focused != gPreviousFocus || gameplay != gPreviousGameplay;
        if (!focused)
        {
            captured = false;
            gResumeCapture = false;
            gTapUntil.fill(0.0);
            gSwallowedMouse.fill(false);
        }
        else if (verdite)
        {
            if (!gameplay)
            {
                if (captured) gResumeCapture = true;
                captured = false;
            }
            else
            {
                if (!gPreviousGameplay && gResumeCapture)
                {
                    captured = true;
                    gResumeCapture = false;
                    gSwallowedMouse = mouse;
                }
                if (!captured && mouse[0] && !gPreviousMouse[0])
                {
                    captured = true;
                    gSwallowedMouse = mouse;
                    discardMotion = true;
                }
                if ((pressed & PAD_CIRCLE) && !(gPreviousPressed & PAD_CIRCLE))
                {
                    if (captured) gResumeCapture = true;
                    captured = false;
                }
            }
            for (int index = 0; index < 3; ++index)
                if (!mouse[index]) gSwallowedMouse[index] = false;
            if (gameplay)
            {
                bind(captured && mouse[0] && !gSwallowedMouse[0], PAD_SQUARE);
                bind(captured && mouse[1] && !gSwallowedMouse[1], PAD_TRIANGLE);
                bind(captured && mouse[2] && !gSwallowedMouse[2], PAD_CROSS);
            }
            else
            {
                bind(mouse[0] && !gSwallowedMouse[0], PAD_CROSS);
                bind(mouse[1] && !gSwallowedMouse[1], PAD_CIRCLE);
                bind(mouse[2] && !gSwallowedMouse[2], PAD_CROSS);
                const float wheel = gScripted ? 0.0f : GetMouseWheelMove();
                bind(wheel > 0.0f, PAD_UP);
                bind(wheel < 0.0f, PAD_DOWN);
            }
        }
        if (captured != wasCaptured) discardMotion = true;
        if (!gScripted && captured != gCursorDisabled)
        {
            if (captured) DisableCursor(); else EnableCursor();
            gCursorDisabled = captured;
        }
        Vector2 delta = GetMouseDelta();
        if (gScripted) delta = {0.0f, 0.0f};
        for (Event &event : gEvents)
        {
            if (event.name == "MOUSE" && !event.consumed && vsyncTick >= event.tick)
            {
                event.consumed = true;
                delta.x += event.dx;
                delta.y += event.dy;
            }
        }

        // Brief taps must survive until a slow guest frame samples its pad.
        // Movement keys are never extended, so releasing W/A/S/D stops promptly.
        const uint16_t rawPressed = pressed;
        for (unsigned bit = 0; bit < 16; ++bit)
        {
            const uint16_t mask = static_cast<uint16_t>(1u << bit);
            if ((kTapButtons & mask) && (rawPressed & mask) && !(gPreviousPressed & mask))
                gTapUntil[bit] = now + 0.1;
            if (focused && now < gTapUntil[bit]) pressed |= mask;
        }
        {
            std::lock_guard<std::mutex> lock(gMutex);
            if (traceEnabled() && (gState.pressed != pressed || gState.captured != captured))
                std::fprintf(stderr, "[host-input] tick=%llu pressed=%04x gameplay=%u captured=%u gamepad=%d\n",
                    static_cast<unsigned long long>(vsyncTick), pressed, gameplay ? 1u : 0u,
                    captured ? 1u : 0u, gamepad);
            gState.pressed = pressed;
            gState.sticks = sticks;
            gState.captured = captured;
            gState.focused = focused;
            gState.inputTime = now;
            const bool indicatorCaptured = verdite && focused && captured && gState.gameplay;
            if (indicatorCaptured != gState.indicatorCaptured)
            {
                gState.indicatorCaptured = indicatorCaptured;
                gState.indicatorShown = true;
                gState.indicatorTime = now;
            }
            if (discardMotion || !captured || !gameplay) gState.yaw = gState.pitch = 0.0;
            else if (std::isfinite(delta.x) && std::isfinite(delta.y) && (delta.x != 0.0f || delta.y != 0.0f))
            {
                // Do not resurrect old motion after the guest stalls.
                if (Clock::now() - gState.motionTime > std::chrono::milliseconds(250))
                    gState.yaw = gState.pitch = 0.0;
                gState.yaw += static_cast<double>(delta.x) * kRadiansPerPixel;
                gState.pitch -= static_cast<double>(delta.y) * kRadiansPerPixel;
                gState.motionTime = Clock::now();
            }
        }
        gPreviousFocus = focused;
        gPreviousGameplay = gameplay;
        gPreviousMouse = mouse;
        gPreviousPressed = rawPressed;
    }
}

bool PSPadBackend::readState(int /*port*/, int /*slot*/, uint8_t *data, size_t size)
{
    if (!data || size < 32) return false;
    std::memset(data, 0, 32);
    data[0] = 0x01;
    data[1] = 0x73;
    uint16_t pressed;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        pressed = gState.pressed;
        std::memcpy(data + 4, gState.sticks.data(), gState.sticks.size());
    }
    pressed |= static_cast<uint16_t>(ps2x_dev::g_padPressedMask.load(std::memory_order_relaxed));
    const uint16_t buttons = static_cast<uint16_t>(~pressed);
    data[2] = static_cast<uint8_t>(buttons);
    data[3] = static_cast<uint8_t>(buttons >> 8);
    return true;
}
