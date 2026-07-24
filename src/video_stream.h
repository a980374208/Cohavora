#pragma once

#include <memory>
#include <deque>
#include <mutex>
#include <condition_variable>
#include "video_frame.h"
#include "video_source.h"
#include "track.h"

namespace livekit {

struct VideoFrameEvent {
    VideoFrame frame;
    std::int64_t timestamp_us{0};
    VideoRotation rotation{VideoRotation::VIDEO_ROTATION_0};
    std::optional<VideoFrameMetadata> metadata;
};

class VideoStream {
public:
    struct Options {
        std::size_t capacity{0};
        VideoBufferType format{VideoBufferType::RGBA};
    };

    static std::shared_ptr<VideoStream> fromTrack(const std::shared_ptr<Track>& track, const Options& options = Options());

    explicit VideoStream(const Options& options);
    virtual ~VideoStream();

    VideoStream(const VideoStream&) = delete;
    VideoStream& operator=(const VideoStream&) = delete;

    bool read(VideoFrameEvent& event);
    void pushFrame(const VideoFrame& frame, const VideoCaptureOptions& options);
    void close();

private:
    Options options_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<VideoFrameEvent> queue_;
    bool closed_{false};
};

} // namespace livekit
