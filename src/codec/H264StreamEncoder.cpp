#include "codec/H264StreamEncoder.h"

#include "video/Nv12Convert.h"

#include <Windows.h>
#include <codecapi.h>
#include <d3d10.h>
#include <d3d11.h>
#include <initguid.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <utility>

namespace screenshare {
namespace {

void ThrowIfFailed(HRESULT hr, const char* operation)
{
    if (FAILED(hr)) {
        throw std::runtime_error(std::string(operation) + " failed: " + HResultMessage(hr));
    }
}

void AppendPackets(std::vector<EncodedPacket>& destination, std::vector<EncodedPacket>&& source)
{
    destination.insert(
        destination.end(),
        std::make_move_iterator(source.begin()),
        std::make_move_iterator(source.end()));
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

std::string GetAllocatedString(IMFAttributes* attributes, REFGUID key)
{
    wchar_t* value = nullptr;
    UINT32 length = 0;
    const HRESULT result = attributes->GetAllocatedString(key, &value, &length);
    if (FAILED(result) || value == nullptr) {
        return {};
    }

    std::wstring text(value, length);
    CoTaskMemFree(value);
    return Narrow(text);
}

bool GetBoolAttribute(IMFAttributes* attributes, REFGUID key)
{
    UINT32 value = 0;
    return attributes != nullptr && SUCCEEDED(attributes->GetUINT32(key, &value)) && value != 0;
}

void TrySetCodecApiBool(ICodecAPI* codecApi, const GUID& key, bool enabled)
{
    if (codecApi == nullptr || codecApi->IsSupported(&key) != S_OK) {
        return;
    }

    VARIANT value{};
    value.vt = VT_BOOL;
    value.boolVal = enabled ? VARIANT_TRUE : VARIANT_FALSE;
    static_cast<void>(codecApi->SetValue(&key, &value));
}

void TrySetCodecApiUInt32(ICodecAPI* codecApi, const GUID& key, uint32_t setting)
{
    if (codecApi == nullptr || codecApi->IsSupported(&key) != S_OK) {
        return;
    }

    VARIANT value{};
    value.vt = VT_UI4;
    value.ulVal = setting;
    static_cast<void>(codecApi->SetValue(&key, &value));
}

void ConfigureLowLatencyEncoderOptions(IMFTransform* transform, const H264StreamEncoderConfig& config)
{
    static constexpr GUID codecApiInterfaceId = {
        0x901db4c7,
        0x31ce,
        0x41a2,
        {0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda},
    };

    Microsoft::WRL::ComPtr<ICodecAPI> codecApi;
    if (FAILED(transform->QueryInterface(codecApiInterfaceId, reinterpret_cast<void**>(codecApi.GetAddressOf()))) || !codecApi) {
        return;
    }

    TrySetCodecApiBool(codecApi.Get(), CODECAPI_AVLowLatencyMode, true);
    TrySetCodecApiUInt32(codecApi.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    if (config.keyframeIntervalFrames > 0) {
        TrySetCodecApiUInt32(codecApi.Get(), CODECAPI_AVEncMPVGOPSize, config.keyframeIntervalFrames);
        TrySetCodecApiUInt32(codecApi.Get(), CODECAPI_AVEncMPVGOPSizeMin, config.keyframeIntervalFrames);
        TrySetCodecApiUInt32(codecApi.Get(), CODECAPI_AVEncMPVGOPSizeMax, config.keyframeIntervalFrames);
        TrySetCodecApiUInt32(codecApi.Get(), CODECAPI_AVEncVideoNumGOPsPerIDR, 1);
        TrySetCodecApiBool(codecApi.Get(), CODECAPI_AVEncMPVGOPOpen, false);
    }
}

Microsoft::WRL::ComPtr<IMFMediaType> CreateH264OutputType(const H264StreamEncoderConfig& config)
{
    Microsoft::WRL::ComPtr<IMFMediaType> outputType;
    ThrowIfFailed(MFCreateMediaType(&outputType), "MFCreateMediaType(output)");
    ThrowIfFailed(outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "IMFMediaType::SetGUID(output major)");
    ThrowIfFailed(outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264), "IMFMediaType::SetGUID(output subtype)");
    ThrowIfFailed(outputType->SetUINT32(MF_MT_AVG_BITRATE, config.bitrate), "IMFMediaType::SetUINT32(output bitrate)");
    ThrowIfFailed(outputType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High), "IMFMediaType::SetUINT32(output profile)");
    ThrowIfFailed(outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "IMFMediaType::SetUINT32(output interlace)");
    SetBt709ColorInfo(outputType.Get());
    SetVideoSize(outputType.Get(), MF_MT_FRAME_SIZE, config.width, config.height);
    SetVideoRatio(outputType.Get(), MF_MT_FRAME_RATE, config.fps, 1);
    SetVideoRatio(outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    return outputType;
}

Microsoft::WRL::ComPtr<IMFMediaType> CreateNv12InputType(const H264StreamEncoderConfig& config)
{
    Microsoft::WRL::ComPtr<IMFMediaType> inputType;
    ThrowIfFailed(MFCreateMediaType(&inputType), "MFCreateMediaType(input)");
    ThrowIfFailed(inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "IMFMediaType::SetGUID(input major)");
    ThrowIfFailed(inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12), "IMFMediaType::SetGUID(input subtype)");
    ThrowIfFailed(inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "IMFMediaType::SetUINT32(input interlace)");
    SetBt709ColorInfo(inputType.Get());
    SetVideoSize(inputType.Get(), MF_MT_FRAME_SIZE, config.width, config.height);
    SetVideoRatio(inputType.Get(), MF_MT_FRAME_RATE, config.fps, 1);
    SetVideoRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    return inputType;
}

std::pair<DWORD, DWORD> GetSingleStreamIds(IMFTransform* transform)
{
    DWORD inputCount = 0;
    DWORD outputCount = 0;
    ThrowIfFailed(transform->GetStreamCount(&inputCount, &outputCount), "IMFTransform::GetStreamCount");
    if (inputCount != 1 || outputCount != 1) {
        throw std::runtime_error("H264 encoder MFT did not expose one input and one output stream");
    }

    DWORD inputStreamId = 0;
    DWORD outputStreamId = 0;
    const HRESULT streamIdsResult = transform->GetStreamIDs(1, &inputStreamId, 1, &outputStreamId);
    if (streamIdsResult == E_NOTIMPL) {
        return {0, 0};
    }
    ThrowIfFailed(streamIdsResult, "IMFTransform::GetStreamIDs");
    return {inputStreamId, outputStreamId};
}

void ConfigureEncoderTypes(
    IMFTransform* transform,
    const H264StreamEncoderConfig& config,
    DWORD& inputStreamId,
    DWORD& outputStreamId)
{
    const auto streamIds = GetSingleStreamIds(transform);
    inputStreamId = streamIds.first;
    outputStreamId = streamIds.second;

    const auto outputType = CreateH264OutputType(config);
    const auto inputType = CreateNv12InputType(config);
    ThrowIfFailed(transform->SetOutputType(outputStreamId, outputType.Get(), 0), "IMFTransform::SetOutputType");
    ThrowIfFailed(transform->SetInputType(inputStreamId, inputType.Get(), 0), "IMFTransform::SetInputType");
}

Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> CreateDxgiDeviceManager(ID3D11Device* preferredDevice)
{
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;

    if (preferredDevice != nullptr) {
        device = preferredDevice;
        preferredDevice->GetImmediateContext(&context);
    } else {
        static constexpr D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };

        D3D_FEATURE_LEVEL selectedFeatureLevel{};
        ThrowIfFailed(
            D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_HARDWARE,
                nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                featureLevels,
                ARRAYSIZE(featureLevels),
                D3D11_SDK_VERSION,
                &device,
                &selectedFeatureLevel,
                &context),
            "D3D11CreateDevice(stream encoder)");
    }

