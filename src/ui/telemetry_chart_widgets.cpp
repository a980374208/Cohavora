#include "telemetry_chart_widgets.h"
#include "app_theme.h"
#include <QtGui/QPainter>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QToolTip>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <map>

namespace MeetingUI {
QString TelemetryAvailabilityText(livekit::telemetry::Availability state) {
    using A=livekit::telemetry::Availability;
    switch(state) {
    case A::Valid:return QStringLiteral("有效");
    case A::WarmingUp:return QStringLiteral("预热中");
    case A::NotExpected:return QStringLiteral("不适用");
    case A::Unsupported:return QStringLiteral("不支持");
    case A::Timeout:return QStringLiteral("超时");
    case A::Stale:return QStringLiteral("数据已过期");
    case A::Invalid:return QStringLiteral("缺少有效值");
    default:return QStringLiteral("等待数据");
    }
}
QString TelemetryReadingText(MetricId id,const MetricReading &r) {
    auto v=r.PlotValue(id);
    if(!v) return QStringLiteral("— · ")+TelemetryAvailabilityText(r.display);
    if(id==MetricId::StallActive) return *v!=0?QStringLiteral("停顿中"):QStringLiteral("未停顿");
    QString value;
    if(TelemetryMetric(id).scale==1 && std::holds_alternative<std::uint64_t>(r.value))
        value=QString::number(std::get<std::uint64_t>(r.value));
    else if(TelemetryMetric(id).scale==1 && std::holds_alternative<std::int64_t>(r.value))
        value=QString::number(std::get<std::int64_t>(r.value));
    else value=QString::number(*v,'f',2);
    return value+QStringLiteral(" ")+QString::fromUtf8(TelemetryMetric(id).unit)
        +(r.partial?QStringLiteral(" · 部分覆盖"):QString());
}
TelemetryTimeSeriesWidget::TelemetryTimeSeriesWidget(QWidget *parent):QWidget(parent) {
    AppTheme::applySurfacePalette(*this,AppTheme::Tone::Dark);
    setMinimumHeight(160); setMouseTracking(true); setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);
    setAccessibleName(QStringLiteral("遥测时序图；点击图例切换曲线"));
    Rebuild();
}
QString TelemetryTimeSeriesWidget::YAxisLabel(double value) const {
    const auto unit=frame_ && !frame_->series.empty()
        ? QString::fromUtf8(TelemetryMetric(frame_->series.front().id).unit) : QString();
    return QString::number(value,'g',3)+(unit.isEmpty()?QString():QStringLiteral(" ")+unit);
}
QRectF TelemetryTimeSeriesWidget::PlotRect() const {
    return plotRect_;
}
double TelemetryTimeSeriesWidget::X(TelemetryClock::time_point time) const {
    const auto span=std::chrono::duration<double>(frame_->end-frame_->begin).count();
    return PlotRect().left()+PlotRect().width()*std::chrono::duration<double>(time-frame_->begin).count()/std::max(0.001,span);
}
void TelemetryTimeSeriesWidget::SetFrame(ChartFramePtr frame) { frame_=std::move(frame);Rebuild();update(); }
void TelemetryTimeSeriesWidget::SetSeriesVisible(MetricId id,bool visible) {
    if(visible) hidden_.erase(id);else hidden_.insert(id);Rebuild();update();
}
void TelemetryTimeSeriesWidget::resizeEvent(QResizeEvent*) { Rebuild(); }
void TelemetryTimeSeriesWidget::changeEvent(QEvent *event) {
    QWidget::changeEvent(event);
    if(event->type()==QEvent::FontChange || event->type()==QEvent::StyleChange) {
        Rebuild();update();
    }
}
void TelemetryTimeSeriesWidget::Rebuild() {
    paths_.clear();legends_.clear();maximum_=1;
    bool percentBounded=frame_ && !frame_->series.empty();
    double dataMax=0;
    if(frame_) for(const auto &series:frame_->series) {
        const auto id=series.id;
        percentBounded&=(id==MetricId::Loss || id==MetricId::Retransmit || id==MetricId::Concealment || id==MetricId::Cpu);
        if(hidden_.count(id))continue;
        for(const auto &point:series.points) if(point.value) dataMax=std::max(dataMax,*point.value);
    }
    maximum_=std::max(1.0,dataMax*1.1);
    if(percentBounded && dataMax<=100.0) maximum_=std::min(100.0,maximum_);
    // Axis layout is shared by every sample in this frame, including hit testing.
    maximumLabel_=YAxisLabel(maximum_);zeroLabel_=YAxisLabel(0);
    const auto margin=std::max(54,std::max(fontMetrics().horizontalAdvance(maximumLabel_),
        fontMetrics().horizontalAdvance(zeroLabel_))+12);
    plotRect_=QRectF(margin,36,std::max(1,width()-margin-18),std::max(1,height()-68));
    if(!frame_)return;
    int left=8;
    for(const auto &series:frame_->series) {
        const auto label=QString::fromUtf8(TelemetryMetric(series.id).label);
        const int w=fontMetrics().horizontalAdvance(label)+24;
        legends_.push_back(QRect(left,4,w,25));left+=w+6;
        QPainterPath path;bool active=false;
        if(!hidden_.count(series.id)) for(const auto &point:series.points) {
            if(!point.value) {active=false;continue;}
            const QPointF pos(X(point.time),PlotRect().bottom()-*point.value/maximum_*PlotRect().height());
            if(active && !point.breakBefore) path.lineTo(pos);else path.moveTo(pos);
            // A single real sample remains visible without interpolating missing data.
            path.addEllipse(pos,1.5,1.5);path.moveTo(pos);active=true;
        }
        paths_.push_back(std::move(path));
    }
}
void TelemetryTimeSeriesWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);p.setRenderHint(QPainter::Antialiasing);p.fillRect(rect(),palette().base());
    p.setPen(palette().mid().color());p.drawRect(PlotRect());
    p.setPen(palette().text().color());
    const auto plot=PlotRect();
    p.drawText(QRectF(0,plot.top()-11,plot.left()-6,22),Qt::AlignRight|Qt::AlignVCenter,maximumLabel_);
    p.drawText(QRectF(0,plot.bottom()-11,plot.left()-6,22),Qt::AlignRight|Qt::AlignVCenter,zeroLabel_);
    if(!frame_)return;
    const auto elapsed = [&](TelemetryClock::time_point time) {
        return QStringLiteral("%1 秒").arg(std::max(0.0,
            std::chrono::duration<double>(time-frame_->timeOrigin).count()),0,'f',1);
    };
    const QRectF axis(plot.left(),plot.bottom()+4,plot.width(),24);
    p.drawText(axis,Qt::AlignLeft|Qt::AlignVCenter,elapsed(frame_->begin));
    p.drawText(axis,Qt::AlignCenter,QStringLiteral("采集经过时间"));
    p.drawText(axis,Qt::AlignRight|Qt::AlignVCenter,elapsed(frame_->end));
    bool hasPoint=false;
    for(size_t i=0;i<frame_->series.size();++i) {
        const auto id=frame_->series[i].id;
        QColor color=property(i%2?"chartSecondaryColor":"chartPrimaryColor").value<QColor>();
        p.setPen(QPen(hidden_.count(id)?palette().mid().color():color,2));
        p.drawText(legends_[i],Qt::AlignCenter,QString::fromUtf8(TelemetryMetric(id).label));
        if(!hidden_.count(id)) {p.drawPath(paths_[i]);hasPoint|=!paths_[i].isEmpty();}
    }
    if(!hasPoint) {p.setPen(palette().text().color());p.drawText(PlotRect(),Qt::AlignCenter,QStringLiteral("此范围内无有效采样"));}
}
void TelemetryTimeSeriesWidget::mousePressEvent(QMouseEvent *event) {
    if(!frame_)return;
    for(size_t i=0;i<legends_.size();++i) if(legends_[i].contains(event->pos()))
        SetSeriesVisible(frame_->series[i].id,hidden_.count(frame_->series[i].id)!=0);
}
void TelemetryTimeSeriesWidget::mouseMoveEvent(QMouseEvent *event) {
    if(!frame_ || !PlotRect().contains(event->pos())) {QToolTip::hideText();return;}
    QStringList lines;
    for(const auto &series:frame_->series) {
        if(hidden_.count(series.id))continue;
        const SeriesPoint *nearest=nullptr;double distance=1e9;
        for(const auto &point:series.points) {double d=std::abs(X(point.time)-event->pos().x());if(d<distance) {nearest=&point;distance=d;}}
        if(!nearest)continue;
        const double elapsed=std::max(0.0,std::chrono::duration<double>(nearest->time-frame_->timeOrigin).count());
        lines << QStringLiteral("%1 · 采集后 %2 秒：%3 %4").arg(QString::fromUtf8(TelemetryMetric(series.id).label))
            .arg(elapsed,0,'f',1).arg(nearest->value?QString::number(*nearest->value,'f',2):QStringLiteral("缺测"))
            .arg(QString::fromUtf8(TelemetryMetric(series.id).unit));
    }
    if(!lines.isEmpty())QToolTip::showText(event->globalPos(),lines.join('\n'),this);
}
void TelemetryTimeSeriesWidget::leaveEvent(QEvent*) {QToolTip::hideText();}
TelemetryStageBarsWidget::TelemetryStageBarsWidget(QWidget *parent):QWidget(parent) {
    AppTheme::applySurfacePalette(*this,AppTheme::Tone::Dark);
    setMinimumHeight(190);
}
void TelemetryStageBarsWidget::SetFrame(StageBarsFramePtr frame) {frame_=std::move(frame);update();}
void TelemetryStageBarsWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);p.fillRect(rect(),palette().base());if(!frame_)return;
    double maximum=1;
    for(size_t i=0;i<4;++i) {auto v=frame_->readings[i].PlotValue(static_cast<MetricId>(size_t(MetricId::Convert)+i));if(v)maximum=std::max(maximum,*v);}
    for(size_t i=0;i<4;++i) {
        auto id=static_cast<MetricId>(size_t(MetricId::Convert)+i);const auto &r=frame_->readings[i];
        const int y=12+int(i)*43;p.setPen(palette().text().color());
        p.drawText(10,y+13,QString::fromUtf8(TelemetryMetric(id).label)+QStringLiteral("：")+TelemetryReadingText(id,r));
        if(auto v=r.PlotValue(id))p.fillRect(QRectF(10,y+20,(width()-20)*(*v/maximum),8),property("chartPrimaryColor").value<QColor>());
    }
}
namespace {
const QStringList coarseBuckets={QStringLiteral("≤16"),QStringLiteral("16–25"),QStringLiteral("25–34"),
    QStringLiteral("34–50"),QStringLiteral("50–100"),QStringLiteral("100–250"),QStringLiteral("250–500"),
    QStringLiteral("500–1000"),QStringLiteral(">1000")};
QString BucketLabel(size_t i,bool fine) {
    return fine?(i==1000?QStringLiteral(">999"):QStringLiteral("≤%1").arg(i)):coarseBuckets[int(i)];
}
QString Quantile(const std::vector<std::uint64_t>& counts,double q,bool fine) {
    const auto total=std::accumulate(counts.begin(),counts.end(),std::uint64_t{});
    if(!total)return QStringLiteral("—");
    const auto rank=static_cast<std::uint64_t>(std::ceil(total*q));std::uint64_t seen=0;
    for(size_t i=0;i<counts.size();++i)if((seen+=counts[i])>=rank)return BucketLabel(i,fine)+QStringLiteral(" ms 桶");
    return QStringLiteral("—");
}
QString BoundaryText(const std::string& boundary) {
    if(boundary=="open")return QStringLiteral("持续中");
    if(boundary=="recovered")return QStringLiteral("提交恢复");
    if(boundary=="hidden")return QStringLiteral("隐藏截断");
    if(boundary=="rebound")return QStringLiteral("换绑截断");
    if(boundary=="stopped")return QStringLiteral("离会截断");
    return QStringLiteral("失活截断（观测时刻）");
}
}
TelemetryHistogramWidget::TelemetryHistogramWidget(QWidget *parent):QWidget(parent) {
    AppTheme::applySurfacePalette(*this,AppTheme::Tone::Dark);setMinimumHeight(250);setMouseTracking(true);
    setAccessibleName(QStringLiteral("帧间隔直方图；分位数为桶范围估计"));
}
void TelemetryHistogramWidget::SetFrame(HistogramFrame frame) {frame_=std::move(frame);update();}
void TelemetryHistogramWidget::SetMode(bool fine,bool percentage) {fine_=fine;percentage_=percentage;update();}
void TelemetryHistogramWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);p.fillRect(rect(),palette().base());p.setPen(palette().text().color());
    if(frame_.availability!=livekit::telemetry::Availability::Valid) {
        p.drawText(rect().adjusted(12,12,-12,-12),Qt::AlignCenter,QStringLiteral("帧间隔：")+TelemetryAvailabilityText(frame_.availability));return;
    }
    if(fine_ && frame_.fine.empty()) {p.drawText(rect(),Qt::AlignCenter,QStringLiteral("细桶未启用或尚未积累可用区间；请选择粗桶"));return;}
    const std::vector<std::uint64_t> counts=fine_?frame_.fine:std::vector<std::uint64_t>(frame_.counts.begin(),frame_.counts.end());
    const auto total=std::accumulate(counts.begin(),counts.end(),std::uint64_t{});
    const auto begin=fine_?frame_.fineBegin:frame_.begin,end=fine_?frame_.fineEnd:frame_.end;
    p.drawText(12,20,QStringLiteral("间隔 %1 个 · 样本跨度 %2 秒 · 绑定 %3（细桶 %4）%5")
        .arg(qulonglong(total)).arg(std::max(0.0,std::chrono::duration<double>(end-begin).count()),0,'f',1).arg(frame_.bindings).arg(frame_.fineBindings)
        .arg(frame_.partial?QStringLiteral(" · 部分覆盖"):QString()));
    p.drawText(12,41,QStringLiteral("P50 %1    P95 %2    P99 %3").arg(Quantile(counts,.5,fine_),Quantile(counts,.95,fine_),Quantile(counts,.99,fine_)));
    p.drawText(12,62,fine_?QStringLiteral("细桶按 5 秒合并；仅计入完整聚合边界，覆盖可能短于所选窗口"):
        QStringLiteral("仅当前稳定绑定范围；按采样窗口结束时刻计入，左边界可能含跨界间隔"));
    const double maximum=std::max(1.0,double(*std::max_element(counts.begin(),counts.end())));
    const auto maxLabel=percentage_?QStringLiteral("%1 %").arg(total?maximum*100/total:0,0,'f',1):QStringLiteral("%1 个").arg(qulonglong(maximum));
    const int left=std::max(65,fontMetrics().horizontalAdvance(maxLabel)+12);
    plot_=QRectF(left,85,std::max(1,width()-left-14),std::max(1,height()-120));
    p.drawText(4,94,maxLabel);p.drawText(4,int(plot_.bottom()),percentage_?QStringLiteral("0 %"):QStringLiteral("0 个"));
    p.setPen(palette().mid().color());p.drawLine(plot_.bottomLeft(),plot_.bottomRight());
    const double cell=plot_.width()/counts.size();
    auto color=property("chartPrimaryColor").value<QColor>();if(!color.isValid())color=palette().highlight().color();
    for(size_t i=0;i<counts.size();++i) {
        const double h=plot_.height()*counts[i]/maximum;
        p.fillRect(QRectF(plot_.left()+i*cell,plot_.bottom()-h,std::max(.3,cell*.85),h),color);
        if(!fine_ || i%250==0) {
            p.setPen(palette().text().color());
            const double labelWidth=std::min(plot_.width(),fine_?70:cell);
            const double x=std::clamp(plot_.left()+i*cell-(fine_?25:0),plot_.left(),plot_.right()-labelWidth);
            p.drawText(QRectF(x,plot_.bottom()+5,labelWidth,20),Qt::AlignCenter,BucketLabel(i,fine_));
        }
    }
    p.drawText(QRectF(0,height()-18,width(),18),Qt::AlignRight,QStringLiteral("帧间隔 ms  "));
}
void TelemetryHistogramWidget::mouseMoveEvent(QMouseEvent *e) {
    if(!plot_.contains(e->pos()))return;
    const auto count=fine_?frame_.fine.size():frame_.counts.size();if(!count)return;
    const auto i=std::min(count-1,size_t((e->pos().x()-plot_.left())/plot_.width()*count));
    const auto value=fine_?frame_.fine[i]:frame_.counts[i];
    QToolTip::showText(e->globalPos(),BucketLabel(i,fine_)+QStringLiteral(" ms：%1 个间隔").arg(value),this);
}
namespace {
QString FormatDurationCompact(std::uint64_t durationUs) {
    const double ms = double(durationUs) / 1000.0;
    if (ms >= 1000.0) return QStringLiteral("%1 s").arg(ms / 1000.0, 0, 'f', 2);
    if (ms >= 100.0) return QStringLiteral("%1 ms").arg(qulonglong(std::llround(ms)));
    return QStringLiteral("%1 ms").arg(ms, 0, 'f', 1);
}
QString MeasurementPointLabel(const std::string &point) {
    if (point == "first_decoded") return QStringLiteral("首帧解码");
    if (point == "first_pcm") return QStringLiteral("首份 PCM");
    if (point == "first_render_submit") return QStringLiteral("首次渲染提交");
    return QString::fromStdString(point);
}
} // namespace

