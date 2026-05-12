#include "audio/OpusCodec.h"

#include <opus/opus.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace screenshare {
namespace {

constexpr int MaxOpusPacketBytes = 4'000;

bool IsValidOpusFrameSize(uint32_t frames)
{
    switch (frames) {
    case 120:
    case 240:
    case 480:
    case 960:
    case 1920:
    case 2880:
        return true;
    default:
        return false;
    }
}

int32_t SignExtend24(uint32_t value)
{
    if ((value & 0x0080'0000U) != 0) {
        value |= 0xFF00'0000U;
    }
    return static_cast<int32_t>(value);
}

float ReadSample(std::span<const std::byte> bytes, const AudioCaptureFormat& format, uint32_t frame, uint16_t channel)
{
    const size_t offset =
        static_cast<size_t>(frame) * static_cast<size_t>(format.blockAlign) +
        static_cast<size_t>(channel) * static_cast<size_t>(format.bitsPerSample / 8);
    if (offset + static_cast<size_t>(format.bitsPerSample / 8) > bytes.size()) {
        return 0.0f;
    }

    if (format.sampleFormat == "float32") {
        float sample = 0.0f;
        std::memcpy(&sample, bytes.data() + offset, sizeof(sample));
        return std::clamp(sample, -1.0f, 1.0f);
    }
    if (format.sampleFormat == "pcm16") {
        int16_t sample = 0;
        std::memcpy(&sample, bytes.data() + offset, sizeof(sample));
        return std::clamp(static_cast<float>(sample) / 32768.0f, -1.0f, 1.0f);
    }
    if (format.sampleFormat == "pcm24") {
        const uint32_t raw =
            static_cast<uint32_t>(std::to_integer<uint8_t>(bytes[offset])) |
            (static_cast<uint32_t>(std::to_integer<uint8_t>(bytes[offset + 1])) << 8) |
            (static_cast<uint32_t>(std::to_integer<uint8_t>(bytes[offset + 2])) << 16);
        return std::clamp(static_cast<float>(SignExtend24(raw)) / 8'388'608.0f, -1.0f, 1.0f);
    }
    if (format.sampleFormat == "pcm32") {
        int32_t sample = 0;
        std::memcpy(&sample, bytes.data() + offset, sizeof(sample));
        return std::clamp(static_cast<float>(sample) / 2'147'483'648.0f, -1.0f, 1.0f);
    }

    return 0.0f;
}

std::vector<float> ConvertToOpusPcm(const CapturedAudioPacket& packet, const AudioCaptureFormat& format)
{
    if (format.sampleRate == 0 || format.channels == 0 || format.blockAlign == 0 || format.bitsPerSample == 0) {
        throw std::invalid_argument("Opus input audio format is incomplete");
    }
    if ((format.bitsPerSample % 8) != 0) {
        throw std::invalid_argument("Opus input bits per sample must be byte-aligned");
    }
    if (packet.frames == 0) {
        return {};
    }

    const auto outputFrames = static_cast<uint32_t>(std::llround(
        (static_cast<double>(packet.frames) * static_cast<double>(OpusAudioEncoder::OutputSampleRate)) /
        static_cast<double>(format.sampleRate)));
    if (!IsValidOpusFrameSize(outputFrames)) {
        std::ostringstream stream;
        stream << "Unsupported Opus frame size after resampling: " << outputFrames
               << " frames from " << packet.frames << " input frames at " << format.sampleRate << " Hz";
        throw std::runtime_error(stream.str());
    }

    std::vector<float> pcm(static_cast<size_t>(outputFrames) * OpusAudioEncoder::OutputChannels);
    if (packet.silent || packet.data.empty()) {
        return pcm;
    }

    for (uint32_t outFrame = 0; outFrame < outputFrames; ++outFrame) {
        const double sourcePosition =
            (static_cast<double>(outFrame) * static_cast<double>(format.sampleRate)) /
            static_cast<double>(OpusAudioEncoder::OutputSampleRate);
        const uint32_t sourceFrame = std::min<uint32_t>(
            static_cast<uint32_t>(std::llround(sourcePosition)),
            packet.frames - 1);

        const float left = ReadSample(packet.data, format, sourceFrame, 0);
        const float right = format.channels > 1 ? ReadSample(packet.data, format, sourceFrame, 1) : left;
        pcm[static_cast<size_t>(outFrame) * 2] = left;
        pcm[static_cast<size_t>(outFrame) * 2 + 1] = right;
    }

    return pcm;
}

} // namespace

OpusAudioEncoder::OpusAudioEncoder() = default;

OpusAudioEncoder::~OpusAudioEncoder()
{
    Stop();
}

void OpusAudioEncoder::Start(const AudioCaptureFormat& inputFormat, uint32_t bitrate)
{
    Stop();
    inputFormat_ = inputFormat;
    bitrate_ = bitrate == 0 ? DefaultBitrate : bitrate;

    int error = OPUS_OK;
    encoder_ = opus_encoder_create(
        static_cast<opus_int32>(OutputSampleRate),
        OutputChannels,
        OPUS_APPLICATION_AUDIO,
        &error);
    if (error != OPUS_OK || encoder_ == nullptr) {
        throw std::runtime_error(std::string("opus_encoder_create failed: ") + opus_strerror(error));
    }

    error = opus_encoder_ctl(static_cast<OpusEncoder*>(encoder_), OPUS_SET_BITRATE(static_cast<opus_int32>(bitrate_)));
    if (error != OPUS_OK) {
        Stop();
        throw std::runtime_error(std::string("OPUS_SET_BITRATE failed: ") + opus_strerror(error));
    }

    error = opus_encoder_ctl(static_cast<OpusEncoder*>(encoder_), OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    if (error != OPUS_OK) {
        Stop();
        throw std::runtime_error(std::string("OPUS_SET_SIGNAL failed: ") + opus_strerror(error));
    }
}

void OpusAudioEncoder::Stop()
{
    if (encoder_ != nullptr) {
        opus_encoder_destroy(static_cast<OpusEncoder*>(encoder_));
        encoder_ = nullptr;
    }
}

OpusEncodedAudioPacket OpusAudioEncoder::Encode(const CapturedAudioPacket& packet)
{
    if (encoder_ == nullptr) {
        throw std::logic_error("OpusAudioEncoder::Start must be called before Encode");
    }

    const auto pcm = ConvertToOpusPcm(packet, inputFormat_);
    if (pcm.empty()) {
        return {};
    }

    std::vector<unsigned char> encoded(MaxOpusPacketBytes);
    const int encodedBytes = opus_encode_float(
        static_cast<OpusEncoder*>(encoder_),
        pcm.data(),
        static_cast<int>(pcm.size() / OutputChannels),
        encoded.data(),
        static_cast<opus_int32>(encoded.size()));
    if (encodedBytes < 0) {
        throw std::runtime_error(std::string("opus_encode_float failed: ") + opus_strerror(encodedBytes));
    }

    OpusEncodedAudioPacket output;
    output.audioFrames = static_cast<uint32_t>(pcm.size() / OutputChannels);
    output.bytes.resize(static_cast<size_t>(encodedBytes));
    std::memcpy(output.bytes.data(), encoded.data(), static_cast<size_t>(encodedBytes));
    return output;
}

OpusAudioDecoder::OpusAudioDecoder() = default;

OpusAudioDecoder::~OpusAudioDecoder()
{
    Stop();
}

void OpusAudioDecoder::Start()
{
    Stop();

    int error = OPUS_OK;
    decoder_ = opus_decoder_create(
        static_cast<opus_int32>(OutputSampleRate),
        OutputChannels,
        &error);
    if (error != OPUS_OK || decoder_ == nullptr) {
        throw std::runtime_error(std::string("opus_decoder_create failed: ") + opus_strerror(error));
    }
}

void OpusAudioDecoder::Stop()
{
    if (decoder_ != nullptr) {
        opus_decoder_destroy(static_cast<OpusDecoder*>(decoder_));
        decoder_ = nullptr;
    }
}

std::vector<std::byte> OpusAudioDecoder::Decode(std::span<const std::byte> packet, uint32_t expectedFrames)
{
    if (decoder_ == nullptr) {
        throw std::logic_error("OpusAudioDecoder::Start must be called before Decode");
    }
    if (packet.empty()) {
        return {};
    }

    const uint32_t maxFrames = expectedFrames == 0 ? 2880U : std::min<uint32_t>(expectedFrames, 2880U);
    std::vector<float> decoded(static_cast<size_t>(maxFrames) * OutputChannels);
    const int frames = opus_decode_float(
        static_cast<OpusDecoder*>(decoder_),
        reinterpret_cast<const unsigned char*>(packet.data()),
        static_cast<opus_int32>(packet.size()),
        decoded.data(),
        static_cast<int>(maxFrames),
        0);
    if (frames < 0) {
        throw std::runtime_error(std::string("opus_decode_float failed: ") + opus_strerror(frames));
    }

    const size_t byteCount = static_cast<size_t>(frames) * OutputBlockAlign;
    std::vector<std::byte> bytes(byteCount);
    std::memcpy(bytes.data(), decoded.data(), byteCount);
    return bytes;
}

} // namespace screenshare
