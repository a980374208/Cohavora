#pragma once

#include <memory>
#include <string>
#include "track.h"
#include "video_source.h"

namespace livekit {

class LocalVideoTrack : public Track {
public:
    static std::shared_ptr<LocalVideoTrack> createLocalVideoTrack(const std::string& name,
                                                                  const std::shared_ptr<VideoSource>& source);

    LocalVideoTrack(const std::string& sid, const std::string& name, std::shared_ptr<VideoSource> source);
    virtual ~LocalVideoTrack() = default;

    std::shared_ptr<VideoSource> source() const { return source_; }

    void mute() { set_muted(true); }
    void unmute() { set_muted(false); }

private:
    std::shared_ptr<VideoSource> source_;
};

} // namespace livekit
