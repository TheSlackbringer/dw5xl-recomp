// DW5XL-TEST: offline inspection and comparison of PS2X_GS_RECORD recordings
// (canonical format v2, see ps2xRuntime/src/lib/gs/gs_record_format.h).
//
//   dw5xl_gsrec stats <rec>
//   dw5xl_gsrec dump  <rec> [--from N] [--count N] [--fields]
//   dw5xl_gsrec diff  <a.rec> <b.rec> [--strict] [--ignore op,op] [--keep op,op]
//                     [--with-ticks] [--ignore-fpround] [--report N] [--context N] [--stop]
//
// diff compares the two command streams record by record after dropping
// host-timing-only records (default: Present, SnapshotVram, ReadVram, Flush,
// Sync(Presentation), Sync(DebugReadback); --strict keeps everything) and
// ignoring Present.vsyncTick (unless --with-ticks). Submit records carry the
// host FP rounding mode active at the call (PATH1/XGKICK packets are drawn
// under the VU's FE_TOWARDZERO): --ignore-fpround skips it. It prints the first
// divergence (stream index, record numbers, frame/tick, field-level details)
// and summary counts. Exit status: 0 identical, 1 different, 2 error.

#include "gs/gs_record_format.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace
{
    using namespace GSRecord;

    struct Rec
    {
        uint8_t op = 0;
        std::vector<uint8_t> payload;
        uint64_t index = 0; // record number in its file (0-based)
    };

    constexpr size_t kPresentTickOffset = 7u * 8u; // pmode..bgcolor precede vsyncTick

    class RecFile
    {
    public:
        explicit RecFile(const std::string &path) : m_path(path)
        {
            m_file = std::fopen(path.c_str(), "rb");
            if (!m_file)
                throw std::runtime_error("cannot open " + path);
            std::setvbuf(m_file, nullptr, _IOFBF, 1u << 22u);
            char magic[8]{};
            uint32_t header[2]{};
            if (std::fread(magic, 1u, 8u, m_file) != 8u || std::memcmp(magic, kMagic, 8u) != 0)
                throw std::runtime_error(path + ": not a GS recording in format v2 (old v1 recordings are not comparable)");
            if (std::fread(header, sizeof(header), 1u, m_file) != 1u || header[0] != kVersion)
                throw std::runtime_error(path + ": unsupported GS recording version");
        }
        ~RecFile()
        {
            if (m_file)
                std::fclose(m_file);
        }

        bool next(Rec &rec)
        {
            uint32_t size = 0;
            if (std::fread(&rec.op, 1u, 1u, m_file) != 1u)
                return false;
            if (std::fread(&size, sizeof(size), 1u, m_file) != 1u)
            {
                m_truncated = true;
                return false;
            }
            rec.payload.resize(size);
            if (size != 0u && std::fread(rec.payload.data(), 1u, size, m_file) != size)
            {
                m_truncated = true;
                return false;
            }
            rec.index = m_count++;
            if (rec.op == static_cast<uint8_t>(Op::Present) && rec.payload.size() >= kPresentTickOffset + 8u)
            {
                std::memcpy(&m_lastTick, rec.payload.data() + kPresentTickOffset, 8u);
                ++m_frames;
            }
            return true;
        }

        const std::string &path() const { return m_path; }
        bool truncated() const { return m_truncated; }
        uint64_t frames() const { return m_frames; }
        uint64_t lastTick() const { return m_lastTick; }
        uint64_t count() const { return m_count; }

    private:
        std::string m_path;
        std::FILE *m_file = nullptr;
        uint64_t m_count = 0;
        uint64_t m_frames = 0;
        uint64_t m_lastTick = 0;
        bool m_truncated = false;
    };

    // Stats key: op, with Sync split by reason.
    std::string statKey(const Rec &rec)
    {
        std::string key = opName(rec.op);
        if (rec.op == static_cast<uint8_t>(Op::Sync) && !rec.payload.empty())
            key += std::string("(") + syncReasonName(rec.payload[0]) + ")";
        return key;
    }

    std::vector<FlatField> flatten(const Rec &rec)
    {
        std::vector<FlatField> out;
        Flattener flat{out, ""};
        Reader in{rec.payload.data(), rec.payload.data() + rec.payload.size()};
        auto blob = [&](const char *name)
        {
            uint32_t size = 0;
            in.raw(size);
            if (static_cast<size_t>(in.end - in.cur) < size)
                throw std::runtime_error("blob truncated");
            uint64_t hash = fnv1a(in.cur, size);
            flat((std::string(name) + ".size").c_str(), size);
            flat((std::string(name) + ".fnv1a").c_str(), hash);
            in.cur += size;
        };
        try
        {
            switch (static_cast<Op>(rec.op))
            {
            case Op::Initialize:
            {
                uint32_t size = 0;
                uint64_t hash = 0;
                in.raw(size);
                in.raw(hash);
                flat("vramSize", size);
                flat("vramFnv1a", hash);
                break;
            }
            case Op::Submit:
            {
                int32_t round = 0;
                GSPrimitiveBatch batch{};
                in.get(round);
                in.get(batch);
                flat("fpRound", round);
                flat.object("batch", batch);
                break;
            }
            case Op::LoadClut:
            {
                GSTex0Reg tex0{};
                GSTexClutReg clut{};
                in.get(tex0);
                in.get(clut);
                flat.object("tex0", tex0);
                flat.object("texclut", clut);
                break;
            }
            case Op::BeginTransfer:
            {
                GSTransferCommand command{};
                in.get(command);
                flat.object("command", command);
                break;
            }
            case Op::UploadImage:
                blob("data");
                break;
            case Op::Sync:
            {
                uint8_t reason = 0;
                in.get(reason);
                flat("reason", reason);
                break;
            }
            case Op::Present:
            {
                GSPresentationRequest request{};
                in.get(request);
                flat.object("request", request);
                break;
            }
            case Op::ClearFramebuffer:
            {
                GSContext context{};
                uint32_t rgba = 0;
                in.get(context);
                in.get(rgba);
                flat.object("context", context);
                flat("rgba", rgba);
                break;
            }
            case Op::ConsumeLocalToHost:
            {
                uint32_t maxBytes = 0;
                in.get(maxBytes);
                flat("maxBytes", maxBytes);
                break;
            }
            case Op::ReadVram:
            case Op::WriteVram:
            {
                static const char *const names[] = {"psm", "base", "bw", "x", "y", "value"};
                const int count = rec.op == static_cast<uint8_t>(Op::ReadVram) ? 5 : 6;
                for (int i = 0; i < count; ++i)
                {
                    uint32_t v = 0;
                    in.get(v);
                    flat(names[i], v);
                }
                break;
            }
            default:
                break;
            }
        }
        catch (const std::exception &error)
        {
            out.push_back({"<decode-error>", error.what(), 0});
        }
        return out;
    }

    std::string describe(const Rec &rec)
    {
        char text[512];
        std::string s = statKey(rec);
        if (rec.op == static_cast<uint8_t>(Op::Submit))
        {
            int32_t round = 0;
            GSPrimitiveBatch b{};
            Reader in{rec.payload.data(), rec.payload.data() + rec.payload.size()};
            try
            {
                in.get(round);
                in.get(b);
            }
            catch (...)
            {
                return s + " <truncated>";
            }
            std::snprintf(text, sizeof(text), " prim=%u n=%u", static_cast<unsigned>(b.state.prim.type), b.vertexCount);
            s += text;
            for (uint8_t i = 0; i < std::min<uint8_t>(b.vertexCount, 3u); ++i)
            {
                const GSVertex &v = b.vertices[i];
                std::snprintf(text, sizeof(text), " (%.4g,%.4g,%.6g rgba=%02x%02x%02x%02x)", v.x, v.y, v.z, v.r, v.g,
                              v.b, v.a);
                s += text;
            }
            std::snprintf(text, sizeof(text), " fbp=0x%x tme=%d tbp0=0x%x round=%d", b.state.context.frame.fbp,
                          b.state.prim.tme ? 1 : 0, b.state.context.tex0.tbp0, round);
            s += text;
            return s;
        }
        const auto fields = flatten(rec);
        size_t shown = 0;
        for (const auto &f : fields)
        {
            if (shown++ == 8)
            {
                s += " ...";
                break;
            }
            s += " " + f.name + "=" + f.value.substr(0, f.value.find(' '));
        }
        return s;
    }

    void printUsage()
    {
        std::fprintf(stderr,
                     "usage: dw5xl_gsrec stats <rec>\n"
                     "       dw5xl_gsrec dump <rec> [--from N] [--count N] [--fields]\n"
                     "       dw5xl_gsrec diff <a.rec> <b.rec> [--strict] [--ignore op,..] [--keep op,..]\n"
                     "                        [--with-ticks] [--ignore-fpround] [--report N] [--context N] [--stop]\n"
                     "ops: Initialize Reset Submit LoadClut BeginTransfer UploadImage Flush TextureFlush Sync\n"
                     "     Sync(Finish|LocalToHost|Presentation|DebugReadback|Reset) Present ClearFramebuffer\n"
                     "     ConsumeLocalToHost ReadVram WriteVram SnapshotVram\n");
    }

    int cmdStats(const std::string &path)
    {
        RecFile file(path);
        std::map<std::string, std::pair<uint64_t, uint64_t>> counts; // records, payload bytes
        Rec rec;
        uint64_t firstTick = UINT64_MAX;
        while (file.next(rec))
        {
            auto &c = counts[statKey(rec)];
            ++c.first;
            c.second += rec.payload.size();
            if (rec.op == static_cast<uint8_t>(Op::Present) && firstTick == UINT64_MAX)
                firstTick = file.lastTick();
        }
        std::printf("%s: %llu records, %llu presents", path.c_str(), static_cast<unsigned long long>(file.count()),
                    static_cast<unsigned long long>(file.frames()));
        if (file.frames() != 0u)
            std::printf(" (vsync ticks %llu..%llu)", static_cast<unsigned long long>(firstTick),
                        static_cast<unsigned long long>(file.lastTick()));
        std::printf("%s\n", file.truncated() ? " [truncated tail]" : "");
        for (const auto &[key, c] : counts)
            std::printf("  %-24s %12llu records %14llu payload bytes\n", key.c_str(),
                        static_cast<unsigned long long>(c.first), static_cast<unsigned long long>(c.second));
        return 0;
    }

    int cmdDump(const std::string &path, uint64_t from, uint64_t count, bool allFields)
    {
        RecFile file(path);
        Rec rec;
        while (file.next(rec))
        {
            if (rec.index < from)
                continue;
            if (rec.index >= from + count)
                break;
            std::printf("#%llu frame=%llu %s\n", static_cast<unsigned long long>(rec.index),
                        static_cast<unsigned long long>(file.frames()), describe(rec).c_str());
            if (allFields)
                for (const auto &f : flatten(rec))
                    std::printf("    %-40s %s\n", f.name.c_str(), f.value.c_str());
        }
        return 0;
    }

    struct DiffOptions
    {
        std::vector<std::string> ignore{"Present", "SnapshotVram", "ReadVram", "Flush", "Sync(Presentation)",
                                        "Sync(DebugReadback)"};
        bool withTicks = false;
        bool ignoreFpRound = false;
        uint64_t report = 1;
        size_t context = 4;
        bool stop = false;
    };

    std::vector<std::string> splitList(const std::string &text)
    {
        std::vector<std::string> out;
        size_t start = 0;
        while (start <= text.size())
        {
            const size_t comma = text.find(',', start);
            const std::string item = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            if (!item.empty())
                out.push_back(item);
            if (comma == std::string::npos)
                break;
            start = comma + 1;
        }
        return out;
    }

    struct ContextEntry
    {
        uint64_t indexA;
        uint64_t indexB;
        uint8_t op;
        uint8_t firstByte;
    };

    // ignore[op][syncReason+1]: column 0 = whole op, 1..5 = Sync by reason.
    using IgnoreTable = std::array<std::array<bool, 6>, kOpCount>;

    IgnoreTable buildIgnoreTable(const DiffOptions &options)
    {
        IgnoreTable table{};
        for (const auto &name : options.ignore)
        {
            bool known = false;
            for (uint8_t op = 1; op < kOpCount; ++op)
            {
                if (name == opName(op))
                {
                    table[op].fill(true);
                    known = true;
                }
                for (uint8_t reason = 0; op == static_cast<uint8_t>(Op::Sync) && reason < 5u; ++reason)
                    if (name == std::string("Sync(") + syncReasonName(reason) + ")")
                    {
                        table[op][reason + 1u] = true;
                        known = true;
                    }
            }
            if (!known)
                throw std::runtime_error("unknown record type in --ignore/--keep: " + name);
        }
        return table;
    }

    bool isIgnored(const Rec &rec, const IgnoreTable &table)
    {
        if (rec.op >= kOpCount)
            return false;
        if (rec.op == static_cast<uint8_t>(Op::Sync) && !rec.payload.empty() && rec.payload[0] < 5u)
            return table[rec.op][rec.payload[0] + 1u];
        return table[rec.op][0];
    }

    bool sameRecord(const Rec &a, const Rec &b, const DiffOptions &options)
    {
        if (a.op != b.op || a.payload.size() != b.payload.size())
            return false;
        if (a.op == static_cast<uint8_t>(Op::Present) && !options.withTicks &&
            a.payload.size() >= kPresentTickOffset + 8u)
        {
            return std::memcmp(a.payload.data(), b.payload.data(), kPresentTickOffset) == 0 &&
                   std::memcmp(a.payload.data() + kPresentTickOffset + 8u, b.payload.data() + kPresentTickOffset + 8u,
                               a.payload.size() - kPresentTickOffset - 8u) == 0;
        }
        if (a.op == static_cast<uint8_t>(Op::Submit) && options.ignoreFpRound && a.payload.size() >= 4u)
            return std::memcmp(a.payload.data() + 4u, b.payload.data() + 4u, a.payload.size() - 4u) == 0;
        return a.payload == b.payload;
    }

    void printDivergence(uint64_t streamIndex, const Rec *a, const Rec *b, const RecFile &fa, const RecFile &fb,
                         const std::deque<ContextEntry> &context, const DiffOptions &options)
    {
        std::printf("DIVERGENCE at compared record %llu\n", static_cast<unsigned long long>(streamIndex));
        auto where = [](const char *label, const Rec *rec, const RecFile &file)
        {
            if (!rec)
            {
                std::printf("  %s: <end of stream after %llu records>\n", label,
                            static_cast<unsigned long long>(file.count()));
                return;
            }
            std::printf("  %s: record #%llu, frame %llu (last presented vsync tick %llu): %s\n", label,
                        static_cast<unsigned long long>(rec->index), static_cast<unsigned long long>(file.frames()),
                        static_cast<unsigned long long>(file.lastTick()), describe(*rec).c_str());
        };
        where("A", a, fa);
        where("B", b, fb);
        if (!context.empty())
        {
            std::printf("  preceding matching records:\n");
            for (const auto &entry : context)
            {
                std::string name = opName(entry.op);
                if (entry.op == static_cast<uint8_t>(Op::Sync))
                    name += std::string("(") + syncReasonName(entry.firstByte) + ")";
                std::printf("    A#%llu B#%llu %s\n", static_cast<unsigned long long>(entry.indexA),
                            static_cast<unsigned long long>(entry.indexB), name.c_str());
            }
        }
        if (a && b && a->op == b->op)
        {
            const auto la = flatten(*a);
            const auto lb = flatten(*b);
            size_t printed = 0;
            for (size_t i = 0; i < std::max(la.size(), lb.size()); ++i)
            {
                const FlatField *x = i < la.size() ? &la[i] : nullptr;
                const FlatField *y = i < lb.size() ? &lb[i] : nullptr;
                if (x && y && x->name == y->name && x->value == y->value)
                    continue;
                if (x && y && x->name == "request.vsyncTick" && !options.withTicks)
                    continue;
                if (x && y && x->name == "fpRound" && options.ignoreFpRound)
                    continue;
                if (printed++ == 24)
                {
                    std::printf("    ...\n");
                    break;
                }
                std::printf("    %-40s A=%s  B=%s\n", (x ? x->name : y->name).c_str(), x ? x->value.c_str() : "-",
                            y ? y->value.c_str() : "-");
            }
            if (printed == 0)
            {
                // Same decoded fields: raw blob bytes differ (e.g. upload data).
                size_t offset = 0;
                while (offset < a->payload.size() && offset < b->payload.size() &&
                       a->payload[offset] == b->payload[offset])
                    ++offset;
                std::printf("    payload bytes differ first at offset %zu (sizes %zu / %zu)\n", offset,
                            a->payload.size(), b->payload.size());
            }
        }
    }

    int cmdDiff(const std::string &pathA, const std::string &pathB, const DiffOptions &options)
    {
        RecFile fa(pathA);
        RecFile fb(pathB);
        std::map<std::string, std::array<uint64_t, 2>> counts;
        std::map<std::string, uint64_t> diffsByOp;
        std::deque<ContextEntry> context;
        uint64_t compared = 0;
        uint64_t differing = 0;
        uint64_t extraA = 0;
        uint64_t extraB = 0;
        bool reportedEnd = false;
        const IgnoreTable ignoreTable = buildIgnoreTable(options);
        std::array<std::map<uint16_t, uint64_t>, 2> rawCounts;

        auto nextKept = [&](RecFile &file, Rec &rec, int side) -> bool
        {
            while (file.next(rec))
            {
                ++rawCounts[side][static_cast<uint16_t>(
                    (rec.op << 8) | (rec.op == static_cast<uint8_t>(Op::Sync) && !rec.payload.empty() ? rec.payload[0] : 0u))];
                if (!isIgnored(rec, ignoreTable))
                    return true;
            }
            return false;
        };

        Rec a;
        Rec b;
        for (;;)
        {
            const bool haveA = nextKept(fa, a, 0);
            const bool haveB = nextKept(fb, b, 1);
            if (!haveA && !haveB)
                break;
            if (!haveA || !haveB)
            {
                if (differing == 0u && !reportedEnd)
                    printDivergence(compared, haveA ? &a : nullptr, haveB ? &b : nullptr, fa, fb, context, options);
                reportedEnd = true;
                (haveA ? extraA : extraB) = 1;
                Rec rest;
                while (haveA ? nextKept(fa, rest, 0) : nextKept(fb, rest, 1))
                    ++(haveA ? extraA : extraB);
                break;
            }
            if (sameRecord(a, b, options))
            {
                if (options.context != 0u)
                {
                    context.push_back({a.index, b.index, a.op, a.payload.empty() ? uint8_t{0} : a.payload[0]});
                    if (context.size() > options.context)
                        context.pop_front();
                }
                ++compared;
                continue;
            }
            ++differing;
            ++diffsByOp[a.op == b.op ? statKey(a) : statKey(a) + " vs " + statKey(b)];
            if (differing <= options.report)
                printDivergence(compared, &a, &b, fa, fb, context, options);
            context.clear();
            ++compared;
            if (options.stop)
                break;
        }

        for (int side = 0; side < 2; ++side)
            for (const auto &[key, n] : rawCounts[side])
            {
                std::string name = opName(static_cast<uint8_t>(key >> 8));
                if ((key >> 8) == static_cast<uint8_t>(Op::Sync))
                    name += std::string("(") + syncReasonName(static_cast<uint8_t>(key & 0xFFu)) + ")";
                counts[name][side] += n;
            }
        std::printf("\nSUMMARY  A=%s  B=%s\n", pathA.c_str(), pathB.c_str());
        std::printf("  ignored: ");
        for (const auto &name : options.ignore)
            std::printf("%s ", name.c_str());
        std::printf("%s%s\n", options.withTicks ? "" : "+ Present.vsyncTick ",
                    options.ignoreFpRound ? "+ Submit.fpRound" : "");
        std::printf("  %-24s %12s %12s\n", "record type", "A", "B");
        for (const auto &[key, c] : counts)
            std::printf("  %-24s %12llu %12llu%s\n", key.c_str(), static_cast<unsigned long long>(c[0]),
                        static_cast<unsigned long long>(c[1]), c[0] != c[1] ? "   <- count differs" : "");
        std::printf("  compared %llu kept records: %llu differ positionally", static_cast<unsigned long long>(compared),
                    static_cast<unsigned long long>(differing));
        if (extraA || extraB)
            std::printf(", %llu extra in A, %llu extra in B", static_cast<unsigned long long>(extraA),
                        static_cast<unsigned long long>(extraB));
        std::printf("%s\n", options.stop && differing ? " (stopped at first divergence)" : "");
        for (const auto &[key, n] : diffsByOp)
            std::printf("    differing %-28s %llu\n", key.c_str(), static_cast<unsigned long long>(n));
        if (fa.truncated() || fb.truncated())
            std::printf("  warning: truncated tail in %s\n", fa.truncated() ? "A" : "B");
        const bool identical = differing == 0u && extraA == 0u && extraB == 0u;
        std::printf("RESULT: %s\n", identical ? "IDENTICAL" : "DIFFERENT");
        return identical ? 0 : 1;
    }
}

