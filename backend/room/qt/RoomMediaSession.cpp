#include "RoomMediaSession.h"
#include "RoomSignalCodec.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <stdexcept>

namespace screenshare::room::qt {
RoomMediaSession::RoomMediaSession(media::SignalingExecutor& executor, bool host, std::string self,
    media::RoomPeerRoster::Add add, media::RoomPeerRoster::Remove remove, Receive receive,
    media::RoomPeerRoster::Ready ready)
    : executor_(executor), host_(host), self_(std::move(self)), roster_(std::move(add), std::move(remove), std::move(ready)),
      receive_(std::move(receive)) { CheckThread(); }
RoomMediaSession::~RoomMediaSession() { Stop(); }
void RoomMediaSession::CheckThread() const {
    if (!executor_.IsCurrent()) throw std::logic_error("Room media session requires signaling executor");
}
void RoomMediaSession::Fail() {
    roster_.TransportLost(generation_);
    pending_.clear(); bytes_ = 0; state_ = State::Failed;
}
void RoomMediaSession::OnEvent(const RoomSocket::Event& event) {
    CheckThread();
    if (state_ == State::Stopped || state_ == State::Failed || event.generation < generation_) return;
    using Kind = RoomSocket::EventKind;
    if (event.kind != Kind::Snapshot && event.kind != Kind::Signal && event.kind != Kind::Error &&
        event.kind != Kind::Reconnecting && event.kind != Kind::Closed) return;
    const auto bytes = size_t(QJsonDocument(event.value).toJson(QJsonDocument::Compact).size());
    if (pending_.size() >= 256 || bytes > 512 * 1024 - bytes_) { Fail(); return; }
    pending_.push_back({event, bytes}); bytes_ += bytes;
}
void RoomMediaSession::Advance() {
    CheckThread();
    using Kind = RoomSocket::EventKind;
    while (!pending_.empty()) {
        auto next = std::move(pending_.front()); pending_.pop_front(); bytes_ -= next.bytes;
        auto& event = next.event;
        if (!event.generation) {
            if (event.kind == Kind::Error) { Fail(); return; }
            continue;
        }
        if (event.generation < generation_) continue;
        if (event.kind == Kind::Closed) { Stop(); return; }
        if (event.kind == Kind::Error || event.kind == Kind::Reconnecting) {
            generation_ = event.generation;
            roster_.TransportLost(generation_); state_ = State::Suspended;
            continue;
        }
        if (event.kind == Kind::Snapshot) {
            if (!event.revision || !event.value["members"].isArray()) { Fail(); return; }
            std::vector<std::string> peers;
            for (const auto& member : event.value["members"].toArray()) {
                const auto object = member.toObject();
                const auto peer = object["peerId"].toString().toStdString();
                if (peer != self_ && object["status"] == "connected" &&
                    object["role"] == (host_ ? "viewer" : "host")) peers.push_back(peer);
            }
            if ((!host_ && peers.size() > 1) || peers.size() > 63) { Fail(); return; }
            const auto result = roster_.Apply(event.generation, *event.revision, std::move(peers));
            if (result == media::RoomPeerRoster::Result::Invalid) { Fail(); return; }
            if (result == media::RoomPeerRoster::Result::Applied) {
                generation_ = event.generation; state_ = State::Active;
            }
            continue;
        }
        if (state_ != State::Active || event.generation != generation_) continue;
        const auto sender = event.value["fromPeerId"].toString().toStdString();
        if (!roster_.contains(sender)) continue;
        auto signal = DecodeRoomSignal(event.value);
        bool accepted = false;
        try { accepted = signal && receive_(sender, std::move(*signal)); } catch (...) {}
        if (!accepted) roster_.Fail(sender);
    }
    if (state_ == State::Active) roster_.RetryPending();
}
void RoomMediaSession::Stop() {
    CheckThread();
    if (state_ == State::Stopped) return;
    roster_.TransportLost(generation_);
    pending_.clear(); bytes_ = 0; state_ = State::Stopped;
}
RoomMediaSession::Status RoomMediaSession::status() const {
    CheckThread();
    return {state_, generation_, roster_.activeCount(), roster_.failedCount(), roster_.pendingCount(), pending_.size(), bytes_};
}
}
