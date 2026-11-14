// DW5XL-TEST issue 049 (amendment v3.12 step 3 part 4), CTest host_input_state.
// Issue 049: the keys, the mouse and the pads of a window made with GLFW directly, kept the way raylib 5.5 keeps
// them (runtime/host_input_state.h). No window, no GLFW, no raylib, no real device and no game data: the events are made
// up here, and every number below is written out from raylib.h and glfw3.h, not taken from the header under test.
//  - a key is "pressed" for exactly one poll and "down" until it is let go; a press and release within one poll
//    is never seen; the system's key repeat; Caps Lock and Num Lock held down while the lock is on; key numbers
//    outside raylib's table;
//  - mouse buttons; the wheel (the larger of sideways and up/down, the last event of a poll); the mouse movement
//    over several polls and across catching the mouse in the window and letting it go;
//  - the focus;
//  - pads: one appearing, every raylib button from the right GLFW button, both triggers as buttons at raylib's
//    threshold, the axes with raylib's values for "no pad", a joystick GLFW has no pad layout for, a pad going
//    away and coming back, pad, button and axis numbers outside the tables.
// A poll is made in raylib's order: the start, the four pad places, then GLFW's events.
#include "runtime/host_input_state.h"

#include <climits>
#include <cmath>
#include <cstdio>
#include <string>

