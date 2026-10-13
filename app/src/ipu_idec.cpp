// Native PS2 IPU IDEC decoder. Written from ISO/IEC 13818-2 (MPEG-2) annex B tables and
// IPU behaviour; see ipu_idec.h for the stream layout.
#include "ipu_idec.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace ipu
{
    namespace
    {
        // ---------------------------------------------------------------- bit reader
        class BitReader
        {
        public:
            BitReader(const uint8_t *data, size_t size, size_t bitOffset)
                : m_data(data), m_bits(size * 8u), m_pos(bitOffset) {}

            uint32_t peek(unsigned count) const
            {
                uint32_t value = 0;
                for (unsigned i = 0; i < count; ++i)
                {
                    const size_t bit = m_pos + i;
                    const uint32_t b = bit < m_bits ? (m_data[bit >> 3] >> (7u - (bit & 7u))) & 1u : 0u;
                    value = (value << 1) | b;
                }
                return value;
            }
            uint32_t get(unsigned count)
            {
                const uint32_t value = peek(count);
                m_pos += count;
                return value;
            }
            bool overrun() const { return m_pos > m_bits; }
            bool atEnd() const { return m_pos >= m_bits; }

        private:
            const uint8_t *m_data;
            size_t m_bits;
            size_t m_pos;
        };

        // ---------------------------------------------------------------- VLC tables
        struct Code
        {
            const char *bits; // without the sign bit
            int run;          // or value for non-coefficient tables
            int level;
        };

        constexpr int kEob = -1;
        constexpr int kEscape = -2;

        // Table B-14 (DCT coefficients table zero), intra form: '10' = EOB, '11s' = run 0 level 1.
        constexpr Code kTableB14[] = {
            {"10", kEob, 0}, {"000001", kEscape, 0},
            {"11", 0, 1}, {"011", 1, 1}, {"0100", 0, 2}, {"0101", 2, 1}, {"00101", 0, 3}, {"00111", 3, 1},
            {"00110", 4, 1}, {"000110", 1, 2}, {"000111", 5, 1}, {"000101", 6, 1}, {"000100", 7, 1},
            {"0000110", 0, 4}, {"0000100", 2, 2}, {"0000111", 8, 1}, {"0000101", 9, 1},
            {"00100110", 0, 5}, {"00100001", 0, 6}, {"00100101", 1, 3}, {"00100100", 3, 2},
            {"00100111", 10, 1}, {"00100011", 11, 1}, {"00100010", 12, 1}, {"00100000", 13, 1},
            {"0000001010", 0, 7}, {"0000001100", 1, 4}, {"0000001011", 2, 3}, {"0000001111", 4, 2},
            {"0000001001", 5, 2}, {"0000001110", 14, 1}, {"0000001101", 15, 1}, {"0000001000", 16, 1},
            {"000000011101", 0, 8}, {"000000011000", 0, 9}, {"000000010011", 0, 10}, {"000000010000", 0, 11},
            {"000000011011", 1, 5}, {"000000010100", 2, 4}, {"000000011100", 3, 3}, {"000000010010", 4, 3},
            {"000000011110", 6, 2}, {"000000010101", 7, 2}, {"000000010001", 8, 2}, {"000000011111", 17, 1},
            {"000000011010", 18, 1}, {"000000011001", 19, 1}, {"000000010111", 20, 1}, {"000000010110", 21, 1},
            {"0000000011010", 0, 12}, {"0000000011001", 0, 13}, {"0000000011000", 0, 14}, {"0000000010111", 0, 15},
            {"0000000010110", 1, 6}, {"0000000010101", 1, 7}, {"0000000010100", 2, 5}, {"0000000010011", 3, 4},
            {"0000000010010", 5, 3}, {"0000000010001", 9, 2}, {"0000000010000", 10, 2}, {"0000000011111", 22, 1},
            {"0000000011110", 23, 1}, {"0000000011101", 24, 1}, {"0000000011100", 25, 1}, {"0000000011011", 26, 1},
            {"00000000011111", 0, 16}, {"00000000011110", 0, 17}, {"00000000011101", 0, 18}, {"00000000011100", 0, 19},
            {"00000000011011", 0, 20}, {"00000000011010", 0, 21}, {"00000000011001", 0, 22}, {"00000000011000", 0, 23},
            {"00000000010111", 0, 24}, {"00000000010110", 0, 25}, {"00000000010101", 0, 26}, {"00000000010100", 0, 27},
            {"00000000010011", 0, 28}, {"00000000010010", 0, 29}, {"00000000010001", 0, 30}, {"00000000010000", 0, 31},
            {"000000000011000", 0, 32}, {"000000000010111", 0, 33}, {"000000000010110", 0, 34}, {"000000000010101", 0, 35},
            {"000000000010100", 0, 36}, {"000000000010011", 0, 37}, {"000000000010010", 0, 38}, {"000000000010001", 0, 39},
            {"000000000010000", 0, 40}, {"000000000011111", 1, 8}, {"000000000011110", 1, 9}, {"000000000011101", 1, 10},
            {"000000000011100", 1, 11}, {"000000000011011", 1, 12}, {"000000000011010", 1, 13}, {"000000000011001", 1, 14},
            {"0000000000010011", 1, 15}, {"0000000000010010", 1, 16}, {"0000000000010001", 1, 17}, {"0000000000010000", 1, 18},
            {"0000000000010100", 6, 3}, {"0000000000011010", 11, 2}, {"0000000000011001", 12, 2}, {"0000000000011000", 13, 2},
            {"0000000000010111", 14, 2}, {"0000000000010110", 15, 2}, {"0000000000010101", 16, 2}, {"0000000000011111", 27, 1},
            {"0000000000011110", 28, 1}, {"0000000000011101", 29, 1}, {"0000000000011100", 30, 1}, {"0000000000011011", 31, 1},
        };

        // Table B-15 (DCT coefficients table one), used for intra blocks when intra_vlc_format = 1.
        constexpr Code kTableB15[] = {
            {"0110", kEob, 0}, {"000001", kEscape, 0},
            {"10", 0, 1}, {"010", 1, 1}, {"110", 0, 2}, {"00101", 2, 1}, {"0111", 0, 3}, {"00111", 3, 1},
            {"000110", 4, 1}, {"00110", 1, 2}, {"000111", 5, 1}, {"0000110", 6, 1}, {"0000100", 7, 1},
            {"11100", 0, 4}, {"0000111", 2, 2}, {"0000101", 8, 1}, {"1111000", 9, 1},
            {"11101", 0, 5}, {"000101", 0, 6}, {"1111001", 1, 3}, {"00100110", 3, 2},
            {"1111010", 10, 1}, {"00100001", 11, 1}, {"00100101", 12, 1}, {"00100100", 13, 1},
            {"000100", 0, 7}, {"00100111", 1, 4}, {"11111100", 2, 3}, {"11111101", 4, 2},
            {"000000100", 5, 2}, {"000000101", 14, 1}, {"000000111", 15, 1}, {"0000001101", 16, 1},
            {"1111011", 0, 8}, {"1111100", 0, 9}, {"00100011", 0, 10}, {"00100010", 0, 11},
            {"00100000", 1, 5}, {"0000001100", 2, 4}, {"000000011100", 3, 3}, {"000000010010", 4, 3},
            {"000000011110", 6, 2}, {"000000010101", 7, 2}, {"000000010001", 8, 2}, {"000000011111", 17, 1},
            {"000000011010", 18, 1}, {"000000011001", 19, 1}, {"000000010111", 20, 1}, {"000000010110", 21, 1},
            {"11111010", 0, 12}, {"11111011", 0, 13}, {"11111110", 0, 14}, {"11111111", 0, 15},
            {"0000000010110", 1, 6}, {"0000000010101", 1, 7}, {"0000000010100", 2, 5}, {"0000000010011", 3, 4},
            {"0000000010010", 5, 3}, {"0000000010001", 9, 2}, {"0000000010000", 10, 2}, {"0000000011111", 22, 1},
            {"0000000011110", 23, 1}, {"0000000011101", 24, 1}, {"0000000011100", 25, 1}, {"0000000011011", 26, 1},
            {"00000000011111", 0, 16}, {"00000000011110", 0, 17}, {"00000000011101", 0, 18}, {"00000000011100", 0, 19},
            {"00000000011011", 0, 20}, {"00000000011010", 0, 21}, {"00000000011001", 0, 22}, {"00000000011000", 0, 23},
            {"00000000010111", 0, 24}, {"00000000010110", 0, 25}, {"00000000010101", 0, 26}, {"00000000010100", 0, 27},
            {"00000000010011", 0, 28}, {"00000000010010", 0, 29}, {"00000000010001", 0, 30}, {"00000000010000", 0, 31},
            {"000000000011000", 0, 32}, {"000000000010111", 0, 33}, {"000000000010110", 0, 34}, {"000000000010101", 0, 35},
            {"000000000010100", 0, 36}, {"000000000010011", 0, 37}, {"000000000010010", 0, 38}, {"000000000010001", 0, 39},
            {"000000000010000", 0, 40}, {"000000000011111", 1, 8}, {"000000000011110", 1, 9}, {"000000000011101", 1, 10},
            {"000000000011100", 1, 11}, {"000000000011011", 1, 12}, {"000000000011010", 1, 13}, {"000000000011001", 1, 14},
            {"0000000000010011", 1, 15}, {"0000000000010010", 1, 16}, {"0000000000010001", 1, 17}, {"0000000000010000", 1, 18},
            {"0000000000010100", 6, 3}, {"0000000000011010", 11, 2}, {"0000000000011001", 12, 2}, {"0000000000011000", 13, 2},
            {"0000000000010111", 14, 2}, {"0000000000010110", 15, 2}, {"0000000000010101", 16, 2}, {"0000000000011111", 27, 1},
            {"0000000000011110", 28, 1}, {"0000000000011101", 29, 1}, {"0000000000011100", 30, 1}, {"0000000000011011", 31, 1},
        };

        // Tables B-12 / B-13: dct_dc_size_luminance / chrominance (value = size).
        constexpr Code kDcLuma[] = {
            {"100", 0, 0}, {"00", 1, 0}, {"01", 2, 0}, {"101", 3, 0}, {"110", 4, 0}, {"1110", 5, 0},
            {"11110", 6, 0}, {"111110", 7, 0}, {"1111110", 8, 0}, {"11111110", 9, 0}, {"111111110", 10, 0},
            {"111111111", 11, 0},
        };
        constexpr Code kDcChroma[] = {
            {"00", 0, 0}, {"01", 1, 0}, {"10", 2, 0}, {"110", 3, 0}, {"1110", 4, 0}, {"11110", 5, 0},
            {"111110", 6, 0}, {"1111110", 7, 0}, {"11111110", 8, 0}, {"111111110", 9, 0}, {"1111111110", 10, 0},
            {"1111111111", 11, 0},
        };

        constexpr uint8_t kZigzag[64] = {
            0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
            35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
        constexpr uint8_t kAlternate[64] = {
            0, 8, 16, 24, 1, 9, 2, 10, 17, 25, 32, 40, 48, 56, 57, 49, 41, 33, 26, 18, 3, 11, 4, 12, 19, 27, 34, 42, 50, 58, 35, 43,
            51, 59, 20, 28, 5, 13, 6, 14, 21, 29, 36, 44, 52, 60, 37, 45, 53, 61, 22, 30, 7, 15, 23, 31, 38, 46, 54, 62, 39, 47, 55, 63};
        // Default intra quantiser matrix, raster order (ISO/IEC 13818-2 6.3.11).
        constexpr uint8_t kIntraMatrix[64] = {
            8, 16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37, 19, 22, 26, 27, 29, 34, 34, 38,
            22, 22, 26, 27, 29, 34, 37, 40, 22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58,
            26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83};
        constexpr uint8_t kNonLinearQuant[32] = {
            0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 18, 20, 22, 24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 96, 104, 112};

        // Prefix-code lookup: (length, code) -> entry index. Codes are at most 16 bits.
        struct Vlc
        {
            std::array<std::map<uint32_t, int>, 17> byLength;
            const Code *codes = nullptr;
            size_t count = 0;
            bool signedCodes = false;
        };

        Vlc buildVlc(const Code *codes, size_t count, bool signedCodes)
        {
            Vlc vlc;
            vlc.codes = codes;
            vlc.count = count;
            vlc.signedCodes = signedCodes;
            for (size_t i = 0; i < count; ++i)
            {
                const size_t length = std::strlen(codes[i].bits);
                uint32_t value = 0;
                for (size_t b = 0; b < length; ++b)
                    value = (value << 1) | (codes[i].bits[b] == '1' ? 1u : 0u);
                vlc.byLength[length][value] = static_cast<int>(i);
            }
            return vlc;
        }

        // Returns the matched entry index or -1.
        int readVlc(BitReader &reader, const Vlc &vlc)
        {
            const uint32_t window = reader.peek(16);
            for (unsigned length = 1; length <= 16; ++length)
            {
                const auto &map = vlc.byLength[length];
                if (map.empty())
                    continue;
                const auto found = map.find(window >> (16u - length));
                if (found != map.end())
                {
                    reader.get(length);
                    return found->second;
                }
            }
            return -1;
        }

        struct Tables
        {
            Vlc b14, b15, dcLuma, dcChroma;
            double idct[8][8]; // idct[x][u] = C(u)/2 * cos((2x+1)u*pi/16)
        };

        const Tables &tables()
        {
            static Tables t;
            static std::once_flag once;
            std::call_once(once, [] {
                t.b14 = buildVlc(kTableB14, std::size(kTableB14), true);
                t.b15 = buildVlc(kTableB15, std::size(kTableB15), true);
                t.dcLuma = buildVlc(kDcLuma, std::size(kDcLuma), false);
                t.dcChroma = buildVlc(kDcChroma, std::size(kDcChroma), false);
                for (int x = 0; x < 8; ++x)
                    for (int u = 0; u < 8; ++u)
                        t.idct[x][u] = (u == 0 ? std::sqrt(0.5) : 1.0) * 0.5 * std::cos((2 * x + 1) * u * M_PI / 16.0);
            });
            return t;
        }

        // Separable reference IDCT (IEEE 1180 accuracy), rounded and clamped for intra output.
        void idct8x8(const int32_t in[64], uint8_t out[64])
        {
            const Tables &t = tables();
            double tmp[64];
            for (int v = 0; v < 8; ++v) // rows of coefficients -> horizontal pass
                for (int x = 0; x < 8; ++x)
                {
                    double sum = 0.0;
                    for (int u = 0; u < 8; ++u)
                        sum += t.idct[x][u] * in[v * 8 + u];
                    tmp[v * 8 + x] = sum;
                }
            for (int x = 0; x < 8; ++x) // vertical pass
                for (int y = 0; y < 8; ++y)
                {
                    double sum = 0.0;
                    for (int v = 0; v < 8; ++v)
                        sum += t.idct[y][v] * tmp[v * 8 + x];
                    const long value = std::lround(sum);
                    out[y * 8 + x] = static_cast<uint8_t>(std::clamp<long>(value, 0, 255));
                }
        }

        uint8_t clamp8(int value) { return static_cast<uint8_t>(std::clamp(value, 0, 255)); }

        std::string checkPrefixCode(const char *name, const Code *codes, size_t count, bool signedCodes,
                                    bool expectComplete)
        {
            double kraft = 0.0;
            for (size_t i = 0; i < count; ++i)
            {
                const size_t length = std::strlen(codes[i].bits);
                const bool hasSign = signedCodes && codes[i].run >= 0;
                (void)hasSign; // a sign bit doubles the codewords and halves each one: mass stays 2^-length
                kraft += std::ldexp(1.0, -static_cast<int>(length));
                for (size_t j = 0; j < count; ++j)
                {
                    if (i == j)
                        continue;
                    const size_t other = std::strlen(codes[j].bits);
                    if (other <= length && std::strncmp(codes[i].bits, codes[j].bits, other) == 0)
                        return std::string(name) + ": '" + codes[j].bits + "' is a prefix of '" + codes[i].bits + "'";
                }
            }
            if (kraft > 1.0 + 1e-12)
                return std::string(name) + ": Kraft sum " + std::to_string(kraft) + " > 1";
            if (expectComplete && kraft < 1.0 - 1e-12)
                return std::string(name) + ": incomplete code, Kraft sum " + std::to_string(kraft);
            return {};
        }
    }

    std::string selfTest()
    {
        // B-14/B-15 leave only codes with 16+ leading zeros unused; DC size tables are complete.
        for (const std::string &error : {
                 checkPrefixCode("B-14", kTableB14, std::size(kTableB14), true, false),
                 checkPrefixCode("B-15", kTableB15, std::size(kTableB15), true, false),
                 checkPrefixCode("B-12", kDcLuma, std::size(kDcLuma), false, true),
                 checkPrefixCode("B-13", kDcChroma, std::size(kDcChroma), false, true)})
            if (!error.empty())
                return error;
        return {};
    }

    IdecResult decodeIdec(const uint8_t *data, size_t size, size_t bitOffset, const IdecConfig &config,
                          uint32_t maxMacroblocks, uint8_t *rgba)
    {
        const Tables &t = tables();
        IdecResult result;
        BitReader reader(data, size, bitOffset);

        const unsigned dcPrecision = config.flags & 3u;
        const bool dctTypePresent = (config.flags & 0x04u) != 0;
        const bool alternateScan = (config.flags & 0x10u) != 0;
        const bool intraVlcFormat = (config.flags & 0x20u) != 0;
        const bool nonLinearQuant = (config.flags & 0x40u) != 0;
        const bool mpeg1 = (config.flags & 0x80u) != 0;
        const uint8_t *scan = alternateScan ? kAlternate : kZigzag;
        const Vlc &coefficients = (intraVlcFormat && !mpeg1) ? t.b15 : t.b14;
        const int dcMultiplier = mpeg1 ? 8 : (8 >> dcPrecision);
        const int dcReset = mpeg1 ? 128 : (128 << dcPrecision);

        int dcPredictor[3] = {dcReset, dcReset, dcReset};
        unsigned quantiserCode = config.quantiserCode & 31u;

        for (uint32_t mb = 0; mb < maxMacroblocks; ++mb)
        {
            if (mb != 0)
            {
                // IDEC ends at a start code (23 zero bits); otherwise the address increment must be 1.
                if (reader.atEnd() || reader.peek(23) == 0)
                {
                    result.startCode = !reader.atEnd();
                    break;
                }
                if (reader.get(1) != 1)
                {
                    result.error = "macroblock_address_increment is not 1 at macroblock " + std::to_string(mb);
                    break;
                }
            }

            // macroblock_type for I pictures (B-2): '1' intra, '01' intra + quant.
            bool quant = false;
            if (reader.get(1) == 0)
            {
                if (reader.get(1) == 0)
                {
                    result.error = "invalid macroblock_type at macroblock " + std::to_string(mb);
                    break;
                }
                quant = true;
            }
            const bool fieldDct = dctTypePresent ? reader.get(1) != 0 : false;
            if (quant)
                quantiserCode = reader.get(5);
            const int quantiserScale = nonLinearQuant ? kNonLinearQuant[quantiserCode] : static_cast<int>(quantiserCode * 2u);

            uint8_t luma[256];
            uint8_t chroma[2][64];
            for (int block = 0; block < 6; ++block)
            {
                const int component = block < 4 ? 0 : block - 3;
                int32_t coefficient[64] = {};

                const int sizeIndex = readVlc(reader, component == 0 ? t.dcLuma : t.dcChroma);
                if (sizeIndex < 0)
                {
                    result.error = "invalid dct_dc_size at macroblock " + std::to_string(mb);
                    break;
                }
                const int dcSize = (component == 0 ? kDcLuma : kDcChroma)[sizeIndex].run;
                int differential = 0;
                if (dcSize != 0)
                {
                    const int bits = static_cast<int>(reader.get(static_cast<unsigned>(dcSize)));
                    differential = bits < (1 << (dcSize - 1)) ? bits - ((1 << dcSize) - 1) : bits;
                }
                dcPredictor[component] += differential;
                coefficient[0] = dcPredictor[component] * dcMultiplier;
                int32_t sum = coefficient[0];

                int index = 0;
                bool blockError = false;
                while (true)
                {
                    const int entry = readVlc(reader, coefficients);
                    if (entry < 0)
                    {
                        blockError = true;
                        break;
                    }
                    const Code &code = coefficients.codes[entry];
                    if (code.run == kEob)
                        break;
                    int run, level;
                    if (code.run == kEscape)
                    {
                        run = static_cast<int>(reader.get(6));
                        if (mpeg1)
                        {
                            level = static_cast<int8_t>(reader.get(8));
                            if (level == 0)
                                level = static_cast<int>(reader.get(8));
                            else if (level == -128)
                                level = static_cast<int>(reader.get(8)) - 256;
                        }
                        else
                        {
                            level = static_cast<int>(reader.get(12));
                            if (level & 0x800)
                                level -= 0x1000;
                        }
                    }
                    else
                    {
                        run = code.run;
                        level = reader.get(1) ? -code.level : code.level;
                    }
                    index += run + 1;
                    if (index > 63 || level == 0)
                    {
                        blockError = true;
                        break;
                    }
                    const int position = scan[index];
                    int magnitude = (std::abs(level) * quantiserScale * kIntraMatrix[position]) >> 4;
                    if (mpeg1 && magnitude != 0)
                        magnitude = (magnitude - 1) | 1;
                    int value = level < 0 ? -magnitude : magnitude;
                    value = std::clamp(value, -2048, 2047);
                    coefficient[position] = value;
                    sum += value;
                }
                if (blockError)
                {
                    result.error = "invalid DCT coefficient code at macroblock " + std::to_string(mb);
                    break;
                }
                if (!mpeg1 && (sum & 1) == 0)
                    coefficient[63] ^= 1; // MPEG-2 mismatch control

                uint8_t pixels[64];
                idct8x8(coefficient, pixels);
                if (component == 0)
                {
                    const int bx = (block & 1) * 8;
                    for (int row = 0; row < 8; ++row)
                    {
                        // Field DCT: blocks 0/1 hold even lines, 2/3 odd lines of the macroblock.
                        const int y = fieldDct ? (block >> 1) + row * 2 : (block >> 1) * 8 + row;
                        std::memcpy(&luma[y * 16 + bx], &pixels[row * 8], 8);
                    }
                }
                else
                    std::memcpy(chroma[component - 1], pixels, 64);
            }
            if (!result.error.empty())
                break;
            if (reader.overrun())
            {
                result.error = "bitstream ended inside macroblock " + std::to_string(mb);
                break;
            }

            // CSC: ITU-R BT.601 (studio range) in the IPU's 7-bit fixed point, chroma replicated 2x2.
            uint8_t *out = rgba + static_cast<size_t>(mb) * 1024u;
            for (int y = 0; y < 16; ++y)
                for (int x = 0; x < 16; ++x)
                {
                    const int lum = (luma[y * 16 + x] - 16) * 0x95;
                    const int cb = chroma[0][(y >> 1) * 8 + (x >> 1)] - 128;
                    const int cr = chroma[1][(y >> 1) * 8 + (x >> 1)] - 128;
                    const uint8_t r = clamp8((lum + 0xCC * cr + 64) >> 7);
                    const uint8_t g = clamp8((lum - 0x32 * cb - 0x68 * cr + 64) >> 7);
                    const uint8_t b = clamp8((lum + 0x102 * cb + 64) >> 7);
                    uint8_t alpha = 0x80;
                    if (r < config.alphaThreshold0 && g < config.alphaThreshold0 && b < config.alphaThreshold0)
                        alpha = 0;
                    else if (r < config.alphaThreshold1 && g < config.alphaThreshold1 && b < config.alphaThreshold1)
                        alpha = 0x40;
                    uint8_t *pixel = out + (y * 16 + x) * 4;
                    pixel[0] = alpha ? r : 0;
                    pixel[1] = alpha ? g : 0;
                    pixel[2] = alpha ? b : 0;
                    pixel[3] = alpha;
                }
            ++result.macroblocks;
        }
        return result;
    }
}
