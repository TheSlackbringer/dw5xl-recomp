// CTest stdio_pump (issue 025 v10 Step 5): a slow log destination must not stall the writing thread.
// The destination is a small pipe that nobody reads for a while (like a lagging console or redirect). Writes through
// the pump must return at once; afterwards every byte arrives, in order; after drain() writes go straight through.
#include "runtime/ps2_stdio_pump.h"

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
    std::printf("%s\n", g_failures ? "stdio_pump: FAIL" : "stdio_pump: PASS");
    return g_failures ? 1 : 0;
}
