#include "transport/SignalingClient.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace screenshare {
namespace {

class WinHttpHandle {
public:
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET handle) : handle_(handle) {}
    ~WinHttpHandle()
    {
        if (handle_ != nullptr) {
            WinHttpCloseHandle(handle_);
        }
    }

    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;

    WinHttpHandle(WinHttpHandle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    WinHttpHandle& operator=(WinHttpHandle&& other) noexcept
    {
        if (this != &other) {
            if (handle_ != nullptr) {
                WinHttpCloseHandle(handle_);
            }
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] HINTERNET get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

private:
    HINTERNET handle_ = nullptr;
};

std::string WindowsErrorMessage(const char* operation, DWORD error)
{
    char* buffer = nullptr;
    const DWORD flags =
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD chars = FormatMessageA(
        flags,
        nullptr,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<char*>(&buffer),
        0,
        nullptr);

    std::string message = std::string(operation) + " failed with Windows error " + std::to_string(error);
    if (chars != 0 && buffer != nullptr) {
        std::string detail(buffer, chars);
        while (!detail.empty() && (detail.back() == '\r' || detail.back() == '\n' || detail.back() == ' ')) {
            detail.pop_back();
        }
        if (!detail.empty()) {
            message += ": " + detail;
        }
    }
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    return message;
}

std::string WinHttpErrorMessage(const char* operation)
{
    return WindowsErrorMessage(operation, GetLastError());
}

std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty()) {
        return {};
    }
    const int wideChars = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0);
    if (wideChars <= 0) {
        throw std::invalid_argument("Text is not valid UTF-8");
    }
    std::wstring wide(static_cast<size_t>(wideChars), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            text.data(),
            static_cast<int>(text.size()),
            wide.data(),
            wideChars) != wideChars) {
        throw std::runtime_error(WinHttpErrorMessage("MultiByteToWideChar"));
    }
    return wide;
}

bool IsValidIpv4(std::string_view ip)
{
    int parts = 0;
    size_t offset = 0;
    while (offset <= ip.size()) {
        const size_t dot = ip.find('.', offset);
        const size_t end = dot == std::string_view::npos ? ip.size() : dot;
        if (end == offset || end - offset > 3) {
            return false;
        }
        int value = 0;
        for (size_t index = offset; index < end; ++index) {
            if (!std::isdigit(static_cast<unsigned char>(ip[index]))) {
                return false;
            }
            value = value * 10 + (ip[index] - '0');
        }
        if (value > 255) {
            return false;
        }
        ++parts;
        if (dot == std::string_view::npos) {
            break;
        }
        offset = dot + 1;
    }
    return parts == 4;
}

bool IsLikelyIpv6(std::string_view ip)
{
    if (ip.find(':') == std::string_view::npos || ip.size() < 2 || ip.size() > 45) {
        return false;
    }
    return std::all_of(ip.begin(), ip.end(), [](char ch) {
        return std::isxdigit(static_cast<unsigned char>(ch)) || ch == ':' || ch == '.';
    });
}

SignalingClient::ParsedUrl ParseServerUrl(const std::string& url)
{
    const std::wstring wideUrl = Utf8ToWide(url);

    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()), 0, &parts)) {
        throw std::invalid_argument(WinHttpErrorMessage("WinHttpCrackUrl"));
    }
    if (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) {
        throw std::invalid_argument("Signaling server URL must start with http:// or https://");
    }
    if (parts.dwHostNameLength == 0) {
        throw std::invalid_argument("Signaling server URL is missing a host");
    }

    SignalingClient::ParsedUrl parsed;
    parsed.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    parsed.port = static_cast<uint16_t>(parts.nPort);
    parsed.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    if (parts.dwUrlPathLength > 0) {
        parsed.pathPrefix.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    }
    if (parts.dwExtraInfoLength > 0) {
        throw std::invalid_argument("Signaling server URL must not include a query string");
    }
    while (parsed.pathPrefix.size() > 1 && parsed.pathPrefix.back() == L'/') {
        parsed.pathPrefix.pop_back();
    }
    if (parsed.pathPrefix == L"/") {
        parsed.pathPrefix.clear();
    }
    return parsed;
}

