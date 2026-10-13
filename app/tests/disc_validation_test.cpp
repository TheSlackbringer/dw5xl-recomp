#include "ps2_runtime_macros.h"
#include "../src/dw5xl_overlays.cpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
extern const uint32_t g_ps2RecompiledFunctionTableBase=0x1000;
extern const uint32_t g_ps2RecompiledFunctionTableEnd=0x1004;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount=1;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[]={nullptr};
namespace {
void original(uint8_t*,R5900Context*,PS2Runtime*) {}
void replacement(uint8_t*,R5900Context*,PS2Runtime*) {}
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
struct Scratch {
 std::filesystem::path path=std::filesystem::temp_directory_path()/
  ("dw5xl-disc-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 Scratch(){std::filesystem::create_directory(path);}
 ~Scratch(){std::error_code ec;std::filesystem::remove_all(path,ec);}
};
}
namespace {
int delegateCalls=0;
void delegate(uint8_t*,R5900Context* ctx,PS2Runtime*) {++delegateCalls;finishDiscCall(ctx,77);}
void guestDiscCheck(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime) {
 if(ctx->pc==kDiscCheck) {
  SET_GPR_U32(ctx,31,0x637F24);discStatus(ram,ctx,runtime);
  require(getRegU32(ctx,2)==1,"original first poll sees tray open");
  SET_GPR_U32(ctx,29,discCheckSession.stack-0x1b0);
  ctx->pc=0x637F08;return; // EE dispatcher yields to this internal alias.
 }
 SET_GPR_U32(ctx,31,0x637F40);discStatus(ram,ctx,runtime);
 require(getRegU32(ctx,2)==0,"original next poll sees tray closed");
 SET_GPR_U32(ctx,31,0x637F68);discReady(ram,ctx,runtime);
 require(getRegU32(ctx,2)==2,"original readiness state advances");
 SET_GPR_U32(ctx,29,discCheckSession.stack);
 SET_GPR_U32(ctx,31,discCheckSession.returnPc);
 finishDiscCall(ctx,0); // Authentic cancellation leaves original caller in charge.
}
const RecompiledModuleEntry checkRows[]={{kDiscCheck,guestDiscCheck},{0x637F08,guestDiscCheck}};
}
extern const RecompiledModule g_ps2Module_dw5xl_ovl00={"test00",0x61A800,0x28000,checkRows,2};
extern const RecompiledModule g_ps2Module_dw5xl_ovl01={"test01",0x61A800,0x28000,checkRows,2};
extern const RecompiledModule g_ps2Module_dw5xl_ovl02={"test02",0x61A800,0x28000,checkRows,2};
int main(){try{
 Scratch scratch;
 const std::array<uint8_t,32> abc={0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
 0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
 using dw5xl::discs::Validation;using dw5xl::discs::validateFile;
 require(validateFile(scratch.path/"SLES_533.39",abc)==Validation::missing,"optional missing original fails verification");
 {std::ofstream f(scratch.path/"SLES_533.39",std::ios::binary);f<<"abc";}
 require(validateFile(scratch.path/"SLES_533.39",abc)==Validation::valid,"valid content passes");
 {std::ofstream f(scratch.path/"SLES_533.39",std::ios::binary);f<<"abd";}
 require(validateFile(scratch.path/"SLES_533.39",abc)==Validation::unsupported,"same serial filename and size do not authenticate wrong bytes");
 require(validateFile(scratch.path,abc)==Validation::unreadable,"directory fails closed");
 PS2Runtime runtime;
 static const RecompiledModuleEntry rows[]={{0x610080,original},{0x610090,original}};
 static const RecompiledModule a={"fixture00",0x610000,0x100,rows,2};
 static const RecompiledModule b={"fixture02",0x610000,0x100,rows,2};
 dw5xl::discs::ModuleWithOverride adapted;
 require(adapted.initialize(b,0x610090,replacement),"module-local check entry replaced");
 require(rows[1].function==original,"generated entries immutable");
 require(runtime.registerCompiledModule(a)&&runtime.registerCompiledModule(adapted.descriptor),"register both modules");
 require(runtime.activeCompiledModule()==nullptr,"registration never activates or unlocks at boot");
 require(runtime.activateCompiledModule("fixture02")&&runtime.lookupFunction(0x610090)==replacement,"02 resolves check replacement");
 require(runtime.lookupFunction(0x610080)==original,"unrelated02 code stays original");
 require(runtime.activateCompiledModule("fixture00")&&runtime.lookupFunction(0x610090)==original,"00 same address stays original");
 runtime.deactivateCompiledModule();require(!runtime.hasFunction(0x610090),"inactive domains fail closed");
 dw5xl::discs::ModuleWithOverride absent;require(!absent.initialize(b,0x610088,replacement),"missing check entry rejected");
 // Exercise the actual wrapper, including dispatcher yield/resume and cancel.
 PS2Runtime checkRuntime;R5900Context checkContext{};uint8_t unusedRam[32]{};
 require(adaptedOvl02.initialize(g_ps2Module_dw5xl_ovl02,kDiscCheck,&checkImportedDisc),"adapt actual check entry");
 for(auto& row:adaptedOvl02.entries)row.function=&checkImportedDisc;
 require(checkRuntime.registerCompiledModule(adaptedOvl02.descriptor)&&checkRuntime.registerCompiledModule(g_ps2Module_dw5xl_ovl00),"register check fixture");
 for(auto& fn:originalDiscCalls)fn=&delegate;
 require(checkRuntime.activateCompiledModule("test02"),"activate check fixture");
 checkContext.pc=kDiscCheck;SET_GPR_U32(&checkContext,4,0);SET_GPR_U32(&checkContext,29,0x10000);SET_GPR_U32(&checkContext,31,0x1234);
 checkImportedDisc(unusedRam,&checkContext,&checkRuntime);
 require(discCheckSession.runtime==&checkRuntime&&checkContext.pc==0x637F08,"verification scope survives a yielded original routine");
 R5900Context other{};SET_GPR_U32(&other,31,0x500000);discReady(unusedRam,&other,&checkRuntime);
 require(getRegU32(&other,2)==77&&delegateCalls==1,"unrelated callers delegate during check");
 checkImportedDisc(unusedRam,&checkContext,&checkRuntime);
 require(!discCheckSession.runtime&&checkContext.pc==0x1234&&getRegU32(&checkContext,2)==0,"original cancellation clears scope and returns zero");
 require(checkRuntime.activateCompiledModule("test00"),"activate noncheck overlay");
 SET_GPR_U32(&other,31,0x637F68);discReady(unusedRam,&other,&checkRuntime);
 require(getRegU32(&other,2)==77&&delegateCalls==2,"same PC in other overlay delegates");
 std::cout<<"disc validation and module isolation passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
