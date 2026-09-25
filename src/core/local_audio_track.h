#pragma once

#include <memory>
#include <mutex>
#include <string>
#include "track.h"
#include "audio_source.h"

namespace livekit {

class RtcAudioSource;
namespace telemetry { struct LocalAudioActivityProbe; }

class LocalAudioTrack : public Track {
public:
    static std::shared_ptr<LocalAudioTrack> createLocalAudioTrack(const std::string& name,
        const std::shared_ptr<AudioSource>& source,
        const AudioPublishPolicy& policy = AudioPublishPolicy{});

    LocalAudioTrack(const std::string& sid, const std::string& name,
        std::shared_ptr<AudioSource> source,
        const AudioPublishPolicy& policy = AudioPublishPolicy{});
    ~LocalAudioTrack() override;

    std::shared_ptr<AudioSource> source() const { return source_; }
    AudioPublishPolicy requested_publish_policy() const {
        return requested_publish_policy_;
    }
    void set_publish_policy(const AudioPublishPolicy& policy) {
        requested_publish_policy_ = policy;
    }
    static ResolvedAudioPublishPlan ResolvePublishPlan(
        const AudioPublishPolicy& requested,
        const std::vector<std::string>& local_sender_codecs,
        bool media_encryption_enabled);

    void mute() { set_muted(true); }
    void unmute() { set_muted(false); }
    void set_rtc_source_for_telemetry(webrtc::scoped_refptr<RtcAudioSource> source);
    void set_publish_telemetry_probe(
        std::shared_ptr<telemetry::LocalAudioActivityProbe> probe);

private:
    std::shared_ptr<AudioSource> source_;
    AudioPublishPolicy requested_publish_policy_;
    mutable std::mutex rtc_source_mutex_;
    webrtc::scoped_refptr<RtcAudioSource> rtc_source_;
    std::shared_ptr<telemetry::LocalAudioActivityProbe> publish_telemetry_probe_;
};

} // namespace livekit
