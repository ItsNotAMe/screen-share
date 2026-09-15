#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>

namespace screenshare::media {
// Portable ownership boundary. Platform adapters define the resource subtype.
struct CaptureResource { virtual ~CaptureResource() = default; };
struct CaptureSample {
    std::shared_ptr<CaptureResource> resource;
    std::chrono::steady_clock::time_point capturedAt;
    uint64_t session = 0, generation = 0, sequence = 0;
};
enum class CaptureState { Starting, Running, Recovering, Closed, Failed, Stopped };
enum class CaptureFailure { None, Source, StartupTimeout, Recovery, Consumer };
struct CaptureStatus {
    CaptureState state = CaptureState::Starting;
    uint64_t session = 0, generation = 1, delivered = 0;
    CaptureFailure failure = CaptureFailure::None;
};
struct CaptureLost : std::runtime_error { CaptureLost() : std::runtime_error("Capture device lost") {} };
class ICaptureSource {
public:
    virtual ~ICaptureSource() = default;
    virtual void Start() = 0;
    virtual std::optional<CaptureSample> Poll() = 0;
    virtual bool Closed() const = 0;
    virtual void Retire() noexcept = 0;
    virtual void Rebuild() = 0;
};
// One instance is one session generation. Factory, source calls and destruction
// run on its worker. Callbacks must be short and may RequestStop, but must not
// destroy/join this owner. The coordinator joins before releasing callback state.
class CaptureSession final {
public:
    using Factory = std::function<std::unique_ptr<ICaptureSource>()>;
    using Deliver = std::function<void(CaptureSample)>;
    CaptureSession(uint64_t session, Factory, Deliver,
                   std::chrono::milliseconds startupTimeout = std::chrono::seconds(5));
    ~CaptureSession();
    CaptureSession(const CaptureSession&) = delete;
    CaptureSession& operator=(const CaptureSession&) = delete;
    void EnableDelivery();
    void RequestStop() noexcept;
    void Stop(); // Idempotent; external owner thread only.
    CaptureStatus status() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
