// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// DW5XL (issue 049): the keys, the mouse and the pads of a window made with GLFW directly, kept the way raylib 5.5
// keeps them, so that such a window gives the answers of ps2x::host::Devices (runtime/host_devices.h) that today's
// raylib window gives. This file is the state only: it is fed GLFW's raw events and knows neither GLFW nor raylib.
// Every rule is raylib's, taken from its source and kept with its quirks; the places are named at each rule as
//   rcore.c    = <raylib>/src/rcore.c
//   desktop.c  = <raylib>/src/platforms/rcore_desktop_glfw.c
//   raylib.h   = <raylib>/src/raylib.h
//   glfw3.h    = <raylib>/src/external/glfw/include/GLFW/glfw3.h
//
// One reading of the window ("poll"), in raylib's order (desktop.c:1126-1264, PollInputEvents):
//   state.beginPoll();
//   for every pad place 0..3: state.pad(place, there, &what it holds);     // read with GLFW's joystick functions
//   glfwPollEvents();                                                       // its callbacks call key(), mouseButton(),
//                                                                           // mouseMoved(), scrolled(), focus()
// The questions are asked between two polls. Pads first or events first gives the same state.
// No locking, as in raylib: feed and ask from one thread, or guard it outside.

#include <cmath>

namespace ps2x::host
{
    // What one pad holds, in the shape and order of GLFW's GLFWgamepadstate (glfw3.h:2113-2123).
    struct RawPad
    {
        unsigned char buttons[15]; // 1 = held, in GLFW's button order
        float axes[6];             // -1..1 in GLFW's axis order; a trigger rests at -1
    };

    namespace input_numbers
    {
        // GLFW's numbers (glfw3.h:331-345, 462-464, 556-562, 620-656).
        inline constexpr int kRelease = 0, kPress = 1, kRepeat = 2;
        inline constexpr int kKeyCapsLock = 280, kKeyNumLock = 282;
        inline constexpr int kModCapsLock = 0x0010, kModNumLock = 0x0020;
        enum GlfwPadButton : int
        {
            kGlfwPadA = 0,
            kGlfwPadB = 1,
            kGlfwPadX = 2,
            kGlfwPadY = 3,
            kGlfwPadLeftBumper = 4,
            kGlfwPadRightBumper = 5,
            kGlfwPadBack = 6,
            kGlfwPadStart = 7,
            kGlfwPadGuide = 8,
            kGlfwPadLeftThumb = 9,
            kGlfwPadRightThumb = 10,
            kGlfwPadDpadUp = 11,
            kGlfwPadDpadRight = 12,
            kGlfwPadDpadDown = 13,
            kGlfwPadDpadLeft = 14,
            kGlfwPadButtonCount = 15
        };
        inline constexpr int kGlfwPadAxisCount = 6; // left x, left y, right x, right y, left trigger, right trigger

        // raylib's numbers, the ones of the questions (raylib.h:725-754, GamepadButton and GamepadAxis).
        enum PadButton : int
        {
            kPadUnknown = 0,
            kPadLeftFaceUp = 1,
            kPadLeftFaceRight = 2,
            kPadLeftFaceDown = 3,
            kPadLeftFaceLeft = 4,
            kPadRightFaceUp = 5,
            kPadRightFaceRight = 6,
            kPadRightFaceDown = 7,
            kPadRightFaceLeft = 8,
            kPadLeftTrigger1 = 9,
            kPadLeftTrigger2 = 10,
            kPadRightTrigger1 = 11,
            kPadRightTrigger2 = 12,
            kPadMiddleLeft = 13,
            kPadMiddle = 14,
            kPadMiddleRight = 15,
            kPadLeftThumb = 16,
            kPadRightThumb = 17
        };
        enum PadAxis : int
        {
            kPadAxisLeftX = 0,
            kPadAxisLeftY = 1,
            kPadAxisRightX = 2,
            kPadAxisRightY = 3,
            kPadAxisLeftTrigger = 4,
            kPadAxisRightTrigger = 5
        };

        // raylib's table sizes (<raylib>/src/config.h:80-84).
        inline constexpr int kMaxKeys = 512;
        inline constexpr int kMaxMouseButtons = 8;
        inline constexpr int kMaxPads = 4;
        inline constexpr int kMaxPadAxes = 8;
        inline constexpr int kMaxPadButtons = 32;

