#include "audio/WasapiCapture.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>

#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <propidl.h>
#include <propsys.h>
#include <windows.h>

namespace screenshare {
namespace {

void ThrowIfFailed(HRESULT hr, const char* operation)
{
    if (FAILED(hr)) {
        std::ostringstream stream;
        stream << operation << " failed with HRESULT 0x"
               << std::hex << std::uppercase << static_cast<unsigned long>(hr);
        throw std::runtime_error(stream.str());
    }
}

std::wstring DeviceId(IMMDevice* device)
{
    LPWSTR raw = nullptr;
    const HRESULT result = device->GetId(&raw);
    if (FAILED(result) || raw == nullptr) {
        return {};
    }

    std::wstring value(raw);
    CoTaskMemFree(raw);
    return value;
}

std::wstring DeviceFriendlyName(IMMDevice* device)
{
    Microsoft::WRL::ComPtr<IPropertyStore> properties;
    const HRESULT openResult = device->OpenPropertyStore(STGM_READ, &properties);
    if (FAILED(openResult)) {
        return L"(unnamed audio device)";
    }

    PROPVARIANT value;
    PropVariantInit(&value);
    const HRESULT getResult = properties->GetValue(PKEY_Device_FriendlyName, &value);
    std::wstring name = L"(unnamed audio device)";
    if (SUCCEEDED(getResult) && value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
        name = value.pwszVal;
    }
    PropVariantClear(&value);
    return name;
}

EDataFlow DataFlowForSource(AudioCaptureSource source)
{
    return source == AudioCaptureSource::SystemOutput ? eRender : eCapture;
}

class ScopedComInitialization {
public:
    ScopedComInitialization()
    {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(result)) {
            initialized_ = true;
            return;
        }
        if (result != RPC_E_CHANGED_MODE) {
            ThrowIfFailed(result, "CoInitializeEx");
        }
    }

    ~ScopedComInitialization()
    {
        if (initialized_) {
            CoUninitialize();
        }
    }

    ScopedComInitialization(const ScopedComInitialization&) = delete;
    ScopedComInitialization& operator=(const ScopedComInitialization&) = delete;

private:
    bool initialized_ = false;
};

Microsoft::WRL::ComPtr<IMMDeviceEnumerator> CreateDeviceEnumerator()
{
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    ThrowIfFailed(
        CoCreateInstance(
            __uuidof(MMDeviceEnumerator),
            nullptr,
            CLSCTX_ALL,
            IID_PPV_ARGS(&enumerator)),
        "CoCreateInstance(MMDeviceEnumerator)");
    return enumerator;
}

bool SameAudioSubFormat(const GUID& lhs, const GUID& rhs)
{
    return std::memcmp(&lhs, &rhs, sizeof(GUID)) == 0;
}

AudioSampleKind DetermineSampleKind(const WAVEFORMATEX& format)
{
    if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT && format.wBitsPerSample == 32) {
        return AudioSampleKind::Float32;
    }
    if (format.wFormatTag == WAVE_FORMAT_PCM) {
        if (format.wBitsPerSample == 16) {
            return AudioSampleKind::Pcm16;
        }
        if (format.wBitsPerSample == 24) {
            return AudioSampleKind::Pcm24;
        }
        if (format.wBitsPerSample == 32) {
            return AudioSampleKind::Pcm32;
        }
    }
    if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto& extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
        if (SameAudioSubFormat(extensible.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) &&
            format.wBitsPerSample == 32) {
            return AudioSampleKind::Float32;
        }
        if (SameAudioSubFormat(extensible.SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
            if (format.wBitsPerSample == 16) {
                return AudioSampleKind::Pcm16;
            }
            if (format.wBitsPerSample == 24) {
                return AudioSampleKind::Pcm24;
            }
            if (format.wBitsPerSample == 32) {
                return AudioSampleKind::Pcm32;
            }
        }
    }
    return AudioSampleKind::Unsupported;
}

const char* SampleKindName(AudioSampleKind kind)
{
    switch (kind) {
    case AudioSampleKind::Float32:
        return "float32";
    case AudioSampleKind::Pcm16:
        return "pcm16";
    case AudioSampleKind::Pcm24:
        return "pcm24";
    case AudioSampleKind::Pcm32:
        return "pcm32";
    case AudioSampleKind::Unsupported:
    default:
        return "unsupported";
    }
}

double ReadNormalizedSample(const std::byte* data, AudioSampleKind kind)
{
    switch (kind) {
    case AudioSampleKind::Float32: {
        float value = 0.0f;
        std::memcpy(&value, data, sizeof(value));
        if (!std::isfinite(value)) {
            return 0.0;
        }
        return static_cast<double>(value);
    }
    case AudioSampleKind::Pcm16: {
        int16_t value = 0;
        std::memcpy(&value, data, sizeof(value));
        return static_cast<double>(value) / 32768.0;
    }
    case AudioSampleKind::Pcm24: {
        const auto* bytes = reinterpret_cast<const uint8_t*>(data);
        int32_t value =
            static_cast<int32_t>(bytes[0]) |
            (static_cast<int32_t>(bytes[1]) << 8) |
            (static_cast<int32_t>(bytes[2]) << 16);
        if ((value & 0x00800000) != 0) {
            value |= static_cast<int32_t>(0xFF000000);
        }
        return static_cast<double>(value) / 8388608.0;
    }
    case AudioSampleKind::Pcm32: {
        int32_t value = 0;
        std::memcpy(&value, data, sizeof(value));
        return static_cast<double>(value) / 2147483648.0;
    }
    case AudioSampleKind::Unsupported:
    default:
        return 0.0;
    }
}

