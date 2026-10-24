#ifndef PS2_PAD_H
#define PS2_PAD_H

#include <cstddef>
#include <cstdint>

class PSPadBackend
{
public:
    PSPadBackend() = default;
    ~PSPadBackend() = default;

    // field: the guest field (VBlank count) of the game's pad read.
    bool readState(int port, int slot, uint8_t *data, size_t size, uint64_t field);
};

#endif