namespace
{
using ps2x::host::InputState;
using ps2x::host::RawPad;

int checks = 0;
int failures = 0;

void check(bool ok, const char *what)
{
    ++checks;
    if (!ok)
    {
        ++failures;
        std::printf("FAIL  %s\n", what);
    }
}

void check(bool ok, const std::string &what) { check(ok, what.c_str()); }

// GLFW's numbers (glfw3.h).
constexpr int kRelease = 0, kPress = 1, kRepeat = 2;
constexpr int kKeyC = 67, kKeyX = 88, kKeyEnter = 257, kKeyCapsLock = 280, kKeyNumLock = 282, kKeyLast = 348;
constexpr int kModShift = 0x0001, kModCapsLock = 0x0010, kModNumLock = 0x0020;
// raylib's numbers (raylib.h): the two second triggers, and the trigger axes.
constexpr int kLeftTrigger2 = 10, kRightTrigger2 = 12;
constexpr int kAxisLeftX = 0, kAxisLeftY = 1, kAxisRightX = 2, kAxisRightY = 3, kAxisLeftTrigger = 4,
              kAxisRightTrigger = 5;

// A pad GLFW knows the layout of, nothing touched: the triggers rest at -1.
RawPad restingPad()
{
    RawPad pad{};
    pad.axes[4] = -1.0f;
    pad.axes[5] = -1.0f;
    return pad;
}

// The start of a poll with what the four pad places hold (nullptr: no pad there). GLFW's events follow it.
void startPoll(InputState &state, const RawPad *pad0 = nullptr, const RawPad *pad1 = nullptr,
               const RawPad *pad2 = nullptr, const RawPad *pad3 = nullptr)
{
    const RawPad *pads[4] = {pad0, pad1, pad2, pad3};
    state.beginPoll();
    for (int index = 0; index < 4; ++index)
        state.pad(index, pads[index] != nullptr, pads[index]);
}

int padButtonsDown(const InputState &state, int pad)
{
    int down = 0;
    for (int button = -4; button < 40; ++button)
        down += state.padButtonDown(pad, button) ? 1 : 0;
    return down;
}

int keysDown(const InputState &state)
{
    int down = 0;
    for (int key = -4; key < 600; ++key)
        down += state.keyDown(key) ? 1 : 0;
    return down;
}

bool movedBy(const InputState &state, float x, float y)
{
    float dx = 99.0f, dy = 99.0f;
    state.mouseDelta(dx, dy);
    return dx == x && dy == y;
}

void keys()
{
    InputState s;
    check(keysDown(s) == 0 && !s.keyPressed(kKeyX) && !s.keyPressedRepeat(kKeyX), "at the start no key is down");

    startPoll(s);
    s.key(kKeyX, kPress);
    check(s.keyDown(kKeyX) && s.keyPressed(kKeyX), "the poll a key goes down in: it is down and pressed");
    check(!s.keyPressedRepeat(kKeyX), "the press itself is not a repeat");
    check(keysDown(s) == 1 && !s.keyDown(kKeyC) && !s.keyPressed(kKeyC), "no other key is down or pressed");
    startPoll(s);
    check(s.keyDown(kKeyX) && !s.keyPressed(kKeyX), "the next poll: still down, no longer pressed");
    startPoll(s);
    startPoll(s);
    check(s.keyDown(kKeyX) && !s.keyPressed(kKeyX), "held over more polls: down, not pressed");
    s.key(kKeyX, kRepeat);
    check(s.keyPressedRepeat(kKeyX) && s.keyDown(kKeyX) && !s.keyPressed(kKeyX),
          "the system repeats the key: a repeat in that poll, still down, not pressed again");
    startPoll(s);
    check(!s.keyPressedRepeat(kKeyX) && s.keyDown(kKeyX), "the poll after: no repeat, still down");
    s.key(kKeyX, kRelease);
    check(!s.keyDown(kKeyX) && !s.keyPressed(kKeyX), "let go: not down, not pressed");
    startPoll(s);
    check(!s.keyDown(kKeyX) && !s.keyPressed(kKeyX), "and it stays so");

    startPoll(s);
    s.key(kKeyC, kPress);
    s.key(kKeyC, kRelease);
    check(!s.keyDown(kKeyC) && !s.keyPressed(kKeyC), "pressed and let go within one poll: raylib never sees it");
    startPoll(s);
    check(!s.keyDown(kKeyC) && !s.keyPressed(kKeyC), "nor in the poll after");

    startPoll(s);
    s.key(kKeyC, kPress);
    s.key(kKeyC, kRelease);
    s.key(kKeyC, kPress);
    check(s.keyDown(kKeyC) && s.keyPressed(kKeyC),
          "pressed, let go and pressed again within one poll: down and pressed");
    startPoll(s);
    s.key(kKeyC, kRelease);
    s.key(kKeyC, kPress);
    check(s.keyDown(kKeyC) && !s.keyPressed(kKeyC),
          "a held key let go and pressed again within one poll: down, but not pressed");
    startPoll(s);
    s.key(kKeyC, kRelease);

    startPoll(s);
    s.key(kKeyEnter, kRepeat);
    check(s.keyPressedRepeat(kKeyEnter) && !s.keyDown(kKeyEnter) && !s.keyPressed(kKeyEnter),
          "a repeat alone marks the repeat and does not put the key down");

    startPoll(s);
    s.key(kKeyX, kPress);
    s.key(kKeyC, kPress);
    startPoll(s);
    s.key(kKeyX, kRelease);
    check(!s.keyDown(kKeyX) && s.keyDown(kKeyC) && keysDown(s) == 1, "two keys are kept apart");
    startPoll(s);
    s.key(kKeyC, kRelease);

    // Key numbers outside raylib's table (512 places; its questions answer from 1 to 511).
    startPoll(s);
    s.key(-1, kPress);
    s.key(512, kPress);
    s.key(100000, kPress);
    s.key(INT_MAX, kPress);
    s.key(INT_MIN, kPress);
    check(keysDown(s) == 0, "a press of key -1, 512 or far outside changes nothing");
    check(!s.keyDown(-1) && !s.keyDown(512) && !s.keyDown(INT_MAX) && !s.keyDown(INT_MIN) && !s.keyPressed(-1) &&
              !s.keyPressed(512) && !s.keyPressedRepeat(-1) && !s.keyPressedRepeat(512),
          "asking for a key outside the table answers no");
    s.key(0, kPress);
    s.key(0, kRepeat);
    check(!s.keyDown(0) && !s.keyPressed(0) && !s.keyPressedRepeat(0), "key 0 is never down, pressed or repeated");
    s.key(511, kPress);
    s.key(kKeyLast, kPress);
    s.key(1, kPress);
    check(s.keyDown(511) && s.keyPressed(511) && s.keyDown(kKeyLast) && s.keyDown(1) && keysDown(s) == 3,
          "the first key (1), GLFW's last (348) and the table's last (511) work");

    // Caps Lock and Num Lock: down for as long as the lock is on.
    InputState locks;
    startPoll(locks);
    locks.key(kKeyCapsLock, kPress, 0);
    startPoll(locks);
    locks.key(kKeyCapsLock, kRelease, kModCapsLock);
    check(locks.keyDown(kKeyCapsLock), "Caps Lock let go with the lock on stays down");
    startPoll(locks);
    locks.key(kKeyCapsLock, kPress, 0);
    startPoll(locks);
    locks.key(kKeyCapsLock, kRelease, 0);
    check(!locks.keyDown(kKeyCapsLock), "Caps Lock let go with the lock off is up");
    startPoll(locks);
    locks.key(kKeyNumLock, kRelease, kModNumLock);
    check(locks.keyDown(kKeyNumLock) && locks.keyPressed(kKeyNumLock), "Num Lock let go with the lock on is down");
    startPoll(locks);
    locks.key(kKeyNumLock, kRelease, kModCapsLock | kModShift);
    check(!locks.keyDown(kKeyNumLock), "Num Lock does not follow the Caps Lock state");
    locks.key(kKeyX, kRelease, kModCapsLock | kModNumLock);
    check(!locks.keyDown(kKeyX), "another key let go with the locks on is up");
}

void mouse()
{
    InputState s;
    check(!s.mouseButtonDown(0) && !s.mouseButtonDown(1) && !s.mouseButtonDown(2),
          "at the start no mouse button is down");
    startPoll(s);
    s.mouseButton(0, kPress);
    check(s.mouseButtonDown(0) && !s.mouseButtonDown(1) && !s.mouseButtonDown(2),
          "left button pressed: it is down, the others are not");
    startPoll(s);
    startPoll(s);
    check(s.mouseButtonDown(0), "it stays down over the polls");
    s.mouseButton(1, kPress);
    s.mouseButton(0, kRelease);
    check(!s.mouseButtonDown(0) && s.mouseButtonDown(1), "left let go and right pressed: only the right one is down");
    s.mouseButton(2, kPress);
    s.mouseButton(7, kPress);
    check(s.mouseButtonDown(2) && s.mouseButtonDown(7), "the middle button and GLFW's last button (7) work");
    s.mouseButton(8, kPress);
    s.mouseButton(-1, kPress);
    s.mouseButton(INT_MAX, kPress);
    check(!s.mouseButtonDown(8) && !s.mouseButtonDown(-1) && !s.mouseButtonDown(INT_MAX) && !s.mouseButtonDown(INT_MIN),
          "a mouse button outside 0..7 is never down");
    s.mouseButton(3, 2);
    check(!s.mouseButtonDown(3), "an action that is neither press nor release does not count as down");

    // The wheel.
    InputState w;
    check(w.mouseWheel() == 0.0f, "at the start the wheel reads 0");
    startPoll(w);
    w.scrolled(0.0, 1.0);
    check(w.mouseWheel() == 1.0f, "one notch up reads 1");
    startPoll(w);
    check(w.mouseWheel() == 0.0f, "the poll after, without a wheel event, it reads 0 again");
    w.scrolled(0.0, -2.0);
    check(w.mouseWheel() == -2.0f, "two notches down read -2");
    startPoll(w);
    w.scrolled(3.0, 1.0);
    check(w.mouseWheel() == 3.0f, "sideways 3 and up 1: the larger one, 3");
    startPoll(w);
    w.scrolled(-3.0, 1.0);
    check(w.mouseWheel() == -3.0f, "sideways -3 and up 1: the larger one by size, -3");
    startPoll(w);
    w.scrolled(1.0, -3.0);
    check(w.mouseWheel() == -3.0f, "sideways 1 and down 3: -3");
    startPoll(w);
    w.scrolled(2.0, -2.0);
    check(w.mouseWheel() == -2.0f, "equal sizes: the up/down one");
    startPoll(w);
    w.scrolled(0.0, 1.0);
    w.scrolled(0.0, 1.0);
    check(w.mouseWheel() == 1.0f, "two wheel events within one poll: the last one counts, they are not added");
    startPoll(w);
    w.scrolled(0.0, 5.0);
    w.scrolled(1.0, 0.0);
    check(w.mouseWheel() == 1.0f, "and the first one is forgotten altogether");
    startPoll(w);
    w.scrolled(0.0, 0.5);
    check(w.mouseWheel() == 0.5f, "part of a notch is kept as it is");

    // The movement.
    InputState m;
    check(movedBy(m, 0.0f, 0.0f), "at the start the mouse has not moved");
    startPoll(m);
    check(movedBy(m, 0.0f, 0.0f), "a poll without mouse events: no movement");
    m.mouseMoved(100.0, 50.0);
    check(movedBy(m, 100.0f, 50.0f), "the first position counts from 0,0, as in raylib");
    startPoll(m);
    check(movedBy(m, 0.0f, 0.0f), "the poll after, the mouse at rest: no movement");
    startPoll(m);
    m.mouseMoved(110.0, 55.0);
    m.mouseMoved(130.0, 40.0);
    check(movedBy(m, 30.0f, -10.0f), "two positions within one poll: from where the poll started to the last one");
    check(movedBy(m, 30.0f, -10.0f), "asking twice gives the same");
    startPoll(m);
    m.mouseMoved(130.5, 40.25);
    check(movedBy(m, 0.5f, 0.25f), "parts of a pixel are kept");
    startPoll(m);
    m.mouseMoved(120.0, 40.25);
    startPoll(m);
    m.mouseMoved(100.0, 60.25);
    check(movedBy(m, -20.0f, 20.0f), "every poll counts from the poll before");

    // Catching the mouse in the window and letting it go (raylib's DisableCursor and EnableCursor).
    check(!m.mouseIsCaptured(), "at the start the mouse is not caught");
    startPoll(m);
    m.mouseMoved(300.0, 200.0);
    m.mouseCaptured(true, 1280, 960);
    check(m.mouseIsCaptured(), "caught");
    check(movedBy(m, 0.0f, 0.0f), "catching the mouse wipes the movement of that poll");
    check(m.mouseX() == 640.0f && m.mouseY() == 480.0f, "and puts the position in the middle of the window");
    startPoll(m);
    check(movedBy(m, 0.0f, 0.0f), "caught and at rest: no movement");
    m.mouseMoved(650.0, 470.0);
    check(movedBy(m, 10.0f, -10.0f), "caught: the movement counts from the middle");
    startPoll(m);
    m.mouseMoved(700.0, 500.0);
    check(movedBy(m, 50.0f, 30.0f), "caught, the next poll: from the poll before");
    startPoll(m);
    m.mouseMoved(5000.0, -3000.0);
    check(movedBy(m, 4300.0f, -3500.0f), "caught, the position may leave the window");
    m.mouseCaptured(false, 1280, 960);
    check(!m.mouseIsCaptured() && movedBy(m, 0.0f, 0.0f) && m.mouseX() == 640.0f && m.mouseY() == 480.0f,
          "letting go does the same: no movement, the position in the middle");
    startPoll(m);
    m.mouseMoved(641.0, 480.0);
    check(movedBy(m, 1.0f, 0.0f), "after letting go the movement counts from the middle");
    m.mouseCaptured(true, 1281, 721);
    check(m.mouseX() == 640.0f && m.mouseY() == 360.0f, "a window of 1281x721: the middle is 640,360, halves dropped");
    m.mouseCaptured(true, 0, 0);
    check(m.mouseX() == 0.0f && m.mouseY() == 0.0f && movedBy(m, 0.0f, 0.0f),
          "a window of size zero: the middle is 0,0");
}

void focus()
{
    InputState s;
    check(s.focused(), "at the start the window counts as focused, as in raylib");
    startPoll(s);
    check(s.focused(), "a poll does not change the focus");
    s.key(kKeyX, kPress);
    s.focus(false);
    check(!s.focused(), "focus lost");
    check(s.keyDown(kKeyX), "losing the focus does not let keys go by itself (GLFW sends the releases)");
    startPoll(s);
    check(!s.focused(), "it stays lost over a poll");
    s.focus(true);
    check(s.focused(), "focus back");
}

void pads()
{
    InputState s;
    const RawPad resting = restingPad();

    bool none = true;
    for (int pad = 0; pad < 4; ++pad)
        none = none && !s.padAvailable(pad) && padButtonsDown(s, pad) == 0;
    check(none, "at the start there is no pad and no pad button is down");
    check(s.padAxis(0, kAxisLeftX) == 0.0f && s.padAxis(0, kAxisLeftY) == 0.0f && s.padAxis(0, kAxisRightX) == 0.0f &&
              s.padAxis(0, kAxisRightY) == 0.0f,
          "no pad: the stick axes read 0");
    check(s.padAxis(0, kAxisLeftTrigger) == -1.0f && s.padAxis(0, kAxisRightTrigger) == -1.0f,
          "no pad: the two trigger axes read -1, a trigger at rest");

    startPoll(s, &resting);
    check(s.padAvailable(0) && !s.padAvailable(1) && !s.padAvailable(2) && !s.padAvailable(3),
          "a pad appears at place 0 only");
    check(padButtonsDown(s, 0) == 0, "nothing touched: no button is down");
    check(s.padAxis(0, kAxisLeftX) == 0.0f && s.padAxis(0, kAxisRightY) == 0.0f &&
              s.padAxis(0, kAxisLeftTrigger) == -1.0f && s.padAxis(0, kAxisRightTrigger) == -1.0f,
          "nothing touched: sticks 0, triggers -1");

    // Every button of GLFW's pad as raylib's button.
    const struct
    {
        int glfw;
        int raylib;
        const char *name;
    } buttons[] = {{0, 7, "GLFW A (0) is raylib RIGHT_FACE_DOWN (7)"},
                   {1, 6, "GLFW B (1) is raylib RIGHT_FACE_RIGHT (6)"},
                   {2, 8, "GLFW X (2) is raylib RIGHT_FACE_LEFT (8)"},
                   {3, 5, "GLFW Y (3) is raylib RIGHT_FACE_UP (5)"},
                   {4, 9, "GLFW left bumper (4) is raylib LEFT_TRIGGER_1 (9)"},
                   {5, 11, "GLFW right bumper (5) is raylib RIGHT_TRIGGER_1 (11)"},
                   {6, 13, "GLFW back (6) is raylib MIDDLE_LEFT (13)"},
                   {7, 15, "GLFW start (7) is raylib MIDDLE_RIGHT (15)"},
                   {8, 14, "GLFW guide (8) is raylib MIDDLE (14)"},
                   {9, 16, "GLFW left stick pressed (9) is raylib LEFT_THUMB (16)"},
                   {10, 17, "GLFW right stick pressed (10) is raylib RIGHT_THUMB (17)"},
                   {11, 1, "GLFW d-pad up (11) is raylib LEFT_FACE_UP (1)"},
                   {12, 2, "GLFW d-pad right (12) is raylib LEFT_FACE_RIGHT (2)"},
                   {13, 3, "GLFW d-pad down (13) is raylib LEFT_FACE_DOWN (3)"},
                   {14, 4, "GLFW d-pad left (14) is raylib LEFT_FACE_LEFT (4)"}};
    bool reached[18] = {};
    for (const auto &b : buttons)
    {
        RawPad held = restingPad();
        held.buttons[b.glfw] = 1;
        startPoll(s, &held);
        check(s.padButtonDown(0, b.raylib) && padButtonsDown(s, 0) == 1, std::string(b.name) + ", and no other button");
        check(s.padButtonPressed(0, b.raylib), std::string(b.name) + ": pressed in the poll it goes down in");
        startPoll(s, &held);
        check(s.padButtonDown(0, b.raylib) && !s.padButtonPressed(0, b.raylib),
              std::string(b.name) + ": the next poll still down, no longer pressed");
        startPoll(s, &resting);
        check(!s.padButtonDown(0, b.raylib) && !s.padButtonPressed(0, b.raylib) && padButtonsDown(s, 0) == 0,
              std::string(b.name) + ": let go, nothing is down");
        reached[b.raylib] = true;
    }
    reached[kLeftTrigger2] = reached[kRightTrigger2] = true; // from the trigger axes, below
    bool all = !reached[0];
    for (int button = 1; button <= 17; ++button)
        all = all && reached[button];
    check(all, "with the two triggers that is every raylib button 1..17, each once, and never button 0");

    RawPad odd = restingPad();
    odd.buttons[0] = 2;
    startPoll(s, &odd);
    check(padButtonsDown(s, 0) == 0, "a button value other than 1 does not count as down");

    // The triggers as buttons: down while the axis is above 0.1 (the axis rests at -1 and ends at 1).
    const struct
    {
        int axis;
        int button;
        int other;
        const char *name;
    } triggers[] = {{4, kLeftTrigger2, kRightTrigger2, "left trigger (axis 4) as LEFT_TRIGGER_2 (10)"},
                    {5, kRightTrigger2, kLeftTrigger2, "right trigger (axis 5) as RIGHT_TRIGGER_2 (12)"}};
    for (const auto &t : triggers)
    {
        const std::string name = t.name;
        RawPad pulled = restingPad();
        pulled.axes[t.axis] = 0.0f;
        startPoll(s, &pulled);
        check(!s.padButtonDown(0, t.button), name + ": half pulled (0) is not down");
        pulled.axes[t.axis] = 0.1f;
        startPoll(s, &pulled);
        check(!s.padButtonDown(0, t.button), name + ": at 0.1 exactly it is not down");
        pulled.axes[t.axis] = std::nextafter(0.1f, 1.0f);
        startPoll(s, &pulled);
        check(s.padButtonDown(0, t.button) && s.padButtonPressed(0, t.button),
              name + ": just above 0.1 it is down and pressed");
        check(!s.padButtonDown(0, t.other) && padButtonsDown(s, 0) == 1,
              name + ": the other trigger and all else stay up");
        pulled.axes[t.axis] = 1.0f;
        startPoll(s, &pulled);
        check(s.padButtonDown(0, t.button) && !s.padButtonPressed(0, t.button),
              name + ": fully pulled, down, pressed only once");
        check(s.padAxis(0, t.axis) == 1.0f, name + ": the axis itself reads 1");
        pulled.axes[t.axis] = 0.09f;
        startPoll(s, &pulled);
        check(!s.padButtonDown(0, t.button), name + ": at 0.09 it is up again");
        startPoll(s, &resting);
        check(!s.padButtonDown(0, t.button), name + ": at rest (-1) it is up");
    }

    RawPad everything = restingPad();
    for (unsigned char &button : everything.buttons)
        button = 1;
    everything.axes[4] = everything.axes[5] = 1.0f;
    startPoll(s, &everything);
    check(padButtonsDown(s, 0) == 17 && !s.padButtonDown(0, 0),
          "everything held: the 17 buttons 1..17 are down, button 0 is not");
    startPoll(s, &resting);

    // The axes: as GLFW gives them, no dead zone.
    RawPad sticks = restingPad();
    sticks.axes[0] = 0.5f;
    sticks.axes[1] = -0.25f;
    sticks.axes[2] = 1.0f;
    sticks.axes[3] = -1.0f;
    sticks.axes[4] = 0.3f;
    sticks.axes[5] = -0.5f;
    startPoll(s, &sticks);
    check(s.padAxis(0, kAxisLeftX) == 0.5f && s.padAxis(0, kAxisLeftY) == -0.25f,
          "left stick: 0.5 and -0.25 as they come");
    check(s.padAxis(0, kAxisRightX) == 1.0f && s.padAxis(0, kAxisRightY) == -1.0f,
          "right stick: 1 and -1 as they come");
    check(s.padAxis(0, kAxisLeftTrigger) == 0.3f && s.padAxis(0, kAxisRightTrigger) == -0.5f,
          "triggers: 0.3 and -0.5 as they come");
    sticks.axes[0] = 0.001f;
    sticks.axes[1] = -0.001f;
    startPoll(s, &sticks);
    check(s.padAxis(0, kAxisLeftX) == 0.001f && s.padAxis(0, kAxisLeftY) == -0.001f,
          "a stick a hair off centre is not rounded to 0");
    sticks.axes[0] = -0.0f;
    sticks.axes[4] = -2.0f;
    startPoll(s, &sticks);
    check(s.padAxis(0, kAxisLeftX) == 0.0f && !std::signbit(s.padAxis(0, kAxisLeftX)),
          "a stick at minus zero reads plain 0");
    check(s.padAxis(0, kAxisLeftTrigger) == -1.0f, "a trigger below -1 reads -1");
    check(s.padAxis(0, 6) == 0.0f && s.padAxis(0, 7) == 0.0f, "raylib's axes 6 and 7 are never written: 0");
    check(s.padAxis(0, 8) == 0.0f && s.padAxis(0, -1) == 0.0f && s.padAxis(0, INT_MAX) == 0.0f &&
              s.padAxis(0, INT_MIN) == 0.0f,
          "an axis outside the table reads 0");
    check(!s.padButtonDown(0, 32) && !s.padButtonDown(0, -1) && !s.padButtonDown(0, INT_MAX) &&
              !s.padButtonPressed(0, 32) && !s.padButtonPressed(0, -1) && !s.padButtonPressed(0, INT_MIN),
          "a button outside the table is never down or pressed");

    // A joystick GLFW has no pad layout for: it is there, and GLFW hands out all zeros.
    InputState j;
    const RawPad zeros{};
    startPoll(j, &zeros);
    check(j.padAvailable(0) && padButtonsDown(j, 0) == 0,
          "a joystick without a pad layout counts as a pad with nothing down");
    check(j.padAxis(0, kAxisLeftX) == 0.0f && j.padAxis(0, kAxisLeftTrigger) == 0.0f &&
              j.padAxis(0, kAxisRightTrigger) == 0.0f,
          "its axes read 0, the triggers too (not -1)");

    // A pad going away and coming back.
    InputState a;
    RawPad heldA = restingPad();
    heldA.buttons[0] = 1;
    heldA.axes[0] = 0.75f;
    heldA.axes[4] = 1.0f;
    startPoll(a, &heldA);
    check(a.padButtonDown(0, 7) && a.padButtonPressed(0, 7) && a.padButtonDown(0, kLeftTrigger2),
          "a pad appearing with a button held: down and pressed in that poll");
    startPoll(a, nullptr);
    check(!a.padAvailable(0), "the pad is gone");
    check(padButtonsDown(a, 0) == 0 && !a.padButtonPressed(0, 7),
          "gone: no button is down or pressed, whatever was held");
    check(a.padAxis(0, kAxisLeftX) == 0.0f && a.padAxis(0, kAxisLeftTrigger) == -1.0f &&
              a.padAxis(0, kAxisRightTrigger) == -1.0f,
          "gone: sticks read 0 and triggers -1");
    startPoll(a, nullptr);
    startPoll(a, &heldA);
    check(a.padAvailable(0) && a.padButtonDown(0, 7) && a.padAxis(0, kAxisLeftX) == 0.75f,
          "back with the button still held: down");
    check(!a.padButtonPressed(0, 7) && !a.padButtonPressed(0, kLeftTrigger2),
          "but not pressed: raylib kept the buttons of the poll before it went away");
    startPoll(a, nullptr);
    RawPad heldB = restingPad();
    heldB.buttons[1] = 1;
    startPoll(a, &heldB);
    check(!a.padButtonDown(0, 7) && a.padButtonDown(0, 6) && a.padButtonPressed(0, 6) && padButtonsDown(a, 0) == 1,
          "back with another button held: that one is down and pressed, the old one is up");
    a.beginPoll();
    a.pad(0, true, nullptr);
    check(!a.padAvailable(0) && padButtonsDown(a, 0) == 0,
          "a place said to be there but without a state counts as no pad");
    a.beginPoll();
    a.pad(0, false, &heldB);
    check(!a.padAvailable(0) && padButtonsDown(a, 0) == 0,
          "a place said not to be there counts as no pad, whatever the state");

    // Places: 0..3, each on its own.
    InputState p;
    RawPad start = restingPad();
    start.buttons[7] = 1;
    startPoll(p, nullptr, nullptr, &start, &heldA);
    check(!p.padAvailable(0) && !p.padAvailable(1) && p.padAvailable(2) && p.padAvailable(3),
          "pads at places 2 and 3 only");
    check(p.padButtonDown(2, 15) && !p.padButtonDown(2, 7) && p.padButtonDown(3, 7) && !p.padButtonDown(3, 15) &&
              !p.padButtonDown(0, 15) && !p.padButtonDown(1, 7),
          "each place has its own buttons");
    check(p.padAxis(3, kAxisLeftX) == 0.75f && p.padAxis(2, kAxisLeftX) == 0.0f, "and its own axes");
    p.pad(4, true, &heldA);
    p.pad(-1, true, &heldA);
    p.pad(INT_MAX, true, &heldA);
    p.pad(INT_MIN, true, &heldA);
    check(!p.padAvailable(4) && !p.padAvailable(-1) && !p.padAvailable(INT_MAX) && !p.padAvailable(INT_MIN),
          "a pad at a place outside 0..3 is not taken");
    check(!p.padButtonDown(4, 7) && !p.padButtonPressed(4, 7) && !p.padButtonDown(-1, 7) && !p.padButtonPressed(-1, 7),
          "its buttons are never down or pressed");
    check(p.padAxis(4, kAxisLeftX) == 0.0f && p.padAxis(-1, kAxisLeftX) == 0.0f &&
              p.padAxis(4, kAxisLeftTrigger) == -1.0f && p.padAxis(-1, kAxisRightTrigger) == -1.0f,
          "its axes read as without a pad: sticks 0, triggers -1");
    check(p.padButtonDown(2, 15) && p.padButtonDown(3, 7), "and the pads that are there are untouched by it");

    // The start of a poll leaves the pads alone: only the reading of a pad moves its buttons on.
    InputState o;
    startPoll(o, &heldA);
    o.beginPoll();
    check(o.padAvailable(0) && o.padButtonDown(0, 7) && o.padButtonPressed(0, 7),
          "the start of a poll alone does not touch a pad: the pad's reading does");
    o.pad(0, true, &heldA);
    check(o.padButtonDown(0, 7) && !o.padButtonPressed(0, 7), "read again: down, no longer pressed");
}

void together()
{
    // One window, a few polls, everything at once.
    InputState s;
    RawPad pad = restingPad();
    startPoll(s, &pad);
    s.focus(true);
    s.key(kKeyX, kPress);
    s.mouseButton(0, kPress);
    s.mouseMoved(10.0, 20.0);
    s.scrolled(0.0, -1.0);
    check(s.focused() && s.keyPressed(kKeyX) && s.mouseButtonDown(0) && movedBy(s, 10.0f, 20.0f) &&
              s.mouseWheel() == -1.0f && s.padAvailable(0) && padButtonsDown(s, 0) == 0,
          "key, mouse button, movement, wheel and pad of one poll are all there");
    pad.buttons[3] = 1;
    startPoll(s, &pad);
    s.key(kKeyX, kRepeat);
    check(s.keyDown(kKeyX) && !s.keyPressed(kKeyX) && s.keyPressedRepeat(kKeyX) && s.mouseButtonDown(0) &&
              movedBy(s, 0.0f, 0.0f) && s.mouseWheel() == 0.0f && s.padButtonPressed(0, 5),
          "the next poll: key held and repeated, button held, no movement, no wheel, pad button pressed");
    startPoll(s, &pad);
    s.key(kKeyX, kRelease);
    s.mouseButton(0, kRelease);
    s.focus(false);
    check(!s.focused() && !s.keyDown(kKeyX) && !s.keyPressedRepeat(kKeyX) && !s.mouseButtonDown(0) &&
              s.padButtonDown(0, 5) && !s.padButtonPressed(0, 5),
          "the poll the focus goes in: key and button let go, pad button still down, not pressed");
}
} // namespace

int main()
{
    keys();
    mouse();
    focus();
    pads();
    together();

    if (failures)
        std::printf("FAIL: %d of %d checks\n", failures, checks);
    else
        std::printf("PASS: %d checks\n", checks);
    return failures ? 1 : 0;
}
