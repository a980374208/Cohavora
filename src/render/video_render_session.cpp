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
                                           const std::string& identity,
                                           const std::string& render_key) {
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

    tracks_.emplace(track_id, TrackBinding{identity, render_key.empty() ? identity : render_key,
        track, binding_generation, std::move(subscription)});
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
        const auto track = binding.track.lock();
        if (!frame || !track || track->muted()) {
            continue;
        }
        RenderFrame(binding.render_key, VideoRenderFrame::FromI420(std::move(frame)));
    }
    if (auto local = local_input_.TakeLatest()) RenderFrame(local_render_key_, std::move(local));
}

void VideoRenderSession::AttachLocalSource(const std::shared_ptr<VideoSource>& source, const std::string& key) {
    if (!active() || key.empty()) return;
    if (local_render_key_ != key) local_input_.Detach();
    local_render_key_ = key;
    local_input_.Attach(source);
}

void VideoRenderSession::DetachLocalSource() {
    local_input_.Detach();
    local_render_key_.clear();
}

void VideoRenderSession::RenderFrame(const std::string& key, VideoRenderFrame::Ptr frame) {
    const auto state = state_;
    if (!state || !state->active.load(std::memory_order_acquire) || !frame || key.empty()) return;
    if (backend_ == Backend::Gpu) {
        if (gpu_frame_ready_callback_) {
            gpu_frame_ready_callback_(key, std::move(frame));
            state->delivered_to_gpu.fetch_add(1, std::memory_order_relaxed);
        }
    } else if (frame_ready_callback_) {
        const auto image = cpu_renderer_.Convert(*frame);
        if (!image.isNull()) {
            frame_ready_callback_(key, image);
            state->delivered_to_qt_cpu.fetch_add(1, std::memory_order_relaxed);
        } else {
            state->qt_cpu_conversion_failures.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void VideoRenderSession::UseGpuBackend(GpuFrameReadyCallback frame_ready_callback) {
    if (!state_ || !state_->active.load(std::memory_order_acquire) || !frame_ready_callback) {
        return;
    }
    gpu_frame_ready_callback_ = std::move(frame_ready_callback);
    backend_ = Backend::Gpu;
}

void VideoRenderSession::UseQtCpuBackend() {
    if (!state_ || !state_->active.load(std::memory_order_acquire)) {
        return;
    }
    backend_ = Backend::QtCpu;
    gpu_frame_ready_callback_ = {};
}

void VideoRenderSession::Deactivate() {
    DetachLocalSource();
    auto state = std::move(state_);
    if (!state) {
        return;
    }

    state->active.store(false, std::memory_order_release);
    state->router->Deactivate(state->generation);
    tracks_.clear();
    frame_ready_callback_ = {};
    gpu_frame_ready_callback_ = {};
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
        state->delivered_to_gpu.load(std::memory_order_relaxed),
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
