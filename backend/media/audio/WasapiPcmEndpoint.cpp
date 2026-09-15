#include "media/audio/WasapiPcmEndpoint.h"
#include <algorithm>
#include <cstring>
#include <deque>
#include <stdexcept>

namespace screenshare::media {
namespace {
void Check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("WASAPI PCM operation failed"); }
class Capture final : public PcmCaptureEndpoint {
public:
    explicit Capture(AudioCaptureConfig config) : config_(std::move(config)) {}
    void Start() override {
        config_.pcm48kStereo = true;
        config_.bufferDuration = std::chrono::milliseconds(10);
        capture_.Start(config_);
        if (capture_.format().sampleRate != 48000 || capture_.format().channels != 2 || capture_.format().bitsPerSample != 16)
            throw std::runtime_error("WASAPI did not accept PCM48 stereo");
    }
    bool Read(PcmBlock& output, std::stop_token stop) override {
        while (pending_.size() < output.size() && !stop.stop_requested()) {
            auto packet = capture_.CapturePacket(std::chrono::milliseconds(10));
            if (!packet) continue;
            if (packet->dataDiscontinuity) { dropped_ += pending_.size() / 2; pending_.clear(); }
            if (packet->data.size() != size_t(packet->frames) * 4) throw std::runtime_error("Invalid capture PCM packet");
            // At most 30 ms of application capture data, even after a scheduling stall.
            const size_t samples = size_t(packet->frames) * 2;
            const size_t keep = std::min<size_t>(samples, 2880);
            dropped_ += (samples - keep) / 2;
            while (pending_.size() + keep > 2880) { pending_.pop_front(); pending_.pop_front(); ++dropped_; }
            for (size_t i = samples - keep; i < samples; ++i) {
                int16_t value = 0;
                if (!packet->silent) std::memcpy(&value, packet->data.data() + i * 2, 2);
                pending_.push_back(value);
            }
        }
        if (stop.stop_requested()) return false;
        for (auto& value : output) { value = pending_.front(); pending_.pop_front(); }
        return true;
    }
    uint32_t DelayMs() const override { return (capture_.bufferFrames() + uint32_t(pending_.size() / 2)) / 48; }
    uint64_t DroppedFrames() const override { return dropped_; }
private:
    AudioCaptureConfig config_;
    WasapiCapture capture_;
    std::deque<int16_t> pending_;
    uint64_t dropped_ = 0;
};

class Playout final : public PcmPlayoutEndpoint {
public:
    explicit Playout(std::wstring device) : deviceId_(std::move(device)) {}
    ~Playout() override {
        if (client_) client_->Stop();
        render_.Reset(); client_.Reset();
        if (event_) CloseHandle(event_);
        if (com_) CoUninitialize();
    }
    void Start() override {
        Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); com_ = true;
        Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
        Check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)));
        Microsoft::WRL::ComPtr<IMMDevice> device;
        if (deviceId_.empty()) Check(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device));
        else Check(enumerator->GetDevice(deviceId_.c_str(), &device));
        Check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client_.GetAddressOf())));
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM; format.nChannels = 2; format.nSamplesPerSec = 48000;
        format.wBitsPerSample = 16; format.nBlockAlign = 4; format.nAvgBytesPerSec = 192000;
        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        Microsoft::WRL::ComPtr<IAudioClient3> lowPeriod;
        HRESULT initialized = E_FAIL;
        if (SUCCEEDED(client_.As(&lowPeriod))) {
            UINT32 normal, fundamental, minimum, maximum;
            if (SUCCEEDED(lowPeriod->GetSharedModeEnginePeriod(&format, &normal, &fundamental, &minimum, &maximum)))
                initialized = lowPeriod->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minimum, &format, nullptr);
        }
        if (FAILED(initialized)) {
            // Re-activate after a rejected low-period initialization.
            lowPeriod.Reset(); client_.Reset();
            Check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client_.GetAddressOf())));
            Check(client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, &format, nullptr));
        }
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event_) throw std::runtime_error("WASAPI render event creation failed");
        Check(client_->SetEventHandle(event_));
        Check(client_->GetBufferSize(&bufferFrames_));
        if (SUCCEEDED(client_.As(&lowPeriod))) {
            WAVEFORMATEX* currentFormat = nullptr;
            UINT32 period = 0;
            if (SUCCEEDED(lowPeriod->GetCurrentSharedModeEnginePeriod(&currentFormat, &period)) && currentFormat && currentFormat->nSamplesPerSec)
                enginePeriodUs_ = uint32_t(uint64_t(period) * 1000000 / currentFormat->nSamplesPerSec);
            CoTaskMemFree(currentFormat);
        }
        Check(client_->GetService(IID_PPV_ARGS(&render_)));
        Check(client_->Start());
    }
    void Write(const PcmBlock& block, std::stop_token stop) override {
        uint32_t offset = 0;
        while (offset < 480 && !stop.stop_requested()) {
            UINT32 padding;
            Check(client_->GetCurrentPadding(&padding));
            delayMs_ = padding / 48;
            const auto frames = std::min(480u - offset, bufferFrames_ - std::min(padding, bufferFrames_));
            if (frames) {
                BYTE* destination;
                Check(render_->GetBuffer(frames, &destination));
                std::memcpy(destination, block.data() + offset * 2, frames * 4);
                Check(render_->ReleaseBuffer(frames, 0));
                offset += frames;
            } else {
                const auto result = WaitForSingleObject(event_, 10);
                if (result != WAIT_OBJECT_0 && result != WAIT_TIMEOUT) throw std::runtime_error("WASAPI render wait failed");
            }
        }
    }
    uint32_t DelayMs() const override { return delayMs_; }
    uint32_t BufferFrames() const override { return bufferFrames_; }
    uint32_t EnginePeriodUs() const override { return enginePeriodUs_; }
private:
    std::wstring deviceId_;
    Microsoft::WRL::ComPtr<IAudioClient> client_;
    Microsoft::WRL::ComPtr<IAudioRenderClient> render_;
    HANDLE event_ = nullptr;
    bool com_ = false;
    uint32_t bufferFrames_ = 0, delayMs_ = 0, enginePeriodUs_ = 0;
};
}
PcmEndpointFactories WasapiPcmEndpoints(AudioCaptureConfig capture, std::wstring playbackDeviceId) {
    return {[capture] { return std::make_unique<Capture>(capture); },
        [playbackDeviceId] { return std::make_unique<Playout>(playbackDeviceId); }};
}
}
