#ifndef PS2_GS_FRONTEND_H
#define PS2_GS_FRONTEND_H

#include <array>
#include <atomic>
#include <bitset>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "runtime/gs/gs_backend.h"

struct GSDebugSnapshot
{
    GSContext ctx[2]{};
    GSPrimReg prim{};
    GSTexaReg texa{};
    GSTexClutReg texclut{};
    uint64_t scanmsk = 0;
    uint64_t dimx = 0;
    uint64_t dthe = 0;
    uint64_t colclamp = 0;
    GSBitBltBuf bitbltbuf{};
    GSTrxPos trxpos{};
    GSTrxReg trxreg{};
    uint32_t trxdir = 0;
    uint32_t transferX = 0;
    uint32_t transferY = 0;
    uint32_t transferTotalPixels = 0;
    uint32_t transferCopiedPixels = 0;
    uint32_t lastDisplayBaseBytes = 0;
    GSFrameReg preferredDisplaySourceFrame{};
    uint32_t preferredDisplayDestFbp = 0;
    bool hasPreferredDisplaySource = false;
    uint32_t hostPresentationWidth = 0;
    uint32_t hostPresentationHeight = 0;
    uint32_t hostPresentationDisplayFbp = 0;
    uint32_t hostPresentationSourceFbp = 0;
    bool hostPresentationUsedPreferred = false;
    bool hasHostPresentationFrame = false;
    size_t localToHostPendingBytes = 0;
};

enum class GSDebugEventKind : uint8_t
{
    GifTag = 0,
    Register = 1,
    Draw = 2,
    Transfer = 3,
    Present = 4,
};

struct GSDebugHistoryEntry
{
    uint64_t seq = 0;
    uint64_t vsyncTick = 0;
    uint32_t frameIndex = 0;
    GSDebugEventKind kind = GSDebugEventKind::Register;

    uint8_t reg = 0;
    uint64_t regValue = 0;

    uint32_t gifSizeBytes = 0;
    uint32_t gifNloop = 0;
    uint8_t gifFlg = 0;
    uint8_t gifNreg = 0;

    GSPrimReg prim{};
    GSFrameReg frame{};
    GSZbufReg zbuf{};
    GSTex0Reg tex0{};
    GSScissorReg scissor{};
    uint64_t test = 0;
    uint64_t alpha = 0;

    uint32_t vertexCount = 0;
    float xMin = 0.0f;
    float xMax = 0.0f;
    float yMin = 0.0f;
    float yMax = 0.0f;
    double zMin = 0.0;
    double zMax = 0.0;
    uint8_t aMin = 0;
    uint8_t aMax = 0;

    GSBitBltBuf bitbltbuf{};
    GSTrxPos trxpos{};
    GSTrxReg trxreg{};
    uint32_t trxdir = 0;
    uint32_t transferPixels = 0;

    uint32_t displayFbp = 0;
    uint32_t sourceFbp = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool usedPreferred = false;
};

namespace ps2x { class StateArchive; }

class GS
{
public:
    // DW5XL (issue 027): GS state for save states (drawing registers, vertex queue, GIF path progress, renderer).
    void serializeState(ps2x::StateArchive &archive);
    GS();
    ~GS() = default;

    void init(uint8_t *vram, uint32_t vramSize, struct GSRegisters *privRegs = nullptr);
    void reset();
    // GIF_CTRL.RST resets packet input, not GS registers or VRAM.
    void resetGifInput();
    void setRasterBackend(std::unique_ptr<GSRasterBackend> backend);

    void processGIFPacket(const uint8_t *data, uint32_t sizeBytes);
    bool processNativePackedGIFPacket(const uint8_t *data, uint32_t sizeBytes);
    void uploadImageNative(uint64_t bitbltbuf,
                           uint64_t trxpos,
                           uint64_t trxreg,
                           uint64_t trxdir,
                           const uint8_t *data,
                           uint32_t sizeBytes);
    void writeRegister(uint8_t regAddr, uint64_t value);

