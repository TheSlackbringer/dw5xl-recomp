#ifndef PS2_GIF_ARBITER_H
#define PS2_GIF_ARBITER_H

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

enum class GifPathId : uint8_t
{
    Path1 = 1,
    Path2 = 2,
    Path3 = 3,
};

struct GifArbiterPacket
{
    GifPathId pathId;
    bool path2DirectHl = false;
    bool path3Image = false;
    std::vector<uint8_t> data;
};

// Arbitrates GIF PATH1/2/3 packets and hands them to the GS in arbitration order. By default the
// GS can process them on its own worker thread (the GS runs in parallel with VU1/VIF on the PS2;
// the runtime enables it unless PS2X_GS_ASYNC=0). Anything that observes GS state calls waitIdle() first.
class GifArbiter
{
public:
    using ProcessPacketFn = std::function<void(const uint8_t *, uint32_t)>;

    GifArbiter() = default;
    explicit GifArbiter(ProcessPacketFn processFn);
    ~GifArbiter();
    GifArbiter(const GifArbiter &) = delete;
    GifArbiter &operator=(const GifArbiter &) = delete;

    void setProcessPacketFn(ProcessPacketFn fn);
    // Process packets on a GS worker thread instead of inside drain(). Off by default.
    void setWorkerEnabled(bool enabled);
    static bool workerRequestedByEnv(); // PS2X_GS_ASYNC (default on; "0" = inline)

    void submit(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool path2DirectHl = false);

    void drain();
    // Between beginBatch() and endBatch() (one device transfer, on the calling
    // thread) drained packets collect in an outbox, in exactly the order the
    // per-packet drains produce, and reach the GS worker in groups instead of
    // one lock/wake-up per packet. endBatch(), waitIdle() on that thread and
    // every kOutboxBatch packets hand them over. Nesting is allowed.
    void beginBatch();
    void endBatch();
    // Blocks until every packet handed to the GS has been processed (no-op inline).
    void waitIdle();
    // Drop only input not yet emitted; completed GS draws remain intact.
    void reset();
    bool empty() const { return m_queue.empty() && m_outbox.empty(); }

private:
    ProcessPacketFn m_processFn;
    std::vector<GifArbiterPacket> m_queue;
    // Packet buffers are reused instead of allocated per packet: processed
    // buffers go to m_returned (worker, under m_workerMutex; inline drain
    // directly), drain() swaps them into m_spare, submit() takes from m_spare.
    std::vector<std::vector<uint8_t>> m_spare;
    static constexpr size_t kOutboxBatch = 32u;
    std::vector<GifArbiterPacket> m_outbox;
    int m_batchDepth = 0;
    std::thread::id m_batchThread;
    void flushOutbox();
    std::vector<std::vector<uint8_t>> m_returned;
    void recycle(std::vector<std::vector<uint8_t>> &pool, std::vector<uint8_t> &&buffer);

    // GS worker: FIFO of arbitrated packets.
    std::thread m_worker;
    std::mutex m_workerMutex;
    std::condition_variable m_workCv;
    std::condition_variable m_idleCv;
    std::deque<GifArbiterPacket> m_pending;
    bool m_workerBusy = false;
    bool m_stopWorker = false;
    bool m_async = false;

    void startWorker();
    void stopWorker();
    void workerLoop();

    static bool isImagePacket(const uint8_t *data, uint32_t sizeBytes);
    static uint8_t pathPriority(GifPathId id);
};

#endif
