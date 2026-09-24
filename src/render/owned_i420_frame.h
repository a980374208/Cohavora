#pragma once

#include <cstddef>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "rtc/video_source.h"

namespace webrtc {
class VideoFrame;
}

namespace livekit::render {

enum class RenderExpectationReason {
    SurfaceVisible,
    SurfaceHidden,
    WindowMinimized,
    StreamPaused,
    BindingEnded,
};

struct RenderFrameMetadata;
class CanvasRenderTimingObserver;

class RenderSubmitObserver {
public:
    using Clock = std::chrono::steady_clock;
    virtual ~RenderSubmitObserver() = default;
    virtual void SetExpected(
        bool expected,
        RenderExpectationReason reason,
        Clock::time_point source_time) = 0;
    virtual void OnSubmitted(
        const RenderFrameMetadata& metadata,
        const char* measurement_point,
        Clock::time_point source_time) = 0;
    virtual void OnStageTiming(
        const RenderFrameMetadata&,
        const char*,
        std::chrono::microseconds,
        Clock::time_point) {}
    // A shared session sink, returned only for a currently valid binding.
    // Canvas operations must be deduplicated by this sink, not by video frame.
    virtual std::shared_ptr<CanvasRenderTimingObserver> CanvasTimingObserver(
        const RenderFrameMetadata&) { return {}; }
};

struct RenderFrameMetadata {
    using Clock = std::chrono::steady_clock;

    std::string series_key;
    std::uint64_t room_generation = 0;
    std::uint64_t binding_epoch = 0;
    std::uint64_t frame_token = 0;
    Clock::time_point decoded_at{};
    std::shared_ptr<RenderSubmitObserver> observer;

    bool valid() const noexcept {
        return !series_key.empty() && room_generation != 0 &&
            binding_epoch != 0 && frame_token != 0 && observer;
    }

    bool same_binding_as(const RenderFrameMetadata& other) const noexcept {
        return valid() && other.valid() && series_key == other.series_key &&
            room_generation == other.room_generation &&
            binding_epoch == other.binding_epoch && observer == other.observer;
    }
};

// Rendering-facing colour metadata. It intentionally records only the values
// needed to select a YUV-to-RGB conversion; WebRTC-specific types do not leak
// beyond the RTC-to-rendering adapter.
enum class RenderColorMatrix {
    Unspecified,
    Bt601,
    Bt709,
    Bt2020,
};

enum class RenderColorRange {
    Unspecified,
    Limited,
    Full,
};

struct RenderColorSpace {
    RenderColorMatrix matrix = RenderColorMatrix::Unspecified;
    RenderColorRange range = RenderColorRange::Unspecified;
};

// An immutable, tightly packed I420 frame owned by the rendering pipeline.
// The object remains valid after WebRTC returns its source buffer to the
// decoder, which makes it safe to hand across worker/UI/render threads.
class OwnedI420Frame final {
public:
    using Ptr = std::shared_ptr<const OwnedI420Frame>;

    static Ptr CopyFrom(
        const webrtc::VideoFrame& frame,
        RenderFrameMetadata render_metadata = {});

    static Ptr CopyFromPlanes(int width,
                              int height,
                              const uint8_t* data_y,
                              int stride_y,
                              const uint8_t* data_u,
                              int stride_u,
                              const uint8_t* data_v,
                              int stride_v,
                              int64_t timestamp_us = 0,
                              VideoRotation rotation = VideoRotation::VIDEO_ROTATION_0,
                              RenderColorSpace color_space = {},
                              RenderFrameMetadata render_metadata = {});

    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }
    int chroma_width() const noexcept { return chroma_width_; }
    int chroma_height() const noexcept { return chroma_height_; }

    const uint8_t* data_y() const noexcept { return storage_.data(); }
    const uint8_t* data_u() const noexcept { return storage_.data() + u_offset_; }
    const uint8_t* data_v() const noexcept { return storage_.data() + v_offset_; }
    int stride_y() const noexcept { return stride_y_; }
    int stride_u() const noexcept { return stride_u_; }
    int stride_v() const noexcept { return stride_v_; }

    int64_t timestamp_us() const noexcept { return timestamp_us_; }
    VideoRotation rotation() const noexcept { return rotation_; }
    const RenderColorSpace& color_space() const noexcept { return color_space_; }
    const RenderFrameMetadata& render_metadata() const noexcept {
        return render_metadata_;
    }

private:
    OwnedI420Frame(int width,
                   int height,
                   int chroma_width,
                   int chroma_height,
                   size_t y_size,
                   size_t u_size,
                   int64_t timestamp_us,
                   VideoRotation rotation,
                   RenderColorSpace color_space,
                   RenderFrameMetadata render_metadata);

    int width_ = 0;
    int height_ = 0;
    int chroma_width_ = 0;
    int chroma_height_ = 0;
    int stride_y_ = 0;
    int stride_u_ = 0;
    int stride_v_ = 0;
    size_t u_offset_ = 0;
    size_t v_offset_ = 0;
    int64_t timestamp_us_ = 0;
    VideoRotation rotation_ = VideoRotation::VIDEO_ROTATION_0;
    RenderColorSpace color_space_;
    RenderFrameMetadata render_metadata_;
    std::vector<uint8_t> storage_;
};

} // namespace livekit::render
