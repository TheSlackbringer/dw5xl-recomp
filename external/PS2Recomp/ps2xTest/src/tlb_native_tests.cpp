// Independent architectural known answers: Sony EE Core User's Manual v6,
// pp65-67,71,90-91,100-102,120-125 (https://docs.alexrp.com/mips/ee.pdf).
// No game input, decoder or production translation helper is used as the oracle.
#include "boundary_fixture_data.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "runtime/state_archive.h"
#include <array>
#include <cstdio>

void runTlbNativeTests(PS2Runtime& runtime, int& checks, int& failures)
{
    auto& mem=runtime.memory();auto* ram=mem.getRDRAM();
    const auto eq=[&](uint64_t actual,uint64_t expected,const char* name) {
        ++checks;
        if(actual!=expected) {++failures;std::printf("FAIL TLB %s actual=%llx expected=%llx\n",name,
            (unsigned long long)actual,(unsigned long long)expected);}
    };
    const auto clear=[&] {
        // Invalid entries still participate in probing. Keep their tags distinct.
        for(unsigned i=0;i<48;++i)mem.tlbWriteRegisters(i,0,0x80000000+i*0x2000,0,0);
    };
    const auto install=[&](uint32_t mask,uint32_t hi,uint32_t lo0,uint32_t lo1) {
        clear();R5900Context c;c.cop0_index=11;c.cop0_pagemask=mask;c.cop0_entryhi=hi;
        c.cop0_entrylo0=lo0;c.cop0_entrylo1=lo1;runtime.handleTLBWI(ram,&c);
    };
    const auto read=[&](R5900Context& c,uint32_t address) {
        try {return runtime.Load32(ram,&c,address);}catch(const EeDispatcherTransfer&) {return 0u;}
    };
    const auto write=[&](R5900Context& c,uint32_t address,uint32_t value) {
        try {runtime.Store32(ram,&c,address,value);}catch(const EeDispatcherTransfer&) {}
    };
    struct Page {uint32_t mask,size;};
    constexpr std::array<Page,7> pages{{{0,4096},{0x6000,16384},{0x1e000,65536},
        {0x7e000,262144},{0x1fe000,1048576},{0x7fe000,4194304},{0x1ffe000,16777216}}};
    for(const auto p:pages) {
        install(p.mask,0xc0000042,0x7,0x40007); // 0 / 16MiB physical pages, global, valid, dirty
        for(unsigned half=0;half<2;++half) for(uint32_t offset: {0u,4u,p.size/2,p.size-4}) {
            const uint32_t physical=half*0x1000000+offset;
            const uint32_t address=0xc0000000+half*p.size+offset;
            mem.write32(physical,0x12345678);
            R5900Context c;c.cop0_status=0;c.cop0_entryhi=0x99;
            eq(read(c,address),0x12345678,"page_half_offset_read");
            eq(c.cop0_status&2,0,"successful_read_no_exception");
            write(c,address,0x456789ab);
            eq(mem.read32(physical),0x456789ab,"page_half_offset_store");
            c.cop0_entryhi=address|0x99;c.cop0_index=0x80000000;
            runtime.handleTLBP(ram,&c);eq(c.cop0_index,11,"probe_both_halves_page_size");
        }
    }
    // Large pages ignore the low PFN bits, rather than ORing them into offsets.
    for(unsigned i=1;i<pages.size();++i) {
        install(pages[i].mask,0xc0000042,0xc7,0x400c7);
        mem.write32(0,0x12345678);mem.write32(0x1000000,0x76543210);
        R5900Context c;c.cop0_status=0;
        eq(read(c,0xc0000000),0x12345678,"large_page_pfn_alignment_even");
        eq(read(c,0xc0000000+pages[i].size),0x76543210,"large_page_pfn_alignment_odd");
    }
    install(0,0xc1000042,0,0x4007);
    {R5900Context c;c.cop0_status=0;c.cop0_entryhi=0x42;mem.write32(0x100000,0x23456789);
     eq(read(c,0xc1001000),0x23456789,"odd_valid_independent_of_even_invalid");}
    // An ASID mismatch is a miss. Neither G bit alone makes an entry global.
    for(unsigned flags=0;flags<4;++flags) {
        install(0,0xc1000042,0x2006|(flags&1),0x4006|((flags>>1)&1));
        for(uint32_t asid: {0x42u,0x43u}) {
            R5900Context c;c.cop0_entryhi=0xc1000000|asid;c.cop0_index=0x80000000;
            runtime.handleTLBP(ram,&c);
            eq((c.cop0_index>>31),asid==0x42||flags==3?0:1,"probe_asid_global");
            c.cop0_status=0;c.cop0_entryhi=asid;mem.write32(0x80000,0x76543210);
            eq(read(c,0xc1000000),asid==0x42||flags==3?0x76543210:0,"load_asid_global");
        }
        R5900Context c;c.cop0_index=11;runtime.handleTLBR(ram,&c);
        eq(c.cop0_entrylo0&1,flags==3?1:0,"single_stored_global_even");
        eq(c.cop0_entrylo1&1,flags==3?1:0,"single_stored_global_odd");
    }
    install(0,0xc1000042,0,0);
    R5900Context probe;probe.cop0_entryhi=0xc1000042;probe.cop0_index=0x80000000;
    runtime.handleTLBP(ram,&probe);eq(probe.cop0_index,11,"probe_matches_invalid_entry");

    // Distinct refill, invalid and modified faults, with precise saved state.
    for(unsigned kind=0;kind<6;++kind) for(bool bev: {false,true})
    for(bool nested: {false,true}) for(bool delay: {false,true}) {
        const bool store=(kind&1)!=0;
        const uint32_t address=0xc1201030;
        if(kind<2)clear();else install(0,0xc1200042,6,kind<4?0:2);
        R5900Context c;c.pc=0x188014;c.branch_pc=0x188010;c.in_delay_slot=delay;
        c.cop0_status=(bev?0x400000:0)|(nested?2:0);
        c.cop0_epc=0x11223344;c.cop0_cause=0x80000400;c.cop0_context=0xa5800000;
        c.cop0_entryhi=0x12344042;mem.write32(0x1030,0x11223344);
        if(store)write(c,address,0xdeadbeef);else read(c,address);
        const unsigned code=kind==5?1:store?3:2; // kind4 is a valid clean read
        if(kind==4) {eq(c.cop0_status&2,nested?2:0,"clean_page_read_allowed");continue;}
        const bool refill=kind<2&&!nested;
        eq((c.cop0_cause>>2)&31,code,"fault_cause");
        eq(c.pc,(bev?0xbfc00200u:0x80000000u)+(refill?0:0x180),"fault_vector");
        eq(c.cop0_badvaddr,address,"fault_bad_virtual_address");
        eq(c.cop0_entryhi,0xc1200042,"fault_vpn_keeps_asid");
        eq(c.cop0_context,0xa5e09000,"fault_context_keeps_ptebase");
        eq(c.cop0_epc,nested?0x11223344:delay?0x188010:0x188014,"fault_epc");
        eq(c.cop0_cause>>31,nested?1:delay?1:0,"fault_bd");
        eq(c.cop0_cause&0x400,0x400,"fault_keeps_interrupt_pending");
        eq(c.cop0_status&2,2,"fault_sets_exl");
        eq(c.in_delay_slot,0,"fault_leaves_delay_slot");
        eq(mem.read32(0x1030),0x11223344,"fault_store_preserves_memory");
    }
    // Legal 16KiB scratchpad mapping, both pair halves and both upper/lower quarters.
    install(0,0xc2000042,0x80000007,7);
    for(uint32_t off: {0u,0xffcu,0x1000u,0x2000u,0x3ffcu}) {
        mem.write32(0x70000000+off,0xaabbccdd);
        R5900Context c;c.cop0_status=0;
        eq(read(c,0xc2000000+off),0xaabbccdd,"mapped_scratchpad_read");
        write(c,0xc2000000+off,0x11224488);
        eq(mem.read32(0x70000000+off),0x11224488,"mapped_scratchpad_store");
    }
    {R5900Context c;c.cop0_index=11;runtime.handleTLBR(ram,&c);
     eq(c.cop0_entrylo0&0x38,0x10,"scratchpad_even_cache_readback");
     eq(c.cop0_entrylo1&0x38,0x10,"scratchpad_odd_cache_readback");}
    // Execute actual emitted MTC0/MFC0, LW/SW and a faulting branch slot.
    for(const auto pair:std::array<std::array<uint32_t,3>,3>{{
        {{0x380,0x11223344,0x83ffffff}},{{0x3a0,0x11223344,0x03ffffff}},
        {{0x3c0,0x00543210,0xffd43210}}}}) {
        R5900Context c;c.pc=boundary_fixture::shared+pair[0];c.cop0_context=pair[1];
        SET_GPR_U32((&c),4,0xffffffff);SET_GPR_U32((&c),31,0x2220);
        runtime.lookupFunction(c.pc)(ram,&c,&runtime);eq(GPR_U32((&c),2),pair[2],"native_cop0_writable_fields");
    }
    for(unsigned route=0;route<4;++route) for(bool mapped: {false,true}) {
        if(mapped)install(0,0xc3000042,0x2007,0x4007);else clear();
        const uint32_t entry=boundary_fixture::shared+(route==3?0x3e0:route==2?0x340:route==1?0x320:0x300);
        R5900Context c;c.pc=entry;c.cop0_status=0;c.cop0_entryhi=0x42;
        SET_GPR_U32((&c),4,0xc3001020);SET_GPR_U32((&c),2,0x33445566);
        SET_GPR_U32((&c),5,77);SET_GPR_U32((&c),6,0);SET_GPR_U32((&c),31,0x2220);
        mem.write32(0x100020,0x12345678);bool transferred=false;
        try {runtime.lookupFunction(entry)(ram,&c,&runtime);}catch(const EeDispatcherTransfer&){transferred=true;}
        eq(transferred,!mapped,"native_fault_unwinds_to_dispatcher");
        eq(c.pc,mapped?0x2220:0x80000000,"native_memory_control_flow");
        eq(GPR_U32((&c),5),mapped&&route!=2?78:77,"native_post_fault_body_not_executed");
        eq(GPR_U32((&c),2),mapped&&route!=1?0x12345678:0x33445566,"native_failed_load_keeps_destination");
        eq(mem.read32(0x100020),mapped&&route==1?0x33445566:0x12345678,"native_store_effect");
        if(!mapped) {
            eq(c.cop0_epc,route==2?entry:route==3?entry+8:entry+4,"native_fault_precise_instruction");
            eq(c.cop0_cause>>31,route==2?1:0,"native_slot_fault_bd");
        }
    }
    // All runtime widths use the same ASID and odd-half translation.
    install(0,0xc4000042,0x2006,0x4006);
    R5900Context c;c.cop0_status=0;c.cop0_entryhi=0x42;
    mem.write32(0x100000,0);
    try {
        runtime.Store8(ram,&c,0xc4001000,0x12);eq(runtime.Load8(ram,&c,0xc4001000),0x12,"byte_mapped");
        runtime.Store16(ram,&c,0xc4001002,0x3456);eq(runtime.Load16(ram,&c,0xc4001002),0x3456,"halfword_mapped");
        runtime.Store64(ram,&c,0xc4001008,0x123456789abcdef0ull);
        eq(runtime.Load64(ram,&c,0xc4001008),0x123456789abcdef0ull,"doubleword_mapped");
        runtime.Store128(ram,&c,0xc4001010,_mm_set_epi32(4,3,2,1));
        alignas(16) uint32_t parts[4];_mm_store_si128((__m128i*)parts,runtime.Load128(ram,&c,0xc4001010));
        for(unsigned i=0;i<4;++i)eq(parts[i],i+1,"quadword_mapped");
    }catch(const EeDispatcherTransfer&) {eq(1,0,"mapped_width_unexpected_fault");}
    // Every width must transfer on a TLB miss before returning a value.
    clear();
    for(unsigned width: {8u,16u,32u,64u,128u}) for(bool store: {false,true}) {
        R5900Context fault;fault.cop0_status=0;fault.pc=0x188014;bool transferred=false;
        try {
            if(store) switch(width) {
                case 8:runtime.Store8(ram,&fault,0xc5000000,0);break;
                case 16:runtime.Store16(ram,&fault,0xc5000000,0);break;
                case 32:runtime.Store32(ram,&fault,0xc5000000,0);break;
                case 64:runtime.Store64(ram,&fault,0xc5000000,0);break;
                case 128:runtime.Store128(ram,&fault,0xc5000000,_mm_setzero_si128());break;
            } else switch(width) {
                case 8:runtime.Load8(ram,&fault,0xc5000000);break;
                case 16:runtime.Load16(ram,&fault,0xc5000000);break;
                case 32:runtime.Load32(ram,&fault,0xc5000000);break;
                case 64:runtime.Load64(ram,&fault,0xc5000000);break;
                case 128:runtime.Load128(ram,&fault,0xc5000000);break;
            }
        }catch(const EeDispatcherTransfer&){transferred=true;}
        eq(transferred,1,"all_width_fault_transfer");
        eq((fault.cop0_cause>>2)&31,store?3:2,"all_width_fault_cause");
    }
    install(0,0xc4000042,0x2006,0x4006);
    auto saved=ps2x::StateArchive::writer();mem.serializeState(saved);clear();
    auto loaded=ps2x::StateArchive::reader(saved.data());mem.serializeState(loaded);
    eq(read(c,0xc4001000),0x34560012,"mapped_state_roundtrip");
    // A resolved physical address must not be interpreted as a virtual RAM mirror.
    // Unbacked physical bus semantics are outside this model; these writes must
    // at least not corrupt unrelated RDRAM through virtual alias normalization.
    install(0,0xc6000042,0x800487,0x8004c7); // physical 0x20012000 / 0x20013000
    for(unsigned width: {8u,16u,32u,64u,128u}) {
        mem.write32(0x12340,0x12345678);c.cop0_entryhi=0x42;c.cop0_status=0;
        try {
            switch(width) {
                case 8:runtime.Store8(ram,&c,0xc6000340,0);break;
                case 16:runtime.Store16(ram,&c,0xc6000340,0);break;
                case 32:runtime.Store32(ram,&c,0xc6000340,0);break;
                case 64:runtime.Store64(ram,&c,0xc6000340,0);break;
                case 128:runtime.Store128(ram,&c,0xc6000340,_mm_setzero_si128());break;
            }
        }catch(const EeDispatcherTransfer&) {}
        eq(mem.read32(0x12340),0x12345678,"physical_pfn_is_not_virtual_ram_alias");
    }
    // A PFN numerically equal to the scratch virtual marker does not set S.
    install(0,0xc6000042,0x1c00007,0x1c00047);
    mem.write32(0x70000000,0x12345678);c.cop0_status=0;
    write(c,0xc6000000,0);
    eq(mem.read32(0x70000000),0x12345678,"physical_pfn_is_not_scratchpad_without_s");
    // Existing low-address and direct aliases remain independent of ASID/TLB state.
    clear();mem.write32(0x12340,0xa1b2c3d4);
    for(uint32_t addr: {0x12340u,0x20012340u,0x80012340u,0xa0012340u}) {
        c.cop0_entryhi=0xff;c.cop0_status=0;
        eq(read(c,addr),0xa1b2c3d4,"direct_alias_control");
    }
}
