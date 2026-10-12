#include "rtc_audio_source.h"
#include "../telemetry/session_telemetry.h"

#include <chrono>

namespace livekit {

// AudioSource callbacks own only a temporary reference to this state, never
// the bridge. It stays valid if an OnData callback releases the last bridge
// reference; retirement then stops the remainder of that frame's fanout.
struct RtcAudioSource::FrameDeliveryState {
    std::recursive_mutex mutex;
    std::vector<webrtc::AudioTrackSinkInterface*> sinks;
    std::shared_ptr<telemetry::LocalAudioActivityProbe> telemetry_probe;
    bool retired = false;

    void SetTelemetryProbe(std::shared_ptr<telemetry::LocalAudioActivityProbe> probe) {
        std::lock_guard lock(mutex);
        auto previous = std::exchange(telemetry_probe, retired ? nullptr : std::move(probe));
        if (previous) previous->active.store(false, std::memory_order_release);
    }

    void Retire() {
        std::lock_guard lock(mutex);
        retired = true;
        sinks.clear();
        SetTelemetryProbe({});
    }

    void Deliver(const AudioFrame& frame) {
        std::lock_guard lock(mutex);
        if (retired) return;
        if (const auto probe = telemetry_probe;
            probe && probe->active.load(std::memory_order_acquire)) {
            const auto sample_rate = static_cast<std::uint32_t>((std::max)(0, frame.sampleRate()));
            const auto channels = static_cast<std::uint32_t>((std::max)(0, frame.numChannels()));
            const auto samples = static_cast<std::uint32_t>((std::max)(0, frame.samplesPerChannel()));
            const auto previous_rate = probe->sample_rate.exchange(sample_rate, std::memory_order_acq_rel);
            const auto previous_channels = probe->channels.exchange(channels, std::memory_order_acq_rel);
            const auto previous_samples = probe->samples_per_channel.exchange(samples, std::memory_order_acq_rel);
            if (previous_rate != 0 && previous_channels != 0 && previous_samples != 0 &&
                (previous_rate != sample_rate || previous_channels != channels || previous_samples != samples)) {
                probe->format_changes.fetch_add(1, std::memory_order_relaxed);
            }
            const auto now = telemetry::SessionTelemetry::Clock::now();
            probe->last_frame_ns.store(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count(),
                std::memory_order_release);
            probe->frame_count.fetch_add(1, std::memory_order_relaxed);
        }

        // OnData may synchronously remove itself, another sink, or the bridge.
        // Serialize RemoveSink against delivery and recheck each copied entry.
        const auto snapshot = sinks;
        for (auto* sink : snapshot) {
            if (retired) break;
            if (std::find(sinks.begin(), sinks.end(), sink) == sinks.end()) continue;
            sink->OnData(frame.data().data(), 16, frame.sampleRate(),
                         static_cast<size_t>(frame.numChannels()),
                         static_cast<size_t>(frame.samplesPerChannel()));
        }
    }
};

webrtc::scoped_refptr<RtcAudioSource> RtcAudioSource::Create(std::shared_ptr<AudioSource> source) {
    return webrtc::make_ref_counted<RtcAudioSource>(source);
}

RtcAudioSource::RtcAudioSource(std::shared_ptr<AudioSource> source)
    : lk_source_(std::move(source)), delivery_state_(std::make_shared<FrameDeliveryState>()) {
    if (lk_source_) {
        subscription_ = lk_source_->subscribe(
            [weak = std::weak_ptr<FrameDeliveryState>(delivery_state_)](const AudioFrame& frame) {
                if (auto state = weak.lock()) state->Deliver(frame);
            });
    }
}

RtcAudioSource::~RtcAudioSource() {
    // Lock order is subscription, then delivery state. Neither AddSink nor
    // RemoveSink enters the subscription lock. Reentrant destruction on the
    // delivery thread is safe because the callback keeps only the state alive.
    if (subscription_) subscription_->disconnect();
    delivery_state_->Retire();
}

void RtcAudioSource::SetTelemetryProbe(
        std::shared_ptr<telemetry::LocalAudioActivityProbe> probe) noexcept {
    delivery_state_->SetTelemetryProbe(std::move(probe));
}

void RtcAudioSource::AddSink(webrtc::AudioTrackSinkInterface* sink) {
    const auto state = delivery_state_;
    std::lock_guard lock(state->mutex);
    if (sink && !state->retired) state->sinks.push_back(sink);
}

void RtcAudioSource::RemoveSink(webrtc::AudioTrackSinkInterface* sink) {
    const auto state = delivery_state_;
    std::lock_guard lock(state->mutex);
    std::erase(state->sinks, sink);
}

} // namespace livekit
