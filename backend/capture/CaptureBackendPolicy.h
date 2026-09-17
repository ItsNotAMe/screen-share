#pragma once
#include <Windows.h>
#include <dxgi.h>
#include <stdexcept>
#include <string>
#include <utility>

namespace screenshare {
class CaptureBackendError : public std::runtime_error {
public:
    CaptureBackendError(HRESULT result, std::string message) : std::runtime_error(std::move(message)), result_(result) {}
    HRESULT result() const noexcept { return result_; }
private:
    HRESULT result_;
};
// Only an unavailable implementation permits display fallback. Permission,
// source, device-loss and arbitrary driver failures never broaden capture.
inline bool CaptureBackendUnavailable(HRESULT result) noexcept {
    return result == E_NOINTERFACE || result == E_NOTIMPL || result == REGDB_E_CLASSNOTREG ||
        result == DXGI_ERROR_UNSUPPORTED || result == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
}
template<class Preferred, class Fallback>
void StartDisplayCapture(bool allowFallback, Preferred&& preferred, Fallback&& fallback) {
    try { preferred(); }
    catch (const CaptureBackendError& error) {
        if (!allowFallback || !CaptureBackendUnavailable(error.result())) throw;
        fallback();
    }
}
}
