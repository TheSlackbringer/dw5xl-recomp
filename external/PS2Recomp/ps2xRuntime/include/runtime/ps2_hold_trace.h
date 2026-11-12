#ifndef PS2_HOLD_TRACE_H
#define PS2_HOLD_TRACE_H

// DW5XL-DEBUG (issue 049, fact-finding 2026-11-12: what holds the game up at 4x with in-between pictures).
// PS2X_HOLD_TRACE=1: counters of where time goes around the GS thread. Nothing behaves differently with it; without
// it nothing is measured.
//  - the game's thread in a drain of its device: how long it waited for the device's worker, and for the GS thread;
//  - the GS thread: per packet of the game, per call in the game's GS order, per packet of a picture;
//  - the GS backend's device lock: who waited for it and who held it, by what the thread was doing;
//  - three pieces of the pictures' work on the GS thread: the copy of changed pages into the picture GS, a field's
//    scanout, a picture's read.
// The smoothing code reads them once per game frame, at the game's send ("[smooth:hold]").

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

namespace ps2x::hold
{
    inline bool enabled()
    {
        static const bool on = []
        {
            const char *v = std::getenv("PS2X_HOLD_TRACE");
            return v && *v && *v != '0';
        }();
        return on;
    }

    inline uint64_t nowNs()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    // What the calling thread is doing. The GS thread sets it around a packet or a call, the window around its
    // presentation work; every other thread (the game's) is Other.
    enum Role : int { Other = 0, GameGs = 1, PictureGs = 2, Present = 3, GsCall = 4, Roles = 5 };
    inline thread_local int t_role = Other;
    struct Scope
    {
        int before;
        explicit Scope(int role) : before(t_role) { t_role = role; }
        ~Scope() { t_role = before; }
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;
    };

    // The calling thread's own waits in drains of the game's device (totals since the thread began).
    inline thread_local uint64_t t_deviceWaitNs = 0, t_gsWaitNs = 0;

    struct Counter
    {
        std::atomic<uint64_t> ns{0}, maxNs{0}, count{0};
        void add(uint64_t v)
        {
            ns.fetch_add(v, std::memory_order_relaxed);
            count.fetch_add(1u, std::memory_order_relaxed);
            uint64_t most = maxNs.load(std::memory_order_relaxed);
            while (v > most && !maxNs.compare_exchange_weak(most, v, std::memory_order_relaxed))
            {
            }
        }
    };
    struct Counters
    {
        std::array<Counter, Roles> packets;            // the GS thread: GameGs, PictureGs (per packet), GsCall (per call)
        std::array<Counter, Roles> lockWait, lockHeld; // the GS backend's device lock, by the role of the thread
        Counter copyPages, fieldScanout, pictureRead;  // the backend functions of those names, start to end
        Counter readFlush, readVsync;                  // inside a picture's read: the flush before it, paraLLEl-GS's vsync,
        Counter readSlots, readBuffer, readSubmit, readWait; // the ring's slots, the read's buffer, the submit, the wait at its end
        std::array<Counter, Roles> fenceAsk;           // a question "is the card done with this read?", by the asker's role
        // A flush of the pictures' GS memory that could move the device on to its next frame context (v3.10): it
        // moved on, or left the move for a later flush because moving on would have waited for the card. The time
        // is the question's ("would it wait?").
        Counter contextMoved, contextLeft;
    };
    inline Counters &counters()
    {
        static Counters all;
        return all;
    }

    // A counter's totals so far and the longest single time since the last take() (one reader).
    struct Value
    {
        uint64_t ns = 0, maxNs = 0, count = 0;
    };
    inline Value take(Counter &counter)
    {
        Value v;
        v.ns = counter.ns.load(std::memory_order_relaxed);
        v.count = counter.count.load(std::memory_order_relaxed);
        v.maxNs = counter.maxNs.exchange(0u, std::memory_order_relaxed);
        return v;
    }
}

#endif
