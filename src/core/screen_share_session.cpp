#include "screen_share_session.h"
#include "room.h"
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <random>

namespace livekit {
using namespace std::chrono_literals;
namespace {
std::string NewShareSessionId() {
    std::array<unsigned char, 16> bytes{};
    std::random_device random;
    for (auto &byte : bytes) byte = static_cast<unsigned char>(random());
    constexpr char digits[] = "0123456789abcdef";
    std::string result = "share-";
    result.reserve(6 + bytes.size() * 2);
    for (const auto byte : bytes) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0xf]);
    }
    return result;
}
}

struct ScreenShareSession::Run {
    struct CaptureCleanup {
        std::mutex mutex;
        std::unique_ptr<IDesktopCapture> capture;
        std::atomic<bool> complete{false};
        std::unique_ptr<IDesktopCapture> Claim() {
            std::lock_guard lock(mutex);
            return std::move(capture);
        }
    };
    struct AudioCaptureCleanup {
        std::mutex mutex;
        std::unique_ptr<ISystemAudioCapture> capture;
        std::atomic<bool> complete{false};
        std::unique_ptr<ISystemAudioCapture> Claim() {
            std::lock_guard lock(mutex);
            return std::move(capture);
        }
    };
    explicit Run(asio::any_io_executor executor, uint64_t id,
                 std::shared_ptr<void> executor_lifetime)
        : executor_lifetime(std::move(executor_lifetime)), timer(executor), audio_timer(executor), id(id),
          source(std::make_shared<VideoSource>(0, 0)),
          preview(std::make_shared<render::VideoRenderRouter>(id, 1)) {}
    // Capture callbacks can retain Run independently of ScreenShareSession.
    // In particular the timer must be destroyed before its context lease.
    const std::shared_ptr<void> executor_lifetime;
    asio::steady_timer timer;
    asio::steady_timer audio_timer;
    const uint64_t id;
    std::unique_ptr<IDesktopCapture> capture;
    std::shared_ptr<CaptureCleanup> capture_cleanup; // strand; job owns stop/join
    std::shared_ptr<VideoSource> source;
    std::shared_ptr<LocalVideoTrack> track;
    std::shared_ptr<render::VideoRenderRouter> preview;
    std::string source_title;
    DesktopSourceKind source_kind = DesktopSourceKind::Window;
    std::optional<ScreenBinding> annotation_binding;
    // Retain source identity checks even while annotations are invalidated by
    // a temporary desktop geometry change. Capture stays on the same source.
    std::optional<ScreenBinding> source_binding;
    std::mutex delivery;
    bool accepting = true; // delivery mutex; Stop is a frame barrier
    std::atomic<bool> first_frame{false};
    std::atomic<bool> ended{false};
    std::optional<DesktopCapturePhase> capture_phase; // delivery mutex
    std::optional<std::chrono::steady_clock::time_point> ready_at; // delivery mutex
    bool stopping = false; // strand
    bool published = false;
    bool driving = false;
    ScreenShareQuality requested;
    std::optional<ScreenShareFrameProfile> applied;
    std::optional<ScreenShareFrameProfile> delivered; // delivery mutex, bounded mailbox
    uint64_t revision = 1;
    bool quality_dirty = false;
    std::chrono::steady_clock::time_point next_metadata_retry{};
    ScreenShareQualityStatus quality_status = ScreenShareQualityStatus::Pending;
    ScreenShareAudioOptions audio_options;
    ScreenShareAudioState audio_state = ScreenShareAudioState::Disabled;
    ScreenShareAudioError audio_error = ScreenShareAudioError::None;
    std::unique_ptr<ISystemAudioCapture> audio_capture;
    std::shared_ptr<AudioCaptureCleanup> audio_cleanup;
    std::shared_ptr<AudioSource> audio_source;
    std::shared_ptr<LocalAudioTrack> audio_track;
    std::mutex audio_delivery;
    uint64_t audio_epoch = 0; // strand writes under audio_delivery; callback generation
    bool audio_accepting = false; // audio_delivery; independent PCM barrier
    std::optional<SystemAudioCaptureStatus> audio_status; // bounded mailbox
    bool audio_first_frame = false; // audio_delivery
    bool audio_muted = false; // strand; native mute is SDK-gated
    bool audio_stopping = false;
    bool audio_publish_attempted = false;
    bool audio_driving = false;
};

