// DW5XL-TEST: GS replay tooling test (tools/gsreplay).
//   dw5xl_gsreplay_test <dw5xl_vif1_replay> <dw5xl_gsrec> <workdir>
//
// 1. Writes the synthetic VIF1 capture (MPG + UNPACK + MSCAL/XGKICK + DIRECT).
// 2. Replays it twice in sync mode, once with --drain each, once with
//    PS2X_VU1_ASYNC=1: all GS recordings and final states must be byte-identical.
// 3. Decodes the GS stream and checks the expected sprites A, B, A, C.
// 4. Checks `dw5xl_gsrec diff` (identical -> 0; a mutated vertex -> 1 + report).
// 5. Round trip of the runtime capture hook: replaying with PS2X_VIF1_CAPTURE
//    set must write a capture whose replay gives the same GS stream.
#include "ps2_runtime.h"
#include "gs/gs_record_format.h"
#include "synthetic_vif1.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x1000;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x1004;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 1;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[] = {nullptr};

namespace
{
    std::string g_replay, g_gsrec, g_dir;

    void require(bool condition, const std::string &message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    std::string q(const std::string &path) { return "'" + path + "'"; }
    std::string at(const std::string &name) { return g_dir + "/" + name; }

    int run(const std::string &command)
    {
        std::cout << "$ " << command << std::endl;
        const int status = std::system(command.c_str());
        return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    }

    std::vector<uint8_t> slurp(const std::string &path)
    {
        std::ifstream in(path, std::ios::binary);
        require(static_cast<bool>(in), "cannot read " + path);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    void replay(const std::string &input, const std::string &name, const std::string &extraArgs = "",
                const std::string &env = "")
    {
        const int rc = run(env + " " + q(g_replay) + " " + q(input) + " --out " + q(at(name + ".rec")) +
                           " --state-out " + q(at(name + ".state")) + " " + extraArgs);
        require(rc == 0, "replay " + name + " failed with exit code " + std::to_string(rc));
    }

    struct Record
    {
        uint8_t op;
        size_t payloadOffset; // in the file
        std::vector<uint8_t> payload;
    };

    std::vector<Record> parse(const std::vector<uint8_t> &file)
    {
        require(file.size() >= 16u && std::memcmp(file.data(), GSRecord::kMagic, 8u) == 0, "bad GS record header");
        std::vector<Record> out;
        size_t pos = 16u;
        while (pos < file.size())
        {
            require(pos + 5u <= file.size(), "truncated GS record");
            uint32_t size = 0;
            std::memcpy(&size, file.data() + pos + 1u, 4u);
            require(pos + 5u + size <= file.size(), "truncated GS record payload");
            out.push_back({file[pos], pos + 5u, {file.begin() + static_cast<std::ptrdiff_t>(pos + 5u),
                                                  file.begin() + static_cast<std::ptrdiff_t>(pos + 5u + size)}});
            pos += 5u + size;
        }
        return out;
    }

    // Vertices carry raw (pre-offset) coordinates; XYOFFSET travels in the draw context.
    void checkSprite(const GSPrimitiveBatch &batch, const SyntheticVif1::Sprite &s, uint16_t offsetX, uint16_t offsetY,
                     const char *label)
    {
        const std::string what = std::string("sprite ") + label + ": ";
        require(batch.vertexCount == 2u, what + "vertex count " + std::to_string(batch.vertexCount));
        require(batch.state.prim.type == GS_PRIM_SPRITE, what + "prim type");
        const GSVertex &v0 = batch.vertices[0];
        const GSVertex &v1 = batch.vertices[1];
        auto near = [](float a, float b) { return std::fabs(a - b) < 0.01f; };
        require(batch.state.context.xyoffset.ofx == (offsetX << 4) && batch.state.context.xyoffset.ofy == (offsetY << 4),
                what + "XYOFFSET_1 in draw context");
        require(near(v0.x, s.x0) && near(v0.y, s.y0) && near(v1.x, s.x1) && near(v1.y, s.y1),
                what + "coordinates (" + std::to_string(v0.x) + "," + std::to_string(v0.y) + ")-(" +
                    std::to_string(v1.x) + "," + std::to_string(v1.y) + ")");
        // Sprites take their colour from the last vertex.
        require(v1.r == s.r && v1.g == s.g && v1.b == s.b && v1.a == s.a, what + "colour");
        require(static_cast<uint32_t>(v1.z) == s.z, what + "z " + std::to_string(v1.z));
    }
}

int main(int argc, char **argv)
{
    try
    {
        require(argc == 4, "usage: dw5xl_gsreplay_test <dw5xl_vif1_replay> <dw5xl_gsrec> <workdir>");
        g_replay = argv[1];
        g_gsrec = argv[2];
        g_dir = argv[3];
        std::filesystem::remove_all(g_dir);
        std::filesystem::create_directories(g_dir);
        const std::string clean = "env -u PS2X_VIF1_CAPTURE -u PS2X_GS_RECORD";

        // 1-2: synthetic capture, deterministic replays, sync vs async switch.
        const std::string input = at("synthetic.v1cap");
        require(SyntheticVif1::write(input), "cannot write synthetic capture");
        replay(input, "sync1", "", clean);
        replay(input, "sync2", "", clean);
        replay(input, "sync_each", "--drain each", clean);
        replay(input, "async", "--mode async", clean);
        const auto sync1 = slurp(at("sync1.rec"));
        for (const char *other : {"sync2", "sync_each", "async"})
        {
            require(sync1 == slurp(at(std::string(other) + ".rec")), std::string("GS stream differs: sync1 vs ") + other);
            require(slurp(at("sync1.state")) == slurp(at(std::string(other) + ".state")),
                    std::string("final state differs: sync1 vs ") + other);
        }

        // 3: expected content.
        const auto records = parse(sync1);
        std::vector<GSPrimitiveBatch> submits;
        size_t firstSubmitOffset = 0;
        int initializes = 0;
        for (const auto &rec : records)
        {
            if (rec.op == static_cast<uint8_t>(GSRecord::Op::Initialize))
                ++initializes;
            if (rec.op != static_cast<uint8_t>(GSRecord::Op::Submit))
                continue;
            GSRecord::Reader in{rec.payload.data(), rec.payload.data() + rec.payload.size()};
            int32_t round = 0;
            GSPrimitiveBatch batch{};
            in.get(round);
            in.get(batch);
            require(in.cur == in.end, "Submit payload has trailing bytes");
            if (submits.empty())
                firstSubmitOffset = rec.payloadOffset;
            submits.push_back(batch);
        }
        require(initializes == 1, "expected exactly one Initialize record");
        require(submits.size() == 4u, "expected 4 Submit records, got " + std::to_string(submits.size()));
        const uint16_t ox = SyntheticVif1::kXyOffsetX, oy = SyntheticVif1::kXyOffsetY;
        checkSprite(submits[0], SyntheticVif1::kSpriteA, 0, 0, "A (PATH1 XGKICK)");
        checkSprite(submits[1], SyntheticVif1::kSpriteB, 0, 0, "B (PATH2 DIRECT)");
        checkSprite(submits[2], SyntheticVif1::kSpriteA, 0, 0, "A again (second MSCAL)");
        checkSprite(submits[3], SyntheticVif1::kSpriteC, ox, oy, "C (DIRECT after XYOFFSET_1)");
        const std::vector<uint8_t> syncState = slurp(at("sync1.state"));
        Vif1Capture::StateSnapshot finalState;
        require(Vif1Capture::readState(syncState.data(), syncState.data() + syncState.size(), finalState),
                "cannot parse final state");
        require(finalState.vif1Regs[3] == 0x1234u, "VIF1_MARK register write was not replayed");
        require(finalState.vif1Regs[15] == 0x1000u && finalState.vif1Regs[18] == 0x1003u, "STROW not applied");

        // 4: the diff tool.
        require(run(q(g_gsrec) + " diff " + q(at("sync1.rec")) + " " + q(at("async.rec"))) == 0,
                "gsrec diff should report identical streams");
        std::vector<uint8_t> mutated = sync1;
        mutated[firstSubmitOffset + 4u + 1u + 16u] ^= 0x01u; // v0.r of the first Submit
        {
            std::ofstream outFile(at("mutated.rec"), std::ios::binary);
            outFile.write(reinterpret_cast<const char *>(mutated.data()), static_cast<std::streamsize>(mutated.size()));
        }
        require(run(q(g_gsrec) + " diff " + q(at("sync1.rec")) + " " + q(at("mutated.rec")) + " > " +
                    q(at("mutated.diff.txt"))) == 1,
                "gsrec diff should report a difference");
        const auto diffText = slurp(at("mutated.diff.txt"));
        const std::string text(diffText.begin(), diffText.end());
        require(text.find("batch.v0.r") != std::string::npos && text.find("RESULT: DIFFERENT") != std::string::npos,
                "diff report should name batch.v0.r");
        require(run(q(g_gsrec) + " stats " + q(at("sync1.rec"))) == 0, "gsrec stats failed");

        // 5: runtime capture hook round trip.
        replay(input, "capturing", "",
               "env -u PS2X_GS_RECORD PS2X_VIF1_CAPTURE=" + q(at("recaptured.v1cap")) +
                   " PS2X_VIF1_CAPTURE_FROM=100 PS2X_VIF1_CAPTURE_TO=101");
        replay(at("recaptured.v1cap"), "recaptured", "", clean);
        require(sync1 == slurp(at("capturing.rec")), "GS stream changed while capturing");
        require(sync1 == slurp(at("recaptured.rec")), "replay of the runtime capture differs from the original");
        require(slurp(at("sync1.state")) == slurp(at("recaptured.state")), "final state differs after recapture");

        std::cout << "GS replay tooling tests passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
