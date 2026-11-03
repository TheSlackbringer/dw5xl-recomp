#pragma once
// Issue 009 v4.5: the IOP modules of the PAL disc (SLES_538.60) that dw5xl_ioplift translates. Names, sizes,
// FNV-1a 64 hashes of the disc files and the IOP RAM address each one is loaded at (the game loads the same modules
// in the same order on every boot). No game data.

#include <cstdint>

namespace dw5xl::ioplift
{
    struct IopModuleInput
    {
        const char *file;  // in the disc's MODULES folder
        const char *name;
        uint32_t fileSize;
        uint64_t fileHash; // FNV-1a 64 of the whole file
        uint32_t base;     // load address
    };

    inline constexpr IopModuleInput kIopModules[] = {
        {"SIO2MAN.IRX", "SIO2MAN", 6641u, 0x47F770C8A7916C8Cull, 0x10000u},
        {"MCMAN.IRX", "MCMAN", 96181u, 0xFCFE4F52AF36F511ull, 0x11000u},
        {"MCSERV.IRX", "MCSERV", 7385u, 0xB5D6ED9D7DAB56D0ull, 0x38400u},
        {"SIO2D.IRX", "SIO2D", 11289u, 0x2EF1FE9D3209C0F2ull, 0x3BC00u},
        {"DBCMAN.IRX", "DBCMAN", 15653u, 0x1FE49E03122198FBull, 0x3EA00u},
        {"DS2O_D.IRX", "DS2O_D", 11885u, 0x6F6E7FFC1AF400D2ull, 0x43C00u},
        {"LIBSD.IRX", "LIBSD", 30085u, 0x5C984764DA994F25ull, 0x46400u},
        {"SDHD.IRX", "SDHD", 14685u, 0x25FC734927C7A450ull, 0x4C100u},
        {"CDVDSTM.IRX", "CDVDSTM", 33521u, 0x6086EC562FB44093ull, 0x4EF00u},
        {"KOEISND.IRX", "KOEISND", 104549u, 0x08A8A5C5B76B4E28ull, 0x53B00u},
    };
}
