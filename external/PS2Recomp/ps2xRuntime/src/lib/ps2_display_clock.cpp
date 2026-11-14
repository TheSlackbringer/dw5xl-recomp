// Issue 049 Part 2, amendment v3.5: the display's clock (see runtime/ps2_display_clock.h).
#include "runtime/ps2_display_clock.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

namespace ps2x::display_clock
{
    int64_t refreshAfter(const Grid &grid, double ms)
    {
        if (!(grid.periodMs > 0.0))
            return 0;
        return static_cast<int64_t>(std::floor((ms + 1e-6 - grid.originMs) / grid.periodMs)) + 1;
    }

    bool parseTestDisplay(const char *text, double nowMs, Grid &out)
    {
        if (!text || !*text)
            return false;
        Grid grid;
        if (std::strncmp(text, "follow", 6) == 0)
        {
            grid.follows = true;
            if (text[6] == ':')
            {
                const double hz = std::atof(text + 7);
                if (!(hz > 0.0))
                    return false;
                grid.periodMs = 1000.0 / hz;
            }
            else if (text[6] != '\0')
                return false;
            out = grid;
            return true;
        }
        char *end = nullptr;
        const double hz = std::strtod(text, &end);
        if (end == text || *end != '\0' || !(hz >= 1.0) || hz > 2000.0)
            return false;
        grid.periodMs = 1000.0 / hz;
        grid.originMs = nowMs;
        out = grid;
        return true;
    }

    Estimator::Estimator(double nominalPeriodMs, size_t window)
        : m_nominalMs(nominalPeriodMs), m_window(window < 8 ? 8 : window), m_periodMs(nominalPeriodMs)
    {
    }

    void Estimator::reset()
    {
        m_reports.clear();
        m_periodMs = m_nominalMs;
        m_originMs = 0.0;
        m_spreadMs = 0.0;
    }

    void Estimator::report(double ms)
    {
        double number = 0.0;
        if (!m_reports.empty())
        {
            // Refreshes since the report before: from the period known so far, at least one.
            const double steps = std::round((ms - m_reports.back().ms) / m_periodMs);
            number = m_reports.back().number + (steps < 1.0 ? 1.0 : steps);
        }
        m_reports.push_back({number, ms});
        fit();
    }

    void Estimator::report(int64_t number, double ms)
    {
        if (m_reports.empty())
            m_firstNumber = static_cast<double>(number);
        const double n = static_cast<double>(number) - m_firstNumber;
        if (!m_reports.empty() && n <= m_reports.back().number)
            return; // an old or repeated report
        m_reports.push_back({n, ms});
        fit();
    }

    void Estimator::fit()
    {
        while (m_reports.size() > m_window)
            m_reports.pop_front();
        const double count = static_cast<double>(m_reports.size());
        if (m_reports.size() < 2)
        {
            m_originMs = m_reports.empty() ? 0.0 : m_reports.front().ms - m_reports.front().number * m_periodMs;
            return;
        }
        // Least squares around the means (the numbers grow without bound; the sums stay small this way).
        double meanN = 0.0, meanT = 0.0;
        for (const Report &r : m_reports)
        {
            meanN += r.number;
            meanT += r.ms;
        }
        meanN /= count;
        meanT /= count;
        double nn = 0.0, nt = 0.0;
        for (const Report &r : m_reports)
        {
            nn += (r.number - meanN) * (r.number - meanN);
            nt += (r.number - meanN) * (r.ms - meanT);
        }
        // With few reports the slope is not trustworthy yet: the nominal period holds until 8 are in.
        if (m_reports.size() >= 8 && nn > 0.0)
            m_periodMs = nt / nn;
        m_originMs = meanT - m_periodMs * meanN;
        double squares = 0.0;
        for (const Report &r : m_reports)
        {
            const double off = r.ms - (m_originMs + m_periodMs * r.number);
            squares += off * off;
        }
        m_spreadMs = std::sqrt(squares / count);
    }

    bool Estimator::settled() const
    {
        return m_reports.size() >= 8 && std::fabs(m_periodMs - m_nominalMs) <= 0.05 * m_nominalMs;
    }

