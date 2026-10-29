#include "runtime/ps2_present_feedback.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#if defined(__linux__) && !defined(__ANDROID__) && __has_include(<xcb/present.h>)
#define PS2X_PRESENT_FEEDBACK_X11 1
#include <dlfcn.h>
#include <xcb/present.h>

struct GLFWwindow;
// GLFW's X11 window of a GLFW window (in raylib's GLFW); weak, so a GLFW without X11 still links.
extern "C" __attribute__((weak)) unsigned long glfwGetX11Window(GLFWwindow *window);
#endif

namespace ps2x::present_feedback
{
    namespace
    {
        uint64_t s_swaps = 0;

        long long nowMicros()
        {
            return std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now().time_since_epoch()).count();
        }

#if defined(PS2X_PRESENT_FEEDBACK_X11)
        // The few libxcb / libxcb-present calls the listener needs, looked up at run time.
        struct Xcb
        {
            xcb_connection_t *(*connect)(const char *, int *) = nullptr;
            int (*connectionHasError)(xcb_connection_t *) = nullptr;
            uint32_t (*generateId)(xcb_connection_t *) = nullptr;
            const xcb_query_extension_reply_t *(*getExtensionData)(xcb_connection_t *, xcb_extension_t *) = nullptr;
            xcb_special_event_t *(*registerForSpecialXge)(xcb_connection_t *, xcb_extension_t *, uint32_t,
                                                          uint32_t *) = nullptr;
            xcb_generic_event_t *(*waitForSpecialEvent)(xcb_connection_t *, xcb_special_event_t *) = nullptr;
            xcb_generic_error_t *(*requestCheck)(xcb_connection_t *, xcb_void_cookie_t) = nullptr;
            xcb_present_query_version_cookie_t (*queryVersion)(xcb_connection_t *, uint32_t, uint32_t) = nullptr;
            xcb_present_query_version_reply_t *(*queryVersionReply)(xcb_connection_t *, xcb_present_query_version_cookie_t,
                                                                    xcb_generic_error_t **) = nullptr;
            xcb_void_cookie_t (*selectInputChecked)(xcb_connection_t *, xcb_present_event_t, xcb_window_t,
                                                    uint32_t) = nullptr;
            xcb_extension_t *presentId = nullptr;
        };

        template <typename T>
        bool lookUp(void *library, const char *name, T &out)
        {
            out = reinterpret_cast<T>(dlsym(library, name));
            return out != nullptr;
        }

        bool loadXcb(Xcb &x)
        {
            void *xcb = dlopen("libxcb.so.1", RTLD_NOW | RTLD_LOCAL);
            void *present = dlopen("libxcb-present.so.0", RTLD_NOW | RTLD_LOCAL);
            if (!xcb || !present)
                return false;
            return lookUp(xcb, "xcb_connect", x.connect) && lookUp(xcb, "xcb_connection_has_error", x.connectionHasError) &&
                   lookUp(xcb, "xcb_generate_id", x.generateId) && lookUp(xcb, "xcb_get_extension_data", x.getExtensionData) &&
                   lookUp(xcb, "xcb_register_for_special_xge", x.registerForSpecialXge) &&
                   lookUp(xcb, "xcb_wait_for_special_event", x.waitForSpecialEvent) &&
                   lookUp(xcb, "xcb_request_check", x.requestCheck) &&
                   lookUp(present, "xcb_present_query_version", x.queryVersion) &&
                   lookUp(present, "xcb_present_query_version_reply", x.queryVersionReply) &&
                   lookUp(present, "xcb_present_select_input_checked", x.selectInputChecked) &&
                   lookUp(present, "xcb_present_id", x.presentId);
        }

        const char *modeName(uint8_t mode)
        {
            switch (mode)
            {
            case XCB_PRESENT_COMPLETE_MODE_COPY: return "copy";
            case XCB_PRESENT_COMPLETE_MODE_FLIP: return "flip";
            case XCB_PRESENT_COMPLETE_MODE_SKIP: return "skip";
            case XCB_PRESENT_COMPLETE_MODE_SUBOPTIMAL_COPY: return "suboptimal";
            default: return "?";
            }
        }

        // Every Present completion of the window, in order. The serial is the swap number (Mesa numbers a drawable's
        // swaps from 1), the msc the display refresh that showed it.
        void listen(Xcb x, xcb_connection_t *connection, xcb_special_event_t *events)
        {
            while (xcb_generic_event_t *event = x.waitForSpecialEvent(connection, events))
            {
                const auto *generic = reinterpret_cast<const xcb_present_generic_event_t *>(event);
                if (generic->evtype == XCB_PRESENT_COMPLETE_NOTIFY)
                {
                    const auto *complete = reinterpret_cast<const xcb_present_complete_notify_event_t *>(event);
                    if (complete->kind == XCB_PRESENT_COMPLETE_KIND_PIXMAP)
                        std::fprintf(stderr, "[present:done] n=%u msc=%llu ust=%llu mode=%s\n", complete->serial,
                                     static_cast<unsigned long long>(complete->msc),
                                     static_cast<unsigned long long>(complete->ust), modeName(complete->mode));
                }
                std::free(event);
            }
            std::fprintf(stderr, "[present:log] X connection closed\n");
        }

