#pragma once
#include "RoomSocket.h"
#include <QNetworkAccessManager>
#include <future>
#include <memory>
#include <optional>
class QNetworkReply;

namespace screenshare::room::qt {
// Networking-thread admission; futures may be observed elsewhere. No automatic
// application retries, including ambiguous disconnect/timeout/cancellation.
class RoomAdmission final : public QObject {
public:
    enum class Error { None, InvalidRequest, Busy, Cancelled, Unconfirmed, Forbidden, NotFound, Full, Closed, RateLimited, Rejected };
    struct Request {
        QUrl origin; // HTTPS origin only, without path/query/credentials.
        bool create = false;
        QString roomId, nickname, name, visibility = "public", password;
        int viewerLimit = 4;
    };
    struct Result {
        uint64_t operation = 0;
        Error error = Error::None;
        bool outcomeUnconfirmed = false;
        std::optional<RoomSocket::Config> membership;
    };
    explicit RoomAdmission(bool allowPlainLoopback = false, QObject* parent = nullptr);
    ~RoomAdmission() override;
    std::future<Result> Start(Request);
    void Cancel();
private:
    struct Pending;
    void CheckThread() const;
    void Read(uint64_t);
    void Complete(uint64_t);
    void Finish(Error, std::optional<RoomSocket::Config> = {});
    QNetworkAccessManager network_;
    QTimer deadline_;
    bool allowPlainLoopback_;
    uint64_t next_ = 0;
    QNetworkReply* reply_ = nullptr;
    std::unique_ptr<Pending> pending_;
};
}
