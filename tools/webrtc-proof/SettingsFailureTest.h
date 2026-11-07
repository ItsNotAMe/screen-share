#pragma once
#include "media/webrtc/ViewerStreamSettings.h"
#include "api/make_ref_counted.h"
#include <stdexcept>

namespace settings_test {
// The boundary that can reject a validated update is WebRTC SetParameters.
// Keep that boundary controllable without production-only fault injection.
class Sender : public webrtc::RtpSenderInterface {
public:
    Sender() { parameters.encodings.resize(1); }
    webrtc::RtpParameters parameters;
    bool reject = false;
    unsigned failuresRemaining = 0;
    unsigned writes = 0;
    mutable unsigned reads = 0;
    bool SetTrack(webrtc::MediaStreamTrackInterface*) override { return false; }
    webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track() const override { return nullptr; }
    webrtc::scoped_refptr<webrtc::DtlsTransportInterface> dtls_transport() const override { return nullptr; }
    uint32_t ssrc() const override { return 0; }
    webrtc::MediaType media_type() const override { return webrtc::MediaType::VIDEO; }
    std::string id() const override { return "settings-test"; }
    std::vector<std::string> stream_ids() const override { return {}; }
    void SetStreams(const std::vector<std::string>&) override {}
    std::vector<webrtc::RtpEncodingParameters> init_send_encodings() const override { return parameters.encodings; }
    webrtc::RtpParameters GetParameters() const override { ++reads; return parameters; }
    webrtc::RTCError SetParameters(const webrtc::RtpParameters& value) override {
        ++writes;
        if (failuresRemaining) { --failuresRemaining; return webrtc::RTCError(webrtc::RTCErrorType::INVALID_MODIFICATION, "temporary test rejection"); }
        if (reject) return webrtc::RTCError(webrtc::RTCErrorType::INVALID_MODIFICATION, "test rejection");
        parameters = value; return webrtc::RTCError::OK();
    }
    webrtc::scoped_refptr<webrtc::DtmfSenderInterface> GetDtmfSender() const override { return nullptr; }
    void SetFrameEncryptor(webrtc::scoped_refptr<webrtc::FrameEncryptorInterface>) override {}
    webrtc::scoped_refptr<webrtc::FrameEncryptorInterface> GetFrameEncryptor() const override { return nullptr; }
};
inline void Recovery() {
    using namespace screenshare::media;
    using namespace std::chrono_literals;
    auto check = [](bool ok) { if (!ok) throw std::runtime_error("Settings retry recovery failed"); };
    auto sender = webrtc::make_ref_counted<Sender>(), other = webrtc::make_ref_counted<Sender>();
    auto source = webrtc::make_ref_counted<CaptureVideoSource>(), otherSource = webrtc::make_ref_counted<CaptureVideoSource>();
    ViewerStreamSettings settings, otherSettings;
    const auto start = ViewerStreamSettings::Clock::time_point{};
    StreamPreferences live;
    auto paused = live; paused.videoPaused = true;
    check(settings.Update(*sender, *source, live, 1, start, 1000000) == SettingsApplyError::None);
    check(settings.Update(*sender, *source, paused, 2, start, 0) == SettingsApplyError::None);
    sender->failuresRemaining = 1;
    check(settings.Update(*sender, *source, live, 3, start, 1000000) == SettingsApplyError::SenderRejected);
    check(settings.pending() && settings.revision() == 2 && source->requestedRevision() == 2 &&
        !sender->parameters.encodings[0].active && settings.appliedVideoBitrateBps() == 0);
    // Another viewer continues independently while this resume is pending.
    check(otherSettings.Update(*other, *otherSource, live, 3, start, 1000000) == SettingsApplyError::None);
    const auto writes = sender->writes, reads = sender->reads;
    check(!settings.Update(*sender, *source, live, 3, start + 249ms, 1000000));
    check(sender->writes == writes && sender->reads == reads);
    // The next attempt must obtain fresh parameters, including a new transaction.
    sender->parameters.transaction_id = "renewed-transaction";
    check(settings.Update(*sender, *source, live, 3, start + 250ms, 1000000) == SettingsApplyError::None);
    check(sender->reads == reads + 1 && sender->parameters.transaction_id == "renewed-transaction" &&
        !settings.pending() && settings.revision() == 3 && source->requestedRevision() == 3 && sender->parameters.encodings[0].active);
    check(!settings.Update(*sender, *source, live, 3, start + 10s, 1000000));

    // A new pause supersedes a rejected change without waiting for its retry.
    sender->reject = true;
    check(settings.Update(*sender, *source, live, 4, start + 11s, 2000000) == SettingsApplyError::SenderRejected);
    sender->reject = false;
    check(settings.Update(*sender, *source, paused, 5, start + 11001ms, 0) == SettingsApplyError::None);
    check(!settings.pending() && !sender->parameters.encodings[0].active && settings.revision() == 5);
    check(!settings.Update(*sender, *source, paused, 5, start + 12s, 0));

    // A permanently rejected resume retains the last applied pause and stops.
    sender->reject = true;
    const auto beforePermanent = sender->writes;
    for (int attempt = 0; attempt < 3; ++attempt)
        check(settings.Update(*sender, *source, live, 6, start + 13s + attempt * 250ms, 1000000) == SettingsApplyError::SenderRejected);
    check(!settings.pending() && settings.revision() == 5 && source->requestedRevision() == 5 && !sender->parameters.encodings[0].active);
    for (int tick = 0; tick < 100; ++tick)
        check(!settings.Update(*sender, *source, live, 6, start + 14s + tick * 1s, 1000000));
    check(sender->writes == beforePermanent + 3);
    sender->reject = false;
    // Structural/validation errors do not enter retry backoff.
    auto invalid = live; invalid.width = 161; invalid.resolution = ResolutionMode::Fixed;
    check(settings.Update(*sender, *source, invalid, 7, start + 120s) == SettingsApplyError::Invalid && !settings.pending());
    check(!settings.Update(*sender, *source, invalid, 7, start + 121s));
    sender->parameters.encodings.clear();
    check(settings.Update(*sender, *source, live, 8, start + 122s) == SettingsApplyError::UnsupportedTopology && !settings.pending());
    check(!settings.Update(*sender, *source, live, 8, start + 123s));
    sender->parameters.encodings.resize(1);
    check(settings.Update(*sender, *source, live, 5, start + 124s) == SettingsApplyError::StaleRevision && !settings.pending());
    check(settings.Update(*sender, *source, live, 0, start + 125s) == SettingsApplyError::StaleRevision);
    check(!settings.pending());

    // Initial negotiation can also recover, without claiming an applied source
    // revision or delivering frames before the sender accepts the contract.
    ViewerStreamSettings initial;
    auto initialSource = webrtc::make_ref_counted<CaptureVideoSource>();
    sender->failuresRemaining = 2;
    check(initial.Update(*sender, *initialSource, live, 1, start, 1000000) == SettingsApplyError::SenderRejected);
    check(initial.pending() && initial.revision() == 0 && initialSource->requestedRevision() == 0);
    check(initial.Update(*sender, *initialSource, live, 1, start + 250ms, 1000000) == SettingsApplyError::SenderRejected);
    check(initial.Update(*sender, *initialSource, live, 1, start + 500ms, 1000000) == SettingsApplyError::None);
    check(!initial.pending() && initial.revision() == 1 && initialSource->requestedRevision() == 1);
}
inline void Run() {
    Recovery();
    using namespace screenshare::media;
    auto check = [](bool ok) { if (!ok) throw std::runtime_error("Settings rejection isolation failed"); };
    auto first = webrtc::make_ref_counted<Sender>(), second = webrtc::make_ref_counted<Sender>();
    auto source = webrtc::make_ref_counted<CaptureVideoSource>(), other = webrtc::make_ref_counted<CaptureVideoSource>();
    ViewerStreamSettings settings, otherSettings;
    StreamPreferences original;
    original.resolution = ResolutionMode::Fixed; original.width = 320; original.height = 180;
    original.fpsMode = SettingMode::Manual; original.fps = 60;
    original.bitrateMode = SettingMode::Manual; original.bitrateLimitBps = 4000000;
    check(settings.Apply(*first, *source, original, 1) == SettingsApplyError::None);
    check(otherSettings.Apply(*second, *other, original, 1) == SettingsApplyError::None);
    auto changed = original; changed.width = 160; changed.height = 90; changed.bitrateLimitBps = 2000000;
    first->reject = true;
    check(settings.Apply(*first, *source, changed, 2) == SettingsApplyError::SenderRejected);
    check(otherSettings.Apply(*second, *other, changed, 2) == SettingsApplyError::None);
    check(source->requestedRevision() == 1 && settings.revision() == 1 && settings.preferences()->width == 320 &&
        settings.appliedVideoBitrateBps() == 4000000 && first->parameters.encodings[0].max_bitrate_bps == 4000000);
    check(other->requestedRevision() == 2 && otherSettings.preferences()->width == 160 &&
        otherSettings.appliedVideoBitrateBps() == 2000000);
    const auto writes = first->writes;
    auto invalid = changed; invalid.width = 161;
    check(settings.Apply(*first, *source, invalid, 3) == SettingsApplyError::Invalid);
    check(settings.Apply(*first, *source, changed, 1) == SettingsApplyError::StaleRevision);
    first->parameters.encodings.clear();
    check(settings.Apply(*first, *source, changed, 3) == SettingsApplyError::UnsupportedTopology);
    check(first->writes == writes && source->requestedRevision() == 1);
    first->parameters.encodings.resize(1); first->reject = false;
    changed.preset = StreamPreset::Quality;
    check(settings.Apply(*first, *source, changed, 3) == SettingsApplyError::None);
    check(settings.preferences()->resolution == ResolutionMode::Fixed && settings.preferences()->fpsMode == SettingMode::Manual &&
        settings.preferences()->fps == 60 && settings.preferences()->bitrateMode == SettingMode::Manual &&
        first->parameters.degradation_preference == webrtc::DegradationPreference::MAINTAIN_FRAMERATE_AND_RESOLUTION &&
        !first->parameters.encodings[0].min_bitrate_bps);
    check(settings.Apply(*first, *source, changed, 4, 0) == SettingsApplyError::None &&
        !first->parameters.encodings[0].active && settings.appliedVideoBitrateBps() == 0);
    check(settings.Apply(*first, *source, changed, 5, 1000000) == SettingsApplyError::None &&
        first->parameters.encodings[0].active && settings.preferences()->bitrateLimitBps == 2000000);
}
}
