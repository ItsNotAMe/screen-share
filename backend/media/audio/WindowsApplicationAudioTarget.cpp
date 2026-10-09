#include "WindowsApplicationAudioTarget.h"
#include "ApplicationAudioTarget.h"
#include <windows.h>
#include <tlhelp32.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <chrono>
#include <stdexcept>

namespace screenshare::media {
namespace {
using Microsoft::WRL::ComPtr;
class Handle {
public:
    HANDLE value = nullptr;
    explicit Handle(HANDLE handle = nullptr) : value(handle) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
uint64_t Created(HANDLE process) {
    FILETIME birth{}, exit{}, kernel{}, user{};
    if (!process || !GetProcessTimes(process, &birth, &exit, &kernel, &user)) return 0;
    return (uint64_t(birth.dwHighDateTime) << 32) | birth.dwLowDateTime;
}
bool Live(HANDLE process) { return process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT; }
Handle Open(uint32_t id) { return Handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, id)); }
std::vector<AudioProcessIdentity> Processes(uint32_t root, uint64_t created) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.value == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot inspect shared app audio processes");
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    std::vector<PROCESSENTRY32W> entries;
    if (!Process32FirstW(snapshot.value, &entry)) throw std::runtime_error("Cannot enumerate shared app audio processes");
    do { entries.push_back(entry); } while (Process32NextW(snapshot.value, &entry));
    std::vector<AudioProcessIdentity> processes{{root, 0, created}};
    for (size_t i = 0; i < processes.size(); ++i) {
        const auto parent = processes[i];
        for (const auto& candidate : entries) {
            if (candidate.th32ParentProcessID != parent.id ||
                std::any_of(processes.begin(), processes.end(), [&](const auto& p) { return p.id == candidate.th32ProcessID; })) continue;
            const auto child = Open(candidate.th32ProcessID);
            const auto birth = Created(child.value);
            if (Live(child.value) && birth >= parent.created) processes.push_back({candidate.th32ProcessID, parent.id, birth});
        }
    }
    return processes;
}
std::vector<uint32_t> Renderers(std::span<const AudioProcessIdentity> processes) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
        throw std::runtime_error("Cannot inspect app audio sessions");
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices)))
        throw std::runtime_error("Cannot enumerate app audio outputs");
    UINT count = 0; devices->GetCount(&count);
    std::vector<uint32_t> active, inactive;
    for (UINT d = 0; d < count; ++d) {
        ComPtr<IMMDevice> device;
        if (FAILED(devices->Item(d, &device))) continue;
        ComPtr<IAudioSessionManager2> manager;
        if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(manager.GetAddressOf())))) continue;
        ComPtr<IAudioSessionEnumerator> sessions;
        if (FAILED(manager->GetSessionEnumerator(&sessions))) continue;
        int n = 0; sessions->GetCount(&n);
        for (int i = 0; i < n; ++i) {
            ComPtr<IAudioSessionControl> control;
            if (FAILED(sessions->GetSession(i, &control))) continue;
            AudioSessionState state{};
            if (FAILED(control->GetState(&state)) || state == AudioSessionStateExpired) continue;
            ComPtr<IAudioSessionControl2> info;
            DWORD id = 0;
            if (SUCCEEDED(control.As(&info)) && SUCCEEDED(info->GetProcessId(&id)) && id &&
                std::any_of(processes.begin(), processes.end(), [=](const auto& p) { return p.id == id; })) {
                auto& ids = state == AudioSessionStateActive ? active : inactive;
                if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
            }
        }
    }
    return active.empty() ? inactive : active;
}
}
struct WindowsApplicationAudioTarget::Impl {
    uint32_t root, target = 0;
    Handle owner;
    std::unique_ptr<Handle> selected;
    uint64_t created;
    bool com = false;
    std::chrono::steady_clock::time_point refresh{};
    explicit Impl(uint32_t id) : root(id), owner(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, id)), created(Created(owner.value)) {
        if (!created || !Live(owner.value)) throw std::runtime_error("Shared audio process is unavailable");
        const auto result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(result)) throw std::runtime_error("Cannot initialize shared app audio discovery");
        com = true;
    }
    ~Impl() { selected.reset(); if (com) CoUninitialize(); }
    uint32_t Resolve() {
        if (!Live(owner.value)) throw std::runtime_error("Shared audio process exited");
        const auto now = std::chrono::steady_clock::now();
        if (now < refresh && selected && Live(selected->value)) return target;
        const auto processes = Processes(root, created);
        const auto renderers = Renderers(processes);
        const auto next = ApplicationAudioTarget(root, processes, renderers);
        const auto identity = std::find_if(processes.begin(), processes.end(), [next](const auto& p) { return p.id == next; });
        auto handle = std::make_unique<Handle>(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, next));
        if (identity == processes.end() || !Live(handle->value) || Created(handle->value) != identity->created)
            throw std::runtime_error("Shared audio process identity changed");
        target = next; selected = std::move(handle);
        refresh = now + std::chrono::milliseconds(500);
        return target;
    }
};
WindowsApplicationAudioTarget::WindowsApplicationAudioTarget(uint32_t process) : impl_(std::make_unique<Impl>(process)) {}
WindowsApplicationAudioTarget::~WindowsApplicationAudioTarget() = default;
uint32_t WindowsApplicationAudioTarget::Resolve() { return impl_->Resolve(); }
}
