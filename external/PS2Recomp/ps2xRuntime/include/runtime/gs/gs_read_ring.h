#ifndef PS2_GS_READ_RING_H
#define PS2_GS_READ_RING_H

// DW5XL: the ring of reads in flight on the graphics card (issue 023; three places per scanout). A read takes the
// ring's oldest place. What it finds there decides what it does, and that choice is here so that it can be tested
// without a graphics card.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

namespace ps2x::gs
{
    enum class ReadPlace
    {
        Free,     // no read in the place: use it
        Finished, // the read in it is done on the card: take it out, then use the place
        Wait,     // the read in it is not done: wait for the card, then take it out
        Refuse    // the read in it is not done and this read does not wait: it is not made
    };

    // `pending`: a read is in the ring's oldest place; `finished`: the card is done with it; `picture`: the new read
    // is an in-between picture's (issue 049), not a field's of the game.
    // Issue 049, amendment v3.9 (approved 2026-11-12): a picture's read never waits for the card. With the three
    // reads before it unfinished it is refused, and the picture is cut off. (Measured 2026-11-12 at 4x: such a
    // wait took up to 19 ms on the GS thread, with the game's own packets waiting behind it.) A field's read is
    // the game's own picture and waits, as before.
    inline ReadPlace readPlace(bool pending, bool finished, bool picture)
    {
        if (!pending)
            return ReadPlace::Free;
        if (finished)
            return ReadPlace::Finished;
        return picture ? ReadPlace::Refuse : ReadPlace::Wait;
    }

    // One read's "is the graphics card done with it?", for the threads that ask and the thread that waits.
    // The card's own handle for a read (a Granite fence) lets one caller in at a time: a question asked while
    // another thread waits on it is answered only when that wait ends. Measured 2026-11-12 at 4x: the GS thread's
    // question about a picture's read took up to 17 ms that way, behind the picture reader's wait for that read,
    // with the device lock held (issue 049, amendment v3.9: nothing waits where a picture is checked). So:
    //  - a thread that is going to wait says so first, under the device lock (ReadWait), and waits without it;
    //  - a question (finished(), under the device lock) goes to the card only while nobody waits. While somebody
    //    waits, the read is unfinished as far as anybody knows: the waiting thread notes its end the moment its
    //    wait returns, and that is the answer from then on.
    // Waits are announced and questions asked under the same lock, so a question that goes to the card never
    // meets a wait there.
    class ReadDone
    {
    public:
        // The card's wait for this read: true when the read is finished; a timeout of 0 only asks. It may be called
        // by two threads at once (the second then waits for the first).
        using CardWait = std::function<bool(uint64_t timeoutNs)>;
        explicit ReadDone(CardWait card) : m_card(std::move(card)) {}
        ReadDone(const ReadDone &) = delete;
        ReadDone &operator=(const ReadDone &) = delete;

        // Caller holds the device lock. Whether the card is done with the read, as far as it can be known without
        // waiting: never waits behind another thread's wait.
        bool finished()
        {
            if (m_finished.load(std::memory_order_acquire))
                return true;
            if (m_waiting.load(std::memory_order_acquire) != 0u)
                return false;
            return card(0u);
        }

    private:
        friend class ReadWait;
        bool card(uint64_t timeoutNs)
        {
            if (!m_card(timeoutNs))
                return false;
            m_finished.store(true, std::memory_order_release);
            return true;
        }
        const CardWait m_card;
        std::atomic<bool> m_finished{false};
        std::atomic<uint32_t> m_waiting{0};
    };

    // A thread's wait for a read. Made under the device lock (that announces the wait); wait() is called without
    // the lock; the wait is withdrawn when this object goes.
    class ReadWait
    {
    public:
        ReadWait() = default;
        explicit ReadWait(std::shared_ptr<ReadDone> read) : m_read(std::move(read))
        {
            if (m_read)
                m_read->m_waiting.fetch_add(1u, std::memory_order_acq_rel);
        }
        ~ReadWait() { withdraw(); }
        ReadWait(ReadWait &&other) noexcept : m_read(std::move(other.m_read)) { other.m_read.reset(); }
        ReadWait &operator=(ReadWait &&other) noexcept
        {
            if (this != &other)
            {
                withdraw();
                m_read = std::move(other.m_read);
                other.m_read.reset();
            }
            return *this;
        }
        ReadWait(const ReadWait &) = delete;
        ReadWait &operator=(const ReadWait &) = delete;

        explicit operator bool() const { return m_read != nullptr; }
        // True when the card finished the read within the time.
        bool wait(uint64_t timeoutNs)
        {
            return m_read && (m_read->m_finished.load(std::memory_order_acquire) || m_read->card(timeoutNs));
        }

    private:
        void withdraw()
        {
            if (m_read)
                m_read->m_waiting.fetch_sub(1u, std::memory_order_acq_rel);
            m_read.reset();
        }
        std::shared_ptr<ReadDone> m_read;
    };
}

#endif
