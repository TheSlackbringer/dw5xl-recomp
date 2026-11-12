#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/ps2_hold_trace.h"
#include "ThreadNaming.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    const bool kGifStats = [] { const char *v = std::getenv("PS2X_GIF_STATS"); return v && *v && *v != '0'; }(); // DW5XL-DEBUG
}

// Path of the packet currently handed to the GS (read by gs_frontend.cpp to
// keep per-path GIFtag continuation state); 0 outside drain().
thread_local int g_gifArbiterCurrentPath = 0;

GifArbiter::GifArbiter(ProcessPacketFn processFn)
{
    setProcessPacketFn(std::move(processFn));
}

GifArbiter::~GifArbiter()
{
    stopWorker();
}

void GifArbiter::setProcessPacketFn(ProcessPacketFn fn)
{
    const bool worker = m_worker.joinable();
    stopWorker();
    m_processFn = std::move(fn);
    if (worker)
        startWorker();
}

bool GifArbiter::workerRequestedByEnv()
{
    const char *value = std::getenv("PS2X_GS_ASYNC");
    return !(value && value[0] == '0' && value[1] == '\0');
}

void GifArbiter::setWorkerEnabled(bool enabled)
{
    if (enabled == m_worker.joinable())
        return;
    if (enabled)
        startWorker();
    else
        stopWorker();
}

void GifArbiter::startWorker()
{
    if (!m_processFn)
        return;
    m_stopWorker = false;
    m_async = true;
    m_worker = std::thread([this]
    {
        ThreadNaming::SetCurrentThreadName("gs-worker");
        workerLoop();
    });
}

void GifArbiter::stopWorker()
{
    if (!m_worker.joinable())
        return;
    waitIdle();
    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        m_stopWorker = true;
    }
    m_workCv.notify_all();
    m_worker.join();
    m_async = false;
}

void GifArbiter::workerLoop()
{
    GifArbiterPacket pkt;
    for (;;)
    {
        bool side = false;
        {
            std::unique_lock<std::mutex> lock(m_workerMutex);
            recycle(m_returned, std::move(pkt.data));
            pkt.data = {};
            m_workerBusy = false;
            const bool wasSide = m_sideRunning;
            m_sideRunning = false;
            if (m_pending.empty())
                m_mainWaiting.store(false, std::memory_order_release);
            if (m_pending.empty() || wasSide)
                m_idleCv.notify_all();
            m_workCv.wait(lock, [this] { return m_stopWorker || !m_pending.empty() || sideReady(); });
            if (!m_pending.empty())
            {
                pkt = std::move(m_pending.front());
                m_pending.pop_front();
                m_workerBusy = true;
            }
            else if (m_stopWorker)
            {
                return;
            }
            else
            {
                // Issue 049 v3.7: nothing of the game's is waiting, so the pictures' packets are read.
                if (!m_sideCurrent)
                {
                    m_sideCurrent = std::move(m_side.front());
                    m_side.pop_front();
                    m_sideAt = 0;
                }
                m_sideRunning = true;
                side = true;
            }
        }
        const bool timed = ps2x::hold::enabled(); // DW5XL-DEBUG PS2X_HOLD_TRACE: what the GS thread spends its time on
        if (side)
        {
            ps2x::hold::Scope role(ps2x::hold::PictureGs);
            runSide();
            continue;
        }
        if (pkt.call)
        {
            ps2x::hold::Scope role(ps2x::hold::GsCall);
            const uint64_t from = timed ? ps2x::hold::nowNs() : 0u;
            pkt.call();
            pkt.call = nullptr;
            if (timed)
                ps2x::hold::counters().packets[ps2x::hold::GsCall].add(ps2x::hold::nowNs() - from);
            continue;
        }
        ps2x::hold::Scope role(ps2x::hold::GameGs);
        const uint64_t from = timed ? ps2x::hold::nowNs() : 0u;
        g_gifArbiterCurrentPath = static_cast<int>(pkt.pathId);
        m_processFn(pkt.data.data(), static_cast<uint32_t>(pkt.data.size()));
        g_gifArbiterCurrentPath = 0;
        if (timed)
            ps2x::hold::counters().packets[ps2x::hold::GameGs].add(ps2x::hold::nowNs() - from);
    }
}

bool GifArbiter::sideReady() const
{
    return m_sideHold == 0 && (m_sideCurrent || (!m_side.empty() && m_side.front()->group <= m_sideAllowed));
}

