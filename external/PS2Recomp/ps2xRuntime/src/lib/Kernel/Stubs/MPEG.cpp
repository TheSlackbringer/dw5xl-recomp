#include <atomic>
std::atomic<uint32_t> g_dw5xlMpegDemux{0}, g_dw5xlMpegGetPic{0}, g_dw5xlMpegPictures{0}, g_dw5xlStrEvents[8]{}; // DW5XL-DEBUG
namespace { std::atomic<uint64_t> g_moviePictures{0}; } // decoded pictures handed to the game (F2 overlay FPS)
#include "Common.h"
#include "MPEG.h"
#include "runtime/ee_scheduler.h"

#if !defined(PS2X_HAS_FFMPEG)
#define PS2X_HAS_FFMPEG 1
#endif

#if PS2X_HAS_FFMPEG
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libswscale/swscale.h>
}
#endif

#include <algorithm>
#include <deque>
#include <memory>

#include "Syscalls/Helpers/State.h"

namespace ps2_stubs
{
    namespace
    {
        struct MpegDecodedFrame
        {
            int width = 0;
            int height = 0;
            int repeatPict = 0;
            int64_t pts90k = -1;
            std::vector<uint8_t> rgba;
        };

#if PS2X_HAS_FFMPEG
        std::string ffmpegErrorString(int err)
        {
            std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
            if (av_strerror(err, buffer.data(), buffer.size()) < 0)
            {
                return "unknown FFmpeg error";
            }
            return std::string(buffer.data());
        }

        void configureFfmpegLogLevel()
        {
            static std::once_flag s_once;
            std::call_once(s_once, []
                           {
#if AGRESSIVE_LOGS
                               av_log_set_level(AV_LOG_WARNING);
#else
                               av_log_set_level(AV_LOG_ERROR);
#endif
                           });
        }

        class MpegFfmpegDecoder
        {
        public:
            MpegFfmpegDecoder() = default;

            ~MpegFfmpegDecoder()
            {
                reset();
            }

            MpegFfmpegDecoder(const MpegFfmpegDecoder &) = delete;
            MpegFfmpegDecoder &operator=(const MpegFfmpegDecoder &) = delete;

            bool feed(const uint8_t *data, size_t size, std::deque<MpegDecodedFrame> &frames, int64_t pts90k = -1, int64_t dts90k = -1)
            {
                if (!data || size == 0)
                {
                    return true;
                }

                if (!ensureInitialized())
                {
                    return false;
                }

                static uint32_t s_feedLogCount = 0u;
                const bool shouldLog = (s_feedLogCount < 32u);
                if (shouldLog)
                    ++s_feedLogCount;

                size_t totalParsed = 0u;
                size_t totalPacketsSent = 0u;
                size_t framesBefore = frames.size();

                const uint8_t *cursor = data;
                size_t remaining = size;
                int64_t parserPts = pts90k >= 0 ? pts90k : AV_NOPTS_VALUE;
                int64_t parserDts = dts90k >= 0 ? dts90k : AV_NOPTS_VALUE;
                while (remaining > 0)
                {
                    uint8_t *packetData = nullptr;
                    int packetSize = 0;
                    const int chunk = static_cast<int>(std::min<size_t>(
                        remaining, static_cast<size_t>(std::numeric_limits<int>::max())));
                    const int used = av_parser_parse2(
                        m_parser,
                        m_codecCtx,
                        &packetData,
                        &packetSize,
                        cursor,
                        chunk,
                        parserPts,
                        parserDts,
                        0);
                    if (used < 0)
                    {
                        std::cerr << "[MPEG] parser failed: " << ffmpegErrorString(used) << std::endl;
                        return false;
                    }
                    if (used == 0 && packetSize == 0)
                    {
                        break;
                    }

                    totalParsed += static_cast<size_t>(used);
                    cursor += used;
                    remaining -= static_cast<size_t>(used);

                    if (used > 0)
                    {
                        parserPts = AV_NOPTS_VALUE;
                        parserDts = AV_NOPTS_VALUE;
                    }

                    if (packetSize > 0)
                    {
                        ++totalPacketsSent;
                        if (!sendPacket(packetData, static_cast<size_t>(packetSize), frames, m_parser->pts, m_parser->dts))
                        {
                            return false;
                        }
                    }
                }

                if (shouldLog)
                {
                    PS2_IF_AGRESSIVE_LOGS({
                        std::cerr << "[MPEG:feed] inSize=" << size
                                  << " parsed=" << totalParsed
                                  << " packets=" << totalPacketsSent
                                  << " newFrames=" << (frames.size() - framesBefore)
                                  << " totalFrames=" << frames.size()
                                  << std::endl;
                    });
                }

                return true;
            }

            bool flush(std::deque<MpegDecodedFrame> &frames)
            {
                if (!m_initialized || m_drained)
                {
                    return true;
                }

                if (m_parser)
                {
                    uint8_t *packetData = nullptr;
                    int packetSize = 0;
                    const int used = av_parser_parse2(
                        m_parser,
                        m_codecCtx,
                        &packetData,
                        &packetSize,
                        nullptr,
                        0,
                        AV_NOPTS_VALUE,
                        AV_NOPTS_VALUE,
                        0);
                    (void)used;
                    if (packetSize > 0 && !sendPacket(packetData, static_cast<size_t>(packetSize), frames, m_parser->pts, m_parser->dts))
                    {
                        return false;
                    }
                }

                const int sendRet = avcodec_send_packet(m_codecCtx, nullptr);
                if (sendRet < 0 && sendRet != AVERROR_EOF)
                {
                    std::cerr << "[MPEG] decoder flush failed: " << ffmpegErrorString(sendRet) << std::endl;
                    return false;
                }

                const bool ok = receiveFrames(frames);
                m_drained = true;
                return ok;
            }

            void reset()
            {
                if (m_swsCtx)
                {
                    sws_freeContext(m_swsCtx);
                    m_swsCtx = nullptr;
                }
                if (m_frame)
                {
                    av_frame_free(&m_frame);
                }
                if (m_packet)
                {
                    av_packet_free(&m_packet);
                }
                if (m_codecCtx)
                {
                    avcodec_free_context(&m_codecCtx);
                }
                if (m_parser)
                {
                    av_parser_close(m_parser);
                    m_parser = nullptr;
                }

                m_swsWidth = 0;
                m_swsHeight = 0;
                m_swsFormat = AV_PIX_FMT_NONE;
                m_initialized = false;
                m_drained = false;
            }

        private:
            bool ensureInitialized()
            {
                if (m_initialized)
                {
                    return true;
                }

                configureFfmpegLogLevel();

                const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
                if (!codec)
                {
                    std::cerr << "[MPEG] FFmpeg MPEG-2 decoder not found." << std::endl;
                    return false;
                }

                m_parser = av_parser_init(AV_CODEC_ID_MPEG2VIDEO);
                if (!m_parser)
                {
                    std::cerr << "[MPEG] FFmpeg MPEG-video parser not found." << std::endl;
                    return false;
                }

                m_codecCtx = avcodec_alloc_context3(codec);
                m_frame = av_frame_alloc();
                m_packet = av_packet_alloc();
                if (!m_codecCtx || !m_frame || !m_packet)
                {
                    std::cerr << "[MPEG] failed to allocate FFmpeg decoder state." << std::endl;
                    reset();
                    return false;
                }

                m_codecCtx->thread_count = 1;
                m_codecCtx->pkt_timebase = AVRational{1, 90000};
                // feedElementaryStream() does not create the decoder until a valid
                // MPEG sequence header has been found.  Dropping non-key pictures
                // here therefore throws away real presentation frames and makes
                // movies finish early once the EE is fast enough to drain them.
                m_codecCtx->skip_frame = AVDISCARD_DEFAULT;
                m_codecCtx->err_recognition = 0;
                const int ret = avcodec_open2(m_codecCtx, codec, nullptr);
                if (ret < 0)
                {
                    std::cerr << "[MPEG] failed to open MPEG decoder: " << ffmpegErrorString(ret) << std::endl;
                    reset();
                    return false;
                }

                m_initialized = true;
                m_drained = false;
                return true;
            }

            bool sendPacket(const uint8_t *data,
                            size_t size,
                            std::deque<MpegDecodedFrame> &frames,
                            int64_t pts = AV_NOPTS_VALUE,
                            int64_t dts = AV_NOPTS_VALUE)
            {
                if (!data || size == 0)
                {
                    return true;
                }

                av_packet_unref(m_packet);
                const int allocRet = av_new_packet(m_packet, static_cast<int>(size));
                if (allocRet < 0)
                {
                    std::cerr << "[MPEG] failed to allocate packet: " << ffmpegErrorString(allocRet) << std::endl;
                    return false;
                }
                std::memcpy(m_packet->data, data, size);
                m_packet->pts = pts;
                m_packet->dts = dts;

                int ret = avcodec_send_packet(m_codecCtx, m_packet);
                if (ret == AVERROR(EAGAIN))
                {
                    if (!receiveFrames(frames))
                    {
                        av_packet_unref(m_packet);
                        return false;
                    }
                    ret = avcodec_send_packet(m_codecCtx, m_packet);
                }
                av_packet_unref(m_packet);
                if (ret < 0 && ret != AVERROR(EAGAIN))
                {
                    static uint32_t s_rejectedPacketLogCount = 0u;
                    if (s_rejectedPacketLogCount < 32u)
                    {
                        std::cerr << "[MPEG] decoder rejected packet, dropping: "
                                  << ffmpegErrorString(ret) << std::endl;
                        ++s_rejectedPacketLogCount;
                    }
                    return true;
                }

                return receiveFrames(frames);
            }

            bool receiveFrames(std::deque<MpegDecodedFrame> &frames)
            {
                while (true)
                {
                    const int ret = avcodec_receive_frame(m_codecCtx, m_frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    {
                        return true;
                    }
                    if (ret < 0)
                    {
                        static uint32_t s_receiveErrorLogCount = 0u;
                        if (s_receiveErrorLogCount < 32u)
                        {
                            std::cerr << "[MPEG] decoder receive failed, dropping: "
                                      << ffmpegErrorString(ret) << std::endl;
                            ++s_receiveErrorLogCount;
                        }
                        return true;
                    }

                    if (!convertFrame(frames))
                    {
                        av_frame_unref(m_frame);
                        return false;
                    }
                    av_frame_unref(m_frame);
                }
            }

            bool convertFrame(std::deque<MpegDecodedFrame> &frames)
            {
                const int width = m_frame->width;
                const int height = m_frame->height;
                const AVPixelFormat srcFormat = static_cast<AVPixelFormat>(m_frame->format);
                if (width <= 0 || height <= 0 || srcFormat == AV_PIX_FMT_NONE)
                {
                    return false;
                }

                if (!m_swsCtx ||
                    m_swsWidth != width ||
                    m_swsHeight != height ||
                    m_swsFormat != srcFormat)
                {
                    if (m_swsCtx)
                    {
                        sws_freeContext(m_swsCtx);
                        m_swsCtx = nullptr;
                    }
                    m_swsCtx = sws_getContext(
                        width,
                        height,
                        srcFormat,
                        width,
                        height,
                        AV_PIX_FMT_RGBA,
                        SWS_BILINEAR,
                        nullptr,
                        nullptr,
                        nullptr);
                    if (!m_swsCtx)
                    {
                        std::cerr << "[MPEG] failed to create FFmpeg scaler." << std::endl;
                        return false;
                    }
                    m_swsWidth = width;
                    m_swsHeight = height;
                    m_swsFormat = srcFormat;
                }

                MpegDecodedFrame decoded;
                decoded.width = width;
                decoded.height = height;
                decoded.repeatPict = std::max(0, m_frame->repeat_pict);
                decoded.pts90k = m_frame->best_effort_timestamp != AV_NOPTS_VALUE
                                     ? m_frame->best_effort_timestamp
                                     : -1;
                decoded.rgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);

                uint8_t *dstData[4] = {decoded.rgba.data(), nullptr, nullptr, nullptr};
                int dstLinesize[4] = {width * 4, 0, 0, 0};
                const int scaledRows = sws_scale(
                    m_swsCtx,
                    m_frame->data,
                    m_frame->linesize,
                    0,
                    height,
                    dstData,
                    dstLinesize);
                if (scaledRows <= 0)
                {
                    std::cerr << "[MPEG] FFmpeg scaler produced no rows." << std::endl;
                    return false;
                }

                frames.push_back(std::move(decoded));
                return true;
            }

            AVCodecParserContext *m_parser = nullptr;
            AVCodecContext *m_codecCtx = nullptr;
            AVFrame *m_frame = nullptr;
            AVPacket *m_packet = nullptr;
            SwsContext *m_swsCtx = nullptr;
            int m_swsWidth = 0;
            int m_swsHeight = 0;
            AVPixelFormat m_swsFormat = AV_PIX_FMT_NONE;
            bool m_initialized = false;
            bool m_drained = false;
        };
#else
        // TODO
        class MpegFfmpegDecoder
        {
        public:
            bool feed(const uint8_t *, size_t, std::deque<MpegDecodedFrame> &, int64_t = -1, int64_t = -1)
            {
                static bool s_warnedNoFfmpeg = false;
                if (!s_warnedNoFfmpeg)
                {
                    std::cerr << "[MPEG] runtime built without FFmpeg; MPEG video decode is disabled." << std::endl;
                    s_warnedNoFfmpeg = true;
                }
                return false;
            }

            bool flush(std::deque<MpegDecodedFrame> &)
            {
                return true;
            }

            void reset() {}
        };
#endif

        struct MpegRegisteredCallback
        {
            uint32_t type = 0u;
            uint32_t streamId = 0u;
            uint32_t func = 0u;
            uint32_t data = 0u;
            uint32_t handle = 0u;
            bool stream = false;
        };

        constexpr uint64_t kPictureClockOne = 1ull << 32u;
        // NTSC-style fields at ~59.94 Hz to keep MPEG timing yet (29.97 fps).
        constexpr uint64_t kDefaultPictureIntervalQ32 = 2ull * kPictureClockOne;
        constexpr size_t kMpegTimingScanLimit = 4096u;
        constexpr size_t kMaxDecodedPicturesAhead = 8u;

        struct MpegDemuxTransaction;

