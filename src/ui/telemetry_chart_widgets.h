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
    void mouseMoveEvent(QMouseEvent*) override;
private:
    TimelineFramePtr frame_;
    TelemetryClock::time_point begin_{},end_{},origin_{};
    std::vector<std::pair<QRectF,QString>> tips_;
};
class TelemetryTimelineWidget final : public QWidget {
public:
    explicit TelemetryTimelineWidget(QWidget *parent=nullptr);
    void SetFrame(TimelineView frame);
    void SetSelectionHandler(std::function<void(QString)> handler) { selected_=std::move(handler); }
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
private:
    QString Description(std::size_t item) const;
    TimelineView frame_;
    std::vector<int> itemY_;
    std::array<int,6> laneY_{34,80,126,172,218,264};
    std::array<int,6> laneHeight_{46,46,46,46,46,46};
    std::vector<std::pair<QRectF,std::size_t>> hits_;
    std::function<void(QString)> selected_;
};
} // namespace MeetingUI
