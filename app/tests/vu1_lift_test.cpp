// Unit test for lifted VU1 microprograms in DW5XL battle.
#define private public
#include "runtime/ps2_vu1.h"
#undef private
#include "runtime/ps2_vu1_lift.h"
#include "ps2_runtime.h"
#include "runtime/ps2_device_queue.h"
#include <condition_variable>
#include <mutex>
#include <string>

#include <cfenv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x1000;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x1004;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 1;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[] = {nullptr};

namespace {

void require(bool condition, const char *msg)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << msg << std::endl;
        throw std::runtime_error(msg);
    }
}

std::vector<uint8_t> loadFile(const std::string &path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        std::ifstream f2(std::string("../") + path, std::ios::binary);
        if (f2)
            return std::vector<uint8_t>((std::istreambuf_iterator<char>(f2)),
                                        std::istreambuf_iterator<char>());
        std::ifstream f3(std::string("../../") + path, std::ios::binary);
        if (f3)
            return std::vector<uint8_t>((std::istreambuf_iterator<char>(f3)),
                                        std::istreambuf_iterator<char>());
        throw std::runtime_error("Could not open file: " + path);
    }
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
}

void testRegistrations()
{
    std::cout << "[TEST] Checking vl::find registrations..." << std::endl;
    auto f1 = vl::find(0x1ad6eefff0c2e7e5ull, 0x0800);
    require(f1 != nullptr, "lift_1ad6_0800 registered");

    auto f2 = vl::find(0xda28291ffe0b3334ull, 0x0010);
    require(f2 != nullptr, "lift_da28_0010 registered");

    auto f3 = vl::find(0x1b91674603e323f2ull, 0x0800);
    require(f3 != nullptr, "lift_1b91_0800 registered");

    auto f4 = vl::find(0x1ad6eefff0c2e7e5ull, 0x1000);
    require(f4 != nullptr, "lift_1ad6_1000 registered");

    auto f_none = vl::find(0xdeadbeefull, 0x0000);
    require(f_none == nullptr, "unregistered hash returns nullptr");

    std::cout << "       All 4 registrations verified successfully!" << std::endl;
}

