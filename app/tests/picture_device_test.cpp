// DW5XL-TEST issue 049, amendment v3.7 with its additions: the pictures' own VIF1/VU1 device (ps2x::PictureDevice)
// and the GS thread's side stream that reads its packets (GifArbiter::submitSide; change 3, "the game first").
// Authored VIF codes, microcode and GIF data only; no game data, no renderer.
// The game's unit (the runtime's memory object and VU1) runs a made-up set-up stream; there the "frame ends" and the
// device takes its start (takeStart). The game's unit then runs a made-up list (MSCAL of a program that XGKICKs a
// packet, a DIRECT, MSCNT, an UNPACK that changes the packet's memory): its PATH1/PATH2 packets are the reference.
//   1. the device makes the reference's packets from the same list, byte for byte, and its hash of them is the
//      hash of the game's packets (the tap criterion 6d uses);
//   2. a second picture of the frame starts from the same copy (the list left VU1 memory changed);
//   3. head, body and tail are read in order, and only the body is hashed;
//   4. a picture's VIF1/VU1 work starts when its frame's start is taken, but none of its packets is read before the
//      frame is opened (the page copy); the next frame's picture starts from the next frame's copy;
//   5. the game first: a call of the GS thread's main stream that comes while a picture's packets are being read
//      runs before the picture's next packet;
//   6. a picture whose packets wait on the GS side when the next frame is opened is dropped there: reported not
//      drawn, nothing of it read; so is a picture of a frame that was passed over;
//   7. a picture still in its VIF1 work when the next frame's start is taken is given up at its next VIF code; the
//      packets of it that were read already are a picture cut off, which the next opening reports;
//   8. VU1's state as a value (RunState) is what a save state holds of it, also in the middle of an XGKICK;
//   9. the side stream without a GS worker: batches are read where they are submitted or allowed.
#include "ps2_runtime.h"
#include "runtime/ps2_picture_device.h"
#include "runtime/state_archive.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x1000;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x1004;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 1;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[] = {nullptr};

