// DW5XL-DEBUG: opt-in VIF1 replay-input capture (see vif1_capture.h).
#include "vif1_capture.h"

#include "runtime/ps2_memory.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <type_traits>

namespace Vif1Capture
{
    namespace
    {
        bool requestedFromEnv()
        {
            const char *path = std::getenv("PS2X_VIF1_CAPTURE");
            return path && *path;
        }

        template <class T> void put(std::vector<uint8_t> &out, const T &value)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            const size_t offset = out.size();
            out.resize(offset + sizeof(T));
            std::memcpy(out.data() + offset, &value, sizeof(T));
        }
        void putBool(std::vector<uint8_t> &out, bool value) { put(out, static_cast<uint8_t>(value ? 1u : 0u)); }
        void putBlob(std::vector<uint8_t> &out, const std::vector<uint8_t> &blob)
        {
            put(out, static_cast<uint32_t>(blob.size()));
            out.insert(out.end(), blob.begin(), blob.end());
        }

        template <class T> bool get(const uint8_t *&cur, const uint8_t *end, T &value)
        {
            if (static_cast<size_t>(end - cur) < sizeof(T))
                return false;
            std::memcpy(&value, cur, sizeof(T));
            cur += sizeof(T);
            return true;
        }
        bool getBool(const uint8_t *&cur, const uint8_t *end, bool &value)
        {
            uint8_t b = 0;
            if (!get(cur, end, b))
                return false;
            value = b != 0u;
            return true;
        }
        bool getBlob(const uint8_t *&cur, const uint8_t *end, std::vector<uint8_t> &blob)
        {
            uint32_t size = 0;
            if (!get(cur, end, size) || static_cast<size_t>(end - cur) < size)
                return false;
            blob.assign(cur, cur + size);
            cur += size;
            return true;
        }

        uint64_t envU64(const char *name, uint64_t fallback)
        {
            const char *value = std::getenv(name);
            return (value && *value) ? std::strtoull(value, nullptr, 0) : fallback;
        }

        struct Capture
        {
            std::mutex mutex;
            bool configured = false;
            bool done = false;
            std::string path;
            uint64_t fromTick = 0;
            uint64_t toTick = 0;
            uint64_t maxBytes = 0;
            Writer writer;
            VU1Interpreter *vu1 = nullptr;
            std::function<uint32_t()> fbrst;

            void configure()
            {
                if (configured)
                    return;
                configured = true;
                path = std::getenv("PS2X_VIF1_CAPTURE");
                fromTick = envU64("PS2X_VIF1_CAPTURE_FROM", 0u);
                toTick = envU64("PS2X_VIF1_CAPTURE_TO", fromTick + 10u);
                maxBytes = envU64("PS2X_VIF1_CAPTURE_MAX_MB", 256u) << 20u;
                std::fprintf(stderr, "[vif1cap] armed: %s ticks %llu..%llu max %llu MiB\n", path.c_str(),
                             static_cast<unsigned long long>(fromTick), static_cast<unsigned long long>(toTick),
                             static_cast<unsigned long long>(maxBytes >> 20u));
            }

            uint32_t currentFbrst() const { return fbrst ? fbrst() : 0u; }

            void finish(bool truncated)
            {
                if (done)
                    return;
                done = true;
                if (writer.isOpen())
                {
                    std::fprintf(stderr, "[vif1cap] done: %u kicks, %llu bytes%s -> %s\n", writer.kicks(),
                                 static_cast<unsigned long long>(writer.kickBytes()),
                                 truncated ? " (size limit reached)" : "", path.c_str());
                    writer.writeEnd(truncated);
                    writer.close();
                }
            }

            ~Capture() { finish(false); }
        };

