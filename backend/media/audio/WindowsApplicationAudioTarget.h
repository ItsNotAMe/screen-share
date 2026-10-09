#pragma once
#include <memory>
#include <cstdint>

namespace screenshare::media {
// Owned and polled on the capture worker, including COM lifetime. Pins the
// window owner's identity and refreshes render sessions as helpers restart.
class WindowsApplicationAudioTarget {
public:
    explicit WindowsApplicationAudioTarget(uint32_t process);
    ~WindowsApplicationAudioTarget();
    WindowsApplicationAudioTarget(const WindowsApplicationAudioTarget&) = delete;
    WindowsApplicationAudioTarget& operator=(const WindowsApplicationAudioTarget&) = delete;
    uint32_t Resolve();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
