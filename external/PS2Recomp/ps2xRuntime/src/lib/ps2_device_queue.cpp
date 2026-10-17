#include "runtime/ps2_device_queue.h"

#include "runtime/ps2_memory.h"
#include "ThreadNaming.h"

#include <array>
#include <atomic>
#include <cfenv>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace ps2x::device
{
    namespace
    {
        constexpr uint32_t kVif0Channel = 0x10008000u;
        constexpr uint32_t kVif1Channel = 0x10009000u;
        constexpr uint32_t kGifChannel = 0x1000A000u;
        constexpr uint32_t kDStat = 0x1000E010u;

        using Clock = std::chrono::steady_clock;
        class DeviceQueue;
        struct InputStopped {};

        uint64_t nsSince(Clock::time_point start)
        {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
        }

        struct Command
        {
            enum class Kind : uint8_t
            {
                Transfer,
                GsRegWrites,
                GsCall,
                Resume,
            };

            Kind kind = Kind::Transfer;
            uint64_t seq = 0;
            TransferSnapshot transfer;
            std::vector<GsRegWrite> gsRegs;
            std::function<void()> call;
            uint32_t vu0Fbrst = 0u;
            // EE floating-point environment at submit (rounding mode, MXCSR). The
            // worker runs the command under it, so DIRECT/PATH3 packets reach the
            // GS with the EE's mode and XGKICK packets with VU1 run()'s
            // round-toward-zero, exactly as on the synchronous path.
            std::fenv_t fenv{};

            // Written by the executing thread; read by the EE after the
            // completion sequence covering this command was published.
            bool vu1StopValid = false;
            uint32_t vu1StopBits = 0u;
            std::exception_ptr error;
            DeviceQueue *device = nullptr;
            uint64_t gifGeneration = 0, vifGeneration = 0;
            uint64_t gifQwcGeneration = 0, vifQwcGeneration = 0;
            uint64_t gifCancel = 0, vifCancel = 0;
            uint64_t gifEpoch = 0, vifEpoch = 0, vuEpoch = 0;
            uint32_t vifAccepted = 0;
            bool completedGif = false, completedVif = false;
            uint32_t resumeChannel = 0;
        };

        thread_local Command *t_workerCommand = nullptr;
        thread_local int t_executingDepth = 0;
        std::array<std::atomic<uint64_t>, static_cast<size_t>(ProfilePart::Count)> g_profileNs{};
        std::atomic<uint64_t> g_gameFrames{0};
        std::atomic<uint64_t> g_lastFrameField{~0ull};

        struct ExecutingScope
        {
            explicit ExecutingScope(Command *workerCommand) : previous(t_workerCommand)
            {
                ++t_executingDepth;
                t_workerCommand = workerCommand;
            }
            ~ExecutingScope()
            {
                t_workerCommand = previous;
                --t_executingDepth;
            }
            Command *previous;
        };

        // Legacy processPendingTransfers() completion tail, channel order GIF, VIF0, VIF1.
        void applyTransferCompletion(PS2Memory &mem, bool hadGif, bool hadVif0, bool hadVif1,
            bool clearGif = true, bool clearVif = true, bool clearGifQwc = true, bool clearVifQwc = true)
        {
            auto raiseDStatChannel = [&](uint32_t channelBit)
            {
                uint32_t dstat = mem.m_ioRegisters.count(kDStat) ? mem.m_ioRegisters[kDStat] : 0u;
                dstat |= (1u << channelBit);

                const uint32_t status = dstat & 0x3FFu;
                const uint32_t mask = (dstat >> 16) & 0x3FFu;
                if ((status & mask) != 0u)
                    dstat |= (1u << 31);
                else
                    dstat &= ~(1u << 31);

                mem.m_ioRegisters[kDStat] = dstat;
            };

            if (hadGif)
            {
                raiseDStatChannel(2u);
                mem.queueCompletedDmacCause(2u);
                if (clearGif)
                    mem.m_ioRegisters[kGifChannel + 0x00] &= ~0x100u;
                if (clearGifQwc)
                    mem.m_ioRegisters[kGifChannel + 0x20] = 0;
            }
            if (hadVif0)
            {
                raiseDStatChannel(0u);
                mem.queueCompletedDmacCause(0u);
                mem.m_ioRegisters[kVif0Channel + 0x00] &= ~0x100u;
                mem.m_ioRegisters[kVif0Channel + 0x20] = 0;
            }
            if (hadVif1)
            {
                raiseDStatChannel(1u);
                mem.queueCompletedDmacCause(1u);
                if (clearVif)
                    mem.m_ioRegisters[kVif1Channel + 0x00] &= ~0x100u;
                if (clearVifQwc)
                    mem.m_ioRegisters[kVif1Channel + 0x20] = 0;
            }
        }

        class DeviceQueue
        {
        public:
            DeviceQueue(PS2Memory &memory, Config configuration)
                : mem(memory), cfg(std::move(configuration))
            {
                traceWindowStart = Clock::now();
            }

            ~DeviceQueue()
            {
                stopWorker();
            }

            PS2Memory &mem;
            Config cfg;

            struct Channel
            {
                std::atomic<uint64_t> generation{0}, qwcGeneration{0}, cancel{0}, epoch{0};
                std::atomic<bool> paused{false};
            };
            Channel gifControl, vifControl;
            std::atomic<uint64_t> vuEpoch{0};

            Channel *channel(uint32_t base)
            {
                return base == kVif1Channel ? &vifControl : base == kGifChannel ? &gifControl : nullptr;
            }

            bool pause(uint32_t base, uint32_t &oldChcr)
            {
                Channel *c = channel(base);
                if (!cfg.async || !isOwner() || t_executingDepth != 0 || !c)
                    return false;
                for (;;)
                {
                    poll();
                    rethrowPending();
                    std::unique_lock<std::mutex> lock(mutex);
                    // Close publication-vs-pause race: an already published
                    // completion must be applied before taking old CHCR.
                    if (completed.load(std::memory_order_acquire) > applied.load(std::memory_order_relaxed))
                        continue;
                    oldChcr = mem.m_ioRegisters[base];
                    c->paused.store(true, std::memory_order_release);
                    c->cancel.fetch_add(1, std::memory_order_acq_rel);
                    c->generation.fetch_add(1, std::memory_order_acq_rel);
                    mem.m_ioRegisters[base] = oldChcr & ~0x100u;
                    return true;
                }
            }

            bool restart(uint32_t base, uint32_t saved, uint32_t &previousStr)
            {
                Channel *c = channel(base);
                if (!cfg.async || !isOwner() || t_executingDepth != 0 || !c)
                    return false;
                poll();
                rethrowPending();
                previousStr = (mem.m_ioRegisters[base] >> 8) & 1u;
                mem.m_ioRegisters[base] = saved;
                if ((saved & 0x100u) == 0)
                    return true;
                c->paused.store(false, std::memory_order_release);
                auto command = std::make_unique<Command>();
                command->kind = Command::Kind::Resume;
                command->resumeChannel = base;
                command->transfer.hadVif1 = base == kVif1Channel;
                command->transfer.hadGif = base == kGifChannel;
                submit(std::move(command));
                return true;
            }

            void invalidateVifInput()
            {
                vifControl.epoch.fetch_add(1, std::memory_order_acq_rel);
                vifControl.cancel.fetch_add(1, std::memory_order_acq_rel);
                vifControl.generation.fetch_add(1, std::memory_order_acq_rel);
                vifControl.paused.store(false, std::memory_order_release);
            }

            void invalidateGifInput()
            {
                gifControl.epoch.fetch_add(1, std::memory_order_acq_rel);
                gifControl.cancel.fetch_add(1, std::memory_order_acq_rel);
                gifControl.generation.fetch_add(1, std::memory_order_acq_rel);
                gifControl.paused.store(false, std::memory_order_release);
            }

            void checkpoint(Command &command, uint32_t consumed)
            {
                command.vifAccepted = consumed;
                if (command.vifEpoch != vifControl.epoch.load(std::memory_order_acquire) ||
                    command.vifCancel != vifControl.cancel.load(std::memory_order_acquire) ||
                    vifControl.paused.load(std::memory_order_acquire))
                    throw InputStopped{};
            }

            void applyControlWrite(uint32_t address, uint32_t value)
            {
                if (address == 0x10003C10u && (value & 1u))
                    retainedVif.clear();
                if (address == 0x10003000u && (value & 1u))
                    retainedGif.clear();
                cfg.controlWrite(address, value);
            }

            void prepareControl()
            {
                poll();
                rethrowPending();
            }

            // ---- submission (EE) -------------------------------------------------
            void submit(std::unique_ptr<Command> command)
            {
                if (cfg.async && isOwner() && t_executingDepth == 0)
                {
                    poll();
                    rethrowPending();
                }
                command->device = this;
                command->vuEpoch = vuEpoch.load(std::memory_order_acquire);
                auto prepare = [&](Channel &c, bool present, uint64_t &generation, uint64_t &cancel, uint64_t &epoch)
                {
                    if (present)
                    {
                        if (command->kind == Command::Kind::Transfer && c.paused.exchange(false, std::memory_order_acq_rel))
                            c.epoch.fetch_add(1, std::memory_order_acq_rel); // new kick replaces paused chain
                        generation = c.generation.fetch_add(1, std::memory_order_acq_rel) + 1;
                    }
                    cancel = c.cancel.load(std::memory_order_acquire);
                    epoch = c.epoch.load(std::memory_order_acquire);
                };
                prepare(gifControl, command->transfer.hadGif, command->gifGeneration, command->gifCancel, command->gifEpoch);
                prepare(vifControl, command->transfer.hadVif1, command->vifGeneration, command->vifCancel, command->vifEpoch);
                command->gifQwcGeneration = gifControl.qwcGeneration.load(std::memory_order_acquire);
                command->vifQwcGeneration = vifControl.qwcGeneration.load(std::memory_order_acquire);
                if (!cfg.async)
                {
                    command->seq = submitted.load(std::memory_order_relaxed) + 1u;
                    submitted.store(command->seq, std::memory_order_relaxed);
                    {
                        ExecutingScope scope(nullptr);
                        execute(*command); // exceptions propagate like the old inline path
                    }
                    completed.store(command->seq, std::memory_order_relaxed);
                    applied.store(command->seq, std::memory_order_relaxed);
                    applyResults(*command);
                    return;
                }

                if (owner == std::thread::id{})
                    owner = std::this_thread::get_id();
                std::fegetenv(&command->fenv);
                ensureWorker();

                const bool hasGif = command->transfer.hadGif;
                const bool hasVif1 = command->transfer.hadVif1;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    const uint64_t seq = submitted.load(std::memory_order_relaxed) + 1u;
                    command->seq = seq;
                    if (hasGif)
                        lastGifSeq.store(seq, std::memory_order_relaxed);
                    if (hasVif1)
                        lastVif1Seq.store(seq, std::memory_order_relaxed);
                    queue.push_back(std::move(command));
                    submitted.store(seq, std::memory_order_release);
                }
                workCv.notify_one();
                maybeReportTrace();
            }

            // ---- drain / apply (EE owner) -----------------------------------------
            bool isOwner() const
            {
                return owner != std::thread::id{} && owner == std::this_thread::get_id();
            }

            // Drains whose caller observes GS state also wait for the GS worker (GifArbiter).
            static bool observesGs(DrainReason reason) noexcept
            {
                switch (reason)
                {
                case DrainReason::Vu1Memory:
                case DrainReason::Vif1Reg:
                case DrainReason::Vif1Fifo:
                case DrainReason::Dmac1Reg:
                case DrainReason::DStat:
                    return false;
                default:
                    return true;
                }
            }

            void waitGs(DrainReason reason)
            {
                if (t_executingDepth == 0 && observesGs(reason) && mem.m_gifArbiter)
                    mem.m_gifArbiter->waitIdle();
            }

            void drainTo(uint64_t target, DrainReason reason)
            {
                if (!cfg.async || t_executingDepth != 0 || !isOwner())
                {
                    waitGs(reason);
                    return;
                }

                const uint64_t appliedNow = applied.load(std::memory_order_relaxed);
                if (target > appliedNow)
                {
                    bool blocked = false;
                    uint64_t waitNs = 0u;
                    if (completed.load(std::memory_order_acquire) < target)
                    {
                        blocked = true;
                        const auto waitStart = Clock::now();
                        std::unique_lock<std::mutex> lock(mutex);
                        doneCv.wait(lock, [&]
                                    { return completed.load(std::memory_order_acquire) >= target; });
                        waitNs = nsSince(waitStart);
                    }
                    const uint64_t pendingBefore = submitted.load(std::memory_order_relaxed) - appliedNow;
                    applyCompleted();
                    statDrains.fetch_add(1u, std::memory_order_relaxed);
                    if (reason == DrainReason::Explicit)
                    {
                        const uint64_t field = mem.gs().vsyncTick.load(std::memory_order_relaxed);
                        if (g_lastFrameField.exchange(field, std::memory_order_relaxed) != field)
                            g_gameFrames.fetch_add(1u, std::memory_order_relaxed);
                    }
                    if (blocked)
                    {
                        statBlockingDrains.fetch_add(1u, std::memory_order_relaxed);
                        statDrainWaitNs.fetch_add(waitNs, std::memory_order_relaxed);
                    }
                    if (cfg.trace)
                        traceDrain(reason, blocked, waitNs, pendingBefore);
                }
                waitGs(reason);
                if (cfg.trace)
                    maybeReportTrace();
                rethrowPending();
            }

            void drainAll(DrainReason reason)
            {
                drainTo(submitted.load(std::memory_order_relaxed), reason);
            }

            bool poll() noexcept
            {
                if (!cfg.async || t_executingDepth != 0 || !isOwner())
                    return false;
                if (applied.load(std::memory_order_relaxed) >= completed.load(std::memory_order_acquire))
                    return false;
                try
                {
                    return applyCompleted();
                }
                catch (...)
                {
                    if (!pendingError)
                        pendingError = std::current_exception();
                    return false;
                }
            }

            bool hasDeferred(uint32_t channelBase) const
            {
                if (!cfg.async)
                    return false;
                const uint64_t appliedNow = applied.load(std::memory_order_relaxed);
                if (channelBase == kVif1Channel)
                    return lastVif1Seq.load(std::memory_order_relaxed) > appliedNow;
                if (channelBase == kGifChannel)
                    return lastGifSeq.load(std::memory_order_relaxed) > appliedNow;
                return false;
            }

            void quiesce(std::chrono::milliseconds timeout)
            {
                if (!cfg.async || t_executingDepth != 0)
                    return;
                std::unique_lock<std::mutex> lock(mutex);
                doneCv.wait_for(lock, timeout, [&]
                                { return completed.load(std::memory_order_acquire) >=
                                         submitted.load(std::memory_order_relaxed); });
            }

            void dropPending()
            {
                quiesce(std::chrono::milliseconds(10000));
                std::lock_guard<std::mutex> lock(mutex);
                const uint64_t done = completed.load(std::memory_order_acquire);
                while (!queue.empty() && queue.front()->seq <= done)
                    queue.pop_front();
                applied.store(done, std::memory_order_relaxed);
                pendingError = nullptr;
                if (done == submitted.load(std::memory_order_acquire))
                {
                    // With the worker idle under this mutex, discard retained
                    // parser input across memory reinitialisation too.
                    retainedGif.clear();
                    retainedVif.clear();
                    gifControl.epoch.fetch_add(1, std::memory_order_acq_rel);
                    vifControl.epoch.fetch_add(1, std::memory_order_acq_rel);
                }
            }

            void stopWorker()
            {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    stop = true;
                }
                workCv.notify_all();
                if (worker.joinable())
                {
                    if (worker.get_id() == std::this_thread::get_id())
                        worker.detach();
                    else
                        worker.join();
                }
            }

            Stats stats() const
            {
                Stats s;
                s.async = cfg.async;
                s.submitted = submitted.load(std::memory_order_relaxed);
                s.completed = completed.load(std::memory_order_relaxed);
                s.applied = applied.load(std::memory_order_relaxed);
                s.drains = statDrains.load(std::memory_order_relaxed);
                s.blockingDrains = statBlockingDrains.load(std::memory_order_relaxed);
                s.drainWaitNs = statDrainWaitNs.load(std::memory_order_relaxed);
                s.workerBusyNs = statBusyNs.load(std::memory_order_relaxed);
                s.workerIdleNs = statIdleNs.load(std::memory_order_relaxed);
                return s;
            }

            std::atomic<uint64_t> submitted{0};
            std::atomic<uint64_t> completed{0};
            std::atomic<uint64_t> applied{0};
            std::atomic<uint64_t> lastGifSeq{0};
            std::atomic<uint64_t> lastVif1Seq{0};

        private:
            // Worker only. Paused commands retire their sequence and leave tails
            // here; the worker never waits for restart/reset queued behind them.
            std::deque<std::unique_ptr<Command>> retainedGif, retainedVif;

            void retain(Command &source, bool vif, size_t chunkIndex, uint32_t accepted = 0)
            {
                auto tail = std::make_unique<Command>();
                tail->kind = Command::Kind::Transfer;
                tail->device = this;
                tail->vifEpoch = source.vifEpoch;
                tail->gifEpoch = source.gifEpoch;
                tail->vu0Fbrst = source.vu0Fbrst;
                tail->fenv = source.fenv;
                tail->vu1StopValid = source.vu1StopValid;
                tail->vu1StopBits = source.vu1StopBits;
                auto &from = vif ? source.transfer.vif1Chunks : source.transfer.gifChunks;
                auto &to = vif ? tail->transfer.vif1Chunks : tail->transfer.gifChunks;
                if (chunkIndex < from.size() && accepted < from[chunkIndex].size())
                    to.emplace_back(from[chunkIndex].begin() + accepted, from[chunkIndex].end());
                for (size_t i = chunkIndex + 1; i < from.size(); ++i)
                    to.push_back(std::move(from[i]));
                tail->transfer.hadVif1 = vif;
                tail->transfer.hadGif = !vif;
                (vif ? retainedVif : retainedGif).push_back(std::move(tail));
            }

            void executeTransfer(Command &command)
            {
                ProfileScope profile(ProfilePart::Transfer);
                // GIF packets of this transfer reach the GS worker in groups (same order).
                struct GifBatch
                {
                    GifArbiter *arbiter;
                    explicit GifBatch(GifArbiter *a) : arbiter(a) { if (arbiter) arbiter->beginBatch(); }
                    ~GifBatch() { if (arbiter) arbiter->endBatch(); }
                } gifBatch(mem.m_gifArbiter);
                bool gifFinished = true, vifFinished = true;
                for (size_t i = 0; i < command.transfer.gifChunks.size(); ++i)
                {
                    if (command.gifEpoch != gifControl.epoch.load(std::memory_order_acquire))
                    { gifFinished = false; break; }
                    if (command.gifCancel != gifControl.cancel.load(std::memory_order_acquire) || gifControl.paused.load(std::memory_order_acquire))
                    { retain(command, false, i); gifFinished = false; break; }
                    const auto &chunk = command.transfer.gifChunks[i];
                    mem.submitGifPacket(GifPathId::Path3, chunk.data(), static_cast<uint32_t>(chunk.size()), false);
                }
                for (size_t i = 0; i < command.transfer.vif1Chunks.size(); ++i)
                {
                    command.vifAccepted = 0;
                    const auto &chunk = command.transfer.vif1Chunks[i];
                    try
                    {
                        checkpoint(command, 0);
                        mem.processVIF1Data(chunk.data(), static_cast<uint32_t>(chunk.size()));
                        checkpoint(command, static_cast<uint32_t>(chunk.size()));
                    }
                    catch (const InputStopped &)
                    {
                        if (command.vifEpoch == vifControl.epoch.load(std::memory_order_acquire))
                            retain(command, true, i, std::min<uint32_t>(command.vifAccepted, chunk.size()));
                        vifFinished = false;
                        break;
                    }
                }
                if (mem.m_gifArbiter)
                    mem.m_gifArbiter->drain();
                command.completedGif = command.transfer.hadGif && gifFinished;
                command.completedVif = command.transfer.hadVif1 && vifFinished;
            }

            void execute(Command &command)
            {
                switch (command.kind)
                {
                case Command::Kind::Transfer:
                {
                    executeTransfer(command);
                    break;
                }
                case Command::Kind::Resume:
                {
                    const bool vif = command.resumeChannel == kVif1Channel;
                    auto &retained = vif ? retainedVif : retainedGif;
                    auto tails = std::move(retained);
                    retained.clear();
                    bool resumed = false, finished = true;
                    for (auto &tail : tails)
                    {
                        if ((vif ? tail->vifEpoch : tail->gifEpoch) != (vif ? command.vifEpoch : command.gifEpoch))
                            continue;
                        tail->gifCancel = command.gifCancel;
                        tail->vifCancel = command.vifCancel;
                        tail->vuEpoch = command.vuEpoch;
                        std::fesetenv(&tail->fenv);
                        ExecutingScope scope(tail.get());
                        executeTransfer(*tail);
                        if (tail->vu1StopValid)
                        {
                            command.vu1StopValid = true;
                            command.vu1StopBits = tail->vu1StopBits;
                        }
                        resumed = true;
                        finished &= vif ? tail->completedVif : tail->completedGif;
                    }
                    command.completedVif = vif && resumed && finished;
                    command.completedGif = !vif && resumed && finished;
                    break;
                }
                case Command::Kind::GsRegWrites:
                    if (cfg.gsWriteRegister)
                    {
                        for (const auto &write : command.gsRegs)
                            cfg.gsWriteRegister(write.first, write.second);
                    }
                    break;
                case Command::Kind::GsCall:
                    if (command.call)
                        command.call();
                    break;
                }
            }

            void applyResults(Command &command)
            {
                if (command.error)
                {
                    // The synchronous path never reached its completion tail
                    // either; keep CHCR.STR/D_STAT as they were.
                    if (!pendingError)
                        pendingError = command.error;
                    return;
                }
                if ((command.kind == Command::Kind::Transfer || command.kind == Command::Kind::Resume) && cfg.async)
                    applyTransferCompletion(mem,
                        command.completedGif && command.gifCancel == gifControl.cancel.load(std::memory_order_acquire) &&
                            command.gifEpoch == gifControl.epoch.load(std::memory_order_acquire), false,
                        command.completedVif && command.vifCancel == vifControl.cancel.load(std::memory_order_acquire) &&
                            command.vifEpoch == vifControl.epoch.load(std::memory_order_acquire),
                        command.gifGeneration == gifControl.generation.load(std::memory_order_acquire),
                        command.vifGeneration == vifControl.generation.load(std::memory_order_acquire),
                        command.gifQwcGeneration == gifControl.qwcGeneration.load(std::memory_order_acquire),
                        command.vifQwcGeneration == vifControl.qwcGeneration.load(std::memory_order_acquire));
                if (command.vu1StopValid && command.vuEpoch == vuEpoch.load(std::memory_order_acquire) && cfg.applyVu1StopBits)
                    cfg.applyVu1StopBits(command.vu1StopBits);
            }

            bool applyCompleted()
            {
                const uint64_t done = completed.load(std::memory_order_acquire);
                bool any = false;
                while (applied.load(std::memory_order_relaxed) < done)
                {
                    std::unique_ptr<Command> command;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        if (queue.empty())
                            break;
                        command = std::move(queue.front());
                        queue.pop_front();
                    }
                    applied.store(command->seq, std::memory_order_relaxed);
                    applyResults(*command);
                    any = true;
                }
                return any;
            }

            void rethrowPending()
            {
                if (pendingError)
                {
                    std::exception_ptr error = std::move(pendingError);
                    pendingError = nullptr;
                    std::rethrow_exception(error);
                }
            }

            void ensureWorker()
            {
                if (workerStarted)
                    return;
                workerStarted = true;
                worker = std::thread([this]
                                     { workerMain(); });
            }

            void workerMain()
            {
                ThreadNaming::SetCurrentThreadName("ps2-device");
                auto windowStart = Clock::now();
                uint64_t windowBusy = 0u;
                uint64_t windowIdle = 0u;
                uint64_t windowCommands = 0u;
                auto idleStart = Clock::now();

                std::unique_lock<std::mutex> lock(mutex);
                while (true)
                {
                    workCv.wait(lock, [&]
                                { return stop || nextExec <= submitted.load(std::memory_order_relaxed); });
                    if (nextExec > submitted.load(std::memory_order_relaxed))
                    {
                        if (stop)
                            break;
                        continue;
                    }
                    Command *command = queue[static_cast<size_t>(nextExec - queue.front()->seq)].get();
                    lock.unlock();

                    const uint64_t idleNs = nsSince(idleStart);
                    const auto busyStart = Clock::now();
                    std::fesetenv(&command->fenv);
                    try
                    {
                        ExecutingScope scope(command);
                        execute(*command);
                    }
                    catch (...)
                    {
                        command->error = std::current_exception();
                    }
                    const uint64_t busyNs = nsSince(busyStart);
                    idleStart = Clock::now();

                    statBusyNs.fetch_add(busyNs, std::memory_order_relaxed);
                    statIdleNs.fetch_add(idleNs, std::memory_order_relaxed);
                    if (cfg.trace)
                    {
                        windowBusy += busyNs;
                        windowIdle += idleNs;
                        ++windowCommands;
                        const auto now = Clock::now();
                        if (now - windowStart >= std::chrono::seconds(1))
                        {
                            const auto windowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - windowStart).count();
                            std::fprintf(stderr, "[vu1-async:worker] windowMs=%lld busyNs=%llu idleNs=%llu commands=%llu\n",
                                         static_cast<long long>(windowMs), static_cast<unsigned long long>(windowBusy),
                                         static_cast<unsigned long long>(windowIdle), static_cast<unsigned long long>(windowCommands));
                            windowStart = now;
                            windowBusy = windowIdle = windowCommands = 0u;
                        }
                    }

                    lock.lock();
                    // Pause may arrive after the final accepted byte but
                    // before completion publication. Preserve an empty tail
                    // so Restart can finish that DMA without replaying bytes.
                    if (command->kind == Command::Kind::Transfer || command->kind == Command::Kind::Resume)
                    {
                        if (command->completedVif && command->vifCancel != vifControl.cancel.load(std::memory_order_acquire) &&
                            command->vifEpoch == vifControl.epoch.load(std::memory_order_acquire))
                        {
                            retain(*command, true, command->transfer.vif1Chunks.size());
                            command->completedVif = false;
                        }
                        if (command->completedGif && command->gifCancel != gifControl.cancel.load(std::memory_order_acquire) &&
                            command->gifEpoch == gifControl.epoch.load(std::memory_order_acquire))
                        {
                            retain(*command, false, command->transfer.gifChunks.size());
                            command->completedGif = false;
                        }
                    }
                    completed.store(command->seq, std::memory_order_release);
                    ++nextExec;
                    doneCv.notify_all();
                    if (cfg.workerIdle && nextExec > submitted.load(std::memory_order_relaxed))
                    {
                        // Wake scheduler waiters after publication; never call out
                        // of the worker while holding the queue lock.
                        lock.unlock();
                        cfg.workerIdle();
                        lock.lock();
                    }
                }
            }

            void traceDrain(DrainReason reason, bool blocked, uint64_t waitNs, uint64_t pendingBefore)
            {
                const uint32_t ra = cfg.callerRa ? cfg.callerRa() : 0u;
                auto &s = reasonStats[static_cast<size_t>(reason)];
                ++s.count;
                s.lastRa = ra;
                if (blocked)
                {
                    ++s.blocked;
                    s.waitNs += waitNs;
                    if (blockedLinesInWindow < 16u)
                    {
                        ++blockedLinesInWindow;
                        std::fprintf(stderr, "[vu1-async] drain reason=%s ra=0x%x waitNs=%llu pending=%llu\n",
                                     drainReasonName(reason), ra, static_cast<unsigned long long>(waitNs),
                                     static_cast<unsigned long long>(pendingBefore));
                    }
                }
            }

            void maybeReportTrace()
            {
                if (!cfg.trace)
                    return;
                const auto now = Clock::now();
                if (now - traceWindowStart < std::chrono::seconds(1))
                    return;
                const auto windowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - traceWindowStart).count();
                std::fprintf(stderr, "[vu1-async] windowMs=%lld submitted=%llu applied=%llu workerBusyNs=%llu workerIdleNs=%llu",
                             static_cast<long long>(windowMs),
                             static_cast<unsigned long long>(submitted.load(std::memory_order_relaxed) - traceSubmittedMark),
                             static_cast<unsigned long long>(applied.load(std::memory_order_relaxed) - traceAppliedMark),
                             static_cast<unsigned long long>(statBusyNs.load(std::memory_order_relaxed) - traceBusyMark),
                             static_cast<unsigned long long>(statIdleNs.load(std::memory_order_relaxed) - traceIdleMark));
                if (profileEnabled())
                {
                    std::array<uint64_t, static_cast<size_t>(ProfilePart::Count)> now{};
                    for (size_t i = 0; i < now.size(); ++i)
                        now[i] = g_profileNs[i].load(std::memory_order_relaxed);
                    std::fprintf(stderr, " profTransferUs=%llu profVu1Us=%llu profGifSubmitUs=%llu profVifUnpackUs=%llu",
                                 static_cast<unsigned long long>((now[0] - traceProfileMark[0]) / 1000u),
                                 static_cast<unsigned long long>((now[1] - traceProfileMark[1]) / 1000u),
                                 static_cast<unsigned long long>((now[2] - traceProfileMark[2]) / 1000u),
                                 static_cast<unsigned long long>((now[3] - traceProfileMark[3]) / 1000u));
                    traceProfileMark = now;
                }
                for (size_t i = 0; i < reasonStats.size(); ++i)
                {
                    const auto &s = reasonStats[i];
                    if (s.count == 0u)
                        continue;
                    std::fprintf(stderr, " %s=%llu/%llu/%lluus@0x%x", drainReasonName(static_cast<DrainReason>(i)),
                                 static_cast<unsigned long long>(s.count), static_cast<unsigned long long>(s.blocked),
                                 static_cast<unsigned long long>(s.waitNs / 1000u), s.lastRa);
                }
                std::fprintf(stderr, "\n");
                reasonStats = {};
                blockedLinesInWindow = 0u;
                traceWindowStart = now;
                traceSubmittedMark = submitted.load(std::memory_order_relaxed);
                traceAppliedMark = applied.load(std::memory_order_relaxed);
                traceBusyMark = statBusyNs.load(std::memory_order_relaxed);
                traceIdleMark = statIdleNs.load(std::memory_order_relaxed);
            }

            // Shared with the worker (guarded by mutex where noted).
            std::mutex mutex;
            std::condition_variable workCv;
            std::condition_variable doneCv;
            std::deque<std::unique_ptr<Command>> queue; // submitted, not yet applied (mutex)
            uint64_t nextExec = 1u;                      // worker (mutex)
            bool stop = false;                           // mutex
            std::thread worker;
            bool workerStarted = false;

            // EE owner only.
            std::thread::id owner{};
            std::exception_ptr pendingError;

            std::atomic<uint64_t> statDrains{0};
            std::atomic<uint64_t> statBlockingDrains{0};
            std::atomic<uint64_t> statDrainWaitNs{0};
            std::atomic<uint64_t> statBusyNs{0};
            std::atomic<uint64_t> statIdleNs{0};

            struct ReasonStats
            {
                uint64_t count = 0u;
                uint64_t blocked = 0u;
                uint64_t waitNs = 0u;
                uint32_t lastRa = 0u;
            };
            std::array<ReasonStats, static_cast<size_t>(DrainReason::Count)> reasonStats{};
            uint32_t blockedLinesInWindow = 0u;
            Clock::time_point traceWindowStart;
            uint64_t traceSubmittedMark = 0u;
            uint64_t traceAppliedMark = 0u;
            uint64_t traceBusyMark = 0u;
            uint64_t traceIdleMark = 0u;
            std::array<uint64_t, static_cast<size_t>(ProfilePart::Count)> traceProfileMark{};
        };

        // Small lock-free lookup table: PS2Memory -> device. Attach/detach are
        // serialised by g_registryMutex and happen at init/shutdown only.
        constexpr size_t kSlots = 8u;
        std::array<std::atomic<const PS2Memory *>, kSlots> g_keys{};
        std::array<std::atomic<DeviceQueue *>, kSlots> g_values{};
        std::atomic<int> g_attached{0};
        std::mutex g_registryMutex;

        DeviceQueue *find(const PS2Memory &mem) noexcept
        {
            if (g_attached.load(std::memory_order_acquire) == 0)
                return nullptr;
            for (size_t i = 0; i < kSlots; ++i)
            {
                if (g_keys[i].load(std::memory_order_acquire) == &mem)
                    return g_values[i].load(std::memory_order_acquire);
            }
            return nullptr;
        }

        DeviceQueue *findAsync(const PS2Memory &mem) noexcept
        {
            DeviceQueue *q = find(mem);
            return (q && q->cfg.async) ? q : nullptr;
        }

        // Removes the registry entry and returns ownership (registry mutex held by caller).
        std::unique_ptr<DeviceQueue> takeLocked(const PS2Memory &mem)
        {
            for (size_t i = 0; i < kSlots; ++i)
            {
                if (g_keys[i].load(std::memory_order_acquire) == &mem)
                {
                    DeviceQueue *q = g_values[i].load(std::memory_order_acquire);
                    g_keys[i].store(nullptr, std::memory_order_release);
                    g_values[i].store(nullptr, std::memory_order_release);
                    g_attached.fetch_sub(1, std::memory_order_acq_rel);
                    return std::unique_ptr<DeviceQueue>(q);
                }
            }
            return nullptr;
        }

        void shutdownQueue(std::unique_ptr<DeviceQueue> q) noexcept
        {
            if (!q)
                return;
            try
            {
                if (q->cfg.async)
                {
                    if (q->isOwner() && t_executingDepth == 0)
                        q->drainAll(DrainReason::Shutdown);
                    else
                        q->quiesce(std::chrono::milliseconds(10000));
                }
            }
            catch (const std::exception &e)
            {
                std::fprintf(stderr, "[vu1-async] device error at shutdown: %s\n", e.what());
            }
            catch (...)
            {
                std::fprintf(stderr, "[vu1-async] unknown device error at shutdown\n");
            }
            q->stopWorker();
        }

        uint64_t drainTargetForAddress(DeviceQueue &q, uint32_t physAddr, DrainReason &reason)
        {
            if (physAddr >= kVif1Channel && physAddr < kVif1Channel + 0x100u)
            {
                reason = DrainReason::Dmac1Reg;
                return q.lastVif1Seq.load(std::memory_order_relaxed);
            }
            if (physAddr >= kGifChannel && physAddr < kGifChannel + 0x100u)
            {
                reason = DrainReason::Dmac2Reg;
                return q.lastGifSeq.load(std::memory_order_relaxed);
            }
            if (physAddr == kDStat)
                reason = DrainReason::DStat;
            else if (physAddr >= 0x10003C00u && physAddr < 0x10003E00u)
                reason = DrainReason::Vif1Reg;
            else if (physAddr >= 0x10003000u && physAddr < 0x10003100u)
                reason = DrainReason::GifReg;
            else if (physAddr >= 0x10005000u && physAddr < 0x10006000u)
                reason = DrainReason::Vif1Fifo;
            else if (physAddr >= 0x12000000u && physAddr < 0x12002000u)
            {
                if ((physAddr & ~7u) == 0x12001000u)
                    return 0u;
                reason = DrainReason::GsPrivReg;
            }
            else if (physAddr >= 0x11008000u && physAddr < 0x11010000u)
                reason = DrainReason::Vu1Memory;
            else
                return 0u;
            return q.submitted.load(std::memory_order_relaxed);
        }
    }

    const char *drainReasonName(DrainReason reason) noexcept
    {
        switch (reason)
        {
        case DrainReason::Dmac1Reg: return "d1";
        case DrainReason::Dmac2Reg: return "d2";
        case DrainReason::DStat: return "dstat";
        case DrainReason::Vif1Reg: return "vif1reg";
        case DrainReason::Vif1Fifo: return "vif1fifo";
        case DrainReason::GifReg: return "gifreg";
        case DrainReason::GsPrivReg: return "gspriv";
        case DrainReason::Vu1Memory: return "vu1mem";
        case DrainReason::StoreImage: return "storeimage";
        case DrainReason::GsDisplayEnv: return "gsdispenv";
        case DrainReason::NativeGifChain: return "nativegif";
        case DrainReason::DirectPath: return "directpath";
        case DrainReason::Reconfigure: return "reconfigure";
        case DrainReason::Shutdown: return "shutdown";
        case DrainReason::Explicit: return "explicit";
        case DrainReason::Count: break;
        }
        return "?";
    }

    bool asyncRequestedByEnv() noexcept
    {
        const char *value = std::getenv("PS2X_VU1_ASYNC");
        return !(value && value[0] == '0' && value[1] == '\0');
    }

    bool traceRequestedByEnv() noexcept
    {
        const char *value = std::getenv("PS2X_VU1_ASYNC_TRACE");
        return value && *value && *value != '0';
    }

    void attach(PS2Memory &mem, Config config)
    {
        std::unique_ptr<DeviceQueue> previous;
        auto next = std::make_unique<DeviceQueue>(mem, std::move(config));
        {
            std::lock_guard<std::mutex> lock(g_registryMutex);
            previous = takeLocked(mem);
        }
        shutdownQueue(std::move(previous));

        std::lock_guard<std::mutex> lock(g_registryMutex);
        for (size_t i = 0; i < kSlots; ++i)
        {
            if (g_keys[i].load(std::memory_order_acquire) == nullptr)
            {
                if (next->cfg.trace)
                    std::fprintf(stderr, "[vu1-async] device attached mode=%s\n", next->cfg.async ? "async" : "sync");
                g_values[i].store(next.release(), std::memory_order_release);
                g_keys[i].store(&mem, std::memory_order_release);
                g_attached.fetch_add(1, std::memory_order_acq_rel);
                return;
            }
        }
        throw std::runtime_error("ps2x::device::attach: too many devices");
    }

    void detach(PS2Memory &mem) noexcept
    {
        std::unique_ptr<DeviceQueue> q;
        {
            std::lock_guard<std::mutex> lock(g_registryMutex);
            q = takeLocked(mem);
        }
        shutdownQueue(std::move(q));
    }

    bool isAttached(const PS2Memory &mem) noexcept
    {
        return find(mem) != nullptr;
    }

    bool isAsync(const PS2Memory &mem) noexcept
    {
        return findAsync(mem) != nullptr;
    }

    bool executingCommand() noexcept
    {
        return t_executingDepth != 0;
    }

    bool onDeviceWorker() noexcept
    {
        return t_workerCommand != nullptr;
    }

    uint32_t workerVu0Fbrst() noexcept
    {
        return t_workerCommand ? t_workerCommand->vu0Fbrst : 0u;
    }

    void workerRecordVu1StopBits(uint32_t stopBits) noexcept
    {
        if (t_workerCommand)
        {
            t_workerCommand->vu1StopValid = true;
            t_workerCommand->vu1StopBits = stopBits;
        }
    }

    void workerCheckpointVif1Input(uint32_t consumedBytes)
    {
        if (t_workerCommand && t_workerCommand->device &&
            (t_workerCommand->kind == Command::Kind::Transfer || t_workerCommand->kind == Command::Kind::Resume))
            t_workerCommand->device->checkpoint(*t_workerCommand, consumedBytes);
    }

    uint64_t gameFrameCount() noexcept
    {
        return g_gameFrames.load(std::memory_order_relaxed);
    }

    bool profileEnabled() noexcept
    {
        static const bool enabled = []
        {
            const char *env = std::getenv("PS2X_DEVICE_PROFILE");
            return env && env[0] == '1';
        }();
        return enabled;
    }

    void profileAdd(ProfilePart part, uint64_t ns) noexcept
    {
        g_profileNs[static_cast<size_t>(part)].fetch_add(ns, std::memory_order_relaxed);
    }

    ProfileScope::ProfileScope(ProfilePart p) noexcept
        : part(p), start(profileEnabled() && t_executingDepth != 0
                             ? std::chrono::steady_clock::now().time_since_epoch().count() : -1)
    {
    }

    ProfileScope::~ProfileScope()
    {
        if (start >= 0)
            profileAdd(part, static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count() - start));
    }

    bool workerShouldCancelVu1() noexcept
    {
        return t_workerCommand && t_workerCommand->device &&
            t_workerCommand->vuEpoch != t_workerCommand->device->vuEpoch.load(std::memory_order_acquire);
    }

    bool pauseDmaChannel(PS2Memory &mem, uint32_t channelBase, uint32_t &oldChcr)
    {
        DeviceQueue *q = findAsync(mem);
        return q ? q->pause(channelBase, oldChcr) : false;
    }

    bool restartDmaChannel(PS2Memory &mem, uint32_t channelBase, uint32_t savedChcr, uint32_t &previousStr)
    {
        DeviceQueue *q = findAsync(mem);
        return q ? q->restart(channelBase, savedChcr, previousStr) : false;
    }

    bool syncDmaChannel(PS2Memory &mem, uint32_t channelBase)
    {
        DeviceQueue *q = findAsync(mem);
        if (!q || !q->isOwner() || executingCommand() || !q->channel(channelBase))
            return false;
        q->prepareControl();
        // A paused/completed channel is idle even if accepted downstream work
        // continues; DMA Sync waits only an active channel's transfer token.
        if ((mem.m_ioRegisters[channelBase] & 0x100u) != 0)
            q->drainTo(channelBase == kVif1Channel ? q->lastVif1Seq.load(std::memory_order_relaxed) :
                q->lastGifSeq.load(std::memory_order_relaxed),
                channelBase == kVif1Channel ? DrainReason::Dmac1Reg : DrainReason::Dmac2Reg);
        return true;
    }

    bool noteDmaRegisterWrite(PS2Memory &mem, uint32_t address)
    {
        DeviceQueue *q = findAsync(mem);
        if (!q || !q->isOwner() || executingCommand())
            return false;
        DeviceQueue::Channel *c = q->channel(address & ~0xFFu);
        if (!c)
            return false;
        if ((address & 0xFFu) == 0u)
            c->generation.fetch_add(1, std::memory_order_acq_rel);
        else if ((address & 0xFFu) == 0x20u)
            c->qwcGeneration.fetch_add(1, std::memory_order_acq_rel);
        else
            return false;
        return true;
    }

    bool submitControlWrite(PS2Memory &mem, uint32_t address, uint32_t value)
    {
        DeviceQueue *q = findAsync(mem);
        const bool vif = address >= 0x10003C00u && address < 0x10003E00u;
        const bool fifo = address >= 0x10005000u && address < 0x10006000u;
        const bool gif = address == 0x10003000u;
        if (!q || !q->isOwner() || executingCommand() || !q->cfg.controlWrite || (!vif && !fifo && !gif))
            return false;
        const bool vifReset = address == 0x10003C10u && (value & 1u);
        const bool gifReset = gif && (value & 1u);
        q->prepareControl();
        if (vifReset)
            q->invalidateVifInput();
        if (gifReset)
            q->invalidateGifInput();
        auto command = std::make_unique<Command>();
        command->kind = Command::Kind::GsCall;
        command->call = [q, address, value] { q->applyControlWrite(address, value); };
        q->submit(std::move(command));
        return true;
    }

    bool submitControlQword(PS2Memory &mem, uint32_t address, uint64_t lo, uint64_t hi)
    {
        DeviceQueue *q = findAsync(mem);
        if (!q || !q->isOwner() || executingCommand() || !q->cfg.controlQwordWrite ||
            address < 0x10005000u || address >= 0x10006000u)
            return false;
        auto command = std::make_unique<Command>();
        command->kind = Command::Kind::GsCall;
        command->call = [q, address, lo, hi] { q->cfg.controlQwordWrite(address, lo, hi); };
        q->submit(std::move(command));
        return true;
    }

    bool submitVu1Reset(PS2Memory &mem, std::function<void()> resetCallback)
    {
        DeviceQueue *q = findAsync(mem);
        if (!q || !q->isOwner() || executingCommand() || !resetCallback)
            return false;
        q->prepareControl();
        q->vuEpoch.fetch_add(1, std::memory_order_acq_rel);
        auto command = std::make_unique<Command>();
        command->kind = Command::Kind::GsCall;
        command->call = std::move(resetCallback);
        q->submit(std::move(command));
        return true;
    }

    bool submitVu1Reset(PS2Memory &mem)
    {
        DeviceQueue *q = findAsync(mem);
        return q ? submitVu1Reset(mem, q->cfg.resetVu1) : false;
    }

    void observeIo(PS2Memory &mem, uint32_t physAddr)
    {
        DeviceQueue *q = findAsync(mem);
        if (!q || executingCommand() || !q->isOwner())
            return;
        if ((physAddr >= kVif1Channel && physAddr < kVif1Channel + 0x100u) ||
            (physAddr >= kGifChannel && physAddr < kGifChannel + 0x100u))
        {
            // Channel registers are EE-owned latches, not an implicit wait
            // for downstream device processing. Explicit Sync remains explicit.
            q->prepareControl();
            return;
        }
        DrainReason reason = DrainReason::Explicit;
        const uint64_t target = drainTargetForAddress(*q, physAddr, reason);
        if (reason == DrainReason::Explicit)
            return;
        q->drainTo(target, reason);
    }

    void observeVu1Memory(PS2Memory &mem)
    {
        if (DeviceQueue *q = findAsync(mem))
            q->drainAll(DrainReason::Vu1Memory);
    }

    void drainAll(PS2Memory &mem, DrainReason reason)
    {
        if (DeviceQueue *q = findAsync(mem))
            q->drainAll(reason);
    }

    uint32_t readLocalToHost(PS2Memory &mem, uint8_t *dst, uint32_t maxBytes)
    {
        DeviceQueue *q = find(mem);
        if (!q || !q->cfg.localToHost || !dst || maxBytes == 0u)
            return 0u;
        // The readback must observe every GS command queued before it
        // (BITBLTBUF/TRXPOS/TRXREG/FINISH/TRXDIR=1).
        if (findAsync(mem))
            q->drainAll(DrainReason::Explicit);
        if (mem.m_gifArbiter)
            mem.m_gifArbiter->waitIdle(); // GS-produced readback data
        return q->cfg.localToHost(dst, maxBytes);
    }

    bool pollCompletions(PS2Memory &mem) noexcept
    {
        DeviceQueue *q = findAsync(mem);
        return q ? q->poll() : false;
    }

    bool hasDeferredChannel(const PS2Memory &mem, uint32_t channelBase) noexcept
    {
        DeviceQueue *q = findAsync(mem);
        return q ? q->hasDeferred(channelBase) : false;
    }

    void quiesce(PS2Memory &mem, std::chrono::milliseconds timeout) noexcept
    {
        if (DeviceQueue *q = findAsync(mem))
            q->quiesce(timeout);
    }

    void quiesceAll(std::chrono::milliseconds timeout) noexcept
    {
        for (size_t i = 0; i < kSlots; ++i)
        {
            DeviceQueue *q = g_values[i].load(std::memory_order_acquire);
            if (q && q->cfg.async)
                q->quiesce(timeout);
        }
    }

    void resetForReinitialize(PS2Memory &mem) noexcept
    {
        if (DeviceQueue *q = findAsync(mem))
            q->dropPending();
    }

    Stats stats(const PS2Memory &mem) noexcept
    {
        DeviceQueue *q = find(mem);
        return q ? q->stats() : Stats{};
    }

    bool submitTransfers(PS2Memory &mem, TransferSnapshot &&snapshot, bool hadVif0)
    {
        DeviceQueue *q = find(mem);
        if (!q)
            return false;

        const bool hadGif = snapshot.hadGif;
        const bool hadVif1 = snapshot.hadVif1;
        if (!q->cfg.async)
        {
            auto command = std::make_unique<Command>();
            command->kind = Command::Kind::Transfer;
            command->transfer = std::move(snapshot);
            q->submit(std::move(command));
            applyTransferCompletion(mem, hadGif, hadVif0, hadVif1);
            return true;
        }

        if (hadVif0)
            applyTransferCompletion(mem, false, true, false);
        if (!hadGif && !hadVif1)
            return true;

        auto command = std::make_unique<Command>();
        command->kind = Command::Kind::Transfer;
        command->transfer = std::move(snapshot);
        if (hadVif1 && q->cfg.currentVu0Fbrst)
            command->vu0Fbrst = q->cfg.currentVu0Fbrst();
        q->submit(std::move(command));
        return true;
    }

    bool submitGsRegWrites(PS2Memory &mem, std::vector<GsRegWrite> writes)
    {
        DeviceQueue *q = find(mem);
        if (!q)
            return false;
        auto command = std::make_unique<Command>();
        command->kind = Command::Kind::GsRegWrites;
        command->gsRegs = std::move(writes);
        q->submit(std::move(command));
        return true;
    }

    bool submitGsCall(PS2Memory &mem, std::function<void()> call)
    {
        DeviceQueue *q = find(mem);
        if (!q)
            return false;
        auto command = std::make_unique<Command>();
        command->kind = Command::Kind::GsCall;
        command->call = std::move(call);
        q->submit(std::move(command));
        return true;
    }
}
