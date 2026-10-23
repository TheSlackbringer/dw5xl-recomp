// Unit test for lifted VU1 microprograms in DW5XL battle.
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_vu1_lift.h"
#include "ps2_runtime.h"
#include "runtime/ps2_device_queue.h"
#include "setup/vu1lift/vu1_translator.h"
#include <map>
#include <condition_variable>
#include <mutex>
#include <string>

#include <cfenv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#if defined(_WIN32)
#include <stdlib.h>
static int setenv(const char *name, const char *value, int) { return _putenv_s(name, value); }
#endif

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

// The VU1 code images the translated programs come from, rebuilt from the player's own executable exactly as the
// translator reads them (issue 036; path from DW5XL_TEST_ELF, set by CTest). Offsets and checksums only: no game data
// is stored with the test.
const std::vector<uint8_t> &discImage(const std::string &key)
{
    static const std::map<std::string, std::vector<uint8_t>> images = []
    {
        const char *elf = std::getenv("DW5XL_TEST_ELF");
        require(elf && *elf, "DW5XL_TEST_ELF must name the player's SLES_538.60 (set by CTest)");
        std::map<std::string, std::vector<uint8_t>> loaded;
        std::string error;
        require(dw5xl::vu1lift::loadVu1Images(elf, loaded, error), (std::string("VU1 images from ") + elf + ": " + error).c_str());
        return loaded;
    }();
    const auto it = images.find(key);
    require(it != images.end() && it->second.size() == 16384, ("no 16 KB VU1 image for " + key).c_str());
    return it->second;
}

