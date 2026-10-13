#include "runtime/ps2_audio.h"
#include "runtime/ps2_memory.h"
#include "ps2_host_backend.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{
    std::atomic<PS2AudioBackend *> g_pcmBackend{nullptr};

    // Single-consumer rings. submitPcm serializes producers; the device callback
    // only copies frames and advances its cursor, without locks or allocation.
    template <size_t CapacityFrames>
    struct PcmRing
    {
        static constexpr size_t CapacitySamples = CapacityFrames * 2;
        std::array<int16_t, CapacitySamples> samples{};
        std::atomic<uint64_t> read{0}, write{0};

        void reset()
        {
            read.store(0, std::memory_order_relaxed);
            write.store(0, std::memory_order_relaxed);
        }
        size_t push(std::span<const int16_t> input)
        {
            const uint64_t end = write.load(std::memory_order_relaxed);
            const uint64_t begin = read.load(std::memory_order_acquire);
            const size_t count = std::min(input.size(), CapacitySamples - static_cast<size_t>(end - begin)) & ~size_t{1};
            if (count == 0) return 0;
            const size_t index = static_cast<size_t>(end % CapacitySamples);
            const size_t first = std::min(count, CapacitySamples - index);
            std::copy_n(input.data(), first, samples.data() + index);
            std::copy_n(input.data() + first, count - first, samples.data());
            write.store(end + count, std::memory_order_release);
            return count;
        }
        size_t pop(std::span<int16_t> output)
        {
            const uint64_t begin = read.load(std::memory_order_relaxed);
            const uint64_t end = write.load(std::memory_order_acquire);
            const size_t count = std::min(output.size(), static_cast<size_t>(end - begin)) & ~size_t{1};
            if (count == 0) return 0;
            const size_t index = static_cast<size_t>(begin % CapacitySamples);
            const size_t first = std::min(count, CapacitySamples - index);
            std::copy_n(samples.data() + index, first, output.data());
            std::copy_n(samples.data(), count - first, output.data() + first);
            read.store(begin + count, std::memory_order_release);
            return count;
        }
        uint32_t queuedFrames() const
        {
            const uint64_t begin = read.load(std::memory_order_acquire);
            const uint64_t end = write.load(std::memory_order_acquire);
            return static_cast<uint32_t>(std::min<uint64_t>(end >= begin ? (end - begin) / 2 : 0, CapacityFrames));
        }
    };

    void writePcmWavHeader(std::FILE *file, uint64_t bytes)
    {
        const uint32_t dataSize = static_cast<uint32_t>(std::min<uint64_t>(bytes, UINT32_MAX - 36u));
        std::array<uint8_t, 44> header{};
        std::memcpy(header.data(), "RIFF", 4);
        std::memcpy(header.data() + 8, "WAVEfmt ", 8);
        std::memcpy(header.data() + 36, "data", 4);
        auto word = [&](size_t offset, uint32_t value) {
            for (unsigned i = 0; i < 4; ++i) header[offset + i] = static_cast<uint8_t>(value >> (i * 8));
        };
        word(4, dataSize + 36);
        word(16, 16);
        header[20] = 1;
        header[22] = 2;
        word(24, 48000);
        word(28, 48000 * 4);
        header[32] = 4;
        header[34] = 16;
        word(40, dataSize);
        std::fseek(file, 0, SEEK_SET);
        std::fwrite(header.data(), 1, header.size(), file);
    }

    std::vector<uint8_t> buildWavFromPcm(const int16_t *pcm, size_t sampleCount, uint32_t sampleRate)
    {
        const uint32_t dataSize = static_cast<uint32_t>(sampleCount * 2);
        const uint32_t fileSize = 36 + dataSize;
        std::vector<uint8_t> wav(8 + fileSize);

        uint8_t *p = wav.data();
        p[0] = 'R';
        p[1] = 'I';
        p[2] = 'F';
        p[3] = 'F';
        p[4] = static_cast<uint8_t>(fileSize);
        p[5] = static_cast<uint8_t>(fileSize >> 8);
        p[6] = static_cast<uint8_t>(fileSize >> 16);
        p[7] = static_cast<uint8_t>(fileSize >> 24);
        p[8] = 'W';
        p[9] = 'A';
        p[10] = 'V';
        p[11] = 'E';
        p[12] = 'f';
        p[13] = 'm';
        p[14] = 't';
        p[15] = ' ';
        p[16] = 16;
        p[17] = 0;
        p[18] = 0;
        p[19] = 0;
        p[20] = 1;
        p[21] = 0;
        p[22] = 1;
        p[23] = 0;
        p[24] = static_cast<uint8_t>(sampleRate);
        p[25] = static_cast<uint8_t>(sampleRate >> 8);
        p[26] = static_cast<uint8_t>(sampleRate >> 16);
        p[27] = static_cast<uint8_t>(sampleRate >> 24);
        const uint32_t byteRate = sampleRate * 2;
        p[28] = static_cast<uint8_t>(byteRate);
        p[29] = static_cast<uint8_t>(byteRate >> 8);
        p[30] = static_cast<uint8_t>(byteRate >> 16);
        p[31] = static_cast<uint8_t>(byteRate >> 24);
        p[32] = 2;
        p[33] = 0;
        p[34] = 16;
        p[35] = 0;
        p[36] = 'd';
        p[37] = 'a';
        p[38] = 't';
        p[39] = 'a';
        p[40] = static_cast<uint8_t>(dataSize);
        p[41] = static_cast<uint8_t>(dataSize >> 8);
        p[42] = static_cast<uint8_t>(dataSize >> 16);
        p[43] = static_cast<uint8_t>(dataSize >> 24);
        std::memcpy(p + 44, pcm, dataSize);
        return wav;
    }
}

