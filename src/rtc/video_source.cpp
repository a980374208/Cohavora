#include "video_source.h"
#include <algorithm>

namespace livekit {

VideoSource::VideoSource(int width, int height)
    : width_(width), height_(height) {}

VideoSource::Subscription::Subscription(FrameSink sink) : state_(std::make_shared<State>(std::move(sink))) {}

void VideoSource::Subscription::disconnect() {
    std::lock_guard lock(state_->mutex);
    state_->sink = {};
}

void VideoSource::Subscription::State::deliver(const VideoFrame& frame, const VideoCaptureOptions& options) {
    std::lock_guard lock(mutex);
    // Copy permits a consumer to disconnect itself synchronously.
    auto callback = sink;
    if (callback) callback(frame, options);
}

std::shared_ptr<VideoSource::Subscription> VideoSource::subscribe(FrameSink sink) {
    auto subscription = std::make_shared<Subscription>(std::move(sink));
    std::lock_guard lock(sink_mutex_);
    std::erase_if(subscriptions_, [](const auto& value) { return value.expired(); });
    subscriptions_.push_back(subscription->state_);
    return subscription;
}

void VideoSource::addSink(FrameSink sink) {
    std::lock_guard<std::mutex> lock(sink_mutex_);
    if (sink) {
        sinks_.push_back(sink);
    }
}

void VideoSource::captureFrame(const VideoFrame& frame, const VideoCaptureOptions& options) {
    if (frame.width() > 0 && frame.height() > 0) {
        width_ = frame.width();
        height_ = frame.height();
    }

    std::vector<FrameSink> sinks_copy;
    std::vector<std::shared_ptr<Subscription::State>> subscriptions;
    {
        std::lock_guard<std::mutex> lock(sink_mutex_);
        sinks_copy = sinks_;
        for (auto& weak : subscriptions_) {
            if (auto subscription = weak.lock()) subscriptions.push_back(std::move(subscription));
        }
    }

    for (const auto& sink : sinks_copy) {
        sink(frame, options);
    }
    for (const auto& subscription : subscriptions) subscription->deliver(frame, options);
}

void VideoSource::captureFrame(const VideoFrame& frame, std::int64_t timestamp_us, VideoRotation rotation) {
    VideoCaptureOptions options;
    options.timestamp_us = timestamp_us;
    options.rotation = rotation;
    captureFrame(frame, options);
}

} // namespace livekit
