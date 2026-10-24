// Compile the actual complete CLI pipeline's output for synthetic input only.
#include "boundary_fixture_data.h"
#include "ps2recomp/ps2_recompiler.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <vector>
int main(int argc,char**argv) {
    try {
        if(argc!=2)throw std::runtime_error("expected fixture output directory");
        const std::filesystem::path dir=std::filesystem::absolute(argv[1]);
        std::filesystem::create_directories(dir);
        const auto bin=dir/"input.bin",csv=dir/"functions.csv",profile=dir/"profile.toml";
        std::vector<uint32_t> image(boundary_fixture::Count*16,0);
        std::ofstream rows(csv);rows<<"Name,Start,End,Size\n";
        for(unsigned i=0;i<boundary_fixture::Count;++i) {
            const uint32_t pc=boundary_fixture::base+i*boundary_fixture::stride;
            auto w=boundary_fixture::words[i];
            if(i<=boundary_fixture::Call)w.branch|=(pc+32)>>2;
            image[i*16]=w.branch;image[i*16+1]=w.slot;
            image[i*16+2]=0x03e00008; // fallthrough: JR ra; NOP
            image[i*16+8]=0x24c60001; // real native callee: ADDIU a2,a2,1; JR a3; NOP
            image[i*16+9]=0x00e00008;
            rows<<"branch_"<<i<<",0x"<<std::hex<<pc<<",0x"<<pc+4<<",4\n";
            rows<<"resume_"<<i<<",0x"<<pc+8<<",0x"<<pc+16<<",8\n";
            rows<<"callee_"<<i<<",0x"<<pc+32<<",0x"<<pc+44<<",12\n"<<std::dec;
        }
        rows.close();
        std::ofstream raw(bin,std::ios::binary);
        for(uint32_t word:image)for(unsigned b=0;b<4;++b)raw.put(static_cast<char>(word>>(8*b)));
        raw.close();
        std::ofstream config(profile);
        config<<"[general]\ninput_format=\"raw\"\nmodule_id=\"boundary_fixture\"\nraw_base="<<boundary_fixture::base
              <<"\nraw_code_offset=0\nsingle_file_output=false\n";
        for(const auto&[key,path]:std::vector<std::pair<const char*,std::filesystem::path>>{{"input",bin},{"ghidra_output",csv},{"output",dir/"units"}})
            config<<key<<'='<<std::quoted(path.generic_string())<<'\n';
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
