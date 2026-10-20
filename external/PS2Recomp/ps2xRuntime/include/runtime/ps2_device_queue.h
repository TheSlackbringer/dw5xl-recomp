#ifndef PS2_DEVICE_QUEUE_H
#define PS2_DEVICE_QUEUE_H

// Ordered device command queue for the EE -> VIF1 -> VU1 -> GIF -> GS pipeline.
//
// The EE side keeps the DMAC: chain walks, kick-time payload snapshots, the
// channel registers (m_ioRegisters) and interrupts. Everything downstream of the
// DMAC -- the VIF1 parser state (vif1_regs, MSKPATH3/PATH3 FIFO, PATH2 IMAGE
// continuation), VU1 (memory, decode cache, MSCAL/MSCNT), the GIF arbiter and
// GS frontend submission -- is "device owned" and is driven by commands consumed
// strictly in EE program order:
//   * Transfer     a VIF1 and/or GIF (PATH3) DMA kick, payloads copied at kick time
//   * GsRegWrites  direct GS register writes issued by HLE stubs
//   * GsCall       other GS frontend operations issued by HLE stubs
// VIF1/GIF reset and FIFO recovery writes execute as ordered worker controls.
// DMA Pause/Restart observe owner-latched channel state without waiting for
// downstream VU/GS work; canceled input tails and completion generations are
// tracked separately from accepted pipeline work.
//
// Executors: with PS2X_VU1_ASYNC=0 commands execute inline on the submitting
// thread exactly as the old synchronous code did. Otherwise (default) one device
// worker thread consumes them. The worker never touches m_ioRegisters; it
// publishes a completion sequence and the EE applies DMA completion (CHCR.STR
// clear, QWC=0, D_STAT.CIS, completion cause) when it observes device state
// ("drain on observation"), so the guest sees what the synchronous path showed.
//
// The header is deliberately not included by ps2_runtime.h (generated-code PCH).

#include <chrono>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

class PS2Memory;

namespace ps2x::device
{
    enum class DrainReason : uint8_t
    {
        Dmac1Reg,       // D1 (VIF1 channel) register read/write
        Dmac2Reg,       // D2 (GIF channel) register read/write
        DStat,          // D_STAT read/write
        Vif1Reg,        // VIF1 registers 0x10003C00-0x10003DFF
        Vif1Fifo,       // VIF1_FIFO 0x10005000
        GifReg,         // GIF_CTRL/GIF_MODE/GIF_STAT/... 0x10003000-0x100030FF
        GsPrivReg,      // GS privileged registers other than owner-latched CSR
        Vu1Memory,      // EE access to VU1 micro/data memory
        StoreImage,     // GS -> host readback
        GsDisplayEnv,   // stub writes to GS display registers
        NativeGifChain, // native GIF chain fast paths that drive the GS inline
        DirectPath,     // EE-direct GIF/VIF entry points outside the queue
        Reconfigure,    // memory/runtime re-initialisation
        Shutdown,
        Explicit,
        Count
    };

    const char *drainReasonName(DrainReason reason) noexcept;

    struct Config
    {
        // false: synchronous executor (commands run inline at submit).
        bool async = false;
        // PS2X_VU1_ASYNC_TRACE: drain reasons with caller RA, worker busy/idle.
        bool trace = false;
        // GsRegWrites sink; runs on the executing thread (worker in async mode).
        std::function<void(uint8_t reg, uint64_t value)> gsWriteRegister;
        // Worker-owned register/FIFO recovery; these callbacks use the original
        // memory handlers while executingCommand() is true.
        std::function<void(uint32_t address, uint32_t value)> controlWrite;
        std::function<void(uint32_t address, uint64_t lo, uint64_t hi)> controlQwordWrite;
        std::function<void()> resetVu1;
        // EE thread, at submit: FBRST of the kicking context (VU1 D/T enables).
        std::function<uint32_t()> currentVu0Fbrst;
        // EE thread, when a completed Transfer that ran MSCAL/MSCNT is applied:
        // the VPU-STAT VU1 D/T stop bits (0x0200/0x0400) the run left behind.
        std::function<void(uint32_t stopBits)> applyVu1StopBits;
        // EE thread: guest return address of the observing code (trace only).
        std::function<uint32_t()> callerRa;
        // Worker thread, called without the queue lock right after the last
        // submitted command completed (queue idle). Used to wake guest threads
        // that wait in the scheduler for the device to finish.
        std::function<void()> workerIdle;
        // EE thread, device idle: copy up to maxBytes of pending GS local->host
        // transfer output (TRXDIR=1) into dst; returns bytes copied.
        std::function<uint32_t(uint8_t *dst, uint32_t maxBytes)> localToHost;
    };

    struct Stats
    {
        uint64_t submitted = 0;
        uint64_t completed = 0;
        uint64_t applied = 0;
        uint64_t drains = 0;         // drains that had un-applied work
        uint64_t blockingDrains = 0; // drains that had to wait for the worker
        uint64_t drainWaitNs = 0;
        uint64_t workerBusyNs = 0;
        uint64_t workerIdleNs = 0;
        bool async = false;
    };

    // Kick-time snapshot of the GIF (PATH3) and VIF1 transfers of one
    // processPendingTransfers() call, chunked exactly as the old code chunked
    // its submitGifPacket()/processVIF1Data() calls.
    struct TransferSnapshot
    {
        std::vector<std::vector<uint8_t>> gifChunks;
        std::vector<std::vector<uint8_t>> vif1Chunks;
        bool hadGif = false;
        bool hadVif1 = false;
    };

    using GsRegWrite = std::pair<uint8_t, uint64_t>;

