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
    unsigned writes = 0;
    bool SetTrack(webrtc::MediaStreamTrackInterface*) override { return false; }
    webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track() const override { return nullptr; }
    webrtc::scoped_refptr<webrtc::DtlsTransportInterface> dtls_transport() const override { return nullptr; }
    uint32_t ssrc() const override { return 0; }
    webrtc::MediaType media_type() const override { return webrtc::MediaType::VIDEO; }
    std::string id() const override { return "settings-test"; }
    std::vector<std::string> stream_ids() const override { return {}; }
    void SetStreams(const std::vector<std::string>&) override {}
    std::vector<webrtc::RtpEncodingParameters> init_send_encodings() const override { return parameters.encodings; }
    webrtc::RtpParameters GetParameters() const override { return parameters; }
    webrtc::RTCError SetParameters(const webrtc::RtpParameters& value) override {
        ++writes;
        if (reject) return webrtc::RTCError(webrtc::RTCErrorType::INVALID_MODIFICATION, "test rejection");
        parameters = value; return webrtc::RTCError::OK();
    }
    webrtc::scoped_refptr<webrtc::DtmfSenderInterface> GetDtmfSender() const override { return nullptr; }
    void SetFrameEncryptor(webrtc::scoped_refptr<webrtc::FrameEncryptorInterface>) override {}
    webrtc::scoped_refptr<webrtc::FrameEncryptorInterface> GetFrameEncryptor() const override { return nullptr; }
};
inline void Run() {
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
