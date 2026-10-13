// Native dispatch for separately compiled SLES_538.60 overlays.
// Original loader/finalizer code owns RAM copying, BSS and constructors.
#include "game_overrides.h"
#include "dw5xl_overlay_validation.h"
#include <cstdio>

extern const RecompiledModule g_ps2Module_dw5xl_ovl00;
extern const RecompiledModule g_ps2Module_dw5xl_ovl01;

namespace {
constexpr uint32_t kFinalizer=0x00100260u;
constexpr uint32_t kQueueLoad=0x00145A50u;
constexpr uint32_t kOverlayBase=0x0061A800u;
const std::array<dw5xl::overlays::SupportedImage,2> supportedOverlays={{
    {&g_ps2Module_dw5xl_ovl00,1u,{
        0x1c,0xb3,0x03,0xe3,0x89,0xc2,0x15,0x41,0x2c,0x3a,0x10,0x4e,0x26,0xb8,0xe4,0x3d,
        0x91,0x05,0xf2,0x30,0xb3,0xb8,0xa7,0x65,0x64,0xed,0x1a,0x0e,0xb9,0x2d,0xa5,0x0f}},
    {&g_ps2Module_dw5xl_ovl01,2u,{
        0x16,0x08,0xd3,0xed,0x86,0x80,0x59,0x01,0x60,0x16,0x5d,0x51,0x83,0x00,0xd7,0x79,
        0xbb,0xb8,0x56,0x29,0x7d,0x6c,0x78,0xaa,0xe9,0x8b,0xfe,0xa3,0x4f,0x69,0x4e,0x18}}
}};
PS2Runtime::RecompiledFunction originalFinalizer=nullptr;
PS2Runtime::RecompiledFunction originalQueueLoad=nullptr;

void queueLoad(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime) {
    // Original145A50(blocking,resource,destination) can yield. Only its entry
    // is wrapped; original continuation aliases do not rerun invalidation.
    if(dw5xl::overlays::invalidateForLoadDestination(*runtime,getRegU32(ctx,6),kOverlayBase))
        std::fprintf(stderr,"[dw5xl:overlay] invalidated before queued load; resource=0x%x generation=%llu\n",
                     getRegU32(ctx,5),static_cast<unsigned long long>(runtime->compiledModuleGeneration()));
    originalQueueLoad(ram,ctx,runtime);
}

void finalizeOverlay(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime) {
    const uint32_t base=getRegU32(ctx,4);
    const uint32_t size=getRegU32(ctx,5);
    if(base!=kOverlayBase) {
        originalFinalizer(ram,ctx,runtime);
        return;
    }
    dw5xl::overlays::PreparedImage image;
    const char* error=nullptr;
    if(!dw5xl::overlays::prepareLoadedImage(*runtime,ram,PS2_RAM_SIZE,base,size,supportedOverlays,image,error)) {
        std::fprintf(stderr,"[dw5xl:overlay] refused activation: %s (loaded size=0x%x)\n",error,size);
        runtime->requestStop();
        return;
    }
    std::fprintf(stderr,"[dw5xl:overlay] activated %s before %u constructors; generation=%llu\n",
                 image.identity->module->identity,(image.ctorEnd-image.ctorBegin)/4,
                 static_cast<unsigned long long>(runtime->compiledModuleGeneration()));
    // Existing original resume aliases remain intact if finalization yields.
    originalFinalizer(ram,ctx,runtime);
}

void installOverlays(PS2Runtime& runtime) {
    if(runtime.memory().read32(kFinalizer)!=0x27BDFFB0u || !runtime.hasFunction(kFinalizer) ||
       runtime.memory().read32(kQueueLoad)!=0x27BDFFA0u || !runtime.hasFunction(kQueueLoad)) {
        std::fprintf(stderr,"[dw5xl:overlay] declined: unexpected loader or finalizer\n");
        return;
    }
    for(const auto& image:supportedOverlays)
        if(!runtime.registerCompiledModule(*image.module)) {
            std::fprintf(stderr,"[dw5xl:overlay] declined: invalid compiled module descriptor\n");
            return;
        }
    const auto finalizer=runtime.lookupFunction(kFinalizer);
    const auto loader=runtime.lookupFunction(kQueueLoad);
    if(finalizer!=&finalizeOverlay) originalFinalizer=finalizer;
    if(loader!=&queueLoad) originalQueueLoad=loader;
    if(!originalFinalizer || !originalQueueLoad) return;
    if(!runtime.replaceFunction(kQueueLoad,&queueLoad)) return;
    if(!runtime.replaceFunction(kFinalizer,&finalizeOverlay)) {
        runtime.replaceFunction(kQueueLoad,originalQueueLoad);
        return;
    }
    std::fprintf(stderr,"[dw5xl:overlay] installed verified OVL00/OVL01 loader and finalizer hooks\n");
}
}
PS2_REGISTER_GAME_OVERRIDE("DW5XL native overlays","SLES_538.60",0u,0u,&installOverlays)
