#pragma once

#include <memory>
#include <string>
#include "track.h"
#include "video_source.h"
#include "api/media_stream_interface.h"

namespace livekit {

class LocalVideoTrack : public Track {
public:
    static std::shared_ptr<LocalVideoTrack> createLocalVideoTrack(const std::string& name,
                                                                  const std::shared_ptr<VideoSource>& source,
                                                                  TrackSource source_type = TrackSource::Camera,
                                                                  const VideoPublishOptions& options = VideoPublishOptions());

    LocalVideoTrack(const std::string& sid, const std::string& name, std::shared_ptr<VideoSource> source,
                    TrackSource source_type = TrackSource::Camera,
                    const VideoPublishOptions& options = VideoPublishOptions());
    virtual ~LocalVideoTrack() = default;

    std::shared_ptr<VideoSource> source() const { return source_; }

    void set_publish_options(const VideoPublishOptions& options) { publish_options_ = options; }
    VideoPublishOptions publish_options() const { return publish_options_; }

    void mute() { set_muted(true); }
    void unmute() { set_muted(false); }

    static VideoPublishOptions ComputeSimulcastOptions(int width, int height, const VideoPublishOptions& input_options);
    static VideoPublishOptions ComputeMultiCodecSimulcastOptions(int width, int height, const VideoPublishOptions& input_options);
    static VideoPublishOptions DefaultVp8SimulcastOptions(int width, int height);

private:
    std::shared_ptr<VideoSource> source_;
    webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> rtc_source_;
    VideoPublishOptions publish_options_;
};

} // namespace livekit
