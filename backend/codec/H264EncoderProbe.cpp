#include "codec/H264EncoderProbe.h"

#include "capture/DesktopCapturer.h"

#include <Windows.h>
#include <codecapi.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <iterator>
#include <stdexcept>
#include <utility>

namespace screenshare {
namespace {

void ThrowIfFailed(HRESULT hr, const char* operation)
{
    if (FAILED(hr)) {
        throw std::runtime_error(std::string(operation) + " failed: " + HResultMessage(hr));
    }
}

std::string GuidToString(const GUID& guid)
{
    wchar_t text[64]{};
    const int length = StringFromGUID2(guid, text, static_cast<int>(std::size(text)));
    if (length <= 0) {
        return {};
    }
    return Narrow(text);
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

void SetVideoSize(IMFAttributes* attributes, REFGUID key, int width, int height)
{
    ThrowIfFailed(
        MFSetAttributeSize(attributes, key, static_cast<UINT32>(width), static_cast<UINT32>(height)),
        "MFSetAttributeSize");
}

void SetVideoRatio(IMFAttributes* attributes, REFGUID key, int numerator, int denominator)
{
    ThrowIfFailed(
        MFSetAttributeRatio(attributes, key, static_cast<UINT32>(numerator), static_cast<UINT32>(denominator)),
        "MFSetAttributeRatio");
}

void SetBt709ColorInfo(IMFMediaType* mediaType)
{
    ThrowIfFailed(mediaType->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709), "IMFMediaType::SetUINT32(YUV matrix)");
    ThrowIfFailed(mediaType->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709), "IMFMediaType::SetUINT32(video primaries)");
    ThrowIfFailed(mediaType->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709), "IMFMediaType::SetUINT32(transfer function)");
    ThrowIfFailed(mediaType->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235), "IMFMediaType::SetUINT32(nominal range)");
}

Microsoft::WRL::ComPtr<IMFMediaType> CreateH264OutputType(const H264EncoderProbeConfig& config)
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

Microsoft::WRL::ComPtr<IMFMediaType> CreateNv12InputType(const H264EncoderProbeConfig& config)
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
        throw std::runtime_error("H.264 encoder MFT did not expose one input and one output stream");
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

void TrySetStreamTypes(IMFTransform* transform, const H264EncoderProbeConfig& config)
{
    const auto [inputStreamId, outputStreamId] = GetSingleStreamIds(transform);
    const auto outputType = CreateH264OutputType(config);
    const auto inputType = CreateNv12InputType(config);

    ThrowIfFailed(transform->SetOutputType(outputStreamId, outputType.Get(), 0), "IMFTransform::SetOutputType");
    ThrowIfFailed(transform->SetInputType(inputStreamId, inputType.Get(), 0), "IMFTransform::SetInputType");
}

Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> CreateDxgiDeviceManager()
{
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;

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
        "D3D11CreateDevice(encoder probe)");

    UINT resetToken = 0;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> deviceManager;
    ThrowIfFailed(MFCreateDXGIDeviceManager(&resetToken, &deviceManager), "MFCreateDXGIDeviceManager");
    ThrowIfFailed(deviceManager->ResetDevice(device.Get(), resetToken), "IMFDXGIDeviceManager::ResetDevice");
    return deviceManager;
}

struct ComSession {
    ComSession()
    {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(result)) {
            initialized = true;
        } else if (result != RPC_E_CHANGED_MODE) {
            ThrowIfFailed(result, "CoInitializeEx");
        }
    }

    ~ComSession()
    {
        if (initialized) {
            CoUninitialize();
        }
    }

    bool initialized = false;
};

struct MfSession {
    MfSession()
    {
        ThrowIfFailed(MFStartup(MF_VERSION, MFSTARTUP_FULL), "MFStartup");
    }

    ~MfSession()
    {
        static_cast<void>(MFShutdown());
    }
};

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

