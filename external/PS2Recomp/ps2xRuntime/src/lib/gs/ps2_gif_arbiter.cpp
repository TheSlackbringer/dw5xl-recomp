#include "runtime/gs/ps2_gif_arbiter.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

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
    m_worker = std::thread([this] { workerLoop(); });
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
    for (;;)
    {
        GifArbiterPacket pkt;
        {
            std::unique_lock<std::mutex> lock(m_workerMutex);
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

void GifArbiter::waitIdle()
{
    if (!m_async)
        return;
    std::unique_lock<std::mutex> lock(m_workerMutex);
    m_idleCv.wait(lock, [this] { return m_pending.empty() && !m_workerBusy; });
}

void GifArbiter::reset()
{
    m_queue.clear();
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
    pkt.data.resize(sizeBytes);
    std::memcpy(pkt.data.data(), data, sizeBytes);
    m_queue.push_back(std::move(pkt));
}

void GifArbiter::drain()
{
    if (!m_processFn)
        return;

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

    if (m_async)
    {
        {
            std::lock_guard<std::mutex> lock(m_workerMutex);
            for (auto &pkt : m_queue)
                if (!pkt.data.empty())
                    m_pending.push_back(std::move(pkt));
        }
        m_workCv.notify_one();
        m_queue.clear();
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
    }
    m_queue.clear();
}

uint8_t GifArbiter::pathPriority(GifPathId id)
{
    return static_cast<uint8_t>(id);
}
