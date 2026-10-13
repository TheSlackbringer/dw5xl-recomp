// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "runtime/gs/gs_backend.h"
#include <memory>

// GS rasterization and texture/VRAM operations execute in paraLLEl-GS Vulkan
// compute shaders. The native EE/IOP runtime and frontend remain in place.
class GSVulkanBackend final : public GSRasterBackend
{
public:
    GSVulkanBackend();
    ~GSVulkanBackend() override;
    void Initialize(uint8_t *, uint32_t) override;
    void Reset() override;
    void Submit(const GSPrimitiveBatch &) override;
    void LoadClut(const GSTex0Reg &, const GSTexClutReg &) override;
    void BeginTransfer(const GSTransferCommand &) override;
    void UploadImage(const uint8_t *, uint32_t) override;
    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason) override;
    PresentationFrame Present(const GSPresentationRequest &) override;
    bool ClearFramebuffer(const GSContext &, uint32_t) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *, uint32_t) override;
    uint32_t ReadVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) const override;
    void WriteVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override;
    void SnapshotVram(std::vector<uint8_t> &) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
