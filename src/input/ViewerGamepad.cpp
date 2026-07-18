#include "input/ViewerGamepad.h"

#include "input/XInputGamepad.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace screenshare {
namespace {

constexpr uint16_t SonyVendorId = 0x054c;
constexpr uint16_t DualShock4ProductId = 0x05c4;
constexpr uint16_t DualShock4SecondGenerationProductId = 0x09cc;
constexpr uint16_t DualShock4DongleProductId = 0x0ba0;
constexpr uint16_t DualSenseProductId = 0x0ce6;
constexpr uint16_t DualSenseEdgeProductId = 0x0df2;
constexpr uint64_t DeviceScanIntervalMs = 500;
constexpr uint64_t InputFreshnessTimeoutMs = 250;
constexpr uint32_t ReflectedCrc32Polynomial = 0xedb88320U;

constexpr uint16_t DpadUp = 0x0001;
constexpr uint16_t DpadDown = 0x0002;
constexpr uint16_t DpadLeft = 0x0004;
constexpr uint16_t DpadRight = 0x0008;
constexpr uint16_t Start = 0x0010;
constexpr uint16_t Back = 0x0020;
constexpr uint16_t LeftThumb = 0x0040;
constexpr uint16_t RightThumb = 0x0080;
constexpr uint16_t LeftShoulder = 0x0100;
constexpr uint16_t RightShoulder = 0x0200;
constexpr uint16_t A = 0x1000;
constexpr uint16_t B = 0x2000;
constexpr uint16_t X = 0x4000;
constexpr uint16_t Y = 0x8000;

class ScopedHandle {
public:
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE handle) : handle_(handle) {}
    ~ScopedHandle()
    {
        reset();
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other) noexcept : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}
    ScopedHandle& operator=(ScopedHandle&& other) noexcept
    {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const { return handle_; }
    [[nodiscard]] bool valid() const { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }

    void reset(HANDLE handle = INVALID_HANDLE_VALUE)
    {
        if (valid()) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

struct ScopedDeviceInfoSet {
    HDEVINFO value = INVALID_HANDLE_VALUE;
    ~ScopedDeviceInfoSet()
    {
        if (value != INVALID_HANDLE_VALUE) {
            SetupDiDestroyDeviceInfoList(value);
        }
    }
};

std::string WideToUtf8(std::wstring_view text)
{
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::optional<PlayStationGamepadModel> ModelForProduct(uint16_t productId)
{
    switch (productId) {
    case DualShock4ProductId:
    case DualShock4SecondGenerationProductId:
    case DualShock4DongleProductId:
        return PlayStationGamepadModel::DualShock4;
    case DualSenseProductId:
    case DualSenseEdgeProductId:
        return PlayStationGamepadModel::DualSense;
    default:
        return std::nullopt;
    }
}

const char* ModelName(uint16_t productId)
{
    if (productId == DualSenseEdgeProductId) {
        return "DualSense Edge (native HID)";
    }
    const auto model = ModelForProduct(productId);
    return model == PlayStationGamepadModel::DualShock4 ?
        "DualShock 4 (native HID)" :
        "DualSense (native HID)";
}

int16_t ConvertStickAxis(uint8_t value, bool invert)
{
    int32_t converted = 0;
    if (value < 128) {
        converted = (static_cast<int32_t>(value) - 128) * 32768 / 128;
    } else if (value > 128) {
        converted = (static_cast<int32_t>(value) - 128) * 32767 / 127;
    }
    if (invert) {
        converted = std::clamp(-converted, -32768, 32767);
    }
    return static_cast<int16_t>(converted);
}

uint32_t UpdateCrc32(uint32_t crc, std::span<const uint8_t> bytes)
{
    for (uint8_t byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1U) != 0 ? ReflectedCrc32Polynomial : 0U);
        }
    }
    return crc;
}

bool HasValidBluetoothCrc(std::span<const uint8_t> report)
{
    if (report.size() < 5) {
        return false;
    }
    const size_t crcOffset = report.size() - 4;
    const uint32_t expected =
        static_cast<uint32_t>(report[crcOffset]) |
        (static_cast<uint32_t>(report[crcOffset + 1]) << 8) |
        (static_cast<uint32_t>(report[crcOffset + 2]) << 16) |
        (static_cast<uint32_t>(report[crcOffset + 3]) << 24);
    const std::array<uint8_t, 1> seed{0xa1};
    uint32_t crc = UpdateCrc32(0xffffffffU, seed);
    crc = ~UpdateCrc32(crc, report.first(crcOffset));
    return crc == expected;
}

