#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <thread>
#include "media/audio/WasapiPcmEndpoint.h"

#include "api/audio/audio_device.h"
#include "api/audio/create_audio_device_module.h"
#include "api/audio_codecs/audio_decoder_factory_template.h"
#include "api/audio_codecs/audio_encoder_factory_template.h"
#include "api/audio_codecs/opus/audio_decoder_opus.h"
#include "api/audio_codecs/opus/audio_encoder_opus.h"
#include "api/create_modular_peer_connection_factory.h"
#include "api/enable_media.h"
#include "api/environment/environment_factory.h"
#include "api/jsep.h"
#include "api/make_ref_counted.h"
#include "api/peer_connection_interface.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"
#include "rtc_base/win32_socket_init.h"
#include "rtc_base/time_utils.h"
#include "rtc_base/logging.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_broadcaster.h"
#include "pc/video_track_source.h"
#include "media/webrtc/MfVideoDecoderFactory.h"
#include "media/webrtc/MfVideoEncoderFactory.h"
#include "media/webrtc/MfHardwareSession.h"
#include "media/webrtc/PcmAudioDeviceModule.h"
#include "SyntheticAudio.h"
#include "CaptureTestWindow.h"
#include "LiveCaptureSource.h"
#include "media/capture/SyntheticCaptureSource.h"
#include "PresentationTestWindow.h"
#include "media/webrtc/CaptureVideoSource.h"

namespace proofmedia {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Predicate>
void Wait(Predicate predicate, const char* failure) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!predicate()) {
        Require(std::chrono::steady_clock::now() < deadline, failure);
        webrtc::Thread::Current()->ProcessMessages(10);
    }
}

class CreatedDescription : public webrtc::CreateSessionDescriptionObserver {
public:
    void OnSuccess(webrtc::SessionDescriptionInterface* value) override {
        description.reset(value);
        done = true;
    }
    void OnFailure(webrtc::RTCError) override { done = true; }
    bool done = false;
    std::unique_ptr<webrtc::SessionDescriptionInterface> description;
};

class AppliedDescription : public webrtc::SetSessionDescriptionObserver {
public:
    void OnSuccess() override { success = true; done = true; }
    void OnFailure(webrtc::RTCError) override { done = true; }
    bool done = false;
    bool success = false;
};

class Channel : public webrtc::DataChannelObserver {
public:
    explicit Channel(webrtc::scoped_refptr<webrtc::DataChannelInterface> value)
        : channel(std::move(value)), expected("screenshare-proof:" + channel->label()) { channel->RegisterObserver(this); }
    ~Channel() override { channel->UnregisterObserver(); }
    void OnStateChange() override {}
    void OnMessage(const webrtc::DataBuffer& buffer) override {
        const std::string actual(buffer.data.cdata<char>(), buffer.data.size());
        received = actual == expected;
    }
    webrtc::scoped_refptr<webrtc::DataChannelInterface> channel;
    std::string expected;
    bool received = false;
};

using SyntheticVideo = screenshare::media::CaptureVideoSource;

class Peer : public webrtc::PeerConnectionObserver, public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    ~Peer() override {
        if (video) video->RemoveSink(this);
        channels.clear();
        if (connection) connection->Close();
    }
    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}
    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState) override {}
    void OnIceCandidate(const webrtc::IceCandidate*) override {}
    void OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) override {
        auto track = transceiver->receiver()->track();
        if (track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
            video = static_cast<webrtc::VideoTrackInterface*>(track.get());
            video->AddOrUpdateSink(this, webrtc::VideoSinkWants());
        }
    }
    void OnFrame(const webrtc::VideoFrame& frame) override {
        auto buffer = frame.video_frame_buffer();
        auto pixels = buffer->type() == webrtc::VideoFrameBuffer::Type::kNV12 ? nullptr : buffer->ToI420();
        auto nv12 = buffer->type() == webrtc::VideoFrameBuffer::Type::kNV12 ? buffer->GetNV12() : nullptr;
        const auto* y = nv12 ? nv12->DataY() : (pixels ? pixels->DataY() : nullptr);
        const int stride = nv12 ? nv12->StrideY() : (pixels ? pixels->StrideY() : 0);
        if (frame.width() != 640 || frame.height() != 360 || !y || y[checkCenter ? stride * 180 + 320 : 0] < 35)
            invalidFrames.fetch_add(1);
        if (checkCenter && nv12 && frame.width() == 640 && frame.height() == 360) {
            const auto* uv = nv12->DataUV() + nv12->StrideUV() * 90 + 320;
            if (std::abs(int(uv[0]) - 128) > 12 || std::abs(int(uv[1]) - 128) > 12) ++invalidFrames;
        }
        if (presentationSink) presentationSink->OnFrame(frame);
        decodedFrames.fetch_add(1);
    }
    void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> value) override {
        channels.push_back(std::make_unique<Channel>(std::move(value)));
    }
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> connection;
    std::vector<std::unique_ptr<Channel>> channels;
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> video;
    std::atomic<unsigned> decodedFrames{0};
    std::atomic<unsigned> invalidFrames{0};
    bool checkCenter = false;
    screenshare::media::LatestVideoFrameSink* presentationSink = nullptr;
};

void TransferDescription(Peer& from, Peer& to, bool offer) {
    auto created = webrtc::make_ref_counted<CreatedDescription>();
    webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
    if (offer) from.connection->CreateOffer(created.get(), options);
    else from.connection->CreateAnswer(created.get(), options);
    Wait([&] { return created->done; }, "SDP creation timed out");
    Require(created->description != nullptr, "SDP creation failed");
    auto applied = webrtc::make_ref_counted<AppliedDescription>();
    from.connection->SetLocalDescription(applied.get(), created->description.release());
    Wait([&] { return applied->done; }, "Local description timed out");
    Require(applied->success, "Local description failed");
    // This local proof bundles candidates in SDP. Production uses targeted trickle ICE.
    Wait([&] { return from.connection->ice_gathering_state() ==
        webrtc::PeerConnectionInterface::kIceGatheringComplete; }, "ICE gathering timed out");
    std::string sdp;
    Require(from.connection->local_description()->ToString(&sdp), "SDP serialization failed");
    auto remote = webrtc::CreateSessionDescription(offer ? webrtc::SdpType::kOffer : webrtc::SdpType::kAnswer, sdp);
    Require(remote != nullptr, "SDP parsing failed");
    auto received = webrtc::make_ref_counted<AppliedDescription>();
    to.connection->SetRemoteDescription(received.get(), remote.release());
    Wait([&] { return received->done; }, "Remote description timed out");
    Require(received->success, "Remote description failed");
}

} // namespace proofmedia
