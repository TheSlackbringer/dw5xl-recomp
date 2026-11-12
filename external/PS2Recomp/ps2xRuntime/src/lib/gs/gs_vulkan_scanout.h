// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "runtime/gs/gs_read_ring.h"
#include "runtime/gs/gs_types.h"
#include "buffer.hpp"
#include "fence.hpp"
#include "image.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <mutex>
#include <vector>
#include <stdexcept>

namespace ParallelGS { class GSInterface; }
namespace Vulkan { class Device; }

class ReusableFieldWeaver;

// Only this exception denotes an unsupported logical viewport. Vulkan failures
// remain ordinary runtime errors and must not silently select another renderer.
class VulkanGsScanoutUnsupported : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

struct VulkanGsScanoutState
{
    struct RbSlot
    {
        Vulkan::BufferHandle buf;
        Vulkan::Fence fence;
        // "Is the card done with this read?" for every thread (set with the fence): questions go through it, and
        // so do the waits made without the device lock, so that a question never waits behind a wait.
        std::shared_ptr<ps2x::gs::ReadDone> done;
        Vulkan::ImageHandle image;
        uint32_t w = 0;
        uint32_t h = 0;
        uint32_t displayFbp = 0;
        uint32_t sourceFbp = 0;
        uint64_t vsyncTick = 0;
        uint64_t tag = 0; // issue 049: a picture read's tag, 0 for a field's read
        uint32_t displayWidth = 0;
        uint32_t displayHeight = 0;
        bool highResolution = false;
        bool frameFields = false;
        bool fieldMode = false;
        int8_t fieldTarget = -1; // issue 023 step 2K: the field the picture was drawn for, -1 if not known
        bool pending = false;
    };
    std::array<RbSlot, 3> slots{};
    uint32_t ringIndex = 0;
    // A completed readback, mapped under the device lock and copied into lastFrame after it is released
    // (finishVulkanGsPresent), so the GS worker is not kept waiting by the frame copy.
    struct StagedRead
    {
        Vulkan::BufferHandle buf;
        const uint8_t *mapped = nullptr;
        uint32_t w = 0;
        uint32_t h = 0;
        uint32_t displayFbp = 0;
        uint32_t sourceFbp = 0;
        uint64_t vsyncTick = 0;
        uint64_t tag = 0; // issue 049: a picture read's tag, 0 for a field's read
        uint32_t displayWidth = 0;
        uint32_t displayHeight = 0;
        bool highResolution = false;
        bool frameFields = false;
        bool fieldMode = false;
        int8_t fieldTarget = -1; // issue 023 step 2K: the field the picture was drawn for, -1 if not known
    };
    std::vector<StagedRead> staged;
    // Copied reads keep their buffers until the next presentation releases them under the device lock.
    std::vector<StagedRead> retired;
    // Readback buffers no read refers to any more (issue 023). A slot never reuses the buffer of a read that is
    // still staged or being copied: it takes one from here or a new one.
    std::vector<Vulkan::BufferHandle> freeBuffers;
    // Caller holds the device lock: the copied reads' buffers become free again.
    void recycleRetired()
    {
        for (auto &read : retired)
            keepFreeBuffer(std::move(read.buf));
        retired.clear();
    }
    // Keeps at most four free buffers, preferring the newest size (a size change, e.g. 4x 2560x1022 -> 2560x2048,
    // must not leave the pool full of buffers nobody can use).
    void keepFreeBuffer(Vulkan::BufferHandle &&buffer)
    {
        if (!buffer)
            return;
        if (freeBuffers.size() >= 4u)
        {
            auto other = std::find_if(freeBuffers.begin(), freeBuffers.end(), [&](const Vulkan::BufferHandle &free)
                                      { return free->get_create_info().size != buffer->get_create_info().size; });
            if (other == freeBuffers.end())
                return;
            freeBuffers.erase(other);
        }
        freeBuffers.push_back(std::move(buffer));
    }
    // Guards lastFrame between finishVulkanGsPresent (no device lock) and reset() (device lock held).
    std::mutex frameMutex;
    PresentationFrame lastFrame{};
    bool hasPresentedFrame = false;
    // Opt-in observation only. Does not select slots or alter fence handling.
    struct Metrics
    {
        uint64_t requests = 0, consumed = 0, consumeBackwards = 0;
        uint64_t duplicates = 0, returnedBackwards = 0, notReady = 0;
        uint64_t fullRingWaits = 0, waitTimeouts = 0;
        uint64_t waitNs = 0, maxWaitNs = 0, ageSum = 0, ageMax = 0;
        uint64_t consumedTick = 0, returnedTick = 0, lastLogNs = 0;
        bool hasConsumed = false, hasReturned = false;
    } metrics{};

