#pragma once

#include <memory>
#include <vector>
#include <mutex>
#include <unordered_map>
#include "remote_track_publication.h"

namespace livekit {

class AdaptiveStreamManager {
public:
    static AdaptiveStreamManager& Instance();

    void RegisterTrack(std::shared_ptr<RemoteTrackPublication> track_pub);
    void UnregisterTrack(const std::string& track_sid);

    void UpdateTrackDimensions(const std::string& track_sid, uint32_t width, uint32_t height);
    void SetTrackVisibility(const std::string& track_sid, bool visible);

    void Clear();

private:
    AdaptiveStreamManager() = default;

private:
    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<RemoteTrackPublication>> tracks_;
};

} // namespace livekit
