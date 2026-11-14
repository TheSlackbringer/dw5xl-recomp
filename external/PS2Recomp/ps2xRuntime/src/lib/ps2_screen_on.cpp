// DW5XL (issue 067): see runtime/ps2_screen_on.h. No window library in here: the system's own headers only.
#include "runtime/ps2_screen_on.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

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

    // ---- Part 2: what the system says of the screen.
    const char *said(Screen screen)
    {
        switch (screen)
        {
        case Screen::Off:
            return "off";
        case Screen::On:
            return "on";
        case Screen::Dim:
            return "dim";
        }
        return "?";
    }

    std::string SwitchLog::report(Screen screen, uint64_t field)
    {
        if (!m_known)
        {
            m_known = true;
            m_screen = screen;
            return std::string("[display] the screen at the start: ") + said(screen);
        }
        if (screen == m_screen)
            return std::string();
        m_screen = screen;
        ++m_switches;
        return std::string("[display] the system switched the screen ") + said(screen) + " (field " + std::to_string(field) + ")";
    }

    namespace
    {
#ifdef _WIN32
        // GUID_CONSOLE_DISPLAY_STATE: 0 the display is off, 1 on, 2 dimmed. Written out here so that no header
        // order or library decides whether the name has a value.
        const GUID kConsoleDisplayState = {0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};

        // Windows sends power-setting changes to a window. The game's own windows belong to the window library,
        // which does not hand such messages on; so the watch has a window of its own, never shown, with its own
        // message loop on its own thread. Windows sends the state once at registration, then every change.
        class DisplayWatch final : public Watch
        {
        public:
            DisplayWatch()
            {
                m_thread = std::thread([this] { run(); });
                std::unique_lock<std::mutex> lock(m_mutex);
                m_ready.wait(lock, [this] { return m_started; }); // the window is there (or could not be made)
            }
            ~DisplayWatch() override
            {
                if (const HWND window = m_window.load(std::memory_order_acquire))
                    PostMessageW(window, WM_CLOSE, 0, 0);
                if (m_thread.joinable())
                    m_thread.join();
            }
            void take(std::vector<Screen> &out) override
            {
                if (!m_pending.load(std::memory_order_acquire))
                    return;
                std::lock_guard<std::mutex> lock(m_mutex);
                out.insert(out.end(), m_reports.begin(), m_reports.end());
                m_reports.clear();
                m_pending.store(false, std::memory_order_release);
            }

        private:
            static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
            {
                auto *self = reinterpret_cast<DisplayWatch *>(GetWindowLongPtrW(window, GWLP_USERDATA));
                if (message == WM_POWERBROADCAST)
                {
                    if (wParam == PBT_POWERSETTINGCHANGE && lParam && self)
                    {
                        const auto *setting = reinterpret_cast<const POWERBROADCAST_SETTING *>(lParam);
                        if (std::memcmp(&setting->PowerSetting, &kConsoleDisplayState, sizeof(GUID)) == 0 &&
                            setting->DataLength >= sizeof(DWORD))
                        {
                            DWORD value = 1u;
                            std::memcpy(&value, setting->Data, sizeof(value));
                            self->add(value == 0u ? Screen::Off : value == 2u ? Screen::Dim : Screen::On);
                        }
                    }
                    return TRUE;
                }
                if (message == WM_CLOSE)
                {
                    if (self && self->m_notify)
                    {
                        UnregisterPowerSettingNotification(self->m_notify);
                        self->m_notify = nullptr;
                    }
                    DestroyWindow(window);
                    return 0;
                }
                if (message == WM_DESTROY)
                {
                    PostQuitMessage(0);
                    return 0;
                }
                return DefWindowProcW(window, message, wParam, lParam);
            }
            void add(Screen screen)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_reports.push_back(screen);
                m_pending.store(true, std::memory_order_release);
            }
            void run()
            {
                const HINSTANCE instance = GetModuleHandleW(nullptr);
                WNDCLASSEXW windowClass{};
                windowClass.cbSize = sizeof(windowClass);
                windowClass.lpfnWndProc = proc;
                windowClass.hInstance = instance;
                windowClass.lpszClassName = L"ps2x_screen_watch";
                RegisterClassExW(&windowClass); // a second watch in the process finds the class there
                const HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"", 0, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
                if (window)
                {
                    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
                    m_notify = RegisterPowerSettingNotification(window, &kConsoleDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);
                }
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_window.store(window, std::memory_order_release);
                    m_started = true;
                }
                m_ready.notify_all();
                if (!window)
                    return;
                MSG message;
                while (GetMessageW(&message, nullptr, 0, 0) > 0)
                    DispatchMessageW(&message);
            }

            std::thread m_thread;
            std::mutex m_mutex;
            std::condition_variable m_ready;
            bool m_started = false;
            std::atomic<HWND> m_window{nullptr};
            HPOWERNOTIFY m_notify = nullptr; // the watch's thread only
            std::vector<Screen> m_reports;
            std::atomic<bool> m_pending{false};
        };
#else
        // Issue 067's second step for Linux; nothing is reported until then.
        class NoWatch final : public Watch
        {
        public:
            void take(std::vector<Screen> &) override {}
        };
#endif
    } // namespace

    std::unique_ptr<Watch> makeWatch()
    {
#ifdef _WIN32
        return std::make_unique<DisplayWatch>();
#else
        return std::make_unique<NoWatch>();
#endif
    }

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
