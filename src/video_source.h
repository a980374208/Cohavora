#pragma once

#include <cstdint>
#include <optional>
#include <vector>
#include <functional>
#include <mutex>
#include "video_frame.h"

namespace livekit {

enum class VideoRotation {
    VIDEO_ROTATION_0 = 0,
    VIDEO_ROTATION_90 = 90,
    VIDEO_ROTATION_180 = 180,
    VIDEO_ROTATION_270 = 270,
};

struct VideoFrameMetadata {
    std::optional<std::uint64_t> user_timestamp_us;
    std::optional<std::uint32_t> frame_id;
};

struct VideoCaptureOptions {
    std::int64_t timestamp_us = 0;
    VideoRotation rotation = VideoRotation::VIDEO_ROTATION_0;
    std::optional<VideoFrameMetadata> metadata;
};

class VideoSource {
public:
    VideoSource(int width, int height);
    virtual ~VideoSource() = default;

    VideoSource(const VideoSource&) = delete;
    VideoSource& operator=(const VideoSource&) = delete;

    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }

    void captureFrame(const VideoFrame& frame, const VideoCaptureOptions& options);
    void captureFrame(const VideoFrame& frame, std::int64_t timestamp_us = 0,
                      VideoRotation rotation = VideoRotation::VIDEO_ROTATION_0);

    using FrameSink = std::function<void(const VideoFrame&, const VideoCaptureOptions&)>;
    void addSink(FrameSink sink);

private:
    int width_{0};
    int height_{0};
    mutable std::mutex sink_mutex_;
    std::vector<FrameSink> sinks_;
};

} // namespace livekit
