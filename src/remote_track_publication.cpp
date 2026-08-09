#include "remote_track_publication.h"
#include <iostream>
#include <algorithm>

namespace livekit {

RemoteTrackPublication::RemoteTrackPublication(std::string sid,
                                               std::string name,
                                               proto::TrackType type,
                                               std::shared_ptr<SignalClient> signal_client)
    : sid_(std::move(sid)),
      name_(std::move(name)),
      type_(type),
      signal_client_(std::move(signal_client)) {}

void RemoteTrackPublication::SetSubscribed(bool subscribed) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (subscribed_ == subscribed) return;
    subscribed_ = subscribed;

    if (signal_client_) {
        signal_client_->SendUpdateSubscription({sid_}, subscribed_);
    }
}

void RemoteTrackPublication::SetVideoQuality(proto::VideoQuality quality) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_quality_ == quality) return;
    current_quality_ = quality;

    SendTrackSettingsUpdate();
}

void RemoteTrackPublication::SetVideoDimensions(uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (width_ == width && height_ == height) return;
    width_ = width;
    height_ = height;

    // Automatically compute appropriate VideoQuality based on dimensions
    uint32_t max_dim = std::max(width, height);
    if (max_dim <= 360) {
        current_quality_ = proto::VideoQuality::LOW;
    } else if (max_dim <= 720) {
        current_quality_ = proto::VideoQuality::MEDIUM;
    } else {
        current_quality_ = proto::VideoQuality::HIGH;
    }

    SendTrackSettingsUpdate();
}

void RemoteTrackPublication::SetEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enabled_ == enabled) return;
    enabled_ = enabled;

    SendTrackSettingsUpdate();
}

void RemoteTrackPublication::SendTrackSettingsUpdate() {
    if (!signal_client_) return;
    signal_client_->SendUpdateTrackSettings(
        sid_,
        !enabled_, // disabled flag
        current_quality_,
        width_,
        height_
    );
}

} // namespace livekit
