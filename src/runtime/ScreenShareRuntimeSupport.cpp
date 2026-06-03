#include "runtime/ScreenShareRuntimeSupport.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <random>
#include <span>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace screenshare_runtime_internal {
namespace {

class TeeStreambuf : public std::streambuf {
public:
    TeeStreambuf(std::streambuf& first, std::streambuf& second, std::mutex& mutex)
        : first_(first),
          second_(second),
          mutex_(mutex)
    {
    }

private:
    int overflow(int ch) override
    {
        if (traits_type::eq_int_type(ch, traits_type::eof())) {
            return traits_type::not_eof(ch);
        }

        std::lock_guard lock(mutex_);
        const auto c = traits_type::to_char_type(ch);
        const bool firstOk = !traits_type::eq_int_type(first_.sputc(c), traits_type::eof());
        const bool secondOk = !traits_type::eq_int_type(second_.sputc(c), traits_type::eof());
        return firstOk && secondOk ? ch : traits_type::eof();
    }

    std::streamsize xsputn(const char* text, std::streamsize count) override
    {
        std::lock_guard lock(mutex_);
        const std::streamsize firstWritten = first_.sputn(text, count);
        const std::streamsize secondWritten = second_.sputn(text, count);
        return std::min(firstWritten, secondWritten);
    }

    int sync() override
    {
        std::lock_guard lock(mutex_);
        const int firstResult = first_.pubsync();
        const int secondResult = second_.pubsync();
        return firstResult == 0 && secondResult == 0 ? 0 : -1;
    }

    std::streambuf& first_;
    std::streambuf& second_;
    std::mutex& mutex_;
};

class CallbackStreambuf : public std::streambuf {
public:
    explicit CallbackStreambuf(std::function<void(std::string_view)> handler)
        : handler_(std::move(handler))
    {
    }

private:
    int overflow(int ch) override
    {
        if (traits_type::eq_int_type(ch, traits_type::eof())) {
            return traits_type::not_eof(ch);
        }

        const char c = traits_type::to_char_type(ch);
        std::lock_guard lock(mutex_);
        handler_(std::string_view(&c, 1));
        return ch;
    }

    std::streamsize xsputn(const char* text, std::streamsize count) override
    {
        if (count <= 0) {
            return 0;
        }

        std::lock_guard lock(mutex_);
        handler_(std::string_view(text, static_cast<size_t>(count)));
        return count;
    }

    int sync() override
    {
        return 0;
    }

    std::function<void(std::string_view)> handler_;
    std::mutex mutex_;
};

uint32_t Crc32(std::span<const uint8_t> bytes)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (const uint8_t byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

std::vector<uint8_t> ReadBinaryFile(const std::filesystem::path& path)
{
    const auto size = std::filesystem::file_size(path);
    if (size > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("Report input is too large for the zip bundle: " + path.string());
    }

    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Failed to open report input: " + path.string());
    }
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!input) {
            throw std::runtime_error("Failed to read report input: " + path.string());
        }
    }
    return bytes;
}

std::vector<uint8_t> StringBytes(const std::string& text)
{
    return std::vector<uint8_t>(text.begin(), text.end());
}

