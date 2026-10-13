#include "ps2_runtime.h"
#include "ps2_stubs.h"
#include <ps2_recompiled_functions.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x110c78;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x1112a0;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = (0x1112a0-0x110c78)/4;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[(0x1112a0-0x110c78)/4] = {};
static void set(R5900Context& c, unsigned i, uint32_t v) { c.r[i]=_mm_cvtsi32_si128(v); }
static uint32_t get(const R5900Context& c, unsigned i) { return _mm_cvtsi128_si32(c.r[i]); }
static void require(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
static uint32_t run(PS2Runtime& rt,uint16_t interlace,uint16_t frame,uint32_t psm,uint32_t w,uint32_t h,uint32_t extra) {
 auto* ram=rt.memory().getRDRAM(); uint16_t params[8]={interlace,2,frame,3,0,0,0,0};
 std::memcpy(ram+0x30f480,params,sizeof(params)); R5900Context c{};
 set(c,4,psm);set(c,5,w);set(c,6,h);set(c,7,extra);set(c,29,0x800000);set(c,31,0xfffffffcu);c.pc=0x1111d8;
 set(c,16,0x12345678);set(c,17,0x34567890);set(c,18,0x56789012);
 for(unsigned step=0;c.pc!=0xfffffffcu && step<16;++step) {
  auto f=rt.lookupFunction(c.pc); require(f!=nullptr,"original helper continuation resolves");f(ram,&c,&rt);
 }
 require(c.pc==0xfffffffcu && get(c,29)==0x800000 && get(c,31)==0xfffffffcu,"helper preserves stack and return");
 require(get(c,16)==0x12345678 && get(c,17)==0x34567890 && get(c,18)==0x56789012,"callee-saved registers preserved");
 require(!std::memcmp(ram+0x30f480,params,sizeof(params)),"helper leaves graphics parameters unchanged");return get(c,2);
}
int main() {
 try {
  PS2Runtime rt;require(rt.memory().initialize(),"memory initialization");
  g_ps2RecompiledFunctionTable[0]=FUN_00110c78_0x110c78;
  g_ps2RecompiledFunctionTable[(0x1111d8-0x110c78)/4]=FUN_001111d8_0x1111d8;
  g_ps2RecompiledFunctionTable[(0x111208-0x110c78)/4]=FUN_001111d8_0x1111d8;
  require(run(rt,1,0,0,640,256,17)==80,"FIELD CT32 allocation");
  require(run(rt,1,1,0,640,256,17)==160,"FRAME CT32 allocation");
  require(run(rt,0,0,0,640,256,999)==160,"progressive CT32 allocation");
  require(run(rt,1,1,2,640,256,17)==80,"FRAME CT16 allocation");
  require(run(rt,1,0,0,65,33,999)==4,"CT32 rounds partial pages");
  require(run(rt,1,1,2,65,33,999)==4,"CT16 rounds partial pages");
  std::cout<<"Original compiled GS helper tests passed\n";return 0;
 } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
