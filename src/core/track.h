#pragma once

#include <winsock2.h>
#include <string>
#include <memory>
#include <vector>
#include <functional>
#include <mutex>
#include "api/media_stream_interface.h"
#include "audio_frame.h"
#include "video_frame.h"
#include "video_source.h"
#include "webrtc_manager.h"

namespace livekit {

enum class TrackKind {
    Audio,
    Video,
    Unknown
};

enum class TrackSource {
    Unknown,
    Camera,
    Microphone,
    ScreenShareVideo,
    ScreenShareAudio
};

struct VideoPreset {
    int width = 0;
    int height = 0;
    int max_bitrate_bps = 0;
    int max_fps = 30;
};

struct VideoLayerSetting {
    int width = 0;
    int height = 0;
    int max_bitrate_bps = 0;
    int max_fps = 30;
    std::string rid; // "f", "h", "q"
    double scale_resolution_down_by = 1.0;
};

struct VideoPublishOptions {
    TrackSource source = TrackSource::Camera;
    bool simulcast = true;
    std::string video_codec = "vp8"; // "vp8", "h264", "vp9", "av1"
    std::string scalability_mode = ""; // e.g. "L3T3_KEY"
    std::vector<VideoLayerSetting> layers;
};

class Track {
public:
    using AudioFrameSink = std::function<void(const AudioFrame&)>;
    using VideoFrameSink = std::function<void(const VideoFrame&, const VideoCaptureOptions&)>;

    Track(const std::string& sid, const std::string& name, TrackKind kind, TrackSource source = TrackSource::Unknown)
        : sid_(sid), name_(name), kind_(kind), source_(source), muted_(false) {}
    virtual ~Track() = default;

    std::string sid() const { return sid_; }
    std::string name() const { return name_; }
    TrackKind kind() const { return kind_; }
    TrackSource source() const { return source_; }
    void set_source(TrackSource source) { source_ = source; }
    bool muted() const { return muted_; }

    void set_muted(bool muted) {
        muted_ = muted;
        if (rtc_track_) {
            rtc_track_->set_enabled(!muted);
        }
    }
    void set_sid(const std::string& sid) { sid_ = sid; }

    webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> rtc_track() const { return rtc_track_; }
    void set_rtc_track(webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> rtc_track) {
        rtc_track_ = rtc_track;
        if (rtc_track_) {
            rtc_track_->set_enabled(!muted_);
        }
    }

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
    TrackSource source_;
    bool muted_;

    webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> rtc_track_;

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
    void set_track(std::shared_ptr<Track> track) { track_ = track; }
    bool muted() const { return track_ ? track_->muted() : false; }

private:
    std::shared_ptr<Track> track_;
    std::string sid_;
    std::string name_;
};

} // namespace livekit
