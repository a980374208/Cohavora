#include "audio_source.h"
#include <iostream>

namespace livekit {

AudioSource::AudioSource(int sample_rate, int num_channels, int queue_size_ms)
    : sample_rate_(sample_rate), num_channels_(num_channels), queue_size_ms_(queue_size_ms) {}

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
    {
        std::lock_guard<std::mutex> lock(sink_mutex_);
        sinks_copy = sinks_;
        queued_duration_sec_ += frame.duration();
    }

    for (const auto& sink : sinks_copy) {
        sink(frame);
    }
}

} // namespace livekit