void AddDpadButtons(uint8_t hat, uint16_t& buttons)
{
    switch (hat) {
    case 0: buttons |= DpadUp; break;
    case 1: buttons |= DpadUp | DpadRight; break;
    case 2: buttons |= DpadRight; break;
    case 3: buttons |= DpadDown | DpadRight; break;
    case 4: buttons |= DpadDown; break;
    case 5: buttons |= DpadDown | DpadLeft; break;
    case 6: buttons |= DpadLeft; break;
    case 7: buttons |= DpadUp | DpadLeft; break;
    default: break;
    }
}

std::optional<RemoteGamepadState> ParsePlayStationReport(
    PlayStationGamepadModel model,
    std::span<const uint8_t> report,
    uint8_t controllerSlot)
{
    size_t commonOffset = 0;
    size_t buttonOffset = 0;
    size_t triggerOffset = 0;
    if (model == PlayStationGamepadModel::DualShock4) {
        if (report.size() >= 10 && report[0] == 0x01) {
            commonOffset = 1;
        } else if (report.size() == 78 && report[0] == 0x11 && HasValidBluetoothCrc(report)) {
            commonOffset = 3;
        } else {
            return std::nullopt;
        }
        buttonOffset = commonOffset + 4;
        triggerOffset = commonOffset + 7;
    } else {
        if (report.size() == 64 && report[0] == 0x01) {
            commonOffset = 1;
            buttonOffset = commonOffset + 7;
            triggerOffset = commonOffset + 4;
        } else if (report.size() >= 10 && report[0] == 0x01) {
            // Bluetooth starts in a compact compatibility mode whose first
            // nine payload bytes match the DualShock 4 core controls.
            commonOffset = 1;
            buttonOffset = commonOffset + 4;
            triggerOffset = commonOffset + 7;
        } else if (report.size() == 78 && report[0] == 0x31 && HasValidBluetoothCrc(report)) {
            commonOffset = 2;
            buttonOffset = commonOffset + 7;
            triggerOffset = commonOffset + 4;
        } else {
            return std::nullopt;
        }
    }

    if (commonOffset + 4 > report.size() || buttonOffset + 3 > report.size() ||
        triggerOffset + 2 > report.size()) {
        return std::nullopt;
    }

    const uint8_t buttons0 = report[buttonOffset];
    const uint8_t buttons1 = report[buttonOffset + 1];
    uint16_t buttons = 0;
    AddDpadButtons(buttons0 & 0x0fU, buttons);
    if ((buttons0 & 0x10U) != 0) buttons |= X;
    if ((buttons0 & 0x20U) != 0) buttons |= A;
    if ((buttons0 & 0x40U) != 0) buttons |= B;
    if ((buttons0 & 0x80U) != 0) buttons |= Y;
    if ((buttons1 & 0x01U) != 0) buttons |= LeftShoulder;
    if ((buttons1 & 0x02U) != 0) buttons |= RightShoulder;
    if ((buttons1 & 0x10U) != 0) buttons |= Back;
    if ((buttons1 & 0x20U) != 0) buttons |= Start;
    if ((buttons1 & 0x40U) != 0) buttons |= LeftThumb;
    if ((buttons1 & 0x80U) != 0) buttons |= RightThumb;

    RemoteGamepadState state;
    state.controllerSlot = controllerSlot;
    state.buttons = buttons;
    state.leftTrigger = report[triggerOffset];
    state.rightTrigger = report[triggerOffset + 1];
    state.thumbLX = ConvertStickAxis(report[commonOffset], false);
    state.thumbLY = ConvertStickAxis(report[commonOffset + 1], true);
    state.thumbRX = ConvertStickAxis(report[commonOffset + 2], false);
    state.thumbRY = ConvertStickAxis(report[commonOffset + 3], true);
    return state;
}

