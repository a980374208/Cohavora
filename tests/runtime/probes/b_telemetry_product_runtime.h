#pragma once
#include "src/core/room.h"
#include "src/core/meeting_session_runtime.h"
#include "src/telemetry/diagnostic_pipeline.h"
#include "src/telemetry/process_resource_sampler.h"
#include "src/telemetry/telemetry_report.h"
#include "src/ui/diagnostic_qt_bridge.h"
#include "src/ui/meeting_log_console.h"
#include "src/ui/telemetry_live_dialog.h"
#include "src/ui/telemetry_panel_controller.h"
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTimer>
#include <array>
#include <atomic>
#include <windows.h>
#include <aclapi.h>

struct BTelemetryTiming final {
    // Fixed 100ns buckets, with explicit >=1ms overflow. No RT allocation,
    // locks, file writes or Qt calls. Start/stop gates delimit measured windows.
    std::array<std::atomic<std::uint64_t>,10001> histogram{};
    std::atomic<bool> active{false};
    std::atomic<std::uint64_t> maximum_ns{0};
    void record(std::chrono::nanoseconds duration) noexcept {
        if (!active.load(std::memory_order_relaxed)) return;
        const auto index=(std::min<std::uint64_t>)(10000,static_cast<std::uint64_t>((std::max<std::int64_t>)(0,duration.count()))/100);
        histogram[index].fetch_add(1,std::memory_order_relaxed);
        const auto elapsed=static_cast<std::uint64_t>((std::max<std::int64_t>)(0,duration.count()));
        auto previous=maximum_ns.load(std::memory_order_relaxed);
        while (previous<elapsed && !maximum_ns.compare_exchange_weak(previous,elapsed,std::memory_order_relaxed)) {}
    }
    void reset() noexcept {
        active.store(false,std::memory_order_release);
        for (auto &value:histogram) value.store(0,std::memory_order_relaxed);
        maximum_ns.store(0,std::memory_order_relaxed);
        active.store(true,std::memory_order_release);
    }
    QJsonObject observation() const {
        QJsonObject values;
        std::uint64_t count=0;
        for (std::size_t i=0;i<histogram.size();++i) {
            const auto value=histogram[i].load(std::memory_order_relaxed);
            count+=value;
            if (value) values.insert(QString::number(i),static_cast<qint64>(value));
        }
        return {{"samples",static_cast<qint64>(count)},{"bucket_ns",100},{"overflow_bucket",10000},
            {"maximum_ns",static_cast<qint64>(maximum_ns.load(std::memory_order_relaxed))},{"histogram",values}};
    }
};

namespace livekit {
class RoomConnectAttemptTestAccess final {
public:
    static void installTiming(Room &room,const std::shared_ptr<BTelemetryTiming> &timing,
                              const std::shared_ptr<BTelemetryTiming> &copy,
                              const std::shared_ptr<BTelemetryTiming> &dispatch) {
        std::lock_guard lock(room.room_mutex_);
        auto hooks=std::make_shared<Room::ConnectAttemptTestHooks>();
        hooks->on_native_video_callback_duration=[timing](auto duration) {timing->record(duration);};
        if (qEnvironmentVariableIntValue("B_TELEMETRY_STAGE_TIMING")==1)
            hooks->on_native_video_callback_phase_durations=[copy,dispatch](auto a,auto b) {
                copy->record(a);dispatch->record(b);
            };
        room.connect_attempt_test_hooks_=std::move(hooks);
    }
};
}

namespace MeetingUI {
struct MeetingLogConsoleTestAccess {
    static void instrument(MeetingLogConsoleWindow &window,const std::shared_ptr<BTelemetryTiming> &timing) {
        QObject::disconnect(window._drainTimer,&QTimer::timeout,&window,&MeetingLogConsoleWindow::drainPending);
        QObject::connect(window._drainTimer,&QTimer::timeout,&window,[&window,timing] {
            const auto start=std::chrono::steady_clock::now();
            window.drainPending();timing->record(std::chrono::steady_clock::now()-start);
        });
    }
    static QJsonObject observe(MeetingLogConsoleWindow &window) {
        QJsonObject value{{"cache_bytes",static_cast<qint64>(window._cacheBytes)},
            {"cache_limit",static_cast<qint64>(window.kMaxCacheBytes)},
            {"entries",static_cast<int>(window._logEntries.size())},
            {"entry_limit",static_cast<int>(window.kMaxLogEntries)},
            {"timer_interval_ms",window._drainTimer->interval()}};
        std::lock_guard lock(window._queue->mutex);
        value.insert("pending_entries",static_cast<int>(window._queue->pending.size()));
        value.insert("pending_bytes",static_cast<qint64>(window._queue->pendingBytes));
        value.insert("pending_limit",static_cast<qint64>(window.kMaxPendingBytes));
        value.insert("dropped",static_cast<qint64>(window._queue->dropped));
        return value;
    }
};
struct TelemetryPanelControllerTestAccess {
    static void instrument(TelemetryPanelController &controller,const std::shared_ptr<BTelemetryTiming> &timing) {
        QObject::disconnect(&controller.timer_,&QTimer::timeout,&controller,nullptr);
        QObject::connect(&controller.timer_,&QTimer::timeout,&controller,[&controller,timing] {
            const auto start=std::chrono::steady_clock::now();
            controller.Refresh();timing->record(std::chrono::steady_clock::now()-start);
        });
    }
};
}

