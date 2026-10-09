// Opt-in S0 feasibility probe: synthetic I420, a real SFU, independent Rooms.
// Test-only sender access deliberately does not add a production quality API.
#include "room.h"
#include "local_video_track.h"
#include "remote_track_publication.h"
#include "signal_client.h"
#include "webrtc_manager.h"
#include "stats.h"
#include "render/owned_i420_frame.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <fstream>
#include <mutex>
#include <openssl/sha.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace livekit {
class RoomUnpublishTestAccess {
public:
    static nlohmann::json SenderSnapshot(Room& room) {
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
        { std::lock_guard lock(room.room_mutex_); pc = room.publisher_pc_; }
        auto value = nlohmann::json::array();
        if (!pc) return value;
        WebRTCManager::Instance().signaling_thread()->BlockingCall([&] {
            for (const auto& sender : pc->GetSenders()) {
                if (!sender->track() || sender->track()->kind() != "video") continue;
                const auto parameters = sender->GetParameters();
                auto encodings = nlohmann::json::array();
                for (const auto& e : parameters.encodings) encodings.push_back({{"rid",e.rid},{"active",e.active},
                    {"max_fps",e.max_framerate.value_or(0)},{"scale",e.scale_resolution_down_by.value_or(1)}});
                value.push_back({{"codec",parameters.codecs.empty() ? "" : parameters.codecs.front().name},{"encodings",encodings}});
            }
        });
        return value;
    }
    static bool RestrictInitialReceiverCodec(Room& room, const std::string& name) {
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
        { std::lock_guard lock(room.room_mutex_); pc = room.publisher_pc_; }
        if (!pc) return false;
        bool changed = false;
        bool success = true;
        WebRTCManager::Instance().signaling_thread()->BlockingCall([&] {
            auto capabilities = WebRTCManager::Instance().factory()->GetRtpReceiverCapabilities(webrtc::MediaType::VIDEO);
            std::vector<webrtc::RtpCodecCapability> vp8;
            for (const auto& codec : capabilities.codecs)
                if (codec.name == name) vp8.push_back(codec);
            for (auto transceiver : pc->GetTransceivers()) {
                if (transceiver->media_type() != webrtc::MediaType::VIDEO) continue;
                success = transceiver->SetCodecPreferences(vp8).ok() && success;
                changed = true;
            }
        });
        return changed && success;
    }
    static asio::awaitable<void> LimitSubscriber(Room& room, int64_t bps) {
        std::shared_ptr<SignalClient> signal;
        { std::lock_guard lock(room.room_mutex_); signal = room.signal_client_; }
        if (!signal) throw std::runtime_error("subscriber_missing");
        proto::SignalRequest request;
        request.mutable_simulate()->set_subscriber_bandwidth(bps);
        co_await signal->SendAsync(request);
    }
    static asio::awaitable<void> Update(Room& room,
            const std::shared_ptr<LocalVideoTrack>& track, int width, int height, int fps) {
        if (std::getenv("QUALITY_PROBE_PRODUCTION") && track->Track::source() == TrackSource::ScreenShareVideo) {
            ScreenShareFrameProfile profile{{ScreenShareResolution::Native, fps}, width, height, width, height, 1};
            co_await room.ApplyScreenShareSenderParametersAsync(track, profile);
            track->SetScreenShareProfile(profile);
            co_await room.SyncScreenShareMetadataAsync(track, profile);
            co_return;
        }
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
        std::shared_ptr<SignalClient> signal;
        {
            std::lock_guard lock(room.room_mutex_);
            pc = room.publisher_pc_;
            signal = room.signal_client_;
        }
        if (!pc || !signal) throw std::runtime_error("publisher_missing");
        bool found = false, success = true;
        proto::SignalRequest layer_request;
        auto* layer_update = layer_request.mutable_update_layers();
        layer_update->set_track_sid(track->sid());
        WebRTCManager::Instance().signaling_thread()->BlockingCall([&] {
            for (const auto& sender : pc->GetSenders()) {
                if (!sender || sender->track() != track->rtc_track()) continue;
                found = true;
                auto parameters = sender->GetParameters();
                for (auto& encoding : parameters.encodings) {
                    const bool lower = encoding.scale_resolution_down_by.value_or(1.0) > 1.0;
                    encoding.max_framerate = lower ? 3 : fps;
                    encoding.max_bitrate_bps = lower ? 200000 : 2500000;
                    auto* layer = layer_update->add_layers();
                    const double scale = encoding.scale_resolution_down_by.value_or(1.0);
                    layer->set_width(int(width / scale)); layer->set_height(int(height / scale));
                    layer->set_bitrate(*encoding.max_bitrate_bps);
                    layer->set_rid(encoding.rid);
                    const int spatial = encoding.rid == "h" ? 1 : encoding.rid == "f" ? 2 : 0;
                    layer->set_spatial_layer(spatial);
                    layer->set_quality(parameters.encodings.size() == 1 ? proto::VideoQuality::HIGH :
                        spatial == 0 ? proto::VideoQuality::LOW : spatial == 1 ? proto::VideoQuality::MEDIUM : proto::VideoQuality::HIGH);
                }
                success = sender->SetParameters(parameters).ok() && success;
            }
        });
        if (!found || !success) throw std::runtime_error("sender_update_failed");
        proto::SignalRequest request;
        auto* update = request.mutable_update_video_track();
        update->set_track_sid(track->sid());
        update->set_width(width);
        update->set_height(height);
        co_await signal->SendAsync(request);
        if (std::getenv("QUALITY_PROBE_LEGACY_LAYERS")) co_await signal->SendAsync(layer_request);
    }

};
}