        struct MpegPlaybackState
        {
            // Some SDK getters are small guest routines which read this arena
            // directly rather than calling a replacement. Keep their status
            // word coherent with the native decoder for each created handle.
            uint8_t *guestRdram = nullptr;
            uint32_t sdkWorkAddr = 0u;
            uint64_t callbackGeneration = 0u;
            uint32_t picturesServed = 0u;
            uint32_t width = 320u;
            uint32_t height = 240u;
            uint32_t decodeMode = 0u;
            uint32_t imageBufferAddr = 0u;
            bool sawInput = false;
            bool sawSequenceEnd = false;
            // The last video ES data handed to the decoder ended with a
            // sequence_end_code and the decoder was drained. The retail
            // sceMpegGetPicture flushes its reference picture and sets
            // sys->isEnd (work+0) when _sceMpegNextHeader returns 0x1B7.
            bool sequenceEndDecoded = false;
            bool acceptedProgramEnd = false;
            bool streamEnded = false;
            bool decoderFailed = false;
            uint64_t cdStreamGeneration = 0u;
            bool waitingForVideoSequenceHeader = true;
            std::vector<uint8_t> videoSequenceSyncBuffer;
            std::deque<MpegDecodedFrame> decodedFrames;
            std::unique_ptr<MpegFfmpegDecoder> decoder;
            uint8_t frameRateCode = 0u;
            uint8_t frameRateExtensionN = 0u;
            uint8_t frameRateExtensionD = 0u;
            bool hasFrameRateExtension = false;
            std::vector<uint8_t> videoTimingScanBuffer;
            uint64_t pictureIntervalQ32 = kDefaultPictureIntervalQ32;
            uint64_t nextPictureTickQ32 = std::numeric_limits<uint64_t>::max();
            uint64_t presentationEndTickQ32 = std::numeric_limits<uint64_t>::max();
            int64_t firstPresentedPts90k = -1;
            uint64_t ptsPresentationBaseTickQ32 = 0u;
            bool presentationPalMode = false;
            std::shared_ptr<MpegDemuxTransaction> demuxTransaction;
            uint32_t retryPacketAddr = 0u;
            size_t retryPacketBytes = 0u;
            size_t retryEvent = 0u;
            size_t retryCallback = 0u;
            bool retryPacket = false;
            uint64_t lastOutputServiceTick = std::numeric_limits<uint64_t>::max();
            uint64_t lastInputServiceTick = std::numeric_limits<uint64_t>::max();
            uint64_t acceptedVideoPayloadBytes = 0u;
        };

        struct MpegStreamCallbackEvent
        {
            uint32_t mpegAddr = 0u;
            uint64_t callbackGeneration = 0u;
            uint32_t streamType = 0u;
            uint32_t dataAddr = 0u;
            uint32_t len = 0u;
            uint64_t pts = 0xFFFFFFFFFFFFFFFFull;
            uint64_t dts = 0xFFFFFFFFFFFFFFFFull;
            std::vector<MpegRegisteredCallback> callbacks;
        };

        struct MpegDemuxTransaction
        {
            uint32_t mpegAddr = 0u;
            uint32_t inputAddr = 0u;
            uint32_t ringBase = 0u;
            uint32_t ringSize = 0u;
            size_t offeredBytes = 0u;
            uint64_t generation = 0u;
            uint64_t waitToken = 0u;
            uint8_t *rdram = nullptr;
            PS2Runtime *runtime = nullptr;
            R5900Context caller{};
            std::vector<uint8_t> input;
            size_t committed = 0u;
            size_t packetEnd = 0u;
            size_t payloadStart = 0u;
            bool videoPacket = false;
            int64_t pts90k = -1;
            int64_t dts90k = -1;
            std::vector<MpegStreamCallbackEvent> events;
            size_t eventIndex = 0u;
            size_t callbackIndex = 0u;
            bool packetPending = false;
            bool deferredReturn = false;
            bool finished = false;
        };

        struct MpegStubState
        {
            bool initialized = false;
            uint32_t nextCallbackHandle = 1u;
            uint64_t cdStreamGeneration = 0u;
            uint64_t cdStreamBytesProduced = 0u;
            uint64_t cdStreamBytesDemuxed = 0u;
            bool cdStreamEofPending = false;
            bool currentCdStreamEofSeen = false;
            uint32_t feedEsTraceCount = 0u;
            uint32_t demuxPssTraceCount = 0u;
            uint32_t demuxRingTraceCount = 0u;
            uint32_t getPictureWaitTraceCount = 0u;
            uint32_t pictureTraceCount = 0u;
            uint32_t isEndTraceCount = 0u;
            std::unordered_map<uint32_t, std::vector<MpegRegisteredCallback>> callbacksByMpeg;
            std::unordered_map<uint32_t, MpegPlaybackState> playbackByMpeg;
        };

        std::mutex g_mpeg_stub_mutex;
        constexpr uint32_t kMpegPictureWaitType = 1u;
        MpegStubState g_mpeg_stub_state;
        uint64_t g_nextPlaybackGeneration = 1u;

        bool mpegTraceEnabled()
        {
            static const bool enabled = []
            {
                const char *value = std::getenv("PS2X_MPEG_TRACE");
                return value && value[0] != '\0' && std::strcmp(value, "0") != 0;
            }();
            return enabled;
        }

        bool mpegTraceSample(uint64_t index)
        {
            const uint64_t count = index + 1u;
            return mpegTraceEnabled() && (count <= 16u || (count & (count - 1u)) == 0u);
        }

        bool decoderComplete(const MpegPlaybackState &playback)
        {
            const bool producerEnded = playback.cdStreamGeneration == g_mpeg_stub_state.cdStreamGeneration &&
                (playback.acceptedProgramEnd || g_mpeg_stub_state.currentCdStreamEofSeen);
            if (!playback.decodedFrames.empty())
                return false;
            // ezmovie-style players stop demultiplexing before the trailing
            // program_end_code (the retail demuxer never consumes 0x1B9) and
            // wait for this word, so a decoded sequence_end_code completes too.
            return playback.sequenceEndDecoded ||
                (producerEnded && (playback.streamEnded || (playback.decoderFailed && playback.sawInput)));
        }

        void publishSdkDecoderStatus(const MpegPlaybackState &playback)
        {
            if (!playback.guestRdram || playback.sdkWorkAddr == 0u)
                return;
            if (uint8_t *work = getMemPtr(playback.guestRdram, playback.sdkWorkAddr))
            {
                const uint32_t complete = decoderComplete(playback) ? 1u : 0u;
                std::memcpy(work, &complete, sizeof(complete));
                // The adjacent SDK getter returns whether this count is zero.
                // Native decoding owns its reference/output queue, so guest
                // getters must observe the same emptiness as the native API.
                const uint32_t references = static_cast<uint32_t>(playback.decodedFrames.size());
                std::memcpy(work + 4u, &references, sizeof(references));
            }
        }

        // TODO this resolution should follow runtime resolution
        constexpr uint32_t kStubMovieWidth = 320u;
        constexpr uint32_t kStubMovieHeight = 240u;
        constexpr uint32_t kMpegStrM2V = 0u;
        // sceMpegStrType: M2V=0, IPU=1, PCM=2, ADPCM=3, DATA=4. These were 1/2, so a game registering
        // its audio callback as ADPCM (DW5XL: type 3) never received any audio.
        constexpr uint32_t kMpegStrPCM = 2u;
        constexpr uint32_t kMpegStrADPCM = 3u;
        constexpr uint8_t kMpegPackHeader = 0xBAu;
        constexpr uint8_t kMpegSystemHeader = 0xBBu;
        constexpr uint8_t kMpegProgramEnd = 0xB9u;
        constexpr uint8_t kMpegSequenceEnd = 0xB7u;
        constexpr uint8_t kMpegPrivateStream1 = 0xBDu;
        constexpr size_t kStartCodeNotFound = std::numeric_limits<size_t>::max();
        constexpr uint32_t kMpegCallbackDataSize = 0x20u;

        uint64_t mpegPictureIntervalQ32(uint8_t frameRateCode, uint8_t frameRateExtensionN = 0u, uint8_t frameRateExtensionD = 0u, bool pal = false)
        {
            uint64_t frameRateNumerator = 0u;
            uint64_t frameRateDenominator = 1u;
            switch (frameRateCode)
            {
            case 1u:
                frameRateNumerator = 24000u;
                frameRateDenominator = 1001u;
                break;
            case 2u:
                frameRateNumerator = 24u;
                break;
            case 3u:
                frameRateNumerator = 25u;
                break;
            case 4u:
                frameRateNumerator = 30000u;
                frameRateDenominator = 1001u;
                break;
            case 5u:
                frameRateNumerator = 30u;
                break;
            case 6u:
                frameRateNumerator = 50u;
                break;
            case 7u:
                frameRateNumerator = 60000u;
                frameRateDenominator = 1001u;
                break;
            case 8u:
                frameRateNumerator = 60u;
                break;
            default:
                return 0u;
            }

            const uint64_t extensionNumerator = static_cast<uint64_t>(frameRateExtensionN) + 1u;
            const uint64_t extensionDenominator = static_cast<uint64_t>(frameRateExtensionD) + 1u;
            const uint64_t displayNumerator = pal ? 50u : 60000u;
            const uint64_t displayDenominator = pal ? 1u : 1001u;
            const uint64_t denominator = displayDenominator * frameRateNumerator * extensionNumerator;
            const uint64_t numerator = (displayNumerator * frameRateDenominator * extensionDenominator) << 32u;
            return std::max(kPictureClockOne, (numerator + denominator / 2u) / denominator);
        }

        uint32_t readMpegBits(const uint8_t *data, size_t bitOffset, uint32_t bitCount)
        {
            uint32_t value = 0u;
            for (uint32_t bit = 0u; bit < bitCount; ++bit)
            {
                const size_t absoluteBit = bitOffset + bit;
                const uint8_t source = data[absoluteBit >> 3u];
                value = (value << 1u) | ((source >> (7u - static_cast<uint32_t>(absoluteBit & 7u))) & 1u);
            }
            return value;
        }

        void updateMpegPictureTiming(MpegPlaybackState &playback, const uint8_t *data, size_t size)
        {
            if (!data || size == 0u)
            {
                return;
            }

            playback.videoTimingScanBuffer.insert(playback.videoTimingScanBuffer.end(), data, data + size);
            if (playback.videoTimingScanBuffer.size() > kMpegTimingScanLimit)
            {
                const size_t discard = playback.videoTimingScanBuffer.size() - kMpegTimingScanLimit;
                playback.videoTimingScanBuffer.erase(playback.videoTimingScanBuffer.begin(), playback.videoTimingScanBuffer.begin() + static_cast<std::ptrdiff_t>(discard));
            }

            const std::vector<uint8_t> &buffer = playback.videoTimingScanBuffer;
            size_t lastSequenceHeader = kStartCodeNotFound;
            for (size_t i = 0u; i + 7u < buffer.size(); ++i)
            {
                if (buffer[i + 0u] == 0x00u &&
                    buffer[i + 1u] == 0x00u &&
                    buffer[i + 2u] == 0x01u &&
                    buffer[i + 3u] == 0xB3u)
                {
                    const uint8_t frameRateCode = buffer[i + 7u] & 0x0Fu;
                    if (mpegPictureIntervalQ32(frameRateCode) != 0u)
                    {
                        lastSequenceHeader = i;
                    }
                }
            }

            if (lastSequenceHeader == kStartCodeNotFound)
            {
                return;
            }

            playback.frameRateCode = buffer[lastSequenceHeader + 7u] & 0x0Fu;
            playback.frameRateExtensionN = 0u;
            playback.frameRateExtensionD = 0u;
            playback.hasFrameRateExtension = false;
            playback.pictureIntervalQ32 = mpegPictureIntervalQ32(playback.frameRateCode);

            for (size_t i = lastSequenceHeader + 8u; i + 9u < buffer.size(); ++i)
            {
                if (buffer[i + 0u] != 0x00u ||
                    buffer[i + 1u] != 0x00u ||
                    buffer[i + 2u] != 0x01u)
                {
                    continue;
                }

                if (buffer[i + 3u] == 0xB3u)
                {
                    break;
                }
                if (buffer[i + 3u] != 0xB5u)
                {
                    continue;
                }

                const uint8_t *extension = buffer.data() + i + 4u;
                if (readMpegBits(extension, 0u, 4u) != 1u)
                {
                    continue;
                }

                playback.frameRateExtensionN = static_cast<uint8_t>(readMpegBits(extension, 41u, 2u));
                playback.frameRateExtensionD = static_cast<uint8_t>(readMpegBits(extension, 43u, 5u));
                playback.hasFrameRateExtension = true;
                playback.pictureIntervalQ32 = mpegPictureIntervalQ32(
                    playback.frameRateCode,
                    playback.frameRateExtensionN,
                    playback.frameRateExtensionD);
                break;
            }

            if (playback.pictureIntervalQ32 == 0u)
            {
                playback.pictureIntervalQ32 = kDefaultPictureIntervalQ32;
            }
        }

        uint64_t decodedFrameIntervalQ32(const MpegPlaybackState &playback,
                                         const MpegDecodedFrame &frame, bool pal)
        {
            // Recompute from physical MPEG frame rate at presentation time so
            // a CRT change cannot leave a cached NTSC tick interval in use.
            uint64_t base = mpegPictureIntervalQ32(playback.frameRateCode,
                                                  playback.frameRateExtensionN,
                                                  playback.frameRateExtensionD, pal);
            if (base == 0u)
                base = pal ? (kDefaultPictureIntervalQ32 * 1001u + 600u) / 1200u
                           : kDefaultPictureIntervalQ32;
            const uint64_t fields = static_cast<uint64_t>(2 + std::max(0, frame.repeatPict));
            return std::max(kPictureClockOne, (base * fields + 1u) / 2u);
        }

        constexpr uint64_t kMpegPtsWrap = 1ull << 33u;
        constexpr uint64_t kMpegPtsHalfWrap = 1ull << 32u;

        int64_t mpegPtsDelta90k(int64_t fromPts, int64_t toPts)
        {
            if (fromPts < 0 || toPts < 0)
            {
                return 0;
            }

            uint64_t from = static_cast<uint64_t>(fromPts) & (kMpegPtsWrap - 1u);
            uint64_t to = static_cast<uint64_t>(toPts) & (kMpegPtsWrap - 1u);
            uint64_t delta = (to - from) & (kMpegPtsWrap - 1u);
            if (delta >= kMpegPtsHalfWrap)
            {
                return -static_cast<int64_t>(kMpegPtsWrap - delta);
            }
            return static_cast<int64_t>(delta);
        }

        uint64_t mpegPtsDeltaToVSyncQ32(uint64_t delta90k, bool pal)
        {
            // 90 kHz MPEG clock -> active field clock.
            // PAL fields=pts/1800; NTSC fields=pts*2/3003.
            const uint64_t kPtsDivisor = pal ? 1800u : 3003u;
            const uint64_t multiplier = pal ? 1u : 2u;
            const uint64_t whole = delta90k / kPtsDivisor;
            const uint64_t remainder = delta90k % kPtsDivisor;
            const uint64_t wholeQ32 = whole * multiplier * kPictureClockOne;
            const uint64_t remainderQ32 = ((remainder * multiplier * kPictureClockOne) + kPtsDivisor / 2u) / kPtsDivisor;
            return wholeQ32 + remainderQ32;
        }

