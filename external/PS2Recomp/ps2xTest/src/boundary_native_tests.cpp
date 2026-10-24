#include "boundary_fixture_data.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <stdexcept>
extern const RecompiledModule g_ps2Module_boundary_fixture;
void reset_ps2_test_function_table();
using namespace boundary_fixture;
namespace {
int failures=0,checks=0;
void equal(uint64_t actual,uint64_t expected,const char*field,Case c,int variant) {
    ++checks;
    if(actual!=expected) {++failures;std::printf("FAIL case=%u variant=%d %s actual=%llx expected=%llx\n",
        unsigned(c),variant,field,(unsigned long long)actual,(unsigned long long)expected);}
}
}
int main() {
    reset_ps2_test_function_table();
    PS2Runtime runtime;
    if(!runtime.memory().initialize())return 2;
    for(size_t i=0;i<g_ps2Module_boundary_fixture.entryCount;++i) {
        const auto &entry=g_ps2Module_boundary_fixture.entries[i];
        if(!runtime.registerFunction(entry.address,entry.function))return 2;
    }
    auto*ram=runtime.memory().getRDRAM();
    for(unsigned id=0;id<Count;++id) for(int variant=0;variant<2;++variant) {
        const Case c=static_cast<Case>(id);
        const uint32_t pc=address(c),target=pc+32;
        R5900Context ctx;
        ctx.pc=pc;
        SET_GPR_U64((&ctx),1,target);SET_GPR_U64((&ctx),4,100);
        SET_GPR_U64((&ctx),7,pc+8);SET_GPR_U64((&ctx),8,0x3000);
        SET_GPR_U64((&ctx),31,0x2220);SET_GPR_U64((&ctx),5,55);
        runtime.memory().writeIORegister(0x1000E020u,variant?0u:1u);
        runtime.memory().writeIORegister(0x1000E010u,0x3ffu); // clear completion bits, not a replacement hardware model
        ctx.fcr31=variant?0x800000:0;
        uint32_t expectedPc=pc+8;uint64_t expectedA0=104,expectedRa=0x2220,expectedR1=target,expectedR5=55;
        unsigned expectedCalls=0;bool exception=false;
        switch(c) {
        case Jump:expectedPc=target;break;
        case Call:expectedRa=pc+8;expectedA0=pc+9;expectedCalls=1;break;
        case RegisterCall:expectedRa=pc+8;expectedR1=target+4;expectedA0=100;expectedCalls=1;break;
        case RegisterCall5:expectedR5=pc+8;expectedA0=pc+9;expectedCalls=1;break;
        case NonLinkingJalr:case RegisterJump:expectedPc=target;expectedR1=target+4;expectedA0=100;break;
        case Return:expectedPc=0x2220;expectedRa=0x2224;expectedA0=100;break;
        case Bltzal:case Bgezal:case Bltzall:case Bgezall: {
            const int64_t value=variant?1:-1;SET_GPR_U64((&ctx),1,value);expectedR1=value;
            const bool taken=(c==Bltzal||c==Bltzall)?!variant:bool(variant);
            expectedPc=taken?target:pc+8;expectedRa=pc+8;
            expectedA0=(c==Bltzall||c==Bgezall)&&!taken?100:pc+9;
            break;
        }
        case BlezWide:case BgtzWide:case BltzWide:case BgezWide: {
            const int64_t value=variant?(int64_t{1}<<32):-(int64_t{1}<<32);
            SET_GPR_U64((&ctx),1,value);expectedR1=value;
            const bool taken=(c==BlezWide||c==BltzWide)?!variant:bool(variant);
            expectedPc=taken?target:pc+8;break;
        }
        case Bc0f:case Bc0t:case Bc0fl:case Bc0tl:
        case Bc1f:case Bc1t:case Bc1fl:case Bc1tl:
        case Bc2f:case Bc2t:case Bc2fl:case Bc2tl: {
            const bool cond=c>=Bc2f?false:bool(variant); // existing synchronous VU0 idle contract
            const unsigned family=c>=Bc2f?c-Bc2f:c>=Bc1f?c-Bc1f:c-Bc0f;
            const bool taken=(family==0||family==2)?!cond:cond;
            expectedPc=taken?target:pc+8;expectedA0=family>=2&&!taken?100:104;break;
        }
        case BreakSlot:SET_GPR_U64((&ctx),1,0);expectedR1=0;expectedA0=100;
            ctx.cop0_status |= 1u << 22; // explicit BEV=1 fixture, independent of constructor defaults
            expectedPc=0xBFC00200u;exception=true;break;
        case StoreSlot:SET_GPR_U64((&ctx),1,0);expectedR1=0;expectedA0=100;expectedPc=target;
            runtime.memory().write32(0x3000,0);break;
        default:throw std::runtime_error("fixture case missing independent expectations");
        }
        runtime.lookupFunction(pc)(ram,&ctx,&runtime);
        equal(ctx.pc,expectedPc,"pc",c,variant);
        equal(GPR_U64((&ctx),1),expectedR1,"r1",c,variant);
        equal(GPR_U64((&ctx),4),expectedA0,"a0",c,variant);
        equal(GPR_U64((&ctx),5),expectedR5,"r5",c,variant);
        equal(GPR_U64((&ctx),31),expectedRa,"ra",c,variant);
        equal(GPR_U64((&ctx),6),expectedCalls,"native_callee_runs",c,variant);
        equal(ctx.in_delay_slot,0,"delay_flag",c,variant);
        if(exception) {
            equal(ctx.cop0_epc,pc,"exception_epc",c,variant);
            equal((ctx.cop0_cause>>31)&1,1,"exception_bd",c,variant);
        }
        if(c==StoreSlot)equal(runtime.memory().read32(0x3000),100,"slot_memory_store",c,variant);
    }
    std::printf("Native boundary effects: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