// Compare two VU states and 16 KB data memories
void verifyEquivalence(const char *name, const VU1State &sInterp, const uint8_t *memInterp,
                       const VU1State &sLift, const uint8_t *memLift,
                       uint32_t expectedFinalPc)
{
    std::cout << "[TEST] Verifying state equivalence for " << name << "..." << std::endl;
    std::cout << "       Interp Q=" << sInterp.q << " Lift Q=" << sLift.q << std::endl;

    // Check PC
    if (sLift.pc != expectedFinalPc)
    {
        std::cerr << "PC mismatch: lift=0x" << std::hex << sLift.pc
                  << " expected=0x" << expectedFinalPc << std::dec << std::endl;
        require(false, "PC mismatch");
    }

    // Check VI registers
    for (int i = 0; i < 16; ++i)
    {
        int16_t viI = static_cast<int16_t>(sInterp.vi[i] & 0xFFFF);
        int16_t viL = static_cast<int16_t>(sLift.vi[i] & 0xFFFF);
        if (viI != viL)
        {
            std::cerr << "VI[" << i << "] mismatch: interp=" << viI << " lift=" << viL << std::endl;
            require(false, "VI register mismatch");
        }
    }

    // Check VF registers
    for (int i = 0; i < 32; ++i)
    {
        for (int c = 0; c < 4; ++c)
        {
            uint32_t bI, bL;
            std::memcpy(&bI, &sInterp.vf[i][c], 4);
            std::memcpy(&bL, &sLift.vf[i][c], 4);
            if (bI != bL)
            {
                std::cerr << "VF[" << i << "][" << "xyzw"[c] << "] mismatch: interp="
                          << sInterp.vf[i][c] << " (0x" << std::hex << bI
                          << ") lift=" << sLift.vf[i][c] << " (0x" << bL << ")"
                          << std::dec << std::endl;
                require(false, "VF register mismatch");
            }
        }
    }

    // Check ACC
    for (int c = 0; c < 4; ++c)
    {
        uint32_t bI, bL;
        std::memcpy(&bI, &sInterp.acc[c], 4);
        std::memcpy(&bL, &sLift.acc[c], 4);
        if (bI != bL)
        {
            std::cerr << "ACC[" << "xyzw"[c] << "] mismatch: interp="
                      << sInterp.acc[c] << " (0x" << std::hex << bI
                      << ") lift=" << sLift.acc[c] << " (0x" << bL << ")"
                      << std::dec << std::endl;
            require(false, "ACC mismatch");
        }
    }

    // Check Q
    {
        uint32_t bI, bL;
        std::memcpy(&bI, &sInterp.q, 4);
        std::memcpy(&bL, &sLift.q, 4);
        if (bI != bL)
        {
            std::cerr << "Q mismatch: interp=" << sInterp.q << " (0x" << std::hex << bI
                      << ") lift=" << sLift.q << " (0x" << bL << ")" << std::dec << std::endl;
            require(false, "Q mismatch");
        }
    }

    // Check CLIP
    if ((sInterp.clip & 0xFFFFFFu) != (sLift.clip & 0xFFFFFFu))
    {
        std::cerr << "CLIP mismatch: interp=0x" << std::hex << (sInterp.clip & 0xFFFFFFu)
                  << " lift=0x" << (sLift.clip & 0xFFFFFFu) << std::dec << std::endl;
        require(false, "CLIP mismatch");
    }

    // Check data memory (16 KB)
    for (size_t i = 0; i < 16384; i += 4)
    {
        uint32_t wI, wL;
        std::memcpy(&wI, memInterp + i, 4);
        std::memcpy(&wL, memLift + i, 4);
        if (wI != wL)
        {
            std::cerr << "Memory mismatch at offset 0x" << std::hex << i
                      << ": interp=0x" << wI << " lift=0x" << wL << std::dec << std::endl;
            require(false, "Data memory mismatch");
        }
    }

    std::cout << "       State and memory identical bit-for-bit!" << std::endl;
}