    // ---- A real display's clock from the driver's reports ----
    namespace
    {
        const size_t kEnoughReports = 12; // before anything is said about the display
        const size_t kOffsKept = 32;
        const size_t kLatenciesKept = 128;
        const size_t kAimedKept = 32;

        // The value a given part of the way through the sorted values (0 = smallest, 1 = largest).
        double partOf(std::deque<double> values, double part)
        {
            std::sort(values.begin(), values.end());
            const double at = part * static_cast<double>(values.size() - 1);
            const size_t low = static_cast<size_t>(std::floor(at));
            const size_t high = std::min(low + 1, values.size() - 1);
            return values[low] + (values[high] - values[low]) * (at - static_cast<double>(low));
        }
    }

    void RealDisplay::reset()
    {
        m_line = Estimator(m_nominalMs > 0.0 ? m_nominalMs : 1.0);
        m_offs.clear();
        m_latencies.clear();
        m_aimed.clear();
        m_missed.clear();
        m_haveLead = false;
        m_kind = Kind::Unknown;
        m_have = false;
        m_lastPresent = 0;
        m_lastOutMs = 0.0;
        m_leadMs = 0.0;
    }

    void RealDisplay::refresh(double periodMs)
    {
        if (!(periodMs > 0.0))
            periodMs = 0.0;
        if (std::fabs(periodMs - m_nominalMs) <= 0.005 * m_nominalMs)
            return;
        m_nominalMs = periodMs;
        reset();
    }

    void RealDisplay::report(const ShownReport &report)
    {
        if (!(m_nominalMs > 0.0))
            return;
        if (m_have && report.present <= m_lastPresent)
            return; // an old or repeated report
        if (m_have)
        {
            const double gap = report.outMs - m_lastOutMs;
            // A long silence (a minimised window, a screen that was off): what was known may not hold any more.
            if (gap > 64.0 * m_nominalMs)
                reset();
            // Two pictures in one refresh: the earlier one was never on screen; this report says nothing new.
            else if (gap < 0.5 * m_nominalMs)
            {
                m_lastPresent = report.present;
                return;
            }
            else
            {
                const double steps = std::max(1.0, std::round(gap / m_nominalMs));
                m_offs.push_back(std::fabs(gap - steps * m_nominalMs) / m_nominalMs);
                while (m_offs.size() > kOffsKept)
                    m_offs.pop_front();
            }
        }
        m_have = true;
        m_lastPresent = report.present;
        m_lastOutMs = report.outMs;
        const bool aimed = report.meantMs > 0.0;
        if (aimed)
        {
            m_aimed.push_back(report.outMs - report.meantMs);
            m_missed.push_back(m_haveLead && !madeItsRefresh(report.meantMs, report.outMs));
            while (m_aimed.size() > kAimedKept)
            {
                m_aimed.pop_front();
                m_missed.pop_front();
            }
        }
        else
        {
            m_latencies.push_back(report.outMs - report.calledMs);
            while (m_latencies.size() > kLatenciesKept)
                m_latencies.pop_front();
        }

        // On a grid, the gaps are whole refreshes (measured on a 240 Hz display: within 0.02 of one); pictures that
        // are shown when handed over leave gaps of their own. In between, the kind stays what it was.
        Kind kind = m_kind;
        if (m_offs.size() >= kEnoughReports)
        {
            const double middle = partOf(m_offs, 0.5);
            if (middle <= 0.06)
                kind = Kind::Fixed;
            else if (middle >= 0.12)
                kind = Kind::Follows;
        }
        if (kind == Kind::Fixed && m_kind == Kind::Follows)
            m_line = Estimator(m_nominalMs); // the reports taken while it followed are not on this grid
        if (kind != Kind::Follows)
            m_line.report(report.outMs);
        m_kind = kind;
        const double period = m_kind == Kind::Fixed ? m_line.grid().periodMs : m_nominalMs;
        if (aimed)
        {
            // Aimed pictures leave the lead alone unless most of them missed their refresh.
            if (m_haveLead && m_missed.size() >= kAimedKept && aimedMissed() * 2 >= m_missed.size())
            {
                m_leadMs = partOf(m_aimed, 0.5);
                m_latencies.clear(); // taken when the window was shown the other way
                std::fill(m_missed.begin(), m_missed.end(), false);
            }
        }
        else if (m_latencies.size() >= kEnoughReports)
        {
            m_leadMs = partOf(m_latencies, 0.1) - 0.1 * period;
            m_haveLead = true;
        }
    }

