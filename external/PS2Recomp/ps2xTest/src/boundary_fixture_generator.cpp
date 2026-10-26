// Compile the actual complete CLI pipeline's output for synthetic input only.
#include "boundary_fixture_data.h"
#include "ps2recomp/ps2_recompiler.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <vector>
#include <algorithm>
#include <initializer_list>
int main(int argc,char**argv) {
    try {
        if(argc!=2)throw std::runtime_error("expected fixture output directory");
        const std::filesystem::path dir=std::filesystem::absolute(argv[1]);
        std::filesystem::create_directories(dir);
        const auto bin=dir/"input.bin",csv=dir/"functions.csv",profile=dir/"profile.toml";
        std::vector<uint32_t> image((boundary_fixture::shared+0x500-boundary_fixture::base)/4,0);
        std::ofstream rows(csv);rows<<"Name,Start,End,Size\n";
        for(unsigned i=0;i<boundary_fixture::Count;++i) {
            const uint32_t pc=boundary_fixture::base+i*boundary_fixture::stride;
            auto w=boundary_fixture::words[i];
            if(i<=boundary_fixture::Call)w.branch|=(pc+32)>>2;
            image[i*16]=w.branch;image[i*16+1]=w.slot;
            image[i*16+2]=0x03e00008; // fallthrough: JR ra; NOP
            image[i*16+8]=0x24c60001; // real native callee: ADDIU a2,a2,1; JR a3; NOP
            image[i*16+9]=0x00e00008;
            const uint32_t end=pc+(i==boundary_fixture::DirectSlotBreak?16:4);
            rows<<"branch_"<<i<<",0x"<<std::hex<<pc<<",0x"<<end<<","<<std::dec<<end-pc<<"\n"<<std::hex;
            rows<<"resume_"<<i<<",0x"<<pc+8<<",0x"<<pc+16<<",8\n";
            rows<<"callee_"<<i<<",0x"<<pc+32<<",0x"<<pc+44<<",12\n"<<std::dec;
        }
        const auto put=[&](uint32_t at,std::initializer_list<uint32_t> words) {
            std::copy(words.begin(),words.end(),image.begin()+(at-boundary_fixture::base)/4);
        };
        const auto row=[&](const char*name,uint32_t start,uint32_t end) {
            rows<<name<<",0x"<<std::hex<<start<<",0x"<<end<<","<<std::dec<<end-start<<"\n";
        };
        using namespace boundary_fixture;
        // Real declared overlapping entry: one wrapper stops at an indirect jump,
        // while another reaches the arithmetic. An explicitly configured interior
        // entry also has to continue through the clipped owner's epilogue.
        put(overlap,{0x00a00008,0,0x24210001,0x24840004,0x24840008,0x03e00008,0});
        row("overlap_owner",overlap,overlap+16);
        row("entry_00190008",overlap+8,overlap+28);
        // Clipped JAL body saves/restores the caller's RA around an actual native
        // callee; its return continuation is not declared in the map.
        put(clipped,{0x27ea0000,0x0c000000|((clipped+128)>>2),0x27e40001,
            0x255f0000,0x24840007,0x03e00008,0});
        put(clipped+32,{0x25290063,0x03e00008,0});
        put(clipped+128,{0x24c60001,0x03e00008,0});
        row("clipped_caller",clipped,clipped+8);
        row("unrelated_call_neighbour",clipped+32,clipped+44);
        row("native_call_target",clipped+128,clipped+140);
        // Two native roots share one undeclared disjoint tail, beyond an
        // unrelated callable body which must not be absorbed or executed.
        put(shared,{0x1020001f,0x24840004,0x24840008,0x03e00008,0});
        put(shared+32,{0x25290063,0x03e00008,0});
        put(shared+64,{0x08000000|((shared+128)>>2),0x24840002});
        put(shared+128,{0x24840010,0x03e00008,0});
        row("shared_first",shared,shared+20);
        row("unrelated_shared_neighbour",shared+32,shared+44);
        row("shared_second",shared+64,shared+72);
        std::copy(copiedWords.begin(),copiedWords.end(),image.begin()+(copySource-base)/4);
        put(copySource+64,{0x2402002a,0x03e00008,0});
        // Independent COP0 field retention fixture: MTC0 EntryHi; MFC0 v0; JR ra.
        put(shared+0x1c0,{0x40845000,0x40025000,0x03e00008,0});
        row("entryhi_roundtrip",shared+0x1c0,shared+0x1d0);
        // A kernel-return syscall must preserve its nonlocal native target.
        put(shared+0x1a0,{0x2403fff8,0x0000000c,0x24840001,0x03e00008,0});
        row("kernel_return",shared+0x1a0,shared+0x1b4);
        // Independent native load/store and COP0 fixtures for mapped access effects.
        put(shared+0x300,{0x24c60001,0x8c820000,0x24a50001,0x03e00008,0});
        row("tlb_load",shared+0x300,shared+0x314);
        put(shared+0x320,{0x24c60001,0xac820000,0x24a50001,0x03e00008,0});
        row("tlb_store",shared+0x320,shared+0x334);
        put(shared+0x340,{0x10000003,0x8c820000,0x24a50001,0,0x03e00008,0});
        row("tlb_load_slot",shared+0x340,shared+0x358);
        put(shared+0x380,{0x40841000,0x40021000,0x03e00008,0});
        row("entrylo0_roundtrip",shared+0x380,shared+0x390);
        put(shared+0x3a0,{0x40841800,0x40021800,0x03e00008,0});
        row("entrylo1_roundtrip",shared+0x3a0,shared+0x3b0);
        put(shared+0x3c0,{0x40842000,0x40022000,0x03e00008,0});
        row("context_roundtrip",shared+0x3c0,shared+0x3d0);
        put(shared+0x3e0,{0x3c04c300,0x34841020,0x8c820000,0x24a50001,0x03e00008,0});
        row("tlb_constant_load",shared+0x3e0,shared+0x3f8);
        rows.close();
        std::ofstream raw(bin,std::ios::binary);
        for(uint32_t word:image)for(unsigned b=0;b<4;++b)raw.put(static_cast<char>(word>>(8*b)));
        raw.close();
        std::ofstream config(profile);
        config<<"[general]\nentry_points=[\"slot_break@0x"<<std::hex<<boundary_fixture::address(boundary_fixture::DirectSlotBreak)+4<<"\",\"overlap_resume@0x"<<boundary_fixture::overlap+12<<"\"]\n"<<std::dec
              <<"input_format=\"raw\"\nmodule_id=\"boundary_fixture\"\nraw_base="<<boundary_fixture::base
              <<"\nraw_code_offset=0\nsingle_file_output=false\n";
        for(const auto&[key,path]:std::vector<std::pair<const char*,std::filesystem::path>>{{"input",bin},{"ghidra_output",csv},{"output",dir/"units"}})
            config<<key<<'='<<std::quoted(path.generic_string())<<'\n';
        config<<"\n[[code_copies]]\nname=\"native_copy\"\nsource="<<copySource
              <<"\ndestination="<<copyBase<<"\nsize=64\nfunctions=[[0,24],[32,16]]\n";
        config<<"\n[[code_copies]]\nname=\"second_copy\"\nsource="<<copySource+64
              <<"\ndestination="<<secondCopyBase<<"\nsize=64\nfunctions=[[0,12]]\n";
        config.close();
        ps2recomp::PS2Recompiler recompiler(profile.string());
        if(!recompiler.initialize()||!recompiler.recompile())throw std::runtime_error("synthetic generation failed");
        recompiler.generateOutput();
        // Use the production multi-file layout. These build-only aggregators
        // include the generator's unchanged synthetic output; they do not replace it.
        std::ofstream aggregate(dir/"ps2_recompiled_functions.cpp");
        for(const auto &file:std::filesystem::directory_iterator(dir/"units"))
            if(file.path().extension()==".cpp" && file.path().filename()!="register_functions.cpp")
                aggregate<<"#include \"units/"<<file.path().filename().generic_string()<<"\"\n";
        std::ofstream table(dir/"register_functions.cpp");
        table<<"#include \"units/register_functions.cpp\"\n";
        return 0;
    } catch(const std::exception&e) {std::cerr<<e.what()<<'\n';return 1;}
}
