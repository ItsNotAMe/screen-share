#include "codec/H264StreamDecoder.h"

#include <Windows.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>

namespace screenshare {
namespace {

constexpr GUID H264DecoderClsid = {
    0x62ce7e72,
    0x4c71,
    0x4d20,
    {0xb1, 0x5d, 0x45, 0x28, 0x31, 0xa8, 0x7d, 0x9d},
};

void ThrowIfFailed(HRESULT hr, const char* operation)
{
    if (FAILED(hr)) {
        throw std::runtime_error(std::string(operation) + " failed: " + HResultMessage(hr));
    }
}

void SetInputSubtype(IMFTransform* transform, DWORD streamId, REFGUID subtype)
{
    Microsoft::WRL::ComPtr<IMFMediaType> inputType;
    ThrowIfFailed(MFCreateMediaType(&inputType), "MFCreateMediaType(decoder input)");
    ThrowIfFailed(inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "IMFMediaType::SetGUID(decoder input major)");
    ThrowIfFailed(inputType->SetGUID(MF_MT_SUBTYPE, subtype), "IMFMediaType::SetGUID(decoder input subtype)");
    ThrowIfFailed(inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "IMFMediaType::SetUINT32(decoder input interlace)");
    ThrowIfFailed(transform->SetInputType(streamId, inputType.Get(), 0), "IMFTransform::SetInputType(decoder)");
}

bool TrySetCodecApiBool(ICodecAPI* codecApi, const GUID& key, bool setting)
{
    if (codecApi == nullptr) {
        return false;
    }

    VARIANT value{};
    value.vt = VT_BOOL;
    value.boolVal = setting ? VARIANT_TRUE : VARIANT_FALSE;
    return SUCCEEDED(codecApi->SetValue(&key, &value));
}

void ConfigureLowLatencyDecoderOptions(IMFTransform* transform)
{
    if (transform == nullptr) {
        return;
    }

    Microsoft::WRL::ComPtr<IMFAttributes> attributes;
    if (SUCCEEDED(transform->GetAttributes(&attributes)) && attributes) {
        static_cast<void>(attributes->SetUINT32(MF_LOW_LATENCY, TRUE));
    }

    static constexpr GUID codecApiInterfaceId = {
        0x901db4c7,
        0x31ce,
        0x41a2,
        {0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda},
    };
    Microsoft::WRL::ComPtr<ICodecAPI> codecApi;
    if (SUCCEEDED(transform->QueryInterface(codecApiInterfaceId, reinterpret_cast<void**>(codecApi.GetAddressOf()))) &&
        codecApi) {
        TrySetCodecApiBool(codecApi.Get(), CODECAPI_AVLowLatencyMode, true);
    }
}

DWORD Nv12BufferBytes(UINT32 width, UINT32 height)
{
    const uint64_t bytes = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 3 / 2;
    if (bytes == 0 || bytes > std::numeric_limits<DWORD>::max()) {
        throw std::runtime_error("Decoded frame is too large");
    }

    return static_cast<DWORD>(bytes);
}

} // namespace

H264StreamDecoder::H264StreamDecoder()
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

H264StreamDecoder::~H264StreamDecoder()
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

void H264StreamDecoder::Start()
{
    Stop();

    ThrowIfFailed(
        CoCreateInstance(H264DecoderClsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&transform_)),
        "CoCreateInstance(CMSH264DecoderMFT)");
    ConfigureLowLatencyDecoderOptions(transform_.Get());

    DWORD inputCount = 0;
    DWORD outputCount = 0;
    ThrowIfFailed(transform_->GetStreamCount(&inputCount, &outputCount), "IMFTransform::GetStreamCount(decoder)");
    if (inputCount != 1 || outputCount != 1) {
        throw std::runtime_error("H264 decoder MFT did not expose one input and one output stream");
    }

    const HRESULT h264EsResult = [&]() -> HRESULT {
        Microsoft::WRL::ComPtr<IMFMediaType> inputType;
        HRESULT hr = MFCreateMediaType(&inputType);
        if (FAILED(hr)) {
            return hr;
        }
        hr = inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (FAILED(hr)) {
            return hr;
        }
        hr = inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264_ES);
        if (FAILED(hr)) {
            return hr;
        }
        hr = inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (FAILED(hr)) {
            return hr;
        }
        return transform_->SetInputType(inputStreamId_, inputType.Get(), 0);
    }();

    if (FAILED(h264EsResult)) {
        SetInputSubtype(transform_.Get(), inputStreamId_, MFVideoFormat_H264);
    }

    ThrowIfFailed(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "IMFTransform::ProcessMessage(decoder BEGIN_STREAMING)");
    ThrowIfFailed(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "IMFTransform::ProcessMessage(decoder START_OF_STREAM)");
}