namespace
{
constexpr uint32_t kLowerNop = 0x8000033Cu, kUpperNop = 0x000002FFu, kEbit = 1u << 30;
int g_failures = 0;

void check(bool ok, const std::string &what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    g_failures += ok ? 0 : 1;
}

struct Stream
{
    std::vector<uint32_t> words;
    void code(uint32_t opcode, uint32_t num, uint32_t imm) { words.push_back((opcode << 24) | (num << 16) | imm); }
    void word(uint32_t w) { words.push_back(w); }
    void alignForDirect()
    {
        while (words.size() % 4u != 3u)
            words.push_back(0u);
    }
    void quad(uint64_t lo, uint64_t hi)
    {
        words.push_back(static_cast<uint32_t>(lo));
        words.push_back(static_cast<uint32_t>(lo >> 32));
        words.push_back(static_cast<uint32_t>(hi));
        words.push_back(static_cast<uint32_t>(hi >> 32));
    }
    std::vector<uint8_t> bytes() const
    {
        const auto *p = reinterpret_cast<const uint8_t *>(words.data());
        return std::vector<uint8_t>(p, p + words.size() * 4u);
    }
};

uint64_t gifTag(uint64_t nloop, bool eop, uint64_t flg, uint64_t nreg)
{
    return nloop | (eop ? 1ull << 15 : 0ull) | (flg << 58) | (nreg << 60);
}

// What the game did before the frame end: the program and the packet it kicks.
Stream setUp()
{
    Stream s;
    s.code(0x01, 0, 0x0101);                     // STCYCL cl 1, wl 1
    s.code(0x4A, 6, 0);                          // MPG: 6 pairs at 0
    const uint32_t program[12] = {
        0x800006FCu | (1u << 11), kUpperNop,     // XGKICK vi1
        kLowerNop, kUpperNop | kEbit,            // end of the first run
        kLowerNop, kUpperNop,
        kLowerNop, kUpperNop | kEbit,            // end of the run MSCNT continues
        kLowerNop, kUpperNop,
        kLowerNop, kUpperNop};
    for (uint32_t w : program) s.word(w);
    s.code(0x6C, 4, 16);                         // UNPACK V4-32, 4 vectors at 16: the packet XGKICK sends
    s.quad(gifTag(3, true, 2, 0), 0u);           // IMAGE, 3 quadwords, EOP
    for (uint32_t i = 0; i < 12u; ++i) s.word(0x11110000u + i * 0x0101u);
    return s;
}

// The frame's list. It leaves the kicked packet's memory changed: run again from what it leaves, it kicks other bytes.
Stream list()
{
    Stream s;
    s.code(0x14, 0, 0);                          // MSCAL 0
    s.alignForDirect();
    s.code(0x50, 0, 2);                          // DIRECT, 2 quadwords
    s.quad(gifTag(1, true, 0, 1), 0xEull);       // PACKED, A+D
    s.quad(0x0000000012345678ull, 0x3Full);      // TEXFLUSH
    s.code(0x17, 0, 0);                          // MSCNT
    s.code(0x6C, 3, 17);                         // UNPACK V4-32, 3 vectors at 17: the packet's image data
    for (uint32_t i = 0; i < 12u; ++i) s.word(0x22220000u + i);
    s.code(0x07, 0, 0x55);                       // MARK
    return s;
}

Stream direct(uint64_t value)
{
    Stream s;
    s.alignForDirect();
    s.code(0x50, 0, 2);
    s.quad(gifTag(1, true, 0, 1), 0xEull);
    s.quad(value, 0x3Full);
    return s;
}

// The game's unit, as the runtime wires it; the tap sees its PATH1/PATH2 packets with their paths.
struct Game
{
    PS2Runtime rt;
    GifArbiter arbiter;
    std::vector<uint8_t> gif;
    ps2x::GifStreamHash hash;
    Game()
    {
        if (!rt.memory().initialize())
            throw std::runtime_error("memory initialization");
        PS2Memory &mem = rt.memory();
        arbiter.setProcessPacketFn([this](const uint8_t *bytes, uint32_t size) { gif.insert(gif.end(), bytes, bytes + size); });
        mem.setGifArbiter(&arbiter);
        mem.m_gifTap = [this](GifPathId path, const uint8_t *bytes, uint32_t size) { hash.add(path, bytes, size); };
        mem.setVu1MscalCallback([this](uint32_t startPC, uint32_t top, uint32_t itop) {
            PS2Memory &m = rt.memory();
            rt.vu1().execute(m.getVU1Code(), PS2_VU1_CODE_SIZE, m.getVU1Data(), PS2_VU1_DATA_SIZE, rt.gs(), &m, startPC, top, itop, 65536);
        });
        mem.setVu1MscntCallback([this](uint32_t top, uint32_t itop) {
            PS2Memory &m = rt.memory();
            rt.vu1().resume(m.getVU1Code(), PS2_VU1_CODE_SIZE, m.getVU1Data(), PS2_VU1_DATA_SIZE, rt.gs(), &m, top, itop, 65536);
        });
    }
    ~Game() { rt.memory().setGifArbiter(nullptr); }
    void run(const std::vector<uint8_t> &stream) { rt.memory().processVIF1Data(stream.data(), static_cast<uint32_t>(stream.size())); }
};

// What the GS side reads of the device's packets, and what the device reports.
struct Seen
{
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<uint8_t> gif;
    uint32_t packets = 0;
    std::vector<ps2x::PictureDevice::Result> done;
    uint32_t cuts = 0;
    std::function<void(uint32_t)> onPacket; // GS side, after packet number n was read (not under the mutex)

