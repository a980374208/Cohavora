#include "telemetry_live_dialog.h"
#include "telemetry_panel_controller.h"
#include "telemetry_chart_widgets.h"
#include "telemetry_dialogs.h"
#include "app_theme.h"
#include "whiteboard/accessible_combo_box.h"
#include "core/meeting_coordinator.h"
#include <QtWidgets/QDialog>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QHeaderView>
#include <QtGui/QShowEvent>
#include <QtGui/QHideEvent>
#include <QtGui/QCloseEvent>
#include <set>
#include <limits>
#include <algorithm>
#include <QtCore/QSignalBlocker>

namespace MeetingUI {
namespace {
class LiveTelemetryDialog final : public QDialog {
public:
    LiveTelemetryDialog(QWidget *parent,SessionKey key,
        std::weak_ptr<livekit::telemetry::TelemetryHistoryStore> store)
    :QDialog(parent),key_(std::move(key)),store_(std::move(store)) {
        setObjectName(QStringLiteral("telemetryLiveDialog"));setAttribute(Qt::WA_DeleteOnClose);
        AppTheme::configureModelessWindow(*this);AppTheme::applySurfacePalette(*this,AppTheme::Tone::Dark);
        setWindowFlag(Qt::WindowMinimizeButtonHint,false);
        AppTheme::setStyleVariant(*this,"telemetry-panel");
        setWindowTitle(QStringLiteral("会议遥测 · 实时"));resize(960,740);setMinimumSize(680,480);
        auto *layout=new QVBoxLayout(this);
        summary_=new QLabel(this);summary_->setWordWrap(true);summary_->setObjectName(QStringLiteral("telemetryLiveSummary"));layout->addWidget(summary_);
        auto *toolbar=new QHBoxLayout;
        range_=createAccessibleComboBox(this);range_->setObjectName(QStringLiteral("telemetryTimeWindow"));
        range_->addItem(QStringLiteral("默认"),0);
        range_->addItem(QStringLiteral("60 秒"),60);range_->addItem(QStringLiteral("5 分钟"),300);
        range_->setToolTip(QStringLiteral("默认使用现有历史缓存；固定时长使用独立图表缓存，数据不足时逐步积累。"));
        range_->setAccessibleName(QStringLiteral("遥测时间范围"));toolbar->addWidget(range_);
        persistent_=new QCheckBox(QStringLiteral("持久化"),this);
        persistent_->setObjectName(QStringLiteral("telemetryPersistHistory"));
        persistent_->setToolTip(QStringLiteral("仅本次会议：关闭窗口后继续采样，重开保留所选档位和图表；离会即清空，不保存到磁盘。"));
        toolbar->addWidget(persistent_);
        retained_=new QLabel(this);retained_->setObjectName(QStringLiteral("telemetryRetainedHistory"));toolbar->addWidget(retained_,1);
        auto *legacy=new QPushButton(QStringLiteral("完整指标、导出与历史…"),this);toolbar->addWidget(legacy);
        layout->addLayout(toolbar);
        tabs_=new QTabWidget(this);tabs_->setObjectName(QStringLiteral("telemetryLiveTabs"));layout->addWidget(tabs_,1);
        const QStringList names={QStringLiteral("网络"),QStringLiteral("声音"),QStringLiteral("资源"),QStringLiteral("视频诊断"),QStringLiteral("会话时间线")};
        for(int page=0;page<5;++page) {
            auto *scroll=new QScrollArea(tabs_);scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);
            auto *content=new QWidget(scroll);auto *vertical=new QVBoxLayout(content);auto *grid=new QGridLayout;
            vertical->addLayout(grid,1);
            if(page==4) {
                timelineStatus_=new QLabel(content);timelineStatus_->setWordWrap(true);
                timelineStatus_->setObjectName(QStringLiteral("telemetryTimelineStatus"));grid->addWidget(timelineStatus_,0,0);
                timelineFilter_=createAccessibleComboBox(content);timelineFilter_->setObjectName(QStringLiteral("telemetryTimelineFilter"));
                timelineFilter_->addItem(QStringLiteral("整个已保留会话"),QString());grid->addWidget(timelineFilter_,1,0);
                timelineLanes_=new TelemetryTimelineWidget(content);timelineLanes_->setObjectName(QStringLiteral("telemetryTimelineLanes"));grid->addWidget(timelineLanes_,2,0);
                timelineDetail_=new QLabel(QStringLiteral("点击泳道阶段或事件行查看测量端点、关联与安全原因"),content);
                timelineDetail_->setObjectName(QStringLiteral("telemetryTimelineDetail"));timelineDetail_->setWordWrap(true);grid->addWidget(timelineDetail_,3,0);
                timelineLanes_->SetSelectionHandler([this](const QString &text){timelineDetail_->setText(text);});
                connect(timelineFilter_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](int){timelineSequence_=~std::uint64_t{};PresentTimeline(timelineSource_);});
                timelineTable_=new QTableWidget(content);timelineTable_->setObjectName(QStringLiteral("telemetryTimeline"));
                timelineTable_->setColumnCount(5);timelineTable_->setHorizontalHeaderLabels({QStringLiteral("相对时间"),QStringLiteral("事件"),QStringLiteral("操作"),QStringLiteral("父操作"),QStringLiteral("状态")});
                timelineTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);timelineTable_->verticalHeader()->hide();
                timelineTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
                timelineTable_->horizontalHeader()->setStretchLastSection(true);timelineTable_->setMinimumHeight(440);grid->addWidget(timelineTable_,4,0);
                connect(timelineTable_,&QTableWidget::cellClicked,this,[this](int row,int column){if(auto *item=timelineTable_->item(row,column))timelineDetail_->setText(item->toolTip());});
                auto *note=new QLabel(QStringLiteral("按事件浏览整个已保留会话；并行操作通过父操作关联，不将各阶段耗时相加。\n缺少开始/结束的操作不推算耗时。关闭页面不删除会话证据，重开会重新装载；裁剪不等于没有发生。"),content);
                note->setWordWrap(true);grid->addWidget(note,5,0);
                scroll->setWidget(content);tabs_->addTab(scroll,names[page]);continue;
            }
            auto chart=[&](std::initializer_list<MetricId> ids,int row,int column,int span) {
                auto *widget=new TelemetryTimeSeriesWidget(content);
                grid->addWidget(widget,row,column,1,span);charts_.push_back({page,widget,std::vector<MetricId>(ids)});
            };
            if(page==0) {
                chart({MetricId::Receive,MetricId::Send},0,0,2);
                chart({MetricId::Rtt,MetricId::Jitter},1,0,1);chart({MetricId::Loss,MetricId::Retransmit},1,1,1);
            } else if(page==1) {
                chart({MetricId::AudioDelay,MetricId::AudioTarget},0,0,2);chart({MetricId::Concealment},1,0,2);
            } else if(page==2) {
                chart({MetricId::Cpu},0,0,1);chart({MetricId::UiLag},0,1,1);
                chart({MetricId::PrivateMemory,MetricId::WorkingSet},1,0,2);
            } else {
                video_=new QLabel(content);video_->setWordWrap(true);grid->addWidget(video_,0,0,1,2);
                chart({MetricId::WindowSubmitFps},1,0,2);
                histogramMode_=createAccessibleComboBox(content);histogramMode_->setObjectName(QStringLiteral("telemetryHistogramMode"));
                histogramMode_->addItems({QStringLiteral("粗桶 · 个数"),QStringLiteral("粗桶 · 占比"),QStringLiteral("细桶 · 个数"),QStringLiteral("细桶 · 占比")});
                grid->addWidget(histogramMode_,2,0,1,2);
                histogram_=new TelemetryHistogramWidget(content);histogram_->setObjectName(QStringLiteral("telemetryHistogram"));grid->addWidget(histogram_,3,0,1,2);
                connect(histogramMode_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](int mode){histogram_->SetMode(mode>=2,mode%2!=0);});
                stalls_=new TelemetryStallWidget(content);stalls_->setObjectName(QStringLiteral("telemetryStalls"));grid->addWidget(stalls_,4,0,1,2);
                bars_=new TelemetryStageBarsWidget(content);bars_->setObjectName(QStringLiteral("telemetryStageBars"));grid->addWidget(bars_,5,0,1,2);
                auto *note=new QLabel(QStringLiteral("各阶段为独立累计最大值，不可相加；CPU 提交/阻塞计时不等于 GPU 执行。\nDX11 可合并 Render + Present，无独立采样的阶段显示缺测。"),content);
                note->setWordWrap(true);grid->addWidget(note,6,0,1,2);
            }
            auto *details=new QTableWidget(content);details->setColumnCount(4);
            details->setHorizontalHeaderLabels({QStringLiteral("指标"),QStringLiteral("当前值"),QStringLiteral("状态 / 数据龄"),QStringLiteral("口径")});
            details->setEditTriggers(QAbstractItemView::NoEditTriggers);details->verticalHeader()->hide();
            details->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
            details->horizontalHeader()->setStretchLastSection(true);details->setMinimumHeight(240);
            details->setObjectName(QStringLiteral("telemetryLiveDetails%1").arg(page));
            const int start=page==0?0:page==1?7:page==2?10:17;
            const int end=page==0?7:page==1?10:page==2?17:int(MetricId::Count);
            details->setRowCount(end-start);
            for(int id=start;id<end;++id) {
                for(int col=0;col<4;++col)details->setItem(id-start,col,new QTableWidgetItem);
                details->item(id-start,0)->setText(QString::fromUtf8(TelemetryMetric(static_cast<MetricId>(id)).label));
                details->item(id-start,3)->setText(QString::fromUtf8(TelemetryMetric(static_cast<MetricId>(id)).semantics));
            }
            detailTables_[page]=details;
            auto *toggle=new QPushButton(QStringLiteral("展开指标口径"),content);toggle->setCheckable(true);
            connect(toggle,&QPushButton::toggled,details,&QWidget::setVisible);
            connect(toggle,&QPushButton::toggled,this,[this,toggle](bool shown){
                toggle->setText(shown ? QStringLiteral("收起指标口径") : QStringLiteral("展开指标口径"));
                if(shown && lastFrame_)Present(lastFrame_);
            });details->hide();
            vertical->addWidget(toggle);vertical->addWidget(details);
            scroll->setWidget(content);tabs_->addTab(scroll,names[page]);
        }
        AppTheme::styleChoiceControls(*this,AppTheme::Tone::Dark);
        controller_=new TelemetryPanelController(store_,this);controller_->BindSession(key_);
        connect(persistent_,&QCheckBox::toggled,this,[this](bool checked) {
            setAttribute(Qt::WA_DeleteOnClose,!checked);
            controller_->SetKeepCollecting(checked);
        });
        connect(controller_,&TelemetryPanelController::FrameReady,this,[this](PanelFramePtr frame){Present(std::move(frame));});
        auto view=[this] {
            histogramPublished_=false;
            range_->setVisible(tabs_->currentIndex()!=4);
            controller_->SetView(static_cast<TelemetryPage>(tabs_->currentIndex()),range_->currentData().toInt());
        };
        connect(tabs_,&QTabWidget::currentChanged,this,[view](int){view();});
        connect(range_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[view](int){view();});
        connect(legacy,&QPushButton::clicked,this,[this] {
            QVariantMap projection;
            if(auto store=store_.lock()) {
                const auto records=store->CurrentRecords();
                for(auto it=records.rbegin();it!=records.rend();++it) {
                    if((*it)->anonymous_session_id==key_.anonymousId && (*it)->snapshot.session_generation==key_.generation) {
                        projection=OpenMeeting::ProjectTelemetrySnapshot((*it)->snapshot);break;
                    }
                }
            }
            auto *dialog=OpenTelemetryDetailsDialog(this,projection);
            dialog->setWindowTitle(QStringLiteral("完整遥测快照 · 导出与历史（打开时数据）"));
        });
    }
    void EndSession() {
        persistent_->setChecked(false);
        controller_->Stop();
        close();
    }
