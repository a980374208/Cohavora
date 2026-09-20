#pragma once

#include <cstdint>
#include <optional>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>
#include <memory>
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
    using FrameSink = std::function<void(const VideoFrame&, const VideoCaptureOptions&)>;
    class Subscription {
    public:
        explicit Subscription(FrameSink sink);
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        ~Subscription() { disconnect(); }
        void disconnect();
    private:
        friend class VideoSource;
        struct State {
            explicit State(FrameSink sink) : sink(std::move(sink)) {}
            void deliver(const VideoFrame& frame, const VideoCaptureOptions& options);
            std::recursive_mutex mutex;
            FrameSink sink;
        };
        std::shared_ptr<State> state_;
    };
    VideoSource(int width, int height);
    virtual ~VideoSource() = default;

    VideoSource(const VideoSource&) = delete;
    VideoSource& operator=(const VideoSource&) = delete;

    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }

    void captureFrame(const VideoFrame& frame, const VideoCaptureOptions& options);
    void captureFrame(const VideoFrame& frame, std::int64_t timestamp_us = 0,
                      VideoRotation rotation = VideoRotation::VIDEO_ROTATION_0);

    void addSink(FrameSink sink);
    // Destruction/disconnect waits for in-flight delivery. The source holds
    // only a weak subscription, so it cannot keep a retired consumer alive.
    std::shared_ptr<Subscription> subscribe(FrameSink sink);

private:
    std::atomic<int> width_{0};
    std::atomic<int> height_{0};
    mutable std::mutex sink_mutex_;
    std::vector<FrameSink> sinks_;
    std::vector<std::weak_ptr<Subscription::State>> subscriptions_;
};

} // namespace livekit
