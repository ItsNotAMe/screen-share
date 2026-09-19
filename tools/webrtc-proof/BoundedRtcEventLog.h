#pragma once
#include "api/rtc_event_log_output.h"
#include <Windows.h>
#include <atomic>
#include <filesystem>
#include <memory>
#include <stdexcept>

namespace proof {
// Synthetic proof only. Files may contain transport metadata: keep in private
// build artifacts. Four peers, at most 8 MiB each, never overwrite a prior log.
struct EventLogEvidence {
    std::atomic<unsigned> opened{0}, closed{0};
    std::atomic<bool> failed{false};
};
class BoundedRtcEventLog final : public webrtc::RtcEventLogOutput {
    std::shared_ptr<EventLogEvidence> evidence_;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    size_t bytes_ = 0;
public:
    BoundedRtcEventLog(const std::filesystem::path& path, std::shared_ptr<EventLogEvidence> evidence)
        : evidence_(std::move(evidence)) {
        file_ = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create RTC trace");
        ++evidence_->opened;
    }
    ~BoundedRtcEventLog() override {
        if (file_ != INVALID_HANDLE_VALUE && !CloseHandle(file_)) evidence_->failed = true;
        ++evidence_->closed;
    }
    bool IsActive() const override { return file_ != INVALID_HANDLE_VALUE && !evidence_->failed; }
    bool Write(absl::string_view data) override {
        if (!IsActive()) return false;
        if (data.size() > 8 * 1024 * 1024 - bytes_) { evidence_->failed = true; return false; }
        DWORD written = 0;
        if (!WriteFile(file_, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) || written != data.size()) {
            evidence_->failed = true; return false;
        }
        bytes_ += written;
        return true;
    }
};
}
