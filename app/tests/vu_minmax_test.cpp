// Instruction regression: invoke the actual runtime upper instruction executor.
// No GS object is constructed and no renderer is used by this test.
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include "runtime/ps2_vu1.h"
#include "ps2_runtime.h"
#include <iostream>
#include <stdexcept>

extern const uint32_t g_ps2RecompiledFunctionTableBase=0x1000;
extern const uint32_t g_ps2RecompiledFunctionTableEnd=0x1004;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount=1;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[]={nullptr};
namespace {
// Reach the private upper executor through explicit instantiation instead of
// "#define private public": the MSVC STL rejects macroized keywords and the
// MSVC ABI encodes member access in mangled names.
using ExecUpperPtr=void (VU1Interpreter::*)(uint32_t);
ExecUpperPtr execUpperPtr();
template<ExecUpperPtr member> struct ExecUpperAccess {friend ExecUpperPtr execUpperPtr(){return member;}};
template struct ExecUpperAccess<&VU1Interpreter::execUpper>;
void execUpper(VU1Interpreter& vu,uint32_t instr){(vu.*execUpperPtr())(instr);}
float value(uint32_t b){float f;std::memcpy(&f,&b,4);return f;}
uint32_t bits(float f){uint32_t b;std::memcpy(&b,&f,4);return b;}
void require(bool condition,const char* reason){if(!condition)throw std::runtime_error(reason);}
struct Case {uint32_t left,right,minimum,maximum;};
constexpr Case cases[]={
 {0x00000080,0x437f0000,0x00000080,0x437f0000}, // transport integer alpha unchanged
 {0x00000001,0x00000080,0x00000001,0x00000080}, // positive subnormal ordering
 {0x80000001,0x80000080,0x80000080,0x80000001}, // negative subnormal ordering
 {0x80000000,0x00000000,0x80000000,0x00000000}, // signed zero ordering
 {0xc0000000,0xbf800000,0xc0000000,0xbf800000}, // -2 and -1
 {0x3f800000,0x40000000,0x3f800000,0x40000000}, // 1 and2
 {0x7f800000,0x7f7fffff,0x7f7fffff,0x7f800000}, // VU special representations survive
 {0x7fffffff,0x7f800000,0x7f800000,0x7fffffff},
 {0xffffffff,0xff800000,0xffffffff,0xff800000}
};
void check(VU1Interpreter& vu,uint32_t opcode,bool maximum,unsigned mask,unsigned broadcast) {
 for(const auto& c:cases) {
  vu.reset();auto& s=vu.state();s.i=value(c.right);
  s.mac=0x1234;s.status=0x5678;s.clip=0xabcdef;
  for(unsigned i=0;i<4;++i){s.vf[18][i]=value(c.left);s.vf[22][i]=value(0x42420000);s.vf[19][i]=value(0xdeadbeef);}
  if(opcode==0x2b||opcode==0x2f)for(float& f:s.vf[22])f=value(c.right);
  else s.vf[22][broadcast]=value(c.right);
  execUpper(vu,(mask<<21)|(22u<<16)|(18u<<11)|(19u<<6)|opcode);
  for(unsigned i=0;i<4;++i) {
   const auto expected=(mask&(8u>>i))?(maximum?c.maximum:c.minimum):0xdeadbeef;
   if(bits(s.vf[19][i])!=expected) {
    std::cerr<<"opcode="<<std::hex<<opcode<<" mask="<<mask<<" lane="<<i<<" left="<<c.left<<" right="<<c.right<<" got="<<bits(s.vf[19][i])<<" expected="<<expected<<'\n';
    throw std::runtime_error("MIN/MAX raw representation or destination mask mismatch");
   }
  }
  require(s.mac==0x1234&&s.status==0x5678&&s.clip==0xabcdef,"MIN/MAX must not modify arithmetic flags");
 }
}
}
int main(){try{
 VU1Interpreter vu;
 // Synthetic comparison/conversion sequence: RGB become integers while raw
 // alpha128 transported in W survives. No game instruction bytes are used.
 auto& s=vu.state();s.vf[3][0]=63.5f;s.vf[3][1]=83.5f;s.vf[3][2]=94.5f;s.vf[3][3]=value(0x80);s.i=255.0f;
 execUpper(vu,(15u<<21)|(3u<<11)|(4u<<6)|0x1fu); // MINIi.xyzw VF4,VF3,I
 require(bits(s.vf[4][3])==0x80,"terrain MINIi must preserve raw alpha128");
 execUpper(vu,(14u<<21)|(4u<<16)|(4u<<11)|(5u<<6)|0x3cu); // FTOI0.xyz VF4,VF4
 require(bits(s.vf[4][0])==63&&bits(s.vf[4][1])==83&&bits(s.vf[4][2])==94&&bits(s.vf[4][3])==128,"terrain RGBA conversion sequence");
 for(unsigned mask:{0u,1u,14u,15u}){
  check(vu,0x2f,false,mask,0);check(vu,0x2b,true,mask,0);
  check(vu,0x1f,false,mask,0);check(vu,0x1d,true,mask,0);
  for(unsigned bc=0;bc<4;++bc){check(vu,0x14+bc,false,mask,bc);check(vu,0x10+bc,true,mask,bc);}
 }
 std::cout<<"VU MIN/MAX raw representation and terrain alpha tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