void AnalyzeSamples(CapturedAudioPacket& packet, const AudioCaptureFormat& format, AudioSampleKind kind)
{
    if (packet.silent) {
        packet.samplesAnalyzed = static_cast<uint64_t>(packet.frames) * format.channels;
        return;
    }
    if (kind == AudioSampleKind::Unsupported || format.channels == 0 || format.bitsPerSample == 0 || packet.data.empty()) {
        return;
    }

    const size_t sampleBytes = static_cast<size_t>(format.bitsPerSample / 8);
    if (sampleBytes == 0) {
        return;
    }

    const size_t sampleCount = packet.data.size() / sampleBytes;
    double peak = 0.0;
    long double sumSquares = 0.0;
    for (size_t index = 0; index < sampleCount; ++index) {
        const double sample = ReadNormalizedSample(packet.data.data() + index * sampleBytes, kind);
        const double magnitude = std::abs(sample);
        peak = std::max(peak, magnitude);
        sumSquares += static_cast<long double>(sample) * static_cast<long double>(sample);
    }

    packet.samplesAnalyzed = static_cast<uint64_t>(sampleCount);
    packet.peak = peak;
    packet.rms = sampleCount == 0 ? 0.0 : std::sqrt(static_cast<double>(sumSquares / sampleCount));
}

} // namespace

WasapiCapture::WasapiCapture() = default;

WasapiCapture::~WasapiCapture()
{
    Stop();
}

void WasapiCapture::InitializeCom()
{
    const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(result)) {
        comInitialized_ = true;
        return;
    }
    if (result == RPC_E_CHANGED_MODE) {
        return;
    }
    ThrowIfFailed(result, "CoInitializeEx");
}

void WasapiCapture::UninitializeCom()
{
    if (comInitialized_) {
        CoUninitialize();
        comInitialized_ = false;
    }
}

std::vector<AudioDeviceInfo> WasapiCapture::EnumerateDevices(AudioCaptureSource source)
{
    ScopedComInitialization com;
    auto enumerator = CreateDeviceEnumerator();
    const EDataFlow flow = DataFlowForSource(source);

    std::wstring defaultId;
    Microsoft::WRL::ComPtr<IMMDevice> defaultDevice;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &defaultDevice)) && defaultDevice) {
        defaultId = DeviceId(defaultDevice.Get());
    }

    Microsoft::WRL::ComPtr<IMMDeviceCollection> collection;
    ThrowIfFailed(
        enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection),
        "IMMDeviceEnumerator::EnumAudioEndpoints");

    UINT count = 0;
    ThrowIfFailed(collection->GetCount(&count), "IMMDeviceCollection::GetCount");

    std::vector<AudioDeviceInfo> devices;
    devices.reserve(count);
    for (UINT index = 0; index < count; ++index) {
        Microsoft::WRL::ComPtr<IMMDevice> device;
        ThrowIfFailed(collection->Item(index, &device), "IMMDeviceCollection::Item");
        AudioDeviceInfo info;
        info.source = source;
        info.id = DeviceId(device.Get());
        info.name = DeviceFriendlyName(device.Get());
        info.isDefault = !defaultId.empty() && info.id == defaultId;
        devices.push_back(std::move(info));
    }

    return devices;
}