namespace {
using namespace std::chrono_literals;
using json = nlohmann::json;
void Emit(json value) { std::cout << "QUALITY_PROBE " << value.dump() << std::endl; }
struct ProbeFailure { const char* code; };
void Check(bool ok, const char* reason) { if (!ok) throw ProbeFailure{reason}; }
asio::awaitable<void> Delay(std::chrono::milliseconds duration) {
    asio::steady_timer timer(co_await asio::this_coro::executor);
    timer.expires_after(duration);
    co_await timer.async_wait(asio::use_awaitable);
}
template<class F> asio::awaitable<void> Until(F ready, const char* reason, int seconds = 25) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!ready()) {
        Check(std::chrono::steady_clock::now() < deadline, reason);
        co_await Delay(25ms);
    }
}
struct Receiver final : livekit::RoomListener {
    std::atomic<int> width{0}, height{0}, frames{0}, subscriptions{0}, removed{0};
    std::string sid;
    std::shared_ptr<livekit::Track> track;
    std::shared_ptr<livekit::RemoteTrackPublication> publication;
    livekit::Track::I420VideoFrameSubscription sink;
    void OnTrackSubscribed(std::shared_ptr<livekit::Track> value,
            std::shared_ptr<livekit::TrackPublication> pub,
            std::shared_ptr<livekit::RemoteParticipant>) override {
        if (value->kind() != livekit::TrackKind::Video) return;
        sid = pub->sid(); track = value;
        publication = std::dynamic_pointer_cast<livekit::RemoteTrackPublication>(pub);
        ++subscriptions;
        sink = value->subscribeI420VideoFrames([this](const auto& frame) {
            width = frame->width(); height = frame->height(); ++frames;
        });
    }
    void OnTrackUnpublished(std::shared_ptr<livekit::RemoteParticipant>,
            std::shared_ptr<livekit::TrackPublication>) override { ++removed; }
};
struct Generator {
    std::shared_ptr<livekit::VideoSource> source = std::make_shared<livekit::VideoSource>(1280, 720);
    int width = 1280, height = 720, fps = 15;
    bool running = true;
};