    bool RealDisplay::madeItsRefresh(double meantMs, double outMs) const
    {
        return std::fabs(outMs - (meantMs + m_leadMs)) <= 0.5 * m_nominalMs;
    }

    size_t RealDisplay::aimedMissed() const
    {
        return static_cast<size_t>(std::count(m_missed.begin(), m_missed.end(), true));
    }

    bool RealDisplay::grid(Grid &out) const
    {
        if (m_kind == Kind::Follows)
        {
            Grid grid;
            grid.follows = true;
            grid.periodMs = m_nominalMs;
            out = grid;
            return true;
        }
        if (m_kind != Kind::Fixed || !m_line.settled() || !m_haveLead)
            return false;
        Grid grid = m_line.grid();
        // The line must be the driver's own grid: its refresh time, and the reports close to it.
        if (std::fabs(grid.periodMs - m_nominalMs) > 0.005 * m_nominalMs || m_line.spreadMs() > 0.1 * m_nominalMs)
            return false;
        grid.originMs -= m_leadMs;
        out = grid;
        return true;
    }

    namespace
    {
        std::mutex g_mutex;
        bool g_havePublished = false;
        Grid g_published;
        std::map<uint64_t, Timed> g_announced;
        double g_latestMs = 0.0;
        std::vector<Outcome> g_outcomes;
    }

    double toMs(std::chrono::steady_clock::time_point time)
    {
        return std::chrono::duration<double, std::milli>(time.time_since_epoch()).count();
    }

    std::chrono::steady_clock::time_point fromMs(double ms)
    {
        return std::chrono::steady_clock::time_point(
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double, std::milli>(ms)));
    }

    double nowMs() { return toMs(std::chrono::steady_clock::now()); }

    void publish(const Grid &grid)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_published = grid;
        g_havePublished = true;
    }

    bool published(Grid &out)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_havePublished)
            out = g_published;
        return g_havePublished;
    }

    void withdraw()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_havePublished = false;
    }

    void announce(uint64_t tag, const Timed &timed)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        // A read that never arrives leaves its entry behind: old ones go when the table grows.
        if (g_announced.size() >= 256u)
        {
            for (auto at = g_announced.begin(); at != g_announced.end();)
                at = at->second.atMs < timed.atMs - 1000.0 ? g_announced.erase(at) : std::next(at);
        }
        g_announced[tag] = timed;
        if (timed.atMs > g_latestMs)
            g_latestMs = timed.atMs;
    }

    bool take(uint64_t tag, Timed &out)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto at = g_announced.find(tag);
        if (at == g_announced.end())
            return false;
        out = at->second;
        g_announced.erase(at);
        return true;
    }

    void voidFrom(double atMs)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto &entry : g_announced)
        {
            if (entry.second.atMs >= atMs)
                entry.second.isVoid = true;
        }
    }

    double latestAnnouncedMs()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_latestMs;
    }

    void forgetAnnounced()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_announced.clear();
        g_latestMs = 0.0;
    }

    void report(const Outcome &outcome)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_outcomes.size() < 4096u) // nobody takes them: stop collecting
            g_outcomes.push_back(outcome);
    }

    void takeOutcomes(std::vector<Outcome> &out)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        out.insert(out.end(), g_outcomes.begin(), g_outcomes.end());
        g_outcomes.clear();
    }

    bool logTimeline()
    {
        static const bool s_on = [] {
            const char *v = std::getenv("PS2X_SMOOTH_TIMELINE");
            return v && *v && *v != '0';
        }();
        return s_on;
    }

    Grid Estimator::grid() const
    {
        Grid grid;
        grid.periodMs = m_periodMs;
        // Refresh numbers of the grid are the estimator's own (0 = the first report), or the system's when it named them.
        grid.originMs = m_originMs - m_firstNumber * m_periodMs;
        return grid;
    }
}