    const uint8_t *lockDisplaySnapshot(uint32_t &outSize);
    void unlockDisplaySnapshot();
    uint32_t getLastDisplayBaseBytes() const;
    const GSFrameReg &getContextFrame(int index) const
    {
        return m_ctx[(index != 0) ? 1 : 0].frame;
    }
    GSDebugSnapshot getDebugSnapshot() const;
    std::vector<GSDebugHistoryEntry> getDebugHistory() const;
    void clearDebugHistory();
    bool isDebugHistoryPaused() const;
    void setDebugHistoryPaused(bool paused);
    bool getPreferredDisplaySource(GSFrameReg &outSource, uint32_t &outDestFbp) const;
    // Issue 023: end-of-field scanout. At the end of each field (EE VBlankStart) the runtime captures the display
    // registers in the order of the game's register writes (captureFieldScanoutRequest, device order) and hands
    // them to the GS side behind the game's GS work (submitFieldScanout, GS worker order), which starts reading the
    // field. The window then only collects finished field reads (latchHostPresentationFrame).
    GSPresentationRequest captureFieldScanoutRequest(uint64_t fieldTick) const;
    void submitFieldScanout(const GSPresentationRequest &captured);
    bool fieldScanoutsActive() const { return m_fieldScanouts.load(std::memory_order_acquire); }
    // waitForField: the field the window is about to show (field pacing), UINT64_MAX = take what is finished.
    void latchHostPresentationFrame(uint64_t waitForField = UINT64_MAX);
    uint64_t hostPresentationGeneration() const;
    bool copyLatchedHostPresentationFrame(std::vector<uint8_t> &outPixels,
                                          uint32_t &outWidth,
                                          uint32_t &outHeight,
                                          uint32_t *outDisplayFbp = nullptr,
                                          uint32_t *outSourceFbp = nullptr,
                                          bool *outUsedPreferred = nullptr,
                                          uint32_t *outDisplayWidth = nullptr,
                                          uint32_t *outDisplayHeight = nullptr) const;
    bool clearFramebufferContext(uint32_t contextIndex, uint32_t rgba);
    bool clearActiveFramebuffer(uint32_t rgba);
    uint64_t nativeImageUploadCount() const { return m_nativeImageUploadCount; }
    uint64_t nativePackedGIFPacketCount() const { return m_nativePackedGIFPacketCount; }

    uint32_t consumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes);

    void refreshDisplaySnapshot();

    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value);
    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const;

    void setResolutionScale(uint32_t scale);
    uint32_t getResolutionScale() const;
    void setAASamples(uint32_t samples);
    uint32_t getAASamples() const;
    void SetResolutionScale(uint32_t scale) { setResolutionScale(scale); }
    uint32_t GetResolutionScale() const { return getResolutionScale(); }