std::string ToZipPath(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

class ZipWriter {
public:
    explicit ZipWriter(const std::filesystem::path& path)
    {
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path());
        }

        output_.open(path, std::ios::binary | std::ios::trunc);
        if (!output_) {
            throw std::runtime_error("Failed to create report bundle: " + path.string());
        }
    }

    void AddFile(const std::string& archiveName, std::span<const uint8_t> bytes)
    {
        if (archiveName.empty()) {
            throw std::runtime_error("Report bundle entry name is empty");
        }
        if (bytes.size() > std::numeric_limits<uint32_t>::max()) {
            throw std::runtime_error("Report bundle entry is too large: " + archiveName);
        }

        Entry entry;
        entry.name = ToZipPath(archiveName);
        if (entry.name.size() > std::numeric_limits<uint16_t>::max()) {
            throw std::runtime_error("Report bundle entry name is too long: " + entry.name);
        }
        entry.crc = Crc32(bytes);
        entry.size = static_cast<uint32_t>(bytes.size());
        entry.localHeaderOffset = Tell();

        WriteU32(0x04034B50U);
        WriteU16(20);
        WriteU16(0);
        WriteU16(0);
        WriteU16(0);
        WriteU16(0);
        WriteU32(entry.crc);
        WriteU32(entry.size);
        WriteU32(entry.size);
        WriteU16(static_cast<uint16_t>(entry.name.size()));
        WriteU16(0);
        output_.write(entry.name.data(), static_cast<std::streamsize>(entry.name.size()));
        if (!bytes.empty()) {
            output_.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        if (!output_) {
            throw std::runtime_error("Failed to write report bundle entry: " + entry.name);
        }

        entries_.push_back(std::move(entry));
    }

    void Finish()
    {
        if (finished_) {
            return;
        }
        if (entries_.size() > std::numeric_limits<uint16_t>::max()) {
            throw std::runtime_error("Report bundle has too many entries");
        }

        const uint32_t centralDirectoryOffset = Tell();
        for (const auto& entry : entries_) {
            WriteU32(0x02014B50U);
            WriteU16(20);
            WriteU16(20);
            WriteU16(0);
            WriteU16(0);
            WriteU16(0);
            WriteU16(0);
            WriteU32(entry.crc);
            WriteU32(entry.size);
            WriteU32(entry.size);
            WriteU16(static_cast<uint16_t>(entry.name.size()));
            WriteU16(0);
            WriteU16(0);
            WriteU16(0);
            WriteU16(0);
            WriteU32(0);
            WriteU32(entry.localHeaderOffset);
            output_.write(entry.name.data(), static_cast<std::streamsize>(entry.name.size()));
        }

        const uint32_t centralDirectorySize = Tell() - centralDirectoryOffset;
        WriteU32(0x06054B50U);
        WriteU16(0);
        WriteU16(0);
        WriteU16(static_cast<uint16_t>(entries_.size()));
        WriteU16(static_cast<uint16_t>(entries_.size()));
        WriteU32(centralDirectorySize);
        WriteU32(centralDirectoryOffset);
        WriteU16(0);
        if (!output_) {
            throw std::runtime_error("Failed to finish report bundle");
        }
        finished_ = true;
    }

private:
    struct Entry {
        std::string name;
        uint32_t crc = 0;
        uint32_t size = 0;
        uint32_t localHeaderOffset = 0;
    };

    uint32_t Tell()
    {
        const auto position = output_.tellp();
        if (position < 0 || static_cast<uint64_t>(position) > std::numeric_limits<uint32_t>::max()) {
            throw std::runtime_error("Report bundle exceeded zip32 size limits");
        }
        return static_cast<uint32_t>(position);
    }

    void WriteU16(uint16_t value)
    {
        const char bytes[2] = {
            static_cast<char>(value & 0xFF),
            static_cast<char>((value >> 8) & 0xFF),
        };
        output_.write(bytes, sizeof(bytes));
    }

    void WriteU32(uint32_t value)
    {
        const char bytes[4] = {
            static_cast<char>(value & 0xFF),
            static_cast<char>((value >> 8) & 0xFF),
            static_cast<char>((value >> 16) & 0xFF),
            static_cast<char>((value >> 24) & 0xFF),
        };
        output_.write(bytes, sizeof(bytes));
    }

    std::ofstream output_;
    std::vector<Entry> entries_;
    bool finished_ = false;
};

std::string UniqueArchiveName(const std::string& requestedName, std::vector<std::string>& usedNames)
{
    std::string baseName = ToZipPath(requestedName);
    if (baseName.empty()) {
        baseName = "file";
    }

    auto isUsed = [&](const std::string& name) {
        return std::find(usedNames.begin(), usedNames.end(), name) != usedNames.end();
    };

    if (!isUsed(baseName)) {
        usedNames.push_back(baseName);
        return baseName;
    }

    const auto slash = baseName.find_last_of('/');
    const auto dot = baseName.find_last_of('.');
    const bool hasExtension = dot != std::string::npos && (slash == std::string::npos || dot > slash);
    const std::string prefix = hasExtension ? baseName.substr(0, dot) : baseName;
    const std::string extension = hasExtension ? baseName.substr(dot) : std::string{};
    for (int suffix = 2;; ++suffix) {
        const std::string candidate = prefix + "-" + std::to_string(suffix) + extension;
        if (!isUsed(candidate)) {
            usedNames.push_back(candidate);
            return candidate;
        }
    }
}

std::string CurrentLocalTimeText()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
    if (const std::tm* local = std::localtime(&time)) {
        localTime = *local;
    }

    std::ostringstream text;
    text << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
    return text.str();
}

bool IsSensitiveOption(std::string_view option)
{
    return option == "--access-code" || option == "--session-code" || option == "--signal-room-password";
}

bool RedactInlineSensitiveOption(std::string& option)
{
    constexpr std::string_view accessCodePrefix = "--access-code=";
    constexpr std::string_view sessionCodePrefix = "--session-code=";
    constexpr std::string_view signalingRoomPasswordPrefix = "--signal-room-password=";
    if (option.rfind(accessCodePrefix, 0) == 0) {
        option = std::string(accessCodePrefix) + "<redacted>";
        return true;
    }
    if (option.rfind(sessionCodePrefix, 0) == 0) {
        option = std::string(sessionCodePrefix) + "<redacted>";
        return true;
    }
    if (option.rfind(signalingRoomPasswordPrefix, 0) == 0) {
        option = std::string(signalingRoomPasswordPrefix) + "<redacted>";
        return true;
    }
    return false;
}

std::string ReadTextFile(const std::filesystem::path& path)
{
    const auto bytes = ReadBinaryFile(path);
    return std::string(bytes.begin(), bytes.end());
}