// Equivalence test for a segment
void testSegment(const std::string &binPath, uint64_t hash, uint32_t entryPc, uint32_t expectedFinalPc)
{
    auto code = loadFile(binPath);
    require(code.size() == 16384, "Code size 16KB");

    auto fn = vl::find(hash, entryPc);
    require(fn != nullptr, "Segment must be registered");

    PS2Runtime rtInterp;
    require(rtInterp.memory().initialize(), "rtInterp.memory().initialize()");

    std::memcpy(rtInterp.memory().getVU1Code(), code.data(), 16384);

    // Setup initial data memory with reproducible test patterns
    uint8_t *dataInterp = rtInterp.memory().getVU1Data();
    alignas(16) uint8_t dataLift[16384];
    for (size_t i = 0; i < 16384; ++i)
        dataInterp[i] = static_cast<uint8_t>((i * 101 + 37) & 0xFF);

    // Provide valid packet header at offset 0 (top=0)
    // vi4 = loop count = 1
    // submesh vertex count = 4
    uint32_t *dw = reinterpret_cast<uint32_t *>(dataInterp);
    dw[0] = 1; // vi4: 1 outer loop
    dw[4] = 4; // vi6: 4 vertices in submesh

    // Set vertex positions, normals, colors, UVs
    for (int v = 0; v < 4; ++v)
    {
        float *pos = reinterpret_cast<float *>(dataInterp + (2 + v) * 16);
        pos[0] = 10.0f + v; pos[1] = 20.0f + v; pos[2] = 30.0f + v; pos[3] = 1.0f;

        float *nrm = reinterpret_cast<float *>(dataInterp + (6 + v) * 16);
        nrm[0] = 0.5f; nrm[1] = 0.5f; nrm[2] = 0.7f; nrm[3] = 0.0f;

        float *col = reinterpret_cast<float *>(dataInterp + (10 + v) * 16);
        col[0] = 128.0f; col[1] = 64.0f; col[2] = 255.0f; col[3] = 128.0f;

        float *uv = reinterpret_cast<float *>(dataInterp + (14 + v) * 16);
        uv[0] = 0.25f * v; uv[1] = 0.5f * v; uv[2] = 0.0f; uv[3] = 1.0f;
    }

    if (hash == 0xda28291ffe0b3334ull)
    {
        dw[12] = 1; // vi2 at 3(vi5): vertex count = 1
        dw[19] = 1; // vi3 at 0(vi7): bone count = 1 -> 1-bone unskinned path
        // Bones at quadwords 16..20
        for (int i = 0; i < 4; ++i)
        {
            float *bmat = reinterpret_cast<float *>(dataInterp + (16 + i) * 16);
            bmat[0] = (i == 0) ? 1.0f : 0.0f;
            bmat[1] = (i == 1) ? 1.0f : 0.0f;
            bmat[2] = (i == 2) ? 1.0f : 0.0f;
            bmat[3] = (i == 3) ? 1.0f : 0.0f;
        }
    }

    std::memcpy(dataLift, dataInterp, 16384);

    // Setup initial VU1State on both
    auto &sInterp = rtInterp.vu1().state();
    for (int i = 1; i < 32; ++i)
    {
        sInterp.vf[i][0] = 1.0f + i * 0.1f;
        sInterp.vf[i][1] = 2.0f + i * 0.1f;
        sInterp.vf[i][2] = 3.0f + i * 0.1f;
        sInterp.vf[i][3] = 4.0f + i * 0.1f;
    }
    sInterp.vf[0][0] = 0.0f; sInterp.vf[0][1] = 0.0f; sInterp.vf[0][2] = 0.0f; sInterp.vf[0][3] = 1.0f;
    sInterp.acc[0] = 0.0f; sInterp.acc[1] = 0.0f; sInterp.acc[2] = 0.0f; sInterp.acc[3] = 0.0f;
    sInterp.q = 1.0f;
    sInterp.i = 255.0f;
    sInterp.clip = 0;
    sInterp.mac = 0;
    sInterp.status = 0;

    if (hash == 0xda28291ffe0b3334ull)
    {
        sInterp.vi[8] = 0x7fff;
        sInterp.vi[10] = 0;
        sInterp.vi[14] = 0x1111;
        sInterp.vi[15] = 0x7fff;
    }

    VU1State sLift = sInterp;

    // Run interpreter (LIFT=0)
    setenv("PS2X_VU1_LIFT", "0", 1);
    rtInterp.vu1().execute(rtInterp.memory().getVU1Code(), 16384,
                           rtInterp.memory().getVU1Data(), 16384,
                           rtInterp.gs(), &rtInterp.memory(),
                           entryPc, 0, 0, 100000);

    std::cout << "[DEBUG] Interp PC=0x" << std::hex << sInterp.pc
              << " cycles=" << std::dec << sInterp.cycles
              << " VI[1]=" << sInterp.vi[1] << std::endl;

    // Run lifted segment
    bool kickActive = false;
    vl::Run c{sLift, dataLift, 1000, 1000, 1000, &rtInterp.vu1(),
              [](void *) {},
              [](void *, uint32_t, uint64_t) {},
              &kickActive, 1000};
    const int prevRound = std::fegetround();
    std::fesetround(FE_TOWARDZERO);
    fn(c);
    std::fesetround(prevRound);

    std::cout << "[DEBUG] Lifted PC=0x" << std::hex << sLift.pc
              << " VI[1]=" << sLift.vi[1] << std::endl;

    verifyEquivalence(binPath.c_str(), sInterp, dataInterp, sLift, dataLift, expectedFinalPc);
}