    Microsoft::WRL::ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(device.As(&multithread)) && multithread) {
        multithread->SetMultithreadProtected(TRUE);
    }

    UINT resetToken = 0;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> deviceManager;
    ThrowIfFailed(MFCreateDXGIDeviceManager(&resetToken, &deviceManager), "MFCreateDXGIDeviceManager");
    ThrowIfFailed(deviceManager->ResetDevice(device.Get(), resetToken), "IMFDXGIDeviceManager::ResetDevice");
    return deviceManager;
}

class ActivateList {
public:
    ActivateList() = default;
    ~ActivateList()
    {
        for (UINT32 i = 0; i < count_; ++i) {
            if (items_[i] != nullptr) {
                items_[i]->Release();
            }
        }
        CoTaskMemFree(items_);
    }

    ActivateList(const ActivateList&) = delete;
    ActivateList& operator=(const ActivateList&) = delete;

    IMFActivate*** put() { return &items_; }
    UINT32* countPut() { return &count_; }
    IMFActivate* operator[](UINT32 index) const { return items_[index]; }
    UINT32 count() const { return count_; }

private:
    IMFActivate** items_ = nullptr;
    UINT32 count_ = 0;
};

struct HardwareEncoderSelection {
    Microsoft::WRL::ComPtr<IMFTransform> transform;
    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> eventGenerator;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> dxgiDeviceManager;
    DWORD inputStreamId = 0;
    DWORD outputStreamId = 0;
    std::string name;
};

