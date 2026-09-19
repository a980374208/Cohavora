#pragma once

#include <string>
#include <memory>
#include <mutex>
#include <functional>
#include <optional>
#include <cstdint>
#include "track.h"
#include "livekit_rtc.pb.h"

namespace livekit {

// The publication describes requested remote state but deliberately does not
// know SignalClient. Room owns session validation and serializes signaling.
struct RemotePublicationControlRequest {
    enum class Kind {
        Subscription,
        Settings,
    };

    Kind kind = Kind::Subscription;
    uint64_t sequence = 0;
    std::optional<bool> subscribed;
    std::optional<bool> enabled;
    std::optional<proto::VideoQuality> quality;
    std::optional<uint32_t> width;
    std::optional<uint32_t> height;
    std::optional<uint32_t> priority;
};

enum class RemotePublicationControlDispatch {
    Rejected,
    Queued,
    Committed,
};

class RemoteTrackPublication final : public TrackPublication {
public:
    using ControlHandler = std::function<RemotePublicationControlDispatch(
        RemoteTrackPublication*, const RemotePublicationControlRequest&)>;
    using MediaDetachHandler = std::function<void(RemoteTrackPublication*, bool notify_listener)>;

    // Production construction: this exact object goes into
    // RemoteParticipant::tracks(). The handler holds only weak Room state.
    RemoteTrackPublication(std::shared_ptr<Track> track,
                           std::string sid,
                           std::string name,
                           proto::TrackType type,
                           uint64_t session_generation,
                           ControlHandler controller = {},
                           bool initially_subscribed = true);

    // Compatibility constructor for synthetic/test-only tiles. It creates a
    // real TrackPublication base object but cannot control a live Room.
    RemoteTrackPublication(std::string sid,
                           std::string name,
                           proto::TrackType type,
                           std::nullptr_t = nullptr);

    proto::TrackType type() const { return type_; }
    uint64_t session_generation() const { return session_generation_; }

    bool is_subscribed() const;
    bool is_enabled() const;
    proto::VideoQuality current_quality() const;
    uint32_t current_width() const;
    uint32_t current_height() const;
    uint32_t priority() const;
    bool has_media_binding() const;
    std::string media_track_id() const;

    // `true` means the operation was accepted for the current publication and
    // session. It does not pretend a fire-and-forget signal was acknowledged.
    bool SetSubscribed(bool subscribed);
    bool SetVideoQuality(proto::VideoQuality quality);
    bool SetVideoDimensions(uint32_t width, uint32_t height);
    bool SetEnabled(bool enabled);
    bool SetPriority(uint32_t priority);

    // Room calls these after revalidating map ownership and generation on its
    // executor. They are public to keep this type independent of room.h.
    void CommitControl(const RemotePublicationControlRequest& request);
    void SetMediaBinding(std::string rtc_track_id, MediaDetachHandler detach_handler);
    void ClearMediaBinding();
    void DetachMedia(bool notify_listener);

private:
    bool DispatchControl(RemotePublicationControlRequest request);
    static proto::VideoQuality QualityForDimensions(uint32_t width, uint32_t height);

    proto::TrackType type_;
    uint64_t session_generation_ = 0;
    ControlHandler control_handler_;

    mutable std::mutex mutex_;
    bool subscribed_{true};
    bool enabled_{true};
    proto::VideoQuality current_quality_{proto::VideoQuality::HIGH};
    uint32_t width_{0};
    uint32_t height_{0};
    uint32_t priority_{0};
    uint64_t next_control_sequence_{1};
    uint64_t last_subscription_sequence_{0};
    uint64_t last_settings_sequence_{0};
    std::string media_track_id_;
    MediaDetachHandler media_detach_handler_;
};

} // namespace livekit