std::string UrlEncode(std::string_view text)
{
    std::ostringstream output;
    output << std::hex;
    for (const unsigned char ch : text) {
        const bool safe =
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' ||
            ch == '_' ||
            ch == '.' ||
            ch == '~';
        if (safe) {
            output << static_cast<char>(ch);
        } else {
            output << '%';
            output.width(2);
            output.fill('0');
            output << static_cast<int>(ch);
        }
    }
    return output.str();
}

std::string RoomPath(const std::string& roomId, std::string_view action)
{
    return "/rooms/" + UrlEncode(roomId) + "/" + std::string(action);
}

std::string RoomEventsPath(
    const std::string& roomId,
    const std::string& peerId,
    const std::string& roomPassword)
{
    std::string path = RoomPath(roomId, "events") + "?peerId=" + UrlEncode(peerId);
    if (!roomPassword.empty()) {
        path += "&roomPassword=" + UrlEncode(roomPassword);
    }
    return path;
}

std::string ReadResponseBody(HINTERNET request)
{
    std::string body;
    while (true) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            throw std::runtime_error(WinHttpErrorMessage("WinHttpQueryDataAvailable"));
        }
        if (available == 0) {
            break;
        }
        const size_t oldSize = body.size();
        body.resize(oldSize + available);
        DWORD read = 0;
        if (!WinHttpReadData(request, body.data() + oldSize, available, &read)) {
            throw std::runtime_error(WinHttpErrorMessage("WinHttpReadData"));
        }
        body.resize(oldSize + read);
    }
    return body;
}

std::string PeerRequestJson(const std::string& peerId)
{
    return "{\"peerId\":\"" + SignalingJsonEscape(peerId) + "\"}";
}

void ValidateSignalingRoomName(const std::string& roomName)
{
    if (roomName.size() > 80) {
        throw std::invalid_argument("Signaling room name must be 80 bytes or shorter");
    }
    for (const unsigned char ch : roomName) {
        if (ch < 0x20) {
            throw std::invalid_argument("Signaling room name must not contain control characters");
        }
    }
}

void ValidateSignalingRoomPassword(const std::string& roomPassword)
{
    if (roomPassword.empty()) {
        return;
    }
    if (roomPassword.size() > 128) {
        throw std::invalid_argument("Signaling room password must be 128 bytes or shorter");
    }
    for (const unsigned char ch : roomPassword) {
        if (ch < 0x20) {
            throw std::invalid_argument("Signaling room password must not contain control characters");
        }
    }
}

std::wstring RoomPasswordHeader(const std::string& roomPassword)
{
    if (roomPassword.empty()) {
        return {};
    }
    return L"X-ScreenShare-Room-Password: " + Utf8ToWide(UrlEncode(roomPassword)) + L"\r\n";
}