// Synthetic SDK-independent microcode: XGKICK A, NOP runway, SQ sentinel,
// XGKICK B, E-bit + delay. No game data and no graphics backend is initialised.
class CancelGate {
public:
    void arrive() { std::unique_lock lock(mutex); entered=true; cv.notify_all(); cv.wait(lock,[&]{return released;}); }
    void wait() { std::unique_lock lock(mutex); cv.wait(lock,[&]{return entered;}); }
    void release() { std::lock_guard lock(mutex); released=true; cv.notify_all(); }
private:
    std::mutex mutex; std::condition_variable cv; bool entered=false,released=false;
};

std::vector<uint32_t> syntheticCancelCode()
{
    constexpr uint32_t lowerNop=0x8000033cu, upperNop=0x000002ffu;
    std::vector<uint32_t> code(4096);
    for(size_t i=0;i<code.size();i+=2) { code[i]=lowerNop; code[i+1]=upperNop; }
    code[0]=0x800006fcu | (1u<<11); // XGKICK vi1
    code[64]=(1u<<25)|(15u<<21)|(1u<<11)|64u; // SQ.xyzw vf1,64(vi0)
    code[66]=0x800006fcu | (2u<<11); // XGKICK vi2
    code[69]=upperNop|(1u<<30); // E bit at PC272
    return code;
}

unsigned syntheticLiftCalls=0; // worker-owned; observed only after explicit drain

void syntheticCancelLift(vl::Run &c)
{
    ++syntheticLiftCalls;
    vl::checkReset(c);
    const uint64_t begin=c.cycle;
    vl::kickStart(c,static_cast<uint32_t>(c.st.vi[1]),begin);
    vl::kickCatchUp(c,begin+32);
    vl::checkReset(c);
    vl::sq<15,true>(c,c.mem,64,vl::load(c.st.vf[1]),begin+32);
    const uint64_t second=vl::kickWait(c,begin+33);
    vl::kickStart(c,static_cast<uint32_t>(c.st.vi[2]),second);
    c.cycle=second+3; c.endCycle=c.cycle; c.st.pc=288;
}