        // Which raylib button a GLFW button is (desktop.c:1201-1223). The two second triggers are not in this list:
        // GLFW has them as axes only, and raylib makes buttons of them (see InputState::pad).
        inline constexpr int kPadButtonOfGlfw[kGlfwPadButtonCount] = {
            kPadRightFaceDown,  // A
            kPadRightFaceRight, // B
            kPadRightFaceLeft,  // X
            kPadRightFaceUp,    // Y
            kPadLeftTrigger1,   // left bumper
            kPadRightTrigger1,  // right bumper
            kPadMiddleLeft,     // back
            kPadMiddleRight,    // start
            kPadMiddle,         // guide
            kPadLeftThumb,      // left stick pressed
            kPadRightThumb,     // right stick pressed
            kPadLeftFaceUp,     // d-pad up
            kPadLeftFaceRight,  // d-pad right
            kPadLeftFaceDown,   // d-pad down
            kPadLeftFaceLeft,   // d-pad left
        };

        // A trigger counts as the button LEFT_TRIGGER_2 / RIGHT_TRIGGER_2 while its axis is above this
        // (desktop.c:1245-1246). The axis rests at -1 and ends at 1, so that is a trigger pulled more than 55 % of
        // its way; a trigger pulled half way (0) is not a button yet.
        inline constexpr float kTriggerButtonAbove = 0.1f;
    } // namespace input_numbers

    class InputState
    {
    public:
        // ---- The start of a poll -----------------------------------------------------------------------------------
        // What raylib does at the start of PollInputEvents, before GLFW's events arrive (desktop.c:1144-1159): the
        // keys of now become those of before, the key repeats and the wheel are forgotten, the mouse position of
        // now becomes the one of before.
        // raylib also keeps the mouse buttons and the wheel of the poll before; no question of Devices reads them,
        // so they are not kept here.
        // The pads are not touched here: raylib copies a pad's buttons only when the pad is there (see pad()).
        void beginPoll()
        {
            for (int i = 0; i < input_numbers::kMaxKeys; ++i)
            {
                m_keyBefore[i] = m_keyNow[i];
                m_keyRepeat[i] = 0;
            }
            m_wheelX = 0.0f;
            m_wheelY = 0.0f;
            m_mouseBeforeX = m_mouseNowX;
            m_mouseBeforeY = m_mouseNowY;
        }

        // ---- GLFW's events ------------------------------------------------------------------------------------------
        // GLFW's key callback (desktop.c:1791-1804, KeyCallback). action: 0 release, 1 press, 2 repeat.
        // mods are the callback's modifier bits; they matter only for Caps Lock and Num Lock, which raylib holds down
        // for as long as the lock is on (it switches GLFW_LOCK_KEY_MODS on, desktop.c:1653).
        // A key below 0 is left out as in raylib; a key from 512 on, which raylib would write outside its table, too
        // (GLFW's highest key is 348).
        void key(int glfwKey, int action, int mods = 0)
        {
            using namespace input_numbers;
            if (glfwKey < 0 || glfwKey >= kMaxKeys)
                return;
            if (action == kRelease)
                m_keyNow[glfwKey] = 0;
            else if (action == kPress)
                m_keyNow[glfwKey] = 1;
            else if (action == kRepeat)
                m_keyRepeat[glfwKey] = 1;
            if ((glfwKey == kKeyCapsLock && (mods & kModCapsLock) > 0) ||
                (glfwKey == kKeyNumLock && (mods & kModNumLock) > 0))
                m_keyNow[glfwKey] = 1;
        }

        // GLFW's mouse button callback (desktop.c:1837-1842, MouseButtonCallback): the action is stored as it comes.
        // GLFW's buttons are 0..7, raylib's table has 8 places; another number is left out here.
        void mouseButton(int button, int action)
        {
            if (button < 0 || button >= input_numbers::kMaxMouseButtons)
                return;
            m_mouseButtonNow[button] = static_cast<char>(action);
        }

        // GLFW's cursor position callback (desktop.c:1873-1876, MouseCursorPosCallback).
        void mouseMoved(double x, double y)
        {
            m_mouseNowX = static_cast<float>(x);
            m_mouseNowY = static_cast<float>(y);
        }