        // How many swaps the window's GL drawable has already had (GLX_OML_sync_control; Mesa counts swaps that do
        // not go through our window loop too), so [present:frame] carries the serial [present:done] reports. Waits
        // once for those swaps to complete, before the first window frame. -1 if unknown.
        long long swapsBefore()
        {
            void *gl = dlopen("libGLX.so.0", RTLD_LAZY | RTLD_NOLOAD);
            if (!gl)
                gl = dlopen("libGL.so.1", RTLD_LAZY | RTLD_NOLOAD);
            using GetProc = void (*(*)(const unsigned char *))();
            using WaitForSbc = int (*)(void *, unsigned long, int64_t, int64_t *, int64_t *, int64_t *);
            GetProc getProc = nullptr;
            void *(*currentDisplay)() = nullptr;
            unsigned long (*currentDrawable)() = nullptr;
            if (!gl || !lookUp(gl, "glXGetProcAddressARB", getProc) || !lookUp(gl, "glXGetCurrentDisplay", currentDisplay) ||
                !lookUp(gl, "glXGetCurrentDrawable", currentDrawable))
                return -1;
            const auto waitForSbc = reinterpret_cast<WaitForSbc>(getProc(reinterpret_cast<const unsigned char *>("glXWaitForSbcOML")));
            int64_t ust = 0, msc = 0, sbc = 0;
            if (!waitForSbc || !currentDisplay() || !currentDrawable() ||
                !waitForSbc(currentDisplay(), currentDrawable(), 0, &ust, &msc, &sbc))
                return -1;
            return sbc;
        }

        void startX11(void *glfwWindow)
        {
            const unsigned long window = (glfwGetX11Window && glfwWindow)
                                             ? glfwGetX11Window(static_cast<GLFWwindow *>(glfwWindow)) : 0ul;
            Xcb x;
            if (window == 0ul || !loadXcb(x))
            {
                std::fprintf(stderr, "[present:log] no X11 window or libxcb-present: display refreshes not reported\n");
                return;
            }
            xcb_connection_t *connection = x.connect(nullptr, nullptr);
            if (!connection || x.connectionHasError(connection))
            {
                std::fprintf(stderr, "[present:log] cannot open an X connection: display refreshes not reported\n");
                return;
            }
            const xcb_query_extension_reply_t *extension = x.getExtensionData(connection, x.presentId);
            if (!extension || !extension->present)
            {
                std::fprintf(stderr, "[present:log] the X server has no Present extension: display refreshes not reported\n");
                return;
            }
            std::free(x.queryVersionReply(connection, x.queryVersion(connection, 1, 2), nullptr));
            const uint32_t eventId = x.generateId(connection);
            xcb_special_event_t *events = x.registerForSpecialXge(connection, x.presentId, eventId, nullptr);
            xcb_generic_error_t *error = x.requestCheck(
                connection, x.selectInputChecked(connection, eventId, static_cast<xcb_window_t>(window),
                                                 XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY));
            if (!events || error)
            {
                std::free(error);
                std::fprintf(stderr, "[present:log] cannot select Present events: display refreshes not reported\n");
                return;
            }
            // Runs until the process ends (the connection closes with it).
            std::thread(listen, x, connection, events).detach();
            const long long before = swapsBefore();
            if (before >= 0)
                s_swaps = static_cast<uint64_t>(before);
            std::fprintf(stderr, "[present:log] X11 Present events of window 0x%lx; earlier swaps %lld%s\n", window,
                         before, before < 0 ? " (unknown: frame numbers start at 1, serials may differ)" : "");
        }
#endif
    }

    bool enabled()
    {
        static const bool on = [] { const char *v = std::getenv("PS2X_PRESENT_LOG"); return v && *v && *v != '0'; }();
        return on;
    }

    void start(void *glfwWindow)
    {
        if (!enabled())
            return;
#if defined(PS2X_PRESENT_FEEDBACK_X11)
        startX11(glfwWindow);
#else
        (void)glfwWindow;
        std::fprintf(stderr, "[present:log] display refreshes not reported on this platform\n");
#endif
    }

    void frameSwapped(uint64_t field, bool newPicture)
    {
        if (!enabled())
            return;
        std::fprintf(stderr, "[present:frame] n=%llu field=%llu new=%d us=%lld\n",
                     static_cast<unsigned long long>(++s_swaps), static_cast<unsigned long long>(field),
                     newPicture ? 1 : 0, nowMicros());
    }
}
