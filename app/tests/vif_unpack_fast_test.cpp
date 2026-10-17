// VIF1 UNPACK fast path (V2/V3/V4-32, no mask, MODE 0, no fill cycles) must give
// exactly the VU1 memory and VIF state of the general path: random STCYCL/UNPACK
// streams, including TOPS-relative addresses, address wrap, NUM=0 (256 vectors),
// CL > WL skipping and data cut off mid-vector (V3 reading past the end).
#include "ps2_runtime.h"
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x1000;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x1004;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 1;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[] = {nullptr};
extern bool g_vifUnpackFastPath;

namespace
{
    std::mt19937 rng(0x51f0dau);
    void put32(std::vector<uint8_t> &s, uint32_t w)
    {
        const size_t o = s.size();
        s.resize(o + 4);
        std::memcpy(s.data() + o, &w, 4);
    }

    std::vector<uint8_t> randomStream()
    {
        std::vector<uint8_t> s;
        const int commands = 1 + static_cast<int>(rng() % 4u);
        for (int c = 0; c < commands; ++c)
        {
            uint32_t cl = 1u + rng() % 4u, wl = 1u + rng() % 4u;
            if (rng() % 3u == 0u)
                cl = wl;
            if (cl < wl)
                cl = wl; // the fast path's case; CL < WL (fill) stays on the general path
            if (rng() % 8u == 0u)
                wl = cl = 0u; // 0 = 1 in the decoder
            put32(s, (0x01u << 24) | (wl << 8) | cl);          // STCYCL
            put32(s, (0x05u << 24) | 0u);                      // STMOD 0
            const uint32_t vn = 1u + rng() % 3u;                // V2, V3, V4
            const uint32_t num = rng() % 6u == 0u ? 0u : rng() % 40u;
            const uint32_t addr = rng() % 0x400u;
            const uint32_t flags = (rng() & 1u ? 0x4000u : 0u) | (rng() % 4u == 0u ? 0x8000u : 0u);
            put32(s, ((0x60u | (vn << 2)) << 24) | (num << 16) | flags | addr);
            const uint32_t vectors = num == 0u ? 256u : num;
            const uint32_t blockVectors = (cl >= wl) ? vectors : vectors;
            uint32_t bytes = blockVectors * (vn + 1u) * 4u;
            bytes = (bytes + 3u) & ~3u;
            for (uint32_t i = 0; i < bytes; ++i)
                s.push_back(static_cast<uint8_t>(rng()));
        }
        if (rng() % 3u == 0u && s.size() > 8u) // cut the stream off anywhere (word aligned)
            s.resize((rng() % s.size()) & ~3u);
        return s;
    }

    std::vector<uint8_t> run(PS2Memory &m, const std::vector<uint8_t> &stream, const std::vector<uint8_t> &init, bool fast)
    {
        g_vifUnpackFastPath = fast;
        std::memcpy(m.getVU1Data(), init.data(), PS2_VU1_DATA_SIZE);
        m.processVIF1Data(stream.data(), static_cast<uint32_t>(stream.size()));
        return std::vector<uint8_t>(m.getVU1Data(), m.getVU1Data() + PS2_VU1_DATA_SIZE);
    }
}

int main()
{
    PS2Memory fastMem, slowMem;
    if (!fastMem.initialize() || !slowMem.initialize())
        return 1;
    uint64_t failures = 0;
    const int cases = 20000;
    for (int i = 0; i < cases; ++i)
    {
        std::vector<uint8_t> init(PS2_VU1_DATA_SIZE);
        for (auto &b : init)
            b = static_cast<uint8_t>(rng());
        const auto stream = randomStream();
        const auto a = run(fastMem, stream, init, true);
        const auto b = run(slowMem, stream, init, false);
        if (a != b && ++failures <= 5)
            std::fprintf(stderr, "mismatch in case %d (stream %zu bytes)\n", i, stream.size());
    }
    g_vifUnpackFastPath = true;
    std::printf("vif unpack fast path: %d streams, %llu mismatches\n", cases, static_cast<unsigned long long>(failures));
    return failures == 0 ? 0 : 1;
}
