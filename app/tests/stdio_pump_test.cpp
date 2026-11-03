// CTest stdio_pump (issue 025 v10 Step 5): a slow log destination must not stall the writing thread.
// The destination is a small pipe that nobody reads for a while (like a lagging console or redirect). Writes through
// the pump must return at once; afterwards every byte arrives, in order; after drain() writes go straight through.
#include "runtime/ps2_stdio_pump.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#define T_PIPE(fds) _pipe(fds, 4096, _O_BINARY)
#define T_DUP _dup
#define T_READ _read
#define T_WRITE _write
#define T_CLOSE _close
#else
#include <fcntl.h>
#include <unistd.h>
#define T_PIPE(fds) pipe(fds)
#define T_DUP dup
#define T_READ read
#define T_WRITE write
#define T_CLOSE close
#endif

namespace
{
    int g_failures = 0;
    void check(bool ok, const char *what)
    {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok)
            ++g_failures;
    }

    std::string readExactly(int fd, size_t bytes)
    {
        std::string out;
        char buffer[4096];
        while (out.size() < bytes)
        {
            const auto n = T_READ(fd, buffer, static_cast<unsigned int>(sizeof(buffer)));
            if (n <= 0)
                break;
            out.append(buffer, static_cast<size_t>(n));
        }
        return out;
    }
}