void testRealVuDmaCancellation(bool lifted)
{
    // runLifted caches its enabled flag. These two modes must run in separate
    // executable invocations before existing equivalence tests initialise it.
    setenv("PS2X_VU1_LIFT",lifted?"1":"0",1);
    const auto code=syntheticCancelCode();
    const vl::Registration registration{vl::hashCode(reinterpret_cast<const uint8_t *>(code.data()),16384),0,syntheticCancelLift};
    const vl::Registrar registrar(&registration,1);
    syntheticLiftCalls=0;
    for(bool hardReset : {false,true}) {
        CancelGate gate;
        PS2Runtime rt;
        require(rt.memory().initialize(),"real cancellation memory init");
        auto &mem=rt.memory(); auto &vu=rt.vu1();
        std::memcpy(mem.getVU1Code(),code.data(),16384);
        std::memset(mem.getVU1Data(),0,16384);
        auto packet=[&](unsigned qword,uint32_t marker) {
            uint64_t words[4]={1ull|(1ull<<15)|(1ull<<60),0xeull,marker,0x4full};
            std::memcpy(mem.getVU1Data()+qword*16,words,sizeof(words));
        };
        packet(0,0x1111); packet(16,0x2222);
        vu.state().vi[1]=0;vu.state().vi[2]=16;
        const float sentinel[4]={11.0f,22.0f,33.0f,44.0f};
        std::memcpy(vu.state().vf[1],sentinel,sizeof(sentinel));
        std::vector<uint32_t> packets;
        GifArbiter arbiter;
        arbiter.setProcessPacketFn([&](const uint8_t *bytes,uint32_t size) {
            require(size==32,"synthetic XGKICK packet length");
            uint32_t marker;std::memcpy(&marker,bytes+16,4);packets.push_back(marker);
            if(packets.size()==1) gate.arrive();
        });
        mem.setGifArbiter(&arbiter);
        mem.setVu1MscalCallback([&](uint32_t pc,uint32_t top,uint32_t itop) {
            vu.execute(mem.getVU1Code(),16384,mem.getVU1Data(),16384,rt.gs(),&mem,pc,top,itop,10000);
        });
        ps2x::device::Config config;config.async=true;
        config.controlWrite=[&](uint32_t address,uint32_t value){mem.writeIORegister(address,value);};
        ps2x::device::attach(mem,std::move(config));
        // Ensure every exception releases the worker before queue destruction.
        struct Cleanup { PS2Memory &mem;CancelGate &gate;~Cleanup(){gate.release();ps2x::device::detach(mem);} } cleanup{mem,gate};
        mem.m_ioRegisters[0x10009000]=0x101;
        ps2x::device::TransferSnapshot transfer;transfer.hadVif1=true;
        const uint32_t vif[4]={0x14000000u,0,0,0};
        transfer.vif1Chunks.emplace_back(reinterpret_cast<const uint8_t *>(vif),reinterpret_cast<const uint8_t *>(vif)+sizeof(vif));
        require(ps2x::device::submitTransfers(mem,std::move(transfer),false),"synthetic VIF transfer submitted");
        gate.wait();
        uint32_t saved=0;
        require(ps2x::device::pauseDmaChannel(mem,0x10009000,saved),"real VU Pause nonblocking");
        require(saved==0x101,"real VU Pause old CHCR");
        if(hardReset) {
            require(ps2x::device::submitControlWrite(mem,0x10003c10,1),"real VIF reset submitted");
            require(ps2x::device::submitVu1Reset(mem,[&]{vu.resetMicroExecution();}),"real hard VU reset submitted");
        }
        gate.release();
        ps2x::device::drainAll(mem,ps2x::device::DrainReason::Explicit);
        require(packets==(hardReset?std::vector<uint32_t>{0x1111}:std::vector<uint32_t>{0x1111,0x2222}),
                "real execution preserves emitted A; hard reset suppresses B; Pause accepts B");
        const uint8_t zeros[16]={};
        require(std::memcmp(mem.getVU1Data()+64*16,hardReset?static_cast<const void *>(zeros):static_cast<const void *>(sentinel),16)==0,
                "real hard reset suppresses later SQ; Pause preserves it");
        require(std::memcmp(vu.state().vf[1],sentinel,16)==0,"micro reset retains VU registers");
        require((syntheticLiftCalls!=0)==lifted,"requested real interpreter/lift execution path was used");
        const auto stats=ps2x::device::stats(mem);
        require(stats.submitted==stats.completed && stats.applied==stats.submitted,"real cancellation retires sequences");
    }
}

} // namespace

int main(int argc, char **argv)
{
    try
    {
        if(argc==2 && (std::string(argv[1])=="dma-cancel-interpreter" || std::string(argv[1])=="dma-cancel-lift")) {
            testRealVuDmaCancellation(std::string(argv[1])=="dma-cancel-lift");
            std::cout << "Real VU DMA cancellation tests passed\n";
            return 0;
        }
        testRegistrations();

        testSegment("notes/evidence/battle-black-agent-20261014/dispatch-observations/vu-census.tsv.1ad6eefff0c2e7e5.bin",
                    0x1ad6eefff0c2e7e5ull, 0x0800, 0x0a78);

        testSegment("notes/evidence/battle-black-agent-20261014/dispatch-observations/vu-census.tsv.1ad6eefff0c2e7e5.bin",
                    0x1ad6eefff0c2e7e5ull, 0x1000, 0x12b0);

        testSegment("notes/evidence/battle-black-agent-20261014/dispatch-observations/vu-census.tsv.1b91674603e323f2.bin",
                    0x1b91674603e323f2ull, 0x0800, 0x0c48);

        testSegment("notes/evidence/battle-black-agent-20261014/dispatch-observations/vu-census.tsv.da28291ffe0b3334.bin",
                    0xda28291ffe0b3334ull, 0x0010, 0x0950);

        std::cout << "\n>>> ALL VU1 LIFT EQUIVALENCE TESTS PASSED! <<<\n" << std::endl;
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
