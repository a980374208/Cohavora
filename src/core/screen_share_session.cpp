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
    explicit Run(asio::any_io_executor executor, uint64_t id,
                 std::shared_ptr<void> executor_lifetime)
        : executor_lifetime(std::move(executor_lifetime)), timer(executor), id(id),
          source(std::make_shared<VideoSource>(0, 0)),
          preview(std::make_shared<render::VideoRenderRouter>(id, 1)) {}
    // Capture callbacks can retain Run independently of ScreenShareSession.
    // In particular the timer must be destroyed before its context lease.
    const std::shared_ptr<void> executor_lifetime;
    asio::steady_timer timer;
    const uint64_t id;
    std::unique_ptr<IDesktopCapture> capture;
    std::shared_ptr<VideoSource> source;
    std::shared_ptr<LocalVideoTrack> track;
    std::shared_ptr<render::VideoRenderRouter> preview;
    std::string source_title;
    DesktopSourceKind source_kind = DesktopSourceKind::Window;
    std::optional<ScreenBinding> annotation_binding;
    std::mutex delivery;
    bool accepting = true; // delivery mutex; Stop is a frame barrier
    std::atomic<bool> first_frame{false};
    std::atomic<bool> ended{false};
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
    snapshot_ = {state, error};
    if (run_) {
        snapshot_.requested_quality = run_->requested;
        snapshot_.applied_quality = run_->applied;
        snapshot_.quality_status = run_->quality_status;
        snapshot_.source_title = run_->source_title;
        snapshot_.source_kind = run_->source_kind;
        snapshot_.annotation_binding = run_->annotation_binding;
        if (state == ScreenShareState::Active) snapshot_.preview = run_->preview;
    }
    if (!closed_ && observer_) observer_(snapshot_);
}

void ScreenShareSession::StopFrames(const std::shared_ptr<Run>& run) {
    {
        std::lock_guard lock(run->delivery);
        run->accepting = false;
        run->preview->Deactivate(run->id);
    }
    if (run->capture) {
        run->capture->Stop();
        run->capture.reset();
    }
}

void ScreenShareSession::Start(DesktopSource source, VideoPublishOptions options, ScreenShareQuality quality) {
    if (!quality.valid()) return;
    if (closed_ || run_ || !transport_ready_ || !backend_.connected()) return;
    auto run = std::make_shared<Run>(strand_, ++next_run_, executor_lifetime_);
    run->requested = quality;
    run->source_title = source.title;
    run->source_kind = source.kind;
    if (source.kind == DesktopSourceKind::Screen && backend_.resolve_screen_binding) {
        try {
            run->annotation_binding = backend_.resolve_screen_binding(
                source, run->id, NewShareSessionId());
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
    SetState(ScreenShareState::Stopping);
    if (!run->driving) {
        run->driving = true;
        asio::co_spawn(strand_, Drive(
            shared_from_this(), run, {}, VideoPublishOptions{}), asio::detached);
    }
}

void ScreenShareSession::Shutdown() {
    auto capture = TakeCaptureForShutdown();
    if (capture) capture->Stop();
}

std::unique_ptr<IDesktopCapture> ScreenShareSession::TakeCaptureForShutdown() {
    closed_ = true;
    observer_ = {};
    std::unique_ptr<IDesktopCapture> capture;
    if (run_) {
        run_->stopping = true;
        {
            std::lock_guard lock(run_->delivery);
            run_->accepting = false;
            run_->preview->Deactivate(run_->id);
        }
        capture = std::move(run_->capture);
        run_->timer.cancel();
        run_.reset();
    }
    snapshot_ = {};
    return capture;
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
            run->capture->Start(std::move(target), [weak](const VideoFrame& frame) {
                if (auto run = weak.lock()) {
                    std::lock_guard lock(run->delivery);
                    if (!run->accepting) return;
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
            }, [weak] { if (auto run = weak.lock()) run->ended.store(true); });
            const auto deadline = std::chrono::steady_clock::now() + self->backend_.first_frame_timeout;
            auto firstReady = [&] {
                std::lock_guard lock(run->delivery);
                return run->first_frame.load(std::memory_order_acquire) && run->delivered.has_value();
            };
            while (!self->closed_ && !run->stopping && !firstReady()) {
                if (run->ended.load() || std::chrono::steady_clock::now() >= deadline)
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
                if (run->annotation_binding && self->backend_.validate_screen_binding &&
                    std::chrono::steady_clock::now() >= nextGeometryCheck) {
                    bool valid = false;
                    try { valid = self->backend_.validate_screen_binding(*run->annotation_binding); }
                    catch (...) { valid = false; }
                    if (!valid) {
                        failure = ScreenShareError::Capture;
                        break;
                    }
                    nextGeometryCheck = std::chrono::steady_clock::now() +
                        self->backend_.geometry_check_interval;
                }
                run->timer.expires_after(20ms);
                std::error_code error;
                co_await run->timer.async_wait(asio::redirect_error(asio::use_awaitable, error));
            }
            if (run->ended.load() && !run->stopping) failure = ScreenShareError::Capture;
        }
    } catch (...) {
        if (failure == ScreenShareError::None) failure = ScreenShareError::Capture;
    }

    self->StopFrames(run);
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
    self->run_.reset();
    self->SetState(failure == ScreenShareError::None ? ScreenShareState::Idle : ScreenShareState::Failed, failure);
}
} // namespace livekit
