#include "local_audio_track.h"
#include "webrtc_manager.h"
#include "rtc_audio_source.h"

#include <algorithm>
#include <cctype>

namespace livekit {

namespace {

std::string AudioCodecName(std::string codec) {
    std::transform(codec.begin(), codec.end(), codec.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    constexpr char kAudioPrefix[] = "audio/";
    if (codec.rfind(kAudioPrefix, 0) == 0) {
        codec.erase(0, sizeof(kAudioPrefix) - 1);
    }
    return codec;
}

} // namespace

LocalAudioTrack::LocalAudioTrack(const std::string& sid, const std::string& name,
        std::shared_ptr<AudioSource> source, const AudioPublishPolicy& policy)
    : Track(sid, name, TrackKind::Audio), source_(std::move(source)),
      requested_publish_policy_(policy) {}

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

ResolvedAudioPublishPlan LocalAudioTrack::ResolvePublishPlan(
    const AudioPublishPolicy& requested,
    const std::vector<std::string>& local_sender_codecs,
    bool media_encryption_enabled) {
    ResolvedAudioPublishPlan plan;
    plan.requested = requested;
    plan.effective = requested;
    plan.effective.codec = AudioCodecName(requested.codec);

    if (plan.effective.codec != "opus") {
        plan.error = "only Opus audio publication is supported";
        return plan;
    }
    const bool opus_available = std::any_of(
        local_sender_codecs.begin(), local_sender_codecs.end(),
        [](const std::string& codec) {
            return AudioCodecName(codec) == "opus";
        });
    if (!opus_available) {
        plan.error = "Opus is unavailable in the local sender capabilities";
        return plan;
    }
    if (requested.max_bitrate_bps != 0 &&
        (requested.max_bitrate_bps < 6000 ||
         requested.max_bitrate_bps > 510000)) {
        plan.error = "Opus max bitrate must be zero or between 6000 and 510000 bps";
        return plan;
    }
    if (media_encryption_enabled && plan.effective.red) {
        plan.effective.red = false;
        plan.red_disabled_for_encryption = true;
    }
    return plan;
}

std::shared_ptr<LocalAudioTrack> LocalAudioTrack::createLocalAudioTrack(const std::string& name,
        const std::shared_ptr<AudioSource>& source,
        const AudioPublishPolicy& policy) {
    std::string sid = "TR_AUD_" + name;
    auto track = std::make_shared<LocalAudioTrack>(sid, name, source, policy);
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