class BTelemetryProductObserver final {
public:
    using Emit=std::function<void(const char*,QJsonObject)>;
    explicit BTelemetryProductObserver(const QString &root,bool disabled,Emit record)
        : disabled_(disabled),emit_(std::move(record)),output_(root+"/telemetry-samples.jsonl") {
        if (!output_.open(QIODevice::WriteOnly|QIODevice::NewOnly)) throw std::runtime_error("telemetry_evidence_open_failed");
        makePrivateDirectory(std::filesystem::path((root+"/diagnostics").toStdWString()));
        makePrivateDirectory(std::filesystem::path((root+"/telemetry-history").toStdWString()));
        pipeline_=std::make_shared<livekit::diagnostic::DiagnosticPipeline>();
        livekit::diagnostic::InstallBusinessPipeline(pipeline_);
        bridge_=std::make_unique<MeetingUI::DiagnosticQtBridge>(pipeline_);
        if (!pipeline_->StartWriter(std::filesystem::path((root+"/diagnostics").toStdWString())))
            throw std::runtime_error("diagnostic_writer_start_failed");
        history_=std::make_shared<livekit::telemetry::TelemetryHistoryStore>(
            std::filesystem::path((root+"/telemetry-history").toStdWString()),std::string(pipeline_->run_id()),
            std::filesystem::path((root+"/diagnostics").toStdWString()));
        livekit::telemetry::InstallTelemetryHistoryStore(history_);
        history_->SetHistoryEnabled(!disabled_);
        pipeline_->SetRetentionEnabled(!disabled_);
        livekit::telemetry::SetFineRenderStatisticsEnabled(!disabled_);
        console_=&MeetingUI::MeetingLogConsoleWindow::Instance();
        MeetingUI::MeetingLogConsoleTestAccess::instrument(*console_,uiTiming_);
        console_->show();
        QObject::connect(&timer_,&QTimer::timeout,&timer_,[this] {
            if (disabled_) pipeline_->PauseProductionForBenchmark(std::chrono::seconds(60));
            sample();
        });
        timer_.setInterval(1000);timer_.start();sample();
    }
    ~BTelemetryProductObserver() {
        timer_.stop();
        if (dialog_) delete dialog_.data();
        bridge_.reset();
        MeetingUI::MeetingLogConsoleWindow::DestroyInstance();
        livekit::telemetry::InstallTelemetryHistoryStore({});
        livekit::diagnostic::InstallBusinessPipeline({});
        history_->Close();pipeline_->Close();
    }
    void afterAdmission(const std::shared_ptr<livekit::Room> &room) {
        if (room) livekit::RoomConnectAttemptTestAccess::installTiming(*room,videoTiming_,copyTiming_,dispatchTiming_);
        stopped_=false;
    }
    void stopSampling(const std::shared_ptr<OpenMeeting::MeetingSessionRuntime> &session) {
        if (!disabled_ || !session) {emit_("telemetry_command_failed",{});return;}
        const auto weak=std::weak_ptr<livekit::telemetry::SessionTelemetry>(session->telemetry());
        const auto gate=QPointer<QTimer>(&timer_);
        session->post([weak,gate,this] {
            if (const auto telemetry=weak.lock()) telemetry->StopOnStrand([gate,this,weak] {
                const auto owner=weak.lock();
                if (!owner) return;
                const auto snapshot=owner->SnapshotOnStrand();
                QMetaObject::invokeMethod(gate,[gate,this,snapshot] {
                    if (!gate) return;
                    stopped_=true;stoppedTerminal_=snapshot->session_complete;stoppedInFlight_=snapshot->stats_in_flight;
                    emit_("telemetry_sampling_stopped",{{"actual_stop_on_strand",true},
                        {"native_terminal",stoppedTerminal_},{"stats_in_flight",stoppedInFlight_}});
                },Qt::QueuedConnection);
            });
        });
    }
    bool handle(const QJsonObject &command) {
        const auto action=command.value("action").toString();
        if (action=="telemetry_begin") {
            phase_=command.value("phase").toString();
            videoTiming_->reset();uiTiming_->reset();refreshTiming_->reset();
            copyTiming_->reset();dispatchTiming_->reset();
            emit_("telemetry_window_started",{{"sequence",command.value("sequence")},{"phase",phase_}});
        } else if (action=="telemetry_observe") {
            auto value=sample();value.insert("sequence",command.value("sequence"));
            value.insert("callback_timing",videoTiming_->observation());
            value.insert("callback_copy_timing",copyTiming_->observation());
            value.insert("callback_dispatch_timing",dispatchTiming_->observation());
            value.insert("stage_timing_enabled",qEnvironmentVariableIntValue("B_TELEMETRY_STAGE_TIMING")==1);
            value.insert("ui_batch_timing",uiTiming_->observation());
            value.insert("ui_refresh_timing",refreshTiming_->observation());
            emit_("telemetry_observation",value);
        } else if (action=="telemetry_open") {
            if (dialog_) delete dialog_.data();
            const auto records=history_->CurrentRecords();
            if (records.empty()) {emit_("telemetry_command_failed",{});return true;}
            const auto &record=*records.back();
            dialog_=MeetingUI::OpenLiveTelemetryDialog(nullptr,
                {record.anonymous_session_id,record.snapshot.session_generation},history_);
            if (auto *controller=dialog_->findChild<MeetingUI::TelemetryPanelController*>())
                MeetingUI::TelemetryPanelControllerTestAccess::instrument(*controller,refreshTiming_);
            else emit_("telemetry_command_failed",{});
        } else if (action=="telemetry_finalize") {
            timer_.stop();
            if (dialog_) delete dialog_.data();
            const auto history=history_->Close();
            const auto diagnostic=pipeline_->Close();
            emit_("telemetry_store_closed",{{"history_completed",history.state==livekit::telemetry::TelemetryCloseState::Completed},
                {"disk_unknown",history.disk_outcome_unknown},
                {"diagnostic_completed",diagnostic==livekit::diagnostic::DrainResult::Completed}});
        } else if (action=="telemetry_close") {
            if (dialog_) delete dialog_.data();
        } else return false;
        return true;
    }
    QJsonObject sample() {
        const auto resource=resources_.Sample();
        FILETIME created{},exited{},kernel{},user{};
        const bool cpu=GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user)!=FALSE;
        ULARGE_INTEGER k{},u{};k.LowPart=kernel.dwLowDateTime;k.HighPart=kernel.dwHighDateTime;
        u.LowPart=user.dwLowDateTime;u.HighPart=user.dwHighDateTime;
        const auto h=history_->Status();const auto d=pipeline_->GetStatus();
        QJsonObject value{{"utc_ms",QDateTime::currentMSecsSinceEpoch()},{"phase",phase_},
            {"disabled",disabled_},{"sampling_stopped",stopped_},{"pid",static_cast<qint64>(GetCurrentProcessId())},
            {"stopped_native_terminal",stoppedTerminal_},{"stopped_stats_in_flight",stoppedInFlight_},
            {"cpu_available",cpu},{"cpu_seconds",(k.QuadPart+u.QuadPart)/1e7},
            {"logical_processors",static_cast<int>(resource.logical_processor_count)},
            {"private_bytes",static_cast<qint64>(resource.private_bytes)},
            {"working_set_bytes",static_cast<qint64>(resource.working_set_bytes)},
            {"handles",static_cast<int>(resource.handle_count)},{"threads",static_cast<int>(resource.thread_count)},
            {"memory_available",resource.memory_availability==livekit::telemetry::Availability::Valid},
            {"history",QJsonObject{{"memory_bytes",static_cast<qint64>(h->memory_bytes)},
                {"queue_bytes",static_cast<qint64>(h->queue_bytes)},{"queue_depth",static_cast<qint64>(h->queue_depth)},
                {"queue_capacity",static_cast<qint64>(h->queue_capacity)},
                {"queue_byte_capacity",static_cast<qint64>(h->queue_byte_capacity)},
                {"pending_bytes",static_cast<qint64>(h->pending_bytes)},
                {"drops",static_cast<qint64>(h->queue_drops)},{"write_failures",static_cast<qint64>(h->write_failures)},
                {"refresh_count",static_cast<qint64>(h->history_refresh_count)}}},
            {"diagnostic",QJsonObject{{"pending",static_cast<qint64>(d.pending)},
                {"accepted",static_cast<qint64>(d.accepted)},{"written",static_cast<qint64>(d.written)},
                {"drops",static_cast<qint64>(d.dropped_ordinary+d.dropped_critical)},
                {"sink_failures",static_cast<qint64>(d.sink_failures)},
                {"production_paused",pipeline_->ProductionPausedForBenchmark()},{"retention_enabled",d.retention_enabled}}},
            {"console",MeetingUI::MeetingLogConsoleTestAccess::observe(*console_)}};
        const auto records=history_->CurrentRecords();
        if (!records.empty()) {
            const auto &s=records.back()->snapshot;
            value.insert("snapshot",QJsonObject{{"revision",static_cast<qint64>(s.revision)},
                {"session_complete",s.session_complete},{"generation",static_cast<qint64>(s.session_generation)},
                {"queue_depth",static_cast<qint64>(s.queue_depth)},{"queue_capacity",static_cast<qint64>(s.queue_capacity)},
                {"snapshot_publications",static_cast<qint64>(s.telemetry_snapshot_publications)},
                {"stats_in_flight",s.stats_in_flight},{"decoded_frames",static_cast<qint64>(s.inbound_video_frames_decoded)}});
        }
        value.insert("callback_samples",videoTiming_->observation().value("samples"));
        output_.write(QJsonDocument(value).toJson(QJsonDocument::Compact)+'\n');output_.flush();
        return value;
    }
