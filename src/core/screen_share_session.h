#pragma once

#include <asio.hpp>
#include "desktop_capture.h"
#include "local_video_track.h"
#include "render/video_render_router.h"
#include <functional>
#include <memory>
#include <optional>

namespace livekit {
class Room;

enum class ScreenShareState { Idle, Starting, Active, Stopping, Failed, StopFailed };
enum class ScreenShareError { None, Capture, Publish, Unpublish };
struct ScreenShareSnapshot {
    ScreenShareState state = ScreenShareState::Idle;
    ScreenShareError error = ScreenShareError::None;
    // Bounded frame mailbox only; the UI never owns capture/Track resources.
    std::shared_ptr<render::VideoRenderRouter> preview;
    std::string source_title;
    DesktopSourceKind source_kind = DesktopSourceKind::Window;
    std::optional<ScreenBinding> annotation_binding;
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
        std::function<std::optional<ScreenBinding>(
            const DesktopSource &, std::uint64_t, std::string)> resolve_screen_binding =
                ResolveScreenBinding;
        std::function<bool(const ScreenBinding &)> validate_screen_binding =
            ValidateScreenBinding;
        std::chrono::milliseconds first_frame_timeout{5000};
        std::chrono::milliseconds geometry_check_interval{500};
    };
    using Observer = std::function<void(ScreenShareSnapshot)>;
    static Backend ForRoom(const std::shared_ptr<Room>& room);
    ScreenShareSession(asio::any_io_executor strand, Backend backend, Observer observer);
    ~ScreenShareSession();
    void Start(DesktopSource source);
    void Stop();
    // Revokes capture synchronously on the strand before Room disconnect and
    // executor teardown. Room disconnect owns the network rollback on leave.
    void Shutdown();
    void SetTransportReady(bool ready) { transport_ready_ = ready; }
    ScreenShareSnapshot snapshot() const { return snapshot_; } // strand only

private:
    struct Run;
    static asio::awaitable<void> Drive(std::shared_ptr<ScreenShareSession> self,
                                       std::shared_ptr<Run> run, DesktopSource source);
    void SetState(ScreenShareState state, ScreenShareError error = ScreenShareError::None);
    void StopFrames(const std::shared_ptr<Run>& run);
    asio::any_io_executor strand_;
    Backend backend_;
    Observer observer_;
    ScreenShareSnapshot snapshot_;
    std::shared_ptr<Run> run_;
    uint64_t next_run_ = 0;
    bool closed_ = false;
    bool transport_ready_ = true;
};
} // namespace livekit
