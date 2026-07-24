#include "video_source.h"

namespace livekit {

VideoSource::VideoSource(int width, int height)
    : width_(width), height_(height) {}

void VideoSource::addSink(FrameSink sink) {
    std::lock_guard<std::mutex> lock(sink_mutex_);
    if (sink) {
        sinks_.push_back(sink);
    }
}

void VideoSource::captureFrame(const VideoFrame& frame, const VideoCaptureOptions& options) {
    std::vector<FrameSink> sinks_copy;
    {
        std::lock_guard<std::mutex> lock(sink_mutex_);
        sinks_copy = sinks_;
    }

    for (const auto& sink : sinks_copy) {
        sink(frame, options);
    }
}

void VideoSource::captureFrame(const VideoFrame& frame, std::int64_t timestamp_us, VideoRotation rotation) {
    VideoCaptureOptions options;
    options.timestamp_us = timestamp_us;
    options.rotation = rotation;
    captureFrame(frame, options);
}

} // namespace livekit
