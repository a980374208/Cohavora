#pragma once

#include <memory>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <string>
#include "audio_frame.h"
#include "track.h"

namespace livekit {

struct AudioFrameEvent {
    AudioFrame frame;
};

class AudioStream {
public:
    struct Options {
        std::size_t capacity{0}; // 0 = 无界队列，非 0 = 环形覆盖
        std::string noise_cancellation_module;
        std::string noise_cancellation_options_json;
    };

    static std::shared_ptr<AudioStream> fromTrack(const std::shared_ptr<Track>& track, const Options& options = Options());

    explicit AudioStream(const Options& options);
    virtual ~AudioStream();

    AudioStream(const AudioStream&) = delete;
    AudioStream& operator=(const AudioStream&) = delete;

    bool read(AudioFrameEvent& event);
    void pushFrame(const AudioFrame& frame);
    void close();

private:
    Options options_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<AudioFrameEvent> queue_;
    bool closed_{false};
};

} // namespace livekit
