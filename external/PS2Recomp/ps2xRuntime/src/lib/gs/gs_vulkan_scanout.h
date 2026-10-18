// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "runtime/gs/gs_types.h"
#include "buffer.hpp"
#include "fence.hpp"
#include "image.hpp"

#include <array>
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
        Vulkan::ImageHandle image;
        uint32_t w = 0;
        uint32_t h = 0;
        uint32_t displayFbp = 0;
        uint32_t sourceFbp = 0;
        uint64_t vsyncTick = 0;
        uint32_t displayWidth = 0;
        uint32_t displayHeight = 0;
        bool highResolution = false;
        bool frameFields = false;
        bool fieldMode = false;
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
        uint32_t displayWidth = 0;
        uint32_t displayHeight = 0;
        bool highResolution = false;
        bool frameFields = false;
        bool fieldMode = false;
    };
    std::vector<StagedRead> staged;
    // Copied reads keep their buffers until the next presentation releases them under the device lock.
    std::vector<StagedRead> retired;
    // Guards lastFrame between finishVulkanGsPresent (no device lock) and reset() (device lock held).
    std::mutex frameMutex;
    PresentationFrame lastFrame{};
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
            slot.image.reset();
            slot.buf.reset();
            slot.w = 0;
            slot.h = 0;
        }
        ringIndex = 0;
        staged.clear();
        retired.clear();
        {
            std::lock_guard<std::mutex> lock(frameMutex);
            lastFrame = {};
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
                     uint32_t outputScale = 1);

// Caller has released the device lock. Copies the reads staged by presentVulkanGs, in order, into
// state.lastFrame (with the field weave) and returns the frame to show.
PresentationFrame finishVulkanGsPresent(Vulkan::Device &device, VulkanGsScanoutState &state,
                                        std::vector<VulkanGsScanoutState::StagedRead> &reads,
                                        ReusableFieldWeaver *weaver = nullptr);


