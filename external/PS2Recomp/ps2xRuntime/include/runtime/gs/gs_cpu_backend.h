#pragma once

#include "runtime/gs/field_history.h"
#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_texture_page_cache.h"

#include <array>
#include <atomic>
#include <bitset>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class GSCpuBackend final : public GSRasterBackend
{
public:
    GSCpuBackend();
    ~GSCpuBackend() override;

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    void Submit(const GSPrimitiveBatch &batch) override;
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override;
    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;

    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    PresentationFrame Present(const GSPresentationRequest &request) override;
    // Convert an already synchronized memory image without CPU raster jobs.
    PresentationFrame PresentFromLocalMemory(const GSPresentationRequest &request, ReusableFieldWeaver *fieldHistory = nullptr);

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;

private:
    ReusableFieldWeaver m_fieldWeaver;

    // Rasterization runs asynchronously on a pool of worker threads. The
    // caller (the GS frontend, on the EE thread) only appends commands to a
    // batch; filled batches queue up and the workers run them in order. Every
    // worker replays the whole batch but only touches the screen rows of its
    // own stripes, so each pixel is written by one thread in submission
    // order. Uploads, CLUT loads and TEXFLUSH are commands too, so the caller
    // never touches VRAM while workers run; readback and local transfers
    // wait for the pool to drain, presentation queues a VRAM snapshot.
    // Draws that depend on memory another stripe may be writing are
    // separated by a barrier, tracked per 8 KiB page.
    struct RasterWorker
    {
        std::array<uint16_t, 512> clut{};
        GSMem::TexturePageCache textureCache;
        uint32_t band = 0;
        uint32_t bandCount = 1;

        bool ownsRow(int y) const
        {
            return bandCount <= 1u || ((static_cast<uint32_t>(y) >> 3u) % bandCount) == band;
        }
    };

    // One bit per 8 KiB GS page.
    using PageSet = std::bitset<512>;

    enum class CommandKind : uint8_t
    {
        Draw,
        LoadClut,
        TextureFlush,
        Upload,
        Barrier,
        // A draw sampling memory it renders to: one worker draws all rows,
        // in order, between barriers.
        SerialDraw,
        // Copy of all of VRAM for presentation, between barriers.
        Snapshot,
    };

    struct Command
    {
        CommandKind kind = CommandKind::Draw;
        // Host FP rounding mode at submission: PATH1 draws are kicked from
        // inside the VU1 interpreter, which runs with FE_TOWARDZERO.
        int roundingMode = 0;
        uint32_t index = 0;
    };

    struct ClutLoad
    {
        GSTex0Reg tex0{};
        GSTexClutReg texclut{};
    };

    struct ImageUpload
    {
        GSTransferCommand transfer{};
        GSTransferSnapshot stateBefore{};
        uint32_t dataOffset = 0;
        uint32_t dataSize = 0;
    };

    struct Batch
    {
        std::vector<Command> commands;
        std::vector<GSPrimitiveBatch> draws;
        std::vector<ClutLoad> clutLoads;
        std::vector<ImageUpload> uploads;
        std::vector<uint8_t> uploadData;
        std::vector<std::vector<uint8_t> *> snapshots;

        bool empty() const { return commands.empty(); }
        void clear()
        {
            commands.clear();
            draws.clear();
            clutLoads.clear();
            uploads.clear();
            uploadData.clear();
            snapshots.clear();
        }
    };

    struct PendingTarget
    {
        uint64_t key = 0;
        PageSet pages;
    };

    // Lets workers of one batch wait for each other at Barrier/Upload.
    class SpinBarrier
    {
    public:
        void setCount(uint32_t count) { m_count = count; }
        // Returns true on the thread that completed the phase.
        bool arriveAndWait();

    private:
        uint32_t m_count = 1;
        std::atomic<uint32_t> m_arrived{0};
        std::atomic<uint32_t> m_phase{0};
    };

    void EnsureWorkers();
    void WorkerLoop(uint32_t index);
    void ExecuteBatch(RasterWorker &worker, const Batch &batch);
    void PushCommand(CommandKind kind, uint32_t index);
    void PushBarrier();
    Batch &FillBatch() { return m_batches[m_submitted % kQueueDepth]; }
    void KickIfIdle(bool force);
    void WaitForCompleted(uint64_t sequence);
    void WaitForInflight();
    void SyncUnlocked();

    void ResetUnlocked();
    void LoadClutUnlocked(RasterWorker &worker, const GSTex0Reg &tex0, const GSTexClutReg &texclut);
    void UploadImageQueued(const uint8_t *data, uint32_t sizeBytes);
    void ApplyImageUpload(const GSTransferCommand &transfer, GSTransferSnapshot &state,
                          const uint8_t *data, uint32_t sizeBytes, bool writeVram);
    uint32_t ReadVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const;
    void WriteVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value);

    void DrawPrimitive(RasterWorker &worker, const GSPrimitiveBatch &batch);
    void DrawSprite(RasterWorker &worker, const GSPrimitiveBatch &batch);
    void DrawTriangle(RasterWorker &worker, const GSPrimitiveBatch &batch);
    void DrawLine(RasterWorker &worker, const GSPrimitiveBatch &batch);
    void WritePixel(RasterWorker &worker, const GSDrawState &state, int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint8_t fog);
    uint32_t SampleTexture(RasterWorker &worker, const GSDrawState &state, float s, float t, float q, uint16_t u, uint16_t v);
    uint32_t LookupCLUT(const RasterWorker &worker, const GSDrawState &state, uint8_t index, uint8_t cpsm, uint8_t csm, uint8_t csa, uint8_t sourcePsm);

    void PerformLocalToLocalTransfer();
    void PerformLocalToHostTransfer();
    bool CopyFrameToHostRgba(const GSFrameReg &frame,
                             uint32_t width,
                             uint32_t height,
                             std::vector<uint8_t> &outPixels,
                             bool preserveAlpha,
                             bool useLocalMemoryLayout,
                             bool frameBaseIsPages,
                             uint32_t sourceOriginX,
                             uint32_t sourceOriginY) const;

    using WriteVramFunc = void (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    using ReadVramFunc = uint32_t (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t);

    static constexpr size_t kPsmHandlerCount = 1u << 6u;
    mutable std::mutex m_mutex;
    uint8_t *m_vram = nullptr;
    uint32_t m_vramSize = 0;
    std::array<ReadVramFunc, kPsmHandlerCount> m_readVramFuncs{};
    std::array<WriteVramFunc, kPsmHandlerCount> m_writeVramFuncs{};
    // CLD compare registers (CBP0/CBP1), evaluated when TEX0 is written.
    std::array<uint32_t, 2> m_clutCbp{};

    GSTransferCommand m_transfer{};
    GSTransferSnapshot m_transferState{};
    std::vector<uint8_t> m_localToHostBuffer;
    size_t m_localToHostReadPos = 0;

    // Ring of batches: m_submitted batches have been handed to the workers
    // (the next one is filling), m_completed of them are done.
    static constexpr uint64_t kQueueDepth = 4u;
    std::array<Batch, kQueueDepth> m_batches;
    uint64_t m_submitted = 0;
    std::atomic<uint64_t> m_completed{0};
    // Hazard tracking for the filling batch since its last barrier.
    PageSet m_segmentReads;
    PageSet m_segmentWrites;
    std::vector<PendingTarget> m_segmentTargets;

    std::vector<std::unique_ptr<RasterWorker>> m_workers;
    std::vector<std::thread> m_workerThreads;
    SpinBarrier m_barrier;
    std::mutex m_poolMutex;
    std::condition_variable m_poolWake;
    std::condition_variable m_poolDone;
    uint64_t m_published = 0; // m_submitted as seen by workers (m_poolMutex)
    bool m_poolStop = false;
};
