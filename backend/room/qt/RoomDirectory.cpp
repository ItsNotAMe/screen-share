#include "RoomDirectory.h"
#include <QJsonArray>
using namespace std::chrono_literals;
namespace screenshare::room::qt {
RoomDirectory::RoomDirectory(bool loopback, QObject* parent) : QObject(parent), loopback_(loopback) {
    timer_.setInterval(25);
    connect(&timer_, &QTimer::timeout, this, [this] { Tick(); });
}
RoomDirectory::~RoomDirectory() { timer_.stop(); network_.reset(); }
void RoomDirectory::Publish() { if (changed) changed(status_); }
bool RoomDirectory::Start(QUrl origin) {
    const bool local = loopback_ && origin.scheme() == "http" && (origin.host() == "127.0.0.1" || origin.host() == "::1");
    if (!origin.isValid() || origin.host().isEmpty() || !origin.userInfo().isEmpty() || origin.hasQuery() || origin.hasFragment() ||
        (!origin.path().isEmpty() && origin.path() != "/") || (origin.scheme() != "https" && !local)) {
        Fail(); return false;
    }
    if (desired_ == origin && network_ && !stopping_.valid()) return true;
    desired_ = origin;
    if (network_ && !stopping_.valid()) stopping_ = network_->StopAll();
    status_ = {Phase::Connecting}; reconnects_.clear(); Publish(); timer_.start();
    return true;
}
void RoomDirectory::Stop() {
    if (!running() && status_.phase == Phase::Stopped) return;
    desired_.reset();
    if (network_ && !stopping_.valid()) stopping_ = network_->StopAll();
    status_ = {}; Publish(); timer_.start();
}
void RoomDirectory::Fail() {
    desired_.reset(); status_ = {Phase::Failed};
    if (network_ && !stopping_.valid()) stopping_ = network_->StopAll();
    Publish(); timer_.start();
}
void RoomDirectory::Tick() {
    if (stopping_.valid()) {
        if (stopping_.wait_for(0ms) != std::future_status::ready) return;
        stopping_.get(); stopping_ = {}; opening_ = {}; network_.reset(); generation_ = 0;
        Publish();
    }
    if (!network_) {
        if (!desired_) { timer_.stop(); return; }
        network_ = std::make_unique<RoomNetwork>(loopback_);
        RoomSocket::Config config; config.directory = true; config.origin = *desired_;
        config.origin.setScheme(config.origin.scheme() == "https" ? "wss" : "ws");
        opening_ = network_->Open(++handle_, std::move(config));
    }
    if (opening_.valid() && opening_.wait_for(0ms) == std::future_status::ready && !opening_.get()) { Fail(); return; }
    for (const auto& item : network_->Drain()) {
        if (!item.socket) { Fail(); return; }
        if (item.socket != handle_ || item.value.generation < generation_) continue;
        const auto& event = item.value; generation_ = event.generation;
        switch (event.kind) {
        case RoomSocket::EventKind::Connecting: ++attempts_; break;
        case RoomSocket::EventKind::Snapshot:
            status_.phase = Phase::Ready; status_.revision = event.revision.value_or(0); status_.rooms.clear();
            for (auto value : event.value["rooms"].toArray()) {
                const auto row = value.toObject();
                status_.rooms.push_back({row["roomId"].toString(), row["name"].toString(), row["status"].toString(),
                    row["viewerCount"].toInt(), row["viewerLimit"].toInt(), row["passwordProtected"].toBool(), row["hostNickname"].toString()});
            }
            Publish(); if (stopping_.valid() || !desired_) return; break;
        case RoomSocket::EventKind::Reconnecting: {
            const auto now = std::chrono::steady_clock::now();
            while (!reconnects_.empty() && now - reconnects_.front() >= 1min) reconnects_.pop_front();
            reconnects_.push_back(now);
            if (reconnects_.size() > 3) { Fail(); return; }
            status_.phase = Phase::Reconnecting; Publish(); if (stopping_.valid() || !desired_) return; break;
        }
        case RoomSocket::EventKind::Error:
            if (event.error != RoomSocket::Error::Transport && event.error != RoomSocket::Error::Timeout) { Fail(); return; }
            status_.phase = Phase::Reconnecting; Publish(); if (stopping_.valid() || !desired_) return; break;
        case RoomSocket::EventKind::Closed: Fail(); return;
        default: break;
        }
    }
}
}
