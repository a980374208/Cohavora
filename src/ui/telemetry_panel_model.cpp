#include "telemetry_panel_model.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace MeetingUI {
namespace diagnostic = livekit::diagnostic;
namespace {
using namespace livekit::telemetry;
constexpr MetricDefinition definitions[] = {
 {MetricId::Receive,"RTP 接收","Mbit/s","各接收流实际采样区间码率之和",1e-6},
 {MetricId::Send,"RTP 发送","Mbit/s","各发送流实际采样区间码率之和",1e-6},
 {MetricId::Rtt,"媒体路径 RTT","ms","选中传输的最大 RTT；非端到端延迟",1},
 {MetricId::Jitter,"接收抖动","ms","接收流抖动最大值",1},
 {MetricId::Loss,"接收丢包","%","区间丢包 / (接收 + 丢包)",100},
 {MetricId::Retransmit,"接收重传","%","区间重传包 / 接收包（含重传）",100},
 {MetricId::Bandwidth,"估计可用发送带宽","Mbit/s","选中传输估计值的最大值；非总发送码率",1e-6},
 {MetricId::AudioDelay,"实际缓冲延迟","ms","区间等待时间 / 区间输出样本数",1},
 {MetricId::AudioTarget,"目标缓冲延迟","ms","按区间输出样本数加权",1},
 {MetricId::Concealment,"音频隐藏比例","%","区间隐藏样本 / 接收样本（含静音）",100},
 {MetricId::Cpu,"进程 CPU","%","已按逻辑处理器数归一化",1},
 {MetricId::UiLag,"UI 探针延迟","ms","探针完成计数变化时观察；时间为首次观察时间",1},
 {MetricId::PrivateMemory,"私有内存","MiB","进程 private bytes",1.0/1048576},
 {MetricId::WorkingSet,"工作集","MiB","进程 working set bytes",1.0/1048576},
 {MetricId::Threads,"线程数","个","原生缓存采样值，采样周期独立于资源刷新",1},
 {MetricId::Handles,"句柄数","个","进程句柄数",1},
 {MetricId::Growth,"私有内存增长斜率","MiB/min","原生资源趋势窗口回归；不随页面时间范围重算",1},
 {MetricId::SubmitFps,"累计平均提交 FPS","FPS","当前探针累计平均提交间隔的倒数；非实时帧率",1},
 {MetricId::StallActive,"当前渲染停顿","","当前绑定的停顿状态",1},
 {MetricId::StallCount,"累计停顿次数","次","当前绑定累计值，绑定变化可重置",1},
 {MetricId::StallDuration,"累计停顿时长","ms","仅累计超出停顿阈值的时长",1},
 {MetricId::Convert,"转换最大耗时","ms","资源级 CPU 计时；独立最大值",0.001},
 {MetricId::Upload,"上传最大耗时","ms","资源级 CPU 计时；独立最大值",0.001},
 {MetricId::Draw,"绘制最大耗时","ms","画布/瓦片 CPU 计时；不等于 GPU 执行",0.001},
 {MetricId::Present,"Present 阻塞最大值","ms","画布 CPU 计时；DX11 可包含 Render + Present",0.001},
 {MetricId::WindowSubmitFps,"窗口合计提交率","帧/s","稳定绑定集合的提交增量 / 实际单调时间；非屏幕呈现 FPS",1},
 {MetricId::RenderBindings,"连续视频绑定","路","当前期望连续提交的绑定数；静态及隐藏内容不计入",1}
};
static_assert(std::size(definitions) == static_cast<size_t>(MetricId::Count));
std::optional<double> Numeric(const PanelMetricValue &value) {
    return std::visit([](const auto &v)->std::optional<double> {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_arithmetic_v<T>) return static_cast<double>(v);
        return {};
    }, value);
}
}
const MetricDefinition &TelemetryMetric(MetricId id) { return definitions[static_cast<size_t>(id)]; }
std::optional<double> MetricReading::PlotValue(MetricId id) const {
    if (display != Availability::Valid) return {};
    auto v = Numeric(value);
    if (!v || !std::isfinite(*v)) return {};
    return *v * TelemetryMetric(id).scale;
}
void TelemetryPanelModel::Clear() {
    std::vector<Sample>().swap(samples_);
    SetHistoryWindow(0);timeOrigin_.reset();++presentationRevision_;
    std::deque<FineSample>().swap(fineHistory_);fineLastEnd_={};timeline_.reset();
}
void TelemetryPanelModel::BindSession(SessionKey key) { key_ = std::move(key); Clear(); }
void TelemetryPanelModel::SetHistoryWindow(int seconds) {
    seconds = seconds == 60 || seconds == 300 ? seconds : 0;
    if (seconds == historySeconds_) return;
    historySeconds_ = seconds;
    chartHistoryBegin_.reset();
    if (!seconds) {
        std::vector<Sample>().swap(chartHistory_);
        return;
    }
    MergeChartHistory();
}
void TelemetryPanelModel::PruneChartHistory(TelemetryClock::time_point now) {
    const auto cutoff=now-std::chrono::seconds(historySeconds_);
    chartHistory_.erase(std::remove_if(chartHistory_.begin(),chartHistory_.end(),
        [&](const Sample &s){return s.generated<cutoff;}),chartHistory_.end());
    // One projection per second, including both window endpoints. No strong
    // snapshot references or unbounded native metric catalogs are retained.
    const auto limit=static_cast<size_t>(historySeconds_+1);
    if(chartHistory_.size()>limit) chartHistory_.erase(chartHistory_.begin(),chartHistory_.end()-limit);
}
void TelemetryPanelModel::MergeChartHistory() {
    if (!historySeconds_ || samples_.empty()) return;
    const auto limit=static_cast<size_t>(historySeconds_+1);
    PruneChartHistory(samples_.back().generated);
    if(chartHistory_.capacity()!=limit) {
        std::vector<Sample> bounded;bounded.reserve(limit);
        bounded.insert(bounded.end(),chartHistory_.begin(),chartHistory_.end());chartHistory_.swap(bounded);
    }
    const auto bucket=[](const Sample &s){return std::chrono::duration_cast<std::chrono::seconds>(s.generated.time_since_epoch()).count();};
    const auto cutoff=samples_.back().generated-std::chrono::seconds(historySeconds_);
    for(const auto &sample:samples_) {
        if(sample.generated<cutoff)continue;
        auto it=std::lower_bound(chartHistory_.begin(),chartHistory_.end(),bucket(sample),
            [&](const Sample &old,auto value){return bucket(old)<value;});
        if(it!=chartHistory_.end() && bucket(*it)==bucket(sample)) {
            if(sample.generated>=it->generated)*it=sample;
            continue;
        }
        auto offset=std::distance(chartHistory_.begin(),it);
        if(chartHistory_.size()==limit) {
            if(offset==0)continue;
            chartHistory_.erase(chartHistory_.begin());--offset;
        }
        chartHistory_.insert(chartHistory_.begin()+offset,sample);
    }
}
void TelemetryPanelModel::Reconcile(std::span<const SafeTelemetryRecordPtr> records) {
    std::vector<Sample> next;
    next.reserve(std::min<size_t>(records.size(),300));
    std::unordered_map<const SafeTelemetryRecord*,const Sample*> cache;
    for(const auto &sample:samples_) if(auto origin=sample.origin.lock()) cache.emplace(origin.get(),&sample);
    for (const auto &record : records) {
        if (!record || key_.anonymousId.empty() || record->anonymous_session_id != key_.anonymousId
            || record->snapshot.session_generation != key_.generation) continue;
        const auto cached=cache.find(record.get());
        if(cached!=cache.end()) { next.push_back(*cached->second); continue; }
        const auto &s = record->snapshot;
        Sample sample{record, s.generated_at, s.revision, s.counter_resets, {}};
        sample.renderEventDrops=s.render_timeline_event_drops;
        auto put = [&](MetricId id, auto value, Availability native, int domain,
                       bool supported = true, bool partial = false) {
            auto &r = sample.readings[static_cast<size_t>(id)];
            using T = decltype(value);
            if constexpr (std::is_same_v<T,bool>) r.value = value;
            else if constexpr (std::is_integral_v<T> && std::is_unsigned_v<T>) r.value = std::uint64_t(value);
            else if constexpr (std::is_integral_v<T>) r.value = std::int64_t(value);
            else r.value = double(value);
            r.sourceRevision = s.revision;
            r.native = r.display = native;
            r.partial = partial;
            r.sampledAt = domain == 0 ? s.last_sample_at : domain == 1 ? s.last_resource_sample_at : s.generated_at;
            r.staleAfterMs = domain == 0 ? s.stats_stale_after_ms : s.runtime_stale_after_ms;
            const auto number = Numeric(r.value);
            if (native == Availability::Valid && (!supported || !number || !std::isfinite(*number)
                || (*number < 0 && id != MetricId::Growth))) r.display = Availability::Invalid;
            if (domain == 1 && s.resource_availability != Availability::Valid && r.display == Availability::Valid)
                r.display = s.resource_availability;
            if (domain == 0 && s.availability != Availability::Valid && r.display == Availability::Valid)
                r.display = s.availability;
            if (r.display == Availability::Valid && number &&
                (((id == MetricId::Loss || id == MetricId::Retransmit || id == MetricId::Concealment) && *number>1)
                 || (id == MetricId::Cpu && *number>100))) r.display=Availability::Invalid;
            r.freshnessAt = r.sampledAt;
            if (r.sampledAt == TelemetryClock::time_point{} && r.display == Availability::Valid)
                r.display = Availability::Unknown;
        };
        put(MetricId::Receive,s.inbound_rtp_bitrate_bps,s.inbound_rtp_traffic_availability,0,true,s.coverage<1);
        put(MetricId::Send,s.outbound_rtp_bitrate_bps,s.outbound_rtp_traffic_availability,0,true,s.coverage<1);
        put(MetricId::Rtt,s.media_path_rtt_max_ms,s.media_path_rtt_availability,0);
        put(MetricId::Jitter,s.inbound_jitter_max_ms,s.inbound_jitter_availability,0);
        put(MetricId::Loss,s.inbound_packet_loss_ratio,s.inbound_packet_loss_availability,0);
        put(MetricId::Retransmit,s.inbound_retransmitted_packet_ratio,s.inbound_retransmission_availability,0);
        put(MetricId::Bandwidth,s.media_available_outgoing_bitrate_bps,s.media_bandwidth_availability,0);
        put(MetricId::AudioDelay,s.audio_jitter_buffer_delay_ms,s.audio_jitter_buffer_availability,0);
        put(MetricId::AudioTarget,s.audio_jitter_buffer_target_delay_ms,s.audio_jitter_buffer_availability,0);
        put(MetricId::Concealment,s.audio_concealed_ratio,s.audio_concealment_availability,0);
        put(MetricId::Cpu,s.process_cpu_percent,s.cpu_availability,1);
        put(MetricId::PrivateMemory,s.private_bytes,s.memory_availability,1);
        put(MetricId::WorkingSet,s.working_set_bytes,s.memory_availability,1);
        put(MetricId::Threads,s.process_thread_count,s.thread_count_availability,1);
        if(s.thread_count_sample_age_ms>=0) {
            sample.readings[static_cast<size_t>(MetricId::Threads)].sampledAt -=
                std::chrono::milliseconds(s.thread_count_sample_age_ms);
        }
        put(MetricId::Handles,s.process_handle_count,s.handle_count_availability,1);
        put(MetricId::Growth,s.private_bytes_growth_mib_per_minute,s.resource_trend_availability,1);
        put(MetricId::UiLag,s.last_ui_lag_ms,s.ui_lag_availability,2,s.ui_lag_samples>0);
        auto &ui = sample.readings[static_cast<size_t>(MetricId::UiLag)];
        ui.observedTime = true; ui.sourceToken = s.ui_lag_samples;
        // Keep first observation stable across replacement and ring eviction.
        const auto &observedHistory=historySeconds_ && !chartHistory_.empty()?chartHistory_:samples_;
        for (const auto &old : observedHistory) {
            const auto &prior = old.readings[static_cast<size_t>(MetricId::UiLag)];
            if (prior.sourceToken == ui.sourceToken) { ui.sampledAt = prior.sampledAt; break; }
        }
        if (!next.empty()) {
            const auto &prior = next.back().readings[static_cast<size_t>(MetricId::UiLag)];
            if (prior.sourceToken == ui.sourceToken) ui.sampledAt = prior.sampledAt;
        }
        ui.freshnessAt = ui.sampledAt;
        put(MetricId::SubmitFps,s.render_submit_fps,s.render_first_frame_availability,2);
        put(MetricId::StallActive,s.render_stall_active,s.render_stall_availability,2);
        put(MetricId::StallCount,s.render_stall_count,s.render_stall_availability,2);
        put(MetricId::StallDuration,s.render_stall_duration_ms,s.render_stall_availability,2);
        put(MetricId::Convert,s.render_convert_max_us,s.render_stage_availability,2,s.render_convert_samples>0);
        put(MetricId::Upload,s.render_upload_max_us,s.render_stage_availability,2,s.render_upload_samples>0);
        put(MetricId::Draw,s.render_draw_max_us,s.render_stage_availability,2,s.render_draw_samples>0);
        put(MetricId::Present,s.render_present_block_max_us,s.render_stage_availability,2,s.render_present_block_samples>0);
        put(MetricId::WindowSubmitFps,s.render_window_submit_fps,s.render_window_availability,2);
        put(MetricId::RenderBindings,s.render_window_bindings,
            s.render_window_scope_epoch ? Availability::Valid : Availability::Unknown,2);
        auto &windowReading=sample.readings[static_cast<size_t>(MetricId::WindowSubmitFps)];
        windowReading.sampledAt=windowReading.freshnessAt=s.render_window_end;
        windowReading.sourceToken=s.render_window_scope_epoch;
        sample.window={s.render_window_begin,s.render_window_end,s.render_window_scope_epoch,
            s.render_window_bindings,s.render_window_fine_bindings,s.render_window_availability,
            s.render_window_interval_histogram};
        if(s.render_window_availability==Availability::Valid && s.render_window_end>fineLastEnd_
            && s.render_window_fine_interval_histogram.size()==1001) {
            if(!fineHistory_.empty() && fineHistory_.back().scope!=s.render_window_scope_epoch)
                fineHistory_.clear();
            const auto bucket=[](auto p){return std::chrono::duration_cast<std::chrono::seconds>(p.time_since_epoch()).count()/5;};
            if(fineHistory_.empty() || bucket(fineHistory_.back().end)!=bucket(s.render_window_end)) {
                fineHistory_.push_back({s.render_window_begin,s.render_window_end,s.render_window_scope_epoch,{}});
            }
            auto &fine=fineHistory_.back();fine.end=s.render_window_end;
            for(size_t i=0;i<1001;++i)fine.counts[i]+=s.render_window_fine_interval_histogram[i];
            fineLastEnd_=s.render_window_end;
            while(fineHistory_.size()>61)fineHistory_.pop_front();
        }
        // Only classify controlled reason tokens; never forward raw diagnostics to UI.
        const auto partial = [&](MetricId id, const std::string &reason) {
            sample.readings[static_cast<size_t>(id)].partial |= reason.find("partial") != std::string::npos;
        };
        partial(MetricId::Receive,s.inbound_rtp_traffic_reason);
        partial(MetricId::Send,s.outbound_rtp_traffic_reason);
        partial(MetricId::Rtt,s.media_path_rtt_reason);
        partial(MetricId::Jitter,s.inbound_jitter_reason);
        partial(MetricId::Loss,s.inbound_packet_loss_reason);
        partial(MetricId::Retransmit,s.inbound_retransmission_reason);
        partial(MetricId::Bandwidth,s.media_bandwidth_reason);
        partial(MetricId::AudioDelay,s.audio_jitter_buffer_reason);
        partial(MetricId::AudioTarget,s.audio_jitter_buffer_reason);
        partial(MetricId::Concealment,s.audio_concealment_reason);
        next.push_back(std::move(sample));
    }
    std::stable_sort(next.begin(), next.end(), [](const Sample &a,const Sample &b){return a.generated<b.generated;});
    // Retain the initial collection origin across ring eviction and view changes.
    if (!timeOrigin_ && !next.empty()) {
        timeOrigin_ = next.front().generated;
        for (const auto &sample : next) {
            for (size_t i = 0; i <= static_cast<size_t>(MetricId::WorkingSet); ++i) {
                const auto &r = sample.readings[i];
                if (r.PlotValue(static_cast<MetricId>(i)) && r.sampledAt != TelemetryClock::time_point{})
                    timeOrigin_ = std::min(*timeOrigin_, r.sampledAt);
            }
            const auto &window=sample.readings[static_cast<size_t>(MetricId::WindowSubmitFps)];
            if(window.PlotValue(MetricId::WindowSubmitFps) && window.sampledAt!=TelemetryClock::time_point{})
                timeOrigin_=std::min(*timeOrigin_,window.sampledAt);
        }
    }
    if (next.size()>300) next.erase(next.begin(), next.end()-300);
    if(next.capacity()>300) {std::vector<Sample> bounded(next.begin(),next.end());next.swap(bounded);}
    samples_ = std::move(next);
    MergeChartHistory();
}
PanelFramePtr TelemetryPanelModel::BuildFrame(int windowSeconds, TelemetryClock::time_point now) {
    auto frame = std::make_shared<PanelFrame>();
    const int seconds=historySeconds_ ? historySeconds_ : (windowSeconds==0?300:std::clamp(windowSeconds,1,300));
    frame->end = now; frame->begin = now-std::chrono::seconds(seconds);
    if(historySeconds_) PruneChartHistory(now);
    const auto &history=historySeconds_?chartHistory_:samples_;
    frame->timeOrigin = timeOrigin_.value_or(now);
    frame->presentationRevision = ++presentationRevision_;
    frame->timeline=timeline_;
    if (!history.empty()) {
        frame->waiting = false;
        frame->sourceRevision = history.back().revision;
        frame->renderEventDrops = history.back().renderEventDrops;
        frame->readings = history.back().readings;
        frame->retainedSeconds = int(std::chrono::duration_cast<std::chrono::seconds>(
            history.back().generated - std::max(frame->begin,history.front().generated)).count());
    }
    for (size_t i=0;i<frame->series.size();++i) {
        auto &series = frame->series[i]; series.id = static_cast<MetricId>(i);
        TelemetryClock::time_point previous{}; std::uint64_t resets=0,scope=0;

        for (const auto &s : history) {
            const auto &r = s.readings[i];
            auto value = r.PlotValue(series.id);
            // Unavailability starts at the observation, not retroactively at the
            // last valid sample. Repeated publications cannot create new samples.
            auto stamp = value ? r.sampledAt : s.generated;
            if (stamp == TelemetryClock::time_point{}) stamp=s.generated;
            if (stamp == previous) {
                if (!series.points.empty() && series.points.back().time == stamp)
                    series.points.back().value = value;
                continue;
            }
            if (stamp < previous) continue;
            bool gap = previous != TelemetryClock::time_point{} &&
                (s.resets != resets || (series.id==MetricId::WindowSubmitFps && r.sourceToken!=scope)
                 || r.staleAfterMs <= 0 || stamp-previous>std::chrono::milliseconds(r.staleAfterMs));
            if (stamp>=frame->begin && stamp<=now) series.points.push_back({stamp,value,gap});
            previous=stamp; resets=s.resets;scope=r.sourceToken;
        }
        auto &r = frame->readings[i];
        if (r.display == Availability::Valid && (r.staleAfterMs<=0 || now<r.freshnessAt
            || now-r.freshnessAt>std::chrono::milliseconds(r.staleAfterMs))) r.display=Availability::Stale;
    }
    // Expand only the available history until the selected rolling window is full.
    // Use plotted source timestamps, not publication indices or detail-only cache ages.
    auto availableBegin = now;
    bool hasHistory = false;
    for (size_t i = 0; i < static_cast<size_t>(MetricId::Count); ++i) {
        if(i>static_cast<size_t>(MetricId::WorkingSet) && i!=static_cast<size_t>(MetricId::WindowSubmitFps))continue;
        if (frame->series[i].points.empty()) continue;
        availableBegin = std::min(availableBegin, frame->series[i].points.front().time);
        hasHistory = true;
    }
    if (historySeconds_ && !chartHistoryBegin_ && hasHistory) chartHistoryBegin_=availableBegin;
    if (historySeconds_ && chartHistoryBegin_) frame->begin=std::max(frame->begin,*chartHistoryBegin_);
    else if (hasHistory) frame->begin = std::max(frame->begin, availableBegin);
    auto &hist=frame->histogram;
    if(!history.empty()) {
        const auto &latest=history.back().window;
        hist.scope=latest.scope;hist.bindings=latest.bindings;hist.fineBindings=latest.fineBindings;
        hist.availability=frame->readings[static_cast<size_t>(MetricId::WindowSubmitFps)].display;
        TelemetryClock::time_point lastEnd{};
        for(const auto &sample:history) {
            const auto &w=sample.window;
            if(w.scope!=hist.scope || w.availability!=Availability::Valid || w.end<=lastEnd
                || w.end<frame->begin || w.end>now)continue;
            if(hist.begin==TelemetryClock::time_point{})hist.begin=w.begin;
            if(lastEnd!=TelemetryClock::time_point{} && w.begin>lastEnd+std::chrono::milliseconds(1))hist.partial=true;
            hist.end=w.end;lastEnd=w.end;
            for(size_t i=0;i<9;++i)hist.counts[i]+=w.counts[i];
        }
        hist.partial|=hist.begin>frame->begin || hist.fineBindings!=hist.bindings;
        for(const auto &fine:fineHistory_) {
            if(fine.scope!=hist.scope || fine.begin<frame->begin || fine.end>now)continue;
            if(hist.fine.empty()) {hist.fine.resize(1001);hist.fineBegin=fine.begin;}
            hist.fineEnd=fine.end;
            for(size_t i=0;i<1001;++i)hist.fine[i]+=fine.counts[i];
        }
    }
    return frame;
}

