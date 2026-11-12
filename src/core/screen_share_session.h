#pragma once

#include <asio.hpp>
#include "desktop_capture.h"
#include "local_video_track.h"
#include "local_audio_track.h"
#include "system_audio_capture.h"
#include "render/video_render_router.h"
#include <functional>
#include <memory>
#include <optional>

namespace livekit {
class Room;

enum class ScreenShareState { Idle, Starting, Active, Stopping, Failed, StopFailed, AwaitingPermission };
enum class ScreenShareError { None, Capture, Publish, Unpublish, PermissionDenied };
enum class ScreenShareQualityStatus { Applied, Pending, Rejected, MetadataPending, Degraded };
enum class ScreenShareAudioState { Disabled, Starting, Active, Stopping, Failed, StopFailed };
enum class ScreenShareAudioError { None, Unavailable, Capture, Publish, Unpublish };
struct ScreenShareAudioOptions {
    bool enabled = false;
    std::string output_id;
    std::string output_name;
};
struct ScreenShareCaptureResources {
    std::unique_ptr<IDesktopCapture> desktop;
    std::unique_ptr<ISystemAudioCapture> system_audio;
};
struct ScreenShareSnapshot {
    ScreenShareState state = ScreenShareState::Idle;
    ScreenShareError error = ScreenShareError::None;
    ScreenShareQuality requested_quality;
    std::optional<ScreenShareFrameProfile> applied_quality;
    ScreenShareQualityStatus quality_status = ScreenShareQualityStatus::Applied;
    // Bounded frame mailbox only; the UI never owns capture/Track resources.
    std::shared_ptr<render::VideoRenderRouter> preview;
    std::string source_title;
    DesktopSourceKind source_kind = DesktopSourceKind::Window;
    std::optional<ScreenBinding> annotation_binding;
    std::string track_sid;
    ScreenShareAudioState audio_state = ScreenShareAudioState::Disabled;
    ScreenShareAudioError audio_error = ScreenShareAudioError::None;
    bool audio_muted = false;
    std::string audio_output_name;
    std::string audio_track_sid;
    uint64_t run_id = 0;
    uint64_t audio_epoch = 0;
    std::string audio_output_id;
};

// Mirrors Flutter's serialized setSourceEnabled(screenShareVideo): capture,
// await publication, and removePublishedTrack on disable/track-ended. All
// control state belongs to the supplied session strand; callbacks only touch
// the per-run frame gate. No Qt or native capture resources cross into the UI.
class ScreenShareSession final : public std::enable_shared_from_this<ScreenShareSession> {
public:
    struct Backend {
        std::function<std::unique_ptr<IDesktopCapture>()> capture = CreateDesktopCapture;
        std::function<bool()> connected;
        std::function<asio::awaitable<void>(std::shared_ptr<LocalVideoTrack>)> publish;
        std::function<asio::awaitable<void>(std::shared_ptr<LocalVideoTrack>)> unpublish;
        std::function<bool()> audio_available = [] {
#if defined(WEBRTC_LINUX)
            return SystemAudioSharingAvailable();
#else
            return false;
#endif
        };
        std::function<std::unique_ptr<ISystemAudioCapture>()> audio_capture = []() -> std::unique_ptr<ISystemAudioCapture> {
#if defined(WEBRTC_LINUX)
            return CreateSystemAudioCapture();
#else
            return {};
#endif
        };
        std::function<asio::awaitable<void>(std::shared_ptr<LocalAudioTrack>)> publish_audio;
        std::function<asio::awaitable<void>(std::shared_ptr<LocalAudioTrack>)> unpublish_audio;
        std::function<void(std::shared_ptr<LocalAudioTrack>, bool)> mute_audio;
        std::function<asio::awaitable<void>(std::shared_ptr<LocalVideoTrack>, ScreenShareFrameProfile)> apply_quality;
        std::function<asio::awaitable<void>(std::shared_ptr<LocalVideoTrack>, ScreenShareFrameProfile)> sync_quality;
        std::function<std::optional<ScreenBinding>(
            const DesktopSource &, std::uint64_t, std::string)> resolve_screen_binding =
                ResolveScreenBinding;
        std::function<ScreenBindingStatus(const ScreenBinding &)> validate_screen_binding =
            CheckScreenBinding;
        // The application injects its native-cleanup queue so capture stop/join
        // precedes later Room/SDK retirement. The process-owned Asio executor
        // is the non-Qt fallback for standalone users.
        std::function<void(std::function<void()>)> dispatch_cleanup = [](std::function<void()> job) {
            asio::post(asio::system_executor(), std::move(job));
        };
        std::chrono::milliseconds first_frame_timeout{5000};
        std::chrono::milliseconds geometry_check_interval{500};
    };
    using Observer = std::function<void(ScreenShareSnapshot)>;
    static Backend ForRoom(const std::shared_ptr<Room>& room);
    ScreenShareSession(asio::any_io_executor strand, Backend backend, Observer observer,
                       std::shared_ptr<void> executor_lifetime = {});
    ~ScreenShareSession();
    void Start(DesktopSource source,
               VideoPublishOptions options = VideoPublishOptions(),
               ScreenShareQuality quality = {},
               ScreenShareAudioOptions audio = {});
    void SetQuality(ScreenShareQuality quality);
    // Explicitly starts/retries audio within an active video share. The former
    // audio driver, publication and native cleanup must have fully completed.
    void StartAudio(ScreenShareAudioOptions audio, uint64_t expected_run = 0,
                    std::optional<uint64_t> expected_audio_epoch = std::nullopt);
    void SetAudioMuted(bool muted, uint64_t expected_run = 0,
                       std::optional<uint64_t> expected_audio_epoch = std::nullopt);
    void StopAudio(uint64_t expected_run = 0,
                   std::optional<uint64_t> expected_audio_epoch = std::nullopt);
    void Stop();
    // Revokes capture synchronously on the strand and queues native stop/join.
    // Application cleanup must drain before Room/SDK/executor teardown.
    void Shutdown();
    // Strand-only revocation, including reclaiming queued/unstarted captures.
    // Both returned captures must be stopped/released by the same serialized
    // cleanup worker while the strand continues draining its coroutine.
    ScreenShareCaptureResources TakeCapturesForShutdown();
    void SetTransportReady(bool ready);
    ScreenShareSnapshot snapshot() const { return snapshot_; } // strand only

private:
    // Retained share owners may outlive SessionRuntime. Destroy their strand,
    // backend callbacks and runs before releasing the execution context.
    const std::shared_ptr<void> executor_lifetime_;
    struct Run;
    static asio::awaitable<void> Drive(std::shared_ptr<ScreenShareSession> self,
                                       std::shared_ptr<Run> run, DesktopSource source,
                                       VideoPublishOptions options);
    static asio::awaitable<void> ApplyQuality(std::shared_ptr<ScreenShareSession> self, std::shared_ptr<Run> run);
    static asio::awaitable<void> DriveAudio(std::shared_ptr<ScreenShareSession> self,
                                           std::shared_ptr<Run> run, uint64_t audio_epoch);
    void SetState(ScreenShareState state, ScreenShareError error = ScreenShareError::None);
    void StopFrames(const std::shared_ptr<Run>& run);
    void StopAudioFrames(const std::shared_ptr<Run>& run);
    void StartAudioDriver(const std::shared_ptr<Run>& run);
    asio::any_io_executor strand_;
    Backend backend_;
    Observer observer_;
    ScreenShareSnapshot snapshot_;
    std::shared_ptr<Run> run_;
    uint64_t next_run_ = 0;
    bool closed_ = false;
    bool transport_ready_ = true;
    uint64_t transport_revision_ = 0;
};
} // namespace livekit
