#include "codec/H264StreamEncoder.h"

#include "video/Nv12Convert.h"

#include <Windows.h>
#include <codecapi.h>
#include <initguid.h>
#include <mfapi.h>
#include <mferror.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace screenshare {
namespace {

void ThrowIfFailed(HRESULT hr, const char* operation)
{
    if (FAILED(hr)) {
        throw std::runtime_error(std::string(operation) + " failed: " + HResultMessage(hr));
    }
}

void SetVideoSize(IMFAttributes* attributes, REFGUID key, int width, int height)
{
    ThrowIfFailed(MFSetAttributeSize(attributes, key, static_cast<UINT32>(width), static_cast<UINT32>(height)), "MFSetAttributeSize");
}

void SetVideoRatio(IMFAttributes* attributes, REFGUID key, int numerator, int denominator)
{
    ThrowIfFailed(MFSetAttributeRatio(attributes, key, static_cast<UINT32>(numerator), static_cast<UINT32>(denominator)), "MFSetAttributeRatio");
}

void SetBt709ColorInfo(IMFMediaType* mediaType)
{
    ThrowIfFailed(mediaType->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709), "IMFMediaType::SetUINT32(YUV matrix)");
    ThrowIfFailed(mediaType->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709), "IMFMediaType::SetUINT32(video primaries)");
    ThrowIfFailed(mediaType->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709), "IMFMediaType::SetUINT32(transfer function)");
    ThrowIfFailed(mediaType->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235), "IMFMediaType::SetUINT32(nominal range)");
}

bool IsBgra8Format(DXGI_FORMAT format)
{
    return
        format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
        format == DXGI_FORMAT_B8G8R8X8_UNORM ||
        format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}

} // namespace

H264StreamEncoder::H264StreamEncoder()
{
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(comResult)) {
        comInitialized_ = true;
    } else if (comResult != RPC_E_CHANGED_MODE) {
        ThrowIfFailed(comResult, "CoInitializeEx");
    }

    ThrowIfFailed(MFStartup(MF_VERSION, MFSTARTUP_FULL), "MFStartup");
    mfStarted_ = true;
}

H264StreamEncoder::~H264StreamEncoder()
{
    try {
        Stop();
    } catch (...) {
    }

    if (mfStarted_) {
        static_cast<void>(MFShutdown());
    }

    if (comInitialized_) {
        CoUninitialize();
    }
}

void H264StreamEncoder::Start(const H264StreamEncoderConfig& config)
{
    Stop();

    if (config.width <= 0 || config.height <= 0) {
        throw std::invalid_argument("H264StreamEncoder width and height must be positive");
    }
    if ((config.width % 2) != 0 || (config.height % 2) != 0) {
        throw std::invalid_argument("H264StreamEncoder width and height must be even for NV12");
    }
    if (config.fps <= 0) {
        throw std::invalid_argument("H264StreamEncoder FPS must be positive");
    }

    config_ = config;
    frameIndex_ = 0;
    frameDuration100ns_ = 10'000'000 / config_.fps;

    ThrowIfFailed(
        CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&transform_)),
        "CoCreateInstance(CMSH264EncoderMFT)");

    DWORD inputCount = 0;
    DWORD outputCount = 0;
    ThrowIfFailed(transform_->GetStreamCount(&inputCount, &outputCount), "IMFTransform::GetStreamCount");
    if (inputCount != 1 || outputCount != 1) {
        throw std::runtime_error("H264 encoder MFT did not expose one input and one output stream");
    }

    Microsoft::WRL::ComPtr<IMFMediaType> outputType;
    ThrowIfFailed(MFCreateMediaType(&outputType), "MFCreateMediaType(output)");
    ThrowIfFailed(outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "IMFMediaType::SetGUID(output major)");
    ThrowIfFailed(outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264), "IMFMediaType::SetGUID(output subtype)");
    ThrowIfFailed(outputType->SetUINT32(MF_MT_AVG_BITRATE, config_.bitrate), "IMFMediaType::SetUINT32(output bitrate)");
    ThrowIfFailed(outputType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High), "IMFMediaType::SetUINT32(output profile)");
    ThrowIfFailed(outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "IMFMediaType::SetUINT32(output interlace)");
    SetBt709ColorInfo(outputType.Get());
    SetVideoSize(outputType.Get(), MF_MT_FRAME_SIZE, config_.width, config_.height);
    SetVideoRatio(outputType.Get(), MF_MT_FRAME_RATE, config_.fps, 1);
    SetVideoRatio(outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    ThrowIfFailed(transform_->SetOutputType(outputStreamId_, outputType.Get(), 0), "IMFTransform::SetOutputType");

    Microsoft::WRL::ComPtr<IMFMediaType> inputType;
    ThrowIfFailed(MFCreateMediaType(&inputType), "MFCreateMediaType(input)");
    ThrowIfFailed(inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "IMFMediaType::SetGUID(input major)");
    ThrowIfFailed(inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12), "IMFMediaType::SetGUID(input subtype)");
    ThrowIfFailed(inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "IMFMediaType::SetUINT32(input interlace)");
    SetBt709ColorInfo(inputType.Get());
    SetVideoSize(inputType.Get(), MF_MT_FRAME_SIZE, config_.width, config_.height);
    SetVideoRatio(inputType.Get(), MF_MT_FRAME_RATE, config_.fps, 1);
    SetVideoRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    ThrowIfFailed(transform_->SetOutputType(outputStreamId_, outputType.Get(), 0), "IMFTransform::SetOutputType");
    ThrowIfFailed(transform_->SetInputType(inputStreamId_, inputType.Get(), 0), "IMFTransform::SetInputType");

    ThrowIfFailed(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "IMFTransform::ProcessMessage(BEGIN_STREAMING)");
    ThrowIfFailed(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "IMFTransform::ProcessMessage(START_OF_STREAM)");
}

