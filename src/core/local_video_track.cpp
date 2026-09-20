#include "local_video_track.h"
#include "webrtc_manager.h"
#include "rtc_video_source.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace livekit {

LocalVideoTrack::LocalVideoTrack(const std::string& sid, const std::string& name, std::shared_ptr<VideoSource> source,
                                 TrackSource source_type,
                                 const VideoPublishOptions& options)
    : Track(sid, name, TrackKind::Video, source_type), source_(source) {
    int w = source_ ? source_->width() : 1280;
    int h = source_ ? source_->height() : 720;
    VideoPublishOptions effective_opts = options;
    effective_opts.source = source_type;
    publish_options_ = ComputeMultiCodecSimulcastOptions(w, h, effective_opts);
}

namespace {

// Default encoding policy from client-sdk-cpp's Rust core:
// livekit/src/room/options.rs @ a0c91f5ae2309f3911c4a5d5843f385e6f20846a.
// Presets choose bitrate/fps and lower layers; the top layer keeps source size.
constexpr VideoPreset kCamera169[] = {
    {160, 90, 90000, 15}, {320, 180, 160000, 15},
    {384, 216, 180000, 15}, {640, 360, 450000, 20},
    {960, 540, 800000, 25}, {1280, 720, 1700000, 30},
    {1920, 1080, 3000000, 30}, {2560, 1440, 5000000, 30},
    {3840, 2160, 8000000, 30},
};
constexpr VideoPreset kCamera43[] = {
    {160, 120, 80000, 15}, {240, 180, 100000, 15},
    {320, 240, 150000, 15}, {480, 360, 225000, 20},
    {640, 480, 300000, 20}, {720, 540, 450000, 25},
    {960, 720, 1500000, 30}, {1440, 1080, 2500000, 30},
    {1920, 1440, 3500000, 30},
};
constexpr VideoPreset kScreenShare[] = {
    {640, 360, 200000, 3}, {1280, 720, 400000, 5},
    {1280, 720, 1000000, 15}, {1920, 1080, 1500000, 15},
    {1920, 1080, 3000000, 30},
};

template <size_t N>
VideoPreset EncodingPreset(const VideoPreset (&presets)[N], int size) {
    for (const auto& preset : presets) {
        // Rust uses a strict > here, including at exact preset boundaries.
        if (preset.width > size) return preset;
    }
    return presets[N - 1];
}

std::string CodecName(std::string codec) {
    for (auto& c : codec) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return codec;
}

} // namespace

VideoPublishOptions LocalVideoTrack::ComputeSimulcastOptions(int width, int height, const VideoPublishOptions& input_options) {
    VideoPublishOptions opts = input_options;
    if (width <= 0 || height <= 0) {
        width = 1280;
        height = 720;
    }

    const int max_size = std::max(width, height);
    const int min_size = std::min(width, height);
    const float aspect = static_cast<float>(max_size) / min_size;
    const bool is_16_9 = std::abs(aspect - 16.0f / 9.0f) < std::abs(aspect - 4.0f / 3.0f);
    const bool screenshare = opts.source == TrackSource::ScreenShareVideo;
    auto original = screenshare ? EncodingPreset(kScreenShare, max_size)
        : (is_16_9 ? EncodingPreset(kCamera169, max_size) : EncodingPreset(kCamera43, max_size));
    original.width = width;
    original.height = height;
    const auto codec = CodecName(opts.video_codec);
    // The reference adjusts only the source encoding, not the lower presets.
    if (codec == "av1") {
        original.max_bitrate_bps = static_cast<int>(static_cast<float>(original.max_bitrate_bps) * 0.7f);
    } else if (codec == "vp9") {
        original.max_bitrate_bps = static_cast<int>(static_cast<float>(original.max_bitrate_bps) * 0.85f);
    }

    // Keep the Rust low-to-high preset order until RID assignment is complete.
    // An explicit SVC mode uses one RTP encoding; codecs do not imply a mode.
    std::vector<VideoPreset> presets;
    if (opts.simulcast && opts.scalability_mode.empty() && max_size >= 480) {
        if (screenshare) {
            const int low_bitrate = std::max(150000,
                original.max_bitrate_bps / (4 * (original.max_fps / 3)));
            presets.push_back({width / 2, height / 2, low_bitrate, 3});
        } else {
            const auto& camera = is_16_9 ? kCamera169 : kCamera43;
            if (max_size >= 960) presets.push_back(camera[1]); // 180p
            presets.push_back(camera[3]); // 360p, also the lower of two layers
        }
    }
    presets.push_back(original);

    opts.layers.clear();
    constexpr char kRids[] = {'q', 'h', 'f'};
    for (size_t i = 0; i < presets.size(); ++i) {
        const auto& preset = presets[i];
        const double scale = std::max(1.0,
            static_cast<double>(min_size) / std::min(preset.width, preset.height));
        opts.layers.push_back({static_cast<int>(width / scale), static_cast<int>(height / scale),
            preset.max_bitrate_bps, preset.max_fps, std::string(1, kRids[i]), scale});
    }
    // Match Rust into_rtp_encodings: f/h/q for three, h/q for two, q for one.
    std::reverse(opts.layers.begin(), opts.layers.end());
    return opts;
}

