#pragma once

#include "audio/WasapiCapture.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace screenshare {

struct OpusEncodedAudioPacket {
    uint32_t sampleRate = 48'000;
    uint16_t channels = 2;
    uint16_t bitsPerSample = 32;
    uint16_t blockAlign = 8;
    uint32_t audioFrames = 0;
    std::vector<std::byte> bytes;
};

class OpusAudioEncoder {
public:
    static constexpr uint32_t OutputSampleRate = 48'000;
    static constexpr uint16_t OutputChannels = 2;
    static constexpr uint16_t OutputBitsPerSample = 32;
    static constexpr uint16_t OutputBlockAlign = OutputChannels * sizeof(float);
    static constexpr uint32_t DefaultBitrate = 128'000;

    OpusAudioEncoder();
    ~OpusAudioEncoder();

    OpusAudioEncoder(const OpusAudioEncoder&) = delete;
    OpusAudioEncoder& operator=(const OpusAudioEncoder&) = delete;

    void Start(const AudioCaptureFormat& inputFormat, uint32_t bitrate = DefaultBitrate);
    void Stop();
    [[nodiscard]] bool isRunning() const noexcept { return encoder_ != nullptr; }
    [[nodiscard]] uint32_t bitrate() const noexcept { return bitrate_; }

    [[nodiscard]] OpusEncodedAudioPacket Encode(const CapturedAudioPacket& packet);

private:
    AudioCaptureFormat inputFormat_{};
    uint32_t bitrate_ = DefaultBitrate;
    void* encoder_ = nullptr;
};

class OpusAudioDecoder {
public:
    static constexpr uint32_t OutputSampleRate = OpusAudioEncoder::OutputSampleRate;
    static constexpr uint16_t OutputChannels = OpusAudioEncoder::OutputChannels;
    static constexpr uint16_t OutputBitsPerSample = OpusAudioEncoder::OutputBitsPerSample;
    static constexpr uint16_t OutputBlockAlign = OpusAudioEncoder::OutputBlockAlign;

    OpusAudioDecoder();
    ~OpusAudioDecoder();

    OpusAudioDecoder(const OpusAudioDecoder&) = delete;
    OpusAudioDecoder& operator=(const OpusAudioDecoder&) = delete;

    void Start();
    void Stop();
    [[nodiscard]] bool isRunning() const noexcept { return decoder_ != nullptr; }

    [[nodiscard]] std::vector<std::byte> Decode(std::span<const std::byte> packet, uint32_t expectedFrames);

private:
    void* decoder_ = nullptr;
};

} // namespace screenshare
