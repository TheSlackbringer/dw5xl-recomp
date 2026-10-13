#include "ps2_runtime.h"
#include "ps2_stubs.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstring>
#include <stdexcept>
#include <chrono>
#include <iostream>
extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x1000;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x1004;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 1;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[] = {nullptr};
static void reg(R5900Context& c, unsigned n, uint32_t v) { c.r[n]=_mm_cvtsi32_si128(v); }
static uint32_t result(R5900Context& c) { return _mm_cvtsi128_si32(c.r[2]); }
static void require(bool ok, const char* why) { if(!ok) throw std::runtime_error(why); }
int main() {
 const auto root=std::filesystem::temp_directory_path()/
  ("dw5xl-cd-iop-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 try {
  std::filesystem::create_directory(root);
  std::vector<uint8_t> expected(2048); for(size_t i=0;i<expected.size();++i) expected[i]=(i*37+11)&255;
  { std::ofstream f(root/"fixture.bin",std::ios::binary); f.write(reinterpret_cast<char*>(expected.data()),expected.size()); }
  PS2Runtime::IoPaths paths{}; paths.cdRoot=root; PS2Runtime::setIoPaths(paths);
  PS2Runtime runtime; R5900Context c{}; std::vector<uint8_t> ee(32*1024*1024,0xa5);
  std::strcpy(reinterpret_cast<char*>(ee.data()+0x3000),"cdrom0:\\fixture.bin;1");
  reg(c,4,0x4000); reg(c,5,0x3000); ps2_stubs::sceCdSearchFile(ee.data(),&c,&runtime);
  require(result(c)==1,"search synthetic sector"); uint32_t lbn; std::memcpy(&lbn,ee.data()+0x4000,4);
  const auto destination=runtime.allocateIopMemory(2048,16); require(destination!=0,"allocate IOP destination");
  reg(c,4,lbn); reg(c,5,1); reg(c,6,destination); ps2_stubs::sceCdReadIOPm(ee.data(),&c,&runtime);
  require(result(c)==1,"IOP read accepted"); std::vector<uint8_t> got(2048);
  require(runtime.readIopMemory(destination,got.data(),got.size()) && got==expected,"sector belongs in IOP RAM");
  for(size_t i=0;i<2048;++i) require(ee[destination+i]==0xa5,"IOP read must not overwrite EE RAM");
  reg(c,5,0xffffffffu); ps2_stubs::sceCdReadIOPm(ee.data(),&c,&runtime); require(result(c)==0,"oversized read rejected");
  require(runtime.readIopMemory(destination,got.data(),got.size()) && got==expected,"invalid read preserves IOP RAM");
  reg(c,5,1); reg(c,6,0x1ffc00); ps2_stubs::sceCdReadIOPm(ee.data(),&c,&runtime); require(result(c)==0,"cross-end read rejected");
  std::filesystem::remove_all(root); std::cout<<"CD-to-IOP memory isolation tests passed\n"; return 0;
 } catch(const std::exception& e) { std::filesystem::remove_all(root); std::cerr<<e.what()<<'\n'; return 1; }
}
