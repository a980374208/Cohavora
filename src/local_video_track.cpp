#include "local_video_track.h"
#include "webrtc_manager.h"
#include "rtc_video_source.h"

namespace livekit {

LocalVideoTrack::LocalVideoTrack(const std::string& sid, const std::string& name, std::shared_ptr<VideoSource> source)
    : Track(sid, name, TrackKind::Video), source_(source) {
    if (source_) {
        publish_options_ = DefaultVp8SimulcastOptions(source_->width(), source_->height());
    }
}

VideoPublishOptions LocalVideoTrack::DefaultVp8SimulcastOptions(int width, int height) {
    VideoPublishOptions opts;
    opts.simulcast = true;
    opts.scalability_mode = "";

    if (width <= 0 || height <= 0) {
        width = 1280;
        height = 720;
    }

    // Cap max Simulcast publish layer 'f' at 720p (1280x720) for standard WebRTC VP8 software encoder
    int target_f_w = 1280;
    int target_f_h = 720;

    if (width < 1280) {
        target_f_w = width;
        target_f_h = height;
    } else {
        target_f_h = (height * 1280) / width;
    }

    double scale_f = static_cast<double>(width) / target_f_w;

    if (target_f_w >= 960) {
        // 3-layer VP8 Simulcast (aligned with official client-sdk H720, H360, H180 presets):
        // f: 1280x720 @1.7Mbps, 30fps
        // h: 640x360 @450kbps, 20fps
        // q: 320x180 @160kbps, 15fps
        VideoLayerSetting f_layer{target_f_w, target_f_h, 1700000, 30, "f", scale_f * 1.0};
        VideoLayerSetting h_layer{target_f_w / 2, target_f_h / 2, 450000, 20, "h", scale_f * 2.0};
        VideoLayerSetting q_layer{target_f_w / 4, target_f_h / 4, 160000, 15, "q", scale_f * 4.0};
        opts.layers = {f_layer, h_layer, q_layer};
    } else if (target_f_w >= 480) {
        // 2-layer VP8 Simulcast
        VideoLayerSetting f_layer{target_f_w, target_f_h, 800000, 25, "f", scale_f * 1.0};
        VideoLayerSetting q_layer{target_f_w / 2, target_f_h / 2, 200000, 15, "q", scale_f * 2.0};
        opts.layers = {f_layer, q_layer};
    } else {
        // Single layer fallback
        VideoLayerSetting f_layer{target_f_w, target_f_h, 300000, 30, "f", scale_f * 1.0};
        opts.layers = {f_layer};
    }
    return opts;
}

std::shared_ptr<LocalVideoTrack> LocalVideoTrack::createLocalVideoTrack(const std::string& name,
                                                              const std::shared_ptr<VideoSource>& source) {
    std::string sid = "TR_VID_" + name;
    auto track = std::make_shared<LocalVideoTrack>(sid, name, source);
    if (source) {
        source->addSink([track](const VideoFrame& frame, const VideoCaptureOptions& options) {
            if (!track->muted()) {
                track->notifyVideoFrame(frame, options);
            }
        });

        auto factory = WebRTCManager::Instance().factory();
        if (factory) {
            WebRTCManager::Instance().worker_thread()->BlockingCall([&]() {
                auto rtc_src = RtcVideoSource::Create(source);
                track->rtc_source_ = rtc_src;
                auto rtc_video_track = factory->CreateVideoTrack(rtc_src, name);
                track->set_rtc_track(rtc_video_track);
            });
        }
    }
    return track;
}

} // namespace livekit
