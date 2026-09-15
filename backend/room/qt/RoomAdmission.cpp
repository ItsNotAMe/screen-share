#include "RoomAdmission.h"
#include "room/protocol/RoomProtocol.h"
#include <QJsonDocument>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QThread>
#include <stdexcept>

namespace screenshare::room::qt {
namespace {
QByteArray Bytes(const QJsonObject& value) { return QJsonDocument(value).toJson(QJsonDocument::Compact); }
bool Id(const QString& value) {
    static const QRegularExpression pattern("\\A[A-Za-z0-9_-]{1,128}\\z");
    return pattern.match(value).hasMatch();
}
bool Exact(const QJsonObject& value, QStringList keys) {
    keys.sort(); return value.keys() == keys;
}
}
struct RoomAdmission::Pending {
    uint64_t id;
    QUrl origin;
    QString roomId;
    bool create;
    QByteArray body;
    std::promise<Result> completion;
};
RoomAdmission::RoomAdmission(bool allowPlainLoopback, QObject* parent)
    : QObject(parent), network_(this), deadline_(this), allowPlainLoopback_(allowPlainLoopback) {
    deadline_.setSingleShot(true);
    connect(&deadline_, &QTimer::timeout, this, [this] { if (pending_) Finish(Error::Unconfirmed); });
}
RoomAdmission::~RoomAdmission() { Cancel(); }
void RoomAdmission::CheckThread() const {
    if (QThread::currentThread() != thread()) throw std::logic_error("Admission requires its networking thread");
}
std::future<RoomAdmission::Result> RoomAdmission::Start(Request request) {
    CheckThread();
    auto operation = std::make_unique<Pending>();
    operation->id = ++next_;
    auto future = operation->completion.get_future();
    auto reject = [&](Error error) {
        operation->completion.set_value({operation->id, error, false, {}});
    };
    if (pending_) { reject(Error::Busy); return future; }
    const auto& url = request.origin;
    const bool loopback = url.host() == "127.0.0.1" || url.host() == "::1";
    bool valid = url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty() &&
        !url.hasQuery() && !url.hasFragment() && (url.path().isEmpty() || url.path() == "/") &&
        (url.scheme() == "https" || (allowPlainLoopback_ && loopback && url.scheme() == "http")) &&
        (request.create ? request.roomId.isEmpty() : Id(request.roomId));
    const auto password = request.password.toUtf8();
    valid = valid && password.size() <= 128 && QString::fromUtf8(password) == request.password;
    for (auto c : password) valid = valid && static_cast<unsigned char>(c) >= 0x20 && c != 0x7f;
    // Reuse exactly the socket protocol's canonical name and policy validation.
    auto profile = wire::ValidateClientCommand(Bytes({{"v", 2}, {"type", "profile.update"},
        {"roomId", "admission"}, {"requestId", "admission"},
        {"payload", QJsonObject{{"nickname", request.nickname}, {"expectedRevision", 0}}}}));
    valid = valid && profile.ok;
    QJsonObject body{{"v", 2}, {"nickname", profile.message["payload"].toObject()["nickname"]}, {"password", request.password}};
    if (request.create) {
        auto policy = wire::ValidateClientCommand(Bytes({{"v", 2}, {"type", "room.update"},
            {"roomId", "admission"}, {"requestId", "admission"},
            {"payload", QJsonObject{{"name", request.name}, {"visibility", request.visibility},
                                   {"viewerLimit", request.viewerLimit}, {"expectedRevision", 0}}}}));
        valid = valid && policy.ok;
        auto fields = policy.message["payload"].toObject(); fields.remove("expectedRevision");
        body["policy"] = fields;
    }
    if (!valid) { reject(Error::InvalidRequest); return future; }
    auto endpoint = url;
    endpoint.setPath(request.create ? "/v2/rooms" : "/v2/rooms/" + request.roomId + "/join");
    QNetworkRequest http(endpoint);
    http.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    http.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    http.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    http.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    http.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    http.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    http.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    operation->origin = url; operation->roomId = request.roomId; operation->create = request.create;
    const auto id = operation->id;
    pending_ = std::move(operation);
    reply_ = network_.post(http, Bytes(body));
    reply_->setReadBufferSize(16 * 1024 + 1);
    connect(reply_, &QNetworkReply::readyRead, this, [this, id] { Read(id); });
    connect(reply_, &QNetworkReply::finished, this, [this, id] { Complete(id); });
    deadline_.start(10000);
    return future;
}
void RoomAdmission::Read(uint64_t id) {
    if (!pending_ || pending_->id != id) return;
    pending_->body += reply_->read(16 * 1024 + 1 - pending_->body.size());
    if (pending_->body.size() > 16 * 1024) Finish(Error::Unconfirmed);
}
void RoomAdmission::Complete(uint64_t id) {
    Read(id);
    if (!pending_ || pending_->id != id) return;
    const auto status = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto contentType = reply_->rawHeader("Content-Type").split(';').front().trimmed().toLower();
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(pending_->body, &parse);
    const auto value = doc.object();
    if (contentType != "application/json" || parse.error != QJsonParseError::NoError || !doc.isObject() || value["v"] != 2) {
        Finish(Error::Unconfirmed); return;
    }
    if (status >= 400 && status < 500 && Exact(value, {"v", "error"})) {
        const auto code = value["error"].toString();
        auto error = Error::Unconfirmed;
        if (status == 400 && code == "invalid_request") error = Error::Rejected;
        if (status == 403 && code == "forbidden") error = Error::Forbidden;
        if (status == 404 && code == "not_found") error = Error::NotFound;
        if (status == 409 && code == "full") error = Error::Full;
        if (status == 409 && code == "closed") error = Error::Closed;
        if (status == 429 && code == "rate_limited") error = Error::RateLimited;
        Finish(error); return;
    }
    const auto token = value["token"].toString().toLatin1();
    const auto decoded = QByteArray::fromBase64(token, QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    const bool validToken = decoded.size() == 32 && decoded.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals) == token;
    if (reply_->error() != QNetworkReply::NoError || status != (pending_->create ? 201 : 200) ||
        !Exact(value, {"v", "roomId", "peerId", "role", "token"}) ||
        !Id(value["roomId"].toString()) || !Id(value["peerId"].toString()) || !validToken ||
        value["role"] != (pending_->create ? "host" : "viewer") ||
        (!pending_->create && value["roomId"] != pending_->roomId)) {
        Finish(Error::Unconfirmed); return;
    }
    RoomSocket::Config membership;
    membership.origin = pending_->origin;
    membership.origin.setScheme(membership.origin.scheme() == "https" ? "wss" : "ws");
    membership.roomId = value["roomId"].toString(); membership.selfPeerId = value["peerId"].toString();
    membership.token = token; membership.expectedRole = value["role"].toString();
    Finish(Error::None, std::move(membership));
}
void RoomAdmission::Finish(Error error, std::optional<RoomSocket::Config> membership) {
    auto operation = std::move(pending_);
    deadline_.stop();
    auto* reply = reply_; reply_ = nullptr;
    if (reply) { reply->disconnect(this); reply->abort(); reply->deleteLater(); }
    operation->completion.set_value({operation->id, error, error == Error::Unconfirmed || error == Error::Cancelled, std::move(membership)});
}
void RoomAdmission::Cancel() {
    CheckThread();
    if (pending_) Finish(Error::Cancelled);
}
}
