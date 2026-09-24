#include "local_audio_track.h"
#include "webrtc_manager.h"
#include "rtc_audio_source.h"

namespace livekit {

LocalAudioTrack::LocalAudioTrack(const std::string& sid, const std::string& name, std::shared_ptr<AudioSource> source)
    : Track(sid, name, TrackKind::Audio), source_(source) {}

LocalAudioTrack::~LocalAudioTrack() = default;

void LocalAudioTrack::set_rtc_source_for_telemetry(
        webrtc::scoped_refptr<RtcAudioSource> source) {
    std::shared_ptr<telemetry::LocalAudioActivityProbe> probe;
    webrtc::scoped_refptr<RtcAudioSource> installed;
    {
        std::lock_guard lock(rtc_source_mutex_);
        rtc_source_.swap(source);
        installed = rtc_source_;
        probe = publish_telemetry_probe_;
    }
    if (installed) installed->SetTelemetryProbe(std::move(probe));
}

void LocalAudioTrack::set_publish_telemetry_probe(
        std::shared_ptr<telemetry::LocalAudioActivityProbe> probe) {
    webrtc::scoped_refptr<RtcAudioSource> source;
    {
        std::lock_guard lock(rtc_source_mutex_);
        publish_telemetry_probe_ = probe;
        source = rtc_source_;
    }
    if (source) source->SetTelemetryProbe(std::move(probe));
}

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
            // MediaStreamTrack proxies are created and primarily accessed on
            // the PeerConnection signaling thread.
            WebRTCManager::Instance().signaling_thread()->BlockingCall([&]() {
                auto rtc_src = RtcAudioSource::Create(source);
                track->set_rtc_source_for_telemetry(rtc_src);
                auto rtc_audio_track = factory->CreateAudioTrack(name, rtc_src.get());
                track->set_rtc_track(rtc_audio_track);
            });
        }
    }
    return track;
}

} // namespace livekit