std::string JoinRequestJson(const SignalingPeerState& peer)
{
    std::string json = "{\"peerId\":\"" + SignalingJsonEscape(peer.peerId) + "\",\"candidates\":[";
    for (size_t index = 0; index < peer.candidates.size(); ++index) {
        const auto& candidate = peer.candidates[index];
        if (index != 0) {
            json += ",";
        }
        json +=
            "{\"type\":\"" + SignalingJsonEscape(candidate.type) +
            "\",\"ip\":\"" + SignalingJsonEscape(candidate.ip) +
            "\",\"port\":" + std::to_string(candidate.port) +
            ",\"protocol\":\"" + SignalingJsonEscape(candidate.protocol) + "\"}";
    }
    json += "]";
    if (!peer.metadata.name.empty() || !peer.metadata.platform.empty()) {
        json += ",\"metadata\":{";
        bool wrote = false;
        if (!peer.metadata.name.empty()) {
            json += "\"name\":\"" + SignalingJsonEscape(peer.metadata.name) + "\"";
            wrote = true;
        }
        if (!peer.metadata.platform.empty()) {
            if (wrote) {
                json += ",";
            }
            json += "\"platform\":\"" + SignalingJsonEscape(peer.metadata.platform) + "\"";
        }
        json += "}";
    }
    if (!peer.roomName.empty() || peer.passwordProtected || !peer.roomPassword.empty()) {
        json += ",\"room\":{";
        bool wrote = false;
        if (!peer.roomName.empty()) {
            json += "\"name\":\"" + SignalingJsonEscape(peer.roomName) + "\"";
            wrote = true;
        }
        if (!peer.roomPassword.empty()) {
            if (wrote) {
                json += ",";
            }
            json += "\"password\":\"" + SignalingJsonEscape(peer.roomPassword) + "\"";
            wrote = true;
        }
        if (peer.passwordProtected) {
            if (wrote) {
                json += ",";
            }
            json += "\"passwordProtected\":true";
        }
        json += "}";
    }
    json += "}";
    return json;
}

enum class JsonType {
    Null,
    Bool,
    Number,
    String,
    Array,
    Object,
};

struct JsonValue {
    JsonType type = JsonType::Null;
    bool boolValue = false;
    double numberValue = 0.0;
    std::string stringValue;
    std::vector<JsonValue> arrayValue;
    std::map<std::string, JsonValue> objectValue;

