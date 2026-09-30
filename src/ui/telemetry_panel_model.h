#pragma once
#include "telemetry/telemetry_report.h"
#include "telemetry/telemetry_operation_timeline.h"
#include "telemetry/diagnostic_pipeline.h"
#include <array>
#include <span>
#include <deque>

namespace MeetingUI {
using TelemetryClock = livekit::telemetry::Snapshot::Clock;
using PanelMetricValue = livekit::telemetry::MetricValue;
enum class TelemetryPage { Network, Audio, Resources, Video, Timeline };
enum class MetricId { Receive, Send, Rtt, Jitter, Loss, Retransmit, Bandwidth,
    AudioDelay, AudioTarget, Concealment, Cpu, UiLag, PrivateMemory, WorkingSet,
    Threads, Handles, Growth, SubmitFps, StallActive, StallCount, StallDuration,
    Convert, Upload, Draw, Present, WindowSubmitFps, RenderBindings, Count };
struct SessionKey {
    std::string anonymousId;
    std::uint64_t generation = 0;
    bool operator==(const SessionKey&) const = default;
};
struct MetricDefinition {
    MetricId id;
    const char *label;
    const char *unit;
    const char *semantics;
    double scale;
};
const MetricDefinition &TelemetryMetric(MetricId id);
struct MetricReading {
    PanelMetricValue value;
    livekit::telemetry::Availability native = livekit::telemetry::Availability::Unknown;
    livekit::telemetry::Availability display = livekit::telemetry::Availability::Unknown;
    TelemetryClock::time_point sampledAt{};
    TelemetryClock::time_point freshnessAt{};
    std::uint64_t sourceRevision = 0;
    std::int64_t staleAfterMs = 0;
    bool partial = false;
    bool observedTime = false;
    std::uint64_t sourceToken = 0;
    std::optional<double> PlotValue(MetricId id) const;
};
struct SeriesPoint {
    TelemetryClock::time_point time;
    std::optional<double> value;
    bool breakBefore = false;
};
struct ChartSeries { MetricId id; std::vector<SeriesPoint> points; };
struct ChartFrame {
    TelemetryClock::time_point begin, end;
    TelemetryClock::time_point timeOrigin{};
    std::vector<ChartSeries> series;
};
using ChartFramePtr = std::shared_ptr<const ChartFrame>;
struct HistogramFrame {
    std::array<std::uint64_t,9> counts{};
    std::vector<std::uint64_t> fine;
    TelemetryClock::time_point begin{}, end{}, fineBegin{}, fineEnd{};
    livekit::telemetry::Availability availability = livekit::telemetry::Availability::Unknown;
    std::uint64_t scope = 0, bindings = 0, fineBindings = 0;
    bool partial = false;
};
struct TimelineFrame {
    std::vector<livekit::telemetry::OperationTimelineEntry> events;
    std::uint64_t omitted = 0, admissionDrops = 0;
    std::size_t retainedBytes = 0;
};
using TimelineFramePtr = std::shared_ptr<const TimelineFrame>;
enum class TimelineLane { Admission, Connect, Publish, Receive, Reconnect, Other };
struct TimelineVisualItem {
    std::size_t first = 0, last = 0;
    std::uint64_t begin = 0, end = 0;
    TimelineLane lane = TimelineLane::Other;
    bool hasBegin = false, hasEnd = false, milestone = false;
};
struct TimelineView {
    TimelineFramePtr source;
    std::vector<TimelineVisualItem> items;
    std::uint64_t begin = 0, end = 0;
};
TimelineView BuildTimelineView(TimelineFramePtr source, const std::string& operation = {});
struct PanelFrame {
    std::array<MetricReading, static_cast<size_t>(MetricId::Count)> readings;
    std::array<ChartSeries, static_cast<size_t>(MetricId::Count)> series;
    TelemetryClock::time_point begin{}, end{};
    TelemetryClock::time_point timeOrigin{};
    std::uint64_t sourceRevision = 0, presentationRevision = 0;
    std::uint64_t renderEventDrops = 0;
    int retainedSeconds = 0;
    bool waiting = true;
    HistogramFrame histogram;
    TimelineFramePtr timeline;
};
using PanelFramePtr = std::shared_ptr<const PanelFrame>;
class TelemetryPanelModel {
public:
    void BindSession(SessionKey key);
    void Reconcile(std::span<const livekit::telemetry::SafeTelemetryRecordPtr> records);
    // Zero uses the store's bounded history; positive values retain lightweight projections.
    void SetHistoryWindow(int seconds);
    PanelFramePtr BuildFrame(int windowSeconds, TelemetryClock::time_point now);
    void Clear();
    void ReconcileTimeline(const livekit::diagnostic::TimelineSnapshot& timeline);
    std::size_t RetainedBytes() const;
private:
    struct WindowSample {
        TelemetryClock::time_point begin{}, end{};
        std::uint64_t scope = 0, bindings = 0, fineBindings = 0;
        livekit::telemetry::Availability availability = livekit::telemetry::Availability::Unknown;
        std::array<std::uint64_t,9> counts{};
    };
    struct FineSample {
        TelemetryClock::time_point begin{}, end{};
        std::uint64_t scope = 0;
        std::array<std::uint64_t,1001> counts{};
    };
    struct Sample {
        std::weak_ptr<const livekit::telemetry::SafeTelemetryRecord> origin;
        TelemetryClock::time_point generated;
        std::uint64_t revision, resets;
        std::array<MetricReading, static_cast<size_t>(MetricId::Count)> readings;
        WindowSample window;
        std::uint64_t renderEventDrops = 0;
    };
    SessionKey key_;
    std::vector<Sample> samples_;
    std::vector<Sample> chartHistory_;
    std::optional<TelemetryClock::time_point> chartHistoryBegin_;
    int historySeconds_ = 0;
    void MergeChartHistory();
    void PruneChartHistory(TelemetryClock::time_point now);
    std::optional<TelemetryClock::time_point> timeOrigin_;
    std::uint64_t presentationRevision_ = 0;
    std::deque<FineSample> fineHistory_;
    TelemetryClock::time_point fineLastEnd_{};
    TimelineFramePtr timeline_;
};
} // namespace MeetingUI
