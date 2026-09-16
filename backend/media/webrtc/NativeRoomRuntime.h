#pragma once
#include "api/RoomSession.h"
#include "media/HostMediaSession.h"
#include "MediaEngine.h"
#include "MediaPeer.h"
#include "CaptureVideoSource.h"

namespace screenshare::media {
// Private native composition boundary. Factories run on signaling/capture owners,
// respectively. Delivery runs on each viewer's capture worker. Shared sinks and
// dependencies are retained until joined shutdown; UI presentation must enqueue.
struct NativeRoomRuntimeOptions {
    std::function<std::unique_ptr<MediaEngine>()> engine;
    std::function<bool()> engineReady; // Optional first-capture-device barrier.
    CaptureSession::Factory capture;
    std::function<void(CaptureVideoSource&, const CaptureSample&)> deliver;
    std::shared_ptr<webrtc::VideoSinkInterface<webrtc::VideoFrame>> frames;
    std::function<void(const std::string&, webrtc::scoped_refptr<webrtc::DataChannelInterface>)> channel;
    StreamPreferences preferences;
    webrtc::PeerConnectionInterface::RTCConfiguration connection;
};
std::unique_ptr<v2::RoomRuntime> CreateNativeRoomRuntime(
    v2::RoomIdentity, v2::RoomSend, NativeRoomRuntimeOptions);
}
