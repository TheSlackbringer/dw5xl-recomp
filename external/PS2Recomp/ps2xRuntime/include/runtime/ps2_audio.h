#ifndef PS2_AUDIO_H
#define PS2_AUDIO_H

#include <cstdint>
#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

class PS2AudioBackend
{
public:
    PS2AudioBackend();
    ~PS2AudioBackend();

    void onVagTransfer(const uint8_t *rdram, uint32_t srcAddr, uint32_t sizeBytes);
    void onVagTransferFromBuffer(const uint8_t *data, uint32_t sizeBytes, uint32_t keyAddr);
    void onSoundCommand(uint32_t sid, uint32_t rpcNum,
                        const uint8_t *sendBuf, uint32_t sendSize,
                        uint8_t *recvBuf, uint32_t recvSize);

    void play(uint32_t sampleAddr, float pitch = 1.0f, float volume = 1.0f,
              uint32_t voiceIndex = 0xFFFFFFFFu);
    void stop(uint32_t voiceId);
    void stopAll();
    void setAudioReady(bool ready) { m_audioReady = ready; }

    struct PcmStats
    {
        uint64_t submittedFrames = 0, consumedFrames = 0, outputFrames = 0;
        uint64_t underrunFrames = 0, droppedFrames = 0, nonzeroSamples = 0;
        uint64_t captureDroppedFrames = 0, sourceCaptureDroppedFrames = 0;
        uint32_t peak = 0, queuedFrames = 0;
        bool running = false;
    };

    // Device/stream lifecycle and optional WAV flushing belong to the UI thread.
    bool startPcmStream();
    void stopPcmStream();
    void updatePcmStream();
    // Actual SPU2 output: interleaved signed 16-bit stereo at 48000 Hz.
    // No device calls are made by the guest-side producer.
    void submitPcm(std::span<const int16_t> stereo48k);
    [[nodiscard]] PcmStats pcmStats() const;

private:
    struct DecodedSample
    {
        std::vector<int16_t> pcm;
        uint32_t sampleRate = 44100;
    };

    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::atomic<bool> m_audioReady{false};
    uint32_t m_mostRecentSampleKey = 0;
    std::vector<DecodedSample> m_loadOrderSamples;
    std::vector<uint32_t> m_loadOrderSampleKeys;
    std::unordered_map<uint32_t, DecodedSample> m_sampleBank;
    std::mutex m_mutex;

    void playDecodedSample(uint32_t sampleKey, DecodedSample &sample, float pitch, float volume,
                          bool isBgm = false);
    void pruneFinishedSounds();
    static void pcmStreamCallback(void *buffer, unsigned int frames);
};

#endif
