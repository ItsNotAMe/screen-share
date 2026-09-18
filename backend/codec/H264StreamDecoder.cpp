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
#include <span>

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
    ThrowIfFailed(transform->QueryInterface(codecApiInterfaceId, reinterpret_cast<void**>(codecApi.GetAddressOf())),
        "H264 decoder codec API");
    // The Microsoft H264 decoder is the documented exception: this property
    // requires VT_UI4, unlike the encoder's VT_BOOL. Never silently ignore a
    // rejected request and then advertise a low-latency decoder.
    VARIANT value{}; value.vt = VT_UI4; value.ulVal = 1;
    ThrowIfFailed(codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &value), "H264 decoder low latency");
    VARIANT actual{};
    const HRESULT read = codecApi->GetValue(&CODECAPI_AVLowLatencyMode, &actual);
    const bool enabled = SUCCEEDED(read) && actual.vt == VT_UI4 && actual.ulVal != 0;
    VariantClear(&actual);
    ThrowIfFailed(read, "H264 decoder low-latency readback");
    if (!enabled) throw std::runtime_error("H264 decoder did not retain low-latency mode");
}

DWORD Nv12BufferBytes(UINT32 width, UINT32 height)
{
    const uint64_t bytes = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 3 / 2;
    if (bytes == 0 || bytes > std::numeric_limits<DWORD>::max()) {
        throw std::runtime_error("Decoded frame is too large");
    }

    return static_cast<DWORD>(bytes);
}

struct VisibleFrameArea {
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
};

bool TryReadVideoArea(IMFMediaType* mediaType, const GUID& key, int codedWidth, int codedHeight, VisibleFrameArea& area)
{
    if (mediaType == nullptr) {
        return false;
    }

    MFVideoArea mfArea{};
    UINT32 blobSize = 0;
    const HRESULT sizeResult = mediaType->GetBlobSize(key, &blobSize);
    if (sizeResult == MF_E_ATTRIBUTENOTFOUND) return false;
    ThrowIfFailed(sizeResult, "Decoder aperture metadata");
    if (blobSize != sizeof(mfArea)) throw std::runtime_error("Invalid decoder aperture size");
    ThrowIfFailed(mediaType->GetBlob(
            key,
            reinterpret_cast<UINT8*>(&mfArea),
            static_cast<UINT32>(sizeof(mfArea)),
            &blobSize), "Decoder aperture");

    VisibleFrameArea candidate;
    candidate.left = static_cast<int>(mfArea.OffsetX.value);
    candidate.top = static_cast<int>(mfArea.OffsetY.value);
    candidate.width = static_cast<int>(mfArea.Area.cx);
    candidate.height = static_cast<int>(mfArea.Area.cy);

    if (candidate.left < 0 ||
        candidate.top < 0 ||
        candidate.width <= 0 ||
        candidate.height <= 0 ||
        candidate.left > codedWidth || candidate.width > codedWidth - candidate.left ||
        candidate.top > codedHeight || candidate.height > codedHeight - candidate.top ||
        mfArea.OffsetX.fract != 0 || mfArea.OffsetY.fract != 0 ||
        (candidate.left % 2) != 0 ||
        (candidate.top % 2) != 0 ||
        (candidate.width % 2) != 0 ||
        (candidate.height % 2) != 0) {
        throw std::runtime_error("Decoder returned an invalid NV12 visible aperture");
    }

    area = candidate;
    return true;
}

VisibleFrameArea SelectVisibleFrameArea(IMFMediaType* outputType, int codedWidth, int codedHeight)
{
    VisibleFrameArea area{0, 0, codedWidth, codedHeight};
    for (const GUID* key : {
             &MF_MT_MINIMUM_DISPLAY_APERTURE,
             &MF_MT_GEOMETRIC_APERTURE,
             &MF_MT_PAN_SCAN_APERTURE,
         }) {
        if (TryReadVideoArea(outputType, *key, codedWidth, codedHeight, area)) {
            return area;
        }
    }
    return area;
}

