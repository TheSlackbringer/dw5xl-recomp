// Issue 049 Part 2, amendment v3.5: the time line of the in-between pictures (see dw5xl_smooth_time.h).
#include "dw5xl_smooth_time.h"

#include <algorithm>
#include <cmath>

namespace dw5xl::smooth
{
    namespace
    {
        constexpr double kEps = 1e-6; // ms: two instants closer than this are the same instant
    }

    void TimeLine::reset()
    {
        m_have = false;
        m_length = 0.0;
        m_haveEnd = false;
        m_haveHead = false;
        m_haveRefresh = false;
    }

    void PictureBudget::prune(Window &window, double nowMs)
    {
        while (!window.empty() && window.front().first < nowMs - kWindowMs)
            window.pop_front();
    }

    void PictureBudget::add(Window &window, double nowMs, double value)
    {
        prune(window, nowMs);
        while (!window.empty() && window.back().second <= value)
            window.pop_back();
        window.emplace_back(nowMs, value);
    }

    void PictureBudget::measured(double nowMs, double pictureMs, bool first)
    {
        if (!(pictureMs > 0.0))
            return;
        add(first ? m_costs : m_next, nowMs, pictureMs);
        if (first)
            m_probing = false;
    }

    void PictureBudget::gameTook(double nowMs, double ms)
    {
        if (ms > 0.0)
            add(m_game, nowMs, ms);
    }

    void PictureBudget::frame(double nowMs, double instantMs, double fieldMs, double reserveMs)
    {
        prune(m_costs, nowMs);
        prune(m_next, nowMs);
        prune(m_game, nowMs);
        m_allowance = m_game.empty() ? 0.0 : std::max(0.0, fieldMs - m_game.front().second - reserveMs);
        m_latest = instantMs + m_allowance;
        m_busyUntil = nowMs;
        m_admitted = 0;
    }

    bool PictureBudget::admit(double nowMs)
    {
        const double start = std::max(nowMs, m_busyUntil);
        const double takes = m_admitted == 0 ? cost() : nextCost();
        if (!known())
        {
            if (m_admitted != 0 || start >= m_latest || (m_probing && nowMs < m_probeUntil))
                return false;
            m_probing = true;
            m_probeUntil = nowMs + kProbeMs;
        }
        else if (start + takes > m_latest)
        {
            return false;
        }
        m_busyUntil = start + takes;
        ++m_admitted;
        return true;
    }

    void PictureBudget::afterDevice(double nowMs, double ms)
    {
        if (ms >= 0.0)
            add(m_after, nowMs, ms);
    }

    void PictureBudget::frameOwn(double nowMs, double untilMs)
    {
        prune(m_costs, nowMs);
        prune(m_next, nowMs);
        prune(m_after, nowMs);
        m_allowance = 0.0;
        m_latest = untilMs;
        m_busyUntil = nowMs;
        m_admitted = 0;
    }

    bool PictureBudget::admitBy(double nowMs, double atMs, double *missBy)
    {
        if (missBy)
            *missBy = 0.0;
        const double start = std::max(nowMs, m_busyUntil);
        if (!known())
        {
            // What a picture costs is not known: one is let through to learn it, as in admit().
            if (m_admitted != 0 || start >= m_latest || (m_probing && nowMs < m_probeUntil))
                return false;
            m_probing = true;
            m_probeUntil = nowMs + kProbeMs;
            ++m_admitted;
            return true;
        }
        const double left = start + (m_admitted == 0 ? cost() : nextCost()); // when it will have left the device
        const double miss = left + after() - atMs;
        if (missBy)
            *missBy = miss;
        if (miss > 0.0 || left > m_latest)
            return false;
        m_busyUntil = left;
        ++m_admitted;
        return true;
    }

    FinishedFrames::Coming FinishedFrames::finished(uint32_t list, uint64_t hash, uint64_t lastField, double lastStart,
                                                    double period)
    {
        if (m_waiting && m_list == list)
        {
            Coming again = m_coming;
            again.again = true;
            return again;
        }
        // A frame that was finished and never sent as a new frame keeps its number; this one gets the next.
        m_waiting = true;
        m_list = list;
        m_hash = hash;
        m_coming.number = ++m_count;
        m_coming.field = lastField + 1u;
        m_coming.instant = lastStart + period;
        m_coming.again = false;
        return m_coming;
    }

    FinishedFrames::Sent FinishedFrames::sent(uint32_t list, uint64_t hash, uint64_t field)
    {
        Sent sent;
        if (m_waiting && m_list == list)
        {
            sent.number = m_coming.number;
            sent.finished = true;
            sent.sameField = m_coming.field == field;
            sent.sameList = hash == 0u || m_hash == 0u || hash == m_hash;
            sent.instant = m_coming.instant;
        }
        else
        {
            sent.number = ++m_count; // a frame that did not come through the frame end
        }
        m_waiting = false;
        return sent;
    }