std::string JoinCommandLine(int argc, char** argv)
{
    std::ostringstream command;
    bool redactNext = false;
    for (int index = 0; index < argc; ++index) {
        if (index > 0) {
            command << ' ';
        }
        std::string arg = argv[index] != nullptr ? argv[index] : "";
        if (redactNext) {
            arg = "<redacted>";
            redactNext = false;
        } else if (RedactInlineSensitiveOption(arg)) {
            redactNext = false;
        } else if (IsSensitiveOption(arg)) {
            redactNext = true;
        }
        const bool needsQuotes = arg.find_first_of(" \t\"") != std::string::npos;
        if (!needsQuotes) {
            command << arg;
            continue;
        }
        command << '"';
        for (const char ch : arg) {
            if (ch == '"') {
                command << '\\';
            }
            command << ch;
        }
        command << '"';
    }
    return command.str();
}

using LogFields = std::unordered_map<std::string, std::string>;

LogFields ParseLogFields(std::string_view line)
{
    LogFields fields;
    size_t index = 0;
    while (index < line.size()) {
        while (index < line.size() && std::isspace(static_cast<unsigned char>(line[index]))) {
            ++index;
        }
        if (index >= line.size()) {
            break;
        }

        const size_t keyStart = index;
        while (index < line.size() &&
               !std::isspace(static_cast<unsigned char>(line[index])) &&
               line[index] != '=') {
            ++index;
        }
        if (index >= line.size() || line[index] != '=') {
            while (index < line.size() && !std::isspace(static_cast<unsigned char>(line[index]))) {
                ++index;
            }
            continue;
        }

        std::string key(line.substr(keyStart, index - keyStart));
        ++index;

        std::string value;
        if (index < line.size() && line[index] == '"') {
            ++index;
            while (index < line.size()) {
                const char ch = line[index++];
                if (ch == '"') {
                    break;
                }
                value.push_back(ch);
            }
        } else {
            const size_t valueStart = index;
            while (index < line.size() && !std::isspace(static_cast<unsigned char>(line[index]))) {
                ++index;
            }
            value.assign(line.substr(valueStart, index - valueStart));
        }

        if (!key.empty()) {
            fields[std::move(key)] = std::move(value);
        }
    }
    return fields;
}

const std::string* FindField(const LogFields& fields, const std::string& name)
{
    const auto found = fields.find(name);
    return found == fields.end() ? nullptr : &found->second;
}

std::string FieldOrDash(const LogFields& fields, const std::string& name)
{
    if (const std::string* value = FindField(fields, name)) {
        return value->empty() ? "-" : *value;
    }
    return "-";
}