        // GLFW's scroll callback (desktop.c:1904-1907, MouseScrollCallback): the last event of a poll replaces the
        // ones before it, they are not added up.
        void scrolled(double x, double y)
        {
            m_wheelX = static_cast<float>(x);
            m_wheelY = static_cast<float>(y);
        }

        // GLFW's window focus callback (desktop.c:1756-1760, WindowFocusCallback). Nothing else changes: the keys
        // that were held are let go by GLFW itself, through key() and mouseButton().
        void focus(bool focused) { m_unfocused = !focused; }

        // ---- The pads, once per poll for every place 0..3 ----------------------------------------------------------
        // present: glfwJoystickPresent(index). state: what glfwGetGamepadState(index, ...) filled in; for a joystick
        // GLFW has no pad layout for, that is all zeros, and raylib takes it as a pad with nothing pressed
        // (desktop.c:1192-1193). present = false or state = nullptr: no pad there.
        // (desktop.c:1176-1249.) A pad that is not there keeps its buttons and axes as they were: raylib neither
        // copies nor clears them, the questions just answer "no" (see padButtonDown, padAxis). An index outside
        // 0..3 is left out.
        void pad(int index, bool present, const RawPad *state)
        {
            using namespace input_numbers;
            if (index < 0 || index >= kMaxPads)
                return;
            const bool ready = present && state != nullptr;
            m_padReady[index] = ready;
            if (!ready)
                return;
            for (int k = 0; k < kMaxPadButtons; ++k)
                m_padButtonBefore[index][k] = m_padButtonNow[index][k];
            for (int k = 0; k < kGlfwPadButtonCount; ++k)
                m_padButtonNow[index][kPadButtonOfGlfw[k]] = state->buttons[k] == kPress ? 1 : 0;
            for (int k = 0; k < kGlfwPadAxisCount; ++k)
                m_padAxis[index][k] = state->axes[k]; // stored as they come: no dead zone, no scaling
            m_padButtonNow[index][kPadLeftTrigger2] =
                m_padAxis[index][kPadAxisLeftTrigger] > kTriggerButtonAbove ? 1 : 0;
            m_padButtonNow[index][kPadRightTrigger2] =
                m_padAxis[index][kPadAxisRightTrigger] > kTriggerButtonAbove ? 1 : 0;
        }

        // ---- The mouse caught in the window or let go ---------------------------------------------------------------
        // What raylib's DisableCursor() (captured) and EnableCursor() (not captured) do to the state
        // (desktop.c:1012-1035 with SetMousePosition, desktop.c:1098-1101): the position of now AND the one of before
        // become the middle of the window, so the movement reads 0 until the mouse moves in a later poll.
        // screenWidth/screenHeight: the window's size as raylib's GetScreenWidth()/GetScreenHeight() give it; the
        // middle is half of each as a whole number (raylib halves unsigned whole numbers).
        // The window then has to put GLFW's cursor at mouseX(), mouseY(), as raylib does.
        void mouseCaptured(bool captured, int screenWidth, int screenHeight)
        {
            m_captured = captured;
            m_mouseNowX = static_cast<float>(static_cast<int>(static_cast<unsigned int>(screenWidth) / 2u));
            m_mouseNowY = static_cast<float>(static_cast<int>(static_cast<unsigned int>(screenHeight) / 2u));
            m_mouseBeforeX = m_mouseNowX;
            m_mouseBeforeY = m_mouseNowY;
        }
        float mouseX() const { return m_mouseNowX; }
        float mouseY() const { return m_mouseNowY; }
        bool mouseIsCaptured() const { return m_captured; } // raylib's IsCursorHidden() after those two functions

        // ---- The questions of ps2x::host::Devices -------------------------------------------------------------------
        // rcore.c:3145-3155, IsKeyDown. Key 0 and keys outside the table are never down.
        bool keyDown(int key) const { return keyInRange(key) && m_keyNow[key] == 1; }
        // rcore.c:3118-3129, IsKeyPressed: down now and not down at the poll before. A key pressed and let go within
        // one poll is never seen.
        bool keyPressed(int key) const { return keyInRange(key) && m_keyBefore[key] == 0 && m_keyNow[key] == 1; }
        // rcore.c:3132-3142, IsKeyPressedRepeat: the system repeated the key during this poll.
        bool keyPressedRepeat(int key) const { return keyInRange(key) && m_keyRepeat[key] == 1; }