private:
    static void makePrivateDirectory(const std::filesystem::path &path) {
        if (std::filesystem::exists(path)) throw std::runtime_error("private_fixture_directory_must_be_new");
        std::filesystem::create_directories(path);
        HANDLE token=nullptr;
        if (!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)) throw std::runtime_error("fixture_token_query_failed");
        DWORD size=0;GetTokenInformation(token,TokenUser,nullptr,0,&size);
        std::vector<std::byte> buffer(size);
        const bool available=GetTokenInformation(token,TokenUser,buffer.data(),size,&size)!=FALSE;
        CloseHandle(token);
        if (!available) throw std::runtime_error("fixture_owner_query_failed");
        auto *user=reinterpret_cast<TOKEN_USER*>(buffer.data());
        PSID owner=nullptr;PSECURITY_DESCRIPTOR descriptor=nullptr;
        const auto queried=GetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()),SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION,&owner,nullptr,nullptr,nullptr,&descriptor);
        const bool owned=queried==ERROR_SUCCESS && owner && EqualSid(owner,user->User.Sid);
        if (descriptor) LocalFree(descriptor);
        if (!owned) throw std::runtime_error("fixture_directory_owner_mismatch");
        EXPLICIT_ACCESSW access{};access.grfAccessPermissions=FILE_ALL_ACCESS;
        access.grfAccessMode=SET_ACCESS;access.grfInheritance=SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        BuildTrusteeWithSidW(&access.Trustee,user->User.Sid);
        PACL acl=nullptr;
        if (SetEntriesInAclW(1,&access,nullptr,&acl)!=ERROR_SUCCESS) throw std::runtime_error("fixture_ACL_create_failed");
        const auto result=SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()),SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr,nullptr,acl,nullptr);
        LocalFree(acl);
        if (result!=ERROR_SUCCESS) throw std::runtime_error("fixture_private_directory_failed_"+std::to_string(result));
    }
    bool disabled_=false,stopped_=false;
    bool stoppedTerminal_=false,stoppedInFlight_=true;
    Emit emit_;QString phase_="prejoin";QFile output_;QTimer timer_;
    livekit::telemetry::ProcessResourceSampler resources_;
    std::shared_ptr<livekit::diagnostic::DiagnosticPipeline> pipeline_;
    std::shared_ptr<livekit::telemetry::TelemetryHistoryStore> history_;
    std::unique_ptr<MeetingUI::DiagnosticQtBridge> bridge_;
    MeetingUI::MeetingLogConsoleWindow *console_=nullptr;
    QPointer<QDialog> dialog_;
    std::shared_ptr<BTelemetryTiming> videoTiming_=std::make_shared<BTelemetryTiming>();
    std::shared_ptr<BTelemetryTiming> copyTiming_=std::make_shared<BTelemetryTiming>();
    std::shared_ptr<BTelemetryTiming> dispatchTiming_=std::make_shared<BTelemetryTiming>();
    std::shared_ptr<BTelemetryTiming> uiTiming_=std::make_shared<BTelemetryTiming>();
    std::shared_ptr<BTelemetryTiming> refreshTiming_=std::make_shared<BTelemetryTiming>();
};
