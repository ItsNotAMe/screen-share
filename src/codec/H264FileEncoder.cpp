#include "codec/H264FileEncoder.h"

#include "video/Nv12Convert.h"

#include <Windows.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>

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

void SetBt709ColorInfo(IMFMediaType* mediaType, MFNominalRange nominalRange)
{
    ThrowIfFailed(mediaType->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709), "IMFMediaType::SetUINT32(YUV matrix)");
    ThrowIfFailed(mediaType->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709), "IMFMediaType::SetUINT32(video primaries)");
    ThrowIfFailed(mediaType->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709), "IMFMediaType::SetUINT32(transfer function)");
    ThrowIfFailed(mediaType->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, nominalRange), "IMFMediaType::SetUINT32(nominal range)");
}

} // namespace

H264FileEncoder::H264FileEncoder()
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

H264FileEncoder::~H264FileEncoder()
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

void H264FileEncoder::Start(const H264EncoderConfig& config)
{
    Stop();

    if (config.outputPath.empty()) {
        throw std::invalid_argument("H264FileEncoder output path is empty");
    }
    if (config.width <= 0 || config.height <= 0) {
        throw std::invalid_argument("H264FileEncoder width and height must be positive");
    }
    if (config.fps <= 0) {
        throw std::invalid_argument("H264FileEncoder FPS must be positive");
    }

    config_ = config;
    frameIndex_ = 0;
    frameDuration100ns_ = 10'000'000 / config_.fps;

    Microsoft::WRL::ComPtr<IMFAttributes> writerAttributes;
    ThrowIfFailed(MFCreateAttributes(&writerAttributes, 1), "MFCreateAttributes");
    ThrowIfFailed(writerAttributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE), "IMFAttributes::SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING)");

    ThrowIfFailed(
        MFCreateSinkWriterFromURL(config_.outputPath.c_str(), nullptr, writerAttributes.Get(), &sinkWriter_),
        "MFCreateSinkWriterFromURL");

    Microsoft::WRL::ComPtr<IMFMediaType> outputType;
    ThrowIfFailed(MFCreateMediaType(&outputType), "MFCreateMediaType(output)");
    ThrowIfFailed(outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "IMFMediaType::SetGUID(MF_MT_MAJOR_TYPE)");
    ThrowIfFailed(outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264), "IMFMediaType::SetGUID(MF_MT_SUBTYPE)");
    ThrowIfFailed(outputType->SetUINT32(MF_MT_AVG_BITRATE, config_.bitrate), "IMFMediaType::SetUINT32(MF_MT_AVG_BITRATE)");
    ThrowIfFailed(outputType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High), "IMFMediaType::SetUINT32(MF_MT_MPEG2_PROFILE)");
    ThrowIfFailed(outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "IMFMediaType::SetUINT32(MF_MT_INTERLACE_MODE)");
    SetBt709ColorInfo(outputType.Get(), MFNominalRange_16_235);
    SetVideoSize(outputType.Get(), MF_MT_FRAME_SIZE, config_.width, config_.height);
    SetVideoRatio(outputType.Get(), MF_MT_FRAME_RATE, config_.fps, 1);
    SetVideoRatio(outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    ThrowIfFailed(sinkWriter_->AddStream(outputType.Get(), &streamIndex_), "IMFSinkWriter::AddStream");

    Microsoft::WRL::ComPtr<IMFMediaType> inputType;
    ThrowIfFailed(MFCreateMediaType(&inputType), "MFCreateMediaType(input)");
    ThrowIfFailed(inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "IMFMediaType::SetGUID(input major)");
    ThrowIfFailed(inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12), "IMFMediaType::SetGUID(input subtype)");
    ThrowIfFailed(inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "IMFMediaType::SetUINT32(input interlace)");
    SetBt709ColorInfo(inputType.Get(), MFNominalRange_16_235);
    SetVideoSize(inputType.Get(), MF_MT_FRAME_SIZE, config_.width, config_.height);
    SetVideoRatio(inputType.Get(), MF_MT_FRAME_RATE, config_.fps, 1);
    SetVideoRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    ThrowIfFailed(sinkWriter_->SetInputMediaType(streamIndex_, inputType.Get(), nullptr), "IMFSinkWriter::SetInputMediaType");
    ThrowIfFailed(sinkWriter_->BeginWriting(), "IMFSinkWriter::BeginWriting");
}

void H264FileEncoder::WriteFrame(const CapturedFrame& frame)
{
    if (!sinkWriter_) {
        throw std::logic_error("H264FileEncoder::Start must be called before WriteFrame");
    }
    if (frame.width != config_.width || frame.height != config_.height) {
        throw std::runtime_error("Encoded frame size does not match encoder configuration");
    }
    const bool isBgra =
        frame.format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        frame.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
        frame.format == DXGI_FORMAT_B8G8R8X8_UNORM ||
        frame.format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;

    if (!isBgra) {
        throw std::runtime_error(
            "H264FileEncoder currently expects a BGRA8 frame, got DXGI format " +
            std::to_string(static_cast<int>(frame.format)));
    }

    const auto nv12 = ConvertBgraToNv12(frame.pixels.data(), frame.rowPitch, frame.width, frame.height);
    const DWORD bufferSize = static_cast<DWORD>(nv12.size());

    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    ThrowIfFailed(MFCreateMemoryBuffer(bufferSize, &buffer), "MFCreateMemoryBuffer");

    BYTE* destination = nullptr;
    DWORD maxLength = 0;
    DWORD currentLength = 0;
    ThrowIfFailed(buffer->Lock(&destination, &maxLength, &currentLength), "IMFMediaBuffer::Lock");

    std::memcpy(destination, nv12.data(), nv12.size());

    ThrowIfFailed(buffer->Unlock(), "IMFMediaBuffer::Unlock");
    ThrowIfFailed(buffer->SetCurrentLength(bufferSize), "IMFMediaBuffer::SetCurrentLength");

    Microsoft::WRL::ComPtr<IMFSample> sample;
    ThrowIfFailed(MFCreateSample(&sample), "MFCreateSample");
    ThrowIfFailed(sample->AddBuffer(buffer.Get()), "IMFSample::AddBuffer");
    ThrowIfFailed(sample->SetSampleTime(frameIndex_ * frameDuration100ns_), "IMFSample::SetSampleTime");
    ThrowIfFailed(sample->SetSampleDuration(frameDuration100ns_), "IMFSample::SetSampleDuration");

    ThrowIfFailed(sinkWriter_->WriteSample(streamIndex_, sample.Get()), "IMFSinkWriter::WriteSample");
    ++frameIndex_;
}

void H264FileEncoder::Stop()
{
    if (sinkWriter_) {
        const HRESULT finalizeResult = sinkWriter_->Finalize();
        sinkWriter_.Reset();
        ThrowIfFailed(finalizeResult, "IMFSinkWriter::Finalize");
    }
}

std::wstring Widen(const std::string& text)
{
    if (text.empty()) {
        return {};
    }

    const int size = MultiByteToWideChar(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0);

    if (size <= 0) {
        return {};
    }

    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        result.data(),
        size);
    return result;
}

} // namespace screenshare
