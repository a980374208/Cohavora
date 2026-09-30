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
    if(frame_) for(const auto &series:frame_->series) if(!hidden_.count(series.id))
        for(const auto &point:series.points) if(point.value) maximum_=std::max(maximum_,*point.value*1.1);
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
TelemetryStallWidget::TelemetryStallWidget(QWidget *parent):QWidget(parent) {
    AppTheme::applySurfacePalette(*this,AppTheme::Tone::Dark);setMinimumHeight(130);setMouseTracking(true);
    setAccessibleName(QStringLiteral("逐绑定停顿区间；隐藏换绑和离会均为截断，非恢复"));
}
void TelemetryStallWidget::SetFrame(TimelineFramePtr frame,TelemetryClock::time_point begin,
    TelemetryClock::time_point end,TelemetryClock::time_point origin) {
    frame_=std::move(frame);begin_=begin;end_=end;origin_=origin;update();
}
void TelemetryStallWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);p.fillRect(rect(),palette().base());p.setPen(palette().text().color());tips_.clear();
    p.drawText(12,22,QStringLiteral("停顿区间 · 分绑定展示，重叠时间不可相加"));
    if(!frame_) {p.drawText(12,54,QStringLiteral("等待会话事件"));return;}
    std::map<std::string,int> rows;
    const auto micros=[](auto t){return std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count();};
    const auto begin=micros(begin_),end=micros(end_);const double span=std::max(std::int64_t{1},end-begin);
    const auto x=[&](std::uint64_t time){return 115+(width()-135)*std::clamp((double(time)-begin)/span,0.0,1.0);};
    for(const auto &event:frame_->events) {
        if(event.event_name!="render.stall.interval" || !event.begin_us || event.begin_us>std::uint64_t(end))continue;
        const auto finish=event.end_us?event.end_us:std::uint64_t(end);
        if(finish<std::uint64_t(begin))continue;
        const auto binding=event.endpoint_id+":"+std::to_string(event.room_generation)+":"+std::to_string(event.binding_epoch);
        auto [it,inserted]=rows.emplace(binding,int(rows.size()));const int y=52+it->second*27;
        p.setPen(palette().text().color());p.drawText(10,y+13,QString::fromStdString(event.endpoint_id.substr(0,5))+QStringLiteral(" / %1").arg(event.binding_epoch));
        const QRectF bar(x(event.begin_us),y,std::max(2.0,x(finish)-x(event.begin_us)),18);
        QColor color=property("chartPrimaryColor").value<QColor>();
        if(event.boundary!="recovered" && event.boundary!="open")color=palette().mid().color();
        p.fillRect(bar,color);if(event.boundary=="open") {p.setPen(palette().text().color());p.drawRect(bar);}
        const auto text=QStringLiteral("%1\n%2 秒 → %3\n房间代际 %4 · 绑定 %5 · 阈值 %6 ms")
            .arg(QString::fromStdString(event.endpoint_id),QString::number((double(event.begin_us)-micros(origin_))/1e6,'f',3),
                event.end_us?QString::number((double(event.end_us)-micros(origin_))/1e6,'f',3)+QStringLiteral(" 秒 · ")+BoundaryText(event.boundary):BoundaryText(event.boundary))
            .arg(event.room_generation).arg(event.binding_epoch).arg(event.threshold_us/1000);
        tips_.push_back({bar,text});
    }
    const int desired=std::max(130,85+int(rows.size())*27);if(minimumHeight()!=desired)setMinimumHeight(desired);
    if(rows.empty())p.drawText(12,56,QStringLiteral("当前范围无已记录停顿（不代表无丢失）"));
    const auto axis=QStringLiteral("%1 秒 → %2 秒    裁剪 %3 · 管线丢弃 %4")
        .arg(std::max(0.0,std::chrono::duration<double>(begin_-origin_).count()),0,'f',1)
        .arg(std::max(0.0,std::chrono::duration<double>(end_-origin_).count()),0,'f',1).arg(frame_->omitted).arg(frame_->admissionDrops);
    p.setPen(palette().text().color());p.drawText(12,height()-10,axis);
}
void TelemetryStallWidget::mouseMoveEvent(QMouseEvent *e) {
    for(const auto &[rect,text]:tips_)if(rect.contains(e->pos())) {QToolTip::showText(e->globalPos(),text,this);return;}
}
TelemetryTimelineWidget::TelemetryTimelineWidget(QWidget *parent):QWidget(parent) {
    AppTheme::applySurfacePalette(*this,AppTheme::Tone::Dark);setMinimumHeight(340);setMouseTracking(true);
    setAccessibleName(QStringLiteral("会话操作并行泳道；选择阶段查看测量端点和安全原因"));
}
void TelemetryTimelineWidget::SetFrame(TimelineView frame) {
    frame_=std::move(frame);hits_.clear();itemY_.assign(frame_.items.size(),0);
    std::array<std::vector<std::uint64_t>,6> rowEnds;
    for(size_t i=0;i<frame_.items.size();++i) {
        const auto &item=frame_.items[i];if(!item.begin)continue;
        auto &rows=rowEnds[size_t(item.lane)];
        const auto free=std::find_if(rows.begin(),rows.end(),[&](auto end){return end<item.begin;});
        const auto index=std::distance(rows.begin(),free);
        if(free==rows.end())rows.push_back(item.end);else *free=item.end;
        itemY_[i]=int(index)*12;
    }
    int y=34;
    for(size_t lane=0;lane<6;++lane) {
        laneY_[lane]=y;laneHeight_[lane]=std::max(46,int(rowEnds[lane].size())*12+15);y+=laneHeight_[lane];
    }
    setMinimumHeight(y+30);update();
}
QString TelemetryTimelineWidget::Description(size_t index) const {
    const auto &item=frame_.items[index];const auto &first=frame_.source->events[item.first],&last=frame_.source->events[item.last];
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
    QPainter p(this);p.fillRect(rect(),palette().base());p.setPen(palette().text().color());hits_.clear();
    const QStringList names={QStringLiteral("入会 / 启动"),QStringLiteral("信令连接"),QStringLiteral("本地发布"),
        QStringLiteral("接收 / 首帧"),QStringLiteral("重连恢复"),QStringLiteral("其他事件")};
    const double left=110,right=std::max(left+1,double(width()-40)),span=std::max(1.0,double(frame_.end-frame_.begin));
    const auto x=[&](std::uint64_t time){return left+(right-left)*(time-frame_.begin)/span;};
    for(int lane=0;lane<6;++lane) {
        const int top=laneY_[lane];p.setPen(palette().mid().color());p.drawLine(QPointF(left,top+laneHeight_[lane]-13),QPointF(right,top+laneHeight_[lane]-13));
        p.setPen(palette().text().color());p.drawText(QRect(8,top,98,35),Qt::AlignVCenter|Qt::TextWordWrap,names[lane]);
    }
    if(frame_.items.empty()) {p.drawText(rect(),Qt::AlignCenter,QStringLiteral("所选操作暂无保留事件"));return;}
    for(size_t i=0;i<frame_.items.size();++i) {
        const auto &item=frame_.items[i];const size_t lane=size_t(item.lane);
        if(!item.begin)continue; // Unknown time remains in the event table, never at t=0.
        const double y=laneY_[lane]+2+itemY_[i];
        QRectF box(x(item.begin),y,std::max(5.0,x(item.end)-x(item.begin)),8);
        QColor color=property("chartPrimaryColor").value<QColor>();
        const auto &last=frame_.source->events[item.last];
        if(last.outcome=="failure" || last.outcome=="timeout")color=palette().brightText().color();
        p.setPen(color);
        if(item.hasBegin && item.hasEnd)p.fillRect(box,color);
        else if(item.milestone) {p.drawEllipse(box.topLeft()+QPointF(2,4),3,3);}
        else {p.drawRect(box);p.drawText(QPointF(box.right()+2,y+8),item.hasBegin?QStringLiteral("→"):QStringLiteral("?"));}
        hits_.push_back({box.adjusted(-3,-3,3,3),i});
    }
    p.setPen(palette().text().color());
    p.drawText(QRectF(left,5,right-left,23),Qt::AlignLeft,QStringLiteral("0 秒"));
    p.drawText(QRectF(left,5,right-left,23),Qt::AlignRight,QStringLiteral("%1 秒（所选事件跨度）").arg(double(frame_.end-frame_.begin)/1e6,0,'f',3));
    const auto unknown=std::count_if(frame_.items.begin(),frame_.items.end(),[](const auto &item){return !item.begin;});
    p.drawText(10,height()-13,QStringLiteral("实线：配对阶段 · 圆点：里程碑 · →：未见结束 · ?：缺开始 · 时间未知 %1 条（详见事件表）").arg(unknown));
}
void TelemetryTimelineWidget::mousePressEvent(QMouseEvent *event) {
    for(auto it=hits_.rbegin();it!=hits_.rend();++it)if(it->first.contains(event->pos())) {
        const auto text=Description(it->second);setAccessibleDescription(text);if(selected_)selected_(text);return;
    }
}
void TelemetryTimelineWidget::mouseMoveEvent(QMouseEvent *event) {
    for(auto it=hits_.rbegin();it!=hits_.rend();++it)if(it->first.contains(event->pos())) {QToolTip::showText(event->globalPos(),Description(it->second),this);return;}
    QToolTip::hideText();
}
} // namespace MeetingUI
