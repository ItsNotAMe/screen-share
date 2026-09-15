#include "input/VirtualGamepadBackend.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <memory>
#include <utility>

namespace screenshare {
namespace {

constexpr uint32_t VigEmErrorNone = 0x20000000U;

struct XusbReport {
    uint16_t buttons = 0;
    uint8_t leftTrigger = 0;
    uint8_t rightTrigger = 0;
    int16_t thumbLX = 0;
    int16_t thumbLY = 0;
    int16_t thumbRX = 0;
    int16_t thumbRY = 0;
};
static_assert(sizeof(XusbReport) == 12);

struct VigEmApi {
    using AllocFn = void* (*)();
    using ConnectFn = uint32_t (*)(void*);
    using DisconnectFn = void (*)(void*);
    using FreeFn = void (*)(void*);
    using TargetAllocFn = void* (*)();
    using TargetAddFn = uint32_t (*)(void*, void*);
    using TargetRemoveFn = uint32_t (*)(void*, void*);
    using TargetFreeFn = void (*)(void*);
    using TargetUpdateFn = uint32_t (*)(void*, void*, XusbReport);

    HMODULE module = nullptr;
    void* client = nullptr;
    DisconnectFn disconnect = nullptr;
    FreeFn freeClient = nullptr;
    TargetAllocFn targetAlloc = nullptr;
    TargetAddFn targetAdd = nullptr;
    TargetRemoveFn targetRemove = nullptr;
    TargetFreeFn targetFree = nullptr;
    TargetUpdateFn targetUpdate = nullptr;

    ~VigEmApi()
    {
        if (client != nullptr && disconnect != nullptr) {
            disconnect(client);
        }
        if (client != nullptr && freeClient != nullptr) {
            freeClient(client);
        }
        if (module != nullptr) {
            FreeLibrary(module);
        }
    }
};

template <typename T>
T Resolve(HMODULE module, const char* name)
{
    return reinterpret_cast<T>(GetProcAddress(module, name));
}

std::pair<std::shared_ptr<VigEmApi>, std::string> LoadVigEm()
{
    auto api = std::make_shared<VigEmApi>();
    api->module = LoadLibraryW(L"ViGEmClient.dll");
    if (api->module == nullptr) {
        return {nullptr, "Controller support is incomplete. Repair or reinstall ScreenShare."};
    }

    const auto alloc = Resolve<VigEmApi::AllocFn>(api->module, "vigem_alloc");
    const auto connect = Resolve<VigEmApi::ConnectFn>(api->module, "vigem_connect");
    api->disconnect = Resolve<VigEmApi::DisconnectFn>(api->module, "vigem_disconnect");
    api->freeClient = Resolve<VigEmApi::FreeFn>(api->module, "vigem_free");
    api->targetAlloc = Resolve<VigEmApi::TargetAllocFn>(api->module, "vigem_target_x360_alloc");
    api->targetAdd = Resolve<VigEmApi::TargetAddFn>(api->module, "vigem_target_add");
    api->targetRemove = Resolve<VigEmApi::TargetRemoveFn>(api->module, "vigem_target_remove");
    api->targetFree = Resolve<VigEmApi::TargetFreeFn>(api->module, "vigem_target_free");
    api->targetUpdate = Resolve<VigEmApi::TargetUpdateFn>(api->module, "vigem_target_x360_update");
    if (alloc == nullptr || connect == nullptr || api->disconnect == nullptr || api->freeClient == nullptr ||
        api->targetAlloc == nullptr || api->targetAdd == nullptr || api->targetRemove == nullptr ||
        api->targetFree == nullptr || api->targetUpdate == nullptr) {
        return {nullptr, "The installed controller support is incompatible. Repair or update ScreenShare."};
    }

    api->client = alloc();
    if (api->client == nullptr) {
        return {nullptr, "The controller service could not be initialized."};
    }
    const uint32_t result = connect(api->client);
    if (result != VigEmErrorNone) {
        return {nullptr, "Controller support is unavailable. Repair or reinstall ScreenShare (error " +
            std::to_string(result) + ")."};
    }
    return {std::move(api), "Controller support is ready."};
}

class VigEmDevice final : public VirtualGamepadDevice {
public:
    VigEmDevice(std::shared_ptr<VigEmApi> api, void* target)
        : api_(std::move(api)), target_(target)
    {
    }

    ~VigEmDevice() override
    {
        Neutralize();
        Destroy();
    }

    bool SubmitState(const RemoteGamepadState& state, std::string* errorMessage) override
    {
        if (target_ == nullptr) {
            if (errorMessage != nullptr) {
                *errorMessage = "The virtual controller has already been destroyed.";
            }
            return false;
        }
        XusbReport report;
        report.buttons = state.buttons;
        report.leftTrigger = state.leftTrigger;
        report.rightTrigger = state.rightTrigger;
        report.thumbLX = state.thumbLX;
        report.thumbLY = state.thumbLY;
        report.thumbRX = state.thumbRX;
        report.thumbRY = state.thumbRY;
        const uint32_t result = api_->targetUpdate(api_->client, target_, report);
        if (result == VigEmErrorNone) {
            return true;
        }
        if (errorMessage != nullptr) {
            *errorMessage = "ViGEm rejected the controller state (error " + std::to_string(result) + ").";
        }
        return false;
    }

    void Neutralize() noexcept override
    {
        if (target_ != nullptr) {
            static_cast<void>(api_->targetUpdate(api_->client, target_, XusbReport{}));
        }
    }

    void Destroy() noexcept override
    {
        if (target_ == nullptr) {
            return;
        }
        static_cast<void>(api_->targetRemove(api_->client, target_));
        api_->targetFree(target_);
        target_ = nullptr;
    }

private:
    std::shared_ptr<VigEmApi> api_;
    void* target_ = nullptr;
};

class VigEmBackend final : public VirtualGamepadBackend {
public:
    VigEmBackend()
    {
        auto [api, message] = LoadVigEm();
        api_ = std::move(api);
        message_ = std::move(message);
    }

    VirtualGamepadBackendStatus Status() const override
    {
        return VirtualGamepadBackendStatus{api_ != nullptr, message_};
    }

    std::unique_ptr<VirtualGamepadDevice> CreateXbox360(std::string* errorMessage) override
    {
        if (api_ == nullptr) {
            if (errorMessage != nullptr) {
                *errorMessage = message_;
            }
            return nullptr;
        }
        void* target = api_->targetAlloc();
        if (target == nullptr) {
            if (errorMessage != nullptr) {
                *errorMessage = "ViGEm could not allocate a virtual Xbox controller.";
            }
            return nullptr;
        }
        const uint32_t result = api_->targetAdd(api_->client, target);
        if (result != VigEmErrorNone) {
            api_->targetFree(target);
            if (errorMessage != nullptr) {
                *errorMessage = "ViGEm could not attach a virtual Xbox controller (error " +
                    std::to_string(result) + ").";
            }
            return nullptr;
        }
        return std::make_unique<VigEmDevice>(api_, target);
    }

private:
    std::shared_ptr<VigEmApi> api_;
    std::string message_;
};

} // namespace

std::unique_ptr<VirtualGamepadBackend> CreateVirtualGamepadBackend()
{
    return std::make_unique<VigEmBackend>();
}

} // namespace screenshare
