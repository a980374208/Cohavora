#include "video_render_session.h"

#include <utility>
#include <vector>

namespace livekit::render {

VideoRenderSession::VideoRenderSession(FrameReadyCallback frame_ready_callback,
                                       size_t max_active_tracks)
    : state_(std::make_shared<State>(NextGeneration(), max_active_tracks)),
      max_active_tracks_(max_active_tracks),
      frame_ready_callback_(std::move(frame_ready_callback)) {}

VideoRenderSession::~VideoRenderSession() {
    Deactivate();
}

void VideoRenderSession::AttachRemoteTrack(const std::shared_ptr<Track>& track,
                                           const std::string& identity) {
    if (!track || track->kind() != TrackKind::Video || identity.empty()) {
        return;
    }
    const auto state = state_;
    if (!state || !state->active.load(std::memory_order_acquire)) {
        return;
    }

    const std::string track_id = track->sid();
    if (track_id.empty()) {
        return;
    }

    // A full Room reconnect can recreate the Track object while preserving its
    // SID.  Keeping the old RAII token in that case silently routes every new
    // frame to a dead source.  Replace only when the object actually changed;
    // repeated control-plane notifications for the same object stay idempotent.
    const auto existing = tracks_.find(track_id);
    if (existing != tracks_.end()) {
        if (existing->second.track.lock() == track) {
            return;
        }
        state->router->RemoveTrack(track_id, state->generation);
        tracks_.erase(existing);
    } else if (tracks_.size() >= max_active_tracks_) {
        state->rejected_track_attachments.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const uint64_t generation = state->generation;
    const uint64_t binding_generation = state->next_track_binding_generation.fetch_add(
        1, std::memory_order_relaxed);
    if (!state->router->RegisterTrackBinding(track_id, generation, binding_generation)) {
        state->rejected_track_attachments.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::weak_ptr<State> weak_state = state;
    auto subscription = track->subscribeI420VideoFrames(
        [weak_state, track_id, generation, binding_generation](OwnedI420Frame::Ptr frame) {
            const auto state = weak_state.lock();
            if (!state || !state->active.load(std::memory_order_acquire)) {
                return;
            }
            state->router->SubmitBound(track_id, generation, binding_generation, std::move(frame));
        });
    if (!subscription.active()) {
        state->router->RemoveTrack(track_id, generation);
        return;
    }

    tracks_.emplace(track_id, TrackBinding{identity, track, binding_generation, std::move(subscription)});
}

void VideoRenderSession::RemoveTrack(const std::string& track_id) {
    const auto state = state_;
    if (!state) {
        return;
    }
    state->router->RemoveTrack(track_id, state->generation);
    tracks_.erase(track_id);
}

void VideoRenderSession::RemoveTracksForIdentity(const std::string& identity) {
    std::vector<std::string> track_ids;
    track_ids.reserve(tracks_.size());
    for (const auto& [track_id, binding] : tracks_) {
        if (binding.identity == identity) {
            track_ids.push_back(track_id);
        }
    }
    for (const auto& track_id : track_ids) {
        RemoveTrack(track_id);
    }
}

void VideoRenderSession::RenderLatestFrames() {
    const auto state = state_;
    if (!state || !state->active.load(std::memory_order_acquire)) {
        return;
    }

    for (const auto& [track_id, binding] : tracks_) {
        auto frame = state->router->TakeLatest(track_id, state->generation);
        if (!frame) {
            continue;
        }
        if (backend_ == Backend::Dx11) {
            if (i420_frame_ready_callback_) {
                i420_frame_ready_callback_(binding.identity, std::move(frame));
                state->delivered_to_dx11.fetch_add(1, std::memory_order_relaxed);
            }
            continue;
        }

        if (frame_ready_callback_) {
            QImage image = cpu_renderer_.Convert(*frame);
            if (!image.isNull()) {
                frame_ready_callback_(binding.identity, image);
                state->delivered_to_qt_cpu.fetch_add(1, std::memory_order_relaxed);
            } else {
                state->qt_cpu_conversion_failures.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

void VideoRenderSession::UseDx11Backend(I420FrameReadyCallback frame_ready_callback) {
    if (!state_ || !state_->active.load(std::memory_order_acquire) || !frame_ready_callback) {
        return;
    }
    i420_frame_ready_callback_ = std::move(frame_ready_callback);
    backend_ = Backend::Dx11;
}

void VideoRenderSession::UseQtCpuBackend() {
    if (!state_ || !state_->active.load(std::memory_order_acquire)) {
        return;
    }
    backend_ = Backend::QtCpu;
    i420_frame_ready_callback_ = {};
}

void VideoRenderSession::Deactivate() {
    auto state = std::move(state_);
    if (!state) {
        return;
    }

    state->active.store(false, std::memory_order_release);
    state->router->Deactivate(state->generation);
    tracks_.clear();
    frame_ready_callback_ = {};
    i420_frame_ready_callback_ = {};
}

bool VideoRenderSession::active() const noexcept {
    return state_ && state_->active.load(std::memory_order_acquire);
}

uint64_t VideoRenderSession::generation() const noexcept {
    return state_ ? state_->generation : 0;
}

VideoRenderSession::Statistics VideoRenderSession::statistics() const noexcept {
    const auto state = state_;
    if (!state) {
        return {};
    }
    return {
        state->router->statistics(),
        state->delivered_to_dx11.load(std::memory_order_relaxed),
        state->delivered_to_qt_cpu.load(std::memory_order_relaxed),
        state->qt_cpu_conversion_failures.load(std::memory_order_relaxed),
        state->rejected_track_attachments.load(std::memory_order_relaxed),
        tracks_.size(),
        backend_,
    };
}

uint64_t VideoRenderSession::NextGeneration() {
    static std::atomic<uint64_t> next_generation{1};
    return next_generation.fetch_add(1, std::memory_order_relaxed);
}

} // namespace livekit::render