std::vector<EncodedPacket> H264StreamEncoder::EncodeFrame(const CapturedFrame& frame)
{
    if (!transform_) {
        throw std::logic_error("H264StreamEncoder::Start must be called before EncodeFrame");
    }
    if (frame.width != config_.width || frame.height != config_.height) {
        throw std::runtime_error("Encoded frame size does not match stream encoder configuration");
    }
    std::vector<std::byte> convertedNv12;
    const std::byte* nv12Data = nullptr;
    size_t nv12Bytes = 0;
    const size_t expectedNv12Bytes = static_cast<size_t>(frame.width) * frame.height * 3 / 2;
    if (!frame.nv12Pixels.empty()) {
        if (frame.nv12Pixels.size() != expectedNv12Bytes) {
            throw std::runtime_error("GPU NV12 frame size does not match stream encoder configuration");
        }
        nv12Data = frame.nv12Pixels.data();
        nv12Bytes = frame.nv12Pixels.size();
    } else {
        if (!IsBgra8Format(frame.format)) {
            throw std::runtime_error(
                "H264StreamEncoder currently expects BGRA8 or NV12 frame data, got DXGI format " +
                std::to_string(static_cast<int>(frame.format)));
        }
        convertedNv12 = ConvertBgraToNv12(frame.pixels.data(), frame.rowPitch, frame.width, frame.height);
        nv12Data = convertedNv12.data();
        nv12Bytes = convertedNv12.size();
    }
    const DWORD bufferSize = static_cast<DWORD>(nv12Bytes);

    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    ThrowIfFailed(MFCreateMemoryBuffer(bufferSize, &buffer), "MFCreateMemoryBuffer(input)");

    BYTE* destination = nullptr;
    DWORD maxLength = 0;
    DWORD currentLength = 0;
    ThrowIfFailed(buffer->Lock(&destination, &maxLength, &currentLength), "IMFMediaBuffer::Lock(input)");
    std::memcpy(destination, nv12Data, nv12Bytes);
    ThrowIfFailed(buffer->Unlock(), "IMFMediaBuffer::Unlock(input)");
    ThrowIfFailed(buffer->SetCurrentLength(bufferSize), "IMFMediaBuffer::SetCurrentLength(input)");

    Microsoft::WRL::ComPtr<IMFSample> sample;
    ThrowIfFailed(MFCreateSample(&sample), "MFCreateSample(input)");
    ThrowIfFailed(sample->AddBuffer(buffer.Get()), "IMFSample::AddBuffer(input)");
    ThrowIfFailed(sample->SetSampleTime(frameIndex_ * frameDuration100ns_), "IMFSample::SetSampleTime(input)");
    ThrowIfFailed(sample->SetSampleDuration(frameDuration100ns_), "IMFSample::SetSampleDuration(input)");

    const HRESULT inputResult = transform_->ProcessInput(inputStreamId_, sample.Get(), 0);
    if (inputResult == MF_E_NOTACCEPTING) {
        auto packets = ReadAvailablePackets();
        ThrowIfFailed(transform_->ProcessInput(inputStreamId_, sample.Get(), 0), "IMFTransform::ProcessInput");
        ++frameIndex_;
        auto extraPackets = ReadAvailablePackets();
        packets.insert(
            packets.end(),
            std::make_move_iterator(extraPackets.begin()),
            std::make_move_iterator(extraPackets.end()));
        return packets;
    }

    ThrowIfFailed(inputResult, "IMFTransform::ProcessInput");
    ++frameIndex_;
    return ReadAvailablePackets();
}

