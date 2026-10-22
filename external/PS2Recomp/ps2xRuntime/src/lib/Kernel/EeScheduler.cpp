#include <atomic>
#include <cstdio>
#include "runtime/ee_scheduler.h"

#include "ps2_log.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_device_queue.h"
#include "runtime/ps2_profile_markers.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <map>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
    constexpr int KE_OK = 0;
    constexpr int KE_ERROR = -1;
    constexpr int KE_ILLEGAL_PRIORITY = -403;
    constexpr int KE_ILLEGAL_THID = -406;
    constexpr int KE_UNKNOWN_THID = -407;
    constexpr int KE_UNKNOWN_SEMID = -408;
    constexpr int KE_UNKNOWN_EVFID = -409;
    constexpr int KE_DORMANT = -413;
    constexpr int KE_NOT_DORMANT = -414;
    constexpr int KE_NOT_SUSPEND = -415;
    constexpr int KE_NOT_WAIT = -416;
    constexpr int KE_RELEASE_WAIT = -418;
    constexpr int KE_SEMA_ZERO = -419;
    constexpr int KE_SEMA_OVF = -420;
    constexpr int KE_EVF_COND = -421;
    constexpr int KE_WAIT_DELETE = -425;

    constexpr uint32_t WEF_OR = 0x01u;
    constexpr uint32_t WEF_CLEAR = 0x10u;
    constexpr uint32_t WEF_CLEAR_ALL = 0x20u;
    // Host and guest timing follow the game-selected CRT field rate.
    std::chrono::nanoseconds vblankPeriod(bool pal)
    {
        return std::chrono::nanoseconds(pal ? 20000000 : 16683333);
    }
    constexpr auto kVBlankDuration = std::chrono::microseconds(500);
    constexpr uint64_t kAlarmTickMicroseconds = 64u;
    constexpr uint32_t kDebugPublishDispatchInterval = 4096u;

    constexpr uint64_t microsecondsToEeCycles(uint64_t microseconds)
    {
        return (microseconds * EeScheduler::kEeClockHz + 999999ull) / 1000000ull;
    }

    std::chrono::nanoseconds eeCyclesToHostDuration(uint64_t cycles)
    {
        constexpr uint64_t kNanosecondsPerSecond = 1000000000ull;
        const uint64_t wholeSeconds = cycles / EeScheduler::kEeClockHz;
        const uint64_t remainingCycles = cycles % EeScheduler::kEeClockHz;
        const uint64_t remainingNanoseconds = (remainingCycles * kNanosecondsPerSecond + EeScheduler::kEeClockHz - 1u) / EeScheduler::kEeClockHz;
        return std::chrono::seconds(wholeSeconds) + std::chrono::nanoseconds(remainingNanoseconds);
    }

    uint64_t vblankPeriodCycles(bool pal)
    {
        // Round upward to the next EE cycle: PAL 50 Hz, NTSC 60000/1001 Hz.
        return pal ? EeScheduler::kEeClockHz / 50u
                   : (EeScheduler::kEeClockHz * 1001u + 59999u) / 60000u;
    }
    constexpr uint64_t kVBlankDurationCycles = microsecondsToEeCycles(500u);
    constexpr uint64_t kAlarmTickCycles = microsecondsToEeCycles(kAlarmTickMicroseconds);

    template <typename Map>
    int allocatePositiveId(int &nextId, const Map &objects)
    {
        const int first = std::max(1, nextId);
        int candidate = first;
        do
        {
            if (!objects.contains(candidate))
            {
                nextId = (candidate == std::numeric_limits<int>::max()) ? 1 : candidate + 1;
                return candidate;
            }
            candidate = (candidate == std::numeric_limits<int>::max()) ? 1 : candidate + 1;
        } while (candidate != first);
        return 0;
    }
}

EeScheduler::EeScheduler(PS2Runtime &runtime)
    : m_runtime(runtime)
{
}

EeScheduler::~EeScheduler()
{
    requestStop();
}

void EeScheduler::setCrtMode(uint32_t mode) noexcept
{
    if (mode == 2u || mode == 3u)
        m_palVideoMode.store(mode == 3u, std::memory_order_relaxed);
}

bool EeScheduler::palVideoMode() const noexcept
{
    return m_palVideoMode.load(std::memory_order_relaxed);
}

namespace
{
// The VU0 macro-mode registers, VU0 control registers and COP2 control
// registers form two contiguous blocks of the guest context.
constexpr size_t kCop2Begin = offsetof(R5900Context, vu0_vf);
constexpr size_t kCop2End = offsetof(R5900Context, vu0_cf) + sizeof(R5900Context::vu0_cf);
constexpr size_t kCop2CcrBegin = offsetof(R5900Context, cop2_ccr);
constexpr size_t kCop2CcrSize = sizeof(R5900Context::cop2_ccr);
}

void EeScheduler::loadSharedCop2(R5900Context &context) const
{
    if (m_sharedCop2.empty())
        return;
    auto *bytes = reinterpret_cast<uint8_t *>(&context);
    std::memcpy(bytes + kCop2Begin, m_sharedCop2.data(), kCop2End - kCop2Begin);
    std::memcpy(bytes + kCop2CcrBegin, m_sharedCop2.data() + (kCop2End - kCop2Begin), kCop2CcrSize);
}

void EeScheduler::storeSharedCop2(const R5900Context &context)
{
    m_sharedCop2.resize(kCop2End - kCop2Begin + kCop2CcrSize);
    const auto *bytes = reinterpret_cast<const uint8_t *>(&context);
    std::memcpy(m_sharedCop2.data(), bytes + kCop2Begin, kCop2End - kCop2Begin);
    std::memcpy(m_sharedCop2.data() + (kCop2End - kCop2Begin), bytes + kCop2CcrBegin, kCop2CcrSize);
}

void EeScheduler::storeDispatchedCop2(int threadId, const R5900Context *context)
{
    const auto it = m_threads.find(threadId);
    if (it == m_threads.end())
        return;
    const GuestThread &thread = it->second;
    bool alive = &thread.context == context;
    for (const GuestInvocation &invocation : thread.invocations)
        alive = alive || &invocation.context == context;
    if (alive)
        storeSharedCop2(*context);
}

void EeScheduler::reset(uint8_t *rdram, const R5900Context &mainContext)
{
    m_executorThread = std::this_thread::get_id();
    m_rdram = rdram;
    m_readyQueues = {};
    m_threads.clear();
    m_semaphores.clear();
    m_eventFlags.clear();
    m_alarms.clear();
    m_intcHandlers.clear();
    m_dmacHandlers.clear();
    m_nextThreadId = kFirstThreadId;
    m_nextInvocationThreadId = -1;
    m_nextSemaphoreId = 1;
    m_nextEventFlagId = 1;
    m_nextAlarmId = 1;
    m_nextIntcHandlerId = 1;
    m_nextDmacHandlerId = 1;
    m_intcHeadOrder = 0;
    m_intcTailOrder = 1000;
    m_dmacHeadOrder = 0;
    m_dmacTailOrder = 1000;
    m_enabledIntcMask = 0xFFFFFFFFu;
    m_enabledDmacMask = 0xFFFFFFFFu;
    m_currentThreadId = 0;
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
    m_insideInterrupt = false;
    m_pendingEeTimerInterrupts = 0u;
    m_eeCycle = 0u;
    m_sliceEndCycle = kDefaultTimeSliceCycles;
    m_stopRequested.store(false, std::memory_order_release);
    m_checkpointPending.store(false, std::memory_order_release);
    m_debugPublishCountdown = 0u;
    {
        std::lock_guard lock(m_eventMutex);
        m_events.clear();
        m_deadlines.clear();
        m_pendingInvocations.clear();
    }
    m_eventSequence = 0;
    m_invocationSequence = 0;
    m_vsyncTick.store(0u, std::memory_order_release);
    m_presentationTick.store(0u, std::memory_order_release);
    m_palVideoMode.store(false, std::memory_order_relaxed);
    m_vsyncFlagAddress = 0;
    m_vsyncTickAddress = 0;
    m_gsVSyncCallback = 0;
    m_gsVSyncCallbackGp = 0;
    m_gsVSyncCallbackSp = 0;
    m_runtime.memory().gs().vsyncTick.store(0u, std::memory_order_release);
    m_runtime.memory().resetEeTimers();

    GuestThread main{};
    main.id = kMainThreadId;
    main.context = mainContext;
    main.entry = mainContext.pc;
    // $sp is live execution state, not the stable initial stack descriptor
    // returned by ReferThreadStatus. SetupThread records that metadata.
    main.stack = 0u;
    main.gp = getRegU32(&mainContext, 28);
    main.initialPriority = 0;
    main.currentPriority = 0;
    main.status = EeThreadStatus::Ready;
    m_threads.emplace(main.id, std::move(main));
    m_readyQueues[0].push_back(kMainThreadId);
    scheduleEvent(m_eeCycle + vblankPeriodCycles(palVideoMode()),
                  std::chrono::steady_clock::now() + vblankPeriod(palVideoMode()),
                  EeEvent{EeEventType::VBlankStart, 0, 0});
    publishSnapshot();
}