    [[nodiscard]] const JsonValue* Find(std::string_view key) const
    {
        if (type != JsonType::Object) {
            return nullptr;
        }
        const auto iterator = objectValue.find(std::string(key));
        return iterator == objectValue.end() ? nullptr : &iterator->second;
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    JsonValue Parse()
    {
        JsonValue value = ParseValue();
        SkipWhitespace();
        if (offset_ != text_.size()) {
            throw std::runtime_error("Signaling response has trailing JSON data");
        }
        return value;
    }

private:
    JsonValue ParseValue()
    {
        SkipWhitespace();
        if (offset_ >= text_.size()) {
            throw std::runtime_error("Signaling response has truncated JSON");
        }
        const char ch = text_[offset_];
        if (ch == 'n') {
            ConsumeLiteral("null");
            return {};
        }
        if (ch == 't') {
            ConsumeLiteral("true");
            JsonValue value;
            value.type = JsonType::Bool;
            value.boolValue = true;
            return value;
        }
        if (ch == 'f') {
            ConsumeLiteral("false");
            JsonValue value;
            value.type = JsonType::Bool;
            value.boolValue = false;
            return value;
        }
        if (ch == '"') {
            JsonValue value;
            value.type = JsonType::String;
            value.stringValue = ParseString();
            return value;
        }
        if (ch == '[') {
            return ParseArray();
        }
        if (ch == '{') {
            return ParseObject();
        }
        if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch))) {
            return ParseNumber();
        }
        throw std::runtime_error("Signaling response has invalid JSON value");
    }

    JsonValue ParseArray()
    {
        Expect('[');
        JsonValue value;
        value.type = JsonType::Array;
        SkipWhitespace();
        if (TryConsume(']')) {
            return value;
        }
        while (true) {
            value.arrayValue.push_back(ParseValue());
            SkipWhitespace();
            if (TryConsume(']')) {
                return value;
            }
            Expect(',');
        }
    }

    JsonValue ParseObject()
    {
        Expect('{');
        JsonValue value;
        value.type = JsonType::Object;
        SkipWhitespace();
        if (TryConsume('}')) {
            return value;
        }
        while (true) {
            SkipWhitespace();
            const std::string key = ParseString();
            SkipWhitespace();
            Expect(':');
            value.objectValue[key] = ParseValue();
            SkipWhitespace();
            if (TryConsume('}')) {
                return value;
            }
            Expect(',');
        }
    }

    JsonValue ParseNumber()
    {
        const size_t start = offset_;
        if (text_[offset_] == '-') {
            ++offset_;
        }
        while (offset_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[offset_]))) {
            ++offset_;
        }
        if (offset_ < text_.size() && text_[offset_] == '.') {
            ++offset_;
            while (offset_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[offset_]))) {
                ++offset_;
            }
        }
        if (offset_ < text_.size() && (text_[offset_] == 'e' || text_[offset_] == 'E')) {
            ++offset_;
            if (offset_ < text_.size() && (text_[offset_] == '+' || text_[offset_] == '-')) {
                ++offset_;
            }
            while (offset_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[offset_]))) {
                ++offset_;
            }
        }
        const std::string numberText(text_.substr(start, offset_ - start));
        char* end = nullptr;
        const double number = std::strtod(numberText.c_str(), &end);
        if (end == numberText.c_str() || *end != '\0') {
            throw std::runtime_error("Signaling response has invalid JSON number");
        }
        JsonValue value;
        value.type = JsonType::Number;
        value.numberValue = number;
        return value;
    }

    std::string ParseString()
    {
        Expect('"');
        std::string output;
        while (offset_ < text_.size()) {
            const char ch = text_[offset_++];
            if (ch == '"') {
                return output;
            }
            if (ch != '\\') {
                output.push_back(ch);
                continue;
            }
            if (offset_ >= text_.size()) {
                throw std::runtime_error("Signaling response has truncated JSON string escape");
            }
            const char escaped = text_[offset_++];
            switch (escaped) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u':
                output += "\\u";
                for (int i = 0; i < 4; ++i) {
                    if (offset_ >= text_.size() || !std::isxdigit(static_cast<unsigned char>(text_[offset_]))) {
                        throw std::runtime_error("Signaling response has invalid JSON unicode escape");
                    }
                    output.push_back(text_[offset_++]);
                }
                break;
            default:
                throw std::runtime_error("Signaling response has invalid JSON string escape");
            }
        }
        throw std::runtime_error("Signaling response has unterminated JSON string");
    }

    void ConsumeLiteral(std::string_view literal)
    {
        if (text_.substr(offset_, literal.size()) != literal) {
            throw std::runtime_error("Signaling response has invalid JSON literal");
        }
        offset_ += literal.size();
    }

    void SkipWhitespace()
    {
        while (offset_ < text_.size()) {
            const char ch = text_[offset_];
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
                break;
            }
            ++offset_;
        }
    }

    void Expect(char expected)
    {
        SkipWhitespace();
        if (offset_ >= text_.size() || text_[offset_] != expected) {
            throw std::runtime_error("Signaling response has malformed JSON");
        }
        ++offset_;
    }

    bool TryConsume(char expected)
    {
        SkipWhitespace();
        if (offset_ < text_.size() && text_[offset_] == expected) {
            ++offset_;
            return true;
        }
        return false;
    }

    std::string_view text_;
    size_t offset_ = 0;
};

std::string JsonStringOrEmpty(const JsonValue* value)
{
    return value != nullptr && value->type == JsonType::String ? value->stringValue : std::string{};
}

int64_t JsonIntegerOrZero(const JsonValue* value)
{
    if (value == nullptr || value->type != JsonType::Number) {
        return 0;
    }
    return static_cast<int64_t>(value->numberValue);
}

bool JsonBoolOrFalse(const JsonValue* value)
{
    return value != nullptr && value->type == JsonType::Bool && value->boolValue;
}

SignalingCandidate ParseCandidate(const JsonValue& value)
{
    if (value.type != JsonType::Object) {
        throw std::runtime_error("Signaling response candidate is not an object");
    }
    SignalingCandidate candidate;
    candidate.type = JsonStringOrEmpty(value.Find("type"));
    candidate.ip = JsonStringOrEmpty(value.Find("ip"));
    candidate.protocol = JsonStringOrEmpty(value.Find("protocol"));
    const int64_t port = JsonIntegerOrZero(value.Find("port"));
    if (port <= 0 || port > 65535) {
        throw std::runtime_error("Signaling response candidate port is out of range");
    }
    candidate.port = static_cast<uint16_t>(port);
    ValidateSignalingCandidate(candidate);
    return candidate;
}

