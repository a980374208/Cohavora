#include "audio_source.h"
#include <algorithm>

namespace livekit {

AudioSource::AudioSource(int sample_rate, int num_channels, int queue_size_ms)
    : sample_rate_(sample_rate), num_channels_(num_channels), queue_size_ms_(queue_size_ms) {}

AudioSource::Subscription::Subscription(FrameSink sink)
    : state_(std::make_shared<State>(std::move(sink))) {}

void AudioSource::Subscription::disconnect() {
    const auto state = state_;
    std::lock_guard lock(state->mutex);
    state->sink = {};
}

void AudioSource::Subscription::State::deliver(const AudioFrame& frame) {
    std::lock_guard lock(mutex);
    // Keep the callable alive if it disconnects or releases its token itself.
    auto callback = sink;
    if (callback) callback(frame);
}

std::shared_ptr<AudioSource::Subscription> AudioSource::subscribe(FrameSink sink) {
    auto subscription = std::make_shared<Subscription>(std::move(sink));
    std::lock_guard lock(sink_mutex_);
    std::erase_if(subscriptions_, [](const auto& value) { return value.expired(); });
    subscriptions_.push_back(subscription->state_);
    return subscription;
}

double AudioSource::queuedDuration() const noexcept {
    std::lock_guard<std::mutex> lock(sink_mutex_);
    return queued_duration_sec_;
}

void AudioSource::clearQueue() {
    std::lock_guard<std::mutex> lock(sink_mutex_);
    queued_duration_sec_ = 0.0;
}

void AudioSource::addSink(FrameSink sink) {
    std::lock_guard<std::mutex> lock(sink_mutex_);
    if (sink) {
        sinks_.push_back(sink);
    }
}

void AudioSource::captureFrame(const AudioFrame& frame, int timeout_ms) {
    if (frame.totalSamples() == 0) return;

    std::vector<FrameSink> sinks_copy;
    std::vector<std::shared_ptr<Subscription::State>> subscriptions;
    {
        std::lock_guard<std::mutex> lock(sink_mutex_);
        sinks_copy = sinks_;
        std::erase_if(subscriptions_, [](const auto& value) { return value.expired(); });
        for (const auto& weak : subscriptions_) {
            if (auto subscription = weak.lock()) subscriptions.push_back(std::move(subscription));
        }
        queued_duration_sec_ += frame.duration();
    }

    for (const auto& sink : sinks_copy) {
        sink(frame);
    }
    for (const auto& subscription : subscriptions) subscription->deliver(frame);
}

} // namespace livekit
