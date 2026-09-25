#pragma once

#include "publication_catalog.h"
#include "video_demand_types.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace livekit {

struct VideoDemandPolicyConfig {
    uint32_t video_budget = 16;
    uint32_t default_grid_page_size = 9;
    uint32_t sidebar_limit = 4;
    std::chrono::milliseconds speaker_candidate_delay{1000};
    std::chrono::milliseconds focus_minimum_residence{3000};
};

class VideoDemandPolicy final {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    explicit VideoDemandPolicy(
        uint64_t coordinator_session = 0,
        VideoDemandPolicyConfig config = {});

    bool UpdateCatalog(const PublicationCatalogSnapshot& snapshot);
    bool UpdateViewport(const ViewportIntent& intent, TimePoint now);
    bool UpdateSpeakers(const std::vector<ActiveSpeakerInfo>& speakers,
                        TimePoint now);
    const VideoDemandPlan& Reconcile(TimePoint now);
    std::optional<TimePoint> NextReconcileAt(TimePoint now) const;
    void Retire();

    bool accepting() const noexcept { return accepting_; }
    const VideoDemandPlan& plan() const noexcept { return plan_; }

private:
    enum class FocusOrigin {
        None,
        Pinned,
        SelectedShare,
        AutoShare,
        ActiveSpeaker,
        Fallback,
    };

    struct PageState {
        uint32_t requested_page = 0;
        uint32_t page_size = 0;
        std::optional<TrackKey> anchor;
    };

    struct SeatExtent {
        uint32_t width = 0;
        uint32_t height = 0;
    };

    struct FocusChoice {
        std::optional<TrackKey> key;
        FocusOrigin origin = FocusOrigin::None;
    };

    struct SeatCandidate {
        TrackKey key;
        TrackSource source = TrackSource::Unknown;
        const RemotePublicationInfo* publication = nullptr;
    };

    const RemotePublicationInfo* Find(const TrackKey& key) const;
    std::vector<const RemotePublicationInfo*> OrderedVideo() const;
    std::vector<SeatCandidate> OrderedSeats() const;
    std::vector<TrackKey> SelectedAudio() const;
    std::optional<TrackKey> ResolveSpeakerCamera() const;
    std::optional<TrackKey> ResolveIntentTrack(
        const std::optional<TrackKey>& key,
        std::optional<TrackSource> required_source = std::nullopt) const;
    std::optional<TrackKey> ResolveIntentSeat(const std::optional<TrackKey>& key) const;
    bool IsAutoShare(const TrackKey& key) const;
    bool IsDemandable(const RemotePublicationInfo& publication) const;
    void AdvanceSpeaker(TimePoint now);
    void RefreshAutoShare();
    FocusChoice ChooseFocus(const std::vector<SeatCandidate>& videos,
                            bool allow_auto_share,
                            TimePoint now);
    VideoDemandPlan BuildPlan(TimePoint now);
    void BuildGrid(VideoDemandPlan& plan,
                   const std::vector<SeatCandidate>& videos,
                   uint32_t page_size,
                   bool use_paging);
    void BuildFocused(VideoDemandPlan& plan,
                      const std::vector<SeatCandidate>& videos,
                      VideoLayoutMode mode,
                      bool allow_auto_share,
                      TimePoint now);
    void AddSeat(VideoDemandPlan& plan,
                 const SeatCandidate& candidate,
                 VideoSeatRole role,
                 SeatExtent extent,
                 FocusOrigin origin = FocusOrigin::None);
    SeatExtent StageExtent() const;
    SeatExtent GridExtent(uint32_t page_size) const;
    SeatExtent MainExtent(bool has_sidebar) const;
    SeatExtent SidebarExtent(uint32_t visible_count) const;
    SeatExtent PictureInPictureExtent() const;
    static uint32_t NormalizePageSize(uint32_t requested,
                                      uint32_t default_size);
    static VideoDemandReason ReasonForFocus(FocusOrigin origin,
                                            TrackSource source);
    static bool SamePlan(const VideoDemandPlan& left,
                         const VideoDemandPlan& right);

    uint64_t coordinator_session_ = 0;
    VideoDemandPolicyConfig config_;
    bool accepting_ = true;
    std::optional<PublicationCatalogSnapshot> catalog_;
    ViewportIntent viewport_;
    bool has_viewport_ = false;
    std::vector<ActiveSpeakerInfo> speakers_;
    std::optional<TrackKey> speaker_candidate_;
    std::optional<TimePoint> speaker_candidate_since_;
    std::optional<TrackKey> stable_speaker_;
    std::optional<TrackKey> current_auto_share_;
    std::optional<TrackKey> current_focus_;
    FocusOrigin current_focus_origin_ = FocusOrigin::None;
    std::optional<TimePoint> focus_since_;
    PageState grid_page_;
    PageState sidebar_page_;
    VideoDemandPlan plan_;
};

} // namespace livekit
