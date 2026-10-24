#include "boundary_fixture_data.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/state_archive.h"
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
void runTlbNativeTests(PS2Runtime&, int&, int&);
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
        case LikelyBreakSlot:SET_GPR_U64((&ctx),1,variant?2:0);expectedR1=variant?2:0;expectedA0=100;
            ctx.cop0_status |= 1u << 22;
            if(!variant) {expectedPc=0xBFC00200u;exception=true;}
            break;
        case DirectSlotBreak:ctx.pc=pc+4;ctx.cop0_status |= 1u << 22;
            expectedPc=0xBFC00200u;expectedA0=100;exception=true;break;
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
            equal(ctx.cop0_epc,c==DirectSlotBreak?pc+4:pc,"exception_epc",c,variant);
            equal((ctx.cop0_cause>>31)&1,c==DirectSlotBreak?0:1,"exception_bd",c,variant);
        }
        if(c==StoreSlot)equal(runtime.memory().read32(0x3000),100,"slot_memory_store",c,variant);
    }
    const auto runPath=[&](uint32_t entry,int64_t r1,uint64_t a0,uint64_t finalR1,unsigned calls,int fixture) {
        R5900Context ctx;ctx.pc=entry;
        SET_GPR_U64((&ctx),1,r1);SET_GPR_U64((&ctx),4,100);SET_GPR_U64((&ctx),31,0x2220);
        for(unsigned step=0;step<16 && ctx.pc!=0x2220;++step) {
            const auto function=runtime.lookupFunction(ctx.pc);
            if(!function) {++failures;std::printf("FAIL path=%d unregistered PC=%x\n",fixture,ctx.pc);break;}
            function(ram,&ctx,&runtime);
        }
        equal(ctx.pc,0x2220,"path_return_pc",Count,fixture);
        equal(GPR_U64((&ctx),4),a0,"path_a0",Count,fixture);
        equal(GPR_U64((&ctx),1),finalR1,"path_r1",Count,fixture);
        equal(GPR_U64((&ctx),6),calls,"path_native_callee_runs",Count,fixture);
        equal(GPR_U64((&ctx),9),0,"unrelated_body_untouched",Count,fixture);
        equal(GPR_U64((&ctx),31),0x2220,"path_restored_ra",Count,fixture);
        equal(ctx.in_delay_slot,0,"path_delay_flag",Count,fixture);
    };
    runPath(overlap+8,3,112,4,0,0);
    runPath(overlap+12,3,112,3,0,1);
    runPath(clipped,3,clipped+20,3,1,2);
    runPath(shared,0,120,0,0,3);
    runPath(shared,1,112,1,0,4);
    runPath(shared+64,3,118,3,0,5);
    // EE Core User's Manual v6, p71: VPN2[31:13], reserved[12:8], ASID[7:0].
    // Fixed independent known answers, not expectations built from the emitter's mask.
    for(const auto pair:std::array<std::array<uint32_t,2>,5>{{
        {{0,0}},{{0x12345678,0x12344078}},{{0xffffffff,0xffffe0ff}},
        {{0x00600042,0x00600042}},{{0x80000000,0x80000000}}}}) {
        R5900Context c;c.pc=shared+0x1c0;
        SET_GPR_U32((&c),4,pair[0]);SET_GPR_U32((&c),31,0x2220);
        runtime.lookupFunction(c.pc)(ram,&c,&runtime);
        equal(GPR_U32((&c),2),pair[1],"entryhi_field_roundtrip",Count,7);
        equal(c.cop0_entryhi,pair[1],"entryhi_register_fields",Count,7);
        equal(c.pc,0x2220,"entryhi_return_pc",Count,7);
    }
    // Both native images must coexist without switching the active game overlay.
    // Missing/partial/modified code must never dispatch a stale native wrapper.
    equal(runtime.hasFunction(copyBase),false,"copy_absent_before_load",Count,6);
    for(unsigned i=0;i<copiedWords.size();++i)runtime.memory().write32(copyBase+i*4,copiedWords[i]);
    runtime.memory().write32(secondCopyBase,0x2402002a);
    equal(runtime.hasFunction(secondCopyBase),false,"partial_copy_not_callable",Count,6);
    runtime.memory().write32(secondCopyBase+4,0x03e00008);
    runtime.memory().write32(secondCopyBase+8,0);
    equal(runtime.hasFunction(copyBase),true,"first_copy_callable",Count,6);
    equal(runtime.hasFunction(secondCopyBase),true,"second_copy_callable",Count,6);
    if(runtime.hasFunction(copyBase)) {
        R5900Context c;c.pc=copyBase;
        SET_GPR_U32((&c),4,100);SET_GPR_U32((&c),31,0x2220);
        for(unsigned step=0;step<16&&c.pc!=0x2220;++step) {
            const auto fn=runtime.lookupFunction(c.pc);if(!fn)break;fn(ram,&c,&runtime);
        }
        equal(c.pc,0x2220,"copy_return_pc",Count,6);
        equal(GPR_U32((&c),2),114,"copy_native_result",Count,6);
        equal(GPR_U32((&c),4),104,"copy_delay_effects",Count,6);
        equal(GPR_U32((&c),3),copyBase+12,"copy_actual_link_address",Count,6);
        equal(GPR_U32((&c),31),0x2220,"copy_restored_ra",Count,6);
    }
    runtime.memory().write32(copyBase+48,0xaabbccdd);
    equal(runtime.hasFunction(copyBase),true,"mutable_copy_data_allowed",Count,6);
    runtime.memory().write32(copyBase,0);
    equal(runtime.hasFunction(copyBase),false,"modified_copy_rejected",Count,6);
    equal(runtime.hasFunction(copyBase+32),false,"modified_copy_interior_rejected",Count,6);
    equal(runtime.hasFunction(secondCopyBase),true,"other_copy_retained",Count,6);
    runtime.memory().write32(copyBase,copiedWords[0]);
    equal(runtime.hasFunction(copyBase),true,"restored_copy_callable",Count,6);
    equal(runtime.hasFunction(copyBase+4),false,"unregistered_copy_interior_rejected",Count,6);
    equal(runtime.hasFunction(copyBase+64),false,"copy_end_rejected",Count,6);
    // Registry contracts are separate from execution of the generated fixture.
    static const RecompiledModuleEntry metadataEntries[]={{0x80072000u,+[](uint8_t*,R5900Context*,PS2Runtime*){}}};
    static const RecompiledCodeWord metadataWords[]={{0x80072000u,0x2402002a},{0x80072004u,0x03e00008},{0x80072008u,0}};
    static const RecompiledCodeImage metadataImage={{"metadata_fixture",0x80072000u,12,metadataEntries,1},metadataWords,3};
    auto malformed=metadataImage;
    malformed.words=nullptr;
    equal(runtime.registerCompiledCodeImage(malformed),false,"copy_null_words_rejected",Count,7);
    malformed=metadataImage;malformed.wordCount=0;
    equal(runtime.registerCompiledCodeImage(malformed),false,"copy_no_words_rejected",Count,7);
    malformed=metadataImage;malformed.module.entries=nullptr;
    equal(runtime.registerCompiledCodeImage(malformed),false,"copy_null_entries_rejected",Count,7);
    malformed=metadataImage;malformed.module.guestBase++;
    equal(runtime.registerCompiledCodeImage(malformed),false,"copy_unaligned_rejected",Count,7);
    malformed=metadataImage;malformed.module.fileSize=4;
    equal(runtime.registerCompiledCodeImage(malformed),false,"copy_metadata_bounds_rejected",Count,7);
    malformed=metadataImage;malformed.module.identity=nullptr;
    equal(runtime.registerCompiledCodeImage(malformed),false,"copy_null_identity_rejected",Count,7);
    equal(runtime.registerCompiledCodeImage(metadataImage),true,"copy_metadata_registered",Count,7);
    equal(runtime.registerCompiledCodeImage(metadataImage),true,"copy_same_descriptor_idempotent",Count,7);
    malformed=metadataImage;
    equal(runtime.registerCompiledCodeImage(malformed),false,"copy_duplicate_identity_rejected",Count,7);
    equal(runtime.hasFunction(0x80072000u),false,"registered_metadata_does_not_load_code",Count,7);
    runtime.deactivateCompiledModule();
    equal(runtime.hasFunction(copyBase),true,"copies_independent_of_overlay_activation",Count,7);
    // Native SDK handlers use the same COP0 TLBWI/TLBWR/TLBR hardware interface.
    // Preserve both page halves, ASID and flags even after intervening register writes.
    {
        R5900Context c;c.cop0_index=5;c.cop0_pagemask=0x6000;
        c.cop0_entryhi=0x00600042;c.cop0_entrylo0=0x0012345f;c.cop0_entrylo1=0x0023459f;
        runtime.handleTLBWI(ram,&c);
        const auto readEntry=[&](unsigned index,int variant) {
            c.cop0_index=index;c.cop0_pagemask=0;c.cop0_entryhi=0;
            c.cop0_entrylo0=0;c.cop0_entrylo1=0;
            runtime.handleTLBR(ram,&c);
            equal(c.cop0_pagemask,0x6000,"tlb_mask_retained",Count,variant);
            equal(c.cop0_entryhi,0x00600042,"tlb_vpn_asid_retained",Count,variant);
            equal(c.cop0_entrylo0,0x0012345f,"tlb_even_flags_retained",Count,variant);
            equal(c.cop0_entrylo1,0x0023459f,"tlb_odd_page_retained",Count,variant);
        };
        readEntry(5,8);
        c.cop0_random=7;c.cop0_wired=3;
        runtime.handleTLBWR(ram,&c);
        readEntry(7,9);
        auto saved=ps2x::StateArchive::writer();runtime.memory().serializeState(saved);
        runtime.memory().tlbWrite(5,0,0,0,false);
        auto loaded=ps2x::StateArchive::reader(saved.data());runtime.memory().serializeState(loaded);
        equal(loaded.atEnd(),true,"tlb_state_fully_consumed",Count,10);
        readEntry(5,10);
        runtime.memory().tlbWrite(5,0,0,0,false);
        c.cop0_index=5;runtime.handleTLBR(ram,&c);
        equal(c.cop0_entryhi,0,"legacy_tlb_reset_hi",Count,11);
        equal(c.cop0_entrylo0,0,"legacy_tlb_reset_lo0",Count,11);
        equal(c.cop0_entrylo1,0,"legacy_tlb_reset_lo1",Count,11);
    }
    // SDK callbacks end with syscall -8: the installed kernel return entry
    // continues this context rather than starting an ordinary nested call.
    {
        R5900Context c;c.pc=0x1234;
        SET_GPR_U32((&c),2,0x55);SET_GPR_U32((&c),29,0x19000);SET_GPR_U32((&c),31,0x2220);
        runtime.setEeSyscallOverride(ram,8,shared+0x1c0);
        runtime.handleSyscall(ram,&c,static_cast<uint32_t>(-8));
        equal(c.pc,shared+0x1c0,"kernel_return_native_transfer",Count,12);
        equal(GPR_U32((&c),2),0x55,"kernel_return_preserves_result",Count,12);
        equal(GPR_U32((&c),29),0x19000,"kernel_return_preserves_stack",Count,12);
        equal(GPR_U32((&c),31),0x2220,"kernel_return_preserves_link",Count,12);
    }
    {
        R5900Context c;c.pc=shared+0x1a0;
        SET_GPR_U32((&c),4,0x00600042);SET_GPR_U32((&c),2,0x55);
        SET_GPR_U32((&c),29,0x19000);SET_GPR_U32((&c),31,0x2220);
        runtime.lookupFunction(c.pc)(ram,&c,&runtime);
        equal(c.pc,shared+0x1c0,"generated_syscall_preserves_transfer",Count,13);
        equal(GPR_U32((&c),4),0x00600042,"nonlocal_syscall_skips_fallthrough",Count,13);
        equal(GPR_U32((&c),2),0x55,"nonlocal_syscall_preserves_result",Count,13);
        if(c.pc==shared+0x1c0)runtime.lookupFunction(c.pc)(ram,&c,&runtime);
        equal(c.pc,0x2220,"nonlocal_native_target_return",Count,13);
        equal(c.cop0_entryhi,0x00600042,"nonlocal_native_target_effect",Count,13);
    }
    runTlbNativeTests(runtime,checks,failures);
    std::printf("Native boundary effects: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
