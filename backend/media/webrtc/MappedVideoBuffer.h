#pragma once
#include "input/v2/FrameMapping.h"
#include "media/StreamPreferences.h"
#include "api/video/video_frame_buffer.h"
#include "api/make_ref_counted.h"

namespace screenshare::media {
// Retains the original GPU/CPU buffer without copies. Only our codec boundary
// interprets this wrapper; native texture consumers explicitly unwrap it.
class MappedVideoBuffer : public webrtc::VideoFrameBuffer {
public:
    MappedVideoBuffer(webrtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer, input::FrameMapping mapping,
                     std::optional<StreamPreset> preset = {})
        : buffer(std::move(buffer)), mapping(mapping), preset(preset) {}
    Type type() const override { return Type::kNative; }
    int width() const override { return buffer->width(); }
    int height() const override { return buffer->height(); }
    webrtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() override { return buffer->ToI420(); }
    const webrtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer;
    const input::FrameMapping mapping;
    // Snapshot from the same settings revision as this frame's adaptation.
    const std::optional<StreamPreset> preset;
};
inline input::FrameMapping Mapping(const webrtc::scoped_refptr<webrtc::VideoFrameBuffer>& buffer) {
    if (auto* mapped = dynamic_cast<MappedVideoBuffer*>(buffer.get())) return mapped->mapping;
    return {};
}
inline webrtc::scoped_refptr<webrtc::VideoFrameBuffer> Unwrap(webrtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer) {
    if (auto* mapped = dynamic_cast<MappedVideoBuffer*>(buffer.get())) return mapped->buffer;
    return buffer;
}
inline webrtc::scoped_refptr<webrtc::VideoFrameBuffer> WithMapping(webrtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer, input::FrameMapping mapping,
                                                              std::optional<StreamPreset> preset = {}) {
    if (!mapping.Valid() || mapping.width != buffer->width() || mapping.height != buffer->height()) {
        if (!preset) return buffer;
        mapping = {};
    }
    return webrtc::make_ref_counted<MappedVideoBuffer>(std::move(buffer), mapping, preset);
}
}
