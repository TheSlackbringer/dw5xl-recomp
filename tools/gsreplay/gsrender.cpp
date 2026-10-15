// DW5XL-TEST: renders frames of a PS2X_GS_RECORD recording offline, so one
// frame can be studied repeatedly with either renderer.
//
//   dw5xl_gsrender <rec> <cpu|vulkan> <outdir> <tick>[,<tick>...]
//                  [--fb fbp,fbw,width,height] [--stop-at record] [--skip a-b[,a-b...]]
//                  [--probe x,y[;x,y...] --z zbp,zpsm]
//
// The recording must start with an Initialize record (full recordings, or
// windowed ones made with PS2X_GS_RECORD_FROM, which snapshot VRAM when the
// window opens). At each listed Present tick it writes:
//   <outdir>/tick<N>.present.ppm  the backend's presentation frame
//   <outdir>/tick<N>.fb.ppm       the PSMCT32 buffer given by --fb (default:
//                                 the draw target of the last Submit)
// --stop-at writes tick<N>.fb.ppm right before that record instead (for
// following one frame draw by draw); --skip leaves out Submit records.
// --probe prints the colour (and with --z the depth) of those pixels at the stop.

#include "gs/gs_record_format.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_vulkan_backend.h"
#include "runtime/gs/ps2_gs_psmct32.h"

#include <cfenv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using namespace GSRecord;

    struct FbSpec
    {
        uint32_t fbp = 0, fbw = 0, width = 0, height = 0;
        bool set = false;
    };

    void writePpm(const std::string &path, const std::vector<uint8_t> &rgba, uint32_t width, uint32_t height,
                  uint32_t strideBytes)
    {
        std::FILE *file = std::fopen(path.c_str(), "wb");
        if (!file)
            throw std::runtime_error("cannot write " + path);
        std::fprintf(file, "P6\n%u %u\n255\n", width, height);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
                std::fwrite(&rgba[y * strideBytes + x * 4u], 1u, 3u, file);
        std::fclose(file);
        std::printf("wrote %s (%ux%u)\n", path.c_str(), width, height);
    }

    void writeFb(const std::string &path, const GSRasterBackend &backend, const FbSpec &fb)
    {
        std::vector<uint8_t> vram;
        backend.SnapshotVram(vram);
        std::vector<uint8_t> rgba(size_t(fb.width) * fb.height * 4u);
        for (uint32_t y = 0; y < fb.height; ++y)
            for (uint32_t x = 0; x < fb.width; ++x)
            {
                const uint32_t addr = GSPSMCT32::addrPSMCT32(fb.fbp * 32u, fb.fbw, x, y) & (uint32_t(vram.size()) - 1u);
                std::memcpy(&rgba[(size_t(y) * fb.width + x) * 4u], &vram[addr], 4u);
            }
        writePpm(path, rgba, fb.width, fb.height, fb.width * 4u);
    }
}

