#pragma once

#include <string>
#include <memory>
#include <mutex>
#include "livekit_rtc.pb.h"
#include "signal_client.h"

namespace livekit {

class RemoteTrackPublication {
public:
    RemoteTrackPublication(std::string sid,
                           std::string name,
                           proto::TrackType type,
                           std::shared_ptr<SignalClient> signal_client);

    std::string sid() const { return sid_; }
    std::string name() const { return name_; }
    proto::TrackType type() const { return type_; }

    bool is_subscribed() const { return subscribed_; }
    bool is_enabled() const { return enabled_; }
    proto::VideoQuality current_quality() const { return current_quality_; }
    uint32_t current_width() const { return width_; }
    uint32_t current_height() const { return height_; }

    // Adaptive Stream & Dynamic Control API
    void SetSubscribed(bool subscribed);
    void SetVideoQuality(proto::VideoQuality quality);
    void SetVideoDimensions(uint32_t width, uint32_t height);
    void SetEnabled(bool enabled);

private:
    void SendTrackSettingsUpdate();

private:
    std::string sid_;
    std::string name_;
    proto::TrackType type_;
    std::shared_ptr<SignalClient> signal_client_;

    mutable std::mutex mutex_;
    bool subscribed_{true};
    bool enabled_{true};
    proto::VideoQuality current_quality_{proto::VideoQuality::HIGH};
    uint32_t width_{0};
    uint32_t height_{0};
};

} // namespace livekit