        Capture &capture()
        {
            static Capture instance;
            return instance;
        }
    }

    const bool g_requested = requestedFromEnv();

    void attach(VU1Interpreter *vu1, std::function<uint32_t()> fbrstProvider)
    {
        if (!g_requested)
            return;
        Capture &c = capture();
        std::lock_guard<std::mutex> lock(c.mutex);
        c.vu1 = vu1;
        c.fbrst = std::move(fbrstProvider);
    }

    void onVif1Data(PS2Memory &memory, const uint8_t *data, uint32_t sizeBytes)
    {
        Capture &c = capture();
        std::lock_guard<std::mutex> lock(c.mutex);
        c.configure();
        if (c.done || !data || sizeBytes == 0u)
            return;
        const uint64_t tick = memory.gs().vsyncTick.load(std::memory_order_acquire);
        if (tick < c.fromTick)
            return;
        if (tick > c.toTick)
        {
            c.finish(false);
            return;
        }
        if (c.writer.kickBytes() + sizeBytes > c.maxBytes)
        {
            c.finish(true);
            return;
        }
        if (!c.writer.isOpen())
        {
            if (!c.writer.open(c.path))
            {
                std::fprintf(stderr, "[vif1cap] cannot open %s\n", c.path.c_str());
                c.done = true;
                return;
            }
            StateSnapshot state = snapshotMemory(memory);
            state.tick = tick;
            state.vu0Fbrst = c.currentFbrst();
            state.hasVu1State = c.vu1 != nullptr;
            if (c.vu1)
                state.vu1 = c.vu1->state();
            c.writer.writeState(state);
            std::fprintf(stderr, "[vif1cap] started at tick %llu%s\n", static_cast<unsigned long long>(tick),
                         c.vu1 ? "" : " (no VU1 core attached: VU1 registers not captured)");
        }
        c.writer.writeKick(tick, c.currentFbrst(), data, sizeBytes);
    }

    void onVif1RegWrite(PS2Memory &memory, uint32_t address, uint32_t value)
    {
        Capture &c = capture();
        std::lock_guard<std::mutex> lock(c.mutex);
        if (c.done || !c.writer.isOpen())
            return;
        c.writer.writeVifReg(memory.gs().vsyncTick.load(std::memory_order_acquire), address, value);
    }

    // ---- format -------------------------------------------------------------
    void writeVu1State(std::vector<uint8_t> &out, const VU1State &s)
    {
        for (const auto &vf : s.vf)
            for (float lane : vf)
                put(out, lane);
        for (int32_t vi : s.vi)
            put(out, vi);
        for (float lane : s.acc)
            put(out, lane);
        put(out, s.q);
        put(out, s.p);
        put(out, s.i);
        for (uint32_t v : {s.r, s.pc, s.mac, s.clip, s.status})
            put(out, v);
        put(out, s.cycles);
        for (bool b : {s.ebit, s.haltAfterDelaySlot, s.dBitEnabled, s.tBitEnabled, s.stoppedByD, s.stoppedByT})
            putBool(out, b);
        put(out, s.top);
        put(out, s.itop);
        putBool(out, s.branchPending);
        put(out, s.branchTarget);
        put(out, s.branchDelay);
    }

    bool readVu1State(const uint8_t *&cur, const uint8_t *end, VU1State &s)
    {
        bool ok = true;
        for (auto &vf : s.vf)
            for (float &lane : vf)
                ok = ok && get(cur, end, lane);
        for (int32_t &vi : s.vi)
            ok = ok && get(cur, end, vi);
        for (float &lane : s.acc)
            ok = ok && get(cur, end, lane);
        ok = ok && get(cur, end, s.q) && get(cur, end, s.p) && get(cur, end, s.i);
        for (uint32_t *v : {&s.r, &s.pc, &s.mac, &s.clip, &s.status})
            ok = ok && get(cur, end, *v);
        ok = ok && get(cur, end, s.cycles);
        for (bool *b : {&s.ebit, &s.haltAfterDelaySlot, &s.dBitEnabled, &s.tBitEnabled, &s.stoppedByD, &s.stoppedByT})
            ok = ok && getBool(cur, end, *b);
        ok = ok && get(cur, end, s.top) && get(cur, end, s.itop) && getBool(cur, end, s.branchPending) &&
             get(cur, end, s.branchTarget) && get(cur, end, s.branchDelay);
        return ok;
    }

    void writeState(std::vector<uint8_t> &out, const StateSnapshot &s)
    {
        put(out, s.tick);
        put(out, s.vu0Fbrst);
        for (uint32_t r : s.vif1Regs)
            put(out, r);
        putBool(out, s.path3Masked);
        put(out, s.pendingPath2Qwc);
        putBool(out, s.pendingPath2DirectHl);
        put(out, static_cast<uint32_t>(s.maskedPath3Packets.size()));
        for (const auto &packet : s.maskedPath3Packets)
            putBlob(out, packet);
        putBool(out, s.hasVu1State);
        writeVu1State(out, s.vu1);
        putBlob(out, s.vu1Code);
        putBlob(out, s.vu1Data);
    }

    bool readState(const uint8_t *cur, const uint8_t *end, StateSnapshot &s)
    {
        bool ok = get(cur, end, s.tick) && get(cur, end, s.vu0Fbrst);
        for (uint32_t &r : s.vif1Regs)
            ok = ok && get(cur, end, r);
        ok = ok && getBool(cur, end, s.path3Masked) && get(cur, end, s.pendingPath2Qwc) &&
             getBool(cur, end, s.pendingPath2DirectHl);
        uint32_t packets = 0;
        ok = ok && get(cur, end, packets);
        s.maskedPath3Packets.clear();
        for (uint32_t i = 0; ok && i < packets; ++i)
        {
            s.maskedPath3Packets.emplace_back();
            ok = getBlob(cur, end, s.maskedPath3Packets.back());
        }
        ok = ok && getBool(cur, end, s.hasVu1State) && readVu1State(cur, end, s.vu1) &&
             getBlob(cur, end, s.vu1Code) && getBlob(cur, end, s.vu1Data);
        return ok;
    }

    StateSnapshot snapshotMemory(const PS2Memory &memory)
    {
        static_assert(sizeof(VIFRegisters) == sizeof(StateSnapshot::vif1Regs));
        StateSnapshot s;
        std::memcpy(s.vif1Regs, &memory.vif1_regs, sizeof(s.vif1Regs));
        s.path3Masked = memory.m_path3Masked;
        s.pendingPath2Qwc = memory.m_vif1PendingPath2ImageQwc;
        s.pendingPath2DirectHl = memory.m_vif1PendingPath2DirectHl;
        s.maskedPath3Packets = memory.m_path3MaskedFifo;
        if (const uint8_t *code = memory.getVU1Code())
            s.vu1Code.assign(code, code + PS2_VU1_CODE_SIZE);
        if (const uint8_t *data = memory.getVU1Data())
            s.vu1Data.assign(data, data + PS2_VU1_DATA_SIZE);
        return s;
    }

    // ---- writer -------------------------------------------------------------
    bool Writer::open(const std::string &path)
    {
        close();
        m_file = std::fopen(path.c_str(), "wb");
        if (!m_file)
            return false;
        std::setvbuf(m_file, nullptr, _IOFBF, 1u << 20u);
        m_kicks = 0;
        m_kickBytes = 0;
        m_endWritten = false;
        std::fwrite(kMagic, 1u, sizeof(kMagic), m_file);
        const uint32_t header[2] = {kVersion, 0u};
        std::fwrite(header, sizeof(header), 1u, m_file);
        return true;
    }

    void Writer::record(Rec type, const std::vector<uint8_t> &payload, const uint8_t *tail, uint32_t tailSize)
    {
        if (!m_file)
            return;
        const uint8_t t = static_cast<uint8_t>(type);
        const uint32_t size = static_cast<uint32_t>(payload.size()) + tailSize;
        std::fwrite(&t, 1u, 1u, m_file);
        std::fwrite(&size, sizeof(size), 1u, m_file);
        if (!payload.empty())
            std::fwrite(payload.data(), 1u, payload.size(), m_file);
        if (tail && tailSize != 0u)
            std::fwrite(tail, 1u, tailSize, m_file);
    }

    void Writer::writeState(const StateSnapshot &state)
    {
        std::vector<uint8_t> payload;
        Vif1Capture::writeState(payload, state);
        record(Rec::State, payload);
        if (m_file)
            std::fflush(m_file);
    }

    void Writer::writeKick(uint64_t tick, uint32_t vu0Fbrst, const uint8_t *data, uint32_t size)
    {
        std::vector<uint8_t> payload;
        put(payload, tick);
        put(payload, vu0Fbrst);
        put(payload, size);
        record(Rec::Kick, payload, data, size);
        ++m_kicks;
        m_kickBytes += size;
        if (m_file)
            std::fflush(m_file); // a capture stays usable when the process exits abruptly
    }

    void Writer::writeVifReg(uint64_t tick, uint32_t address, uint32_t value)
    {
        std::vector<uint8_t> payload;
        put(payload, tick);
        put(payload, address);
        put(payload, value);
        record(Rec::VifReg, payload);
    }

    void Writer::writeEnd(bool truncatedByLimit)
    {
        if (!m_file || m_endWritten)
            return;
        std::vector<uint8_t> payload;
        put(payload, m_kicks);
        put(payload, m_kickBytes);
        putBool(payload, truncatedByLimit);
        record(Rec::End, payload);
        m_endWritten = true;
    }

    void Writer::close()
    {
        if (m_file)
        {
            std::fclose(m_file);
            m_file = nullptr;
        }
    }

    bool readFile(const std::string &path, std::vector<Record> &records, std::string &message)
    {
        records.clear();
        std::FILE *file = std::fopen(path.c_str(), "rb");
        if (!file)
        {
            message = "cannot open " + path;
            return false;
        }
        char magic[8]{};
        uint32_t header[2]{};
        if (std::fread(magic, 1u, sizeof(magic), file) != sizeof(magic) ||
            std::memcmp(magic, kMagic, sizeof(magic)) != 0 ||
            std::fread(header, sizeof(header), 1u, file) != 1u || header[0] != kVersion)
        {
            std::fclose(file);
            message = path + ": not a v1cap (version " + std::to_string(kVersion) + ") file";
            return false;
        }
        for (;;)
        {
            uint8_t type = 0;
            uint32_t size = 0;
            if (std::fread(&type, 1u, 1u, file) != 1u)
                break;
            Record rec;
            rec.type = static_cast<Rec>(type);
            if (std::fread(&size, sizeof(size), 1u, file) != 1u)
            {
                message = "truncated record header at end of file (dropped)";
                break;
            }
            rec.payload.resize(size);
            if (size != 0u && std::fread(rec.payload.data(), 1u, size, file) != size)
            {
                message = "truncated record at end of file (dropped)";
                break;
            }
            records.push_back(std::move(rec));
        }
        std::fclose(file);
        return true;
    }
}
