#include "ui/QtRoomSession.h"
#include "input/v2/GamepadSink.h"
using namespace screenshare::v2;
using namespace std::chrono_literals;

QtRoomSession::QtRoomSession(QObject* parent, Factory factory, bool loopback)
    : QObject(parent), factory_(std::move(factory)), loopback_(loopback) {
    timer_.setInterval(5); timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_, &QTimer::timeout, this, [this] {
        try { tick(); }
        catch (...) { if (error) error(QStringLiteral("Room media operation failed.")); stop(); }
    });
}
QtRoomSession::~QtRoomSession() { timer_.stop(); session_.reset(); }
bool QtRoomSession::prepareInputGrant(uint8_t capabilities) {
    if(loopback_ || !(capabilities&screenshare::input::Mouse))return true;
    const auto selected=status().capture.selected;
    if(selected.kind!=screenshare::media::CaptureKind::Window)return true;
    const auto window=reinterpret_cast<HWND>(selected.window);
    // This follows the host's explicit Grant click. Never restore a minimized or
    // hidden source, and never try to bypass foreground restrictions.
    return IsWindowVisible(window) && !IsIconic(window) &&
        (GetForegroundWindow()==GetAncestor(window,GA_ROOT) || SetForegroundWindow(window));
}
bool QtRoomSession::start(RoomSessionConfig config) {
    if (session_) return false;
    try {
        frames_ = std::make_shared<LatestRoomVideoFrame>();
        lastFrames_ = {};
        config.media.frames = frames_;
        presentation_ = std::make_shared<screenshare::media::PresentationTelemetry>();
        config.media.presentation = presentation_;
        // Devices are created only after explicit host consent. Diagnostics never
        // substitute a physical sink for a missing recording implementation.
        if (config.room.host && !loopback_ && !config.media.inputSink)
            config.media.enableDesktopInput = true;
        auto factory = factory_(config.media);
        session_ = std::make_unique<RoomSession>(std::move(factory), loopback_);
        config_ = std::move(config);
        admission_ = session_->Start(config_.room);
        pending_.reset(); submitted_.reset(); applying_ = {}; stopping_ = {};
        nextChange_ = nextCaptureChange_ = nextAudioChange_ = nextPlaybackChange_ = 0; started_ = nextStatus_ = std::chrono::steady_clock::now();
        last_ = session_->Status(); timer_.start(); return true;
    } catch (...) {
        session_.reset(); frames_.reset();
        if (error) error(QStringLiteral("Could not start the room session."));
        return false;
    }
}
void QtRoomSession::stop() {
    if (frames_) frames_->Stop();
    pending_.reset();
    if (session_ && !stopping_.valid()) stopping_ = session_->Stop();
}
void QtRoomSession::apply(screenshare::media::StreamPreferences preferences) {
    if (!session_ || stopping_.valid() || !config_.room.host) return;
    try { screenshare::media::ValidateStreamPreferences(preferences); }
    catch (...) { if (error) error(QStringLiteral("Invalid stream settings.")); return; }
    pending_ = preferences; // Replace superseded UI edits; never queue an edit storm.
}
RoomStatus QtRoomSession::status() const { return session_ ? session_->Status() : last_; }
void QtRoomSession::switchCapture(screenshare::media::CaptureSelection selection) {
    if (session_ && !stopping_.valid() && !captureUpdate_.valid()) captureUpdate_ = session_->SwitchCaptureSource(selection);
}
void QtRoomSession::switchAudio(screenshare::media::AudioSelection selection) {
    if (session_ && !stopping_.valid() && !audioUpdate_.valid()) audioUpdate_ = session_->SwitchAudioSource(std::move(selection));
}
void QtRoomSession::updatePlayback(screenshare::media::PlaybackSelection selection) {
    if (session_ && !stopping_.valid() && !playbackUpdate_.valid()) playbackUpdate_ = session_->UpdatePlayback(std::move(selection));
}
void QtRoomSession::updateNickname(std::string nickname, uint64_t revision) {
    if (session_ && !stopping_.valid() && !mutation_.valid()) mutation_ = session_->UpdateNickname(std::move(nickname), revision);
}
void QtRoomSession::updatePolicy(RoomPolicy policy, uint64_t revision) {
    if (session_ && !stopping_.valid() && !mutation_.valid()) mutation_ = session_->UpdateRoomPolicy(std::move(policy), revision);
}
void QtRoomSession::tick() {
    if (!session_) return;
    const auto now = std::chrono::steady_clock::now();
    if (admission_.valid() && admission_.wait_for(0ms) == std::future_status::ready) {
        const auto result = admission_.get();
        if (result.error != RoomError::None && (result.error != RoomError::Cancelled || result.outcomeUnconfirmed)) {
            if (error) error(result.outcomeUnconfirmed ? QStringLiteral("Admission ended before the server outcome could be confirmed.") :
                             QStringLiteral("Could not enter the room. Check the service, room and password."));
        }
    }
    if (config_.duration.count() && now - started_ >= config_.duration) stop();
    const auto current = session_->Status();
    if (playbackUpdate_.valid() && playbackUpdate_.wait_for(0ms) == std::future_status::ready) {
        const auto result = playbackUpdate_.get(); if (playbackUpdated) playbackUpdated(result);
    }
    if (audioUpdate_.valid() && audioUpdate_.wait_for(0ms) == std::future_status::ready) {
        const auto result = audioUpdate_.get(); if (audioUpdated) audioUpdated(result);
    }
    if (captureUpdate_.valid() && captureUpdate_.wait_for(0ms) == std::future_status::ready) {
        const auto result = captureUpdate_.get(); if (captureUpdated) captureUpdated(result);
    }
    if (mutation_.valid() && mutation_.wait_for(0ms) == std::future_status::ready) {
        const auto result = mutation_.get();
        if (roomUpdated) roomUpdated(result);
    }
    if (current.phase == RoomPhase::Failed || current.phase == RoomPhase::Stopped) stop();
    if (applying_.valid() && applying_.wait_for(0ms) == std::future_status::ready) {
        const auto result = applying_.get();
        if ((result.error == StreamUpdateError::Busy || result.error == StreamUpdateError::Unavailable) && !stopping_.valid()) {
            if (!pending_) pending_ = submitted_;
        } else if (settingsAccepted) settingsAccepted(result);
        submitted_.reset();
    }
    if (!stopping_.valid()) {
        if (!playbackUpdate_.valid() && current.phase == RoomPhase::Active && nextPlaybackChange_ < config_.playbackChanges.size() &&
            now - started_ >= config_.playbackChanges[nextPlaybackChange_].at)
            updatePlayback(config_.playbackChanges[nextPlaybackChange_++].selection);
        if (!audioUpdate_.valid() && current.phase == RoomPhase::Active && nextAudioChange_ < config_.audioChanges.size() &&
            now - started_ >= config_.audioChanges[nextAudioChange_].at)
            switchAudio(config_.audioChanges[nextAudioChange_++].selection);
        if (!captureUpdate_.valid() && current.phase == RoomPhase::Active && nextCaptureChange_ < config_.captureChanges.size() &&
            now - started_ >= config_.captureChanges[nextCaptureChange_].at)
            switchCapture(config_.captureChanges[nextCaptureChange_++].selection);
        if (nextChange_ < config_.changes.size() && now - started_ >= config_.changes[nextChange_].at)
            pending_ = config_.changes[nextChange_++].preferences;
        if (pending_ && !applying_.valid() && current.phase == RoomPhase::Active) {
            submitted_ = pending_; pending_.reset(); applying_ = session_->UpdateStreamPreferences(*submitted_);
        }
        if (frameReady) if (auto frame = frames_->Take()) frameReady(std::move(*frame));
    }
    if (now >= nextStatus_) {
        last_ = current; nextStatus_ = now + 100ms;
        if (statusChanged) statusChanged(current);
    }
    if (stopping_.valid() && stopping_.wait_for(0ms) == std::future_status::ready) {
        stopping_.get(); last_ = session_->Status();
        if (playbackUpdate_.valid()) { const auto result = playbackUpdate_.get(); if (playbackUpdated) playbackUpdated(result); }
        if (audioUpdate_.valid()) { const auto result = audioUpdate_.get(); if (audioUpdated) audioUpdated(result); }
        if (captureUpdate_.valid()) { const auto result = captureUpdate_.get(); if (captureUpdated) captureUpdated(result); }
        if (mutation_.valid()) {
            const auto result = mutation_.get();
            if (roomUpdated) roomUpdated(result);
        }
        if (admission_.valid()) {
            const auto result = admission_.get();
            if (result.outcomeUnconfirmed && error) error(QStringLiteral("Admission ended before the server outcome could be confirmed."));
        }
        applying_ = {}; pending_.reset(); submitted_.reset();
        lastFrames_ = frames_->statistics(); session_.reset(); frames_.reset(); timer_.stop();
        config_.media.frames.reset();
        config_.room.password.clear();
        if (statusChanged) statusChanged(last_);
        if (finished) finished(last_);
    }
}