        uint64_t presentationTickForFrame(MpegPlaybackState &playback, const MpegDecodedFrame &frame, uint64_t currentTickQ32, bool pal)
        {
            if (playback.presentationPalMode != pal)
            {
                // Re-anchor at the current picture when changing field units.
                playback.presentationPalMode = pal;
                playback.firstPresentedPts90k = -1;
                playback.nextPictureTickQ32 = currentTickQ32;
                playback.presentationEndTickQ32 = currentTickQ32;
            }
            if (frame.pts90k < 0)
            {
                if (playback.nextPictureTickQ32 == std::numeric_limits<uint64_t>::max())
                {
                    playback.nextPictureTickQ32 = currentTickQ32;
                }
                return playback.nextPictureTickQ32;
            }

            if (playback.firstPresentedPts90k < 0)
            {
                playback.firstPresentedPts90k = frame.pts90k;
                playback.ptsPresentationBaseTickQ32 = currentTickQ32;
                return currentTickQ32;
            }

            const int64_t delta = mpegPtsDelta90k(playback.firstPresentedPts90k, frame.pts90k);
            if (delta >= 0)
            {
                return playback.ptsPresentationBaseTickQ32 + mpegPtsDeltaToVSyncQ32(static_cast<uint64_t>(delta), pal);
            }

            const uint64_t backwards = mpegPtsDeltaToVSyncQ32(static_cast<uint64_t>(-delta), pal);
            return playback.ptsPresentationBaseTickQ32 > backwards
                       ? playback.ptsPresentationBaseTickQ32 - backwards
                       : 0u;
        }

        uint32_t align16(uint32_t value)
        {
            return (value + 15u) & ~15u;
        }

        uint32_t readStackArg(uint8_t *rdram, R5900Context *ctx, uint32_t offset)
        {
            if (!rdram || !ctx)
            {
                return 0u;
            }
            return FAST_READ32(getRegU32(ctx, 29) + offset);
        }

        uint32_t readAbiArg4(uint8_t *rdram, R5900Context *ctx)
        {
            const uint32_t regArg = getRegU32(ctx, 8);
            if (regArg != 0u)
            {
                return regArg;
            }
            return readStackArg(rdram, ctx, 0x10u);
        }

        MpegPlaybackState &getPlaybackState(uint32_t mpegAddr)
        {
            MpegPlaybackState &playback = g_mpeg_stub_state.playbackByMpeg[mpegAddr];
            if (playback.callbackGeneration == 0u)
                playback.callbackGeneration = g_nextPlaybackGeneration++;
            return playback;
        }

        MpegPlaybackState makeFreshPlaybackState()
        {
            MpegPlaybackState playback{};
            // The counter persists across SDK/global resets and handle reuse.
            // A CD generation alone cannot distinguish reset/recreate within
            // the same stream. All state creation occurs under the MPEG lock.
            playback.callbackGeneration = g_nextPlaybackGeneration++;
            playback.cdStreamGeneration = g_mpeg_stub_state.cdStreamGeneration;
            return playback;
        }

        MpegPlaybackState makeFreshPlaybackStatePreservingConfig(const MpegPlaybackState &oldPlayback)
        {
            MpegPlaybackState playback = makeFreshPlaybackState();
            playback.guestRdram = oldPlayback.guestRdram;
            playback.sdkWorkAddr = oldPlayback.sdkWorkAddr;
            playback.decodeMode = oldPlayback.decodeMode;
            playback.imageBufferAddr = oldPlayback.imageBufferAddr;
            playback.width = oldPlayback.width;
            playback.height = oldPlayback.height;
            return playback;
        }