// Equivalence test for a segment
void testSegment(const std::string &key, uint64_t hash, uint32_t entryPc, uint32_t expectedFinalPc)
{
    const auto &code = discImage(key);

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

    // Lifted code runs on the interpreter's scheduler: execute the same
    // program twice through the real dispatch, interpreted and lifted.
    PS2Runtime rtLift;
    require(rtLift.memory().initialize(), "rtLift.memory().initialize()");
    std::memcpy(rtLift.memory().getVU1Code(), code.data(), 16384);
    std::memcpy(rtLift.memory().getVU1Data(), dataLift, 16384);
    rtLift.vu1().state() = sLift;

    // Run interpreter
    vl::setEnabledOverride(0);
    rtInterp.vu1().execute(rtInterp.memory().getVU1Code(), 16384,
                           rtInterp.memory().getVU1Data(), 16384,
                           rtInterp.gs(), &rtInterp.memory(),
                           entryPc, 0, 0, 100000);

    std::cout << "[DEBUG] Interp PC=0x" << std::hex << sInterp.pc
              << " cycles=" << std::dec << sInterp.cycles
              << " VI[1]=" << sInterp.vi[1] << std::endl;

    // Run lifted (the registered segment is dispatched by runLifted)
    vl::setEnabledOverride(1);
    rtLift.vu1().execute(rtLift.memory().getVU1Code(), 16384,
                         rtLift.memory().getVU1Data(), 16384,
                         rtLift.gs(), &rtLift.memory(),
                         entryPc, 0, 0, 100000);
    vl::setEnabledOverride(-1);
    // A lookup miss would run the interpreter twice and compare it with itself (issue 036).
    require(rtLift.vu1().lastRunLifted(), "the translated program must have run (lookup on the rebuilt image)");
    const VU1State &sLiftOut = rtLift.vu1().state();

    std::cout << "[DEBUG] Lifted PC=0x" << std::hex << sLiftOut.pc
              << " cycles=" << std::dec << sLiftOut.cycles
              << " VI[1]=" << sLiftOut.vi[1] << std::endl;
    require(sLiftOut.cycles == sInterp.cycles, "lifted run must take the interpreter's cycle count");

    verifyEquivalence(key.c_str(), sInterp, dataInterp, sLiftOut, rtLift.memory().getVU1Data(), expectedFinalPc);
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
    // XGKICK vi1 at 0, SQ vf1 at 256, XGKICK vi2 at 264, E bit at 272 + delay slot.
    for (uint32_t pc = 0; pc <= 280; pc += 8)
    {
        vl::LiftAccess::begin(c, pc, c.st.vi);
        if (pc == 0)
            vl::LiftAccess::xgkick(c, static_cast<uint32_t>(c.st.vi[1]) & 0x3FFu);
        else if (pc == 256)
            vl::sq<15, false>(c, c.mem, 64, vl::load(c.st.vf[1]), 0u);
        else if (pc == 264)
            vl::LiftAccess::xgkick(c, static_cast<uint32_t>(c.st.vi[2]) & 0x3FFu);
        vl::LiftAccess::end(c, pc);
    }
    c.st.pc = 288;
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

// Timing benchmark (not run by CTest): repeatedly executes one captured
// program through the real dispatch, lifted and interpreted.
void benchSegment(const std::string &key, uint32_t entryPc, int iterations, bool liftedOnly)
{
    const auto &code = discImage(key);
    for (int lifted : {1, 0})
    {
        if (liftedOnly && !lifted)
            break;
        PS2Runtime rt;
        require(rt.memory().initialize(), "bench memory init");
        std::memcpy(rt.memory().getVU1Code(), code.data(), 16384);
        uint8_t *data = rt.memory().getVU1Data();
        for (size_t i = 0; i < 16384; ++i)
            data[i] = static_cast<uint8_t>((i * 101 + 37) & 0xFF);
        uint32_t *dw = reinterpret_cast<uint32_t *>(data);
        dw[0] = 1; dw[4] = 4; dw[12] = 48; dw[19] = 1; // 48 vertices
        const std::vector<uint8_t> image(data, data + 16384);
        auto &vu = rt.vu1();
        const VU1State initial = vu.state();
        vl::setEnabledOverride(lifted);
        uint64_t cycles = 0, last = 0;
        double ns = 0;
        for (int n = -1; n < iterations; ++n)
        {
            std::memcpy(data, image.data(), 16384);
            vu.state() = initial;
            vu.state().vi[8] = 0x7fff; vu.state().vi[14] = 0x1111; vu.state().vi[15] = 0x7fff;
            const auto t0 = std::chrono::steady_clock::now();
            vu.execute(rt.memory().getVU1Code(), 16384, data, 16384, rt.gs(), &rt.memory(), entryPc, 0, 0, 100000);
            if (n >= 0) ns += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
            // The interpreter's cycle counter keeps running across programs.
            if (n >= 0) cycles += vu.state().cycles - last;
            last = vu.state().cycles;
        }
        vl::setEnabledOverride(-1);
        std::cout << (lifted ? "lifted" : "interp") << ": " << ns / cycles << " ns/cycle over " << cycles << " cycles\n";
    }
}

// Real VU1 runs captured in game (PS2X_VU1_RUN_CAPTURE=<prefix>): replays each one `iterations` times, native
// and interpreted, from the captured start state; reports ns per VU cycle and checks both end identically.
void benchCaptured(const std::string &prefix, int count, int iterations, bool liftedOnly)
{
    double ns[2] = {0, 0};
    uint64_t cycles[2] = {0, 0};
    for (int capture = 0; capture < count; ++capture)
    {
        const std::string stem = prefix + "." + std::to_string(capture);
        const auto code = loadFile(stem + ".code"), image = loadFile(stem + ".data"), stateBytes = loadFile(stem + ".state");
        require(code.size() == 16384 && image.size() == 16384 && stateBytes.size() == sizeof(VU1State), "capture files");
        VU1State initial{};
        std::memcpy(&initial, stateBytes.data(), sizeof(initial));
        std::vector<uint8_t> endData[2];
        VU1State endState[2]{};
        for (int lifted : {1, 0})
        {
            if (liftedOnly && !lifted)
                break;
            PS2Runtime rt;
            require(rt.memory().initialize(), "bench memory init");
            std::memcpy(rt.memory().getVU1Code(), code.data(), 16384);
            uint8_t *data = rt.memory().getVU1Data();
            auto &vu = rt.vu1();
            vl::setEnabledOverride(lifted);
            uint64_t last = 0;
            for (int n = -1; n < iterations; ++n)
            {
                std::memcpy(data, image.data(), 16384);
                vu.state() = initial;
                const auto t0 = std::chrono::steady_clock::now();
                vu.execute(rt.memory().getVU1Code(), 16384, data, 16384, rt.gs(), &rt.memory(), initial.pc, initial.top, initial.itop, 100000);
                if (n >= 0)
                {
                    ns[lifted] += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
                    cycles[lifted] += vu.state().cycles - last;
                }
                last = vu.state().cycles;
            }
            vl::setEnabledOverride(-1);
            endState[lifted] = vu.state();
            endData[lifted].assign(data, data + 16384);
        }
        if (!liftedOnly)
        {
            const VU1State &a = endState[1], &b = endState[0];
            require(std::memcmp(a.vf, b.vf, sizeof(a.vf)) == 0 && std::memcmp(a.vi, b.vi, sizeof(a.vi)) == 0 &&
                    a.mac == b.mac && a.status == b.status && a.clip == b.clip && a.pc == b.pc && endData[0] == endData[1],
                    "captured run: native and interpreted results differ");
        }
    }
    std::cout << "captured lifted: " << ns[1] / cycles[1] << " ns/cycle over " << cycles[1] << " cycles\n";
    if (!liftedOnly)
        std::cout << "captured interp: " << ns[0] / cycles[0] << " ns/cycle over " << cycles[0] << " cycles\n";
}

// Issue 018: replays one saved run (PS2X_VU1_VERIFY_DUMP or PS2X_VU1_RUN_CAPTURE files) through the native code
// and the interpreter and compares registers, flags, VU memory and the cycle count. Returns false on a difference.
bool replayCaptured(const std::string &stem)
{
    const auto code = loadFile(stem + ".code"), image = loadFile(stem + ".data"), stateBytes = loadFile(stem + ".state");
    require(code.size() == 16384 && image.size() == 16384 && stateBytes.size() == sizeof(VU1State), "capture files");
    VU1State initial{};
    std::memcpy(&initial, stateBytes.data(), sizeof(initial));
    std::vector<uint8_t> endData[2];
    VU1State endState[2]{};
    for (int lifted : {1, 0})
    {
        PS2Runtime rt;
        require(rt.memory().initialize(), "replay memory init");
        std::memcpy(rt.memory().getVU1Code(), code.data(), 16384);
        uint8_t *data = rt.memory().getVU1Data();
        std::memcpy(data, image.data(), 16384);
        auto &vu = rt.vu1();
        vl::setEnabledOverride(lifted);
        vu.state() = initial;
        vu.execute(rt.memory().getVU1Code(), 16384, data, 16384, rt.gs(), &rt.memory(), initial.pc, initial.top, initial.itop, 100000);
        vl::setEnabledOverride(-1);
        endState[lifted] = vu.state();
        endData[lifted].assign(data, data + 16384);
    }
    const VU1State &a = endState[1], &b = endState[0];
    const bool same = std::memcmp(a.vf, b.vf, sizeof(a.vf)) == 0 && std::memcmp(a.vi, b.vi, sizeof(a.vi)) == 0 &&
                      a.mac == b.mac && a.status == b.status && a.clip == b.clip && a.pc == b.pc &&
                      endData[0] == endData[1] && a.cycles == b.cycles;
    std::cout << stem << ": native cycles " << (a.cycles - initial.cycles) << ", interpreter "
              << (b.cycles - initial.cycles) << (same ? " (identical)" : " (DIFFERENT)") << "\n";
    return same;
}

} // namespace