namespace ps2_vag
{
    bool decode(const uint8_t *data, uint32_t sizeBytes,
                std::vector<int16_t> &outPcm, uint32_t &outSampleRate);
}

struct PS2AudioBackend::Impl
{
    struct TrackedSound
    {
        Sound snd;
        uint32_t sampleKey;
    };
    std::vector<TrackedSound> activeSounds;
    AudioStream pcmStream{};
    std::mutex producerMutex;
    PcmRing<8192> playback;
    PcmRing<262144> capture;
    PcmRing<262144> sourceCapture;
    std::atomic<bool> running{false}, captureEnabled{false}, sourceCaptureEnabled{false};
    bool playbackStarted = false; // UI thread only.
    std::atomic<uint64_t> submittedFrames{0}, consumedFrames{0}, outputFrames{0};
    std::atomic<uint64_t> underrunFrames{0}, droppedFrames{0}, nonzeroSamples{0};
    std::atomic<uint64_t> captureDroppedFrames{0}, sourceCaptureDroppedFrames{0};
    std::atomic<uint32_t> peak{0};
    std::FILE *captureFile = nullptr;
    std::FILE *sourceCaptureFile = nullptr;
    uint64_t captureBytes = 0;
    uint64_t sourceCaptureBytes = 0;
    bool trace = false;
    std::chrono::steady_clock::time_point lastTrace{};
};

PS2AudioBackend::PS2AudioBackend() : m_impl(std::make_unique<Impl>())
{
}

PS2AudioBackend::~PS2AudioBackend()
{
    if (m_impl)
    {
        stopPcmStream();
        stopAll();
    }
}

