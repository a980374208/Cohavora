#include "rtc_audio_source.h"

namespace livekit {

webrtc::scoped_refptr<RtcAudioSource> RtcAudioSource::Create(std::shared_ptr<AudioSource> source) {
    return webrtc::make_ref_counted<RtcAudioSource>(source);
}

RtcAudioSource::RtcAudioSource(std::shared_ptr<AudioSource> source)
    : lk_source_(source) {
    if (lk_source_) {
        lk_source_->addSink([this](const AudioFrame& frame) {
            OnAudioFrame(frame);
        });
    }
}

RtcAudioSource::~RtcAudioSource() = default;

void RtcAudioSource::AddSink(webrtc::AudioTrackSinkInterface* sink) {
    std::lock_guard<std::mutex> lock(sink_mutex_);
    if (sink) {
        sinks_.push_back(sink);
    }
}

void RtcAudioSource::RemoveSink(webrtc::AudioTrackSinkInterface* sink) {
    std::lock_guard<std::mutex> lock(sink_mutex_);
    sinks_.erase(std::remove(sinks_.begin(), sinks_.end(), sink), sinks_.end());
}

void RtcAudioSource::OnAudioFrame(const AudioFrame& frame) {
    std::lock_guard<std::mutex> lock(sink_mutex_);
    if (sinks_.empty()) return;

    const void* audio_data = frame.data().data();
    int bits_per_sample = 16;
    int sample_rate = frame.sampleRate();
    size_t number_of_channels = static_cast<size_t>(frame.numChannels());
    size_t number_of_frames = static_cast<size_t>(frame.samplesPerChannel());

    for (auto* sink : sinks_) {
        sink->OnData(audio_data, bits_per_sample, sample_rate, number_of_channels, number_of_frames);
    }
}

} // namespace livekit
