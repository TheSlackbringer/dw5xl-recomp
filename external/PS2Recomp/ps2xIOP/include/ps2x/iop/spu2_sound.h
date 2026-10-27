#pragma once
// DW5XL (issue 023 3S): a sound the SPU2 started, captured at its key-on, and replayed on its own. A host screen
// that pauses the whole game (sound processor included) plays the game's own menu sounds this way.
#include <cstdint>
#include <span>
#include <vector>

namespace ps2x::iop
{
    struct Spu2KeyOn
    {
        unsigned core = 0, voice = 0;
        uint16_t volumeLeft = 0, volumeRight = 0, pitch = 0, adsr1 = 0, adsr2 = 0;
        bool dryLeft = false, dryRight = false;         // the voice reaches its core's dry output
        int32_t gainLeft = 0x8000, gainRight = 0x8000;  // from the voice to the speakers after its volume (0x8000 = 1)
        bool loops = false;                             // the sample repeats (a stream, a held sound)
        std::vector<uint8_t> adpcm;                     // 16-byte blocks from the start through the end block
    };

    // The voices together as the SPU2 plays them (dry, no reverb): 48 kHz interleaved stereo, up to maxFrames,
    // ending when every voice has ended.
    std::vector<int16_t> renderSpu2KeyOns(std::span<const Spu2KeyOn> voices, uint32_t maxFrames);
}
