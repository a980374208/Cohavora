#include "rtc_audio_source.h"
#include "../telemetry/session_telemetry.h"

#include <chrono>

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

RtcAudioSource::~RtcAudioSource() {
    SetTelemetryProbe({});
}

void RtcAudioSource::SetTelemetryProbe(
        std::shared_ptr<telemetry::LocalAudioActivityProbe> probe) noexcept {
    auto previous = std::atomic_exchange_explicit(
        &telemetry_probe_, std::move(probe), std::memory_order_acq_rel);
    if (previous) previous->active.store(false, std::memory_order_release);
}

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
    if (const auto probe = std::atomic_load_explicit(
            &telemetry_probe_, std::memory_order_acquire);
        probe && probe->active.load(std::memory_order_acquire)) {
        const auto sample_rate = static_cast<std::uint32_t>((std::max)(0, frame.sampleRate()));
        const auto channels = static_cast<std::uint32_t>((std::max)(0, frame.numChannels()));
        const auto samples = static_cast<std::uint32_t>(
            (std::max)(0, frame.samplesPerChannel()));
        const auto previous_rate = probe->sample_rate.exchange(
            sample_rate, std::memory_order_acq_rel);
        const auto previous_channels = probe->channels.exchange(
            channels, std::memory_order_acq_rel);
        const auto previous_samples = probe->samples_per_channel.exchange(
            samples, std::memory_order_acq_rel);
        if (previous_rate != 0 && previous_channels != 0 && previous_samples != 0 &&
            (previous_rate != sample_rate || previous_channels != channels ||
             previous_samples != samples)) {
            probe->format_changes.fetch_add(1, std::memory_order_relaxed);
        }
        const auto now = telemetry::SessionTelemetry::Clock::now();
        probe->last_frame_ns.store(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                now.time_since_epoch()).count(),
            std::memory_order_release);
        probe->frame_count.fetch_add(1, std::memory_order_relaxed);
    }
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
