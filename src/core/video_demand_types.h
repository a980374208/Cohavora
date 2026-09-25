#pragma once

#include "participant_event.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace livekit {

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
};

enum class VideoQualityTier {
    None,
    P180,
    P360,
    P720,
    P1080,
};

struct LogicalViewport {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct ViewportIntent {
    uint64_t coordinator_session = 0;
    uint64_t view_revision = 0;
    uint64_t catalog_revision = 0;
    VideoLayoutMode mode = VideoLayoutMode::Auto;
    uint32_t page = 0;
    uint32_t page_size = 9;
    std::optional<TrackKey> page_anchor;
    std::optional<TrackKey> pinned;
    std::optional<TrackKey> selected_share;
    StageContent stage_content = StageContent::Video;
    LogicalViewport stage_rect;
    double device_pixel_ratio = 1.0;
    bool window_visible = true;
    bool minimized = false;
};

struct VideoSeat {
    // A participant without video still occupies a display seat. Its key keeps
    // the participant lifetime, but has no publication and must not request media.
    TrackKey key;
    TrackSource source = TrackSource::Unknown;
    VideoSeatRole role = VideoSeatRole::Grid;
    uint32_t width = 0;
    uint32_t height = 0;
    VideoQualityTier quality = VideoQualityTier::None;
    uint32_t priority = 0;
    VideoDemandReason reason = VideoDemandReason::Visible;
    TrackPublication::SubscriptionError subscription_error =
        TrackPublication::SubscriptionError::None;

    bool IsParticipantPlaceholder() const { return key.publication_sid.empty(); }
};

struct VideoDemandPlan {
    uint64_t coordinator_session = 0;
    uint64_t policy_revision = 0;
    uint64_t catalog_revision = 0;
    VideoLayoutMode requested_mode = VideoLayoutMode::Auto;
    VideoLayoutMode mode = VideoLayoutMode::Auto;
    StageContent stage_content = StageContent::Video;
    uint32_t page = 0;
    uint32_t page_size = 0;
    uint32_t page_count = 1;
    std::optional<TrackKey> focused;
    std::optional<TrackKey> stable_speaker;
    std::vector<VideoSeat> visible_seats;
    std::vector<TrackKey> selected_video;
    std::vector<TrackKey> selected_audio;
    VideoDemandReason reason = VideoDemandReason::Hidden;
};

struct RemoteTrackDemand {
    TrackKey key;
    uint64_t policy_revision = 0;
    bool subscribed = false;
    bool enabled = false;
    uint32_t width = 0;
    uint32_t height = 0;
    VideoQualityTier quality = VideoQualityTier::None;
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