        uint16_t readBe16(const uint8_t *p)
        {
            return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8u) | static_cast<uint16_t>(p[1]));
        }

        bool isVideoStreamId(uint8_t streamId)
        {
            return streamId >= 0xE0u && streamId <= 0xEFu;
        }

        bool isAudioStreamId(uint8_t streamId)
        {
            return streamId == kMpegPrivateStream1 || (streamId >= 0xC0u && streamId <= 0xDFu);
        }

        bool isLengthPrefixedHeader(uint8_t streamId)
        {
            switch (streamId)
            {
            case kMpegSystemHeader:
            case 0xBCu: // program_stream_map
            case 0xBEu: // padding_stream
            case 0xBFu: // private_stream_2
            case 0xF0u: // ECM
            case 0xF1u: // EMM
            case 0xF2u: // DSMCC
            case 0xF8u: // ITU-T H.222.1 type E
            case 0xFFu: // program_stream_directory
                return true;
            default:
                return false;
            }
        }

        size_t findStartCode(const std::vector<uint8_t> &buffer, size_t from)
        {
            if (buffer.size() < 4 || from >= buffer.size() - 3u)
            {
                return kStartCodeNotFound;
            }

            for (size_t i = from; i + 3u < buffer.size(); ++i)
            {
                if (buffer[i] == 0x00u && buffer[i + 1u] == 0x00u && buffer[i + 2u] == 0x01u)
                {
                    return i;
                }
            }
            return kStartCodeNotFound;
        }

        bool containsMpegSequenceEnd(const uint8_t *data, size_t size)
        {
            if (!data || size < 4)
            {
                return false;
            }

            for (size_t i = 0; i + 3u < size; ++i)
            {
                if (data[i] == 0x00u &&
                    data[i + 1u] == 0x00u &&
                    data[i + 2u] == 0x01u &&
                    data[i + 3u] == kMpegSequenceEnd)
                {
                    return true;
                }
            }
            return false;
        }

        // True when the last start code in this ES chunk is a sequence_end_code,
        // i.e. no picture or new sequence follows it in the data seen so far.
        bool endsWithMpegSequenceEnd(const uint8_t *data, size_t size)
        {
            if (!data || size < 4u)
            {
                return false;
            }

            for (size_t i = size - 4u + 1u; i-- > 0u;)
            {
                if (data[i] == 0x00u && data[i + 1u] == 0x00u && data[i + 2u] == 0x01u)
                {
                    return data[i + 3u] == kMpegSequenceEnd;
                }
            }
            return false;
        }

        size_t findMpegSequenceHeader(const uint8_t *data, size_t size)
        {
            if (!data || size < 8u)
            {
                return kStartCodeNotFound;
            }

            for (size_t i = 0; i + 7u < size; ++i)
            {
                if (data[i] == 0x00u &&
                    data[i + 1u] == 0x00u &&
                    data[i + 2u] == 0x01u &&
                    data[i + 3u] == 0xB3u)
                {
                    const uint32_t width = (static_cast<uint32_t>(data[i + 4u]) << 4u) |
                                           (static_cast<uint32_t>(data[i + 5u]) >> 4u);
                    const uint32_t height = ((static_cast<uint32_t>(data[i + 5u]) & 0x0Fu) << 8u) |
                                            static_cast<uint32_t>(data[i + 6u]);
                    const uint8_t frameRateCode = data[i + 7u] & 0x0Fu;
                    if (width != 0u && height != 0u && width <= 4096u && height <= 4096u &&
                        mpegPictureIntervalQ32(frameRateCode) != 0u)
                    {
                        return i;
                    }
                }
            }
            return kStartCodeNotFound;
        }

        struct MpegPesHeader
        {
            size_t payloadOffset = 0u;
            int64_t pts90k = -1;
            int64_t dts90k = -1;
        };

        int64_t decodePesTimestamp90k(const uint8_t *p, size_t remaining)
        {
            if (!p || remaining < 5u)
            {
                return -1;
            }

            const uint64_t value =
                (static_cast<uint64_t>((p[0] >> 1u) & 0x07u) << 30u) |
                (static_cast<uint64_t>(p[1]) << 22u) |
                (static_cast<uint64_t>((p[2] >> 1u) & 0x7Fu) << 15u) |
                (static_cast<uint64_t>(p[3]) << 7u) |
                static_cast<uint64_t>((p[4] >> 1u) & 0x7Fu);
            return static_cast<int64_t>(value & (kMpegPtsWrap - 1u));
        }

        MpegPesHeader parsePesHeader(const uint8_t *packet, size_t packetSize)
        {
            MpegPesHeader result{};
            result.payloadOffset = packetSize;
            if (!packet || packetSize <= 6u)
            {
                return result;
            }

            size_t pos = 6u;
            if (packetSize >= 9u && (packet[pos] & 0xC0u) == 0x80u)
            {
                const uint8_t ptsDtsFlags = packet[pos + 1u] & 0xC0u;
                const size_t headerDataLength = static_cast<size_t>(packet[pos + 2u]);
                const size_t optionalStart = 9u;
                const size_t optionalEnd = std::min(packetSize, optionalStart + headerDataLength);
                if ((ptsDtsFlags == 0x80u || ptsDtsFlags == 0xC0u) && optionalEnd >= optionalStart + 5u)
                {
                    result.pts90k = decodePesTimestamp90k(packet + optionalStart, optionalEnd - optionalStart);
                }
                if (ptsDtsFlags == 0xC0u && optionalEnd >= optionalStart + 10u)
                {
                    result.dts90k = decodePesTimestamp90k(packet + optionalStart + 5u, optionalEnd - optionalStart - 5u);
                }
                result.payloadOffset = optionalEnd;
                return result;
            }

            // MPEG-1 PES.
            while (pos < packetSize && packet[pos] == 0xFFu)
            {
                ++pos;
            }
            if (pos + 1u < packetSize && (packet[pos] & 0xC0u) == 0x40u)
            {
                pos += 2u;
            }
            if (pos >= packetSize)
            {
                return result;
            }

            const uint8_t marker = packet[pos] & 0xF0u;
            if (marker == 0x20u && pos + 5u <= packetSize)
            {
                result.pts90k = decodePesTimestamp90k(packet + pos, packetSize - pos);
                pos += 5u;
            }
            else if (marker == 0x30u && pos + 10u <= packetSize)
            {
                result.pts90k = decodePesTimestamp90k(packet + pos, packetSize - pos);
                result.dts90k = decodePesTimestamp90k(packet + pos + 5u, packetSize - pos - 5u);
                pos += 10u;
            }
            else if (packet[pos] == 0x0Fu)
            {
                ++pos;
            }

            result.payloadOffset = std::min(packetSize, pos);
            return result;
        }

        void flushDecoderIfEnded(MpegPlaybackState &playback)
        {
            if (playback.streamEnded && playback.decoder)
            {
                playback.decoder->flush(playback.decodedFrames);
            }
        }

        void feedElementaryStream(MpegPlaybackState &playback, const uint8_t *data, size_t size, int64_t pts90k = -1, int64_t dts90k = -1)
        {
            if (!data || size == 0)
            {
                return;
            }

            const uint32_t feedEsIdx = g_mpeg_stub_state.feedEsTraceCount++;
            if (feedEsIdx < 32u)
            {
                PS2_IF_AGRESSIVE_LOGS({
                    char hexBuf[16] = {};
                    for (size_t i = 0; i < std::min<size_t>(4u, size); ++i)
                    {
                        ::snprintf(hexBuf + i * 2, 3, "%02x", data[i]);
                    }
                    std::cerr << "[MPEG:feedES] #" << feedEsIdx
                              << " size=" << size
                              << " first4=" << hexBuf
                              << " decoderFailed=" << playback.decoderFailed
                              << " waitSeq=" << playback.waitingForVideoSequenceHeader
                              << std::endl;
                });
            }

            if (playback.sequenceEndDecoded)
            {
                // Zero stuffing after the sequence_end_code changes nothing.
                if (std::all_of(data, data + size, [](uint8_t byte) { return byte == 0u; }))
                {
                    return;
                }
                // More video follows (a new sequence must start with a sequence
                // header). Like the SDK, which clears isEnd on the next decode,
                // continue with a fresh decoder; the drained one cannot accept
                // input. Already queued pictures are kept.
                playback.sequenceEndDecoded = false;
                playback.decoder.reset();
            }

            playback.sawInput = true;
            updateMpegPictureTiming(playback, data, size);
            if (playback.waitingForVideoSequenceHeader)
            {
                playback.videoSequenceSyncBuffer.insert(
                    playback.videoSequenceSyncBuffer.end(),
                    data,
                    data + size);

                constexpr size_t kMaxVideoSequenceSyncBytes = 2u * 1024u * 1024u;
                if (playback.videoSequenceSyncBuffer.size() > kMaxVideoSequenceSyncBytes)
                {
                    const size_t keepFrom = playback.videoSequenceSyncBuffer.size() - 3u;
                    playback.videoSequenceSyncBuffer.erase(
                        playback.videoSequenceSyncBuffer.begin(),
                        playback.videoSequenceSyncBuffer.begin() + static_cast<std::ptrdiff_t>(keepFrom));
                }

                const size_t sequenceHeader = findMpegSequenceHeader(
                    playback.videoSequenceSyncBuffer.data(),
                    playback.videoSequenceSyncBuffer.size());
                if (sequenceHeader == kStartCodeNotFound)
                {
                    return;
                }

                if (sequenceHeader != 0u)
                {
                    playback.videoSequenceSyncBuffer.erase(
                        playback.videoSequenceSyncBuffer.begin(),
                        playback.videoSequenceSyncBuffer.begin() + static_cast<std::ptrdiff_t>(sequenceHeader));
                }

                data = playback.videoSequenceSyncBuffer.data();
                size = playback.videoSequenceSyncBuffer.size();
                if (playback.pictureIntervalQ32 == 0u)
                {
                    playback.pictureIntervalQ32 = kDefaultPictureIntervalQ32;
                }
                playback.nextPictureTickQ32 = std::numeric_limits<uint64_t>::max();
                playback.presentationEndTickQ32 = std::numeric_limits<uint64_t>::max();
                playback.firstPresentedPts90k = -1;
                playback.ptsPresentationBaseTickQ32 = 0u;
                playback.waitingForVideoSequenceHeader = false;
                playback.decoderFailed = false;
                playback.decoder.reset();
                playback.decodedFrames.clear();
            }

            if (containsMpegSequenceEnd(data, size))
            {
                playback.sawSequenceEnd = true;
                playback.cdStreamGeneration = g_mpeg_stub_state.cdStreamGeneration;
            }

            const bool endsAtSequenceEnd = endsWithMpegSequenceEnd(data, size);

            if (!playback.decoder)
            {
                playback.decoder = std::make_unique<MpegFfmpegDecoder>();
            }

            if (!playback.decoder->feed(data, size, playback.decodedFrames, pts90k, dts90k))
            {
                playback.decoder.reset();
                playback.waitingForVideoSequenceHeader = true;
                playback.videoSequenceSyncBuffer.clear();
                playback.decoderFailed = false;
                return;
            }

            playback.videoSequenceSyncBuffer.clear();
            if (endsAtSequenceEnd)
            {
                // The SDK's _sceMpegFlush outputs the held reference picture at
                // the sequence_end_code; the FFmpeg parser also holds the last
                // picture until drained. Afterwards the SDK reports isEnd.
                playback.decoder->flush(playback.decodedFrames);
                playback.sequenceEndDecoded = true;
            }
            flushDecoderIfEnded(playback);
        }

        std::vector<MpegRegisteredCallback> matchingStreamCallbacks(uint32_t mpegAddr, uint32_t streamType)
        {
            std::vector<MpegRegisteredCallback> out;
            auto it = g_mpeg_stub_state.callbacksByMpeg.find(mpegAddr);
            if (it == g_mpeg_stub_state.callbacksByMpeg.end())
            {
                return out;
            }

            for (const MpegRegisteredCallback &callback : it->second)
            {
                if (callback.stream && callback.type == streamType)
                {
                    out.push_back(callback);
                }
            }
            return out;
        }

        std::vector<MpegRegisteredCallback> matchingOrdinaryCallbacks(uint32_t mpegAddr, uint32_t type)
        {
            std::vector<MpegRegisteredCallback> out;
            const auto found = g_mpeg_stub_state.callbacksByMpeg.find(mpegAddr);
            if (found != g_mpeg_stub_state.callbacksByMpeg.end())
                for (const MpegRegisteredCallback &callback : found->second)
                    if (!callback.stream && callback.type == type)
                        out.push_back(callback);
            return out;
        }

        void queueStreamCallbackEvent(uint32_t mpegAddr,
                                      uint32_t streamType,
                                      uint32_t dataAddr,
                                      uint32_t len,
                                      std::vector<MpegStreamCallbackEvent> &callbackEvents,
                                      int64_t pts90k = -1,
                                      int64_t dts90k = -1)
        {
            MpegStreamCallbackEvent event{};
            event.mpegAddr = mpegAddr;
            event.callbackGeneration = getPlaybackState(mpegAddr).callbackGeneration;
            event.streamType = streamType;
            event.dataAddr = dataAddr;
            event.len = len;
            event.pts = pts90k >= 0 ? static_cast<uint64_t>(pts90k) : 0xFFFFFFFFFFFFFFFFull;
            event.dts = dts90k >= 0 ? static_cast<uint64_t>(dts90k) : 0xFFFFFFFFFFFFFFFFull;
            event.callbacks = matchingStreamCallbacks(mpegAddr, streamType);
            g_dw5xlStrEvents[streamType & 7u].fetch_add(1u, std::memory_order_relaxed); // DW5XL-DEBUG
            if (!event.callbacks.empty())
            {
                callbackEvents.push_back(std::move(event));
            }
        }

        void finishPlaybackStream(uint32_t mpegAddr, MpegPlaybackState &playback)
        {
            (void)mpegAddr;
            // PSS packets remain in the guest ring until their callbacks accept
            // them. EOF can flush video only after those packets are committed.
            playback.streamEnded = true;
            playback.cdStreamGeneration = g_mpeg_stub_state.cdStreamGeneration;
            flushDecoderIfEnded(playback);
        }

        void finalizeCdStreamEofUnlocked(std::vector<uint32_t> &completedMpegIds, bool &changed)
        {
            for (const auto &[mpegAddr, playback] : g_mpeg_stub_state.playbackByMpeg)
                if (playback.demuxTransaction && !playback.demuxTransaction->finished)
                {
                    g_mpeg_stub_state.cdStreamEofPending = true;
                    return;
                }
            g_mpeg_stub_state.cdStreamEofPending = false;
            g_mpeg_stub_state.currentCdStreamEofSeen = true;
            for (auto &[mpegAddr, playback] : g_mpeg_stub_state.playbackByMpeg)
            {
                completedMpegIds.push_back(mpegAddr);
                if (!playback.sawInput || playback.streamEnded)
                {
                    publishSdkDecoderStatus(playback);
                    continue;
                }

                finishPlaybackStream(mpegAddr, playback);
                publishSdkDecoderStatus(playback);
                changed = true;
            }
        }

        bool mpegDemuxBackpressured(const MpegPlaybackState &playback)
        {
            // Let EOF finalization drain any tail that is already in the guest
            // ring, otherwise bound decode lead to a handful of pictures.
            //
            // Important: do not park sceMpegDemuxPss/Ring here. Code Veronica
            // explicitly wakes its video thread before every demux call and that
            // thread sleeps again after presenting one picture. A single host
            // decoder feed can enqueue more than kMaxDecodedPicturesAhead frames;
            // parking the producer then leaves the consumer asleep after draining
            // just one frame, with nobody left to issue the next WakeupThread.
            // Returning 0 bytes consumed instead leaves the guest ring intact and
            // lets the game's producer loop wake the consumer again. Backpressure
            // still propagates naturally to sceCdStRead because the ring does not
            // advance while this is true.
            return !g_mpeg_stub_state.currentCdStreamEofSeen &&
                   playback.decodedFrames.size() >= kMaxDecodedPicturesAhead;
        }

        void recordCdStreamBytesDemuxedUnlocked(
            size_t consumed,
            std::vector<uint32_t> &completedMpegIds,
            bool &changed)
        {
            g_mpeg_stub_state.cdStreamBytesDemuxed += consumed;
            if (g_mpeg_stub_state.cdStreamEofPending &&
                g_mpeg_stub_state.cdStreamBytesDemuxed >= g_mpeg_stub_state.cdStreamBytesProduced)
            {
                finalizeCdStreamEofUnlocked(completedMpegIds, changed);
            }
        }

        bool writeMpegCallbackData(uint8_t *rdram, uint32_t addr, const MpegStreamCallbackEvent &event)
        {
            if (!rdram || addr == 0u)
            {
                return false;
            }

            uint8_t *data = getMemPtr(rdram, addr);
            if (!data)
            {
                return false;
            }

            std::memset(data, 0, kMpegCallbackDataSize);
            *reinterpret_cast<uint32_t *>(data + 0x00u) = event.streamType;
            *reinterpret_cast<uint32_t *>(data + 0x08u) = event.dataAddr;
            *reinterpret_cast<uint32_t *>(data + 0x0Cu) = event.len;
            *reinterpret_cast<uint64_t *>(data + 0x10u) = event.pts;
            *reinterpret_cast<uint64_t *>(data + 0x18u) = event.dts;
            return true;
        }


        // Callback data normally comes from the guest heap, but games whose SetupHeap/malloc take all
        // memory (DW5XL) leave nothing for the runtime and every movie callback was dropped. Fall back
        // to a ring of slots reserved once in kernel RAM; slots are reused only after 256 more callbacks.
        constexpr uint32_t kMpegCallbackScratchSlots = 256u;
        uint32_t g_mpegCallbackScratchBase = 0u;
        uint32_t g_mpegCallbackScratchNext = 0u;
        std::mutex g_mpegCallbackScratchMutex;

        uint32_t allocMpegCallbackData(PS2Runtime *runtime, bool &fromHeap)
        {
            fromHeap = true;
            if (const uint32_t addr = runtime->guestMalloc(kMpegCallbackDataSize, 16u))
            {
                return addr;
            }
            fromHeap = false;
            std::lock_guard lock(g_mpegCallbackScratchMutex);
            if (g_mpegCallbackScratchBase == 0u)
            {
                const uint32_t size = kMpegCallbackScratchSlots * kMpegCallbackDataSize;
                const uint32_t top = runtime->reserveAsyncCallbackStack(size + 0x20u, 16u);
                if (top == 0u)
                {
                    return 0u;
                }
                g_mpegCallbackScratchBase = (top - size) & ~0xFu;
            }
            const uint32_t slot = g_mpegCallbackScratchNext++ % kMpegCallbackScratchSlots;
            return g_mpegCallbackScratchBase + slot * kMpegCallbackDataSize;
        }

        void freeMpegCallbackData(PS2Runtime *runtime, uint32_t addr, bool fromHeap)
        {
            if (addr != 0u && fromHeap)
            {
                runtime->guestFree(addr);
            }
        }

        bool dispatchGuestStreamCallback(uint8_t *rdram,
                                         R5900Context *callerCtx,
                                         PS2Runtime *runtime,
                                         const MpegStreamCallbackEvent &event,
                                         const MpegRegisteredCallback &callback,
                                         std::function<void(int32_t)> completion = {},
                                         std::shared_ptr<MpegDemuxTransaction> transaction = {})
        {
            if (!rdram || !callerCtx || !runtime || callback.func == 0u || !runtime->hasFunction(callback.func))
            {
                return false;
            }

            bool cbFromHeap = true;
            const uint32_t cbDataAddr = allocMpegCallbackData(runtime, cbFromHeap);
            if (cbDataAddr == 0u)
            {
                return false;
            }
            if (!writeMpegCallbackData(rdram, cbDataAddr, event))
            {
                freeMpegCallbackData(runtime, cbDataAddr, cbFromHeap);
                return false;
            }

            R5900Context callbackCtx = *callerCtx;
            SET_GPR_U32(&callbackCtx, 4, event.mpegAddr);
            SET_GPR_U32(&callbackCtx, 5, cbDataAddr);
            SET_GPR_U32(&callbackCtx, 6, callback.data);
            SET_GPR_U32(&callbackCtx, 7, 0u);
            SET_GPR_U32(&callbackCtx, 29, 0u);
            SET_GPR_U32(&callbackCtx, 31, 0u);
            callbackCtx.pc = callback.func;

            GuestInvocation invocation{};
            invocation.kind = GuestInvocationKind::RpcCallback;
            invocation.context = callbackCtx;
            auto executed = std::make_shared<bool>(false);
            invocation.shouldExecute = [mpeg = event.mpegAddr,
                                        generation = event.callbackGeneration,
                                        transaction, executed]
            {
                std::lock_guard lock(g_mpeg_stub_mutex);
                const auto found = g_mpeg_stub_state.playbackByMpeg.find(mpeg);
                const bool valid = found != g_mpeg_stub_state.playbackByMpeg.end() &&
                    found->second.callbackGeneration == generation &&
                    (!transaction || (!transaction->finished &&
                        found->second.demuxTransaction == transaction &&
                        found->second.cdStreamGeneration == transaction->generation));
                *executed = valid;
                return valid;
            };
            invocation.onComplete = [runtime, cbDataAddr, cbFromHeap, function = callback.func,
                                     rdram, mpeg = event.mpegAddr,
                                     type = event.streamType, bytes = event.len,
                                     executed,
                                     completion = std::move(completion)](const R5900Context &result, R5900Context &)
            {
                if (*executed && mpegTraceEnabled())
                {
                    static std::array<std::atomic<uint64_t>, 6u> calls{};
                    static std::array<std::atomic<uint64_t>, 6u> rejected{};
                    const size_t slot = std::min<size_t>(type, calls.size() - 1u);
                    const uint64_t count = calls[slot].fetch_add(1u, std::memory_order_relaxed) + 1u;
                    const int32_t value = static_cast<int32_t>(getRegU32(&result, 2));
                    const uint64_t failures = value <= 0
                        ? rejected[slot].fetch_add(1u, std::memory_order_relaxed) + 1u
                        : rejected[slot].load(std::memory_order_relaxed);
                    if (mpegTraceSample(count - 1u) || (value <= 0 && mpegTraceSample(failures - 1u)))
                    {
                        std::cerr << "[MPEG:CallbackComplete] func=0x" << std::hex << function << std::dec
                                  << " type=" << type << " bytes=" << bytes << " result=" << value
                                  << " calls=" << count << " rejected=" << failures << std::endl;
                        // Diagnostic only: this verified retail callback owns
                        // the KFIV audio ring. Inspect it after guest completion
                        // on the EE executor without altering its state.
                        if (function == 0x0021BF00u && mpeg == 0x00437938u)
                        {
                            const uint8_t *audio = getConstMemPtr(rdram, 0x004379F0u);
                            if (audio)
                            {
                                const auto field = [audio](uint32_t offset)
                                {
                                    uint32_t word = 0u;
                                    std::memcpy(&word, audio + offset, sizeof(word));
                                    return word;
                                };
                                std::cerr << "[MPEG:KFIVAudioRing] state=" << field(0u)
                                          << " headerBytes=" << field(0x2Cu)
                                          << " write=" << field(0x34u) << " pending=" << field(0x38u)
                                          << " capacity=" << field(0x3Cu) << " total=" << field(0x40u)
                                          << " iopBase=0x" << std::hex << field(0x44u) << std::dec
                                          << " iopCapacity=" << field(0x48u) << " iopWrite=" << field(0x4Cu)
                                          << " transferred=" << field(0x54u) << std::endl;
                            }
                        }
                    }
                }
                freeMpegCallbackData(runtime, cbDataAddr, cbFromHeap);
                if (completion)
                    completion(*executed ? static_cast<int32_t>(getRegU32(&result, 2)) : KE_WAIT_DELETE);
            };
            runtime->eeScheduler().queueInvocation(std::move(invocation));
            return true;
        }

        void dispatchStreamCallbacks(uint8_t *rdram,
                                     R5900Context *ctx,
                                     PS2Runtime *runtime,
                                     const std::vector<MpegStreamCallbackEvent> &events)
        {
            if (events.empty())
            {
                return;
            }

            for (const MpegStreamCallbackEvent &event : events)
            {
                for (const MpegRegisteredCallback &callback : event.callbacks)
                {
                    dispatchGuestStreamCallback(rdram, ctx, runtime, event, callback);
                }
            }
        }

        void dispatchStreamCallbacksUnlocked(uint8_t *rdram,
                                             R5900Context *ctx,
                                             PS2Runtime *runtime,
                                             const std::vector<MpegStreamCallbackEvent> &events)
        {
            if (events.empty())
            {
                return;
            }

            dispatchStreamCallbacks(rdram, ctx, runtime, events);
        }

        constexpr uint32_t kMpegDemuxWaitType = 2u;
        std::atomic<uint64_t> g_nextDemuxToken{1u};

        uint32_t demuxGuestAddress(const MpegDemuxTransaction &transaction, size_t offset)
        {
            if (transaction.ringSize != 0u)
            {
                const uint32_t start = (transaction.inputAddr - transaction.ringBase) & PS2_RAM_MASK;
                return transaction.ringBase + static_cast<uint32_t>((start + offset) % transaction.ringSize);
            }
            return transaction.inputAddr + static_cast<uint32_t>(offset);
        }

        void snapshotDemuxInput(MpegDemuxTransaction &transaction, size_t requested)
        {
            const size_t limit = std::min(requested, transaction.offeredBytes);
            size_t copied = transaction.input.size();
            if (limit <= copied)
                return;
            transaction.input.resize(limit);
            while (copied < limit)
            {
                const uint32_t guest = demuxGuestAddress(transaction, copied);
                size_t bytes = std::min(limit - copied, static_cast<size_t>(PS2_RAM_SIZE - (guest & PS2_RAM_MASK)));
                if (transaction.ringSize != 0u)
                {
                    const uint32_t ringOffset = (guest - transaction.ringBase) & PS2_RAM_MASK;
                    bytes = std::min(bytes, static_cast<size_t>(transaction.ringSize - ringOffset));
                }
                const uint8_t *source = getConstMemPtr(transaction.rdram, guest);
                if (!source || bytes == 0u)
                    break;
                std::memcpy(transaction.input.data() + copied, source, bytes);
                copied += bytes;
            }
            transaction.input.resize(copied);
        }

        void finishDemuxTransaction(const std::shared_ptr<MpegDemuxTransaction> &transaction)
        {
            std::vector<uint32_t> completed;
            bool eofChanged = false;
            {
                std::lock_guard lock(g_mpeg_stub_mutex);
                if (transaction->finished)
                    return;
                transaction->finished = true;
                const auto found = g_mpeg_stub_state.playbackByMpeg.find(transaction->mpegAddr);
                if (found != g_mpeg_stub_state.playbackByMpeg.end() &&
                    found->second.demuxTransaction == transaction)
                {
                    recordCdStreamBytesDemuxedUnlocked(transaction->committed, completed, eofChanged);
                    publishSdkDecoderStatus(found->second);
                    if (transaction->deferredReturn)
                        found->second.demuxTransaction.reset();
                }
            }
            static std::atomic<uint64_t> commitCount{0u};
            if (mpegTraceSample(commitCount.fetch_add(1u, std::memory_order_relaxed)))
                std::cerr << "[MPEG:DemuxCommit] mpeg=0x" << std::hex << transaction->mpegAddr
                          << std::dec << " offered=" << transaction->offeredBytes
                          << " snapshot=" << transaction->input.size()
                          << " committed=" << transaction->committed << std::endl;
            for (const uint32_t mpeg : completed)
                transaction->runtime->eeScheduler().completeExternalWait(kMpegPictureWaitType, mpeg, KE_OK);
            if (transaction->deferredReturn)
                transaction->runtime->eeScheduler().completeExternalWait(
                    kMpegDemuxWaitType, transaction->waitToken, static_cast<int>(transaction->committed));
        }

        void advanceDemuxTransaction(const std::shared_ptr<MpegDemuxTransaction> &transaction);

        void serviceMpegInput(const std::shared_ptr<MpegDemuxTransaction> &transaction,
                              const MpegStreamCallbackEvent &event, size_t index)
        {
            if (transaction->finished)
                return;
            if (index == event.callbacks.size())
            {
                advanceDemuxTransaction(transaction);
                return;
            }
            if (!dispatchGuestStreamCallback(transaction->rdram, &transaction->caller,
                    transaction->runtime, event, event.callbacks[index],
                    [transaction, event, index](int32_t)
                    { serviceMpegInput(transaction, event, index + 1u); }, transaction))
                finishDemuxTransaction(transaction);
        }

        void serviceRejectedDecoderConsumer(const std::shared_ptr<MpegDemuxTransaction> &transaction,
                                        const MpegStreamCallbackEvent &event, size_t index)
        {
            if (transaction->finished)
                return;
            if (index == event.callbacks.size())
            {
                finishDemuxTransaction(transaction);
                return;
            }
            if (!dispatchGuestStreamCallback(transaction->rdram, &transaction->caller,
                    transaction->runtime, event, event.callbacks[index],
                    [transaction, event, index](int32_t)
                    { serviceRejectedDecoderConsumer(transaction, event, index + 1u); }, transaction))
                finishDemuxTransaction(transaction);
        }

        void acceptDemuxCallback(const std::shared_ptr<MpegDemuxTransaction> &transaction, int32_t result)
        {
            bool rejected = result <= 0;
            MpegStreamCallbackEvent consumerService;
            {
                std::lock_guard lock(g_mpeg_stub_mutex);
                const auto found = g_mpeg_stub_state.playbackByMpeg.find(transaction->mpegAddr);
                if (found == g_mpeg_stub_state.playbackByMpeg.end() ||
                    found->second.demuxTransaction != transaction || transaction->finished)
                    return;
                if (std::getenv("PS2X_MPEGAUDIO_TRACE") && transaction->eventIndex < transaction->events.size()) // DW5XL-DEBUG
                {
                    const auto &ev = transaction->events[transaction->eventIndex];
                    if (ev.streamType == kMpegStrADPCM)
                    {
                        static uint64_t accepted = 0, rejectedCount = 0, packets = 0, lastTick = 0;
                        const uint64_t tick = transaction->runtime->eeScheduler().currentVSyncTick();
                        if (rejected) ++rejectedCount; else { accepted += ev.len; ++packets; }
                        if (tick >= lastTick + 60u)
                        {
                            std::fprintf(stderr, "[dw5xl:mpegaudio] tick=%llu acceptedBytes=%llu packets=%llu rejects=%llu lastLen=%u\n",
                                         (unsigned long long)tick, (unsigned long long)accepted, (unsigned long long)packets,
                                         (unsigned long long)rejectedCount, ev.len);
                            lastTick = tick;
                        }
                    }
                }
                if (rejected)
                {
                    MpegPlaybackState &playback = found->second;
                    playback.retryPacket = true;
                    playback.retryPacketAddr = demuxGuestAddress(*transaction, transaction->committed);
                    playback.retryPacketBytes = transaction->packetEnd - transaction->committed;
                    playback.retryEvent = transaction->eventIndex;
                    playback.retryCallback = transaction->callbackIndex;
                    const uint32_t type = transaction->events[transaction->eventIndex].streamType;
                    const uint64_t tick = transaction->runtime->eeScheduler().currentVSyncTick();
                    if (type == kMpegStrM2V)
                    {
                        static std::atomic<uint64_t> inputRejectCount{0u};
                        if (mpegTraceSample(inputRejectCount.fetch_add(1u, std::memory_order_relaxed)))
                        {
                            auto &memory = transaction->runtime->memory();
                            std::cerr << "[MPEG:VideoInputBlocked] mpeg=0x" << std::hex << transaction->mpegAddr
                                      << " chcr=0x" << memory.read32(0x1000B400u)
                                      << " madr=0x" << memory.read32(0x1000B410u)
                                      << " qwc=0x" << memory.read32(0x1000B420u)
                                      << " tadr=0x" << memory.read32(0x1000B430u) << std::dec
                                      << " tick=" << tick << " pictures=" << playback.picturesServed
                                      << " queued=" << playback.decodedFrames.size()
                                      << " acceptedPayload=" << playback.acceptedVideoPayloadBytes
                                      << " rejectedPayload=" << transaction->events[transaction->eventIndex].len
                                      << " committed=" << transaction->committed << std::endl;
                        }
                    }
                    if ((type == kMpegStrPCM || type == kMpegStrADPCM) &&
                        playback.picturesServed != 0u && playback.decodedFrames.empty() &&
                        !playback.streamEnded && !playback.decoderFailed &&
                        playback.lastOutputServiceTick != tick)
                    {
                        // The SDK's UPDATE boundary occurs during individual
                        // IPU output DMA batches, before a whole picture is
                        // returned. A whole-frame decoder otherwise starves
                        // an audio consumer after it blocks further video PES.
                        // Service the existing callback without accepting the
                        // rejected packet or producing another picture. Bound
                        // additional service to one host VSync per handle.
                        consumerService.mpegAddr = transaction->mpegAddr;
                        consumerService.callbackGeneration = playback.callbackGeneration;
                        consumerService.streamType = 4u;
                        consumerService.callbacks = matchingOrdinaryCallbacks(transaction->mpegAddr, 4u);
                        if (!consumerService.callbacks.empty())
                            playback.lastOutputServiceTick = tick;
                    }
                    if (type == kMpegStrM2V && playback.picturesServed != 0u &&
                        playback.decoder && playback.decodedFrames.empty() &&
                        !playback.streamEnded && !playback.decoderFailed &&
                        playback.lastInputServiceTick != tick)
                    {
                        // The SDK's busy IPU input loop also emits NODATA,
                        // independently of whole-picture output. A native
                        // decoder waiting for accepted input must still let
                        // that registered consumer reconcile real DMA progress.
                        // Preserve this rejected PES until an explicit retry
                        // accepts it, and bound extra service to one VSync.
                        consumerService.mpegAddr = transaction->mpegAddr;
                        consumerService.callbackGeneration = playback.callbackGeneration;
                        consumerService.streamType = 1u;
                        consumerService.callbacks = matchingOrdinaryCallbacks(transaction->mpegAddr, 1u);
                        if (!consumerService.callbacks.empty())
                            playback.lastInputServiceTick = tick;
                    }
                }
                else
                    ++transaction->callbackIndex;
            }
            if (rejected)
            {
                if (consumerService.callbacks.empty())
                    finishDemuxTransaction(transaction);
                else
                    serviceRejectedDecoderConsumer(transaction, consumerService, 0u);
            }
            else
                advanceDemuxTransaction(transaction);
        }

        void advanceDemuxTransaction(const std::shared_ptr<MpegDemuxTransaction> &transaction)
        {
            MpegStreamCallbackEvent event;
            MpegRegisteredCallback callback{};
            bool dispatch = false;
            bool finish = false;
            bool wakePicture = false;
            uint64_t consumedVideoBytes = 0u;
            MpegStreamCallbackEvent inputService;
            bool serviceInput = false;
            {
                std::lock_guard lock(g_mpeg_stub_mutex);
                const auto found = g_mpeg_stub_state.playbackByMpeg.find(transaction->mpegAddr);
                if (found == g_mpeg_stub_state.playbackByMpeg.end() ||
                    found->second.demuxTransaction != transaction || transaction->finished)
                    return;
                MpegPlaybackState &playback = found->second;
                const auto &input = transaction->input;
                if (playback.streamEnded)
                {
                    transaction->committed = input.size();
                    finish = true;
                }
                while (!dispatch && !finish && !serviceInput)
                {
                    if (transaction->packetPending)
                    {
                        while (transaction->eventIndex < transaction->events.size() &&
                               transaction->callbackIndex >= transaction->events[transaction->eventIndex].callbacks.size())
                        {
                            ++transaction->eventIndex;
                            transaction->callbackIndex = 0u;
                        }
                        if (transaction->eventIndex < transaction->events.size())
                        {
                            event = transaction->events[transaction->eventIndex];
                            callback = event.callbacks[transaction->callbackIndex];
                            dispatch = true;
                            break;
                        }
                        if (transaction->videoPacket)
                        {
                            const size_t before = playback.decodedFrames.size();
                            feedElementaryStream(playback, input.data() + transaction->payloadStart,
                                transaction->packetEnd - transaction->payloadStart,
                                transaction->pts90k, transaction->dts90k);
                            wakePicture |= before != playback.decodedFrames.size() || playback.decoderFailed ||
                                           playback.sequenceEndDecoded;
                            if (!playback.decoderFailed)
                            {
                                consumedVideoBytes += transaction->packetEnd - transaction->payloadStart;
                                playback.acceptedVideoPayloadBytes += transaction->packetEnd - transaction->payloadStart;
                            }
                        }
                        playback.retryPacket = false;
                        transaction->committed = transaction->packetEnd;
                        transaction->packetPending = false;
                        // A known rejected packet is the only input needed on
                        // another zero-acceptance retry. Once it accepts, extend
                        // the snapshot and continue through the offered tail.
                        if (input.size() < transaction->offeredBytes)
                            snapshotDemuxInput(*transaction, transaction->offeredBytes);
                        if (transaction->videoPacket && !playback.decoderFailed)
                        {
                            // The decoder has consumed the submitted input and
                            // is ready for more. The SDK's NODATA callback primes
                            // or reconciles its compressed-input DMA ring.
                            inputService.mpegAddr = transaction->mpegAddr;
                            inputService.callbackGeneration = playback.callbackGeneration;
                            inputService.streamType = 1u;
                            inputService.callbacks = matchingOrdinaryCallbacks(transaction->mpegAddr, 1u);
                            serviceInput = !inputService.callbacks.empty();
                            if (serviceInput)
                                break;
                        }
                    }

                    if (transaction->committed == input.size() || mpegDemuxBackpressured(playback))
                    {
                        finish = true;
                        break;
                    }
                    const size_t start = findStartCode(input, transaction->committed);
                    if (start == kStartCodeNotFound)
                    {
                        // Preserve a split start code in the original guest ring.
                        if (g_mpeg_stub_state.cdStreamEofPending &&
                            g_mpeg_stub_state.cdStreamBytesDemuxed + input.size() >= g_mpeg_stub_state.cdStreamBytesProduced)
                            transaction->committed = input.size();
                        else if (input.size() > transaction->committed + 3u)
                            transaction->committed = input.size() - 3u;
                        finish = true;
                        break;
                    }
                    transaction->committed = start;
                    if (input.size() - start < 4u)
                    {
                        finish = true;
                        break;
                    }
                    const uint8_t stream = input[start + 3u];
                    if (stream == kMpegProgramEnd)
                    {
                        // This marker is reached only after every preceding PES
                        // callback accepts. The program can end before rounded
                        // CD sectors or padding have all been demultiplexed.
                        playback.acceptedProgramEnd = true;
                        playback.streamEnded = true;
                        flushDecoderIfEnded(playback);
                        transaction->committed = start + 4u;
                        wakePicture = true;
                        finish = true;
                        break;
                    }
                    if (stream == kMpegPackHeader)
                    {
                        if (input.size() - start < 12u)
                        {
                            finish = true;
                            break;
                        }
                        size_t bytes = 12u;
                        if ((input[start + 4u] & 0xC0u) == 0x40u)
                        {
                            if (input.size() - start < 14u)
                            {
                                finish = true;
                                break;
                            }
                            bytes = 14u + (input[start + 13u] & 7u);
                        }
                        if (input.size() - start < bytes)
                        {
                            finish = true;
                            break;
                        }
                        transaction->committed = start + bytes;
                        continue;
                    }
                    if (input.size() - start < 6u)
                    {
                        finish = true;
                        break;
                    }
                    const uint16_t bytes = readBe16(input.data() + start + 4u);
                    size_t end = start + 6u + bytes;
                    if (bytes == 0u && !isLengthPrefixedHeader(stream))
                    {
                        // A bounded retry may omit the following start code
                        // which originally established a zero-length PES end.
                        // Its previously rejected packet boundary remains known.
                        if (playback.retryPacket && playback.retryPacketAddr == demuxGuestAddress(*transaction, start) &&
                            playback.retryPacketBytes <= input.size() - start)
                            end = start + playback.retryPacketBytes;
                        else
                            end = findStartCode(input, start + 6u);
                        if (end == kStartCodeNotFound && g_mpeg_stub_state.cdStreamEofPending &&
                            g_mpeg_stub_state.cdStreamBytesDemuxed + input.size() >= g_mpeg_stub_state.cdStreamBytesProduced)
                            end = input.size();
                    }
                    if (end == kStartCodeNotFound || end > input.size())
                    {
                        finish = true;
                        break;
                    }
                    if (!isVideoStreamId(stream) && !isAudioStreamId(stream))
                    {
                        transaction->committed = end;
                        continue;
                    }
                    const MpegPesHeader pes = parsePesHeader(input.data() + start, end - start);
                    const size_t payload = start + pes.payloadOffset;
                    transaction->events.clear();
                    if (payload < end)
                    {
                        if (isVideoStreamId(stream))
                            queueStreamCallbackEvent(transaction->mpegAddr, kMpegStrM2V,
                                demuxGuestAddress(*transaction, payload), static_cast<uint32_t>(end - payload),
                                transaction->events, pes.pts90k, pes.dts90k);
                        else if (end - payload >= 2u && (input[payload + 1u] & 0xF0u) == 0xA0u)
                        {
                            // PSS private stream 1 carries a 4-byte prefix whose second byte is the
                            // sub-stream id: 0xA0-0xAF are audio (SShd/SSbd ADPCM or PCM). Other ids
                            // (e.g. DW5XL's 0x90 data track) must not reach audio callbacks.
                            if (std::getenv("PS2X_MPEGAUDIO_TRACE")) // DW5XL-DEBUG: audio sub-stream bytes
                            {
                                static unsigned logged = 0;
                                static uint32_t lastPrefix = 0;
                                const uint32_t prefix = (input[payload] << 8) | input[payload + 1];
                                if (end - payload > 48 && input[payload + 4] == 'S' && input[payload + 5] == 'S' && input[payload + 6] == 'h')
                                {
                                    std::fprintf(stderr, "[dw5xl:sshd] sub=0x%02x:", input[payload + 1]);
                                    for (size_t k = 4; k < 44; ++k)
                                        std::fprintf(stderr, " %02x", input[payload + k]);
                                    std::fprintf(stderr, "\n");
                                }
                                if (false && (prefix != lastPrefix || logged++ < 4))
                                    lastPrefix = prefix, std::fprintf(stderr, "[dw5xl:audiopkt] stream=0x%02x len=%zu first=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                                                 stream, end - payload, input[payload], input[payload + 1], input[payload + 2], input[payload + 3],
                                                 input[payload + 4], input[payload + 5], input[payload + 6], input[payload + 7]);
                            }
                            queueStreamCallbackEvent(transaction->mpegAddr, kMpegStrPCM,
                                demuxGuestAddress(*transaction, payload), static_cast<uint32_t>(end - payload),
                                transaction->events, pes.pts90k, pes.dts90k);
                            queueStreamCallbackEvent(transaction->mpegAddr, kMpegStrADPCM,
                                demuxGuestAddress(*transaction, payload), static_cast<uint32_t>(end - payload),
                                transaction->events, pes.pts90k, pes.dts90k);
                        }
                    }
                    transaction->packetEnd = end;
                    transaction->payloadStart = payload;
                    transaction->pts90k = pes.pts90k;
                    transaction->dts90k = pes.dts90k;
                    transaction->videoPacket = isVideoStreamId(stream) && payload < end;
                    transaction->eventIndex = 0u;
                    transaction->callbackIndex = 0u;
                    if (playback.retryPacket && playback.retryPacketAddr == demuxGuestAddress(*transaction, start) &&
                        playback.retryPacketBytes == end - start)
                    {
                        transaction->eventIndex = playback.retryEvent;
                        transaction->callbackIndex = playback.retryCallback;
                    }
                    transaction->packetPending = true;
                }
                publishSdkDecoderStatus(playback);
            }
            if (wakePicture)
                transaction->runtime->eeScheduler().completeExternalWait(kMpegPictureWaitType, transaction->mpegAddr, KE_OK);
            if (consumedVideoBytes != 0u)
                transaction->runtime->memory().creditIpuInputBytes(consumedVideoBytes);
            if (serviceInput)
                serviceMpegInput(transaction, inputService, 0u);
            else if (finish)
                finishDemuxTransaction(transaction);
            else if (dispatch && !dispatchGuestStreamCallback(transaction->rdram, &transaction->caller,
                         transaction->runtime, event, callback,
                         [transaction](int32_t result) { acceptDemuxCallback(transaction, result); }, transaction))
                acceptDemuxCallback(transaction, 0);
        }

        void demuxGuestInput(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                            uint32_t ringBase, uint32_t ringSize)
        {
            const uint32_t mpeg = getRegU32(ctx, 4);
            const uint32_t addr = getRegU32(ctx, 5);
            const uint32_t bytes = getRegU32(ctx, 6);
            if (!rdram || !runtime || bytes > PS2_RAM_SIZE)
            {
                setReturnS32(ctx, -1);
                return;
            }
            auto transaction = std::make_shared<MpegDemuxTransaction>();
            {
                std::lock_guard lock(g_mpeg_stub_mutex);
                MpegPlaybackState &playback = getPlaybackState(mpeg);
                if (playback.demuxTransaction)
                {
                    const auto old = playback.demuxTransaction;
                    if (!old->finished)
                    {
                        setReturnS32(ctx, 0);
                        return;
                    }
                    playback.demuxTransaction.reset();
                    setReturnS32(ctx, static_cast<int32_t>(old->committed));
                    return;
                }
                if (bytes == 0u || mpegDemuxBackpressured(playback))
                {
                    setReturnS32(ctx, 0);
                    return;
                }
                if (playback.cdStreamGeneration != g_mpeg_stub_state.cdStreamGeneration)
                    playback = makeFreshPlaybackStatePreservingConfig(playback);
                playback.sawInput = true;
                playback.cdStreamGeneration = g_mpeg_stub_state.cdStreamGeneration;
                transaction->mpegAddr = mpeg;
                transaction->inputAddr = addr;
                transaction->offeredBytes = bytes;
                transaction->generation = playback.cdStreamGeneration;
                transaction->rdram = rdram;
                transaction->runtime = runtime;
                transaction->caller = *ctx;
                transaction->waitToken = g_nextDemuxToken.fetch_add(1u, std::memory_order_relaxed);
                transaction->deferredReturn = runtime->eeScheduler().currentThreadId() != 0;
                const uint32_t offset = (addr - ringBase) & PS2_RAM_MASK;
                const bool wrapped = ringBase != 0u && ringSize != 0u && ringSize <= PS2_RAM_SIZE && offset < ringSize;
                if (wrapped)
                {
                    transaction->ringBase = ringBase;
                    transaction->ringSize = ringSize;
                }
                const size_t needed = playback.retryPacket && playback.retryPacketAddr == addr &&
                    playback.retryPacketBytes <= bytes ? playback.retryPacketBytes : bytes;
                snapshotDemuxInput(*transaction, needed);
                playback.demuxTransaction = transaction;
            }
            advanceDemuxTransaction(transaction);
            if (transaction->finished)
            {
                std::lock_guard lock(g_mpeg_stub_mutex);
                const auto found = g_mpeg_stub_state.playbackByMpeg.find(mpeg);
                if (found != g_mpeg_stub_state.playbackByMpeg.end() && found->second.demuxTransaction == transaction)
                    found->second.demuxTransaction.reset();
                setReturnS32(ctx, static_cast<int32_t>(transaction->committed));
                return;
            }
            if (transaction->deferredReturn)
                runtime->eeScheduler().waitExternal(EeWaitReason::Mpeg, kMpegDemuxWaitType, transaction->waitToken);
            setReturnS32(ctx, 0);
        }

        void writeBlankMpegFrame(uint8_t *rdram, uint32_t destAddr, uint32_t width, uint32_t height)
        {
            if (!rdram || destAddr == 0u)
            {
                return;
            }

            const uint32_t outWidth = align16(width == 0u ? kStubMovieWidth : width);
            const uint32_t outHeight = align16(height == 0u ? kStubMovieHeight : height);
            const uint32_t macroblockColumns = outWidth / 16u;
            for (uint32_t mbx = 0u; mbx < macroblockColumns; ++mbx)
            {
                const size_t stripOffset =
                    static_cast<size_t>(mbx) * static_cast<size_t>(outHeight) * 16u * 4u;
                for (uint32_t y = 0u; y < outHeight; ++y)
                {
                    uint8_t *dst = getMemPtr(
                        rdram,
                        destAddr + static_cast<uint32_t>(stripOffset + static_cast<size_t>(y) * 16u * 4u));
                    if (!dst)
                    {
                        continue;
                    }
                    for (uint32_t x = 0u; x < 16u; ++x)
                    {
                        dst[x * 4u + 0u] = 0u;
                        dst[x * 4u + 1u] = 0u;
                        dst[x * 4u + 2u] = 0u;
                        dst[x * 4u + 3u] = 0x80u;
                    }
                }
            }
        }

        void writeDecodedFrameToGuest(uint8_t *rdram, uint32_t destAddr, const MpegDecodedFrame &frame)
        {
            if (!rdram || destAddr == 0u || frame.rgba.empty() || frame.width <= 0 || frame.height <= 0)
            {
                return;
            }

            const uint32_t width = static_cast<uint32_t>(frame.width);
            const uint32_t height = static_cast<uint32_t>(frame.height);
            const uint32_t outWidth = align16(width);
            const uint32_t outHeight = align16(height);
            const uint32_t macroblockColumns = outWidth / 16u;

            for (uint32_t mbx = 0u; mbx < macroblockColumns; ++mbx)
            {
                const size_t stripOffset =
                    static_cast<size_t>(mbx) * static_cast<size_t>(outHeight) * 16u * 4u;
                for (uint32_t y = 0u; y < outHeight; ++y)
                {
                    uint8_t *dst = getMemPtr(
                        rdram,
                        destAddr + static_cast<uint32_t>(stripOffset + static_cast<size_t>(y) * 16u * 4u));
                    if (!dst)
                    {
                        continue;
                    }

                    for (uint32_t x = 0u; x < 16u; ++x)
                    {
                        const uint32_t srcX = mbx * 16u + x;
                        const uint8_t *src = nullptr;
                        if (srcX < width && y < height)
                        {
                            src = frame.rgba.data() +
                                  (static_cast<size_t>(y) * static_cast<size_t>(width) + srcX) * 4u;
                        }

                        if (src)
                        {
                            dst[x * 4u + 0u] = src[0u];
                            dst[x * 4u + 1u] = src[1u];
                            dst[x * 4u + 2u] = src[2u];
                            dst[x * 4u + 3u] = 0x80u;
                        }
                        else
                        {
                            dst[x * 4u + 0u] = 0u;
                            dst[x * 4u + 1u] = 0u;
                            dst[x * 4u + 2u] = 0u;
                            dst[x * 4u + 3u] = 0x80u;
                        }
                    }
                }
            }
        }

        void resetMpegStubStateUnlocked()
        {
            g_mpeg_stub_state.initialized = false;
            g_mpeg_stub_state.nextCallbackHandle = 1u;
            g_mpeg_stub_state.cdStreamGeneration = 0u;
            g_mpeg_stub_state.cdStreamBytesProduced = 0u;
            g_mpeg_stub_state.cdStreamBytesDemuxed = 0u;
            g_mpeg_stub_state.cdStreamEofPending = false;
            g_mpeg_stub_state.currentCdStreamEofSeen = false;
            g_mpeg_stub_state.feedEsTraceCount = 0u;
            g_mpeg_stub_state.demuxPssTraceCount = 0u;
            g_mpeg_stub_state.demuxRingTraceCount = 0u;
            g_mpeg_stub_state.getPictureWaitTraceCount = 0u;
            g_mpeg_stub_state.pictureTraceCount = 0u;
            g_mpeg_stub_state.isEndTraceCount = 0u;
            g_mpeg_stub_state.callbacksByMpeg.clear();
            g_mpeg_stub_state.playbackByMpeg.clear();
        }
    }

    void resetMpegStubState()
    {
        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        resetMpegStubStateUnlocked();
    }

    void enqueueMpegDecodedFrameForTesting(uint32_t mpegAddr)
    {
        constexpr int kTestFrameWidth = 16;
        constexpr int kTestFrameHeight = 16;

        MpegDecodedFrame frame;
        frame.width = kTestFrameWidth;
        frame.height = kTestFrameHeight;
        frame.rgba.resize(static_cast<size_t>(kTestFrameWidth * kTestFrameHeight * 4), 0x80u);

        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        MpegPlaybackState &playback = getPlaybackState(mpegAddr);
        playback.sawInput = true;
        playback.decodedFrames.push_back(std::move(frame));
        publishSdkDecoderStatus(playback);
    }

    void notifyMpegCdStreamStart(PS2Runtime *runtime)
    {
        std::vector<uint64_t> cancelled;
        {
        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        ++g_mpeg_stub_state.cdStreamGeneration;
        g_mpeg_stub_state.cdStreamBytesProduced = 0u;
        g_mpeg_stub_state.cdStreamBytesDemuxed = 0u;
        g_mpeg_stub_state.cdStreamEofPending = false;
        g_mpeg_stub_state.currentCdStreamEofSeen = false;
        g_mpeg_stub_state.feedEsTraceCount = 0u;
        g_mpeg_stub_state.demuxPssTraceCount = 0u;
        g_mpeg_stub_state.demuxRingTraceCount = 0u;
        g_mpeg_stub_state.getPictureWaitTraceCount = 0u;
        g_mpeg_stub_state.pictureTraceCount = 0u;
        g_mpeg_stub_state.isEndTraceCount = 0u;

        for (auto &[mpegAddr, playback] : g_mpeg_stub_state.playbackByMpeg)
        {
            if (playback.demuxTransaction)
            {
                playback.demuxTransaction->finished = true;
                cancelled.push_back(playback.demuxTransaction->waitToken);
            }
            playback = makeFreshPlaybackStatePreservingConfig(playback);
            publishSdkDecoderStatus(playback);
        }
        if (mpegTraceEnabled())
            std::cerr << "[MPEG:CdStreamStart] generation=" << g_mpeg_stub_state.cdStreamGeneration
                      << " handles=" << g_mpeg_stub_state.playbackByMpeg.size() << std::endl;
        PS2_IF_AGRESSIVE_LOGS({
            std::cerr << "[MPEG:CdStreamStart] generation=" << g_mpeg_stub_state.cdStreamGeneration
                      << " reopened=" << g_mpeg_stub_state.playbackByMpeg.size() << std::endl;
        });
        }
        if (runtime)
        {
            runtime->memory().resetIpuInputConsumption();
            for (uint64_t token : cancelled)
                runtime->eeScheduler().completeExternalWait(kMpegDemuxWaitType, token, KE_WAIT_DELETE);
        }
    }

    void notifyMpegCdStreamDataProduced(uint32_t byteCount, bool endOfStream)
    {
        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        g_mpeg_stub_state.cdStreamBytesProduced += byteCount;
        if (endOfStream)
        {
            g_mpeg_stub_state.cdStreamEofPending = true;
            if (mpegTraceEnabled())
                std::cerr << "[MPEG:ProducerEof] produced=" << g_mpeg_stub_state.cdStreamBytesProduced
                          << " demuxed=" << g_mpeg_stub_state.cdStreamBytesDemuxed << std::endl;
        }
    }

    void notifyMpegCdStreamEof(PS2Runtime *runtime)
    {
        std::vector<uint32_t> completedMpegIds;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
            g_mpeg_stub_state.cdStreamEofPending = true;
            if (g_mpeg_stub_state.cdStreamBytesDemuxed >= g_mpeg_stub_state.cdStreamBytesProduced)
                finalizeCdStreamEofUnlocked(completedMpegIds, changed);
            if (mpegTraceEnabled())
                std::cerr << "[MPEG:CdStreamEof] produced=" << g_mpeg_stub_state.cdStreamBytesProduced
                          << " demuxed=" << g_mpeg_stub_state.cdStreamBytesDemuxed
                          << " handles=" << completedMpegIds.size() << std::endl;
        }

        if (changed)
        {
            static uint32_t s_eofLogCount = 0u;
            if (s_eofLogCount < 8u)
            {
                PS2_IF_AGRESSIVE_LOGS({
                    std::cerr << "[MPEG:CdStreamEof] finalized active MPEG playback" << std::endl;
                });
                ++s_eofLogCount;
            }
        }
        if (runtime)
        {
            for (const uint32_t mpegAddr : completedMpegIds)
            {
                runtime->eeScheduler().completeExternalWait(kMpegPictureWaitType, mpegAddr, KE_OK);
            }
        }
    }

    void sceMpegFlush(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        const uint32_t mpegAddr = getRegU32(ctx, 4);
        bool wakePictureWaiter = false;
        {
            std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
            MpegPlaybackState &playback = getPlaybackState(mpegAddr);
            const size_t framesBefore = playback.decodedFrames.size();
            if (playback.decoder)
            {
                playback.decoder->flush(playback.decodedFrames);
            }
            wakePictureWaiter = playback.decodedFrames.size() != framesBefore ||
                                playback.streamEnded ||
                                playback.decoderFailed;
            publishSdkDecoderStatus(playback);
        }
        if (wakePictureWaiter)
        {
            runtime->eeScheduler().completeExternalWait(kMpegPictureWaitType, mpegAddr, KE_OK);
        }
        setReturnS32(ctx, 0);
    }

    void sceMpegAddBs(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t mpegAddr = getRegU32(ctx, 4);
        const uint32_t dataAddr = getRegU32(ctx, 5);
        const uint32_t byteCount = getRegU32(ctx, 6);

        size_t copied = 0u;
        bool wakePictureWaiter = false;
        {
            std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
            MpegPlaybackState &playback = getPlaybackState(mpegAddr);
            const size_t framesBefore = playback.decodedFrames.size();
            while (copied < byteCount)
            {
                const uint32_t curAddr = dataAddr + static_cast<uint32_t>(copied);
                const uint32_t offset = curAddr & PS2_RAM_MASK;
                const size_t chunk = std::min<size_t>(static_cast<size_t>(byteCount) - copied, PS2_RAM_SIZE - offset);
                const uint8_t *src = getConstMemPtr(rdram, curAddr);
                if (!src || chunk == 0u)
                {
                    break;
                }
                feedElementaryStream(playback, src, chunk);
                copied += chunk;
            }
            wakePictureWaiter = playback.decodedFrames.size() != framesBefore || playback.streamEnded || playback.decoderFailed;
            publishSdkDecoderStatus(playback);
        }

        if (wakePictureWaiter)
        {
            runtime->eeScheduler().completeExternalWait(kMpegPictureWaitType, mpegAddr, KE_OK);
        }
        setReturnS32(ctx, static_cast<int32_t>(copied));
    }

    void sceMpegAddCallback(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;

        const uint32_t mpegAddr = getRegU32(ctx, 4);
        const uint32_t callbackType = getRegU32(ctx, 5);
        const uint32_t callbackFunc = getRegU32(ctx, 6);
        const uint32_t callbackData = getRegU32(ctx, 7);

        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        g_mpeg_stub_state.initialized = true;
        (void)getPlaybackState(mpegAddr);

        const uint32_t handle = g_mpeg_stub_state.nextCallbackHandle++;
        g_mpeg_stub_state.callbacksByMpeg[mpegAddr].push_back(
            MpegRegisteredCallback{callbackType, 0u, callbackFunc, callbackData, handle, false});

        setReturnU32(ctx, handle);
    }

    void sceMpegAddStrCallback(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        const uint32_t mpegAddr = getRegU32(ctx, 4);
        const uint32_t streamType = getRegU32(ctx, 5);
        const uint32_t streamId = getRegU32(ctx, 6);
        const uint32_t callbackFunc = getRegU32(ctx, 7);
        const uint32_t callbackData = readAbiArg4(rdram, ctx);

        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        g_mpeg_stub_state.initialized = true;
        (void)getPlaybackState(mpegAddr);
        const uint32_t handle = g_mpeg_stub_state.nextCallbackHandle++;
        g_mpeg_stub_state.callbacksByMpeg[mpegAddr].push_back(
            MpegRegisteredCallback{streamType, streamId, callbackFunc, callbackData, handle, true});
        if (mpegTraceEnabled())
            std::cerr << "[MPEG:RegisterStream] mpeg=0x" << std::hex << mpegAddr
                      << " func=0x" << callbackFunc << " user=0x" << callbackData << std::dec
                      << " type=" << streamType << " id=" << streamId << std::endl;
        setReturnU32(ctx, 0u);
    }

    void sceMpegClearRefBuff(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)ctx;
        (void)runtime;
        static const uint32_t kRefGlobalAddrs[] = {
            0x171800u, 0x17180Cu, 0x171818u, 0x171804u, 0x171810u, 0x17181Cu};
        for (uint32_t addr : kRefGlobalAddrs)
        {
            uint8_t *p = getMemPtr(rdram, addr);
            if (!p)
                continue;
            uint32_t ptr = *reinterpret_cast<uint32_t *>(p);
            if (ptr != 0u)
            {
                uint8_t *q = getMemPtr(rdram, ptr + 0x28u);
                if (q)
                    *reinterpret_cast<uint32_t *>(q) = 0u;
            }
        }
        setReturnU32(ctx, 1u);
    }

    static void mpegGuestWrite32(uint8_t *rdram, uint32_t addr, uint32_t value)
    {
        if (uint8_t *p = getMemPtr(rdram, addr))
            *reinterpret_cast<uint32_t *>(p) = value;
    }
    static void mpegGuestWrite64(uint8_t *rdram, uint32_t addr, uint64_t value)
    {
        if (uint8_t *p = getMemPtr(rdram, addr))
        {
            *reinterpret_cast<uint32_t *>(p) = static_cast<uint32_t>(value);
            *reinterpret_cast<uint32_t *>(p + 4) = static_cast<uint32_t>(value >> 32);
        }
    }

    void sceMpegCreate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t param_1 = getRegU32(ctx, 4); // a0
        const uint32_t param_2 = getRegU32(ctx, 5); // a1
        const uint32_t param_3 = getRegU32(ctx, 6); // a2

        const uint32_t uVar3 = (param_2 + 3u) & 0xFFFFFFFCu;
        const int32_t iVar2_signed = static_cast<int32_t>(param_3) - static_cast<int32_t>(uVar3 - param_2);

        if (iVar2_signed <= 0x117)
        {
            setReturnU32(ctx, 0u);
            return;
        }

        uint64_t cancelled = 0u;
        {
            std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
            MpegPlaybackState &old = getPlaybackState(param_1);
            if (old.demuxTransaction)
            {
                old.demuxTransaction->finished = true;
                cancelled = old.demuxTransaction->waitToken;
            }
            MpegPlaybackState fresh = makeFreshPlaybackState();
            fresh.guestRdram = rdram;
            fresh.sdkWorkAddr = uVar3;
            // Create initializes the SDK callback tables for this handle.
            // Reset preserves registrations, but recreating an arena must not
            // append new consumers to those from a previous movie instance.
            g_mpeg_stub_state.callbacksByMpeg.erase(param_1);
            getPlaybackState(param_1) = std::move(fresh);
        }
        if (runtime)
        {
            runtime->memory().resetIpuInputConsumption();
            if (cancelled != 0u)
                runtime->eeScheduler().completeExternalWait(kMpegDemuxWaitType, cancelled, KE_WAIT_DELETE);
        }

        const uint32_t puVar4 = uVar3 + 0x108u;
        const uint32_t innerSize = static_cast<uint32_t>(iVar2_signed) - 0x118u;

        mpegGuestWrite32(rdram, param_1 + 0x40, uVar3);
        // sceMpegIsEnd's retail SDK getter reads *(mpeg->work + 0).
        // The caller-provided arena can contain a previous allocation's data.
        mpegGuestWrite32(rdram, uVar3 + 0x00, 0u);
        mpegGuestWrite32(rdram, uVar3 + 0x04, 0u);

        const uint32_t a1_init = uVar3 + 0x118u;
        mpegGuestWrite32(rdram, puVar4 + 0x0, a1_init);
        mpegGuestWrite32(rdram, puVar4 + 0x4, innerSize);
        mpegGuestWrite32(rdram, puVar4 + 0x8, a1_init);
        mpegGuestWrite32(rdram, puVar4 + 0xC, a1_init);

        const uint32_t allocResult = runtime ? runtime->guestMalloc(0x600, 8u) : (uVar3 + 0x200u);
        mpegGuestWrite32(rdram, uVar3 + 0x44, allocResult);

        // param_1[0..2] = 0; param_1[4..0xe] = 0xffffffff/0 as per decompilation
        mpegGuestWrite32(rdram, param_1 + 0x00, 0);
        mpegGuestWrite32(rdram, param_1 + 0x04, 0);
        mpegGuestWrite32(rdram, param_1 + 0x08, 0);
        mpegGuestWrite64(rdram, param_1 + 0x10, 0xFFFFFFFFFFFFFFFFULL);
        mpegGuestWrite64(rdram, param_1 + 0x18, 0xFFFFFFFFFFFFFFFFULL);
        mpegGuestWrite64(rdram, param_1 + 0x20, 0);
        mpegGuestWrite64(rdram, param_1 + 0x28, 0xFFFFFFFFFFFFFFFFULL);
        mpegGuestWrite64(rdram, param_1 + 0x30, 0xFFFFFFFFFFFFFFFFULL);
        mpegGuestWrite64(rdram, param_1 + 0x38, 0);

        static const unsigned s_zeroOffsets[] = {
            0xB4, 0xB8, 0xBC, 0xC0, 0xC4, 0xC8, 0xCC, 0xD0, 0xD4, 0xD8, 0xDC, 0xE0, 0xE4, 0xE8, 0xF8,
            0x0C, 0x14, 0x2C, 0x34, 0x3C,
            0x48, 0xFC, 0x100, 0x104, 0x70, 0x90, 0xAC};
        for (unsigned off : s_zeroOffsets)
            mpegGuestWrite32(rdram, uVar3 + off, 0u);
        mpegGuestWrite64(rdram, uVar3 + 0x78, 0);
        mpegGuestWrite64(rdram, uVar3 + 0x88, 0);

        mpegGuestWrite64(rdram, uVar3 + 0xF0, 0xFFFFFFFFFFFFFFFFULL);
        mpegGuestWrite32(rdram, uVar3 + 0x1C, 0x1209F8u);
        mpegGuestWrite32(rdram, uVar3 + 0x24, 0x120A08u);
        mpegGuestWrite32(rdram, uVar3 + 0xB0, 1u);
        mpegGuestWrite32(rdram, uVar3 + 0x9C, 0xFFFFFFFFu);
        mpegGuestWrite32(rdram, uVar3 + 0x80, 0xFFFFFFFFu);
        mpegGuestWrite32(rdram, uVar3 + 0x94, 0xFFFFFFFFu);
        mpegGuestWrite32(rdram, uVar3 + 0x98, 0xFFFFFFFFu);

        mpegGuestWrite32(rdram, 0x1717BCu, param_1);

        static const uint32_t s_refValues[] = {
            0x171A50u, 0x171C58u, 0x171CC0u, 0x171D28u, 0x171D90u,
            0x171AB8u, 0x171B20u, 0x171B88u, 0x171BF0u};
        for (unsigned i = 0; i < 9u; ++i)
            mpegGuestWrite32(rdram, 0x171800u + i * 4u, s_refValues[i]);

        uint32_t setDynamicRet = a1_init;
        if (uint8_t *p = getMemPtr(rdram, puVar4 + 8))
            setDynamicRet = *reinterpret_cast<uint32_t *>(p);
        mpegGuestWrite32(rdram, puVar4 + 12, setDynamicRet);

        setReturnU32(ctx, setDynamicRet);
        if (mpegTraceEnabled())
            std::cerr << "[MPEG:Create] mpeg=0x" << std::hex << param_1
                      << " work=0x" << uVar3 << std::dec << " workBytes=" << iVar2_signed
                      << " status=0" << std::endl;
    }

    void sceMpegDelete(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;

        const uint32_t mpegAddr = getRegU32(ctx, 4);
        uint64_t cancelled = 0u;
        {
            std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
            const auto found = g_mpeg_stub_state.playbackByMpeg.find(mpegAddr);
            if (found != g_mpeg_stub_state.playbackByMpeg.end() && found->second.demuxTransaction)
            {
                found->second.demuxTransaction->finished = true;
                cancelled = found->second.demuxTransaction->waitToken;
            }
            g_mpeg_stub_state.callbacksByMpeg.erase(mpegAddr);
            g_mpeg_stub_state.playbackByMpeg.erase(mpegAddr);
        }
        runtime->memory().resetIpuInputConsumption();
        if (cancelled != 0u)
            runtime->eeScheduler().completeExternalWait(kMpegDemuxWaitType, cancelled, KE_WAIT_DELETE);
        runtime->eeScheduler().completeExternalWait(kMpegPictureWaitType, mpegAddr, KE_WAIT_DELETE);
        setReturnU32(ctx, 0u);
    }

    void sceMpegDemuxPss(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        demuxGuestInput(rdram, ctx, runtime, 0u, 0u);
    }

    void sceMpegDemuxPssRing(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_dw5xlMpegDemux.fetch_add(1u, std::memory_order_relaxed); // DW5XL-DEBUG
        demuxGuestInput(rdram, ctx, runtime, getRegU32(ctx, 7), readAbiArg4(rdram, ctx));
    }

    void sceMpegDispCenterOffX(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 0);
    }

    void sceMpegDispCenterOffY(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 0);
    }

    void sceMpegDispHeight(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        const uint32_t mpegAddr = getRegU32(ctx, 4);
        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        setReturnU32(ctx, getPlaybackState(mpegAddr).height);
    }

    void sceMpegDispWidth(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        const uint32_t mpegAddr = getRegU32(ctx, 4);
        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        setReturnU32(ctx, getPlaybackState(mpegAddr).width);
    }

    void sceMpegGetDecodeMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        const uint32_t mpegAddr = getRegU32(ctx, 4);
        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        setReturnU32(ctx, getPlaybackState(mpegAddr).decodeMode);
    }

    uint64_t moviePictureCount() noexcept
    {
        return g_moviePictures.load(std::memory_order_relaxed);
    }

    void sceMpegGetPicture(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_dw5xlMpegGetPic.fetch_add(1u, std::memory_order_relaxed); // DW5XL-DEBUG
        const uint32_t mpegAddr = getRegU32(ctx, 4);
        const uint32_t imageAddr = getRegU32(ctx, 5);
        uint32_t width = kStubMovieWidth;
        uint32_t height = kStubMovieHeight;
        uint32_t frameCount = 0u;
        bool haveFrame = false;
        MpegDecodedFrame frame;
        std::vector<MpegStreamCallbackEvent> pictureCallbacks;
        {
            std::unique_lock<std::mutex> lock(g_mpeg_stub_mutex);
            MpegPlaybackState &playback = getPlaybackState(mpegAddr);
            if (playback.decodedFrames.empty() &&
                !g_mpeg_stub_state.currentCdStreamEofSeen &&
                !playback.streamEnded &&
                !playback.sequenceEndDecoded &&
                !playback.decoderFailed)
            {
                static std::atomic<uint64_t> waitCount{0u};
                if (mpegTraceSample(waitCount.fetch_add(1u, std::memory_order_relaxed)))
                    std::cerr << "[MPEG:GetPictureWait] mpeg=0x" << std::hex << mpegAddr << std::dec
                              << " sawInput=" << playback.sawInput << std::endl;
                if (g_mpeg_stub_state.getPictureWaitTraceCount < 32u)
                {
                    PS2_IF_AGRESSIVE_LOGS({
                        std::cerr << "[MPEG:GetPicture] waiting for frames, mpeg=0x" << std::hex << mpegAddr
                                  << std::dec << " ended=" << playback.streamEnded
                                  << " failed=" << playback.decoderFailed
                                  << " sawInput=" << playback.sawInput << std::endl;
                    });
                    ++g_mpeg_stub_state.getPictureWaitTraceCount;
                }
                lock.unlock();
                runtime->eeScheduler().waitExternal(
                    EeWaitReason::Mpeg,
                    kMpegPictureWaitType,
                    mpegAddr,
                    [rdram, runtime](R5900Context &resumeContext)
                    {
                        if (static_cast<int32_t>(getRegU32(&resumeContext, 2)) < 0)
                        {
                            return;
                        }
                        sceMpegGetPicture(rdram, &resumeContext, runtime);
                    });
            }

            if (!playback.decodedFrames.empty())
            {
                const uint64_t currentTick = runtime->eeScheduler().currentVSyncTick();
                const uint64_t currentTickQ32 = currentTick << 32u;
                const MpegDecodedFrame &nextFrame = playback.decodedFrames.front();
                const bool pal = runtime->eeScheduler().palVideoMode();
                const uint64_t frameIntervalQ32 = decodedFrameIntervalQ32(playback, nextFrame, pal);
                uint64_t presentationTargetQ32 = presentationTickForFrame(playback, nextFrame, currentTickQ32, pal);

                if (currentTickQ32 > presentationTargetQ32 && currentTickQ32 - presentationTargetQ32 >= frameIntervalQ32)
                {
                    const uint64_t correction = currentTickQ32 - presentationTargetQ32;
                    presentationTargetQ32 = currentTickQ32;
                    if (nextFrame.pts90k >= 0 && playback.firstPresentedPts90k >= 0)
                    {
                        playback.ptsPresentationBaseTickQ32 += correction;
                    }
                    playback.nextPictureTickQ32 = currentTickQ32;
                }

                if (currentTickQ32 < presentationTargetQ32)
                {
                    const uint64_t eligibleTick = (presentationTargetQ32 + kPictureClockOne - 1u) >> 32u;
                    lock.unlock();
                    runtime->eeScheduler().waitVSync(
                        eligibleTick - 1u,
                        -1,
                        [rdram, runtime](R5900Context &resumeContext)
                        {
                            if (static_cast<int32_t>(getRegU32(&resumeContext, 2)) < 0)
                            {
                                return;
                            }
                            sceMpegGetPicture(rdram, &resumeContext, runtime);
                        });
                }

                frame = std::move(playback.decodedFrames.front());
                playback.decodedFrames.pop_front();
                playback.width = static_cast<uint32_t>(frame.width);
                playback.height = static_cast<uint32_t>(frame.height);
                width = playback.width;
                height = playback.height;
                frameCount = playback.picturesServed;
                playback.picturesServed += 1u;
                playback.nextPictureTickQ32 = presentationTargetQ32 + frameIntervalQ32;
                playback.presentationEndTickQ32 = playback.nextPictureTickQ32;
                haveFrame = true;
                playback.lastOutputServiceTick = currentTick;
                // The SDK emits callback type 4 during decoded picture DMA
                // output. Its tuple contains the callback type; the registered
                // user data is the third argument. Retail movie clients use
                // this service boundary to pump their accompanying audio.
                MpegStreamCallbackEvent update{};
                update.mpegAddr = mpegAddr;
                update.callbackGeneration = playback.callbackGeneration;
                update.streamType = 4u;
                update.callbacks = matchingOrdinaryCallbacks(mpegAddr, 4u);
                if (!update.callbacks.empty())
                    pictureCallbacks.push_back(std::move(update));
                // The SDK services NODATA after the picture's output DMA.
                // Let the registered input consumer reconcile real DMA
                // progress and prime the next batch after UPDATE completes.
                MpegStreamCallbackEvent input{};
                input.mpegAddr = mpegAddr;
                input.callbackGeneration = playback.callbackGeneration;
                input.streamType = 1u;
                input.callbacks = matchingOrdinaryCallbacks(mpegAddr, 1u);
                if (!input.callbacks.empty())
                    pictureCallbacks.push_back(std::move(input));
                if (g_mpeg_stub_state.pictureTraceCount < 32u)
                {
                    PS2_IF_AGRESSIVE_LOGS({
                        std::cerr << "[MPEG:GetPicture:FRAME] mpeg=0x" << std::hex << mpegAddr
                                  << std::dec << " generation=" << g_mpeg_stub_state.cdStreamGeneration
                                  << " frame=" << frameCount
                                  << " queued=" << playback.decodedFrames.size()
                                  << " size=" << width << "x" << height << std::endl;
                    });
                    ++g_mpeg_stub_state.pictureTraceCount;
                }
            }
            else
            {
                width = playback.width;
                height = playback.height;
                frameCount = playback.picturesServed;
            }
            // This SDK word reports decoder handoff, not the final scanout
            // time. The retail consumer drains its own output queue after it
            // observes completion; sceMpegIsEnd retains host presentation
            // timing for callers which use the native query directly.
            publishSdkDecoderStatus(playback);
            static std::atomic<uint64_t> pictureCount{0u};
            if (mpegTraceSample(pictureCount.fetch_add(1u, std::memory_order_relaxed)))
                std::cerr << "[MPEG:GetPicture] mpeg=0x" << std::hex << mpegAddr << std::dec
                          << " frame=" << frameCount << " haveFrame=" << haveFrame
                          << " remaining=" << playback.decodedFrames.size()
                          << " eof=" << g_mpeg_stub_state.currentCdStreamEofSeen
                          << " sdkComplete=" << decoderComplete(playback)
                          << " size=" << width << 'x' << height << std::endl;
        }

        mpegGuestWrite32(rdram, mpegAddr + 0x00u, width);
        mpegGuestWrite32(rdram, mpegAddr + 0x04u, height);
        mpegGuestWrite32(rdram, mpegAddr + 0x08u, frameCount);

        if (uint8_t *base = getMemPtr(rdram, mpegAddr))
        {
            const uint32_t iVar1 = *reinterpret_cast<uint32_t *>(base + 0x40);
            if (uint8_t *inner = getMemPtr(rdram, iVar1))
            {
                *reinterpret_cast<uint32_t *>(inner + 0xb0) = 1;
                *reinterpret_cast<uint32_t *>(inner + 0xd8) = (getRegU32(ctx, 5) & 0x0FFFFFFFu) | 0x20000000u;
                *reinterpret_cast<uint32_t *>(inner + 0xe4) = getRegU32(ctx, 6);
                *reinterpret_cast<uint32_t *>(inner + 0xdc) = 0;
                *reinterpret_cast<uint32_t *>(inner + 0xe0) = 0;
            }
        }

        if (haveFrame)
        {
            writeDecodedFrameToGuest(rdram, imageAddr, frame);
            g_moviePictures.fetch_add(1u, std::memory_order_relaxed);
            dispatchStreamCallbacksUnlocked(rdram, ctx, runtime, pictureCallbacks);
        }
        else if (frameCount == 0u)
        {
            writeBlankMpegFrame(rdram, imageAddr, width, height);
        }

        setReturnS32(ctx, 0);
    }

    void sceMpegGetPictureRAW8(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceMpegGetPictureRAW8", rdram, ctx, runtime);
    }

    void sceMpegGetPictureRAW8xy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceMpegGetPictureRAW8xy", rdram, ctx, runtime);
    }

    void sceMpegInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        std::vector<uint64_t> cancelled;
        {
        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        for (auto &[mpegAddr, playback] : g_mpeg_stub_state.playbackByMpeg)
            if (playback.demuxTransaction)
            {
                playback.demuxTransaction->finished = true;
                cancelled.push_back(playback.demuxTransaction->waitToken);
            }
        const uint64_t cdStreamGeneration = g_mpeg_stub_state.cdStreamGeneration;
        const uint64_t cdStreamBytesProduced = g_mpeg_stub_state.cdStreamBytesProduced;
        const uint64_t cdStreamBytesDemuxed = g_mpeg_stub_state.cdStreamBytesDemuxed;
        const bool cdStreamEofPending = g_mpeg_stub_state.cdStreamEofPending;
        const bool currentCdStreamEofSeen = g_mpeg_stub_state.currentCdStreamEofSeen;
        resetMpegStubStateUnlocked();
        g_mpeg_stub_state.initialized = true;
        g_mpeg_stub_state.cdStreamGeneration = cdStreamGeneration;
        g_mpeg_stub_state.cdStreamBytesProduced = cdStreamBytesProduced;
        g_mpeg_stub_state.cdStreamBytesDemuxed = cdStreamBytesDemuxed;
        g_mpeg_stub_state.cdStreamEofPending = cdStreamEofPending;
        g_mpeg_stub_state.currentCdStreamEofSeen = currentCdStreamEofSeen;
        }
        if (runtime)
        {
            runtime->memory().resetIpuInputConsumption();
            for (uint64_t token : cancelled)
                runtime->eeScheduler().completeExternalWait(kMpegDemuxWaitType, token, KE_WAIT_DELETE);
        }
        setReturnU32(ctx, 0u);
    }

    void sceMpegIsEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        const uint32_t mpegAddr = getRegU32(ctx, 4);

        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        g_mpeg_stub_state.initialized = true;
        MpegPlaybackState &playback = getPlaybackState(mpegAddr);
        // A committed program_end_code or producer/demux EOF is authoritative.
        // A video sequence_end_code can precede more PSS data, and a decoder
        // failure before program/producer end may recover on a later sequence.
        const bool producerEnded =
            playback.cdStreamGeneration == g_mpeg_stub_state.cdStreamGeneration &&
            (playback.acceptedProgramEnd || g_mpeg_stub_state.currentCdStreamEofSeen);
        const bool ended = producerEnded &&
                           (playback.streamEnded || (playback.decoderFailed && playback.sawInput));
        const uint64_t presentationEnd = playback.presentationEndTickQ32;
        const uint64_t currentTickQ32 = runtime != nullptr
                                            ? (runtime->eeScheduler().currentVSyncTick() << 32u)
                                            : std::numeric_limits<uint64_t>::max();
        const bool presentationComplete =
            presentationEnd == std::numeric_limits<uint64_t>::max() ||
            currentTickQ32 >= presentationEnd;

        publishSdkDecoderStatus(playback);
        static std::atomic<uint64_t> queryCount{0u};
        if (mpegTraceSample(queryCount.fetch_add(1u, std::memory_order_relaxed)))
            std::cerr << "[MPEG:IsEnd] mpeg=0x" << std::hex << mpegAddr << std::dec
                      << " producerEof=" << producerEnded << " streamEnded=" << playback.streamEnded
                      << " frames=" << playback.decodedFrames.size()
                      << " sdkComplete=" << decoderComplete(playback)
                      << " presentationComplete=" << presentationComplete << std::endl;

        if (g_mpeg_stub_state.isEndTraceCount < 16u)
        {
            PS2_IF_AGRESSIVE_LOGS({
                std::cerr << "[MPEG:IsEnd] mpeg=0x" << std::hex << mpegAddr << std::dec
                          << " ended=" << ended
                          << " producerEof=" << producerEnded
                          << " seqEnd=" << playback.sawSequenceEnd
                          << " streamEnded=" << playback.streamEnded
                          << " presentationComplete=" << presentationComplete
                          << " frames=" << playback.decodedFrames.size()
                          << " sawInput=" << playback.sawInput << std::endl;
            });
            ++g_mpeg_stub_state.isEndTraceCount;
        }

        setReturnS32(ctx, (ended && playback.decodedFrames.empty() && presentationComplete) ? 1 : 0);
    }

    void sceMpegIsRefBuffEmpty(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        const uint32_t mpegAddr = getRegU32(ctx, 4);
        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        const MpegPlaybackState &playback = getPlaybackState(mpegAddr);
        setReturnS32(ctx, playback.decodedFrames.empty() ? 1 : 0);
    }

    void sceMpegReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t param_1 = getRegU32(ctx, 4);
        uint64_t cancelled = 0u;
        {
            std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
            MpegPlaybackState &playback = getPlaybackState(param_1);
            if (playback.demuxTransaction)
            {
                playback.demuxTransaction->finished = true;
                cancelled = playback.demuxTransaction->waitToken;
            }
            MpegPlaybackState resetState = makeFreshPlaybackStatePreservingConfig(playback);
            if (playback.streamEnded || playback.decoderFailed)
            {
                resetState.sawInput = true;
                resetState.streamEnded = true;
                resetState.cdStreamGeneration = playback.cdStreamGeneration;
            }
            playback = std::move(resetState);
        }
        if (runtime)
        {
            runtime->memory().resetIpuInputConsumption();
            if (cancelled != 0u)
                runtime->eeScheduler().completeExternalWait(kMpegDemuxWaitType, cancelled, KE_WAIT_DELETE);
        }
        uint8_t *base = getMemPtr(rdram, param_1);
        if (!base)
        {
            return;
        }
        uint32_t inner = *reinterpret_cast<uint32_t *>(base + 0x40);
        if (inner == 0u)
            return;
        mpegGuestWrite32(rdram, param_1 + 0x00u, 0u);
        mpegGuestWrite32(rdram, param_1 + 0x04u, 0u);
        mpegGuestWrite32(rdram, param_1 + 0x08u, 0u);
        mpegGuestWrite32(rdram, inner + 0x00, 0u);
        mpegGuestWrite32(rdram, inner + 0x04, 0u);
        mpegGuestWrite32(rdram, inner + 0x08, 0u);
        mpegGuestWrite32(rdram, param_1 + 0x08, 0u);
        mpegGuestWrite32(rdram, inner + 0x80, 0xFFFFFFFFu);
        mpegGuestWrite32(rdram, inner + 0xAC, 0u);
        mpegGuestWrite32(rdram, 0x171904u, 0u);
    }

    void sceMpegResetDefaultPtsGap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 0);
    }

    void sceMpegSetDecodeMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        const uint32_t mpegAddr = getRegU32(ctx, 4);
        const uint32_t mode = getRegU32(ctx, 5);
        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        getPlaybackState(mpegAddr).decodeMode = mode;
        setReturnS32(ctx, 0);
    }

    void sceMpegSetDefaultPtsGap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 0);
    }

    void sceMpegSetImageBuff(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        const uint32_t mpegAddr = getRegU32(ctx, 4);
        const uint32_t imageBufferAddr = getRegU32(ctx, 5);
        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        getPlaybackState(mpegAddr).imageBufferAddr = imageBufferAddr;
        setReturnS32(ctx, 0);
    }
}
