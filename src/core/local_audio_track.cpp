#include "local_audio_track.h"
#include "webrtc_manager.h"
#include "rtc_audio_source.h"

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

        auto factory = WebRTCManager::Instance().factory();
        if (factory) {
            WebRTCManager::Instance().worker_thread()->BlockingCall([&]() {
                auto rtc_src = RtcAudioSource::Create(source);
                auto rtc_audio_track = factory->CreateAudioTrack(name, rtc_src.get());
                track->set_rtc_track(rtc_audio_track);
            });
        }
    }
    return track;
}

} // namespace livekit
