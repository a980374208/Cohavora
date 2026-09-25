#include "local_video_track.h"
#include "webrtc_manager.h"
#include "rtc_video_source.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <set>

namespace livekit {

LocalVideoTrack::LocalVideoTrack(const std::string& sid, const std::string& name, std::shared_ptr<VideoSource> source,
                                 TrackSource source_type,
                                 const VideoPublishOptions& options)
    : Track(sid, name, TrackKind::Video, source_type), source_(source) {
    int w = source_ ? source_->width() : 1280;
    int h = source_ ? source_->height() : 720;
    requested_publish_options_ = options;
    requested_publish_options_.source = source_type;
    publish_options_ = ComputeMultiCodecSimulcastOptions(
        w, h, requested_publish_options_);
}

LocalVideoTrack::~LocalVideoTrack() = default;

void LocalVideoTrack::set_publish_options(const VideoPublishOptions& options) {
    requested_publish_options_ = options;
    requested_publish_options_.source = Track::source();
    const int width = source_ && source_->width() > 0 ? source_->width() : 1280;
    const int height = source_ && source_->height() > 0 ? source_->height() : 720;
    publish_options_ = ComputeMultiCodecSimulcastOptions(
        width, height, requested_publish_options_);
}

VideoFrameDiagnostics LocalVideoTrack::frame_diagnostics() const noexcept {
    webrtc::scoped_refptr<RtcVideoSource> rtc_source;
    {
        std::lock_guard lock(rtc_source_mutex_);
        rtc_source = rtc_source_;
    }
    // No WebRTC thread hop is needed to read the bridge's counters.
    if (rtc_source) return rtc_source->frame_diagnostics();
    VideoFrameDiagnostics result;
    result.source_available = static_cast<bool>(source_);
    if (source_) result.source_frames = source_->captured_frame_count();
    return result;
}

void LocalVideoTrack::set_rtc_source_for_diagnostics(webrtc::scoped_refptr<RtcVideoSource> source) {
    std::shared_ptr<telemetry::LocalVideoActivityProbe> telemetry_probe;
    webrtc::scoped_refptr<RtcVideoSource> installed;
    {
        std::lock_guard lock(rtc_source_mutex_);
        rtc_source_.swap(source);
        installed = rtc_source_;
        telemetry_probe = publish_telemetry_probe_;
    }
    if (installed) installed->SetTelemetryProbe(std::move(telemetry_probe));
    // Release a replaced source outside the lock: disconnect can wait for a
    // frame already being delivered to WebRTC.
}

void LocalVideoTrack::set_publish_telemetry_probe(
    std::shared_ptr<telemetry::LocalVideoActivityProbe> probe) {
    webrtc::scoped_refptr<RtcVideoSource> source;
    {
        std::lock_guard lock(rtc_source_mutex_);
        publish_telemetry_probe_ = probe;
        source = rtc_source_;
    }
    if (source) source->SetTelemetryProbe(std::move(probe));
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
    constexpr char kVideoPrefix[] = "video/";
    if (codec.rfind(kVideoPrefix, 0) == 0) codec.erase(0, sizeof(kVideoPrefix) - 1);
    return codec;
}

std::set<std::string> CodecSet(const std::vector<std::string>& codecs) {
    std::set<std::string> result;
    for (const auto& codec : codecs) {
        const auto normalized = CodecName(codec);
        if (!normalized.empty()) result.insert(normalized);
    }
    return result;
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

int LocalVideoTrack::SpatialLayersFromScalabilityMode(const std::string& mode) {
    if (mode.size() < 2 || (mode[0] != 'L' && mode[0] != 'l')) return 1;
    int layers = 0;
    for (size_t index = 1; index < mode.size(); ++index) {
        if (!std::isdigit(static_cast<unsigned char>(mode[index]))) break;
        layers = layers * 10 + (mode[index] - '0');
    }
    return std::max(1, layers);
}

std::vector<VideoLayerSetting> LocalVideoTrack::ComputeSignalLayers(
    const SimulcastCodecSpec& spec) {
    const int spatial_layers = SpatialLayersFromScalabilityMode(
        spec.scalability_mode);
    if (spatial_layers <= 1 || spec.layers.size() != 1) return spec.layers;

    const auto& source = spec.layers.front();
    std::vector<VideoLayerSetting> result;
    result.reserve(static_cast<size_t>(spatial_layers));
    for (int index = 0; index < spatial_layers; ++index) {
        const int scale = 1 << (spatial_layers - 1 - index);
        result.push_back({
            std::max(1, source.width / scale),
            std::max(1, source.height / scale),
            source.max_bitrate_bps / spatial_layers,
            source.max_fps,
            {},
            static_cast<double>(scale),
        });
    }
    return result;
}

ResolvedVideoPublishPlan LocalVideoTrack::ResolvePublishPlan(
    int width,
    int height,
    const VideoPublishOptions& requested_options,
    const std::vector<std::string>& local_sender_codecs,
    const std::vector<std::string>& server_enabled_codecs) {
    ResolvedVideoPublishPlan plan;
    plan.requested = requested_options;
    plan.requested_codec = CodecName(requested_options.video_codec);
    if (plan.requested_codec.empty()) plan.requested_codec = "auto";

    const auto local = CodecSet(local_sender_codecs);
    const auto server = CodecSet(server_enabled_codecs);
    const auto allowed = [&](const std::string& codec) {
        return local.contains(codec) &&
            (server_enabled_codecs.empty() || server.contains(codec));
    };

    constexpr std::array<const char*, 4> kFallbackOrder{
        "vp8", "h264", "vp9", "av1"};
    plan.effective_codec = plan.requested_codec;
    if (plan.requested_codec == "auto") {
        const auto selected = std::find_if(
            kFallbackOrder.begin(), kFallbackOrder.end(),
            [&](const char* codec) { return allowed(codec); });
        if (selected == kFallbackOrder.end()) {
            plan.error = "no video codec is available in the local/server publish intersection";
            return plan;
        }
        plan.effective_codec = *selected;
    } else if (!allowed(plan.effective_codec)) {
        const auto fallback = std::find_if(
            kFallbackOrder.begin(), kFallbackOrder.end(),
            [&](const char* codec) { return allowed(codec); });
        if (fallback == kFallbackOrder.end()) {
            plan.error = "no video codec is available in the local/server publish intersection";
            return plan;
        }
        plan.effective_codec = *fallback;
        plan.fallback_reason = "requested codec " + plan.requested_codec +
            " is unavailable in the local/server publish intersection";
    }

    plan.effective = requested_options;
    plan.effective.video_codec = plan.effective_codec;
    if (plan.requested_codec != "auto" && plan.used_fallback()) {
        plan.effective.scalability_mode.clear();
    }

    if (!plan.effective.scalability_mode.empty()) {
        const bool svc_codec = plan.effective_codec == "vp9" ||
            plan.effective_codec == "av1";
        if (!svc_codec || !IsVideoEncoderFormatSupported(
                plan.effective_codec, plan.effective.scalability_mode)) {
            plan.error = "scalability mode " + plan.effective.scalability_mode +
                " is not supported for " + plan.effective_codec;
            return plan;
        }
    }

    const bool wants_backup = plan.effective.simulcast &&
        (plan.effective_codec == "vp9" || plan.effective_codec == "av1") &&
        (requested_options.auto_backup_codec || requested_options.backup_codec.has_value());
    std::optional<std::string> backup;
    if (wants_backup && requested_options.backup_codec.has_value()) {
        const auto explicit_backup = CodecName(*requested_options.backup_codec);
        if (explicit_backup == plan.effective_codec || !allowed(explicit_backup)) {
            plan.error = "requested backup codec is not a distinct codec in the local/server publish intersection";
            return plan;
        }
        backup = explicit_backup;
    } else if (wants_backup) {
        for (const auto* candidate : {"vp8", "h264"}) {
            if (plan.effective_codec != candidate && allowed(candidate)) {
                backup = candidate;
                break;
            }
        }
    }

    plan.effective.backup_codec.reset();
    plan.effective.auto_backup_codec = false;
    plan.effective = ComputeMultiCodecSimulcastOptions(
        width, height, plan.effective);
    if (backup.has_value()) {
        VideoPublishOptions backup_options = requested_options;
        backup_options.video_codec = *backup;
        backup_options.scalability_mode.clear();
        backup_options.backup_codec.reset();
        backup_options.auto_backup_codec = false;
        const auto computed = ComputeSimulcastOptions(
            width, height, backup_options);
        plan.effective.simulcast_codecs.push_back({
            *backup, {}, {}, computed.layers});
        plan.effective.backup_codec = *backup;
    }
    plan.effective.auto_backup_codec = requested_options.auto_backup_codec;
    return plan;
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
                track->set_rtc_source_for_diagnostics(rtc_src);
                auto rtc_video_track = factory->CreateVideoTrack(rtc_src, name);
                track->set_rtc_track(rtc_video_track);
            });
        }
    }
    return track;
}

} // namespace livekit
