#include "audio/WasapiRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>

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

bool SameGuid(const GUID& lhs, const GUID& rhs)
{
    return std::memcmp(&lhs, &rhs, sizeof(GUID)) == 0;
}

const char* WaveSampleFormatName(const WAVEFORMATEX& format)
{
    if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT && format.wBitsPerSample == 32) {
        return "float32";
    }
    if (format.wFormatTag == WAVE_FORMAT_PCM) {
        if (format.wBitsPerSample == 16) {
            return "pcm16";
        }
        if (format.wBitsPerSample == 24) {
            return "pcm24";
        }
        if (format.wBitsPerSample == 32) {
            return "pcm32";
        }
    }
    if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto& extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
        if (SameGuid(extensible.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) &&
            format.wBitsPerSample == 32) {
            return "float32";
        }
        if (SameGuid(extensible.SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
            if (format.wBitsPerSample == 16) {
                return "pcm16";
            }
            if (format.wBitsPerSample == 24) {
                return "pcm24";
            }
            if (format.wBitsPerSample == 32) {
                return "pcm32";
            }
        }
    }
    return "unknown";
}

std::string DescribeWaveFormat(const WAVEFORMATEX& format)
{
    std::ostringstream stream;
    stream << format.nSamplesPerSec << " Hz, "
           << format.nChannels << " ch, "
           << format.wBitsPerSample << "-bit "
           << WaveSampleFormatName(format)
           << ", block_align=" << format.nBlockAlign;
    return stream.str();
}

DWORD ChannelMaskForCount(uint16_t channels)
{
    switch (channels) {
    case 1:
        return SPEAKER_FRONT_CENTER;
    case 2:
        return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    case 4:
        return KSAUDIO_SPEAKER_QUAD;
    case 6:
        return KSAUDIO_SPEAKER_5POINT1_SURROUND;
    case 8:
        return KSAUDIO_SPEAKER_7POINT1_SURROUND;
    default:
        return 0;
    }
}