int main()
{
    int dest[2];
    if (T_PIPE(dest) != 0)
    {
        std::printf("FAIL cannot create the destination pipe\n");
        return 1;
    }
    const int program = T_DUP(dest[1]); // the descriptor the "game" writes to
    T_CLOSE(dest[1]);
    check(ps2x::stdio_pump::installFd(program, 1u << 20), "pump installed in front of the descriptor");

    // 400 lines of 100 bytes = 40 KB, ten times what the unread destination pipe can hold.
    std::string expected;
    for (int i = 0; i < 400; ++i)
    {
        char line[101];
        std::snprintf(line, sizeof(line), "[line] %04d %s\n", i, std::string(86, 'a' + i % 26).c_str());
        expected += line;
    }
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t at = 0; at < expected.size(); at += 100)
        T_WRITE(program, expected.data() + at, 100);
    const double writeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("      400 writes took %.2f ms with the destination unread\n", writeMs);
    check(writeMs < 200.0, "writes return while the destination is not being read");

    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // the destination stays unread a while longer
    const std::string got = readExactly(dest[0], expected.size());
    check(got == expected, "every byte arrives, in order");

    // drain() while bytes are still queued: they arrive, and later writes go straight to the destination.
    T_WRITE(program, "queued\n", 7);
    std::string tail;
    std::thread reader([&] { tail = readExactly(dest[0], 7 + 7); });
    ps2x::stdio_pump::drain();
    T_WRITE(program, "direct\n", 7);
    reader.join();
    check(tail == "queued\ndirect\n", "drain() delivers what is queued, then writes go straight through");
    ps2x::stdio_pump::drain(); // a second drain does nothing

    T_CLOSE(program);
    T_CLOSE(dest[0]);

    // Issue 025 Step 7: two descriptors with the same destination (stdout and stderr into one log) share one pipe, so
    // writes alternating between them arrive in exactly the order they were written.
    {
        int log[2];
        if (T_PIPE(log) != 0)
        {
            std::printf("FAIL cannot create the shared destination pipe\n");
            return 1;
        }
        const int out = T_DUP(log[1]), err = T_DUP(log[1]);
        T_CLOSE(log[1]);
        check(ps2x::stdio_pump::installPair(out, err, 1u << 20), "pair installed (same destination)");
        std::string written;
        for (int i = 0; i < 1000; ++i)
        {
            char line[48];
            const int fd = (i % 2) ? err : out;
            const int n = std::snprintf(line, sizeof(line), "%s %04d ordered line\n", (i % 2) ? "err" : "out", i);
            T_WRITE(fd, line, n);
            written.append(line, static_cast<size_t>(n));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // the log is read only afterwards
        std::string got;
        std::thread reader([&] { got = readExactly(log[0], written.size()); });
        ps2x::stdio_pump::drain();
        reader.join();
        check(got == written, "same destination: writes to both descriptors keep their program order");
        T_CLOSE(out);
        T_CLOSE(err);
        T_CLOSE(log[0]);
    }

    // Different destinations stay separate: each descriptor's bytes reach its own destination, in order.
    {
        int a[2], b[2];
        if (T_PIPE(a) != 0 || T_PIPE(b) != 0)
        {
            std::printf("FAIL cannot create the two destination pipes\n");
            return 1;
        }
        const int out = T_DUP(a[1]), err = T_DUP(b[1]);
        T_CLOSE(a[1]);
        T_CLOSE(b[1]);
        check(ps2x::stdio_pump::installPair(out, err, 1u << 20), "pair installed (different destinations)");
        std::string toA, toB;
        for (int i = 0; i < 200; ++i)
        {
            char line[48];
            int n = std::snprintf(line, sizeof(line), "a %04d\n", i);
            T_WRITE(out, line, n);
            toA.append(line, static_cast<size_t>(n));
            n = std::snprintf(line, sizeof(line), "b %04d\n", i);
            T_WRITE(err, line, n);
            toB.append(line, static_cast<size_t>(n));
        }
        std::string gotA, gotB;
        std::thread readerA([&] { gotA = readExactly(a[0], toA.size()); });
        std::thread readerB([&] { gotB = readExactly(b[0], toB.size()); });
        ps2x::stdio_pump::drain();
        readerA.join();
        readerB.join();
        check(gotA == toA && gotB == toB, "different destinations: each stream reaches its own destination");
        T_CLOSE(out);
        T_CLOSE(err);
        T_CLOSE(a[0]);
        T_CLOSE(b[0]);
    }

    // Issue 053: a small pump pipe. A PC whose account is over the kernel's per-user pipe limit gives a new pipe only
    // 8 KB and refuses a bigger one, so the pump must hold the backlog itself. A 4 KB pump pipe in front of an unread
    // 4 KB destination: 40 KB of writes return at once, then every byte arrives in order. (Before 053 the writes
    // blocked; the case then reports FAIL after 2 s instead of hanging, and reads the destination to let them finish.)
    {
        int small[2];
        if (T_PIPE(small) != 0)
        {
            std::printf("FAIL cannot create the small destination pipe\n");
            return 1;
        }
#if defined(F_SETPIPE_SZ)
        fcntl(small[1], F_SETPIPE_SZ, 4096); // Windows: T_PIPE already makes it 4 KB
#endif
        const int writer = T_DUP(small[1]);
        T_CLOSE(small[1]);
        check(ps2x::stdio_pump::installFd(writer, 4096), "pump installed with a 4 KB pipe");
        std::atomic<bool> done{false};
        double ms = 0.0;
        std::thread writes([&] {
            const auto start = std::chrono::steady_clock::now();
            for (size_t at = 0; at < expected.size(); at += 100)
                T_WRITE(writer, expected.data() + at, 100);
            ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            done = true;
        });
        for (int waited = 0; waited < 2000 && !done; waited += 10)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const bool returned = done;
        std::printf("      small pipe: 400 writes %s (%.2f ms)\n", returned ? "returned" : "still blocked after 2 s", ms);
        check(returned && ms < 200.0, "small pipe: writes return while the destination is not being read");
        const std::string got = readExactly(small[0], expected.size());
        writes.join();
        check(got == expected, "small pipe: every byte arrives, in order");
        ps2x::stdio_pump::drain();
        T_CLOSE(writer);
        T_CLOSE(small[0]);
    }

    std::printf("%s\n", g_failures ? "stdio_pump: FAIL" : "stdio_pump: PASS");
    return g_failures ? 1 : 0;
}
