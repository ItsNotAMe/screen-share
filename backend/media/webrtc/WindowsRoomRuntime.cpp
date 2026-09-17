#include "WindowsRoomRuntime.h"
#include "MicrophoneCapture.h"
#include "media/capture/WindowsCaptureSource.h"
#include "media/audio/WasapiPcmEndpoint.h"
#include "MfHardwareSession.h"
#include "MfVideoEncoderFactory.h"
#include "MfVideoDecoderFactory.h"
#include "PcmAudioDeviceModule.h"
#include <mutex>

namespace screenshare::media {
namespace {
struct DeviceState {
    std::mutex mutex;
    std::shared_ptr<MfHardwareSession> hardware;
    std::shared_ptr<MfHardwareSession> Get() { std::lock_guard lock(mutex); return hardware; }
};
class DeviceCapture final : public ICaptureSource {
    WindowsCaptureSource source_;
    std::shared_ptr<DeviceState> state_;
public:
    DeviceCapture(CaptureConfig config, std::shared_ptr<DeviceState> state) : source_(config), state_(std::move(state)) {}
    void Start() override { source_.Start(); }
    std::optional<CaptureSample> Poll() override {
        auto sample = source_.Poll();
        if (sample) {
            const auto resource = std::static_pointer_cast<WindowsCaptureResource>(sample->resource);
            std::lock_guard lock(state_->mutex);
            if (!state_->hardware) state_->hardware = std::make_shared<MfHardwareSession>(resource->device);
        }
        return sample;
    }
    bool Closed() const override { return source_.Closed(); }
    void Retire() noexcept override { source_.Retire(); }
    void Rebuild() override { source_.Rebuild(); }
};
}
v2::RoomRuntimeFactory WindowsRoomRuntimeFactory(WindowsRoomRuntimeOptions options) {
    ValidateStreamPreferences(options.preferences);
    return [options = std::move(options)](const v2::RoomIdentity& identity, v2::RoomSend send) {
        auto state = std::make_shared<DeviceState>();
        NativeRoomRuntimeOptions native;
        auto endpoints = options.audioEndpoints.value_or(WasapiPcmEndpoints(options.audio, options.playbackDeviceId));
        if (identity.host) {
            AudioSelection initial{options.audio.source == AudioCaptureSource::Microphone ? AudioKind::Microphone :
                options.audio.source == AudioCaptureSource::ProcessOutput ? AudioKind::Process :
                options.audio.source == AudioCaptureSource::None ? AudioKind::None : AudioKind::System,
                options.audio.deviceId, options.audio.processId};
            ValidateAudioSelection(initial);
            native.audioSwitch = std::make_shared<AudioSwitchControl>(initial, endpoints.capture, ProcessMicrophone);
            endpoints.capture = [control = native.audioSwitch] { return std::make_unique<SwitchablePcmCapture>(control); };
            if (options.audioForSelection) native.audioForSelection = options.audioForSelection;
            else if (!options.audioEndpoints) native.audioForSelection = [](AudioSelection selection) {
                AudioCaptureConfig config;
                config.source = selection.kind == AudioKind::Microphone ? AudioCaptureSource::Microphone :
                    selection.kind == AudioKind::Process ? AudioCaptureSource::ProcessOutput : AudioCaptureSource::SystemOutput;
                config.deviceId = selection.deviceId; config.processId = selection.processId;
                return WasapiPcmEndpoints(config).capture;
            };
        }
        if (!identity.host) {
            PlaybackSelection initial{options.playbackDeviceId, options.playbackVolume, options.playbackMuted};
            ValidatePlaybackSelection(initial);
            native.playback = std::make_shared<PlaybackControl>(initial, endpoints.playout);
            endpoints.playout = [control = native.playback] { return std::make_unique<ControlledPcmPlayout>(control); };
            if (options.playbackForSelection) native.playbackForSelection = options.playbackForSelection;
            else if (!options.audioEndpoints) native.playbackForSelection = [](PlaybackSelection selection) {
                return WasapiPcmEndpoints({}, selection.deviceId).playout;
            };
        }
        native.engine = [endpoints = std::move(endpoints), state] {
            return std::make_unique<MediaEngine>(CreatePcmAudioDeviceModule(endpoints, std::make_shared<PcmAudioDiagnostics>()),
                std::make_unique<MfVideoEncoderFactory>(state->Get()), std::make_unique<MfVideoDecoderFactory>());
        };
        native.engineReady = [state] { return bool(state->Get()); };
        native.capture = [capture = options.capture, state] { return std::make_unique<DeviceCapture>(capture, state); };
        native.initialCapture = {options.capture.sourceType == CaptureSourceType::Window ? CaptureKind::Window : CaptureKind::Display,
            options.capture.displayIndex, options.capture.windowHandle, options.capture.targetFps};
        native.captureForSelection = [base = options.capture, state](CaptureSelection selection) -> CaptureSession::Factory {
            auto config = base; config.sourceType = selection.kind == CaptureKind::Window ? CaptureSourceType::Window : CaptureSourceType::Display;
            config.displayIndex = selection.display; config.windowHandle = selection.window; config.targetFps = selection.fps;
            return [config, state] { return std::make_unique<DeviceCapture>(config, state); };
        };
        native.deliver = [](CaptureVideoSource& source, const CaptureSample& sample) {
            source.PushBuffer(std::static_pointer_cast<WindowsCaptureResource>(sample.resource)->buffer, sample.capturedAt);
        };
        native.preferences = options.preferences; native.frames = options.frames; native.channel = options.channel;
        native.presentation = options.presentation;
        native.codecStatus = [state] {
            CodecPipelineStatus value;
            if (const auto hardware = state->Get()) {
                value.available = true; value.hardwareFrames = hardware->hardwareFrames.load();
                value.softwareFallbacks = hardware->softwareFallbacks.load();
                value.quarantined = hardware->quarantined.load(); value.retired = hardware->device->retired();
            }
            return value;
        };
        return CreateNativeRoomRuntime(identity, std::move(send), std::move(native));
    };
}
}
