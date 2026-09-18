#pragma once
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

namespace screenshare::input {
inline std::wstring WindowIdentityProperty(uint64_t source) {return L"ScreenShare.InputCaptureIdentity.v2."+std::to_wstring(source);}
struct DesktopTarget {
    uint64_t source = 0, window = 0;
    uint32_t process = 0;
    int left = 0, top = 0, width = 0, height = 0;
    bool operator==(const DesktopTarget&) const = default;
    bool Valid() const { return source && width > 1 && height > 1 && width <= 32768 && height <= 32768; }
};
// Capture publishes geometry only with a real frame. An unchanged idle source
// may refresh liveness; a moved/resized source requires another captured frame.
class DesktopTargetState {
public:
    struct Snapshot { DesktopTarget target; uint64_t generation = 0; };
    uint64_t Publish(DesktopTarget target) {
        std::lock_guard lock(mutex_);
        if(!target.Valid())return 0;
        if(!current_.generation || target!=current_.target)current_={target,++next_};
        seen_=std::chrono::steady_clock::now();return current_.generation;
    }
    void Touch(DesktopTarget target) {
        std::lock_guard lock(mutex_);
        if(target==current_.target)seen_=std::chrono::steady_clock::now();
        else if(target.source==current_.target.source)current_.generation=0;
    }
    void Invalidate(uint64_t source=0) {
        std::lock_guard lock(mutex_);
        if(!source || current_.target.source==source)current_.generation=0;
    }
    Snapshot Read() const {
        std::lock_guard lock(mutex_);
        if(std::chrono::steady_clock::now()-seen_>std::chrono::seconds(1))return {};
        return current_;
    }
private:
    mutable std::mutex mutex_;
    Snapshot current_;
    uint64_t next_=0;
    std::chrono::steady_clock::time_point seen_{};
};
}