VideoPublishOptions LocalVideoTrack::ComputeMultiCodecSimulcastOptions(int width, int height, const VideoPublishOptions& input_options) {
    VideoPublishOptions opts = ComputeSimulcastOptions(width, height, input_options);

    const auto codec_lower = CodecName(opts.video_codec);

    SimulcastCodecSpec primary_spec;
    primary_spec.codec = opts.video_codec;
    primary_spec.scalability_mode = opts.scalability_mode;
    primary_spec.layers = opts.layers;
    opts.simulcast_codecs = { primary_spec };

    // When primary codec is advanced (AV1 or VP9) and backup codec is enabled, compute fallback codec layers (e.g. VP8 / H264)
    if (opts.simulcast && (codec_lower == "av1" || codec_lower == "vp9") && (opts.auto_backup_codec || opts.backup_codec.has_value())) {
        std::string backup_codec_name = opts.backup_codec.value_or("vp8");
        VideoPublishOptions backup_input = input_options;
        backup_input.video_codec = backup_codec_name;
        backup_input.scalability_mode = "";
        backup_input.backup_codec = std::nullopt;
        backup_input.auto_backup_codec = false;

        VideoPublishOptions backup_computed = ComputeSimulcastOptions(width, height, backup_input);
        SimulcastCodecSpec backup_spec;
        backup_spec.codec = backup_codec_name;
        backup_spec.layers = backup_computed.layers;
        opts.simulcast_codecs.push_back(backup_spec);
        opts.backup_codec = backup_codec_name;
    }

    return opts;
}

VideoPublishOptions LocalVideoTrack::DefaultVp8SimulcastOptions(int width, int height) {
    VideoPublishOptions opts;
    opts.source = TrackSource::Camera;
    opts.simulcast = true;
    opts.video_codec = "vp8";
    return ComputeMultiCodecSimulcastOptions(width, height, opts);
}

std::shared_ptr<LocalVideoTrack> LocalVideoTrack::createLocalVideoTrack(const std::string& name,
                                                                      const std::shared_ptr<VideoSource>& source,
                                                                      TrackSource source_type,
                                                                      const VideoPublishOptions& options) {
    std::string sid = "TR_VID_" + name;
    auto track = std::make_shared<LocalVideoTrack>(sid, name, source, source_type, options);
    if (source) {
        source->addSink([weak = std::weak_ptr<LocalVideoTrack>(track)](const VideoFrame& frame, const VideoCaptureOptions& cap_options) {
            if (auto track = weak.lock(); track && !track->muted()) {
                track->notifyVideoFrame(frame, cap_options);
            }
        });

        auto factory = WebRTCManager::Instance().factory();
        if (factory) {
            // MediaStreamTrack proxies are created and primarily accessed on
            // the PeerConnection signaling thread.
            WebRTCManager::Instance().signaling_thread()->BlockingCall([&]() {
                auto rtc_src = RtcVideoSource::Create(source, source_type == TrackSource::ScreenShareVideo);
                track->rtc_source_ = rtc_src;
                auto rtc_video_track = factory->CreateVideoTrack(rtc_src, name);
                track->set_rtc_track(rtc_video_track);
            });
        }
    }
    return track;
}

} // namespace livekit
