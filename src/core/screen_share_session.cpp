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

void ScreenShareSession::Start(DesktopSource source) {
    if (closed_ || run_ || !transport_ready_ || !backend_.connected()) return;
    auto run = std::make_shared<Run>(strand_, ++next_run_, executor_lifetime_);
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
    asio::co_spawn(strand_, Drive(shared_from_this(), run, std::move(source)), asio::detached);
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
        asio::co_spawn(strand_, Drive(shared_from_this(), run, {}), asio::detached);
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
                                               std::shared_ptr<Run> run, DesktopSource target) {
    ScreenShareError failure = ScreenShareError::None;
    try {
        if (!run->stopping && !self->closed_) {
            run->capture = self->backend_.capture();
            if (!run->capture) throw std::runtime_error("screen capture unavailable");
            const std::weak_ptr<Run> weak = run;
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
            while (!self->closed_ && !run->stopping && !run->first_frame.load(std::memory_order_acquire)) {
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
                    "screen_video_" + std::to_string(run->id), run->source, TrackSource::ScreenShareVideo);
                co_await self->backend_.publish(run->track);
                run->published = true;
                failure = ScreenShareError::None;
                if (!self->closed_ && !run->stopping && !run->ended.load())
                    self->SetState(ScreenShareState::Active);
            }
            auto nextGeometryCheck = std::chrono::steady_clock::now();
            while (!self->closed_ && !run->stopping && !run->ended.load()) {
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
