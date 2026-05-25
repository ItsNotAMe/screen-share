#include "runtime/ScreenShareRuntimeSupport.h"

#include <algorithm>
#include <chrono>
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