ScreenShareSession::Backend ScreenShareSession::ForRoom(const std::shared_ptr<Room>& room) {
    Backend backend;
    const std::weak_ptr<Room> weak = room;
    backend.connected = [weak] {
        auto room = weak.lock();
        return room && room->connection_state() == ConnectionState::Connected;
    };
    backend.publish = [weak](std::shared_ptr<LocalVideoTrack> track) -> asio::awaitable<void> {
        auto room = weak.lock();
        auto local = room ? room->local_participant() : nullptr;
        if (!local) throw std::runtime_error("screen publish session closed");
        co_await local->PublishTrackAsync(std::move(track));
    };
    backend.unpublish = [weak](std::shared_ptr<LocalVideoTrack> track) -> asio::awaitable<void> {
        auto room = weak.lock();
        auto local = room ? room->local_participant() : nullptr;
        if (!local) throw std::runtime_error("screen unpublish session closed");
        // Full reconnect may replace the SID. Resolve the current publication
        // by track instance, never retain the pre-reconnect SID in a UI value.
        for (const auto& [sid, publication] : local->tracks()) {
            if (publication && publication->track() == track) {
                co_await local->UnpublishTrackAsync(sid);
                co_return;
            }
        }
        // A completed full reconnect can legitimately retire this publication.
        // Do not touch the camera or a successor screen track by source alone.
    };
    backend.publish_audio = [weak](std::shared_ptr<LocalAudioTrack> track) -> asio::awaitable<void> {
        auto room = weak.lock();
        auto local = room ? room->local_participant() : nullptr;
        if (!local) throw std::runtime_error("share audio publish session closed");
        co_await local->PublishTrackAsync(std::move(track));
    };
    backend.unpublish_audio = [weak](std::shared_ptr<LocalAudioTrack> track) -> asio::awaitable<void> {
        auto room = weak.lock();
        auto local = room ? room->local_participant() : nullptr;
        if (!local) throw std::runtime_error("share audio unpublish session closed");
        for (const auto& [sid, publication] : local->tracks()) {
            if (publication && publication->track() == track) {
                co_await local->UnpublishTrackAsync(sid);
                co_return;
            }
        }
    };
    backend.mute_audio = [weak](std::shared_ptr<LocalAudioTrack> track, bool muted) {
        auto room = weak.lock();
        auto local = room ? room->local_participant() : nullptr;
        if (!local) return;
        for (const auto& [sid, publication] : local->tracks()) {
            if (publication && publication->track() == track) {
                local->SetMuted(sid, muted);
                return;
            }
        }
    };
    backend.apply_quality = [weak](auto track, auto profile) -> asio::awaitable<void> {
        auto room = weak.lock();
        if (!room) throw std::runtime_error("screen room retired");
        co_await room->ApplyScreenShareSenderParametersAsync(std::move(track), profile);
    };
    backend.sync_quality = [weak](auto track, auto profile) -> asio::awaitable<void> {
        auto room = weak.lock();
        if (!room) throw std::runtime_error("screen room retired");
        co_await room->SyncScreenShareMetadataAsync(std::move(track), profile);
    };
    return backend;
}

ScreenShareSession::ScreenShareSession(asio::any_io_executor strand, Backend backend,
                                     Observer observer, std::shared_ptr<void> executor_lifetime)
    : executor_lifetime_(std::move(executor_lifetime)), strand_(std::move(strand)),
      backend_(std::move(backend)), observer_(std::move(observer)) {}

ScreenShareSession::~ScreenShareSession() {
    if (run_) StopFrames(run_);
}

void ScreenShareSession::SetState(ScreenShareState state, ScreenShareError error) {
    snapshot_ = {};
    snapshot_.state = state;
    snapshot_.error = error;
    if (run_) {
        snapshot_.run_id = run_->id;
        snapshot_.audio_epoch = run_->audio_epoch;
        snapshot_.requested_quality = run_->requested;
        snapshot_.applied_quality = run_->applied;
        snapshot_.quality_status = run_->quality_status;
        snapshot_.source_title = run_->source_title;
        snapshot_.source_kind = run_->source_kind;
        snapshot_.annotation_binding = run_->annotation_binding;
        snapshot_.track_sid = run_->track ? run_->track->sid() : std::string();
        snapshot_.audio_state = run_->audio_state;
        snapshot_.audio_error = run_->audio_error;
        snapshot_.audio_muted = run_->audio_muted;
        snapshot_.audio_output_name = run_->audio_options.output_name;
        snapshot_.audio_output_id = run_->audio_options.output_id;
        snapshot_.audio_track_sid = run_->audio_track ? run_->audio_track->sid() : std::string();
        if (state == ScreenShareState::Active) snapshot_.preview = run_->preview;
    }
    if (!closed_ && observer_) observer_(snapshot_);
}

void ScreenShareSession::StopFrames(const std::shared_ptr<Run>& run) {
    run->audio_stopping = true;
    if (run->audio_driving || run->audio_publish_attempted)
        run->audio_state = ScreenShareAudioState::Stopping;
    StopAudioFrames(run);
    run->audio_timer.cancel();
    {
        std::lock_guard lock(run->delivery);
        run->accepting = false;
        run->preview->Deactivate(run->id);
    }
    if (run->capture) {
        auto cleanup = std::make_shared<Run::CaptureCleanup>();
        cleanup->capture = std::move(run->capture);
        run->capture_cleanup = cleanup;
        backend_.dispatch_cleanup([cleanup, lifetime = run->executor_lifetime] {
            auto capture = cleanup->Claim();
            if (capture) capture->Stop();
            capture.reset();
            cleanup->complete.store(true, std::memory_order_release);
        });
    }
}