bool PS2AudioBackend::startPcmStream()
{
#if defined(PLATFORM_VITA)
    return false;
#else
    if (m_impl->running.load(std::memory_order_acquire)) return true;
    if (!m_audioReady.load(std::memory_order_acquire) || !IsAudioDeviceReady()) return false;
    PS2AudioBackend *expected = nullptr;
    if (!g_pcmBackend.compare_exchange_strong(expected, this, std::memory_order_acq_rel)) return false;
    auto &state = *m_impl;
    {
        std::lock_guard<std::mutex> lock(state.producerMutex);
        state.playback.reset();
        state.capture.reset();
        state.sourceCapture.reset();
        state.submittedFrames = state.consumedFrames = state.outputFrames = 0;
        state.underrunFrames = state.droppedFrames = state.nonzeroSamples = 0;
        state.captureDroppedFrames = state.sourceCaptureDroppedFrames = 0;
        state.peak = 0;
    }
    state.pcmStream = LoadAudioStream(48000, 16, 2);
    state.playbackStarted = false;
    if (!IsAudioStreamValid(state.pcmStream))
    {
        g_pcmBackend.store(nullptr, std::memory_order_release);
        state.pcmStream = {};
        return false;
    }
    const char *trace = std::getenv("PS2X_AUDIO_TRACE");
    state.trace = trace && *trace && std::strcmp(trace, "0") != 0;
    state.lastTrace = std::chrono::steady_clock::now();
    const char *dump = std::getenv("PS2X_AUDIO_DUMP");
    if (dump && *dump)
    {
        state.captureFile = std::fopen(dump, "wb");
        state.captureBytes = 0;
        if (state.captureFile)
        {
            writePcmWavHeader(state.captureFile, 0);
            state.captureEnabled.store(true, std::memory_order_release);
        }
        else std::fprintf(stderr, "[spu2-audio] cannot open PCM capture '%s'\n", dump);
    }
    const char *sourceDump = std::getenv("PS2X_AUDIO_SOURCE_DUMP");
    if (sourceDump && *sourceDump)
    {
        if (dump && std::strcmp(sourceDump, dump) == 0)
            std::fprintf(stderr, "[spu2-audio] source capture requires a separate path from device capture\n");
        else
        {
            state.sourceCaptureFile = std::fopen(sourceDump, "wb");
            state.sourceCaptureBytes = 0;
            if (state.sourceCaptureFile)
            {
                writePcmWavHeader(state.sourceCaptureFile, 0);
                state.sourceCaptureEnabled.store(true, std::memory_order_release);
            }
            else std::fprintf(stderr, "[spu2-audio] cannot open source PCM capture '%s'\n", sourceDump);
        }
    }
    SetAudioStreamCallback(state.pcmStream, pcmStreamCallback);
    state.running.store(true, std::memory_order_release);
    return true;
#endif
}

void PS2AudioBackend::pcmStreamCallback(void *buffer, unsigned int frames)
{
    auto *output = static_cast<int16_t *>(buffer);
    const size_t sampleCount = static_cast<size_t>(frames) * 2;
    std::fill_n(output, sampleCount, int16_t{0});
    PS2AudioBackend *backend = g_pcmBackend.load(std::memory_order_acquire);
    if (!backend) return;
    auto &state = *backend->m_impl;
    if (!state.running.load(std::memory_order_acquire)) return;
    const size_t consumed = state.playback.pop(std::span<int16_t>(output, sampleCount));
    state.outputFrames.fetch_add(frames, std::memory_order_relaxed);
    state.consumedFrames.fetch_add(consumed / 2, std::memory_order_relaxed);
    state.underrunFrames.fetch_add(frames - consumed / 2, std::memory_order_relaxed);
    uint64_t nonzero = 0;
    uint32_t peak = 0;
    for (size_t i = 0; i < consumed; ++i)
    {
        const int32_t sample = output[i];
        nonzero += sample != 0;
        peak = std::max(peak, static_cast<uint32_t>(sample < 0 ? -sample : sample));
    }
    state.nonzeroSamples.fetch_add(nonzero, std::memory_order_relaxed);
    uint32_t priorPeak = state.peak.load(std::memory_order_relaxed);
    while (peak > priorPeak && !state.peak.compare_exchange_weak(priorPeak, peak, std::memory_order_relaxed)) { }
    if (state.captureEnabled.load(std::memory_order_acquire))
    {
        const size_t captured = state.capture.push(std::span<const int16_t>(output, sampleCount));
        state.captureDroppedFrames.fetch_add((sampleCount - captured) / 2, std::memory_order_relaxed);
    }
}

void PS2AudioBackend::submitPcm(std::span<const int16_t> stereo48k)
{
    auto &state = *m_impl;
    if (!state.running.load(std::memory_order_acquire) || stereo48k.empty()) return;
    stereo48k = stereo48k.first(stereo48k.size() & ~size_t{1});
    if (stereo48k.empty()) return;
    std::lock_guard<std::mutex> lock(state.producerMutex);
    if (!state.running.load(std::memory_order_acquire)) return;
    const size_t accepted = state.playback.push(stereo48k);
    state.submittedFrames.fetch_add(stereo48k.size() / 2, std::memory_order_relaxed);
    state.droppedFrames.fetch_add((stereo48k.size() - accepted) / 2, std::memory_order_relaxed);
    if (state.sourceCaptureEnabled.load(std::memory_order_acquire))
    {
        const size_t captured = state.sourceCapture.push(stereo48k);
        state.sourceCaptureDroppedFrames.fetch_add((stereo48k.size() - captured) / 2, std::memory_order_relaxed);
    }
}

