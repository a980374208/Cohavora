#include "remote_track_publication.h"

#include <algorithm>

namespace livekit {
namespace {

TrackKind TrackKindFromProto(proto::TrackType type) {
    return type == proto::TrackType::AUDIO ? TrackKind::Audio : TrackKind::Video;
}

} // namespace

RemoteTrackPublication::RemoteTrackPublication(std::shared_ptr<Track> track,
                                               std::string sid,
                                               std::string name,
                                               proto::TrackType type,
                                               uint64_t session_generation,
                                               ControlHandler controller)
    : TrackPublication(std::move(track), sid, name),
      type_(type),
      session_generation_(session_generation),
      control_handler_(std::move(controller)) {}

RemoteTrackPublication::RemoteTrackPublication(std::string sid,
                                               std::string name,
                                               proto::TrackType type,
                                               std::nullptr_t)
    : TrackPublication(std::make_shared<Track>(sid, name, TrackKindFromProto(type)), sid, name),
      type_(type) {}

bool RemoteTrackPublication::is_subscribed() const {
    std::lock_guard lock(mutex_);
    return subscribed_;
}

bool RemoteTrackPublication::is_enabled() const {
    std::lock_guard lock(mutex_);
    return enabled_;
}

proto::VideoQuality RemoteTrackPublication::current_quality() const {
    std::lock_guard lock(mutex_);
    return current_quality_;
}

uint32_t RemoteTrackPublication::current_width() const {
    std::lock_guard lock(mutex_);
    return width_;
}

uint32_t RemoteTrackPublication::current_height() const {
    std::lock_guard lock(mutex_);
    return height_;
}

uint32_t RemoteTrackPublication::priority() const {
    std::lock_guard lock(mutex_);
    return priority_;
}

bool RemoteTrackPublication::has_media_binding() const {
    std::lock_guard lock(mutex_);
    return !media_track_id_.empty();
}

std::string RemoteTrackPublication::media_track_id() const {
    std::lock_guard lock(mutex_);
    return media_track_id_;
}

bool RemoteTrackPublication::SetSubscribed(bool subscribed) {
    RemotePublicationControlRequest request;
    request.kind = RemotePublicationControlRequest::Kind::Subscription;
    request.subscribed = subscribed;
    return DispatchControl(std::move(request));
}

bool RemoteTrackPublication::SetVideoQuality(proto::VideoQuality quality) {
    RemotePublicationControlRequest request;
    request.kind = RemotePublicationControlRequest::Kind::Settings;
    request.quality = quality;
    return DispatchControl(std::move(request));
}

bool RemoteTrackPublication::SetVideoDimensions(uint32_t width, uint32_t height) {
    RemotePublicationControlRequest request;
    request.kind = RemotePublicationControlRequest::Kind::Settings;
    request.width = width;
    request.height = height;
    request.quality = QualityForDimensions(width, height);
    return DispatchControl(std::move(request));
}

bool RemoteTrackPublication::SetEnabled(bool enabled) {
    RemotePublicationControlRequest request;
    request.kind = RemotePublicationControlRequest::Kind::Settings;
    request.enabled = enabled;
    return DispatchControl(std::move(request));
}

bool RemoteTrackPublication::SetPriority(uint32_t priority) {
    RemotePublicationControlRequest request;
    request.kind = RemotePublicationControlRequest::Kind::Settings;
    request.priority = priority;
    return DispatchControl(std::move(request));
}

bool RemoteTrackPublication::DispatchControl(RemotePublicationControlRequest request) {
    ControlHandler controller;
    {
        std::lock_guard lock(mutex_);
        request.sequence = next_control_sequence_++;
        controller = control_handler_;
    }

    const auto outcome = controller
        ? controller(this, request)
        : RemotePublicationControlDispatch::Committed;
    if (outcome == RemotePublicationControlDispatch::Rejected) {
        return false;
    }
    if (outcome == RemotePublicationControlDispatch::Committed) {
        if (request.subscribed.has_value() && !*request.subscribed) {
            // The render/sink path is torn down before a successful
            // unsubscribe is committed, so late native frames cannot outlive
            // a hidden tile. Rejected controls leave media untouched.
            DetachMedia(/*notify_listener=*/true);
        }
        CommitControl(request);
    }
    return true;
}

void RemoteTrackPublication::CommitControl(const RemotePublicationControlRequest& request) {
    std::lock_guard lock(mutex_);
    if (request.subscribed.has_value()) subscribed_ = *request.subscribed;
    if (request.enabled.has_value()) enabled_ = *request.enabled;
    if (request.quality.has_value()) current_quality_ = *request.quality;
    if (request.width.has_value()) width_ = *request.width;
    if (request.height.has_value()) height_ = *request.height;
    if (request.priority.has_value()) priority_ = *request.priority;
}

void RemoteTrackPublication::SetMediaBinding(
    std::string rtc_track_id,
    MediaDetachHandler detach_handler) {
    std::lock_guard lock(mutex_);
    media_track_id_ = std::move(rtc_track_id);
    media_detach_handler_ = std::move(detach_handler);
}

void RemoteTrackPublication::ClearMediaBinding() {
    std::lock_guard lock(mutex_);
    media_track_id_.clear();
    media_detach_handler_ = {};
}

void RemoteTrackPublication::DetachMedia(bool notify_listener) {
    MediaDetachHandler detach_handler;
    {
        std::lock_guard lock(mutex_);
        detach_handler = media_detach_handler_;
    }
    if (detach_handler) {
        detach_handler(this, notify_listener);
    }
}

proto::VideoQuality RemoteTrackPublication::QualityForDimensions(uint32_t width,
                                                                  uint32_t height) {
    const uint32_t max_dim = std::max(width, height);
    if (max_dim <= 360) return proto::VideoQuality::LOW;
    if (max_dim <= 720) return proto::VideoQuality::MEDIUM;
    return proto::VideoQuality::HIGH;
}

} // namespace livekit
