// Native decoder for PS2 IPU "IDEC" intra bitstreams (the format of Sony's ipum still images).
//
// The IPU decodes a run of MPEG-2 (or MPEG-1) intra macroblocks without slice/picture headers:
//   macroblock_type, [dct_type], [quantiser_scale_code], 6 intra blocks; then '1' (address
//   increment 1) before every following macroblock, until a start code or the end of data.
// Picture parameters come from IPU_CTRL, which ipum streams carry in their first byte:
//   bits 0-1 intra_dc_precision, bit 2 dct_type present, bit 4 alternate_scan,
//   bit 5 intra_vlc_format, bit 6 q_scale_type, bit 7 MPEG-1.
// Output matches IDEC with OFM=0: per macroblock 16x16 RGBA8 pixels (1024 bytes), row-major,
// macroblocks in decode order.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace ipu
{
    struct IdecConfig
    {
        uint8_t flags = 0;          // IPU_CTRL picture bits as stored in an ipum frame's first byte
        uint8_t quantiserCode = 1;  // IDEC command QSC (used until a macroblock carries its own)
        uint16_t alphaThreshold0 = 0; // SETTH: all of R,G,B below -> alpha 0
        uint16_t alphaThreshold1 = 0; // SETTH: all of R,G,B below -> alpha 0x40, else 0x80
    };

    struct IdecResult
    {
        uint32_t macroblocks = 0; // macroblocks written to the output
        bool startCode = false;   // stopped at a start code (IPU SCD)
        std::string error;        // non-empty on a bitstream error (IPU ECD)
    };

    // Decodes up to maxMacroblocks from `data` (MSB-first bitstream starting at bit `bitOffset`)
    // into `rgba`, which must hold maxMacroblocks * 1024 bytes.
    IdecResult decodeIdec(const uint8_t *data, size_t size, size_t bitOffset, const IdecConfig &config,
                          uint32_t maxMacroblocks, uint8_t *rgba);

    // Validates the VLC tables (prefix-free, complete). Returns an empty string when consistent.
    std::string selfTest();
}
