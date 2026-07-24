#pragma once

#include <string>
#include <memory>
#include <vector>
#include <functional>
#include <mutex>
#include "audio_frame.h"
#include "video_frame.h"
#include "video_source.h"

namespace livekit {

enum class TrackKind {
    Audio,
    Video,
    Unknown
};

class Track {
public:
    using AudioFrameSink = std::function<void(const AudioFrame&)>;
    using VideoFrameSink = std::function<void(const VideoFrame&, const VideoCaptureOptions&)>;

    Track(const std::string& sid, const std::string& name, TrackKind kind)
        : sid_(sid), name_(name), kind_(kind), muted_(false) {}
    virtual ~Track() = default;

    std::string sid() const { return sid_; }
    std::string name() const { return name_; }
    TrackKind kind() const { return kind_; }
    bool muted() const { return muted_; }

    void set_muted(bool muted) { muted_ = muted; }
    void set_sid(const std::string& sid) { sid_ = sid; }

    void addAudioSink(AudioFrameSink sink) {
        std::lock_guard<std::mutex> lock(sink_mutex_);
        if (sink) audio_sinks_.push_back(sink);
    }

    void addVideoSink(VideoFrameSink sink) {
        std::lock_guard<std::mutex> lock(sink_mutex_);
        if (sink) video_sinks_.push_back(sink);
    }

    void notifyAudioFrame(const AudioFrame& frame) {
        std::vector<AudioFrameSink> sinks;
        {
            std::lock_guard<std::mutex> lock(sink_mutex_);
            sinks = audio_sinks_;
        }
        for (const auto& s : sinks) s(frame);
    }

    void notifyVideoFrame(const VideoFrame& frame, const VideoCaptureOptions& options) {
        std::vector<VideoFrameSink> sinks;
        {
            std::lock_guard<std::mutex> lock(sink_mutex_);
            sinks = video_sinks_;
        }
        for (const auto& s : sinks) s(frame, options);
    }

private:
    std::string sid_;
    std::string name_;
    TrackKind kind_;
    bool muted_;

    std::mutex sink_mutex_;
    std::vector<AudioFrameSink> audio_sinks_;
    std::vector<VideoFrameSink> video_sinks_;
};

class TrackPublication {
public:
    TrackPublication(std::shared_ptr<Track> track, const std::string& sid, const std::string& name)
        : track_(track), sid_(sid), name_(name) {}

    std::string sid() const { return sid_; }
    std::string name() const { return name_; }
    std::shared_ptr<Track> track() const { return track_; }
    bool muted() const { return track_ ? track_->muted() : false; }

private:
    std::shared_ptr<Track> track_;
    std::string sid_;
    std::string name_;
};

} // namespace livekit