int32_t SignExtend24(uint32_t value)
{
    if ((value & 0x0080'0000U) != 0) {
        value |= 0xFF00'0000U;
    }
    return static_cast<int32_t>(value);
}

void WritePcm24(BYTE* destination, int32_t sample)
{
    const uint32_t raw = static_cast<uint32_t>(sample) & 0x00FF'FFFFU;
    destination[0] = static_cast<BYTE>(raw & 0xFFU);
    destination[1] = static_cast<BYTE>((raw >> 8) & 0xFFU);
    destination[2] = static_cast<BYTE>((raw >> 16) & 0xFFU);
}

void CopyScaledAudio(
    BYTE* destination,
    std::span<const std::byte> source,
    const AudioPlaybackFormat& format,
    float volume)
{
    if (volume == 1.0f) {
        std::memcpy(destination, source.data(), source.size());
        return;
    }

    const size_t sampleBytes = static_cast<size_t>(format.bitsPerSample / 8);
    const size_t sampleCount = source.size() / sampleBytes;
    const double gain = static_cast<double>(std::max(0.0f, volume));

    switch (format.sampleFormat) {
    case udp_protocol::AudioSampleFormat::Float32:
        for (size_t index = 0; index < sampleCount; ++index) {
            float sample = 0.0f;
            std::memcpy(&sample, source.data() + index * sizeof(float), sizeof(sample));
            if (!std::isfinite(sample)) {
                sample = 0.0f;
            }
            sample = std::clamp(sample * volume, -1.0f, 1.0f);
            std::memcpy(destination + index * sizeof(float), &sample, sizeof(sample));
        }
        break;
    case udp_protocol::AudioSampleFormat::Pcm16:
        for (size_t index = 0; index < sampleCount; ++index) {
            int16_t sample = 0;
            std::memcpy(&sample, source.data() + index * sizeof(sample), sizeof(sample));
            const auto scaled = static_cast<int16_t>(std::clamp(
                std::llround(static_cast<double>(sample) * gain),
                static_cast<long long>(std::numeric_limits<int16_t>::min()),
                static_cast<long long>(std::numeric_limits<int16_t>::max())));
            std::memcpy(destination + index * sizeof(scaled), &scaled, sizeof(scaled));
        }
        break;
    case udp_protocol::AudioSampleFormat::Pcm24:
        for (size_t index = 0; index < sampleCount; ++index) {
            const size_t offset = index * 3;
            const uint32_t raw =
                static_cast<uint32_t>(std::to_integer<uint8_t>(source[offset])) |
                (static_cast<uint32_t>(std::to_integer<uint8_t>(source[offset + 1])) << 8) |
                (static_cast<uint32_t>(std::to_integer<uint8_t>(source[offset + 2])) << 16);
            const auto scaled = static_cast<int32_t>(std::clamp(
                std::llround(static_cast<double>(SignExtend24(raw)) * gain),
                -8'388'608LL,
                8'388'607LL));
            WritePcm24(destination + offset, scaled);
        }
        break;
    case udp_protocol::AudioSampleFormat::Pcm32:
        for (size_t index = 0; index < sampleCount; ++index) {
            int32_t sample = 0;
            std::memcpy(&sample, source.data() + index * sizeof(sample), sizeof(sample));
            const auto scaled = static_cast<int32_t>(std::clamp(
                std::llround(static_cast<double>(sample) * gain),
                static_cast<long long>(std::numeric_limits<int32_t>::min()),
                static_cast<long long>(std::numeric_limits<int32_t>::max())));
            std::memcpy(destination + index * sizeof(scaled), &scaled, sizeof(scaled));
        }
        break;
    case udp_protocol::AudioSampleFormat::Unknown:
    default:
        throw std::invalid_argument("Unsupported audio playback sample format for volume control");
    }
}

WAVEFORMATEXTENSIBLE BuildWaveFormat(const AudioPlaybackFormat& format)
{
    if (format.sampleRate == 0 || format.channels == 0 || format.bitsPerSample == 0 || format.blockAlign == 0) {
        throw std::invalid_argument("Audio playback format is incomplete");
    }
    if ((format.bitsPerSample % 8) != 0) {
        throw std::invalid_argument("Audio playback bits per sample must be byte-aligned");
    }
    const auto expectedBlockAlign = static_cast<uint16_t>(format.channels * (format.bitsPerSample / 8));
    if (format.blockAlign != expectedBlockAlign) {
        throw std::invalid_argument("Audio playback block alignment does not match the channel/sample format");
    }
    const uint64_t avgBytesPerSecond =
        static_cast<uint64_t>(format.sampleRate) * static_cast<uint64_t>(format.blockAlign);
    if (avgBytesPerSecond > std::numeric_limits<DWORD>::max()) {
        throw std::invalid_argument("Audio playback format byte rate is too large");
    }

    WAVEFORMATEXTENSIBLE wave{};
    wave.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wave.Format.nChannels = format.channels;
    wave.Format.nSamplesPerSec = format.sampleRate;
    wave.Format.nAvgBytesPerSec = static_cast<DWORD>(avgBytesPerSecond);
    wave.Format.nBlockAlign = format.blockAlign;
    wave.Format.wBitsPerSample = format.bitsPerSample;
    wave.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wave.Samples.wValidBitsPerSample = format.bitsPerSample;
    wave.dwChannelMask = ChannelMaskForCount(format.channels);

    switch (format.sampleFormat) {
    case udp_protocol::AudioSampleFormat::Float32:
        if (format.bitsPerSample != 32) {
            throw std::invalid_argument("Float32 audio playback requires 32-bit samples");
        }
        wave.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        break;
    case udp_protocol::AudioSampleFormat::Pcm16:
    case udp_protocol::AudioSampleFormat::Pcm24:
    case udp_protocol::AudioSampleFormat::Pcm32:
        wave.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        break;
    case udp_protocol::AudioSampleFormat::Unknown:
    default:
        throw std::invalid_argument("Unsupported audio playback sample format");
    }

    return wave;
}

} // namespace

WasapiRenderer::WasapiRenderer() = default;

WasapiRenderer::~WasapiRenderer()
{
    Stop();
}

void WasapiRenderer::InitializeCom()
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

void WasapiRenderer::UninitializeCom()
{
    if (comInitialized_) {
        CoUninitialize();
        comInitialized_ = false;
    }
}

void WasapiRenderer::Start(const AudioPlaybackConfig& config)
{
    Stop();
    InitializeCom();
    config_ = config;
    format_ = config.format;
    volume_ = std::clamp(config.volume, 0.0f, 2.0f);
    muted_ = config.muted;

    auto enumerator = CreateDeviceEnumerator();
    Microsoft::WRL::ComPtr<IMMDevice> device;
    if (!config.deviceId.empty()) {
        ThrowIfFailed(
            enumerator->GetDevice(config.deviceId.c_str(), &device),
            "IMMDeviceEnumerator::GetDevice");
    } else {
        ThrowIfFailed(
            enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device),
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

    auto waveFormat = BuildWaveFormat(format_);
    WAVEFORMATEX* rawClosest = nullptr;
    const HRESULT support = audioClient_->IsFormatSupported(
        AUDCLNT_SHAREMODE_SHARED,
        reinterpret_cast<WAVEFORMATEX*>(&waveFormat),
        &rawClosest);

    struct WaveFormatDeleter {
        void operator()(WAVEFORMATEX* value) const noexcept { CoTaskMemFree(value); }
    };
    std::unique_ptr<WAVEFORMATEX, WaveFormatDeleter> closest(rawClosest);
    if (FAILED(support)) {
        ThrowIfFailed(support, "IAudioClient::IsFormatSupported");
    }

    const REFERENCE_TIME bufferDuration =
        static_cast<REFERENCE_TIME>(std::max<int64_t>(1, config.bufferDuration.count())) * 10'000;
    constexpr DWORD streamFlags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    const HRESULT initializeResult = audioClient_->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        streamFlags,
        bufferDuration,
        0,
        reinterpret_cast<WAVEFORMATEX*>(&waveFormat),
        nullptr);
    if (FAILED(initializeResult)) {
        std::ostringstream stream;
        stream << "Default audio render endpoint does not support incoming playback format "
               << AudioPlaybackFormatName(format_);
        if (closest) {
            stream << "; closest render mix format is " << DescribeWaveFormat(*closest);
        }
        stream << "; IAudioClient::Initialize failed with HRESULT 0x"
               << std::hex << std::uppercase << static_cast<unsigned long>(initializeResult);
        throw std::runtime_error(stream.str());
    }

    ThrowIfFailed(audioClient_->GetBufferSize(&bufferFrames_), "IAudioClient::GetBufferSize");
    ThrowIfFailed(audioClient_->GetService(IID_PPV_ARGS(&renderClient_)), "IAudioClient::GetService");
    ThrowIfFailed(audioClient_->Start(), "IAudioClient::Start");

    stats_ = {};
    stats_.bufferFrames = bufferFrames_;
    started_ = true;
}

