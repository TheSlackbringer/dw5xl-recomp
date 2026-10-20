#include "ps2x/state_archive.h"
#include "iop_spu2.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <utility>
#include <vector>

// Gaussian coefficients and the common SPU envelope/reverb algorithms are
// adapted from RecompOne (MIT). See ps2xIOP/licenses/RecompOne-SPU.txt.
// The SPU2 register map, shared RAM, two-core routing and DMA are implemented
// here from the PS2SDK register definitions and documented hardware behavior.
namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t RamMask = 0xfffff;
        constexpr unsigned Voices = 24;
        constexpr uint32_t AllVoices = 0xffffff;
        constexpr uint32_t Attr = 0x19a, Irqa = 0x19c, Kon = 0x1a0, Koff = 0x1a4;
        constexpr uint32_t Tsa = 0x1a8, Data = 0x1ac, Admas = 0x1b0;
        constexpr uint32_t Endx = 0x340, Statx = 0x344;
        constexpr std::array<int, 5> Filter0{0, 60, 115, 98, 122};
        constexpr std::array<int, 5> Filter1{0, 0, -52, -55, -60};
        constexpr std::array<int, 4> StepUp{7, 6, 5, 4};
        constexpr std::array<int, 4> StepDown{-8, -7, -6, -5};
        bool traceEnabled()
        {
            const char *value = std::getenv("PS2X_SPU2_TRACE");
            return value && *value && !(value[0] == '0' && value[1] == '\0');
        }
        constexpr std::array<int16_t, 512> Gauss{
        -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001,
        -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001,
        0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0001,
        0x0001, 0x0001, 0x0001, 0x0002, 0x0002, 0x0002, 0x0003, 0x0003,
        0x0003, 0x0004, 0x0004, 0x0005, 0x0005, 0x0006, 0x0007, 0x0007,
        0x0008, 0x0009, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E,
        0x000F, 0x0010, 0x0011, 0x0012, 0x0013, 0x0015, 0x0016, 0x0018,
        0x0019, 0x001B, 0x001C, 0x001E, 0x0020, 0x0021, 0x0023, 0x0025,
        0x0027, 0x0029, 0x002C, 0x002E, 0x0030, 0x0033, 0x0035, 0x0038,
        0x003A, 0x003D, 0x0040, 0x0043, 0x0046, 0x0049, 0x004D, 0x0050,
        0x0054, 0x0057, 0x005B, 0x005F, 0x0063, 0x0067, 0x006B, 0x006F,
        0x0074, 0x0078, 0x007D, 0x0082, 0x0087, 0x008C, 0x0091, 0x0096,
        0x009C, 0x00A1, 0x00A7, 0x00AD, 0x00B3, 0x00BA, 0x00C0, 0x00C7,
        0x00CD, 0x00D4, 0x00DB, 0x00E3, 0x00EA, 0x00F2, 0x00FA, 0x0101,
        0x010A, 0x0112, 0x011B, 0x0123, 0x012C, 0x0135, 0x013F, 0x0148,
        0x0152, 0x015C, 0x0166, 0x0171, 0x017B, 0x0186, 0x0191, 0x019C,
        0x01A8, 0x01B4, 0x01C0, 0x01CC, 0x01D9, 0x01E5, 0x01F2, 0x0200,
        0x020D, 0x021B, 0x0229, 0x0237, 0x0246, 0x0255, 0x0264, 0x0273,
        0x0283, 0x0293, 0x02A3, 0x02B4, 0x02C4, 0x02D6, 0x02E7, 0x02F9,
        0x030B, 0x031D, 0x0330, 0x0343, 0x0356, 0x036A, 0x037E, 0x0392,
        0x03A7, 0x03BC, 0x03D1, 0x03E7, 0x03FC, 0x0413, 0x042A, 0x0441,
        0x0458, 0x0470, 0x0488, 0x04A0, 0x04B9, 0x04D2, 0x04EC, 0x0506,
        0x0520, 0x053B, 0x0556, 0x0572, 0x058E, 0x05AA, 0x05C7, 0x05E4,
        0x0601, 0x061F, 0x063E, 0x065C, 0x067C, 0x069B, 0x06BB, 0x06DC,
        0x06FD, 0x071E, 0x0740, 0x0762, 0x0784, 0x07A7, 0x07CB, 0x07EF,
        0x0813, 0x0838, 0x085D, 0x0883, 0x08A9, 0x08D0, 0x08F7, 0x091E,
        0x0946, 0x096F, 0x0998, 0x09C1, 0x09EB, 0x0A16, 0x0A40, 0x0A6C,
        0x0A98, 0x0AC4, 0x0AF1, 0x0B1E, 0x0B4C, 0x0B7A, 0x0BA9, 0x0BD8,
        0x0C07, 0x0C38, 0x0C68, 0x0C99, 0x0CCB, 0x0CFD, 0x0D30, 0x0D63,
        0x0D97, 0x0DCB, 0x0E00, 0x0E35, 0x0E6B, 0x0EA1, 0x0ED7, 0x0F0F,
        0x0F46, 0x0F7F, 0x0FB7, 0x0FF1, 0x102A, 0x1065, 0x109F, 0x10DB,
        0x1116, 0x1153, 0x118F, 0x11CD, 0x120B, 0x1249, 0x1288, 0x12C7,
        0x1307, 0x1347, 0x1388, 0x13C9, 0x140B, 0x144D, 0x1490, 0x14D4,
        0x1517, 0x155C, 0x15A0, 0x15E6, 0x162C, 0x1672, 0x16B9, 0x1700,
        0x1747, 0x1790, 0x17D8, 0x1821, 0x186B, 0x18B5, 0x1900, 0x194B,
        0x1996, 0x19E2, 0x1A2E, 0x1A7B, 0x1AC8, 0x1B16, 0x1B64, 0x1BB3,
        0x1C02, 0x1C51, 0x1CA1, 0x1CF1, 0x1D42, 0x1D93, 0x1DE5, 0x1E37,
        0x1E89, 0x1EDC, 0x1F2F, 0x1F82, 0x1FD6, 0x202A, 0x207F, 0x20D4,
        0x2129, 0x217F, 0x21D5, 0x222C, 0x2282, 0x22DA, 0x2331, 0x2389,
        0x23E1, 0x2439, 0x2492, 0x24EB, 0x2545, 0x259E, 0x25F8, 0x2653,
        0x26AD, 0x2708, 0x2763, 0x27BE, 0x281A, 0x2876, 0x28D2, 0x292E,
        0x298B, 0x29E7, 0x2A44, 0x2AA1, 0x2AFF, 0x2B5C, 0x2BBA, 0x2C18,
        0x2C76, 0x2CD4, 0x2D33, 0x2D91, 0x2DF0, 0x2E4F, 0x2EAE, 0x2F0D,
        0x2F6C, 0x2FCC, 0x302B, 0x308B, 0x30EA, 0x314A, 0x31AA, 0x3209,
        0x3269, 0x32C9, 0x3329, 0x3389, 0x33E9, 0x3449, 0x34A9, 0x3509,
        0x3569, 0x35C9, 0x3629, 0x3689, 0x36E8, 0x3748, 0x37A8, 0x3807,
        0x3867, 0x38C6, 0x3926, 0x3985, 0x39E4, 0x3A43, 0x3AA2, 0x3B00,
        0x3B5F, 0x3BBD, 0x3C1B, 0x3C79, 0x3CD7, 0x3D35, 0x3D92, 0x3DEF,
        0x3E4C, 0x3EA9, 0x3F05, 0x3F62, 0x3FBD, 0x4019, 0x4074, 0x40D0,
        0x412A, 0x4185, 0x41DF, 0x4239, 0x4292, 0x42EB, 0x4344, 0x439C,
        0x43F4, 0x444C, 0x44A3, 0x44FA, 0x4550, 0x45A6, 0x45FC, 0x4651,
        0x46A6, 0x46FA, 0x474E, 0x47A1, 0x47F4, 0x4846, 0x4898, 0x48E9,
        0x493A, 0x498A, 0x49D9, 0x4A29, 0x4A77, 0x4AC5, 0x4B13, 0x4B5F,
        0x4BAC, 0x4BF7, 0x4C42, 0x4C8D, 0x4CD7, 0x4D20, 0x4D68, 0x4DB0,
        0x4DF7, 0x4E3E, 0x4E84, 0x4EC9, 0x4F0E, 0x4F52, 0x4F95, 0x4FD7,
        0x5019, 0x505A, 0x509A, 0x50DA, 0x5118, 0x5156, 0x5194, 0x51D0,
        0x520C, 0x5247, 0x5281, 0x52BA, 0x52F3, 0x532A, 0x5361, 0x5397,
        0x53CC, 0x5401, 0x5434, 0x5467, 0x5499, 0x54CA, 0x54FA, 0x5529,
        0x5558, 0x5585, 0x55B2, 0x55DE, 0x5609, 0x5632, 0x565B, 0x5684,
        0x56AB, 0x56D1, 0x56F6, 0x571B, 0x573E, 0x5761, 0x5782, 0x57A3,
        0x57C3, 0x57E2, 0x57FF, 0x581C, 0x5838, 0x5853, 0x586D, 0x5886,
        0x589E, 0x58B5, 0x58CB, 0x58E0, 0x58F4, 0x5907, 0x5919, 0x592A,
        0x593A, 0x5949, 0x5958, 0x5965, 0x5971, 0x597C, 0x5986, 0x598F,
        0x5997, 0x599E, 0x59A4, 0x59A9, 0x59AD, 0x59B0, 0x59B2, 0x59B3
            };

        int16_t clamp16(int64_t value)
        {
            return static_cast<int16_t>(std::clamp<int64_t>(value, -32768, 32767));
        }
        int mul(int sample, int volume)
        {
            return static_cast<int>((static_cast<int64_t>(sample) * volume) >> 15);
        }
        void envTick(int16_t &level, uint32_t &counter, unsigned shift, unsigned stepIndex,
                     bool exponential, bool decrease, bool negative = false)
        {
            int magnitude = negative ? -static_cast<int>(level) : static_cast<int>(level);
            uint32_t cycles = 1u << (shift > 11 ? shift - 11 : 0);
            // Multiplication avoids undefined left shifts of negative values.
            int step = (decrease ? StepDown : StepUp)[stepIndex] * (1 << (shift < 11 ? 11 - shift : 0));
            if (exponential && !decrease && magnitude > 0x6000)
                cycles *= 4;
            if (exponential && decrease)
            {
                step = step * magnitude / 0x8000;
                if (!step)
                    step = -1;
            }
            if (++counter < cycles)
                return;
            counter = 0;
            const int next = std::clamp(magnitude + step, 0, 0x7fff);
            level = static_cast<int16_t>(negative ? -next : next);
        }
        struct Volume
        {
            uint16_t reg = 0;
            int16_t level = 0;
            uint32_t counter = 0;
            void write(uint16_t value)
            {
                reg = value;
                counter = 0;
                if (!(reg & 0x8000))
                    level = static_cast<int16_t>(reg << 1);
            }
            void tick()
            {
                if (reg & 0x8000)
                    envTick(level, counter, (reg >> 2) & 31, reg & 3,
                            (reg & 0x4000) != 0, (reg & 0x2000) != 0, (reg & 0x1000) != 0);
            }
        };
        enum class Phase { Off, Attack, Decay, Sustain, Release };
        struct Voice
        {
            Volume left, right;
            Phase phase = Phase::Off;
            int16_t envelope = 0;
            uint32_t envelopeCounter = 0;
            uint32_t address = 0;
            uint32_t fraction = 0;
            std::array<int16_t, 28> decoded{};
            std::array<int16_t, 4> history{};
            unsigned sampleIndex = 28;
            uint8_t blockFlags = 0;
            int old = 0, older = 0;
            bool manualLoop = false;
        };
        struct Core
        {
            std::array<Voice, Voices> voices;
            Volume masterLeft, masterRight;
            uint32_t keyOn = 0, keyOff = 0;
            uint32_t endx = AllVoices;
            uint32_t noise = 1;
            int noiseTimer = 0;
            bool dmaBusy = false;
            bool dmaAuto = false;
            std::deque<std::array<int16_t, 2>> input;
            uint32_t inputPosition = 0;
            uint32_t reverbPosition = 0;
            std::array<int16_t, 2> reverbOutput{};
            std::array<int32_t, 2> reverbInput{};
        };
    }

    struct IopSpu2::Impl
    {
        std::vector<uint16_t> ram = std::vector<uint16_t>(RamMask + 1);
        std::array<uint16_t, 0x400> regs{};
        std::array<Core, 2> cores;
        std::vector<int16_t> pcm;
        std::function<void(std::span<const int16_t>)> pcmCallback;
        std::function<void()> irqCallback;
        uint64_t remainder = 0;
        Statistics stats;
        bool trace = traceEnabled();
        uint64_t traceWrites = 0;

        uint16_t &reg(unsigned core, uint32_t offset) { return regs[(core * 0x400 + offset) / 2]; }
        uint16_t reg(unsigned core, uint32_t offset) const { return regs[(core * 0x400 + offset) / 2]; }
        uint16_t ext(unsigned core, uint32_t offset) const { return regs[(0x760 + core * 0x28 + offset) / 2]; }
        uint32_t mask(unsigned core, uint32_t offset) const
        {
            return reg(core, offset) | (static_cast<uint32_t>(reg(core, offset + 2) & 0xff) << 16);
        }
        uint32_t address(unsigned core, uint32_t offset) const
        {
            return ((static_cast<uint32_t>(reg(core, offset) & 15) << 16) | reg(core, offset + 2)) & RamMask;
        }
        uint32_t parameter(unsigned core, uint32_t offset) const
        {
            return (static_cast<uint32_t>(reg(core, offset)) << 16) | reg(core, offset + 2);
        }
        void setAddress(unsigned core, uint32_t offset, uint32_t value)
        {
            reg(core, offset) = static_cast<uint16_t>((value >> 16) & 15);
            reg(core, offset + 2) = static_cast<uint16_t>(value);
        }
        unsigned long long pioWrites[2] = {0, 0}; // DW5XL-DEBUG
        void checkIrq(uint32_t word)
        {
            word &= RamMask;
            for (unsigned core = 0; core != 2; ++core)
            {
                const uint16_t bit = static_cast<uint16_t>(4 << core);
                if ((reg(core, Attr) & 0x40) && address(core, Irqa) == word && !(regs[0x7c2 / 2] & bit))
                {
                    regs[0x7c2 / 2] |= bit;
                    ++stats.irqAssertions;
                    if (irqCallback)
                        irqCallback();
                    if (trace)
                        std::fprintf(stderr, "[SPU2] irq core=%u word=%05X frame=%llu\n", core, word,
                                     static_cast<unsigned long long>(stats.frames));
                }
            }
        }
        uint16_t ramRead(uint32_t word)
        {
            checkIrq(word);
            return ram[word & RamMask];
        }
        void ramWrite(uint32_t word, uint16_t value)
        {
            checkIrq(word);
            ram[word & RamMask] = value;
        }
        void cancelDma(unsigned core)
        {
            auto &state = cores[core];
            const size_t discarded = state.input.size();
            state.input.clear();
            state.dmaBusy = false;
            state.dmaAuto = false;
            reg(core, Statx) &= static_cast<uint16_t>(~0x480u);
            if (trace)
                std::fprintf(stderr, "[SPU2] dma-cancel core=%u discardedFrames=%zu frame=%llu\n",
                             core, discarded, static_cast<unsigned long long>(stats.frames));
        }
        void reset()
        {
            std::fill(ram.begin(), ram.end(), uint16_t{0});
            regs.fill(0);
            cores = {};
            pcm.clear();
            pcm.reserve(512);
            remainder = 0;
            stats = {};
            traceWrites = 0;
        }
        void envelope(unsigned core, unsigned index)
        {
            auto &voice = cores[core].voices[index];
            const uint16_t lo = reg(core, index * 16 + 6), hi = reg(core, index * 16 + 8);
            if (voice.phase == Phase::Attack && voice.envelope == 0x7fff)
            {
                voice.phase = Phase::Decay;
                voice.envelopeCounter = 0;
            }
            if (voice.phase == Phase::Decay && voice.envelope <= ((lo & 15) + 1) * 0x800)
            {
                voice.phase = Phase::Sustain;
                voice.envelopeCounter = 0;
            }
            switch (voice.phase)
            {
            case Phase::Attack:
                envTick(voice.envelope, voice.envelopeCounter, (lo >> 10) & 31, (lo >> 8) & 3,
                        (lo & 0x8000) != 0, false);
                break;
            case Phase::Decay:
                envTick(voice.envelope, voice.envelopeCounter, (lo >> 4) & 15, 0, true, true);
                break;
            case Phase::Sustain:
                envTick(voice.envelope, voice.envelopeCounter, (hi >> 8) & 31, (hi >> 6) & 3,
                        (hi & 0x8000) != 0, (hi & 0x4000) != 0);
                break;
            case Phase::Release:
                envTick(voice.envelope, voice.envelopeCounter, hi & 31, 0, (hi & 0x20) != 0, true);
                if (!voice.envelope)
                    voice.phase = Phase::Off;
                break;
            case Phase::Off: break;
            }
        }
        void decode(unsigned core, unsigned index)
        {
            auto &voice = cores[core].voices[index];
            const uint32_t word = voice.address & ~7u;
            std::array<uint8_t, 16> block{};
            for (unsigned i = 0; i != 8; ++i)
            {
                const uint16_t value = ramRead(word + i);
                block[i * 2] = static_cast<uint8_t>(value);
                block[i * 2 + 1] = static_cast<uint8_t>(value >> 8);
            }
            const unsigned shift = block[0] & 15;
            const unsigned filter = (block[0] >> 4) & 15;
            const int filter0 = filter < Filter0.size() ? Filter0[filter] : 0;
            const int filter1 = filter < Filter1.size() ? Filter1[filter] : 0;
            for (unsigned i = 0; i != 28; ++i)
            {
                const unsigned nibble = (block[2 + i / 2] >> ((i & 1) * 4)) & 15;
                const int signedNibble = nibble < 8 ? static_cast<int>(nibble) : static_cast<int>(nibble) - 16;
                int sample = (signedNibble * 4096) >> shift;
                sample += (voice.old * filter0 + voice.older * filter1 + 32) >> 6;
                voice.older = voice.old;
                voice.old = clamp16(sample);
                voice.decoded[i] = static_cast<int16_t>(voice.old);
            }
            voice.blockFlags = block[1];
            if ((voice.blockFlags & 4) && !voice.manualLoop)
                setAddress(core, 0x1c0 + index * 12 + 4, word);
            voice.address = (word + 8) & RamMask;
            setAddress(core, 0x1c0 + index * 12 + 8, voice.address);
            voice.sampleIndex = 0;
            ++stats.decodedBlocks;
        }
        int16_t nextSample(unsigned core, unsigned index)
        {
            auto &voice = cores[core].voices[index];
            if (voice.sampleIndex == 28)
            {
                if (voice.blockFlags & 1)
                {
                    cores[core].endx |= 1u << index;
                    voice.address = address(core, 0x1c0 + index * 12 + 4) & ~7u;
                    setAddress(core, 0x1c0 + index * 12 + 8, voice.address);
                    if (!(voice.blockFlags & 2))
                    {
                        voice.phase = Phase::Off;
                        voice.envelope = 0;
                        return 0;
                    }
                }
                decode(core, index);
            }
            return voice.decoded[voice.sampleIndex++];
        }
        void keys(unsigned core)
        {
            auto &state = cores[core];
            for (unsigned index = 0; index != Voices; ++index)
            {
                auto &voice = state.voices[index];
                const uint32_t bit = 1u << index;
                if (state.keyOn & bit)
                {
                    const Volume left = voice.left, right = voice.right;
                    voice = {};
                    voice.left = left;
                    voice.right = right;
                    voice.phase = Phase::Attack;
                    voice.address = address(core, 0x1c0 + index * 12) & ~7u;
                    voice.sampleIndex = 28;
                    voice.history[3] = nextSample(core, index);
                    state.endx &= ~bit;
                    ++stats.keyOns;
                    if (trace)
                    {
                        const uint32_t start = address(core, 0x1c0 + index * 12);
                        std::fprintf(stderr, "[SPU2] keyon core=%u voice=%u ssa=%05X lsa=%05X pitch=%04X adsr=%04X/%04X vol=%04X/%04X header=%04X frame=%llu\n",
                                     core, index, start, address(core, 0x1c0 + index * 12 + 4),
                                     reg(core, index * 16 + 4), reg(core, index * 16 + 6), reg(core, index * 16 + 8),
                                     voice.left.reg, voice.right.reg, ram[start & RamMask],
                                     static_cast<unsigned long long>(stats.frames));
                    }
                }
                else if ((state.keyOff & bit) && voice.phase != Phase::Off)
                {
                    voice.phase = Phase::Release;
                    voice.envelopeCounter = 0;
                }
            }
            state.keyOn = state.keyOff = 0;
        }
        int sample(unsigned core, unsigned index, int previous)
        {
            auto &voice = cores[core].voices[index];
            if (voice.phase == Phase::Off)
                return 0;
            envelope(core, index);
            int result;
            if (mask(core, 0x184) & (1u << index))
                result = static_cast<int16_t>(cores[core].noise);
            else
            {
                const unsigned fraction = (voice.fraction >> 4) & 255;
                int interpolated = mul(voice.history[0], Gauss[0xff - fraction])
                                 + mul(voice.history[1], Gauss[0x1ff - fraction])
                                 + mul(voice.history[2], Gauss[0x100 + fraction]);
                // The SPU wraps the intermediate sum before the fourth tap.
                result = clamp16(static_cast<int16_t>(interpolated) + mul(voice.history[3], Gauss[fraction]));
            }
            result = mul(result, voice.envelope);
            unsigned pitch = reg(core, index * 16 + 4) & 0x3fff;
            if (index && (mask(core, 0x180) & (1u << index)))
                pitch = std::min<unsigned>((pitch * (previous + 32768)) >> 15, 0x3fff);
            voice.fraction += pitch;
            while (voice.fraction >= 0x1000)
            {
                voice.fraction -= 0x1000;
                std::move(voice.history.begin() + 1, voice.history.end(), voice.history.begin());
                voice.history[3] = nextSample(core, index);
                if (voice.phase == Phase::Off)
                    break;
            }
            return result;
        }
        uint32_t reverbAddress(unsigned core, int64_t offset) const
        {
            const uint32_t start = address(core, 0x2e0);
            const uint32_t end = std::min<uint32_t>((static_cast<uint32_t>(reg(core, 0x33c)) << 16) | 0xffff, RamMask);
            if (end < start)
                return start;
            const int64_t size = static_cast<int64_t>(end) - start + 1;
            int64_t relative = (static_cast<int64_t>(cores[core].reverbPosition) + offset) % size;
            if (relative < 0)
                relative += size;
            return start + static_cast<uint32_t>(relative);
        }
        int reverbRead(unsigned core, uint32_t offset, int64_t extra = 0)
        {
            return static_cast<int16_t>(ramRead(reverbAddress(core, parameter(core, offset) + extra)));
        }
        void reverbWrite(unsigned core, uint32_t offset, int value)
        {
            if (reg(core, Attr) & 0x80)
                ramWrite(reverbAddress(core, parameter(core, offset)), static_cast<uint16_t>(clamp16(value)));
        }
        void reverb(unsigned core, int inputLeft, int inputRight)
        {
            auto &state = cores[core];
            state.reverbInput[0] += inputLeft;
            state.reverbInput[1] += inputRight;
            if (!(stats.frames & 1))
                return;
            // The effect network runs at 24 kHz. Addresses remain SPU halfwords.
            const auto coeff = [&](uint32_t offset) { return static_cast<int16_t>(ext(core, offset)); };
            const int left = mul(state.reverbInput[0] / 2, coeff(0x24));
            const int right = mul(state.reverbInput[1] / 2, coeff(0x26));
            state.reverbInput = {};
            const int prevLeft = reverbRead(core, 0x2ec, -1), prevRight = reverbRead(core, 0x2f0, -1);
            reverbWrite(core, 0x2ec, prevLeft + mul(left + mul(reverbRead(core, 0x304), coeff(0x1e)) - prevLeft, coeff(0x14)));
            reverbWrite(core, 0x2f0, prevRight + mul(right + mul(reverbRead(core, 0x308), coeff(0x1e)) - prevRight, coeff(0x14)));
            const int diffLeft = reverbRead(core, 0x30c, -1), diffRight = reverbRead(core, 0x310, -1);
            reverbWrite(core, 0x30c, diffLeft + mul(left + mul(reverbRead(core, 0x328), coeff(0x1e)) - diffLeft, coeff(0x14)));
            reverbWrite(core, 0x310, diffRight + mul(right + mul(reverbRead(core, 0x324), coeff(0x1e)) - diffRight, coeff(0x14)));
            int outputLeft = 0, outputRight = 0;
            constexpr uint32_t comb[4] = {0x2f4, 0x2fc, 0x314, 0x31c};
            for (unsigned i = 0; i != 4; ++i)
            {
                outputLeft += mul(reverbRead(core, comb[i]), coeff(0x16 + i * 2));
                outputRight += mul(reverbRead(core, comb[i] + 4), coeff(0x16 + i * 2));
            }
            for (unsigned i = 0; i != 2; ++i)
            {
                const uint32_t destination = 0x32c + i * 8;
                const int64_t delay = -static_cast<int64_t>(parameter(core, 0x2e4 + i * 4));
                const int tapLeft = reverbRead(core, destination, delay), tapRight = reverbRead(core, destination + 4, delay);
                const int gain = coeff(0x20 + i * 2);
                outputLeft -= mul(tapLeft, gain);
                outputRight -= mul(tapRight, gain);
                reverbWrite(core, destination, outputLeft);
                reverbWrite(core, destination + 4, outputRight);
                outputLeft = mul(outputLeft, gain) + tapLeft;
                outputRight = mul(outputRight, gain) + tapRight;
            }
            state.reverbOutput = {clamp16(outputLeft), clamp16(outputRight)};
            const uint32_t start = address(core, 0x2e0);
            const uint32_t end = std::min<uint32_t>((static_cast<uint32_t>(reg(core, 0x33c)) << 16) | 0xffff, RamMask);
            const uint32_t size = end >= start ? end - start + 1 : 1;
            state.reverbPosition = (state.reverbPosition + 1) % size;
        }
        std::array<int16_t, 2> mix(unsigned core, std::array<int16_t, 2> external)
        {
            auto &state = cores[core];
            if (!(reg(core, Attr) & 0x8000))
                return {};
            keys(core);
            const unsigned shift = (reg(core, Attr) >> 10) & 15;
            state.noiseTimer -= ((reg(core, Attr) >> 8) & 3) + 4;
            if (state.noiseTimer < 0)
            {
                const uint32_t parity = ((state.noise >> 15) ^ (state.noise >> 12) ^ (state.noise >> 11) ^ (state.noise >> 10) ^ 1) & 1;
                state.noise = ((state.noise << 1) | parity) & 0xffff;
                state.noiseTimer += 0x20000 >> shift;
            }
            std::array<int, 2> dry{}, wet{};
            int previous = 0;
            for (unsigned index = 0; index != Voices; ++index)
            {
                auto &voice = state.voices[index];
                voice.left.tick();
                voice.right.tick();
                previous = sample(core, index, previous);
                if (index == 1 || index == 3)
                    ramWrite((index == 1 ? 0x400 : 0x600) + core * 0x800 + (stats.frames & 511),
                             static_cast<uint16_t>(clamp16(previous)));
                const int left = mul(previous, voice.left.level), right = mul(previous, voice.right.level);
                const uint32_t bit = 1u << index;
                if (mask(core, 0x188) & bit) dry[0] += left;
                if (mask(core, 0x190) & bit) dry[1] += right;
                if (mask(core, 0x18c) & bit) wet[0] += left;
                if (mask(core, 0x194) & bit) wet[1] += right;
            }
            // The sound processor exposes its per-core voice mixer outputs in
            // 512-frame circular capture windows. They are real RAM accesses
            // and therefore participate in both cores' IRQA comparisons.
            for (unsigned channel = 0; channel != 2; ++channel)
            {
                dry[channel] = clamp16(dry[channel]);
                wet[channel] = clamp16(wet[channel]);
                ramWrite(0x1000 + core * 0x800 + channel * 0x200 + (stats.frames & 511),
                         static_cast<uint16_t>(dry[channel]));
                ramWrite(0x1400 + core * 0x800 + channel * 0x200 + (stats.frames & 511),
                         static_cast<uint16_t>(wet[channel]));
            }
            const uint16_t mmix = reg(core, 0x198);
            if (!(mmix & 0x800)) dry[0] = 0;
            if (!(mmix & 0x400)) dry[1] = 0;
            if (!(mmix & 0x200)) wet[0] = 0;
            if (!(mmix & 0x100)) wet[1] = 0;
            std::array<int16_t, 2> input{};
            if (!state.input.empty())
            {
                input = state.input.front();
                state.input.pop_front();
                const uint32_t position = state.inputPosition++ & 511;
                ramWrite(0x2000 + core * 0x400 + position, static_cast<uint16_t>(input[0]));
                ramWrite(0x2200 + core * 0x400 + position, static_cast<uint16_t>(input[1]));
            }
            for (unsigned channel = 0; channel != 2; ++channel)
            {
                const unsigned side = 1 - channel;
                const int inputSample = mul(input[channel], static_cast<int16_t>(ext(core, 0xc + channel * 2)));
                const int extSample = core ? mul(external[channel], static_cast<int16_t>(ext(core, 8 + channel * 2))) : 0;
                if (mmix & (1 << (side + 6))) dry[channel] += inputSample;
                if (mmix & (1 << (side + 4))) wet[channel] += inputSample;
                if (mmix & (1 << (side + 2))) dry[channel] += extSample;
                if (mmix & (1 << side)) wet[channel] += extSample;
            }
            reverb(core, clamp16(wet[0]), clamp16(wet[1]));
            dry[0] += mul(state.reverbOutput[0], static_cast<int16_t>(ext(core, 4)));
            dry[1] += mul(state.reverbOutput[1], static_cast<int16_t>(ext(core, 6)));
            state.masterLeft.tick();
            state.masterRight.tick();
            return {clamp16(mul(clamp16(dry[0]), state.masterLeft.level)),
                    clamp16(mul(clamp16(dry[1]), state.masterRight.level))};
        }
        void flush()
        {
            if (pcmCallback && !pcm.empty())
                pcmCallback(pcm);
            pcm.clear();
        }
        void frame()
        {
            const auto first = mix(0, {});
            ramWrite(0x800 + (stats.frames & 511), static_cast<uint16_t>(first[0]));
            ramWrite(0xa00 + (stats.frames & 511), static_cast<uint16_t>(first[1]));
            const auto output = mix(1, first);
            pcm.push_back(output[0]);
            pcm.push_back(output[1]);
            if (output[0] || output[1])
                ++stats.nonzeroFrames;
            ++stats.frames;
            if (pcm.size() == 512)
                flush();
            if (trace && stats.frames % 48000 == 0)
                std::fprintf(stderr, "[SPU2] frames=%llu nonzero=%llu keys=%llu blocks=%llu dmaBytes=%llu attr=%04X/%04X mmix=%04X/%04X\n",
                             static_cast<unsigned long long>(stats.frames), static_cast<unsigned long long>(stats.nonzeroFrames),
                             static_cast<unsigned long long>(stats.keyOns), static_cast<unsigned long long>(stats.decodedBlocks),
                             static_cast<unsigned long long>(stats.dmaBytes), reg(0, Attr), reg(1, Attr), reg(0, 0x198), reg(1, 0x198));
            if (trace && stats.frames % 48000 == 0) // DW5XL-DEBUG
                std::fprintf(stderr, "[SPU2] pio=%llu/%llu tsa=%05X/%05X\n", pioWrites[0], pioWrites[1], address(0, Tsa), address(1, Tsa)),
                [&] { // DW5XL-DEBUG: non-plain ADPCM block flags in the core1 stream ring
                    std::fprintf(stderr, "[SPU2] ringflags:");
                    for (uint32_t w = 0x2808; w < 0x3808; w += 8)
                        if (const uint8_t f = static_cast<uint8_t>(ram[w] >> 8); f != 0x02)
                            std::fprintf(stderr, " %05X=%02X", w, f);
                    std::fprintf(stderr, "\n");
                }(),
                std::fprintf(stderr, "[SPU2] irqa=%05X/%05X irqinfo=%04X irqs=%llu v22 nax=%05X lsa=%05X ssa=%05X\n",
                             address(0, Irqa), address(1, Irqa), regs[0x7c2 / 2], static_cast<unsigned long long>(stats.irqAssertions),
                             address(1, 0x1c0 + 22 * 12 + 8), address(1, 0x1c0 + 22 * 12 + 4), address(1, 0x1c0 + 22 * 12));
        }
    };

    IopSpu2::IopSpu2() : m_impl(std::make_unique<Impl>()) { m_impl->reset(); }
    IopSpu2::~IopSpu2() = default;
    void IopSpu2::reset() { m_impl->reset(); }
    uint16_t IopSpu2::read16(uint32_t offset) const
    {
        auto &impl = *m_impl;
        if (offset >= 0x800 || (offset & 1))
            return 0;
        const unsigned core = offset / 0x400;
        const uint32_t local = offset & 0x3ff;
        if (local == Endx || local == Endx + 2)
            return static_cast<uint16_t>(impl.cores[core].endx >> ((local - Endx) * 8));
        if (local == Data)
        {
            const uint32_t word = impl.address(core, Tsa);
            const uint16_t value = impl.ramRead(word);
            impl.setAddress(core, Tsa, word + 1);
            return value;
        }
        if (local < 0x180)
        {
            const auto &voice = impl.cores[core].voices[local / 16];
            if ((local & 15) == 0xa) return static_cast<uint16_t>(voice.envelope);
            if ((local & 15) == 0xc) return static_cast<uint16_t>(voice.left.level);
            if ((local & 15) == 0xe) return static_cast<uint16_t>(voice.right.level);
        }
        for (unsigned masterCore = 0; masterCore != 2; ++masterCore)
        {
            if (offset == 0x770 + masterCore * 0x28) return static_cast<uint16_t>(impl.cores[masterCore].masterLeft.level);
            if (offset == 0x772 + masterCore * 0x28) return static_cast<uint16_t>(impl.cores[masterCore].masterRight.level);
        }
        return impl.regs[offset / 2];
    }
    void IopSpu2::write16(uint32_t offset, uint16_t value)
    {
        auto &impl = *m_impl;
        if (offset >= 0x800 || (offset & 1))
            return;
        const unsigned core = offset / 0x400;
        const uint32_t local = offset & 0x3ff;
        auto &state = impl.cores[core];
        const uint16_t old = impl.regs[offset / 2];
        impl.regs[offset / 2] = value;
        if (impl.trace && (impl.traceWrites++ < 256 || local == Kon || local == Kon + 2 || local == Koff || local == Koff + 2 || local == Attr || local == Admas))
            std::fprintf(stderr, "[SPU2] reg offset=%03X value=%04X frame=%llu\n", offset, value, static_cast<unsigned long long>(impl.stats.frames));
        if (local == Kon || local == Kon + 2)
        {
            state.keyOn |= (static_cast<uint32_t>(value) << ((local - Kon) * 8)) & AllVoices;
            state.keyOff &= ~state.keyOn;
        }
        else if (local == Koff || local == Koff + 2)
            state.keyOff |= (static_cast<uint32_t>(value) << ((local - Koff) * 8)) & AllVoices & ~state.keyOn;
        else if (local == Endx || local == Endx + 2)
            state.endx &= ~(0xffffu << ((local - Endx) * 8));
        else if (local == Attr)
        {
            if (!(value & 0x40))
                impl.regs[0x7c2 / 2] &= static_cast<uint16_t>(~(4 << core));
            if (!state.dmaBusy)
            {
                if (!(value & 0x30)) impl.reg(core, Statx) &= ~0x80u;
                else if (!(old & 0x30)) impl.reg(core, Statx) |= 0x80;
            }
            if (!(value & 0x8000))
            {
                for (auto &voice : state.voices) { voice.phase = Phase::Off; voice.envelope = 0; }
                state.keyOn = state.keyOff = 0;
                state.input.clear();
            }
        }
        else if (local == Data)
        {
            ++impl.pioWrites[core]; // DW5XL-DEBUG
            const uint32_t word = impl.address(core, Tsa);
            impl.ramWrite(word, value);
            impl.setAddress(core, Tsa, word + 1);
        }
        else if (local == Admas && (old & (1u << core)) && !(value & (1u << core)))
            impl.cancelDma(core);
        if (local < 0x180)
        {
            auto &voice = state.voices[local / 16];
            if ((local & 15) == 0) voice.left.write(value);
            if ((local & 15) == 2) voice.right.write(value);
            if ((local & 15) == 0xa) voice.envelope = static_cast<int16_t>(value & 0x7fff);
            if ((local & 15) == 0xc) voice.left.level = static_cast<int16_t>(value);
            if ((local & 15) == 0xe) voice.right.level = static_cast<int16_t>(value);
        }
        if (local >= 0x1c0 && local < 0x2e0)
        {
            const uint32_t sub = (local - 0x1c0) % 12;
            if (sub == 4 || sub == 6)
                state.voices[(local - 0x1c0) / 12].manualLoop = true;
        }
        if (local == 0x2e0 || local == 0x2e2 || local == 0x33c)
            state.reverbPosition = 0;
        for (unsigned masterCore = 0; masterCore != 2; ++masterCore)
        {
            if (offset == 0x760 + masterCore * 0x28) impl.cores[masterCore].masterLeft.write(value);
            if (offset == 0x762 + masterCore * 0x28) impl.cores[masterCore].masterRight.write(value);
        }
    }
    void IopSpu2::dmaTransfer(unsigned core, bool toSpu, uint8_t *data, uint32_t words)
    {
        if (core > 1 || !data)
            return;
        auto &impl = *m_impl;
        auto &state = impl.cores[core];
        state.dmaBusy = true;
        state.dmaAuto = toSpu && (impl.reg(core, Admas) & (1u << core));
        impl.reg(core, Statx) = static_cast<uint16_t>((impl.reg(core, Statx) & ~0x80u) | 0x400);
        const uint32_t start = impl.address(core, Tsa);
        if (state.dmaAuto)
        {
            // AutoDMA receives groups of 256 left halfwords followed by 256
            // right halfwords, not interleaved PCM or an ADPCM/VAG header.
            const auto half = [&](uint32_t index) { return static_cast<int16_t>(data[index * 2] | (data[index * 2 + 1] << 8)); };
            for (uint32_t block = 0; block < words * 2; block += 512)
            {
                const uint32_t leftCount = std::min<uint32_t>(256, words * 2 - block);
                const uint32_t rightCount = words * 2 > block + 256 ? std::min<uint32_t>(256, words * 2 - block - 256) : 0;
                for (uint32_t frame = 0; frame < leftCount; ++frame)
                    state.input.push_back({half(block + frame), frame < rightCount ? half(block + 256 + frame) : int16_t{0}});
            }
        }
        else
        {
            for (uint32_t i = 0; i < words * 2; ++i)
            {
                if (toSpu)
                    impl.ramWrite(start + i, static_cast<uint16_t>(data[i * 2] | (data[i * 2 + 1] << 8)));
                else
                {
                    const uint16_t value = impl.ramRead(start + i);
                    data[i * 2] = static_cast<uint8_t>(value);
                    data[i * 2 + 1] = static_cast<uint8_t>(value >> 8);
                }
            }
            impl.setAddress(core, Tsa, start + words * 2);
        }
        if (toSpu) ++impl.stats.dmaWrites;
        else ++impl.stats.dmaReads;
        impl.stats.dmaBytes += static_cast<uint64_t>(words) * 4;
        if (impl.trace)
            std::fprintf(stderr, "[SPU2] dma core=%u direction=%s words=%u tsa=%05X auto=%u queued=%zu\n",
                         core, toSpu ? "write" : "read", words, start, state.dmaAuto, state.input.size());
    }
    uint64_t IopSpu2::dmaCompletionCycles(unsigned core, uint32_t words) const
    {
        if (core > 1)
            return 64;
        const auto &state = m_impl->cores[core];
        if (state.dmaAuto)
        {
            // MEMIN has two 256-frame halves. DMA completes when its last
            // half is filled, before the mixer consumes that queued half.
            // The retail libsd IRQ handler can therefore submit the next
            // transfer while genuine samples remain. Include the previous
            // transfer's unplayed tail in this transfer's fill deadline.
            // Reference: ps2tek SPU2 AutoDMA; PCSX2 SPU2/ReadInput.cpp raises
            // DMA IRQ after the last free-half refill, not at PCM exhaustion.
            const uint64_t queued = state.input.size();
            const uint64_t untilFinalHalf = queued > 256 ? queued - 256 : 0;
            return std::max<uint64_t>(untilFinalHalf * CyclesPerFrame, 64);
        }
        return std::max<uint64_t>(static_cast<uint64_t>(words) * 2, 64);
    }
    void IopSpu2::completeDma(unsigned core)
    {
        if (core > 1)
            return;
        auto &impl = *m_impl;
        auto &state = impl.cores[core];
        if (!state.dmaBusy)
            return;
        state.dmaBusy = false;
        impl.reg(core, Statx) &= ~0x400u;
        if ((impl.reg(core, Attr) & 0x30) || state.dmaAuto)
            impl.reg(core, Statx) |= 0x80;
    }
    void IopSpu2::cancelDma(unsigned core)
    {
        if (core < 2)
            m_impl->cancelDma(core);
    }
    void IopSpu2::advance(uint64_t cycles)
    {
        const uint64_t frames = cycles / CyclesPerFrame;
        const uint64_t total = m_impl->remainder + cycles % CyclesPerFrame;
        m_impl->remainder = total % CyclesPerFrame;
        for (uint64_t frame = 0; frame < frames + total / CyclesPerFrame; ++frame)
            m_impl->frame();
    }
    void IopSpu2::flushPcm() { m_impl->flush(); }
    void IopSpu2::setPcmCallback(std::function<void(std::span<const int16_t>)> callback) { m_impl->pcmCallback = std::move(callback); }
    void IopSpu2::setIrqCallback(std::function<void()> callback) { m_impl->irqCallback = std::move(callback); }
    IopSpu2::Statistics IopSpu2::statistics() const { return m_impl->stats; }
}

// DW5XL (issue 027): save states.
namespace ps2x::iop::detail
{
    void IopSpu2::serializeState(ps2x::StateArchive &a)
    {
        Impl &s = *m_impl;
        a.tag("SPU2");
        a.vector(s.ram);
        a.value(s.regs);
        for (auto &core : s.cores)
        {
            a.value(core.voices);
            a.value(core.masterLeft);
            a.value(core.masterRight);
            a.value(core.keyOn);
            a.value(core.keyOff);
            a.value(core.endx);
            a.value(core.noise);
            a.value(core.noiseTimer);
            a.value(core.dmaBusy);
            a.value(core.dmaAuto);
            a.deque(core.input);
            a.value(core.inputPosition);
            a.value(core.reverbPosition);
            a.value(core.reverbOutput);
            a.value(core.reverbInput);
        }
        a.value(s.remainder);
        if (a.loading())
            s.pcm.clear(); // host output buffer: refilled from the restored voices
    }
}
