#pragma once

#include "transport/UdpProtocol.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

namespace screenshare {

struct AudioPlaybackFormat {
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint16_t bitsPerSample = 0;
    uint16_t blockAlign = 0;
    udp_protocol::AudioSampleFormat sampleFormat = udp_protocol::AudioSampleFormat::Unknown;
};

struct AudioPlaybackConfig {
    AudioPlaybackFormat format;
    std::wstring deviceId;
    std::chrono::milliseconds bufferDuration{100};
    float volume = 1.0f;
    bool muted = false;
};

struct AudioPlaybackStats {
    uint64_t packetsRendered = 0;
    uint64_t framesRendered = 0;
    uint64_t bufferFullEvents = 0;
    uint32_t bufferFrames = 0;
    uint32_t lastPaddingFrames = 0;
};

class WasapiRenderer {
public:
    WasapiRenderer();
    ~WasapiRenderer();

    WasapiRenderer(const WasapiRenderer&) = delete;
    WasapiRenderer& operator=(const WasapiRenderer&) = delete;

    void Start(const AudioPlaybackConfig& config);
    void Stop();
    void SetVolume(float volume) noexcept;
    void SetMuted(bool muted) noexcept;

    [[nodiscard]] bool RenderPacket(std::span<const std::byte> bytes, uint32_t frames, bool silent);
    [[nodiscard]] uint32_t AvailableFrames();

    [[nodiscard]] bool started() const noexcept { return started_; }
    [[nodiscard]] float volume() const noexcept { return volume_; }
    [[nodiscard]] bool muted() const noexcept { return muted_; }
    [[nodiscard]] const AudioPlaybackFormat& format() const noexcept { return format_; }
    [[nodiscard]] const std::wstring& deviceName() const noexcept { return deviceName_; }
    [[nodiscard]] const std::wstring& deviceId() const noexcept { return deviceId_; }
    [[nodiscard]] uint32_t bufferFrames() const noexcept { return bufferFrames_; }
    [[nodiscard]] const AudioPlaybackStats& stats() const noexcept { return stats_; }

private:
    void InitializeCom();
    void UninitializeCom();

    AudioPlaybackConfig config_{};
    AudioPlaybackFormat format_{};
    std::wstring deviceId_;
    std::wstring deviceName_;
    uint32_t bufferFrames_ = 0;
    float volume_ = 1.0f;
    bool muted_ = false;
    bool comInitialized_ = false;
    bool started_ = false;

    AudioPlaybackStats stats_{};
    Microsoft::WRL::ComPtr<IAudioClient> audioClient_;
    Microsoft::WRL::ComPtr<IAudioRenderClient> renderClient_;
};

std::string AudioPlaybackFormatName(const AudioPlaybackFormat& format);
bool SameAudioPlaybackFormat(const AudioPlaybackFormat& lhs, const AudioPlaybackFormat& rhs) noexcept;

} // namespace screenshare
