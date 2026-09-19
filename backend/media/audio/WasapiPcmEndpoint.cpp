#include "media/audio/WasapiPcmEndpoint.h"
#include "SilentPcmCapture.h"
#include "StereoDownmix.h"
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
    void Start() override { Start({}); }
    void Start(std::stop_token stop) override {
        config_.pcm48kStereo = false;
        config_.pcm48kNativeChannels = true;
        // Capacity, not a forced capture delay. Match the legacy reserve so a
        // brief scheduling stall does not overwrite the hardware's PCM ring.
        config_.bufferDuration = std::chrono::milliseconds(100);
        capture_.Start(config_, stop);
        if (capture_.format().sampleRate != 48000 || capture_.format().bitsPerSample != 16)
            throw std::runtime_error("WASAPI did not accept PCM48");
        downmix_.emplace(capture_.format().channels, capture_.format().channelMask);
    }
    bool Read(PcmBlock& output, std::stop_token stop) override {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        while (pending_.size() < output.size() && !stop.stop_requested()) {
            auto packet = capture_.CapturePacket(std::chrono::milliseconds(10));
            if (!packet) {
                // Packet/event timing is not the audio sample clock. A single
                // 10ms wait timing out must not insert zeros into a partial block.
                if(std::chrono::steady_clock::now()<deadline)continue;
                // A genuinely silent loopback device may report no packets.
                // The output clock supplies silence while this bounded read waits.
                if (stop.stop_requested()) return false;
                for (auto& value : output) {
                    value = pending_.empty() ? 0 : pending_.front();
                    if (!pending_.empty()) pending_.pop_front();
                }
                return true;
            }
            if (packet->dataDiscontinuity) { dropped_ += pending_.size() / 2; pending_.clear(); }
            const auto inputFrameBytes = size_t(capture_.format().channels) * 2;
            if (packet->data.size() != size_t(packet->frames) * inputFrameBytes) throw std::runtime_error("Invalid capture PCM packet");
            // Preserve up to the native ring capacity, including delayed bursts.
            const size_t samples = size_t(packet->frames) * 2;
            const size_t keep = std::min<size_t>(samples, 9600);
            dropped_ += (samples - keep) / 2;
            while (pending_.size() + keep > 9600) { pending_.pop_front(); pending_.pop_front(); ++dropped_; }
            std::array<int16_t, 9600> stereo{};
            if (!packet->silent) downmix_->Convert(std::span(packet->data).subspan((samples - keep) / 2 * inputFrameBytes),
                std::span(stereo).first(keep));
            for (size_t i = 0; i < keep; ++i) pending_.push_back(stereo[i]);
        }
        if (stop.stop_requested()) return false;
        for (auto& value : output) { value = pending_.front(); pending_.pop_front(); }
        return true;
    }
    // Ring capacity is spare space, not queued audio latency.
    uint32_t DelayMs() const override { return uint32_t(pending_.size() / 2) / 48; }
    uint64_t DroppedFrames() const override { return dropped_; }
private:
    AudioCaptureConfig config_;
    WasapiCapture capture_;
    std::deque<int16_t> pending_;
    uint64_t dropped_ = 0;
    std::optional<StereoDownmix> downmix_;
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
        // Keep 50ms of output headroom, rather than the engine's minimum-size
        // ring. Legacy used 100ms; this tolerates short video/CPU scheduling stalls
        // while retaining a smaller, bounded audio delay.
        Check(client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 50 * 10000, 0, &format, nullptr));
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
        BYTE* initial;
        Check(render_->GetBuffer(bufferFrames_, &initial));
        Check(render_->ReleaseBuffer(bufferFrames_, AUDCLNT_BUFFERFLAGS_SILENT));
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
    return {[capture]() -> std::unique_ptr<PcmCaptureEndpoint> {
            if (capture.source == AudioCaptureSource::None) {
                if (!capture.deviceId.empty() || capture.processId) throw std::invalid_argument("No shared audio cannot select a device or process");
                return std::make_unique<SilentPcmCapture>();
            }
            return std::make_unique<Capture>(capture);
        },
        [playbackDeviceId] { return std::make_unique<Playout>(playbackDeviceId); }};
}
}