protected:
    void showEvent(QShowEvent *e) override {QDialog::showEvent(e);controller_->SetPresentationVisible(!isMinimized());}
    void hideEvent(QHideEvent *e) override {controller_->SetPresentationVisible(false);QDialog::hideEvent(e);}
    void changeEvent(QEvent *e) override {
        QDialog::changeEvent(e);
        if(controller_ && e->type()==QEvent::WindowStateChange)controller_->SetPresentationVisible(isVisible()&&!isMinimized());
    }
    void closeEvent(QCloseEvent *e) override {if(!persistent_->isChecked())controller_->Stop();QDialog::closeEvent(e);}
    void done(int result) override {if(!persistent_->isChecked())controller_->Stop();QDialog::done(result);}
private:
    void Present(PanelFramePtr frame) {
        lastFrame_=frame;
        auto text=[&](MetricId id){return TelemetryReadingText(id,frame->readings[static_cast<size_t>(id)]);};
        summary_->setText(QStringLiteral("接收 %1    发送 %2    RTT %3\nCPU %4    私有内存 %5（内存展示每 5 秒更新）")
            .arg(text(MetricId::Receive),text(MetricId::Send),text(MetricId::Rtt),text(MetricId::Cpu),text(MetricId::PrivateMemory)));
        retained_->setText(frame->waiting?QStringLiteral("等待本会话采样"):
            (range_->currentData().toInt()==0
                ?QStringLiteral("实际保留 %1 秒 · 源版本 %2").arg(frame->retainedSeconds).arg(frame->sourceRevision)
                :QStringLiteral("图表已保留 %1 秒 · 窗口 %2 秒").arg(frame->retainedSeconds).arg(range_->currentData().toInt())));
        if(tabs_->currentIndex()==4)retained_->setText(QStringLiteral("会话证据 · 不受图表时间范围限制"));
        for(const auto &chart:charts_) if(chart.page==tabs_->currentIndex()) {
            auto data=std::make_shared<ChartFrame>();data->begin=frame->begin;data->end=frame->end;
            data->timeOrigin=frame->timeOrigin;
            for(auto id:chart.ids)data->series.push_back(frame->series[static_cast<size_t>(id)]);
            chart.widget->SetFrame(data);
        }
        if(tabs_->currentIndex()==3) {
            video_->setText(QStringLiteral("窗口合计提交率：%1 · 连续绑定：%2\n累计平均提交率：%3（不是屏幕呈现 FPS）\n当前状态：%4 · 累计停顿：%5 / %6\n原生停顿事件丢失 %7（不补造丢失区间）")
                .arg(text(MetricId::WindowSubmitFps),text(MetricId::RenderBindings),text(MetricId::SubmitFps),
                    text(MetricId::StallActive),text(MetricId::StallCount),text(MetricId::StallDuration)).arg(frame->renderEventDrops));
            if(!histogramPublished_ || histogramScope_!=frame->histogram.scope || histogramAvailability_!=frame->histogram.availability
                || frame->end-histogramPublishedAt_>=std::chrono::seconds(5)) {
                histogram_->SetFrame(frame->histogram);histogramPublished_=true;histogramPublishedAt_=frame->end;
                histogramScope_=frame->histogram.scope;histogramAvailability_=frame->histogram.availability;
            }
            stalls_->SetFrame(frame->timeline,frame->begin,frame->end,frame->timeOrigin);
            auto data=std::make_shared<StageBarsFrame>();for(size_t i=0;i<4;++i)data->readings[i]=frame->readings[size_t(MetricId::Convert)+i];bars_->SetFrame(data);
        }
        if(tabs_->currentIndex()==4) PresentTimeline(frame->timeline);
        for(int page=0;page<4;++page) {
            int start=page==0?0:page==1?7:page==2?10:17;
            auto *table=detailTables_[page];if(page!=tabs_->currentIndex() || !table->isVisible())continue;
            for(int row=0;row<table->rowCount();++row) {
                auto id=static_cast<MetricId>(start+row);const auto &r=frame->readings[start+row];
                table->item(row,1)->setText(text(id));
                auto age=r.freshnessAt==TelemetryClock::time_point{}?QStringLiteral("—"):
                    QString::number(std::chrono::duration<double>(frame->end-r.freshnessAt).count(),'f',1)+QStringLiteral(" 秒");
                table->item(row,2)->setText(TelemetryAvailabilityText(r.display)+QStringLiteral(" / ")+age);
                if(id==MetricId::Threads && r.sampledAt!=TelemetryClock::time_point{})
                    table->item(row,2)->setText(TelemetryAvailabilityText(r.display)+QStringLiteral(" / 缓存龄 %1 秒")
                        .arg(std::chrono::duration<double>(frame->end-r.sampledAt).count(),0,'f',1));
                table->item(row,2)->setToolTip(QStringLiteral("原生状态：%1；%2；值版本 %3").arg(TelemetryAvailabilityText(r.native),
                    r.observedTime?QStringLiteral("首次观察时间"):QStringLiteral("源采样时间")).arg(r.sourceRevision));
            }
        }
    }
    void PresentTimeline(TimelineFramePtr frame) {
        timelineSource_=frame;
        if(!frame) {
            timelineStatus_->setText(QStringLiteral("尚无会话事件"));timelineTable_->setRowCount(0);
            timelineLanes_->SetFrame({});timelineDetail_->setText(QStringLiteral("暂无操作详情"));
            QSignalBlocker blocked(timelineFilter_);timelineFilter_->clear();timelineFilter_->addItem(QStringLiteral("整个已保留会话"),QString());
            timelineSequence_=~std::uint64_t{};return;
        }
        const auto last=frame->events.empty()?0:frame->events.back().event_sequence;
        if(last==timelineSequence_ && timelineNativeDrops_==(lastFrame_?lastFrame_->renderEventDrops:0) && frame->events.size()==timelineCount_ && frame->omitted==timelineOmitted_ && frame->admissionDrops==timelineDrops_)return;
        timelineNativeDrops_=lastFrame_?lastFrame_->renderEventDrops:0;
        timelineSequence_=last;timelineCount_=frame->events.size();timelineOmitted_=frame->omitted;timelineDrops_=frame->admissionDrops;
        {
            QSignalBlocker blocked(timelineFilter_);const auto selected=timelineFilter_->currentData().toString();
            timelineFilter_->clear();timelineFilter_->addItem(QStringLiteral("整个已保留会话"),QString());
            int reconnect=0;std::set<std::string> options;
            for(const auto &e:frame->events) {
                if((e.event_name=="admission.started" || e.event_name=="admission.terminal") && options.insert(e.operation_id).second)
                    timelineFilter_->addItem(QStringLiteral("入会"),QString::fromStdString(e.operation_id));
                else if((e.event_name=="reconnect.episode.started" || e.event_name=="reconnect.episode.terminal") && options.insert(e.operation_id).second)
                    timelineFilter_->addItem(QStringLiteral("保留重连 %1").arg(++reconnect),QString::fromStdString(e.operation_id));
            }
            auto index=timelineFilter_->findData(selected);
            if(index<0 && !selected.isEmpty()) {timelineFilter_->addItem(QStringLiteral("所选操作已裁剪"),selected);index=timelineFilter_->count()-1;}
            timelineFilter_->setCurrentIndex(std::max(0,index));
        }
        auto view=BuildTimelineView(frame,timelineFilter_->currentData().toString().toStdString());
        std::set<size_t> visible;
        for(const auto &item:view.items){visible.insert(item.first);visible.insert(item.last);}
        timelineLanes_->SetFrame(std::move(view));
        QString admission=QStringLiteral("入会 → 可用：证据未齐");
        for(const auto &terminal:frame->events) {
            if(terminal.event_name!="startup.terminal")continue;
            const auto start=std::find_if(frame->events.begin(),frame->events.end(),[&](const auto &e){return e.event_name=="admission.started" && e.operation_id==terminal.parent_operation_id;});
            if(start==frame->events.end())continue;
            if((terminal.outcome=="success" || terminal.outcome=="degraded_success") && terminal.source_monotonic_us>=start->source_monotonic_us)
                admission=QStringLiteral("入会 → 可用：%1 ms（生命周期入队观测时间，不等于首帧）").arg(double(terminal.source_monotonic_us-start->source_monotonic_us)/1000,0,'f',1);
            else admission=QStringLiteral("入会 → 可用：")+QString::fromStdString(terminal.outcome);
        }
        timelineStatus_->setText(admission+QStringLiteral("\n")+QStringLiteral("会话事件 %1 条 · 裁剪 %2 · 管线丢弃 %3（后两项为进程累计）\n实时页显示已接受事件，磁盘是否完整以导出结果为准；时间从最早保留事件起算。\n原生停顿事件丢失 %4；未记录区间不可推算。")
            .arg(frame->events.size()).arg(frame->omitted).arg(frame->admissionDrops).arg(lastFrame_?lastFrame_->renderEventDrops:0));
        std::uint64_t origin=(std::numeric_limits<std::uint64_t>::max)();std::set<std::string> starts;
        for(const auto &e:frame->events) {if(e.source_monotonic_us)origin=std::min(origin,e.source_monotonic_us);if(e.event_name.ends_with(".started"))starts.insert(e.operation_id);}
        timelineTable_->setRowCount(int(visible.size()));
        int row=0;
        for(const auto index:visible) {
            const auto &e=frame->events[index];
            QString name=QString::fromStdString(e.event_name);
            if(e.event_name=="admission.started")name=QStringLiteral("入会请求接受");
            else if(e.event_name=="admission.terminal")name=QStringLiteral("入会请求结束");
            else if(e.event_name=="room.connect.started")name=QStringLiteral("连接房间开始");
            else if(e.event_name=="room.connect.terminal")name=QStringLiteral("连接房间结束");
            else if(e.event_name=="startup.terminal")name=QStringLiteral("媒体启动结束");
            else if(e.event_name=="reconnect.episode.started")name=QStringLiteral("重连开始");
            else if(e.event_name=="reconnect.episode.terminal")name=QStringLiteral("重连结束");
            else if(e.event_name=="media.first_observed")name=e.measurement_point=="first_decoded"?QStringLiteral("首帧解码"):
                e.measurement_point=="first_pcm"?QStringLiteral("首份 PCM"):QStringLiteral("首次渲染提交");
            else if(e.event_name=="render.stall.interval")name=QStringLiteral("停顿 · ")+QString::fromStdString(e.boundary);
            QString state=QString::fromStdString(e.outcome);
            if((e.event_name.ends_with(".terminal") || e.event_name.ends_with(".completed")) && !starts.count(e.operation_id))state+=QStringLiteral(" · 缺少开始");
            if(e.event_name.ends_with(".started")) {
                const bool terminal=std::any_of(frame->events.begin(),frame->events.end(),[&](const auto &other){return other.operation_id==e.operation_id && (other.event_name.ends_with(".terminal") || other.event_name.ends_with(".completed"));});
                if(!terminal)state+=QStringLiteral(" · 未见结束");
            }
            const QStringList values={e.source_monotonic_us?QStringLiteral("%1 秒").arg(double(e.source_monotonic_us-origin)/1e6,0,'f',3):QStringLiteral("时间未知"),
                name,QString::fromStdString(e.operation_id.substr(0,12)),QString::fromStdString(e.parent_operation_id.substr(0,12)),state};
            for(int col=0;col<5;++col) {
                if(!timelineTable_->item(row,col))timelineTable_->setItem(row,col,new QTableWidgetItem);
                timelineTable_->item(row,col)->setText(values[col]);
                timelineTable_->item(row,col)->setToolTip(QStringLiteral("operation: %1\nparent: %2\n会话代际 %3 · 房间代际 %4 · 绑定 %5 · 事件 %6\n安全原因 %7 / %8 · 结果 %9\n测量端点 %10 · 恢复代际 %11")
                    .arg(QString::fromStdString(e.operation_id),QString::fromStdString(e.parent_operation_id))
                    .arg(e.session_generation).arg(e.room_generation).arg(e.binding_epoch).arg(e.event_sequence)
                    .arg(QString::fromStdString(e.error_code),QString::fromStdString(e.error_layer),QString::fromStdString(e.outcome),QString::fromStdString(e.measurement_point)).arg(e.recovery_epoch));
            }
            ++row;
        }
    }
    struct Chart {int page;TelemetryTimeSeriesWidget *widget;std::vector<MetricId> ids;};
    PanelFramePtr lastFrame_;
    TimelineFramePtr timelineSource_;
    TelemetryTimelineWidget *timelineLanes_=nullptr;
    QComboBox *timelineFilter_=nullptr;QLabel *timelineDetail_=nullptr;
    bool histogramPublished_=false;
    TelemetryClock::time_point histogramPublishedAt_{};
    std::uint64_t histogramScope_=0;
    livekit::telemetry::Availability histogramAvailability_=livekit::telemetry::Availability::Unknown;
    SessionKey key_;
    std::weak_ptr<livekit::telemetry::TelemetryHistoryStore> store_;
    TelemetryPanelController *controller_=nullptr;
    QTabWidget *tabs_=nullptr;QComboBox *range_=nullptr;
    QCheckBox *persistent_=nullptr;
    QLabel *summary_=nullptr,*retained_=nullptr,*video_=nullptr;
    TelemetryStageBarsWidget *bars_=nullptr;
    TelemetryHistogramWidget *histogram_=nullptr;TelemetryStallWidget *stalls_=nullptr;
    QComboBox *histogramMode_=nullptr;QTableWidget *timelineTable_=nullptr;QLabel *timelineStatus_=nullptr;
    std::uint64_t timelineSequence_=~std::uint64_t{},timelineOmitted_=0,timelineDrops_=0,timelineNativeDrops_=0;
    size_t timelineCount_=0;
    std::array<QTableWidget*,4> detailTables_{};
    std::vector<Chart> charts_;
};
}
QDialog *OpenLiveTelemetryDialog(QWidget *parent,SessionKey key,
    std::weak_ptr<livekit::telemetry::TelemetryHistoryStore> store) {
    auto *dialog=new LiveTelemetryDialog(parent,std::move(key),std::move(store));dialog->show();return dialog;
}
void CloseLiveTelemetryDialog(QDialog *dialog) {
    if(auto *live=dynamic_cast<LiveTelemetryDialog*>(dialog)) live->EndSession();
}
} // namespace MeetingUI
