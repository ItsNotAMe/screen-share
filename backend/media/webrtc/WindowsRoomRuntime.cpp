#include "WindowsRoomRuntime.h"
#include "RoomIceConfiguration.h"
#include "MicrophoneCapture.h"
#include "media/capture/WindowsCaptureSource.h"
#include "media/audio/WasapiPcmEndpoint.h"
#include "media/audio/SourcePcmCapture.h"
#include "media/capture/SwitchableCaptureSource.h"
#include "MfHardwareSession.h"
#include "MfVideoEncoderFactory.h"
#include "MfVideoDecoderFactory.h"
#include "PcmAudioDeviceModule.h"
#include "input/v2/DesktopSink.h"
#include "capture/CaptureBackendPolicy.h"
#include <mutex>

namespace screenshare::media {
namespace {
struct DeviceState {
    std::mutex mutex;
    std::shared_ptr<MfHardwareSession> hardware;
    std::shared_ptr<DiagnosticHistory> diagnostics = std::make_shared<DiagnosticHistory>();
    std::weak_ptr<AudioSwitchControl> audioControl;
    std::shared_ptr<MfHardwareSession> Get() { std::lock_guard lock(mutex); return hardware; }
};
class DeviceCapture final : public ICaptureSource {
    WindowsCaptureSource source_;
    std::shared_ptr<DeviceState> state_;
    template<class Work> auto Observe(const char* operation, Work work) {
        try { return work(); }
        catch (const CaptureBackendError& error) { state_->diagnostics->Event(operation, uint32_t(error.result())); throw; }
        catch (const CaptureDeviceLostError& error) { state_->diagnostics->Event(operation, uint32_t(error.reason())); throw; }
        catch (...) { state_->diagnostics->Event(operation, -1); throw; }
    }
public:
    DeviceCapture(CaptureConfig config, std::shared_ptr<DeviceState> state, std::shared_ptr<input::DesktopTargetState> target) : source_(config,std::move(target)), state_(std::move(state)) {}
    void Start() override { Observe("capture-start-failed", [&] { source_.Start(); }); }
    std::optional<CaptureSample> Poll() override {
        auto sample = Observe("capture-poll-failed", [&] { return source_.Poll(); });
        if (sample) {
            const auto resource = std::static_pointer_cast<WindowsCaptureResource>(sample->resource);
            std::lock_guard lock(state_->mutex);
            if (!state_->hardware) state_->hardware = std::make_shared<MfHardwareSession>(resource->device);
        }
        return sample;
    }
    bool Closed() const override { return source_.Closed(); }
    bool Minimized() const override { return source_.Minimized(); }
    CaptureSourceInfo Info() const override { return source_.Info(); }
    void Retire() noexcept override { source_.Retire(); }
    void Rebuild() override { state_->diagnostics->Event("capture-rebuild"); Observe("capture-rebuild-failed", [&] { source_.Rebuild(); }); }
};
}
v2::RoomRuntimeFactory WindowsRoomRuntimeFactory(WindowsRoomRuntimeOptions options) {
    ValidateStreamPreferences(options.preferences);
    return [options = std::move(options)](const v2::RoomIdentity& identity, v2::RoomSend send) {
        auto state = std::make_shared<DeviceState>();
        auto target = options.inputTarget ? options.inputTarget :
            options.inputSink || options.enableDesktopInput ? std::make_shared<input::DesktopTargetState>() : nullptr;
        NativeRoomRuntimeOptions native;
        native.diagnostics = state->diagnostics;
        native.initialCapture = {options.capture.sourceType == CaptureSourceType::Window ? CaptureKind::Window : CaptureKind::Display,
            options.capture.displayIndex, options.capture.windowHandle, options.capture.targetFps};
        if (identity.host) native.captureSwitch = std::make_shared<CaptureSwitchControl>(native.initialCapture);
        auto endpoints = options.audioEndpoints.value_or(WasapiPcmEndpoints(options.audio, options.playbackDeviceId));
        if (identity.host) {
            AudioSelection initial{options.audio.source == AudioCaptureSource::Microphone ? AudioKind::Microphone :
                options.audio.source == AudioCaptureSource::ProcessOutput ? AudioKind::Process :
                options.audio.source == AudioCaptureSource::None ? AudioKind::None :
                options.audio.source == AudioCaptureSource::SharedSource ? AudioKind::SharedSource : AudioKind::System,
                options.audio.deviceId, options.audio.processId};
            ValidateAudioSelection(initial);
            if (options.audioForSelection) native.audioForSelection = options.audioForSelection;
            else if (!options.audioEndpoints) native.audioForSelection = [capture = native.captureSwitch, state](AudioSelection selection) -> AudioSwitchControl::Factory {
                if (selection.kind == AudioKind::SharedSource) return [capture, state] {
                    auto resolve = [capture, window = uint64_t(0), process = DWORD(0)]() mutable -> AudioSelection {
                        const auto selected = capture->Status().selected;
                        if (selected.kind == CaptureKind::Display) { window = 0; process = 0; return {}; }
                        DWORD pid = 0;
                        const auto handle = reinterpret_cast<HWND>(selected.window);
                        if (!IsWindow(handle) || !GetWindowThreadProcessId(handle, &pid) || !pid)
                            throw std::runtime_error("Shared audio window is unavailable");
                        if (window != selected.window) { window = selected.window; process = pid; }
                        if (process != pid) throw std::runtime_error("Shared audio window identity changed");
                        return {AudioKind::Process, {}, pid};
                    };
                    return std::make_unique<SourcePcmCapture>(std::move(resolve), [](AudioSelection source) {
                        AudioCaptureConfig config;
                        config.source = source.kind == AudioKind::Process ? AudioCaptureSource::ProcessOutput : AudioCaptureSource::SystemOutput;
                        config.processId = source.processId;
                        return WasapiPcmEndpoints(config).capture();
                    }, [state](bool ready) {
                        if (auto control = state->audioControl.lock()) { if (ready) control->Ready(); else control->Failed(); }
                    });
                };
                AudioCaptureConfig config;
                config.source = selection.kind == AudioKind::Microphone ? AudioCaptureSource::Microphone :
                    selection.kind == AudioKind::Process ? AudioCaptureSource::ProcessOutput : AudioCaptureSource::SystemOutput;
                config.deviceId = selection.deviceId; config.processId = selection.processId;
                return WasapiPcmEndpoints(config).capture;
            };
            const auto initialFactory = initial.kind == AudioKind::SharedSource && !options.audioEndpoints ?
                native.audioForSelection(initial) : endpoints.capture;
            native.audioSwitch = std::make_shared<AudioSwitchControl>(initial, initialFactory, ProcessMicrophone);
            state->audioControl = native.audioSwitch;
            endpoints.capture = [control = native.audioSwitch] { return std::make_unique<SwitchablePcmCapture>(control); };
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
        native.engine = [endpoints = std::move(endpoints), state, preferHardware = options.preferHardwareEncoding,
                         hardwareDecode = options.preferHardwareDecoding,
                         encoderDecorator = options.encoderDecorator, decoderDecorator = options.decoderDecorator,
                         packetFactory = options.packetFactory, diagnostics = native.diagnostics] {
            std::unique_ptr<webrtc::VideoEncoderFactory> encoder = std::make_unique<MfVideoEncoderFactory>(preferHardware ? state->Get() : nullptr, diagnostics);
            std::unique_ptr<webrtc::VideoDecoderFactory> decoder = std::make_unique<MfVideoDecoderFactory>(hardwareDecode, MfVideoDecoderFactory::DeviceFactory{}, diagnostics);
            if (encoderDecorator) encoder = encoderDecorator(std::move(encoder));
            if (decoderDecorator) decoder = decoderDecorator(std::move(decoder));
            return std::make_unique<MediaEngine>(CreatePcmAudioDeviceModule(endpoints, std::make_shared<PcmAudioDiagnostics>()),
                std::move(encoder), std::move(decoder), packetFactory);
        };
        native.engineReady = [state] { return bool(state->Get()); };
        native.capture = [capture = options.capture, state, target] { return std::make_unique<DeviceCapture>(capture, state, target); };
        if (options.captureDecorator) native.capture = options.captureDecorator(std::move(native.capture));
        native.connection = RoomIceConfiguration(options.connection, options.useDefaultStun && !options.audioEndpoints && !options.packetFactory);
        native.captureForSelection = [base = options.capture, state, target, decorate = options.captureDecorator](CaptureSelection selection) -> CaptureSession::Factory {
            auto config = base; config.sourceType = selection.kind == CaptureKind::Window ? CaptureSourceType::Window : CaptureSourceType::Display;
            config.displayIndex = selection.display; config.windowHandle = selection.window; config.targetFps = selection.fps;
            CaptureSession::Factory factory = [config, state, target] { return std::make_unique<DeviceCapture>(config, state, target); };
            return decorate ? decorate(std::move(factory)) : std::move(factory);
        };
        native.deliver = [](CaptureVideoSource& source, const CaptureSample& sample) {
            source.PushBuffer(std::static_pointer_cast<WindowsCaptureResource>(sample.resource)->buffer, sample.capturedAt, sample.resource->inputGeneration);
        };
        native.preferences = options.preferences; native.frames = options.frames; native.channel = options.channel;
        native.inputSink = options.inputSink;
        if(identity.host && options.enableDesktopInput && !native.inputSink)native.inputSink=input::CreateWindowsDesktopSink(target);
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
