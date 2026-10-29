#include "run_process.h"
#include <array>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <thread>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace dw5xl::setup {

#ifdef _WIN32
namespace {
std::wstring widen(const std::string &utf8)
{
    if (utf8.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), out.data(), n);
    return out;
}

// Quotes one argument the way CommandLineToArgvW / the MSVC and UCRT runtimes parse it.
void appendQuoted(std::wstring &cmd, const std::wstring &arg)
{
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
    {
        cmd += arg;
        return;
    }
    cmd += L'"';
    for (auto it = arg.begin();; ++it)
    {
        size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\')
        {
            ++it;
            ++backslashes;
        }
        if (it == arg.end())
        {
            cmd.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"')
        {
            cmd.append(backslashes * 2 + 1, L'\\');
            cmd += L'"';
        }
        else
        {
            cmd.append(backslashes, L'\\');
            cmd += *it;
        }
    }
    cmd += L'"';
}
} // namespace

ProcessResult runProcess(const std::filesystem::path &program, const std::vector<std::string> &args,
                         const std::filesystem::path &workingDirectory, const std::filesystem::path &logFile,
                         const std::atomic<bool> *cancel)
{
    ProcessResult result;
    std::wstring cmd;
    appendQuoted(cmd, program.wstring());
    for (const auto &arg : args)
    {
        cmd += L' ';
        appendQuoted(cmd, widen(arg));
    }
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE log = CreateFileW(logFile.wstring().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &inherit, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE)
    {
        result.error = "cannot open the log " + logFile.string();
        return result;
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = log;
    si.hStdError = log;
    PROCESS_INFORMATION pi{};
    const std::wstring dir = workingDirectory.wstring();
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                        dir.empty() ? nullptr : dir.c_str(), &si, &pi))
    {
        result.error = "cannot start " + program.string() + " (error " + std::to_string(GetLastError()) + ")";
        CloseHandle(log);
        return result;
    }
    // A job holds the program and everything it starts, so a cancel stops the whole tree.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    bool contained = false;
    if (job)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        contained = SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) &&
                    AssignProcessToJobObject(job, pi.hProcess);
    }
    if (!contained)
    {
        TerminateProcess(pi.hProcess, 1);
        if (job) CloseHandle(job);
        CloseHandle(pi.hThread);CloseHandle(pi.hProcess);CloseHandle(log);
        result.error = "cannot safely contain the child process tree";
        return result;
    }
    ResumeThread(pi.hThread);
    result.started = true;
    while (WaitForSingleObject(pi.hProcess, 100) == WAIT_TIMEOUT)
    {
        if (cancel && *cancel)
        {
            if (job)
                TerminateJobObject(job, 1);
            else
                TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, INFINITE);
            result.cancelled = true;
            break;
        }
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    result.exitCode = int(code);
    if (job)
        CloseHandle(job);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(log);
    return result;
}