    bool waitDone(size_t count)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(20), [&] { return done.size() >= count; });
    }
    bool waitPackets(uint32_t count)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(20), [&] { return packets >= count; });
    }
    std::vector<uint8_t> take()
    {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<uint8_t> out;
        out.swap(gif);
        done.clear();
        packets = 0;
        return out;
    }
};

ps2x::PictureDevice::Picture picture(Seen &seen, uint64_t frame, std::vector<uint8_t> body, std::vector<uint8_t> head = {},
                                     std::vector<uint8_t> tail = {})
{
    ps2x::PictureDevice::Picture p;
    p.frame = frame;
    p.head = std::move(head);
    p.body = std::move(body);
    p.tail = std::move(tail);
    p.hashBody = true;
    p.done = [&seen](const ps2x::PictureDevice::Result &result) {
        {
            std::lock_guard<std::mutex> lock(seen.mutex);
            seen.done.push_back(result);
        }
        seen.changed.notify_all();
    };
    return p;
}

std::vector<uint8_t> stateOf(VU1Interpreter &vu)
{
    ps2x::StateArchive out = ps2x::StateArchive::writer();
    vu.serializeState(out);
    return out.data();
}

void run()
{
    Game game;
    PS2Memory &mem = game.rt.memory();
    std::memset(mem.getVU1Code(), 0, PS2_VU1_CODE_SIZE);
    std::memset(mem.getVU1Data(), 0, PS2_VU1_DATA_SIZE);
    game.rt.vu1().reset();
    game.rt.vu1().state().vi[1] = 16;
    game.run(setUp().bytes());

    // The GS thread: its main stream gets calls only here; its side stream reads the device's packets.
    Seen seen;
    GifArbiter gsSide;
    gsSide.setProcessPacketFn([](const uint8_t *, uint32_t) {});
    gsSide.setWorkerEnabled(true);
    gsSide.setSideProcessFn([&seen](GifPathId, const uint8_t *bytes, uint32_t size) {
        uint32_t number = 0;
        {
            std::lock_guard<std::mutex> lock(seen.mutex);
            seen.gif.insert(seen.gif.end(), bytes, bytes + size);
            number = ++seen.packets;
        }
        seen.changed.notify_all();
        if (seen.onPacket) seen.onPacket(number);
    });
    ps2x::PictureDevice::Hooks hooks;
    hooks.gif = [&gsSide](std::shared_ptr<GifArbiter::SideBatch> batch) { return gsSide.submitSide(std::move(batch)); };
    hooks.dropBefore = [&gsSide, &seen](uint64_t frame) {
        const bool cut = gsSide.dropSideBefore(frame);
        std::lock_guard<std::mutex> lock(seen.mutex);
        seen.cuts += cut ? 1u : 0u;
        return cut;
    };
    hooks.allow = [&gsSide](uint64_t frame) { gsSide.allowSide(frame); };
    ps2x::PictureDevice device(game.rt.gs(), hooks);
    // open() in the GS order, as the game's page copy is: a call of the main stream.
    const auto open = [&](uint64_t frame, std::function<void()> prepare = {}) {
        gsSide.submitCall([&device, frame, prepare] { device.open(frame, prepare); });
        gsSide.waitIdle();
    };

    // The frame ends here: the device's start is the game's state now. Then the game runs its own list.
    device.takeStart(1u, mem.m_vif1, game.rt.vu1());
    const std::vector<uint8_t> body = list().bytes();
    game.gif.clear();
    game.hash = ps2x::GifStreamHash{};
    game.run(body);
    const std::vector<uint8_t> reference = game.gif;
    const ps2x::GifStreamHash referenceHash = game.hash;
    check(reference.size() == 64u + 32u, "the reference: the XGKICK packet (64 bytes) and the DIRECT (32 bytes)");
    check(referenceHash.bytes == reference.size(), "the tap saw every PATH1/PATH2 byte of the game's unit");

    // 1
    bool prepared = false;
    open(1u, [&prepared] { prepared = true; });
    check(prepared, "open: the page copy's place was called");
    device.submit(picture(seen, 1u, body));
    check(seen.waitDone(1u), "1. the picture was run");
    {
        std::lock_guard<std::mutex> lock(seen.mutex);
        check(!seen.done.empty() && seen.done[0].drawn, "1. it is reported drawn");
        check(seen.gif == reference, "1. the device's packets are the game's unit's, byte for byte");
        check(!seen.done.empty() && seen.done[0].bodyHash == referenceHash.hash && seen.done[0].bodyBytes == referenceHash.bytes,
              "1. its hash of them is the hash of the game's packets");
    }
    seen.take();

    // 2
    device.submit(picture(seen, 1u, body));
    check(seen.waitDone(1u) && seen.take() == reference, "2. a second picture of the frame starts from the same copy");

    // 3
    const std::vector<uint8_t> head = direct(0xAAAA0001ull).bytes(), tail = direct(0xBBBB0002ull).bytes();
    device.submit(picture(seen, 1u, body, head, tail));
    check(seen.waitDone(1u), "3. a picture with head and tail was run");
    {
        std::lock_guard<std::mutex> lock(seen.mutex);
        const size_t n = reference.size();
        const bool sized = seen.gif.size() == n + 64u;
        check(sized && std::memcmp(seen.gif.data() + 32u, reference.data(), n) == 0, "3. the body's packets between the head's and the tail's");
        uint64_t first = 0, last = 0;
        if (sized) {
            std::memcpy(&first, seen.gif.data() + 16u, 8);
            std::memcpy(&last, seen.gif.data() + 32u + n + 16u, 8);
        }
        check(first == 0xAAAA0001ull && last == 0xBBBB0002ull, "3. head first, tail last");
        check(!seen.done.empty() && seen.done[0].bodyHash == referenceHash.hash, "3. only the body is hashed");
    }
    seen.take();

    // 4
    device.takeStart(2u, mem.m_vif1, game.rt.vu1());   // the game's state after its own list
    game.gif.clear();
    game.run(body);                                     // the game's own next list, from that state
    const std::vector<uint8_t> second = game.gif;
    check(second != reference, "4. (the next frame's list kicks other bytes: the list changed the packet's memory)");
    device.submit(picture(seen, 2u, body));
    device.waitIdle();                                  // its VIF1/VU1 work is done: the start was taken
    {
        std::lock_guard<std::mutex> lock(seen.mutex);
        check(seen.gif.empty() && seen.done.empty(), "4. before the frame is opened none of its packets is read");
    }
    bool nothingYet = false;
    open(2u, [&] {
        std::lock_guard<std::mutex> lock(seen.mutex);
        nothingYet = seen.gif.empty() && seen.done.empty();
    });
    check(nothingYet, "4. nor at the page copy itself");
    check(seen.waitDone(1u) && seen.take() == second, "4. after it the picture is read: drawn from the next frame's start");

    // 5
    {
        std::mutex gate;
        std::condition_variable turn;
        bool firstRead = false, callIn = false;
        uint32_t packetsAtCall = 0;
        seen.onPacket = [&](uint32_t number) {
            if (number != 1u) return;
            std::unique_lock<std::mutex> lock(gate);
            firstRead = true;
            turn.notify_all();
            turn.wait_for(lock, std::chrono::seconds(20), [&] { return callIn; });
        };
        const uint64_t gaveWayBefore = gsSide.sideTotals().gaveWay;
        device.submit(picture(seen, 2u, body));
        {
            std::unique_lock<std::mutex> lock(gate);
            turn.wait_for(lock, std::chrono::seconds(20), [&] { return firstRead; });
        }
        gsSide.submitCall([&] {
            std::lock_guard<std::mutex> lock(seen.mutex);
            packetsAtCall = seen.packets;
        });
        {
            std::lock_guard<std::mutex> lock(gate);
            callIn = true;
        }
        turn.notify_all();
        check(seen.waitDone(1u), "5. the picture is read to its end");
        gsSide.waitIdle();
        seen.onPacket = nullptr;
        check(firstRead && packetsAtCall == 1u, "5. a call of the main stream ran before the picture's second packet");
        check(gsSide.sideTotals().gaveWay == gaveWayBefore + 1u && seen.take() == second, "5. the picture gave way once and is complete");
    }

    // 6
    device.takeStart(3u, mem.m_vif1, game.rt.vu1());
    device.submit(picture(seen, 3u, body));
    device.waitIdle();
    device.takeStart(4u, mem.m_vif1, game.rt.vu1());
    bool waiting = false;
    open(4u, [&] {
        std::lock_guard<std::mutex> lock(seen.mutex);
        waiting = seen.gif.empty();
    });
    check(seen.waitDone(1u), "6. a picture whose packets waited on the GS side at the next frame's opening is answered");
    {
        std::lock_guard<std::mutex> lock(seen.mutex);
        check(waiting && !seen.done.empty() && !seen.done[0].drawn && seen.gif.empty() && seen.cuts == 1u,
              "6. it was dropped there: not drawn, nothing of it read");
    }
    seen.take();
    device.submit(picture(seen, 1u, body));
    check(seen.waitDone(1u), "6. a picture of a frame passed over is answered");
    {
        std::lock_guard<std::mutex> lock(seen.mutex);
        check(!seen.done.empty() && !seen.done[0].drawn && seen.gif.empty(), "6. it is not drawn");
    }
    seen.take();

    // 7
    {
        // More packets than one batch holds, then tens of milliseconds of VIF codes: the first batch is read while
        // the picture is still in its VIF1 work.
        const uint32_t many = static_cast<uint32_t>(ps2x::PictureDevice::kBatchPackets) + 8u;
        Stream longWork;
        for (uint32_t i = 0; i < many; ++i) {
            const Stream one = direct(0xCCCC0000ull + i);
            longWork.words.insert(longWork.words.end(), one.words.begin(), one.words.end());
        }
        for (uint32_t i = 0; i < 4u * 1024u * 1024u; ++i) longWork.code(0x00, 0, 0);
        device.takeStart(5u, mem.m_vif1, game.rt.vu1());
        open(5u);
        device.submit(picture(seen, 5u, longWork.bytes()));
        check(seen.waitPackets(static_cast<uint32_t>(ps2x::PictureDevice::kBatchPackets)), "7. a picture's first batch is read while its VIF1 work goes on");
        device.takeStart(6u, mem.m_vif1, game.rt.vu1());
        check(seen.waitDone(1u), "7. a picture in its VIF1 work when the next frame's start is taken is answered");
        uint32_t cutsBefore = 0;
        {
            std::lock_guard<std::mutex> lock(seen.mutex);
            check(!seen.done.empty() && !seen.done[0].drawn, "7. it was given up: not drawn");
            check(seen.packets == ps2x::PictureDevice::kBatchPackets, "7. nothing more of it was read");
            cutsBefore = seen.cuts;
        }
        open(6u);
        {
            std::lock_guard<std::mutex> lock(seen.mutex);
            check(seen.cuts == cutsBefore + 1u, "7. the next opening reports a picture cut off");
        }
        seen.take();
        device.submit(picture(seen, 6u, body));
        check(seen.waitDone(1u), "7. the next frame's picture is run");
        {
            std::lock_guard<std::mutex> lock(seen.mutex);
            check(!seen.done.empty() && seen.done[0].drawn && seen.gif.size() == reference.size(), "7. and drawn whole");
        }
        seen.take();
    }
    device.waitIdle();
    gsSide.waitIdle();
    const ps2x::PictureDevice::Totals totals = device.totals();
    check(totals.pictures == 7u && totals.givenUp == 2u && totals.dropped == 1u,
          "the device counted 7 pictures sent, 2 given up, 1 of the sent dropped on the GS side");

    // 9 (here: the device is still alive for part 8 below; this part needs no device)
    {
        GifArbiter inl;                                 // no GS worker
        std::vector<uint8_t> read;
        std::vector<bool> thens;
        inl.setSideProcessFn([&read](GifPathId, const uint8_t *bytes, uint32_t size) { read.insert(read.end(), bytes, bytes + size); });
        const auto batch = [&thens](uint64_t group, uint8_t value, bool last) {
            auto b = std::make_shared<GifArbiter::SideBatch>();
            b->group = group;
            b->bytes.assign(16u, value);
            b->packets.push_back({GifPathId::Path2, 0u, 16u});
            if (last) b->then = [&thens](bool dropped) { thens.push_back(dropped); };
            return b;
        };
        const bool taken = inl.submitSide(batch(2u, 0x22, true));
        const bool held = read.empty() && thens.empty();
        inl.allowSide(1u);
        const bool stillHeld = read.empty();
        inl.allowSide(2u);
        check(taken && held && stillHeld && read.size() == 16u && thens.size() == 1u && !thens[0],
              "9. without a GS worker a batch is read where its group is allowed");
        inl.submitSide(batch(2u, 0x23, true));
        check(read.size() == 32u && thens.size() == 2u, "9. or where it is submitted, once allowed");
        inl.submitSide(batch(3u, 0x33, false));         // a picture's first batch; its last has not come
        inl.submitSide(batch(3u, 0x34, true));
        const bool cut = inl.dropSideBefore(4u);
        check(cut && read.size() == 32u && thens.size() == 3u && thens[2], "9. batches waiting when their group is dropped are not read; the unit's last one says so");
        check(!inl.submitSide(batch(3u, 0x35, true)) && thens.size() == 3u, "9. a later batch of a dropped group is refused");
        check(!inl.dropSideBefore(4u), "9. nothing left to cut");
    }

    // 8
    {
        std::vector<uint8_t> code(PS2_VU1_CODE_SIZE), data(PS2_VU1_DATA_SIZE);
        std::memcpy(code.data(), mem.getVU1Code(), PS2_VU1_CODE_SIZE);
        std::memcpy(data.data(), mem.getVU1Data(), PS2_VU1_DATA_SIZE);
        std::atomic<uint64_t> generation{PS2Memory::nextCodeGeneration()};
        VU1Interpreter::Host host;
        host.code = code.data();
        host.generation = &generation;
        host.gif = [](const uint8_t *, uint32_t) {};
        auto a = std::make_unique<VU1Interpreter>(VU1Interpreter::Unit::VU1), b = std::make_unique<VU1Interpreter>(VU1Interpreter::Unit::VU1);
        a->setHost(&host);
        a->reset();
        b->reset();
        a->state().vi[1] = 16;
        a->execute(code.data(), PS2_VU1_CODE_SIZE, data.data(), PS2_VU1_DATA_SIZE, game.rt.gs(), nullptr, 0u, 3u, 5u, 3u);
        check(stateOf(*a) != stateOf(*b), "8. (a VU1 stopped in the middle of an XGKICK differs from a fresh one)");
        auto value = std::make_unique<VU1Interpreter::RunState>();
        a->saveRunState(*value);
        b->loadRunState(*value);
        check(stateOf(*a) == stateOf(*b), "8. VU1's state as a value is what a save state holds of it");
    }
}
} // namespace

int main()
{
#ifdef _WIN32
    _putenv_s("PS2X_GS_BACKEND", "cpu");
#else
    setenv("PS2X_GS_BACKEND", "cpu", 1);   // no GS work is done here; the VU1 calls only want a GS to name
#endif
    try
    {
        run();
    }
    catch (const std::exception &error)
    {
        std::printf("FAIL  exception: %s\n", error.what());
        return 1;
    }
    std::printf("%s (%d failed)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