void ScreenShareSession::StopAudioFrames(const std::shared_ptr<Run>& run) {
    {
        std::lock_guard lock(run->audio_delivery);
        run->audio_accepting = false;
    }
    if (run->audio_capture) {
        auto cleanup = std::make_shared<Run::AudioCaptureCleanup>();
        cleanup->capture = std::move(run->audio_capture);
        run->audio_cleanup = cleanup;
        backend_.dispatch_cleanup([cleanup, lifetime = run->executor_lifetime] {
            auto capture = cleanup->Claim();
            if (capture) capture->Stop();
            capture.reset();
            cleanup->complete.store(true, std::memory_order_release);
        });
    }
}

void ScreenShareSession::Start(DesktopSource source, VideoPublishOptions options, ScreenShareQuality quality,
                               ScreenShareAudioOptions audio) {
    if (!quality.valid()) return;
    if (closed_ || run_ || !transport_ready_ || !backend_.connected()) return;
    auto run = std::make_shared<Run>(strand_, ++next_run_, executor_lifetime_);
    run->requested = quality;
    run->source_title = source.title;
    run->source_kind = source.kind;
    run->audio_options = std::move(audio);
    if (source.kind == DesktopSourceKind::Screen && backend_.resolve_screen_binding) {
        try {
            run->annotation_binding = backend_.resolve_screen_binding(
                source, run->id, NewShareSessionId());
            run->source_binding = run->annotation_binding;
        } catch (...) {
            run->annotation_binding.reset();
        }
    }
    run_ = run;
    SetState(ScreenShareState::Starting);
    run->driving = true;
    options.source = TrackSource::ScreenShareVideo;
    asio::co_spawn(strand_, Drive(
        shared_from_this(), run, std::move(source), std::move(options)), asio::detached);
}

void ScreenShareSession::StartAudioDriver(const std::shared_ptr<Run>& run) {
    if (run->audio_driving) return;
    run->audio_driving = true;
    asio::co_spawn(strand_, DriveAudio(shared_from_this(), run, run->audio_epoch), asio::detached);
}

void ScreenShareSession::StartAudio(ScreenShareAudioOptions audio, uint64_t expected_run,
                                   std::optional<uint64_t> expected_audio_epoch) {
    if (!audio.enabled || closed_ || !run_ || run_->stopping ||
        (expected_run && run_->id != expected_run) ||
        (expected_audio_epoch && run_->audio_epoch != *expected_audio_epoch) ||
        snapshot_.state != ScreenShareState::Active || !transport_ready_ || !backend_.connected() ||
        (run_->audio_state != ScreenShareAudioState::Disabled && run_->audio_state != ScreenShareAudioState::Failed) ||
        run_->audio_driving || run_->audio_publish_attempted || run_->audio_capture ||
        run_->audio_track || run_->audio_source ||
        (run_->audio_cleanup && !run_->audio_cleanup->complete.load(std::memory_order_acquire))) return;
    auto run = run_;
    run->audio_options = std::move(audio);
    run->audio_cleanup.reset();
    run->audio_stopping = false;
    run->audio_muted = false;
    run->audio_error = ScreenShareAudioError::None;
    {
        std::lock_guard lock(run->audio_delivery);
        ++run->audio_epoch;
        run->audio_accepting = false;
        run->audio_status.reset();
        run->audio_first_frame = false;
    }
    if (!backend_.audio_available()) {
        run->audio_state = ScreenShareAudioState::Failed;
        run->audio_error = ScreenShareAudioError::Unavailable;
        SetState(ScreenShareState::Active);
        return;
    }
    run->audio_state = ScreenShareAudioState::Starting;
    SetState(ScreenShareState::Active);
    StartAudioDriver(run);
}

void ScreenShareSession::SetAudioMuted(bool muted, uint64_t expected_run,
                                      std::optional<uint64_t> expected_audio_epoch) {
    if (closed_ || !run_ || run_->stopping || run_->audio_stopping ||
        (expected_run && run_->id != expected_run) ||
        (expected_audio_epoch && run_->audio_epoch != *expected_audio_epoch) ||
        run_->audio_state != ScreenShareAudioState::Active || !transport_ready_ ||
        !backend_.audio_available()) return;
    if (run_->audio_muted == muted) return;
    run_->audio_muted = muted;
    run_->audio_track->set_muted(muted);
    if (backend_.mute_audio) backend_.mute_audio(run_->audio_track, muted);
    SetState(snapshot_.state, snapshot_.error);
}

void ScreenShareSession::StopAudio(uint64_t expected_run,
                                  std::optional<uint64_t> expected_audio_epoch) {
    if (closed_ || !run_ || !run_->audio_options.enabled ||
        (expected_run && run_->id != expected_run) ||
        (expected_audio_epoch && run_->audio_epoch != *expected_audio_epoch) ||
        run_->audio_state == ScreenShareAudioState::Disabled ||
        run_->audio_state == ScreenShareAudioState::Failed) return;
    auto run = run_;
    run->audio_stopping = true;
    StopAudioFrames(run);
    run->audio_timer.cancel();
    run->audio_state = ScreenShareAudioState::Stopping;
    SetState(snapshot_.state, snapshot_.error);
    StartAudioDriver(run);
}