void EeScheduler::run()
{
    assertExecutor();
    m_running.store(true, std::memory_order_release);
    // DW5XL (issue 027): test switches. PS2X_SAVE_STATE_AT=<tick>:<file> saves at the first quiet point from that
    // field on; PS2X_LOAD_STATE=<file> loads once the game reached field PS2X_LOAD_STATE_TICK (default 30, so the
    // host side has finished starting up).
    if (const char *at = std::getenv("PS2X_SAVE_STATE_AT"); at && *at)
    {
        const std::string spec = at;
        const size_t colon = spec.find(':');
        if (colon != std::string::npos)
        {
            m_stateSaveAtTick = std::max<uint64_t>(1u, std::strtoull(spec.substr(0, colon).c_str(), nullptr, 10));
            m_stateSaveAtPath = spec.substr(colon + 1);
        }
    }
    if (const char *load = std::getenv("PS2X_LOAD_STATE"); load && *load)
    {
        const char *tick = std::getenv("PS2X_LOAD_STATE_TICK");
        m_stateLoadAtTick = std::max<uint64_t>(1u, tick && *tick ? std::strtoull(tick, nullptr, 10) : 30u);
        m_stateLoadAtPath = load;
    }
    const char *dispatchEnv = std::getenv("PS2X_EE_DISPATCH_TRACE");
    const bool dispatchTrace = dispatchEnv && *dispatchEnv && *dispatchEnv != '0';
    struct DispatchStats { uint64_t calls = 0, ns = 0, dmaCalls = 0, dmaNs = 0, dmaMaxNs = 0; };
    std::map<std::pair<int, bool>, DispatchStats> dispatchStats;
    auto dispatchWindow = std::chrono::steady_clock::now();

    while (!m_stopRequested.load(std::memory_order_acquire))
    {
        processPendingEvents();
        if (m_stopRequested.load(std::memory_order_acquire))
        {
            break;
        }
        // Async VIF1/VU1/GIF device: apply DMA completions the worker already
        // published (CHCR.STR, D_STAT.CIS) and deliver their DMAC causes, the
        // way a finished transfer raises its interrupt between EE instructions.
        if (ps2x::device::pollCompletions(m_runtime.memory()))
        {
            m_runtime.drainCompletedDmacHandlers(m_rdram);
        }

        // DW5XL (issue 027): a save/load request is carried out here, at the first quiet point. After a load the
        // scheduler's state is the saved one: start the loop over.
        if (m_stateRequestPending.load(std::memory_order_acquire) || m_stateSaveAtTick != 0u || m_stateLoadAtTick != 0u)
        {
            if (serviceStateRequest())
            {
                continue;
            }
        }

        if (m_currentThreadId == 0)
        {
            GuestThread *next = selectReady();
            if (!next && m_pendingInvocations.empty())
            {
                copyMainContextToRuntime();
                publishIdleDebugContext();
                publishSnapshot();
                waitForEvent();
                continue;
            }
            if (next)
            {
                makeRunning(*next);
            }
            else
            {
                GuestThread *owner = &acquireInvocationThread();
                GuestInvocation invocation = std::move(m_pendingInvocations.front());
                m_pendingInvocations.pop_front();
                owner->status = EeThreadStatus::Running;
                m_currentThreadId = owner->id;
                renewTimeSlice();
                if (getRegU32(&invocation.context, 29) == 0u)
                {
                    SET_GPR_U32(&invocation.context, 29, invocationStackTop());
                }
                owner->invocations.push_back(std::move(invocation));
            }
        }

        GuestThread *running = currentThread();
        assert(running != nullptr);
        if (running->resumeCompletion)
        {
            auto completion = std::move(running->resumeCompletion);
            running->resumeCompletion = {};
            try
            {
                completion(running->activeContext());
            }
            catch (const EeDispatcherTransfer &)
            {
            }
            if (m_currentThreadId == 0)
            {
                continue;
            }
        }
        R5900Context &context = running->activeContext();
        if (m_debugPublishCountdown == 0u)
        {
            copyMainContextToRuntime();
            publishSnapshot();
            m_debugPublishCountdown = kDebugPublishDispatchInterval - 1u;
        }
        else
        {
            --m_debugPublishCountdown;
        }

        publishDebugContext(context);

        if (context.pc == 0u)
        {
            if (!running->invocations.empty())
            {
                GuestInvocation completed = std::move(running->invocations.back());
                running->invocations.pop_back();
                if (completed.onComplete)
                {
                    try
                    {
                        completed.onComplete(completed.context, running->activeContext());
                    }
                    catch (const EeDispatcherTransfer &)
                    {
                    }
                }
                continue;
            }
            makeDormant(*running);
            m_currentThreadId = 0;
            copyMainContextToRuntime();
            publishIdleDebugContext();
            publishSnapshot();
            continue;
        }

        if (!m_pendingInvocations.empty() && running->invocations.empty())
        {
            GuestInvocation invocation = std::move(m_pendingInvocations.front());
            m_pendingInvocations.pop_front();
            if (getRegU32(&invocation.context, 29) == 0u)
            {
                SET_GPR_U32(&invocation.context, 29, invocationStackTop());
            }
            running->invocations.push_back(std::move(invocation));
            continue;
        }

        if (!m_runtime.hasFunction(context.pc))
        {
            if (!running->invocations.empty())
            {
                context.pc = 0u;
            }
            else
            {
                m_runtime.reportMissingFunction(m_rdram,
                                                &context,
                                                context.pc,
                                                context.pc,
                                                PS2Runtime::GuestBranchKind::DirectJump,
                                                "EE scheduler");
                makeDormant(*running);
                m_currentThreadId = 0;
            }
            continue;
        }
        PS2Runtime::RecompiledFunction function = m_runtime.lookupFunction(context.pc);

        if (checkpointDue(kGuestDispatchCycles))
        {
            continue;
        }

        const int dispatchTid = running->id;
        const bool dispatchCallback = !running->invocations.empty();
        const uint32_t dispatchPc = context.pc;
        const bool dispatchDma = dispatchPc == 0x112FE8u || dispatchPc == 0x113050u || dispatchPc == 0x1130C8u;
        const auto dispatchBegin = dispatchTrace ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        try
        {
            if (!running->invocations.empty() && running->invocations.back().shouldExecute)
            {
                // Do this after the dispatch checkpoint: a queued call may have
                // been invalidated while another guest thread ran. Once guest
                // execution starts, preserve the callback's normal stack and
                // suspension semantics rather than cancelling it mid-call.
                auto shouldExecute = std::move(running->invocations.back().shouldExecute);
                running->invocations.back().shouldExecute = {};
                if (!shouldExecute())
                {
                    context.pc = 0u;
                    continue;
                }
            }
            m_insideInterrupt = !running->invocations.empty() && running->invocations.back().kind == GuestInvocationKind::Interrupt;
            loadSharedCop2(context);
            m_guestExecuting.store(true, std::memory_order_release);
            function(m_rdram, &context, &m_runtime);
            m_guestExecuting.store(false, std::memory_order_release);
            m_insideInterrupt = false;
            storeDispatchedCop2(dispatchTid, &context);
        }
        catch (const EeDispatcherTransfer &)
        {
            m_guestExecuting.store(false, std::memory_order_release);
            m_insideInterrupt = false;
            storeDispatchedCop2(dispatchTid, &context);
        }
        catch (...)
        {
            m_guestExecuting.store(false, std::memory_order_release);
            m_running.store(false, std::memory_order_release);
            publishSnapshot();
            throw;
        }

        if (dispatchTrace)
        {
            const auto now = std::chrono::steady_clock::now();
            const uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - dispatchBegin).count();
            auto &stats = dispatchStats[{dispatchTid, dispatchCallback}];
            ++stats.calls;
            stats.ns += ns;
            if (dispatchDma)
            {
                ++stats.dmaCalls;
                stats.dmaNs += ns;
                stats.dmaMaxNs = std::max(stats.dmaMaxNs, ns);
            }
            if (now - dispatchWindow >= std::chrono::seconds(1))
            {
                const auto windowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - dispatchWindow).count();
                for (const auto &[key, value] : dispatchStats)
                    std::fprintf(stderr, "[ee:dispatch-time] field=%llu windowMs=%lld tid=%d callback=%d calls=%llu hostNs=%llu dmaCalls=%llu dmaNs=%llu dmaMaxNs=%llu\n",
                        (unsigned long long)m_vsyncTick, (long long)windowMs, key.first, key.second ? 1 : 0,
                        (unsigned long long)value.calls, (unsigned long long)value.ns,
                        (unsigned long long)value.dmaCalls, (unsigned long long)value.dmaNs, (unsigned long long)value.dmaMaxNs);
                dispatchStats.clear();
                dispatchWindow = now;
            }
        }
        processPendingEvents();
        if (m_rescheduleRequested && m_currentThreadId != 0)
        {
            GuestThread *preempted = currentThread();
            assert(preempted != nullptr);
            enqueueReady(*preempted, !m_timeSliceExpired);
            m_currentThreadId = 0;
            m_rescheduleRequested = false;
            m_timeSliceExpired = false;
        }
    }

    m_guestExecuting.store(false, std::memory_order_release);
    m_running.store(false, std::memory_order_release);
    copyMainContextToRuntime();
    publishSnapshot();
}

void EeScheduler::requestStop()
{
    m_stopRequested.store(true, std::memory_order_release);
    m_checkpointPending.store(true, std::memory_order_release);
    m_eventCv.notify_all();
    m_presentationCv.notify_all();
}

void EeScheduler::postEvent(EeEvent event)
{
    if (event.type == EeEventType::Stop)
    {
        requestStop();
        return;
    }

    {
        std::lock_guard lock(m_eventMutex);
        m_events.push_back(event);
        m_checkpointPending.store(true, std::memory_order_release);
    }
    m_eventCv.notify_one();
}

namespace
{
    // Wall-clock time of the earliest pending VBlank edge (steady_clock ns, 0 = none). On the PS2 the field clock
    // is hardware time: a guest that is still running when a field is due must be interrupted then, not only once
    // its estimated cycle count reaches the field. Written under m_eventMutex, read by the executor at checkpoints.
    std::atomic<int64_t> g_nextFieldHostDeadlineNs{0};
    uint32_t g_fieldClockPoll = 0u;
    constexpr uint32_t kFieldClockPollInterval = 64u;

    int64_t steadyNs(std::chrono::steady_clock::time_point time) noexcept
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
    }
}

bool EeScheduler::checkpointDue(uint32_t cycles) noexcept
{
    accountCycles(cycles);

    if (m_checkpointPending.load(std::memory_order_acquire) ||
        m_stopRequested.load(std::memory_order_acquire))
    {
        return true;
    }

    const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
    if (nextEventCycle != 0u && m_eeCycle >= nextEventCycle)
    {
        m_checkpointPending.store(true, std::memory_order_release);
        return true;
    }

    if (++g_fieldClockPoll >= kFieldClockPollInterval)
    {
        g_fieldClockPoll = 0u;
        const int64_t fieldDeadline = g_nextFieldHostDeadlineNs.load(std::memory_order_acquire);
        if (fieldDeadline != 0 && steadyNs(std::chrono::steady_clock::now()) >= fieldDeadline)
        {
            // processDueDeadlines advances the EE clock to the wall-due field edge and emits it.
            m_checkpointPending.store(true, std::memory_order_release);
            return true;
        }
    }

    if (m_eeCycle < m_sliceEndCycle)
    {
        return false;
    }

    const GuestThread *running = currentThread();
    if (running != nullptr && hasReadyAtOrAbovePriority(running->currentPriority))
    {
        m_rescheduleRequested = true;
        m_timeSliceExpired = true;
        return true;
    }

    renewTimeSlice();
    return false;
}

