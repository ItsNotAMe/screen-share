#include "media/webrtc/PcmAudioDeviceModule.h"
#include "api/make_ref_counted.h"
#include <algorithm>
#include <cstring>
#include <future>
#include <mutex>
#include <thread>

namespace screenshare::media {
namespace {
template<class T> int32_t Out(T* target, T value) { if (!target) return -1; *target = value; return 0; }
class PcmAdm : public webrtc::AudioDeviceModule {
public:
    PcmAdm(PcmEndpointFactories endpoints, std::shared_ptr<PcmAudioDiagnostics> diagnostics)
        : endpoints_(std::move(endpoints)), diagnostics_(std::move(diagnostics)) {}
    ~PcmAdm() override { Terminate(); }
    int32_t ActiveAudioLayer(AudioLayer* layer) const override { return Out(layer, kWindowsCoreAudio); }
    int32_t RegisterAudioCallback(webrtc::AudioTransport* callback) override {
        std::lock_guard lock(callbackMutex_); callback_ = callback; return 0;
    }
    int32_t Init() override { initialized_ = true; return 0; }
    int32_t Terminate() override {
        StopRecording(); StopPlayout();
        RegisterAudioCallback(nullptr);
        initialized_ = speaker_ = microphone_ = false;
        return 0;
    }
    bool Initialized() const override { return initialized_; }
    int16_t PlayoutDevices() override { return endpoints_.playout ? 1 : 0; }
    int16_t RecordingDevices() override { return endpoints_.capture ? 1 : 0; }
    int32_t PlayoutDeviceName(uint16_t index, char* name, char* guid) override { return Name(index, name, guid, bool(endpoints_.playout), "Configured output"); }
    int32_t RecordingDeviceName(uint16_t index, char* name, char* guid) override { return Name(index, name, guid, bool(endpoints_.capture), "Configured capture"); }
    int32_t SetPlayoutDevice(uint16_t index) override { return index == 0 && endpoints_.playout && !playing_ ? 0 : -1; }
    int32_t SetRecordingDevice(uint16_t index) override { return index == 0 && endpoints_.capture && !recording_ ? 0 : -1; }
    int32_t SetPlayoutDevice(WindowsDeviceType device) override { return device == kDefaultDevice ? SetPlayoutDevice(uint16_t(0)) : -1; }
    int32_t SetRecordingDevice(WindowsDeviceType device) override { return device == kDefaultDevice ? SetRecordingDevice(uint16_t(0)) : -1; }
    int32_t PlayoutIsAvailable(bool* value) override { return Out(value, bool(endpoints_.playout)); }
    int32_t RecordingIsAvailable(bool* value) override { return Out(value, bool(endpoints_.capture)); }
    int32_t InitPlayout() override { playInitialized_ = initialized_ && bool(endpoints_.playout); return playInitialized_ ? 0 : -1; }
    int32_t InitRecording() override { recordInitialized_ = initialized_ && bool(endpoints_.capture); return recordInitialized_ ? 0 : -1; }
    bool PlayoutIsInitialized() const override { return playInitialized_; }
    bool RecordingIsInitialized() const override { return recordInitialized_; }
    bool Playing() const override { return playing_; }
    bool Recording() const override { return recording_; }
    int32_t StartRecording() override {
        if (recording_) return 0;
        if (!recordInitialized_) return -1;
        if (captureThread_.joinable()) captureThread_.join();
        std::promise<bool> ready; auto started = ready.get_future();
        captureThread_ = std::jthread([this, ready = std::move(ready)](std::stop_token stop) mutable {
            bool announced = false;
            try {
                auto device = endpoints_.capture();
                if (!device) throw std::runtime_error("Missing PCM capture endpoint");
                device->Start(stop);
                recording_ = true; announced = true; ready.set_value(true);
                PcmBlock block;
                while (!stop.stop_requested() && device->Read(block, stop)) {
                    if (stop.stop_requested()) break;
                    diagnostics_->estimatedCaptureDelayMs = device->DelayMs();
                    diagnostics_->droppedCaptureFrames = device->DroppedFrames();
                    const size_t channels = recordStereo_ ? 2 : 1;
                    if (channels == 1) for (size_t i = 0; i < 480; ++i) block[i] = int16_t((int(block[2*i]) + block[2*i+1]) / 2);
                    if (recordMuted_) std::fill_n(block.begin(), 480 * channels, int16_t(0));
                    {
                        std::lock_guard lock(callbackMutex_);
                        uint32_t newLevel = 0;
                        if (callback_ && callback_->RecordedDataIsAvailable(block.data(), 480, channels * 2, channels, 48000,
                            device->DelayMs() + diagnostics_->playoutDelayMs, 0, 0, false, newLevel) != 0)
                            throw std::runtime_error("Audio capture transport failed");
                    }
                    ++diagnostics_->capturedBlocks;
                }
            } catch (...) { ++diagnostics_->captureErrors; if (!announced) ready.set_value(false); }
            recording_ = false;
        });
        return started.get() ? 0 : -1;
    }
    int32_t StartPlayout() override {
        if (playing_) return 0;
        if (!playInitialized_) return -1;
        if (playThread_.joinable()) playThread_.join();
        std::promise<bool> ready; auto started = ready.get_future();
        playThread_ = std::jthread([this, ready = std::move(ready)](std::stop_token stop) mutable {
            bool announced = false;
            try {
                auto device = endpoints_.playout();
                if (!device) throw std::runtime_error("Missing PCM playout endpoint");
                device->Start();
                diagnostics_->playoutBufferFrames = device->BufferFrames();
                diagnostics_->playoutEnginePeriodUs = device->EnginePeriodUs();
                playing_ = true; announced = true; ready.set_value(true);
                while (!stop.stop_requested()) {
                    PcmBlock block{};
                    const size_t channels = playStereo_ ? 2 : 1;
                    size_t samples = 0; int64_t elapsed = -1, ntp = -1;
                    {
                        std::lock_guard lock(callbackMutex_);
                        if (callback_ && callback_->NeedMorePlayData(480, channels * 2, channels, 48000, block.data(), samples, &elapsed, &ntp) != 0)
                            throw std::runtime_error("Audio playout transport failed");
                    }
                    // Pinned AudioTransportImpl reports interleaved sample count,
                    // whereas its request count is frames per channel.
                    if (samples > 480 * channels || samples % channels) throw std::runtime_error("Audio transport returned invalid sample count");
                    std::fill(block.begin() + samples, block.end(), int16_t(0));
                    if (channels == 1) for (int i = 479; i >= 0; --i) block[2*i] = block[2*i+1] = block[i];
                    const auto gain = playMuted_ ? 0u : volume_.load();
                    for (auto& sample : block) sample = int16_t(int(sample) * int(gain) / 255);
                    if (stop.stop_requested()) break;
                    device->Write(block, stop);
                    diagnostics_->playoutDelayMs = device->DelayMs();
                    ++diagnostics_->playedBlocks;
                }
            } catch (...) { ++diagnostics_->playoutErrors; if (!announced) ready.set_value(false); }
            playing_ = false;
        });
        return started.get() ? 0 : -1;
    }
    int32_t StopRecording() override {
        captureThread_.request_stop(); if (captureThread_.joinable()) captureThread_.join();
        recording_ = recordInitialized_ = false; return 0;
    }
    int32_t StopPlayout() override {
        playThread_.request_stop(); if (playThread_.joinable()) playThread_.join();
        playing_ = playInitialized_ = false; return 0;
    }
    int32_t InitSpeaker() override { speaker_ = initialized_ && bool(endpoints_.playout); return speaker_ ? 0 : -1; }
    int32_t InitMicrophone() override { microphone_ = initialized_ && bool(endpoints_.capture); return microphone_ ? 0 : -1; }
    bool SpeakerIsInitialized() const override { return speaker_; }
    bool MicrophoneIsInitialized() const override { return microphone_; }
    int32_t SpeakerVolumeIsAvailable(bool* value) override { return Out(value, bool(endpoints_.playout)); }
    int32_t SetSpeakerVolume(uint32_t value) override { if (value > 255) return -1; volume_ = value; return 0; }
    int32_t SpeakerVolume(uint32_t* value) const override { return Out(value, volume_.load()); }
    int32_t MaxSpeakerVolume(uint32_t* value) const override { return Out(value, 255u); }
    int32_t MinSpeakerVolume(uint32_t* value) const override { return Out(value, 0u); }
    int32_t MicrophoneVolumeIsAvailable(bool* value) override { return Out(value, false); }
    int32_t SetMicrophoneVolume(uint32_t) override { return -1; }
    int32_t MicrophoneVolume(uint32_t* value) const override { Out(value, 0u); return -1; }
    int32_t MaxMicrophoneVolume(uint32_t* value) const override { Out(value, 0u); return -1; }
    int32_t MinMicrophoneVolume(uint32_t* value) const override { Out(value, 0u); return -1; }
    int32_t SpeakerMuteIsAvailable(bool* value) override { return Out(value, bool(endpoints_.playout)); }
    int32_t SetSpeakerMute(bool value) override { playMuted_ = value; return 0; }
    int32_t SpeakerMute(bool* value) const override { return Out(value, playMuted_.load()); }
    int32_t MicrophoneMuteIsAvailable(bool* value) override { return Out(value, bool(endpoints_.capture)); }
    int32_t SetMicrophoneMute(bool value) override { recordMuted_ = value; return 0; }
    int32_t MicrophoneMute(bool* value) const override { return Out(value, recordMuted_.load()); }
    int32_t StereoPlayoutIsAvailable(bool* value) const override { return Out(value, bool(endpoints_.playout)); }
    int32_t StereoRecordingIsAvailable(bool* value) const override { return Out(value, bool(endpoints_.capture)); }
    int32_t SetStereoPlayout(bool value) override { if (playing_) return -1; playStereo_ = value; return 0; }
    int32_t SetStereoRecording(bool value) override { if (recording_) return -1; recordStereo_ = value; return 0; }
    int32_t StereoPlayout(bool* value) const override { return Out(value, playStereo_); }
    int32_t StereoRecording(bool* value) const override { return Out(value, recordStereo_); }
    int32_t PlayoutDelay(uint16_t* value) const override { return Out(value, uint16_t(std::min(65535u, diagnostics_->playoutDelayMs.load()))); }
    bool BuiltInAECIsAvailable() const override { return false; }
    bool BuiltInAGCIsAvailable() const override { return false; }
    bool BuiltInNSIsAvailable() const override { return false; }
    int32_t EnableBuiltInAEC(bool) override { return -1; }
    int32_t EnableBuiltInAGC(bool) override { return -1; }
    int32_t EnableBuiltInNS(bool) override { return -1; }
private:
    static int32_t Name(uint16_t index, char* name, char* guid, bool available, const char* label) {
        if (index || !available || !name || !guid) return -1;
        std::strcpy(name, label); std::strcpy(guid, "configured"); return 0;
    }
    const PcmEndpointFactories endpoints_;
    const std::shared_ptr<PcmAudioDiagnostics> diagnostics_;
    std::jthread captureThread_, playThread_;
    std::mutex callbackMutex_;
    webrtc::AudioTransport* callback_ = nullptr;
    std::atomic<bool> recording_{false}, playing_{false}, recordMuted_{false}, playMuted_{false};
    std::atomic<uint32_t> volume_{255};
    bool initialized_ = false, recordInitialized_ = false, playInitialized_ = false, speaker_ = false, microphone_ = false;
    bool playStereo_ = true, recordStereo_ = true;
};
}
webrtc::scoped_refptr<webrtc::AudioDeviceModule> CreatePcmAudioDeviceModule(
    PcmEndpointFactories endpoints, std::shared_ptr<PcmAudioDiagnostics> diagnostics) {
    if (!diagnostics) throw std::invalid_argument("PCM diagnostics must be owned");
    return webrtc::make_ref_counted<PcmAdm>(std::move(endpoints), std::move(diagnostics));
}
}
