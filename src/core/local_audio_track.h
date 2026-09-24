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
                                                                  const std::shared_ptr<AudioSource>& source);

    LocalAudioTrack(const std::string& sid, const std::string& name, std::shared_ptr<AudioSource> source);
    ~LocalAudioTrack() override;

    std::shared_ptr<AudioSource> source() const { return source_; }

    void mute() { set_muted(true); }
    void unmute() { set_muted(false); }
    void set_rtc_source_for_telemetry(webrtc::scoped_refptr<RtcAudioSource> source);
    void set_publish_telemetry_probe(
        std::shared_ptr<telemetry::LocalAudioActivityProbe> probe);

private:
    std::shared_ptr<AudioSource> source_;
    mutable std::mutex rtc_source_mutex_;
    webrtc::scoped_refptr<RtcAudioSource> rtc_source_;
    std::shared_ptr<telemetry::LocalAudioActivityProbe> publish_telemetry_probe_;
};

} // namespace livekit
