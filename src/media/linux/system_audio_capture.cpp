#include "../system_audio_capture.h"
#include <pulse/pulseaudio.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace livekit {
namespace {
using namespace std::chrono_literals;
constexpr std::size_t kSamples = 960; // 480 stereo frames, exactly 10 ms.
constexpr std::size_t kQueueSamples = kSamples * 10;
constexpr std::size_t kMaxStreams = 32;
bool IsOtherProcess(const pa_sink_input_info& info) {
    const auto* pid = pa_proplist_gets(info.proplist, PA_PROP_APPLICATION_PROCESS_ID);
    unsigned long process = 0;
    if (!pid) return false;
    const auto end = pid + std::char_traits<char>::length(pid);
    auto result = std::from_chars(pid, end, process);
    return result.ec == std::errc() && result.ptr == end && process > 0 &&
           process != static_cast<unsigned long>(getpid());
}

struct PulseOwner {
    pa_mainloop* loop = pa_mainloop_new();
    pa_context* context = nullptr;
    PulseOwner() {
        if (loop) context = pa_context_new(pa_mainloop_get_api(loop), "Cohavora Shared Audio");
    }
    ~PulseOwner() {
        if (context) { pa_context_disconnect(context); pa_context_unref(context); }
        if (loop) pa_mainloop_free(loop);
    }
    bool Connect() {
        return context && pa_context_connect(context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) == 0;
    }
    bool Iterate() {
        return loop && pa_mainloop_iterate(loop, 0, nullptr) >= 0;
    }
};

// Bounded control worker: no PulseAudio wait/signal APIs or shared global loop.
template <typename Predicate>
bool Await(PulseOwner& owner, Predicate done, std::chrono::milliseconds timeout = 3000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (!owner.Iterate() || std::chrono::steady_clock::now() >= deadline) return false;
        const auto state = pa_context_get_state(owner.context);
        if (!PA_CONTEXT_IS_GOOD(state)) return false;
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

struct OutputQuery {
    std::string default_name;
    std::vector<SystemAudioOutput> outputs;
    bool done = false;
};
void ServerInfo(pa_context*, const pa_server_info* info, void* data) {
    auto& query = *static_cast<OutputQuery*>(data);
    if (info && info->default_sink_name) query.default_name = info->default_sink_name;
    query.done = true;
}
void OutputInfo(pa_context*, const pa_sink_info* info, int eol, void* data) {
    auto& query = *static_cast<OutputQuery*>(data);
    if (eol) { query.done = true; return; }
    if (info && info->name && info->monitor_source != PA_INVALID_INDEX) {
        query.outputs.push_back({info->name, info->description ? info->description : info->name,
                                 query.default_name == info->name});
    }
}
bool CompleteQuery(PulseOwner& owner, pa_operation* op, OutputQuery& query) {
    if (!op) return false;
    const bool ok = Await(owner, [&] { return query.done; });
    if (!ok) pa_operation_cancel(op);
    pa_operation_unref(op);
    return ok;
}

class PulseSystemAudioCapture final : public ISystemAudioCapture {
    struct Feed {
        PulseSystemAudioCapture* owner = nullptr;
        pa_stream* stream = nullptr; // control worker only
        uint32_t input_index = PA_INVALID_INDEX;
        std::array<std::int16_t, kQueueSamples> pcm{}; // mutex_
        std::size_t head = 0, size = 0;
        bool ready = false; // mutex_
        ~Feed() {
            if (stream) {
                pa_stream_set_read_callback(stream, nullptr, nullptr);
                pa_stream_set_state_callback(stream, nullptr, nullptr);
                pa_stream_disconnect(stream);
                pa_stream_unref(stream);
            }
        }
    };
    struct Input { uint32_t index; uint32_t sink; bool excluded; };
    struct RouteQuery {
        std::string sink_name, monitor_name;
        uint32_t sink = PA_INVALID_INDEX;
        std::vector<Input> inputs;
        bool done = false, failed = false;
    };
public:
    ~PulseSystemAudioCapture() override { Stop(); }
    bool Start(SystemAudioCaptureConfig config, FrameCallback frame, StatusCallback status) override {
        if (control_.joinable() || !frame) return false;
        config_ = std::move(config);
        frame_ = std::move(frame);
        status_ = std::move(status);
        stop_.store(false);
        failure_.store(SystemAudioCaptureError::None);
        try {
            delivery_ = std::thread([this] { Deliver(); });
            control_ = std::thread([this] { Control(); });
        } catch (...) { Stop(); return false; }
        return true;
    }
    void Stop() override {
        stop_.store(true);
        wake_.notify_all();
        if (control_.joinable()) control_.join();
        if (delivery_.joinable()) delivery_.join();
        frame_ = {};
        status_ = {};
    }
    SystemAudioCaptureStats stats() const override {
        return {delivered_.load(), dropped_.load(), excluded_.load(), changes_.load()};
    }
private:
    void Notify(SystemAudioCapturePhase phase, SystemAudioCaptureError error = SystemAudioCaptureError::None) {
        if (!stop_.load() && status_) {
            try { status_({phase, error}); }
            catch (...) { Fail(SystemAudioCaptureError::Stream); }
        }
    }
    void Fail(SystemAudioCaptureError error) {
        { std::lock_guard lock(mutex_); routes_ready_ = false; }
        failure_.store(error);
    }
    static void Read(pa_stream* stream, std::size_t, void* data) {
        auto& feed = *static_cast<Feed*>(data);
        auto& self = *feed.owner;
        const void* bytes = nullptr;
        std::size_t count = 0;
        if (pa_stream_peek(stream, &bytes, &count) < 0) {
            self.Fail(SystemAudioCaptureError::Stream); return;
        }
        // Holes advance the stream and produce bounded silence. Borrowed data
        // never survives drop; publication/conversion never runs here.
        if (count % (sizeof(int16_t) * 2) != 0) {
            pa_stream_drop(stream); self.Fail(SystemAudioCaptureError::Format); return;
        }
        {
            std::lock_guard lock(self.mutex_);
            if (self.routes_ready_ && feed.ready) {
                auto samples = count / sizeof(int16_t);
                const auto* pcm = static_cast<const int16_t*>(bytes);
                if (samples > kQueueSamples) {
                    self.dropped_.fetch_add(samples - kQueueSamples);
                    if (pcm) pcm += samples - kQueueSamples;
                    samples = kQueueSamples;
                }
                if (feed.size + samples > kQueueSamples) {
                    const auto discard = feed.size + samples - kQueueSamples;
                    feed.head = (feed.head + discard) % kQueueSamples;
                    feed.size -= discard;
                    self.dropped_.fetch_add(discard);
                }
                for (std::size_t i = 0; i < samples; ++i)
                    feed.pcm[(feed.head + feed.size + i) % kQueueSamples] = pcm ? pcm[i] : 0;
                feed.size += samples;
            }
        }
        pa_stream_drop(stream);
    }
    static void StreamState(pa_stream* stream, void* data) {
        auto& feed = *static_cast<Feed*>(data);
        const auto state = pa_stream_get_state(stream);
        if (state == PA_STREAM_READY) {
            const auto* spec = pa_stream_get_sample_spec(stream);
            if (!spec || spec->format != PA_SAMPLE_S16LE || spec->rate != 48000 || spec->channels != 2) {
                feed.owner->Fail(SystemAudioCaptureError::Format); return;
            }
            std::lock_guard lock(feed.owner->mutex_);
            feed.ready = true;
        } else if (state == PA_STREAM_FAILED || state == PA_STREAM_TERMINATED) {
            // Natural playback completion may end its targeted monitor before
            // the topology event arrives. Refresh the snapshot; repeated
            // failures of a still-existing target fail closed.
            feed.owner->failed_inputs_.push_back(feed.input_index);
            feed.owner->Invalidate();
        }
    }
    void Invalidate() {
        { std::lock_guard lock(mutex_);
          routes_ready_ = false;
          for (auto& feed : feeds_) { feed->head = 0; feed->size = 0; }
        }
        dirty_ = true; // PulseAudio control thread only
    }
    static void ChangedInput(pa_context*, const pa_sink_input_info* info, int, void* data) {
        if (!info) return;
        auto& self = *static_cast<PulseSystemAudioCapture*>(data);
        bool tracked = false;
        { std::lock_guard lock(self.mutex_);
          tracked = std::any_of(self.feeds_.begin(), self.feeds_.end(), [&](const auto& f) { return f->input_index == info->index; });
        }
        const bool eligible = info->sink == self.sink_index_ && !info->corked && IsOtherProcess(*info);
        // Ordinary volume/proplist changes do not disturb PCM or recreate
        // monitor streams. Identity/routing changes revoke before refresh.
        if (tracked && !eligible) self.Invalidate();
        else if (!tracked && eligible) self.dirty_ = true;
    }
    static void Topology(pa_context* context, pa_subscription_event_type_t event, uint32_t index, void* data) {
        auto& self = *static_cast<PulseSystemAudioCapture*>(data);
        const auto facility = event & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
        const auto type = event & PA_SUBSCRIPTION_EVENT_TYPE_MASK;
        if (facility == PA_SUBSCRIPTION_EVENT_SINK_INPUT) {
            if (type == PA_SUBSCRIPTION_EVENT_REMOVE) {
                bool tracked;
                { std::lock_guard lock(self.mutex_);
                  tracked = std::any_of(self.feeds_.begin(), self.feeds_.end(), [&](const auto& f) { return f->input_index == index; });
                }
                if (tracked) self.Invalidate();
            } else {
                if (auto* op = pa_context_get_sink_input_info(context, index, ChangedInput, &self)) pa_operation_unref(op);
            }
        } else if (facility == PA_SUBSCRIPTION_EVENT_SINK) {
            if (type == PA_SUBSCRIPTION_EVENT_REMOVE && index == self.sink_index_) self.Invalidate();
        } else if (facility == PA_SUBSCRIPTION_EVENT_SERVER && self.config_.output_id.empty()) {
            self.Invalidate();
        }
    }
    static void DefaultSink(pa_context*, const pa_server_info* info, void* data) {
        auto& q = *static_cast<RouteQuery*>(data);
        if (info && info->default_sink_name) q.sink_name = info->default_sink_name;
        else q.failed = true;
        q.done = true;
    }
    static void Sink(pa_context*, const pa_sink_info* info, int eol, void* data) {
        auto& q = *static_cast<RouteQuery*>(data);
        if (eol) { q.done = true; if (eol < 0) q.failed = true; return; }
        if (info && info->monitor_source_name && info->monitor_source != PA_INVALID_INDEX) {
            q.sink = info->index;
            q.monitor_name = info->monitor_source_name;
        }
    }
    static void Inputs(pa_context*, const pa_sink_input_info* info, int eol, void* data) {
        auto& q = *static_cast<RouteQuery*>(data);
        if (eol) { q.done = true; if (eol < 0) q.failed = true; return; }
        if (!info || info->sink != q.sink || info->corked) return;
        // Positive inclusion: unidentified streams never enter a record stream.
        q.inputs.push_back({info->index, info->sink, !IsOtherProcess(*info)});
    }
    bool Query(PulseOwner& owner, pa_operation* op, RouteQuery& query) {
        if (!op) return false;
        const bool ok = Await(owner, [&] { return stop_.load() || query.done; });
        if (!ok || stop_.load()) pa_operation_cancel(op);
        pa_operation_unref(op);
        const bool success = ok && !stop_.load() && !query.failed;
        query.done = false;
        return success;
    }
    bool Refresh(PulseOwner& owner) {
        dirty_ = false;
        RouteQuery query;
        query.sink_name = config_.output_id;
        if (query.sink_name.empty() && !Query(owner,
            pa_context_get_server_info(owner.context, DefaultSink, &query), query)) return false;
        if (!Query(owner, pa_context_get_sink_info_by_name(owner.context, query.sink_name.c_str(), Sink, &query), query) ||
            query.sink == PA_INVALID_INDEX) { Fail(SystemAudioCaptureError::OutputUnavailable); return false; }
        if (!Query(owner, pa_context_get_sink_input_info_list(owner.context, Inputs, &query), query)) return false;
        if (dirty_) return true;
        const auto included = std::count_if(query.inputs.begin(), query.inputs.end(), [](const auto& i) { return !i.excluded; });
        if (included > static_cast<int>(kMaxStreams)) { Fail(SystemAudioCaptureError::TooManyStreams); return false; }
        const auto exists = [&](uint32_t index) {
            return std::any_of(query.inputs.begin(), query.inputs.end(), [&](const auto& i) { return !i.excluded && i.index == index; });
        };
        std::erase_if(retry_counts_, [&](const auto& item) { return !exists(item.first); });
        std::sort(failed_inputs_.begin(), failed_inputs_.end());
        failed_inputs_.erase(std::unique(failed_inputs_.begin(), failed_inputs_.end()), failed_inputs_.end());
        for (const auto index : failed_inputs_) {
            if (exists(index) && ++retry_counts_[index] > 3) {
                Fail(SystemAudioCaptureError::Stream); return false;
            }
        }
        std::vector<std::unique_ptr<Feed>> retired;
        std::vector<std::unique_ptr<Feed>> retained;
        {
            std::lock_guard lock(mutex_);
            for (auto& feed : feeds_) {
                const bool present = monitor_name_ == query.monitor_name &&
                    std::any_of(query.inputs.begin(), query.inputs.end(), [&](const auto& i) { return !i.excluded && i.index == feed->input_index; }) &&
                    std::find(failed_inputs_.begin(), failed_inputs_.end(), feed->input_index) == failed_inputs_.end();
                (present ? retained : retired).push_back(std::move(feed));
            }
            feeds_.clear();
        }
        retired.clear(); // only PA owner unhooks/disconnects record streams
        failed_inputs_.clear();
        std::vector<std::unique_ptr<Feed>> next;
        for (const auto& input : query.inputs) {
            if (input.excluded) { excluded_.fetch_add(1); continue; }
            const auto existing = std::find_if(retained.begin(), retained.end(), [&](const auto& f) { return f && f->input_index == input.index; });
            if (existing != retained.end()) { next.push_back(std::move(*existing)); continue; }
            auto feed = std::make_unique<Feed>();
            feed->owner = this;
            feed->input_index = input.index;
            pa_sample_spec spec{PA_SAMPLE_S16LE, 48000, 2};
            pa_channel_map map;
            pa_channel_map_init_stereo(&map);
            feed->stream = pa_stream_new(owner.context, "Cohavora Shared Input", &spec, &map);
            if (!feed->stream || pa_stream_set_monitor_stream(feed->stream, input.index) < 0) {
                Fail(SystemAudioCaptureError::Stream); return false;
            }
            pa_stream_set_read_callback(feed->stream, Read, feed.get());
            pa_stream_set_state_callback(feed->stream, StreamState, feed.get());
            pa_buffer_attr attr{static_cast<uint32_t>(kQueueSamples * 2), UINT32_MAX, UINT32_MAX, UINT32_MAX, 3840};
            if (pa_stream_connect_record(feed->stream, query.monitor_name.c_str(), &attr,
                static_cast<pa_stream_flags_t>(PA_STREAM_ADJUST_LATENCY | PA_STREAM_DONT_MOVE)) < 0) {
                Fail(SystemAudioCaptureError::Stream); return false;
            }
            next.push_back(std::move(feed));
        }
        // A topology change during the async snapshot invalidates this whole
        // transaction. No stale route is ever published as ready.
        if (dirty_) return true;
        { std::lock_guard lock(mutex_); feeds_ = std::move(next); routes_ready_ = true; }
        monitor_name_ = query.monitor_name;
        sink_index_ = query.sink;
        changes_.fetch_add(1);
        return true;
    }
    void Control() {
        Notify(SystemAudioCapturePhase::Connecting);
        PulseOwner owner;
        if (!owner.Connect() || !Await(owner, [&] {
            return stop_.load() || pa_context_get_state(owner.context) == PA_CONTEXT_READY;
        })) { Notify(SystemAudioCapturePhase::Failed, SystemAudioCaptureError::Connect); return; }
        if (stop_.load()) return;
        pa_context_set_subscribe_callback(owner.context, Topology, this);
        int subscribed = 0;
        auto* subscribe = pa_context_subscribe(owner.context,
            static_cast<pa_subscription_mask_t>(PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SINK_INPUT | PA_SUBSCRIPTION_MASK_SERVER),
            [](pa_context*, int success, void* data) { *static_cast<int*>(data) = success ? 1 : -1; }, &subscribed);
        if (!subscribe) { Notify(SystemAudioCapturePhase::Failed, SystemAudioCaptureError::Connect); return; }
        const bool subscription_ready = Await(owner, [&] { return stop_.load() || subscribed != 0; });
        if (!subscription_ready || stop_.load()) pa_operation_cancel(subscribe);
        pa_operation_unref(subscribe);
        if (stop_.load()) return;
        if (!subscription_ready || subscribed != 1) {
            Notify(SystemAudioCapturePhase::Failed, SystemAudioCaptureError::Connect); return;
        }
        dirty_ = true;
        bool announced = false;
        auto ready_deadline = std::chrono::steady_clock::now() + 3s;
        auto stable_since = std::chrono::steady_clock::now();
        while (!stop_.load()) {
            if (!owner.Iterate() || pa_context_get_state(owner.context) != PA_CONTEXT_READY) {
                Fail(SystemAudioCaptureError::Connect); break;
            }
            if (dirty_) {
                if (!Refresh(owner)) {
                    if (failure_.load() == SystemAudioCaptureError::None) Fail(SystemAudioCaptureError::Stream);
                    break;
                }
                ready_deadline = std::chrono::steady_clock::now() + 3s;
                stable_since = std::chrono::steady_clock::now();
            }
            if (failure_.load() != SystemAudioCaptureError::None) break;
            bool ready = false;
            { std::lock_guard lock(mutex_);
              ready = routes_ready_ && std::all_of(feeds_.begin(), feeds_.end(), [](const auto& f) { return f->ready; });
            }
            if (ready && !announced) { Notify(SystemAudioCapturePhase::Ready); announced = true; }
            if (ready && std::chrono::steady_clock::now() - stable_since > 1s) retry_counts_.clear();
            if (!ready && std::chrono::steady_clock::now() >= ready_deadline) {
                Fail(SystemAudioCaptureError::Stream); break;
            }
            std::unique_lock lock(wait_mutex_);
            wake_.wait_for(lock, 2ms, [&] { return stop_.load(); });
        }
        { std::lock_guard lock(mutex_); routes_ready_ = false; feeds_.clear(); }
        pa_context_set_subscribe_callback(owner.context, nullptr, nullptr);
        if (!stop_.load()) Notify(SystemAudioCapturePhase::Failed, failure_.load());
    }
    void Deliver() {
        auto next = std::chrono::steady_clock::now();
        while (!stop_.load()) {
            next += 10ms;
            std::vector<int16_t> pcm(kSamples, 0);
            bool deliver = false;
            { std::lock_guard lock(mutex_);
              deliver = routes_ready_ && std::all_of(feeds_.begin(), feeds_.end(), [](const auto& f) { return f->ready; });
              std::array<int32_t, kSamples> mixed{};
              for (auto& feed : feeds_) {
                  const auto count = std::min(kSamples, feed->size);
                  for (std::size_t i = 0; i < count; ++i) mixed[i] += feed->pcm[(feed->head + i) % kQueueSamples];
                  feed->head = (feed->head + count) % kQueueSamples;
                  feed->size -= count;
              }
              for (std::size_t i = 0; i < kSamples; ++i) pcm[i] = static_cast<int16_t>(std::clamp(mixed[i], -32768, 32767));
            }
            if (deliver && !stop_.load()) {
                try { frame_(AudioFrame(std::move(pcm), 48000, 2, 480)); delivered_.fetch_add(1); }
                catch (...) { Fail(SystemAudioCaptureError::Stream); }
            }
            std::unique_lock lock(wait_mutex_);
            wake_.wait_until(lock, next, [&] { return stop_.load(); });
            // Long stalls discard elapsed slots; never burst stale media.
            if (next < std::chrono::steady_clock::now() - 20ms) next = std::chrono::steady_clock::now();
        }
    }
    SystemAudioCaptureConfig config_;
    FrameCallback frame_;
    StatusCallback status_;
    std::thread control_, delivery_;
    std::atomic<bool> stop_{false};
    std::atomic<SystemAudioCaptureError> failure_{SystemAudioCaptureError::None};
    mutable std::mutex mutex_, wait_mutex_;
    std::condition_variable wake_;
    bool routes_ready_ = false, dirty_ = true;
    std::vector<std::unique_ptr<Feed>> feeds_;
    std::string monitor_name_; // control worker
    uint32_t sink_index_ = PA_INVALID_INDEX;
    std::vector<uint32_t> failed_inputs_;
    std::map<uint32_t, unsigned> retry_counts_;
    std::atomic<uint64_t> delivered_{0}, dropped_{0}, excluded_{0}, changes_{0};
};
} // namespace

std::unique_ptr<ISystemAudioCapture> CreateSystemAudioCapture() { return std::make_unique<PulseSystemAudioCapture>(); }
std::vector<SystemAudioOutput> EnumerateSystemAudioOutputs() {
    PulseOwner owner;
    if (!owner.Connect() || !Await(owner, [&] { return pa_context_get_state(owner.context) == PA_CONTEXT_READY; })) return {};
    OutputQuery query;
    if (!CompleteQuery(owner, pa_context_get_server_info(owner.context, ServerInfo, &query), query)) return {};
    query.done = false;
    if (!CompleteQuery(owner, pa_context_get_sink_info_list(owner.context, OutputInfo, &query), query)) return {};
    return query.outputs;
}
bool SystemAudioSharingAvailable() {
#if defined(COHAVORA_LINUX_SYSTEM_AUDIO_VERIFIED)
    return true;
#else
    return false;
#endif
}
} // namespace livekit