// Receive-only peer for the separate product Qt/UI publisher fixture. The
// request remains 4K while sender UI changes quality, so received pixels prove
// the publication's new source profile rather than a receiver downscale.
struct ProductUiReceiver final : livekit::RoomListener {
    struct State {
        std::mutex mutex;
        std::string sid, initial_sid;
        std::weak_ptr<livekit::Track> initial_track;
        uint64_t frames = 0, subscriptions = 0, removed = 0, unsubscribed = 0;
        uint64_t callback_frames = 0, post_detach_callbacks = 0;
        uint64_t frames4k = 0, frames1080 = 0;
        int width = 0, height = 0;
        bool attached = false, same_track = false, dimensions_accepted = false;
        std::chrono::steady_clock::time_point last_frame{};
    };
    std::shared_ptr<State> state = std::make_shared<State>();
    livekit::Track::I420VideoFrameSubscription sink;
    void OnTrackSubscribed(std::shared_ptr<livekit::Track> value,
            std::shared_ptr<livekit::TrackPublication> publication,
            std::shared_ptr<livekit::RemoteParticipant>) override {
        if (!value || value->kind() != livekit::TrackKind::Video ||
            value->source() != livekit::TrackSource::ScreenShareVideo) return;
        auto remote = std::dynamic_pointer_cast<livekit::RemoteTrackPublication>(publication);
        const bool accepted = remote && remote->SetVideoDimensions(3840, 2160);
        {
            std::lock_guard lock(state->mutex);
            if (state->initial_sid.empty()) { state->initial_sid = publication->sid(); state->initial_track = value; }
            state->sid = publication->sid(); ++state->subscriptions;
            state->attached = true; state->same_track = value == state->initial_track.lock();
            state->dimensions_accepted = accepted;
        }
        sink = value->subscribeI420VideoFrames([data = state](const auto &frame) {
            std::lock_guard lock(data->mutex);
            // Keep callback-entry evidence independent of the media binding.
            // A callback copied before detach can arrive after publication
            // removal; ignoring it would falsely prove stop-time silence.
            ++data->callback_frames;
            if (!data->attached) { ++data->post_detach_callbacks; return; }
            if (!frame) return;
            data->width = frame->width(); data->height = frame->height(); ++data->frames;
            data->frames4k += frame->width() == 3840 && frame->height() == 2160;
            data->frames1080 += frame->width() == 1920 && frame->height() == 1080;
            data->last_frame = std::chrono::steady_clock::now();
        });
    }
    void OnTrackUnpublished(std::shared_ptr<livekit::RemoteParticipant>,
            std::shared_ptr<livekit::TrackPublication> publication) override {
        std::lock_guard lock(state->mutex);
        if (publication && publication->sid() == state->sid) { ++state->removed; state->attached = false; }
    }
    void OnTrackUnsubscribed(std::shared_ptr<livekit::Track>,
            std::shared_ptr<livekit::TrackPublication> publication,
            std::shared_ptr<livekit::RemoteParticipant>) override {
        std::lock_guard lock(state->mutex);
        if (publication && publication->sid() == state->sid) { ++state->unsubscribed; state->attached = false; }
    }
    json Snapshot() {
        std::lock_guard lock(state->mutex);
        const bool observed = state->last_frame != std::chrono::steady_clock::time_point{};
        std::string sid_hash;
        if (!state->sid.empty()) {
            uint8_t digest[SHA256_DIGEST_LENGTH];
            SHA256(reinterpret_cast<const uint8_t*>(state->sid.data()), state->sid.size(), digest);
            const char hex[] = "0123456789abcdef";
            for (int i = 0; i < 8; ++i) { sid_hash += hex[digest[i] >> 4]; sid_hash += hex[digest[i] & 15]; }
        }
        return {{"track_sid", state->sid}, {"sid_hash", sid_hash}, {"frames", state->frames}, {"frames_4k", state->frames4k},
            {"callback_frames", state->callback_frames}, {"post_detach_callbacks", state->post_detach_callbacks},
            {"frames_1080p", state->frames1080}, {"width", state->width}, {"height", state->height},
            {"subscriptions", state->subscriptions}, {"removed", state->removed},
            {"unsubscribed", state->unsubscribed}, {"attached", state->attached},
            {"same_sid", !state->initial_sid.empty() && state->sid == state->initial_sid},
            {"same_track", state->same_track}, {"dimensions_request_accepted", state->dimensions_accepted},
            {"requested_width",3840}, {"requested_height",2160},
            {"frame_age_ms", observed ? std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - state->last_frame).count() : -1}};
    }
};

