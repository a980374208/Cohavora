#include "telemetry_panel_controller.h"
#include <QtCore/QThread>
#include <algorithm>
namespace MeetingUI {
TelemetryPanelController::TelemetryPanelController(
    std::weak_ptr<livekit::telemetry::TelemetryHistoryStore> store,QObject *parent)
: TelemetryPanelController([store] {
    auto locked=store.lock();
    return locked ? locked->CurrentRecords() : std::vector<livekit::telemetry::SafeTelemetryRecordPtr>{};
},parent) {}
TelemetryPanelController::TelemetryPanelController(Reader reader,QObject *parent)
: QObject(parent),reader_(std::move(reader)) {
    timer_.setInterval(1000);
    connect(&timer_,&QTimer::timeout,this,&TelemetryPanelController::Refresh);
}
void TelemetryPanelController::BindSession(SessionKey key) {
    Q_ASSERT(thread()==QThread::currentThread());
    timer_.stop(); previous_.reset(); key_=key; model_.BindSession(std::move(key)); seconds_=0; keepCollecting_=false; stopped_=false;
    if(visible_) { Refresh(); timer_.start(); }
}
void TelemetryPanelController::SetView(TelemetryPage page,int seconds) {
    page_=page; seconds_=seconds==60 || seconds==300?seconds:0;
    model_.SetHistoryWindow(seconds_);
    if(visible_ && !stopped_) Publish(true);
}
void TelemetryPanelController::SetPresentationVisible(bool visible) {
    Q_ASSERT(thread()==QThread::currentThread());
    if(stopped_ || visible_==visible) return;
    visible_=visible;
    if(visible) { Refresh(); timer_.start(); } else if(!keepCollecting_) timer_.stop();
}
void TelemetryPanelController::SetKeepCollecting(bool enabled) {
    Q_ASSERT(thread()==QThread::currentThread());
    if(stopped_) return;
    keepCollecting_=enabled;
    if(keepCollecting_ || visible_) timer_.start(); else timer_.stop();
}
void TelemetryPanelController::Stop() { timer_.stop(); visible_=false; stopped_=true; keepCollecting_=false; seconds_=0; previous_.reset(); model_.Clear(); }
void TelemetryPanelController::Refresh() {
    if((!visible_ && !keepCollecting_) || stopped_) return;
    const auto records=reader_(); model_.Reconcile(records);
    // Persistent collection must not refresh hidden tables or chart widgets.
    if(visible_) Publish();
}
void TelemetryPanelController::Publish(bool forceMemory) {
    const auto now=TelemetryClock::now();
    if(page_==TelemetryPage::Video || page_==TelemetryPage::Timeline) {
        if(const auto pipeline=livekit::diagnostic::InstalledBusinessPipeline())
            model_.ReconcileTimeline(pipeline->RecentTimeline(key_.anonymousId,key_.generation));
    }
    auto frame=std::make_shared<PanelFrame>(*model_.BuildFrame(seconds_,now));
    bool refresh=forceMemory || !previous_ || frame->waiting || now-memoryPublishedAt_>=std::chrono::seconds(5);
    for(auto id:{MetricId::PrivateMemory,MetricId::WorkingSet}) {
        const auto i=static_cast<size_t>(id);
        if(previous_ && (previous_->readings[i].display!=frame->readings[i].display
            || previous_->readings[i].native!=frame->readings[i].native)) refresh=true;
    }
    if(refresh) memoryPublishedAt_=now;
    else for(auto id:{MetricId::PrivateMemory,MetricId::WorkingSet}) {
        const auto i=static_cast<size_t>(id);
        const auto fresh=frame->readings[i];
        frame->readings[i]=previous_->readings[i];
        frame->readings[i].freshnessAt=fresh.sampledAt;
        frame->readings[i].display=fresh.display;
        frame->series[i]=previous_->series[i];
        auto &points=frame->series[i].points;
        points.erase(std::remove_if(points.begin(),points.end(),[&](const SeriesPoint &p){return p.time<frame->begin;}),points.end());
    }
    previous_=frame;
    emit FrameReady(std::move(frame));
}
} // namespace MeetingUI
