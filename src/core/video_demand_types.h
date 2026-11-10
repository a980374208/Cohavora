#pragma once

#include "participant_event.h"
#include "video_capacity.h"
#include "video_resolution_match.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace livekit {

enum class SpeakerLayout {
    Right,
    Top,
    TopLeft,
    BottomLeft,
    Floating,
};

enum class VideoLayoutMode {
    Auto,
    Grid,
    Speaker,
    PictureInPicture,
};

enum class StageContent {
    Video,
    Whiteboard,
};

enum class VideoSeatRole {
    Grid,
    Main,
    Sidebar,
    PictureInPicture,
};

enum class VideoDemandReason {
    Visible,
    Pinned,
    ActiveSpeaker,
    ScreenShare,
    Hidden,
    Whiteboard,
    PermissionDenied,
    Muted,
    NoCompatibleLayer,
};

enum class VideoQualityTier {
    None,
    P180,
    P360,
    P720,
    P1080,
    P1440,
    P2160,
};

struct LogicalViewport {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool operator==(const LogicalViewport&) const = default;
};

// Local previews are layout targets, never remote subscription TrackKeys.
enum class LocalVideoTarget {
    Camera,
    ScreenShare,
};

// A second presentation surface in the same meeting, never a second session.
struct DetachedShareViewport {
    uint64_t surface_id = 0;
    std::optional<TrackKey> track;
    bool local = false;
    LogicalViewport rect;
    double device_pixel_ratio = 1.0;
    bool visible = false;
    bool minimized = false;

    bool operator==(const DetachedShareViewport&) const = default;
};

struct ViewportIntent {
    uint64_t coordinator_session = 0;
    uint64_t view_revision = 0;
    uint64_t catalog_revision = 0;
    VideoLayoutMode mode = VideoLayoutMode::Auto;
    VideoResolutionMatch resolution_match = VideoResolutionMatch::Auto;
    SpeakerLayout speaker_layout = SpeakerLayout::Right;
    bool speaking_only = false;
    uint32_t page = 0;
    uint32_t page_size = 9;
    std::optional<TrackKey> page_anchor;
    std::optional<TrackKey> pinned;
    // Mutually exclusive with pinned; a local target takes precedence.
    std::optional<LocalVideoTarget> pinned_local;
    std::optional<TrackKey> selected_share;
    StageContent stage_content = StageContent::Video;
    // Full available area before the optional paging row is reserved. A zero
    // row height preserves callers that already supply the final video area.
    LogicalViewport stage_rect;
    int paging_bar_height = 0;
    double device_pixel_ratio = 1.0;
    bool remote_video_enabled = true;
    bool window_visible = true;
    bool minimized = false;
    // Local previews consume grid seats on the first page only.
    bool local_participant_present = false;
    bool local_screen_share_present = false;
    std::optional<DetachedShareViewport> detached_share;
};

struct VideoSeat {
    // A participant without video still occupies a display seat. Its key keeps
    // the participant lifetime, but has no publication and must not request media.
    TrackKey key;
    TrackSource source = TrackSource::Unknown;
    VideoSeatRole role = VideoSeatRole::Grid;
    LogicalViewport rect;
    uint32_t width = 0;
    uint32_t height = 0;
    VideoQualityTier quality = VideoQualityTier::None;
    // Layout pixel demand and selected transport layer are deliberately separate.
    uint32_t subscription_width = 0;
    uint32_t subscription_height = 0;
    std::optional<PublishedVideoQuality> selected_layer_quality;
    uint32_t priority = 0;
    VideoDemandReason reason = VideoDemandReason::Visible;
    TrackPublication::SubscriptionError subscription_error =
        TrackPublication::SubscriptionError::None;

    bool IsParticipantPlaceholder() const { return key.publication_sid.empty(); }
    bool operator==(const VideoSeat&) const = default;
};

