// Native dispatch for separately compiled SLES_538.60 overlays.
// Original loader/finalizer code owns RAM copying, BSS and constructors.
#include "game_overrides.h"
#include "dw5xl_disc_validation.h"
#include "runtime/ee_scheduler.h"
#include "ps2_runtime_macros.h"
#include <string>
#include <cstring>
#include <cstdio>

extern const RecompiledModule g_ps2Module_dw5xl_ovl00;
extern const RecompiledModule g_ps2Module_dw5xl_ovl01;
extern const RecompiledModule g_ps2Module_dw5xl_ovl02;

namespace {
constexpr uint32_t kFinalizer=0x00100260u;
constexpr uint32_t kQueueLoad=0x00145A50u;
constexpr uint32_t kOverlayBase=0x0061A800u;
std::array<dw5xl::overlays::SupportedImage,3> supportedOverlays={{
    {&g_ps2Module_dw5xl_ovl00,1u,{
        0x1c,0xb3,0x03,0xe3,0x89,0xc2,0x15,0x41,0x2c,0x3a,0x10,0x4e,0x26,0xb8,0xe4,0x3d,
        0x91,0x05,0xf2,0x30,0xb3,0xb8,0xa7,0x65,0x64,0xed,0x1a,0x0e,0xb9,0x2d,0xa5,0x0f}},
    {&g_ps2Module_dw5xl_ovl01,2u,{
        0x16,0x08,0xd3,0xed,0x86,0x80,0x59,0x01,0x60,0x16,0x5d,0x51,0x83,0x00,0xd7,0x79,
        0xbb,0xb8,0x56,0x29,0x7d,0x6c,0x78,0xaa,0xe9,0x8b,0xfe,0xa3,0x4f,0x69,0x4e,0x18}},
    {&g_ps2Module_dw5xl_ovl02,3u,{
        0xdc,0x4a,0x89,0xaa,0xfa,0x44,0x2e,0x40,0xcd,0xcc,0x9c,0x25,0xd6,0x02,0x86,0xcc,
        0x05,0x9e,0x9e,0x05,0x68,0xf6,0xe2,0x70,0xf6,0x84,0x70,0x50,0x88,0xdc,0x17,0x27}}
}};
PS2Runtime::RecompiledFunction originalFinalizer=nullptr;
PS2Runtime::RecompiledFunction originalQueueLoad=nullptr;

// One verification session survives EE yielding. Other threads and modules
// continue through the ordinary disc implementation.
constexpr uint32_t kDiscCheck=0x00637E90u;
constexpr uint32_t kDiscCheckEnd=0x006381A4u;
dw5xl::discs::ModuleWithOverride adaptedOvl02;
struct DiscCheckSession {
    PS2Runtime* runtime=nullptr;
    int threadId=0;
    uint32_t returnPc=0,stack=0,which=0;
    dw5xl::discs::Validation validation=dw5xl::discs::Validation::missing;
    void clear(){*this={};}
} discCheckSession;
std::array<PS2Runtime::RecompiledFunction,4> originalDiscCalls{};
constexpr std::array<uint32_t,4> discCallAddresses={0x0012D2E0u,0x0012CB80u,0x0012D220u,0x0012C158u};

bool scopedDiscCall(PS2Runtime* runtime,R5900Context* ctx,uint32_t first,uint32_t second=0) {
    const auto ra=getRegU32(ctx,31);
    return discCheckSession.runtime==runtime &&
        runtime->activeCompiledModule()==&adaptedOvl02.descriptor &&
        runtime->eeScheduler().currentThreadId()==discCheckSession.threadId &&
        (ra==first || (second && ra==second));
}
void finishDiscCall(R5900Context* ctx,int32_t value) {
    setReturnS32(ctx,value);ctx->pc=getRegU32(ctx,31);
}
void discStatus(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime) {
    if(!scopedDiscCall(runtime,ctx,0x637F24u,0x637F40u)) {originalDiscCalls[0](ram,ctx,runtime);return;}
    // This address is sceCdStatus in this PAL executable, not sceCdTrayReq.
    finishDiscCall(ctx,getRegU32(ctx,31)==0x637F24u?1:0);
}
void discReady(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime) {
    if(!scopedDiscCall(runtime,ctx,0x637F68u,0x637F84u)) {originalDiscCalls[1](ram,ctx,runtime);return;}
    finishDiscCall(ctx,2);
}
void discType(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime) {
    if(!scopedDiscCall(runtime,ctx,0x637FA0u)) {originalDiscCalls[2](ram,ctx,runtime);return;}
    discCheckSession.validation=dw5xl::discs::validateImportedDisc(PS2Runtime::getIoPaths(),discCheckSession.which);
    const auto status=discCheckSession.validation;
    std::fprintf(stderr,"[dw5xl:disc] menu verification disc=%u result=%u\n",discCheckSession.which,static_cast<unsigned>(status));
    // Wrong bytes behave like a wrong inserted disc; absent/unreadable files
    // use the game's missing-disc prompt. The original routine owns Retry/Cancel.
    finishDiscCall(ctx,status==dw5xl::discs::Validation::valid || status==dw5xl::discs::Validation::unsupported?0x14:0);
}
void discSearch(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime) {
    if(!scopedDiscCall(runtime,ctx,0x637FD8u,0x637FF4u)) {originalDiscCalls[3](ram,ctx,runtime);return;}
    const uint32_t pointer=getRegU32(ctx,5),output=getRegU32(ctx,4);
    std::string path;
    if(pointer<PS2_RAM_SIZE) for(uint32_t i=0;i<64 && uint64_t(pointer)+i<PS2_RAM_SIZE;++i) {
        const char c=static_cast<char>(ram[pointer+i]);if(!c)break;path+=c;
    }
    if(!path.empty() && (path.front()=='\\' || path.front()=='/')) path.erase(0,1);
    if(path.ends_with(";1"))path.resize(path.size()-2);
    const char* expected=discCheckSession.which==0?"SLES_533.39":"SLES_538.60";
    const bool success=discCheckSession.validation==dw5xl::discs::Validation::valid && path==expected && output<=PS2_RAM_SIZE-32;
    // The original caller uses only the success result, not this file record.
    if(success)std::memset(ram+output,0,32);
    finishDiscCall(ctx,success?1:0);
}
void checkImportedDisc(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime) {
    const uint32_t entry=ctx->pc;
    if(entry==kDiscCheck && discCheckSession.runtime) {
        // Do not let a concurrent or nested check steal a yielded session.
        // Re-dispatch before the prologue may enter the same PC again safely.
        if(discCheckSession.runtime!=runtime ||
           discCheckSession.threadId!=runtime->eeScheduler().currentThreadId() ||
           discCheckSession.stack!=getRegU32(ctx,29) || discCheckSession.returnPc!=getRegU32(ctx,31)) {
            std::fprintf(stderr,"[dw5xl:disc] refused overlapping verification session\n");
            runtime->requestStop();return;
        }
    }
    if(entry==kDiscCheck && !discCheckSession.runtime) {
        discCheckSession.clear();
        discCheckSession.runtime=runtime;
        discCheckSession.threadId=runtime->eeScheduler().currentThreadId();
        discCheckSession.returnPc=getRegU32(ctx,31);
        discCheckSession.stack=getRegU32(ctx,29);
        discCheckSession.which=getRegU32(ctx,4);
    }
    const auto* end=g_ps2Module_dw5xl_ovl02.entries+g_ps2Module_dw5xl_ovl02.entryCount;
    const auto* original=std::lower_bound(g_ps2Module_dw5xl_ovl02.entries,end,entry,
        [](const auto& row,uint32_t address){return row.address<address;});
    if(original==end || original->address!=entry) {runtime->requestStop();return;}
    original->function(ram,ctx,runtime);
    // Do not clear on a dispatcher yield: continuation entries are wrapped too.
    if(ctx->pc==discCheckSession.returnPc && getRegU32(ctx,29)==discCheckSession.stack)
        discCheckSession.clear();
}

bool prepareDiscHooks(PS2Runtime& runtime) {
    if(!adaptedOvl02.initialize(g_ps2Module_dw5xl_ovl02,kDiscCheck,&checkImportedDisc))return false;
    // Every internal resume address of the original blocking routine must
    // return through the wrapper so session lifetime includes yield/resume.
    for(auto& entry:adaptedOvl02.entries)
        if(entry.address>=kDiscCheck && entry.address<kDiscCheckEnd)entry.function=&checkImportedDisc;
    for(size_t i=0;i<discCallAddresses.size();++i) {
        if(!runtime.hasFunction(discCallAddresses[i]))return false;
        constexpr std::array<PS2Runtime::RecompiledFunction,4> replacements={&discStatus,&discReady,&discType,&discSearch};
        const auto current=runtime.lookupFunction(discCallAddresses[i]);
        if(current!=replacements[i])originalDiscCalls[i]=current;
        if(!originalDiscCalls[i])return false;
    }
    return true;
}
void installDiscHooks(PS2Runtime& runtime) {
    constexpr std::array<PS2Runtime::RecompiledFunction,4> replacements={&discStatus,&discReady,&discType,&discSearch};
    for(size_t i=0;i<replacements.size();++i)runtime.replaceFunction(discCallAddresses[i],replacements[i]);
}

void queueLoad(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime) {
    // Original145A50(blocking,resource,destination) can yield. Only its entry
    // is wrapped; original continuation aliases do not rerun invalidation.
    if(dw5xl::overlays::invalidateForLoadDestination(*runtime,getRegU32(ctx,6),kOverlayBase)) {
        discCheckSession.clear();
        std::fprintf(stderr,"[dw5xl:overlay] invalidated before queued load; resource=0x%x generation=%llu\n",
                     getRegU32(ctx,5),static_cast<unsigned long long>(runtime->compiledModuleGeneration()));
    }
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

// SLES_538.60 has three contiguous 32-byte predicate functions at 0x1c24f0, 0x1c2510, 0x1c2530
// that Ghidra grouped into a single entry_001c24f0. The battle scene dispatches to 0x1c2510 via
// function pointer ($s2), so 0x1c2510 and 0x1c2530 need their own entries in the function table.
void entry_001c2510(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
    (void)runtime;
    const uint32_t a0 = GPR_U32(ctx, 4);
    uint32_t v1 = 0;
    std::memcpy(&v1, getMemPtr(rdram, a0 + 136), sizeof(v1));
    SET_GPR_S32(ctx, 3, static_cast<int32_t>(v1));
    const uint32_t srl = (v1 & 0x60000u) >> 17;
    const uint64_t sltiu = (srl < 2u) ? 1u : 0u;
    SET_GPR_U64(ctx, 2, sltiu ^ 1u);
    ctx->pc = GPR_U32(ctx, 31);
}

void entry_001c2530(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
    (void)runtime;
    const uint32_t a0 = GPR_U32(ctx, 4);
    uint32_t v1 = 0;
    std::memcpy(&v1, getMemPtr(rdram, a0 + 136), sizeof(v1));
    SET_GPR_S32(ctx, 3, static_cast<int32_t>(v1));
    const uint32_t srl = (v1 & 0x60000u) >> 17;
    const uint64_t xori = srl ^ 2u;
    SET_GPR_U64(ctx, 2, (xori < 1u) ? 1u : 0u);
    ctx->pc = GPR_U32(ctx, 31);
}

void installOverlays(PS2Runtime& runtime) {
    if(runtime.memory().read32(kFinalizer)!=0x27BDFFB0u || !runtime.hasFunction(kFinalizer) ||
       runtime.memory().read32(kQueueLoad)!=0x27BDFFA0u || !runtime.hasFunction(kQueueLoad)) {
        std::fprintf(stderr,"[dw5xl:overlay] declined: unexpected loader or finalizer\n");
        return;
    }
    if(!prepareDiscHooks(runtime)) {
        std::fprintf(stderr,"[dw5xl:disc] declined: disc-check entries unavailable\n");return;
    }
    supportedOverlays[2].module=&adaptedOvl02.descriptor;
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
    installDiscHooks(runtime);
    if (runtime.memory().read32(0x001C2510u) == 0x8C830088u) {
        runtime.replaceFunction(0x001C2510u, &entry_001c2510);
    }
    if (runtime.memory().read32(0x001C2530u) == 0x8C830088u) {
        runtime.replaceFunction(0x001C2530u, &entry_001c2530);
    }
    std::fprintf(stderr,"[dw5xl:overlay] installed verified OVL00/OVL01/OVL02 loader and finalizer hooks\n");
}
}
PS2_REGISTER_GAME_OVERRIDE("DW5XL native overlays","SLES_538.60",0u,0u,&installOverlays)
