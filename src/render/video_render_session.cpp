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

VideoRenderSession::AttachResult VideoRenderSession::AttachRemoteTrack(
        const std::shared_ptr<Track>& track,
        const std::string& identity,
        const std::string& render_key) {
    RemoteTrackSelection selection;
    selection.track = track;
    selection.identity = identity;
    selection.render_key = render_key;
    return AttachSelection(selection, false);
}

VideoRenderSession::SelectionResult VideoRenderSession::ApplySelection(
        uint64_t coordinator_session,
        uint64_t policy_revision,
        const std::vector<RemoteTrackSelection>& selection) {
    SelectionResult result;
    result.coordinator_session = coordinator_session;
    result.policy_revision = policy_revision;
    result.requested = selection.size();

    const auto state = state_;
    if (!state || !state->active.load(std::memory_order_acquire)) {
        for (const auto& value : selection) {
            result.tracks.emplace_back(value.key.publication_sid, AttachResult::Inactive);
        }
        return result;
    }
    if (selection_coordinator_session_ != 0 &&
        (selection_coordinator_session_ != coordinator_session ||
         policy_revision < selection_policy_revision_)) {
        for (const auto& value : selection) {
            result.tracks.emplace_back(
                value.key.publication_sid, AttachResult::StaleSelection);
        }
        result.attached = tracks_.size();
        return result;
    }

    std::unordered_map<std::string, const RemoteTrackSelection*> requested;
    requested.reserve(selection.size());
    for (const auto& value : selection) {
        if (!value.key.publication_sid.empty()) {
            requested.emplace(value.key.publication_sid, &value);
        }
    }

    std::vector<std::string> removed;
    removed.reserve(tracks_.size());
    for (const auto& [track_id, binding] : tracks_) {
        const auto desired = requested.find(track_id);
        if (desired == requested.end() || !binding.validates_lease ||
            binding.key != desired->second->key ||
            binding.media_binding_key != desired->second->media_binding_key ||
            binding.track.lock() != desired->second->track) {
            removed.push_back(track_id);
        }
    }
    for (const auto& track_id : removed) RemoveTrack(track_id);

    selection_coordinator_session_ = coordinator_session;
    selection_policy_revision_ = policy_revision;
    result.tracks.reserve(selection.size());
    for (const auto& value : selection) {
        const auto attach = AttachSelection(value, true);
        result.tracks.emplace_back(value.key.publication_sid, attach);
    }
    result.attached = tracks_.size();
    return result;
}

VideoRenderSession::AttachResult VideoRenderSession::AttachSelection(
        const RemoteTrackSelection& selection,
        bool validates_lease) {
    const auto& track = selection.track;
    const auto& identity = selection.identity;
    const auto& render_key = selection.render_key;
    if (!track || track->kind() != TrackKind::Video || identity.empty()) {
        return AttachResult::InvalidTrack;
    }
    if (validates_lease &&
        (selection.key.publication_sid.empty() ||
         selection.key.publication_sid != track->sid() ||
         selection.media_binding_key.track != selection.key ||
         !IsTrackTicketActive(selection.ticket, selection.key) ||
         !IsMediaBindingTicketActive(
             selection.media_binding_ticket, selection.media_binding_key))) {
        return AttachResult::StaleBinding;
    }
    const auto state = state_;
    if (!state || !state->active.load(std::memory_order_acquire)) {
        return AttachResult::Inactive;
    }

    const std::string track_id = track->sid();
    if (track_id.empty()) {
        return AttachResult::InvalidTrack;
    }

    // A full Room reconnect can recreate the Track object while preserving its
    // SID.  Keeping the old RAII token in that case silently routes every new
    // frame to a dead source.  Replace only when the object actually changed;
    // repeated control-plane notifications for the same object stay idempotent.
    const auto existing = tracks_.find(track_id);
    if (existing != tracks_.end()) {
        if (existing->second.track.lock() == track &&
            existing->second.render_key == (render_key.empty() ? identity : render_key) &&
            existing->second.validates_lease == validates_lease &&
            (!validates_lease ||
             (existing->second.key == selection.key &&
              existing->second.media_binding_key == selection.media_binding_key &&
              BindingLeaseActive(existing->second)))) {
            return AttachResult::AlreadyBound;
        }
        state->router->RemoveTrack(track_id, state->generation);
        tracks_.erase(existing);
    } else if (tracks_.size() >= max_active_tracks_) {
        state->rejected_track_attachments.fetch_add(1, std::memory_order_relaxed);
        return AttachResult::CapacityExceeded;
    }

    const uint64_t generation = state->generation;
    const uint64_t binding_generation = state->next_track_binding_generation.fetch_add(
        1, std::memory_order_relaxed);
    if (!state->router->RegisterTrackBinding(track_id, generation, binding_generation)) {
        state->rejected_track_attachments.fetch_add(1, std::memory_order_relaxed);
        return AttachResult::CapacityExceeded;
    }
    std::weak_ptr<State> weak_state = state;
    const auto track_key = selection.key;
    const auto track_ticket = selection.ticket;
    const auto media_binding_key = selection.media_binding_key;
    const auto media_binding_ticket = selection.media_binding_ticket;
    auto subscription = track->subscribeI420VideoFrames(
        [weak_state, track_id, generation, binding_generation, validates_lease,
         track_key, track_ticket, media_binding_key, media_binding_ticket](OwnedI420Frame::Ptr frame) {
            const auto state = weak_state.lock();
            if (!state || !state->active.load(std::memory_order_acquire)) {
                return;
            }
            if (validates_lease &&
                (!IsTrackTicketActive(track_ticket, track_key) ||
                 !IsMediaBindingTicketActive(
                     media_binding_ticket, media_binding_key))) {
                return;
            }
            state->router->SubmitBound(track_id, generation, binding_generation, std::move(frame));
        });
    if (!subscription.active()) {
        state->router->RemoveTrack(track_id, generation);
        return AttachResult::InvalidTrack;
    }

    tracks_.emplace(track_id, TrackBinding{identity, render_key.empty() ? identity : render_key,
        track, selection.key, selection.ticket, selection.media_binding_key,
        selection.media_binding_ticket, validates_lease, binding_generation,
        std::move(subscription)});
    return AttachResult::Attached;
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
        if (!BindingLeaseActive(binding)) {
            continue;
        }
        auto frame = state->router->TakeLatest(track_id, state->generation);
        const auto track = binding.track.lock();
        if (!frame || !track || track->muted()) {
            continue;
        }
        RenderFrame(binding.render_key, VideoRenderFrame::FromI420(std::move(frame)));
    }
    if (auto local = local_input_.TakeLatest()) RenderFrame(local_render_key_, std::move(local));
}

bool VideoRenderSession::BindingLeaseActive(const TrackBinding& binding) const {
    return !binding.validates_lease ||
        (IsTrackTicketActive(binding.ticket, binding.key) &&
         IsMediaBindingTicketActive(
             binding.media_binding_ticket, binding.media_binding_key));
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
        const auto convert_started_at = std::chrono::steady_clock::now();
        const auto image = cpu_renderer_.Convert(*frame);
        frame->NotifyRenderStage(
            "qt_cpu_convert",
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - convert_started_at));
        if (!image.isNull()) {
            frame_ready_callback_(key, image, std::move(frame));
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
    selection_coordinator_session_ = 0;
    selection_policy_revision_ = 0;
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
