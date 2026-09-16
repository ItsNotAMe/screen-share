#pragma once
#include "api/peer_connection_interface.h"
#include "api/audio/audio_device.h"
#include "api/video_codecs/video_encoder_factory.h"
#include "api/video_codecs/video_decoder_factory.h"
#include "rtc_base/thread.h"
#include <array>
#include <memory>

namespace screenshare::media {
// Private native boundary. Construct/use/destroy on the signaling executor.
// Close and release every peer/track before destroying the engine. Observers
// remain caller-owned and must outlive their peers. SSL is application-owned.
class MediaEngine final {
public:
    MediaEngine(webrtc::scoped_refptr<webrtc::AudioDeviceModule> audio,
                std::unique_ptr<webrtc::VideoEncoderFactory> encoder,
                std::unique_ptr<webrtc::VideoDecoderFactory> decoder);
    ~MediaEngine();
    MediaEngine(const MediaEngine&) = delete;
    MediaEngine& operator=(const MediaEngine&) = delete;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> CreatePeer(
        webrtc::PeerConnectionObserver& observer,
        webrtc::PeerConnectionInterface::RTCConfiguration config = {});
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> CreateAudioTrack();
    webrtc::scoped_refptr<webrtc::RtpSenderInterface> AttachHostMedia(
        webrtc::PeerConnectionInterface& peer,
        webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> video,
        webrtc::scoped_refptr<webrtc::AudioTrackInterface> audio);
    std::array<webrtc::scoped_refptr<webrtc::DataChannelInterface>, 3>
        CreateHostChannels(webrtc::PeerConnectionInterface& peer);
private:
    void CheckThread() const;
    webrtc::Thread* signaling_;
    std::unique_ptr<webrtc::Thread> network_, worker_;
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
};
}
