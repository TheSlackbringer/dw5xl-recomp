// GIF arbiter order (issue 025 spec v9, guard for C3b): the packets the arbiter hands to the GS — their order,
// path and bytes — must stay exactly as they are. Mixed PATH1/2/3 queues (incl. PATH2 DIRECTHL and PATH3 IMAGE, whose
// priority rules the arbiter's sort applies), single-packet drains, more packets than one outbox batch, and long
// packets followed by short ones (buffer reuse). The stream must be the same inline, with the GS worker, and with the
// worker inside a batch, and equal to the stream recorded from the arbiter on 2026-10-23 (DW5XL_PRINT_GOLDEN=1
// prints the current value). Authored packets only; no game data.
#include "runtime/gs/ps2_gif_arbiter.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

extern thread_local int g_gifArbiterCurrentPath;

namespace
{
    struct Seen
    {
        int path;
        std::vector<uint8_t> bytes;
        bool operator==(const Seen &) const = default;
    };

    // A packet of `qwords` quadwords: a GIFtag (FLG = 2 makes it an IMAGE packet for PATH3), then a fill pattern
    // tagged with `id` so every packet is distinct.
    std::vector<uint8_t> packet(uint32_t id, uint32_t qwords, bool image)
    {
        std::vector<uint8_t> p(qwords * 16u);
        const uint64_t tagLo = (qwords - 1u) | (1ull << 15) | ((image ? 2ull : 0ull) << 58);
        std::memcpy(p.data(), &tagLo, 8);
        std::memcpy(p.data() + 8, &id, 4);
        for (size_t i = 16; i < p.size(); ++i)
            p[i] = static_cast<uint8_t>(id * 31u + i);
        return p;
    }

    struct Item
    {
        GifPathId path;
        bool directHl, image;
        uint32_t qwords;
    };

    // Groups submitted before one drain; within a group the arbiter's priority rules may reorder.
    std::vector<std::vector<Item>> script()
    {
        using P = GifPathId;
        std::vector<std::vector<Item>> s;
        for (uint32_t i = 0; i < 40; ++i) // single-packet drains, like VU1 XGKICK packets (more than one batch)
            s.push_back({{P::Path1, false, false, 2u + (i % 5u)}});
        s.push_back({{P::Path3, false, false, 4}, {P::Path1, false, false, 2}, {P::Path2, false, false, 3}});
        s.push_back({{P::Path3, false, true, 6}, {P::Path2, true, false, 2}, {P::Path1, false, false, 2}});
        s.push_back({{P::Path2, true, false, 2}, {P::Path3, false, true, 5}, {P::Path2, false, false, 2},
                     {P::Path3, false, false, 3}, {P::Path1, false, false, 4}});
        s.push_back({{P::Path2, false, false, 2}, {P::Path2, false, false, 3}, {P::Path1, false, false, 2},
                     {P::Path1, false, false, 3}});
        for (uint32_t i = 0; i < 6; ++i) // long packets, then short ones: reused buffers must not leak bytes
            s.push_back({{P::Path1, false, false, i % 2u ? 2u : 512u}});
        s.push_back({});
        return s;
    }

    enum class Mode
    {
        Inline,
        Worker,
        WorkerBatch
    };

    std::vector<Seen> run(Mode mode)
    {
        std::vector<Seen> seen;
        GifArbiter arbiter([&](const uint8_t *data, uint32_t size)
                           { seen.push_back({g_gifArbiterCurrentPath, std::vector<uint8_t>(data, data + size)}); });
        if (mode != Mode::Inline)
            arbiter.setWorkerEnabled(true);
        if (mode == Mode::WorkerBatch)
            arbiter.beginBatch();
        uint32_t id = 1;
        for (const auto &group : script())
        {
            for (const auto &item : group)
            {
                const auto p = packet(id++, item.qwords, item.image);
                arbiter.submit(item.path, p.data(), static_cast<uint32_t>(p.size()), item.directHl);
            }
            arbiter.drain();
        }
        if (mode == Mode::WorkerBatch)
            arbiter.endBatch();
        arbiter.waitIdle();
        return seen;
    }