std::optional<double> FieldDouble(const LogFields& fields, const std::string& name)
{
    const std::string* value = FindField(fields, name);
    if (value == nullptr || value->empty() || *value == "-") {
        return std::nullopt;
    }
    try {
        size_t parsed = 0;
        const double number = std::stod(*value, &parsed);
        return parsed == value->size() ? std::optional<double>(number) : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

std::string FormatNumber(double value)
{
    if (!std::isfinite(value)) {
        return "-";
    }

    std::ostringstream text;
    text << std::fixed << std::setprecision(2) << value;
    std::string formatted = text.str();
    while (formatted.size() > 1 && formatted.back() == '0') {
        formatted.pop_back();
    }
    if (!formatted.empty() && formatted.back() == '.') {
        formatted.pop_back();
    }
    return formatted;
}

std::string CsvEscape(const std::string& value)
{
    if (value.find_first_of(",\"\r\n") == std::string::npos) {
        return value;
    }

    std::string escaped = "\"";
    for (const char ch : value) {
        if (ch == '"') {
            escaped += "\"\"";
        } else {
            escaped += ch;
        }
    }
    escaped += '"';
    return escaped;
}

struct MetricSummary {
    size_t count = 0;
    double sum = 0.0;
    double min = std::numeric_limits<double>::infinity();
    double max = -std::numeric_limits<double>::infinity();
    double last = 0.0;

    void Add(double value)
    {
        if (!std::isfinite(value)) {
            return;
        }
        ++count;
        sum += value;
        min = std::min(min, value);
        max = std::max(max, value);
        last = value;
    }

    [[nodiscard]] double Average() const
    {
        return count == 0 ? 0.0 : sum / static_cast<double>(count);
    }
};

struct PerformanceSample {
    std::string role;
    size_t lineNumber = 0;
    LogFields fields;
};

struct PerformanceReport {
    std::string summaryMarkdown;
    std::string samplesCsv;
    size_t senderSamples = 0;
    size_t receiverSamples = 0;
    size_t viewerSamples = 0;

    [[nodiscard]] bool HasSamples() const
    {
        return senderSamples != 0 || receiverSamples != 0 || viewerSamples != 0;
    }
};

bool HasAnyField(const LogFields& fields, std::initializer_list<const char*> names)
{
    return std::any_of(names.begin(), names.end(), [&](const char* name) {
        return fields.find(name) != fields.end();
    });
}

std::string SampleResolution(const PerformanceSample& sample)
{
    if (sample.role == "sender") {
        return FieldOrDash(sample.fields, "output");
    }
    if (sample.role == "receiver") {
        return FieldOrDash(sample.fields, "h264_decoded_output");
    }
    return "-";
}

std::string SampleFps(const PerformanceSample& sample)
{
    if (sample.role == "sender") {
        return FieldOrDash(sample.fields, "output_fps");
    }
    if (sample.role == "receiver") {
        return FieldOrDash(sample.fields, "completed_fps");
    }
    return "-";
}

std::string SampleHealth(const PerformanceSample& sample)
{
    if (sample.role == "sender") {
        return FieldOrDash(sample.fields, "udp_feedback_health");
    }
    if (sample.role == "receiver") {
        return FieldOrDash(sample.fields, "receiver_health");
    }
    return FieldOrDash(sample.fields, "viewer_feedback_health");
}

void AddMetric(
    std::unordered_map<std::string, MetricSummary>& summaries,
    const PerformanceSample& sample,
    const std::string& field,
    const std::string& label)
{
    if (const auto value = FieldDouble(sample.fields, field)) {
        summaries[label].Add(*value);
    }
}

void AddMetricSum(
    std::unordered_map<std::string, MetricSummary>& summaries,
    const PerformanceSample& sample,
    std::initializer_list<const char*> fields,
    const std::string& label)
{
    double sum = 0.0;
    bool hasValue = false;
    for (const char* field : fields) {
        if (const auto value = FieldDouble(sample.fields, field)) {
            sum += *value;
            hasValue = true;
        }
    }
    if (hasValue) {
        summaries[label].Add(sum);
    }
}

void AppendMetricTable(
    std::ostringstream& report,
    const std::string& title,
    const std::vector<std::pair<std::string, MetricSummary>>& metrics)
{
    report << "### " << title << "\n\n";
    bool wroteAny = false;
    report << "| Metric | Samples | Avg | Max | Last |\n";
    report << "| --- | ---: | ---: | ---: | ---: |\n";
    for (const auto& [label, metric] : metrics) {
        if (metric.count == 0) {
            continue;
        }
        wroteAny = true;
        report
            << "| " << label
            << " | " << metric.count
            << " | " << FormatNumber(metric.Average())
            << " | " << FormatNumber(metric.max)
            << " | " << FormatNumber(metric.last)
            << " |\n";
    }
    if (!wroteAny) {
        report << "| No numeric samples found | 0 | - | - | - |\n";
    }
    report << "\n";
}

void AppendPerformanceHints(
    std::ostringstream& report,
    const std::unordered_map<std::string, MetricSummary>& metrics)
{
    std::vector<std::string> hints;
    auto metric = [&](const std::string& name) -> const MetricSummary* {
        const auto found = metrics.find(name);
        return found == metrics.end() || found->second.count == 0 ? nullptr : &found->second;
    };

    if (const auto* capture = metric("sender capture ms"); capture != nullptr && capture->max >= 8.0) {
        hints.push_back("sender capture is taking a noticeable part of a 60 FPS frame budget");
    }
    if (const auto* encode = metric("sender encode ms"); encode != nullptr && encode->max >= 8.0) {
        hints.push_back("sender encoding is taking a noticeable part of a 60 FPS frame budget");
    }
    if (const auto* udpQueue = metric("sender UDP queue ms"); udpQueue != nullptr && udpQueue->max >= 250.0) {
        hints.push_back("sender UDP pacing queue is building live latency");
    }
    if (const auto* streamQueue = metric("sender encoder queue"); streamQueue != nullptr && streamQueue->max >= 3.0) {
        hints.push_back("encoder input queue is backing up");
    }
    if (const auto* previewQueue = metric("receiver preview queue"); previewQueue != nullptr && previewQueue->max >= 8.0) {
        hints.push_back("receiver preview queue is growing");
    }
    if (const auto* decode = metric("receiver decode ms"); decode != nullptr && decode->max >= 8.0) {
        hints.push_back("receiver H.264 decode is taking a noticeable part of a 60 FPS frame budget");
    }
    if (const auto* playoutDelay = metric("receiver video playout delay ms"); playoutDelay != nullptr && playoutDelay->max >= 250.0) {
        hints.push_back("receiver preview playout delay is high");
    }
    if (const auto* audioQueue = metric("receiver audio queue ms"); audioQueue != nullptr && audioQueue->max >= 300.0) {
        hints.push_back("receiver audio queue is carrying high latency");
    }
    if (const auto* viewerQueue = metric("viewer queue ms"); viewerQueue != nullptr && viewerQueue->max >= 250.0) {
        hints.push_back("at least one viewer-specific sender queue is building latency");
    }

    report << "### Bottleneck Hints\n\n";
    if (hints.empty()) {
        report << "- No obvious queue or timing bottleneck crossed the first-pass thresholds.\n\n";
        return;
    }
    for (const std::string& hint : hints) {
        report << "- " << hint << ".\n";
    }
    report << "\n";
}

PerformanceReport BuildPerformanceReport(std::string_view consoleLog)
{
    std::vector<PerformanceSample> samples;

    std::istringstream input{std::string(consoleLog)};
    std::string line;
    size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        LogFields fields = ParseLogFields(line);
        if (fields.empty()) {
            continue;
        }

        std::string role;
        if (fields.find("viewer_target") != fields.end()) {
            role = "viewer";
        } else if (HasAnyField(fields, {"capture_avg_ms", "stream_encode_avg_ms", "output_fps"}) &&
                   HasAnyField(fields, {"source", "output"})) {
            role = "sender";
        } else if (HasAnyField(fields, {"completed_fps", "preview_queue", "audio_playback_queue_ms"}) &&
                   HasAnyField(fields, {"receiver_health", "completed_frames", "udp_datagrams"})) {
            role = "receiver";
        }

        if (!role.empty()) {
            samples.push_back(PerformanceSample{std::move(role), lineNumber, std::move(fields)});
        }
    }

    PerformanceReport result;
    std::unordered_map<std::string, MetricSummary> metrics;

    for (const PerformanceSample& sample : samples) {
        if (sample.role == "sender") {
            ++result.senderSamples;
            AddMetric(metrics, sample, "output_fps", "sender output FPS");
            AddMetric(metrics, sample, "desktop_update_fps", "sender desktop update FPS");
            AddMetric(metrics, sample, "capture_avg_ms", "sender capture ms");
            AddMetric(metrics, sample, "stream_encode_avg_ms", "sender encode ms");
            AddMetric(metrics, sample, "stream_queue", "sender encoder queue");
            AddMetric(metrics, sample, "stream_dropped", "sender encoder dropped frames");
            AddMetric(metrics, sample, "udp_queue_ms", "sender UDP queue ms");
            AddMetric(metrics, sample, "udp_peak_queue_ms", "sender peak UDP queue ms");
            AddMetric(metrics, sample, "udp_pending", "sender pending datagrams");
            AddMetric(metrics, sample, "udp_dropped_frames", "sender UDP dropped frames");
            AddMetricSum(
                metrics,
                sample,
                {"capture_avg_ms", "stream_encode_avg_ms", "udp_queue_ms"},
                "sender estimated video path ms");
        } else if (sample.role == "receiver") {
            ++result.receiverSamples;
            AddMetric(metrics, sample, "completed_fps", "receiver completed FPS");
            AddMetric(metrics, sample, "pending_frames", "receiver pending frames");
            AddMetric(metrics, sample, "h264_decode_packets", "receiver H264 packets");
            AddMetric(metrics, sample, "h264_decode_avg_ms", "receiver decode ms");
            AddMetric(metrics, sample, "h264_decoded_frames", "receiver decoded frames");
            AddMetric(metrics, sample, "preview_queue", "receiver preview queue");
            AddMetric(metrics, sample, "video_playout_delay_avg_ms", "receiver video playout delay ms");
            AddMetric(metrics, sample, "video_playout_delay_max_ms", "receiver max video playout delay ms");
            AddMetric(metrics, sample, "preview_late_drops", "receiver preview late drops");
            AddMetric(metrics, sample, "preview_overflow_drops", "receiver preview overflow drops");
            AddMetric(metrics, sample, "preview_startup_drops", "receiver preview startup drops");
            AddMetric(metrics, sample, "preview_catchup_drops", "receiver preview catchup drops");
            AddMetric(metrics, sample, "audio_playback_queue_ms", "receiver audio queue ms");
            AddMetric(metrics, sample, "audio_playback_drops", "receiver audio playback drops");
            AddMetric(metrics, sample, "audio_render_padding", "receiver audio render padding");
            AddMetricSum(
                metrics,
                sample,
                {"h264_decode_avg_ms", "video_playout_delay_avg_ms"},
                "receiver estimated video path ms");
        } else if (sample.role == "viewer") {
            ++result.viewerSamples;
            AddMetric(metrics, sample, "viewer_queue_ms", "viewer queue ms");
            AddMetric(metrics, sample, "viewer_pending", "viewer pending datagrams");
            AddMetric(metrics, sample, "viewer_feedback_completed_frames", "viewer feedback completed frames");
            AddMetric(metrics, sample, "viewer_feedback_resyncs", "viewer feedback resyncs");
        }
    }

    std::ostringstream markdown;
    markdown
        << "# ScreenShare Performance Summary\n\n"
        << "This file is generated from `logs/console.log` after the run. It adds no work to the live media path.\n\n"
        << "| Sample type | Count |\n"
        << "| --- | ---: |\n"
        << "| Sender | " << result.senderSamples << " |\n"
        << "| Receiver | " << result.receiverSamples << " |\n"
        << "| Viewer target | " << result.viewerSamples << " |\n\n";

    AppendMetricTable(markdown, "Sender Metrics", {
        {"Output FPS", metrics["sender output FPS"]},
        {"Desktop update FPS", metrics["sender desktop update FPS"]},
        {"Capture ms", metrics["sender capture ms"]},
        {"Encode ms", metrics["sender encode ms"]},
        {"Encoder queue", metrics["sender encoder queue"]},
        {"Encoder dropped frames", metrics["sender encoder dropped frames"]},
        {"UDP queue ms", metrics["sender UDP queue ms"]},
        {"Peak UDP queue ms", metrics["sender peak UDP queue ms"]},
        {"Pending datagrams", metrics["sender pending datagrams"]},
        {"UDP dropped frames", metrics["sender UDP dropped frames"]},
        {"Estimated video path ms", metrics["sender estimated video path ms"]},
    });
    AppendMetricTable(markdown, "Receiver Metrics", {
        {"Completed FPS", metrics["receiver completed FPS"]},
        {"Pending frames", metrics["receiver pending frames"]},
        {"H.264 packets", metrics["receiver H264 packets"]},
        {"Decode ms", metrics["receiver decode ms"]},
        {"Decoded frames", metrics["receiver decoded frames"]},
        {"Preview queue", metrics["receiver preview queue"]},
        {"Video playout delay ms", metrics["receiver video playout delay ms"]},
        {"Max video playout delay ms", metrics["receiver max video playout delay ms"]},
        {"Preview late drops", metrics["receiver preview late drops"]},
        {"Preview overflow drops", metrics["receiver preview overflow drops"]},
        {"Preview startup drops", metrics["receiver preview startup drops"]},
        {"Preview catchup drops", metrics["receiver preview catchup drops"]},
        {"Audio queue ms", metrics["receiver audio queue ms"]},
        {"Audio playback drops", metrics["receiver audio playback drops"]},
        {"Audio render padding", metrics["receiver audio render padding"]},
        {"Estimated video path ms", metrics["receiver estimated video path ms"]},
    });
    AppendMetricTable(markdown, "Viewer Target Metrics", {
        {"Viewer queue ms", metrics["viewer queue ms"]},
        {"Viewer pending datagrams", metrics["viewer pending datagrams"]},
        {"Viewer completed frames", metrics["viewer feedback completed frames"]},
        {"Viewer resyncs", metrics["viewer feedback resyncs"]},
    });
    AppendPerformanceHints(markdown, metrics);

    markdown
        << "### Timeline Samples\n\n"
        << "| # | Role | Line | Resolution | FPS | Capture ms | Encode ms | Decode ms | UDP queue ms | Preview queue | Video playout ms | Audio queue ms | Health |\n"
        << "| ---: | --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |\n";
    for (size_t index = 0; index < samples.size(); ++index) {
        const PerformanceSample& sample = samples[index];
        markdown
            << "| " << (index + 1)
            << " | " << sample.role
            << " | " << sample.lineNumber
            << " | " << SampleResolution(sample)
            << " | " << SampleFps(sample)
            << " | " << FieldOrDash(sample.fields, "capture_avg_ms")
            << " | " << FieldOrDash(sample.fields, "stream_encode_avg_ms")
            << " | " << FieldOrDash(sample.fields, "h264_decode_avg_ms")
            << " | " << (sample.role == "viewer" ? FieldOrDash(sample.fields, "viewer_queue_ms") : FieldOrDash(sample.fields, "udp_queue_ms"))
            << " | " << FieldOrDash(sample.fields, "preview_queue")
            << " | " << FieldOrDash(sample.fields, "video_playout_delay_avg_ms")
            << " | " << FieldOrDash(sample.fields, "audio_playback_queue_ms")
            << " | " << SampleHealth(sample)
            << " |\n";
    }
    if (samples.empty()) {
        markdown << "| 0 | none | 0 | - | - | - | - | - | - | - | - | - | no periodic performance samples found |\n";
    }

    std::vector<std::string> csvColumns = {
        "sample",
        "role",
        "line",
        "session",
        "source",
        "output",
        "h264_decoded_output",
        "output_fps",
        "completed_fps",
        "capture_avg_ms",
        "stream_encode_avg_ms",
        "stream_queue",
        "stream_dropped",
        "udp_queue_ms",
        "udp_peak_queue_ms",
        "udp_pending",
        "udp_targets",
        "udp_active_targets",
        "udp_feedback_health",
        "receiver_health",
        "pending_frames",
        "h264_decode_packets",
        "h264_decode_avg_ms",
        "h264_decoded_frames",
        "preview_queue",
        "video_playout_delay_avg_ms",
        "video_playout_delay_max_ms",
        "video_playout_delay_last_ms",
        "preview_late_drops",
        "preview_overflow_drops",
        "preview_startup_drops",
        "preview_catchup_drops",
        "audio_capture",
        "audio_playback",
        "audio_playback_queue_ms",
        "audio_playback_drops",
        "viewer_queue_ms",
        "viewer_feedback_health",
    };

    std::ostringstream csv;
    for (size_t column = 0; column < csvColumns.size(); ++column) {
        if (column > 0) {
            csv << ',';
        }
        csv << csvColumns[column];
    }
    csv << "\n";
    for (size_t index = 0; index < samples.size(); ++index) {
        const PerformanceSample& sample = samples[index];
        for (size_t column = 0; column < csvColumns.size(); ++column) {
            if (column > 0) {
                csv << ',';
            }
            if (csvColumns[column] == "sample") {
                csv << (index + 1);
            } else if (csvColumns[column] == "role") {
                csv << sample.role;
            } else if (csvColumns[column] == "line") {
                csv << sample.lineNumber;
            } else {
                csv << CsvEscape(FieldOrDash(sample.fields, csvColumns[column]));
            }
        }
        csv << "\n";
    }

    result.summaryMarkdown = markdown.str();
    result.samplesCsv = csv.str();
    return result;
}

void AppendReceiverFeedbackSummary(
    std::ostringstream& report,
    const std::optional<screenshare::udp_protocol::FeedbackSnapshot>& feedback)
{
    report << "Latest receiver feedback: ";
    if (!feedback) {
        report << "not observed\n";
        return;
    }

    const auto& snapshot = *feedback;
    report
        << "\n"
        << "  Session fingerprint: "
        << (snapshot.sessionFingerprint == 0 ? "unknown" : FormatSessionFingerprint(snapshot.sessionFingerprint))
        << "\n"
        << "  Access code required: " << (snapshot.accessCodeFingerprint == 0 ? "no" : "yes") << "\n"
        << "  Health: " << screenshare::udp_protocol::FeedbackHealthStateName(snapshot.healthState) << "\n"
        << "  Sequence: " << snapshot.sequence << "\n"
        << "  Completed frames: " << snapshot.completedFrames << "\n"
        << "  Dropped datagrams: " << snapshot.droppedDatagrams << "\n"
        << "  Invalid datagrams: " << snapshot.invalidDatagrams << "\n"
        << "  Incomplete frames dropped: " << snapshot.incompleteFramesDropped << "\n"
        << "  Decode resyncs: " << snapshot.decodeResyncs << "\n"
        << "  Decode skipped packets: " << snapshot.decodeSkippedPackets << "\n"
        << "  Preview late drops: " << snapshot.previewLateDrops << "\n"
        << "  Preview overflow drops: " << snapshot.previewOverflowDrops << "\n"
        << "  Pending frames: " << snapshot.pendingFrames << "\n"
        << "  Pending decode packets: " << snapshot.pendingDecodePackets << "\n"
        << "  Preview queued frames: " << snapshot.previewQueuedFrames << "\n";
}

} // namespace

