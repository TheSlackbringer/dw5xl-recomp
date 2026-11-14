// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "runtime/gs/gs_backend.h"
#include <memory>

struct GSVulkanPresenterLink;

// GS rasterization and texture/VRAM operations execute in paraLLEl-GS Vulkan
// compute shaders. The native EE/IOP runtime and frontend remain in place.
class GSVulkanBackend final : public GSRasterBackend
{
public:
    GSVulkanBackend();
    ~GSVulkanBackend() override;
    void Initialize(uint8_t *, uint32_t) override;
    void Reset() override;
    void SerializeState(ps2x::StateArchive &archive) override; // DW5XL (issue 027)
    std::unique_ptr<GSRasterBackend> MakeSecondMemory() override; // DW5XL (issue 049, fact-finding)
    // DW5XL (issue 049, amendment v3.12 step 2): whether this renderer's graphics device was made with window
    // support (the system's surface extensions and the swap chain extension), so that a presenter can show its
    // pictures without reading them back.
    bool WindowSupport() const;
    // DW5XL (issue 049, amendment v3.12 step 3): what a presenter needs to show this renderer's pictures without
    // reading them back: the Vulkan context to make a device object of its own on, the lock for the queues, and
    // the hand-over where the game's fields arrive on the graphics card once the presenter switches it on. The type
    // is in a header of the runtime's own (src/lib/gs/gs_vulkan_handover.h). The same link for every caller.
    std::shared_ptr<GSVulkanPresenterLink> PresenterLink() override;
    bool CopyPagesFrom(GSRasterBackend &source, const std::bitset<512> &pages) override;
    bool SubmitPictureRead(const GSPresentationRequest &) override;
    bool CollectPictureReads(std::vector<GSPictureRead> &) override;
    bool PictureReadHasPlace() override;
    std::function<void(uint64_t)> PictureReadWaiter(bool &collectable) override;
    void Submit(const GSPrimitiveBatch &) override;
    void LoadClut(const GSTex0Reg &, const GSTexClutReg &) override;
    void BeginTransfer(const GSTransferCommand &) override;
    void UploadImage(const uint8_t *, uint32_t) override;
    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason) override;
    PresentationFrame Present(const GSPresentationRequest &) override;
    bool PresentInto(const GSPresentationRequest &, PresentationFrame &) override;
    bool SubmitFieldScanout(const GSPresentationRequest &) override;
    bool CollectFieldScanouts(PresentationFrame &, uint64_t, uint64_t) override;
    bool ClearFramebuffer(const GSContext &, uint32_t) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *, uint32_t) override;
    uint32_t ReadVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) const override;
    void WriteVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override;
    void SnapshotVram(std::vector<uint8_t> &) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;
    void SetResolutionScale(uint32_t scale) override;
    uint32_t GetResolutionScale() const override;
    void SetAASamples(uint32_t samples) override;
    uint32_t GetAASamples() const override;
    // DW5XL-TEST (issue 049 v3.7): the stretches of work (render passes) this memory's renderer has ended since the
    // last call; what is still open is flushed first.
    uint32_t TakeRenderPasses();
private:
    struct Impl;
    explicit GSVulkanBackend(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};
