#pragma once

#include <winsock2.h>
#include <cstdint>
#include <string>
#include <memory>
#include <vector>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <utility>
#include "api/media_stream_interface.h"
#include "audio_frame.h"
#include "video_frame.h"
#include "video_source.h"
#include "webrtc_manager.h"

namespace livekit {

namespace render {
class OwnedI420Frame;
}

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

enum class BackupCodecPolicy {
    PreferRegression = 0,
    Simulcast = 1,
    Regression = 2,
};

struct SimulcastCodecSpec {
    std::string codec = "vp8";
    std::string cid;
    std::string scalability_mode;
    std::vector<VideoLayerSetting> layers;
};

struct VideoPublishOptions {
    TrackSource source = TrackSource::Camera;
    bool simulcast = true;
    std::string video_codec = "vp8"; // "vp8", "h264", "vp9", "av1"
    std::string scalability_mode = ""; // e.g. "L3T3_KEY"
    std::vector<VideoLayerSetting> layers;

    // GAP-03: Backup Codec & Multi-Codec Simulcast
    std::optional<std::string> backup_codec; // e.g. "vp8", "h264"
    BackupCodecPolicy backup_codec_policy = BackupCodecPolicy::PreferRegression;
    std::vector<SimulcastCodecSpec> simulcast_codecs;
    bool auto_backup_codec = true;
};

class Track {
private:
    struct I420VideoSinkRegistry;

public:
    using AudioFrameSink = std::function<void(const AudioFrame&)>;
    using VideoFrameSink = std::function<void(const VideoFrame&, const VideoCaptureOptions&)>;
    using I420VideoFrameSink = std::function<void(std::shared_ptr<const render::OwnedI420Frame>)>;

    // Move-only token. Destroying or resetting it unregisters the sink without
    // depending on the Track object's lifetime.
    class I420VideoFrameSubscription {
    public:
        I420VideoFrameSubscription() = default;
        ~I420VideoFrameSubscription() { reset(); }

        I420VideoFrameSubscription(const I420VideoFrameSubscription&) = delete;
        I420VideoFrameSubscription& operator=(const I420VideoFrameSubscription&) = delete;

        I420VideoFrameSubscription(I420VideoFrameSubscription&& other) noexcept
            : registry_(std::move(other.registry_)), id_(std::exchange(other.id_, 0)) {}

        I420VideoFrameSubscription& operator=(I420VideoFrameSubscription&& other) noexcept {
            if (this != &other) {
                reset();
                registry_ = std::move(other.registry_);
                id_ = std::exchange(other.id_, 0);
            }
            return *this;
        }

        void reset() noexcept;
        bool active() const noexcept { return id_ != 0 && !registry_.expired(); }

    private:
        friend class Track;
        I420VideoFrameSubscription(std::weak_ptr<I420VideoSinkRegistry> registry, uint64_t id)
            : registry_(std::move(registry)), id_(id) {}

        std::weak_ptr<I420VideoSinkRegistry> registry_;
        uint64_t id_ = 0;
    };

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
            rtc_track_->set_enabled(!muted && volume_ > 0.001);
        }
    }

    void set_volume(double volume) {
        volume_ = volume;
        if (rtc_track_ && kind_ == TrackKind::Audio) {
            auto* audio_track = static_cast<webrtc::AudioTrackInterface*>(rtc_track_.get());
            if (audio_track) {
                audio_track->SetVolume(volume);
                if (volume <= 0.001) {
                    audio_track->set_enabled(false);
                } else if (!muted_) {
                    audio_track->set_enabled(true);
                }
            }
        }
    }
    double volume() const { return volume_; }

    void set_sid(const std::string& sid) { sid_ = sid; }

    webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> rtc_track() const { return rtc_track_; }
    void set_rtc_track(webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> rtc_track) {
        rtc_track_ = rtc_track;
        if (rtc_track_) {
            rtc_track_->set_enabled(!muted_ && volume_ > 0.001);
            if (kind_ == TrackKind::Audio) {
                auto* audio_track = static_cast<webrtc::AudioTrackInterface*>(rtc_track_.get());
                if (audio_track) {
                    audio_track->SetVolume(volume_);
                }
            }
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

    I420VideoFrameSubscription subscribeI420VideoFrames(I420VideoFrameSink sink) {
        if (!sink) return {};

        auto registry = i420_video_sink_registry_;
        std::lock_guard<std::mutex> lock(registry->mutex);
        const uint64_t id = registry->next_id++;
        registry->sinks.emplace(id, std::move(sink));
        return I420VideoFrameSubscription(registry, id);
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

    void notifyI420VideoFrame(std::shared_ptr<const render::OwnedI420Frame> frame) {
        if (!frame) return;

        auto registry = i420_video_sink_registry_;
        std::vector<I420VideoFrameSink> sinks;
        {
            std::lock_guard<std::mutex> lock(registry->mutex);
            sinks.reserve(registry->sinks.size());
            for (const auto& [id, sink] : registry->sinks) {
                sinks.push_back(sink);
            }
        }
        for (const auto& sink : sinks) sink(frame);
    }

private:
    struct I420VideoSinkRegistry {
        std::mutex mutex;
        uint64_t next_id = 1;
        std::unordered_map<uint64_t, I420VideoFrameSink> sinks;
    };

    std::string sid_;
    std::string name_;
    TrackKind kind_;
    TrackSource source_;
    bool muted_;
    double volume_ = 1.0;

    webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> rtc_track_;

    std::mutex sink_mutex_;
    std::vector<AudioFrameSink> audio_sinks_;
    std::vector<VideoFrameSink> video_sinks_;
    std::shared_ptr<I420VideoSinkRegistry> i420_video_sink_registry_ = std::make_shared<I420VideoSinkRegistry>();
};

inline void Track::I420VideoFrameSubscription::reset() noexcept {
    const uint64_t id = std::exchange(id_, 0);
    if (auto registry = registry_.lock()) {
        std::lock_guard<std::mutex> lock(registry->mutex);
        registry->sinks.erase(id);
    }
    registry_.reset();
}

class TrackPublication {
public:
    TrackPublication(std::shared_ptr<Track> track, const std::string& sid, const std::string& name)
        : track_(track), sid_(sid), name_(name) {}
    virtual ~TrackPublication() = default;

    std::string sid() const { return sid_; }
    std::string name() const { return name_; }
    std::shared_ptr<Track> track() const { return track_; }
    void set_track(std::shared_ptr<Track> track) { track_ = track; }
    bool muted() const { return track_ ? track_->muted() : false; }
    enum class StreamState {
        Active,
        Paused,
    };

    StreamState stream_state() const { return stream_state_; }
    void set_stream_state(StreamState state) { stream_state_ = state; }

    // The server can temporarily deny a subscription for a participant/track
    // pair. Keep that state on the real publication even before the remote
    // publication-control unification work lands.
    bool subscription_allowed() const { return subscription_allowed_; }
    void set_subscription_allowed(bool allowed) { subscription_allowed_ = allowed; }

private:
    std::shared_ptr<Track> track_;
    std::string sid_;
    std::string name_;
    StreamState stream_state_{StreamState::Active};
    bool subscription_allowed_{true};
};

} // namespace livekit
