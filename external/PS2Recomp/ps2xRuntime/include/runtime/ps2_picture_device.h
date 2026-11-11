#ifndef PS2_PICTURE_DEVICE_H
#define PS2_PICTURE_DEVICE_H

// DW5XL (issue 049, amendment v3.7, changes 2 and 3 with addition 2): a VIF1/VU1 device of the in-between pictures'
// own, on a thread of its own. The game's device has no picture work in its queue.
//
// It is the game's VIF1 interpreter and VU1 (Vif1Unit, VU1Interpreter with a host: one implementation, a second
// instance) on registers and VU1 memory of its own. Three calls drive it:
//   takeStart(frame, ...)  in the game's device order, when the game has finished a frame: the game's VIF1
//                          registers, VU1 registers and VU1 memory are copied. Every picture of that frame starts
//                          from that copy. Pictures of earlier frames that are not through their VIF1/VU1 work are
//                          given up: one in that work stops at its next VIF code, those not started are not started.
//   open(frame, prepare)   in the game's GS order: what the GS side still holds of earlier frames' pictures is
//                          dropped (nothing waits), `prepare` runs (the copy of the changed pages into the pictures'
//                          GS), and from then on the frame's packets are read there.
//   submit(picture)        any thread: a picture, run once its frame's start is taken.
// A picture's VIF1/VU1 work starts as soon as its frame's start is taken and runs beside the game's. Its GIF packets
// leave in batches for the GS side (Hooks::gif: a side stream of the GS thread, GifArbiter::submitSide), which reads
// them while VU1 goes on: one packet at a time, the game's packets first (change 3). `done` follows a picture's last
// packet there. The packets pass no arbitration: PATH1 and PATH2 packets of a VIF1 stream are handed over one by one
// in the game's device as well, so arbitration never reorders them, and there is no PATH3 here.

#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/ps2_vif1_unit.h"
#include "runtime/ps2_vu1.h"

#include <atomic>
#include <cfenv>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

class GS;

namespace ps2x
{
    // A hash of GIF packets in the order they are handed over that does not depend on where a packet was cut (a
    // DIRECT cut off at a transfer's end arrives in two parts). Criterion 6d compares the game's device with the
    // pictures' by it.
    struct GifStreamHash
    {
        uint64_t hash = 1469598103934665603ull;
        uint64_t bytes = 0;
        int path = 0;
        void add(GifPathId pathId, const uint8_t *data, uint32_t size);
    };

    class PictureDevice
    {
    public:
        struct Result
        {
            bool drawn = false;      // every packet of the picture is in the GS; false: it was given up or dropped
            uint64_t bodyHash = 0;   // Picture::hashBody: the body's packets (GifStreamHash) and their bytes
            uint64_t bodyBytes = 0;
        };
        // One picture: three VIF1 streams run one after the other from its frame's start state.
        struct Picture
        {
            uint64_t frame = 0;
            std::vector<uint8_t> head, body, tail;
            bool hashBody = false;
            // Called once. Drawn: on the GS side right after the picture's last packet (the place for its read).
            // Not drawn: on the thread that gave it up or dropped it.
            std::function<void(const Result &)> done;
        };
        struct Hooks
        {
            // A batch of a picture's GIF packets for the GS side, in the order the device made them (the device's
            // thread); its group is the picture's frame. False: refused, the frame was dropped there.
            std::function<bool(std::shared_ptr<GifArbiter::SideBatch>)> gif;
            // open(): the GS side drops what it holds of frames before `frame` (true: a picture was cut off) ...
            std::function<bool(uint64_t frame)> dropBefore;
            // ... and after `prepare` reads the batches of frames up to `frame`.
            std::function<void(uint64_t frame)> allow;
        };
        // A batch leaves for the GS side when it holds this many packets or bytes, and at a picture's end.
        static constexpr size_t kBatchPackets = 32u, kBatchBytes = 64u * 1024u;

        // `anyGs`: VU1's interface wants a GS; a VU with a host never uses it.
        PictureDevice(GS &anyGs, Hooks hooks);
        ~PictureDevice();
        PictureDevice(const PictureDevice &) = delete;
        PictureDevice &operator=(const PictureDevice &) = delete;

        void takeStart(uint64_t frame, const Vif1Unit &vif1, const VU1Interpreter &vu1);
        void open(uint64_t frame, const std::function<void()> &prepare);
        void submit(Picture picture);
        // Blocks until no picture whose frame's start is taken is waiting for or in its VIF1/VU1 work (tests,
        // shutdown). The GS side may still hold packets.
        void waitIdle();

        struct Totals
        {
            uint64_t pictures = 0; // their VIF1/VU1 work was done and every packet left for the GS side
            uint64_t givenUp = 0;  // given up in or before that work, or refused there
            uint64_t dropped = 0;  // of `pictures`: dropped on the GS side
            uint64_t workNs = 0;   // VIF1 and VU1 (the pictures counted in `pictures`)
            uint64_t bytes = 0;    // their GIF bytes
        };
        Totals totals() const;

    private:
        struct Start
        {
            uint64_t frame = 0;
            VIFRegisters regs{};
            uint32_t pendingPath2ImageQwc = 0u;
            bool pendingPath2DirectHl = false;
            std::vector<uint8_t> code, data;
            VU1Interpreter::RunState vu{};
            std::fenv_t fenv{};
        };
        struct Unit;                                    // the VIF1 unit with its host, VU1 and memory
        struct Counters;                                // what a picture's `then` on the GS side counts

        void threadMain();
        void run(Picture &picture, const Start &start);
        bool givenUp(uint64_t frame) const { return frame < m_startFrame.load(std::memory_order_acquire); }
        void addPacket(GifPathId path, const uint8_t *bytes, uint32_t size, uint8_t part);
        void newBatch(uint64_t frame);
        void sendBatch();

        Hooks m_hooks;
        std::unique_ptr<Unit> m_unit;
        std::shared_ptr<Counters> m_counters;
        // The running picture (the device's thread only).
        std::shared_ptr<GifArbiter::SideBatch> m_batch;
        std::vector<std::shared_ptr<GifArbiter::SideBatch>> m_batches; // reused once the GS side let go of them
        GifStreamHash m_bodyHash;
        bool m_hashBody = false;
        bool m_refused = false;                         // the GS side refused a batch of it
        uint64_t m_bytes = 0;
        // DW5XL-DEBUG PS2X_SMOOTH_CUT_EVERY=n: every n-th picture stops part way, as one whose frame is over does,
        // after a number of packets that moves from one such picture to the next; what it made so far reaches the
        // GS side. For proving that the pictures after a cut-off one are whole. Unset: nothing.
        uint64_t m_cutEvery = 0, m_runs = 0, m_cutAt = 0, m_packets = 0;
        bool cutNow() const { return m_cutAt != 0u && m_packets >= m_cutAt; }

        mutable std::mutex m_mutex;
        std::condition_variable m_wake;                 // the device's thread: a picture to run, a start taken
        std::condition_variable m_still;                // waitIdle(): the device let go
        std::deque<Picture> m_queue;
        std::vector<std::shared_ptr<Start>> m_starts, m_spare;
        std::atomic<uint64_t> m_startFrame{0};          // the newest frame whose start is taken; earlier ones are given up
        bool m_busy = false;                            // a picture is in its VIF1/VU1 work
        bool m_stop = false;
        Totals m_totals;                                // pictures, givenUp, workNs, bytes (dropped: m_counters)
        std::thread m_thread;
    };
}

#endif // PS2_PICTURE_DEVICE_H