    // Issue 023: an ordered call (submitCall, the end-of-field scanout) runs after every packet submitted before it —
    // including packets not drained yet — and before every later one. Calls are recorded as path -1 with their number
    // and the number of packets submitted before them.
    std::vector<Seen> runWithCalls(Mode mode)
    {
        std::vector<Seen> seen;
        GifArbiter arbiter([&](const uint8_t *data, uint32_t size)
                           { seen.push_back({g_gifArbiterCurrentPath, std::vector<uint8_t>(data, data + size)}); });
        if (mode != Mode::Inline)
            arbiter.setWorkerEnabled(true);
        if (mode == Mode::WorkerBatch)
            arbiter.beginBatch();
        uint32_t id = 1;
        uint8_t call = 1;
        for (const auto &group : script())
        {
            for (const auto &item : group)
            {
                const auto p = packet(id++, item.qwords, item.image);
                arbiter.submit(item.path, p.data(), static_cast<uint32_t>(p.size()), item.directHl);
            }
            // Each call records its number and how many packets were submitted before it.
            auto submitCall = [&] { arbiter.submitCall([&seen, n = call++, before = static_cast<uint8_t>(id - 1u)]
                                                       { seen.push_back({-1, {n, before}}); }); };
            if (id % 3u == 0u) // sometimes before the group's drain, sometimes after it
                submitCall();
            arbiter.drain();
            if (id % 3u == 1u)
                submitCall();
        }
        if (mode == Mode::WorkerBatch)
            arbiter.endBatch();
        arbiter.waitIdle();
        return seen;
    }

    uint64_t fnv(const std::vector<Seen> &stream)
    {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](uint8_t b) { h = (h ^ b) * 1099511628211ull; };
        for (const auto &s : stream)
        {
            mix(static_cast<uint8_t>(s.path));
            for (uint8_t b : s.bytes)
                mix(b);
        }
        return h;
    }
}

int main()
{
    constexpr uint64_t kRecorded = 0x95f3fbbf9ddb66f4ull; // recorded from the arbiter on 2026-10-23
    constexpr size_t kRecordedPackets = 61;
    int failures = 0;
    const auto inlineStream = run(Mode::Inline);
    if (std::getenv("DW5XL_PRINT_GOLDEN"))
    {
        std::cout << "packets " << inlineStream.size() << " hash 0x" << std::hex << fnv(inlineStream) << std::dec << "\n";
        return 0;
    }
    const struct { Mode mode; const char *name; } modes[] = {
        {Mode::Inline, "inline"}, {Mode::Worker, "GS worker"}, {Mode::WorkerBatch, "GS worker in a batch"}};
    for (const auto &m : modes)
    {
        const auto stream = m.mode == Mode::Inline ? inlineStream : run(m.mode);
        try
        {
            if (m.mode != Mode::Inline && !(stream == inlineStream))
                throw std::runtime_error("stream differs from the inline stream");
            if (stream.size() != kRecordedPackets)
                throw std::runtime_error("packet count " + std::to_string(stream.size()));
            if (fnv(stream) != kRecorded)
                throw std::runtime_error("stream differs from the recorded one");
            std::cout << "PASS gif_arbiter_order " << m.name << "\n";
        }
        catch (const std::exception &e)
        {
            ++failures;
            std::cout << "FAIL gif_arbiter_order " << m.name << ": " << e.what() << "\n";
        }
    }
    // Ordered calls (issue 023): the packets around them keep the recorded stream, and each call sits exactly where
    // it was submitted, in every mode.
    std::vector<Seen> inlineCalls;
    for (const auto &m : modes)
    {
        const auto stream = runWithCalls(m.mode);
        try
        {
            std::vector<Seen> packets;
            size_t calls = 0;
            for (const auto &s : stream)
            {
                if (s.path == -1)
                {
                    if (s.bytes.size() != 2u || s.bytes[0] != calls + 1u)
                        throw std::runtime_error("call " + std::to_string(calls + 1u) + " out of order");
                    if (packets.size() != s.bytes[1])
                        throw std::runtime_error("call " + std::to_string(calls + 1u) + " ran after " +
                                                 std::to_string(packets.size()) + " packets, submitted after " +
                                                 std::to_string(s.bytes[1]));
                    ++calls;
                }
                else
                    packets.push_back(s);
            }
            if (calls == 0u)
                throw std::runtime_error("no call ran");
            if (!(packets == inlineStream))
                throw std::runtime_error("packets around the calls differ from the recorded stream");
            if (m.mode == Mode::Inline)
                inlineCalls = stream;
            else if (!(stream == inlineCalls))
                throw std::runtime_error("calls sit elsewhere than inline");
            std::cout << "PASS gif_arbiter_order calls " << m.name << " (" << calls << " calls)\n";
        }
        catch (const std::exception &e)
        {
            ++failures;
            std::cout << "FAIL gif_arbiter_order calls " << m.name << ": " << e.what() << "\n";
        }
    }
    return failures ? 1 : 0;
}