    void reset()
    {
        for (auto &slot : slots)
        {
            slot.pending = false;
            slot.fence.reset();
            slot.done.reset();
            slot.image.reset();
            slot.buf.reset();
            slot.w = 0;
            slot.h = 0;
        }
        ringIndex = 0;
        staged.clear();
        retired.clear();
        freeBuffers.clear();
        {
            std::lock_guard<std::mutex> lock(frameMutex);
            lastFrame = {};
            hasPresentedFrame = false;
        }
        metrics = {};
    }
};

// Caller holds the device lock and advances the device frame context.
// Rasterization, VRAM sampling, circuit merge and field deinterlacing stay on
// the GPU. The RGBA readback bridges the existing fixed 640-stride host UI.
// Completed readbacks are only mapped here (state.staged); finishVulkanGsPresent copies them.
void presentVulkanGs(ParallelGS::GSInterface &gs, Vulkan::Device &device,
                     VulkanGsScanoutState &state,
                     const GSPresentationRequest &request,
                     uint32_t outputScale = 1,
                     bool flushBeforeVsync = true,
                     bool antiAliased = false,
                     bool pictureRead = false); // issue 049: an in-between picture's read (no progress lines)

// Issue 023. Caller holds the device lock. Stages the finished readbacks in field order without starting a new one,
// none of a field after maxField (fix 2: it stays for that field's own window frame).
void collectVulkanGsReads(Vulkan::Device &device, VulkanGsScanoutState &state, uint64_t maxField = UINT64_MAX);
// Caller holds the device lock. Whether the graphics card is done with the read in a place, as far as that is known
// without waiting (ps2x::gs::ReadDone). DW5XL-DEBUG PS2X_HOLD_TRACE: the question is timed by the asker's role.
bool vulkanGsReadFinished(VulkanGsScanoutState::RbSlot &slot);
// Issue 049, amendment v3.9. Caller holds the device lock. False: the ring's oldest place holds a read the graphics
// card has not finished; a picture's read would have to wait there, so it is not made.
bool vulkanGsPictureReadHasPlace(VulkanGsScanoutState &state);
// Caller holds the device lock. A wait for the oldest unfinished readback of field `field` or older (empty if they
// are all finished), announced here: the caller waits with it without the device lock, and lets it go before it
// asks about reads again.
ps2x::gs::ReadWait pendingReadWait(VulkanGsScanoutState &state, uint64_t field);

// Caller has released the device lock. Copies the reads staged by presentVulkanGs, in order, into
// outFrame (with the field weave), reusing outFrame.pixels capacity. Returns true if outFrame was updated.
bool finishVulkanGsPresentInto(Vulkan::Device &device, VulkanGsScanoutState &state,
                               std::vector<VulkanGsScanoutState::StagedRead> &reads,
                               ReusableFieldWeaver *weaver,
                               PresentationFrame &outFrame);

PresentationFrame finishVulkanGsPresent(Vulkan::Device &device, VulkanGsScanoutState &state,
                                        std::vector<VulkanGsScanoutState::StagedRead> &reads,
                                        ReusableFieldWeaver *weaver = nullptr);


