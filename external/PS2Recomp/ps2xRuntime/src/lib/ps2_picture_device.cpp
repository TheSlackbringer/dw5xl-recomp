// DW5XL (issue 049, amendment v3.7): the pictures' own VIF1/VU1 device. See the header.
#include "runtime/ps2_picture_device.h"
#include "runtime/ps2_memory.h"
#include "ThreadNaming.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace ps2x
{
    void GifStreamHash::add(GifPathId pathId, const uint8_t *data, uint32_t size)
    {
        const auto mix = [this](uint64_t word)
        {
            hash = (hash ^ word) * 1099511628211ull;
            hash ^= hash >> 29;
        };
        if (static_cast<int>(pathId) != path)
        {
            path = static_cast<int>(pathId);
            mix(0x5041544800000000ull | static_cast<uint64_t>(path));
        }
        // GIF packets are whole quadwords, so the words fall the same wherever a packet was cut.
        uint32_t i = 0;
        for (; i + 8u <= size; i += 8u)
        {
            uint64_t word;
            std::memcpy(&word, data + i, sizeof(word));
            mix(word);
        }
        for (; i < size; ++i)
            mix(data[i]);
        bytes += size;
    }

    namespace
    {
        // A picture of a frame that is over stops at its next VIF code (as the game's device stops a transfer at
        // its checkpoint), or where the GS side refuses its packets: a private signal, never seen outside the device.
        struct GivenUp {};
    }

    struct PictureDevice::Counters
    {
        std::atomic<uint64_t> dropped{0};
    };

    struct PictureDevice::Unit final : Vif1Unit::Host
    {
        PictureDevice &device;
        GS &gs;
        Vif1Unit vif1;
        std::vector<uint8_t> code = std::vector<uint8_t>(PS2_VU1_CODE_SIZE);
        std::vector<uint8_t> data = std::vector<uint8_t>(PS2_VU1_DATA_SIZE);
        std::atomic<uint64_t> generation{PS2Memory::nextCodeGeneration()};
        VU1Interpreter vu{VU1Interpreter::Unit::VU1};
        VU1Interpreter::Host vuHost;
        uint64_t frame = 0; // the running picture's
        uint8_t part = 0;   // head, body or tail

        Unit(PictureDevice &owner, GS &anyGs) : device(owner), gs(anyGs)
        {
            vif1.vu1Code = code.data();
            vif1.vu1Data = data.data();
            vuHost.code = code.data();
            vuHost.generation = &generation;
            vuHost.gif = [this](const uint8_t *bytes, uint32_t size) { device.addPacket(GifPathId::Path1, bytes, size, part); };
            vu.setHost(&vuHost);
        }

        void vif1Gif(const uint8_t *bytes, uint32_t size, bool) override { device.addPacket(GifPathId::Path2, bytes, size, part); }
        void vif1Start(bool resume, uint32_t startPC, uint32_t top, uint32_t itop) override
        {
            if (resume)
                vu.resume(vif1.vu1Code, PS2_VU1_CODE_SIZE, vif1.vu1Data, PS2_VU1_DATA_SIZE, gs, nullptr, top, itop, 65536);
            else
                vu.execute(vif1.vu1Code, PS2_VU1_CODE_SIZE, vif1.vu1Data, PS2_VU1_DATA_SIZE, gs, nullptr, startPC, top, itop, 65536);
        }
        void vif1Path3Mask(bool) override {} // no PATH3 here
        void vif1CodeChanged() override { generation.store(PS2Memory::nextCodeGeneration(), std::memory_order_relaxed); }
        void vif1Checkpoint(uint32_t) override
        {
            // (m_refused, amendment v3.9: the GS side refused the picture at its start, there was no place for
            // its read. Its VIF1 work stops here too.)
            if (device.givenUp(frame) || device.cutNow() || device.m_refused)
                throw GivenUp{};
        }
    };

    PictureDevice::PictureDevice(GS &anyGs, Hooks hooks)
        : m_hooks(std::move(hooks)), m_unit(std::make_unique<Unit>(*this, anyGs)), m_counters(std::make_shared<Counters>())
    {
        if (const char *v = std::getenv("PS2X_SMOOTH_CUT_EVERY"); v && *v)
            m_cutEvery = std::strtoull(v, nullptr, 0);
        m_thread = std::thread([this]
        {
            ThreadNaming::SetCurrentThreadName("ps2-pictures");
            threadMain();
        });
    }

    PictureDevice::~PictureDevice()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_wake.notify_all();
        if (m_thread.joinable())
            m_thread.join();
    }

    void PictureDevice::takeStart(uint64_t frame, const Vif1Unit &vif1, const VU1Interpreter &vu1)
    {
        std::shared_ptr<Start> start;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            // The starts of earlier frames are over; one that no running picture holds is used again.
            for (auto it = m_starts.begin(); it != m_starts.end();)
            {
                if ((*it)->frame < frame)
                {
                    if (it->use_count() == 1 && m_spare.size() < 4u)
                        m_spare.push_back(std::move(*it));
                    it = m_starts.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            if (!m_spare.empty())
            {
                start = std::move(m_spare.back());
                m_spare.pop_back();
            }
        }
        if (!start)
        {
            start = std::make_shared<Start>();
            start->code.resize(PS2_VU1_CODE_SIZE);
            start->data.resize(PS2_VU1_DATA_SIZE);
        }
        start->frame = frame;
        start->regs = vif1.regs;
        start->pendingPath2ImageQwc = vif1.pendingPath2ImageQwc;
        start->pendingPath2DirectHl = vif1.pendingPath2DirectHl;
        if (vif1.vu1Code)
            std::memcpy(start->code.data(), vif1.vu1Code, PS2_VU1_CODE_SIZE);
        if (vif1.vu1Data)
            std::memcpy(start->data.data(), vif1.vu1Data, PS2_VU1_DATA_SIZE);
        vu1.saveRunState(start->vu);
        // The game's device runs its commands under the floating-point environment of the thread that submitted
        // them; this is called from one, so the pictures run under the same.
        std::fegetenv(&start->fenv);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_starts.push_back(std::move(start));
            if (frame > m_startFrame.load(std::memory_order_relaxed))
                m_startFrame.store(frame, std::memory_order_release); // pictures of earlier frames give up
        }
        m_wake.notify_all();
    }

    void PictureDevice::open(uint64_t frame, const std::function<void()> &prepare)
    {
        if (m_hooks.dropBefore)
            m_hooks.dropBefore(frame);
        if (prepare)
            prepare();
        if (m_hooks.allow)
            m_hooks.allow(frame);
    }

    void PictureDevice::submit(Picture picture)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_queue.push_back(std::move(picture));
        }
        m_wake.notify_all();
    }

    void PictureDevice::waitIdle()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_still.wait(lock, [this]
        {
            return !m_busy && (m_queue.empty() || m_queue.front().frame > m_startFrame.load(std::memory_order_relaxed));
        });
    }

    PictureDevice::Totals PictureDevice::totals() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Totals totals = m_totals;
        totals.dropped = m_counters->dropped.load(std::memory_order_relaxed);
        return totals;
    }

    void PictureDevice::threadMain()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        for (;;)
        {
            m_wake.wait(lock, [this]
            {
                return m_stop || (!m_queue.empty() && m_queue.front().frame <= m_startFrame.load(std::memory_order_relaxed));
            });
            if (m_stop)
                return;
            Picture picture = std::move(m_queue.front());
            m_queue.pop_front();
            std::shared_ptr<Start> start;
            if (!givenUp(picture.frame))
            {
                for (const std::shared_ptr<Start> &s : m_starts)
                    if (s->frame == picture.frame)
                        start = s;
            }
            m_busy = true;
            lock.unlock();
            if (start)
            {
                run(picture, *start);
            }
            else
            {
                if (picture.done)
                    picture.done(Result{});
                std::lock_guard<std::mutex> count(m_mutex);
                ++m_totals.givenUp;
            }
            start.reset();
            lock.lock();
            m_busy = false;
            m_still.notify_all();
        }
    }

    void PictureDevice::newBatch(uint64_t frame)
    {
        m_batch.reset(); // (it stays in m_batches)
        for (const std::shared_ptr<GifArbiter::SideBatch> &batch : m_batches)
        {
            if (batch.use_count() == 1) // the GS side let go of it
            {
                m_batch = batch;
                break;
            }
        }
        if (!m_batch)
        {
            m_batch = std::make_shared<GifArbiter::SideBatch>();
            m_batches.push_back(m_batch);
        }
        m_batch->group = frame;
        m_batch->unit = m_unitNumber;
        m_batch->bytes.clear();
        m_batch->packets.clear();
        m_batch->then = nullptr;
    }

    void PictureDevice::sendBatch()
    {
        // Refused: the GS side dropped the picture's frame. Nothing more of the picture is sent; its VIF1 work stops
        // at the next VIF code (the frame's end came first) or runs out. (No stop from here: this is called from
        // inside a VU1 run.)
        if (!m_refused && (!m_hooks.gif || !m_hooks.gif(m_batch)))
            m_refused = true;
        newBatch(m_batch->group);
    }

    void PictureDevice::addPacket(GifPathId path, const uint8_t *bytes, uint32_t size, uint8_t part)
    {
        GifArbiter::SideBatch &batch = *m_batch;
        batch.packets.push_back({path, static_cast<uint32_t>(batch.bytes.size()), size});
        batch.bytes.insert(batch.bytes.end(), bytes, bytes + size);
        m_bytes += size;
        ++m_packets;
        if (m_hashBody && part == 1u)
            m_bodyHash.add(path, bytes, size);
        if (batch.packets.size() >= kBatchPackets || batch.bytes.size() >= kBatchBytes)
            sendBatch();
    }

    void PictureDevice::run(Picture &picture, const Start &start)
    {
        using Clock = std::chrono::steady_clock;
        Unit &u = *m_unit;
        const auto began = Clock::now();
        std::fesetenv(&start.fenv);
        u.frame = picture.frame;
        u.vif1.regs = start.regs;
        u.vif1.pendingPath2ImageQwc = start.pendingPath2ImageQwc;
        u.vif1.pendingPath2DirectHl = start.pendingPath2DirectHl;
        if (std::memcmp(u.code.data(), start.code.data(), PS2_VU1_CODE_SIZE) != 0)
        {
            std::memcpy(u.code.data(), start.code.data(), PS2_VU1_CODE_SIZE);
            u.vif1CodeChanged();
        }
        std::memcpy(u.data.data(), start.data.data(), PS2_VU1_DATA_SIZE);
        u.vu.loadRunState(start.vu);
        m_bodyHash = GifStreamHash{};
        m_hashBody = picture.hashBody;
        m_bytes = 0;
        m_refused = false;
        m_packets = 0;
        m_cutAt = m_cutEvery != 0u && ++m_runs % m_cutEvery == 0u ? 1u + (m_runs / m_cutEvery * 97u) % 1000u : 0u;
        m_batch.reset();
        ++m_unitNumber;
        newBatch(picture.frame);

        bool gaveUp = false;
        try
        {
            const std::vector<uint8_t> *const parts[3] = {&picture.head, &picture.body, &picture.tail};
            for (uint8_t part = 0; part < 3u; ++part)
            {
                u.part = part;
                if (!parts[part]->empty())
                    u.vif1.process(u, parts[part]->data(), static_cast<uint32_t>(parts[part]->size()));
            }
        }
        catch (const GivenUp &)
        {
            gaveUp = true;
            if (cutNow() && !m_batch->packets.empty())
                sendBatch(); // DW5XL-DEBUG PS2X_SMOOTH_CUT_EVERY: the GS side reads up to the cut
        }
        const uint64_t workNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - began).count());
        gaveUp = gaveUp || m_refused;
        if (!gaveUp)
        {
            // The picture's last batch: what follows its last packet on the GS side.
            Result result;
            result.drawn = true;
            if (picture.hashBody)
            {
                result.bodyHash = m_bodyHash.hash;
                result.bodyBytes = m_bodyHash.bytes;
            }
            m_batch->then = [done = picture.done, result, counters = m_counters](bool dropped)
            {
                Result r = result;
                r.drawn = !dropped;
                if (dropped)
                    counters->dropped.fetch_add(1u, std::memory_order_relaxed);
                if (done)
                    done(r);
            };
            if (!m_hooks.gif || !m_hooks.gif(m_batch))
            {
                m_batch->then = nullptr;
                gaveUp = true;
            }
        }
        m_batch.reset();
        if (gaveUp && picture.done)
            picture.done(Result{});
        std::lock_guard<std::mutex> lock(m_mutex);
        if (gaveUp)
        {
            ++m_totals.givenUp;
        }
        else
        {
            ++m_totals.pictures;
            m_totals.workNs += workNs;
            m_totals.bytes += m_bytes;
        }
    }
}