std::vector<DecodedFrameInfo> H264StreamDecoder::DecodePacket(const EncodedPacket& packet)
{
    if (!transform_) {
        throw std::logic_error("H264StreamDecoder::Start must be called before DecodePacket");
    }
    if (packet.bytes.empty()) {
        return ReadAvailableFrames();
    }
    if (packet.bytes.size() > std::numeric_limits<DWORD>::max()) {
        throw std::runtime_error("Encoded packet is too large for H264 decoder");
    }

    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    ThrowIfFailed(MFCreateMemoryBuffer(static_cast<DWORD>(packet.bytes.size()), &buffer), "MFCreateMemoryBuffer(decoder input)");

    BYTE* destination = nullptr;
    DWORD maxLength = 0;
    DWORD currentLength = 0;
    ThrowIfFailed(buffer->Lock(&destination, &maxLength, &currentLength), "IMFMediaBuffer::Lock(decoder input)");
    std::memcpy(destination, packet.bytes.data(), packet.bytes.size());
    ThrowIfFailed(buffer->Unlock(), "IMFMediaBuffer::Unlock(decoder input)");
    ThrowIfFailed(buffer->SetCurrentLength(static_cast<DWORD>(packet.bytes.size())), "IMFMediaBuffer::SetCurrentLength(decoder input)");

    Microsoft::WRL::ComPtr<IMFSample> sample;
    ThrowIfFailed(MFCreateSample(&sample), "MFCreateSample(decoder input)");
    ThrowIfFailed(sample->AddBuffer(buffer.Get()), "IMFSample::AddBuffer(decoder input)");
    ThrowIfFailed(sample->SetSampleTime(packet.timestamp100ns), "IMFSample::SetSampleTime(decoder input)");
    ThrowIfFailed(sample->SetSampleDuration(packet.duration100ns), "IMFSample::SetSampleDuration(decoder input)");

    const HRESULT inputResult = transform_->ProcessInput(inputStreamId_, sample.Get(), 0);
    if (inputResult == MF_E_NOTACCEPTING) {
        auto frames = ReadAvailableFrames();
        ThrowIfFailed(transform_->ProcessInput(inputStreamId_, sample.Get(), 0), "IMFTransform::ProcessInput(decoder)");
        auto extraFrames = ReadAvailableFrames();
        frames.insert(
            frames.end(),
            std::make_move_iterator(extraFrames.begin()),
            std::make_move_iterator(extraFrames.end()));
        return frames;
    }

    ThrowIfFailed(inputResult, "IMFTransform::ProcessInput(decoder)");
    return ReadAvailableFrames();
}

std::vector<DecodedFrameInfo> H264StreamDecoder::Drain()
{
    if (!transform_) {
        return {};
    }

    ThrowIfFailed(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0), "IMFTransform::ProcessMessage(decoder END_OF_STREAM)");
    ThrowIfFailed(transform_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0), "IMFTransform::ProcessMessage(decoder DRAIN)");
    return ReadAvailableFrames();
}

void H264StreamDecoder::Stop()
{
    transform_.Reset();
    outputWidth_ = 0;
    outputHeight_ = 0;
    outputBufferBytes_ = 0;
    outputTypeConfigured_ = false;
}

bool H264StreamDecoder::TryConfigureOutputType()
{
    if (!transform_) {
        return false;
    }

    bool sawOutputType = false;
    for (DWORD index = 0;; ++index) {
        Microsoft::WRL::ComPtr<IMFMediaType> outputType;
        const HRESULT typeResult = transform_->GetOutputAvailableType(outputStreamId_, index, &outputType);
        if (typeResult == MF_E_NO_MORE_TYPES) {
            if (sawOutputType) {
                throw std::runtime_error("H264 decoder did not offer NV12 output");
            }
            return false;
        }
        if (typeResult == MF_E_TRANSFORM_TYPE_NOT_SET) {
            return false;
        }
        ThrowIfFailed(typeResult, "IMFTransform::GetOutputAvailableType(decoder)");
        sawOutputType = true;

        GUID subtype{};
        if (FAILED(outputType->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != MFVideoFormat_NV12) {
            continue;
        }

        UINT32 width = 0;
        UINT32 height = 0;
        ThrowIfFailed(MFGetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, &width, &height), "MFGetAttributeSize(decoder output)");
        ThrowIfFailed(transform_->SetOutputType(outputStreamId_, outputType.Get(), 0), "IMFTransform::SetOutputType(decoder)");

        outputWidth_ = static_cast<int>(width);
        outputHeight_ = static_cast<int>(height);
        outputBufferBytes_ = Nv12BufferBytes(width, height);
        outputTypeConfigured_ = true;
        return true;
    }
}