class ScopedCallbackLogRedirect::Impl {
public:
    explicit Impl(std::function<void(std::string_view)> handler)
        : outputBuffer_(std::move(handler))
    {
        oldCout_ = std::cout.rdbuf(&outputBuffer_);
        oldCerr_ = std::cerr.rdbuf(&outputBuffer_);
    }

    ~Impl()
    {
        std::cout.flush();
        std::cerr.flush();
        if (oldCout_ != nullptr) {
            std::cout.rdbuf(oldCout_);
        }
        if (oldCerr_ != nullptr) {
            std::cerr.rdbuf(oldCerr_);
        }
    }

private:
    CallbackStreambuf outputBuffer_;
    std::streambuf* oldCout_ = nullptr;
    std::streambuf* oldCerr_ = nullptr;
};

ScopedCallbackLogRedirect::ScopedCallbackLogRedirect(std::function<void(std::string_view)> handler)
    : impl_(std::make_unique<Impl>(std::move(handler)))
{
}

ScopedCallbackLogRedirect::~ScopedCallbackLogRedirect() = default;

class ScopedLogRedirect::Impl {
public:
    explicit Impl(const std::filesystem::path& path, bool announce)
    {
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path());
        }

        log_.open(path, std::ios::out | std::ios::trunc);
        if (!log_) {
            throw std::runtime_error("Failed to open log file: " + path.string());
        }

        coutTee_ = std::make_unique<TeeStreambuf>(*std::cout.rdbuf(), *log_.rdbuf(), mutex_);
        cerrTee_ = std::make_unique<TeeStreambuf>(*std::cerr.rdbuf(), *log_.rdbuf(), mutex_);
        oldCout_ = std::cout.rdbuf(coutTee_.get());
        oldCerr_ = std::cerr.rdbuf(cerrTee_.get());

        if (announce) {
            std::cout << "Logging console output to " << path.string() << "\n";
        }
    }

    ~Impl()
    {
        std::cout.flush();
        std::cerr.flush();
        if (oldCout_ != nullptr) {
            std::cout.rdbuf(oldCout_);
        }
        if (oldCerr_ != nullptr) {
            std::cerr.rdbuf(oldCerr_);
        }
        log_.flush();
    }