struct VideoDemandPlan {
    uint64_t coordinator_session = 0;
    uint64_t view_revision = 0;
    uint64_t policy_revision = 0;
    uint64_t catalog_revision = 0;
    VideoLayoutMode requested_mode = VideoLayoutMode::Auto;
    VideoLayoutMode mode = VideoLayoutMode::Auto;
    SpeakerLayout speaker_layout = SpeakerLayout::Right;
    bool speaking_only = false;
    StageContent stage_content = StageContent::Video;
    uint32_t page = 0;
    uint32_t page_size = 0;
    uint32_t page_count = 1;
    std::optional<TrackKey> focused;
    std::optional<LocalVideoTarget> focused_local;
    // Validated pin, including while the stage is hidden. UI decorations use
    // this accepted state rather than the pending viewport request.
    std::optional<TrackKey> pinned;
    std::optional<LocalVideoTarget> pinned_local;
    std::optional<TrackKey> stable_speaker;
    std::vector<VideoSeat> visible_seats;
    std::vector<TrackKey> selected_video;
    std::vector<TrackKey> selected_audio;
    VideoDemandReason reason = VideoDemandReason::Hidden;
    bool show_local_participant = false;
    bool show_local_screen_share = false;
    std::optional<LogicalViewport> local_participant_rect;
    std::optional<LogicalViewport> local_screen_share_rect;
    // Main seats remain distinct from the independently visible shared surface.
    uint64_t detached_surface_id = 0;
    std::optional<VideoSeat> detached_share;
    bool detached_local = false;
    bool show_detached_local = false;
    uint32_t effective_main_page_size = 0;

    const VideoSeat* FindMediaSeat(const TrackKey& key) const {
        if (detached_share && detached_share->key == key) return &*detached_share;
        for (const auto& seat : visible_seats) if (seat.key == key) return &seat;
        return nullptr;
    }
};

struct RemoteTrackDemand {
    TrackKey key;
    uint64_t policy_revision = 0;
    bool subscribed = false;
    bool enabled = false;
    uint32_t width = 0;
    uint32_t height = 0;
    VideoQualityTier quality = VideoQualityTier::None;
    std::optional<PublishedVideoQuality> selected_layer_quality;
    std::optional<uint32_t> max_fps;
    uint32_t priority = 0;
};

struct RemoteMediaPlan {
    uint64_t coordinator_session = 0;
    uint64_t native_room_generation = 0;
    uint64_t catalog_revision = 0;
    uint64_t policy_revision = 0;
    uint64_t recovery_epoch = 0;
    std::string recovery_token;
    std::vector<TrackKey> known_publications;
    std::vector<RemoteTrackDemand> video;
    std::vector<TrackKey> audio;
};

struct RemoteMediaRecoveryRequest {
    uint64_t coordinator_session = 0;
    uint64_t native_room_generation = 0;
    uint64_t catalog_revision = 0;
    uint64_t recovery_epoch = 0;
    std::string recovery_token;
};

enum class ControlRejectionReason {
    None,
    StaleSession,
    StaleRoomGeneration,
    StaleCatalog,
    StalePolicy,
    MissingPublication,
    PermissionDenied,
    Retired,
    InvalidDemand,
    StaleRecovery,
};

struct ControlApplyResult {
    uint64_t coordinator_session = 0;
    uint64_t native_room_generation = 0;
    uint64_t catalog_revision = 0;
    uint64_t policy_revision = 0;
    uint64_t recovery_epoch = 0;
    std::string recovery_token;
    bool accepted = false;
    bool pending_reconnect = false;
    bool pending_send = false;
    std::vector<TrackKey> rejected;
    ControlRejectionReason reason = ControlRejectionReason::None;
};

struct VideoRenderSelectionObservation {
    uint64_t coordinator_session = 0;
    uint64_t native_room_generation = 0;
    uint64_t catalog_revision = 0;
    uint64_t policy_revision = 0;
    std::vector<TrackKey> actual_video;
    std::vector<TrackKey> bound_video;
};

} // namespace livekit
