#include "adaptive_stream_manager.h"
#include <iostream>

namespace livekit {

AdaptiveStreamManager& AdaptiveStreamManager::Instance() {
    static AdaptiveStreamManager instance;
    return instance;
}

void AdaptiveStreamManager::RegisterTrack(std::shared_ptr<RemoteTrackPublication> track_pub) {
    if (!track_pub) return;
    std::lock_guard<std::mutex> lock(mutex_);
    tracks_[track_pub->sid()] = track_pub;
    std::cout << "[ADAPTIVE STREAM MANAGER] Registered Track: " << track_pub->sid() << std::endl;
}

void AdaptiveStreamManager::UnregisterTrack(const std::string& track_sid) {
    std::lock_guard<std::mutex> lock(mutex_);
    tracks_.erase(track_sid);
    std::cout << "[ADAPTIVE STREAM MANAGER] Unregistered Track: " << track_sid << std::endl;
}

void AdaptiveStreamManager::UpdateTrackDimensions(const std::string& track_sid, uint32_t width, uint32_t height) {
    std::shared_ptr<RemoteTrackPublication> pub;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tracks_.find(track_sid);
        if (it != tracks_.end()) {
            pub = it->second;
        }
    }

    if (pub) {
        pub->SetVideoDimensions(width, height);
    }
}

void AdaptiveStreamManager::SetTrackVisibility(const std::string& track_sid, bool visible) {
    std::shared_ptr<RemoteTrackPublication> pub;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tracks_.find(track_sid);
        if (it != tracks_.end()) {
            pub = it->second;
        }
    }

    if (pub) {
        pub->SetEnabled(visible);
    }
}

void AdaptiveStreamManager::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    tracks_.clear();
}

} // namespace livekit