struct PlayStationDevice {
    std::wstring path;
    std::string id;
    std::string name;
    PlayStationGamepadModel model = PlayStationGamepadModel::DualShock4;
    ScopedHandle handle;
    ScopedHandle event;
    OVERLAPPED overlapped{};
    std::vector<uint8_t> inputBuffer;
    std::optional<RemoteGamepadState> state;
    uint64_t lastStateAt = 0;
    bool readPending = false;
    bool failed = false;

    ~PlayStationDevice()
    {
        if (handle.valid()) {
            CancelIoEx(handle.get(), &overlapped);
        }
    }

    void AcceptReport(DWORD bytesRead)
    {
        if (const auto parsed = ParsePlayStationReport(model, {inputBuffer.data(), bytesRead}, 0)) {
            state = *parsed;
            lastStateAt = GetTickCount64();
        }
    }

    bool BeginRead()
    {
        if (!handle.valid() || !event.valid() || inputBuffer.empty()) {
            return false;
        }
        ResetEvent(event.get());
        overlapped = {};
        overlapped.hEvent = event.get();
        DWORD bytesRead = 0;
        if (ReadFile(
                handle.get(), inputBuffer.data(), static_cast<DWORD>(inputBuffer.size()),
                &bytesRead, &overlapped)) {
            AcceptReport(bytesRead);
            return true;
        }
        if (GetLastError() == ERROR_IO_PENDING) {
            readPending = true;
            return true;
        }
        return false;
    }

    void Pump()
    {
        for (int completedReads = 0; completedReads < 8 && !failed; ++completedReads) {
            if (readPending) {
                DWORD bytesRead = 0;
                if (!GetOverlappedResult(handle.get(), &overlapped, &bytesRead, FALSE)) {
                    if (GetLastError() == ERROR_IO_INCOMPLETE) {
                        return;
                    }
                    failed = true;
                    return;
                }
                readPending = false;
                AcceptReport(bytesRead);
            }
            if (!BeginRead()) {
                failed = true;
                return;
            }
            if (readPending) {
                return;
            }
        }
    }
};

class PlayStationDeviceManager {
public:
    static PlayStationDeviceManager& Instance()
    {
        static PlayStationDeviceManager manager;
        return manager;
    }

    std::vector<ViewerGamepadDevice> ConnectedDevices()
    {
        std::scoped_lock lock(mutex_);
        ScanIfDue();
        std::vector<ViewerGamepadDevice> result;
        for (const auto& device : devices_) {
            device->Pump();
            if (!device->failed) {
                result.push_back({device->id, device->name});
            }
        }
        return result;
    }

    std::optional<RemoteGamepadState> ReadState(std::string_view id)
    {
        std::scoped_lock lock(mutex_);
        ScanIfDue();
        const auto found = std::find_if(devices_.begin(), devices_.end(), [&](const auto& device) {
            return device->id == id;
        });
        if (found == devices_.end()) {
            return std::nullopt;
        }
        (*found)->Pump();
        if ((*found)->failed || !(*found)->state ||
            GetTickCount64() - (*found)->lastStateAt > InputFreshnessTimeoutMs) {
            return std::nullopt;
        }
        return (*found)->state;
    }

private:
    void ScanIfDue()
    {
        const uint64_t now = GetTickCount64();
        if (hasScanned_ && now - lastScanAt_ < DeviceScanIntervalMs) {
            return;
        }
        hasScanned_ = true;
        lastScanAt_ = now;
        Scan();
    }

