#ifndef PS2_GIF_ARBITER_H
#define PS2_GIF_ARBITER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
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
    // Set for an ordered call (submitCall) instead of a GIF packet.
    std::function<void()> call;
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
    // Runs `call` on the GS side after every packet submitted before it and before any later one (issue 023: the
    // end-of-field scanout). Same producer thread as submit(); inline when there is no GS worker.
    void submitCall(std::function<void()> call);
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

    // DW5XL (issue 049, amendment v3.7, change 3: "the game first"): a second stream to the GS worker, for the
    // in-between pictures' device. Side batches are processed in the order they were submitted, but only while
    // nothing of the main stream is waiting: between two side packets the main stream's packets and calls go
    // first. A batch belongs to a group (the pictures' frame); a group's batches are processed only once
    // allowSide() named it. Without a GS worker side batches are processed where they are submitted or allowed.
    struct SideBatch
    {
        struct Packet { GifPathId path; uint32_t offset, size; };
        uint64_t group = 0;
        uint64_t unit = 0;           // the unit (one picture) it belongs to, a running number; 0: not said
        std::vector<uint8_t> bytes;  // the packets, one after the other
        std::vector<Packet> packets;
        // The last batch of its unit (one picture) has one. Called once: on the GS side right after the batch's
        // packets (dropped = false), or where the batch is dropped (dropped = true). It must not call the arbiter.
        std::function<void(bool dropped)> then;
    };
    using SideProcessFn = std::function<void(GifPathId, const uint8_t *, uint32_t)>;
    void setSideProcessFn(SideProcessFn fn); // before the first batch
    // Any thread (one producer). False: the batch's group was dropped already; `then` is not called.
    bool submitSide(std::shared_ptr<SideBatch> batch);
    // Issue 049, amendment v3.9: asked on the GS side right before the first packet of a unit would be read (only
    // for batches that say their unit). False: the unit is refused. Nothing of it is read; its batches are dropped
    // as they come up (its last one's `then` says dropped) and later ones are refused at submitSide.
    using SideStartFn = std::function<bool()>;
    void setSideStartFn(SideStartFn fn);
    // Any thread; inside a call of the main stream it takes effect at that place in the stream. Side work of
    // groups below `group` that is waiting or half done is dropped, and later batches of those groups are
    // refused. True when a unit was cut off: a batch of it was dropped, or its last batch had not come yet.
    bool dropSideBefore(uint64_t group);
    // Batches of groups up to `group` may be processed from now on (same threads as dropSideBefore).
    void allowSide(uint64_t group);
    struct SideTotals
    {
        uint64_t packets = 0, bytes = 0;
        uint64_t ns = 0;      // processing them
        uint64_t gaveWay = 0; // times the main stream went first in the middle of a batch
    };
    SideTotals sideTotals() const;

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

    // The side stream (issue 049 v3.7). Under m_workerMutex, except m_sideCurrent, m_sideAt and the open unit while
    // m_sideRunning is set: they are then the GS worker's alone (runSide).
    SideProcessFn m_sideProcessFn;
    SideStartFn m_sideStartFn;
    uint64_t m_sideAskedUnit = 0;                    // the unit asked about last (the GS worker's alone)
    std::atomic<uint64_t> m_sideRefusedUnit{0};      // the unit refused last: its batches are dropped and refused
    std::deque<std::shared_ptr<SideBatch>> m_side;
    std::shared_ptr<SideBatch> m_sideCurrent; // taken from m_side, m_sideAt packets of it are processed
    size_t m_sideAt = 0;
    bool m_sideRunning = false;               // the GS worker is processing side packets
    int m_sideHold = 0;                       // dropSideBefore() on another thread waits for the worker to pause
    bool m_sideUnitOpen = false;              // the last batch processed was not its unit's last
    uint64_t m_sideUnitGroup = 0;
    uint64_t m_sideMinGroup = 0;              // batches of groups below are refused
    uint64_t m_sideAllowed = 0;               // batches of groups up to this one may be processed
    std::atomic<bool> m_mainWaiting{false};   // the main stream has work for the GS worker (set under the lock)
    std::recursive_mutex m_sideInline;        // no GS worker: one thread at a time processes or drops side batches
    std::atomic<uint64_t> m_sidePackets{0}, m_sideBytes{0}, m_sideNs{0}, m_sideGaveWay{0};
    bool sideReady() const;                   // under m_workerMutex
    void runSide();
    void drainSideInline();

    static bool isImagePacket(const uint8_t *data, uint32_t sizeBytes);
    static uint8_t pathPriority(GifPathId id);

    // DW5XL-DEBUG issue 025 (C3b step 1): PS2X_GIF_STATS=1 prints per-second counts of the per-packet work
    // ([gifstats]); producer-side counters, read once at start, no effect when off.
    struct Stats
    {
        uint64_t submits[4] = {};
        uint64_t drains = 0, drainsMulti = 0, handovers = 0, wakes = 0, spareMisses = 0, grows = 0;
        size_t pendingMax = 0;
    };
    Stats m_stats;
    int64_t m_statsWindowStart = 0;
    void statsTick();
};

#endif