PS2AudioBackend::PcmStats PS2AudioBackend::pcmStats() const
{
    const auto &state = *m_impl;
    return {state.submittedFrames.load(std::memory_order_relaxed),
            state.consumedFrames.load(std::memory_order_relaxed),
            state.outputFrames.load(std::memory_order_relaxed),
            state.underrunFrames.load(std::memory_order_relaxed),
            state.droppedFrames.load(std::memory_order_relaxed),
            state.nonzeroSamples.load(std::memory_order_relaxed),
            state.captureDroppedFrames.load(std::memory_order_relaxed),
            state.sourceCaptureDroppedFrames.load(std::memory_order_relaxed),
            state.peak.load(std::memory_order_relaxed), state.playback.queuedFrames(),
            state.running.load(std::memory_order_acquire)};
}

void PS2AudioBackend::updatePcmStream()
{
    auto &state = *m_impl;
#if !defined(PLATFORM_VITA)
    if (state.running.load(std::memory_order_acquire) && !state.playbackStarted &&
        state.playback.queuedFrames() >= 4800u && IsAudioDeviceReady())
    {
        // Queue genuine samples before starting the fixed-rate device stream.
        // This reserve absorbs short GS stalls without replay or pitch changes.
        PlayAudioStream(state.pcmStream);
        state.playbackStarted = true;
    }
#endif
    auto flushCapture = [](auto &ring, std::FILE *file, uint64_t &bytes,
                           std::atomic<bool> &enabled, const char *label)
    {
        if (!file) return;
        std::array<int16_t, 8192> samples{};
        for (size_t blocks = 0; blocks < 64; ++blocks)
        {
            const size_t count = ring.pop(samples);
            if (count == 0) break;
            const size_t written = std::fwrite(samples.data(), sizeof(int16_t), count, file);
            bytes += written * sizeof(int16_t);
            if (written != count)
            {
                enabled.store(false, std::memory_order_release);
                std::fprintf(stderr, "[spu2-audio] %s PCM capture write failed\n", label);
                break;
            }
        }
    };
    flushCapture(state.capture, state.captureFile, state.captureBytes, state.captureEnabled, "device");
    flushCapture(state.sourceCapture, state.sourceCaptureFile, state.sourceCaptureBytes,
                 state.sourceCaptureEnabled, "source");
    const auto now = std::chrono::steady_clock::now();
    if (state.trace && now - state.lastTrace >= std::chrono::seconds(1))
    {
        const auto stats = pcmStats();
        std::fprintf(stderr, "[spu2-audio] submitted=%llu consumed=%llu output=%llu underrun=%llu dropped=%llu nonzero=%llu peak=%u queued=%u capture-dropped=%llu source-capture-dropped=%llu\n",
                     static_cast<unsigned long long>(stats.submittedFrames),
                     static_cast<unsigned long long>(stats.consumedFrames),
                     static_cast<unsigned long long>(stats.outputFrames),
                     static_cast<unsigned long long>(stats.underrunFrames),
                     static_cast<unsigned long long>(stats.droppedFrames),
                     static_cast<unsigned long long>(stats.nonzeroSamples), stats.peak, stats.queuedFrames,
                     static_cast<unsigned long long>(stats.captureDroppedFrames),
                     static_cast<unsigned long long>(stats.sourceCaptureDroppedFrames));
        state.lastTrace = now;
    }
}

void PS2AudioBackend::stopPcmStream()
{
    auto &state = *m_impl;
    state.running.store(false, std::memory_order_release);
#if !defined(PLATFORM_VITA)
    if (state.pcmStream.buffer && IsAudioDeviceReady())
    {
        // Both calls take Raylib's device lock, which also surrounds callbacks.
        // Once detached, the callback cannot retain a pointer to this backend.
        StopAudioStream(state.pcmStream);
        SetAudioStreamCallback(state.pcmStream, nullptr);
        UnloadAudioStream(state.pcmStream);
    }
#endif
    PS2AudioBackend *expected = this;
    g_pcmBackend.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
    state.pcmStream = {};
    state.playbackStarted = false;
    state.captureEnabled.store(false, std::memory_order_release);
    {
        // A producer may have passed the running check just before shutdown.
        // Finish its copy before disabling and draining the source capture.
        std::lock_guard<std::mutex> lock(state.producerMutex);
        state.sourceCaptureEnabled.store(false, std::memory_order_release);
    }
    updatePcmStream();
    if (state.captureFile)
    {
        writePcmWavHeader(state.captureFile, state.captureBytes);
        std::fclose(state.captureFile);
        state.captureFile = nullptr;
    }
    if (state.sourceCaptureFile)
    {
        writePcmWavHeader(state.sourceCaptureFile, state.sourceCaptureBytes);
        std::fclose(state.sourceCaptureFile);
        state.sourceCaptureFile = nullptr;
    }
}