HardwareEncoderSelection CreateHardwareEncoder(const H264StreamEncoderConfig& config)
{
    MFT_REGISTER_TYPE_INFO inputInfo{};
    inputInfo.guidMajorType = MFMediaType_Video;
    inputInfo.guidSubtype = MFVideoFormat_NV12;

    MFT_REGISTER_TYPE_INFO outputInfo{};
    outputInfo.guidMajorType = MFMediaType_Video;
    outputInfo.guidSubtype = MFVideoFormat_H264;

    ActivateList activates;
    const HRESULT enumResult = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        &inputInfo,
        &outputInfo,
        activates.put(),
        activates.countPut());
    if (enumResult == MF_E_NOT_FOUND || activates.count() == 0) {
        throw std::runtime_error("No hardware H.264 encoder MFTs were found");
    }
    ThrowIfFailed(enumResult, "MFTEnumEx(hardware H.264 encoders)");

    auto deviceManager = CreateDxgiDeviceManager(config.d3dDevice.Get());
    std::string failures;

    for (UINT32 i = 0; i < activates.count(); ++i) {
        IMFActivate* activate = activates[i];
        const std::string name = GetAllocatedString(activate, MFT_FRIENDLY_NAME_Attribute);
        const std::string label = name.empty() ? "(unnamed hardware encoder)" : name;

        Microsoft::WRL::ComPtr<IMFTransform> transform;
        HRESULT result = activate->ActivateObject(IID_PPV_ARGS(&transform));
        if (FAILED(result)) {
            failures += "\n  " + label + ": activation failed: " + HResultMessage(result);
            continue;
        }

        Microsoft::WRL::ComPtr<IMFAttributes> attributes;
        bool async = false;
        bool d3d11Aware = false;
        if (SUCCEEDED(transform->GetAttributes(&attributes))) {
            async = GetBoolAttribute(attributes.Get(), MF_TRANSFORM_ASYNC);
            d3d11Aware = GetBoolAttribute(attributes.Get(), MF_SA_D3D11_AWARE);
            if (async) {
                result = attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
                if (FAILED(result)) {
                    failures += "\n  " + label + ": async unlock failed: " + HResultMessage(result);
                    static_cast<void>(activate->ShutdownObject());
                    continue;
                }
            }
        }

        if (!async) {
            failures += "\n  " + label + ": hardware encoder is not asynchronous";
            static_cast<void>(activate->ShutdownObject());
            continue;
        }
        if (!d3d11Aware) {
            failures += "\n  " + label + ": hardware encoder is not D3D11-aware";
            static_cast<void>(activate->ShutdownObject());
            continue;
        }

        ConfigureLowLatencyEncoderOptions(transform.Get(), config);

        result = transform->ProcessMessage(
            MFT_MESSAGE_SET_D3D_MANAGER,
            reinterpret_cast<ULONG_PTR>(deviceManager.Get()));
        if (FAILED(result)) {
            failures += "\n  " + label + ": D3D manager rejected: " + HResultMessage(result);
            static_cast<void>(activate->ShutdownObject());
            continue;
        }

        DWORD inputStreamId = 0;
        DWORD outputStreamId = 0;
        try {
            ConfigureEncoderTypes(transform.Get(), config, inputStreamId, outputStreamId);
        } catch (const std::exception& error) {
            failures += "\n  " + label + ": stream type setup failed: " + error.what();
            static_cast<void>(activate->ShutdownObject());
            continue;
        }

        Microsoft::WRL::ComPtr<IMFMediaEventGenerator> eventGenerator;
        result = transform.As(&eventGenerator);
        if (FAILED(result) || !eventGenerator) {
            failures += "\n  " + label + ": async event interface unavailable: " + HResultMessage(result);
            static_cast<void>(activate->ShutdownObject());
            continue;
        }

        HardwareEncoderSelection selected;
        selected.transform = std::move(transform);
        selected.eventGenerator = std::move(eventGenerator);
        selected.dxgiDeviceManager = std::move(deviceManager);
        selected.inputStreamId = inputStreamId;
        selected.outputStreamId = outputStreamId;
        selected.name = label;
        return selected;
    }

    throw std::runtime_error("No hardware H.264 encoder accepted the stream configuration:" + failures);
}