// The batch in m_sideCurrent, from packet m_sideAt on: one packet at a time, until the main stream has work.
void GifArbiter::runSide()
{
    SideBatch &batch = *m_sideCurrent;
    // Issue 049, amendment v3.9: before a unit's first packet is read: may it start? (The answer comes from the
    // renderer: is there a place for the picture's read.) A unit refused is dropped batch by batch as they come up.
    if (batch.unit != 0u)
    {
        if (m_sideAt == 0u && batch.unit != m_sideAskedUnit)
        {
            m_sideAskedUnit = batch.unit;
            SideStartFn start;
            {
                std::lock_guard<std::mutex> lock(m_workerMutex);
                start = m_sideStartFn;
            }
            if (start && !start())
                m_sideRefusedUnit.store(batch.unit, std::memory_order_release);
        }
        if (batch.unit == m_sideRefusedUnit.load(std::memory_order_acquire))
        {
            if (batch.then)
            {
                batch.then(true);
                batch.then = nullptr;
            }
            m_sideUnitOpen = false; // nothing of it was read: no unit is half done
            m_sideCurrent.reset();
            m_sideAt = 0;
            return;
        }
    }
    const auto from = std::chrono::steady_clock::now();
    uint64_t packets = 0, bytes = 0;
    bool gaveWay = false;
    while (m_sideAt < batch.packets.size())
    {
        if (m_mainWaiting.load(std::memory_order_acquire))
        {
            gaveWay = true; // the game first
            break;
        }
        const SideBatch::Packet &p = batch.packets[m_sideAt++];
        if (m_sideProcessFn)
        {
            const bool timed = ps2x::hold::enabled(); // DW5XL-DEBUG PS2X_HOLD_TRACE
            const uint64_t packetFrom = timed ? ps2x::hold::nowNs() : 0u;
            g_gifArbiterCurrentPath = static_cast<int>(p.path);
            m_sideProcessFn(p.path, batch.bytes.data() + p.offset, p.size);
            g_gifArbiterCurrentPath = 0;
            if (timed)
                ps2x::hold::counters().packets[ps2x::hold::PictureGs].add(ps2x::hold::nowNs() - packetFrom);
        }
        ++packets;
        bytes += p.size;
    }
    if (m_sideAt >= batch.packets.size())
    {
        m_sideUnitOpen = !batch.then;
        m_sideUnitGroup = batch.group;
        if (batch.then)
        {
            batch.then(false);
            batch.then = nullptr;
        }
        m_sideCurrent.reset();
        m_sideAt = 0;
    }
    m_sidePackets.fetch_add(packets, std::memory_order_relaxed);
    m_sideBytes.fetch_add(bytes, std::memory_order_relaxed);
    m_sideNs.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - from).count()), std::memory_order_relaxed);
    m_sideGaveWay.fetch_add(gaveWay ? 1u : 0u, std::memory_order_relaxed);
}

// No GS worker: the waiting batches that are allowed, on the calling thread.
void GifArbiter::drainSideInline()
{
    std::lock_guard<std::recursive_mutex> turn(m_sideInline);
    for (;;)
    {
        {
            std::lock_guard<std::mutex> lock(m_workerMutex);
            if (m_sideCurrent || m_side.empty() || m_side.front()->group > m_sideAllowed)
                return;
            m_sideCurrent = std::move(m_side.front());
            m_side.pop_front();
            m_sideAt = 0;
        }
        runSide(); // m_mainWaiting is never set without a worker: the whole batch
    }
}

void GifArbiter::setSideProcessFn(SideProcessFn fn)
{
    std::lock_guard<std::mutex> lock(m_workerMutex);
    m_sideProcessFn = std::move(fn);
}

void GifArbiter::setSideStartFn(SideStartFn fn)
{
    std::lock_guard<std::mutex> lock(m_workerMutex);
    m_sideStartFn = std::move(fn);
}

bool GifArbiter::submitSide(std::shared_ptr<SideBatch> batch)
{
    if (!batch)
        return false;
    bool wake = false;
    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        if (batch->group < m_sideMinGroup)
            return false;
        if (batch->unit != 0u && batch->unit == m_sideRefusedUnit.load(std::memory_order_acquire))
            return false; // amendment v3.9: its unit was refused at its start
        wake = m_async && !m_workerBusy && !m_sideRunning; // a worker that may be waiting
        m_side.push_back(std::move(batch));
    }
    if (wake)
        m_workCv.notify_one();
    else if (!m_async)
        drainSideInline();
    return true;
}