void WasapiCapture::Start(const AudioCaptureConfig& config)
{
    Stop();
    InitializeCom();
    config_ = config;

    auto enumerator = CreateDeviceEnumerator();
    Microsoft::WRL::ComPtr<IMMDevice> device;
    if (!config.deviceId.empty()) {
        ThrowIfFailed(
            enumerator->GetDevice(config.deviceId.c_str(), &device),
            "IMMDeviceEnumerator::GetDevice");
    } else {
        ThrowIfFailed(
            enumerator->GetDefaultAudioEndpoint(DataFlowForSource(config.source), eConsole, &device),
            "IMMDeviceEnumerator::GetDefaultAudioEndpoint");
    }

    deviceId_ = DeviceId(device.Get());
    deviceName_ = DeviceFriendlyName(device.Get());

    ThrowIfFailed(
        device->Activate(
            __uuidof(IAudioClient),
            CLSCTX_ALL,
            nullptr,
            reinterpret_cast<void**>(audioClient_.ReleaseAndGetAddressOf())),
        "IMMDevice::Activate(IAudioClient)");

    WAVEFORMATEX* rawMixFormat = nullptr;
    ThrowIfFailed(audioClient_->GetMixFormat(&rawMixFormat), "IAudioClient::GetMixFormat");
    if (rawMixFormat == nullptr) {
        throw std::runtime_error("IAudioClient::GetMixFormat returned no format");
    }

    struct MixFormatDeleter {
        void operator()(WAVEFORMATEX* value) const noexcept { CoTaskMemFree(value); }
    };
    std::unique_ptr<WAVEFORMATEX, MixFormatDeleter> mixFormat(rawMixFormat);

    sampleKind_ = DetermineSampleKind(*mixFormat);
    format_.sampleRate = mixFormat->nSamplesPerSec;
    format_.channels = mixFormat->nChannels;
    format_.bitsPerSample = mixFormat->wBitsPerSample;
    format_.blockAlign = mixFormat->nBlockAlign;
    format_.sampleFormat = SampleKindName(sampleKind_);

    const REFERENCE_TIME bufferDuration =
        static_cast<REFERENCE_TIME>(std::max<int64_t>(1, config.bufferDuration.count())) * 10'000;
    DWORD streamFlags = 0;
    if (config.source == AudioCaptureSource::SystemOutput) {
        streamFlags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
    }

    ThrowIfFailed(
        audioClient_->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            streamFlags,
            bufferDuration,
            0,
            mixFormat.get(),
            nullptr),
        "IAudioClient::Initialize");
    ThrowIfFailed(audioClient_->GetBufferSize(&bufferFrames_), "IAudioClient::GetBufferSize");
    ThrowIfFailed(audioClient_->GetService(IID_PPV_ARGS(&captureClient_)), "IAudioClient::GetService");
    ThrowIfFailed(audioClient_->Start(), "IAudioClient::Start");
    started_ = true;
}

void WasapiCapture::Stop()
{
    if (audioClient_ && started_) {
        audioClient_->Stop();
    }
    started_ = false;
    captureClient_.Reset();
    audioClient_.Reset();
    bufferFrames_ = 0;
    deviceId_.clear();
    deviceName_.clear();
    format_ = {};
    sampleKind_ = AudioSampleKind::Unsupported;
    UninitializeCom();
}

std::optional<CapturedAudioPacket> WasapiCapture::CapturePacket(std::chrono::milliseconds timeout)
{
    if (!started_ || !captureClient_) {
        throw std::runtime_error("WASAPI capture is not started");
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        UINT32 packetFrames = 0;
        ThrowIfFailed(captureClient_->GetNextPacketSize(&packetFrames), "IAudioCaptureClient::GetNextPacketSize");
        if (packetFrames > 0) {
            BYTE* data = nullptr;
            DWORD flags = 0;
            UINT64 devicePosition = 0;
            UINT64 qpcPosition = 0;
            ThrowIfFailed(
                captureClient_->GetBuffer(&data, &packetFrames, &flags, &devicePosition, &qpcPosition),
                "IAudioCaptureClient::GetBuffer");

            CapturedAudioPacket packet;
            packet.frames = packetFrames;
            packet.devicePosition = devicePosition;
            packet.qpcPosition = qpcPosition;
            packet.silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            packet.dataDiscontinuity = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
            packet.timestampError = (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0;

            const size_t bytes =
                static_cast<size_t>(packetFrames) * static_cast<size_t>(format_.blockAlign);
            try {
                packet.data.resize(bytes);
                if (!packet.silent && data != nullptr && bytes > 0) {
                    std::memcpy(packet.data.data(), data, bytes);
                }
            } catch (...) {
                captureClient_->ReleaseBuffer(packetFrames);
                throw;
            }

            const HRESULT releaseResult = captureClient_->ReleaseBuffer(packetFrames);
            ThrowIfFailed(releaseResult, "IAudioCaptureClient::ReleaseBuffer");
            AnalyzeSamples(packet, format_, sampleKind_);
            return packet;
        }

        if (timeout.count() <= 0 || std::chrono::steady_clock::now() >= deadline) {
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

AudioCaptureSource ParseAudioCaptureSource(const std::string& value)
{
    if (value == "system" || value == "loopback" || value == "render") {
        return AudioCaptureSource::SystemOutput;
    }
    if (value == "microphone" || value == "mic" || value == "capture") {
        return AudioCaptureSource::Microphone;
    }
    throw std::invalid_argument("Invalid audio capture source: " + value + " (expected system or microphone)");
}

const char* AudioCaptureSourceName(AudioCaptureSource source)
{
    switch (source) {
    case AudioCaptureSource::SystemOutput:
        return "system";
    case AudioCaptureSource::Microphone:
        return "microphone";
    default:
        return "unknown";
    }
}

std::string AudioCaptureFormatName(const AudioCaptureFormat& format)
{
    std::ostringstream stream;
    stream << format.sampleRate << " Hz, "
           << format.channels << " ch, "
           << format.bitsPerSample << "-bit "
           << format.sampleFormat
           << ", block_align=" << format.blockAlign;
    return stream.str();
}

} // namespace screenshare
