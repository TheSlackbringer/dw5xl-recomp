// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// DW5XL (issue 049, amendment v3.12 step 3): the keys, the mouse and the pads of the game's window behind one small
// interface, so that the code which reads them is the same whichever window shows the game. Today's window answers
// through raylib, call for call as that code asked raylib before.
// Key, pad button and pad axis numbers are raylib's (its key numbers are GLFW's own).
namespace ps2x::host
{
    struct Devices
    {
        virtual ~Devices() = default;
        virtual bool ready() const = 0;                              // a window exists and can be asked
        virtual bool focused() const = 0;
        virtual double seconds() const = 0;                          // the window's own clock
        virtual bool keyDown(int key) const = 0;
        virtual bool keyPressed(int key) const = 0;                  // went down since the window last polled
        virtual bool keyPressedRepeat(int key) const = 0;            // the system's key repeat
        virtual bool padAvailable(int pad) const = 0;
        virtual bool padButtonDown(int pad, int button) const = 0;
        virtual bool padButtonPressed(int pad, int button) const = 0;
        virtual float padAxis(int pad, int axis) const = 0;
        virtual bool mouseButtonDown(int button) const = 0;
        virtual float mouseWheel() const = 0;
        virtual void mouseDelta(float &x, float &y) const = 0;
        virtual void captureMouse(bool captured) = 0;                // hide the cursor and keep it in the window
    };

    // The devices of the window that shows the game: today's raylib window unless another was set.
    Devices &devices();
    // nullptr: today's raylib window again. The object must outlive its use.
    void setDevices(Devices *replacement);
} // namespace ps2x::host
