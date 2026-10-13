// Test tool: decode dumped DW5XL IPU frames with our decoder and with FFmpeg's IPU decoder,
// convert FFmpeg's YUV with the same IPU CSC, and report PSNR. Not part of the game build.
#include "../../app/src/ipu_idec.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/log.h>
}
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

int main(int argc, char **argv)
{
    if (argc < 4) { std::fprintf(stderr, "usage: %s frame.bin width height\n", argv[0]); return 2; }
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), {});
    const int w = std::atoi(argv[2]), h = std::atoi(argv[3]);
    // FFmpeg's IPU decoder expects the frame to end right after the 00 00 01 B0 end code.
    for (size_t i = 3; i < data.size(); ++i)
        if (data[i - 3] == 0 && data[i - 2] == 0 && data[i - 1] == 1 && data[i] == 0xB0)
        {
            data.resize(i + 1);
            break;
        }
    const uint32_t mbs = static_cast<uint32_t>((w / 16) * (h / 16));

    std::vector<uint8_t> ours(mbs * 1024u);
    ipu::IdecConfig config; config.flags = data[0];
    const auto result = ipu::decodeIdec(data.data(), data.size(), 8, config, mbs, ours.data());

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_IPU);
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    ctx->width = w; ctx->height = h;
    if (avcodec_open2(ctx, codec, nullptr) < 0) { std::fprintf(stderr, "open failed\n"); return 1; }
    AVPacket *pkt = av_packet_alloc();
    std::vector<uint8_t> padded(data.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    std::copy(data.begin(), data.end(), padded.begin());
    pkt->data = padded.data(); pkt->size = static_cast<int>(data.size());
    AVFrame *frame = av_frame_alloc();
    int rs = avcodec_send_packet(ctx, pkt);
    int rr = rs < 0 ? rs : avcodec_receive_frame(ctx, frame);
    if (rr == AVERROR(EAGAIN)) { avcodec_send_packet(ctx, nullptr); rr = avcodec_receive_frame(ctx, frame); }
    if (rs < 0 || rr < 0) { char e[128]; av_strerror(rs < 0 ? rs : rr, e, sizeof(e)); std::fprintf(stderr, "ffmpeg decode failed send=%d recv=%d (%s)\n", rs, rr, e); return 1; }

    double se = 0; int maxDiff = 0; long count = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const int Y = frame->data[0][y * frame->linesize[0] + x];
            const int cb = frame->data[1][(y / 2) * frame->linesize[1] + x / 2] - 128;
            const int cr = frame->data[2][(y / 2) * frame->linesize[2] + x / 2] - 128;
            const int lum = (Y - 16) * 0x95;
            const int ref[3] = {std::clamp((lum + 0xCC * cr + 64) >> 7, 0, 255),
                                std::clamp((lum - 0x32 * cb - 0x68 * cr + 64) >> 7, 0, 255),
                                std::clamp((lum + 0x102 * cb + 64) >> 7, 0, 255)};
            const int mb = (y / 16) * (w / 16) + x / 16;
            const uint8_t *p = &ours[mb * 1024 + ((y % 16) * 16 + x % 16) * 4];
            for (int c = 0; c < 3; ++c)
            {
                const int d = p[c] - ref[c];
                se += d * d; maxDiff = std::max(maxDiff, std::abs(d)); ++count;
            }
        }
    const double mse = se / count;
    std::printf("%s mbs=%u/%u%s psnr=%.2f dB maxdiff=%d\n", argv[1], result.macroblocks, mbs,
                result.error.empty() ? "" : (" ERR " + result.error).c_str(),
                mse == 0 ? 99.0 : 10 * std::log10(255.0 * 255.0 / mse), maxDiff);
    return 0;
}