std::vector<DecodedFrameInfo> H264StreamDecoder::ReadAvailableFrames()
{
    std::vector<DecodedFrameInfo> frames;

    while (true) {
        if (!outputTypeConfigured_ && !TryConfigureOutputType()) {
            break;
        }

        MFT_OUTPUT_STREAM_INFO streamInfo{};
        ThrowIfFailed(transform_->GetOutputStreamInfo(outputStreamId_, &streamInfo), "IMFTransform::GetOutputStreamInfo(decoder)");

        DWORD outputBufferBytes = std::max(outputBufferBytes_, streamInfo.cbSize);
        if (outputBufferBytes == 0) {
            outputBufferBytes = 16 * 1'048'576;
        }

        while (true) {
            Microsoft::WRL::ComPtr<IMFMediaBuffer> outputBuffer;
            Microsoft::WRL::ComPtr<IMFSample> outputSample;
            ThrowIfFailed(MFCreateMemoryBuffer(outputBufferBytes, &outputBuffer), "MFCreateMemoryBuffer(decoder output)");
            ThrowIfFailed(MFCreateSample(&outputSample), "MFCreateSample(decoder output)");
            ThrowIfFailed(outputSample->AddBuffer(outputBuffer.Get()), "IMFSample::AddBuffer(decoder output)");

            MFT_OUTPUT_DATA_BUFFER outputData{};
            outputData.dwStreamID = outputStreamId_;
            outputData.pSample = outputSample.Get();

            DWORD status = 0;
            const HRESULT outputResult = transform_->ProcessOutput(0, 1, &outputData, &status);

            if (outputData.pEvents != nullptr) {
                outputData.pEvents->Release();
            }

            if (outputResult == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                return frames;
            }
            if (outputResult == MF_E_TRANSFORM_STREAM_CHANGE) {
                outputTypeConfigured_ = false;
                outputWidth_ = 0;
                outputHeight_ = 0;
                outputBufferBytes_ = 0;
                break;
            }
            if (outputResult == MF_E_BUFFERTOOSMALL) {
                if (outputBufferBytes >= 64 * 1'048'576) {
                    ThrowIfFailed(outputResult, "IMFTransform::ProcessOutput(decoder)");
                }

                outputBufferBytes *= 2;
                outputBufferBytes_ = outputBufferBytes;
                continue;
            }

            ThrowIfFailed(outputResult, "IMFTransform::ProcessOutput(decoder)");

            DecodedFrameInfo frame;
            frame.width = outputWidth_;
            frame.height = outputHeight_;

            LONGLONG sampleTime = 0;
            LONGLONG sampleDuration = 0;
            if (SUCCEEDED(outputSample->GetSampleTime(&sampleTime))) {
                frame.timestamp100ns = sampleTime;
            }
            if (SUCCEEDED(outputSample->GetSampleDuration(&sampleDuration))) {
                frame.duration100ns = sampleDuration;
            }

            Microsoft::WRL::ComPtr<IMFMediaBuffer> contiguousBuffer;
            ThrowIfFailed(outputSample->ConvertToContiguousBuffer(&contiguousBuffer), "IMFSample::ConvertToContiguousBuffer(decoder output)");

            DWORD currentLength = 0;
            ThrowIfFailed(contiguousBuffer->GetCurrentLength(&currentLength), "IMFMediaBuffer::GetCurrentLength(decoder output)");
            frame.bytes = currentLength;
            frame.data.resize(currentLength);

            BYTE* source = nullptr;
            DWORD maxLength = 0;
            DWORD lockedCurrentLength = 0;
            ThrowIfFailed(contiguousBuffer->Lock(&source, &maxLength, &lockedCurrentLength), "IMFMediaBuffer::Lock(decoder output)");
            std::memcpy(frame.data.data(), source, std::min(currentLength, lockedCurrentLength));
            ThrowIfFailed(contiguousBuffer->Unlock(), "IMFMediaBuffer::Unlock(decoder output)");

            frames.push_back(frame);
            break;
        }
    }

    return frames;
}

} // namespace screenshare
