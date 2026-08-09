#include "audio_stream.h"

namespace livekit {

AudioStream::AudioStream(const Options& options)
    : options_(options) {}

AudioStream::~AudioStream() {
    close();
}

std::shared_ptr<AudioStream> AudioStream::fromTrack(const std::shared_ptr<Track>& track, const Options& options) {
    auto stream = std::make_shared<AudioStream>(options);
    if (track) {
        track->addAudioSink([stream](const AudioFrame& frame) {
            stream->pushFrame(frame);
        });
    }
    return stream;
}

void AudioStream::pushFrame(const AudioFrame& frame) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (closed_) return;

    AudioFrameEvent ev;
    ev.frame = frame;

    if (options_.capacity > 0 && queue_.size() >= options_.capacity) {
        queue_.pop_front(); // 环形覆盖最旧点
    }

    queue_.push_back(std::move(ev));
    cv_.notify_one();
}

bool AudioStream::read(AudioFrameEvent& event) {
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

void AudioStream::close() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!closed_) {
        closed_ = true;
        cv_.notify_all();
    }
}

} // namespace livekit