TelemetryStallWidget::TelemetryStallWidget(QWidget *parent):QWidget(parent) {
    AppTheme::applySurfacePalette(*this,AppTheme::Tone::Dark);setMinimumHeight(140);setMouseTracking(true);
    setAccessibleName(QStringLiteral("逐绑定停顿区间与全局并发热力带；隐藏换绑和离会均为截断，非恢复"));
}
void TelemetryStallWidget::SetFrame(TimelineFramePtr frame,TelemetryClock::time_point begin,
    TelemetryClock::time_point end,TelemetryClock::time_point origin) {
    frame_=std::move(frame);begin_=begin;end_=end;origin_=origin;
    RebuildData();
    update();
}
void TelemetryStallWidget::resizeEvent(QResizeEvent*) {
    RebuildGeometry();
    update();
}
void TelemetryStallWidget::changeEvent(QEvent *event) {
    QWidget::changeEvent(event);
    if(event->type()==QEvent::FontChange || event->type()==QEvent::StyleChange) {
        RebuildGeometry();
        update();
    }
}
void TelemetryStallWidget::RebuildData() {
    rows_.clear();
    heatSlots_.clear();
    peakConcurrent_=0;
    peakTimeUs_=0;
    totalIntervals_=0;
    hoveredHit_=-1;
    if(!frame_) { RebuildGeometry(); return; }
    const auto micros=[](auto t){return std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count();};
    const auto rawBegin=std::max<std::int64_t>(0, micros(begin_));
    const auto rawEnd=std::max<std::int64_t>(rawBegin+1, micros(end_));
    const auto winBegin=std::uint64_t(rawBegin);
    const auto winEnd=std::uint64_t(rawEnd);
    std::map<std::string, size_t> rowIndex;
    for(const auto &event : frame_->events) {
        if(event.event_name!="render.stall.interval" || !event.begin_us || event.begin_us>winEnd) continue;
        const auto finish=event.end_us ? event.end_us : winEnd;
        if(finish<winBegin) continue;
        const auto clippedBegin=std::max(event.begin_us, winBegin);
        const auto clippedEnd=std::min(finish, winEnd);
        const auto clippedDur=clippedEnd>=clippedBegin ? clippedEnd-clippedBegin : 0;
        const auto binding=event.endpoint_id+":"+std::to_string(event.room_generation)+":"+std::to_string(event.binding_epoch);
        auto [it, inserted]=rowIndex.emplace(binding, rows_.size());
        if(inserted) {
            BindingRow row;
            row.key=binding;
            row.shortLabel=QString::fromStdString(event.endpoint_id.substr(0,5))+QStringLiteral(" / %1").arg(event.binding_epoch);
            rows_.push_back(std::move(row));
        }
        auto &row=rows_[it->second];
        IntervalEntry entry;
        entry.beginUs=event.begin_us;
        entry.endUs=finish;
        entry.clippedDurationUs=clippedDur;
        entry.thresholdUs=event.threshold_us;
        entry.roomGeneration=event.room_generation;
        entry.bindingEpoch=event.binding_epoch;
        entry.hasEnd=event.end_us!=0;
        entry.endpointId=event.endpoint_id;
        entry.boundary=event.boundary;
        if(event.boundary=="open") row.hasOpen=true;
        row.maxDurationUs=std::max(row.maxDurationUs, clippedDur);
        row.latestEndUs=std::max(row.latestEndUs, clippedEnd);
        ++row.intervalCount;
        ++totalIntervals_;
        row.intervals.push_back(std::move(entry));
    }
    for(auto &row : rows_) {
        std::sort(row.intervals.begin(), row.intervals.end(), [](const IntervalEntry &a, const IntervalEntry &b) {
            return a.beginUs<b.beginUs;
        });
        // 仅在单个绑定内部合并自身区间计算窗口内时长；跨绑定绝不相加。
        std::uint64_t merged=0, curBegin=0, curEnd=0;
        bool hasCur=false;
        for(const auto &iv : row.intervals) {
            const auto cb=std::max(iv.beginUs, winBegin);
            const auto ce=std::min(iv.endUs, winEnd);
            if(ce<cb) continue;
            if(!hasCur) { curBegin=cb; curEnd=ce; hasCur=true; }
            else if(cb<=curEnd) { curEnd=std::max(curEnd, ce); }
            else { merged+=curEnd-curBegin; curBegin=cb; curEnd=ce; }
        }
        if(hasCur) merged+=curEnd-curBegin;
        row.totalDurationUs=merged;
    }
    std::sort(rows_.begin(), rows_.end(), [](const BindingRow &a, const BindingRow &b) {
        if(a.hasOpen!=b.hasOpen) return a.hasOpen>b.hasOpen;
        if(a.totalDurationUs!=b.totalDurationUs) return a.totalDurationUs>b.totalDurationUs;
        if(a.intervalCount!=b.intervalCount) return a.intervalCount>b.intervalCount;
        if(a.latestEndUs!=b.latestEndUs) return a.latestEndUs>b.latestEndUs;
        return a.key<b.key;
    });
    if(rows_.size()<=kDefaultTopK) expanded_=false;

    constexpr size_t kSlotCount=160;
    const double span=double(winEnd-winBegin);
    heatSlots_.resize(kSlotCount);
    for(size_t s=0; s<kSlotCount; ++s) {
        heatSlots_[s].beginUs=winBegin+std::uint64_t(span*double(s)/double(kSlotCount));
        heatSlots_[s].endUs=winBegin+std::uint64_t(span*double(s+1)/double(kSlotCount));
    }
    std::vector<std::uint8_t> seen(kSlotCount, 0);
    for(const auto &row : rows_) {
        std::fill(seen.begin(), seen.end(), 0);
        for(const auto &iv : row.intervals) {
            const auto cb=std::max(iv.beginUs, winBegin);
            const auto ce=std::min(iv.endUs, winEnd);
            if(ce<cb) continue;
            const size_t s0=std::min(kSlotCount-1, size_t(std::clamp((double(cb)-double(winBegin))/span, 0.0, 1.0)*double(kSlotCount)));
            const size_t s1=std::min(kSlotCount-1, size_t(std::clamp((double(ce)-double(winBegin))/span, 0.0, 1.0)*double(kSlotCount)));
            for(size_t s=s0; s<=s1; ++s) {
                if(!seen[s]) {
                    seen[s]=1;
                    ++heatSlots_[s].concurrentBindings;
                }
                if(iv.boundary=="open") ++heatSlots_[s].openCount;
                else if(iv.boundary=="recovered") ++heatSlots_[s].recoveredCount;
                else ++heatSlots_[s].truncatedCount;
            }
        }
    }
    for(const auto &slot : heatSlots_) {
        if(slot.concurrentBindings>peakConcurrent_) {
            peakConcurrent_=slot.concurrentBindings;
            peakTimeUs_=slot.beginUs;
        }
    }
    RebuildGeometry();
}
void TelemetryStallWidget::RebuildGeometry() {
    hits_.clear();
    toggleRect_=QRectF();
    const int labelCol=176;
    const double plotLeft=double(labelCol);
    const double plotWidth=std::max(40.0, double(width()-labelCol-18));
    heatRect_=QRectF(plotLeft, 36.0, plotWidth, 24.0);

    const auto micros=[](auto t){return std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count();};
    const auto rawBegin=std::max<std::int64_t>(0, micros(begin_));
    const auto rawEnd=std::max<std::int64_t>(rawBegin+1, micros(end_));
    const auto originUs=micros(origin_);
    const double span=double(rawEnd-rawBegin);
    const auto x=[&](std::uint64_t time) {
        return plotLeft+plotWidth*std::clamp((double(time)-double(rawBegin))/span, 0.0, 1.0);
    };

    if(!heatSlots_.empty() && peakConcurrent_>0) {
        const double slotW=plotWidth/double(heatSlots_.size());
        for(size_t s=0; s<heatSlots_.size(); ++s) {
            const auto &slot=heatSlots_[s];
            if(slot.concurrentBindings==0) continue;
            const QRectF visual(plotLeft+double(s)*slotW, heatRect_.top()+2.0, std::max(2.0, slotW), heatRect_.height()-4.0);
            const QRectF hit=visual.adjusted(-4.0, -3.0, 4.0, 3.0);
            const double ratioPct=rows_.empty()?0.0:100.0*double(slot.concurrentBindings)/double(rows_.size());
            const QString tip=QStringLiteral("全局同步停顿热力带\n时段：%1 秒 → %2 秒\n同时停顿绑定：%3 路（占窗口有停顿绑定 %4 路的 %5%）\n覆盖区间：持续中 %6 · 提交恢复 %7 · 截断 %8")
                .arg(QString::number(std::max(0.0, (double(slot.beginUs)-double(originUs))/1e6), 'f', 2),
                     QString::number(std::max(0.0, (double(slot.endUs)-double(originUs))/1e6), 'f', 2))
                .arg(slot.concurrentBindings).arg(rows_.size()).arg(ratioPct, 0, 'f', 0)
                .arg(slot.openCount).arg(slot.recoveredCount).arg(slot.truncatedCount);
            hits_.push_back({visual, hit, tip, true});
        }
    }

    const size_t visibleCount=expanded_ ? rows_.size() : std::min(rows_.size(), kDefaultTopK);
    constexpr int kRowsTop=74;
    constexpr int kRowStep=28;
    for(size_t r=0; r<visibleCount; ++r) {
        const auto &row=rows_[r];
        const double y=double(kRowsTop+int(r)*kRowStep);
        for(const auto &iv : row.intervals) {
            const double x0=x(iv.beginUs);
            const double x1=x(iv.endUs);
            const QRectF bar(x0, y+5.0, std::max(3.0, x1-x0), 16.0);
            // 为极窄停顿条放宽命中区，支持悬停轻松读取 Tooltip。
            const QRectF hit=bar.adjusted(-7.0, -5.0, 7.0, 5.0);
            const auto durUs=iv.endUs>=iv.beginUs ? iv.endUs-iv.beginUs : 0;
            const auto text=QStringLiteral("%1\n%2 秒 → %3（时长 %4）\n房间代际 %5 · 绑定 %6 · 阈值 %7 ms")
                .arg(QString::fromStdString(iv.endpointId),
                     QString::number((double(iv.beginUs)-double(originUs))/1e6,'f',3),
                     iv.hasEnd ? QString::number((double(iv.endUs)-double(originUs))/1e6,'f',3)+QStringLiteral(" 秒 · ")+BoundaryText(iv.boundary)
                               : BoundaryText(iv.boundary),
                     FormatDurationCompact(durUs))
                .arg(iv.roomGeneration).arg(iv.bindingEpoch).arg(iv.thresholdUs/1000);
            hits_.push_back({bar, hit, text, false});
        }
    }

    int bottomY=kRowsTop+int(visibleCount)*kRowStep;
    if(rows_.empty()) bottomY=kRowsTop+30;
    if(rows_.size()>kDefaultTopK) {
        toggleRect_=QRectF(12.0, double(bottomY+4), std::max(140.0, double(width()-24)), 26.0);
        bottomY+=34;
    }
    const int desired=std::max(140, bottomY+30);
    if(minimumHeight()!=desired) setMinimumHeight(desired);
}
int TelemetryStallWidget::FindHitIndex(const QPoint &pos) const {
    int best=-1;
    double bestDist=1e18;
    bool bestDirect=false;
    for(size_t i=0; i<hits_.size(); ++i) {
        if(!hits_[i].hitRect.contains(pos)) continue;
        const bool direct=hits_[i].visualRect.contains(pos);
        const double dist=std::abs(double(pos.x())-hits_[i].visualRect.center().x())
            + std::abs(double(pos.y())-hits_[i].visualRect.center().y())*0.25;
        if(best<0 || (direct && !bestDirect) || (direct==bestDirect && dist<bestDist)) {
            best=int(i);
            bestDist=dist;
            bestDirect=direct;
        }
    }
    return best;
}
void TelemetryStallWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(),palette().base());
    p.setPen(palette().text().color());
    p.drawText(12,22,QStringLiteral("停顿区间 · 分绑定展示，重叠时间不可相加"));
    if(!frame_) {p.drawText(12,56,QStringLiteral("等待会话事件"));return;}

    const auto micros=[](auto t){return std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count();};
    const auto originUs=micros(origin_);
    const QColor primaryColor=property("chartPrimaryColor").value<QColor>().isValid()
        ? property("chartPrimaryColor").value<QColor>() : AppTheme::color(AppTheme::ColorRole::Accent);
    const QColor warnColor=AppTheme::color(AppTheme::ColorRole::Warning);
    const QColor dangerColor=AppTheme::color(AppTheme::ColorRole::Danger);
    const QColor borderColor=palette().mid().color();
    QColor mutedText=palette().text().color();
    mutedText.setAlpha(165);

    if(peakConcurrent_>0) {
        const QString summary=QStringLiteral("峰值并发 %1 路（@ %2 秒）· 共 %3 个绑定 / %4 次")
            .arg(peakConcurrent_)
            .arg(std::max(0.0, (double(peakTimeUs_)-double(originUs))/1e6), 0, 'f', 1)
            .arg(rows_.size()).arg(totalIntervals_);
        const double summaryW=std::max(40.0, double(width()-278));
        p.setPen(mutedText);
        p.drawText(QRectF(260, 6, summaryW, 22), Qt::AlignRight|Qt::AlignVCenter,
            p.fontMetrics().elidedText(summary, Qt::ElideRight, int(summaryW)));
    }

    // 顶部「全局同步停顿热力带」
    p.setPen(palette().text().color());
    p.drawText(QRectF(12, heatRect_.top(), 82, heatRect_.height()), Qt::AlignLeft|Qt::AlignVCenter, QStringLiteral("全局并发"));
    p.setPen(mutedText);
    QFont smallFont=font();
    smallFont.setPointSizeF(std::max(8.0, smallFont.pointSizeF()-1.5));
    p.setFont(smallFont);
    p.drawText(QRectF(72, heatRect_.top(), heatRect_.left()-80, heatRect_.height()), Qt::AlignRight|Qt::AlignVCenter,
        peakConcurrent_>0 ? QStringLiteral("峰值 %1 路").arg(peakConcurrent_) : QStringLiteral("0 路"));
    p.setFont(font());

    QColor trackBg=borderColor;
    trackBg.setAlpha(32);
    p.setPen(QPen(borderColor, 1.0));
    p.setBrush(trackBg);
    p.drawRoundedRect(heatRect_, 4.0, 4.0);
    p.setBrush(Qt::NoBrush);

    if(!heatSlots_.empty() && peakConcurrent_>0) {
        const double slotW=heatRect_.width()/double(heatSlots_.size());
        for(size_t s=0; s<heatSlots_.size(); ++s) {
            const auto &slot=heatSlots_[s];
            if(slot.concurrentBindings==0) continue;
            const double ratio=double(slot.concurrentBindings)/double(peakConcurrent_);
            QColor slotColor=primaryColor;
            if(slot.openCount>0 || (slot.concurrentBindings>=2 && ratio>=0.55)) {
                slotColor=dangerColor;
                slotColor.setAlpha(int(175+70*ratio));
            } else if(slot.concurrentBindings>=2) {
                slotColor=warnColor;
                slotColor.setAlpha(int(155+75*ratio));
            } else {
                slotColor.setAlpha(155);
            }
            const double barH=std::max(6.0, (heatRect_.height()-4.0)*(0.38+0.62*ratio));
            const QRectF cell(heatRect_.left()+double(s)*slotW, heatRect_.bottom()-2.0-barH, std::max(2.0, slotW), barH);
            p.fillRect(cell, slotColor);
        }
    }

    // 热力带与逐绑定列表之间的分隔线
    QColor divider=borderColor;
    divider.setAlpha(120);
    p.setPen(QPen(divider, 1.0));
    p.drawLine(QPointF(12.0, 67.0), QPointF(width()-18.0, 67.0));

    // Top-K 排序逐绑定行
    const auto rawBegin=std::max<std::int64_t>(0, micros(begin_));
    const auto rawEnd=std::max<std::int64_t>(rawBegin+1, micros(end_));
    const double span=double(rawEnd-rawBegin);
    const auto x=[&](std::uint64_t time) {
        return heatRect_.left()+heatRect_.width()*std::clamp((double(time)-double(rawBegin))/span, 0.0, 1.0);
    };

    const size_t visibleCount=expanded_ ? rows_.size() : std::min(rows_.size(), kDefaultTopK);
    constexpr int kRowsTop=74;
    constexpr int kRowStep=28;
    for(size_t r=0; r<visibleCount; ++r) {
        const auto &row=rows_[r];
        const double y=double(kRowsTop+int(r)*kRowStep);
        if(r%2==1) {
            QColor stripe=borderColor;
            stripe.setAlpha(16);
            p.fillRect(QRectF(10.0, y+1.0, double(width()-24), double(kRowStep-2)), stripe);
        }
        double labelX=12.0;
        if(row.hasOpen) {
            p.setPen(Qt::NoPen);
            p.setBrush(dangerColor);
            p.drawEllipse(QPointF(labelX+4.0, y+13.0), 3.5, 3.5);
            p.setBrush(Qt::NoBrush);
            labelX+=11.0;
        }
        p.setPen(palette().text().color());
        p.setFont(font());
        const double idWidth=std::max(24.0, 96.0-labelX);
        p.drawText(QRectF(labelX, y, idWidth, double(kRowStep)), Qt::AlignLeft|Qt::AlignVCenter,
            p.fontMetrics().elidedText(row.shortLabel, Qt::ElideRight, int(idWidth)));
        p.setFont(smallFont);
        p.setPen(row.hasOpen ? dangerColor : mutedText);
        const QString statText=QStringLiteral("%1次·%2").arg(row.intervalCount).arg(FormatDurationCompact(row.totalDurationUs));
        const double statWidth=std::max(24.0, heatRect_.left()-106.0);
        p.drawText(QRectF(98.0, y, statWidth, double(kRowStep)), Qt::AlignRight|Qt::AlignVCenter,
            p.fontMetrics().elidedText(statText, Qt::ElideRight, int(statWidth)));
        p.setFont(font());

        for(const auto &iv : row.intervals) {
            const double x0=x(iv.beginUs);
            const double x1=x(iv.endUs);
            const QRectF bar(x0, y+5.0, std::max(3.0, x1-x0), 16.0);
            QColor color=primaryColor;
            if(iv.boundary=="open") color=dangerColor;
            else if(iv.boundary!="recovered") color=palette().mid().color();
            p.setPen(Qt::NoPen);
            p.setBrush(color);
            p.drawRoundedRect(bar, 1.5, 1.5);
            if(iv.boundary=="open") {
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(palette().text().color(), 1.2));
                p.drawRoundedRect(bar, 1.5, 1.5);
            }
        }
    }

    // 悬停高亮外框
    if(hoveredHit_>=0 && size_t(hoveredHit_)<hits_.size()) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(palette().text().color(), 1.5));
        p.drawRoundedRect(hits_[size_t(hoveredHit_)].visualRect.adjusted(-1.0, -1.0, 1.0, 1.0), 2.5, 2.5);
    }

    // Top-K 折叠 / 展开交互栏
    if(!toggleRect_.isEmpty()) {
        QColor btnBg=borderColor;
        btnBg.setAlpha(toggleHovered_ ? 55 : 28);
        p.setPen(QPen(borderColor, 1.0));
        p.setBrush(btnBg);
        p.drawRoundedRect(toggleRect_, 6.0, 6.0);
        p.setBrush(Qt::NoBrush);
        p.setPen(palette().text().color());
        const size_t hiddenCount=rows_.size()-std::min(rows_.size(), kDefaultTopK);
        const QString toggleText=expanded_
            ? QStringLiteral("▾ 收起至前 %1 个最严重绑定（当前显示全部 %2 个）").arg(kDefaultTopK).arg(rows_.size())
            : QStringLiteral("▸ 其余 %1 个绑定已折叠（按停顿严重度排序，点击展开全部 %2 个）").arg(hiddenCount).arg(rows_.size());
        p.drawText(toggleRect_, Qt::AlignCenter, toggleText);
    }

    if(rows_.empty()) {
        p.setPen(mutedText);
        p.drawText(QRectF(12, kRowsTop, double(width()-24), 24), Qt::AlignLeft|Qt::AlignVCenter,
            QStringLiteral("当前范围无已记录停顿（不代表无丢失）"));
    }
    const auto axis=QStringLiteral("%1 秒 → %2 秒    裁剪 %3 · 管线丢弃 %4")
        .arg(std::max(0.0,std::chrono::duration<double>(begin_-origin_).count()),0,'f',1)
        .arg(std::max(0.0,std::chrono::duration<double>(end_-origin_).count()),0,'f',1).arg(frame_->omitted).arg(frame_->admissionDrops);
    p.setPen(mutedText);
    p.drawText(12,height()-10,axis);
}
void TelemetryStallWidget::mouseMoveEvent(QMouseEvent *e) {
    const bool overToggle=!toggleRect_.isEmpty() && toggleRect_.contains(e->pos());
    if(overToggle) {
        if(!toggleHovered_ || hoveredHit_!=-1) {
            toggleHovered_=true;
            hoveredHit_=-1;
            setCursor(Qt::PointingHandCursor);
            QToolTip::hideText();
            update();
        }
        return;
    }
    if(toggleHovered_) {
        toggleHovered_=false;
        unsetCursor();
        update();
    }
    const int hit=FindHitIndex(e->pos());
    if(hit!=hoveredHit_) {
        hoveredHit_=hit;
        update();
    }
    if(hit>=0) {
        QToolTip::showText(e->globalPos(), hits_[size_t(hit)].tip, this);
    } else {
        QToolTip::hideText();
    }
}
void TelemetryStallWidget::mousePressEvent(QMouseEvent *e) {
    if(e->button()==Qt::LeftButton && !toggleRect_.isEmpty() && toggleRect_.contains(e->pos())) {
        expanded_=!expanded_;
        hoveredHit_=-1;
        RebuildGeometry();
        update();
        return;
    }
    QWidget::mousePressEvent(e);
}
void TelemetryStallWidget::leaveEvent(QEvent*) {
    bool dirty=false;
    if(hoveredHit_!=-1) { hoveredHit_=-1; dirty=true; }
    if(toggleHovered_) { toggleHovered_=false; unsetCursor(); dirty=true; }
    QToolTip::hideText();
    if(dirty) update();
}