    void HeadStartMeter::add(double nowMs, double used, double spare)
    {
        const double need = used - spare;
        if (!m_any)
        {
            m_any = true;
            m_firstMs = nowMs;
        }
        m_lastMs = nowMs;
        while (!m_needs.empty() && m_needs.back().second <= need)
            m_needs.pop_back();
        m_needs.emplace_back(nowMs, need);
        while (m_needs.front().first < nowMs - kWindowMs)
            m_needs.pop_front();
    }

    void RateChoice::configure(bool follows, double displayPeriod, double fieldPeriod, int mostPerFrame)
    {
        const int most = mostPerFrame < 1 ? 1 : mostPerFrame;
        if (m_configured && follows == m_follows && std::fabs(displayPeriod - m_displayPeriod) < 1e-6 &&
            std::fabs(fieldPeriod - m_fieldPeriod) < 1e-6 && most == m_most)
            return;
        m_configured = true;
        m_follows = follows;
        m_displayPeriod = displayPeriod;
        m_fieldPeriod = fieldPeriod;
        m_most = most;
        if (follows)
        {
            // One to `most` pictures per frame, not faster than the display's top rate.
            int top = most;
            while (top > 1 && displayPeriod > 0.0 && fieldPeriod / static_cast<double>(top) < displayPeriod - kEps)
                --top;
            m_levels = top;
        }
        else
        {
            // Steps from the largest that still shows the game's own rate down to every refresh; no more pictures
            // per frame than `most` (rounded up: a frame's share of the refreshes is not a whole number).
            const double perFrame = displayPeriod > 0.0 ? fieldPeriod / displayPeriod : 1.0;
            int largest = static_cast<int>(std::floor(perFrame + kEps));
            if (largest < 1)
                largest = 1;
            int smallest = 1;
            while (smallest < largest && std::ceil(perFrame / static_cast<double>(smallest) - kEps) > static_cast<double>(most))
                ++smallest;
            m_levels = largest - smallest + 1;
        }
        m_level = 0;
        m_count = 0;
        m_frames.clear();
        m_notBefore.assign(static_cast<size_t>(m_levels) + 1u, 0);
        m_wait.assign(static_cast<size_t>(m_levels) + 1u, kFirstWait);
    }

    int RateChoice::step() const
    {
        if (m_follows || m_level < 0)
            return 1;
        const double perFrame = m_displayPeriod > 0.0 ? m_fieldPeriod / m_displayPeriod : 1.0;
        int largest = static_cast<int>(std::floor(perFrame + kEps));
        if (largest < 1)
            largest = 1;
        return largest - m_level;
    }

    int RateChoice::perFrame() const { return m_follows && m_level >= 0 ? m_level + 1 : 1; }

    double RateChoice::picturesPerFrame(int level) const
    {
        if (level < 0)
            return 0.0;
        if (m_follows)
            return static_cast<double>(level + 1);
        const double perFrame = m_displayPeriod > 0.0 ? m_fieldPeriod / m_displayPeriod : 1.0;
        int largest = static_cast<int>(std::floor(perFrame + kEps));
        if (largest < 1)
            largest = 1;
        return perFrame / static_cast<double>(largest - level);
    }

    int RateChoice::largestBatch(int level) const
    {
        return level < 0 ? 0 : static_cast<int>(std::ceil(picturesPerFrame(level) - kEps));
    }

    void RateChoice::changed()
    {
        m_frames.clear();
        m_settle = kSettle;
        m_leastRoom = 1e18;
        m_leastSpare = 1e18;
    }

    void RateChoice::frame(const Frame &frame)
    {
        if (!m_configured)
            return;
        ++m_count;
        if (m_level < 0)
        {
            // Off: the fewest pictures are tried again when their wait is over.
            if (m_count >= m_notBefore[0])
            {
                m_level = 0;
                changed();
            }
            return;
        }
        if (m_settle > 0)
        {
            --m_settle;
            return;
        }
        m_frames.push_back(frame);
        while (m_frames.size() > static_cast<size_t>(kWindow))
            m_frames.pop_front();

        m_leastRoom = std::min(m_leastRoom, frame.room);
        m_leastSpare = std::min(m_leastSpare, frame.spare);

        // Down: the game was held up repeatedly in the last frames.
        int heldUp = 0, moments = 0;
        bool before = false;
        for (const Frame &f : m_frames)
        {
            heldUp += f.heldUp || f.noRoom ? 1 : 0;
            moments += f.heldUp && !before ? 1 : 0;
            before = f.heldUp;
        }
        if (moments >= kHeldUpMoments || heldUp >= kHeldUpFrames)
        {
            const size_t failed = static_cast<size_t>(m_level);
            m_notBefore[failed] = m_count + m_wait[failed];
            m_wait[failed] = m_wait[failed] * 2 > kLongestWait ? kLongestWait : m_wait[failed] * 2;
            --m_level; // below the fewest: off
            changed();
            return;
        }

        // Up: a full window without the game held up, the next level's wait over, and room before the field's start
        // in the tightest frame since this level began for the pictures the next level adds to a frame's largest batch.
        if (m_level + 1 >= m_levels || m_frames.size() < static_cast<size_t>(kWindow) || heldUp != 0 ||
            m_count < m_notBefore[static_cast<size_t>(m_level) + 1u])
            return;
        const int more = largestBatch(m_level + 1) - largestBatch(m_level);
        if (more > 0)
        {
            std::vector<double> costs;
            for (const Frame &f : m_frames)
            {
                if (f.cost > 0.0)
                    costs.push_back(f.cost);
            }
            if (costs.empty())
                return; // what a picture costs is not known yet
            std::nth_element(costs.begin(), costs.begin() + static_cast<std::ptrdiff_t>(costs.size() / 2u), costs.end());
            if (m_leastRoom - static_cast<double>(more) * costs[costs.size() / 2u] < 0.0)
                return;
        }
        ++m_level;
        changed();
    }

