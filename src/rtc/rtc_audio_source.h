#pragma once

#include <memory>
#include <vector>
#include <mutex>
#include <algorithm>
#include "api/media_stream_interface.h"
#include "api/notifier.h"
#include "rtc_base/ref_counted_object.h"
#include "audio_source.h"

namespace livekit {

namespace telemetry { struct LocalAudioActivityProbe; }

class RtcAudioSource : public webrtc::Notifier<webrtc::AudioSourceInterface> {
public:
    static webrtc::scoped_refptr<RtcAudioSource> Create(std::shared_ptr<AudioSource> source);

    explicit RtcAudioSource(std::shared_ptr<AudioSource> source);
    ~RtcAudioSource() override;

    // webrtc::MediaSourceInterface impl
    SourceState state() const override { return kLive; }
    bool remote() const override { return false; }

    // webrtc::AudioSourceInterface impl
    void AddSink(webrtc::AudioTrackSinkInterface* sink) override;
    void RemoveSink(webrtc::AudioTrackSinkInterface* sink) override;
    void SetTelemetryProbe(
        std::shared_ptr<telemetry::LocalAudioActivityProbe> probe) noexcept;

private:
    struct FrameDeliveryState;

    std::shared_ptr<AudioSource> lk_source_;
    std::shared_ptr<FrameDeliveryState> delivery_state_;
    std::shared_ptr<AudioSource::Subscription> subscription_;
};

} // namespace livekit
