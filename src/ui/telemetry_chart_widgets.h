#pragma once
#include "telemetry_panel_model.h"
#include <QtWidgets/QWidget>
#include <QtGui/QPainterPath>
#include <set>
#include <functional>

namespace MeetingUI {
class TelemetryTimeSeriesWidget final : public QWidget {
public:
    explicit TelemetryTimeSeriesWidget(QWidget *parent=nullptr);
    void SetFrame(ChartFramePtr frame);
    void SetSeriesVisible(MetricId id,bool visible);
protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void changeEvent(QEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;
private:
    void Rebuild();
    QRectF PlotRect() const;
    QString YAxisLabel(double value) const;
    double X(TelemetryClock::time_point time) const;
    ChartFramePtr frame_;
    std::set<MetricId> hidden_;
    std::vector<QPainterPath> paths_;
    std::vector<QRect> legends_;
    double maximum_=1;
    QRectF plotRect_;
    QString maximumLabel_,zeroLabel_;
};
struct StageBarsFrame { std::array<MetricReading,4> readings; };
using StageBarsFramePtr=std::shared_ptr<const StageBarsFrame>;
class TelemetryStageBarsWidget final : public QWidget {
public:
    explicit TelemetryStageBarsWidget(QWidget *parent=nullptr);
    void SetFrame(StageBarsFramePtr frame);
protected:
    void paintEvent(QPaintEvent*) override;
private:
    StageBarsFramePtr frame_;
};
QString TelemetryReadingText(MetricId id,const MetricReading &reading);
QString TelemetryAvailabilityText(livekit::telemetry::Availability state);
class TelemetryHistogramWidget final : public QWidget {
public:
    explicit TelemetryHistogramWidget(QWidget *parent=nullptr);
    void SetFrame(HistogramFrame frame);
    void SetMode(bool fine,bool percentage);
    TelemetryClock::time_point SourceEnd() const { return frame_.end; }
protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
private:
    HistogramFrame frame_;
    bool fine_=false,percentage_=false;
    QRectF plot_;
};
class TelemetryStallWidget final : public QWidget {
public:
    explicit TelemetryStallWidget(QWidget *parent=nullptr);
    void SetFrame(TimelineFramePtr timeline,TelemetryClock::time_point begin,
        TelemetryClock::time_point end,TelemetryClock::time_point origin);
protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void changeEvent(QEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;
private:
    static constexpr std::size_t kDefaultTopK = 8;
    struct IntervalEntry {
        std::uint64_t beginUs = 0;
        std::uint64_t endUs = 0;
        std::uint64_t clippedDurationUs = 0;
        std::uint64_t thresholdUs = 0;
        std::uint64_t roomGeneration = 0;
        std::uint64_t bindingEpoch = 0;
        bool hasEnd = false;
        std::string endpointId;
        std::string boundary;
    };
    struct BindingRow {
        std::string key;
        QString shortLabel;
        std::uint64_t totalDurationUs = 0;
        std::uint64_t maxDurationUs = 0;
        std::uint64_t latestEndUs = 0;
        std::size_t intervalCount = 0;
        bool hasOpen = false;
        std::vector<IntervalEntry> intervals;
    };
    struct HeatSlot {
        std::uint64_t beginUs = 0;
        std::uint64_t endUs = 0;
        std::size_t concurrentBindings = 0;
        std::size_t openCount = 0;
        std::size_t recoveredCount = 0;
        std::size_t truncatedCount = 0;
    };
    struct HitTarget {
        QRectF visualRect;
        QRectF hitRect;
        QString tip;
        bool isHeat = false;
    };
    void RebuildData();
    void RebuildGeometry();
    int FindHitIndex(const QPoint &pos) const;
    TimelineFramePtr frame_;
    TelemetryClock::time_point begin_{},end_{},origin_{};
    std::vector<BindingRow> rows_;
    std::vector<HeatSlot> heatSlots_;
    std::size_t peakConcurrent_ = 0;
    std::uint64_t peakTimeUs_ = 0;
    std::size_t totalIntervals_ = 0;
    bool expanded_ = false;
    QRectF heatRect_;
    QRectF toggleRect_;
    std::vector<HitTarget> hits_;
    int hoveredHit_ = -1;
    bool toggleHovered_ = false;
};
class TelemetryTimelineWidget final : public QWidget {
public:
    explicit TelemetryTimelineWidget(QWidget *parent=nullptr);
    void SetFrame(TimelineView frame);
    void SetSelectionHandler(std::function<void(QString)> handler) { selected_=std::move(handler); }
protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void changeEvent(QEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;
private:
    struct VisualBox {
        std::size_t itemIndex = 0;
        QRectF visualRect;
        QRectF spanRect;
        QRectF hitRect;
        QString label;
    };
    void RebuildLayout();
    int FindHitIndex(const QPoint &pos) const;
    QString Description(std::size_t item) const;
    QString ClusterBadgeText(const TimelineVisualItem &item) const;
    TimelineView frame_;
    std::array<int,6> laneY_{34,80,126,172,218,264};
    std::array<int,6> laneHeight_{46,46,46,46,46,46};
    std::vector<VisualBox> boxes_;
    int hoveredBox_ = -1;
    int selectedBox_ = -1;
    std::function<void(QString)> selected_;
};
} // namespace MeetingUI
