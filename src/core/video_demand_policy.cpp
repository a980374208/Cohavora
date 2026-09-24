#include "video_demand_policy.h"

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
        left.page_size == right.page_size &&
        SameOptionalKey(left.page_anchor, right.page_anchor) &&
        SameOptionalKey(left.pinned, right.pinned) &&
        SameOptionalKey(left.selected_share, right.selected_share) &&
        left.stage_content == right.stage_content &&
        left.stage_rect.x == right.stage_rect.x &&
        left.stage_rect.y == right.stage_rect.y &&
        left.stage_rect.width == right.stage_rect.width &&
        left.stage_rect.height == right.stage_rect.height &&
        left.device_pixel_ratio == right.device_pixel_ratio &&
        left.window_visible == right.window_visible &&
        left.minimized == right.minimized;
}

bool SameSpeakerInput(const ActiveSpeakerInfo& left,
                      const ActiveSpeakerInfo& right) {
    return left.key == right.key && left.is_local == right.is_local &&
        left.speaking == right.speaking;
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

} // namespace

VideoDemandPolicy::VideoDemandPolicy(uint64_t coordinator_session,
                                     VideoDemandPolicyConfig config)
    : coordinator_session_(coordinator_session), config_(std::move(config)) {
    if (config_.video_budget > 16) config_.video_budget = 16;
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
    if (!has_viewport_ || intent.mode != viewport_.mode ||
        intent.page != viewport_.page ||
        normalized_size != NormalizePageSize(
            viewport_.page_size, config_.default_grid_page_size)) {
        if (intent.mode == VideoLayoutMode::Speaker) {
            sidebar_page_.anchor.reset();
        } else {
            grid_page_.anchor.reset();
        }
    }
    viewport_ = intent;
    has_viewport_ = true;
    if (intent.page_anchor) {
        if (intent.mode == VideoLayoutMode::Speaker) {
            sidebar_page_.anchor = intent.page_anchor;
            sidebar_page_.requested_page = intent.page;
            sidebar_page_.page_size = config_.sidebar_limit;
        } else {
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
        Find(*current_focus_) && !override_focus && focus_since_) {
        consider(*focus_since_ + config_.focus_minimum_residence);
    }
    return next;
}

void VideoDemandPolicy::Retire() {
    if (!accepting_) return;
    accepting_ = false;
    catalog_.reset();
    speakers_.clear();
    speaker_candidate_.reset();
    speaker_candidate_since_.reset();
    stable_speaker_.reset();
    current_auto_share_.reset();
    current_focus_.reset();
    focus_since_.reset();
    grid_page_.anchor.reset();
    sidebar_page_.anchor.reset();

    VideoDemandPlan stopped;
    stopped.coordinator_session = coordinator_session_;
    stopped.policy_revision = plan_.policy_revision;
    stopped.requested_mode = viewport_.mode;
    stopped.mode = viewport_.mode;
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
    if (!catalog_) return result;
    for (const auto& participant : catalog_->participants) {
        std::vector<const RemotePublicationInfo*> publications;
        for (const auto& publication : participant.publications) {
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
        result.insert(result.end(), publications.begin(), publications.end());
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
    const std::vector<const RemotePublicationInfo*>& videos,
    bool allow_auto_share,
    TimePoint now) {
    const auto pinned = ResolveIntentTrack(viewport_.pinned);
    const auto selected_share = ResolveIntentTrack(
        viewport_.selected_share, TrackSource::ScreenShareVideo);
    const auto auto_share = allow_auto_share && current_auto_share_ &&
            IsAutoShare(*current_auto_share_)
        ? current_auto_share_ : std::nullopt;

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
    const bool current_valid = current_focus_ && Find(*current_focus_);
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
        return set_focus(videos.front()->key, FocusOrigin::Fallback);
    }
    return set_focus(std::nullopt, FocusOrigin::None);
}

VideoDemandPlan VideoDemandPolicy::BuildPlan(TimePoint now) {
    VideoDemandPlan result;
    result.coordinator_session = coordinator_session_;
    result.requested_mode = viewport_.mode;
    result.mode = viewport_.mode;
    result.stage_content = viewport_.stage_content;
    if (!catalog_) return result;

    result.catalog_revision = catalog_->catalog_revision;
    result.selected_audio = SelectedAudio();
    AdvanceSpeaker(now);
    RefreshAutoShare();
    result.stable_speaker = ResolveIntentTrack(
        stable_speaker_, TrackSource::Camera);

    const auto stage = StageExtent();
    if (!viewport_.window_visible || viewport_.minimized ||
        stage.width == 0 || stage.height == 0) {
        result.reason = VideoDemandReason::Hidden;
        return result;
    }
    if (viewport_.stage_content == StageContent::Whiteboard) {
        result.reason = VideoDemandReason::Whiteboard;
        return result;
    }

    const auto videos = OrderedVideo();
    const bool pinned = ResolveIntentTrack(viewport_.pinned).has_value();
    const bool selected_share = ResolveIntentTrack(
        viewport_.selected_share, TrackSource::ScreenShareVideo).has_value();
    const bool auto_share = current_auto_share_ && IsAutoShare(*current_auto_share_);

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
    return result;
}

void VideoDemandPolicy::BuildGrid(
    VideoDemandPlan& plan,
    const std::vector<const RemotePublicationInfo*>& videos,
    uint32_t page_size,
    bool use_paging) {
    page_size = std::max<uint32_t>(1, page_size);
    plan.page_size = page_size;
    plan.page_count = use_paging ? CeilDiv(videos.size(), page_size) : 1;
    plan.page = use_paging
        ? std::min(viewport_.page, plan.page_count - 1) : 0;

    std::size_t start = use_paging
        ? static_cast<std::size_t>(plan.page) * page_size : 0;
    if (use_paging) {
        if (grid_page_.requested_page != viewport_.page ||
            grid_page_.page_size != page_size || plan.page != viewport_.page) {
            grid_page_.anchor.reset();
        }
        grid_page_.requested_page = viewport_.page;
        grid_page_.page_size = page_size;
        if (grid_page_.anchor) {
            const auto found = std::find_if(videos.begin(), videos.end(),
                [&](const auto* publication) {
                    return publication->key == *grid_page_.anchor;
                });
            if (found != videos.end()) {
                start = static_cast<std::size_t>(found - videos.begin());
            } else {
                grid_page_.anchor.reset();
            }
        }
        if (!grid_page_.anchor && start < videos.size()) {
            grid_page_.anchor = videos[start]->key;
        }
    }

    const auto extent = GridExtent(page_size);
    const std::size_t limit = std::min<std::size_t>(
        videos.size(), start + std::min(page_size, config_.video_budget));
    for (std::size_t index = start; index < limit; ++index) {
        AddSeat(plan, *videos[index], VideoSeatRole::Grid, extent);
    }
    plan.reason = VideoDemandReason::Visible;
}

void VideoDemandPolicy::BuildFocused(
    VideoDemandPlan& plan,
    const std::vector<const RemotePublicationInfo*>& videos,
    VideoLayoutMode mode,
    bool allow_auto_share,
    TimePoint now) {
    plan.mode = mode;
    const auto focus = ChooseFocus(videos, allow_auto_share, now);
    plan.focused = focus.key;
    if (!focus.key) {
        plan.reason = VideoDemandReason::Visible;
        return;
    }
    const auto* main = Find(*focus.key);
    if (!main) return;

    std::vector<const RemotePublicationInfo*> other;
    for (const auto* publication : videos) {
        if (publication->key != main->key) other.push_back(publication);
    }

    if (mode == VideoLayoutMode::PictureInPicture) {
        const bool has_pip = !other.empty() && config_.video_budget > 1;
        AddSeat(plan, *main, VideoSeatRole::Main,
                MainExtent(false), focus.origin);
        if (has_pip) {
            AddSeat(plan, *other.front(), VideoSeatRole::PictureInPicture,
                    PictureInPictureExtent());
        }
        plan.page = 0;
        plan.page_size = has_pip ? 1 : 0;
        plan.page_count = 1;
    } else {
        const uint32_t side_limit = config_.video_budget > 1
            ? std::min(config_.sidebar_limit, config_.video_budget - 1) : 0;
        plan.page_size = side_limit;
        plan.page_count = CeilDiv(other.size(), std::max<uint32_t>(1, side_limit));
        plan.page = std::min(viewport_.page, plan.page_count - 1);
        std::size_t start = static_cast<std::size_t>(plan.page) * side_limit;
        if (side_limit == 0) start = 0;
        if (sidebar_page_.requested_page != viewport_.page ||
            sidebar_page_.page_size != side_limit || plan.page != viewport_.page) {
            sidebar_page_.anchor.reset();
        }
        sidebar_page_.requested_page = viewport_.page;
        sidebar_page_.page_size = side_limit;
        if (sidebar_page_.anchor) {
            const auto found = std::find_if(other.begin(), other.end(),
                [&](const auto* publication) {
                    return publication->key == *sidebar_page_.anchor;
                });
            if (found != other.end()) {
                start = static_cast<std::size_t>(found - other.begin());
            } else {
                sidebar_page_.anchor.reset();
            }
        }
        if (!sidebar_page_.anchor && start < other.size()) {
            sidebar_page_.anchor = other[start]->key;
        }
        const auto remaining = start < other.size() ? other.size() - start : 0;
        const uint32_t side_count = static_cast<uint32_t>(
            std::min<std::size_t>(side_limit, remaining));
        AddSeat(plan, *main, VideoSeatRole::Main,
                MainExtent(side_count != 0), focus.origin);
        const auto side_extent = SidebarExtent(side_count);
        for (uint32_t index = 0; index != side_count; ++index) {
            AddSeat(plan, *other[start + index], VideoSeatRole::Sidebar,
                    side_extent);
        }
    }

    plan.reason = ReasonForFocus(focus.origin, main->source);
}

void VideoDemandPolicy::AddSeat(VideoDemandPlan& plan,
                                const RemotePublicationInfo& publication,
                                VideoSeatRole role,
                                SeatExtent extent,
                                FocusOrigin origin) {
    if (plan.visible_seats.size() >= config_.video_budget) return;
    VideoSeat seat;
    seat.key = publication.key;
    seat.source = publication.source;
    seat.role = role;
    seat.reason = ReasonForFocus(origin, publication.source);
    if (!publication.subscription_allowed) {
        seat.reason = VideoDemandReason::PermissionDenied;
    } else if (publication.muted) {
        seat.reason = VideoDemandReason::Muted;
    }

    uint32_t max_width = 640;
    uint32_t max_height = 360;
    if (role == VideoSeatRole::Main) {
        if (publication.source == TrackSource::ScreenShareVideo) {
            max_width = 1920;
            max_height = 1080;
        } else {
            max_width = 1280;
            max_height = 720;
        }
    }
    seat.width = std::min(extent.width, max_width);
    seat.height = std::min(extent.height, max_height);
    if (seat.width != 0 && seat.height != 0) {
        if (seat.height <= 180) seat.quality = VideoQualityTier::P180;
        else if (seat.height <= 360) seat.quality = VideoQualityTier::P360;
        else if (seat.height <= 720) seat.quality = VideoQualityTier::P720;
        else seat.quality = VideoQualityTier::P1080;
    }

    switch (role) {
    case VideoSeatRole::Grid: seat.priority = 100; break;
    case VideoSeatRole::Sidebar: seat.priority = 120; break;
    case VideoSeatRole::PictureInPicture: seat.priority = 200; break;
    case VideoSeatRole::Main: seat.priority = 400; break;
    }
    if (publication.source == TrackSource::ScreenShareVideo) seat.priority += 50;
    if (origin == FocusOrigin::Pinned) seat.priority = 500;

    plan.visible_seats.push_back(seat);
    if (IsDemandable(publication) && seat.width != 0 && seat.height != 0 &&
        plan.selected_video.size() < config_.video_budget &&
        !Contains(plan.selected_video, publication.key)) {
        plan.selected_video.push_back(publication.key);
    }
}

VideoDemandPolicy::SeatExtent VideoDemandPolicy::StageExtent() const {
    const double dpr = std::isfinite(viewport_.device_pixel_ratio) &&
            viewport_.device_pixel_ratio > 0.0
        ? viewport_.device_pixel_ratio : 1.0;
    const auto scale = [dpr](int value) -> uint32_t {
        if (value <= 0) return 0;
        const double scaled = static_cast<double>(value) * dpr;
        return static_cast<uint32_t>(std::min<double>(
            std::lround(scaled), std::numeric_limits<uint32_t>::max()));
    };
    return {scale(viewport_.stage_rect.width),
            scale(viewport_.stage_rect.height)};
}

VideoDemandPolicy::SeatExtent VideoDemandPolicy::GridExtent(
    uint32_t page_size) const {
    const auto stage = StageExtent();
    uint32_t columns = 1;
    uint32_t rows = 1;
    if (page_size == 2) {
        columns = 2;
    } else if (page_size <= 4) {
        columns = rows = 2;
    } else if (page_size <= 9) {
        columns = rows = 3;
    } else {
        columns = rows = 4;
    }
    return {stage.width / columns, stage.height / rows};
}

VideoDemandPolicy::SeatExtent VideoDemandPolicy::MainExtent(
    bool has_sidebar) const {
    const auto stage = StageExtent();
    return {has_sidebar ? stage.width * 3 / 4 : stage.width, stage.height};
}

VideoDemandPolicy::SeatExtent VideoDemandPolicy::SidebarExtent(
    uint32_t visible_count) const {
    const auto stage = StageExtent();
    if (visible_count == 0) return {};
    return {std::max<uint32_t>(1, stage.width / 4),
            std::max<uint32_t>(1, stage.height / visible_count)};
}

VideoDemandPolicy::SeatExtent VideoDemandPolicy::PictureInPictureExtent() const {
    const auto stage = StageExtent();
    return {std::max<uint32_t>(1, stage.width / 3),
            std::max<uint32_t>(1, stage.height / 3)};
}

uint32_t VideoDemandPolicy::NormalizePageSize(uint32_t requested,
                                              uint32_t default_size) {
    if (requested == 4 || requested == 9 || requested == 16) return requested;
    return default_size == 4 || default_size == 9 || default_size == 16
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
        left.mode != right.mode || left.stage_content != right.stage_content ||
        left.page != right.page || left.page_size != right.page_size ||
        left.page_count != right.page_count ||
        !SameOptionalKey(left.focused, right.focused) ||
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
            lhs.role != rhs.role ||
            lhs.width != rhs.width || lhs.height != rhs.height ||
            lhs.quality != rhs.quality || lhs.priority != rhs.priority ||
            lhs.reason != rhs.reason) {
            return false;
        }
    }
    return true;
}

} // namespace livekit