void ScreenShareSession::SetQuality(ScreenShareQuality quality) {
    if (!quality.valid() || closed_ || !run_ || run_->stopping || !transport_ready_ ||
        snapshot_.state != ScreenShareState::Active || run_->quality_status == ScreenShareQualityStatus::Degraded) return;
    run_->requested = quality;
    ++run_->revision;
    run_->quality_dirty = true;
    run_->quality_status = ScreenShareQualityStatus::Pending;
    run_->timer.cancel();
    SetState(ScreenShareState::Active);
}

void ScreenShareSession::SetTransportReady(bool ready) {
    const bool restoring = ready && !transport_ready_;
    if (ready != transport_ready_) ++transport_revision_;
    transport_ready_ = ready;
    if (restoring && run_ && !run_->stopping && run_->quality_status != ScreenShareQualityStatus::Degraded) {
        run_->quality_dirty = true;
        run_->quality_status = ScreenShareQualityStatus::Pending;
        run_->timer.cancel();
    }
}

asio::awaitable<void> ScreenShareSession::ApplyQuality(std::shared_ptr<ScreenShareSession> self, std::shared_ptr<Run> run) {
    if (!run->applied || !run->capture) co_return;
    const auto previous = *run->applied;
    const auto revision = run->revision;
    const auto transport_revision = self->transport_revision_;
    const auto requested = run->requested;
    std::optional<ScreenShareFrameProfile> latest;
    { std::lock_guard lock(run->delivery); latest = run->delivered; }
    const auto geometry = latest.value_or(previous);
    auto target = ResolveScreenShareProfile(geometry.source_width, geometry.source_height, requested, revision);
    bool failed = !target;
    bool uncertain = false;
    auto alive = [&] { return !self->closed_ && self->run_ == run && !run->stopping; };
    auto callback = [weak = std::weak_ptr(run)](ScreenShareFrameProfile profile) {
        if (auto run = weak.lock()) {
            std::lock_guard lock(run->delivery);
            if (run->accepting) run->delivered = profile;
        }
    };
    if (target) try {
        co_await self->backend_.apply_quality(run->track, *target);
        if (!alive()) co_return;
        if (!run->capture->SetQuality(requested, revision, callback)) throw std::runtime_error("capture rejected quality");
        const auto deadline = std::chrono::steady_clock::now() + self->backend_.first_frame_timeout;
        for (;;) {
            if (!alive()) co_return;
            { std::lock_guard lock(run->delivery); latest = run->delivered; }
            if (latest && latest->revision == revision && latest->quality == requested) break;
            if (run->ended.load() || std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("quality frame timeout");
            run->timer.expires_after(20ms);
            std::error_code error;
            co_await run->timer.async_wait(asio::redirect_error(asio::use_awaitable, error));
        }
        if (latest->width != target->width || latest->height != target->height)
            co_await self->backend_.apply_quality(run->track, *latest);
        if (!alive()) co_return;
        run->track->SetScreenShareProfile(*latest);
        run->applied = latest;
    } catch (const OperationError& error) {
        failed = true;
        uncertain = error.code() == OperationErrorCode::StateUncertain;
    } catch (...) { failed = true; }
    if (!alive()) co_return;
    if (failed && transport_revision != self->transport_revision_) {
        // A reconnect may retire the old sender during this transaction. Replay
        // the latest intent against the recovered publication; never compensate
        // through a disconnected or replaced transport.
        run->quality_dirty = true;
        run->quality_status = ScreenShareQualityStatus::Pending;
        self->SetState(ScreenShareState::Active);
        co_return;
    }
    if (failed) {
        // Compensation keeps capture/publication/preview ownership intact.
        bool restored = !uncertain;
        const bool superseded = run->revision != revision;
        const auto recovery_revision = ++run->revision;
        if (!superseded) run->requested = previous.quality;
        try {
            co_await self->backend_.apply_quality(run->track, previous);
            if (!alive()) co_return;
            restored = run->capture->SetQuality(previous.quality, recovery_revision, callback) && restored;
            const auto deadline = std::chrono::steady_clock::now() + self->backend_.first_frame_timeout;
            while (restored && alive()) {
                { std::lock_guard lock(run->delivery); latest = run->delivered; }
                if (latest && latest->quality == previous.quality && latest->revision == recovery_revision) break;
                if (run->ended.load() || std::chrono::steady_clock::now() >= deadline) { restored = false; break; }
                run->timer.expires_after(20ms);
                std::error_code error;
                co_await run->timer.async_wait(asio::redirect_error(asio::use_awaitable, error));
            }
        } catch (...) { restored = false; }
        if (!alive()) co_return;
        if (restored && latest) {
            run->applied = latest;
            run->track->SetScreenShareProfile(*latest);
        }
        run->quality_status = restored ? ScreenShareQualityStatus::Rejected : ScreenShareQualityStatus::Degraded;
        run->quality_dirty = restored && (superseded || run->revision != recovery_revision);
        if (run->quality_dirty) run->quality_status = ScreenShareQualityStatus::Pending;
    } else {
        bool synced = false;
        try { co_await self->backend_.sync_quality(run->track, *run->applied); synced = true; } catch (...) {}
        if (!alive()) co_return;
        run->quality_dirty = run->revision != revision || transport_revision != self->transport_revision_;
        run->quality_status = run->quality_dirty ? ScreenShareQualityStatus::Pending :
            (synced ? ScreenShareQualityStatus::Applied : ScreenShareQualityStatus::MetadataPending);
    }
    self->SetState(ScreenShareState::Active);
}

void ScreenShareSession::Stop() {
    if (closed_ || !run_) return;
    auto run = run_;
    run->stopping = true;
    StopFrames(run);
    run->timer.cancel();
    // Reserve the parent driver before starting an audio retry: co_spawn can
    // complete inline on this strand and otherwise finish run_ reentrantly.
    const bool start_driver = !run->driving;
    if (start_driver) run->driving = true;
    SetState(ScreenShareState::Stopping);
    if (run->audio_publish_attempted && !run->audio_driving) StartAudioDriver(run);
    if (start_driver) {
        asio::co_spawn(strand_, Drive(
            shared_from_this(), run, {}, VideoPublishOptions{}), asio::detached);
    }
}

void ScreenShareSession::Shutdown() {
    closed_ = true;
    observer_ = {};
    if (run_) {
        run_->stopping = true;
        StopFrames(run_);
        run_->timer.cancel();
        run_.reset();
    }
    snapshot_ = {};
}

ScreenShareCaptureResources ScreenShareSession::TakeCapturesForShutdown() {
    closed_ = true;
    observer_ = {};
    ScreenShareCaptureResources capture;
    if (run_) {
        run_->stopping = true;
        run_->audio_stopping = true;
        {
            std::lock_guard lock(run_->delivery);
            run_->accepting = false;
            run_->preview->Deactivate(run_->id);
        }
        {
            std::lock_guard lock(run_->audio_delivery);
            run_->audio_accepting = false;
        }
        capture.desktop = std::move(run_->capture);
        capture.system_audio = std::move(run_->audio_capture);
        // Session retirement may already occupy the same cleanup worker while
        // awaiting this strand barrier. Reclaim an unstarted queued stop so
        // that retirement can stop capture before destroying Room/SDK owners;
        // its later queued job simply observes that ownership was transferred.
        if (!capture.desktop && run_->capture_cleanup)
            capture.desktop = run_->capture_cleanup->Claim();
        if (!capture.system_audio && run_->audio_cleanup)
            capture.system_audio = run_->audio_cleanup->Claim();
        run_->timer.cancel();
        run_->audio_timer.cancel();
        run_.reset();
    }
    snapshot_ = {};
    return capture;
}

asio::awaitable<void> ScreenShareSession::DriveAudio(std::shared_ptr<ScreenShareSession> self,
                                                   std::shared_ptr<Run> run, uint64_t audio_epoch) {
    auto failure = run->audio_error == ScreenShareAudioError::Unpublish
        ? ScreenShareAudioError::None : run->audio_error;
    const auto current = [&] {
        return !self->closed_ && self->run_ == run && run->audio_epoch == audio_epoch;
    };
    const auto notify = [&] {
        if (current()) self->SetState(self->snapshot_.state, self->snapshot_.error);
    };
    try {
        if (current() && !run->stopping && !run->audio_stopping) {
            failure = ScreenShareAudioError::Unavailable;
            if (!self->backend_.audio_available() || !self->backend_.publish_audio ||
                !self->backend_.unpublish_audio)
                throw std::runtime_error("share audio unavailable");
            run->audio_capture = self->backend_.audio_capture();
            if (!run->audio_capture) throw std::runtime_error("share audio unavailable");
            failure = ScreenShareAudioError::Capture;
            run->audio_source = std::make_shared<AudioSource>(48000, 2);
            run->audio_state = ScreenShareAudioState::Starting;
            run->audio_error = ScreenShareAudioError::None;
            {
                std::lock_guard lock(run->audio_delivery);
                run->audio_accepting = true;
            }
            notify();
            const std::weak_ptr<Run> weak = run;
            const bool started = run->audio_capture->Start({run->audio_options.output_id},
                [weak, audio_epoch](const AudioFrame& frame) {
                    if (auto run = weak.lock()) {
                        std::lock_guard lock(run->audio_delivery);
                        if (!run->audio_accepting || run->audio_epoch != audio_epoch) return;
                        run->audio_source->captureFrame(frame);
                        run->audio_first_frame = true;
                    }
                }, [weak, audio_epoch](SystemAudioCaptureStatus status) {
                    if (auto run = weak.lock()) {
                        std::lock_guard lock(run->audio_delivery);
                        if (!run->audio_accepting || run->audio_epoch != audio_epoch ||
                            (run->audio_status && run->audio_status->phase == SystemAudioCapturePhase::Failed)) return;
                        run->audio_status = status;
                        if (status.phase == SystemAudioCapturePhase::Failed)
                            run->audio_accepting = false;
                    }
                });
            if (!started) throw std::runtime_error("share audio startup rejected");
            const auto deadline = std::chrono::steady_clock::now() + self->backend_.first_frame_timeout;
            while (current() && !run->stopping && !run->audio_stopping) {
                std::optional<SystemAudioCaptureStatus> status;
                bool first = false;
                {
                    std::lock_guard lock(run->audio_delivery);
                    status = run->audio_status;
                    first = run->audio_first_frame;
                }
                if (status && status->phase == SystemAudioCapturePhase::Failed)
                    throw std::runtime_error("share audio capture failed");
                if (status && status->phase == SystemAudioCapturePhase::Ready && first) break;
                if (std::chrono::steady_clock::now() >= deadline)
                    throw std::runtime_error("share audio first frame unavailable");
                run->audio_timer.expires_after(20ms);
                std::error_code error;
                co_await run->audio_timer.async_wait(asio::redirect_error(asio::use_awaitable, error));
            }
            if (current() && !run->stopping && !run->audio_stopping) {
                failure = ScreenShareAudioError::Publish;
                AudioPublishPolicy policy;
                policy.max_bitrate_bps = 128000;
                policy.dtx = false;
                run->audio_track = LocalAudioTrack::createLocalAudioTrack(
                    "screen_audio_" + std::to_string(run->id) + "_" + std::to_string(audio_epoch),
                    run->audio_source, policy);
                run->audio_track->set_source(TrackSource::ScreenShareAudio);
                // Resolve this exact track on rollback even if publication
                // fails after installing a native sender or changes its SID.
                run->audio_publish_attempted = true;
                co_await self->backend_.publish_audio(run->audio_track);
                failure = ScreenShareAudioError::None;
                if (current() && !run->stopping && !run->audio_stopping) {
                    run->audio_state = ScreenShareAudioState::Active;
                    notify();
                }
            } else {
                failure = ScreenShareAudioError::None;
            }
            while (current() && !run->stopping && !run->audio_stopping) {
                {
                    std::lock_guard lock(run->audio_delivery);
                    if (run->audio_status && run->audio_status->phase == SystemAudioCapturePhase::Failed)
                        throw std::runtime_error("share audio capture ended");
                }
                run->audio_timer.expires_after(20ms);
                std::error_code error;
                co_await run->audio_timer.async_wait(asio::redirect_error(asio::use_awaitable, error));
            }
        }
    } catch (...) {
        if (failure == ScreenShareAudioError::None) failure = ScreenShareAudioError::Capture;
    }
    run->audio_stopping = true;
    self->StopAudioFrames(run);
    if (!current()) co_return;
    run->audio_state = ScreenShareAudioState::Stopping;
    run->audio_error = failure;
    notify();
    while (current() && run->audio_cleanup && !run->audio_cleanup->complete.load(std::memory_order_acquire)) {
        run->audio_timer.expires_after(20ms);
        std::error_code error;
        co_await run->audio_timer.async_wait(asio::redirect_error(asio::use_awaitable, error));
    }
    if (!current()) co_return;
    if (run->audio_publish_attempted) {
        while (current() && (!self->transport_ready_ || !self->backend_.connected())) {
            run->audio_timer.expires_after(50ms);
            std::error_code error;
            co_await run->audio_timer.async_wait(asio::redirect_error(asio::use_awaitable, error));
        }
        if (!current()) co_return;
        try {
            co_await self->backend_.unpublish_audio(run->audio_track);
            run->audio_publish_attempted = false;
        } catch (...) {
            if (!current()) co_return;
            run->audio_driving = false;
            run->audio_state = ScreenShareAudioState::StopFailed;
            run->audio_error = ScreenShareAudioError::Unpublish;
            notify();
            co_return;
        }
    }
    if (!current()) co_return;
    run->audio_track.reset();
    run->audio_source.reset();
    run->audio_driving = false;
    run->audio_error = failure;
    run->audio_state = failure == ScreenShareAudioError::None
        ? ScreenShareAudioState::Disabled : ScreenShareAudioState::Failed;
    notify();
    // A whole-share stop can have removed video while audio rollback failed.
    // Completing the audio-only retry must also finish that parent transaction
    // and release run_, so a new share does not require a second global Stop.
    if (run->stopping && !run->driving) {
        run->driving = true;
        asio::co_spawn(self->strand_, Drive(self, run, {}, VideoPublishOptions{}), asio::detached);
    }
}

asio::awaitable<void> ScreenShareSession::Drive(std::shared_ptr<ScreenShareSession> self,
                                               std::shared_ptr<Run> run, DesktopSource target,
                                               VideoPublishOptions options) {
    ScreenShareError failure = ScreenShareError::None;
    try {
        if (!run->stopping && !self->closed_) {
            run->capture = self->backend_.capture();
            if (!run->capture) throw std::runtime_error("screen capture unavailable");
            const std::weak_ptr<Run> weak = run;
            if (!run->capture->SetQuality(run->requested, run->revision, [weak](auto profile) {
                if (auto run = weak.lock()) {
                    std::lock_guard lock(run->delivery);
                    if (run->accepting) run->delivered = profile;
                }
            })) throw std::runtime_error("screen quality unsupported");
            run->capture->StartWithStatus(std::move(target), [weak](const VideoFrame& frame) {
                if (auto run = weak.lock()) {
                    std::lock_guard lock(run->delivery);
                    if (!run->accepting || run->capture_phase == DesktopCapturePhase::AwaitingPermission) return;
                    run->source->captureFrame(frame);
                    if (frame.type() == VideoBufferType::I420) {
                        const auto planes = frame.planeInfos();
                        if (planes.size() >= 3) {
                            run->preview->Submit("screen", run->id, render::OwnedI420Frame::CopyFromPlanes(
                                frame.width(), frame.height(),
                                reinterpret_cast<const uint8_t*>(planes[0].data_ptr), planes[0].stride,
                                reinterpret_cast<const uint8_t*>(planes[1].data_ptr), planes[1].stride,
                                reinterpret_cast<const uint8_t*>(planes[2].data_ptr), planes[2].stride));
                        }
                    }
                    run->first_frame.store(true, std::memory_order_release);
                }
            }, [weak] { if (auto run = weak.lock()) run->ended.store(true); },
            [weak](DesktopCapturePhase phase) {
                if (auto run = weak.lock()) {
                    std::lock_guard lock(run->delivery);
                    if (!run->accepting) return;
                    if (run->capture_phase == DesktopCapturePhase::Cancelled ||
                        run->capture_phase == DesktopCapturePhase::Denied ||
                        run->capture_phase == DesktopCapturePhase::Failed) return;
                    // Late or duplicate permission notifications must not
                    // downgrade Ready or extend the first-frame deadline.
                    if (run->ready_at && (phase == DesktopCapturePhase::AwaitingPermission ||
                                          phase == DesktopCapturePhase::Ready)) return;
                    run->capture_phase = phase;
                    if (phase == DesktopCapturePhase::Ready)
                        run->ready_at = std::chrono::steady_clock::now();
                    else if (phase != DesktopCapturePhase::AwaitingPermission) {
                        run->accepting = false;
                        run->ended.store(true);
                    }
                }
            });
            while (!self->closed_ && !run->stopping) {
                std::optional<DesktopCapturePhase> phase;
                std::optional<std::chrono::steady_clock::time_point> ready_at;
                bool first_ready = false;
                {
                    std::lock_guard lock(run->delivery);
                    phase = run->capture_phase;
                    ready_at = run->ready_at;
                    first_ready = ready_at && run->first_frame.load(std::memory_order_acquire)
                        && run->delivered.has_value();
                }
                if (phase == DesktopCapturePhase::Cancelled || phase == DesktopCapturePhase::Denied ||
                    phase == DesktopCapturePhase::Failed) {
                    failure = phase == DesktopCapturePhase::Cancelled ? ScreenShareError::None
                        : phase == DesktopCapturePhase::Denied ? ScreenShareError::PermissionDenied
                        : ScreenShareError::Capture;
                    run->stopping = true;
                    break;
                }
                if (run->ended.load())
                    throw std::runtime_error("screen capture first frame unavailable");
                const auto state = phase == DesktopCapturePhase::AwaitingPermission
                    ? ScreenShareState::AwaitingPermission : ScreenShareState::Starting;
                if (self->snapshot_.state != state) self->SetState(state);
                if (first_ready) break;
                if (ready_at && std::chrono::steady_clock::now() >=
                        *ready_at + self->backend_.first_frame_timeout)
                    throw std::runtime_error("screen capture first frame unavailable");
                run->timer.expires_after(20ms);
                std::error_code error;
                co_await run->timer.async_wait(asio::redirect_error(asio::use_awaitable, error));
            }
            if (!self->closed_ && !run->stopping) {
                if (run->ended.load()) throw std::runtime_error("screen capture ended before publish");
                failure = ScreenShareError::Publish;
                run->track = LocalVideoTrack::createLocalVideoTrack(
                    "screen_video_" + std::to_string(run->id), run->source,
                    TrackSource::ScreenShareVideo, options);
                {
                    std::lock_guard lock(run->delivery);
                    if (!run->delivered) throw std::runtime_error("screen profile unavailable");
                    run->applied = run->delivered;
                }
                run->track->SetScreenShareProfile(*run->applied);
                co_await self->backend_.publish(run->track);
                run->quality_status = ScreenShareQualityStatus::Applied;
                run->published = true;
                failure = ScreenShareError::None;
                if (!self->closed_ && !run->stopping && !run->ended.load())
                    self->SetState(ScreenShareState::Active);
                if (!self->closed_ && !run->stopping && !run->ended.load() && run->audio_options.enabled)
                    self->StartAudio(run->audio_options, run->id, run->audio_epoch);
            }
            auto nextGeometryCheck = std::chrono::steady_clock::now();
            while (!self->closed_ && !run->stopping && !run->ended.load()) {
                bool geometry_changed = false;
                {
                    std::lock_guard lock(run->delivery);
                    geometry_changed = run->delivered && run->applied && *run->delivered != *run->applied;
                }
                if ((run->quality_dirty || geometry_changed) && self->transport_ready_ && self->backend_.connected() &&
                    run->quality_status != ScreenShareQualityStatus::Degraded && run->quality_status != ScreenShareQualityStatus::Rejected)
                    co_await ApplyQuality(self, run);
                if (self->closed_ || run->stopping) break;
                if (run->quality_status == ScreenShareQualityStatus::MetadataPending && run->applied &&
                    self->transport_ready_ && self->backend_.connected() &&
                    std::chrono::steady_clock::now() >= run->next_metadata_retry) {
                    run->next_metadata_retry = std::chrono::steady_clock::now() + 1s;
                    bool synced = false;
                    try { co_await self->backend_.sync_quality(run->track, *run->applied); synced = true; } catch (...) {}
                    if (self->closed_ || run->stopping) break;
                    if (synced && !run->quality_dirty) {
                        run->quality_status = ScreenShareQualityStatus::Applied;
                        self->SetState(ScreenShareState::Active);
                    }
                }
                if (run->source_binding && self->backend_.validate_screen_binding &&
                    std::chrono::steady_clock::now() >= nextGeometryCheck) {
                    auto status = ScreenBindingStatus::Unavailable;
                    try { status = self->backend_.validate_screen_binding(*run->source_binding); }
                    catch (...) { status = ScreenBindingStatus::Unavailable; }
                    if (status != ScreenBindingStatus::Valid && status != ScreenBindingStatus::GeometryChanged) {
                        failure = ScreenShareError::Capture;
                        break;
                    }
                    if (status == ScreenBindingStatus::GeometryChanged && run->annotation_binding) {
                        // Stop drawing at obsolete coordinates immediately;
                        // the capture backend can recover without republishing.
                        run->annotation_binding.reset();
                        self->SetState(ScreenShareState::Active);
                    } else if (status == ScreenBindingStatus::Valid && !run->annotation_binding) {
                        // The original monitor and geometry are valid again.
                        // Retire the old annotation scope before offering a new
                        // one; do not restore old strokes or stale callbacks.
                        ++run->source_binding->source_epoch;
                        run->source_binding->share_session_id = NewShareSessionId();
                        run->annotation_binding = run->source_binding;
                        self->SetState(ScreenShareState::Active);
                    }
                    nextGeometryCheck = std::chrono::steady_clock::now() +
                        self->backend_.geometry_check_interval;
                }
                run->timer.expires_after(20ms);
                std::error_code error;
                co_await run->timer.async_wait(asio::redirect_error(asio::use_awaitable, error));
            }
            if (run->ended.load() && !run->stopping) {
                std::lock_guard lock(run->delivery);
                failure = run->capture_phase == DesktopCapturePhase::Cancelled ? ScreenShareError::None
                    : run->capture_phase == DesktopCapturePhase::Denied ? ScreenShareError::PermissionDenied
                    : ScreenShareError::Capture;
            }
        }
    } catch (...) {
        if (failure == ScreenShareError::None) failure = ScreenShareError::Capture;
    }

    self->StopFrames(run);
    if (self->closed_) co_return;
    while (!self->closed_ && run->audio_driving) {
        run->timer.expires_after(20ms);
        std::error_code error;
        co_await run->timer.async_wait(asio::redirect_error(asio::use_awaitable, error));
    }
    if (self->closed_) co_return;
    if (run->capture_cleanup && !run->capture_cleanup->complete.load(std::memory_order_acquire)) {
        self->SetState(ScreenShareState::Stopping);
        while (!self->closed_ && !run->capture_cleanup->complete.load(std::memory_order_acquire)) {
            run->timer.expires_after(20ms);
            std::error_code error;
            co_await run->timer.async_wait(asio::redirect_error(asio::use_awaitable, error));
        }
    }
    if (self->closed_) co_return;
    if (run->published) {
        self->SetState(ScreenShareState::Stopping);
        // The user can stop capture during reconnect. Preserve that intent and
        // remove the recovered publication once the transport is ready.
        while (!self->closed_ && (!self->transport_ready_ || !self->backend_.connected())) {
            run->timer.expires_after(50ms);
            std::error_code error;
            co_await run->timer.async_wait(asio::redirect_error(asio::use_awaitable, error));
        }
        if (self->closed_) co_return;
        try {
            co_await self->backend_.unpublish(run->track);
            run->published = false;
        } catch (...) {
            if (self->closed_) co_return;
            run->driving = false;
            self->SetState(ScreenShareState::StopFailed, ScreenShareError::Unpublish);
            co_return;
        }
    }
    if (self->closed_) co_return;
    if (run->audio_publish_attempted) {
        run->driving = false;
        self->SetState(ScreenShareState::StopFailed, ScreenShareError::Unpublish);
        co_return;
    }
    self->run_.reset();
    self->SetState(failure == ScreenShareError::None ? ScreenShareState::Idle : ScreenShareState::Failed, failure);
}
} // namespace livekit
