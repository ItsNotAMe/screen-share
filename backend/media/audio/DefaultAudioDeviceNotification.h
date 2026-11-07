#pragma once
#include <mmdeviceapi.h>
#include <wrl/implements.h>
#include <atomic>
#include <cstdint>

namespace screenshare::media {
// Notifications may run on arbitrary system threads. Do not open/release audio
// clients or wait here. The registered owner retains this object until after
// UnregisterEndpointNotificationCallback returns.
class DefaultAudioDeviceNotification final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMMNotificationClient> {
    std::atomic<uint64_t> revision_{0};
    const EDataFlow flow_;
public:
    explicit DefaultAudioDeviceNotification(EDataFlow flow = eRender) : flow_(flow) {}
    uint64_t revision() const { return revision_.load(); }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == flow_ && role == eConsole) ++revision_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
};
}