TelemetryTimelineWidget::TelemetryTimelineWidget(QWidget *parent):QWidget(parent) {
    AppTheme::applySurfacePalette(*this,AppTheme::Tone::Dark);setMinimumHeight(300);setMouseTracking(true);
    setAccessibleName(QStringLiteral("会话操作并行泳道；首帧按批次聚类胶囊展示，选择查看测量端点和安全原因"));
}
void TelemetryTimelineWidget::SetFrame(TimelineView frame) {
    frame_=std::move(frame);
    hoveredBox_=-1;
    selectedBox_=-1;
    RebuildLayout();
    update();
}
void TelemetryTimelineWidget::resizeEvent(QResizeEvent*) {
    RebuildLayout();
    update();
}
void TelemetryTimelineWidget::changeEvent(QEvent *event) {
    QWidget::changeEvent(event);
    if(event->type()==QEvent::FontChange || event->type()==QEvent::StyleChange) {
        RebuildLayout();
        update();
    }
}
QString TelemetryTimelineWidget::ClusterBadgeText(const TimelineVisualItem &item) const {
    if(!frame_.source || item.first>=frame_.source->events.size()) return QString();
    const auto &first=frame_.source->events[item.first];
    const auto count=item.members.empty() ? size_t{1} : item.members.size();
    if(first.measurement_point=="first_decoded") return QStringLiteral("解码 ×%1").arg(count);
    if(first.measurement_point=="first_render_submit") return QStringLiteral("渲染 ×%1").arg(count);
    if(first.measurement_point=="first_pcm") return QStringLiteral("音频 ×%1").arg(count);
    return QStringLiteral("首帧 ×%1").arg(count);
}
void TelemetryTimelineWidget::RebuildLayout() {
    boxes_.clear();
    const double left=110.0;
    const double right=std::max(left+40.0, double(width()-28));
    const double span=std::max(1.0, double(frame_.end-frame_.begin));
    const auto x=[&](std::uint64_t time){return left+(right-left)*std::clamp(double(time-frame_.begin)/span, 0.0, 1.0);};

    QFont badgeFont=font();
    badgeFont.setPointSizeF(std::max(8.5, badgeFont.pointSizeF()-1.0));
    const QFontMetrics badgeFm(badgeFont);

    struct LaneRow {
        double endPx = -1e9;
        int height = 14;
        int yOffset = 0;
    };
    std::array<std::vector<LaneRow>, 6> laneRows;
    struct PendingPlacement {
        size_t itemIndex = 0;
        size_t lane = 0;
        size_t row = 0;
        double x0 = 0, x1 = 0;
        double spanX0 = 0, spanX1 = 0;
        int itemH = 8;
        QString label;
    };
    std::vector<PendingPlacement> placed;
    placed.reserve(frame_.items.size());

    for(size_t i=0; i<frame_.items.size(); ++i) {
        const auto &item=frame_.items[i];
        if(!item.begin) continue; // Unknown time remains in the event table, never at t=0.
        const size_t lane=size_t(item.lane);
        const double sx0=x(item.begin);
        const double sx1=std::max(sx0, x(item.end));
        double vx0=sx0, vx1=sx1;
        int itemH=9, rowH=15;
        QString label;
        double reserveEnd=vx1;
        if(item.isCluster) {
            label=ClusterBadgeText(item);
            const double minW=double(badgeFm.horizontalAdvance(label))+16.0;
            const double w=std::min(right-left, std::max(sx1-sx0, minW));
            vx0=sx0;
            if(vx0+w>right) vx0=std::max(left, right-w);
            vx1=vx0+w;
            reserveEnd=vx1;
            itemH=19;
            rowH=24;
        } else if(item.milestone) {
            vx0=sx0-3.0;
            vx1=sx0+5.0;
            reserveEnd=vx1;
            itemH=9;
            rowH=15;
        } else {
            vx1=std::max(vx0+6.0, sx1);
            reserveEnd=vx1+((!item.hasBegin || !item.hasEnd) ? 12.0 : 0.0); // 为 → / ? 留出横向排布间距，不拉宽条形本身
            itemH=9;
            rowH=15;
        }

        auto &rows=laneRows[lane];
        size_t rowIdx=rows.size();
        for(size_t r=0; r<rows.size(); ++r) {
            if(rows[r].endPx+4.0<=vx0) {
                rowIdx=r;
                break;
            }
        }
        if(rowIdx==rows.size()) {
            rows.push_back({reserveEnd, rowH, 0});
        } else {
            rows[rowIdx].endPx=reserveEnd;
            rows[rowIdx].height=std::max(rows[rowIdx].height, rowH);
        }
        placed.push_back({i, lane, rowIdx, vx0, vx1, sx0, sx1, itemH, std::move(label)});
    }

    int y=34;
    for(size_t lane=0; lane<6; ++lane) {
        laneY_[lane]=y;
        int offset=4;
        for(auto &row : laneRows[lane]) {
            row.yOffset=offset;
            offset+=row.height;
        }
        laneHeight_[lane]=std::max(46, offset+16);
        y+=laneHeight_[lane];
    }

    boxes_.reserve(placed.size());
    for(const auto &pl : placed) {
        const auto &row=laneRows[pl.lane][pl.row];
        const double topY=double(laneY_[pl.lane]+row.yOffset+(row.height-pl.itemH)/2);
        const QRectF visual(pl.x0, topY, std::max(5.0, pl.x1-pl.x0), double(pl.itemH));
        const QRectF spanRect(pl.spanX0, visual.bottom()-2.0, std::max(2.0, pl.spanX1-pl.spanX0), 2.5);
        const QRectF hit=visual.adjusted(-4.0, -4.0, 4.0, 4.0);
        boxes_.push_back({pl.itemIndex, visual, spanRect, hit, pl.label});
    }
    const int desired=std::max(300, y+34);
    if(minimumHeight()!=desired) setMinimumHeight(desired);
}
int TelemetryTimelineWidget::FindHitIndex(const QPoint &pos) const {
    int best=-1;
    double bestDist=1e18;
    bool bestDirect=false;
    for(size_t i=0; i<boxes_.size(); ++i) {
        if(!boxes_[i].hitRect.contains(pos)) continue;
        const bool direct=boxes_[i].visualRect.contains(pos);
        const double dist=std::abs(double(pos.x())-boxes_[i].visualRect.center().x())
            + std::abs(double(pos.y())-boxes_[i].visualRect.center().y());
        if(best<0 || (direct && !bestDirect) || (direct==bestDirect && dist<=bestDist)) {
            best=int(i);
            bestDist=dist;
            bestDirect=direct;
        }
    }
    return best;
}
QString TelemetryTimelineWidget::Description(size_t index) const {
    const auto &item=frame_.items[index];
    const auto &first=frame_.source->events[item.first];
    const auto &last=frame_.source->events[item.last];
    if(item.isCluster && !item.members.empty()) {
        std::set<std::string> endpoints;
        QStringList sampleEndpoints;
        for(const auto memberIdx : item.members) {
            const auto &ev=frame_.source->events[memberIdx];
            if(!ev.endpoint_id.empty() && endpoints.insert(ev.endpoint_id).second && sampleEndpoints.size()<5) {
                sampleEndpoints << QString::fromStdString(ev.endpoint_id.substr(0, std::min<size_t>(8, ev.endpoint_id.size())));
            }
        }
        const auto medianIdx=item.members[item.members.size()/2];
        const auto medianUs=frame_.source->events[medianIdx].source_monotonic_us;
        const double relFirst=double(item.begin-frame_.begin)/1e6;
        const double relLast=double(item.end-frame_.begin)/1e6;
        const double relMedian=medianUs>=frame_.begin ? double(medianUs-frame_.begin)/1e6 : relFirst;
        const double spanMs=double(item.end-item.begin)/1000.0;
        const QString endpointLine=sampleEndpoints.isEmpty()
            ? QStringLiteral("端点：无标识")
            : QStringLiteral("端点样例：%1%2").arg(sampleEndpoints.join(QStringLiteral(", ")),
                endpoints.size()>size_t(sampleEndpoints.size())
                    ? QStringLiteral(" 等 %1 路").arg(endpoints.size()) : QString());
        return QStringLiteral("首帧批次聚类 · %1（%2 条 / %3 路端点）\n相对时间：%4 秒 → %5 秒（首尾跨度 %6 ms · 中位 %7 秒）\n结果 %8 · 会话 %9 / 房间 %10 / 恢复 %11\nparent %12\n%13\n保留事件裁剪 %14 · 管线丢弃 %15（单条原始记录见下方事件表）")
            .arg(MeasurementPointLabel(first.measurement_point))
            .arg(item.members.size()).arg(endpoints.size())
            .arg(relFirst, 0, 'f', 3).arg(relLast, 0, 'f', 3).arg(spanMs, 0, 'f', 1).arg(relMedian, 0, 'f', 3)
            .arg(QString::fromStdString(last.outcome))
            .arg(last.session_generation).arg(last.room_generation).arg(last.recovery_epoch)
            .arg(QString::fromStdString(first.parent_operation_id.empty() ? std::string("none") : first.parent_operation_id))
            .arg(endpointLine)
            .arg(frame_.source->omitted).arg(frame_.source->admissionDrops);
    }
    const auto duration=item.hasBegin && item.hasEnd
        ?QStringLiteral("%1 ms（事件源时间差）").arg(double(item.end-item.begin)/1000,0,'f',1)
        :item.milestone?QStringLiteral("独立观测点，无阶段耗时"):item.hasBegin?QStringLiteral("未见结束；不推算时长"):QStringLiteral("缺少开始；不推算时长");
    return QStringLiteral("%1 → %2\n%3 · 结果 %4 · 原因 %5 / %6\n测量端点 %7 · 会话 %8 / 房间 %9 / 恢复 %10\noperation %11\nparent %12\n保留事件裁剪 %13 · 管线丢弃 %14；首帧为原生观测，其余生命周期为入队观测")
        .arg(QString::fromStdString(first.event_name),QString::fromStdString(last.event_name),duration,
            QString::fromStdString(last.outcome),QString::fromStdString(last.error_code),QString::fromStdString(last.error_layer),
            QString::fromStdString(last.measurement_point)).arg(last.session_generation).arg(last.room_generation).arg(last.recovery_epoch)
        .arg(QString::fromStdString(first.operation_id),QString::fromStdString(first.parent_operation_id))
        .arg(frame_.source->omitted).arg(frame_.source->admissionDrops);
}
void TelemetryTimelineWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(),palette().base());
    p.setPen(palette().text().color());
    const QStringList names={QStringLiteral("入会 / 启动"),QStringLiteral("信令连接"),QStringLiteral("本地发布"),
        QStringLiteral("接收 / 首帧"),QStringLiteral("重连恢复"),QStringLiteral("其他事件")};
    const double left=110.0, right=std::max(left+40.0, double(width()-28));
    QColor divider=palette().mid().color();
    divider.setAlpha(120);
    for(int lane=0;lane<6;++lane) {
        const int top=laneY_[lane];
        p.setPen(QPen(divider, 1.0));
        p.drawLine(QPointF(left,top+laneHeight_[lane]-8),QPointF(right,top+laneHeight_[lane]-8));
        p.setPen(palette().text().color());
        p.drawText(QRect(8,top,96,laneHeight_[lane]-8),Qt::AlignVCenter|Qt::TextWordWrap,names[lane]);
    }
    if(!frame_.source || frame_.items.empty()) {
        const QString emptyText=frame_.excludedStallCount>0
            ? QStringLiteral("所选操作无泳道阶段（已将 %1 条渲染停顿分流至「视频诊断」）").arg(frame_.excludedStallCount)
            : QStringLiteral("所选操作暂无保留事件");
        p.drawText(rect(),Qt::AlignCenter,emptyText);
        return;
    }

    const QColor primaryColor=property("chartPrimaryColor").value<QColor>().isValid()
        ? property("chartPrimaryColor").value<QColor>() : AppTheme::color(AppTheme::ColorRole::Accent);
    const QColor secondaryColor=property("chartSecondaryColor").value<QColor>().isValid()
        ? property("chartSecondaryColor").value<QColor>() : AppTheme::color(AppTheme::ColorRole::Success);
    const QColor warnColor=AppTheme::color(AppTheme::ColorRole::Warning);
    const QColor dangerColor=AppTheme::color(AppTheme::ColorRole::Danger);

    QFont badgeFont=font();
    badgeFont.setPointSizeF(std::max(8.5, badgeFont.pointSizeF()-1.0));

    for(size_t idx=0; idx<boxes_.size(); ++idx) {
        const auto &box=boxes_[idx];
        const auto &item=frame_.items[box.itemIndex];
        const auto &first=frame_.source->events[item.first];
        const auto &last=frame_.source->events[item.last];
        const bool isError=(last.outcome=="failure" || last.outcome=="timeout");
        const bool isActive=(int(idx)==hoveredBox_ || int(idx)==selectedBox_);

        QColor color=primaryColor;
        if(isError) color=dangerColor;
        else if(item.isCluster) {
            if(first.measurement_point=="first_render_submit") color=secondaryColor;
            else if(first.measurement_point=="first_pcm") color=warnColor;
        }

        if(item.isCluster) {
            // 底部细刻度线标记该批次实际的首尾时间跨度
            QColor spanColor=color;
            spanColor.setAlpha(210);
            p.fillRect(box.spanRect, spanColor);

            QColor fill=color;
            fill.setAlpha(isActive ? 68 : 38);
            QColor stroke=color;
            stroke.setAlpha(isActive ? 255 : 205);
            p.setBrush(fill);
            p.setPen(QPen(stroke, isActive ? 2.0 : 1.2));
            p.drawRoundedRect(box.visualRect, 9.0, 9.0);
            p.setBrush(Qt::NoBrush);
            p.setPen(palette().text().color());
            p.setFont(badgeFont);
            p.drawText(box.visualRect, Qt::AlignCenter, box.label);
            p.setFont(font());
        } else if(item.hasBegin && item.hasEnd) {
            p.setPen(isActive ? QPen(palette().text().color(), 1.5) : Qt::NoPen);
            p.setBrush(color);
            p.drawRoundedRect(box.visualRect, 2.0, 2.0);
            p.setBrush(Qt::NoBrush);
        } else if(item.milestone) {
            p.setPen(QPen(color, isActive ? 2.2 : 1.5));
            p.setBrush(isActive ? color : Qt::NoBrush);
            p.drawEllipse(box.visualRect.center(), 3.5, 3.5);
            p.setBrush(Qt::NoBrush);
        } else {
            p.setPen(QPen(color, isActive ? 2.0 : 1.2));
            p.drawRect(box.visualRect);
            p.drawText(QPointF(box.visualRect.right()+2, box.visualRect.bottom()), item.hasBegin?QStringLiteral("→"):QStringLiteral("?"));
        }
    }

    QColor mutedText=palette().text().color();
    mutedText.setAlpha(175);
    p.setPen(mutedText);
    p.drawText(QRectF(left,5,right-left,23),Qt::AlignLeft,QStringLiteral("0 秒"));
    p.drawText(QRectF(left,5,right-left,23),Qt::AlignRight,QStringLiteral("%1 秒（所选事件跨度）").arg(double(frame_.end-frame_.begin)/1e6,0,'f',3));
    const auto unknown=std::count_if(frame_.items.begin(),frame_.items.end(),[](const auto &item){return !item.begin;});
    QString legend=QStringLiteral("实线：配对阶段 · 胶囊：首帧批次聚类 · 圆点：单点里程碑 · →/？：缺结束/开始 · 时间未知 %1 条").arg(unknown);
    if(frame_.excludedStallCount>0) {
        legend+=QStringLiteral(" · 已分流 %1 条停顿至「视频诊断」").arg(frame_.excludedStallCount);
    }
    p.drawText(10,height()-12,p.fontMetrics().elidedText(legend, Qt::ElideRight, std::max(80, width()-20)));
}
void TelemetryTimelineWidget::mousePressEvent(QMouseEvent *event) {
    const int hit=FindHitIndex(event->pos());
    if(hit>=0) {
        selectedBox_=hit;
        const auto text=Description(boxes_[size_t(hit)].itemIndex);
        setAccessibleDescription(text);
        if(selected_) selected_(text);
        update();
    }
}
void TelemetryTimelineWidget::mouseMoveEvent(QMouseEvent *event) {
    const int hit=FindHitIndex(event->pos());
    if(hit!=hoveredBox_) {
        hoveredBox_=hit;
        update();
    }
    if(hit>=0) {
        QToolTip::showText(event->globalPos(), Description(boxes_[size_t(hit)].itemIndex), this);
    } else {
        QToolTip::hideText();
    }
}
void TelemetryTimelineWidget::leaveEvent(QEvent*) {
    if(hoveredBox_!=-1) {
        hoveredBox_=-1;
        update();
    }
    QToolTip::hideText();
}
} // namespace MeetingUI