Microsoft::WRL::ComPtr<IMFTransform> CreateSoftwareEncoder(const H264StreamEncoderConfig& config)
{
    Microsoft::WRL::ComPtr<IMFTransform> transform;
    ThrowIfFailed(
        CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&transform)),
        "CoCreateInstance(CMSH264EncoderMFT)");
    ConfigureLowLatencyEncoderOptions(transform.Get(), config);
    return transform;
}

EncodedPacket PacketFromOutputSample(IMFSample* sample)
{
    DWORD totalLength = 0;
    ThrowIfFailed(sample->GetTotalLength(&totalLength), "IMFSample::GetTotalLength");

    EncodedPacket packet;
    LONGLONG sampleTime = 0;
    LONGLONG sampleDuration = 0;
    if (SUCCEEDED(sample->GetSampleTime(&sampleTime))) {
        packet.timestamp100ns = sampleTime;
    }
    if (SUCCEEDED(sample->GetSampleDuration(&sampleDuration))) {
        packet.duration100ns = sampleDuration;
    }

    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    ThrowIfFailed(sample->ConvertToContiguousBuffer(&buffer), "IMFSample::ConvertToContiguousBuffer");

    BYTE* source = nullptr;
    DWORD maxLength = 0;
    DWORD currentLength = 0;
    ThrowIfFailed(buffer->Lock(&source, &maxLength, &currentLength), "IMFMediaBuffer::Lock(output)");

    packet.bytes.resize(totalLength);
    std::memcpy(packet.bytes.data(), source, std::min<DWORD>(totalLength, currentLength));

    ThrowIfFailed(buffer->Unlock(), "IMFMediaBuffer::Unlock(output)");
    return packet;
}