    bool fieldClockBroke(uint64_t beforeField, double beforeStart, uint64_t field, double start, double fieldMs)
    {
        if (!(fieldMs > 0.0))
            return false;
        if (field <= beforeField)
            return true;
        const double expected = static_cast<double>(field - beforeField) * fieldMs;
        return std::fabs((start - beforeStart) - expected) > fieldMs / 2.0;
    }

    double TimeLine::frame(uint32_t number, double instant, double field, const Display &display, std::vector<Picture> &out)
    {
        if (!m_have || number != m_number + 1u || !(instant > m_instant) || !(field > 0.0))
        {
            // The first frame, or a break in the frames (another scene in between): the line starts again with it.
            reset();
            m_have = true;
            m_number = number;
            m_instant = instant;
            return -1.0;
        }
        const double length = instant - m_instant;

        // The head start moves towards the wanted one, slowly.
        if (!m_haveHead)
        {
            m_headStart = m_wanted;
            m_haveHead = true;
        }
        else
        {
            const double most = kHeadStartRate * length;
            m_headStart += std::clamp(m_wanted - m_headStart, -most, most);
        }

        // How long this stretch is on screen: one field; when the last two frames were both longer, their length
        // (the game runs steadily below its rate, and the next frame is expected as late as these came).
        const long fields = std::max(1L, std::lround(length / field));
        const long before = m_length > 0.0 ? std::max(1L, std::lround(m_length / field)) : 1L;
        const double shown = static_cast<double>(fields >= 2 && before >= 2 ? std::min(fields, before) : 1L) * field;

        double start = instant + m_headStart;
        const double end = start + shown;
        double voidFrom = -1.0;
        if (m_haveEnd)
        {
            if (std::fabs(m_end - start) < field / 4.0)
                start = m_end; // on schedule: one unbroken line (the head start's small moves are taken up here)
            else if (m_end > start)
                voidFrom = start; // the frame came sooner than the line expected: the line moves on to it
            // else: the game missed fields; nothing is made for the gap and the last picture stays
        }
        const double span = end - start;

        if (span > kEps)
        {
            if (!display.follows && display.period > 0.0)
            {
                const double period = display.period;
                const auto instantOf = [&](int64_t refresh) { return display.origin + static_cast<double>(refresh) * period; };
                int64_t refresh;
                if (m_haveRefresh && m_lastStep == m_step)
                {
                    // Keep the same refreshes of the step across stretches and gaps.
                    const double stride = period * static_cast<double>(m_step);
                    const auto steps = static_cast<int64_t>(std::floor((start + kEps - instantOf(m_lastRefresh)) / stride)) + 1;
                    refresh = m_lastRefresh + steps * m_step;
                }
                else
                {
                    refresh = static_cast<int64_t>(std::floor((start + kEps - display.origin) / period)) + 1;
                }
                for (; instantOf(refresh) <= end + kEps; refresh += m_step)
                {
                    Picture p;
                    p.at = instantOf(refresh);
                    p.refresh = refresh;
                    p.frame = number;
                    p.t = std::clamp((p.at - start) / span, 0.0, 1.0);
                    p.own = p.t >= 1.0 - 1e-9;
                    if (p.own)
                        p.t = 1.0;
                    out.push_back(p);
                    m_haveRefresh = true;
                    m_lastRefresh = refresh;
                }
                m_lastStep = m_step;
            }
            else
            {
                // An even cadence of our own; not faster than the display's top rate.
                int count = m_perFrame;
                while (count > 1 && display.period > 0.0 && span / static_cast<double>(count) < display.period - kEps)
                    --count;
                for (int i = 1; i <= count; ++i)
                {
                    Picture p;
                    p.t = i == count ? 1.0 : static_cast<double>(i) / static_cast<double>(count);
                    p.at = i == count ? end : start + span * p.t;
                    p.frame = number;
                    p.own = i == count;
                    out.push_back(p);
                }
                m_haveRefresh = false;
            }
        }

        m_start = start;
        m_end = end;
        m_haveEnd = true;
        m_length = length;
        m_number = number;
        m_instant = instant;
        return voidFrom;
    }
}
