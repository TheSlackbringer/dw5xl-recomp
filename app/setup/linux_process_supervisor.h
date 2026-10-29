#pragma once

// A private reaper per command, never a process-wide reaper in the threaded UI.
// Nested commands may create new process groups or sessions. Once their parents
// exit, Linux reparents them here, so cancellation can kill and reap the entire
// owned tree without enumerating/signalling unrelated system processes.
// Everything after fork uses stack storage and async-signal-safe operations
// (plus the Linux prctl/close_range system calls); no allocator or C++ locks.
#include <cerrno>
#include <climits>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace dw5xl::setup::linux_process {
struct Message
{
    enum Kind { Started, Failed, Finished } kind;
    int code;
    int error;
};

inline bool sendMessage(int fd, Message message)
{
    ssize_t n;
    do { n = send(fd, &message, sizeof(message), MSG_NOSIGNAL); } while (n < 0 && errno == EINTR);
    return n == sizeof(message);
}

inline void childChanged(int) {}

inline int signalOwnedChild(pid_t pid)
{
    // All listed PIDs are our unreaped children; their IDs cannot be reused.
    // Only signal a group when its leader is itself one of those children.
    int error = 0;
    if (getpgid(pid) == pid && kill(-pid, SIGKILL) < 0 && errno != ESRCH)
        error = errno;
    if (kill(pid, SIGKILL) < 0 && errno != ESRCH && !error)
        error = errno;
    return error;
}

inline int stopChildren(int children)
{
    if (lseek(children, 0, SEEK_SET) < 0) return errno;
    char bytes[4096];
    unsigned long pid = 0;
    int error = 0;
    for (;;)
    {
        ssize_t n;
        do { n = read(children, bytes, sizeof(bytes)); } while (n < 0 && errno == EINTR);
        if (n < 0) return error ? error : errno;
        for (ssize_t i = 0; i < n; ++i)
        {
            const char c = bytes[i];
            if (c >= '0' && c <= '9')
            {
                if (pid > (ULONG_MAX - unsigned(c - '0')) / 10) return EOVERFLOW;
                pid = pid * 10 + unsigned(c - '0');
            }
            else if (pid)
            {
                if (pid > INT_MAX) return EOVERFLOW;
                const int e = signalOwnedChild(pid_t(pid));
                if (!error) error = e;
                pid = 0;
            }
        }
        if (!n) break;
    }
    if (pid)
    {
        if (pid > INT_MAX) return EOVERFLOW;
        const int e = signalOwnedChild(pid_t(pid));
        if (!error) error = e;
    }
    return error;
}

[[noreturn]] inline void supervise(int channel, int log, int input, int maxFd, const char *cwd,
                                 char *const *argv, char *const *environment,
                                 const char *const *programs, size_t programCount)
{
    // Sources are >= 5, so these moves cannot overwrite one another. Drop all
    // other inherited descriptors, including another concurrent launch's IPC.
    if (dup2(channel, 3) < 0 || dup2(log, 4) < 0 || dup2(4, 1) < 0 || dup2(4, 2) < 0 ||
        (input >= 0 && dup2(input, 0) < 0))
        _exit(125);
    if (input < 0) close(0);
#ifdef SYS_close_range
    if (syscall(SYS_close_range, 5u, ~0u, 0u) < 0)
#endif
        for (int fd = 5; fd < maxFd; ++fd) close(fd);
    if (fcntl(3, F_SETFD, FD_CLOEXEC) < 0 || fcntl(4, F_SETFD, FD_CLOEXEC) < 0 ||
        prctl(PR_SET_CHILD_SUBREAPER, 1L) < 0)
    {
        sendMessage(3, {Message::Failed, 0, errno});
        _exit(125);
    }
    const int children = open("/proc/thread-self/children", O_RDONLY | O_CLOEXEC);
    if (children < 0)
    {
        sendMessage(3, {Message::Failed, 0, errno});
        _exit(125);
    }
    sigset_t blocked, original, waiting;
    sigemptyset(&blocked); sigaddset(&blocked, SIGCHLD);
    struct sigaction action{};
    action.sa_handler = childChanged;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGCHLD, &action, nullptr) < 0 || sigprocmask(SIG_BLOCK, &blocked, &original) < 0)
    {
        sendMessage(3, {Message::Failed, 0, errno});
        _exit(125);
    }
    waiting = original; sigdelset(&waiting, SIGCHLD);
    int execution[2];
    if (pipe2(execution, O_CLOEXEC) < 0)
    {
        sendMessage(3, {Message::Failed, 0, errno});
        _exit(125);
    }
    // Do not invoke inherited pthread_atfork handlers from this already-forked
    // supervisor. The exec child only performs the syscall-safe setup below.
    const pid_t child = pid_t(syscall(SYS_fork));
    if (child == 0)
    {
        close(execution[0]);
        int error = 0;
        action.sa_handler = SIG_DFL;
        if (setpgid(0, 0) < 0 || sigaction(SIGCHLD, &action, nullptr) < 0 ||
            sigprocmask(SIG_SETMASK, &original, nullptr) < 0 || (cwd && chdir(cwd) < 0))
            error = errno;
        else
        {
            bool denied = false;
            for (size_t i = 0; i < programCount; ++i)
            {
                execve(programs[i], argv, environment);
                error = errno;
                if (error == EACCES) denied = true;
                else if (error != ENOENT && error != ENOTDIR) break;
            }
            if (denied && (error == ENOENT || error == ENOTDIR || error == EACCES)) error = EACCES;
        }
        ssize_t sent;
        do { sent = write(execution[1], &error, sizeof(error)); } while (sent < 0 && errno == EINTR);
        _exit(127);
    }
    close(execution[1]);
    if (child < 0)
    {
        sendMessage(3, {Message::Failed, 0, errno});
        _exit(125);
    }
    int executionError = 0;
    ssize_t n;
    do { n = read(execution[0], &executionError, sizeof(executionError)); } while (n < 0 && errno == EINTR);
    close(execution[0]);
    bool cleanup = n != 0;
    int error = n < 0 ? errno : executionError;
    if (!cleanup && !sendMessage(3, {Message::Started, 0, 0})) cleanup = true;
    int status = 0;
    bool reaped = false;
    while (!cleanup)
    {
        const pid_t done = waitpid(child, &status, WNOHANG);
        if (done == child) { reaped = true; break; }
        if (done < 0 && errno != EINTR) { error = errno; break; }
        struct pollfd event{3, POLLIN, 0};
        // SIGCHLD stays blocked between the status check and this atomic wait.
        // No grace period, busy loop or missed-exit wake-up is needed.
        const int ready = ppoll(&event, 1, nullptr, &waiting);
        if (ready < 0 && errno != EINTR) { error = errno; break; }
        if (ready > 0) cleanup = true; // cancel packet, or owning parent exited
    }
    for (;;)
    {
        const int e = stopChildren(children);
        if (e) { if (!error) error = e; break; }
        int nextStatus = 0;
        const pid_t done = waitpid(-1, &nextStatus, 0);
        if (done == child) { status = nextStatus; reaped = true; }
        if (done > 0) continue; // newly orphaned descendants are now ours
        if (done < 0 && errno == EINTR) continue;
        if (done < 0 && errno != ECHILD && !error) error = errno;
        break;
    }
    const int code = reaped ? (WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status)) : -1;
    sendMessage(3, {n != 0 ? Message::Failed : Message::Finished, code, error});
    _exit(0);
}
} // namespace dw5xl::setup::linux_process