int main(int argc, char **argv)
{
    try
    {
        if (argc >= 3 && std::string(argv[1]) == "replaycap")
        {
            bool all = true;
            for (int i = 2; i < argc; ++i)
                all = replayCaptured(argv[i]) && all;
            return all ? 0 : 1;
        }
        if (argc >= 4 && std::string(argv[1]) == "benchcap")
        {
            benchCaptured(argv[2], std::atoi(argv[3]), argc >= 5 ? std::atoi(argv[4]) : 2000, argc >= 6);
            return 0;
        }
        if (argc >= 2 && std::string(argv[1]) == "bench")
        {
            benchSegment("da28291ffe0b3334", 0x0010,
                         argc >= 3 ? std::atoi(argv[2]) : 20000, argc >= 4);
            return 0;
        }
        if(argc==2 && (std::string(argv[1])=="dma-cancel-interpreter" || std::string(argv[1])=="dma-cancel-lift")) {
            testRealVuDmaCancellation(std::string(argv[1])=="dma-cancel-lift");
            std::cout << "Real VU DMA cancellation tests passed\n";
            return 0;
        }
        testRegistrations();

        // Each check below uses fresh runtimes; the translation lookup is cached per thread by code pointer and
        // generation, so two memories (possibly at the same address) must never share a generation (issue 023).
        {
            PS2Runtime first, second;
            require(first.memory().initialize() && second.memory().initialize(), "memories initialise");
            require(first.memory().getVU1CodeGeneration() != second.memory().getVU1CodeGeneration() &&
                        first.memory().getVU0CodeGeneration() != second.memory().getVU0CodeGeneration(),
                    "two memories must not share a VU code generation");
        }

        testSegment("1ad6eefff0c2e7e5",
                    0x1ad6eefff0c2e7e5ull, 0x0800, 0x0a78);

        testSegment("1ad6eefff0c2e7e5",
                    0x1ad6eefff0c2e7e5ull, 0x1000, 0x12b0);

        testSegment("1b91674603e323f2",
                    0x1b91674603e323f2ull, 0x0800, 0x0c48);

        testSegment("da28291ffe0b3334",
                    0xda28291ffe0b3334ull, 0x0010, 0x0950);

        // The translator's timing mirror must match the interpreter's decode,
        // or segments silently fall back to interpretation.
        require(vl::declinedSegments() == 0, "lifted segment timing differs from the interpreter decode");
        std::cout << "\n>>> ALL VU1 LIFT EQUIVALENCE TESTS PASSED! <<<\n" << std::endl;
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
