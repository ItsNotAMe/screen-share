#pragma once
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace screenshare::media {
// Only explicitly selected numeric fields and fixed vocabulary enter these
// records. Never store native ToJson(), SDP, exception text or network addresses.
struct DiagnosticRecord {
    uint64_t atMs = 0;
    std::map<std::string, double> numbers;
    std::map<std::string, std::string> labels;
};
struct DiagnosticHistorySnapshot {
    std::shared_ptr<const std::vector<DiagnosticRecord>> records;
    uint64_t omitted = 0;
    uint64_t startedUtcMs = 0, elapsedMs = 0;
    std::shared_ptr<const DiagnosticRecord> firstFailure;
};
class DiagnosticHistory {
public:
    using Clock = std::chrono::steady_clock;
    explicit DiagnosticHistory(size_t limit = 128, bool keepStartup = true)
        : limit_(std::max(size_t(1), limit)), retainFirst_(keepStartup ? std::min(size_t(16), limit_ / 4) : 0) {}
    uint64_t elapsedMs() const { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started_).count(); }
    void Add(DiagnosticRecord value) {
        std::lock_guard lock(mutex_);
        value.atMs = elapsedMs();
        if (!firstFailure_) for (const auto& [key, label] : value.labels) {
            if ((key == "state" && label == "failed") || (key == "event" &&
                (label.find("failed") != std::string::npos || label.find("failure") != std::string::npos || label.find("-error") != std::string::npos))) {
                firstFailure_ = std::make_shared<const DiagnosticRecord>(value); break;
            }
        }
        if (records_.size() == limit_) { records_.erase(records_.begin() + retainFirst_); ++omitted_; }
        records_.push_back(std::move(value)); cached_.reset();
    }
    void Event(const char* operation, int64_t code = 0, int64_t detail = 0) {
        Add({0, {{"code", double(code)}, {"detail", double(detail)}}, {{"event", operation}}});
    }
    DiagnosticHistorySnapshot Read() const {
        std::lock_guard lock(mutex_);
        if (!cached_) cached_ = std::make_shared<const std::vector<DiagnosticRecord>>(records_.begin(), records_.end());
        return {cached_, omitted_, startedUtcMs_, elapsedMs(), firstFailure_};
    }
private:
    const Clock::time_point started_ = Clock::now();
    const uint64_t startedUtcMs_ = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const size_t limit_;
    const size_t retainFirst_;
    mutable std::mutex mutex_;
    std::deque<DiagnosticRecord> records_;
    uint64_t omitted_ = 0;
    mutable std::shared_ptr<const std::vector<DiagnosticRecord>> cached_;
    std::shared_ptr<const DiagnosticRecord> firstFailure_;
};
template<class T> inline void DiagnosticNumber(DiagnosticRecord& record, const char* name, std::optional<T> value, double scale = 1) {
    if (value && std::isfinite(double(*value) * scale)) record.numbers[name] = double(*value) * scale;
}
inline std::string DiagnosticLabel(const std::optional<std::string>& value, std::initializer_list<const char*> allowed) {
    if (value) for (const auto* item : allowed) if (*value == item) return item;
    return "unknown";
}
}
