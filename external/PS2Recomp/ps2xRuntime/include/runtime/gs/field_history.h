#ifndef PS2X_FIELD_HISTORY_H
#define PS2X_FIELD_HISTORY_H

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

// History belongs to a scanout instance, not a framebuffer address. Reusing a
// framebuffer for alternating fields is valid; pairing distant fields is not.
class ReusableFieldWeaver
{
public:
    enum class Mode { Progressive, FullHeightField, HalfHeightFrame };

    void reset()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        resetUnlocked();
    }

private:
    void resetUnlocked()
    {
        m_valid = false;
        m_fields[0].clear();
        m_fields[1].clear();
        m_previousFields[0].clear();
        m_previousFields[1].clear();
        m_previousTicks[0] = m_previousTicks[1] = std::numeric_limits<uint64_t>::max();
        m_ticks[0] = m_ticks[1] = std::numeric_limits<uint64_t>::max();
    }

public:
    // RGBA rows use strideBytes; width is the number of active pixels. FIELD
    // input contains full-height source rows; FRAME input is one half-height
    // field. Missing opposite fields are line-doubled until a pair is available.
    bool present(std::vector<uint8_t> &pixels, uint32_t width, uint32_t &height,
                 size_t strideBytes, uint64_t tick, bool odd, Mode mode)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (mode == Mode::Progressive)
        {
            resetUnlocked();
            return true;
        }
        const uint64_t rowBytes64 = uint64_t(width) * 4u;
        const uint64_t inputBytes = uint64_t(height) * strideBytes;
        const uint64_t outputHeight64 = mode == Mode::HalfHeightFrame ? uint64_t(height) * 2u : height;
        if (!width || !height || rowBytes64 > strideBytes || inputBytes > pixels.size() ||
            outputHeight64 > std::numeric_limits<uint32_t>::max() ||
            outputHeight64 > std::numeric_limits<size_t>::max() / strideBytes)
        {
            resetUnlocked();
            return false;
        }
        const uint32_t outputHeight = static_cast<uint32_t>(outputHeight64);
        const uint32_t fieldHeight = mode == Mode::HalfHeightFrame ? height : height / 2u + height % 2u;
        const size_t rowBytes = static_cast<size_t>(rowBytes64);
        const bool sameGeometry = m_valid && m_width == width && m_inputHeight == height &&
                                  m_stride == strideBytes && m_mode == mode;
        const bool repeat = sameGeometry && tick == m_lastTick && odd == m_lastOdd;
        // One or two dropped fields keep the history when the parity sequence
        // is still consistent (an odd gap alternates parity). Stale opposite
        // rows are then woven only where the picture is proven stationary.
        constexpr uint64_t kNoTick = std::numeric_limits<uint64_t>::max();
        const uint64_t gap = sameGeometry && m_lastTick != kNoTick && tick > m_lastTick ? tick - m_lastTick : 0u;
        const bool consecutive = gap != 0u && gap <= 3u && ((gap & 1u) != 0u) == (odd != m_lastOdd);
        if (!repeat && !consecutive)
            resetUnlocked();
        m_width = width;
        m_inputHeight = height;
        m_stride = strideBytes;
        m_mode = mode;
        const size_t parity = odd ? 1u : 0u;
        auto &current = m_fields[parity];
        if (!repeat)
        {
            // Retain the previous sample of each parity. Swapping reuses the
            // buffers instead of allocating/copying another full field.
            m_previousFields[parity].swap(current);
            m_previousTicks[parity] = m_ticks[parity];
        }
        current.resize(static_cast<size_t>(fieldHeight) * rowBytes);
        for (uint32_t y = 0; y < fieldHeight; ++y)
        {
            const uint32_t sourceY = mode == Mode::HalfHeightFrame ? y : std::min(2u * y + uint32_t(odd), height - 1u);
            std::memcpy(current.data() + static_cast<size_t>(y) * rowBytes,
                        pixels.data() + static_cast<size_t>(sourceY) * strideBytes, rowBytes);
        }
        m_ticks[parity] = tick;
        const auto &opposite = m_fields[1u - parity];
        const uint64_t oppositeTick = m_ticks[1u - parity];
        const bool oppositeValid = oppositeTick != kNoTick && tick > oppositeTick && opposite.size() == current.size();
        const bool pair = oppositeValid && tick - oppositeTick == 1u;
        const bool stalePair = oppositeValid && tick - oppositeTick == 3u;
        const auto recent = [](uint64_t newer, uint64_t older) {
            return older != kNoTick && newer > older && newer - older <= 4u;
        };
        const bool temporalHistory = (pair || stalePair) &&
            recent(tick, m_previousTicks[parity]) &&
            recent(oppositeTick, m_previousTicks[1u - parity]) &&
            m_previousFields[parity].size() == current.size() &&
            m_previousFields[1u - parity].size() == opposite.size();
        pixels.resize(std::max(pixels.size(), static_cast<size_t>(outputHeight) * strideBytes));
        for (uint32_t y = 0; y < outputHeight; ++y)
        {
            const bool currentRow = (y & 1u) == parity;
            uint8_t *dstRow = pixels.data() + static_cast<size_t>(y) * strideBytes;
            const uint8_t *doubledRow = current.data() + static_cast<size_t>(y / 2u) * rowBytes;
            if (currentRow || !(pair || stalePair))
            {
                std::memcpy(dstRow, doubledRow, rowBytes);
            }
            else
            {
                const int32_t topIdx = parity == 0u ? static_cast<int32_t>(y / 2u)
                                                    : static_cast<int32_t>(y / 2u) - 1;
                const int32_t botIdx = parity == 0u ? static_cast<int32_t>(y / 2u) + 1
                                                    : static_cast<int32_t>(y / 2u);
                const uint8_t *oppRow = opposite.data() + static_cast<size_t>(y / 2u) * rowBytes;
                if (topIdx < 0 || botIdx >= static_cast<int32_t>(fieldHeight))
                {
                    std::memcpy(dstRow, stalePair ? doubledRow : oppRow, rowBytes);
                }
                else
                {
                    const uint8_t *topRow = current.data() + static_cast<size_t>(topIdx) * rowBytes;
                    const uint8_t *botRow = current.data() + static_cast<size_t>(botIdx) * rowBytes;
                    for (uint32_t x = 0; x < width; ++x)
                    {
                        const size_t px = static_cast<size_t>(x) * 4u;
                        // Spatial contrast alone also describes stationary
                        // one-line glyph strokes. Preserve that detail only
                        // when BOTH parity histories agree at these samples.
                        bool stationary = temporalHistory;
                        if (stationary)
                        {
                            const uint8_t *oldOpp = m_previousFields[1u - parity].data() +
                                static_cast<size_t>(y / 2u) * rowBytes;
                            const uint8_t *oldTop = m_previousFields[parity].data() +
                                static_cast<size_t>(topIdx) * rowBytes;
                            const uint8_t *oldBot = m_previousFields[parity].data() +
                                static_cast<size_t>(botIdx) * rowBytes;
                            for (size_t ch = 0; ch < 3u; ++ch)
                                stationary = stationary && oppRow[px + ch] == oldOpp[px + ch] &&
                                    topRow[px + ch] == oldTop[px + ch] &&
                                    botRow[px + ch] == oldBot[px + ch];
                        }
                        if (stalePair && !stationary)
                        {
                            // Never weave a stale opposite sample that may have moved.
                            std::memcpy(dstRow + px, doubledRow + px, 4u);
                            continue;
                        }
                        int combMax = 0;
                        for (size_t ch = 0; ch < 3u && !stationary; ++ch)
                        {
                            const int vo = oppRow[px + ch];
                            const int vt = topRow[px + ch];
                            const int vb = botRow[px + ch];
                            const int low = std::min(vt, vb);
                            const int high = std::max(vt, vb);
                            // Low enough to catch faint motion combing on ground textures;
                            // stationary detail is protected by the temporal guard above.
                            constexpr int T = 8;
                            int d = 0;
                            if (vo > high + T)
                                d = vo - (high + T);
                            else if (vo < low - T)
                                d = (low - T) - vo;
                            if (d > combMax)
                                combMax = d;
                        }
                        if (combMax == 0)
                        {
                            dstRow[px + 0] = oppRow[px + 0];
                            dstRow[px + 1] = oppRow[px + 1];
                            dstRow[px + 2] = oppRow[px + 2];
                        }
                        else
                        {
                            const int w = std::min(32, combMax * 4);
                            for (size_t ch = 0; ch < 3u; ++ch)
                            {
                                const int vo = oppRow[px + ch];
                                const int vt = topRow[px + ch];
                                const int vb = botRow[px + ch];
                                const int avg = (vt + vb + 1) / 2;
                                dstRow[px + ch] = static_cast<uint8_t>(((32 - w) * vo + w * avg + 16) / 32);
                            }
                        }
                        dstRow[px + 3] = oppRow[px + 3];
                    }
                }
            }
        }
        // DW5XL-DEBUG issue 023: PS2X_WEAVE_TRACE=1 logs every weave: field, parity, mode and whether the picture was
        // woven from a pair, a stale pair or line-doubled (a line-doubled odd field sits one line below an even one).
        static const bool s_weaveTrace = [] { const char *v = std::getenv("PS2X_WEAVE_TRACE"); return v && *v && *v != '0'; }();
        if (s_weaveTrace)
            std::fprintf(stderr, "[weave] tick=%llu odd=%d mode=%d repeat=%d consecutive=%d pair=%d stale=%d oppTick=%lld out=%u\n",
                         static_cast<unsigned long long>(tick), odd ? 1 : 0, static_cast<int>(mode), repeat ? 1 : 0,
                         consecutive ? 1 : 0, pair ? 1 : 0, stalePair ? 1 : 0,
                         oppositeTick == kNoTick ? -1ll : static_cast<long long>(oppositeTick), outputHeight);
        height = outputHeight;
        m_lastTick = tick;
        m_lastOdd = odd;
        m_valid = true;
        return true;
    }

private:
    std::mutex m_mutex;
    std::vector<uint8_t> m_fields[2];
    std::vector<uint8_t> m_previousFields[2];
    uint64_t m_previousTicks[2] = {std::numeric_limits<uint64_t>::max(), std::numeric_limits<uint64_t>::max()};
    uint64_t m_ticks[2] = {std::numeric_limits<uint64_t>::max(), std::numeric_limits<uint64_t>::max()};
    uint64_t m_lastTick = 0;
    uint32_t m_width = 0, m_inputHeight = 0;
    size_t m_stride = 0;
    Mode m_mode = Mode::Progressive;
    bool m_lastOdd = false, m_valid = false;
};
#endif