SignalingPeer ParsePeer(const JsonValue& value)
{
    if (value.type != JsonType::Object) {
        throw std::runtime_error("Signaling response peer is not an object");
    }
    SignalingPeer peer;
    peer.peerId = JsonStringOrEmpty(value.Find("peerId"));
    ValidateSignalingPeerId(peer.peerId);
    peer.lastSeen = JsonIntegerOrZero(value.Find("lastSeen"));

    const JsonValue* metadata = value.Find("metadata");
    if (metadata != nullptr && metadata->type == JsonType::Object) {
        peer.metadata.name = JsonStringOrEmpty(metadata->Find("name"));
        peer.metadata.platform = JsonStringOrEmpty(metadata->Find("platform"));
    }

    const JsonValue* candidates = value.Find("candidates");
    if (candidates != nullptr && candidates->type == JsonType::Array) {
        for (const auto& candidate : candidates->arrayValue) {
            peer.candidates.push_back(ParseCandidate(candidate));
        }
    }
    return peer;
}

SignalingRoomResponse ParseRoomResponse(const std::string& text)
{
    const JsonValue root = JsonParser(text).Parse();
    if (root.type != JsonType::Object) {
        throw std::runtime_error("Signaling response root is not an object");
    }
    SignalingRoomResponse response;
    const JsonValue* ok = root.Find("ok");
    response.ok = ok != nullptr && ok->type == JsonType::Bool && ok->boolValue;
    response.roomAccessKey = JsonStringOrEmpty(root.Find("roomAccessKey"));
    response.roomName = JsonStringOrEmpty(root.Find("roomName"));
    response.passwordProtected =
        JsonBoolOrFalse(root.Find("passwordProtected")) ||
        JsonBoolOrFalse(root.Find("requiresRoomKey"));
    if (response.roomName.size() > 80) {
        throw std::runtime_error("Signaling response room name is invalid");
    }
    if (!response.roomAccessKey.empty() &&
        (response.roomAccessKey.size() < 8 ||
         response.roomAccessKey.size() > 128 ||
         !std::all_of(response.roomAccessKey.begin(), response.roomAccessKey.end(), [](char ch) {
             return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-';
         }))) {
        throw std::runtime_error("Signaling response room access key is invalid");
    }
    const JsonValue* peers = root.Find("peers");
    if (peers != nullptr && peers->type == JsonType::Array) {
        for (const auto& peer : peers->arrayValue) {
            response.peers.push_back(ParsePeer(peer));
        }
    }
    return response;
}

void ThrowIfErrorResponse(const std::string& body)
{
    std::string errorText;
    try {
        const JsonValue root = JsonParser(body).Parse();
        if (root.type != JsonType::Object) {
            return;
        }
        const std::string code = JsonStringOrEmpty(root.Find("error"));
        const std::string message = JsonStringOrEmpty(root.Find("message"));
        if (!code.empty() || !message.empty()) {
            errorText =
                "Signaling server error" +
                (code.empty() ? std::string{} : " " + code) +
                (message.empty() ? std::string{} : ": " + message);
        }
    } catch (...) {
        return;
    }
    if (!errorText.empty()) {
        throw std::runtime_error(errorText);
    }
}

} // namespace

std::string SignalingJsonEscape(const std::string& text)
{
    std::string output;
    output.reserve(text.size() + 8);
    for (const unsigned char ch : text) {
        switch (ch) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (ch < 0x20) {
                char escaped[7]{};
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", ch);
                output += escaped;
            } else {
                output.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    return output;
}

void ValidateSignalingRoomId(const std::string& roomId)
{
    if (roomId.size() < 3 || roomId.size() > 96) {
        throw std::invalid_argument("Signaling room ID must be 3-96 characters");
    }
    for (const char ch : roomId) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_' && ch != '-') {
            throw std::invalid_argument("Signaling room ID may contain only letters, numbers, underscore, or dash");
        }
    }
}

