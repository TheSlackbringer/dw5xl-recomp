#include "runtime/ps2_stdio_pump.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#define PUMP_DUP _dup
#define PUMP_DUP2 _dup2
#define PUMP_CLOSE _close
#define PUMP_READ _read
#define PUMP_WRITE _write
#else
#include <fcntl.h>
#include <unistd.h>
#define PUMP_DUP dup
#define PUMP_DUP2 dup2
#define PUMP_CLOSE close
#define PUMP_READ read
#define PUMP_WRITE write
#endif

namespace ps2x::stdio_pump
{
    namespace
    {
        struct Pump
        {
            int fd = -1;          // the descriptor the program writes to (now the pipe's write end)
            int destination = -1; // duplicate of its original destination
            int pipeRead = -1;
            int mode = 0;         // Windows: the descriptor's text/binary mode, kept across the swap
            std::thread thread;
        };

        std::mutex g_mutex;
        std::vector<Pump *> g_pumps;

        bool makePipe(int fds[2], size_t bytes)
        {
#if defined(_WIN32)
            return _pipe(fds, static_cast<unsigned int>(bytes), _O_BINARY | _O_NOINHERIT) == 0;
#else
            if (pipe(fds) != 0)
                return false;
            fcntl(fds[0], F_SETFD, FD_CLOEXEC);
            fcntl(fds[1], F_SETFD, FD_CLOEXEC);
#if defined(F_SETPIPE_SZ)
            fcntl(fds[1], F_SETPIPE_SZ, static_cast<int>(bytes)); // best effort (capped by the system's maximum)
#endif
            return true;
#endif
        }

        bool validDestination(int fd)
        {
#if defined(_WIN32)
            const intptr_t handle = _get_osfhandle(fd);
            return handle != -1 && handle != -2;
#else
            return fcntl(fd, F_GETFD) != -1;
#endif
        }

        void writeAll(int fd, const char *data, size_t size)
        {
            while (size > 0)
            {
                const auto n = PUMP_WRITE(fd, data, static_cast<unsigned int>(size));
                if (n <= 0)
                    return; // the destination is gone: drop, as a direct write would have failed
                data += n;
                size -= static_cast<size_t>(n);
            }
        }

        void copyLoop(int pipeRead, int destination)
        {
            char buffer[16384];
            for (;;)
            {
                const auto n = PUMP_READ(pipeRead, buffer, sizeof(buffer));
                if (n <= 0)
                    return; // every write end closed: drained
                writeAll(destination, buffer, static_cast<size_t>(n));
            }
        }

#if defined(_WIN32)
        DWORD stdHandleId(int fd)
        {
            return fd == 1 ? STD_OUTPUT_HANDLE : fd == 2 ? STD_ERROR_HANDLE : 0;
        }
#endif

        bool install(int fd, size_t pipeBytes)
        {
            if (!validDestination(fd))
                return false;
            for (const Pump *p : g_pumps)
                if (p->fd == fd)
                    return false;
            int fds[2];
            if (!makePipe(fds, pipeBytes))
                return false;
            int mode = 0;
#if defined(_WIN32)
            mode = _setmode(fd, _O_BINARY); // learn the mode (text for stdout/stderr) ...
            _setmode(fd, mode);             // ... and leave it as it was
#endif
            const int destination = PUMP_DUP(fd);
            // dup2 returns the new descriptor on POSIX and 0 on Windows: only a negative value is a failure.
            if (destination < 0 || PUMP_DUP2(fds[1], fd) < 0)
            {
                if (destination >= 0)
                    PUMP_CLOSE(destination);
                PUMP_CLOSE(fds[0]);
                PUMP_CLOSE(fds[1]);
                return false;
            }
            PUMP_CLOSE(fds[1]); // fd itself is now the only write end
#if defined(_WIN32)
            // The writers' bytes are translated once, on fd (its original mode); the copy passes them on unchanged.
            _setmode(fd, mode);
            _setmode(destination, _O_BINARY);
            if (const DWORD id = stdHandleId(fd))
                SetStdHandle(id, reinterpret_cast<HANDLE>(_get_osfhandle(fd))); // Win32-level writers too
#endif
            auto *pump = new Pump{fd, destination, fds[0], mode, {}};
            pump->thread = std::thread(copyLoop, fds[0], destination);
            g_pumps.push_back(pump);
            return true;
        }
    }

    bool installFd(int fd, size_t pipeBytes)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return install(fd, pipeBytes);
    }

    bool installStdio()
    {
        if (const char *direct = std::getenv("PS2X_STDIO_DIRECT"); direct && *direct == '1')
            return false;
        std::fflush(stdout);
        std::fflush(stderr);
        bool any = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            any = install(1, 1u << 20) | install(2, 1u << 20);
        }
        if (any)
            std::atexit(drain);
        return any;
    }

    void drain()
    {
        std::fflush(stdout);
        std::fflush(stderr);
        std::vector<Pump *> pumps;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            pumps.swap(g_pumps);
        }
        for (Pump *pump : pumps)
        {
            // Point the descriptor back at its destination: this closes the pipe's last write end, so the thread
            // copies what is queued, reads end-of-file and stops. Later writes go straight to the destination.
            PUMP_DUP2(pump->destination, pump->fd);
#if defined(_WIN32)
            _setmode(pump->fd, pump->mode);
            if (const DWORD id = stdHandleId(pump->fd))
                SetStdHandle(id, reinterpret_cast<HANDLE>(_get_osfhandle(pump->fd)));
#endif
            if (pump->thread.joinable())
                pump->thread.join();
            PUMP_CLOSE(pump->pipeRead);
            PUMP_CLOSE(pump->destination);
            delete pump;
        }
    }
}