bool launchDetached(const std::filesystem::path &program, const std::vector<std::string> &args,
                    const std::filesystem::path &workingDirectory, std::string &error)
{
    std::wstring cmd;
    appendQuoted(cmd, program.wstring());
    for (const auto &arg : args)
    {
        cmd += L' ';
        appendQuoted(cmd, widen(arg));
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const std::wstring dir = workingDirectory.wstring();
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
                        nullptr, dir.empty() ? nullptr : dir.c_str(), &si, &pi))
    {
        error = "cannot start " + program.string() + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

#else

ProcessResult runProcess(const std::filesystem::path &program, const std::vector<std::string> &args,
                         const std::filesystem::path &workingDirectory, const std::filesystem::path &logFile,
                         const std::atomic<bool> *cancel)
{
    ProcessResult result;
    std::vector<std::string> owned{program.string()};
    owned.insert(owned.end(), args.begin(), args.end());
    std::vector<char *> argv;
    for (auto &arg : owned)
        argv.push_back(arg.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, logFile.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    if (!workingDirectory.empty())
        posix_spawn_file_actions_addchdir_np(&actions, workingDirectory.c_str());
    // Its own process group, so a cancel reaches everything it starts.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);
    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, owned[0].c_str(), &actions, &attr, argv.data(), environ); // searches PATH for bare names
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    if (rc != 0)
    {
        result.error = "cannot start " + program.string() + " (error " + std::to_string(rc) + ")";
        return result;
    }
    result.started = true;
    int status = 0;
    for (;;)
    {
        const pid_t done = waitpid(pid, &status, cancel ? WNOHANG : 0);
        if (done == pid)
            break;
        if (done < 0 && errno != EINTR)
        {
            result.error = "lost track of " + program.string();
            return result;
        }
        if (cancel && *cancel)
        {
            result.cancelled = true;
            kill(-pid, SIGKILL); // explicit cancellation of build tools; no delayed, machine-tuned grace period
        }
        if (cancel)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    return result;
}

bool launchDetached(const std::filesystem::path &program, const std::vector<std::string> &args,
                    const std::filesystem::path &workingDirectory, std::string &error)
{
    std::vector<std::string> owned{program.string()};
    owned.insert(owned.end(), args.begin(), args.end());
    std::vector<char *> argv;
    for (auto &arg : owned)
        argv.push_back(arg.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (!workingDirectory.empty())
        posix_spawn_file_actions_addchdir_np(&actions, workingDirectory.c_str());
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID); // own session: survives the wizard
    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, owned[0].c_str(), &actions, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    if (rc != 0)
    {
        error = "cannot start " + program.string() + " (error " + std::to_string(rc) + ")";
        return false;
    }
    return true; // not waited for; the wizard's exit reaps nothing it needs
}

#endif

ProcessResult runProcessObserved(const std::filesystem::path &program,const std::vector<std::string> &args,
                                  const std::filesystem::path &workingDirectory,const std::filesystem::path &logFile,
                                  const std::function<bool(const std::string &)> &line,const std::atomic<bool> *cancel)
{
    ProcessResult result;
    if(cancel && *cancel) {result.cancelled=true;return result;}
    std::error_code ec;
    uintmax_t offset=std::filesystem::file_size(logFile,ec);
    if(ec) offset=0;
    std::atomic<bool> stop{false};
    std::mutex mutex;std::condition_variable changed;bool finished=false;
    std::thread worker([&] {
        try {result=runProcess(program,args,workingDirectory,logFile,&stop);}
        catch(const std::exception &e) {result.error=e.what();}
        {std::lock_guard lock(mutex);finished=true;}
        changed.notify_one();
    });
    std::ifstream input;
    std::string pending;
    auto consume=[&] {
        const auto size=std::filesystem::file_size(logFile,ec);
        if(ec) return;
        if(size<offset) {offset=0;pending.clear();}
        if(!input.is_open()) input.open(logFile,std::ios::binary);
        if(!input.is_open()) return;
        input.clear();input.seekg(std::streamoff(offset));
        std::array<char,65536> block;
        for(int batch=0;batch<4;++batch)
        {
            input.read(block.data(),block.size());const auto got=input.gcount();
            if(!got) break;
            offset+=uintmax_t(got);pending.append(block.data(),size_t(got));
            size_t at=0;
            while((at=pending.find('\n'))!=std::string::npos)
            {
                std::string complete=pending.substr(0,at);pending.erase(0,at+1);
                if(!complete.empty() && complete.back()=='\r') complete.pop_back();
                if(line && !line(complete)) stop=true;
            }
            if(pending.size()>131072) pending.erase(0,pending.size()-131072);
        }
    };
    try
    {
        for(;;)
        {
            if(cancel && *cancel) stop=true;
            consume();
            std::unique_lock lock(mutex);
            if(finished) break;
            changed.wait_for(lock,std::chrono::milliseconds(100),[&] {return finished;});
        }
        worker.join();
        // Remaining complete lines and a final unbuffered partial line are bounded as above.
        consume();
        if(!pending.empty() && line && !line(pending)) stop=true;
    }
    catch(...)
    {
        stop=true;
        if(worker.joinable()) worker.join();
        throw;
    }
    if(stop) result.cancelled=true;
    return result;
}

} // namespace dw5xl::setup