void ValidateSignalingPeerId(const std::string& peerId)
{
    if (peerId.empty() || peerId.size() > 96) {
        throw std::invalid_argument("Signaling peer ID must be 1-96 characters");
    }
    for (const char ch : peerId) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_' && ch != '-') {
            throw std::invalid_argument("Signaling peer ID may contain only letters, numbers, underscore, or dash");
        }
    }
}

void ValidateSignalingCandidate(const SignalingCandidate& candidate)
{
    if (candidate.type != "srflx" && candidate.type != "host") {
        throw std::invalid_argument("Signaling candidate type must be srflx or host");
    }
    if (candidate.protocol != "udp") {
        throw std::invalid_argument("Signaling candidate protocol must be udp");
    }
    if (candidate.port == 0) {
        throw std::invalid_argument("Signaling candidate port must be 1-65535");
    }
    if (candidate.ip.empty() || candidate.ip.size() > 45 || (!IsValidIpv4(candidate.ip) && !IsLikelyIpv6(candidate.ip))) {
        throw std::invalid_argument("Signaling candidate IP must be an IPv4 or IPv6 address");
    }
}

SignalingClient::SignalingClient(SignalingClientConfig config)
    : config_(std::move(config)),
      url_(ParseServerUrl(config_.serverUrl))
{
    if (config_.timeout.count() < 100 || config_.timeout.count() > 30000) {
        throw std::invalid_argument("Signaling timeout must be between 100 and 30000 ms");
    }
}

void SignalingClient::Health()
{
    const std::string body = Request(L"GET", "/health", std::nullopt);
    const JsonValue root = JsonParser(body).Parse();
    const JsonValue* ok = root.Find("ok");
    if (ok == nullptr || ok->type != JsonType::Bool || !ok->boolValue) {
        throw std::runtime_error("Signaling health response was not ok");
    }
}

SignalingRoomResponse SignalingClient::Join(const std::string& roomId, const SignalingPeerState& peer)
{
    ValidateSignalingRoomId(roomId);
    ValidateSignalingPeerId(peer.peerId);
    if (peer.candidates.empty()) {
        throw std::invalid_argument("Signaling join requires at least one candidate");
    }
    for (const auto& candidate : peer.candidates) {
        ValidateSignalingCandidate(candidate);
    }
    ValidateSignalingRoomName(peer.roomName);
    ValidateSignalingRoomPassword(peer.roomPassword);
    return ParseRoomResponse(Request(L"POST", RoomPath(roomId, "join"), JoinRequestJson(peer)));
}

SignalingRoomResponse SignalingClient::Peers(
    const std::string& roomId,
    const std::string& peerId,
    const std::string& roomPassword)
{
    ValidateSignalingRoomId(roomId);
    ValidateSignalingPeerId(peerId);
    ValidateSignalingRoomPassword(roomPassword);
    std::string path = RoomPath(roomId, "peers") + "?peerId=" + UrlEncode(peerId);
    if (!roomPassword.empty()) {
        path += "&roomPassword=" + UrlEncode(roomPassword);
    }
    return ParseRoomResponse(Request(
        L"GET",
        path,
        std::nullopt,
        RoomPasswordHeader(roomPassword)));
}

