// The GS worker must be stopped before the GS and its renderer go away when a PS2Runtime ends.
// Found 2026-11-11: dw5xl_vif1_replay (CTest gsreplay_synthetic) crashed at exit on a busy PC, about 4 runs in 100
// with 16 at a time. PS2Runtime declares m_gifArbiter before m_gs, so m_gs and its renderer were destroyed first
// and the arbiter's destructor joined the GS worker only afterwards, while the worker could still be working
// through the destroyed GS (SIGSEGV in GS::vertexKick / the renderer's Submit).
// The test keeps the GS worker busy for a moment and ends the runtime meanwhile: the renderer must still be alive
// when the worker's work finishes. No timing is judged; the pause only makes the old order show every time.
#include "ps2_runtime.h"
#include "runtime/gs/gs_backend.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>

extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x1000;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x1004;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 1;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[] = {nullptr};

namespace
{
    struct Flags
    {
        std::atomic<bool> rendererGone{false};
        std::atomic<bool> workRan{false};
        std::atomic<bool> rendererGoneBeforeWorkEnded{false};
        std::atomic<bool> onWorkerThread{false};
    };

    // A renderer that does nothing and says when it is destroyed.
    class WatchedBackend final : public GSRasterBackend
    {
    public:
        explicit WatchedBackend(std::shared_ptr<Flags> flags) : m_flags(std::move(flags)) {}
        ~WatchedBackend() override { m_flags->rendererGone.store(true); }

        void Initialize(uint8_t *, uint32_t) override {}
        void Reset() override {}
        void Submit(const GSPrimitiveBatch &) override {}
        void LoadClut(const GSTex0Reg &, const GSTexClutReg &) override {}
        void BeginTransfer(const GSTransferCommand &) override {}
        void UploadImage(const uint8_t *, uint32_t) override {}
        void Flush() override {}
        void TextureFlush() override {}
        void Sync(GSSyncReason) override {}
        PresentationFrame Present(const GSPresentationRequest &) override { return {}; }
        bool ClearFramebuffer(const GSContext &, uint32_t) override { return false; }
        uint32_t ConsumeLocalToHostBytes(uint8_t *, uint32_t) override { return 0; }
        uint32_t ReadVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) const override { return 0; }
        void WriteVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
        void SnapshotVram(std::vector<uint8_t> &out) const override { out.clear(); }
        GSTransferSnapshot GetTransferSnapshot() const override { return {}; }

    private:
        std::shared_ptr<Flags> m_flags;
    };
}

int main()
{
#if defined(_WIN32)
    _putenv_s("PS2X_GS_BACKEND", "cpu");
    _putenv_s("PS2X_GS_RECORD", "");
#else
    setenv("PS2X_GS_BACKEND", "cpu", 1); // no Vulkan device for the renderer the GS builds before ours goes in
    unsetenv("PS2X_GS_RECORD");
#endif
    auto flags = std::make_shared<Flags>();
    const std::thread::id mainThread = std::this_thread::get_id();
    {
        auto rt = std::make_unique<PS2Runtime>();
        if (!rt->memory().initialize() || !rt->syncCoreSubsystems())
        {
            std::cerr << "FAIL: the runtime did not set up\n";
            return 1;
        }
        rt->gs().setRasterBackend(std::make_unique<WatchedBackend>(flags));
        rt->gifArbiter().setWorkerEnabled(true);
        rt->gifArbiter().submitCall([flags, mainThread] {
            flags->onWorkerThread.store(std::this_thread::get_id() != mainThread);
            std::this_thread::sleep_for(std::chrono::milliseconds(300)); // the GS worker is still busy
            flags->rendererGoneBeforeWorkEnded.store(flags->rendererGone.load());
            flags->workRan.store(true);
        });
        rt.reset(); // the runtime ends while that work is going on
    }
    if (!flags->workRan.load())
    {
        std::cerr << "FAIL: the GS work never ran\n";
        return 1;
    }
    if (!flags->onWorkerThread.load())
    {
        std::cerr << "FAIL: the GS work ran on the calling thread: no GS worker, nothing was tested\n";
        return 1;
    }
    if (flags->rendererGoneBeforeWorkEnded.load())
    {
        std::cerr << "FAIL: the renderer was destroyed while the GS worker was still working\n";
        return 1;
    }
    if (!flags->rendererGone.load())
    {
        std::cerr << "FAIL: the renderer was never destroyed\n";
        return 1;
    }
    std::cout << "PASS: the GS worker finished before the renderer was destroyed\n";
    return 0;
}
