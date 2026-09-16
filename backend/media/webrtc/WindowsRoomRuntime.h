#pragma once
#include "NativeRoomRuntime.h"
#include "capture/DesktopCapturer.h"
#include "audio/WasapiCapture.h"
#include "media/audio/PcmAudioEndpoint.h"

namespace screenshare::media {
// Windows binding for the shared runtime. Application owns WindowsMediaRuntime
// and SSL. Presentation sinks must enqueue onto their window thread. Optional
// audio endpoints permit headless tests without capturing/playing desktop audio.
struct WindowsRoomRuntimeOptions {
    CaptureConfig capture;
    AudioCaptureConfig audio;
    std::wstring playbackDeviceId;
    unsigned playbackVolume = 100;
    bool playbackMuted = false;
    std::function<PlaybackControl::Factory(PlaybackSelection)> playbackForSelection;
    std::optional<PcmEndpointFactories> audioEndpoints;
    // Test/embedding override. Never fall back to physical capture when supplied.
    std::function<AudioSwitchControl::Factory(AudioSelection)> audioForSelection;
    StreamPreferences preferences;
    std::shared_ptr<webrtc::VideoSinkInterface<webrtc::VideoFrame>> frames;
    std::function<void(const std::string&, webrtc::scoped_refptr<webrtc::DataChannelInterface>)> channel;
};
v2::RoomRuntimeFactory WindowsRoomRuntimeFactory(WindowsRoomRuntimeOptions);
}
