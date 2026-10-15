// Authored synthetic backend test: real GS presentation packing, no GPU/game data.
#include "ps2_runtime.h"
#include <iostream>
#include <stdexcept>
extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x1000;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x1004;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 1;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[] = {nullptr};
static void check(bool v, const char *msg) { if (!v) throw std::runtime_error(msg); }
class SyntheticBackend final : public GSRasterBackend {
public:
 PresentationFrame frame; uint32_t scale=1, samples=1;
 void Initialize(uint8_t*,uint32_t) override {} void Reset() override {}
 void Submit(const GSPrimitiveBatch&) override {} void LoadClut(const GSTex0Reg&,const GSTexClutReg&) override {}
 void BeginTransfer(const GSTransferCommand&) override {} void UploadImage(const uint8_t*,uint32_t) override {}
 void Flush() override {} void TextureFlush() override {} void Sync(GSSyncReason) override {}
 PresentationFrame Present(const GSPresentationRequest&) override { return frame; }
 bool ClearFramebuffer(const GSContext&,uint32_t) override {return false;}
 uint32_t ConsumeLocalToHostBytes(uint8_t*,uint32_t) override {return 0;}
 uint32_t ReadVram(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t) const override {return 0;}
 void WriteVram(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t) override {}
 void SnapshotVram(std::vector<uint8_t>& out) const override {out.clear();}
 GSTransferSnapshot GetTransferSnapshot() const override {return {};}
 void SetResolutionScale(uint32_t v) override {scale=v;} uint32_t GetResolutionScale() const override {return scale;}
 void SetAASamples(uint32_t v) override {samples=v;} uint32_t GetAASamples() const override {return samples;}
};
static PresentationFrame makeFrame(uint32_t w,uint32_t h,uint32_t stride,uint32_t dw,uint32_t dh) {
 PresentationFrame f; f.width=w;f.height=h;f.strideBytes=stride;f.displayWidth=dw;f.displayHeight=dh;
 const uint32_t storageStride=stride?stride:640*4;
 f.pixels.assign(size_t(storageStride)*h,0xee);
 for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x) {
  const size_t p=size_t(y)*storageStride+x*4;
  f.pixels[p]=uint8_t(x);f.pixels[p+1]=uint8_t(y);f.pixels[p+2]=uint8_t(x^y);f.pixels[p+3]=255;
 }
 return f;
}
int main() {try {
 GS gs;auto fake=std::make_unique<SyntheticBackend>();auto* backend=fake.get();gs.setRasterBackend(std::move(fake));
 std::vector<uint8_t> ram(4u<<20);GSRegisters regs{};gs.init(ram.data(),ram.size(),&regs);
 gs.setAASamples(4);gs.setResolutionScale(2);check(gs.getAASamples()==4 && gs.getResolutionScale()==2,"AA and output forwarded separately");
 for(const auto& f : {makeFrame(512,512,0,0,0),makeFrame(1280,511,1280*4,640,512),makeFrame(1280,1024,1280*4,640,512),makeFrame(640,512,640*4,640,512)}) {
  backend->frame=f;gs.latchHostPresentationFrame();std::vector<uint8_t> out;uint32_t w=0,h=0,dw=0,dh=0;
  check(gs.copyLatchedHostPresentationFrame(out,w,h,nullptr,nullptr,nullptr,&dw,&dh),"valid frame packed");
  check(w==f.width && h==f.height && out.size()==size_t(w)*h*4,"actual pixel geometry retained");
  check(dw==(f.displayWidth?f.displayWidth:w) && dh==(f.displayHeight?f.displayHeight:h),"logical aspect separate from stored geometry");
  for(uint32_t y : {0u,h/2,h-1})for(uint32_t x : {0u,w/2,w-1}) {
   size_t p=(size_t(y)*w+x)*4;check(out[p]==uint8_t(x)&&out[p+1]==uint8_t(y)&&out[p+2]==uint8_t(x^y)&&out[p+3]==255,"packing crosses old640 width boundary correctly");
  }
 }
 backend->frame=makeFrame(1280,511,1280*4,640,512);backend->frame.pixels.pop_back();gs.latchHostPresentationFrame();
 std::vector<uint8_t> out;uint32_t w=9,h=9;check(!gs.copyLatchedHostPresentationFrame(out,w,h)&&out.empty()&&w==0&&h==0,"truncated wider frame rejected");
 gs.reset();check(!gs.copyLatchedHostPresentationFrame(out,w,h),"reset clears prior-scale presentation");
 std::cout<<"Synthetic presentation stride/geometry tests passed\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