std::vector<H264EncoderProbeInfo> EnumerateEncoderClass(
    const H264EncoderProbeConfig& config,
    UINT32 flags,
    bool hardware,
    IMFDXGIDeviceManager* deviceManager)
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
        flags,
        &inputInfo,
        &outputInfo,
        activates.put(),
        activates.countPut());
    if (enumResult == MF_E_NOT_FOUND) {
        return {};
    }
    ThrowIfFailed(enumResult, "MFTEnumEx(H.264 encoders)");

    std::vector<H264EncoderProbeInfo> encoders;
    for (UINT32 i = 0; i < activates.count(); ++i) {
        IMFActivate* activate = activates[i];
        H264EncoderProbeInfo info;
        info.hardware = hardware;
        info.friendlyName = GetAllocatedString(activate, MFT_FRIENDLY_NAME_Attribute);
        info.hardwareUrl = GetAllocatedString(activate, MFT_ENUM_HARDWARE_URL_Attribute);

        GUID clsid{};
        if (SUCCEEDED(activate->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &clsid))) {
            info.clsid = GuidToString(clsid);
        }

        Microsoft::WRL::ComPtr<IMFTransform> transform;
        HRESULT activateResult = activate->ActivateObject(IID_PPV_ARGS(&transform));
        if (FAILED(activateResult)) {
            info.activationError = HResultMessage(activateResult);
            encoders.push_back(std::move(info));
            continue;
        }

        Microsoft::WRL::ComPtr<IMFAttributes> attributes;
        if (SUCCEEDED(transform->GetAttributes(&attributes))) {
            info.async = GetBoolAttribute(attributes.Get(), MF_TRANSFORM_ASYNC);
            info.d3d11Aware = GetBoolAttribute(attributes.Get(), MF_SA_D3D11_AWARE);
            if (info.async && SUCCEEDED(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE))) {
                info.asyncUnlocked = true;
            }
        }

        if (info.d3d11Aware && deviceManager != nullptr) {
            const HRESULT d3dResult = transform->ProcessMessage(
                MFT_MESSAGE_SET_D3D_MANAGER,
                reinterpret_cast<ULONG_PTR>(deviceManager));
            if (SUCCEEDED(d3dResult)) {
                info.d3dManagerAccepted = true;
            } else {
                info.d3dManagerError = HResultMessage(d3dResult);
            }
        }

        try {
            TrySetStreamTypes(transform.Get(), config);
            info.streamTypesAccepted = true;
        } catch (const std::exception& error) {
            info.streamTypeError = error.what();
        }

        static_cast<void>(activate->ShutdownObject());
        encoders.push_back(std::move(info));
    }

    return encoders;
}

} // namespace

std::vector<H264EncoderProbeInfo> ProbeH264Encoders(const H264EncoderProbeConfig& config)
{
    if (config.width <= 0 || config.height <= 0 || (config.width % 2) != 0 || (config.height % 2) != 0) {
        throw std::invalid_argument("H.264 encoder probe width and height must be positive and even");
    }
    if (config.fps <= 0) {
        throw std::invalid_argument("H.264 encoder probe FPS must be positive");
    }

    ComSession com;
    MfSession mf;
    const auto deviceManager = CreateDxgiDeviceManager();

    std::vector<H264EncoderProbeInfo> encoders;
    auto hardwareEncoders = EnumerateEncoderClass(
        config,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        true,
        deviceManager.Get());
    encoders.insert(
        encoders.end(),
        std::make_move_iterator(hardwareEncoders.begin()),
        std::make_move_iterator(hardwareEncoders.end()));

    auto softwareEncoders = EnumerateEncoderClass(
        config,
        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
        false,
        deviceManager.Get());
    encoders.insert(
        encoders.end(),
        std::make_move_iterator(softwareEncoders.begin()),
        std::make_move_iterator(softwareEncoders.end()));

    return encoders;
}

} // namespace screenshare
