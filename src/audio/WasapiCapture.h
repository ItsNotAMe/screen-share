#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

namespace screenshare {

enum class AudioCaptureSource {
    SystemOutput,
    Microphone,
};

struct AudioDeviceInfo {
    AudioCaptureSource source = AudioCaptureSource::SystemOutput;
    std::wstring id;
    std::wstring name;
    bool isDefault = false;
};

struct AudioCaptureConfig {
    AudioCaptureSource source = AudioCaptureSource::SystemOutput;
    std::wstring deviceId;
    std::chrono::milliseconds bufferDuration{100};
};

struct AudioCaptureFormat {
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint16_t bitsPerSample = 0;
    uint16_t blockAlign = 0;
    std::string sampleFormat;
};

enum class AudioSampleKind {
    Unsupported,
    Float32,
    Pcm16,
    Pcm24,
    Pcm32,
};

struct CapturedAudioPacket {
    uint32_t frames = 0;
    uint64_t devicePosition = 0;
    uint64_t qpcPosition = 0;
    bool silent = false;
    bool dataDiscontinuity = false;
    bool timestampError = false;
    std::vector<std::byte> data;
    uint64_t samplesAnalyzed = 0;
    double peak = 0.0;
    double rms = 0.0;
};

class WasapiCapture {
public:
    WasapiCapture();
    ~WasapiCapture();

    WasapiCapture(const WasapiCapture&) = delete;
    WasapiCapture& operator=(const WasapiCapture&) = delete;

    static std::vector<AudioDeviceInfo> EnumerateDevices(AudioCaptureSource source);

    void Start(const AudioCaptureConfig& config);
    void Stop();

    [[nodiscard]] std::optional<CapturedAudioPacket> CapturePacket(std::chrono::milliseconds timeout);
    [[nodiscard]] const AudioCaptureFormat& format() const noexcept { return format_; }
    [[nodiscard]] const std::wstring& deviceId() const noexcept { return deviceId_; }
    [[nodiscard]] const std::wstring& deviceName() const noexcept { return deviceName_; }
    [[nodiscard]] uint32_t bufferFrames() const noexcept { return bufferFrames_; }

private:
    void InitializeCom();
    void UninitializeCom();

    AudioCaptureConfig config_{};
    AudioCaptureFormat format_{};
    AudioSampleKind sampleKind_ = AudioSampleKind::Unsupported;
    std::wstring deviceId_;
    std::wstring deviceName_;
    uint32_t bufferFrames_ = 0;
    bool comInitialized_ = false;
    bool started_ = false;

    Microsoft::WRL::ComPtr<IAudioClient> audioClient_;
    Microsoft::WRL::ComPtr<IAudioCaptureClient> captureClient_;
};

AudioCaptureSource ParseAudioCaptureSource(const std::string& value);
const char* AudioCaptureSourceName(AudioCaptureSource source);
std::string AudioCaptureFormatName(const AudioCaptureFormat& format);

} // namespace screenshare
