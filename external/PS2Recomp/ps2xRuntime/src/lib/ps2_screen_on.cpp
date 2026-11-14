// DW5XL (issue 067): see runtime/ps2_screen_on.h. No window library in here: the system's own headers only.
#include "runtime/ps2_screen_on.h"

#include <cstdio>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ps2x::screen_on
{
    bool wanted(const State &state, Why &why)
    {
        if (state.exiting)
            why = Why::Exit;
        else if (state.hidden)
            why = Why::Hidden;
        else if (state.minimized)
            why = Why::Minimized;
        else if (state.paused)
            why = Why::Paused;
        else
            why = Why::Playing;
        return why == Why::Playing;
    }

    const char *said(Why why)
    {
        switch (why)
        {
        case Why::Playing:
            return "playing";
        case Why::Paused:
            return "paused";
        case Why::Hidden:
            return "hidden";
        case Why::Minimized:
            return "minimised";
        case Why::Exit:
            return "exit";
        }
        return "?";
    }

    namespace
    {
#ifdef _WIN32
        // A power request (Windows 7 and later). It is the program's, not a thread's, and the system shows its
        // reason text in its own list of requests. It ends with CloseHandle, and with the process whatever we do.
        class PowerRequest final : public System
        {
        public:
            explicit PowerRequest(const std::string &program)
            {
                const std::string reason = program + ": the game is being played";
                m_reason.assign(reason.begin(), reason.end()); // the title is plain ASCII
            }
            ~PowerRequest() override
            {
                letGo();
                if (m_request)
                    CloseHandle(m_request);
            }
            bool hold() override
            {
                // The system counts the requests of one kind: each one set must be cleared once. Held is held.
                if (m_held)
                    return true;
                if (!m_request)
                {
                    REASON_CONTEXT context{};
                    context.Version = POWER_REQUEST_CONTEXT_VERSION;
                    context.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
                    context.Reason.SimpleReasonString = m_reason.data();
                    const HANDLE request = PowerCreateRequest(&context);
                    if (!request || request == INVALID_HANDLE_VALUE)
                    {
                        std::snprintf(m_refused, sizeof(m_refused), "the system made no power request (error %lu)", GetLastError());
                        return false;
                    }
                    m_request = request;
                }
                if (!PowerSetRequest(m_request, PowerRequestDisplayRequired))
                {
                    std::snprintf(m_refused, sizeof(m_refused), "the system refused the request for the display (error %lu)",
                                  GetLastError());
                    return false;
                }
                // The PC does not go to sleep in a battle either. The display is what the rule is about: a system
                // that takes the first request and not this one still keeps the screen on.
                m_system = PowerSetRequest(m_request, PowerRequestSystemRequired) != 0;
                m_held = true;
                return true;
            }
            void letGo() override
            {
                if (!m_held)
                    return;
                PowerClearRequest(m_request, PowerRequestDisplayRequired);
                if (m_system)
                    PowerClearRequest(m_request, PowerRequestSystemRequired);
                m_held = false;
                m_system = false;
            }
            const char *refused() const override { return m_refused; }

        private:
            std::wstring m_reason;
            HANDLE m_request = nullptr;
            bool m_held = false;
            bool m_system = false;
            char m_refused[96] = "";
        };
#else
        // Issue 067's second step: the desktop's own request on Linux. Until it is built nothing is asked.
        class NoRequest final : public System
        {
        public:
            bool hold() override { return false; }
            void letGo() override {}
            const char *refused() const override { return "not built for this system yet (issue 067, step 2)"; }
        };
#endif
    } // namespace

    std::unique_ptr<System> makeSystem(const std::string &program)
    {
#ifdef _WIN32
        return std::make_unique<PowerRequest>(program);
#else
        (void)program;
        return std::make_unique<NoRequest>();
#endif
    }

    Keeper::Keeper(System &system, bool log) : m_system(system), m_log(log) {}

    Keeper::~Keeper()
    {
        if (m_held)
        {
            m_system.letGo();
            ++m_calls;
        }
    }

    void Keeper::update(const State &state)
    {
        Why why = Why::Exit;
        const bool want = wanted(state, why);
        if (m_known && want == m_wanted && why == m_why)
            return;
        const bool changed = !m_known || want != m_wanted;
        m_known = true;
        m_wanted = want;
        m_why = why;
        if (!changed)
            return; // another reason for the same answer (paused, then minimised): the request is as it should be
        if (want)
        {
            m_held = m_system.hold();
            ++m_calls;
            if (m_log)
            {
                if (m_held)
                    std::fprintf(stderr, "[display] the screen is kept on (%s)\n", said(why));
                else
                    std::fprintf(stderr, "[display] the screen is not kept on: %s\n", m_system.refused());
            }
            return;
        }
        if (m_held)
        {
            m_system.letGo();
            ++m_calls;
            m_held = false;
        }
        if (m_log)
            std::fprintf(stderr, "[display] the screen may switch off (%s)\n", said(why));
    }
} // namespace ps2x::screen_on
