#pragma once

#include "ps2_runtime.h"
extern "C" {
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}
#include <array>
#include <cstring>
#include <memory>
#include <span>

namespace dw5xl::overlays {
struct SupportedImage {
    const RecompiledModule* module;
    uint32_t moduleId;
    std::array<uint8_t,32> sha256;
};
struct PreparedImage {
    const SupportedImage* identity = nullptr;
    uint32_t ctorBegin = 0;
    uint32_t ctorEnd = 0;
};
inline uint32_t readWord(const uint8_t* ram, uint32_t address) {
    uint32_t value;
    std::memcpy(&value,ram+address,sizeof(value));
    return value;
}

// The original queue loader receives its destination before any file copy.
// Resume aliases stay with the original loader, so this runs once per request.
inline bool invalidateForLoadDestination(PS2Runtime& runtime,uint32_t destination,uint32_t overlayBase) {
    if(destination!=overlayBase) return false;
    runtime.deactivateCompiledModule();
    return true;
}

// Verify completed original bytes, activate the matching immutable code table,
// and validate constructors. BSS writes and constructor execution stay original.
inline bool prepareLoadedImage(PS2Runtime& runtime,const uint8_t* ram,uint32_t ramSize,
                               uint32_t base,uint32_t size,std::span<const SupportedImage> supported,
                               PreparedImage& result,const char*& error) {
    result={};
    error=nullptr;
    runtime.deactivateCompiledModule();
    const auto reject=[&](const char* reason) {
        error=reason;
        runtime.deactivateCompiledModule();
        return false;
    };
    if(!ram || size<0x80u || uint64_t(base)+size>ramSize)
        return reject("invalid loaded image range");
    bool knownSize=false;
    for(const auto& image:supported)
        if(image.module && image.module->guestBase==base && image.module->fileSize==size) knownSize=true;
    if(!knownSize) return reject("unsupported module size or base");
    auto sha=std::unique_ptr<AVSHA,decltype(&av_free)>(av_sha_alloc(),&av_free);
    std::array<uint8_t,32> digest{};
    if(!sha || av_sha_init(sha.get(),256)<0) return reject("SHA256 initialization failed");
    av_sha_update(sha.get(),ram+base,size);
    av_sha_final(sha.get(),digest.data());
    const SupportedImage* selected=nullptr;
    for(const auto& image:supported)
        if(image.module && image.module->guestBase==base && image.module->fileSize==size && image.sha256==digest) {
            selected=&image;
            break;
        }
    if(!selected) return reject("loaded bytes do not match a supported overlay");
    if(readWord(ram,base)!=0x336F574Du || readWord(ram,base+4)!=selected->moduleId || readWord(ram,base+8)!=base)
        return reject("unexpected module header identity");
    const uint32_t bssSize=readWord(ram,base+0x14);
    const uint32_t ctorBegin=readWord(ram,base+0x18);
    const uint32_t ctorEnd=readWord(ram,base+0x1c);
    if(uint64_t(base)+size+bssSize>ramSize || ctorBegin>ctorEnd ||
       (ctorBegin&3u) || (ctorEnd&3u) || ctorBegin<base || ctorEnd>base+size)
        return reject("invalid BSS or constructor bounds");
    if(!runtime.activateCompiledModule(selected->module->identity))
        return reject("compiled overlay table unavailable");
    for(uint32_t pointer=ctorBegin;pointer<ctorEnd;pointer+=4)
        if(!runtime.hasFunction(readWord(ram,pointer)))
            return reject("constructor has no compiled target");
    result={selected,ctorBegin,ctorEnd};
    return true;
}
}
