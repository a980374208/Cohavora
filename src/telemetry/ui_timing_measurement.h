#pragma once

#include "src/telemetry/diagnostic_pipeline.h"
#include <chrono>
#include <memory>

namespace livekit::diagnostic {

// UI-thread owned. No native resources, identifiers supplied by the user, or
// wall-clock subtraction. Started records survive an interrupted operation.
class UiTimingMeasurement final {
public:
    using Clock = std::chrono::steady_clock;
    UiTimingMeasurement(TimingMetric metric, TimingScene scene,
                        Clock::time_point start = Clock::now())
        : metric_(metric), scene_(scene), start_(Micros(start)),
          operation_(NewCorrelationId()) { Emit(TimingPhase::Started, Outcome::Unknown); }
    ~UiTimingMeasurement() { Finish(Outcome::Cancelled); }
    void Bind(Context context) {
        if (bound_) return;
        bound_ = true;
        context_ = context;
        Emit(TimingPhase::Bound, Outcome::Unknown);
    }
    void Finish(Outcome outcome) {
        if (terminal_) return;
        terminal_ = true;
        Emit(TimingPhase::Terminal, outcome);
    }
    void Media(bool audioRequested, bool videoRequested, bool audioReady, bool videoReady) {
        audioRequested_ = audioRequested; videoRequested_ = videoRequested;
        audioReady_ = audioReady; videoReady_ = videoReady;
    }
    bool terminal() const { return terminal_; }
    bool businessReady = false;
    Outcome businessOutcome = Outcome::Unknown;
    static std::uint64_t Micros(Clock::time_point t = Clock::now()) {
        return std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count();
    }
private:
    void Emit(TimingPhase phase, Outcome outcome) {
        Event event;
        event.kind = EventKind::UiTiming;
        event.thread_role = ThreadRole::Ui;
        event.context = context_;
        event.context.operation_id = operation_;
        event.timing_audio_requested = audioRequested_;
        event.timing_video_requested = videoRequested_;
        event.timing_audio_ready = audioReady_;
        event.timing_video_ready = videoReady_;
        event.timing_metric = metric_;
        event.timing_scene = scene_;
        event.timing_phase = phase;
        event.interval_begin_us = start_;
        event.source_monotonic_us = Micros();
        event.interval_end_us = phase == TimingPhase::Terminal ? event.source_monotonic_us : 0;
        event.outcome = outcome;
        EmitBusinessEvent(event);
    }
    TimingMetric metric_;
    TimingScene scene_;
    std::uint64_t start_;
    OpaqueId operation_;
    Context context_;
    bool terminal_ = false;
    bool bound_ = false;
    bool audioRequested_ = false, videoRequested_ = false;
    bool audioReady_ = false, videoReady_ = false;
};

} // namespace livekit::diagnostic
