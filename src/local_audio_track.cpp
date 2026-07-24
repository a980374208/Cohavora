#include "local_audio_track.h"

namespace livekit {

LocalAudioTrack::LocalAudioTrack(const std::string& sid, const std::string& name, std::shared_ptr<AudioSource> source)
    : Track(sid, name, TrackKind::Audio), source_(source) {}

std::shared_ptr<LocalAudioTrack> LocalAudioTrack::createLocalAudioTrack(const std::string& name,
                                                              const std::shared_ptr<AudioSource>& source) {
    std::string sid = "TR_AUD_" + name;
    auto track = std::make_shared<LocalAudioTrack>(sid, name, source);
    if (source) {
        source->addSink([track](const AudioFrame& frame) {
            if (!track->muted()) {
                track->notifyAudioFrame(frame);
            }
        });
    }
    return track;
}

} // namespace livekit