    // PS2X_VU1_ASYNC unset or != "0" -> true. PS2X_VU1_ASYNC_TRACE=1 -> trace.
    bool asyncRequestedByEnv() noexcept;
    bool traceRequestedByEnv() noexcept;

    // Lifecycle. attach() replaces an existing device for this memory (the old
    // one is drained and joined first). detach() drains (owner thread) or waits
    // for the worker to go idle (other threads), then joins it.
    void attach(PS2Memory &mem, Config config);
    void detach(PS2Memory &mem) noexcept;
    bool isAttached(const PS2Memory &mem) noexcept;
    bool isAsync(const PS2Memory &mem) noexcept;

    // True while the calling thread executes a device command (worker, or the
    // synchronous executor). EE-direct entry points use it to avoid draining
    // from inside the device.
    bool executingCommand() noexcept;
    // Async worker only: VU1 MSCAL/MSCNT state for the command being executed.
    bool onDeviceWorker() noexcept;
    uint32_t workerVu0Fbrst() noexcept;
    void workerRecordVu1StopBits(uint32_t stopBits) noexcept;
    // consumedBytes is the accepted prefix of the current VIF chunk. Call at
    // safe parser boundaries. Pause preserves the remaining input; reset drops
    // it. The private signal is caught by the queue, never a guest exception.
    void workerCheckpointVif1Input(uint32_t consumedBytes);
    // Pause NEVER cancels an already accepted MSCAL. Only hard VU1 reset does.
    bool workerShouldCancelVu1() noexcept;

    // Game frames: fields in which the game finished at least one frame (an
    // explicit render-sync drain that had device work). Per second = the FPS a
    // screen shows; counts 3D frames (two syncs per frame), movie frames (one)
    // and never more than one per field (F2 FPS overlay, issue 014).
    uint64_t gameFrameCount() noexcept;

    // PS2X_DEVICE_PROFILE=1 (with PS2X_VU1_ASYNC_TRACE=1): device-worker time per
    // part, printed with each [vu1-async] line. Parts nest: Transfer contains
    // the VU1 runs it starts, a VU1 run contains its XGKICK GIF submits;
    // VifUnpack is the UNPACK decoding inside a transfer.
    enum class ProfilePart : uint8_t { Transfer, Vu1, GifSubmit, VifUnpack, Count };
    bool profileEnabled() noexcept;
    void profileAdd(ProfilePart part, uint64_t ns) noexcept;
    struct ProfileScope
    {
        explicit ProfileScope(ProfilePart part) noexcept;
        ~ProfileScope();
        ProfileScope(const ProfileScope &) = delete;
        ProfileScope &operator=(const ProfileScope &) = delete;
        ProfilePart part;
        int64_t start;
    };

    // EE owner, nonblocking; false selects the existing unsupported/sync path.
    bool pauseDmaChannel(PS2Memory &mem, uint32_t channelBase, uint32_t &oldChcr);
    bool restartDmaChannel(PS2Memory &mem, uint32_t channelBase, uint32_t savedChcr, uint32_t &previousStr);
    bool syncDmaChannel(PS2Memory &mem, uint32_t channelBase);
    // Owner marks newly programmed latches before observation can apply an old
    // completion; CHCR and QWC are protected independently.
    bool noteDmaRegisterWrite(PS2Memory &mem, uint32_t address);
    bool submitControlWrite(PS2Memory &mem, uint32_t address, uint32_t value);
    bool submitControlQword(PS2Memory &mem, uint32_t address, uint64_t lo, uint64_t hi);
    bool submitVu1Reset(PS2Memory &mem, std::function<void()> resetCallback);
    bool submitVu1Reset(PS2Memory &mem);

    // EE side (no-ops without an async device, from non-owner threads, and
    // inside the device). observeIo classifies a physical IO/GS/VU address.
    void observeIo(PS2Memory &mem, uint32_t physAddr);
    void observeVu1Memory(PS2Memory &mem);
    void drainAll(PS2Memory &mem, DrainReason reason);
    // VIF1 DIR=0 (GS -> memory) readback source: drains the device, then reads
    // the GS local->host transfer output. Returns bytes copied (0 if none).
    uint32_t readLocalToHost(PS2Memory &mem, uint8_t *dst, uint32_t maxBytes);
    // Non-blocking: apply completions the worker has already published.
    // Never throws; worker exceptions surface at the next drain.
    bool pollCompletions(PS2Memory &mem) noexcept;
    // A VIF1/GIF kick on this channel whose completion is not applied yet.
    bool hasDeferredChannel(const PS2Memory &mem, uint32_t channelBase) noexcept;
    // Scheduler wait type/token for guest threads waiting until the device is idle.
    inline constexpr uint32_t kDeviceIdleWaitType = 0x44564944u; // 'DVID'
    inline constexpr uint64_t kDeviceIdleWaitToken = 0u;
    // Any thread: wait until the worker executed everything submitted (no apply).
    void quiesce(PS2Memory &mem, std::chrono::milliseconds timeout) noexcept;
    void quiesceAll(std::chrono::milliseconds timeout) noexcept;
    // Memory re-initialisation: wait for the worker, drop pending completions.
    void resetForReinitialize(PS2Memory &mem) noexcept;
    Stats stats(const PS2Memory &mem) noexcept;

    // Submission. Each returns false when no device is attached; the caller then
    // runs its legacy inline path.
    // hadVif0: the VIF0 transfers of the same processPendingTransfers() call were
    // processed inline; their completion is applied in the legacy order.
    bool submitTransfers(PS2Memory &mem, TransferSnapshot &&snapshot, bool hadVif0);
    bool submitGsRegWrites(PS2Memory &mem, std::vector<GsRegWrite> writes);
    bool submitGsCall(PS2Memory &mem, std::function<void()> call);
}

#endif // PS2_DEVICE_QUEUE_H
