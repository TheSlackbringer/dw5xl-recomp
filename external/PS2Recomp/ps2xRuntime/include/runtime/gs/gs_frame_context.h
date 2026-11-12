#ifndef PS2_GS_FRAME_CONTEXT_H
#define PS2_GS_FRAME_CONTEXT_H

// DW5XL: when a flush of a GS memory moves the graphics device on to its next frame context (issue 049).
// The device keeps a ring of frame contexts (the command memory and the things to free of one stretch of work).
// A flush of a GS memory moves on to the next one, and before a context is used again the library (Granite) waits,
// without a limit, until the graphics card has finished everything that context last held. Measured 2026-11-12
// at 4x: that wait held the GS thread 2-4 ms inside a picture's read (its flush, or paraLLEl-GS's vsync), with the
// game's packets waiting behind it (amendment v3.10: a picture's flush does not wait for the graphics card). So a
// flush of the pictures' memory moves on only when that does not wait; otherwise the work stays in the current
// context and a later flush moves on. The game's memory moves on as it always did. The choice is here so that it
// can be tested without a graphics card.

namespace ps2x::gs
{
    enum class Memory { Game, Pictures };

    // Whether a flush of `memory` leaves the frame context alone (true; a later flush moves on) or moves on (false).
    //  otherOpen:     another GS memory of the device has command buffers open. Granite waits for every open
    //                 command buffer when a frame context ends, so the flush leaves it alone then (v3.7).
    //  nextWouldWait: asks the device whether moving on would wait for the graphics card. Asked only when the
    //                 answer is needed and never while otherOpen: the question itself waits for open command buffers.
    template <class NextWouldWait>
    inline bool holdFrameContext(Memory memory, bool otherOpen, NextWouldWait &&nextWouldWait)
    {
        if (otherOpen)
            return true;
        return memory == Memory::Pictures && nextWouldWait();
    }
}

#endif