void TelemetryPanelModel::ReconcileTimeline(const diagnostic::TimelineSnapshot& source) {
    auto result=std::make_shared<TimelineFrame>();
    result->omitted=source.omitted;result->admissionDrops=source.admission_drops;
    // Reserve open intervals first; closed/lifecycle entries use the remaining
    // budget, newest first. Identity and token validation is shared with export.
    constexpr size_t limit=512;
    const auto append=[&](const diagnostic::Event& event) {
        if(event.context.anonymous_session_id.View()!=key_.anonymousId
            || !event.context.has_session_generation || event.context.session_generation!=key_.generation)return;
        auto projected=ProjectTimelineEvent(event);if(!projected)return;
        const auto &e=*projected;
        size_t bytes=sizeof(e);
        for(const auto *s:{&e.event_name,&e.process_run_id,&e.operation_id,&e.parent_operation_id,&e.request_id,
            &e.stage,&e.outcome,&e.error_code,&e.error_layer,&e.leave_reason,&e.media_kind,&e.measurement_point,
            &e.endpoint_id,&e.previous_endpoint_id,&e.boundary}) bytes+=s->capacity();
        if(result->events.size()==limit || result->retainedBytes+bytes>1024*1024) {++result->omitted;return;}
        result->retainedBytes+=bytes;result->events.push_back(std::move(*projected));
    };
    result->events.reserve(std::min(limit,source.events.size()));
    for(const auto &e:source.events)if(e.kind==diagnostic::EventKind::RenderStallInterval && e.stall_boundary==diagnostic::StallBoundary::Open)append(e);
    for(auto it=source.events.rbegin();it!=source.events.rend();++it)
        if(it->kind!=diagnostic::EventKind::RenderStallInterval || it->stall_boundary!=diagnostic::StallBoundary::Open)append(*it);
    std::sort(result->events.begin(),result->events.end(),[](const auto &a,const auto &b){return a.event_sequence<b.event_sequence;});
    timeline_=std::move(result);
}

