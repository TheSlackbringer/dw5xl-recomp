// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// DW5XL issue 066: the game's own store of compiled shader programs, as a file.
//
// The Vulkan renderer makes a shader program when it first needs it, in the middle of play. Whether that is quick
// depends on a folder of the graphics driver's, which the driver fills and empties as it likes. So the renderer
// keeps what was compiled in a file of its own: read when the renderer's device is made, written when the game
// ends after a run in which a compile held the renderer up.
//
// This is the file only: a short head of ours, then the renderer library's own data, which this code does not
// look into (the library checks that it was written for this graphics card and driver). No graphics code here.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ps2x::gs::shader_store
{
    enum class Read
    {
        Taken,        // the content is handed out
        NoFile,       // there is no such file (a first start)
        NotOurs,      // not a store, or one of another format number
        OtherShaders, // written by a program with other shaders: its programs cannot be asked for by this one
        Damaged,      // cut short, added to, or its content changed
    };
    // The store's content from `path` when the file is whole and was written for the shaders marked `mark`.
    // Anything else leaves `content` empty.
    Read read(const std::string &path, uint64_t mark, std::vector<uint8_t> &content);
    // Words for the log.
    const char *said(Read result);

    // Writes the store whole into `<path>.part` and renames that over `path`, so no reader ever finds half a file
    // and a run that dies while writing leaves the store that was there. False: it could not be written.
    bool write(const std::string &path, uint64_t mark, const uint8_t *content, size_t size);

    // A mark for a set of bytes (FNV-1a, 64 bits): the shaders' mark and the content's check sum.
    uint64_t markOf(const void *bytes, size_t size, uint64_t from = 14695981039346656037ull);

    // ---- Where the store lies, and its writing at the game's end (any thread) ----
    // The game names the file before its renderer is made (beside the settings file). Empty: no store.
    void setPath(const std::string &path);
    // The file in force: DW5XL-DEBUG PS2X_SHADER_STORE=<file> instead of the game's (tests and tools; `off`: none),
    // else what setPath named.
    std::string path();
    // The renderer says how its stores are written (`always`: even when no compile held it up; for tests).
    void setWriter(void (*writer)(bool always));
    // Writes the stores now. The renderer writes them itself when it ends; this is for where the game leaves
    // without the renderer's end (a test's last field, the developer's quit).
    void writeNow(bool always = false);
} // namespace ps2x::gs::shader_store
