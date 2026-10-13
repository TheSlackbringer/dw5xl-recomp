// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "runtime/gs/gs_types.h"
#include <stdexcept>

namespace ParallelGS { class GSInterface; }
namespace Vulkan { class Device; }

// Only this exception denotes an unsupported logical viewport. Vulkan failures
// remain ordinary runtime errors and must not silently select another renderer.
class VulkanGsScanoutUnsupported : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// Caller serializes GS/device access and advances the device frame context.
// Rasterization, VRAM sampling, circuit merge and field deinterlacing stay on
// the GPU. The RGBA readback bridges the existing fixed 640-stride host UI.
PresentationFrame presentVulkanGs(ParallelGS::GSInterface &gs, Vulkan::Device &device,
                                  const GSPresentationRequest &request);