Microsoft::WRL::ComPtr<IMFSample> CreateInputSampleFromBuffer(
    IMFMediaBuffer* buffer,
    int64_t sampleTime,
    int64_t sampleDuration)
{
    Microsoft::WRL::ComPtr<IMFSample> sample;
    ThrowIfFailed(MFCreateSample(&sample), "MFCreateSample(input)");
    ThrowIfFailed(sample->AddBuffer(buffer), "IMFSample::AddBuffer(input)");
    ThrowIfFailed(sample->SetSampleTime(sampleTime), "IMFSample::SetSampleTime(input)");
    ThrowIfFailed(sample->SetSampleDuration(sampleDuration), "IMFSample::SetSampleDuration(input)");
    return sample;
}

Microsoft::WRL::ComPtr<IMFSample> CreateInputSampleFromDxgiTexture(
    const CapturedFrame& frame,
    int64_t sampleTime,
    int64_t sampleDuration)
{
    if (!frame.nv12Texture) {
        throw std::runtime_error("Direct3D H.264 input requested but captured frame has no NV12 texture");
    }

    D3D11_TEXTURE2D_DESC textureDesc{};
    frame.nv12Texture->GetDesc(&textureDesc);
    if (textureDesc.Format != DXGI_FORMAT_NV12 ||
        textureDesc.Width != static_cast<UINT>(frame.width) ||
        textureDesc.Height != static_cast<UINT>(frame.height)) {
        throw std::runtime_error("Captured NV12 texture does not match stream encoder configuration");
    }

    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    ThrowIfFailed(
        MFCreateDXGISurfaceBuffer(
            __uuidof(ID3D11Texture2D),
            frame.nv12Texture.Get(),
            frame.nv12TextureSubresource,
            FALSE,
            &buffer),
        "MFCreateDXGISurfaceBuffer(input)");

    DWORD maxLength = 0;
    if (SUCCEEDED(buffer->GetMaxLength(&maxLength)) && maxLength > 0) {
        ThrowIfFailed(buffer->SetCurrentLength(maxLength), "IMFMediaBuffer::SetCurrentLength(DXGI input)");
    }

    return CreateInputSampleFromBuffer(buffer.Get(), sampleTime, sampleDuration);
}