std::vector<EncodedPacket> H264StreamEncoder::Drain()
{
    if (!transform_) {
        return {};
    }

    ThrowIfFailed(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0), "IMFTransform::ProcessMessage(END_OF_STREAM)");
    ThrowIfFailed(transform_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0), "IMFTransform::ProcessMessage(DRAIN)");
    return ReadAvailablePackets();
}

void H264StreamEncoder::Stop()
{
    transform_.Reset();
}

std::vector<EncodedPacket> H264StreamEncoder::ReadAvailablePackets()
{
    std::vector<EncodedPacket> packets;

    while (true) {
        MFT_OUTPUT_STREAM_INFO streamInfo{};
        ThrowIfFailed(transform_->GetOutputStreamInfo(outputStreamId_, &streamInfo), "IMFTransform::GetOutputStreamInfo");

        Microsoft::WRL::ComPtr<IMFMediaBuffer> outputBuffer;
        Microsoft::WRL::ComPtr<IMFSample> outputSample;
        HRESULT outputResult = S_OK;
        DWORD outputBufferSize = streamInfo.cbSize == 0 ? 1'048'576 : streamInfo.cbSize;

        while (true) {
            outputBuffer.Reset();
            outputSample.Reset();

            ThrowIfFailed(MFCreateMemoryBuffer(outputBufferSize, &outputBuffer), "MFCreateMemoryBuffer(output)");
            ThrowIfFailed(MFCreateSample(&outputSample), "MFCreateSample(output)");
            ThrowIfFailed(outputSample->AddBuffer(outputBuffer.Get()), "IMFSample::AddBuffer(output)");

            MFT_OUTPUT_DATA_BUFFER outputData{};
            outputData.dwStreamID = outputStreamId_;
            outputData.pSample = outputSample.Get();

            DWORD status = 0;
            outputResult = transform_->ProcessOutput(0, 1, &outputData, &status);

            if (outputData.pEvents != nullptr) {
                outputData.pEvents->Release();
            }

            if (outputResult != MF_E_BUFFERTOOSMALL) {
                break;
            }

            if (outputBufferSize >= 64 * 1'048'576) {
                ThrowIfFailed(outputResult, "IMFTransform::ProcessOutput");
            }

            outputBufferSize *= 2;
        }

        if (outputResult == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            break;
        }
        if (outputResult == MF_E_TRANSFORM_STREAM_CHANGE) {
            continue;
        }

        ThrowIfFailed(outputResult, "IMFTransform::ProcessOutput");

        DWORD totalLength = 0;
        ThrowIfFailed(outputSample->GetTotalLength(&totalLength), "IMFSample::GetTotalLength");

        EncodedPacket packet;
        LONGLONG sampleTime = 0;
        LONGLONG sampleDuration = 0;
        if (SUCCEEDED(outputSample->GetSampleTime(&sampleTime))) {
            packet.timestamp100ns = sampleTime;
        }
        if (SUCCEEDED(outputSample->GetSampleDuration(&sampleDuration))) {
            packet.duration100ns = sampleDuration;
        }

        packet.bytes.resize(totalLength);

        BYTE* source = nullptr;
        DWORD maxLength = 0;
        DWORD currentLength = 0;
        ThrowIfFailed(outputBuffer->Lock(&source, &maxLength, &currentLength), "IMFMediaBuffer::Lock(output)");
        std::memcpy(packet.bytes.data(), source, std::min<DWORD>(totalLength, currentLength));
        ThrowIfFailed(outputBuffer->Unlock(), "IMFMediaBuffer::Unlock(output)");

        packets.push_back(std::move(packet));
    }

    return packets;
}

} // namespace screenshare