bool WriteProductUiStatus(const std::filesystem::path &path, const json &value) {
    const auto temporary = path.wstring() + L".tmp";
    { std::ofstream stream(std::filesystem::path(temporary), std::ios::binary | std::ios::trunc);
      if (!stream) return false; stream << value.dump() << '\n'; stream.flush(); if (!stream) return false; }
    return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

asio::awaitable<int> RunProductUiObserver(const std::filesystem::path directory) {
    const auto executor = co_await asio::this_coro::executor;
    auto room = livekit::Room::Create(executor);
    room->SetLogHandler([](const auto &, const auto &, const auto &) {});
    auto receiver = std::make_shared<ProductUiReceiver>(); room->AddListener(receiver);
    int result = 1;
    uint64_t sequence = 0;
    try {
        livekit::SignalOptions options;
        options.auto_subscribe = true; options.single_peer_connection = true;
        options.allow_insecure_transport = std::getenv("LIVEKIT_TEST_ALLOW_INSECURE") != nullptr;
        options.connect_timeout = 20s;
        co_await room->ConnectAsync(std::getenv("LIVEKIT_URL"), std::getenv("RECEIVER_TOKEN"), options);
        const auto start = std::chrono::steady_clock::now();
        Emit({{"event", "product_ui_observer_connected"}});
        std::ofstream samples(directory / "samples.jsonl", std::ios::binary | std::ios::trunc);
        Check(bool(samples), "product_ui_observer_samples_unavailable");
        while (!std::filesystem::exists(directory / "observer.stop") && std::chrono::steady_clock::now() - start < 360s) {
            auto value = receiver->Snapshot();
            value["schema"] = 1; value["sample_seq"] = ++sequence;
            value["pid"] = GetCurrentProcessId();
            value["utc_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            value["connected"] = room->connection_state() == livekit::ConnectionState::Connected;
            value["elapsed_s"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            value["scope"] = "Independent native Room receives actual product UI shared media; no synthetic source";
            Check(WriteProductUiStatus(directory / "status.json", value), "product_ui_observer_status_write_failed");
            samples << value.dump() << '\n'; samples.flush();
            co_await Delay(250ms);
        }
        result = std::filesystem::exists(directory / "observer.stop") ? 0 : 1;
    } catch (const ProbeFailure &failure) {
        Emit({{"event", "failure"}, {"code", failure.code}});
    } catch (...) { Emit({{"event", "failure"}, {"code", "product_ui_observer_failed"}}); }
    co_await room->DisconnectAsync(); room->RemoveListener(receiver); receiver->sink.reset();
    auto value = receiver->Snapshot(); value["connected"] = false; value["finished"] = true;
    value["status"] = result == 0 ? "PASS" : "FAIL";
    WriteProductUiStatus(directory / "final.json", value);
    Emit({{"event", "product_ui_observer_result"}, {"status", result == 0 ? "PASS" : "FAIL"}});
    co_return result;
}

asio::awaitable<void> Frames(std::shared_ptr<Generator> generator) {
    int width = 0, height = 0, sequence = 0;
    std::optional<livekit::VideoFrame> frame;
    asio::steady_timer timer(co_await asio::this_coro::executor);
    auto next = std::chrono::steady_clock::now();
    while (generator->running) {
        if (width != generator->width || height != generator->height) {
            width = generator->width; height = generator->height;
            frame = livekit::VideoFrame::create(width, height, livekit::VideoBufferType::I420);
            std::fill(frame->data(), frame->data() + frame->dataSize(), uint8_t{128});
        }
        std::fill(frame->data(), frame->data() + width * height, uint8_t{40});
        const int x = (++sequence * 12) % (width - 80);
        for (int y = height / 4; y < height * 3 / 4; ++y)
            std::fill(frame->data() + y * width + x, frame->data() + y * width + x + 80, uint8_t{220});
        generator->source->captureFrame(*frame);
        const auto period = std::chrono::microseconds(1000000 / generator->fps);
        next += period;
        if (next < std::chrono::steady_clock::now()) next = std::chrono::steady_clock::now() + period;
        timer.expires_at(next);
        co_await timer.async_wait(asio::use_awaitable);
    }
}

asio::awaitable<int> Run(std::string codec, bool simulcast) {
    const bool camera = std::getenv("QUALITY_PROBE_CAMERA") != nullptr;
    const auto executor = co_await asio::this_coro::executor;
    auto sender = livekit::Room::Create(executor);
    auto receiver = livekit::Room::Create(executor);
    auto late = livekit::Room::Create(executor);
    auto observed = std::make_shared<Receiver>();
    auto late_observed = std::make_shared<Receiver>();
    receiver->AddListener(observed); late->AddListener(late_observed);
    // Only allowlisted probe records are persisted by the launcher.
    for (auto room : {sender, receiver, late}) room->SetLogHandler([](const auto&, const auto&, const auto&) {});
    auto generator = std::make_shared<Generator>();
    int result = 1;
    try {
        livekit::SignalOptions connection;
        connection.auto_subscribe = true; connection.single_peer_connection = true;
        connection.allow_insecure_transport = std::getenv("LIVEKIT_TEST_ALLOW_INSECURE") != nullptr;
        connection.connect_timeout = 20s;
        auto restricted = std::make_shared<bool>(false);
        if (std::getenv("QUALITY_PROBE_BACKUP")) {
            // Test-only scheduling point: SUB_PERM is emitted after native PC
            // installation and before the first single-PC offer. Restrict the
            // capability m-line before Pion caches its negotiated codec list.
            // No raw logs or credentials leave this callback.
            receiver->SetLogHandler([weak = std::weak_ptr(receiver), restricted](const auto&, const auto& code, const auto&) {
                if (code == "SUB_PERM") if (auto room = weak.lock())
                    *restricted = livekit::RoomUnpublishTestAccess::RestrictInitialReceiverCodec(*room, "VP8");
            });
        }
        if (std::getenv("QUALITY_PROBE_BACKUP")) {
            late->SetLogHandler([weak = std::weak_ptr(late)](const auto&, const auto& code, const auto&) {
                if (code == "SUB_PERM") if (auto room = weak.lock())
                    livekit::RoomUnpublishTestAccess::RestrictInitialReceiverCodec(*room, "VP9");
            });
        }
        co_await receiver->ConnectAsync(std::getenv("LIVEKIT_URL"), std::getenv("LIVEKIT_PEER_TOKEN"), connection);
        if (std::getenv("QUALITY_PROBE_BACKUP")) Check(*restricted, "initial_vp8_capability_not_restricted");
        co_await sender->ConnectAsync(std::getenv("LIVEKIT_URL"), std::getenv("LIVEKIT_TOKEN"), connection);
        if (std::getenv("QUALITY_PROBE_BACKUP")) Emit({{"event","backup_connection"},{"code","sender_connected"}});
        livekit::VideoPublishOptions options;
        options.video_codec = codec; options.simulcast = simulcast; options.auto_backup_codec = false;
        if (std::getenv("QUALITY_PROBE_BACKUP")) {
            options.backup_codec = "vp8";
            options.backup_codec_policy = livekit::BackupCodecPolicy::Simulcast;
        }
        // Match the application's default: codec selection does not request SVC.
        auto track = livekit::LocalVideoTrack::createLocalVideoTrack(
            "quality-synthetic", generator->source,
            camera ? livekit::TrackSource::Camera : livekit::TrackSource::ScreenShareVideo, options);
        asio::co_spawn(executor, Frames(generator), asio::detached);
        auto publication = co_await sender->local_participant()->PublishTrackAsync(track);
        if (std::getenv("QUALITY_PROBE_BACKUP")) Emit({{"event","backup_connection"},{"code","sender_published"}});
        const auto sid = publication->sid();
        const auto rtc = track->rtc_track();
        const auto pc_counts = sender->GetPublisherMediaObjectCounts();
        struct Stage { int width, height, fps; };
        const std::vector<Stage> stages = camera
            ? std::vector<Stage>{{1280,720,15},{1280,720,20},{1280,720,30}}
            : std::vector<Stage>{{1280,720,15},{1920,1080,20},{2560,1440,30},{1920,1080,15},{1280,720,20},
                                 {1280,720,15},{1920,1080,20},{2560,1440,30},{1920,1080,15},{1280,720,20}};
        for (int index = 0; index < int(stages.size()); ++index) {
            const auto stage = stages[index];
            co_await livekit::RoomUnpublishTestAccess::Update(*sender, track, stage.width, stage.height, stage.fps);
            generator->width = stage.width; generator->height = stage.height; generator->fps = stage.fps;
            co_await Until([&] { return bool(observed->publication); }, "remote_publication_missing");
            const int before_main = observed->frames;
            Check(observed->publication->SetVideoDimensions(stage.width, stage.height), "durable_main_request_rejected");
            co_await Until([&] { return observed->frames > before_main && observed->width == stage.width && observed->height == stage.height; },
                           "remote_dimensions_not_converged");
            const int late_join_index = std::getenv("QUALITY_PROBE_BACKUP") ? 0 : 1;
            if (index == late_join_index) {
                co_await late->ConnectAsync(std::getenv("LIVEKIT_URL"), std::getenv("LIVEKIT_LATE_TOKEN"), connection);
                co_await Until([&] { return bool(late_observed->publication); }, "late_publication_missing");
                Check(late_observed->publication->SetVideoDimensions(stage.width, stage.height), "durable_late_request_rejected");
                co_await Until([&] { return late_observed->width == stage.width && late_observed->height == stage.height; },
                               "late_join_dimensions_not_converged");
            } else if (index > late_join_index) {
                co_await Until([&] { return bool(late_observed->publication); }, "late_publication_missing");
                Check(late_observed->publication->SetVideoDimensions(stage.width, stage.height), "durable_late_request_rejected");
                co_await Until([&] { return late_observed->width == stage.width && late_observed->height == stage.height; },
                               "late_peer_switch_failed");
            }
            if (std::getenv("QUALITY_PROBE_BACKUP")) {
                const int before = late_observed->frames;
                Check(late_observed->publication->SetVideoDimensions(stage.width / 2, stage.height / 2), "primary_low_request_rejected");
                co_await Until([&] { return late_observed->frames > before && late_observed->width == stage.width / 2 &&
                    late_observed->height == stage.height / 2; }, "primary_low_not_converged");
                Check(late_observed->publication->SetVideoDimensions(stage.width, stage.height), "primary_main_request_rejected");
                co_await Until([&] { return late_observed->width == stage.width && late_observed->height == stage.height; }, "primary_main_not_converged");
            }
            if (simulcast) {
                Check(observed->publication && observed->publication->SetVideoDimensions(stage.width / 2, stage.height / 2),
                      "durable_dimension_request_rejected");
                const int layer_before = observed->frames;
                const auto layer_start = std::chrono::steady_clock::now();
                while ((observed->frames <= layer_before || observed->width != stage.width / 2 || observed->height != stage.height / 2) &&
                       std::chrono::steady_clock::now() - layer_start < 25s) co_await Delay(25ms);
                const bool dimension_selected = observed->frames > layer_before &&
                    observed->width == stage.width / 2 && observed->height == stage.height / 2;
                if (!dimension_selected) {
                    const auto diagnostic = co_await sender->GetStats();
                    nlohmann::json streams = nlohmann::json::array();
                    for (const auto& report : diagnostic.reports) for (const auto& stream : report.outbound_rtp)
                        streams.push_back({{"rid",stream.rid},{"width",stream.frame_width},{"height",stream.frame_height},
                            {"fps",stream.frames_per_second},{"frames",stream.frames_encoded},{"bytes",stream.bytes_sent}});
                    Emit({{"event","layer_failure_stats"},{"streams",streams}});
                }
                Emit({{"event","layer_dimensions"},{"index",index},{"matched",dimension_selected},
                      {"convergence_ms",std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-layer_start).count()},
                      {"requested_width",stage.width/2},{"requested_height",stage.height/2},
                      {"width",observed->width.load()},{"height",observed->height.load()}});
                Check(observed->publication->SetVideoDimensions(0, 0) &&
                      observed->publication->SetVideoQuality(livekit::proto::VideoQuality::LOW),
                      "durable_low_request_rejected");
                const int low_before = observed->frames;
                const auto low_start = std::chrono::steady_clock::now();
                while ((observed->frames <= low_before || observed->width != stage.width / 2 || observed->height != stage.height / 2) &&
                       std::chrono::steady_clock::now() - low_start < 25s) co_await Delay(25ms);
                Emit({{"event","layer_explicit_low"},{"index",index},
                      {"matched",observed->frames > low_before && observed->width == stage.width / 2 && observed->height == stage.height / 2},
                      {"width",observed->width.load()},{"height",observed->height.load()}});
                Check(observed->publication->SetVideoDimensions(stage.width, stage.height), "durable_high_request_rejected");
                co_await Until([&] { return observed->width == stage.width && observed->height == stage.height; },
                               "simulcast_high_layer_not_restored");
            }
            co_await Delay(1500ms);
            const int before = observed->frames;
            const auto start = std::chrono::steady_clock::now();
            co_await Delay(4000ms);
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            const double fps = (observed->frames - before) / seconds;
            Check(fps >= stage.fps * 0.70 && fps <= stage.fps * 1.20, "remote_fps_outside_probe_tolerance");
            Check(publication->sid() == sid && track->rtc_track() == rtc && observed->sid == sid &&
                  observed->subscriptions == 1 && observed->removed == 0, "track_identity_changed");
            Check(sender->GetPublisherMediaObjectCounts().transceivers == pc_counts.transceivers,
                  "transceiver_count_changed");
            auto stats = co_await sender->GetStats();
            bool codec_verified = false;
            uint64_t packets = 0, bytes = 0, encoded = 0;
            for (const auto& report : stats.reports) for (const auto& out : report.outbound_rtp) {
                if (!out.frames_encoded_available || out.frames_encoded == 0) continue;
                packets += out.packets_sent; bytes += out.bytes_sent; encoded += out.frames_encoded;
                for (const auto& value : report.codecs) {
                    if (value.id != out.codec_id) continue;
                    auto mime = value.mime_type;
                    std::transform(mime.begin(), mime.end(), mime.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                    if (mime == "video/" + codec) codec_verified = true;
                }
            }
            Check(codec_verified, "requested_codec_not_on_wire");
            if (std::getenv("QUALITY_PROBE_BACKUP")) {
                const auto received_stats = co_await receiver->GetStats();
                bool backup_decoded = false;
                for (const auto& report : received_stats.reports) for (const auto& incoming : report.inbound_rtp)
                    if (incoming.frames_decoded_available && incoming.frames_decoded > 0)
                        for (const auto& wire_codec : report.codecs) if (wire_codec.id == incoming.codec_id &&
                            (wire_codec.mime_type == "video/VP8" || wire_codec.mime_type == "video/vp8")) backup_decoded = true;
                Emit({{"event","backup_decode"},{"index",index},{"matched",backup_decoded},{"codec","vp8"}});
                Check(backup_decoded, "backup_codec_not_decoded");
                const auto primary_stats = co_await late->GetStats();
                bool primary_decoded = false;
                for (const auto& report : primary_stats.reports) for (const auto& incoming : report.inbound_rtp)
                    if (incoming.frames_decoded_available && incoming.frames_decoded > 0)
                        for (const auto& wire_codec : report.codecs) if (wire_codec.id == incoming.codec_id &&
                            (wire_codec.mime_type == "video/VP9" || wire_codec.mime_type == "video/vp9")) primary_decoded = true;
                Emit({{"event","primary_decode"},{"index",index},{"matched",primary_decoded},{"codec","vp9"}});
                Check(primary_decoded, "primary_codec_not_decoded");
            }
            Emit({{"event","stage"}, {"index",index}, {"codec",codec}, {"simulcast",simulcast},
                  {"cycle_id",index/5},{"source",camera?"camera":"screen"},
                  {"rtp_packets_sent",packets},{"rtp_bytes_sent",bytes},{"frames_encoded",encoded},
                  {"decoded_frames_sample",observed->frames.load()-before},
                  {"width",stage.width}, {"height",stage.height}, {"target_fps",stage.fps},
                  {"decoded_fps",fps}, {"sid",sid}, {"same_track",true}, {"codec_verified",true},
                  {"late_peer_verified",index >= 1}});
            const char* ack_root = std::getenv("QUALITY_ACK_DIR");
            if (ack_root) {
                const auto ack = std::filesystem::path(ack_root) / (std::to_string(index) + ".ack");
                co_await Until([&] { return std::filesystem::exists(ack); }, "metadata_observer_timeout", 45);
            }
        }
        if (std::getenv("QUALITY_PROBE_WEAK")) {
            Check(simulcast, "weak_probe_requires_managed_simulcast");
            co_await livekit::RoomUnpublishTestAccess::LimitSubscriber(*receiver, 1000);
            const int before_limit = observed->frames;
            Emit({{"event","weak_start"},{"subscriber_cap_bps",1000},{"seconds",30}});
            co_await Delay(15000ms);
            const auto change_start = std::chrono::steady_clock::now();
            co_await livekit::RoomUnpublishTestAccess::Update(*sender, track, 1920, 1080, 20);
            generator->width = 1920; generator->height = 1080; generator->fps = 20;
            const auto change_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - change_start).count();
            Check(observed->publication->SetVideoDimensions(1920, 1080), "weak_dimension_request_rejected");
            co_await Delay(15000ms);
            const int limited_frames = observed->frames.load() - before_limit;
            co_await livekit::RoomUnpublishTestAccess::LimitSubscriber(*receiver, 0);
            const int before_recovery = observed->frames;
            co_await Until([&] { return observed->frames > before_recovery + 5 && observed->width == 1920 && observed->height == 1080; },
                           "weak_recovery_failed");
            Check(publication->sid() == sid && track->rtc_track() == rtc && observed->subscriptions == 1 && observed->removed == 0,
                  "weak_identity_changed");
            Emit({{"event","weak_result"},{"status","PASS"},{"subscriber_cap_bps",1000},
                  {"limited_received_frames",limited_frames},{"quality_change_ms",change_ms},{"same_track",true},
                  {"restored_width",observed->width.load()},{"restored_height",observed->height.load()}});
        }
        co_await sender->local_participant()->UnpublishTrackAsync(sid);
        result = 0;
    } catch (const ProbeFailure& error) {
        Emit({{"event","failure"},{"code",error.code}});
    } catch (const livekit::OperationError& error) {
        Emit({{"event","failure"},{"code","operation_failed"},
              {"operation",int(error.operation())},{"error_code",int(error.code())},{"stage",error.stage()}});
    } catch (const std::exception&) {
        Emit({{"event","failure"},{"code","probe_exception"}});
    } catch (...) { Emit({{"event","failure"},{"code","unknown_exception"}}); }
    if (result != 0) {
        Emit({{"event","sender_parameters"},{"senders",livekit::RoomUnpublishTestAccess::SenderSnapshot(*sender)}});
        for (const auto& [label, room] : std::vector<std::pair<std::string, std::shared_ptr<livekit::Room>>>{{"sender",sender},{"receiver",receiver},{"late",late}}) {
            try {
                const auto stats = co_await room->GetStats();
                nlohmann::json streams = nlohmann::json::array();
                for (const auto& report : stats.reports) {
                    for (const auto& out : report.outbound_rtp) if (out.kind == "video")
                        streams.push_back({{"direction","out"},{"rid",out.rid},{"width",out.frame_width},{"height",out.frame_height},
                            {"fps",out.frames_per_second},{"frames",out.frames_encoded},{"bytes",out.bytes_sent}});
                    for (const auto& in : report.inbound_rtp) if (in.kind == "video")
                        streams.push_back({{"direction","in"},{"frames",in.frames_decoded},{"bytes",in.bytes_received}});
                }
                Emit({{"event","failure_stats"},{"peer",label},{"streams",streams}});
            } catch (...) {}
        }
    }
    generator->running = false;
    co_await Delay(100ms);
    co_await late->DisconnectAsync(); co_await receiver->DisconnectAsync(); co_await sender->DisconnectAsync();
    receiver->RemoveListener(observed); late->RemoveListener(late_observed);
    Emit({{"event","result"},{"status",result == 0 ? "PASS" : "FAIL"}});
    co_return result;
}
}
int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--product-ui-observer") {
        if (!std::getenv("LIVEKIT_URL") || !std::getenv("RECEIVER_TOKEN") ||
            !std::filesystem::is_directory(argv[2])) return 2;
        asio::io_context io;
        auto future = asio::co_spawn(asio::make_strand(io), RunProductUiObserver(std::filesystem::path(argv[2])), asio::use_future);
        io.run();
        try { return future.get(); } catch (...) { return 1; }
    }
    if (argc < 2 || !std::getenv("LIVEKIT_URL") || !std::getenv("LIVEKIT_TOKEN") ||
        !std::getenv("LIVEKIT_PEER_TOKEN") || !std::getenv("LIVEKIT_LATE_TOKEN")) return 2;
    asio::io_context io;
    auto future = asio::co_spawn(asio::make_strand(io), Run(argv[1], argc > 2), asio::use_future);
    io.run();
    try { return future.get(); } catch (...) { return 1; }
}
