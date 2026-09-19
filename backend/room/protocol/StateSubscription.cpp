#include "StateSubscription.h"
#include "RoomProtocol.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <utility>

namespace screenshare::room::wire {
std::uint64_t StateSubscription::Start(QString roomId, QString selfPeerId) {
    generation_ = tracker_.Start(); active_ = true; payload_ = {};
    roomId_ = std::move(roomId); selfPeerId_ = std::move(selfPeerId);
    return generation_;
}
void StateSubscription::Stop() {
    tracker_.Stop(); active_ = false; payload_ = {}; roomId_.clear(); selfPeerId_.clear();
}
bool StateSubscription::ApplyDelta(QJsonObject& next, const QJsonObject& delta) const {
    const auto op = delta["op"].toString();
    if (directory_) {
        auto rooms = next["rooms"].toArray();
        auto room = delta["room"].toObject();
        auto id = op == "upsert" ? room["roomId"] : delta["roomId"];
        qsizetype index = -1;
        for (qsizetype i = 0; i < rooms.size(); ++i) if (rooms[i].toObject()["roomId"] == id) { index = i; break; }
        if (op == "remove") { if (index < 0) return false; rooms.removeAt(index); }
        else if (index < 0) rooms.append(room);
        else {
            if (room["summaryVersion"].toDouble() <= rooms[index].toObject()["summaryVersion"].toDouble()) return false;
            rooms[index] = room;
        }
        next["rooms"] = rooms;
    } else if (op == "policy") next["policy"] = delta["policy"];
    else {
        auto members = next["members"].toArray();
        if (op == "host.status") {
            next["status"] = delta["status"];
            for (qsizetype i = 0; i < members.size(); ++i) {
                auto m = members[i].toObject();
                if (m["role"] == "host") { m["status"] = delta["status"] == "open" ? "connected" : "reconnecting"; members[i] = m; }
            }
        } else {
            auto member = delta["member"].toObject();
            auto id = op == "member.upsert" ? member["peerId"] : delta["peerId"];
            qsizetype index = -1;
            for (qsizetype i = 0; i < members.size(); ++i) if (members[i].toObject()["peerId"] == id) { index = i; break; }
            if (op == "member.remove") { if (index < 0) return false; members.removeAt(index); }
            else if (index < 0) members.append(member);
            else {
                if (member["role"] != members[index].toObject()["role"]) return false;
                members[index] = member;
            }
        }
        next["members"] = members;
    }
    return true;
}
StateSubscription::Result StateSubscription::Receive(std::uint64_t generation, const QByteArray& bytes) {
    if (!active_ || generation != generation_) return Result::Ignore;
    auto decoded = ValidateServerEvent(bytes, directory_);
    if (!decoded.ok) return Result::Invalid;
    const auto m = decoded.message;
    if (!directory_ && m["roomId"] != roomId_) return Result::Invalid;
    const auto type = m["type"].toString();
    if (type == "room.closed") { Stop(); return Result::Closed; }
    if (type != "state.snapshot" && type != "state.delta") return Result::Ignore;
    auto candidate = tracker_;
    const auto revision = static_cast<std::uint64_t>(m["revision"].toDouble());
    auto decision = type == "state.snapshot" ? candidate.Snapshot(generation, revision) : candidate.Delta(generation, revision);
    if (decision == RevisionTracker::Decision::Ignore) return Result::Ignore;
    if (decision == RevisionTracker::Decision::Resync) { tracker_ = candidate; return Result::Resync; }
    auto next = type == "state.snapshot" ? m["payload"].toObject() : payload_;
    bool valid = true;
    if (type == "state.delta") valid = ApplyDelta(next, m["payload"].toObject());
    QJsonObject snapshot{{"v", 2}, {"type", "state.snapshot"}, {"revision", static_cast<double>(revision)}, {"payload", next}};
    if (!directory_) snapshot["roomId"] = roomId_;
    valid = valid && ValidateServerEvent(QJsonDocument(snapshot).toJson(QJsonDocument::Compact), directory_).ok &&
        (directory_ || next["selfPeerId"] == selfPeerId_);
    if (!valid) {
        if (type == "state.snapshot") return Result::Invalid;
        return tracker_.RequestResync(generation) == RevisionTracker::Decision::Resync ? Result::Resync : Result::Ignore;
    }
    tracker_ = candidate; payload_ = next;
    return Result::Applied;
}
}
