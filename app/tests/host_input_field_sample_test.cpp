// One controller sample per guest field (issue 023 5c): a host-input script is evaluated for the field of the game's
// pad read, presses last exactly the fields the script names (no real-time tap hold), and every read within one field
// returns the sample taken at the first read of that field.
#include "runtime/ps2_pad.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace ps2x_dev
{
    extern std::atomic<uint32_t> g_padPressedMask;
}

namespace {
int failures = 0;
void check(bool ok, const std::string &what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    failures += ok ? 0 : 1;
}

constexpr uint16_t kStart = 0x0008u, kCross = 0x4000u, kSquare = 0x8000u, kCircle = 0x2000u;

uint16_t pressedAt(PSPadBackend &pad, uint64_t field)
{
    uint8_t data[32]{};
    if (!pad.readState(0, 0, data, sizeof(data), field))
        return 0xFFFFu;
    return static_cast<uint16_t>(~(data[2] | (data[3] << 8)));
}
} // namespace

int main()
{
#ifdef _WIN32
    _putenv_s("PS2X_HOST_INPUT", "10:X:3,20:ENTER:1,30:Z:1");
#else
    setenv("PS2X_HOST_INPUT", "10:X:3,20:ENTER:1,30:Z:1", 1);
#endif
    PSPadBackend pad;
    check(!(pressedAt(pad, 9) & kCross), "X not pressed in field 9");
    check((pressedAt(pad, 10) & kCross) && (pressedAt(pad, 11) & kCross) && (pressedAt(pad, 12) & kCross),
          "X pressed in fields 10, 11 and 12 (10:X:3)");
    check(!(pressedAt(pad, 13) & kCross), "X released in field 13: no real-time hold for a script");
    check((pressedAt(pad, 20) & kStart) && (pressedAt(pad, 20) & kStart), "Start pressed in field 20, every read");
    check(!(pressedAt(pad, 21) & kStart), "Start released in field 21 (20:ENTER:1)");
    check((pressedAt(pad, 30) & kSquare) && !(pressedAt(pad, 31) & kSquare), "Square only in field 30");

    // A change between two reads of the same field reaches the game in the next field, not mid-field.
    check(!(pressedAt(pad, 40) & kCircle), "Circle not pressed in field 40 (first read)");
    ps2x_dev::g_padPressedMask.store(kCircle, std::memory_order_relaxed);
    check(!(pressedAt(pad, 40) & kCircle), "a press arriving mid-field is not seen by a later read of field 40");
    check((pressedAt(pad, 41) & kCircle) != 0, "the press is seen in field 41");
    ps2x_dev::g_padPressedMask.store(0u, std::memory_order_relaxed);
    check((pressedAt(pad, 41) & kCircle) != 0, "a release arriving mid-field is not seen by a later read of field 41");
    check(!(pressedAt(pad, 42) & kCircle), "the release is seen in field 42");

    std::printf("%s\n", failures ? "host_input_field_sample: FAIL" : "host_input_field_sample: PASS");
    return failures ? 1 : 0;
}
