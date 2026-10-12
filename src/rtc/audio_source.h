#pragma once

#include <cstdint>
#include <vector>
#include <functional>
#include <mutex>
#include <memory>
#include <utility>
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
    class Subscription {
    public:
        explicit Subscription(FrameSink sink);
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        ~Subscription() { disconnect(); }
        // Waits for delivery on other threads; callback-side disconnect is
        // reentrant and suppresses subsequent delivery.
        void disconnect();
    private:
        friend class AudioSource;
        struct State {
            explicit State(FrameSink sink) : sink(std::move(sink)) {}
            void deliver(const AudioFrame& frame);
            std::recursive_mutex mutex;
            FrameSink sink;
        };
        std::shared_ptr<State> state_;
    };
    void addSink(FrameSink sink);
    // The consumer owns its subscription. The source cannot retain it.
    std::shared_ptr<Subscription> subscribe(FrameSink sink);

private:
    int sample_rate_;
    int num_channels_;
    int queue_size_ms_;

    mutable std::mutex sink_mutex_;
    std::vector<FrameSink> sinks_;
    std::vector<std::weak_ptr<Subscription::State>> subscriptions_;
    double queued_duration_sec_{0.0};
};

} // namespace livekit
