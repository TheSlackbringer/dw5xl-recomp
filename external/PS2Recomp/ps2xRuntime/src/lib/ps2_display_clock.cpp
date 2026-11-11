// Issue 049 Part 2, amendment v3.5: the display's clock (see runtime/ps2_display_clock.h).
#include "runtime/ps2_display_clock.h"

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
