#pragma once

#include <memory>
#include <string>
#include "track.h"
#include "audio_source.h"

namespace livekit {

class LocalAudioTrack : public Track {
public:
    static std::shared_ptr<LocalAudioTrack> createLocalAudioTrack(const std::string& name,
                                                                  const std::shared_ptr<AudioSource>& source);

    LocalAudioTrack(const std::string& sid, const std::string& name, std::shared_ptr<AudioSource> source);
    virtual ~LocalAudioTrack() = default;

    std::shared_ptr<AudioSource> source() const { return source_; }

    void mute() { set_muted(true); }
    void unmute() { set_muted(false); }

private:
    std::shared_ptr<AudioSource> source_;
};

} // namespace livekit
