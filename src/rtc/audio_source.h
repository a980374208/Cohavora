#pragma once

#include <cstdint>
#include <vector>
#include <functional>
#include <mutex>
#include <chrono>
#include "audio_frame.h"

namespace livekit {

class AudioSource {
public:
    AudioSource(int sample_rate, int num_channels, int queue_size_ms = 0);
    virtual ~AudioSource() = default;

    AudioSource(const AudioSource&) = delete;
    AudioSource& operator=(const AudioSource&) = delete;

    int sampleRate() const noexcept { return sample_rate_; }
    int numChannels() const noexcept { return num_channels_; }
    int queueSizeMs() const noexcept { return queue_size_ms_; }

    double queuedDuration() const noexcept;
    void clearQueue();

    void captureFrame(const AudioFrame& frame, int timeout_ms = 20);

    using FrameSink = std::function<void(const AudioFrame&)>;
    void addSink(FrameSink sink);

private:
    int sample_rate_;
    int num_channels_;
    int queue_size_ms_;

    mutable std::mutex sink_mutex_;
    std::vector<FrameSink> sinks_;
    double queued_duration_sec_{0.0};
};

} // namespace livekit
