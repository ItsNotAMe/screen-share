#pragma once
#include "api/audio_codecs/opus/audio_decoder_opus.h"
#include "api/audio_codecs/opus/audio_encoder_opus.h"

namespace screenshare::media {
// Opus's /2 RTP channel count alone does not enable stereo. Advertise the
// receive preference and stereo source explicitly on both sides of SDP.
// Keep upstream parsing so older peers can still negotiate mono.
struct SharedAudioEncoder : webrtc::AudioEncoderOpus {
    static void AppendSupportedEncoders(std::vector<webrtc::AudioCodecSpec>* specs) {
        const auto start = specs->size();
        AudioEncoderOpus::AppendSupportedEncoders(specs);
        for (size_t i = start; i < specs->size(); ++i) {
            auto& spec = (*specs)[i];
            spec.format.parameters["stereo"] = "1";
            spec.format.parameters["sprop-stereo"] = "1";
            spec.info = QueryAudioEncoder(*SdpToConfig(spec.format));
        }
    }
};
struct SharedAudioDecoder : webrtc::AudioDecoderOpus {
    static void AppendSupportedDecoders(std::vector<webrtc::AudioCodecSpec>* specs) {
        const auto start = specs->size();
        AudioDecoderOpus::AppendSupportedDecoders(specs);
        for (size_t i = start; i < specs->size(); ++i) {
            auto& spec = (*specs)[i];
            spec.format.parameters["stereo"] = "1";
            spec.format.parameters["sprop-stereo"] = "1";
            spec.info.num_channels = 2;
        }
    }
};
}
