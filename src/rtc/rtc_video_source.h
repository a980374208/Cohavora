#pragma once

#include <memory>
#include "media/base/adapted_video_track_source.h"
#include "api/video/video_frame.h"
#include "api/video/i420_buffer.h"
#include "rtc_base/ref_counted_object.h"
#include "video_source.h"

namespace livekit {

class RtcVideoSource : public webrtc::AdaptedVideoTrackSource {
public:
    static webrtc::scoped_refptr<RtcVideoSource> Create(std::shared_ptr<VideoSource> source, bool screencast = false);

    explicit RtcVideoSource(std::shared_ptr<VideoSource> source, bool screencast = false);
    ~RtcVideoSource() override;

    // webrtc::MediaSourceInterface impl
    SourceState state() const override { return kLive; }
    bool remote() const override { return false; }
    bool is_screencast() const override { return screencast_; }
    std::optional<bool> needs_denoising() const override { return false; }

private:
    void OnVideoFrame(const VideoFrame& frame, const VideoCaptureOptions& options);

    std::shared_ptr<VideoSource> lk_source_;
    const bool screencast_;
    std::shared_ptr<VideoSource::Subscription> subscription_;
};

} // namespace livekit
