#pragma once
#include "RevisionTracker.h"
#include <QJsonObject>
#include <QByteArray>
#include <QString>

namespace screenshare::room::wire {
// Networking-thread state cache. Returned payloads use Qt implicit sharing;
// callers cannot mutate the accepted cache through their copy.
class StateSubscription {
public:
    enum class Result { Ignore, Applied, Resync, Invalid, Closed };
    explicit StateSubscription(bool directory = false) : directory_(directory) {}
    std::uint64_t Start(QString roomId = {}, QString selfPeerId = {});
    void Stop();
    Result Receive(std::uint64_t generation, const QByteArray& bytes);
    QJsonObject Payload() const { return payload_; }
    std::optional<std::uint64_t> Revision() const { return tracker_.Revision(); }
private:
    bool ApplyDelta(QJsonObject& next, const QJsonObject& delta) const;
    bool directory_;
    bool active_ = false;
    std::uint64_t generation_ = 0;
    QString roomId_, selfPeerId_;
    RevisionTracker tracker_;
    QJsonObject payload_;
};
}