void WasapiRenderer::Stop()
{
    if (audioClient_ && started_) {
        audioClient_->Stop();
    }
    started_ = false;
    renderClient_.Reset();
    audioClient_.Reset();
    bufferFrames_ = 0;
    deviceId_.clear();
    deviceName_.clear();
    format_ = {};
    stats_ = {};
    UninitializeCom();
}

void WasapiRenderer::SetVolume(float volume) noexcept
{
    volume_ = std::clamp(volume, 0.0f, 2.0f);
}

void WasapiRenderer::SetMuted(bool muted) noexcept
{
    muted_ = muted;
}

uint32_t WasapiRenderer::AvailableFrames()
{
    if (!started_ || !audioClient_) {
        return 0;
    }

    UINT32 padding = 0;
    ThrowIfFailed(audioClient_->GetCurrentPadding(&padding), "IAudioClient::GetCurrentPadding");
    stats_.lastPaddingFrames = padding;
    return padding >= bufferFrames_ ? 0 : bufferFrames_ - padding;
}

bool WasapiRenderer::RenderPacket(std::span<const std::byte> bytes, uint32_t frames, bool silent)
{
    if (!started_ || !renderClient_) {
        throw std::runtime_error("WASAPI renderer is not started");
    }
    if (frames == 0) {
        return true;
    }
    if (frames > bufferFrames_) {
        ++stats_.bufferFullEvents;
        return false;
    }
    const size_t expectedBytes = static_cast<size_t>(frames) * static_cast<size_t>(format_.blockAlign);
    if (bytes.size() != expectedBytes) {
        throw std::invalid_argument("Audio playback packet byte count does not match its frame count");
    }
    if (AvailableFrames() < frames) {
        ++stats_.bufferFullEvents;
        return false;
    }

    BYTE* renderData = nullptr;
    ThrowIfFailed(renderClient_->GetBuffer(frames, &renderData), "IAudioRenderClient::GetBuffer");
    const bool renderSilent = silent || muted_ || volume_ <= 0.0001f;
    try {
        if (!renderSilent) {
            if (std::abs(volume_ - 1.0f) <= 0.0001f) {
                std::memcpy(renderData, bytes.data(), bytes.size());
            } else {
                CopyScaledAudio(renderData, bytes, format_, volume_);
            }
        }
    } catch (...) {
        renderClient_->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT);
        throw;
    }

    const DWORD flags = renderSilent ? AUDCLNT_BUFFERFLAGS_SILENT : 0;
    ThrowIfFailed(renderClient_->ReleaseBuffer(frames, flags), "IAudioRenderClient::ReleaseBuffer");
    ++stats_.packetsRendered;
    stats_.framesRendered += frames;
    return true;
}

std::string AudioPlaybackFormatName(const AudioPlaybackFormat& format)
{
    std::ostringstream stream;
    stream << format.sampleRate << " Hz, "
           << format.channels << " ch, "
           << format.bitsPerSample << "-bit "
           << udp_protocol::AudioSampleFormatName(format.sampleFormat)
           << ", block_align=" << format.blockAlign;
    return stream.str();
}

bool SameAudioPlaybackFormat(const AudioPlaybackFormat& lhs, const AudioPlaybackFormat& rhs) noexcept
{
    return lhs.sampleRate == rhs.sampleRate &&
        lhs.channels == rhs.channels &&
        lhs.bitsPerSample == rhs.bitsPerSample &&
        lhs.blockAlign == rhs.blockAlign &&
        lhs.sampleFormat == rhs.sampleFormat;
}

} // namespace screenshare
