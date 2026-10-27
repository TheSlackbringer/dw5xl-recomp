// CTest spu2_render (issue 023 3S): a sound captured at an SPU2 key-on is replayed on its own (the in-play PC SETTINGS
// screen plays the game's menu sounds this way). Synthetic one-shot sample only: two ADPCM blocks of a constant level,
// the second one the end block.
#include "ps2x/iop/spu2_sound.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace {
int g_failures = 0;
void check(bool ok, const char *what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}
ps2x::iop::Spu2KeyOn tone(bool loops)
{
    ps2x::iop::Spu2KeyOn k;
    k.volumeLeft = k.volumeRight = 0x3fff;      // fixed volume ~1.0
    k.pitch = 0x1000;                           // one sample per output frame
    k.adsr1 = 0x000f;                           // fastest attack, sustain level max
    k.adsr2 = 0x0000;                           // hold the sustain level
    k.dryLeft = k.dryRight = true;
    for (int block = 0; block < 2; ++block) {
        k.adpcm.push_back(0x00);                // shift 0, filter 0
        k.adpcm.push_back(block == 1 ? uint8_t(loops ? 0x03 : 0x01) : uint8_t(0x00));   // end (and repeat)
        for (int i = 0; i < 14; ++i) k.adpcm.push_back(0x33);   // nibbles 3: 3 * 4096 = 12288
    }
    return k;
}
}

int main()
{
    using ps2x::iop::renderSpu2KeyOns;
    const ps2x::iop::Spu2KeyOn k = tone(false);
    const std::vector<int16_t> pcm = renderSpu2KeyOns({&k, 1}, 48000);
    int peak = 0;
    size_t lastSound = 0;
    for (size_t i = 0; i < pcm.size(); ++i)
        if (std::abs(pcm[i]) > 0) { peak = std::max(peak, std::abs(int(pcm[i]))); lastSound = i / 2; }
    check(peak > 4000, "the sample sounds (constant level through the voice and core volumes)");
    check(lastSound >= 28 && lastSound < 200, "a one-shot sample ends after its end block");
    check(pcm.size() / 2 < 48000, "rendering stops once the voice has ended");
    ps2x::iop::Spu2KeyOn half = k;
    half.gainLeft = half.gainRight = 0x4000;
    const std::vector<int16_t> quiet = renderSpu2KeyOns({&half, 1}, 48000);
    int quietPeak = 0;
    for (int16_t v : quiet) quietPeak = std::max(quietPeak, std::abs(int(v)));
    check(std::abs(quietPeak * 2 - peak) <= 2, "the captured speaker gain scales the output");
    check(renderSpu2KeyOns({}, 48000).empty(), "no voices, no sound");
    std::printf("%s\n", g_failures ? "spu2_render: FAIL" : "spu2_render: PASS");
    return g_failures ? 1 : 0;
}