bool GifArbiter::dropSideBefore(uint64_t group)
{
    std::vector<std::shared_ptr<SideBatch>> dropped;
    bool cut = false, wake = false;
    {
        std::unique_lock<std::recursive_mutex> turn(m_sideInline, std::defer_lock);
        if (!m_async)
            turn.lock();
        std::unique_lock<std::mutex> lock(m_workerMutex);
        // From another thread than the GS worker: not in the middle of a side packet (the worker starts no other
        // side packet while this waits).
        const bool other = m_async && std::this_thread::get_id() != m_worker.get_id();
        if (other)
        {
            ++m_sideHold;
            m_idleCv.wait(lock, [this] { return !m_sideRunning; });
            --m_sideHold;
        }
        if (group > m_sideMinGroup)
            m_sideMinGroup = group;
        if (m_sideCurrent && m_sideCurrent->group < group)
        {
            dropped.push_back(std::move(m_sideCurrent));
            m_sideCurrent.reset();
            m_sideAt = 0;
        }
        for (auto it = m_side.begin(); it != m_side.end();)
        {
            if ((*it)->group < group)
            {
                dropped.push_back(std::move(*it));
                it = m_side.erase(it);
            }
            else
            {
                ++it;
            }
        }
        cut = !dropped.empty() || (m_sideUnitOpen && m_sideUnitGroup < group);
        if (cut)
            m_sideUnitOpen = false;
        wake = other && sideReady();
    }
    if (wake)
        m_workCv.notify_one();
    for (std::shared_ptr<SideBatch> &batch : dropped)
    {
        if (batch->then)
        {
            batch->then(true);
            batch->then = nullptr;
        }
    }
    return cut;
}

void GifArbiter::allowSide(uint64_t group)
{
    bool wake = false;
    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        if (group > m_sideAllowed)
            m_sideAllowed = group;
        wake = m_async && !m_workerBusy && !m_sideRunning && sideReady();
    }
    if (wake)
        m_workCv.notify_one();
    else if (!m_async)
        drainSideInline();
}

GifArbiter::SideTotals GifArbiter::sideTotals() const
{
    SideTotals totals;
    totals.packets = m_sidePackets.load(std::memory_order_relaxed);
    totals.bytes = m_sideBytes.load(std::memory_order_relaxed);
    totals.ns = m_sideNs.load(std::memory_order_relaxed);
    totals.gaveWay = m_sideGaveWay.load(std::memory_order_relaxed);
    return totals;
}

void GifArbiter::submitCall(std::function<void()> call)
{
    if (!call)
        return;
    // Everything submitted so far goes to the GS first, in arbitration order.
    drain();
    if (!m_async)
    {
        call();
        return;
    }
    GifArbiterPacket pkt;
    pkt.call = std::move(call);
    m_outbox.push_back(std::move(pkt));
    flushOutbox();
}

void GifArbiter::flushOutbox()
{
    if (m_outbox.empty())
        return;
    bool wake = false;
    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        // A busy worker re-checks the queue before it waits, so only an idle one needs a wake-up.
        wake = !m_workerBusy && m_pending.empty();
        for (auto &pkt : m_outbox)
            m_pending.push_back(std::move(pkt));
        m_mainWaiting.store(true, std::memory_order_release); // issue 049 v3.7: side packets give way
        if (kGifStats) // DW5XL-DEBUG issue 025
            m_stats.pendingMax = std::max(m_stats.pendingMax, m_pending.size());
        if (m_spare.empty())
            m_spare.swap(m_returned);
    }
    if (wake)
        m_workCv.notify_one();
    m_outbox.clear();
    if (kGifStats) // DW5XL-DEBUG issue 025
    {
        ++m_stats.handovers;
        m_stats.wakes += wake ? 1u : 0u;
        statsTick();
    }
}

void GifArbiter::statsTick() // DW5XL-DEBUG issue 025: one [gifstats] line per second of host time
{
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
    if (m_statsWindowStart == 0)
        m_statsWindowStart = now;
    if (now - m_statsWindowStart < 1000)
        return;
    size_t pending = 0;
    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        pending = m_pending.size();
    }
    std::fprintf(stderr, "[gifstats] ms=%lld path1=%llu path2=%llu path3=%llu drains=%llu multi=%llu handovers=%llu "
                         "wakes=%llu spareMiss=%llu grow=%llu pendingMax=%zu pendingNow=%zu\n",
                 static_cast<long long>(now - m_statsWindowStart), (unsigned long long)m_stats.submits[1],
                 (unsigned long long)m_stats.submits[2], (unsigned long long)m_stats.submits[3],
                 (unsigned long long)m_stats.drains, (unsigned long long)m_stats.drainsMulti,
                 (unsigned long long)m_stats.handovers, (unsigned long long)m_stats.wakes,
                 (unsigned long long)m_stats.spareMisses, (unsigned long long)m_stats.grows, m_stats.pendingMax, pending);
    m_stats = {};
    m_statsWindowStart = now;
}

void GifArbiter::beginBatch()
{
    if (m_batchDepth++ == 0)
        m_batchThread = std::this_thread::get_id();
}

void GifArbiter::endBatch()
{
    if (m_batchDepth > 0 && --m_batchDepth == 0)
    {
        flushOutbox();
        m_batchThread = {};
    }
}