void EeScheduler::accountCycles(uint32_t cycles) noexcept
{
    const uint64_t elapsed = std::max<uint64_t>(1u, cycles);
    m_eeCycle += elapsed;
    m_pendingEeTimerInterrupts |= m_runtime.memory().advanceEeTimers(elapsed);
    m_runtime.advanceIopEeCycles(elapsed);
    if (m_pendingEeTimerInterrupts != 0u)
    {
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

bool EeScheduler::isExecutingGuest() const noexcept
{
    return m_guestExecuting.load(std::memory_order_acquire);
}

void EeScheduler::setupCurrentThread(uint32_t stack, uint32_t stackSize, uint32_t gp)
{
    assertExecutor();
    GuestThread *target = currentThread();
    if (!target)
    {
        return;
    }

    target->stack = stack;
    target->stackSize = stackSize;
    target->gp = gp;
    publishSnapshot();
}

int EeScheduler::createThread(const EeThreadCreateParams &params)
{
    assertExecutor();
    // SDK InitThread creates KernelTopThread at priority zero before lowering
    // its caller to priority one. Rejecting that helper strands bootstrap at
    // priority zero and starves workers which it rotates at priority one.
    if (params.priority < 0 || params.priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }

    const int id = allocateThreadId();
    if (id == 0)
    {
        return KE_ERROR;
    }

    GuestThread thread{};
    thread.id = id;
    thread.entry = params.entry;
    thread.stack = params.stack;
    thread.stackSize = params.stackSize;
    thread.gp = params.gp;
    thread.attr = params.attr;
    thread.option = params.option;
    thread.initialPriority = params.priority;
    thread.currentPriority = params.priority;
    thread.status = EeThreadStatus::Dormant;
    m_threads.emplace(id, std::move(thread));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteThread(int id, uint32_t &ownedStack)
{
    assertExecutor();
    ownedStack = 0;
    if (id <= kMainThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    auto it = m_threads.find(id);
    if (it == m_threads.end())
    {
        return KE_UNKNOWN_THID;
    }
    if (it->second.status != EeThreadStatus::Dormant)
    {
        return KE_NOT_DORMANT;
    }
    if (it->second.ownsStack)
    {
        ownedStack = it->second.stack;
    }
    m_threads.erase(it);
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::startThread(int id, uint32_t arg, const R5900Context &caller, bool interruptSafe)
{
    assertExecutor();
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status != EeThreadStatus::Dormant)
    {
        return KE_NOT_DORMANT;
    }

    target->context = R5900Context{};
    target->context.pc = target->entry;
    target->arg = arg;
    target->suspendCount = 0;
    target->wakeupCount = 0;
    target->wait = {};
    SET_GPR_U32(&target->context, 4, arg);
    SET_GPR_U32(&target->context, 28, target->gp != 0u ? target->gp : getRegU32(&caller, 28));
    const uint32_t stackTop = target->stack != 0u
                                  ? (target->stack + target->stackSize) & ~0xFu
                                  : getRegU32(&caller, 29);
    SET_GPR_U32(&target->context, 29, stackTop);
    SET_GPR_U32(&target->context, 31, 0u);
    enqueueReady(*target);
    requestPreemptionIfHigher(*target, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

[[noreturn]] void EeScheduler::exitCurrent(bool deleteThreadRecord)
{
    assertExecutor();
    GuestThread *exiting = currentThread();
    assert(exiting != nullptr);
    const int id = exiting->id;
    const uint32_t ownedStack = deleteThreadRecord && exiting->ownsStack ? exiting->stack : 0u;
    makeDormant(*exiting);
    m_currentThreadId = 0;
    if (deleteThreadRecord && id != kMainThreadId)
    {
        m_threads.erase(id);
    }
    if (ownedStack != 0u)
    {
        m_runtime.guestFree(ownedStack);
    }
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

int EeScheduler::terminateThread(int id, uint32_t &ownedStack, bool interruptSafe)
{
    assertExecutor();
    ownedStack = 0;
    if (id == 0 || id == m_currentThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }
    if (target->ownsStack)
    {
        ownedStack = target->stack;
        target->ownsStack = false;
    }
    makeDormant(*target);
    (void)interruptSafe;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::suspendThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }

    ++target->suspendCount;
    switch (target->status)
    {
    case EeThreadStatus::Running:
        target->status = EeThreadStatus::Suspended;
        m_currentThreadId = 0;
        m_rescheduleRequested = true;
        break;
    case EeThreadStatus::Ready:
        removeReady(*target);
        target->status = EeThreadStatus::Suspended;
        break;
    case EeThreadStatus::Waiting:
        target->status = EeThreadStatus::WaitingSuspended;
        break;
    case EeThreadStatus::WaitingSuspended:
    case EeThreadStatus::Suspended:
        break;
    case EeThreadStatus::Dormant:
        break;
    }
    if (interruptSafe && m_insideInterrupt)
    {
        m_rescheduleRequested = true;
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::resumeThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->suspendCount == 0)
    {
        return KE_NOT_SUSPEND;
    }
    --target->suspendCount;
    if (target->suspendCount != 0)
    {
        return KE_OK;
    }
    if (target->status == EeThreadStatus::WaitingSuspended)
    {
        target->status = EeThreadStatus::Waiting;
    }
    else if (target->status == EeThreadStatus::Suspended)
    {
        enqueueReady(*target);
        requestPreemptionIfHigher(*target, interruptSafe);
    }
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::sleepCurrent()
{
    assertExecutor();
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (self->wakeupCount != 0u)
    {
        --self->wakeupCount;
        setReturnS32(&self->activeContext(), KE_OK);
        return;
    }
    blockCurrent(EeWaitState{EeWaitReason::Sleep, std::monostate{}});
}

int EeScheduler::wakeupThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0 || id == m_currentThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }
    if ((target->status == EeThreadStatus::Waiting || target->status == EeThreadStatus::WaitingSuspended) &&
        target->wait.reason == EeWaitReason::Sleep)
    {
        makeReady(*target, KE_OK, interruptSafe);
    }
    else
    {
        ++target->wakeupCount;
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::cancelWakeup(int id)
{
    assertExecutor();
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    const int old = static_cast<int>(target->wakeupCount);
    target->wakeupCount = 0;
    publishSnapshot();
    return old;
}

int EeScheduler::changePriority(int id, int priority, bool interruptSafe, int &oldPriority)
{
    assertExecutor();
    if (priority < 1 || priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    oldPriority = target->currentPriority;
    if (target->status == EeThreadStatus::Ready)
    {
        removeReady(*target);
        target->currentPriority = priority;
        enqueueReady(*target);
        requestPreemptionIfHigher(*target, interruptSafe);
    }
    else
    {
        target->currentPriority = priority;
        if (target->status == EeThreadStatus::Running)
        {
            for (int p = 0; p < target->currentPriority; ++p)
            {
                if (!m_readyQueues[p].empty())
                {
                    m_rescheduleRequested = true;
                    break;
                }
            }
        }
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::rotateReadyQueue(int priority, bool interruptSafe)
{
    assertExecutor();
    if (priority == 0)
    {
        const GuestThread *self = currentThread();
        priority = self ? self->currentPriority : 0;
    }
    if (priority < 0 || priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }

    GuestThread *self = currentThread();
    if (self && self->currentPriority == priority)
    {
        // DW5XL: a yield costs a few guest cycles but a host context switch, so threads that
        // busy-yield to each other (DW5XL's movie player) starved guest time and vblanks arrived
        // ~35x too slowly. Charge a realistic slice; events stay paced by host deadlines.
        constexpr uint32_t kYieldCycles = 100000u;
        accountCycles(kYieldCycles);
        enqueueReady(*self);
        m_currentThreadId = 0;
        m_rescheduleRequested = true;
    }
    else
    {
        auto &queue = m_readyQueues[priority];
        if (queue.size() > 1u)
        {
            const int head = queue.front();
            queue.pop_front();
            queue.push_back(head);
        }
    }
    (void)interruptSafe;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::releaseWait(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status != EeThreadStatus::Waiting && target->status != EeThreadStatus::WaitingSuspended)
    {
        return KE_NOT_WAIT;
    }
    removeFromWaitObject(*target);
    makeReady(*target, KE_RELEASE_WAIT, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::transferIfRequested(bool interruptSafe)
{
    assertExecutor();
    if (interruptSafe || m_insideInterrupt || !m_rescheduleRequested)
    {
        return;
    }
    if (m_currentThreadId != 0)
    {
        GuestThread *self = currentThread();
        assert(self != nullptr);
        enqueueReady(*self, true);
        m_currentThreadId = 0;
    }
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

int EeScheduler::createSemaphore(int initCount, int maxCount, uint32_t attr, uint32_t option)
{
    assertExecutor();
    if (maxCount <= 0 || initCount < 0 || initCount > maxCount)
    {
        return KE_ERROR;
    }
    const int id = allocatePositiveId(m_nextSemaphoreId, m_semaphores);
    if (id == 0)
    {
        return KE_ERROR;
    }
    EeSemaphore semaphore{};
    semaphore.id = id;
    semaphore.count = initCount;
    semaphore.maxCount = maxCount;
    semaphore.initCount = initCount;
    semaphore.attr = attr;
    semaphore.option = option;
    m_semaphores.emplace(id, std::move(semaphore));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteSemaphore(int id, bool interruptSafe)
{
    assertExecutor();
    auto it = m_semaphores.find(id);
    if (it == m_semaphores.end())
    {
        return KE_UNKNOWN_SEMID;
    }
    std::deque<int> waiters = std::move(it->second.waiters);
    m_semaphores.erase(it);
    for (const int threadId : waiters)
    {
        if (GuestThread *waiter = thread(threadId))
        {
            makeReady(*waiter, KE_WAIT_DELETE, interruptSafe);
        }
    }
    publishSnapshot();
    return id;
}

int EeScheduler::signalSemaphore(int id, bool interruptSafe)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        return KE_UNKNOWN_SEMID;
    }
    if (!object->waiters.empty())
    {
        const int waiterId = object->waiters.front();
        object->waiters.pop_front();
        GuestThread *waiter = thread(waiterId);
        assert(waiter != nullptr);
        makeReady(*waiter, id, interruptSafe);
        publishSnapshot();
        return id;
    }
    if (object->count == object->maxCount)
    {
        return KE_SEMA_OVF;
    }
    ++object->count;
    publishSnapshot();
    return id;
}

int EeScheduler::pollSemaphore(int id)
{
    assertExecutor();
    // EE PollSema/iPollSema return -1 on failure, unlike IOP error codes.
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        return -1;
    }
    if (object->count == 0)
    {
        return -1;
    }
    --object->count;
    publishSnapshot();
    return id;
}

void EeScheduler::waitSemaphore(int id)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        GuestThread *self = currentThread();
        assert(self != nullptr);
        setReturnS32(&self->activeContext(), KE_UNKNOWN_SEMID);
        return;
    }
    if (object->count != 0)
    {
        --object->count;
        GuestThread *self = currentThread();
        assert(self != nullptr);
        setReturnS32(&self->activeContext(), id);
        publishSnapshot();
        return;
    }
    GuestThread *self = currentThread();
    assert(self != nullptr);
    object->waiters.push_back(self->id);
    blockCurrent(EeWaitState{EeWaitReason::Semaphore, EeSemaphoreWait{id}});
}

int EeScheduler::createEventFlag(uint32_t initialBits, uint32_t attr, uint32_t option)
{
    assertExecutor();
    const int id = allocatePositiveId(m_nextEventFlagId, m_eventFlags);
    if (id == 0)
    {
        return KE_ERROR;
    }
    EeEventFlag flag{};
    flag.id = id;
    flag.attr = attr;
    flag.option = option;
    flag.initBits = initialBits;
    flag.bits = initialBits;
    m_eventFlags.emplace(id, std::move(flag));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteEventFlag(int id, bool interruptSafe)
{
    assertExecutor();
    auto it = m_eventFlags.find(id);
    if (it == m_eventFlags.end())
    {
        return KE_UNKNOWN_EVFID;
    }
    std::deque<int> waiters = std::move(it->second.waiters);
    m_eventFlags.erase(it);
    for (const int threadId : waiters)
    {
        if (GuestThread *waiter = thread(threadId))
        {
            makeReady(*waiter, KE_WAIT_DELETE, interruptSafe);
        }
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::setEventFlag(int id, uint32_t bits, bool interruptSafe)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    flag->bits |= bits;
    finishEventWaiters(*flag, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::clearEventFlag(int id, uint32_t mask)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    flag->bits &= mask;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::pollEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t &observedBits)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    if (!eventCondition(flag->bits, bits, mode))
    {
        return KE_EVF_COND;
    }
    observedBits = flag->bits;
    if ((mode & WEF_CLEAR_ALL) != 0u)
    {
        flag->bits = 0;
    }
    else if ((mode & WEF_CLEAR) != 0u)
    {
        flag->bits &= ~bits;
    }
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::waitEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t resultAddress)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (!flag)
    {
        setReturnS32(&self->activeContext(), KE_UNKNOWN_EVFID);
        return;
    }
    if (eventCondition(flag->bits, bits, mode))
    {
        const uint32_t observed = flag->bits;
        writeGuestU32(resultAddress, observed);
        if ((mode & WEF_CLEAR_ALL) != 0u)
        {
            flag->bits = 0;
        }
        else if ((mode & WEF_CLEAR) != 0u)
        {
            flag->bits &= ~bits;
        }
        setReturnS32(&self->activeContext(), KE_OK);
        publishSnapshot();
        return;
    }
    flag->waiters.push_back(self->id);
    blockCurrent(EeWaitState{EeWaitReason::EventFlag,
                             EeEventFlagWait{id, bits, mode, resultAddress}});
}

int EeScheduler::setAlarm(uint16_t ticks,
                          uint32_t handler,
                          uint32_t argument,
                          uint32_t gp,
                          uint32_t sp)
{
    assertExecutor();
    if (handler == 0u || !m_runtime.hasFunction(handler))
    {
        return KE_ERROR;
    }
    const int id = allocatePositiveId(m_nextAlarmId, m_alarms);
    if (id == 0)
    {
        return KE_ERROR;
    }
    m_alarms.emplace(id, EeAlarm{id, ticks, handler, argument, gp, sp});
    const uint64_t tickCount = ticks == 0u ? 1u : static_cast<uint64_t>(ticks);
    scheduleEvent(m_eeCycle + tickCount * kAlarmTickCycles,
                  std::chrono::steady_clock::now() + std::chrono::microseconds(tickCount * kAlarmTickMicroseconds),
                  EeEvent{EeEventType::Alarm, static_cast<uint32_t>(id), 0});
    return id;
}

int EeScheduler::cancelAlarm(int id)
{
    assertExecutor();
    if (m_alarms.erase(id) == 0u)
    {
        return KE_ERROR;
    }
    {
        std::lock_guard lock(m_eventMutex);
        std::erase_if(m_deadlines, [id](const ScheduledEvent &scheduled)
                      { return scheduled.event.type == EeEventType::Alarm &&
                               scheduled.event.id == static_cast<uint32_t>(id); });
        updateNextDeadline();
    }
    return KE_OK;
}

void EeScheduler::queueInvocation(GuestInvocation invocation)
{
    assertExecutor();
    invocation.sequence = ++m_invocationSequence;
    m_pendingInvocations.push_back(std::move(invocation));
    m_checkpointPending.store(true, std::memory_order_release);
}

[[noreturn]] void EeScheduler::invokeCurrent(GuestInvocation invocation)
{
    assertExecutor();
    GuestThread *owner = currentThread();
    assert(owner != nullptr);
    if (getRegU32(&invocation.context, 29) == 0u)
    {
        SET_GPR_U32(&invocation.context, 29, invocationStackTop());
    }
    invocation.sequence = ++m_invocationSequence;
    owner->invocations.push_back(std::move(invocation));
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

[[noreturn]] void EeScheduler::invokeCurrentSequence(std::vector<GuestInvocation> invocations)
{
    assertExecutor();
    GuestThread *owner = currentThread();
    assert(owner != nullptr);
    assert(!invocations.empty());
    for (auto it = invocations.rbegin(); it != invocations.rend(); ++it)
    {
        if (getRegU32(&it->context, 29) == 0u)
        {
            SET_GPR_U32(&it->context, 29, invocationStackTop());
        }
        it->sequence = ++m_invocationSequence;
        owner->invocations.push_back(std::move(*it));
    }
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

bool EeScheduler::hasInvocation(GuestInvocationKind kind, uint64_t tag) const
{
    const GuestThread *owner = currentThread();
    if (!owner)
    {
        return false;
    }
    return std::any_of(owner->invocations.begin(), owner->invocations.end(),
                       [kind, tag](const GuestInvocation &invocation)
                       {
                           return invocation.kind == kind && invocation.tag == tag;
                       });
}

uint32_t EeScheduler::invocationStackTop()
{
    assertExecutor();
    const GuestThread *owner = currentThread();
    if (!owner)
    {
        throw std::logic_error("EE invocation stack requested without a current guest context");
    }
    const size_t depth = owner ? owner->invocations.size() : 0u;
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(owner->id)) << 32u) |
                         static_cast<uint32_t>(depth);
    const auto existing = m_invocationStackTops.find(key);
    if (existing != m_invocationStackTops.end())
    {
        return existing->second;
    }
    constexpr uint32_t kInvocationStackSize = 0x4000u;
    const uint32_t top = m_runtime.reserveAsyncCallbackStack(kInvocationStackSize, 16u);
    if (top == 0u)
    {
        throw std::runtime_error("EE invocation stack space exhausted");
    }
    m_invocationStackTops.emplace(key, top);
    return top;
}

int EeScheduler::addIrqHandler(bool dmac,
                               uint32_t cause,
                               uint32_t handler,
                               bool append,
                               uint32_t argument,
                               uint32_t gp,
                               uint32_t sp)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    int &nextId = dmac ? m_nextDmacHandlerId : m_nextIntcHandlerId;
    const int id = allocatePositiveId(nextId, handlers);
    if (id == 0)
    {
        return KE_ERROR;
    }
    int &head = dmac ? m_dmacHeadOrder : m_intcHeadOrder;
    int &tail = dmac ? m_dmacTailOrder : m_intcTailOrder;
    handlers.emplace(id,
                     EeIrqHandler{id,
                                  cause,
                                  handler,
                                  argument,
                                  gp,
                                  sp,
                                  true,
                                  append ? ++tail : --head});
    return id;
}

int EeScheduler::removeIrqHandler(bool dmac, uint32_t cause, int id)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    auto it = handlers.find(id);
    if (it != handlers.end() && it->second.cause == cause)
    {
        handlers.erase(it);
    }
    return KE_OK;
}

int EeScheduler::setIrqHandlerEnabled(bool dmac, int id, bool enabled)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    auto it = handlers.find(id);
    if (it != handlers.end())
    {
        it->second.enabled = enabled;
    }
    return KE_OK;
}

int EeScheduler::setIrqCauseEnabled(bool dmac, uint32_t cause, bool enabled)
{
    assertExecutor();
    if (cause < 32u)
    {
        uint32_t &mask = dmac ? m_enabledDmacMask : m_enabledIntcMask;
        if (enabled)
        {
            mask |= 1u << cause;
        }
        else
        {
            mask &= ~(1u << cause);
        }
    }
    return KE_OK;
}

void EeScheduler::dispatchIrq(bool dmac, uint32_t cause)
{
    assertExecutor();
    const uint32_t mask = dmac ? m_enabledDmacMask : m_enabledIntcMask;
    static const bool traceIrq = [] {
        const char *flag = std::getenv("PS2X_IRQ_TRACE");
        return flag && flag[0] != '\0' && flag[0] != '0';
    }();
    static std::array<uint64_t, 64> traceCounts{};
    const uint64_t traceCount = traceIrq && cause < 32u ? ++traceCounts[(dmac ? 32u : 0u) + cause] : 0u;
    const bool traceSample = traceCount != 0u &&
        (traceCount <= 8u || (traceCount & (traceCount - 1u)) == 0u);
    if (traceSample)
        std::cerr << "[ee-irq] dmac=" << dmac << " cause=" << cause
                  << " mask=0x" << std::hex << mask << std::dec
                  << " calls=" << traceCount << std::endl;
    if (cause < 32u && (mask & (1u << cause)) == 0u)
    {
        return;
    }
    const auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    std::vector<EeIrqHandler> matching;
    for (const auto &[id, handler] : handlers)
    {
        (void)id;
        if (handler.enabled && handler.cause == cause && handler.handler != 0u &&
            m_runtime.hasFunction(handler.handler))
        {
            matching.push_back(handler);
        }
    }
    std::sort(matching.begin(), matching.end(), [](const EeIrqHandler &left, const EeIrqHandler &right)
              { return left.order < right.order; });
    for (const EeIrqHandler &handler : matching)
    {
        if (traceSample)
            std::cerr << "[ee-irq:queue] dmac=" << dmac << " cause=" << cause
                      << " handler=0x" << std::hex << handler.handler << std::dec << std::endl;
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::Interrupt;
        invocation.context.pc = handler.handler;
        SET_GPR_U32(&invocation.context, 4, cause);
        SET_GPR_U32(&invocation.context, 5, handler.argument);
        SET_GPR_U32(&invocation.context, 28, handler.gp);
        SET_GPR_U32(&invocation.context, 29, 0u);
        SET_GPR_U32(&invocation.context, 31, 0u);
        if (traceSample)
            invocation.onComplete = [dmac, cause, address = handler.handler](const R5900Context &context, R5900Context &) {
                std::cerr << "[ee-irq:complete] dmac=" << dmac << " cause=" << cause
                          << " handler=0x" << std::hex << address
                          << " result=0x" << getRegU32(&context, 2) << std::dec << std::endl;
            };
        queueInvocation(std::move(invocation));
    }
}

void EeScheduler::setVSyncFlag(uint32_t flagAddress, uint32_t tickAddress)
{
    assertExecutor();
    m_vsyncFlagAddress = flagAddress;
    m_vsyncTickAddress = tickAddress;
    writeGuestU32(flagAddress, 0u);
    if (tickAddress != 0u)
    {
        const uint32_t physical = tickAddress & 0x1FFFFFFFu;
        if (m_rdram && physical <= PS2_RAM_SIZE - sizeof(uint64_t))
        {
            const uint64_t zero = 0u;
            std::memcpy(m_rdram + physical, &zero, sizeof(zero));
        }
    }
}

uint64_t EeScheduler::currentVSyncTick() const noexcept
{
    return m_vsyncTick.load(std::memory_order_acquire);
}

void EeScheduler::stepVSyncTicksForTesting(uint64_t ticks) noexcept
{
    m_vsyncTick.fetch_add(ticks, std::memory_order_acq_rel);
}

uint64_t EeScheduler::waitForNextPresentationField(uint64_t &lastTick) const
{
    // The host window shows each guest field once: wait for the next VBlankEnd (or one field period, so the
    // window stays responsive while the game is stalled).
    uint64_t cur = m_presentationTick.load(std::memory_order_acquire);
    if (cur <= lastTick)
    {
        std::unique_lock lock(m_presentationMutex);
        m_presentationCv.wait_for(lock, vblankPeriod(palVideoMode()), [this, lastTick]()
                                  { return m_presentationTick.load(std::memory_order_acquire) > lastTick ||
                                           m_stopRequested.load(std::memory_order_acquire); });
        cur = m_presentationTick.load(std::memory_order_acquire);
    }
    if (cur > lastTick)
    {
        lastTick = cur;
    }
    return cur != 0u ? cur : m_vsyncTick.load(std::memory_order_acquire);
}

uint32_t EeScheduler::setGsVSyncCallback(uint32_t callback, uint32_t gp, uint32_t sp)
{
    assertExecutor();
    (void)sp;
    const uint32_t previous = m_gsVSyncCallback;
    m_gsVSyncCallback = callback;
    m_gsVSyncCallbackGp = gp;
    m_gsVSyncCallbackSp = 0u;
    return previous;
}

[[noreturn]] void EeScheduler::waitVSync(uint64_t afterTick, int fixedResult, std::function<void(R5900Context &)> completion)
{
    blockCurrent(EeWaitState{
        EeWaitReason::VSync,
        EeVSyncWait{afterTick, fixedResult},
        std::move(completion)});
}

void EeScheduler::completeVSync(uint64_t tick)
{
    assertExecutor();
    std::vector<int> completed;
    for (const auto &[id, candidate] : m_threads)
    {
        if ((candidate.status == EeThreadStatus::Waiting || candidate.status == EeThreadStatus::WaitingSuspended) &&
            candidate.wait.reason == EeWaitReason::VSync &&
            std::get<EeVSyncWait>(candidate.wait.payload).afterTick < tick)
        {
            completed.push_back(id);
        }
    }
    std::sort(completed.begin(), completed.end());
    for (const int id : completed)
    {
        GuestThread *waiter = thread(id);
        assert(waiter != nullptr);
        const EeVSyncWait wait = std::get<EeVSyncWait>(waiter->wait.payload);
        const int result = wait.fixedResult >= 0
                               ? wait.fixedResult
                               : static_cast<int>((tick - 1u) & 1u);
        makeReady(*waiter, result, false);
    }
    publishSnapshot();
}

void EeScheduler::completeExternalWait(uint32_t type, uint64_t token, int result)
{
    assertExecutor();
    std::vector<int> completed;
    for (const auto &[id, candidate] : m_threads)
    {
        if ((candidate.status != EeThreadStatus::Waiting && candidate.status != EeThreadStatus::WaitingSuspended) ||
            (candidate.wait.reason != EeWaitReason::External &&
             candidate.wait.reason != EeWaitReason::Mpeg))
        {
            continue;
        }
        const auto &external = std::get<EeExternalWait>(candidate.wait.payload);
        if (external.type == type && external.token == token)
        {
            completed.push_back(id);
        }
    }
    std::sort(completed.begin(), completed.end());
    for (const int id : completed)
    {
        GuestThread *waiter = thread(id);
        assert(waiter != nullptr);
        makeReady(*waiter, result, false);
    }
    publishSnapshot();
}

[[noreturn]] void EeScheduler::waitExternal(EeWaitReason reason,
                                            uint32_t type,
                                            uint64_t token,
                                            std::function<void(R5900Context &)> completion)
{
    EeWaitState wait{reason, EeExternalWait{type, token}, std::move(completion)};
    blockCurrent(std::move(wait));
}

GuestThread *EeScheduler::thread(int id)
{
    auto it = m_threads.find(id);
    return it == m_threads.end() ? nullptr : &it->second;
}

const GuestThread *EeScheduler::thread(int id) const
{
    auto it = m_threads.find(id);
    return it == m_threads.end() ? nullptr : &it->second;
}

EeSemaphore *EeScheduler::semaphore(int id)
{
    auto it = m_semaphores.find(id);
    return it == m_semaphores.end() ? nullptr : &it->second;
}

const EeSemaphore *EeScheduler::semaphore(int id) const
{
    auto it = m_semaphores.find(id);
    return it == m_semaphores.end() ? nullptr : &it->second;
}

EeEventFlag *EeScheduler::eventFlag(int id)
{
    auto it = m_eventFlags.find(id);
    return it == m_eventFlags.end() ? nullptr : &it->second;
}

const EeEventFlag *EeScheduler::eventFlag(int id) const
{
    auto it = m_eventFlags.find(id);
    return it == m_eventFlags.end() ? nullptr : &it->second;
}

GuestThread *EeScheduler::currentThread()
{
    return thread(m_currentThreadId);
}

const GuestThread *EeScheduler::currentThread() const
{
    return thread(m_currentThreadId);
}

int EeScheduler::currentThreadId() const noexcept
{
    return m_currentThreadId;
}

R5900Context *EeScheduler::currentContext()
{
    GuestThread *self = currentThread();
    return self ? &self->activeContext() : nullptr;
}

uint8_t *EeScheduler::rdram() const noexcept
{
    return m_rdram;
}

void EeScheduler::bindMainContextForSyscall(R5900Context &ctx, uint8_t *rdram)
{
    if (m_executorThread == std::thread::id{})
    {
        reset(rdram, ctx);
        GuestThread *main = selectReady();
        assert(main != nullptr);
        makeRunning(*main);
        return;
    }
    assertExecutor();
    m_rdram = rdram;
    if (m_currentThreadId == 0)
    {
        GuestThread *main = thread(kMainThreadId);
        assert(main != nullptr);
        assert(main->status == EeThreadStatus::Ready);
        removeReady(*main);
        makeRunning(*main);
    }
}

EeKernelSnapshot EeScheduler::snapshot() const
{
    std::lock_guard lock(m_snapshotMutex);
    return m_snapshot;
}

void EeScheduler::publishSnapshot()
{
    EeKernelSnapshot next{};
    next.sequence = ++m_snapshotSequence;
    next.eeCycle = m_eeCycle;
    next.sliceEndCycle = m_sliceEndCycle;
    next.nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
    next.runningThreadId = m_currentThreadId;
    next.threads.reserve(m_threads.size());
    for (const auto &[id, item] : m_threads)
    {
        if (id < 0)
        {
            continue;
        }
        EeThreadSnapshot snapshot{};
        snapshot.id = id;
        const R5900Context &context = item.activeContext();
        snapshot.pc = context.pc;
        snapshot.ra = getRegU32(&context, 31);
        snapshot.sp = getRegU32(&context, 29);
        snapshot.contextGp = getRegU32(&context, 28);
        snapshot.entry = item.entry;
        snapshot.stack = item.stack;
        snapshot.stackSize = item.stackSize;
        snapshot.gp = item.gp;
        snapshot.initialPriority = item.initialPriority;
        snapshot.currentPriority = item.currentPriority;
        snapshot.status = item.status;
        snapshot.waitReason = item.wait.reason;
        snapshot.waitId = waitObjectId(item.wait);
        snapshot.suspendCount = item.suspendCount;
        snapshot.wakeupCount = item.wakeupCount;
        snapshot.invocationDepth = static_cast<uint32_t>(item.invocations.size());
        next.threads.push_back(snapshot);
    }
    std::sort(next.threads.begin(), next.threads.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    next.semaphores.reserve(m_semaphores.size());
    for (const auto &[id, item] : m_semaphores)
    {
        next.semaphores.push_back(EeSemaphoreSnapshot{id,
                                                      item.count,
                                                      item.maxCount,
                                                      static_cast<uint32_t>(item.waiters.size())});
    }
    std::sort(next.semaphores.begin(), next.semaphores.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    next.eventFlags.reserve(m_eventFlags.size());
    for (const auto &[id, item] : m_eventFlags)
    {
        next.eventFlags.push_back(EeEventFlagSnapshot{id,
                                                      item.bits,
                                                      item.initBits,
                                                      item.attr,
                                                      static_cast<uint32_t>(item.waiters.size())});
    }
    std::sort(next.eventFlags.begin(), next.eventFlags.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    {
        std::lock_guard lock(m_snapshotMutex);
        m_snapshot = std::move(next);
    }
}

void EeScheduler::assertExecutor() const
{
    assert(m_executorThread == std::this_thread::get_id());
}

int EeScheduler::allocateThreadId()
{
    for (int attempts = 0; attempts <= kLastThreadId - kFirstThreadId; ++attempts)
    {
        const int candidate = m_nextThreadId;
        m_nextThreadId = candidate == kLastThreadId ? kFirstThreadId : candidate + 1;
        if (!m_threads.contains(candidate))
        {
            return candidate;
        }
    }
    return 0;
}

GuestThread &EeScheduler::acquireInvocationThread()
{
    for (auto &[id, candidate] : m_threads)
    {
        if (id < 0 && candidate.status == EeThreadStatus::Dormant && candidate.invocations.empty())
        {
            return candidate;
        }
    }

    GuestThread dispatcher{};
    dispatcher.id = m_nextInvocationThreadId--;
    dispatcher.initialPriority = 0;
    dispatcher.currentPriority = 0;
    dispatcher.status = EeThreadStatus::Dormant;
    return m_threads.emplace(dispatcher.id, std::move(dispatcher)).first->second;
}

void EeScheduler::enqueueReady(GuestThread &item, bool front)
{
    assert(item.currentPriority >= 0 && item.currentPriority < kPriorityCount);
    item.status = EeThreadStatus::Ready;
    auto &queue = m_readyQueues[item.currentPriority];
    if (front)
    {
        queue.push_front(item.id);
    }
    else
    {
        queue.push_back(item.id);
    }
}

void EeScheduler::removeReady(GuestThread &item)
{
    if (item.status != EeThreadStatus::Ready)
    {
        return;
    }
    auto &queue = m_readyQueues[item.currentPriority];
    auto it = std::find(queue.begin(), queue.end(), item.id);
    assert(it != queue.end());
    queue.erase(it);
}

GuestThread *EeScheduler::selectReady()
{
    for (auto &queue : m_readyQueues)
    {
        if (queue.empty())
        {
            continue;
        }
        const int id = queue.front();
        queue.pop_front();
        GuestThread *selected = thread(id);
        assert(selected != nullptr);
        assert(selected->status == EeThreadStatus::Ready);
        return selected;
    }
    return nullptr;
}

void EeScheduler::makeRunning(GuestThread &item)
{
    assert(m_currentThreadId == 0);
    assert(item.status == EeThreadStatus::Ready);
    item.status = EeThreadStatus::Running;
    m_currentThreadId = item.id;
    renewTimeSlice();
}

void EeScheduler::makeDormant(GuestThread &item)
{
    removeReady(item);
    removeFromWaitObject(item);
    item.status = EeThreadStatus::Dormant;
    item.wait = {};
    item.resumeCompletion = {};
    item.suspendCount = 0;
    item.wakeupCount = 0;
    item.invocations.clear();
}

void EeScheduler::removeFromWaitObject(GuestThread &item)
{
    const int id = item.id;
    if (item.wait.reason == EeWaitReason::Semaphore)
    {
        const int objectId = std::get<EeSemaphoreWait>(item.wait.payload).id;
        if (EeSemaphore *object = semaphore(objectId))
        {
            auto it = std::find(object->waiters.begin(), object->waiters.end(), id);
            if (it != object->waiters.end())
            {
                object->waiters.erase(it);
            }
        }
    }
    else if (item.wait.reason == EeWaitReason::EventFlag)
    {
        const int objectId = std::get<EeEventFlagWait>(item.wait.payload).id;
        if (EeEventFlag *object = eventFlag(objectId))
        {
            auto it = std::find(object->waiters.begin(), object->waiters.end(), id);
            if (it != object->waiters.end())
            {
                object->waiters.erase(it);
            }
        }
    }
    item.wait = {};
}

void EeScheduler::blockCurrent(EeWaitState wait)
{
    GuestThread *self = currentThread();
    assert(self != nullptr);
    self->wait = std::move(wait);
    self->status = self->suspendCount == 0 ? EeThreadStatus::Waiting : EeThreadStatus::WaitingSuspended;
    m_currentThreadId = 0;
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

void EeScheduler::makeReady(GuestThread &item, int result, bool interruptSafe)
{
    auto completion = std::move(item.wait.completion);
    item.wait = {};
    setReturnS32(&item.activeContext(), result);
    item.resumeCompletion = std::move(completion);
    if (item.suspendCount != 0)
    {
        item.status = EeThreadStatus::Suspended;
        return;
    }
    enqueueReady(item);
    requestPreemptionIfHigher(item, interruptSafe);
}

void EeScheduler::requestPreemptionIfHigher(const GuestThread &readyThread, bool interruptSafe)
{
    const GuestThread *running = currentThread();
    if (!running || readyThread.currentPriority >= running->currentPriority)
    {
        return;
    }
    m_rescheduleRequested = true;
    if (interruptSafe || m_insideInterrupt)
    {
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

void EeScheduler::applyPendingPreemption()
{
    if (!m_rescheduleRequested)
    {
        return;
    }
    if (m_currentThreadId == 0)
    {
        m_rescheduleRequested = false;
        m_timeSliceExpired = false;
        return;
    }
    GuestThread *self = currentThread();
    assert(self != nullptr);
    enqueueReady(*self, !m_timeSliceExpired);
    m_currentThreadId = 0;
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
}

void EeScheduler::processPendingEvents()
{
    assertExecutor();
    processDueDeadlines();
    const uint32_t timerInterrupts = m_pendingEeTimerInterrupts;
    m_pendingEeTimerInterrupts = 0u;
    for (uint32_t timer = 0u; timer < 4u; ++timer)
    {
        if ((timerInterrupts & (1u << timer)) != 0u)
        {
            dispatchIrq(false, 9u + timer);
        }
    }
    std::deque<EeEvent> pending;
    {
        std::lock_guard lock(m_eventMutex);
        pending.swap(m_events);
    }
    for (const EeEvent &event : pending)
    {
        processEvent(event);
    }

    {
        std::lock_guard lock(m_eventMutex);
        const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
        const bool cycleEventDue = nextEventCycle != 0u && m_eeCycle >= nextEventCycle;
        const bool pendingWork = !m_events.empty() || cycleEventDue || m_stopRequested.load(std::memory_order_acquire);
        m_checkpointPending.store(pendingWork, std::memory_order_release);
    }
    applyPendingPreemption();
}

// PS2X_LATE_VBLANK=1 logs every field emitted more than 3 ms after its wall-clock time ([late-vblank], used by
// tools/heavy_battle_test.py), with the time since the previous deadline service.
static std::chrono::steady_clock::time_point g_lastDeadlineService{};
static std::chrono::steady_clock::time_point g_prevDeadlineService{};
void EeScheduler::processDueDeadlines()
{
    g_prevDeadlineService = g_lastDeadlineService;
    g_lastDeadlineService = std::chrono::steady_clock::now();
    for (;;)
    {
        std::vector<ScheduledEvent> due;
        std::chrono::steady_clock::time_point pacingDeadline{};
        {
            std::unique_lock lock(m_eventMutex);
            const auto now = std::chrono::steady_clock::now();
            uint64_t pacedHardwareCycle = std::numeric_limits<uint64_t>::max();
            for (const ScheduledEvent &item : m_deadlines)
            {
                if ((item.event.type == EeEventType::VBlankStart ||
                     item.event.type == EeEventType::VBlankEnd) &&
                    item.hostDeadline <= now)
                {
                    pacedHardwareCycle = std::min(pacedHardwareCycle, item.deadlineCycle);
                }
            }
            if (pacedHardwareCycle != std::numeric_limits<uint64_t>::max() &&
                pacedHardwareCycle > m_eeCycle)
            {
                // Native SDK replacements omit the instruction loops which
                // consumed time on the EE. Ready polling threads must not keep
                // the independent display clock waiting for those instructions.
                // Advance only to the earliest wall-due field event, through
                // the same timer/IOP accounting used by idle deadline waits.
                // Retain the existing emission gates and exact field offsets.
                uint64_t remaining = pacedHardwareCycle - m_eeCycle;
                lock.unlock();
                while (remaining > 0u)
                {
                    const uint32_t step = static_cast<uint32_t>(std::min<uint64_t>(
                        remaining, std::numeric_limits<uint32_t>::max()));
                    accountCycles(step);
                    remaining -= step;
                }
                continue;
            }
            for (const ScheduledEvent &item : m_deadlines)
            {
                if (item.deadlineCycle <= m_eeCycle &&
                    (pacingDeadline == std::chrono::steady_clock::time_point{} ||
                     item.hostDeadline < pacingDeadline))
                {
                    pacingDeadline = item.hostDeadline;
                }
            }

            if (pacingDeadline == std::chrono::steady_clock::time_point{})
            {
                updateNextDeadline();
                return;
            }

            if (now < pacingDeadline)
            {
                m_eventCv.wait_until(lock, pacingDeadline, [this]()
                                     { return !m_events.empty() ||
                                              m_stopRequested.load(std::memory_order_acquire); });
                if (!m_events.empty() || m_stopRequested.load(std::memory_order_acquire))
                {
                    updateNextDeadline();
                    return;
                }
            }

            const auto pacedNow = std::chrono::steady_clock::now();
            auto firstFuture = std::partition(m_deadlines.begin(), m_deadlines.end(),
                                              [this, pacedNow](const ScheduledEvent &item)
                                              { return item.deadlineCycle <= m_eeCycle &&
                                                       item.hostDeadline <= pacedNow; });
            due.insert(due.end(),
                       std::make_move_iterator(m_deadlines.begin()),
                       std::make_move_iterator(firstFuture));
            m_deadlines.erase(m_deadlines.begin(), firstFuture);
            updateNextDeadline();
        }

        std::sort(due.begin(), due.end(), [](const ScheduledEvent &left, const ScheduledEvent &right)
                  {
                      if (left.deadlineCycle != right.deadlineCycle)
                      {
                          return left.deadlineCycle < right.deadlineCycle;
                      }
                      if (left.event.type != right.event.type)
                      {
                          return left.event.type < right.event.type;
                      }
                      if (left.event.id != right.event.id)
                      {
                          return left.event.id < right.event.id;
                      }
                      return left.sequence < right.sequence; });

        if (due.empty())
        {
            return;
        }

        for (ScheduledEvent &scheduled : due)
        {
            if (scheduled.event.type == EeEventType::VBlankStart)
            {
                {
                    static const bool on = [] { const char *v = std::getenv("PS2X_LATE_VBLANK"); return v && *v == '1'; }();
                    const auto lateUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - scheduled.hostDeadline).count();
                    if (on && lateUs > 3000) {
                        const auto gapUs = g_prevDeadlineService == std::chrono::steady_clock::time_point{} ? -1 : std::chrono::duration_cast<std::chrono::microseconds>(g_lastDeadlineService - g_prevDeadlineService).count();
                        std::fprintf(stderr, "[late-vblank] tick=%llu late=%lldus serviceGap=%lldus thread=%u\n", (unsigned long long)(m_vsyncTick + 1u), (long long)lateUs, (long long)gapUs, m_currentThreadId);
                    }
                }
                // Keep host fields on a fixed cadence so small wake-up lateness
                // is repaid (otherwise guest time drifts ~0.6% behind the audio
                // clock and streamed audio starves). Only a guest that is
                // genuinely behind by several fields re-anchors to now, so later
                // VSync waits still do not repay a long stall in a burst.
                const auto pacedNow = std::chrono::steady_clock::now();
                const auto hostFieldStart = pacedNow - scheduled.hostDeadline > 3 * vblankPeriod(palVideoMode())
                                                ? pacedNow
                                                : scheduled.hostDeadline;
                scheduleEvent(scheduled.deadlineCycle + kVBlankDurationCycles,
                              hostFieldStart + kVBlankDuration,
                              EeEvent{EeEventType::VBlankEnd, 0, m_vsyncTick + 1u});
                scheduleEvent(scheduled.deadlineCycle + vblankPeriodCycles(palVideoMode()),
                              hostFieldStart + vblankPeriod(palVideoMode()),
                              EeEvent{EeEventType::VBlankStart, 0, 0});
            }
            processEvent(scheduled.event);
        }
    }
}

static unsigned g_dw5xlCbQueued = 0; // DW5XL-DEBUG
extern std::atomic<uint32_t> g_dw5xlMpegDemux, g_dw5xlMpegGetPic, g_dw5xlMpegPictures, g_dw5xlStrEvents[8];

void EeScheduler::processEvent(const EeEvent &event)
{
    switch (event.type)
    {
    case EeEventType::Stop:
        requestStop();
        break;
    case EeEventType::VBlankStart:
        ++m_vsyncTick;
        ps2prof::record(ps2prof::Event::FieldStart, m_vsyncTick); // DW5XL-DEBUG issue 025
        m_runtime.memory().vsyncTickNs().store(std::chrono::steady_clock::now().time_since_epoch().count(),
                                                  std::memory_order_relaxed);
        m_runtime.memory().gs().vsyncTick.store(m_vsyncTick, std::memory_order_release);
        if ((m_vsyncTick & 1u) != 0u)
        {
            m_runtime.memory().gs().csr.fetch_or(0x2000ull, std::memory_order_acq_rel);
        }
        else
        {
            m_runtime.memory().gs().csr.fetch_and(~0x2000ull, std::memory_order_acq_rel);
        }
        writeGuestU32(m_vsyncFlagAddress, 1u);
        if (m_vsyncTickAddress != 0u)
        {
            const uint32_t physical = m_vsyncTickAddress & 0x1FFFFFFFu;
            if (m_rdram && physical <= PS2_RAM_SIZE - sizeof(uint64_t))
            {
                std::memcpy(m_rdram + physical, &m_vsyncTick, sizeof(m_vsyncTick));
            }
        }
        m_vsyncFlagAddress = 0u;
        m_vsyncTickAddress = 0u;
        // DW5XL-DEBUG (temporary): movie handshake state every 120 vsyncs.
        if (uint8_t *ram = m_runtime.memory().getRDRAM(); ram && (m_vsyncTick % 120u) == 0u)
        {
            auto rd = [&](uint32_t a) { uint32_t v = 0; std::memcpy(&v, ram + (a & 0x1FFFFFFu), 4); return v; };
            const uint32_t base = rd(0x3591B8u);
            const uint32_t head = rd(0x3697D0u);
            std::fprintf(stderr, "[dw5xl:movie] queued_cb=%u vsync=%llu cb=0x%x base=0x%x active=0x%x flag=0x%x list=0x%x",
                         g_dw5xlCbQueued, static_cast<unsigned long long>(m_vsyncTick), m_gsVSyncCallback, base,
                         base ? rd(base + 0x912C8u) : 0u, base ? rd(base + 0x912C0u) : 0u, head);
            for (uint32_t n = head, i = 0; n && i < 6; n = rd(n), ++i)
                std::fprintf(stderr, " [fn=0x%x]", rd(n + 4u));
            if (base)
            {
                for (uint32_t off : {0x91210u, 0x91268u})
                {
                    const uint32_t b = base + off;
                    std::fprintf(stderr, " buf%x{0=%x 4=%x 34=%x 38=%x 3c=%x 40=%x 48=%x 4c=%x 50=%x 54=%x}", off, rd(b), rd(b + 4),
                                 rd(b + 0x34), rd(b + 0x38), rd(b + 0x3c), rd(b + 0x40), rd(b + 0x48), rd(b + 0x4c), rd(b + 0x50),
                                 rd(b + 0x54));
                }
                const uint32_t vr = base + 0x911A0u; // DW5XL-DEBUG video ES ring (FUN_0013d018: c*0x800 + 14)
                std::fprintf(stderr, " vring{10=%x 14=%x 8=%x c=%x}", rd(vr + 0x10), rd(vr + 0x14), rd(vr + 8), rd(vr + 0xC));
                const uint32_t q = base + 0x91144u;
                std::fprintf(stderr, " picq{c=%x 10=%x 8=%x}", rd(q + 0xC), rd(q + 0x10), rd(q + 8));
            }
            for (auto &[tid, t] : m_threads) // DW5XL-DEBUG thread states
            {
                const R5900Context &ac = t.invocations.empty() ? t.context : t.invocations.back().context;
                std::fprintf(stderr, " T%d{st=%d w=%d id=%d inv=%zu pc=%x ra=%x}", tid, static_cast<int>(t.status),
                             static_cast<int>(t.wait.reason), (std::holds_alternative<EeSemaphoreWait>(t.wait.payload) ? static_cast<int>(std::get<EeSemaphoreWait>(t.wait.payload).id) : -1), t.invocations.size(), ac.pc,
                             static_cast<uint32_t>(getRegU32(&ac, 31)));
                if (const char *trace = std::getenv("PS2X_THREAD_BASE_TRACE"); trace && *trace && *trace != '0')
                    std::fprintf(stderr, " base{pc=%x ra=%x sp=%x pri=%d}", t.context.pc,
                                 getRegU32(&t.context, 31), getRegU32(&t.context, 29), t.currentPriority);
            }
            std::fprintf(stderr, " demux=%u getpic=%u pictures=%u ev[v=%u a2=%u a3=%u]\n", g_dw5xlMpegDemux.load(),
                         g_dw5xlMpegGetPic.load(), g_dw5xlMpegPictures.load(), g_dw5xlStrEvents[0].load(),
                         g_dw5xlStrEvents[2].load(), g_dw5xlStrEvents[3].load());
        }
        completeVSync(m_vsyncTick);
        if (m_gsVSyncCallback != 0u && m_runtime.hasFunction(m_gsVSyncCallback))
        {
            GuestInvocation invocation{};
            invocation.kind = GuestInvocationKind::GsCallback;
            ++g_dw5xlCbQueued; // DW5XL-DEBUG
            invocation.context.pc = m_gsVSyncCallback;
            SET_GPR_U32(&invocation.context, 4, static_cast<uint32_t>(m_vsyncTick));
            SET_GPR_U32(&invocation.context, 28, m_gsVSyncCallbackGp);
            SET_GPR_U32(&invocation.context, 29, m_gsVSyncCallbackSp);
            SET_GPR_U32(&invocation.context, 31, 0u);
            queueInvocation(std::move(invocation));
        }
        dispatchIrq(false, 2u);
        break;
    case EeEventType::ExternalWake:
        completeExternalWait(event.id, event.value, KE_OK);
        break;
    case EeEventType::VBlankEnd:
        dispatchIrq(false, 3u);
        {
            std::lock_guard lock(m_presentationMutex);
            m_presentationTick.store(m_vsyncTick.load(std::memory_order_relaxed), std::memory_order_release);
        }
        m_presentationCv.notify_all();
        break;
    case EeEventType::Dmac:
        break;
    case EeEventType::Alarm:
    {
        auto it = m_alarms.find(static_cast<int>(event.id));
        if (it == m_alarms.end())
        {
            break;
        }
        const EeAlarm alarm = it->second;
        m_alarms.erase(it);
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::Alarm;
        invocation.context.pc = alarm.handler;
        SET_GPR_U32(&invocation.context, 4, static_cast<uint32_t>(alarm.id));
        SET_GPR_U32(&invocation.context, 5, static_cast<uint32_t>(alarm.ticks));
        SET_GPR_U32(&invocation.context, 6, alarm.argument);
        SET_GPR_U32(&invocation.context, 28, alarm.gp);
        SET_GPR_U32(&invocation.context, 29, 0u);
        SET_GPR_U32(&invocation.context, 31, 0u);
        queueInvocation(std::move(invocation));
        break;
    }
    }
}

void EeScheduler::finishEventWaiters(EeEventFlag &flag, bool interruptSafe)
{
    for (auto it = flag.waiters.begin(); it != flag.waiters.end();)
    {
        GuestThread *waiter = thread(*it);
        assert(waiter != nullptr);
        const EeEventFlagWait wait = std::get<EeEventFlagWait>(waiter->wait.payload);
        if (!eventCondition(flag.bits, wait.bits, wait.mode))
        {
            ++it;
            continue;
        }
        const uint32_t observed = flag.bits;
        writeGuestU32(wait.resultAddress, observed);
        if ((wait.mode & WEF_CLEAR_ALL) != 0u)
        {
            flag.bits = 0;
        }
        else if ((wait.mode & WEF_CLEAR) != 0u)
        {
            flag.bits &= ~wait.bits;
        }
        it = flag.waiters.erase(it);
        makeReady(*waiter, KE_OK, interruptSafe);
    }
}

bool EeScheduler::eventCondition(uint32_t current, uint32_t requested, uint32_t mode)
{
    return (mode & WEF_OR) != 0u ? (current & requested) != 0u
                                 : (current & requested) == requested;
}

int EeScheduler::waitObjectId(const EeWaitState &wait)
{
    switch (wait.reason)
    {
    case EeWaitReason::Semaphore:
        return std::get<EeSemaphoreWait>(wait.payload).id;
    case EeWaitReason::EventFlag:
        return std::get<EeEventFlagWait>(wait.payload).id;
    default:
        return 0;
    }
}

void EeScheduler::writeGuestU32(uint32_t address, uint32_t value)
{
    if (address == 0u)
    {
        return;
    }
    const uint32_t physical = address & 0x1FFFFFFFu;
    if (!m_rdram || physical > PS2_RAM_SIZE - sizeof(value))
    {
        return;
    }
    std::memcpy(m_rdram + physical, &value, sizeof(value));
}

void EeScheduler::waitForEvent()
{
    std::unique_lock lock(m_eventMutex);
    if (!m_events.empty() || m_stopRequested.load(std::memory_order_acquire))
    {
        return;
    }
    const uint64_t timerCycles = m_runtime.memory().cyclesUntilNextEeTimerInterrupt();
    const bool hasTimerDeadline = timerCycles != std::numeric_limits<uint64_t>::max();
    if (m_deadlines.empty() && !hasTimerDeadline)
    {
        m_eventCv.wait(lock, [this]()
                       { return !m_events.empty() || m_stopRequested.load(std::memory_order_acquire); });
        return;
    }

    uint64_t deadlineCycle = 0u;
    auto hostDeadline = std::chrono::steady_clock::time_point::max();
    if (!m_deadlines.empty())
    {
        const auto next = std::min_element(m_deadlines.begin(), m_deadlines.end(),
                                           [](const ScheduledEvent &left, const ScheduledEvent &right)
                                           {
                                               if (left.deadlineCycle != right.deadlineCycle)
                                               {
                                                   return left.deadlineCycle < right.deadlineCycle;
                                               }
                                               return left.sequence < right.sequence;
                                           });
        deadlineCycle = next->deadlineCycle;
        hostDeadline = next->hostDeadline;
    }
    if (hasTimerDeadline)
    {
        const auto timerHostDeadline = std::chrono::steady_clock::now() + eeCyclesToHostDuration(timerCycles);
        if (timerHostDeadline < hostDeadline)
        {
            deadlineCycle = m_eeCycle + timerCycles;
            hostDeadline = timerHostDeadline;
        }
    }

    const bool signaled = m_eventCv.wait_until(lock, hostDeadline, [this]()
                                               { return !m_events.empty() ||
                                                        m_stopRequested.load(std::memory_order_acquire); });
    if (!signaled)
    {
        const uint64_t elapsed = deadlineCycle > m_eeCycle ? deadlineCycle - m_eeCycle : 0u;
        lock.unlock();
        uint64_t remaining = elapsed;
        while (remaining > 0u)
        {
            const uint32_t step = static_cast<uint32_t>(std::min<uint64_t>(remaining, std::numeric_limits<uint32_t>::max()));
            accountCycles(step);
            remaining -= step;
        }
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

void EeScheduler::scheduleEvent(uint64_t deadlineCycle,
                                std::chrono::steady_clock::time_point hostDeadline,
                                EeEvent event)
{
    {
        std::lock_guard lock(m_eventMutex);
        m_deadlines.push_back(ScheduledEvent{deadlineCycle, hostDeadline, event, ++m_eventSequence});
        updateNextDeadline();
    }
    m_eventCv.notify_one();
}

void EeScheduler::updateNextDeadline()
{
    int64_t fieldDeadline = 0;
    for (const ScheduledEvent &item : m_deadlines)
    {
        if (item.event.type == EeEventType::VBlankStart || item.event.type == EeEventType::VBlankEnd)
        {
            const int64_t ns = steadyNs(item.hostDeadline);
            fieldDeadline = fieldDeadline == 0 ? ns : std::min(fieldDeadline, ns);
        }
    }
    g_nextFieldHostDeadlineNs.store(fieldDeadline, std::memory_order_release);
    if (m_deadlines.empty())
    {
        m_nextDeadlineCycle.store(0u, std::memory_order_release);
        return;
    }
    const auto it = std::min_element(m_deadlines.begin(), m_deadlines.end(),
                                     [](const ScheduledEvent &left, const ScheduledEvent &right)
                                     {
                                         if (left.deadlineCycle != right.deadlineCycle)
                                         {
                                             return left.deadlineCycle < right.deadlineCycle;
                                         }
                                         return left.sequence < right.sequence;
                                     });
    m_nextDeadlineCycle.store(it->deadlineCycle, std::memory_order_release);
}

bool EeScheduler::hasReadyAtOrAbovePriority(int priority) const
{
    const int last = std::clamp(priority, 0, kPriorityCount - 1);
    for (int p = 0; p <= last; ++p)
    {
        if (!m_readyQueues[static_cast<size_t>(p)].empty())
        {
            return true;
        }
    }
    return false;
}

void EeScheduler::renewTimeSlice()
{
    m_sliceEndCycle = m_eeCycle + kDefaultTimeSliceCycles;
    m_timeSliceExpired = false;
}

void EeScheduler::copyMainContextToRuntime()
{
    const GuestThread *main = thread(kMainThreadId);
    if (main)
    {
        m_runtime.m_cpuContext = main->context;
    }
}

void EeScheduler::publishDebugContext(const R5900Context &context)
{
    m_runtime.m_debugPc.store(context.pc, std::memory_order_relaxed);
    m_runtime.m_debugRa.store(getRegU32(&context, 31), std::memory_order_relaxed);
    m_runtime.m_debugSp.store(getRegU32(&context, 29), std::memory_order_relaxed);
    m_runtime.m_debugGp.store(getRegU32(&context, 28), std::memory_order_relaxed);
}

void EeScheduler::publishIdleDebugContext()
{
    // Temporary IRQ/RPC/alarm invocations deliberately return to PC=0. Once
    // the scheduler is idle, show a real EE thread context instead of leaving
    // the debugger pinned to that completed dispatcher frame.
    const GuestThread *selected = thread(kMainThreadId);
    if (!selected)
    {
        for (const auto &[id, candidate] : m_threads)
        {
            if (id > 0 && candidate.status != EeThreadStatus::Dormant)
            {
                selected = &candidate;
                break;
            }
        }
    }

    if (selected)
    {
        publishDebugContext(selected->activeContext());
    }
}

// ---- DW5XL (issue 027): save states --------------------------------------------------------------------------------

void EeScheduler::requestState(StateRequestKind kind, std::string path)
{
    {
        std::lock_guard lock(m_stateRequestMutex);
        m_stateRequestKind = kind;
        m_stateRequestPath = std::move(path);
        m_stateRequestPending.store(kind != StateRequestKind::None, std::memory_order_release);
    }
    std::lock_guard lock(m_eventMutex); // wake an idle executor so it reaches the top of run()
    m_eventCv.notify_all();
}

bool EeScheduler::stateQuiet(std::string *reason) const
{
    auto fail = [&](std::string why)
    {
        if (reason)
            *reason = std::move(why);
        return false;
    };
    if (!m_events.empty())
        return fail("events are queued");
    for (const GuestInvocation &invocation : m_pendingInvocations)
        if (invocation.onComplete || invocation.shouldExecute)
            return fail("an invocation with a host callback is queued");
    for (const auto &[id, thread] : m_threads)
    {
        if (!thread.invocations.empty())
            return fail("thread " + std::to_string(id) + " is inside an interrupt, alarm or callback");
        if (thread.resumeCompletion)
            return fail("thread " + std::to_string(id) + " has a resume completion");
        if (thread.wait.completion)
            return fail("thread " + std::to_string(id) + " waits with a host completion");
    }
    if (m_insideInterrupt)
        return fail("inside an interrupt");
    return true;
}

bool EeScheduler::serviceStateRequest()
{
    const uint64_t tick = m_vsyncTick.load(std::memory_order_acquire);
    if (m_stateSaveAtTick != 0u && tick >= m_stateSaveAtTick)
    {
        m_stateSaveAtTick = 0u;
        requestState(StateRequestKind::Save, m_stateSaveAtPath);
    }
    if (m_stateLoadAtTick != 0u && tick >= m_stateLoadAtTick)
    {
        m_stateLoadAtTick = 0u;
        requestState(StateRequestKind::Load, m_stateLoadAtPath);
    }
    if (!m_stateRequestPending.load(std::memory_order_acquire))
        return false;
    static std::string s_lastRefusal;
    std::string reason;
    if (!stateQuiet(&reason))
    {
        if (reason != s_lastRefusal) // say once per reason, not every iteration
            std::fprintf(stderr, "[state] waiting for a quiet point at tick %llu: %s\n",
                         static_cast<unsigned long long>(tick), reason.c_str());
        s_lastRefusal = reason;
        return false;
    }
    s_lastRefusal.clear();
    StateRequestKind kind;
    std::string path;
    {
        std::lock_guard lock(m_stateRequestMutex);
        kind = m_stateRequestKind;
        path = m_stateRequestPath;
        m_stateRequestKind = StateRequestKind::None;
        m_stateRequestPending.store(false, std::memory_order_release);
    }
    bool retry = false;
    const bool ok = m_runtime.performStateRequest(kind == StateRequestKind::Save, path, retry);
    if (retry)
    {
        std::lock_guard lock(m_stateRequestMutex); // the drain made new work: handle it, come back next pass
        if (!m_stateRequestPending.load(std::memory_order_acquire))
        {
            m_stateRequestKind = kind;
            m_stateRequestPath = path;
            m_stateRequestPending.store(true, std::memory_order_release);
        }
        return false;
    }
    return ok && kind == StateRequestKind::Load;
}

void EeScheduler::serializeState(ps2x::StateArchive &a)
{
    std::string reason;
    if (a.saving() && !stateQuiet(&reason))
        throw ps2x::StateError("EE scheduler is not at a quiet point: " + reason);

    auto context = [](ps2x::StateArchive &x, R5900Context &c) { x.value(c); };
    auto wait = [](ps2x::StateArchive &x, EeWaitState &w)
    {
        x.value(w.reason);
        uint32_t kind = static_cast<uint32_t>(w.payload.index());
        x.value(kind);
        if (x.loading())
        {
            switch (kind)
            {
            case 0: w.payload = std::monostate{}; break;
            case 1: w.payload = EeSemaphoreWait{}; break;
            case 2: w.payload = EeEventFlagWait{}; break;
            case 3: w.payload = EeVSyncWait{}; break;
            case 4: w.payload = EeExternalWait{}; break;
            default: throw ps2x::StateError("unknown EE wait kind");
            }
            w.completion = {};
        }
        std::visit([&](auto &payload)
        {
            if constexpr (!std::is_same_v<std::decay_t<decltype(payload)>, std::monostate>)
                x.value(payload);
        }, w.payload);
    };
    auto invocation = [&](ps2x::StateArchive &x, GuestInvocation &inv)
    {
        x.value(inv.kind);
        x.value(inv.sequence);
        x.value(inv.tag);
        context(x, inv.context);
        if (x.loading())
        {
            inv.shouldExecute = {};
            inv.onComplete = {};
        }
    };

    a.tag("EESC");
    a.map(m_threads, [&](ps2x::StateArchive &x, GuestThread &t)
    {
        x.value(t.id);
        context(x, t.context);
        x.value(t.entry);
        x.value(t.stack);
        x.value(t.stackSize);
        x.value(t.gp);
        x.value(t.attr);
        x.value(t.option);
        x.value(t.arg);
        x.value(t.initialPriority);
        x.value(t.currentPriority);
        x.value(t.status);
        x.value(t.suspendCount);
        x.value(t.wakeupCount);
        x.value(t.ownsStack);
        x.value(t.tlsBase);
        wait(x, t.wait);
        if (x.loading())
            t.resumeCompletion = {};
        x.sequence(t.invocations, invocation); // empty at a quiet point
    });
    for (auto &queue : m_readyQueues)
        a.deque(queue);
    a.vector(m_sharedCop2);
    a.map(m_semaphores, [](ps2x::StateArchive &x, EeSemaphore &s)
    {
        x.value(s.id);
        x.value(s.count);
        x.value(s.maxCount);
        x.value(s.initCount);
        x.value(s.attr);
        x.value(s.option);
        x.deque(s.waiters);
    });
    a.map(m_eventFlags, [](ps2x::StateArchive &x, EeEventFlag &f)
    {
        x.value(f.id);
        x.value(f.attr);
        x.value(f.option);
        x.value(f.initBits);
        x.value(f.bits);
        x.deque(f.waiters);
    });
    a.podMap(m_alarms);
    a.podMap(m_intcHandlers);
    a.podMap(m_dmacHandlers);
    a.value(m_nextThreadId);
    a.value(m_nextInvocationThreadId);
    a.value(m_nextSemaphoreId);
    a.value(m_nextEventFlagId);
    a.value(m_nextAlarmId);
    a.value(m_nextIntcHandlerId);
    a.value(m_nextDmacHandlerId);
    a.value(m_intcHeadOrder);
    a.value(m_intcTailOrder);
    a.value(m_dmacHeadOrder);
    a.value(m_dmacTailOrder);
    a.value(m_enabledIntcMask);
    a.value(m_enabledDmacMask);
    a.value(m_currentThreadId);
    a.value(m_rescheduleRequested);
    a.value(m_timeSliceExpired);
    a.value(m_insideInterrupt);
    a.value(m_pendingEeTimerInterrupts);
    a.value(m_eeCycle);
    a.value(m_sliceEndCycle);
    a.deque(m_events); // empty at a quiet point
    // Deadlines: guest cycles only. The host (wall-clock) deadline is re-derived on load from the cycles still to
    // go, so saving the same state twice gives the same bytes.
    a.sequence(m_deadlines, [this](ps2x::StateArchive &x, ScheduledEvent &d)
    {
        x.value(d.deadlineCycle);
        x.value(d.event);
        x.value(d.sequence);
        if (x.loading())
        {
            const uint64_t remaining = d.deadlineCycle > m_eeCycle ? d.deadlineCycle - m_eeCycle : 0u;
            d.hostDeadline = std::chrono::steady_clock::now() + eeCyclesToHostDuration(remaining);
        }
    });
    {
        // Queued invocations without host callbacks (a quiet point allows no others).
        std::vector<GuestInvocation> pending(m_pendingInvocations.begin(), m_pendingInvocations.end());
        a.sequence(pending, invocation);
        if (a.loading())
            m_pendingInvocations.assign(pending.begin(), pending.end());
    }
    a.value(m_eventSequence);
    a.value(m_invocationSequence);
    uint64_t vsyncTick = m_vsyncTick.load(std::memory_order_acquire);
    uint64_t presentationTick = m_presentationTick.load(std::memory_order_acquire);
    bool pal = m_palVideoMode.load(std::memory_order_acquire);
    a.value(vsyncTick);
    a.value(presentationTick);
    a.value(pal);
    a.value(m_vsyncFlagAddress);
    a.value(m_vsyncTickAddress);
    a.value(m_gsVSyncCallback);
    a.value(m_gsVSyncCallbackGp);
    a.value(m_gsVSyncCallbackSp);
    a.podMap(m_invocationStackTops);
    a.tag("EEND");
    if (a.loading())
    {
        m_vsyncTick.store(vsyncTick, std::memory_order_release);
        m_presentationTick.store(presentationTick, std::memory_order_release);
        m_palVideoMode.store(pal, std::memory_order_release);
        m_checkpointPending.store(false, std::memory_order_release);
        updateNextDeadline();
        publishSnapshot();
    }
}
