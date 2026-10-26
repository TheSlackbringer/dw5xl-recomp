// CTest log_lines_whole (issue 023 step 7T): log lines written by many threads must arrive whole. The game writes
// most of its log as std::cerr << ... << std::endl chains; std::cerr is unbuffered, so every << used to be its own
// write and another thread's line could land inside it ("[MPEG:CdStreamEof] produced=1343488[stats] t=19.00 ...").
// 8 threads each write 2,000 lines into stderr, as std::cerr chains mixed with whole fprintf(stderr) lines; stderr
// points at a file. Every line must arrive whole and complete, each exactly once.
#include "runtime/ps2_stdio_pump.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#define T_DUP _dup
#define T_DUP2 _dup2
#define T_CLOSE _close
#define T_OPEN(path) _open(path, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, 0644)
#else
#include <fcntl.h>
#include <unistd.h>
#define T_DUP dup
#define T_DUP2 dup2
#define T_CLOSE close
#define T_OPEN(path) open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644)
#endif

namespace
{
    constexpr int kThreads = 8;
    constexpr int kLines = 2000;
    const char *kFile = "log_lines_whole.tmp";

    int g_failures = 0;
    void check(bool ok, const char *what)
    {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok)
            ++g_failures;
    }

    void writer(int t)
    {
        for (int i = 0; i < kLines; ++i)
        {
            if (i % 2 == 0)
                std::cerr << "C " << t << ' ' << i << " part-a" << " part-b" << " part-c" << " end" << std::endl;
            else
                std::fprintf(stderr, "F %d %d whole fprintf line end\n", t, i);
        }
    }
}

int main()
{
    std::fflush(stderr);
    const int savedStderr = T_DUP(2);
    const int file = T_OPEN(kFile);
    if (savedStderr < 0 || file < 0 || T_DUP2(file, 2) < 0)
    {
        std::printf("FAIL cannot point stderr at %s\n", kFile);
        return 1;
    }
    T_CLOSE(file);

    const bool installed = ps2x::stdio_pump::installLineStreams();

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back(writer, t);
    for (auto &thread : threads)
        thread.join();
    ps2x::stdio_pump::drain();
    std::cerr.flush();
    std::fflush(stderr);
    T_DUP2(savedStderr, 2);
    T_CLOSE(savedStderr);

    check(installed, "line streams installed for std::cout, std::cerr and std::clog");

    std::FILE *in = std::fopen(kFile, "rb");
    if (!in)
    {
        std::printf("FAIL cannot read %s back\n", kFile);
        return 1;
    }
    std::string text;
    char buffer[65536];
    for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), in)) > 0;)
        text.append(buffer, n);
    std::fclose(in);
    std::remove(kFile);

    std::set<std::pair<int, int>> seen;
    size_t lines = 0, torn = 0, duplicates = 0;
    std::string firstTorn;
    for (size_t at = 0; at < text.size();)
    {
        const size_t end = text.find('\n', at);
        const std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
        at = end == std::string::npos ? text.size() : end + 1;
        ++lines;
        int t = -1, i = -1, used = 0;
        char kind = 0;
        bool whole = std::sscanf(line.c_str(), "%c %d %d%n", &kind, &t, &i, &used) == 3 && t >= 0 && t < kThreads &&
                     i >= 0 && i < kLines;
        if (whole)
        {
            const std::string rest = line.substr(static_cast<size_t>(used));
            whole = (kind == 'C' && i % 2 == 0 && rest == " part-a part-b part-c end") ||
                    (kind == 'F' && i % 2 == 1 && rest == " whole fprintf line end");
        }
        if (!whole)
        {
            ++torn;
            if (firstTorn.empty())
                firstTorn = line.substr(0, 120);
            continue;
        }
        if (!seen.emplace(t, i).second)
            ++duplicates;
    }
    std::printf("      %zu lines read, %zu torn, %zu duplicates, %zu of %d distinct lines found\n", lines, torn,
                duplicates, seen.size(), kThreads * kLines);
    if (!firstTorn.empty())
        std::printf("      first torn line: %s\n", firstTorn.c_str());
    check(torn == 0, "every line arrives whole");
    check(duplicates == 0 && seen.size() == static_cast<size_t>(kThreads * kLines), "every line arrives exactly once");

    std::printf("%s\n", g_failures ? "log_lines_whole: FAIL" : "log_lines_whole: PASS");
    return g_failures ? 1 : 0;
}