void SignalingClient::ListenRoomEvents(
    const std::string& roomId,
    const std::string& peerId,
    const std::string& roomPassword,
    const std::function<void(const std::string&)>& onEvent,
    const std::function<bool()>& shouldStop)
{
    ValidateSignalingRoomId(roomId);
    ValidateSignalingPeerId(peerId);
    ValidateSignalingRoomPassword(roomPassword);

    const WinHttpHandle session(WinHttpOpen(
        L"ScreenShare/0.1",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (!session) {
        throw std::runtime_error(WinHttpErrorMessage("WinHttpOpen"));
    }

    const int timeoutMs = static_cast<int>(config_.timeout.count());
    if (!WinHttpSetTimeouts(session.get(), timeoutMs, timeoutMs, timeoutMs, timeoutMs)) {
        throw std::runtime_error(WinHttpErrorMessage("WinHttpSetTimeouts"));
    }

    const WinHttpHandle connection(WinHttpConnect(
        session.get(),
        url_.host.c_str(),
        static_cast<INTERNET_PORT>(url_.port),
        0));
    if (!connection) {
        throw std::runtime_error(WinHttpErrorMessage("WinHttpConnect"));
    }

    const std::wstring path = url_.pathPrefix + Utf8ToWide(RoomEventsPath(roomId, peerId, roomPassword));
    const WinHttpHandle request(WinHttpOpenRequest(
        connection.get(),
        L"GET",
        path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        url_.secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request) {
        throw std::runtime_error(WinHttpErrorMessage("WinHttpOpenRequest"));
    }

    if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
        throw std::runtime_error(WinHttpErrorMessage("WinHttpSetOption(websocket)"));
    }
    const std::wstring extraHeaders = RoomPasswordHeader(roomPassword);
    const wchar_t* headers = extraHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : extraHeaders.c_str();
    const DWORD headerBytes = extraHeaders.empty() ? 0 : static_cast<DWORD>(-1);
    if (!WinHttpSendRequest(
            request.get(),
            headers,
            headerBytes,
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0)) {
        throw std::runtime_error(WinHttpErrorMessage("WinHttpSendRequest"));
    }
    if (!WinHttpReceiveResponse(request.get(), nullptr)) {
        throw std::runtime_error(WinHttpErrorMessage("WinHttpReceiveResponse"));
    }

    DWORD statusCode = 0;
    DWORD statusCodeBytes = sizeof(statusCode);
    if (!WinHttpQueryHeaders(
            request.get(),
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &statusCode,
            &statusCodeBytes,
            WINHTTP_NO_HEADER_INDEX)) {
        throw std::runtime_error(WinHttpErrorMessage("WinHttpQueryHeaders(status)"));
    }
    if (statusCode != 101) {
        const std::string body = ReadResponseBody(request.get());
        ThrowIfErrorResponse(body);
        throw std::runtime_error(
            "Signaling room events returned HTTP " + std::to_string(statusCode) +
            (body.empty() ? std::string{} : ": " + body));
    }

    const WinHttpHandle websocket(WinHttpWebSocketCompleteUpgrade(request.get(), 0));
    if (!websocket) {
        throw std::runtime_error(WinHttpErrorMessage("WinHttpWebSocketCompleteUpgrade"));
    }

    std::vector<char> buffer(16 * 1024);
    std::string message;
    while (!shouldStop()) {
        DWORD bytesRead = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE bufferType = WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
        const DWORD result = WinHttpWebSocketReceive(
            websocket.get(),
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &bytesRead,
            &bufferType);
        if (result == ERROR_WINHTTP_TIMEOUT) {
            continue;
        }
        if (result != NO_ERROR) {
            throw std::runtime_error(WindowsErrorMessage("WinHttpWebSocketReceive", result));
        }
        if (bufferType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            break;
        }
        if (bufferType != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE &&
            bufferType != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) {
            continue;
        }

        message.append(buffer.data(), buffer.data() + bytesRead);
        if (bufferType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
            if (!message.empty()) {
                onEvent(message);
            }
            message.clear();
        }
    }

    WinHttpWebSocketClose(
        websocket.get(),
        WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,
        nullptr,
        0);
}

void SignalingClient::Heartbeat(const std::string& roomId, const std::string& peerId)
{
    ValidateSignalingRoomId(roomId);
    ValidateSignalingPeerId(peerId);
    const std::string body = Request(L"POST", RoomPath(roomId, "heartbeat"), PeerRequestJson(peerId));
    const JsonValue root = JsonParser(body).Parse();
    const JsonValue* ok = root.Find("ok");
    if (ok == nullptr || ok->type != JsonType::Bool || !ok->boolValue) {
        throw std::runtime_error("Signaling heartbeat response was not ok");
    }
}

void SignalingClient::Leave(const std::string& roomId, const std::string& peerId)
{
    ValidateSignalingRoomId(roomId);
    ValidateSignalingPeerId(peerId);
    const std::string body = Request(L"POST", RoomPath(roomId, "leave"), PeerRequestJson(peerId));
    const JsonValue root = JsonParser(body).Parse();
    const JsonValue* ok = root.Find("ok");
    if (ok == nullptr || ok->type != JsonType::Bool || !ok->boolValue) {
        throw std::runtime_error("Signaling leave response was not ok");
    }
}

std::string SignalingClient::Request(
    const wchar_t* method,
    const std::string& pathAndQuery,
    const std::optional<std::string>& jsonBody,
    const std::wstring& extraHeaders)
{
    const WinHttpHandle session(WinHttpOpen(
        L"ScreenShare/0.1",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (!session) {
        throw std::runtime_error(WinHttpErrorMessage("WinHttpOpen"));
    }

    const int timeoutMs = static_cast<int>(config_.timeout.count());
    if (!WinHttpSetTimeouts(session.get(), timeoutMs, timeoutMs, timeoutMs, timeoutMs)) {
        throw std::runtime_error(WinHttpErrorMessage("WinHttpSetTimeouts"));
    }

    const WinHttpHandle connection(WinHttpConnect(
        session.get(),
        url_.host.c_str(),
        static_cast<INTERNET_PORT>(url_.port),
        0));
    if (!connection) {
        throw std::runtime_error(WinHttpErrorMessage("WinHttpConnect"));
    }

    std::wstring path = url_.pathPrefix + Utf8ToWide(pathAndQuery);
    const WinHttpHandle request(WinHttpOpenRequest(
        connection.get(),
        method,
        path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        url_.secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request) {
        throw std::runtime_error(WinHttpErrorMessage("WinHttpOpenRequest"));
    }

    std::wstring headers;
    if (jsonBody) {
        headers = L"Content-Type: application/json\r\nAccept: application/json\r\n";
    } else {
        headers = L"Accept: application/json\r\n";
    }
    headers += extraHeaders;

    void* bodyPointer = WINHTTP_NO_REQUEST_DATA;
    DWORD bodyBytes = 0;
    if (jsonBody) {
        bodyPointer = const_cast<char*>(jsonBody->data());
        bodyBytes = static_cast<DWORD>(jsonBody->size());
    }

    if (!WinHttpSendRequest(
            request.get(),
            headers.c_str(),
            static_cast<DWORD>(-1),
            bodyPointer,
            bodyBytes,
            bodyBytes,
            0)) {
        throw std::runtime_error(WinHttpErrorMessage("WinHttpSendRequest"));
    }
    if (!WinHttpReceiveResponse(request.get(), nullptr)) {
        throw std::runtime_error(WinHttpErrorMessage("WinHttpReceiveResponse"));
    }

    DWORD statusCode = 0;
    DWORD statusCodeBytes = sizeof(statusCode);
    if (!WinHttpQueryHeaders(
            request.get(),
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &statusCode,
            &statusCodeBytes,
            WINHTTP_NO_HEADER_INDEX)) {
        throw std::runtime_error(WinHttpErrorMessage("WinHttpQueryHeaders(status)"));
    }

    const std::string body = ReadResponseBody(request.get());

    if (statusCode < 200 || statusCode >= 300) {
        ThrowIfErrorResponse(body);
        throw std::runtime_error(
            "Signaling server returned HTTP " + std::to_string(statusCode) +
            (body.empty() ? std::string{} : ": " + body));
    }

    return body;
}

} // namespace screenshare
