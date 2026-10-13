#include "ps2_runtime.h"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x1000;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x1004;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 1;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[] = {nullptr};

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static void append32(std::vector<uint8_t>& bytes, uint32_t word) {
    const size_t offset=bytes.size(); bytes.resize(offset+4);
    std::memcpy(bytes.data()+offset,&word,4);
}
static void checkFormat(PS2Memory& memory, uint32_t opcode, uint32_t laneBytes,
                        uint32_t vectors, bool trailingNops) {
    std::vector<uint8_t> stream;
    append32(stream,(opcode<<24)|((vectors&255u)<<16)|0x4000u);
    const uint32_t payloadBytes=vectors*4*laneBytes;
    for(uint32_t i=0;i<payloadBytes;++i) stream.push_back((i*13+7)&255);
    const auto payload=stream;
    if(trailingNops) for(int i=0;i<4;++i) append32(stream,0);
    std::memset(memory.getVU1Data(),0xa5,PS2_VU1_DATA_SIZE);
    memory.processVIF1Data(stream.data(),static_cast<uint32_t>(stream.size()));
    for(uint32_t vector=0;vector<vectors;++vector) {
        for(uint32_t lane=0;lane<4;++lane) {
            uint32_t expected=0,actual=0;
            std::memcpy(&expected,payload.data()+4+(vector*4+lane)*laneBytes,laneBytes);
            std::memcpy(&actual,memory.getVU1Data()+vector*16+lane*4,4);
            require(actual==expected,"UNPACK must retain all lanes including exact-ending last vector");
        }
    }
    for(uint32_t byte=vectors*16;byte<vectors*16+16;++byte)
        require(memory.getVU1Data()[byte]==0xa5,"UNPACK must preserve the following VU vector");
}
int main() {
    try {
        PS2Memory memory;
        require(memory.initialize(),"memory initialization");
        for(const bool trailing : {false,true})
            for(const uint32_t vectors : {1u,2u,17u,256u}) {
                checkFormat(memory,0x6c,4,vectors,trailing);
                checkFormat(memory,0x6d,2,vectors,trailing);
                checkFormat(memory,0x6e,1,vectors,trailing);
            }
        std::cout<<"VIF exact-ending UNPACK boundary tests passed\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<error.what()<<'\n'; return 1;
    }
}