int main(int argc, char **argv)
{
    try
    {
        if (argc < 3)
        {
            printUsage();
            return 2;
        }
        const std::string cmd = argv[1];
        if (cmd == "stats")
            return cmdStats(argv[2]);
        if (cmd == "dump")
        {
            uint64_t from = 0, count = UINT64_MAX;
            bool fields = false;
            for (int i = 3; i < argc; ++i)
            {
                const std::string arg = argv[i];
                if (arg == "--from" && i + 1 < argc)
                    from = std::strtoull(argv[++i], nullptr, 0);
                else if (arg == "--count" && i + 1 < argc)
                    count = std::strtoull(argv[++i], nullptr, 0);
                else if (arg == "--fields")
                    fields = true;
                else
                {
                    printUsage();
                    return 2;
                }
            }
            return cmdDump(argv[2], from, count, fields);
        }
        if (cmd == "diff" && argc >= 4)
        {
            DiffOptions options;
            for (int i = 4; i < argc; ++i)
            {
                const std::string arg = argv[i];
                if (arg == "--strict")
                    options.ignore.clear();
                else if (arg == "--ignore" && i + 1 < argc)
                    for (const auto &item : splitList(argv[++i]))
                        options.ignore.push_back(item);
                else if (arg == "--keep" && i + 1 < argc)
                    for (const auto &item : splitList(argv[++i]))
                        options.ignore.erase(std::remove(options.ignore.begin(), options.ignore.end(), item),
                                             options.ignore.end());
                else if (arg == "--with-ticks")
                    options.withTicks = true;
                else if (arg == "--ignore-fpround")
                    options.ignoreFpRound = true;
                else if (arg == "--report" && i + 1 < argc)
                    options.report = std::strtoull(argv[++i], nullptr, 0);
                else if (arg == "--context" && i + 1 < argc)
                    options.context = std::strtoull(argv[++i], nullptr, 0);
                else if (arg == "--stop")
                    options.stop = true;
                else
                {
                    printUsage();
                    return 2;
                }
            }
            return cmdDiff(argv[2], argv[3], options);
        }
        printUsage();
        return 2;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "dw5xl_gsrec: %s\n", error.what());
        return 2;
    }
}