int main(int argc, char **argv)
{
    if (argc < 5)
    {
        std::fprintf(stderr, "usage: %s <rec> <cpu|vulkan> <outdir> <tick>[,<tick>...] [--fb fbp,fbw,w,h] "
                             "[--stop-at record] [--skip a-b[,a-b...]]\n", argv[0]);
        return 2;
    }
    try
    {
        const std::string recPath = argv[1], which = argv[2], outDir = argv[3];
        std::set<uint64_t> ticks;
        for (char *tok = std::strtok(argv[4], ","); tok; tok = std::strtok(nullptr, ","))
            ticks.insert(std::strtoull(tok, nullptr, 10));
        FbSpec fb;
        uint64_t stopAt = UINT64_MAX;
        std::vector<std::pair<uint64_t, uint64_t>> skips;
        std::vector<std::pair<uint32_t, uint32_t>> probes;
        uint32_t zbp = 0, zpsm = 0;
        bool haveZ = false;
        for (int i = 5; i < argc; ++i)
        {
            const std::string arg = argv[i];
            if (arg == "--fb" && i + 1 < argc)
            {
                if (std::sscanf(argv[++i], "%u,%u,%u,%u", &fb.fbp, &fb.fbw, &fb.width, &fb.height) != 4)
                    throw std::runtime_error("--fb wants fbp,fbw,width,height");
                fb.set = true;
            }
            else if (arg == "--stop-at" && i + 1 < argc)
                stopAt = std::strtoull(argv[++i], nullptr, 10);
            else if (arg == "--skip" && i + 1 < argc)
            {
                for (char *tok = std::strtok(argv[++i], ","); tok; tok = std::strtok(nullptr, ","))
                {
                    uint64_t a = 0, b = 0;
                    if (std::sscanf(tok, "%lu-%lu", &a, &b) == 2)
                        skips.emplace_back(a, b);
                    else
                        skips.emplace_back(std::strtoull(tok, nullptr, 10), std::strtoull(tok, nullptr, 10));
                }
            }
            else if (arg == "--probe" && i + 1 < argc)
            {
                for (char *tok = std::strtok(argv[++i], ";"); tok; tok = std::strtok(nullptr, ";"))
                {
                    uint32_t x = 0, y = 0;
                    if (std::sscanf(tok, "%u,%u", &x, &y) == 2)
                        probes.emplace_back(x, y);
                }
            }
            else if (arg == "--z" && i + 1 < argc)
                haveZ = std::sscanf(argv[++i], "%u,%u", &zbp, &zpsm) == 2;
            else
                throw std::runtime_error("unknown option " + arg);
        }

        std::unique_ptr<GSRasterBackend> backend;
        if (which == "cpu")
            backend = std::make_unique<GSCpuBackend>();
        else if (which == "vulkan")
            backend = std::make_unique<GSVulkanBackend>();
        else
            throw std::runtime_error("backend must be cpu or vulkan");

        std::FILE *file = std::fopen(recPath.c_str(), "rb");
        if (!file)
            throw std::runtime_error("cannot open " + recPath);
        std::setvbuf(file, nullptr, _IOFBF, 1u << 22u);
        char magic[8]{};
        uint32_t header[2]{};
        if (std::fread(magic, 1u, 8u, file) != 8u || std::memcmp(magic, kMagic, 8u) != 0 ||
            std::fread(header, sizeof(header), 1u, file) != 1u || header[0] != kVersion)
            throw std::runtime_error("not a GS recording in format v2");

        std::vector<uint8_t> vram(4u << 20u);
        std::vector<uint8_t> payload;
        bool initialized = false;
        uint64_t index = 0, lastTick = 0;
        FbSpec lastTarget;
        for (;; ++index)
        {
            uint8_t op = 0;
            uint32_t size = 0;
            if (std::fread(&op, 1u, 1u, file) != 1u || std::fread(&size, 4u, 1u, file) != 1u)
                break;
            payload.resize(size);
            if (size && std::fread(payload.data(), 1u, size, file) != size)
                break;
            Reader in{payload.data(), payload.data() + payload.size()};
            if (index == stopAt)
            {
                const FbSpec &target = fb.set ? fb : lastTarget;
                for (const auto &[x, y] : probes)
                {
                    const uint32_t c = backend->ReadVram(0u, target.fbp * 32u, target.fbw, x, y);
                    std::printf("probe %u,%u rgba=%08x", x, y, c);
                    if (haveZ)
                        std::printf(" z=%u", backend->ReadVram(zpsm, zbp * 32u, target.fbw, x, y));
                    std::printf("\n");
                }
                writeFb(outDir + "/tick" + std::to_string(lastTick) + ".at" + std::to_string(index) + ".fb.ppm",
                        *backend, fb.set ? fb : lastTarget);
                return 0;
            }
            switch (static_cast<Op>(op))
            {
            case Op::Initialize:
            {
                uint32_t vsize = 0;
                uint64_t hash = 0;
                in.raw(vsize);
                in.raw(hash);
                if (vsize != vram.size())
                    throw std::runtime_error("unexpected VRAM size in Initialize");
                std::memcpy(vram.data(), in.cur, vsize);
                backend->Initialize(vram.data(), vsize);
                initialized = true;
                std::printf("record %lu: Initialize (VRAM snapshot)\n", (unsigned long)index);
                break;
            }
            case Op::Reset:
                if (initialized) backend->Reset();
                break;
            case Op::Submit:
            {
                int32_t round = 0;
                GSPrimitiveBatch batch{};
                in.get(round);
                in.get(batch);
                bool skip = false;
                for (const auto &[a, b] : skips)
                    skip |= index >= a && index <= b;
                if (!initialized || skip)
                    break;
                const int saved = std::fegetround();
                std::fesetround(round);
                backend->Submit(batch);
                std::fesetround(saved);
                const GSFrameReg &frame = batch.state.context.frame;
                lastTarget = {frame.fbp, frame.fbw, frame.fbw * 64u,
                              uint32_t(batch.state.context.scissor.y1) + 1u, true};
                break;
            }
            case Op::LoadClut:
            {
                GSTex0Reg tex0{};
                GSTexClutReg clut{};
                in.get(tex0);
                in.get(clut);
                if (initialized) backend->LoadClut(tex0, clut);
                break;
            }
            case Op::BeginTransfer:
            {
                GSTransferCommand command{};
                in.get(command);
                if (initialized) backend->BeginTransfer(command);
                break;
            }
            case Op::UploadImage:
            {
                uint32_t bytes = 0;
                in.raw(bytes);
                if (initialized) backend->UploadImage(in.cur, bytes);
                break;
            }
            case Op::Flush:
                if (initialized) backend->Flush();
                break;
            case Op::TextureFlush:
                if (initialized) backend->TextureFlush();
                break;
            case Op::Sync:
            {
                uint8_t reason = 0;
                in.get(reason);
                if (initialized) backend->Sync(static_cast<GSSyncReason>(reason));
                break;
            }
            case Op::Present:
            {
                GSPresentationRequest request{};
                in.get(request);
                lastTick = request.vsyncTick;
                if (!initialized)
                    break;
                const PresentationFrame frame = backend->Present(request);
                if (ticks.count(request.vsyncTick))
                {
                    const std::string stem = outDir + "/tick" + std::to_string(request.vsyncTick);
                    if (frame)
                        writePpm(stem + ".present.ppm", frame.pixels, frame.width, frame.height,
                                 frame.strideBytes ? frame.strideBytes : frame.width * 4u);
                    writeFb(stem + ".fb.ppm", *backend, fb.set ? fb : lastTarget);
                    ticks.erase(request.vsyncTick);
                    if (ticks.empty())
                        return 0;
                }
                break;
            }
            case Op::ClearFramebuffer:
            {
                GSContext context{};
                uint32_t rgba = 0;
                in.get(context);
                in.get(rgba);
                if (initialized) (void)backend->ClearFramebuffer(context, rgba);
                break;
            }
            case Op::ConsumeLocalToHost:
            {
                uint32_t maxBytes = 0;
                in.raw(maxBytes);
                std::vector<uint8_t> sink(maxBytes);
                if (initialized) (void)backend->ConsumeLocalToHostBytes(sink.data(), maxBytes);
                break;
            }
            case Op::WriteVram:
            {
                uint32_t psm = 0, base = 0, bw = 0, x = 0, y = 0, value = 0;
                in.raw(psm); in.raw(base); in.raw(bw); in.raw(x); in.raw(y); in.raw(value);
                if (initialized) backend->WriteVram(psm, base, bw, x, y, value);
                break;
            }
            default: // ReadVram, SnapshotVram: no state change
                break;
            }
        }
        if (!initialized)
            std::fprintf(stderr, "recording has no Initialize record (record from the start or with PS2X_GS_RECORD_FROM)\n");
        for (uint64_t t : ticks)
            std::fprintf(stderr, "tick %lu not found\n", (unsigned long)t);
        return ticks.empty() ? 0 : 1;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 2;
    }
}
