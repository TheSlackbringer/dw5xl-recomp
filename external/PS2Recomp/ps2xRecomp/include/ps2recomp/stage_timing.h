#pragma once
// Bounded build-stage evidence (DW5XL issue 042), independent of runtime/game diagnostics.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <ostream>
#include <string>

namespace ps2recomp::timing {
inline std::string quote(const std::string &text)
{
    std::string out = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : text)
    {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if(c=='\n') out+="\\n";
        else if(c=='\r') out+="\\r";
        else if(c=='\t') out+="\\t";
        else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += char(c);
    }
    return out + '"';
}

inline std::string newRunId()
{
    static std::atomic<uint64_t> sequence{0};
    return std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count()) + "-" + std::to_string(++sequence);
}

class Session
{
public:
    using Clock = std::function<int64_t()>;
    Session(std::ostream &out, std::string run, std::string producer, std::string profile = {},
            Clock clock = {}, std::string prefix = {})
        : m_out(out), m_run(std::move(run)), m_producer(std::move(producer)),
          m_profile(std::move(profile)), m_clock(std::move(clock)), m_prefix(std::move(prefix))
    {
        if (!m_clock)
        {
            const auto origin = std::chrono::steady_clock::now();
            m_clock = [origin] { return std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - origin).count(); };
        }
    }
    int64_t now() const { return m_clock(); }
    const std::string &run() const { return m_run; }
    bool healthy() const { return m_healthy.load(); }
    uint64_t nextId() { return ++m_next; }
    void record(uint64_t id, uint64_t parent, const std::string &stage, const std::string &profile,
                const char *event, int64_t at, int64_t duration, const std::string &result,
                const std::map<std::string, uint64_t> &counts,
                const std::map<std::string,std::string> &fields = {}) noexcept
    {
        try
        {
            std::lock_guard lock(m_mutex);
            m_out << m_prefix << "{\"schema\":1,\"clock\":\"process-steady\",\"run\":" << quote(m_run)
                  << ",\"producer\":" << quote(m_producer) << ",\"profile\":" << quote(profile.empty() ? m_profile : profile)
                  << ",\"id\":" << id << ",\"parent\":" << parent << ",\"stage\":" << quote(stage)
                  << ",\"event\":" << quote(event) << ",\"at_us\":" << at;
            if (std::string(event) == "end")
                m_out << ",\"duration_us\":" << duration << ",\"result\":" << quote(result);
            if (std::string(event) == "end" || !counts.empty())
            {
                m_out << ",\"counts\":{";
                bool first = true;
                for (const auto &[key, value] : counts)
                {
                    if (!first) m_out << ',';
                    m_out << quote(key) << ':' << value;
                    first = false;
                }
                m_out << '}';
            }
            if(!fields.empty())
            {
                m_out<<",\"fields\":{";bool first=true;
                for(const auto &[name,value]:fields)
                {if(!first) m_out<<',';first=false;m_out<<quote(name)<<':'<<quote(value);}
                m_out<<'}';
            }
            m_out << "}\n";
            m_out.flush(); // begin evidence survives interruption; only a bounded number of stage records
            if (!m_out) m_healthy = false;
        }
        catch (...) { m_healthy = false; }
    }
private:
    std::ostream &m_out;
    std::string m_run, m_producer, m_profile;
    Clock m_clock;
    std::string m_prefix;
    std::mutex m_mutex;
    std::atomic<uint64_t> m_next{0};
    std::atomic<bool> m_healthy{true};
};

class Stage
{
public:
    Stage(Session *session, std::string name, uint64_t parent = 0, std::string profile = {}, int64_t start = -1)
        : m_session(session), m_name(std::move(name)), m_profile(std::move(profile)), m_parent(parent)
    {
        if (m_session)
        {
            m_id = m_session->nextId();
            m_start = start >= 0 ? start : m_session->now();
            m_session->record(m_id, m_parent, m_name, m_profile, "begin", m_start, 0, {}, {});
        }
    }
    Stage(const Stage &) = delete;
    Stage &operator=(const Stage &) = delete;
    ~Stage() { finish("incomplete"); }
    uint64_t id() const { return m_id; }
    void count(const std::string &name, uint64_t value) { if (m_session) m_counts[name] = value; }
    void field(const std::string &name,const std::string &value) {if(m_session) m_fields[name]=value;}
    void progress(uint64_t done, uint64_t total)
    {
        if (!m_session || (done != total && done != 0 && done - m_lastProgress < 256)) return;
        m_lastProgress = done;
        m_session->record(m_id, m_parent, m_name, m_profile, "progress", m_session->now(), 0, {},
                           {{"done",done},{"total",total}});
    }
    void finish(const std::string &result = "success") noexcept
    {
        if (!m_session || m_finished) return;
        m_finished = true;
        const int64_t end = m_session->now();
        m_session->record(m_id, m_parent, m_name, m_profile, "end", end, end - m_start, result, m_counts,m_fields);
    }
private:
    Session *m_session;
    std::string m_name, m_profile;
    uint64_t m_parent = 0, m_id = 0;
    uint64_t m_lastProgress = 0;
    int64_t m_start = 0;
    bool m_finished = false;
    std::map<std::string, uint64_t> m_counts;
    std::map<std::string,std::string> m_fields;
};
} // namespace ps2recomp::timing
