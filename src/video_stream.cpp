#include "video_stream.h"

namespace livekit {

VideoStream::VideoStream(const Options& options)
    : options_(options) {}

VideoStream::~VideoStream() {
    close();
}

std::shared_ptr<VideoStream> VideoStream::fromTrack(const std::shared_ptr<Track>& track, const Options& options) {
    auto stream = std::make_shared<VideoStream>(options);
    if (track) {
        track->addVideoSink([stream](const VideoFrame& frame, const VideoCaptureOptions& opts) {
            stream->pushFrame(frame, opts);
        });
    }
    return stream;
}

void VideoStream::pushFrame(const VideoFrame& frame, const VideoCaptureOptions& options) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (closed_) return;

    VideoFrameEvent ev;
    ev.frame = frame;
    ev.timestamp_us = options.timestamp_us;
    ev.rotation = options.rotation;
    ev.metadata = options.metadata;

    if (options_.capacity > 0 && queue_.size() >= options_.capacity) {
        queue_.pop_front();
    }

    queue_.push_back(std::move(ev));
    cv_.notify_one();
}

bool VideoStream::read(VideoFrameEvent& event) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this]() {
        return !queue_.empty() || closed_;
    });

    if (queue_.empty() && closed_) {
        return false;
    }

    event = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

void VideoStream::close() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!closed_) {
        closed_ = true;
        cv_.notify_all();
    }
}

} // namespace livekit
