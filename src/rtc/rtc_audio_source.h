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

private:
    void OnAudioFrame(const AudioFrame& frame);

    std::shared_ptr<AudioSource> lk_source_;
    std::mutex sink_mutex_;
    std::vector<webrtc::AudioTrackSinkInterface*> sinks_;
};

} // namespace livekit
