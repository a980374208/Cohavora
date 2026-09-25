#pragma once

#include <memory>
#include <mutex>
#include <string>
#include "track.h"
#include "video_source.h"
#include "api/media_stream_interface.h"

namespace livekit {

class RtcVideoSource;
namespace telemetry { struct LocalVideoActivityProbe; }

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
    void set_publish_telemetry_probe(
        std::shared_ptr<telemetry::LocalVideoActivityProbe> probe);

    void set_publish_options(const VideoPublishOptions& options);
    VideoPublishOptions publish_options() const { return publish_options_; }
    VideoPublishOptions requested_publish_options() const { return requested_publish_options_; }

    void mute() { set_muted(true); }
    void unmute() { set_muted(false); }

    static VideoPublishOptions ComputeSimulcastOptions(int width, int height, const VideoPublishOptions& input_options);
    static VideoPublishOptions ComputeMultiCodecSimulcastOptions(int width, int height, const VideoPublishOptions& input_options);
    static ResolvedVideoPublishPlan ResolvePublishPlan(
        int width,
        int height,
        const VideoPublishOptions& requested_options,
        const std::vector<std::string>& local_sender_codecs,
        const std::vector<std::string>& server_enabled_codecs);
    static std::vector<VideoLayerSetting> ComputeSignalLayers(
        const SimulcastCodecSpec& spec);
    static int SpatialLayersFromScalabilityMode(const std::string& mode);
    static VideoPublishOptions DefaultVp8SimulcastOptions(int width, int height);

private:
    std::shared_ptr<VideoSource> source_;
    mutable std::mutex rtc_source_mutex_;
    webrtc::scoped_refptr<RtcVideoSource> rtc_source_;
    std::shared_ptr<telemetry::LocalVideoActivityProbe> publish_telemetry_probe_;
    VideoPublishOptions requested_publish_options_;
    VideoPublishOptions publish_options_;
};

} // namespace livekit
