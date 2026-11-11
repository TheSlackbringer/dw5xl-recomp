#pragma once

#include "runtime/gs/gs_types.h"

#include <functional>
#include <bitset>
#include <memory>
#include <cstdint>
#include <vector>

namespace ps2x { class StateArchive; }

class GSRasterBackend
{
public:
    virtual ~GSRasterBackend() = default;

    virtual void Initialize(uint8_t *vram, uint32_t vramSize) = 0;
    virtual void Reset() = 0;

    virtual void Submit(const GSPrimitiveBatch &batch) = 0;
    virtual void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) = 0;

    virtual void BeginTransfer(const GSTransferCommand &command) = 0;
    virtual void UploadImage(const uint8_t *data, uint32_t sizeBytes) = 0;

    virtual void Flush() = 0;
    virtual void TextureFlush() = 0;
    virtual void Sync(GSSyncReason reason) = 0;
    virtual PresentationFrame Present(const GSPresentationRequest &request) = 0;
    virtual bool PresentInto(const GSPresentationRequest &request, PresentationFrame &out)
    {
        out = Present(request);
        return true;
    }

    // Issue 023: end-of-field scanout. SubmitFieldScanout runs on the GS side in order with the game's GS work, at the
    // end of field request.vsyncTick, and starts reading that field; CollectFieldScanouts (window thread) turns the
    // finished reads into `out`, first waiting for the read of field `waitForField` if it is not UINT64_MAX; reads of
    // fields after `maxField` stay for a later call (issue 023 fix 2: a frame shows exactly the field it was given).
    // Renderers without it return false and the window reads at host time (Present/PresentInto).
    virtual bool SubmitFieldScanout(const GSPresentationRequest &request)
    {
        (void)request;
        return false;
    }
    virtual bool CollectFieldScanouts(PresentationFrame &out, uint64_t waitForField, uint64_t maxField = UINT64_MAX)
    {
        (void)out;
        (void)waitForField;
        (void)maxField;
        return false;
    }

    virtual bool ClearFramebuffer(const GSContext &context, uint32_t rgba) = 0;
    virtual uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) = 0;

    virtual uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const = 0;
    virtual void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) = 0;
    virtual void SnapshotVram(std::vector<uint8_t> &out) const = 0;
    virtual GSTransferSnapshot GetTransferSnapshot() const = 0;

    // DW5XL (issue 027): renderer state for save states (VRAM the renderer owns, register shadows, transfers).
    // Called on the EE executor at a quiet point after a flush. Renderers without support refuse.
    virtual void SerializeState(ps2x::StateArchive &archive);

    // Issue 049 v3.2: a rollback point for extra pictures drawn between two of the game's. SaveRollback keeps the
    // given VRAM pages (one bit per 8 KB page), the renderer's registers and the palette as they are after
    // everything submitted so far; Rollback puts them back after everything submitted since. GS side, in stream
    // order. Renderers without it return false (then no extra pictures are drawn).
    virtual bool SaveRollback(const std::bitset<512> &pages)
    {
        (void)pages;
        return false;
    }
    virtual bool Rollback() { return false; }
    // Pages that are about to be written behind the rollback point and are not kept yet: keeps them too, as they
    // are after everything submitted so far. Returns the pages added (0: none, or no rollback point).
    virtual uint32_t AddRollbackPages(const std::bitset<512> &pages)
    {
        (void)pages;
        return 0;
    }
    // Bytes the last SaveRollback copied (0: none).
    virtual uint64_t RollbackBytes() const { return 0; }
    // Issue 049 (fact-finding 2026-11-11): a second GS memory inside the same renderer, on the same graphics
    // device, with registers and VRAM of its own: what is drawn there never touches this one. Its output scale is
    // this one's. It must be destroyed before this one. Renderers without it return nothing.
    virtual std::unique_ptr<GSRasterBackend> MakeSecondMemory() { return nullptr; }
    // The given VRAM pages of `source` (a memory of the same renderer), its palette and its registers as they are
    // after everything submitted to it so far, copied into this memory on the graphics card, after everything
    // submitted to this one so far. False when the two do not share a device.
    virtual bool CopyPagesFrom(GSRasterBackend &source, const std::bitset<512> &pages)
    {
        (void)source;
        (void)pages;
        return false;
    }
    // Issue 049 v3.2: the read of an in-between picture: the displayed picture as a field's read gives it, started
    // GS side in stream order, with request.pictureTag. It has a queue of its own: the field reads and their
    // bookkeeping are not touched. CollectPictureReads (any thread): the finished reads, oldest first, appended.
    virtual bool SubmitPictureRead(const GSPresentationRequest &request)
    {
        (void)request;
        return false;
    }
    virtual bool CollectPictureReads(std::vector<GSPictureRead> &out)
    {
        (void)out;
        return false;
    }
    // Issue 049 v3.5: a function that waits until the oldest picture read still in the works is finished (at most
    // the given nanoseconds). It is called with no lock of the GS held, so a thread that waits for picture reads
    // never keeps the window's field read or the GS waiting. Empty: no read is in the works. `collectable`: a
    // finished read is waiting to be collected.
    virtual std::function<void(uint64_t)> PictureReadWaiter(bool &collectable)
    {
        collectable = false;
        return {};
    }

    virtual void SetResolutionScale(uint32_t scale) { (void)scale; }
    virtual uint32_t GetResolutionScale() const { return 1; }
    // Anti-aliasing samples per output pixel, independent of the output scale.
    virtual void SetAASamples(uint32_t samples) { (void)samples; }
    virtual uint32_t GetAASamples() const { return 1; }
    void setResolutionScale(uint32_t scale) { SetResolutionScale(scale); }
    uint32_t getResolutionScale() const { return GetResolutionScale(); }
};