std::vector<std::byte> CopyVisibleNv12(
    std::span<const std::byte> source,
    int codedWidth,
    int codedHeight,
    const VisibleFrameArea& visible)
{
    const uint64_t codedLumaBytes = static_cast<uint64_t>(codedWidth) * static_cast<uint64_t>(codedHeight);
    const uint64_t codedRequiredBytes = codedLumaBytes + codedLumaBytes / 2;
    const uint64_t visibleLumaBytes = static_cast<uint64_t>(visible.width) * static_cast<uint64_t>(visible.height);
    const uint64_t visibleRequiredBytes = visibleLumaBytes + visibleLumaBytes / 2;
    if (codedRequiredBytes > source.size() || visibleRequiredBytes > std::numeric_limits<size_t>::max()) {
        throw std::runtime_error("Decoded NV12 frame data is too small for visible crop");
    }

    std::vector<std::byte> cropped(static_cast<size_t>(visibleRequiredBytes));
    const auto* sourceY = source.data();
    const auto* sourceUv = sourceY + static_cast<size_t>(codedLumaBytes);
    auto* destinationY = cropped.data();
    auto* destinationUv = destinationY + static_cast<size_t>(visibleLumaBytes);

    for (int y = 0; y < visible.height; ++y) {
        const auto* row = sourceY +
            static_cast<size_t>(visible.top + y) * static_cast<size_t>(codedWidth) +
            static_cast<size_t>(visible.left);
        std::memcpy(
            destinationY + static_cast<size_t>(y) * static_cast<size_t>(visible.width),
            row,
            static_cast<size_t>(visible.width));
    }

    for (int y = 0; y < visible.height / 2; ++y) {
        const auto* row = sourceUv +
            static_cast<size_t>((visible.top / 2) + y) * static_cast<size_t>(codedWidth) +
            static_cast<size_t>(visible.left);
        std::memcpy(
            destinationUv + static_cast<size_t>(y) * static_cast<size_t>(visible.width),
            row,
            static_cast<size_t>(visible.width));
    }

    return cropped;
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

void H264StreamDecoder::Start(int maxWidth, int maxHeight, ID3D11Device* device)
{
    Stop();
    if (maxWidth <= 0 || maxHeight <= 0 || maxWidth > 16384 || maxHeight > 16384) {
        throw std::invalid_argument("Invalid H264 decoder dimension limit");
    }
    maxWidth_ = maxWidth;
    maxHeight_ = maxHeight;

    ThrowIfFailed(
        CoCreateInstance(H264DecoderClsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&transform_)),
        "CoCreateInstance(CMSH264DecoderMFT)");
    ConfigureLowLatencyDecoderOptions(transform_.Get());
    if (device) {
        Microsoft::WRL::ComPtr<IMFAttributes> attributes;
        ThrowIfFailed(transform_->GetAttributes(&attributes), "Decoder attributes");
        if (!MFGetAttributeUINT32(attributes.Get(), MF_SA_D3D11_AWARE, FALSE))
            throw std::runtime_error("Decoder does not support D3D11");
        UINT token = 0;
        ThrowIfFailed(MFCreateDXGIDeviceManager(&token, &manager_), "Decoder device manager");
        ThrowIfFailed(manager_->ResetDevice(device, token), "Decoder device binding");
        ThrowIfFailed(transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
            reinterpret_cast<ULONG_PTR>(manager_.Get())), "Decoder acceleration binding");
        device_ = device;
    }

    DWORD inputCount = 0;
    DWORD outputCount = 0;
    ThrowIfFailed(transform_->GetStreamCount(&inputCount, &outputCount), "IMFTransform::GetStreamCount(decoder)");
    if (inputCount != 1 || outputCount != 1) {
        throw std::runtime_error("H264 decoder MFT did not expose one input and one output stream");
    }

    // Each packet is one complete Annex-B access unit. H264_ES allows partial
    // pictures and makes the parser wait for a subsequent frame boundary.
    SetInputSubtype(transform_.Get(), inputStreamId_, MFVideoFormat_H264);

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
    manager_.Reset(); device_.Reset();
    outputWidth_ = 0;
    outputHeight_ = 0;
    outputVisibleLeft_ = 0;
    outputVisibleTop_ = 0;
    outputVisibleWidth_ = 0;
    outputVisibleHeight_ = 0;
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
        // Validate before allocating output buffers or accepting a remote stream type.
        if (width == 0 || height == 0 || width > static_cast<UINT32>(maxWidth_) ||
            height > static_cast<UINT32>(maxHeight_) || width % 2 != 0 || height % 2 != 0) {
            throw std::runtime_error("H264 decoded dimensions exceed configured limits");
        }
        ThrowIfFailed(transform_->SetOutputType(outputStreamId_, outputType.Get(), 0), "IMFTransform::SetOutputType(decoder)");

        outputWidth_ = static_cast<int>(width);
        outputHeight_ = static_cast<int>(height);
        const VisibleFrameArea visible = SelectVisibleFrameArea(outputType.Get(), outputWidth_, outputHeight_);
        outputVisibleLeft_ = visible.left;
        outputVisibleTop_ = visible.top;
        outputVisibleWidth_ = visible.width;
        outputVisibleHeight_ = visible.height;
        outputBufferBytes_ = Nv12BufferBytes(width, height);
        outputTypeConfigured_ = true;
        return true;
    }
}