void GifArbiter::waitIdle()
{
    if (!m_async)
        return;
    if (std::this_thread::get_id() == m_batchThread)
        flushOutbox();
    std::unique_lock<std::mutex> lock(m_workerMutex);
    m_idleCv.wait(lock, [this] { return m_pending.empty() && !m_workerBusy; });
}

void GifArbiter::reset()
{
    m_queue.clear();
    m_outbox.clear();
    if (!m_async)
        return;
    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        m_pending.clear();
    }
    waitIdle();
}

bool GifArbiter::isImagePacket(const uint8_t *data, uint32_t sizeBytes)
{
    if (!data || sizeBytes < 16u)
        return false;

    uint64_t tagLo = 0;
    std::memcpy(&tagLo, data, sizeof(tagLo));
    const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
    return flg == 2u;
}

void GifArbiter::submit(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool path2DirectHl)
{
    if (!data || sizeBytes < 16 || !m_processFn)
        return;

    GifArbiterPacket pkt;
    pkt.pathId = pathId;
    pkt.path2DirectHl = (pathId == GifPathId::Path2) && path2DirectHl;
    pkt.path3Image = (pathId == GifPathId::Path3) && isImagePacket(data, sizeBytes);
    if (m_spare.empty() && m_async)
    {
        // Take back the buffers the GS worker has finished with before allocating a new one (issue 025 C3b).
        std::lock_guard<std::mutex> lock(m_workerMutex);
        m_spare.swap(m_returned);
    }
    if (!m_spare.empty())
    {
        pkt.data = std::move(m_spare.back());
        m_spare.pop_back();
    }
    if (kGifStats) // DW5XL-DEBUG issue 025
    {
        ++m_stats.submits[static_cast<int>(pathId) & 3];
        if (pkt.data.capacity() == 0u)
            ++m_stats.spareMisses;
        else if (pkt.data.capacity() < sizeBytes)
            ++m_stats.grows;
    }
    // A buffer that must grow gets room for a typical packet at least, so reused buffers rarely grow again (C3b).
    constexpr size_t kMinPacketCapacity = 16u * 1024u;
    if (pkt.data.capacity() < sizeBytes)
        pkt.data.reserve(std::max<size_t>(sizeBytes, kMinPacketCapacity));
    pkt.data.assign(data, data + sizeBytes);
    m_queue.push_back(std::move(pkt));
}

void GifArbiter::drain()
{
    if (!m_processFn)
        return;
    if (kGifStats) // DW5XL-DEBUG issue 025
    {
        ++m_stats.drains;
        m_stats.drainsMulti += m_queue.size() > 1u ? 1u : 0u;
    }

    // A single packet cannot change order: skip the sort (issue 025 C3b; the usual case is one VU1 packet).
    if (m_queue.size() > 1u)
    {
        std::stable_sort(m_queue.begin(), m_queue.end(),
                         [](const GifArbiterPacket &a, const GifArbiterPacket &b)
                         {
                             // DIRECTHL cannot preempt PATH3 IMAGE transfers.
                             if (a.path2DirectHl != b.path2DirectHl || a.path3Image != b.path3Image)
                             {
                                 if (a.path3Image && b.path2DirectHl)
                                     return true;
                                 if (a.path2DirectHl && b.path3Image)
                                     return false;
                             }
                             return pathPriority(a.pathId) < pathPriority(b.pathId);
                         });
    }

    if (m_async)
    {
        for (auto &pkt : m_queue)
            if (!pkt.data.empty())
                m_outbox.push_back(std::move(pkt));
        m_queue.clear();
        if (m_batchDepth == 0 || std::this_thread::get_id() != m_batchThread || m_outbox.size() >= kOutboxBatch)
            flushOutbox();
        return;
    }
    for (size_t i = 0; i < m_queue.size(); ++i)
    {
        auto &pkt = m_queue[i];
        if (!pkt.data.empty())
        {
            g_gifArbiterCurrentPath = static_cast<int>(pkt.pathId);
            m_processFn(pkt.data.data(), static_cast<uint32_t>(pkt.data.size()));
            g_gifArbiterCurrentPath = 0;
        }
        recycle(m_spare, std::move(pkt.data));
    }
    m_queue.clear();
}

void GifArbiter::recycle(std::vector<std::vector<uint8_t>> &pool, std::vector<uint8_t> &&buffer)
{
    // Bounded: a few hundred buffers cover a frame's packets; very large ones
    // (big texture uploads) are freed rather than kept.
    if (pool.size() < 512u && buffer.capacity() != 0u && buffer.capacity() <= (1u << 20))
    {
        buffer.clear();
        pool.push_back(std::move(buffer));
    }
}

uint8_t GifArbiter::pathPriority(GifPathId id)
{
    return static_cast<uint8_t>(id);
}
