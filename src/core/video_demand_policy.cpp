#include "video_demand_policy.h"
#include "video_layout_geometry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace livekit {
namespace {

bool SameOptionalKey(const std::optional<TrackKey>& left,
                     const std::optional<TrackKey>& right) {
    if (left.has_value() != right.has_value()) return false;
    return !left || *left == *right;
}

bool SameViewport(const ViewportIntent& left, const ViewportIntent& right) {
    return left.coordinator_session == right.coordinator_session &&
        left.catalog_revision == right.catalog_revision &&
        left.mode == right.mode && left.page == right.page &&
        left.resolution_match == right.resolution_match &&
        left.speaker_layout == right.speaker_layout &&
        left.speaking_only == right.speaking_only &&
        left.page_size == right.page_size &&
        SameOptionalKey(left.page_anchor, right.page_anchor) &&
        SameOptionalKey(left.pinned, right.pinned) &&
        left.pinned_local == right.pinned_local &&
        SameOptionalKey(left.selected_share, right.selected_share) &&
        left.stage_content == right.stage_content &&
        left.stage_rect.x == right.stage_rect.x &&
        left.stage_rect.y == right.stage_rect.y &&
        left.stage_rect.width == right.stage_rect.width &&
        left.stage_rect.height == right.stage_rect.height &&
        left.paging_bar_height == right.paging_bar_height &&
        left.device_pixel_ratio == right.device_pixel_ratio &&
        left.remote_video_enabled == right.remote_video_enabled &&
        left.window_visible == right.window_visible &&
        left.minimized == right.minimized &&
        left.local_participant_present == right.local_participant_present &&
        left.local_screen_share_present == right.local_screen_share_present &&
        left.detached_share == right.detached_share;
}

bool SameSpeakerInput(const ActiveSpeakerInfo& left,
                      const ActiveSpeakerInfo& right) {
    return left.key == right.key && left.is_local == right.is_local &&
        left.speaking == right.speaking;
}

LogicalViewport StageWithPaging(const ViewportIntent& viewport, bool paging) {
    auto stage = viewport.stage_rect;
    if (paging) {
        stage.height -= std::clamp(viewport.paging_bar_height, 0,
                                   std::max(0, stage.height));
    }
    return stage;
}

int SourceOrder(TrackSource source) {
    switch (source) {
    case TrackSource::Camera: return 0;
    case TrackSource::ScreenShareVideo: return 1;
    default: return 2;
    }
}

uint32_t CeilDiv(std::size_t count, uint32_t page_size) {
    if (page_size == 0 || count == 0) return 1;
    return static_cast<uint32_t>((count + page_size - 1) / page_size);
}

bool Contains(const std::vector<TrackKey>& keys, const TrackKey& key) {
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

uint64_t LayerPixels(const PublishedVideoLayer& layer) {
    return uint64_t(layer.width) * layer.height;
}

bool SmallerLayer(const PublishedVideoLayer& left,
                  const PublishedVideoLayer& right) {
    return std::tuple{LayerPixels(left), left.width, left.height, left.quality, left.rid} <
        std::tuple{LayerPixels(right), right.width, right.height, right.quality, right.rid};
}

std::optional<PublishedVideoLayer> SelectPublishedLayer(
    const std::vector<PublishedVideoLayer>& layers,
    VideoSeatRole role, uint32_t width, uint32_t height,
    VideoResolutionMatch mode) {
    if (layers.size() == 1) return layers.front();
    const uint32_t long_limit = role == VideoSeatRole::Main ? 3840 : 2560;
    const uint32_t short_limit = role == VideoSeatRole::Main ? 2160 : 1440;
    const PublishedVideoLayer* nearest = nullptr;
    const PublishedVideoLayer* boundary = nullptr;
    const bool upward = mode == VideoResolutionMatch::Up ||
        (mode == VideoResolutionMatch::Auto && role == VideoSeatRole::Main);
    // Compare relative width/height error with a common denominator W*H.
    // Integer scores make exact ties deterministic, including portrait video.
    const auto distance = [width, height](const PublishedVideoLayer& layer) {
        const auto dx = layer.width > width ? layer.width - width : width - layer.width;
        const auto dy = layer.height > height ? layer.height - height : height - layer.height;
        return uint64_t(dx) * height + uint64_t(dy) * width;
    };
    for (const auto& layer : layers) {
        if (std::max(layer.width, layer.height) > long_limit ||
            std::min(layer.width, layer.height) > short_limit) continue;
        if (mode == VideoResolutionMatch::Nearest) {
            if (!nearest || distance(layer) < distance(*nearest) ||
                (distance(layer) == distance(*nearest) && SmallerLayer(layer, *nearest))) {
                nearest = &layer;
            }
            continue;
        }
        if (!boundary || (upward ? SmallerLayer(*boundary, layer)
                               : SmallerLayer(layer, *boundary))) {
            boundary = &layer;
        }
        const bool fits = upward
            ? layer.width >= width && layer.height >= height
            : layer.width <= width && layer.height <= height;
        if (fits && (!nearest || (upward ? SmallerLayer(layer, *nearest)
                                       : SmallerLayer(*nearest, layer)))) {
            nearest = &layer;
        }
    }
    if (nearest) return *nearest;
    if (boundary) return *boundary;
    return std::nullopt;
}

} // namespace

VideoDemandPolicy::VideoDemandPolicy(uint64_t coordinator_session,
                                     VideoDemandPolicyConfig config)
    : coordinator_session_(coordinator_session), config_(std::move(config)) {
    config_.video_budget = std::min(config_.video_budget, kMaximumRemoteVideoTracks);
    config_.default_grid_page_size =
        NormalizePageSize(config_.default_grid_page_size, 9);
    viewport_.coordinator_session = coordinator_session_;
    viewport_.page_size = config_.default_grid_page_size;
    plan_.coordinator_session = coordinator_session_;
}

bool VideoDemandPolicy::UpdateCatalog(
    const PublicationCatalogSnapshot& snapshot) {
    if (!accepting_ || snapshot.coordinator_session != coordinator_session_ ||
        snapshot.native_room_generation == 0) {
        return false;
    }
    if (catalog_) {
        if (snapshot.native_room_generation < catalog_->native_room_generation) {
            return false;
        }
        if (snapshot.native_room_generation == catalog_->native_room_generation &&
            snapshot.catalog_revision < catalog_->catalog_revision) {
            return false;
        }
        if (snapshot.native_room_generation == catalog_->native_room_generation &&
            snapshot.catalog_revision == catalog_->catalog_revision) {
            return false;
        }
        if (snapshot.native_room_generation != catalog_->native_room_generation) {
            speaker_candidate_.reset();
            speaker_candidate_since_.reset();
            stable_speaker_.reset();
            current_auto_share_.reset();
            current_focus_.reset();
            current_focus_origin_ = FocusOrigin::None;
            focus_since_.reset();
            thumbnail_speakers_.clear();
            if (grid_page_.anchor) {
                const auto& anchor = *grid_page_.anchor;
                const auto* publication = Find(anchor);
                const auto source = publication ? publication->source : TrackSource::Camera;
                const auto seats = OrderedSeats();
                const bool unique_source = std::count_if(seats.begin(), seats.end(),
                    [&](const auto& candidate) {
                        return candidate.publication && candidate.source == source &&
                            candidate.key.participant.identity == anchor.participant.identity;
                    }) == 1;
                grid_page_.replay_anchor = PageAnchorIdentity{
                    anchor.participant.identity, anchor.publication_sid, source, unique_source};
            }
            // Each member is replayed separately. Retain the business anchor
            // until its new lifetime arrives instead of anchoring a partial page.
            grid_page_.anchor.reset();
            sidebar_page_.anchor.reset();
        }
    }
    catalog_ = snapshot;
    return true;
}

bool VideoDemandPolicy::UpdateViewport(const ViewportIntent& intent,
                                       TimePoint now) {
    (void)now;
    if (!accepting_ || intent.coordinator_session != coordinator_session_) {
        return false;
    }
    if (has_viewport_ && intent.view_revision < viewport_.view_revision) {
        return false;
    }
    const bool same = has_viewport_ && SameViewport(intent, viewport_);
    if (has_viewport_ && intent.view_revision == viewport_.view_revision && !same) {
        return false;
    }

    const uint32_t normalized_size = NormalizePageSize(
        intent.page_size, config_.default_grid_page_size);
    const bool layout_changed = has_viewport_ &&
        (intent.mode != viewport_.mode ||
         intent.speaker_layout != viewport_.speaker_layout ||
         intent.speaking_only != viewport_.speaking_only ||
         normalized_size != NormalizePageSize(viewport_.page_size,
             config_.default_grid_page_size));
    const bool local_seats_changed = has_viewport_ &&
        (intent.local_participant_present != viewport_.local_participant_present ||
         intent.local_screen_share_present != viewport_.local_screen_share_present);
    if (local_seats_changed) grid_page_.ResetAnchor();
    if (has_viewport_ &&
        (!SameOptionalKey(intent.pinned, viewport_.pinned) ||
         intent.pinned_local != viewport_.pinned_local)) {
        // A different main target changes the sidebar membership. Its old
        // anchor must not skip a remote seat returned from the main stage.
        sidebar_page_.anchor.reset();
    }
    if (!has_viewport_ || intent.mode != viewport_.mode ||
        intent.page != viewport_.page ||
        normalized_size != NormalizePageSize(
            viewport_.page_size, config_.default_grid_page_size)) {
        if (intent.mode == VideoLayoutMode::Speaker) {
            sidebar_page_.anchor.reset();
        } else {
            grid_page_.ResetAnchor();
        }
    }
    viewport_ = intent;
    if (layout_changed) {
        viewport_.page = 0;
        viewport_.page_anchor.reset();
        grid_page_.ResetAnchor();
        sidebar_page_.ResetAnchor();
    }
    has_viewport_ = true;
    if (intent.page_anchor && !layout_changed) {
        if (intent.mode == VideoLayoutMode::Speaker) {
            sidebar_page_.anchor = intent.page_anchor;
            sidebar_page_.requested_page = intent.page;
            // A caller-supplied anchor is valid for this request's effective
            // layout capacity, which follows the strip dimensions on resize.
            // BuildFocused records that capacity after resolving local previews.
            sidebar_page_.page_size = 0;
        } else if (!local_seats_changed) {
            grid_page_.replay_anchor.reset();
            grid_page_.anchor = intent.page_anchor;
            grid_page_.requested_page = intent.page;
            grid_page_.page_size = normalized_size;
        }
    }
    return !same;
}

bool VideoDemandPolicy::UpdateSpeakers(
    const std::vector<ActiveSpeakerInfo>& speakers,
    TimePoint now) {
    if (!accepting_) return false;
    bool same = speakers.size() == speakers_.size();
    if (same) {
        for (std::size_t index = 0; index != speakers.size(); ++index) {
            if (!SameSpeakerInput(speakers[index], speakers_[index])) {
                same = false;
                break;
            }
        }
    }
    // Keep every speaker independently of the single stable main-stage focus.
    // Missing entries mean stopped, rather than immediate removal from the list.
    for (auto& entry : thumbnail_speakers_) {
        const bool active = std::any_of(speakers.begin(), speakers.end(),
            [&](const auto& speaker) {
                return speaker.is_local == entry.is_local &&
                    (entry.is_local || speaker.key == entry.key) && speaker.speaking;
            });
        if (!active && !entry.stopped) entry.stopped = now;
    }
    for (const auto& speaker : speakers) {
        if (!speaker.speaking) continue;
        auto found = std::find_if(thumbnail_speakers_.begin(), thumbnail_speakers_.end(),
            [&](const auto& entry) {
                return speaker.is_local == entry.is_local &&
                    (entry.is_local || speaker.key == entry.key);
            });
        if (found == thumbnail_speakers_.end()) {
            thumbnail_speakers_.push_back({speaker.key, speaker.is_local, now, std::nullopt});
        } else if (found->stopped) {
            const bool was_confirmed = *found->stopped - found->started >=
                config_.thumbnail_speaker_delay;
            if (!was_confirmed || now - *found->stopped >= config_.thumbnail_speaker_retention) {
                found->started = now;
            }
            found->stopped.reset();
        }
    }
    if (!same) speakers_ = speakers;
    AdvanceSpeaker(now);
    return !same;
}

const VideoDemandPlan& VideoDemandPolicy::Reconcile(TimePoint now) {
    if (!accepting_) return plan_;
    auto next = BuildPlan(now);
    next.policy_revision = plan_.policy_revision;
    if (!SamePlan(next, plan_)) {
        next.policy_revision = plan_.policy_revision + 1;
        if (next.policy_revision == 0) ++next.policy_revision;
        plan_ = std::move(next);
    }
    // Echo the consumed request without creating a new policy for a semantic
    // duplicate. UI can distinguish an old acceptance from a pending pin.
    plan_.view_revision = viewport_.view_revision;
    return plan_;
}

std::optional<VideoDemandPolicy::TimePoint>
VideoDemandPolicy::NextReconcileAt(TimePoint now) const {
    if (!accepting_) return std::nullopt;

    std::optional<TimePoint> next;
    const auto consider = [&](TimePoint value) {
        if (value <= now) value = now;
        if (!next || value < *next) next = value;
    };

    if (viewport_.speaking_only &&
        (viewport_.mode == VideoLayoutMode::Speaker || plan_.mode == VideoLayoutMode::Speaker)) {
        for (const auto& speaker : thumbnail_speakers_) {
            if (!speaker.stopped) {
                const auto confirmed_at = speaker.started + config_.thumbnail_speaker_delay;
                if (confirmed_at > now) consider(confirmed_at);
            } else if (*speaker.stopped - speaker.started >= config_.thumbnail_speaker_delay) {
                const auto expires_at = *speaker.stopped + config_.thumbnail_speaker_retention;
                if (expires_at > now) consider(expires_at);
            }
        }
    }

    if (speaker_candidate_ && speaker_candidate_since_ &&
        !SameOptionalKey(speaker_candidate_, stable_speaker_) &&
        ResolveIntentTrack(speaker_candidate_, TrackSource::Camera)) {
        consider(*speaker_candidate_since_ + config_.speaker_candidate_delay);
    }

    const auto stable = ResolveIntentTrack(
        stable_speaker_, TrackSource::Camera);
    const bool override_focus =
        current_focus_origin_ == FocusOrigin::Pinned ||
        current_focus_origin_ == FocusOrigin::SelectedShare ||
        current_focus_origin_ == FocusOrigin::AutoShare;
    if (stable && current_focus_ && *stable != *current_focus_ &&
        ResolveIntentSeat(current_focus_) && !override_focus && focus_since_) {
        consider(*focus_since_ + config_.focus_minimum_residence);
    }
    return next;
}

void VideoDemandPolicy::Retire() {
    if (!accepting_) return;
    accepting_ = false;
    catalog_.reset();
    speakers_.clear();
    thumbnail_speakers_.clear();
    speaker_candidate_.reset();
    speaker_candidate_since_.reset();
    stable_speaker_.reset();
    current_auto_share_.reset();
    current_focus_.reset();
    focus_since_.reset();
    grid_page_.ResetAnchor();
    sidebar_page_.anchor.reset();

    VideoDemandPlan stopped;
    stopped.coordinator_session = coordinator_session_;
    stopped.policy_revision = plan_.policy_revision;
    stopped.requested_mode = viewport_.mode;
    stopped.mode = viewport_.mode;
    stopped.speaker_layout = viewport_.speaker_layout;
    stopped.speaking_only = viewport_.speaking_only;
    stopped.stage_content = viewport_.stage_content;
    stopped.reason = VideoDemandReason::Hidden;
    if (!SamePlan(stopped, plan_)) {
        stopped.policy_revision = plan_.policy_revision + 1;
        if (stopped.policy_revision == 0) ++stopped.policy_revision;
    }
    plan_ = std::move(stopped);
}

const RemotePublicationInfo* VideoDemandPolicy::Find(const TrackKey& key) const {
    if (!catalog_ ||
        key.participant.native_room_generation != catalog_->native_room_generation) {
        return nullptr;
    }
    for (const auto& participant : catalog_->participants) {
        if (participant.key != key.participant) continue;
        const auto found = std::find_if(participant.publications.begin(),
            participant.publications.end(),
            [&](const auto& publication) { return publication.key == key; });
        return found == participant.publications.end() ? nullptr : &*found;
    }
    return nullptr;
}

std::vector<const RemotePublicationInfo*> VideoDemandPolicy::OrderedVideo() const {
    std::vector<const RemotePublicationInfo*> result;
    for (const auto& candidate : OrderedSeats()) {
        if (candidate.publication) result.push_back(candidate.publication);
    }
    return result;
}

std::vector<VideoDemandPolicy::SeatCandidate> VideoDemandPolicy::OrderedSeats() const {
    std::vector<SeatCandidate> result;
    if (!catalog_) return result;
    std::vector<const PublicationCatalogParticipant*> participants;
    participants.reserve(catalog_->participants.size());
    for (const auto& participant : catalog_->participants) {
        if (participant.key.native_room_generation == catalog_->native_room_generation) {
            participants.push_back(&participant);
        }
    }
    // Roster replay order and native SIDs can change on reconnect. Business
    // identity fixes page membership; full TrackKeys still govern lifetimes.
    std::sort(participants.begin(), participants.end(), [](const auto* left, const auto* right) {
        return std::tie(left->key.identity, left->key.sid, left->key.incarnation) <
            std::tie(right->key.identity, right->key.sid, right->key.incarnation);
    });
    for (const auto* participant : participants) {
        std::vector<const RemotePublicationInfo*> publications;
        for (const auto& publication : participant->publications) {
            if (publication.kind == TrackKind::Video &&
                publication.key.participant.native_room_generation ==
                    catalog_->native_room_generation) {
                publications.push_back(&publication);
            }
        }
        std::stable_sort(publications.begin(), publications.end(),
            [](const auto* left, const auto* right) {
                const auto left_order = SourceOrder(left->source);
                const auto right_order = SourceOrder(right->source);
                if (left_order != right_order) return left_order < right_order;
                return left->key.publication_sid < right->key.publication_sid;
            });
        for (const auto* publication : publications) {
            result.push_back({publication->key, publication->source, publication});
        }
        if (publications.empty()) {
            result.push_back({{participant->key, 0, {}}, TrackSource::Camera, nullptr});
        }
    }
    return result;
}

std::vector<TrackKey> VideoDemandPolicy::SelectedAudio() const {
    std::vector<TrackKey> result;
    if (!catalog_) return result;
    for (const auto& participant : catalog_->participants) {
        for (const auto& publication : participant.publications) {
            if (publication.kind == TrackKind::Audio &&
                publication.subscription_allowed &&
                publication.key.participant.native_room_generation ==
                    catalog_->native_room_generation &&
                !Contains(result, publication.key)) {
                result.push_back(publication.key);
            }
        }
    }
    return result;
}

std::optional<TrackKey> VideoDemandPolicy::ResolveSpeakerCamera() const {
    for (const auto& speaker : speakers_) {
        if (speaker.is_local || !speaker.speaking || !catalog_ ||
            speaker.key.native_room_generation != catalog_->native_room_generation) {
            continue;
        }
        for (const auto& publication : OrderedVideo()) {
            if (publication->key.participant == speaker.key &&
                publication->source == TrackSource::Camera &&
                IsDemandable(*publication)) {
                return publication->key;
            }
        }
    }
    return std::nullopt;
}

std::optional<TrackKey> VideoDemandPolicy::ResolveIntentTrack(
    const std::optional<TrackKey>& key,
    std::optional<TrackSource> required_source) const {
    if (!key) return std::nullopt;
    const auto* publication = Find(*key);
    if (!publication || publication->kind != TrackKind::Video ||
        (required_source && publication->source != *required_source)) {
        return std::nullopt;
    }
    return publication->key;
}

std::optional<TrackKey> VideoDemandPolicy::ResolveIntentSeat(
    const std::optional<TrackKey>& key) const {
    if (!key) return std::nullopt;
    if (!key->publication_sid.empty()) return ResolveIntentTrack(key);
    for (const auto& candidate : OrderedSeats()) {
        if (candidate.key == *key) return candidate.key;
    }
    return std::nullopt;
}

bool VideoDemandPolicy::IsAutoShare(const TrackKey& key) const {
    const auto* publication = Find(key);
    return publication && publication->source == TrackSource::ScreenShareVideo &&
        IsDemandable(*publication);
}

bool VideoDemandPolicy::IsDemandable(
    const RemotePublicationInfo& publication) const {
    return publication.kind == TrackKind::Video &&
        publication.subscription_allowed && !publication.muted;
}

void VideoDemandPolicy::AdvanceSpeaker(TimePoint now) {
    const auto resolved = ResolveSpeakerCamera();
    if (!SameOptionalKey(resolved, speaker_candidate_)) {
        speaker_candidate_ = resolved;
        speaker_candidate_since_ = resolved ? std::optional<TimePoint>(now)
                                            : std::nullopt;
        if (!resolved) stable_speaker_.reset();
        return;
    }
    if (!speaker_candidate_ || !speaker_candidate_since_) {
        stable_speaker_.reset();
        return;
    }
    if (now - *speaker_candidate_since_ >= config_.speaker_candidate_delay) {
        stable_speaker_ = speaker_candidate_;
    }
    if (stable_speaker_ && !Find(*stable_speaker_)) stable_speaker_.reset();
}

void VideoDemandPolicy::RefreshAutoShare() {
    if (current_auto_share_ && IsAutoShare(*current_auto_share_)) return;
    current_auto_share_.reset();
    for (const auto* publication : OrderedVideo()) {
        if (publication->source == TrackSource::ScreenShareVideo &&
            IsDemandable(*publication)) {
            current_auto_share_ = publication->key;
            return;
        }
    }
}

VideoDemandPolicy::FocusChoice VideoDemandPolicy::ChooseFocus(
    const std::vector<SeatCandidate>& videos,
    bool allow_auto_share,
    TimePoint now) {
    const auto eligible = [&](std::optional<TrackKey> key) {
        return key && std::any_of(videos.begin(), videos.end(),
            [&](const auto& seat) { return seat.key == *key; }) ? key : std::nullopt;
    };
    const auto pinned = viewport_.pinned_local
        ? std::nullopt : eligible(ResolveIntentSeat(viewport_.pinned));
    const auto selected_share = eligible(ResolveIntentTrack(
        viewport_.selected_share, TrackSource::ScreenShareVideo));
    const auto auto_share = allow_auto_share && current_auto_share_ &&
            IsAutoShare(*current_auto_share_)
        ? eligible(current_auto_share_) : std::nullopt;

    auto set_focus = [&](const std::optional<TrackKey>& key,
                         FocusOrigin origin) -> FocusChoice {
        if (!SameOptionalKey(current_focus_, key)) focus_since_ = now;
        current_focus_ = key;
        current_focus_origin_ = key ? origin : FocusOrigin::None;
        if (!key) focus_since_.reset();
        return {key, current_focus_origin_};
    };

    if (pinned) return set_focus(pinned, FocusOrigin::Pinned);
    if (selected_share) {
        return set_focus(selected_share, FocusOrigin::SelectedShare);
    }
    if (auto_share) return set_focus(auto_share, FocusOrigin::AutoShare);

    const bool previous_override =
        current_focus_origin_ == FocusOrigin::Pinned ||
        current_focus_origin_ == FocusOrigin::SelectedShare ||
        current_focus_origin_ == FocusOrigin::AutoShare;
    const bool current_valid = eligible(ResolveIntentSeat(current_focus_)).has_value();
    const auto stable = ResolveIntentTrack(stable_speaker_, TrackSource::Camera);
    if (stable) {
        if (current_focus_ && *current_focus_ == *stable) {
            current_focus_origin_ = FocusOrigin::ActiveSpeaker;
            return {current_focus_, current_focus_origin_};
        }
        if (current_valid && !previous_override && focus_since_ &&
            now - *focus_since_ < config_.focus_minimum_residence) {
            return {current_focus_, current_focus_origin_};
        }
        return set_focus(stable, FocusOrigin::ActiveSpeaker);
    }

    if (current_valid && !previous_override) {
        return {current_focus_, current_focus_origin_};
    }
    if (!videos.empty()) {
        return set_focus(videos.front().key, FocusOrigin::Fallback);
    }
    return set_focus(std::nullopt, FocusOrigin::None);
}

VideoDemandPlan VideoDemandPolicy::BuildPlan(TimePoint now) {
    std::erase_if(thumbnail_speakers_, [&](const auto& speaker) {
        return speaker.stopped &&
            now - *speaker.stopped >= config_.thumbnail_speaker_retention;
    });
    VideoDemandPlan result;
    result.coordinator_session = coordinator_session_;
    result.view_revision = viewport_.view_revision;
    result.requested_mode = viewport_.mode;
    result.mode = viewport_.mode;
    result.speaker_layout = viewport_.speaker_layout;
    result.speaking_only = viewport_.speaking_only;
    result.stage_content = viewport_.stage_content;
    const auto stage = StageExtent();
    const bool local_visible = viewport_.window_visible && !viewport_.minimized &&
        stage.width != 0 && stage.height != 0 &&
        viewport_.stage_content != StageContent::Whiteboard;
    result.show_local_participant = local_visible &&
        viewport_.local_participant_present;
    result.show_local_screen_share = local_visible &&
        viewport_.local_screen_share_present;
    const auto* detached = viewport_.detached_share && viewport_.detached_share->surface_id
        ? &*viewport_.detached_share : nullptr;
    const bool detached_visible = detached && detached->visible && !detached->minimized &&
        detached->rect.width > 0 && detached->rect.height > 0;
    if (detached) {
        result.detached_surface_id = detached->surface_id;
        result.detached_local = detached->local && !detached->track &&
            viewport_.local_screen_share_present;
        result.show_detached_local = result.detached_local && detached_visible;
        if (result.detached_local) result.show_local_screen_share = false;
    }
    if (viewport_.pinned_local) {
        if ((*viewport_.pinned_local == LocalVideoTarget::Camera &&
             viewport_.local_participant_present) ||
            (*viewport_.pinned_local == LocalVideoTarget::ScreenShare &&
             viewport_.local_screen_share_present && !result.detached_local)) {
            result.pinned_local = viewport_.pinned_local;
        }
    } else {
        result.pinned = ResolveIntentSeat(viewport_.pinned);
    }
    if (!catalog_) {
        FinalizeLayout(result);
        return result;
    }

    result.catalog_revision = catalog_->catalog_revision;
    result.selected_audio = SelectedAudio();
    if (detached && !detached->local) {
        const auto key = ResolveIntentTrack(detached->track, TrackSource::ScreenShareVideo);
        if (key) {
            const auto* publication = Find(*key);
            VideoDemandPlan share;
            const double dpr = std::isfinite(detached->device_pixel_ratio) &&
                detached->device_pixel_ratio > 0 ? detached->device_pixel_ratio : 1.0;
            const auto pixels = [dpr, detached_visible](int value) -> uint32_t {
                return detached_visible ? static_cast<uint32_t>(std::min<double>(
                    std::max(0, value) * dpr, 16384.0)) : 0;
            };
            AddSeat(share, SeatCandidate{publication->key, publication->source, publication},
                VideoSeatRole::Main, {pixels(detached->rect.width), pixels(detached->rect.height)},
                FocusOrigin::SelectedShare);
            if (!share.visible_seats.empty()) result.detached_share = share.visible_seats.front();
            result.selected_video = std::move(share.selected_video);
            if (result.pinned == key) result.pinned.reset();
        }
    }
    AdvanceSpeaker(now);
    RefreshAutoShare();
    result.stable_speaker = ResolveIntentTrack(
        stable_speaker_, TrackSource::Camera);

    if (!viewport_.window_visible || viewport_.minimized ||
        stage.width == 0 || stage.height == 0) {
        result.reason = VideoDemandReason::Hidden;
        return result;
    }
    if (viewport_.stage_content == StageContent::Whiteboard) {
        result.reason = VideoDemandReason::Whiteboard;
        return result;
    }

    auto videos = OrderedSeats();
    if (result.detached_share) {
        std::erase_if(videos, [&](const auto& seat) { return seat.key == result.detached_share->key; });
    }
    const auto on_main = [&](const std::optional<TrackKey>& key) {
        return key && std::any_of(videos.begin(), videos.end(),
            [&](const auto& seat) { return seat.key == *key; });
    };
    const bool pinned = result.pinned.has_value() || result.pinned_local.has_value();
    const bool selected_share = on_main(ResolveIntentTrack(
        viewport_.selected_share, TrackSource::ScreenShareVideo));
    const bool auto_share = on_main(current_auto_share_) && IsAutoShare(*current_auto_share_);

    if (viewport_.mode == VideoLayoutMode::PictureInPicture) {
        BuildFocused(result, videos, VideoLayoutMode::PictureInPicture,
                     true, now);
    } else if (viewport_.mode == VideoLayoutMode::Speaker || pinned ||
               selected_share ||
               (viewport_.mode == VideoLayoutMode::Auto && auto_share)) {
        BuildFocused(result, videos, VideoLayoutMode::Speaker,
                     viewport_.mode != VideoLayoutMode::Grid, now);
    } else if (viewport_.mode == VideoLayoutMode::Auto && videos.size() <= 2) {
        result.mode = VideoLayoutMode::Auto;
        BuildGrid(result, videos, 2, false);
    } else {
        result.mode = viewport_.mode == VideoLayoutMode::Auto
            ? VideoLayoutMode::Grid : viewport_.mode;
        BuildGrid(result, videos,
                  NormalizePageSize(viewport_.page_size,
                                    config_.default_grid_page_size),
                  true);
    }
    FinalizeLayout(result);
    return result;
}

void VideoDemandPolicy::BuildGrid(
    VideoDemandPlan& plan,
    const std::vector<SeatCandidate>& videos,
    uint32_t page_size,
    bool use_paging) {
    const uint32_t reserved = plan.detached_share && plan.detached_share->width &&
        plan.detached_share->height ? 1 : 0;
    if (reserved) page_size = std::min(page_size, config_.video_budget - reserved);
    page_size = std::max<uint32_t>(1, page_size);
    plan.effective_main_page_size = page_size;
    plan.page_size = page_size;
    const uint32_t local_count = use_paging
        ? static_cast<uint32_t>(plan.show_local_participant) +
            static_cast<uint32_t>(plan.show_local_screen_share)
        : 0;
    plan.page_count = use_paging
        ? CeilDiv(videos.size() + local_count, page_size) : 1;
    plan.page = use_paging
        ? std::min(viewport_.page, plan.page_count - 1) : 0;

    const std::size_t page_offset = use_paging
        ? static_cast<std::size_t>(plan.page) * page_size : 0;
    std::size_t start = page_offset > local_count ? page_offset - local_count : 0;
    const bool first_local_page = use_paging && plan.page == 0 && local_count != 0;
    if (first_local_page) {
        // Local tiles fix the beginning of the first page; a stale remote anchor
        // must not skip participants after an earlier remote seat disappears.
        if (viewport_.page == 0 || !grid_page_.replay_anchor) {
            grid_page_.ResetAnchor();
        }
    } else if (use_paging) {
        if (grid_page_.requested_page != viewport_.page ||
            grid_page_.page_size != page_size) {
            grid_page_.ResetAnchor();
        } else if (plan.page != viewport_.page && !grid_page_.replay_anchor) {
            grid_page_.anchor.reset();
        }
        grid_page_.requested_page = viewport_.page;
        grid_page_.page_size = page_size;
        if (grid_page_.replay_anchor && plan.page == viewport_.page) {
            const auto& identity = *grid_page_.replay_anchor;
            const auto same_source = [&](const auto& candidate) {
                return candidate.publication &&
                    candidate.key.participant.identity == identity.participant_identity &&
                    candidate.source == identity.source;
            };
            auto found = std::find_if(videos.begin(), videos.end(),
                [&](const auto& candidate) {
                    return same_source(candidate) &&
                        candidate.key.publication_sid == identity.publication_sid;
                });
            // Uniqueness in a partial replay alone cannot distinguish two old
            // cameras. Only an originally unique source may change its SID.
            if (found == videos.end() && identity.source_was_unique &&
                std::count_if(videos.begin(), videos.end(), same_source) == 1) {
                found = std::find_if(videos.begin(), videos.end(), same_source);
            }
            if (found != videos.end()) {
                grid_page_.anchor = found->key;
                grid_page_.replay_anchor.reset();
            }
        }
        if (grid_page_.anchor) {
            const auto found = std::find_if(videos.begin(), videos.end(),
                [&](const auto& candidate) {
                    return candidate.key == *grid_page_.anchor;
                });
            if (found != videos.end()) {
                start = static_cast<std::size_t>(found - videos.begin());
            } else {
                grid_page_.anchor.reset();
            }
        }
        if (!grid_page_.anchor && !grid_page_.replay_anchor && start < videos.size()) {
            grid_page_.anchor = videos[start].key;
        }
    }

    const uint32_t remote_capacity = first_local_page ? (page_size > local_count ? page_size - local_count : 0)
                                                    : page_size;
    if (use_paging && plan.page != 0) {
        plan.show_local_participant = false;
        plan.show_local_screen_share = false;
    }
    const std::size_t limit = std::min<std::size_t>(
        videos.size(), start + std::min(remote_capacity, config_.video_budget));
    for (std::size_t index = start; index < limit; ++index) {
        AddSeat(plan, videos[index], VideoSeatRole::Grid, {});
    }
    plan.reason = VideoDemandReason::Visible;
}

void VideoDemandPolicy::BuildFocused(
    VideoDemandPlan& plan,
    const std::vector<SeatCandidate>& videos,
    VideoLayoutMode mode,
    bool allow_auto_share,
    TimePoint now) {
    plan.mode = mode;
    const auto focus = plan.pinned_local
        ? FocusChoice{std::nullopt, FocusOrigin::Pinned}
        : ChooseFocus(videos, allow_auto_share, now);
    plan.focused = focus.key;
    plan.focused_local = plan.pinned_local;
    if (!focus.key && !plan.focused_local) {
        if (plan.show_local_participant) plan.focused_local = LocalVideoTarget::Camera;
        else if (plan.show_local_screen_share) plan.focused_local = LocalVideoTarget::ScreenShare;
    }
    if (plan.focused_local && plan.pinned_local) {
        current_focus_.reset();
        current_focus_origin_ = FocusOrigin::Pinned;
        focus_since_.reset();
    }
    if (!focus.key && !plan.focused_local) {
        plan.reason = VideoDemandReason::Visible;
        return;
    }
    const auto main = std::find_if(videos.begin(), videos.end(),
        [&](const auto& candidate) { return focus.key && candidate.key == *focus.key; });
    if (main == videos.end() && !plan.focused_local) return;

    // Legacy PiP is an internal compatibility mode: local camera/share previews
    // remain visible independently of its one additional remote thumbnail.
    // The new Floating speaker list uses the paginated speaker path below.
    if (mode == VideoLayoutMode::PictureInPicture) {
        if (main != videos.end()) {
            AddSeat(plan, *main, VideoSeatRole::Main, {}, focus.origin);
        }
        const auto secondary = std::find_if(videos.begin(), videos.end(),
            [&](const auto& candidate) {
                return main == videos.end() || candidate.key != main->key;
            });
        const bool has_pip = secondary != videos.end() && config_.video_budget > 1;
        if (has_pip) AddSeat(plan, *secondary, VideoSeatRole::PictureInPicture, {});
        plan.page = 0;
        plan.page_size = has_pip ? 1 : 0;
        plan.page_count = 1;
        const auto source = main != videos.end() ? main->source
            : (*plan.focused_local == LocalVideoTarget::ScreenShare
                ? TrackSource::ScreenShareVideo : TrackSource::Camera);
        plan.reason = ReasonForFocus(focus.origin, source);
        return;
    }

    const bool filter = viewport_.speaking_only;
    std::vector<SeatCandidate> other;
    for (const auto& candidate : videos) {
        if (main != videos.end() && candidate.key == main->key) continue;
        if (filter && !IsThumbnailSpeaker(candidate.key.participant, false, now)) continue;
        other.push_back(candidate);
    }
    const bool camera_main = plan.focused_local == LocalVideoTarget::Camera;
    const bool screen_main = plan.focused_local == LocalVideoTarget::ScreenShare;
    const bool local_speaking = !filter || IsThumbnailSpeaker({}, true, now);
    const bool camera_other = plan.show_local_participant && !camera_main && local_speaking;
    const bool screen_other = plan.show_local_screen_share && !screen_main && local_speaking;
    const uint32_t local_count = static_cast<uint32_t>(camera_other) +
        static_cast<uint32_t>(screen_other);
    const std::size_t thumbnail_count = other.size() + local_count;
    const auto capacity_for = [&](LogicalViewport stage) {
        uint32_t capacity = SpeakerThumbnailCapacity(viewport_.speaker_layout, stage);
        const bool l_shape = viewport_.speaker_layout == SpeakerLayout::TopLeft ||
            viewport_.speaker_layout == SpeakerLayout::BottomLeft;
        const uint64_t configured = uint64_t(config_.sidebar_limit) * (l_shape ? 2 : 1);
        capacity = static_cast<uint32_t>(std::min<uint64_t>(capacity, configured));
        const uint32_t remote_main = main != videos.end() ? 1u : 0u;
        // Keep one display capacity across pages: adding first-page local seats
        // to the remote budget would skip remote candidates on subsequent pages.
        // Local previews themselves never create remote bindings, including a
        // local-only meeting whose remote-video budget is zero.
        const uint32_t detached_count = plan.detached_share && plan.detached_share->width &&
            plan.detached_share->height ? 1 : 0;
        const uint32_t remote_limit = config_.video_budget -
            std::min(config_.video_budget, remote_main + detached_count);
        return std::min(capacity, other.empty() ? local_count : remote_limit);
    };
    uint32_t side_limit = capacity_for(viewport_.stage_rect);
    if (thumbnail_count > side_limit && side_limit != 0) {
        // Decide whether paging is needed from the full area, independent of
        // the previous UI plan. Reserving a row may shrink horizontal cards
        // and increase capacity; do not let that undo the paging decision.
        side_limit = std::min(side_limit, capacity_for(StageWithPaging(viewport_, true)));
    }
    plan.page_size = side_limit;
    plan.page_count = side_limit == 0 ? 1 : CeilDiv(thumbnail_count, side_limit);
    plan.page = std::min(viewport_.page, plan.page_count - 1);
    const std::size_t offset = static_cast<std::size_t>(plan.page) * side_limit;
    std::size_t start = offset > local_count ? offset - local_count : 0;
    const bool camera_on_page = camera_other && offset == 0 && side_limit != 0;
    const bool screen_on_page = screen_other &&
        static_cast<std::size_t>(camera_other) >= offset &&
        static_cast<std::size_t>(camera_other) < offset + side_limit;
    const uint32_t local_on_page = static_cast<uint32_t>(camera_on_page) +
        static_cast<uint32_t>(screen_on_page);
    // Speaking membership changes at confirmation/retention deadlines, even
    // without a new viewport. Page it by current roster offset: retaining an
    // old anchor would skip speakers inserted before it and leave page gaps.
    if (filter || sidebar_page_.requested_page != viewport_.page ||
        (sidebar_page_.page_size != 0 && sidebar_page_.page_size != side_limit) ||
        plan.page != viewport_.page ||
        local_on_page != 0) {
        sidebar_page_.anchor.reset();
    }
    sidebar_page_.requested_page = viewport_.page;
    sidebar_page_.page_size = side_limit;
    if (sidebar_page_.anchor) {
        const auto found = std::find_if(other.begin(), other.end(),
            [&](const auto& candidate) { return candidate.key == *sidebar_page_.anchor; });
        if (found != other.end()) start = static_cast<std::size_t>(found - other.begin());
        else sidebar_page_.anchor.reset();
    }
    if (!filter && !sidebar_page_.anchor && start < other.size() && local_on_page == 0) {
        sidebar_page_.anchor = other[start].key;
    }
    plan.show_local_participant = camera_main || camera_on_page;
    plan.show_local_screen_share = screen_main || screen_on_page;
    const uint32_t remote_capacity = side_limit - local_on_page;
    const auto remaining = start < other.size() ? other.size() - start : 0;
    const uint32_t side_count = static_cast<uint32_t>(
        std::min<std::size_t>(remote_capacity, remaining));
    if (main != videos.end()) {
        AddSeat(plan, *main, VideoSeatRole::Main, {}, focus.origin);
    }
    for (uint32_t index = 0; index != side_count; ++index) {
        AddSeat(plan, other[start + index], VideoSeatRole::Sidebar, {});
    }
    const auto source = main != videos.end() ? main->source
        : (*plan.focused_local == LocalVideoTarget::ScreenShare
            ? TrackSource::ScreenShareVideo : TrackSource::Camera);
    plan.reason = ReasonForFocus(focus.origin, source);
}

bool VideoDemandPolicy::IsThumbnailSpeaker(const ParticipantKey& key,
                                           bool is_local, TimePoint now) const {
    return std::any_of(thumbnail_speakers_.begin(), thumbnail_speakers_.end(),
        [&](const auto& speaker) {
            if (speaker.is_local != is_local || (!is_local && speaker.key != key)) return false;
            const auto confirmed_at = speaker.started + config_.thumbnail_speaker_delay;
            if (now < confirmed_at || (speaker.stopped && *speaker.stopped < confirmed_at)) return false;
            return !speaker.stopped || now - *speaker.stopped < config_.thumbnail_speaker_retention;
        });
}

void VideoDemandPolicy::FinalizeLayout(VideoDemandPlan& plan) {
    const bool gallery = plan.mode == VideoLayoutMode::Grid || plan.mode == VideoLayoutMode::Auto;
    // This is the same display order used by MeetingRoomWindow: camera first,
    // gallery screen preview second, then remote seats; focused screen last.
    const std::size_t camera_index = 0;
    const std::size_t screen_index = gallery
        ? static_cast<std::size_t>(plan.show_local_participant)
        : static_cast<std::size_t>(plan.show_local_participant) + plan.visible_seats.size();
    const std::size_t remote_offset = static_cast<std::size_t>(plan.show_local_participant) +
        static_cast<std::size_t>(gallery && plan.show_local_screen_share);
    const std::size_t total = plan.visible_seats.size() +
        static_cast<std::size_t>(plan.show_local_participant) +
        static_cast<std::size_t>(plan.show_local_screen_share);
    std::size_t main_index = 0;
    if (plan.focused_local == LocalVideoTarget::Camera && plan.show_local_participant) {
        main_index = camera_index;
    } else if (plan.focused_local == LocalVideoTarget::ScreenShare && plan.show_local_screen_share) {
        main_index = screen_index;
    } else if (plan.focused) {
        const auto found = std::find_if(plan.visible_seats.begin(), plan.visible_seats.end(),
            [&](const auto& seat) { return seat.key == *plan.focused; });
        if (found != plan.visible_seats.end()) {
            main_index = remote_offset + static_cast<std::size_t>(found - plan.visible_seats.begin());
        }
    }
    const auto rects = CalculateVideoLayout(plan.mode, plan.speaker_layout,
        StageWithPaging(viewport_, plan.page_count > 1), plan.page_size, total, main_index);
    if (plan.show_local_participant) plan.local_participant_rect = rects[camera_index];
    if (plan.show_local_screen_share) plan.local_screen_share_rect = rects[screen_index];
    auto seats = std::move(plan.visible_seats);
    plan.visible_seats.clear();
    // Preserve the independent surface's validated demand while rebuilding
    // main-window geometry and subscriptions.
    const bool keep_detached = plan.detached_share && Contains(plan.selected_video, plan.detached_share->key);
    plan.selected_video.clear();
    if (keep_detached) plan.selected_video.push_back(plan.detached_share->key);
    for (std::size_t index = 0; index < seats.size(); ++index) {
        const auto& old = seats[index];
        FocusOrigin origin = FocusOrigin::None;
        if (old.reason == VideoDemandReason::Pinned || old.priority == 500) origin = FocusOrigin::Pinned;
        else if (old.reason == VideoDemandReason::ActiveSpeaker) origin = FocusOrigin::ActiveSpeaker;
        const auto& rect = rects[remote_offset + index];
        AddSeat(plan, {old.key, old.source, Find(old.key)}, old.role, RectExtent(rect), origin);
        if (plan.visible_seats.size() == index + 1) plan.visible_seats.back().rect = rect;
    }
}

void VideoDemandPolicy::AddSeat(VideoDemandPlan& plan,
                                const SeatCandidate& candidate,
                                VideoSeatRole role,
                                SeatExtent extent,
                                FocusOrigin origin) {
    const uint32_t reserved = plan.detached_share && plan.detached_share->width &&
        plan.detached_share->height ? 1 : 0;
    if (plan.visible_seats.size() + reserved >= config_.video_budget) return;
    VideoSeat seat;
    seat.key = candidate.key;
    seat.source = candidate.source;
    seat.role = role;
    seat.reason = ReasonForFocus(origin, candidate.source);
    if (!candidate.publication) {
        plan.visible_seats.push_back(seat);
        return;
    }
    const auto& publication = *candidate.publication;
    seat.subscription_error = publication.subscription_error;
    if (!publication.subscription_allowed) {
        seat.reason = VideoDemandReason::PermissionDenied;
    } else if (publication.muted) {
        seat.reason = VideoDemandReason::Muted;
    }

    if (extent.width != 0 && extent.height != 0) {
        const bool known_source = publication.source_width != 0 &&
            publication.source_height != 0;
        auto layers = NormalizePublishedVideoLayers(publication.published_video_layers);
        const auto largest = layers.empty() ? layers.end()
            : std::max_element(layers.begin(), layers.end(), SmallerLayer);
        // When original dimensions are absent, declared layer geometry still
        // supplies the video aspect. With neither, use the viewport aspect.
        const double source_width = known_source ? publication.source_width
            : largest != layers.end() ? largest->width : extent.width;
        const double source_height = known_source ? publication.source_height
            : largest != layers.end() ? largest->height : extent.height;
        uint32_t max_width = role == VideoSeatRole::Main ? 3840 : 2560;
        uint32_t max_height = role == VideoSeatRole::Main ? 2160 : 1440;
        if (source_height > source_width) {
            std::swap(max_width, max_height);
        }
        double scale = std::min(extent.width / source_width,
                                extent.height / source_height);
        scale = std::min(scale, std::min(max_width / source_width,
                                        max_height / source_height));
        // Never request an upscale when the publication advertises a smaller source.
        if (known_source || largest != layers.end()) scale = std::min(scale, 1.0);
        seat.width = std::min(max_width, static_cast<uint32_t>(
            std::max<long>(1, std::lround(source_width * scale))));
        seat.height = std::min(max_height, static_cast<uint32_t>(
            std::max<long>(1, std::lround(source_height * scale))));
        const auto short_edge = std::min(seat.width, seat.height);
        if (short_edge <= 180) seat.quality = VideoQualityTier::P180;
        else if (short_edge <= 360) seat.quality = VideoQualityTier::P360;
        else if (short_edge <= 720) seat.quality = VideoQualityTier::P720;
        else if (short_edge <= 1080) seat.quality = VideoQualityTier::P1080;
        else if (short_edge <= 1440) seat.quality = VideoQualityTier::P1440;
        else seat.quality = VideoQualityTier::P2160;

        if (layers.empty()) {
            // Compatibility when no codec layer declaration is available.
            // Original source dimensions alone do not prove a single layer.
            // This expresses a pixel request, not evidence of an available layer.
            seat.subscription_width = seat.width;
            seat.subscription_height = seat.height;
        } else if (const auto selected = SelectPublishedLayer(
                       layers, role, seat.width, seat.height, viewport_.resolution_match)) {
            seat.subscription_width = selected->width;
            seat.subscription_height = selected->height;
            seat.selected_layer_quality = selected->quality;
        } else if (publication.subscription_allowed && !publication.muted) {
            seat.reason = VideoDemandReason::NoCompatibleLayer;
        }
    }

    switch (role) {
    case VideoSeatRole::Grid: seat.priority = 100; break;
    case VideoSeatRole::Sidebar: seat.priority = 120; break;
    case VideoSeatRole::PictureInPicture: seat.priority = 200; break;
    case VideoSeatRole::Main: seat.priority = 400; break;
    }
    if (publication.source == TrackSource::ScreenShareVideo) seat.priority += 50;
    if (origin == FocusOrigin::Pinned) seat.priority = 500;

    if (!viewport_.remote_video_enabled) seat.reason = VideoDemandReason::Hidden;
    plan.visible_seats.push_back(seat);
    if (viewport_.remote_video_enabled && IsDemandable(publication) && seat.subscription_width != 0 &&
        seat.subscription_height != 0 &&
        plan.selected_video.size() < config_.video_budget &&
        !Contains(plan.selected_video, publication.key)) {
        plan.selected_video.push_back(publication.key);
    }
}

VideoDemandPolicy::SeatExtent VideoDemandPolicy::RectExtent(const LogicalViewport& rect) const {
    const double dpr = std::isfinite(viewport_.device_pixel_ratio) &&
            viewport_.device_pixel_ratio > 0.0
        ? viewport_.device_pixel_ratio : 1.0;
    const auto scale = [dpr](int value) -> uint32_t {
        if (value <= 0) return 0;
        const double scaled = static_cast<double>(value) * dpr;
        return static_cast<uint32_t>(std::round(std::min<double>(
            scaled, std::numeric_limits<uint32_t>::max())));
    };
    return {scale(rect.width), scale(rect.height)};
}

VideoDemandPolicy::SeatExtent VideoDemandPolicy::StageExtent() const {
    return RectExtent(viewport_.stage_rect);
}

uint32_t VideoDemandPolicy::NormalizePageSize(uint32_t requested,
                                              uint32_t default_size) {
    if (requested == 4 || requested == 9 || requested == 16 ||
        requested == 25 || requested == 49) return requested;
    return default_size == 4 || default_size == 9 || default_size == 16 ||
            default_size == 25 || default_size == 49
        ? default_size : 9;
}

VideoDemandReason VideoDemandPolicy::ReasonForFocus(FocusOrigin origin,
                                                    TrackSource source) {
    switch (origin) {
    case FocusOrigin::Pinned:
        return VideoDemandReason::Pinned;
    case FocusOrigin::SelectedShare:
    case FocusOrigin::AutoShare:
        return VideoDemandReason::ScreenShare;
    case FocusOrigin::ActiveSpeaker:
        return VideoDemandReason::ActiveSpeaker;
    default:
        return source == TrackSource::ScreenShareVideo
            ? VideoDemandReason::ScreenShare
            : VideoDemandReason::Visible;
    }
}

bool VideoDemandPolicy::SamePlan(const VideoDemandPlan& left,
                                 const VideoDemandPlan& right) {
    if (left.coordinator_session != right.coordinator_session ||
        left.catalog_revision != right.catalog_revision ||
        left.requested_mode != right.requested_mode ||
        left.speaker_layout != right.speaker_layout ||
        left.speaking_only != right.speaking_only ||
        left.mode != right.mode || left.stage_content != right.stage_content ||
        left.page != right.page || left.page_size != right.page_size ||
        left.page_count != right.page_count ||
        left.show_local_participant != right.show_local_participant ||
        left.show_local_screen_share != right.show_local_screen_share ||
        left.local_participant_rect != right.local_participant_rect ||
        left.local_screen_share_rect != right.local_screen_share_rect ||
        left.detached_surface_id != right.detached_surface_id ||
        left.detached_share != right.detached_share ||
        left.detached_local != right.detached_local ||
        left.show_detached_local != right.show_detached_local ||
        left.effective_main_page_size != right.effective_main_page_size ||
        !SameOptionalKey(left.focused, right.focused) ||
        left.focused_local != right.focused_local ||
        !SameOptionalKey(left.pinned, right.pinned) ||
        left.pinned_local != right.pinned_local ||
        !SameOptionalKey(left.stable_speaker, right.stable_speaker) ||
        left.reason != right.reason ||
        left.selected_video != right.selected_video ||
        left.selected_audio != right.selected_audio ||
        left.visible_seats.size() != right.visible_seats.size()) {
        return false;
    }
    for (std::size_t index = 0; index != left.visible_seats.size(); ++index) {
        const auto& lhs = left.visible_seats[index];
        const auto& rhs = right.visible_seats[index];
        if (lhs.key != rhs.key || lhs.source != rhs.source ||
            lhs.role != rhs.role || lhs.rect != rhs.rect ||
            lhs.width != rhs.width || lhs.height != rhs.height ||
            lhs.subscription_width != rhs.subscription_width ||
            lhs.subscription_height != rhs.subscription_height ||
            lhs.selected_layer_quality != rhs.selected_layer_quality ||
            lhs.quality != rhs.quality || lhs.priority != rhs.priority ||
            lhs.reason != rhs.reason || lhs.subscription_error != rhs.subscription_error) {
            return false;
        }
    }
    return true;
}

} // namespace livekit