Microsoft::WRL::ComPtr<IMFSample> CreateInputSampleFromMemory(
    const CapturedFrame& frame,
    int64_t sampleTime,
    int64_t sampleDuration)
{
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
        if (!IsBgra8Format(frame.format) || frame.pixels.empty()) {
            throw std::runtime_error(
                "H264StreamEncoder memory input expects BGRA8 pixels or NV12 bytes, got DXGI format " +
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

    return CreateInputSampleFromBuffer(buffer.Get(), sampleTime, sampleDuration);
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
    if (config.keyframeIntervalFrames > static_cast<uint32_t>(config.fps * 30)) {
        throw std::invalid_argument("H264StreamEncoder keyframe interval is too large");
    }

    config_ = config;
    backend_ = config.backend;
    lastInputMode_ =
        (backend_ == H264StreamEncoderBackend::Hardware && config.d3dDevice) ?
        H264StreamEncoderInputMode::Direct3D :
        H264StreamEncoderInputMode::Memory;
    encoderName_.clear();
    eventGenerator_.Reset();
    dxgiDeviceManager_.Reset();
    pendingAsyncInputs_ = 0;
    pendingAsyncOutputs_ = 0;
    queuedAsyncInputs_.clear();
    droppedAsyncInputs_ = 0;
    maxQueuedAsyncInputs_ = std::clamp<size_t>(static_cast<size_t>((config_.fps + 1) / 2), 8, 32);
    asyncDrainComplete_ = false;
    frameIndex_ = 0;
    frameDuration100ns_ = 10'000'000 / config_.fps;

    if (backend_ == H264StreamEncoderBackend::Hardware) {
        auto hardware = CreateHardwareEncoder(config_);
        transform_ = std::move(hardware.transform);
        eventGenerator_ = std::move(hardware.eventGenerator);
        dxgiDeviceManager_ = std::move(hardware.dxgiDeviceManager);
        inputStreamId_ = hardware.inputStreamId;
        outputStreamId_ = hardware.outputStreamId;
        encoderName_ = std::move(hardware.name);
    } else {
        transform_ = CreateSoftwareEncoder(config_);
        ConfigureEncoderTypes(transform_.Get(), config_, inputStreamId_, outputStreamId_);
        encoderName_ = "Microsoft H.264 Encoder MFT";
    }

    ThrowIfFailed(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "IMFTransform::ProcessMessage(BEGIN_STREAMING)");
    ThrowIfFailed(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "IMFTransform::ProcessMessage(START_OF_STREAM)");

    if (backend_ == H264StreamEncoderBackend::Hardware) {
        static_cast<void>(WaitForAsyncInputRequest());
    }
}

std::vector<EncodedPacket> H264StreamEncoder::EncodeFrame(const CapturedFrame& frame)
{
    if (!transform_) {
        throw std::logic_error("H264StreamEncoder::Start must be called before EncodeFrame");
    }
    if (frame.width != config_.width || frame.height != config_.height) {
        throw std::runtime_error("Encoded frame size does not match stream encoder configuration");
    }
    const int64_t sampleTime = frameIndex_ * frameDuration100ns_;
    const bool useDirect3dInput =
        backend_ == H264StreamEncoderBackend::Hardware &&
        config_.d3dDevice != nullptr &&
        frame.nv12Texture != nullptr;
    Microsoft::WRL::ComPtr<IMFSample> sample = useDirect3dInput ?
        CreateInputSampleFromDxgiTexture(frame, sampleTime, frameDuration100ns_) :
        CreateInputSampleFromMemory(frame, sampleTime, frameDuration100ns_);
    lastInputMode_ = useDirect3dInput ? H264StreamEncoderInputMode::Direct3D : H264StreamEncoderInputMode::Memory;

    if (backend_ == H264StreamEncoderBackend::Hardware) {
        ++frameIndex_;

        auto packets = PumpAsyncEvents();
        AppendPackets(packets, SubmitQueuedAsyncInputs());
        AppendPackets(packets, QueueAsyncInput(std::move(sample)));
        AppendPackets(packets, PumpAsyncEvents());
        AppendPackets(packets, SubmitQueuedAsyncInputs());
        return packets;
    }

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

    std::vector<EncodedPacket> packets;
    if (backend_ == H264StreamEncoderBackend::Hardware) {
        AppendPackets(packets, WaitForAsyncQueue());
    }

    ThrowIfFailed(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0), "IMFTransform::ProcessMessage(END_OF_STREAM)");
    ThrowIfFailed(transform_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0), "IMFTransform::ProcessMessage(DRAIN)");
    if (backend_ == H264StreamEncoderBackend::Hardware) {
        AppendPackets(packets, WaitForAsyncDrain());
        return packets;
    }
    return ReadAvailablePackets();
}

void H264StreamEncoder::Stop()
{
    if (transform_) {
        static_cast<void>(transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0));
        Microsoft::WRL::ComPtr<IMFShutdown> shutdown;
        if (SUCCEEDED(transform_.As(&shutdown)) && shutdown) {
            static_cast<void>(shutdown->Shutdown());
        }
    }
    eventGenerator_.Reset();
    transform_.Reset();
    dxgiDeviceManager_.Reset();
    config_.d3dDevice.Reset();
    pendingAsyncInputs_ = 0;
    pendingAsyncOutputs_ = 0;
    queuedAsyncInputs_.clear();
    maxQueuedAsyncInputs_ = 8;
    droppedAsyncInputs_ = 0;
    asyncDrainComplete_ = false;
    lastInputMode_ = H264StreamEncoderInputMode::Memory;
    encoderName_.clear();
}

std::vector<EncodedPacket> H264StreamEncoder::ReadAvailablePackets()
{
    return backend_ == H264StreamEncoderBackend::Hardware ?
        ReadAsyncAvailablePackets() :
        ReadSyncAvailablePackets();
}