void PS2AudioBackend::onVagTransfer(const uint8_t *rdram, uint32_t srcAddr, uint32_t sizeBytes)
{
    if (!rdram || sizeBytes < 48)
        return;

    const uint32_t physAddr = srcAddr & PS2_RAM_MASK;
    if (physAddr + sizeBytes > PS2_RAM_SIZE)
        return;

    std::vector<int16_t> pcm;
    uint32_t sampleRate = 44100;
    if (!ps2_vag::decode(rdram + physAddr, sizeBytes, pcm, sampleRate))
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    DecodedSample sample;
    sample.pcm = std::move(pcm);
    sample.sampleRate = sampleRate;
    m_sampleBank[physAddr] = std::move(sample);
    m_mostRecentSampleKey = physAddr;
}

void PS2AudioBackend::onVagTransferFromBuffer(const uint8_t *data, uint32_t sizeBytes, uint32_t keyAddr)
{
    if (!data || sizeBytes < 48)
        return;

    std::vector<int16_t> pcm;
    uint32_t sampleRate = 44100;
    if (!ps2_vag::decode(data, sizeBytes, pcm, sampleRate))
        return;

    const uint32_t physAddr = keyAddr & PS2_RAM_MASK;
    std::lock_guard<std::mutex> lock(m_mutex);
    DecodedSample sample;
    sample.pcm = std::move(pcm);
    sample.sampleRate = sampleRate;
    m_sampleBank[physAddr] = sample;
    m_mostRecentSampleKey = physAddr;
    m_loadOrderSamples.push_back(std::move(sample));
    m_loadOrderSampleKeys.push_back(physAddr);
    constexpr size_t kMaxLoadOrderSamples = 32;
    if (m_loadOrderSamples.size() > kMaxLoadOrderSamples)
    {
        m_loadOrderSamples.erase(m_loadOrderSamples.begin());
        m_loadOrderSampleKeys.erase(m_loadOrderSampleKeys.begin());
    }
}

namespace
{
    constexpr uint32_t LIBSD_CMD_SET_VOICE = 0x8010u;
}

void PS2AudioBackend::onSoundCommand(uint32_t sid, uint32_t rpcNum,
                                     const uint8_t *sendBuf, uint32_t sendSize,
                                     uint8_t *recvBuf, uint32_t recvSize)
{
    if (sid != 0x80000701u)
        return;

    if ((rpcNum == LIBSD_CMD_SET_VOICE || (rpcNum & 0xFF00u) == 0x8100u) &&
        sendBuf && sendSize >= 20)
    {
        uint32_t sampleAddr = 0;
        uint32_t voiceIndex = 0xFFFFFFFFu;
        for (int vo = 4; vo >= 0 && voiceIndex == 0xFFFFFFFFu; vo -= 4)
        {
            if (vo < static_cast<int>(sendSize))
            {
                uint32_t v = 0;
                std::memcpy(&v, sendBuf + vo, sizeof(v));
                if (v < 24u)
                    voiceIndex = v;
            }
        }

        constexpr uint32_t kMinPlausibleAddr = 0x1000u;
        for (int off = 12; off <= 24 && sampleAddr == 0; off += 4)
        {
            if (sendSize >= static_cast<uint32_t>(off + 4))
            {
                uint32_t cand = 0;
                std::memcpy(&cand, sendBuf + off, sizeof(cand));
                if (cand >= kMinPlausibleAddr && (cand <= PS2_RAM_MASK || (cand & ~PS2_RAM_MASK) == 0))
                    sampleAddr = cand;
            }
        }
        if (sampleAddr == 0)
            sampleAddr = m_mostRecentSampleKey;

        float pitch = 1.0f;
        if (sendSize >= 12)
        {
            uint16_t pitchHalf = 0;
            std::memcpy(&pitchHalf, sendBuf + 8, sizeof(pitchHalf));
            if (pitchHalf != 0)
                pitch = 4096.0f / static_cast<float>(pitchHalf);
        }
        play(sampleAddr, pitch, 1.0f, voiceIndex);
    }
}

