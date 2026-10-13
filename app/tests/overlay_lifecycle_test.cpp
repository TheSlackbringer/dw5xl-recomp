#include "../src/dw5xl_overlay_validation.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>

extern const uint32_t g_ps2RecompiledFunctionTableBase=0x1000;
extern const uint32_t g_ps2RecompiledFunctionTableEnd=0x1004;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount=1;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[]={nullptr};

namespace {
constexpr uint32_t base=0x610000u;
constexpr uint32_t imageSize=0x100u;
int called=0;
void a(uint8_t*,R5900Context*,PS2Runtime*) { called=1; }
void b(uint8_t*,R5900Context*,PS2Runtime*) { called=2; }
void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
void word(std::vector<uint8_t>& bytes,uint32_t address,uint32_t value) { std::memcpy(bytes.data()+address,&value,4); }
std::array<uint8_t,32> fingerprint(const std::vector<uint8_t>& bytes) {
    auto sha=std::unique_ptr<AVSHA,decltype(&av_free)>(av_sha_alloc(),&av_free);
    require(sha && av_sha_init(sha.get(),256)==0,"fixture SHA initialization");
    std::array<uint8_t,32> digest{};
    av_sha_update(sha.get(),bytes.data()+base,imageSize);
    av_sha_final(sha.get(),digest.data());
    return digest;
}
void header(std::vector<uint8_t>& bytes,uint32_t id,bool ctor) {
    std::fill(bytes.begin()+base,bytes.begin()+base+imageSize,0);
    word(bytes,base,0x336F574D); word(bytes,base+4,id); word(bytes,base+8,base);
    word(bytes,base+0x14,0x40);
    word(bytes,base+0x18,base+(ctor?0xa0:imageSize));
    word(bytes,base+0x1c,base+(ctor?0xa4:imageSize));
    if(ctor) word(bytes,base+0xa0,base+0x80);
    bytes[base+0x80]=static_cast<uint8_t>(id);
}
}

int main() {
    try {
        PS2Runtime runtime; R5900Context context{};
        static const RecompiledModuleEntry ae[]={{base+0x80,a},{base+0xf0,a}};
        static const RecompiledModuleEntry be[]={{base+0x80,b}};
        static const RecompiledModule A={"fixture00",base,imageSize,ae,2};
        static const RecompiledModule B={"fixture01",base,imageSize,be,1};
        require(runtime.registerCompiledModule(A) && runtime.registerCompiledModule(B),"register fixture modules");
        std::vector<uint8_t> ram(0x620000,0);
        header(ram,1,true); const auto aBytes=ram; const auto aHash=fingerprint(ram);
        header(ram,2,false); const auto bBytes=ram; const auto bHash=fingerprint(ram);
        const std::array<dw5xl::overlays::SupportedImage,2> identities={{{&A,1,aHash},{&B,2,bHash}}};
        dw5xl::overlays::PreparedImage result; const char* error=nullptr;
        const auto prepare=[&](std::span<const dw5xl::overlays::SupportedImage> supported) {
            return dw5xl::overlays::prepareLoadedImage(runtime,ram.data(),static_cast<uint32_t>(ram.size()),
                                                       base,imageSize,supported,result,error);
        };
        ram=aBytes; require(prepare(identities),"00 content selects compiled00");
        require(result.identity->module==&A && result.ctorEnd-result.ctorBegin==4,"constructor range retained");
        runtime.lookupFunction(base+0x80)(ram.data(),&context,&runtime); require(called==1,"00 target");
        require(!dw5xl::overlays::invalidateForLoadDestination(runtime,0x5000,base) && runtime.activeCompiledModule()==&A,
                "ordinary file request preserves active module");
        require(dw5xl::overlays::invalidateForLoadDestination(runtime,base,base) && !runtime.hasFunction(base+0x80),
                "queued overlay write invalidates before bytes change");
        ram=bBytes; require(prepare(identities),"01 content selects compiled01");
        require(result.identity->module==&B && result.ctorBegin==result.ctorEnd,"01 empty ctor range accepted");
        runtime.lookupFunction(base+0x80)(ram.data(),&context,&runtime);
        require(called==2 && !runtime.hasFunction(base+0xf0),"same PC switches identity; old00 tail suppressed");
        ram=aBytes; require(prepare(identities) && runtime.activeCompiledModule()==&A,"01 back to00");
        ram[base+0x90]^=1; require(!prepare(identities) && !runtime.activeCompiledModule() && !result.identity,"unknown bytes fail closed");
        ram=bBytes; word(ram,base+0x14,0xffffffffu);
        const std::array<dw5xl::overlays::SupportedImage,1> badBss={{{&B,2,fingerprint(ram)}}};
        require(!prepare(badBss) && !runtime.activeCompiledModule(),"BSS beyond RAM rejected despite matching identity");
        ram=aBytes; word(ram,base+0xa0,base+0x84);
        const std::array<dw5xl::overlays::SupportedImage,1> missingCtor={{{&A,1,fingerprint(ram)}}};
        require(!prepare(missingCtor) && !runtime.activeCompiledModule(),"uncompiled constructor fails closed");
        ram=bBytes; word(ram,base+0x18,base+imageSize+4);
        const std::array<dw5xl::overlays::SupportedImage,1> badCtorBounds={{{&B,2,fingerprint(ram)}}};
        require(!prepare(badCtorBounds) && !runtime.activeCompiledModule(),"invalid constructor bounds rejected");
        ram=bBytes; word(ram,base+4,9);
        const std::array<dw5xl::overlays::SupportedImage,1> wrongHeader={{{&B,2,fingerprint(ram)}}};
        require(!prepare(wrongHeader) && !runtime.activeCompiledModule(),"matching hash cannot bypass header identity check");
        ram=bBytes;
        static const RecompiledModule unavailable={"unregistered",base,imageSize,be,1};
        const std::array<dw5xl::overlays::SupportedImage,1> absentTable={{{&unavailable,2,bHash}}};
        require(!prepare(absentTable) && !runtime.activeCompiledModule(),"unregistered compiled table fails closed");
        ram=bBytes;
        require(!dw5xl::overlays::prepareLoadedImage(runtime,ram.data(),static_cast<uint32_t>(ram.size()),
                                                      base,0xfffffff0u,identities,result,error) && !runtime.activeCompiledModule(),
                "oversized range rejected before hash or reads");
        std::cout<<"Overlay load identity/lifecycle tests passed\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
