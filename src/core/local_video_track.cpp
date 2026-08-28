#include "local_video_track.h"
#include "webrtc_manager.h"
#include "rtc_video_source.h"

namespace livekit {

LocalVideoTrack::LocalVideoTrack(const std::string& sid, const std::string& name, std::shared_ptr<VideoSource> source,
                                 TrackSource source_type,
                                 const VideoPublishOptions& options)
    : Track(sid, name, TrackKind::Video, source_type), source_(source) {
    int w = source_ ? source_->width() : 1280;
    int h = source_ ? source_->height() : 720;
    VideoPublishOptions effective_opts = options;
    effective_opts.source = source_type;
    publish_options_ = ComputeSimulcastOptions(w, h, effective_opts);
}

static double findEvenScaleDownBy(int src_w, int src_h, int target_w, int target_h) {
    int src_max = std::max(src_w, src_h);
    int target_max = std::max(target_w, target_h);
    if (target_max <= 0) return 1.0;

    for (int i = 0; i <= 30; ++i) {
        double scale = static_cast<double>(src_max) / (target_max + i);
        if (scale < 1.0) scale = 1.0;
        int scaled_w = static_cast<int>(src_w / scale);
        int scaled_h = static_cast<int>(src_h / scale);
        if (scaled_w % 2 == 0 && scaled_h % 2 == 0) {
            return scale;
        }
    }
    return static_cast<double>(src_max) / target_max;
}

VideoPublishOptions LocalVideoTrack::ComputeSimulcastOptions(int width, int height, const VideoPublishOptions& input_options) {
    VideoPublishOptions opts = input_options;

    if (width <= 0 || height <= 0) {
        width = 1280;
        height = 720;
    }

    // 1. 编码器码率折算系数 (AV1=0.7x, VP9=0.85x, VP8/H264=1.0x)
    double codec_factor = 1.0;
    std::string codec_lower = opts.video_codec;
    for (auto& c : codec_lower) c = static_cast<char>(tolower(c));
    if (codec_lower == "av1") {
        codec_factor = 0.70;
    } else if (codec_lower == "vp9") {
        codec_factor = 0.85;
    }

    // 2. 屏幕共享专属策略 (分辨率与文字清晰度优先，帧率限制在 15fps)
    if (opts.source == TrackSource::ScreenShareVideo) {
        if (codec_lower == "vp9" || codec_lower == "av1") {
            opts.scalability_mode = "L1T3";
        }

        if (!opts.simulcast) {
            VideoLayerSetting f_layer{width, height, static_cast<int>(2500000 * codec_factor), 15, "f", 1.0};
            opts.layers = {f_layer};
            return opts;
        }

        // 屏幕共享 2 层 Simulcast (100% 原生分辨率 + 50% 缩放)
        int half_w = (width / 2 / 2) * 2;
        int half_h = (height / 2 / 2) * 2;
        VideoLayerSetting f_layer{width, height, static_cast<int>(2500000 * codec_factor), 15, "f", 1.0};
        VideoLayerSetting q_layer{half_w, half_h, static_cast<int>(800000 * codec_factor), 15, "q", 2.0};
        opts.layers = {f_layer, q_layer};
        return opts;
    }

    // 3. 摄像头推流：画幅比例自适应 (16:9 vs 4:3)
    double aspect = static_cast<double>(width) / height;
    bool is_16_9 = std::abs(aspect - (16.0 / 9.0)) <= std::abs(aspect - (4.0 / 3.0));

    // SVC 模式支持 (VP9 / AV1 默认启用 L3T3_KEY)
    if (opts.simulcast && (codec_lower == "vp9" || codec_lower == "av1")) {
        opts.scalability_mode = "L3T3_KEY";
    }

    if (!opts.simulcast) {
        int max_dim = std::max(width, height);
        int bitrate = (max_dim >= 1280) ? 2000000 : ((max_dim >= 720) ? 1200000 : 500000);
        VideoLayerSetting f_layer{width, height, static_cast<int>(bitrate * codec_factor), 30, "f", 1.0};
        opts.layers = {f_layer};
        return opts;
    }

    // 4. 标准 Simulcast 分层计算 (基于宽高最大维度)
    int max_size = std::max(width, height);

    if (max_size >= 960) {
        // 3 层推流 (High 720p@30fps, Mid 360p@20fps, Low 180p@15fps)
        int f_w = is_16_9 ? 1280 : 960;
        int f_h = 720;
        int h_w = is_16_9 ? 640 : 480;
        int h_h = is_16_9 ? 360 : 360;
        int q_w = is_16_9 ? 320 : 240;
        int q_h = is_16_9 ? 180 : 180;

        double scale_f = findEvenScaleDownBy(width, height, f_w, f_h);
        double scale_h = findEvenScaleDownBy(width, height, h_w, h_h);
        double scale_q = findEvenScaleDownBy(width, height, q_w, q_h);

        int bit_f = static_cast<int>(1700000 * codec_factor);
        int bit_h = static_cast<int>(450000 * codec_factor);
        int bit_q = static_cast<int>(160000 * codec_factor);

        VideoLayerSetting f_layer{static_cast<int>(width / scale_f), static_cast<int>(height / scale_f), bit_f, 30, "f", scale_f};
        VideoLayerSetting h_layer{static_cast<int>(width / scale_h), static_cast<int>(height / scale_h), bit_h, 20, "h", scale_h};
        VideoLayerSetting q_layer{static_cast<int>(width / scale_q), static_cast<int>(height / scale_q), bit_q, 15, "q", scale_q};

        opts.layers = {f_layer, h_layer, q_layer};
    } else if (max_size >= 480) {
        // 2 层推流 (High + Low)
        int f_w = is_16_9 ? 640 : 480;
        int f_h = is_16_9 ? 360 : 360;
        int q_w = is_16_9 ? 320 : 240;
        int q_h = is_16_9 ? 180 : 180;

        double scale_f = findEvenScaleDownBy(width, height, f_w, f_h);
        double scale_q = findEvenScaleDownBy(width, height, q_w, q_h);

        int bit_f = static_cast<int>(800000 * codec_factor);
        int bit_q = static_cast<int>(180000 * codec_factor);

        VideoLayerSetting f_layer{static_cast<int>(width / scale_f), static_cast<int>(height / scale_f), bit_f, 25, "f", scale_f};
        VideoLayerSetting q_layer{static_cast<int>(width / scale_q), static_cast<int>(height / scale_q), bit_q, 15, "q", scale_q};

        opts.layers = {f_layer, q_layer};
    } else {
        // 1 层推流
        int bit_f = static_cast<int>(300000 * codec_factor);
        VideoLayerSetting f_layer{width, height, bit_f, 30, "f", 1.0};
        opts.layers = {f_layer};
    }

    return opts;
}

VideoPublishOptions LocalVideoTrack::DefaultVp8SimulcastOptions(int width, int height) {
    VideoPublishOptions opts;
    opts.source = TrackSource::Camera;
    opts.simulcast = true;
    opts.video_codec = "vp8";
    return ComputeSimulcastOptions(width, height, opts);
}

std::shared_ptr<LocalVideoTrack> LocalVideoTrack::createLocalVideoTrack(const std::string& name,
                                                                      const std::shared_ptr<VideoSource>& source,
                                                                      TrackSource source_type,
                                                                      const VideoPublishOptions& options) {
    std::string sid = "TR_VID_" + name;
    auto track = std::make_shared<LocalVideoTrack>(sid, name, source, source_type, options);
    if (source) {
        source->addSink([track](const VideoFrame& frame, const VideoCaptureOptions& cap_options) {
            if (!track->muted()) {
                track->notifyVideoFrame(frame, cap_options);
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