private:
    void snapshotVRAM();
    void writeRegisterUnlocked(uint8_t regAddr, uint64_t value);
    void writeRegisterPacked(uint8_t regDesc, uint64_t lo, uint64_t hi);
    void uploadImageNativeUnlocked(uint64_t bitbltbuf,
                                   uint64_t trxpos,
                                   uint64_t trxreg,
                                   uint64_t trxdir,
                                   const uint8_t *data,
                                   uint32_t sizeBytes);
    void vertexKick(bool drawing);

    void recordDebugEventUnlocked(GSDebugHistoryEntry entry);
    GSDebugHistoryEntry makeDebugEventUnlocked(GSDebugEventKind kind) const;
    void recordGifTagDebugEventUnlocked(uint32_t sizeBytes, uint32_t nloop, uint8_t flg, uint32_t nreg);
    void recordRegisterDebugEventUnlocked(uint8_t regAddr, uint64_t value);
    void recordDrawDebugEventUnlocked(int vertexCount);
    void recordTransferDebugEventUnlocked();
    void recordPresentDebugEventUnlocked(uint32_t displayFbp, uint32_t sourceFbp, uint32_t width, uint32_t height, bool usedPreferred);

    void processImageData(const uint8_t *data, uint32_t sizeBytes);
    bool tryProcessNativeImageUploadPacket(const uint8_t *data, uint32_t sizeBytes);
    GSPrimitiveBatch buildDrawBatch(int vertexCount) const;
    void updatePreferredDisplaySourceForDraw(const GSPrimitiveBatch &batch);
    GSPresentationRequest buildPresentationRequestUnlocked() const;


    struct GifPathResumeState
    {
        uint32_t remaining = 0;
        uint32_t consumed = 0;
        uint32_t nreg = 0;
        uint8_t flg = 0;
        bool reglistPad = false;
        uint8_t regs[16] = {};
    };
    std::array<GifPathResumeState, 4> m_gifPathStates{};
    GifPathResumeState &gifPathState();
    bool gifPathMidTag();

    GSContext &activeContext();

    uint8_t *m_localMemoryStorage = nullptr;
    uint32_t m_localMemorySize = 0u;
    struct GSRegisters *m_privRegs = nullptr;
    mutable std::recursive_mutex m_stateMutex;
    mutable std::mutex m_backendLifetimeMutex;
    mutable std::mutex m_presentationMutex;

    GSContext m_ctx[2];
    GSPrimReg m_prim{};
    GSPrimReg m_primRegister{};
    GSPrimReg m_prmodeRegister{};

    uint8_t m_curR = 0x80, m_curG = 0x80, m_curB = 0x80, m_curA = 0x80;
    float m_curQ = 1.0f;
    float m_curS = 0.0f, m_curT = 0.0f;
    uint16_t m_curU = 0, m_curV = 0;
    uint8_t m_curFog = 0;
    uint8_t m_fogR = 0, m_fogG = 0, m_fogB = 0;

    bool m_prmodecont = true;
    bool m_pabe = false;
    uint64_t m_scanmsk = 0;
    uint64_t m_dimx = 0;
    uint64_t m_dthe = 0;
    uint64_t m_colclamp = 0;
    GSTexaReg m_texa{0u, false, 0u};
    GSTexClutReg m_texclut{0u, 0u, 0u};

    GSBitBltBuf m_bitbltbuf{};
    GSTrxPos m_trxpos{};
    GSTrxReg m_trxreg{};
    uint32_t m_trxdir = 3;


    static constexpr int kMaxVerts = 6;
    GSVertex m_vtxQueue[kMaxVerts];
    int m_vtxCount = 0;
    int m_vtxIndex = 0;

    std::vector<uint8_t> m_displaySnapshot;
    std::mutex m_snapshotMutex;
    uint32_t m_lastDisplayBaseBytes = 0;
    GSFrameReg m_preferredDisplaySourceFrame{};
    uint32_t m_preferredDisplayDestFbp = 0;
    bool m_hasPreferredDisplaySource = false;
    PresentationFrame m_latchedFrame{};
    std::vector<uint8_t> m_hostPresentationFrame;
    uint32_t m_hostPresentationWidth = 0;
    uint32_t m_hostPresentationHeight = 0;
    uint32_t m_hostPresentationStride = 0;
    uint32_t m_hostDisplayWidth = 0;
    uint32_t m_hostDisplayHeight = 0;
    uint32_t m_hostPresentationDisplayFbp = 0;
    uint32_t m_hostPresentationSourceFbp = 0;
    bool m_hostPresentationUsedPreferred = false;
    bool m_hasHostPresentationFrame = false;
    uint64_t m_hostPresentationGeneration = 0;
    // Issue 023: field scanouts run on the GS side (false: the window reads at host time, e.g. the CPU renderer).
    std::atomic<bool> m_fieldScanouts{false};
    std::mutex m_fieldScanoutMutex;
    std::condition_variable m_fieldScanoutCv;
    uint64_t m_lastSubmittedField = 0;
    bool m_hasSubmittedField = false;
    // Issue 023 step 2R: a field read is new only if, since the previous read, a GS write touched the displayed
    // buffer or the display registers changed; otherwise it is a repeat and the window keeps the previous picture.
    // GS side, in stream order (under m_stateMutex).
    struct DisplayRegs
    {
        uint64_t pmode = 0, smode2 = 0, dispfb1 = 0, display1 = 0, dispfb2 = 0, display2 = 0, bgcolor = 0;
        bool operator==(const DisplayRegs &o) const
        {
            return pmode == o.pmode && smode2 == o.smode2 && dispfb1 == o.dispfb1 && display1 == o.display1 &&
                   dispfb2 == o.dispfb2 && display2 == o.display2 && bgcolor == o.bgcolor;
        }
    };
    void noteDisplayWriteUnlocked(uint32_t basePage, uint32_t fbw, uint8_t psm, uint32_t x0, uint32_t y0,
                                  uint32_t x1, uint32_t y1);
    void noteDrawWriteUnlocked(const GSContext &context, bool textured = false);
    const std::bitset<512> &displayPagesUnlocked();
    std::bitset<512> m_displayPages;
    DisplayRegs m_displayPagesRegs{};
    bool m_displayPagesValid = false;
    uint64_t m_displayPagesGeneration = 0;
    struct DrawOverlap
    {
        uint64_t frameKey = ~0ull, scissorKey = ~0ull, displayGeneration = ~0ull;
        bool overlaps = false;
    } m_drawOverlap;
    uint64_t m_displayWritesSinceRead = 0;
    // Issue 023 step 2K, per frame buffer (FBP): the field its picture was drawn for — 1 + the half-line bit of
    // XYOFFSET.ofy of the draws into it (1 even, 2 odd), carried along by a draw textured from another buffer's base
    // (a copy); 0 = not known (never drawn, or written by a transfer).
    std::array<uint8_t, 512> m_pictureTarget{};
    DisplayRegs m_lastFieldReadRegs{};
    bool m_hasLastFieldRead = false;
    // Output scale/AA change, reset or a loaded save state: the next field is read even without a GS write.
    std::atomic<bool> m_forceFieldRead{true};
    uint64_t m_nativeImageUploadCount = 0;
    uint64_t m_nativePackedGIFPacketCount = 0;

    static constexpr size_t kDebugHistoryCapacity = 512;
    std::array<GSDebugHistoryEntry, kDebugHistoryCapacity> m_debugHistory{};
    size_t m_debugHistoryWrite = 0;
    size_t m_debugHistoryCount = 0;
    uint64_t m_debugNextSeq = 1;
    uint32_t m_debugFrameIndex = 0;
    uint64_t m_debugLastVsyncTick = UINT64_MAX;
    bool m_debugHistoryPaused = true;

    std::unique_ptr<GSRasterBackend> m_backend;
};

#endif
