#pragma once

#include <memory>
#include <mutex>
#include <string>
#include "track.h"
#include "video_source.h"
#include "api/media_stream_interface.h"

namespace livekit {

class RtcVideoSource;

class LocalVideoTrack : public Track {
public:
    static std::shared_ptr<LocalVideoTrack> createLocalVideoTrack(const std::string& name,
                                                                  const std::shared_ptr<VideoSource>& source,
                                                                  TrackSource source_type = TrackSource::Camera,
                                                                  const VideoPublishOptions& options = VideoPublishOptions());

    LocalVideoTrack(const std::string& sid, const std::string& name, std::shared_ptr<VideoSource> source,
                    TrackSource source_type = TrackSource::Camera,
                    const VideoPublishOptions& options = VideoPublishOptions());
    ~LocalVideoTrack() override;

    std::shared_ptr<VideoSource> source() const { return source_; }
    VideoFrameDiagnostics frame_diagnostics() const noexcept;
    // Bind the exact native bridge at normal or delayed RTC-track creation.
    void set_rtc_source_for_diagnostics(webrtc::scoped_refptr<RtcVideoSource> source);

    void set_publish_options(const VideoPublishOptions& options) { publish_options_ = options; }
    VideoPublishOptions publish_options() const { return publish_options_; }

    void mute() { set_muted(true); }
    void unmute() { set_muted(false); }

    static VideoPublishOptions ComputeSimulcastOptions(int width, int height, const VideoPublishOptions& input_options);
    static VideoPublishOptions ComputeMultiCodecSimulcastOptions(int width, int height, const VideoPublishOptions& input_options);
    static VideoPublishOptions DefaultVp8SimulcastOptions(int width, int height);

private:
    std::shared_ptr<VideoSource> source_;
    mutable std::mutex rtc_source_mutex_;
    webrtc::scoped_refptr<RtcVideoSource> rtc_source_;
    VideoPublishOptions publish_options_;
};

} // namespace livekit