std::vector<EncodedPacket> H264StreamEncoder::ReadSyncAvailablePackets()
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

        packets.push_back(PacketFromOutputSample(outputSample.Get()));
    }

    return packets;
}

std::vector<EncodedPacket> H264StreamEncoder::ReadAsyncAvailablePackets()
{
    std::vector<EncodedPacket> packets;

    while (pendingAsyncOutputs_ > 0) {
        MFT_OUTPUT_STREAM_INFO streamInfo{};
        ThrowIfFailed(transform_->GetOutputStreamInfo(outputStreamId_, &streamInfo), "IMFTransform::GetOutputStreamInfo");

        const bool transformProvidesSamples = (streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
        DWORD outputBufferSize = streamInfo.cbSize == 0 ? 1'048'576 : streamInfo.cbSize;
        HRESULT outputResult = S_OK;
        Microsoft::WRL::ComPtr<IMFSample> outputSample;

        while (true) {
            Microsoft::WRL::ComPtr<IMFMediaBuffer> outputBuffer;
            outputSample.Reset();

            MFT_OUTPUT_DATA_BUFFER outputData{};
            outputData.dwStreamID = outputStreamId_;

            if (!transformProvidesSamples) {
                ThrowIfFailed(MFCreateMemoryBuffer(outputBufferSize, &outputBuffer), "MFCreateMemoryBuffer(output)");
                ThrowIfFailed(MFCreateSample(&outputSample), "MFCreateSample(output)");
                ThrowIfFailed(outputSample->AddBuffer(outputBuffer.Get()), "IMFSample::AddBuffer(output)");
                outputData.pSample = outputSample.Get();
            }

            DWORD status = 0;
            outputResult = transform_->ProcessOutput(0, 1, &outputData, &status);

            if (outputData.pEvents != nullptr) {
                outputData.pEvents->Release();
            }
            if (transformProvidesSamples && outputData.pSample != nullptr) {
                outputSample.Attach(outputData.pSample);
            }

            if (outputResult != MF_E_BUFFERTOOSMALL) {
                break;
            }

            if (outputBufferSize >= 64 * 1'048'576) {
                ThrowIfFailed(outputResult, "IMFTransform::ProcessOutput");
            }

            outputBufferSize *= 2;
        }

        --pendingAsyncOutputs_;

        if (outputResult == MF_E_TRANSFORM_STREAM_CHANGE) {
            continue;
        }
        ThrowIfFailed(outputResult, "IMFTransform::ProcessOutput");
        if (!outputSample) {
            throw std::runtime_error("Async H.264 encoder reported output without an output sample");
        }

        packets.push_back(PacketFromOutputSample(outputSample.Get()));
    }

    return packets;
}

std::vector<EncodedPacket> H264StreamEncoder::PumpAsyncEvents()
{
    std::vector<EncodedPacket> packets;
    if (!eventGenerator_) {
        return packets;
    }

    while (true) {
        Microsoft::WRL::ComPtr<IMFMediaEvent> event;
        const HRESULT eventResult = eventGenerator_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
        if (eventResult == MF_E_NO_EVENTS_AVAILABLE) {
            break;
        }
        ThrowIfFailed(eventResult, "IMFMediaEventGenerator::GetEvent");

        MediaEventType eventType = MEUnknown;
        ThrowIfFailed(event->GetType(&eventType), "IMFMediaEvent::GetType");

        HRESULT eventStatus = S_OK;
        ThrowIfFailed(event->GetStatus(&eventStatus), "IMFMediaEvent::GetStatus");
        ThrowIfFailed(eventStatus, "Async H.264 encoder event");

        if (eventType == METransformNeedInput) {
            ++pendingAsyncInputs_;
        } else if (eventType == METransformHaveOutput) {
            ++pendingAsyncOutputs_;
            auto outputPackets = ReadAsyncAvailablePackets();
            packets.insert(
                packets.end(),
                std::make_move_iterator(outputPackets.begin()),
                std::make_move_iterator(outputPackets.end()));
        } else if (eventType == METransformDrainComplete) {
            asyncDrainComplete_ = true;
        }
    }

    return packets;
}

std::vector<EncodedPacket> H264StreamEncoder::QueueAsyncInput(Microsoft::WRL::ComPtr<IMFSample> sample)
{
    std::vector<EncodedPacket> packets;

    if (queuedAsyncInputs_.size() >= maxQueuedAsyncInputs_) {
        queuedAsyncInputs_.pop_front();
        ++droppedAsyncInputs_;
    }
    queuedAsyncInputs_.push_back(std::move(sample));

    AppendPackets(packets, SubmitQueuedAsyncInputs());
    return packets;
}

std::vector<EncodedPacket> H264StreamEncoder::SubmitQueuedAsyncInputs()
{
    std::vector<EncodedPacket> packets;

    while (pendingAsyncInputs_ > 0 && !queuedAsyncInputs_.empty()) {
        auto sample = std::move(queuedAsyncInputs_.front());
        queuedAsyncInputs_.pop_front();

        const HRESULT inputResult = transform_->ProcessInput(inputStreamId_, sample.Get(), 0);
        if (inputResult == MF_E_NOTACCEPTING) {
            queuedAsyncInputs_.push_front(std::move(sample));
            pendingAsyncInputs_ = 0;
            break;
        }

        ThrowIfFailed(inputResult, "IMFTransform::ProcessInput");
        --pendingAsyncInputs_;

        AppendPackets(packets, PumpAsyncEvents());
    }

    return packets;
}

std::vector<EncodedPacket> H264StreamEncoder::WaitForAsyncInputRequest()
{
    std::vector<EncodedPacket> packets;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

    while (pendingAsyncInputs_ == 0) {
        AppendPackets(packets, PumpAsyncEvents());

        if (pendingAsyncInputs_ > 0) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("Timed out waiting for hardware H.264 encoder input request");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return packets;
}

std::vector<EncodedPacket> H264StreamEncoder::WaitForAsyncQueue()
{
    std::vector<EncodedPacket> packets;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

    while (!queuedAsyncInputs_.empty()) {
        AppendPackets(packets, PumpAsyncEvents());
        AppendPackets(packets, SubmitQueuedAsyncInputs());

        if (queuedAsyncInputs_.empty()) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("Timed out waiting for hardware H.264 encoder queued input");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    AppendPackets(packets, PumpAsyncEvents());
    return packets;
}

std::vector<EncodedPacket> H264StreamEncoder::WaitForAsyncDrain()
{
    std::vector<EncodedPacket> packets;
    asyncDrainComplete_ = false;
    pendingAsyncInputs_ = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

    while (!asyncDrainComplete_) {
        auto eventPackets = PumpAsyncEvents();
        packets.insert(
            packets.end(),
            std::make_move_iterator(eventPackets.begin()),
            std::make_move_iterator(eventPackets.end()));

        if (asyncDrainComplete_) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("Timed out waiting for hardware H.264 encoder drain");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    auto remainingPackets = PumpAsyncEvents();
    packets.insert(
        packets.end(),
        std::make_move_iterator(remainingPackets.begin()),
        std::make_move_iterator(remainingPackets.end()));
    return packets;
}

const char* H264StreamEncoderBackendName(H264StreamEncoderBackend backend)
{
    switch (backend) {
    case H264StreamEncoderBackend::Software:
        return "software";
    case H264StreamEncoderBackend::Hardware:
        return "hardware";
    default:
        return "unknown";
    }
}

const char* H264StreamEncoderInputModeName(H264StreamEncoderInputMode inputMode)
{
    switch (inputMode) {
    case H264StreamEncoderInputMode::Memory:
        return "memory";
    case H264StreamEncoderInputMode::Direct3D:
        return "d3d11";
    default:
        return "unknown";
    }
}

} // namespace screenshare
