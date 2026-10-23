// Synthetic callback-fed decoder regressions; no disc, frame capture or guest binary.
#include "MiniTest.h"
#include "Stubs/MPEG.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

void reset_ps2_test_function_table();
namespace
{
constexpr uint32_t entry = 0x130000, resume = 0x130010, callback = 0x130020, producer = 0x130030,
                   nestedReturn = 0x130040;
constexpr uint32_t handle = 0x140000, image = 0x150000, work = 0x160000, packet = 0x170000;
constexpr uint32_t user = 0xDEAD1234, gp = 0x12345000, saved = 0xABCDEF12;
constexpr uint32_t ipu = 0x1000B400, dmaBase = 0x180000;
constexpr int cancelled = -425; // KE_WAIT_DELETE

enum class Mode
{
    Startup,
    Retry,
    Empty,
    Rejected,
    Reset,
    Delete,
    Recreate,
    Restart,
    Init,
    CancelInCallback,
    Ordering,
    Credits,
    Progress,
    Nested
};
struct Scenario
{
    TestCase &test;
    Mode mode;
    PS2Runtime runtime;
    std::vector<uint8_t> ram = std::vector<uint8_t>(PS2_RAM_SIZE);
    std::vector<uint32_t> events;
    unsigned inputs = 0, step = 0;
    bool completed = false, accept = false;
    int result = -999;
};
Scenario *active = nullptr;

void addCallback(Scenario &s, uint32_t type, bool stream = false)
{
    R5900Context ctx{};
    SET_GPR_U32((&ctx), 4, handle);
    SET_GPR_U32((&ctx), 5, type);
    if (stream)
    {
        SET_GPR_U32((&ctx), 6, 0);
        SET_GPR_U32((&ctx), 7, callback);
        SET_GPR_U32((&ctx), 8, user);
        ps2_stubs::sceMpegAddStrCallback(s.ram.data(), &ctx, &s.runtime);
    }
    else
    {
        SET_GPR_U32((&ctx), 6, callback);
        SET_GPR_U32((&ctx), 7, user);
        ps2_stubs::sceMpegAddCallback(s.ram.data(), &ctx, &s.runtime);
    }
}
void cancelDecoder(Scenario &s)
{
    R5900Context ctx{};
    SET_GPR_U32((&ctx), 4, handle);
    switch (s.mode)
    {
    case Mode::Reset:
    case Mode::CancelInCallback:
        ps2_stubs::sceMpegReset(s.ram.data(), &ctx, &s.runtime);
        break;
    case Mode::Delete:
        ps2_stubs::sceMpegDelete(s.ram.data(), &ctx, &s.runtime);
        break;
    case Mode::Recreate:
        SET_GPR_U32((&ctx), 5, work);
        SET_GPR_U32((&ctx), 6, 0x2000);
        ps2_stubs::sceMpegCreate(s.ram.data(), &ctx, &s.runtime);
        break;
    case Mode::Restart:
        ps2_stubs::notifyMpegCdStreamStart(&s.runtime);
        break;
    case Mode::Init:
        ps2_stubs::sceMpegInit(s.ram.data(), &ctx, &s.runtime);
        break;
    default:
        break;
    }
}
void onCallback(uint8_t *ram, R5900Context *ctx, PS2Runtime *)
{
    auto &s = *active;
    const uint32_t type = Ps2FastRead32(ram, getRegU32(ctx, 5));
    s.events.push_back(type);
    s.test.Equals(getRegU32(ctx, 4), handle, "callback a0 must be the MPEG handle");
    s.test.Equals(getRegU32(ctx, 6), user, "callback a2 must retain user data");
    s.test.Equals(getRegU32(ctx, 28), gp, "callback must retain caller GP");
    s.test.IsTrue(getRegU32(ctx, 29) != 0 && getRegU32(ctx, 29) % 16 == 0,
                  "callback must have its own aligned stack");
    if (type == 1)
    {
        ++s.inputs;
        if (s.mode == Mode::CancelInCallback)
            cancelDecoder(s);
        else if ((s.mode == Mode::Startup && s.inputs == 1) ||
                 ((s.mode == Mode::Retry || s.mode == Mode::Progress || s.mode == Mode::Nested) &&
                  s.inputs == 2))
            ps2_stubs::enqueueMpegDecodedFrameForTesting(handle);
    }
    if (s.mode == Mode::Credits || (s.mode == Mode::Nested && type == 0))
    {
        s.test.Equals(type, 0u, "stream callback must remain M2V");
        s.test.Equals(Ps2FastRead32(ram, getRegU32(ctx, 5) + 8), packet + 9,
                      "PES payload address must exclude its header");
        s.test.Equals(Ps2FastRead32(ram, getRegU32(ctx, 5) + 12), 16u, "PES payload length must be exact");
    }
    if (s.mode == Mode::Progress && type == 1 && s.inputs == 1)
        ps2_stubs::notifyMpegInputReady(handle,
                                        &s.runtime); // Real readiness during the callback must not be lost.
    if (s.mode == Mode::Nested && type == 1 && s.inputs == 1)
    {
        ctx->pc = nestedReturn;
        SET_GPR_U32(ctx, 4, handle);
        SET_GPR_U32(ctx, 5, packet);
        SET_GPR_U32(ctx, 6, 25);
        SET_GPR_U32(ctx, 7, packet);
        SET_GPR_U32(ctx, 8, 25);
        ps2_stubs::sceMpegDemuxPssRing(ram, ctx, &s.runtime);
        return;
    }
    SET_GPR_U32(ctx, 16, 0xBAD); // Callee writes must not replace the caller context.
    SET_GPR_S32(ctx, 2, s.mode == Mode::Rejected ? -1 : s.mode == Mode::Credits ? (s.accept ? 1 : 0) : 42);
    ctx->pc = 0;
}
void onNestedReturn(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    active->test.Equals(getRegU32(ctx, 2), 25u,
                        "NODATA's nested demux must return accepted bytes to that callback");
    SET_GPR_U32(ctx, 2, 42);
    ctx->pc = 0;
}
void getPicture(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    SET_GPR_U32(ctx, 4, handle);
    SET_GPR_U32(ctx, 5, image);
    ctx->pc = resume;
    if (active->mode == Mode::Credits)
    {
        SET_GPR_U32(ctx, 5, packet);
        SET_GPR_U32(ctx, 6, 25);
        SET_GPR_U32(ctx, 7, packet);
        SET_GPR_U32(ctx, 8, 25);
        ps2_stubs::sceMpegDemuxPssRing(ram, ctx, runtime);
    }
    else
        ps2_stubs::sceMpegGetPicture(ram, ctx, runtime);
}
void onReturn(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
{
    auto &s = *active;
    s.result = static_cast<int32_t>(getRegU32(ctx, 2));
    s.test.Equals(getRegU32(ctx, 16), saved, "callback must not clobber caller saved register");
    if (s.mode == Mode::Credits && !s.accept)
    {
        s.test.Equals(s.result, 0, "rejected PES must return zero committed bytes");
        s.test.Equals(runtime->memory().readIORegister(ipu + 0x20), 2u,
                      "rejected PES must earn no input credit");
        s.accept = true;
        ctx->pc = entry;
        return;
    }
    s.completed = true;
    ctx->pc = 0;
    runtime->requestStop();
}
void inputProducer(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
{
    auto &s = *active;
    s.test.Equals(s.inputs, 1u, "empty producer must be called once, without spinning");
    if (s.step++ == 0)
    {
        runtime->eeScheduler().completeVSync(runtime->eeScheduler().currentVSyncTick() + 1);
        ps2_stubs::notifyMpegCdStreamDataProduced(0, false, runtime);
        ctx->pc = producer;
        return; // Give a wrongly awakened GetPicture a real dispatch opportunity.
    }
    if (s.mode == Mode::Retry)
        ps2_stubs::notifyMpegCdStreamDataProduced(64, false, runtime);
    else if (s.mode == Mode::Empty || s.mode == Mode::Rejected)
        ps2_stubs::notifyMpegCdStreamEof(runtime);
    else
        cancelDecoder(s);
    ctx->pc = 0;
}
void run(TestCase &t, Mode mode)
{
    Scenario s{t, mode};
    active = &s;
    ps2_stubs::resetMpegStubState();
    ps2_stubs::notifyMpegCdStreamStart();
    auto &runtime = s.runtime;
    runtime.registerFunction(entry, getPicture);
    runtime.registerFunction(resume, onReturn);
    runtime.registerFunction(callback, onCallback);
    runtime.registerFunction(producer, inputProducer);
    runtime.registerFunction(nestedReturn, onNestedReturn);
    if (mode == Mode::Credits || mode == Mode::Retry)
    {
        t.IsTrue(runtime.memory().initialize(), "memory must initialize for real DMA credit observations");
        runtime.memory().writeIORegister(0x1000E000, 1);
        runtime.memory().writeIORegister(ipu + 0x10, dmaBase);
        runtime.memory().writeIORegister(ipu + 0x20, 2);
        runtime.memory().writeIORegister(ipu, 0x101);
    }
    if (mode == Mode::Credits)
    {
        // MPEG-2 PES containing sixteen synthetic bytes, not a valid game stream.
        const uint8_t pes[25] = {0, 0, 1, 0xE0, 0, 19, 0x80, 0, 0, 0x11, 0x22, 0x33};
        std::copy(pes, pes + 25, s.ram.data() + packet);
        addCallback(s, 0, true);
    }
    else
        addCallback(s, 1);
    if (mode == Mode::Nested)
    {
        const uint8_t pes[25] = {0, 0, 1, 0xE0, 0, 19, 0x80, 0, 0, 0x11, 0x22, 0x33};
        std::copy(pes, pes + 25, s.ram.data() + packet);
        addCallback(s, 0, true);
    }
    if (mode == Mode::CancelInCallback)
        addCallback(s, 1); // Second registration must be generation-canceled.
    if (mode == Mode::Ordering)
    {
        addCallback(s, 4);
        ps2_stubs::enqueueMpegDecodedFrameForTesting(handle);
    }
    R5900Context context{};
    context.pc = entry;
    SET_GPR_U32((&context), 28, gp);
    SET_GPR_U32((&context), 16, saved);
    SET_GPR_U32((&context), 29, 0x1F00000);
    auto &ee = runtime.eeScheduler();
    ee.reset(s.ram.data(), context);
    const bool needsProducer = mode != Mode::Startup && mode != Mode::Ordering &&
                               mode != Mode::CancelInCallback && mode != Mode::Credits &&
                               mode != Mode::Progress && mode != Mode::Nested;
    if (needsProducer)
    {
        int id = ee.createThread(EeThreadCreateParams{0, producer, 0, 0, 0, 10, 0});
        t.IsTrue(id > 1, "input progress thread must be created");
        t.Equals(ee.startThread(id, 0, context, false), 0, "input progress thread must start");
    }
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false, expired = false;
    std::thread guard([&] {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, std::chrono::seconds(1), [&] { return done; }))
        {
            expired = true;
            runtime.requestStop();
        }
    });
    auto join = [&] {
        {
            std::lock_guard lock(mutex);
            done = true;
        }
        changed.notify_one();
        guard.join();
    };
    try
    {
        ee.run();
    }
    catch (...)
    {
        join();
        active = nullptr;
        throw;
    }
    join();
    active = nullptr;
    t.IsFalse(expired, "watchdog cancellation is a failure, never success");
    t.IsTrue(s.completed, "real guest return continuation must execute");
    const bool canceled = mode == Mode::Reset || mode == Mode::Delete || mode == Mode::Recreate ||
                          mode == Mode::Restart || mode == Mode::Init || mode == Mode::CancelInCallback;
    t.Equals(s.result,
             canceled                ? cancelled
             : mode == Mode::Credits ? 25
                                     : 0,
             "GetPicture owns its result; actual=" + std::to_string(s.result));
    if (canceled)
        t.Equals(s.inputs, 1u, "old-generation callbacks must not run after cancellation");
    if (mode == Mode::Nested)
        t.IsTrue(s.events == std::vector<uint32_t>{1, 0, 1, 1},
                 "nested stream service must preserve startup, accepted-input and output ordering");
    if (mode == Mode::Ordering)
        t.IsTrue(s.events == std::vector<uint32_t>{4, 1}, "output must service UPDATE before NODATA");
    if (mode == Mode::Startup)
        t.IsTrue(s.events == std::vector<uint32_t>{1, 1},
                 "startup input service and later output service must both execute");
    if (mode == Mode::Retry || mode == Mode::Progress)
        t.IsTrue(s.events == std::vector<uint32_t>{1, 1, 1},
                 "only input arrival may retry; preserve after-output input service");
    if (mode == Mode::Empty || mode == Mode::Rejected)
        t.Equals(s.inputs, 1u, "EOF must finish without spinning or a timer retry");
    if (mode == Mode::Retry)
    {
        t.Equals(runtime.memory().readIORegister(ipu + 0x20), 2u,
                 "input availability alone must not earn decoder credit");
        t.Equals(runtime.memory().readIORegister(ipu + 0x10), dmaBase,
                 "input-ready events must not advance DMA MADR");
    }
    if (mode == Mode::Credits)
    {
        t.Equals(s.events.size(), size_t(2), "one rejected and one accepted callback must execute");
        t.Equals(runtime.memory().readIORegister(ipu + 0x20), 1u,
                 "only sixteen accepted PES bytes may retire one DMA qword");
        t.Equals(runtime.memory().readIORegister(ipu + 0x10), dmaBase + 16,
                 "MADR must reflect exact accepted payload bytes");
    }
}
} // namespace
int main()
{
    MiniTest::BeforeEach(reset_ps2_test_function_table);
    MiniTest::Case("MpegInput", [](TestCase &tc) {
        tc.Run("startup callback and caller ownership", [](TestCase &t) { run(t, Mode::Startup); });
        tc.Run("input arrival retries but VSync and zero bytes do not",
               [](TestCase &t) { run(t, Mode::Retry); });
        tc.Run("positive return without output blocks until EOF", [](TestCase &t) { run(t, Mode::Empty); });
        tc.Run("rejected input blocks until EOF", [](TestCase &t) { run(t, Mode::Rejected); });
        tc.Run("reset cancels a waiting generation", [](TestCase &t) { run(t, Mode::Reset); });
        tc.Run("delete cancels a waiting generation", [](TestCase &t) { run(t, Mode::Delete); });
        tc.Run("create cancels a waiting generation", [](TestCase &t) { run(t, Mode::Recreate); });
        tc.Run("stream restart cancels a waiting generation", [](TestCase &t) { run(t, Mode::Restart); });
        tc.Run("init cancels a waiting generation", [](TestCase &t) { run(t, Mode::Init); });
        tc.Run("reset inside callback cancels remaining callbacks",
               [](TestCase &t) { run(t, Mode::CancelInCallback); });
        tc.Run("output callback ordering", [](TestCase &t) { run(t, Mode::Ordering); });
        tc.Run("progress during startup callback is not lost", [](TestCase &t) { run(t, Mode::Progress); });
        tc.Run("NODATA can yield through demux and its stream callback",
               [](TestCase &t) { run(t, Mode::Nested); });
        tc.Run("PES rejection and exact DMA credits", [](TestCase &t) { run(t, Mode::Credits); });
    });
    return MiniTest::Run();
}
