#ifndef PS2X_FIELD_HISTORY_H
#define PS2X_FIELD_HISTORY_H

#include <algorithm>
#include <cstdint>
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
        const bool consecutive = sameGeometry && m_lastTick != std::numeric_limits<uint64_t>::max() &&
                                 tick == m_lastTick + 1u && odd != m_lastOdd;
        if (!repeat && !consecutive)
            resetUnlocked();
        m_width = width;
        m_inputHeight = height;
        m_stride = strideBytes;
        m_mode = mode;
        const size_t parity = odd ? 1u : 0u;
        auto &current = m_fields[parity];
        current.resize(static_cast<size_t>(fieldHeight) * rowBytes);
        for (uint32_t y = 0; y < fieldHeight; ++y)
        {
            const uint32_t sourceY = mode == Mode::HalfHeightFrame ? y : std::min(2u * y + uint32_t(odd), height - 1u);
            std::memcpy(current.data() + static_cast<size_t>(y) * rowBytes,
                        pixels.data() + static_cast<size_t>(sourceY) * strideBytes, rowBytes);
        }
        m_ticks[parity] = tick;
        const auto &opposite = m_fields[1u - parity];
        const bool pair = tick != 0u && m_ticks[1u - parity] == tick - 1u && opposite.size() == current.size();
        pixels.resize(std::max(pixels.size(), static_cast<size_t>(outputHeight) * strideBytes));
        for (uint32_t y = 0; y < outputHeight; ++y)
        {
            const bool currentRow = (y & 1u) == parity;
            const auto &source = currentRow || !pair ? current : opposite;
            std::memcpy(pixels.data() + static_cast<size_t>(y) * strideBytes,
                        source.data() + static_cast<size_t>(y / 2u) * rowBytes, rowBytes);
        }
        height = outputHeight;
        m_lastTick = tick;
        m_lastOdd = odd;
        m_valid = true;
        return true;
    }

private:
    std::mutex m_mutex;
    std::vector<uint8_t> m_fields[2];
    uint64_t m_ticks[2] = {std::numeric_limits<uint64_t>::max(), std::numeric_limits<uint64_t>::max()};
    uint64_t m_lastTick = 0;
    uint32_t m_width = 0, m_inputHeight = 0;
    size_t m_stride = 0;
    Mode m_mode = Mode::Progressive;
    bool m_lastOdd = false, m_valid = false;
};
#endif
