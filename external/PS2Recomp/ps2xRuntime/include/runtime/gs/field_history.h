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
    // DW5XL-DEBUG issue 023 2J: how the last picture was built (for PS2X_WEAVE_TRACE): woven with the opposite field of
    // the previous tick (by target, 2K: with the previous picture when it was of the other target), with a
    // 3-ticks-old opposite field where stationary, the single field line-doubled, or (2K, during drops) still pixels
    // kept from the previous window picture.
    enum class Kind { None, Pair, StalePair, Doubled, Kept };
    Kind lastKind() const { return m_lastKind; }

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
        m_lastOutput.clear();
        m_lastOutputTick = std::numeric_limits<uint64_t>::max();
    }

public:
    // RGBA rows use strideBytes; width is the number of active pixels. FIELD
    // input contains full-height source rows; FRAME input is one half-height
    // field. Missing opposite fields are line-doubled until a pair is available.
    // target (issue 023 step 2K): the field a FRAME picture was drawn for (0 even, 1 odd; the half line of the game's
    // XYOFFSET), -1 if not known. When the game draws its pictures for alternating fields, every picture is placed on
    // the rows of its target and the history survives dropped fields: a picture following one of the other target is
    // woven with it as a pair (also when fields were dropped in between); a picture following one of its own target
    // keeps the other rows from the previous window picture where the pixels are proven still (its own lines next to
    // the row unchanged, bit-exact; one unchanged side is enough, so a still edge next to motion stays) and
    // line-doubles the rest. In normal play the target is the field's parity and the output is unchanged.
    bool present(std::vector<uint8_t> &pixels, uint32_t width, uint32_t &height,
                 size_t strideBytes, uint64_t tick, bool odd, Mode mode, int target = -1)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (mode == Mode::Progressive)
        {
            resetUnlocked();
            m_fieldPictures = false;
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
        const bool targetKnown = mode == Mode::HalfHeightFrame && target >= 0;
        // Per-field pictures: the game changed the field it draws for (a picture's target differs from the previous
        // picture's). A game that draws every picture with one offset never does, and brings a new picture of one
        // target in field after field; a per-field game brings new pictures of one target in at most two
        // consecutive fields, during drops, where fields showing the previous picture again come in between (measured,
        // issue 023 2K). So three consecutive fields with a new picture of one target switch back; a field showing
        // the previous picture again unchanged starts the count over.
        if (!targetKnown)
        {
            m_fieldPictures = false;
            m_sameTargetRun = 0;
        }
        else if (sameGeometry && m_lastTarget >= 0 && target != m_lastTarget)
        {
            m_fieldPictures = true;
            m_sameTargetRun = 0;
        }
        else if (sameGeometry && tick == m_lastTick + 1u && m_lastTarget >= 0)
        {
            const auto &last = m_fields[m_lastOdd ? 1u : 0u];
            bool shownAgain = last.size() == size_t(height) * rowBytes;
            for (uint32_t y = 0; y < height && shownAgain; ++y)
                shownAgain = std::memcmp(last.data() + size_t(y) * rowBytes, pixels.data() + size_t(y) * strideBytes,
                                         rowBytes) == 0;
            if (shownAgain)
                m_sameTargetRun = 0;
            else if (++m_sameTargetRun >= 2u)
                m_fieldPictures = false;
        }
        else
            m_sameTargetRun = 0;
        const bool byTarget = targetKnown && m_fieldPictures;
        if (byTarget)
            odd = target != 0;
        const bool repeat = sameGeometry && tick == m_lastTick && odd == m_lastOdd;
        // One or two dropped fields keep the history when the parity sequence
        // is still consistent (an odd gap alternates parity). Stale opposite
        // rows are then woven only where the picture is proven stationary.
        constexpr uint64_t kNoTick = std::numeric_limits<uint64_t>::max();
        const uint64_t gap = sameGeometry && m_lastTick != kNoTick && tick > m_lastTick ? tick - m_lastTick : 0u;
        const bool consecutive = gap != 0u && gap <= 3u && ((gap & 1u) != 0u) == (odd != m_lastOdd);
        if (!repeat && !(byTarget ? sameGeometry : consecutive))
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
        const bool pair = oppositeValid && (tick - oppositeTick == 1u || (byTarget && oppositeTick == m_lastTick));
        const bool stalePair = !byTarget && oppositeValid && tick - oppositeTick == 3u;
        // 2K, during drops: the previous picture had this target, and the previous window picture was built from it on
        // the same rows.
        const uint64_t previousTick = m_previousTicks[parity];
        const size_t outputBytes = static_cast<size_t>(outputHeight) * rowBytes;
        const bool kept = byTarget && !pair && previousTick != kNoTick && previousTick == m_lastTick &&
                          m_previousFields[parity].size() == current.size() && m_lastOutputTick == previousTick &&
                          m_lastOutputOdd == odd && m_lastOutput.size() == outputBytes;
        const auto recent = [](uint64_t newer, uint64_t older) {
            return older != kNoTick && newer > older && newer - older <= 4u;
        };
        m_lastKind = pair ? Kind::Pair : stalePair ? Kind::StalePair : kept ? Kind::Kept
                                                            : Kind::Doubled; // DW5XL-DEBUG issue 023 2J
        // By target (2K) the previous pictures of both targets are older than the pair, however long ago (drops).
        const bool temporalHistory = (pair || stalePair) &&
            (byTarget ? m_previousTicks[parity] != kNoTick && m_previousTicks[1u - parity] != kNoTick
                      : recent(tick, m_previousTicks[parity]) && recent(oppositeTick, m_previousTicks[1u - parity])) &&
            m_previousFields[parity].size() == current.size() &&
            m_previousFields[1u - parity].size() == opposite.size();
        pixels.resize(std::max(pixels.size(), static_cast<size_t>(outputHeight) * strideBytes));
        for (uint32_t y = 0; y < outputHeight; ++y)
        {
            const bool currentRow = (y & 1u) == parity;
            uint8_t *dstRow = pixels.data() + static_cast<size_t>(y) * strideBytes;
            // A missing row repeats the picture's line above it; by target (2K) that holds for both fields, so a
            // line-doubled picture of either target puts the scene at the same height.
            const uint32_t doubledLine = byTarget && parity == 1u && !currentRow && y > 0u ? (y - 1u) / 2u : y / 2u;
            const uint8_t *doubledRow = current.data() + static_cast<size_t>(doubledLine) * rowBytes;
            if (currentRow || !(pair || stalePair || kept))
            {
                std::memcpy(dstRow, doubledRow, rowBytes);
            }
            else if (kept)
            {
                // The current picture's lines above and below this row (at the picture's edge, the one there is).
                const uint32_t below = std::min(parity == 0u ? y / 2u + 1u : y / 2u, fieldHeight - 1u);
                const uint32_t above = parity == 0u ? y / 2u : (y >= 2u ? y / 2u - 1u : below);
                const size_t top = static_cast<size_t>(above) * rowBytes, bot = static_cast<size_t>(below) * rowBytes;
                const uint8_t *source = m_lastOutput.data() + static_cast<size_t>(y) * rowBytes;
                const uint8_t *cur = current.data(), *old = m_previousFields[parity].data();
                for (uint32_t x = 0; x < width; ++x)
                {
                    const size_t px = static_cast<size_t>(x) * 4u;
                    bool topStill = true, botStill = true;
                    for (size_t ch = 0; ch < 3u; ++ch)
                    {
                        topStill = topStill && cur[top + px + ch] == old[top + px + ch];
                        botStill = botStill && cur[bot + px + ch] == old[bot + px + ch];
                    }
                    const bool still = topStill || botStill;
                    std::memcpy(dstRow + px, still ? source + px : doubledRow + px, 4u);
                }
            }
            else
            {
                const int32_t topIdx = parity == 0u ? static_cast<int32_t>(y / 2u)
                                                    : static_cast<int32_t>(y / 2u) - 1;
                const int32_t botIdx = parity == 0u ? static_cast<int32_t>(y / 2u) + 1
                                                    : static_cast<int32_t>(y / 2u);
                const uint8_t *oppRow = opposite.data() + static_cast<size_t>(y / 2u) * rowBytes;
                if ((topIdx < 0 || botIdx >= static_cast<int32_t>(fieldHeight)) && !stalePair && tick - oppositeTick > 1u)
                {
                    // 2K: the edge row of a pair across dropped fields has one neighbour line; the older opposite
                    // picture is woven there only where proven still (that line and its own sample unchanged).
                    const size_t edge = static_cast<size_t>(topIdx < 0 ? botIdx : topIdx) * rowBytes;
                    const uint8_t *oldOpp = m_previousFields[1u - parity].data() + static_cast<size_t>(y / 2u) * rowBytes;
                    for (uint32_t x = 0; x < width; ++x)
                    {
                        const size_t px = static_cast<size_t>(x) * 4u;
                        bool still = temporalHistory;
                        for (size_t ch = 0; ch < 3u && still; ++ch)
                            still = current[edge + px + ch] == m_previousFields[parity][edge + px + ch] &&
                                    oppRow[px + ch] == oldOpp[px + ch];
                        std::memcpy(dstRow + px, still ? oppRow + px : doubledRow + px, 4u);
                    }
                }
                else if (topIdx < 0 || botIdx >= static_cast<int32_t>(fieldHeight))
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
        if (byTarget)
        {
            m_lastOutput.resize(outputBytes);
            for (uint32_t y = 0; y < outputHeight; ++y)
                std::memcpy(m_lastOutput.data() + static_cast<size_t>(y) * rowBytes,
                            pixels.data() + static_cast<size_t>(y) * strideBytes, rowBytes);
            m_lastOutputTick = tick;
            m_lastOutputOdd = odd;
        }
        else
        {
            m_lastOutput.clear();
            m_lastOutputTick = kNoTick;
        }
        m_lastTarget = targetKnown ? target : -1;
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
    Kind m_lastKind = Kind::None; // DW5XL-DEBUG issue 023 2J
    // Issue 023 step 2K: the window picture last built by target, and whether the game draws per-field pictures.
    std::vector<uint8_t> m_lastOutput;
    uint64_t m_lastOutputTick = std::numeric_limits<uint64_t>::max();
    bool m_lastOutputOdd = false;
    int m_lastTarget = -1;
    bool m_fieldPictures = false;
    uint32_t m_sameTargetRun = 0; // consecutive fields that brought a new picture of the previous picture's target
    uint32_t m_width = 0, m_inputHeight = 0;
    size_t m_stride = 0;
    Mode m_mode = Mode::Progressive;
    bool m_lastOdd = false, m_valid = false;
};
#endif