private:
    std::ofstream log_;
    std::mutex mutex_;
    std::unique_ptr<TeeStreambuf> coutTee_;
    std::unique_ptr<TeeStreambuf> cerrTee_;
    std::streambuf* oldCout_ = nullptr;
    std::streambuf* oldCerr_ = nullptr;
};

ScopedLogRedirect::ScopedLogRedirect(const std::filesystem::path& path, bool announce)
    : impl_(std::make_unique<Impl>(path, announce))
{
}

ScopedLogRedirect::~ScopedLogRedirect() = default;

std::string FormatSessionFingerprint(uint64_t fingerprint)
{
    std::ostringstream text;
    text << std::uppercase << std::hex << std::setw(16) << std::setfill('0') << fingerprint;
    return text.str();
}

uint64_t SessionFingerprint(std::string_view sessionId)
{
    uint64_t hash = 14695981039346656037ULL;
    for (const char ch : sessionId) {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 1099511628211ULL;
    }
    return hash == 0 ? 1 : hash;
}

std::string GenerateSessionId()
{
    uint64_t randomPart = 0;
    try {
        std::random_device random;
        randomPart = (static_cast<uint64_t>(random()) << 32) ^ static_cast<uint64_t>(random());
    } catch (...) {
        randomPart = 0;
    }
    const uint64_t timePart = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    const uint64_t fingerprint = (randomPart ^ timePart) == 0 ? 1 : (randomPart ^ timePart);
    return FormatSessionFingerprint(fingerprint);
}