void PS2AudioBackend::play(uint32_t sampleAddr, float pitch, float volume, uint32_t voiceIndex)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    DecodedSample *sampleToPlay = nullptr;
    uint32_t sampleKey = 0;

    auto it = m_sampleBank.find(sampleAddr & PS2_RAM_MASK);
    if (it != m_sampleBank.end())
    {
        sampleToPlay = &it->second;
        sampleKey = it->first;
    }
    else if (voiceIndex != 0xFFFFFFFFu &&
             voiceIndex < m_loadOrderSamples.size() &&
             voiceIndex < m_loadOrderSampleKeys.size())
    {
        sampleToPlay = &m_loadOrderSamples[voiceIndex];
        sampleKey = m_loadOrderSampleKeys[voiceIndex];
    }
    else
    {
        it = m_sampleBank.find(m_mostRecentSampleKey);
        if (it == m_sampleBank.end())
            return;
        sampleToPlay = &it->second;
        sampleKey = it->first;
    }
    if (!sampleToPlay || sampleToPlay->pcm.empty())
        return;

    const bool isBgm = (sampleToPlay->pcm.size() > static_cast<size_t>(sampleToPlay->sampleRate * 5));
    playDecodedSample(sampleKey, *sampleToPlay, pitch, volume, isBgm);
}

void PS2AudioBackend::pruneFinishedSounds()
{
#if defined(PLATFORM_VITA)
    return;
#else
    auto &sounds = m_impl->activeSounds;
    auto it = sounds.begin();
    while (it != sounds.end())
    {
        if (!IsSoundPlaying(it->snd))
        {
            UnloadSound(it->snd);
            it = sounds.erase(it);
        }
        else
        {
            ++it;
        }
    }
#endif
}

void PS2AudioBackend::playDecodedSample(uint32_t sampleKey, DecodedSample &sample, float pitch, float volume,
                                        bool isBgm)
{
#if defined(PLATFORM_VITA)
    (void)sampleKey;
    (void)sample;
    (void)pitch;
    (void)volume;
    (void)isBgm;
    return;
#else
    if (!m_audioReady || sample.pcm.empty())
        return;

    pruneFinishedSounds();

    for (const auto &t : m_impl->activeSounds)
    {
        if (t.sampleKey == sampleKey && IsSoundPlaying(t.snd))
            return;
    }

    auto &sounds = m_impl->activeSounds;
    if (isBgm)
    {
        for (auto it = sounds.begin(); it != sounds.end();)
        {
            if (IsSoundPlaying(it->snd))
            {
                StopSound(it->snd);
                UnloadSound(it->snd);
                it = sounds.erase(it);
            }
            else
                ++it;
        }
    }

    constexpr int kMaxConcurrentSounds = 4;
    while (static_cast<int>(sounds.size()) >= kMaxConcurrentSounds)
    {
        StopSound(sounds.front().snd);
        UnloadSound(sounds.front().snd);
        sounds.erase(sounds.begin());
    }

    std::vector<uint8_t> wav = buildWavFromPcm(sample.pcm.data(), sample.pcm.size(), sample.sampleRate);
    Wave wave = LoadWaveFromMemory(".wav", wav.data(), static_cast<int>(wav.size()));
    if (wave.frameCount <= 0)
        return;
    Sound snd = LoadSoundFromWave(wave);
    UnloadWave(wave);
    SetSoundPitch(snd, pitch);
    SetSoundVolume(snd, volume);
    m_impl->activeSounds.push_back({snd, sampleKey});
    PlaySound(snd);
#endif
}

void PS2AudioBackend::stop(uint32_t voiceId)
{
    (void)voiceId;
}

void PS2AudioBackend::stopAll()
{
    std::lock_guard<std::mutex> lock(m_mutex);
#if defined(PLATFORM_VITA)
    return;
#else
    if (!IsAudioDeviceReady())
    {
        m_impl->activeSounds.clear();
        return;
    }
    for (auto &t : m_impl->activeSounds)
    {
        StopSound(t.snd);
        UnloadSound(t.snd);
    }
    m_impl->activeSounds.clear();
#endif
}