std::vector<DecodedFrameInfo> H264StreamDecoder::ReadAvailableFrames()
{
    std::vector<DecodedFrameInfo> frames;

    unsigned operations = 0;
    while (true) {
        if (++operations > 64 || frames.size() >= 32) throw std::runtime_error("Decoder output work limit exceeded");
        if (!outputTypeConfigured_ && !TryConfigureOutputType()) {
            break;
        }

        MFT_OUTPUT_STREAM_INFO streamInfo{};
        ThrowIfFailed(transform_->GetOutputStreamInfo(outputStreamId_, &streamInfo), "IMFTransform::GetOutputStreamInfo(decoder)");

        DWORD outputBufferBytes = std::max(outputBufferBytes_, streamInfo.cbSize);
        if (outputBufferBytes > 128 * 1'048'576) {
            throw std::runtime_error("H264 decoder requested excessive output storage");
        }
        if (outputBufferBytes == 0) {
            outputBufferBytes = 16 * 1'048'576;
        }

        while (true) {
            Microsoft::WRL::ComPtr<IMFMediaBuffer> outputBuffer;
            Microsoft::WRL::ComPtr<IMFSample> outputSample;
            if (!(streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
                ThrowIfFailed(MFCreateMemoryBuffer(outputBufferBytes, &outputBuffer), "MFCreateMemoryBuffer(decoder output)");
                ThrowIfFailed(MFCreateSample(&outputSample), "MFCreateSample(decoder output)");
                ThrowIfFailed(outputSample->AddBuffer(outputBuffer.Get()), "IMFSample::AddBuffer(decoder output)");
            }

            MFT_OUTPUT_DATA_BUFFER outputData{};
            outputData.dwStreamID = outputStreamId_;
            outputData.pSample = outputSample.Get();

            DWORD status = 0;
            const HRESULT outputResult = transform_->ProcessOutput(0, 1, &outputData, &status);
            if (!outputSample && outputData.pSample) outputSample.Attach(outputData.pSample);

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
                outputVisibleLeft_ = 0;
                outputVisibleTop_ = 0;
                outputVisibleWidth_ = 0;
                outputVisibleHeight_ = 0;
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
            if (!outputSample) throw std::runtime_error("Decoder returned no sample");

            DecodedFrameInfo frame;
            frame.width = outputVisibleWidth_ > 0 ? outputVisibleWidth_ : outputWidth_;
            frame.height = outputVisibleHeight_ > 0 ? outputVisibleHeight_ : outputHeight_;
            frame.codedWidth = outputWidth_;
            frame.codedHeight = outputHeight_;

            LONGLONG sampleTime = 0;
            LONGLONG sampleDuration = 0;
            if (SUCCEEDED(outputSample->GetSampleTime(&sampleTime))) {
                frame.timestamp100ns = sampleTime;
            }
            if (SUCCEEDED(outputSample->GetSampleDuration(&sampleDuration))) {
                frame.duration100ns = sampleDuration;
            }

            if (device_) {
                Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
                Microsoft::WRL::ComPtr<IMFDXGIBuffer> dxgi;
                Microsoft::WRL::ComPtr<ID3D11Texture2D> decoded;
                ThrowIfFailed(outputSample->GetBufferByIndex(0, &buffer), "Decoder GPU buffer");
                ThrowIfFailed(buffer.As(&dxgi), "Decoder GPU output required");
                ThrowIfFailed(dxgi->GetResource(IID_PPV_ARGS(&decoded)), "Decoder GPU texture");
                UINT subresource = 0; ThrowIfFailed(dxgi->GetSubresourceIndex(&subresource), "Decoder GPU slice");
                D3D11_TEXTURE2D_DESC description{}; decoded->GetDesc(&description);
                Microsoft::WRL::ComPtr<ID3D11Device> actualDevice; decoded->GetDevice(&actualDevice);
                if (description.Format != DXGI_FORMAT_NV12 || description.MipLevels != 1 || description.SampleDesc.Count != 1 ||
                    subresource >= description.ArraySize || actualDevice.Get() != device_.Get() ||
                    description.Width < UINT(outputWidth_) || description.Height < UINT(outputHeight_))
                    throw std::runtime_error("Invalid decoder GPU resource");
                // Detach from MF's recycled surface pool. One GPU copy, no CPU
                // readback/upload; published textures are immutable and owned.
                description.Width = frame.width; description.Height = frame.height;
                description.ArraySize = 1; description.Usage = D3D11_USAGE_DEFAULT;
                description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                description.CPUAccessFlags = description.MiscFlags = 0;
                ThrowIfFailed(device_->CreateTexture2D(&description, nullptr, &frame.texture), "Owned decoder texture");
                Microsoft::WRL::ComPtr<ID3D11DeviceContext> context; device_->GetImmediateContext(&context);
                D3D11_BOX crop{UINT(outputVisibleLeft_), UINT(outputVisibleTop_), 0,
                    UINT(outputVisibleLeft_ + frame.width), UINT(outputVisibleTop_ + frame.height), 1};
                context->CopySubresourceRegion(frame.texture.Get(), 0, 0, 0, 0, decoded.Get(), subresource, &crop);
                context->Flush();
                ThrowIfFailed(device_->GetDeviceRemovedReason(), "Decoder device health");
                frames.push_back(std::move(frame));
                break;
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

            if (frame.width != frame.codedWidth ||
                frame.height != frame.codedHeight ||
                outputVisibleLeft_ != 0 ||
                outputVisibleTop_ != 0) {
                frame.data = CopyVisibleNv12(
                    frame.data,
                    frame.codedWidth,
                    frame.codedHeight,
                    VisibleFrameArea{outputVisibleLeft_, outputVisibleTop_, frame.width, frame.height});
                frame.bytes = static_cast<uint32_t>(frame.data.size());
            }

            frames.push_back(std::move(frame));
            break;
        }
    }

    return frames;
}

} // namespace screenshare
