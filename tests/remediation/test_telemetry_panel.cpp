#include "src/ui/telemetry_panel_model.h"
#include "src/ui/telemetry_panel_controller.h"
#include "src/ui/telemetry_live_dialog.h"
#include "src/ui/telemetry_dialogs.h"
#include "src/ui/telemetry_chart_widgets.h"
#include "src/ui/app_theme.h"
#include "tests/support/test_check.h"
#include <QtWidgets/QApplication>
#include <QtWidgets/QDialog>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QCheckBox>
#include <QtCore/QPointer>
#include <QtCore/QEventLoop>
#include <QtCore/QTimer>
#include <QtCore/QTemporaryDir>
#include <QtWidgets/QLabel>
#include <QtWidgets/QTableWidget>
#include <QtGui/QMouseEvent>
#include <cmath>
#include <iostream>
using namespace MeetingUI;
using namespace livekit::telemetry;
namespace {
constexpr size_t Index(MetricId id) {return static_cast<size_t>(id);}
void Pump(int ms) {QEventLoop loop;QTimer::singleShot(ms,&loop,&QEventLoop::quit);loop.exec();}
}
void RunTelemetryPanelContract(QApplication &app) {
    const auto now=TelemetryClock::now();
    const SessionKey key{"0123456789abcdef0123456789abcdef",7};
    auto record=[&](int seconds,std::uint64_t revision) {
        auto r=std::make_shared<SafeTelemetryRecord>();r->anonymous_session_id=key.anonymousId;
        auto &s=r->snapshot;s.session_generation=7;s.revision=revision;s.generated_at=now+std::chrono::seconds(seconds);
        s.last_sample_at=s.last_resource_sample_at=s.generated_at;
        s.stats_stale_after_ms=3000;s.runtime_stale_after_ms=8000;
        s.availability=s.resource_availability=Availability::Valid;s.coverage=1;
        s.inbound_rtp_traffic_availability=Availability::Valid;s.inbound_rtp_bitrate_bps=2000000;
        s.cpu_availability=Availability::Valid;s.process_cpu_percent=0;
        s.thread_count_availability=Availability::Valid;s.process_thread_count=17;s.thread_count_sample_age_ms=5000;
        s.memory_availability=Availability::Valid;s.private_bytes=9007199254740993ULL;
        s.ui_lag_availability=Availability::Valid;s.ui_lag_samples=1;s.last_ui_lag_ms=4;
        s.render_stage_availability=Availability::Valid;s.render_draw_max_us=1000;s.render_draw_samples=0;
        s.render_upload_max_us=2000;s.render_upload_samples=1;
        return r;
    };
    TelemetryPanelModel model;model.BindSession(key);
    std::vector<SafeTelemetryRecordPtr> rows{record(-2,1),record(-1,2),record(0,3)};
    model.Reconcile(rows);auto frame=model.BuildFrame(60,now);
    TEST_CHECK(frame->begin==now-std::chrono::seconds(2));
    TEST_CHECK(frame->series[Index(MetricId::Receive)].points.size()==3);
    TEST_CHECK(*frame->readings[Index(MetricId::Receive)].PlotValue(MetricId::Receive)==2);
    TEST_CHECK(*frame->readings[Index(MetricId::Cpu)].PlotValue(MetricId::Cpu)==0);
    TEST_CHECK(std::get<std::uint64_t>(frame->readings[Index(MetricId::PrivateMemory)].value)==9007199254740993ULL);
    TEST_CHECK(!frame->readings[Index(MetricId::Draw)].PlotValue(MetricId::Draw));
    TEST_CHECK(*frame->readings[Index(MetricId::Upload)].PlotValue(MetricId::Upload)==2);
    TEST_CHECK(frame->series[Index(MetricId::UiLag)].points.size()==1);
    TEST_CHECK(frame->readings[Index(MetricId::Threads)].display==Availability::Valid);
    TEST_CHECK(frame->readings[Index(MetricId::Threads)].sampledAt==now-std::chrono::seconds(5));
    auto stale=model.BuildFrame(60,now+std::chrono::seconds(4));
    TEST_CHECK(stale->sourceRevision==frame->sourceRevision);
    TEST_CHECK(stale->readings[Index(MetricId::Receive)].display==Availability::Stale);
    TEST_CHECK(stale->readings[Index(MetricId::PrivateMemory)].display==Availability::Valid);
    TEST_CHECK(stale->series[Index(MetricId::Receive)].points[0].value.has_value());
    auto replacement=record(0,4);replacement->snapshot.inbound_rtp_bitrate_bps=7000000;
    rows.back()=replacement;model.Reconcile(rows);frame=model.BuildFrame(60,now);
    TEST_CHECK(frame->series[Index(MetricId::Receive)].points.size()==3);
    TEST_CHECK(*frame->series[Index(MetricId::Receive)].points.back().value==7);
    rows.erase(rows.begin());model.Reconcile(rows);frame=model.BuildFrame(60,now);
    TEST_CHECK(frame->series[Index(MetricId::Receive)].points.size()==2);
    TEST_CHECK(frame->series[Index(MetricId::UiLag)].points.front().time==now-std::chrono::seconds(2));
    replacement=std::make_shared<SafeTelemetryRecord>(*replacement); rows.back()=replacement;
    replacement->snapshot.process_cpu_percent=-1;model.Reconcile(rows);frame=model.BuildFrame(60,now);
    TEST_CHECK(!frame->readings[Index(MetricId::Cpu)].PlotValue(MetricId::Cpu));
    replacement=std::make_shared<SafeTelemetryRecord>(*replacement); rows.back()=replacement;
    replacement->snapshot.cpu_availability=Availability::Unsupported;model.Reconcile(rows);frame=model.BuildFrame(60,now);
    TEST_CHECK(frame->readings[Index(MetricId::Cpu)].display==Availability::Unsupported);
    replacement=std::make_shared<SafeTelemetryRecord>(*replacement); rows.back()=replacement;
    replacement->snapshot.session_generation=8;model.Reconcile(rows);frame=model.BuildFrame(60,now);
    TEST_CHECK(frame->sourceRevision==2);
    rows={record(-10,5),record(0,6)};model.Reconcile(rows);frame=model.BuildFrame(60,now);
    TEST_CHECK(frame->series[Index(MetricId::Receive)].points.back().breakBefore);
    auto duplicate=record(0,7);duplicate->snapshot.last_sample_at=rows.back()->snapshot.last_sample_at;
    duplicate->snapshot.inbound_rtp_bitrate_bps=3000000;
    rows.push_back(duplicate);model.Reconcile(rows);frame=model.BuildFrame(60,now);
    TEST_CHECK(frame->series[Index(MetricId::Receive)].points.size()==2);
    TEST_CHECK(*frame->series[Index(MetricId::Receive)].points.back().value==3);
    rows.clear();for(int i=0;i<350;++i)rows.push_back(record(i-349,i+1));
    model.Reconcile(rows);frame=model.BuildFrame(300,now);
    TEST_CHECK(frame->series[Index(MetricId::Receive)].points.size()==300);
    TEST_CHECK(frame->begin==now-std::chrono::seconds(299));
    auto shortWindow=model.BuildFrame(60,now);
    TEST_CHECK(shortWindow->begin==now-std::chrono::seconds(60));
    model.Reconcile({});TEST_CHECK(model.BuildFrame(60,now)->waiting);
    model.BindSession({key.anonymousId,99});model.Reconcile(rows);TEST_CHECK(model.BuildFrame(60,now)->waiting);

    // Elapsed labels keep their origin when history rolls or the view changes.
    TelemetryPanelModel elapsedModel;elapsedModel.BindSession(key);
    std::vector<SafeTelemetryRecordPtr> elapsedRows{record(0,1),record(22,2)};
    elapsedModel.Reconcile(elapsedRows);
    auto elapsedFrame=elapsedModel.BuildFrame(60,now+std::chrono::seconds(22));
    TEST_CHECK(elapsedFrame->timeOrigin==now && elapsedFrame->begin==now);
    elapsedRows={record(22,2),record(82,3)};
    elapsedModel.Reconcile(elapsedRows);
    elapsedFrame=elapsedModel.BuildFrame(60,now+std::chrono::seconds(82));
    TEST_CHECK(elapsedFrame->timeOrigin==now);
    TEST_CHECK(elapsedFrame->begin-elapsedFrame->timeOrigin==std::chrono::seconds(22));
    TEST_CHECK(elapsedFrame->end-elapsedFrame->timeOrigin==std::chrono::seconds(82));
    TEST_CHECK(elapsedModel.BuildFrame(300,now+std::chrono::seconds(82))->timeOrigin==now);
    elapsedModel.Reconcile({});
    TEST_CHECK(elapsedModel.BuildFrame(60,now+std::chrono::seconds(83))->timeOrigin==now);
    elapsedModel.BindSession(key);elapsedModel.Reconcile(elapsedRows);
    TEST_CHECK(elapsedModel.BuildFrame(60,now+std::chrono::seconds(82))->timeOrigin==now+std::chrono::seconds(22));

    // A one-record upstream cache must not evict fixed-window chart samples.
    TelemetryPanelModel buffered;buffered.BindSession(key);buffered.SetHistoryWindow(300);
    std::weak_ptr<const SafeTelemetryRecord> expiredSource;
    for(int second=0;second<=360;++second) {
        std::vector<SafeTelemetryRecordPtr> latest{record(second,second+1)};
        if(second==0) expiredSource=latest.front();
        buffered.Reconcile(latest);
        const auto f=buffered.BuildFrame(300,now+std::chrono::seconds(second));
        TEST_CHECK(f->series[Index(MetricId::Receive)].points.size()<=301);
        if(second>=300) {
            TEST_CHECK(f->end-f->begin==std::chrono::seconds(300));
            TEST_CHECK(f->retainedSeconds==300);
            TEST_CHECK(f->series[Index(MetricId::Receive)].points.size()==301);
        }
    }
    TEST_CHECK(expiredSource.expired());
    buffered.SetHistoryWindow(60);
    auto bufferedFrame=buffered.BuildFrame(60,now+std::chrono::seconds(360));
    TEST_CHECK(bufferedFrame->retainedSeconds==60);
    TEST_CHECK(bufferedFrame->series[Index(MetricId::Receive)].points.size()==61);
    buffered.SetHistoryWindow(0);
    TEST_CHECK(buffered.BuildFrame(0,now+std::chrono::seconds(360))->series[Index(MetricId::Receive)].points.size()==1);
    buffered.SetHistoryWindow(300);
    TEST_CHECK(buffered.BuildFrame(300,now+std::chrono::seconds(360))->retainedSeconds==0);
    TEST_CHECK(buffered.BuildFrame(300,now+std::chrono::seconds(360))->begin==now+std::chrono::seconds(360));
    // Missing input cannot extend old samples or join across a sampling outage.
    buffered.Reconcile({});
    std::vector<SafeTelemetryRecordPtr> resumed{record(370,371)};
    buffered.Reconcile(resumed);
    bufferedFrame=buffered.BuildFrame(300,now+std::chrono::seconds(370));
    TEST_CHECK(bufferedFrame->series[Index(MetricId::Receive)].points.back().breakBefore);
    TEST_CHECK(bufferedFrame->series[Index(MetricId::UiLag)].points.empty());
    buffered.BindSession({key.anonymousId,99});buffered.Reconcile(resumed);
    TEST_CHECK(buffered.BuildFrame(0,now+std::chrono::seconds(370))->waiting);

    // Native window timestamps, scope discontinuities and bounded fine history.
    TelemetryPanelModel video;video.BindSession(key);video.SetHistoryWindow(300);
    auto videoRecord=[&](int second,std::uint64_t scope) {
        auto r=record(second,second+1);auto &s=r->snapshot;
        s.render_window_availability=Availability::Valid;s.render_window_scope_epoch=scope;
        s.render_window_bindings=s.render_window_fine_bindings=2;
        s.render_window_begin=now+std::chrono::seconds(second-1);
        s.render_window_end=now+std::chrono::seconds(second)-std::chrono::milliseconds(100);
        s.render_window_submit_fps=second%2?30:0;s.render_timeline_event_drops=3;
        s.render_window_interval_histogram[2]=27;
        s.render_window_fine_interval_histogram.assign(1001,0);
        s.render_window_fine_interval_histogram[33]=27;
        return r;
    };
    for(int second=1;second<=360;++second) {
        std::vector<SafeTelemetryRecordPtr> input{videoRecord(second,1)};
        video.Reconcile(input);video.Reconcile(input); // Repeated publication is not another interval.
        TEST_CHECK(video.RetainedBytes()<4*1024*1024);
    }
    auto vf=video.BuildFrame(300,now+std::chrono::seconds(360));
    TEST_CHECK(vf->series[Index(MetricId::WindowSubmitFps)].points.back().time==now+std::chrono::milliseconds(359900));
    TEST_CHECK(vf->readings[Index(MetricId::WindowSubmitFps)].PlotValue(MetricId::WindowSubmitFps)==0);
    TEST_CHECK(vf->histogram.counts[2]==300*27 && vf->renderEventDrops==3);
    TEST_CHECK(vf->histogram.fine.size()==1001 && vf->histogram.fine[33]<=300*27 && vf->histogram.fine[33]>=290*27);
    std::vector<SafeTelemetryRecordPtr> nextScope{videoRecord(361,2)};video.Reconcile(nextScope);
    vf=video.BuildFrame(300,now+std::chrono::seconds(361));
    TEST_CHECK(vf->series[Index(MetricId::WindowSubmitFps)].points.back().breakBefore);
    TEST_CHECK(vf->histogram.scope==2 && vf->histogram.counts[2]==27 && vf->histogram.fine[33]==27);
    TEST_CHECK(video.BuildFrame(300,now+std::chrono::seconds(380))->histogram.availability==Availability::Stale);
    auto noFine=videoRecord(362,3);noFine->snapshot.render_window_fine_interval_histogram.clear();
    noFine->snapshot.render_window_fine_bindings=0;nextScope={noFine};video.Reconcile(nextScope);
    TEST_CHECK(video.BuildFrame(300,now+std::chrono::seconds(362))->histogram.fine.empty());
    auto missing=videoRecord(363,3);missing->snapshot.render_window_availability=Availability::WarmingUp;
    nextScope={missing};video.Reconcile(nextScope);
    TEST_CHECK(!video.BuildFrame(300,now+std::chrono::seconds(363))->readings[Index(MetricId::WindowSubmitFps)].PlotValue(MetricId::WindowSubmitFps));

    // Projection rejects other session generations and reserves open intervals under churn.
    livekit::diagnostic::TimelineSnapshot timeline;
    livekit::diagnostic::Event event;
    event.kind=livekit::diagnostic::EventKind::RenderStallInterval;
    event.context.anonymous_session_id.Assign(key.anonymousId);
    event.context.operation_id.Assign(key.anonymousId);
    event.context.session_generation=key.generation;event.context.has_session_generation=true;
    event.context.room_generation=2;event.context.has_room_generation=true;
    event.media_endpoint_id.Assign(key.anonymousId);event.binding_epoch=1;
    event.media_kind=livekit::diagnostic::MediaKind::Video;
    event.source_monotonic_us=2000000;event.interval_begin_us=1500000;
    event.stall_threshold_us=500000;event.stall_boundary=livekit::diagnostic::StallBoundary::Open;
    event.event_sequence=1;timeline.events.push_back(event);
    event.stall_boundary=livekit::diagnostic::StallBoundary::Recovered;event.interval_end_us=2000000;
    for(int i=2;i<700;++i) {event.event_sequence=i;timeline.events.push_back(event);}
    video.ReconcileTimeline(timeline);vf=video.BuildFrame(300,now);
    TEST_CHECK(vf->timeline && vf->timeline->events.size()==512 && vf->timeline->omitted==187);
    TEST_CHECK(vf->timeline->events.front().boundary=="open");
    {
        TelemetryPanelModel maximum;maximum.BindSession(key);maximum.SetHistoryWindow(300);
        std::vector<SafeTelemetryRecordPtr> dense;
        for(int i=1;i<=300;++i)dense.push_back(videoRecord(i,1));
        maximum.Reconcile(dense);maximum.ReconcileTimeline(timeline);
        TEST_CHECK(maximum.RetainedBytes()<4*1024*1024);
        TEST_CHECK(maximum.BuildFrame(300,now+std::chrono::seconds(300))->histogram.fine.size()==1001);
    }
    TEST_CHECK(video.RetainedBytes()<4*1024*1024);
    event.context.session_generation=8;timeline.events={event};video.ReconcileTimeline(timeline);
    TEST_CHECK(video.BuildFrame(300,now)->timeline->events.empty());
    video.Clear();TEST_CHECK(!video.BuildFrame(300,now)->timeline && video.RetainedBytes()==0);

    // Parallel operations retain source endpoints, including child-before-parent order.
    auto lanes=std::make_shared<TimelineFrame>();
    auto timelineEvent=[&](const char *name,const char *id,const char *parent,std::uint64_t time) {
        OperationTimelineEntry e;e.event_name=name;e.operation_id=id;e.parent_operation_id=parent;e.source_monotonic_us=time;
        lanes->events.push_back(e);
    };
    timelineEvent("media.first_observed","first","connect",450);
    timelineEvent("room.connect.started","connect","admission",200);
    timelineEvent("room.connect.terminal","connect","admission",500);
    timelineEvent("media.publish.started","publish","admission",300);
    timelineEvent("media.publish.terminal","publish","admission",400);
    timelineEvent("admission.started","admission","",100);
    timelineEvent("admission.terminal","admission","",600);
    timelineEvent("startup.terminal","orphan","",700);
    timelineEvent("reconnect.episode.started","reconnect","",800);
    auto laneView=BuildTimelineView(lanes);
    TEST_CHECK(laneView.items.size()==6 && laneView.begin==100 && laneView.end==800);
    auto admissionView=BuildTimelineView(lanes,"admission");
    TEST_CHECK(admissionView.items.size()==4 && admissionView.begin==100 && admissionView.end==600);
    TEST_CHECK(admissionView.items[1].lane==TimelineLane::Connect && admissionView.items[1].hasBegin && admissionView.items[1].hasEnd);
    TEST_CHECK(admissionView.items[2].lane==TimelineLane::Publish && admissionView.items[2].begin<admissionView.items[1].end);
    TEST_CHECK(admissionView.items[3].milestone);
    auto reconnectView=BuildTimelineView(lanes,"reconnect");
    TEST_CHECK(reconnectView.items.size()==1 && reconnectView.items[0].hasBegin && !reconnectView.items[0].hasEnd);
    TEST_CHECK(BuildTimelineView(lanes,"unknown").items.empty());
    timelineEvent("media.first_observed","unknown-time","",0);
    auto unknownTime=BuildTimelineView(lanes,"unknown-time");
    TEST_CHECK(unknownTime.items.size()==1 && unknownTime.begin==0 && unknownTime.end==0);
    timelineEvent("room.connect.started","backward","",1000);
    timelineEvent("room.connect.terminal","backward","",900);
    auto backward=BuildTimelineView(lanes,"backward");
    TEST_CHECK(backward.items.size()==2 && !backward.items[0].hasBegin && !backward.items[1].hasEnd);

    int reads=0,frames=0;
    PanelFramePtr delivered;
    TelemetryPanelController controller([&]{++reads;return rows;});
    QObject::connect(&controller,&TelemetryPanelController::FrameReady,[&](PanelFramePtr f){++frames;delivered=std::move(f);});
    controller.BindSession(key);TEST_CHECK(!controller.IsPolling()&&reads==0);
    controller.SetPresentationVisible(true);TEST_CHECK(controller.IsPolling()&&reads==1&&frames==1);
    controller.SetView(TelemetryPage::Audio,300);TEST_CHECK(reads==1&&frames==2);
    auto changed=record(0,400);
    changed->snapshot.private_bytes=128*1048576;
    changed->snapshot.generated_at=changed->snapshot.last_resource_sample_at=TelemetryClock::now();
    rows.back()=changed;
    Pump(1150);TEST_CHECK(reads==2);
    TEST_CHECK(std::get<std::uint64_t>(delivered->readings[Index(MetricId::PrivateMemory)].value)==9007199254740993ULL);
    TEST_CHECK(delivered->readings[Index(MetricId::PrivateMemory)].freshnessAt==changed->snapshot.last_resource_sample_at);
    controller.SetPresentationVisible(false);Pump(1150);TEST_CHECK(reads==2&&!controller.IsPolling());
    changed=std::make_shared<SafeTelemetryRecord>(*changed);changed->snapshot.memory_availability=Availability::Unsupported;rows.back()=changed;
    controller.SetPresentationVisible(true);TEST_CHECK(reads==3);
    TEST_CHECK(delivered->readings[Index(MetricId::PrivateMemory)].display==Availability::Unsupported);
    controller.SetKeepCollecting(true);controller.SetPresentationVisible(false);
    const auto hiddenFrames=frames;
    Pump(1150);TEST_CHECK(reads==4 && frames==hiddenFrames && controller.IsPolling());
    controller.Stop();controller.SetPresentationVisible(true);controller.SetKeepCollecting(true);
    Pump(1150);TEST_CHECK(reads==4&&!controller.IsPolling());

    AppTheme::install(app);
    QTemporaryDir temp;TEST_CHECK(temp.isValid());
    auto store=std::make_shared<TelemetryHistoryStore>(std::filesystem::path(temp.path().toStdWString()));
    auto populated=videoRecord(0,10)->snapshot;
    populated.generated_at=populated.last_sample_at=populated.last_resource_sample_at=TelemetryClock::now();
    populated.private_bytes=64*1048576;
    TEST_CHECK(store->SubmitSnapshot(std::make_shared<const Snapshot>(populated),{},key.anonymousId));
    for(int i=0;i<60 && store->CurrentRecords().empty();++i) Pump(50);
    TEST_CHECK(!store->CurrentRecords().empty());
    QPointer<QDialog> dialog=OpenLiveTelemetryDialog(nullptr,key,store);app.processEvents();
    auto *tabs=dialog->findChild<QTabWidget*>("telemetryLiveTabs");
    auto *range=dialog->findChild<QComboBox*>("telemetryTimeWindow");
    auto *liveController=dialog->findChild<TelemetryPanelController*>();
    auto *persistent=dialog->findChild<QCheckBox*>("telemetryPersistHistory");
    TEST_CHECK(tabs && tabs->count()==5 && range && liveController);
    auto *histogram=dialog->findChild<TelemetryHistogramWidget*>("telemetryHistogram");
    auto *histogramMode=dialog->findChild<QComboBox*>("telemetryHistogramMode");
    TEST_CHECK(histogram && histogramMode && histogramMode->count()==4);
    TEST_CHECK(dialog->findChild<TelemetryStallWidget*>("telemetryStalls"));
    TEST_CHECK(dialog->findChild<QTableWidget*>("telemetryTimeline"));
    TEST_CHECK(dialog->findChild<QComboBox*>("telemetryTimelineFilter"));
    TEST_CHECK(dialog->findChild<TelemetryTimelineWidget*>("telemetryTimelineLanes"));
    TEST_CHECK(persistent && !persistent->isChecked());
    TEST_CHECK(!dialog->isModal() && (dialog->windowFlags()&Qt::WindowTitleHint));
    TEST_CHECK(!(dialog->windowFlags()&Qt::WindowMinimizeButtonHint));
    TEST_CHECK(range->count()==3 && range->currentIndex()==0 && range->currentData().toInt()==0);
    TEST_CHECK(range->itemData(1).toInt()==60 && range->itemData(2).toInt()==300);
    range->setCurrentIndex(2);
    TEST_CHECK(liveController->IsPolling());
    TEST_CHECK(dialog->palette().color(QPalette::Base).lightness()<100);
    TEST_CHECK(dialog->findChild<QLabel*>("telemetryLiveSummary")->text().contains("2.00"));
    auto *resourceDetails=dialog->findChild<QTableWidget*>("telemetryLiveDetails2");
    TEST_CHECK(resourceDetails->item(2,1)->text().isEmpty()); // Hidden details do not format.
    tabs->setCurrentIndex(2);resourceDetails->show();liveController->SetView(TelemetryPage::Resources,300);
    TEST_CHECK(resourceDetails->item(2,1)->text().contains("64.00"));resourceDetails->hide();
    for(int page=0;page<5;++page) {
        tabs->setCurrentIndex(page);app.processEvents();
        TEST_CHECK(range->isVisible()==(page!=4));
        const auto image=dialog->grab().toImage();TEST_CHECK(!image.isNull());
        TEST_CHECK(image.save(QStringLiteral("telemetry-panel-%1-page-%2.png").arg(qEnvironmentVariable("QT_SCALE_FACTOR","1.0")).arg(page)));
    }
    TelemetryTimelineWidget timelineWidget;timelineWidget.resize(900,340);timelineWidget.SetFrame(laneView);
    timelineWidget.show();app.processEvents();
    TEST_CHECK(timelineWidget.grab().save(QStringLiteral("telemetry-timeline-%1.png").arg(qEnvironmentVariable("QT_SCALE_FACTOR","1.0"))));
    QString selectedStage;timelineWidget.SetSelectionHandler([&](const QString &text){selectedStage=text;});
    QMouseEvent stageClick(QEvent::MouseButtonPress,QPointF(200,40),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(&timelineWidget,&stageClick);
    TEST_CHECK(selectedStage.contains("admission") && selectedStage.contains(QStringLiteral("0.5 ms")));
    timelineWidget.hide();
    auto overlaps=std::make_shared<TimelineFrame>();
    for(int i=0;i<6;++i) {
        OperationTimelineEntry e;e.operation_id=std::to_string(i);e.event_name="admission.started";e.source_monotonic_us=100;
        overlaps->events.push_back(e);e.event_name="admission.terminal";e.source_monotonic_us=600;overlaps->events.push_back(e);
    }
    TelemetryTimelineWidget overlapWidget;overlapWidget.SetFrame(BuildTimelineView(overlaps));
    TEST_CHECK(overlapWidget.minimumHeight()>340); // More than three parallel stages remain separate.

    tabs->setCurrentIndex(4);auto laneFrame=std::make_shared<PanelFrame>();laneFrame->timeline=lanes;
    liveController->FrameReady(laneFrame);
    auto *operationFilter=dialog->findChild<QComboBox*>("telemetryTimelineFilter");
    auto *eventTable=dialog->findChild<QTableWidget*>("telemetryTimeline");
    TEST_CHECK(operationFilter->count()==3);
    operationFilter->setCurrentIndex(operationFilter->findData(QStringLiteral("reconnect")));
    TEST_CHECK(eventTable->rowCount()==1);
    operationFilter->setCurrentIndex(operationFilter->findData(QStringLiteral("admission")));
    TEST_CHECK(eventTable->rowCount()==7);
    eventTable->cellClicked(0,0);
    TEST_CHECK(dialog->findChild<QLabel*>("telemetryTimelineDetail")->text().contains(QStringLiteral("安全原因")));
    tabs->setCurrentIndex(3);
    for(int mode=0;mode<4;++mode) {
        histogramMode->setCurrentIndex(mode);app.processEvents();
        TEST_CHECK(!histogram->grab().isNull());
    }
    auto presented=std::make_shared<PanelFrame>();presented->end=TelemetryClock::now();
    presented->histogram.scope=987;presented->histogram.availability=Availability::Valid;
    presented->histogram.end=presented->end;
    liveController->FrameReady(presented);const auto histogramEnd=histogram->SourceEnd();
    auto faster=std::make_shared<PanelFrame>(*presented);faster->end+=std::chrono::seconds(1);faster->histogram.end=faster->end;
    liveController->FrameReady(faster);TEST_CHECK(histogram->SourceEnd()==histogramEnd);
    auto due=std::make_shared<PanelFrame>(*presented);due->end+=std::chrono::seconds(5);due->histogram.end=due->end;
    liveController->FrameReady(due);TEST_CHECK(histogram->SourceEnd()==due->end);
    tabs->setCurrentIndex(0);auto hiddenEnd=histogram->SourceEnd();
    auto hidden=std::make_shared<PanelFrame>(*due);hidden->end+=std::chrono::seconds(10);hidden->histogram.end=hidden->end;
    liveController->FrameReady(hidden);TEST_CHECK(histogram->SourceEnd()==hiddenEnd);
    dialog->hide();TEST_CHECK(!liveController->IsPolling());
    dialog->show();TEST_CHECK(liveController->IsPolling());
    dialog->showMinimized();app.processEvents();TEST_CHECK(!liveController->IsPolling());
    dialog->showNormal();app.processEvents();TEST_CHECK(liveController->IsPolling());
    dialog->close();TEST_CHECK(!liveController->IsPolling());
    QApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);TEST_CHECK(!dialog);
    dialog=OpenLiveTelemetryDialog(nullptr,key,store);app.processEvents();
    TEST_CHECK(dialog->findChild<QComboBox*>("telemetryTimeWindow")->currentIndex()==0);
    QPointer<QDialog> details=OpenTelemetryDetailsDialog(dialog,{});
    TEST_CHECK(!(details->windowFlags()&Qt::WindowMinimizeButtonHint));
    details->close();dialog->close();
    QApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    TEST_CHECK(!details && !dialog);

    dialog=OpenLiveTelemetryDialog(nullptr,key,store);app.processEvents();
    range=dialog->findChild<QComboBox*>("telemetryTimeWindow");
    persistent=dialog->findChild<QCheckBox*>("telemetryPersistHistory");
    liveController=dialog->findChild<TelemetryPanelController*>();
    range->setCurrentIndex(1);persistent->setChecked(true);
    dialog->close();QApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    TEST_CHECK(dialog && !dialog->isVisible() && liveController->IsPolling());
    dialog->showNormal();app.processEvents();
    TEST_CHECK(range->currentData().toInt()==60 && persistent->isChecked());
    // Escape/Reject is also a close path and must obey the persistence policy.
    dialog->reject();QApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    TEST_CHECK(dialog && !dialog->isVisible() && liveController->IsPolling());
    CloseLiveTelemetryDialog(dialog);TEST_CHECK(!liveController->IsPolling());
    QApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);TEST_CHECK(!dialog);

    dialog=OpenLiveTelemetryDialog(nullptr,key,store);app.processEvents();
    persistent=dialog->findChild<QCheckBox*>("telemetryPersistHistory");
    TEST_CHECK(!persistent->isChecked());
    TEST_CHECK(dialog->findChild<QComboBox*>("telemetryTimeWindow")->currentData().toInt()==0);
    persistent->setChecked(true);persistent->setChecked(false);
    dialog->reject();QApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);TEST_CHECK(!dialog);
    TEST_CHECK(store->Close().state==TelemetryCloseState::Completed);
    store.reset();
    std::cout<<"TELEMETRY_PANEL model/history/freshness/controller/five_pages=PASS real_media=NOT_RUN\n";
}
