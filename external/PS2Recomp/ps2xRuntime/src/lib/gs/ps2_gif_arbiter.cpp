#include "runtime/gs/ps2_gif_arbiter.h"
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
        {
            std::unique_lock<std::mutex> lock(m_workerMutex);
            recycle(m_returned, std::move(pkt.data));
            pkt.data = {};
            m_workerBusy = false;
            if (m_pending.empty())
                m_idleCv.notify_all();
            m_workCv.wait(lock, [this] { return m_stopWorker || !m_pending.empty(); });
            if (m_pending.empty())
                return;
            pkt = std::move(m_pending.front());
            m_pending.pop_front();
            m_workerBusy = true;
        }
        g_gifArbiterCurrentPath = static_cast<int>(pkt.pathId);
        m_processFn(pkt.data.data(), static_cast<uint32_t>(pkt.data.size()));
        g_gifArbiterCurrentPath = 0;
    }
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