TimelineView BuildTimelineView(TimelineFramePtr source,const std::string& operation) {
    TimelineView result;result.source=std::move(source);if(!result.source)return result;
    const auto &events=result.source->events;
    std::unordered_set<std::string> included;
    if(!operation.empty()) {
        included.insert(operation);
        // Fixed-point traversal also handles children admitted before their parent.
        for(size_t pass=0;pass<events.size();++pass) {
            const auto size=included.size();
            for(const auto &e:events)if(!e.operation_id.empty() && included.count(e.parent_operation_id))included.insert(e.operation_id);
            if(size==included.size())break;
        }
    }
    const auto selected=[&](const auto &e){return operation.empty() || included.count(e.operation_id) || included.count(e.parent_operation_id);};
    const auto lane=[](const std::string &name) {
        if(name.starts_with("admission.") || name.starts_with("startup."))return TimelineLane::Admission;
        if(name.starts_with("room.connect."))return TimelineLane::Connect;
        if(name.starts_with("media.publish") || name.starts_with("media.unpublish"))return TimelineLane::Publish;
        if(name.starts_with("reconnect."))return TimelineLane::Reconnect;
        if(name.starts_with("media.") || name.starts_with("render."))return TimelineLane::Receive;
        return TimelineLane::Other;
    };
    std::unordered_map<std::string,size_t> starts,ends;
    for(size_t i=0;i<events.size();++i) {
        const auto &e=events[i];if(!selected(e) || e.operation_id.empty())continue;
        if(e.event_name.ends_with(".started"))starts.try_emplace(e.operation_id,i);
        if(e.event_name.ends_with(".terminal") || e.event_name.ends_with(".completed"))ends[e.operation_id]=i;
    }
    result.items.reserve(events.size());
    for(size_t i=0;i<events.size();++i) {
        const auto &e=events[i];if(!selected(e))continue;
        const bool start=e.event_name.ends_with(".started");
        const bool end=e.event_name.ends_with(".terminal") || e.event_name.ends_with(".completed");
        if(end && starts.count(e.operation_id)
            && events[starts[e.operation_id]].source_monotonic_us
            && e.source_monotonic_us>=events[starts[e.operation_id]].source_monotonic_us)continue;
        TimelineVisualItem item{i,i,e.source_monotonic_us,e.source_monotonic_us,lane(e.event_name),start,end,!start&&!end};
        if(start) {
            if(starts[e.operation_id]!=i)continue;
            const auto terminal=ends.find(e.operation_id);
            if(item.begin && terminal!=ends.end() && events[terminal->second].source_monotonic_us>=item.begin) {
                item.last=terminal->second;item.end=events[item.last].source_monotonic_us;item.hasEnd=true;
            }
        }
        if(item.begin && (!result.begin || item.begin<result.begin))result.begin=item.begin;
        result.end=std::max(result.end,item.end);result.items.push_back(item);
    }
    std::stable_sort(result.items.begin(),result.items.end(),[](const auto &a,const auto &b){return a.begin<b.begin;});
    return result;
}

size_t TelemetryPanelModel::RetainedBytes() const {
    return (samples_.capacity()+chartHistory_.capacity())*sizeof(Sample)+fineHistory_.size()*sizeof(FineSample)
        +(timeline_?timeline_->retainedBytes+(timeline_->events.capacity()-timeline_->events.size())*sizeof(livekit::telemetry::OperationTimelineEntry):0);
}
} // namespace MeetingUI