void RemoveFileIfExists(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::remove(path, error);
}

std::filesystem::path TemporaryReportLogPath(const std::filesystem::path& reportPath)
{
    std::filesystem::path tempPath = reportPath;
    tempPath += ".console.log.tmp";
    return tempPath;
}

void WriteSavedReport(
    const std::filesystem::path& outputPath,
    const std::optional<std::filesystem::path>& consoleLogPath,
    const char* argv0,
    int argc,
    char** argv,
    const SavedReportContext& reportContext,
    int exitCode)
{
    const std::filesystem::path executablePath = std::filesystem::absolute(argv0);
    const std::filesystem::path dependencyManifest =
        executablePath.has_parent_path() ?
        executablePath.parent_path() / "ScreenShare-runtime-dependencies.txt" :
        std::filesystem::path("ScreenShare-runtime-dependencies.txt");

    std::optional<PerformanceReport> performanceReport;
    if (consoleLogPath && std::filesystem::exists(*consoleLogPath) && std::filesystem::is_regular_file(*consoleLogPath)) {
        performanceReport = BuildPerformanceReport(ReadTextFile(*consoleLogPath));
    }

    std::ostringstream report;
    report
        << "ScreenShare saved run report\n\n"
        << "Generated local time: " << CurrentLocalTimeText() << "\n"
        << "Session ID: " << reportContext.sessionId << "\n"
        << "Session fingerprint: " << FormatSessionFingerprint(reportContext.sessionFingerprint) << "\n"
        << "Access code required: " << (reportContext.accessCodeRequired ? "yes" : "no") << "\n"
        << "UDP encryption: " << (reportContext.encryptionEnabled ? "yes" : "no") << "\n"
        << "Exit code: " << exitCode << "\n"
        << "Working directory: " << std::filesystem::current_path().string() << "\n"
        << "Executable: " << executablePath.string() << "\n"
        << "Command line: " << JoinCommandLine(argc, argv) << "\n"
        << "Build: "
#ifdef NDEBUG
        << "release"
#else
        << "debug"
#endif
        << "\n"
        << "Pointer bits: " << (sizeof(void*) * 8) << "\n"
        << "Console log: "
        << (consoleLogPath ? std::filesystem::absolute(*consoleLogPath).string() : "not captured")
        << "\n";
    if (consoleLogPath && std::filesystem::exists(*consoleLogPath)) {
        report << "Console log bytes: " << std::filesystem::file_size(*consoleLogPath) << "\n";
    }
    if (performanceReport && performanceReport->HasSamples()) {
        report
            << "Performance samples: sender=" << performanceReport->senderSamples
            << ", receiver=" << performanceReport->receiverSamples
            << ", viewer=" << performanceReport->viewerSamples
            << "\n"
            << "Performance summary: performance/performance-summary.md\n"
            << "Performance samples CSV: performance/performance-samples.csv\n";
    } else {
        report << "Performance samples: none found\n";
    }
    AppendReceiverFeedbackSummary(report, reportContext.latestReceiverFeedback);
    report
        << "Runtime dependency manifest: "
        << (std::filesystem::exists(dependencyManifest) ? dependencyManifest.string() : "not found")
        << "\n";

    ZipWriter zip(outputPath);
    std::vector<std::string> archiveNames;
    zip.AddFile(UniqueArchiveName("ScreenShare-report.txt", archiveNames), StringBytes(report.str()));

    if (consoleLogPath && std::filesystem::exists(*consoleLogPath) && std::filesystem::is_regular_file(*consoleLogPath)) {
        zip.AddFile(
            UniqueArchiveName("logs/console.log", archiveNames),
            ReadBinaryFile(*consoleLogPath));
    }

    if (performanceReport) {
        zip.AddFile(
            UniqueArchiveName("performance/performance-summary.md", archiveNames),
            StringBytes(performanceReport->summaryMarkdown));
        zip.AddFile(
            UniqueArchiveName("performance/performance-samples.csv", archiveNames),
            StringBytes(performanceReport->samplesCsv));
    }

    if (std::filesystem::exists(dependencyManifest) && std::filesystem::is_regular_file(dependencyManifest)) {
        zip.AddFile(
            UniqueArchiveName("runtime/ScreenShare-runtime-dependencies.txt", archiveNames),
            ReadBinaryFile(dependencyManifest));
    }

    zip.Finish();
}

std::vector<char*> MutableArgv(std::vector<std::string>& arguments)
{
    if (arguments.empty()) {
        arguments.emplace_back("ScreenShare");
    }

    std::vector<char*> argv;
    argv.reserve(arguments.size());
    for (std::string& argument : arguments) {
        argv.push_back(argument.data());
    }
    return argv;
}

} // namespace screenshare_runtime_internal