    void Scan()
    {
        GUID hidGuid{};
        HidD_GetHidGuid(&hidGuid);
        ScopedDeviceInfoSet deviceInfoSet;
        deviceInfoSet.value = SetupDiGetClassDevsW(
            &hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (deviceInfoSet.value == INVALID_HANDLE_VALUE) {
            return;
        }

        std::unordered_set<std::wstring> seenPaths;
        for (DWORD index = 0;; ++index) {
            SP_DEVICE_INTERFACE_DATA interfaceData{};
            interfaceData.cbSize = sizeof(interfaceData);
            if (!SetupDiEnumDeviceInterfaces(
                    deviceInfoSet.value, nullptr, &hidGuid, index, &interfaceData)) {
                if (GetLastError() == ERROR_NO_MORE_ITEMS) {
                    break;
                }
                continue;
            }

            DWORD requiredSize = 0;
            SetupDiGetDeviceInterfaceDetailW(
                deviceInfoSet.value, &interfaceData, nullptr, 0, &requiredSize, nullptr);
            if (requiredSize < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
                continue;
            }
            std::vector<uint8_t> detailBuffer(requiredSize);
            auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBuffer.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (!SetupDiGetDeviceInterfaceDetailW(
                    deviceInfoSet.value, &interfaceData, detail, requiredSize, nullptr, nullptr)) {
                continue;
            }

            const std::wstring path = detail->DevicePath;
            const auto existing = std::find_if(devices_.begin(), devices_.end(), [&](const auto& device) {
                return device->path == path;
            });
            if (existing != devices_.end() && !(*existing)->failed) {
                seenPaths.insert(path);
                continue;
            }

            ScopedHandle handle(CreateFileW(
                path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr));
            if (!handle.valid()) {
                continue;
            }

            HIDD_ATTRIBUTES attributes{};
            attributes.Size = sizeof(attributes);
            if (!HidD_GetAttributes(handle.get(), &attributes) || attributes.VendorID != SonyVendorId) {
                continue;
            }
            const auto model = ModelForProduct(attributes.ProductID);
            if (!model) {
                continue;
            }

            PHIDP_PREPARSED_DATA preparsedData = nullptr;
            HIDP_CAPS capabilities{};
            if (!HidD_GetPreparsedData(handle.get(), &preparsedData)) {
                continue;
            }
            const NTSTATUS capsStatus = HidP_GetCaps(preparsedData, &capabilities);
            HidD_FreePreparsedData(preparsedData);
            if (capsStatus != HIDP_STATUS_SUCCESS || capabilities.UsagePage != 0x01 ||
                (capabilities.Usage != 0x04 && capabilities.Usage != 0x05) ||
                capabilities.InputReportByteLength < 10) {
                continue;
            }

            auto device = std::make_unique<PlayStationDevice>();
            device->path = path;
            device->id = "playstation:" + WideToUtf8(path);
            device->name.assign(ModelName(attributes.ProductID));
            device->model = *model;
            device->handle = std::move(handle);
            device->event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            device->inputBuffer.resize(capabilities.InputReportByteLength);
            if (!device->event.valid() || !device->BeginRead()) {
                continue;
            }

            if (existing != devices_.end()) {
                *existing = std::move(device);
            } else {
                devices_.push_back(std::move(device));
            }
            seenPaths.insert(path);
        }

        std::erase_if(devices_, [&](const auto& device) {
            return !seenPaths.contains(device->path);
        });
    }

    std::mutex mutex_;
    std::vector<std::unique_ptr<PlayStationDevice>> devices_;
    uint64_t lastScanAt_ = 0;
    bool hasScanned_ = false;
};

} // namespace

std::vector<ViewerGamepadDevice> ViewerGamepad::ConnectedDevices()
{
    std::vector<ViewerGamepadDevice> devices;
    for (int slot : XInputGamepad::ConnectedSlots()) {
        devices.push_back({
            "xinput:" + std::to_string(slot),
            "Xbox / XInput Controller " + std::to_string(slot + 1)});
    }
    auto playStationDevices = PlayStationDeviceManager::Instance().ConnectedDevices();
    devices.insert(
        devices.end(),
        std::make_move_iterator(playStationDevices.begin()),
        std::make_move_iterator(playStationDevices.end()));
    return devices;
}

std::optional<RemoteGamepadState> ViewerGamepad::ReadState(std::string_view deviceId)
{
    constexpr std::string_view XInputPrefix = "xinput:";
    if (deviceId.starts_with(XInputPrefix)) {
        int slot = -1;
        const std::string_view slotText = deviceId.substr(XInputPrefix.size());
        const auto [end, error] = std::from_chars(slotText.data(), slotText.data() + slotText.size(), slot);
        if (error != std::errc{} || end != slotText.data() + slotText.size()) {
            return std::nullopt;
        }
        return XInputGamepad::ReadState(slot);
    }
    return PlayStationDeviceManager::Instance().ReadState(deviceId);
}

std::optional<RemoteGamepadState> ViewerGamepad::ParsePlayStationReport(
    PlayStationGamepadModel model,
    std::span<const uint8_t> report,
    uint8_t controllerSlot)
{
    return ::screenshare::ParsePlayStationReport(model, report, controllerSlot);
}

} // namespace screenshare
