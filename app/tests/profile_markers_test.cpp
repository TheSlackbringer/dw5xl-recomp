// Issue 025: profiling markers with DW5XL_DEVELOPER_DIAGNOSTICS compiled in. A full buffer drops and counts
// records, recording allocates nothing, flush writes the header with two clock anchors, and records keep their
// per-thread order.
#ifndef DW5XL_DEVELOPER_DIAGNOSTICS
#define DW5XL_DEVELOPER_DIAGNOSTICS 1
#endif
#include "runtime/ps2_profile_markers.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static std::atomic<long> g_allocations{0};
void *operator new(std::size_t n)
{
    ++g_allocations;
    if (void *p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }

static void require(bool value, const char *what)
{
    if (!value)
        throw std::runtime_error(what);
}

int main()
{
    try
    {
        namespace fs = std::filesystem;
        const fs::path path = fs::temp_directory_path() / ("dw5xl_profile_markers_" + std::to_string(std::rand()) + ".bin");
        fs::remove(path);
        require(ps2prof::kCompiledIn, "compiled in");
        require(ps2prof::open(path.string().c_str(), 64), "open");
        require(ps2prof::enabled(), "enabled after open");

        const long before = g_allocations.load();
        for (uint64_t i = 0; i < 10; ++i)
            ps2prof::record(ps2prof::Event::User, i, 1, 0x00280010u, 100u + uint32_t(i));
        require(g_allocations.load() == before, "record allocates nothing");

        std::vector<std::thread> threads;
        for (int t = 0; t < 2; ++t)
            threads.emplace_back([t] {
                for (uint64_t i = 0; i < 20; ++i)
                    ps2prof::record(ps2prof::Event::JobEnd, 1000u * (t + 1) + i, uint16_t(t));
            });
        for (auto &t : threads)
            t.join();
        require(ps2prof::recorded() == 50 && ps2prof::dropped() == 0, "50 recorded, none dropped");
        for (uint64_t i = 0; i < 20; ++i)
            ps2prof::record(ps2prof::Event::User, 5000u + i);
        require(ps2prof::recorded() == 64 && ps2prof::dropped() == 6, "full buffer drops and counts");

        require(ps2prof::flush(), "flush");
        const auto size = fs::file_size(path);
        require(ps2prof::flush(), "second flush");
        require(fs::file_size(path) == size, "flush is idempotent");

        FILE *f = std::fopen(path.string().c_str(), "rb");
        require(f != nullptr, "file exists");
        ps2prof::FileHeader header{};
        require(std::fread(&header, sizeof(header), 1, f) == 1, "header");
        std::vector<ps2prof::Record> records(header.count);
        require(header.count == 0 || std::fread(records.data(), sizeof(ps2prof::Record), records.size(), f) == records.size(), "records");
        std::fclose(f);
        fs::remove(path);

        require(std::memcmp(header.magic, "DW5XLPM1", 8) == 0 && header.version == 1, "magic and version");
        require(header.recordSize == sizeof(ps2prof::Record), "record size");
        require(header.count == 64 && header.dropped == 6, "counts in header");
        require(header.atOpen.steadyNs && header.atOpen.osTicks && header.atOpen.osFreq, "anchor at open");
        require(header.atFlush.steadyNs >= header.atOpen.steadyNs && header.atFlush.osTicks >= header.atOpen.osTicks,
                "anchor at flush");
        require(records[0].event == uint16_t(ps2prof::Event::User) && records[0].seq == 0 && records[0].kind == 1 &&
                    records[0].program == 0x00280010u && records[0].vu1Cycles == 100,
                "fields kept");
        std::map<uint32_t, ps2prof::Record> last;
        std::map<uint16_t, uint64_t> nextSeq{{0, 1000}, {1, 2000}};
        for (const auto &r : records)
        {
            require(r.tid != 0, "thread id");
            auto it = last.find(r.tid);
            require(it == last.end() || it->second.ns <= r.ns, "per-thread order");
            last[r.tid] = r;
            if (r.event == uint16_t(ps2prof::Event::JobEnd))
                require(r.seq == nextSeq[r.kind]++, "per-thread sequence");
        }
        std::cout << "profile_markers: ok\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "profile_markers: FAIL " << e.what() << "\n";
        return 1;
    }
}