        // rcore.c:3242-3249, IsGamepadAvailable. (raylib does not check for a pad below 0; here that is "no".)
        bool padAvailable(int pad) const { return pad >= 0 && pad < input_numbers::kMaxPads && m_padReady[pad]; }
        // rcore.c:3269-3277, IsGamepadButtonDown. (A button below 0, unchecked in raylib, is "no" here.)
        bool padButtonDown(int pad, int button) const
        {
            return padAvailable(pad) && padButtonInRange(button) && m_padButtonNow[pad][button] == 1;
        }
        // rcore.c:3258-3266, IsGamepadButtonPressed: down at this poll and not at the last poll the pad was there.
        bool padButtonPressed(int pad, int button) const
        {
            return padAvailable(pad) && padButtonInRange(button) && m_padButtonBefore[pad][button] == 0 &&
                   m_padButtonNow[pad][button] == 1;
        }
        // rcore.c:3314-3325, GetGamepadAxisMovement. Without the pad, or for an axis outside 0..7: 0 for a stick axis
        // and -1 for the two triggers (a trigger at rest). With the pad: the stored value; a trigger below -1 reads
        // -1. There is no dead zone. Axes 6 and 7 exist in raylib's table and are never written: 0.
        float padAxis(int pad, int axis) const
        {
            using namespace input_numbers;
            float value = (axis == kPadAxisLeftTrigger || axis == kPadAxisRightTrigger) ? -1.0f : 0.0f;
            if (padAvailable(pad) && axis >= 0 && axis < kMaxPadAxes)
            {
                const float stored = m_padAxis[pad][axis];
                const float movement = value < 0.0f ? stored : std::fabs(stored);
                if (movement > value)
                    value = stored;
            }
            return value;
        }

        // rcore.c:3349-3359, IsMouseButtonDown. (raylib does not check the number; outside 0..7 is "no" here. Its
        // touch table, which the same GLFW callback fills with the same value, adds nothing on a desktop.)
        bool mouseButtonDown(int button) const
        {
            return button >= 0 && button < input_numbers::kMaxMouseButtons && m_mouseButtonNow[button] == 1;
        }
        // rcore.c:3438-3446, GetMouseWheelMove: of the sideways and the up/down amount the larger one, up/down
        // when they are equal.
        float mouseWheel() const { return std::fabs(m_wheelX) > std::fabs(m_wheelY) ? m_wheelX : m_wheelY; }
        // rcore.c:3413-3421, GetMouseDelta: the position now minus the position at the start of this poll. The
        // position starts at 0,0 (rcore.c:663), so the first movement a window reports counts from there.
        void mouseDelta(float &x, float &y) const
        {
            x = m_mouseNowX - m_mouseBeforeX;
            y = m_mouseNowY - m_mouseBeforeY;
        }
        // rcore.c:779-782, IsWindowFocused: "yes" from the start, changed only by focus events.
        bool focused() const { return !m_unfocused; }

    private:
        static bool keyInRange(int key) { return key > 0 && key < input_numbers::kMaxKeys; }
        static bool padButtonInRange(int button) { return button >= 0 && button < input_numbers::kMaxPadButtons; }

        // raylib's CORE.Input (rcore.c:304-353), all zero at the start (rcore.c:663).
        char m_keyNow[input_numbers::kMaxKeys] = {};
        char m_keyBefore[input_numbers::kMaxKeys] = {};
        char m_keyRepeat[input_numbers::kMaxKeys] = {};
        char m_mouseButtonNow[input_numbers::kMaxMouseButtons] = {};
        float m_wheelX = 0.0f, m_wheelY = 0.0f;
        float m_mouseNowX = 0.0f, m_mouseNowY = 0.0f;
        float m_mouseBeforeX = 0.0f, m_mouseBeforeY = 0.0f;
        bool m_captured = false;
        bool m_unfocused = false;
        bool m_padReady[input_numbers::kMaxPads] = {};
        char m_padButtonNow[input_numbers::kMaxPads][input_numbers::kMaxPadButtons] = {};
        char m_padButtonBefore[input_numbers::kMaxPads][input_numbers::kMaxPadButtons] = {};
        float m_padAxis[input_numbers::kMaxPads][input_numbers::kMaxPadAxes] = {};
    };
} // namespace ps2x::host
