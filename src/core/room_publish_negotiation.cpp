#include "detail/room_internal.h"
#include "webrtc_manager.h"
#include "rtc/h265_level_constraints.h"
#include "stats_collector.h"
#include "local_audio_track.h"
#include "local_video_track.h"
#include "rtc_audio_source.h"
#include "rtc_video_source.h"
#include "livekit_rtc.pb.h"
#include "livekit_models.pb.h"
#include "api/jsep.h"
#include "modules/video_coding/codecs/h264/include/h264.h"
#include "pc/session_description.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <limits>
#include <set>

namespace livekit {

namespace {

std::vector<std::string> LocalVideoSenderCodecs() {
    std::vector<std::string> result;
    const auto factory = WebRTCManager::Instance().factory();
    if (factory) {
        const auto capabilities = factory->GetRtpSenderCapabilities(
            webrtc::MediaType::VIDEO);
        for (const auto& codec : capabilities.codecs) {
            if (codec.IsMediaCodec()) result.push_back(codec.name);
        }
        if (!result.empty()) return result;
    }
    if (const auto encoder_factory = CreateVideoEncoderFactory()) {
        for (const auto& format : encoder_factory->GetSupportedFormats()) {
            if (!format.name.empty()) result.push_back(format.name);
        }
    }
    return result;
}

std::vector<webrtc::SdpVideoFormat> LocalHardwareH264Formats(bool screenshare) {
    std::vector<webrtc::SdpVideoFormat> formats;
    const auto factory = WebRTCManager::Instance().factory();
    if (!factory) return formats;
    const auto capabilities = factory->GetRtpSenderCapabilities(webrtc::MediaType::VIDEO);
    for (const auto& codec : capabilities.codecs) {
        if (!codec.IsMediaCodec() || CompareCodecName(codec.name.c_str(), "h264") != 0) continue;
        webrtc::SdpVideoFormat format(codec.name, codec.parameters);
        if (IsHardwareH264FormatSupported(format, screenshare)) formats.push_back(std::move(format));
    }
    return formats;
}

std::optional<std::uint8_t> LocalH265SendLevel() {
    const auto factory = WebRTCManager::Instance().factory();
    std::optional<std::uint8_t> level;
    if (factory) {
        for (const auto& codec : factory->GetRtpSenderCapabilities(webrtc::MediaType::VIDEO).codecs) {
            const auto format = detail::ProjectH265Format(codec.name, codec.parameters);
            if (codec.IsMediaCodec() && format && format->valid)
                level = std::max(level.value_or(0), format->level);
        }
        if (level) return level;
    }
    if (const auto encoder_factory = CreateVideoEncoderFactory()) {
        for (const auto& format : encoder_factory->GetSupportedFormats()) {
            const auto projected = detail::ProjectH265Format(format.name, format.parameters);
            if (projected && projected->valid)
                level = std::max(level.value_or(0), projected->level);
        }
    }
    return level;
}

std::string NormalizePreferenceToken(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool HardwareH264AvailableForPreference(
        const std::string& preference, const VideoEncoderCapabilities& hardware) {
    if (!hardware.ready) return false;
    const auto pref = NormalizePreferenceToken(preference);
    if (pref == "software") return false;
    if (pref == "amf") return hardware.amf.h264;
    if (pref == "nvenc") return hardware.nvenc.h264;
    if (pref == "qsv") return hardware.qsv.h264;
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
    if (pref == "vaapi") return hardware.vaapi.h264;
    return hardware.nvenc.h264 || hardware.qsv.h264 || hardware.vaapi.h264;
#else
    return hardware.amf.h264 || hardware.nvenc.h264 || hardware.qsv.h264;
#endif
}

std::optional<webrtc::H264Level> PredictedHardwareH264ScreenShareLevel(
        const std::string& preference, const VideoEncoderCapabilities& hardware) {
    if (!HardwareH264AvailableForPreference(preference, hardware)) return std::nullopt;
    std::optional<webrtc::H264Level> max_level;
    auto consider_formats = [&](const std::vector<webrtc::SdpVideoFormat>& formats) {
        for (const auto& format : formats) {
            if (CompareCodecName(format.name.c_str(), "h264") != 0) continue;
            if (!IsHardwareH264FormatSupportedForPreference(format, preference, hardware, true)) continue;
            const auto projected = detail::ProjectH264Format(format.name, format.parameters);
            if (!projected || !projected->valid) continue;
            const auto level = static_cast<webrtc::H264Level>(projected->level);
            max_level = max_level ? std::max(*max_level, level) : level;
        }
    };
    if (const auto factory = WebRTCManager::Instance().factory()) {
        std::vector<webrtc::SdpVideoFormat> formats;
        for (const auto& codec : factory->GetRtpSenderCapabilities(webrtc::MediaType::VIDEO).codecs) {
            if (codec.IsMediaCodec() && CompareCodecName(codec.name.c_str(), "h264") == 0) {
                formats.emplace_back(codec.name, codec.parameters);
            }
        }
        consider_formats(formats);
    }
    if (!max_level) {
        if (const auto encoder_factory = CreateVideoEncoderFactory()) {
            consider_formats(encoder_factory->GetSupportedFormats());
        }
    }
    return max_level;
}

[[noreturn]] void RejectH264Profile() {
    throw OperationError(OperationKind::PublishTrack, OperationErrorCode::CodecUnsupported,
        "video_quality", "the negotiated H264 format cannot support this video profile", false,
        "h264", OperationRecoveryAction::SelectSupportedCodec);
}

std::optional<webrtc::H264Level> SenderH264Level(const webrtc::RtpParameters& parameters) {
    std::optional<webrtc::H264Level> level;
    for (const auto& encoding : parameters.encodings) {
        const auto* codec = encoding.codec ? &*encoding.codec :
            (parameters.codecs.empty() ? nullptr : &parameters.codecs.front());
        if (!codec || CompareCodecName(codec->name.c_str(), "h264") != 0) continue;
        const auto parsed = webrtc::ParseSdpForH264ProfileLevelId(codec->parameters);
        if (!parsed) RejectH264Profile();
        level = level ? std::min(*level, parsed->level) : parsed->level;
    }
    return level;
}

[[noreturn]] void RejectH265Profile() {
    throw OperationError(OperationKind::PublishTrack, OperationErrorCode::CodecUnsupported,
        "screen_quality", "the negotiated H265 format cannot support this screen profile", false,
        "h265", OperationRecoveryAction::SelectSupportedCodec);
}

// A sender may select a format per encoding. Never substitute a locally
// advertised level for the actual format after negotiation.
std::optional<std::uint8_t> SenderH265Level(const webrtc::RtpParameters& parameters, bool require_format = false) {
    std::optional<std::uint8_t> level;
    if (parameters.encodings.empty()) RejectH265Profile();
    for (const auto& encoding : parameters.encodings) {
        const auto* codec = encoding.codec ? &*encoding.codec :
            (parameters.codecs.empty() ? nullptr : &parameters.codecs.front());
        if (!codec) { if (require_format) RejectH265Profile(); else continue; }
        const auto format = detail::ProjectH265Format(codec->name, codec->parameters);
        if (!format) continue;
        if (!format->valid) RejectH265Profile();
        level = std::min(level.value_or(format->level), format->level);
    }
    return level;
}

std::optional<webrtc::H264Level> MaxH264FormatLevel(
        const std::vector<webrtc::SdpVideoFormat>& formats) {
    std::optional<webrtc::H264Level> maximum;
    for (const auto& format : formats) {
        if (CompareCodecName(format.name.c_str(), "h264") != 0) continue;
        const auto projected = detail::ProjectH264Format(format.name, format.parameters);
        if (!projected || !projected->valid) continue;
        const auto level = static_cast<webrtc::H264Level>(projected->level);
        if (!maximum || level > *maximum) maximum = level;
    }
    return maximum;
}

bool ConstrainH264LayersList(
        std::vector<VideoLayerSetting>& layers,
        webrtc::H264Level level) {
    for (auto& layer : layers) {
        if (layer.width <= 0 || layer.height <= 0 || layer.max_fps <= 0) {
            return false;
        }
        const auto constrained = detail::ConstrainH264Dimensions(
            level, layer.width, layer.height, static_cast<std::uint32_t>(layer.max_fps));
        if (!constrained) return false;
        layer.scale_resolution_down_by *= double(layer.width) / constrained->first;
        layer.width = constrained->first;
        layer.height = constrained->second;
    }
    return true;
}

bool ConstrainH264PublishOptions(
        VideoPublishOptions& options,
        webrtc::H264Level level) {
    if (CompareCodecName(options.video_codec.c_str(), "h264") == 0) {
        if (!ConstrainH264LayersList(options.layers, level)) return false;
    }
    for (auto& spec : options.simulcast_codecs) {
        if (CompareCodecName(spec.codec.c_str(), "h264") == 0) {
            if (!ConstrainH264LayersList(spec.layers, level)) return false;
        }
    }
    return true;
}

bool H264LayersFitLevel(
        const std::vector<VideoLayerSetting>& layers,
        webrtc::H264Level level) {
    for (const auto& layer : layers) {
        if (layer.width <= 0 || layer.height <= 0 || layer.max_fps <= 0) return false;
        if (detail::CheckH264LevelConstraints(
                level, layer.width, layer.height,
                static_cast<std::uint32_t>(layer.max_fps)) !=
            detail::H264LevelConstraintStatus::Supported) {
            return false;
        }
    }
    return true;
}

bool ExactH264Format(const webrtc::SdpVideoFormat& left,
                     const webrtc::SdpVideoFormat& right) {
    return CompareCodecName(left.name.c_str(), right.name.c_str()) == 0 &&
        left.parameters == right.parameters;
}


std::set<std::string> NormalizedVideoCodecs(const std::vector<std::string>& codecs) {
    std::set<std::string> result;
    for (auto name : codecs) {
        std::transform(name.begin(), name.end(), name.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (name.rfind("video/", 0) == 0) name.erase(0, 6);
        result.insert(std::move(name));
    }
    return result;
}

const char* VideoPreflightStatusName(VideoEncoderPreflightStatus status) {
    switch (status) {
    case VideoEncoderPreflightStatus::Ready: return "ready";
    case VideoEncoderPreflightStatus::InvalidParameters: return "invalid_parameters";
    case VideoEncoderPreflightStatus::UnsupportedFormat: return "unsupported_format";
    case VideoEncoderPreflightStatus::Unavailable: return "unavailable";
    case VideoEncoderPreflightStatus::InitializationFailed: return "initialization_failed";
    case VideoEncoderPreflightStatus::NotHardware: return "not_hardware";
    case VideoEncoderPreflightStatus::Cancelled: return "cancelled";
    case VideoEncoderPreflightStatus::TimedOut: return "timed_out";
    case VideoEncoderPreflightStatus::RetirementFailed: return "retirement_failed";
    }
    return "unavailable";
}

std::vector<std::string> LocalAudioSenderCodecs() {
    std::vector<std::string> result;
    const auto factory = WebRTCManager::Instance().factory();
    if (!factory) return result;
    const auto capabilities = factory->GetRtpSenderCapabilities(
        webrtc::MediaType::AUDIO);
    for (const auto& codec : capabilities.codecs) {
        if (codec.IsMediaCodec()) result.push_back(codec.name);
    }
    return result;
}

ResolvedVideoPublishPlan ResolveVideoPublishPlan(
    const std::shared_ptr<LocalVideoTrack>& track,
    const std::vector<std::string>& enabled_codecs,
    bool encrypted_auto = false) {
    const auto source = track ? track->source() : nullptr;
    const auto profile = track ? track->screen_share_profile() : std::nullopt;
    const auto [width, height] = profile ? std::pair{profile->width, profile->height}
        : source ? source->dimensions() : std::pair{1280,720};
    const auto requested = track->requested_publish_options();
    auto sender_codecs = LocalVideoSenderCodecs();
    const bool screenshare = requested.source == TrackSource::ScreenShareVideo;
    if (encrypted_auto && (requested.video_codec.empty() ||
        CompareCodecName(requested.video_codec.c_str(), "auto") == 0)) {
        // Preserve the automatic choice until hardware policy is resolved,
        // while keeping E2EE-supported single-codec profiles (VP8/H264 for
        // camera; VP9/VP8/H264 for screen share).
        std::erase_if(sender_codecs, [screenshare](const std::string& codec) {
            return CompareCodecName(codec.c_str(), "vp8") != 0 &&
                CompareCodecName(codec.c_str(), "h264") != 0 &&
                (!screenshare || CompareCodecName(codec.c_str(), "vp9") != 0);
        });
    }
    const auto hardware = GetVideoEncoderCapabilities();
    const bool prefer_hardware_h264 =
        HardwareH264AvailableForPreference(GetVideoEncoderPreference(), hardware);
    return LocalVideoTrack::ResolvePublishPlan(
        width,
        height,
        requested,
        sender_codecs,
        enabled_codecs,
        prefer_hardware_h264);
}

ResolvedAudioPublishPlan ResolveAudioPublishPlan(
    const std::shared_ptr<LocalAudioTrack>& track,
    bool media_encryption_enabled) {
    return LocalAudioTrack::ResolvePublishPlan(
        track->requested_publish_policy(),
        LocalAudioSenderCodecs(),
        media_encryption_enabled);
}

std::string MediaDiagnosticToken(std::string_view value) {
    if (value.empty()) return "N/A";
    std::string result;
    for (const unsigned char ch : value.substr(0, 96)) {
        result.push_back(std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.'
            ? static_cast<char>(ch) : '_');
    }
    return result;
}

std::string MediaCounter(bool available, uint64_t value) {
    return available ? std::to_string(value) : "N/A";
}

std::string MediaCounterDelta(bool available, uint64_t value,
                              bool previous_available, uint64_t previous) {
    if (!available || !previous_available) return "N/A";
    return value >= previous ? std::to_string(value - previous) : "reset";
}

struct PublisherRtpTotals {
    size_t audio_streams = 0, video_streams = 0, unknown_kind_streams = 0;
    uint64_t audio_packets = 0, video_packets = 0, video_bytes = 0, video_frames = 0;
    bool audio_packets_available = true, video_packets_available = true;
    bool video_bytes_available = true, video_frames_available = true;
};

PublisherRtpTotals SummarizePublisherRtp(const StatsReport& report) {
    PublisherRtpTotals totals;
    for (const auto& stream : report.outbound_rtp) {
        if (stream.kind_available && stream.kind == "audio") {
            ++totals.audio_streams;
            totals.audio_packets += stream.packets_sent;
            totals.audio_packets_available &= stream.packets_sent_available;
        } else if (stream.kind_available && stream.kind == "video") {
            ++totals.video_streams;
            totals.video_packets += stream.packets_sent;
            totals.video_bytes += stream.bytes_sent;
            totals.video_frames += stream.frames_encoded;
            totals.video_packets_available &= stream.packets_sent_available;
            totals.video_bytes_available &= stream.bytes_sent_available;
            totals.video_frames_available &= stream.frames_encoded_available;
        } else {
            ++totals.unknown_kind_streams;
        }
    }
    const bool known_kinds = totals.unknown_kind_streams == 0;
    totals.audio_packets_available &= totals.audio_streams > 0 && known_kinds;
    totals.video_packets_available &= totals.video_streams > 0 && known_kinds;
    totals.video_bytes_available &= totals.video_streams > 0 && known_kinds;
    totals.video_frames_available &= totals.video_streams > 0 && known_kinds;
    return totals;
}

std::string LocalPublishTelemetryKey(std::string_view rtc_track_id) {
    return rtc_track_id.empty()
        ? std::string{}
        : "local_publish/" + std::string(rtc_track_id);
}

telemetry::LocalVideoPipelineSample SampleLocalVideoPipeline(
    const std::shared_ptr<LocalVideoTrack>& track) {
    telemetry::LocalVideoPipelineSample sample;
    if (!track) return sample;
    const auto frames = track->frame_diagnostics();
    sample.source_available = frames.source_available;
    sample.rtc_available = frames.rtc_available;
    sample.source_frames = frames.source_frames;
    sample.rtc_input_frames = frames.rtc_input_frames;
    sample.rtc_output_frames = frames.rtc_output_frames;
    sample.rtc_dropped_frames = frames.rtc_dropped_frames;
    return sample;
}

std::shared_ptr<telemetry::LocalVideoActivityProbe> MakeLocalVideoActivityProbe(
    const std::shared_ptr<telemetry::SessionTelemetry>& telemetry_owner,
    const std::shared_ptr<LocalVideoTrack>& video,
    std::string series_key,
    std::uint64_t room_generation,
    std::uint64_t publication_epoch) {
    if (!telemetry_owner || series_key.empty() || room_generation == 0 ||
        publication_epoch == 0) {
        return {};
    }
    auto probe = std::make_shared<telemetry::LocalVideoActivityProbe>();
    probe->telemetry = telemetry_owner;
    probe->series_key = std::move(series_key);
    probe->room_generation = room_generation;
    probe->publication_epoch = publication_epoch;
    probe->pipeline_sampler = [weak_video = std::weak_ptr<LocalVideoTrack>(video)] {
        return SampleLocalVideoPipeline(weak_video.lock());
    };
    probe->active.store(true, std::memory_order_release);
    return probe;
}

std::shared_ptr<telemetry::LocalAudioActivityProbe> MakeLocalAudioActivityProbe(
    const std::shared_ptr<telemetry::SessionTelemetry>& telemetry_owner) {
    if (!telemetry_owner) return {};
    auto probe = std::make_shared<telemetry::LocalAudioActivityProbe>();
    probe->active.store(true, std::memory_order_release);
    return probe;
}

} // namespace

void Room::AddTrackToPublisher(std::shared_ptr<Track> track) {
    auto operation_admission = callback_gate_->Enter();
    if (!operation_admission) return;
    if (!track) return;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pub_pc;
    std::string stream_id;
    {
        std::lock_guard lock(room_mutex_);
        pub_pc = publisher_pc_;
        stream_id = "livekit_stream_" + (local_participant_ ? local_participant_->identity() : "local");
    }

    if (!pub_pc) return;
    webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> rtc_track = track->rtc_track();
    if (!rtc_track && WebRTCManager::Instance().factory()) {
        if (track->kind() == TrackKind::Audio) {
            auto audio_track = std::dynamic_pointer_cast<LocalAudioTrack>(track);
            if (audio_track && audio_track->source()) {
                auto rtc_src = RtcAudioSource::Create(audio_track->source());
                auto rtc_audio_track = WebRTCManager::Instance().factory()->CreateAudioTrack(track->name(), rtc_src.get());
                track->set_rtc_track(rtc_audio_track);
                rtc_track = rtc_audio_track;
            }
        } else if (track->kind() == TrackKind::Video) {
            auto video_track = std::dynamic_pointer_cast<LocalVideoTrack>(track);
            if (video_track && video_track->source()) {
                auto rtc_src = RtcVideoSource::Create(video_track->source(),
                    video_track->source() && video_track->Track::source() == TrackSource::ScreenShareVideo);
                auto rtc_video_track = WebRTCManager::Instance().factory()->CreateVideoTrack(rtc_src, track->name());
                video_track->set_rtc_source_for_diagnostics(rtc_src);
                track->set_rtc_track(rtc_video_track);
                rtc_track = rtc_video_track;
            }
        }
    }

    if (rtc_track) {
        VideoPublishOptions publish_opts;
        if (track->kind() == TrackKind::Video) {
            auto video_track = std::dynamic_pointer_cast<LocalVideoTrack>(track);
            if (video_track) {
                publish_opts = video_track->publish_options();
            }
        }

        struct AddTrackParams {
            webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
            webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track;
            std::string stream_id;
            VideoPublishOptions publish_opts;
            std::shared_ptr<Room> room;
        };
        auto* p = new AddTrackParams{pub_pc, rtc_track, stream_id, publish_opts, shared_from_this()};
        WebRTCManager::Instance().signaling_thread()->PostTask([p]() {
            if (p->pc && p->track) {
                if (p->track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind && p->publish_opts.simulcast && !p->publish_opts.layers.empty()) {
                    webrtc::RtpTransceiverInit init;
                    init.direction = webrtc::RtpTransceiverDirection::kSendOnly;
                    init.stream_ids = { p->stream_id };

                    for (const auto& layer : p->publish_opts.layers) {
                        webrtc::RtpEncodingParameters encoding;
                        encoding.active = true;
                        encoding.rid = layer.rid;
                        encoding.scale_resolution_down_by = layer.scale_resolution_down_by;
                        encoding.max_bitrate_bps = layer.max_bitrate_bps;
                        encoding.max_framerate = layer.max_fps;
                        if (!p->publish_opts.scalability_mode.empty()) {
                            encoding.scalability_mode = p->publish_opts.scalability_mode;
                        }
                        init.send_encodings.push_back(encoding);
                    }

                    auto transceiver_res = p->pc->AddTransceiver(p->track, init);
                    if (!transceiver_res.ok()) {
                        diagnostic::Event fallback;
                        fallback.kind = diagnostic::EventKind::MediaFallback;
                        fallback.media_kind = diagnostic::MediaKind::Video;
                        fallback.media_fallback_reason =
                            diagnostic::MediaFallbackReason::AddTransceiverFailed;
                        p->room->EmitDiagnostic(fallback);
                        p->pc->AddTrack(p->track, { p->stream_id });
                    }
                } else {
                    p->pc->AddTrack(p->track, { p->stream_id });
                }
                p->room->Log("TRACK", "PUB_ATTACH",
                    "Added a local media track to the Publisher PeerConnection");
                p->room->SendPublishOffer();
            }
            delete p;
        });
    }
}

void Room::ApplySimulcastParameters(webrtc::scoped_refptr<webrtc::RtpSenderInterface> sender, const VideoPublishOptions& opts) {
    if (!sender) return;
    webrtc::RtpParameters parameters = sender->GetParameters();
    if (parameters.encodings.empty()) {
        diagnostic::Event fallback;
        fallback.kind = diagnostic::EventKind::MediaFallback;
        fallback.media_kind = diagnostic::MediaKind::Video;
        fallback.media_fallback_reason =
            diagnostic::MediaFallbackReason::NoEncodings;
        diagnostic::EmitBusinessEvent(fallback);
        return;
    }

    bool updated = false;
    for (const auto& layer : opts.layers) {
        for (auto& enc : parameters.encodings) {
            if (enc.rid == layer.rid || (parameters.encodings.size() == 1 && layer.rid == "f")) {
                enc.active = true;
                if (layer.scale_resolution_down_by > 0) {
                    enc.scale_resolution_down_by = layer.scale_resolution_down_by;
                }
                if (layer.max_bitrate_bps > 0) {
                    enc.max_bitrate_bps = layer.max_bitrate_bps;
                }
                if (layer.max_fps > 0) {
                    enc.max_framerate = layer.max_fps;
                }
                if (!opts.scalability_mode.empty()) {
                    enc.scalability_mode = opts.scalability_mode;
                }
                updated = true;
            }
        }
    }

    if (updated) {
        auto status = sender->SetParameters(parameters);
        if (!status.ok()) {
            diagnostic::Event fallback;
            fallback.kind = diagnostic::EventKind::MediaFallback;
            fallback.media_kind = diagnostic::MediaKind::Video;
            fallback.media_fallback_reason =
                diagnostic::MediaFallbackReason::SenderParametersFailed;
            diagnostic::EmitBusinessEvent(fallback);
        }
    }
}

void Room::NegotiatePublisher() {
    auto operation_admission = callback_gate_->Enter();
    if (!operation_admission) return;
    NegotiatePublisher(session_generation_.load(std::memory_order_acquire));
}

void Room::NegotiatePublisher(uint64_t generation) {
    auto operation_admission = callback_gate_->Enter();
    if (!operation_admission) return;
    std::chrono::milliseconds timeout;
    {
        std::lock_guard lock(room_mutex_);
        if (!IsNativeGenerationCurrentLocked(generation)) return;
        timeout = operation_timeouts_.negotiation;
    }
    livekit::safe_co_spawn(executor_, [self = shared_from_this(), generation, timeout]() -> asio::awaitable<void> {
        try {
            co_await self->NegotiatePublisherAsync(timeout, generation);
        } catch (const std::exception&) {
            self->Log("ERROR", "NEGOTIATION_ASYNC",
                      secure_log::ExceptionSummary("publisher_negotiation"));
        }
    });
}

void Room::RollbackPublishedSenderBundle(
    const webrtc::scoped_refptr<webrtc::PeerConnectionInterface>& pc,
    const PublishedSenderBundle& bundle) {
    if (!pc) return;
    for (auto sender = bundle.senders.rbegin();
         sender != bundle.senders.rend(); ++sender) {
        if (!*sender || !pc->RemoveTrackOrError(*sender).ok() || !bundle.screen_share) {
            continue;
        }
        for (const auto& transceiver : pc->GetTransceivers()) {
            if (transceiver && transceiver->sender() == *sender) {
                transceiver->StopStandard();
                break;
            }
        }
    }
}

asio::awaitable<PublishedSenderBundle>
Room::AddTrackToPublisherAsync(
    std::shared_ptr<Track> track,
    uint64_t generation,
    std::optional<VideoPublishOptions> video_publish_options,
    std::optional<AudioPublishPolicy> audio_publish_policy,
    bool require_encryption
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
    , std::shared_ptr<webrtc::VideoEncoderFactory> linux_encoder_factory
#endif
    ) {
    auto operation_admission = AdmitOperation(OperationKind::PublishTrack, "AddTrackToPublisherAsync");
    if (!track) {
        throw OperationError(OperationKind::PublishTrack,
                             OperationErrorCode::InvalidState,
                             "install_sender",
                             "track is null");
    }

    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
    std::string stream_id;
    std::string encryption_identity;
    std::shared_ptr<E2eeManager> encryption;
    {
        std::lock_guard lock(room_mutex_);
        if (generation != session_generation_.load(std::memory_order_acquire) ||
            connection_state_ != ConnectionState::Connected) {
            throw OperationError(OperationKind::PublishTrack,
                                 OperationErrorCode::Cancelled,
                                 "install_sender",
                                 "room session changed while publishing");
        }
        pc = publisher_pc_;
        stream_id = "livekit_stream_" +
            (local_participant_ ? local_participant_->identity() : "local");
        encryption_identity = local_participant_ ? local_participant_->identity() : "";
        encryption = e2ee_manager_;
    }
    if (require_encryption && (!encryption || !encryption->enabled())) {
        throw OperationError(OperationKind::PublishTrack, OperationErrorCode::EncryptionFailed,
            "e2ee_sender_admission", "encryption policy changed before sender installation", false);
    }
    if (!pc || !WebRTCManager::Instance().factory() ||
        !WebRTCManager::Instance().signaling_thread()) {
        throw OperationError(OperationKind::PublishTrack,
                             OperationErrorCode::InvalidState,
                             "install_sender",
                             "publisher peer connection is unavailable");
    }

    auto rtc_track = track->rtc_track();
    if (!rtc_track) {
        if (track->kind() == TrackKind::Audio) {
            auto audio_track = std::dynamic_pointer_cast<LocalAudioTrack>(track);
            if (audio_track && audio_track->source()) {
                auto rtc_src = RtcAudioSource::Create(audio_track->source());
                rtc_track = WebRTCManager::Instance().factory()->CreateAudioTrack(
                    track->name(), rtc_src.get());
            }
        } else if (track->kind() == TrackKind::Video) {
            auto video_track = std::dynamic_pointer_cast<LocalVideoTrack>(track);
            if (video_track && video_track->source()) {
                auto rtc_src = RtcVideoSource::Create(video_track->source(),
                    video_track->source() && video_track->Track::source() == TrackSource::ScreenShareVideo);
                video_track->set_rtc_source_for_diagnostics(rtc_src);
                rtc_track = WebRTCManager::Instance().factory()->CreateVideoTrack(
                    rtc_src, track->name());
            }
        }
        if (rtc_track) track->set_rtc_track(rtc_track);
    }
    if (!rtc_track) {
        throw OperationError(OperationKind::PublishTrack,
                             OperationErrorCode::InvalidState,
                             "install_sender",
                             "failed to create native WebRTC track");
    }

    VideoPublishOptions publish_options;
    if (auto video = std::dynamic_pointer_cast<LocalVideoTrack>(track)) {
        publish_options = video_publish_options.has_value()
            ? *video_publish_options : video->publish_options();
    }
    AudioPublishPolicy resolved_audio_policy;
    if (auto audio = std::dynamic_pointer_cast<LocalAudioTrack>(track)) {
        resolved_audio_policy = audio_publish_policy.has_value()
            ? *audio_publish_policy : audio->requested_publish_policy();
    }

    auto completion = std::make_shared<AwaitableState<PublishedSenderBundle>>(executor_);
    auto cancel_native = callback_gate_->CancelWhenClosed(
        [weak = std::weak_ptr(completion)] {
            if (auto pending = weak.lock()) {
                FailAwaitable(pending, std::make_exception_ptr(OperationError(
                    OperationKind::PublishTrack, OperationErrorCode::Cancelled,
                    "install_sender", "room is retired")));
            }
        });
    // WebRTC is linked with a static CRT and PostTask type-erases the closure
    // inside webrtc::Thread. Keep the closure trivially destructible: putting
    // std::string/std::vector directly in it can make absl::AnyInvocable free
    // their storage through a different CRT at task teardown.
    struct AddTrackTaskParams {
        std::shared_ptr<Room> room;
        std::shared_ptr<AwaitableState<PublishedSenderBundle>> completion;
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
        webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> rtc_track;
        std::string stream_id;
        VideoPublishOptions publish_options;
        AudioPublishPolicy audio_publish_policy;
        uint64_t generation = 0;
        bool screen_share = false;
        std::shared_ptr<E2eeManager> encryption;
        std::string encryption_identity;
        bool require_encryption = false;
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
        std::shared_ptr<webrtc::VideoEncoderFactory> linux_encoder_factory;
#endif
    };
    auto* params = new AddTrackTaskParams{
        shared_from_this(), completion, pc, rtc_track, stream_id,
        std::move(publish_options), std::move(resolved_audio_policy),
        generation, track->source() == TrackSource::ScreenShareVideo,
        encryption, encryption_identity, require_encryption
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
        , std::move(linux_encoder_factory)
#endif
        };
    WebRTCManager::Instance().signaling_thread()->PostTask(
        [params]() {
            // Destruction happens in this translation unit rather than in the
            // AnyInvocable manager instantiated across the WebRTC ABI boundary.
            std::unique_ptr<AddTrackTaskParams> owned(params);
            auto& task = *owned;
            if (task.generation != task.room->session_generation_.load(std::memory_order_acquire)) {
                FailAwaitable(task.completion, std::make_exception_ptr(OperationError(
                    OperationKind::PublishTrack,
                    OperationErrorCode::Cancelled,
                    "install_sender",
                    "session changed before sender installation")));
                return;
            }

            PublishedSenderBundle bundle;
            bundle.screen_share = task.screen_share;
            webrtc::scoped_refptr<webrtc::RtpSenderInterface> sender;
            webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver;
            std::string error;
            // Reuse only senders retired by this room's screen publications.
            // AddTrack could instead take a downstream recvonly transceiver.
            webrtc::RtpTransceiverInit init;
            init.direction = webrtc::RtpTransceiverDirection::kSendOnly;
            init.stream_ids = {task.stream_id};
            if (task.rtc_track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind &&
                !task.publish_options.layers.empty()) {
                const bool primary_active =
                    task.publish_options.simulcast_codecs.size() <= 1 ||
                    task.publish_options.backup_codec_policy != BackupCodecPolicy::Regression;
                const bool h264_primary =
                    CompareCodecName(task.publish_options.video_codec.c_str(), "h264") == 0;
                for (const auto& layer : task.publish_options.layers) {
                    webrtc::RtpEncodingParameters encoding;
                    encoding.active = primary_active;
                    encoding.rid = layer.rid;
                    encoding.scale_resolution_down_by = layer.scale_resolution_down_by;
                    if (h264_primary && layer.width > 0 && layer.height > 0) {
                        encoding.scale_resolution_down_to =
                            webrtc::Resolution{layer.width, layer.height};
                    }
                    encoding.max_bitrate_bps = layer.max_bitrate_bps;
                    encoding.max_framerate = layer.max_fps;
                    if ((task.screen_share && CompareCodecName(task.publish_options.video_codec.c_str(), "h265") == 0) ||
                        CompareCodecName(task.publish_options.video_codec.c_str(), "h264") == 0)
                        encoding.scale_resolution_down_to = webrtc::Resolution{layer.width, layer.height};
                    init.send_encodings.push_back(std::move(encoding));
                }
            } else if (task.rtc_track->kind() ==
                           webrtc::MediaStreamTrackInterface::kAudioKind &&
                       task.audio_publish_policy.max_bitrate_bps > 0) {
                webrtc::RtpEncodingParameters encoding;
                encoding.max_bitrate_bps =
                    task.audio_publish_policy.max_bitrate_bps;
                init.send_encodings.push_back(std::move(encoding));
            }
            if (error.empty()) {
                auto result = task.pc->AddTransceiver(task.rtc_track, init);
                if (result.ok()) {
                    transceiver = result.MoveValue();
                    sender = transceiver->sender();
                } else {
                    error = result.error().message();
                }
            }
            if (sender) {
                bundle.primary = sender;
                bundle.senders.push_back(sender);
                bundle.scalability_modes.push_back(
                    task.publish_options.video_codec == "vp9" && task.publish_options.scalability_mode.empty() &&
                    task.publish_options.layers.size() > 1 ? "L1T1" : task.publish_options.scalability_mode);
                bundle.track_ids.push_back(task.rtc_track->id());
            }

            if (!sender) {
                FailAwaitable(task.completion, std::make_exception_ptr(OperationError(
                    OperationKind::PublishTrack,
                    OperationErrorCode::StateUncertain,
                    "install_sender",
                    error.empty() ? "WebRTC did not return an RTP sender" : error,
                    true)));
                return;
            }
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
            if (task.rtc_track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
                auto factory = task.linux_encoder_factory ? task.linux_encoder_factory :
                    CreateLinuxPublicationVideoEncoderFactory("software", {});
                const auto installed = sender->SetVideoEncoderFactory(std::move(factory));
                if (!installed.ok()) {
                    task.room->RollbackPublishedSenderBundle(task.pc, bundle);
                    FailAwaitable(task.completion, std::make_exception_ptr(OperationError(
                        OperationKind::PublishTrack, OperationErrorCode::TrackPublishRejected,
                        "install_encoder_policy", "immutable video encoder policy could not be installed")));
                    return;
                }
            }
#endif
            if (transceiver &&
                task.rtc_track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind &&
                !task.publish_options.video_codec.empty()) {
                auto get_prefs = [&](const std::string& codec_name) {
                    std::string selected = codec_name;
                    std::transform(selected.begin(), selected.end(), selected.begin(),
                                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                    auto capabilities = WebRTCManager::Instance().factory()->GetRtpSenderCapabilities(
                        webrtc::MediaType::VIDEO);
                    std::vector<webrtc::RtpCodecCapability> preferences;
                    std::set<int> selected_payload_types;
                    for (const auto& codec : capabilities.codecs) {
                        std::string name = codec.name;
                        std::transform(name.begin(), name.end(), name.begin(),
                                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                        if (name == selected) {
                            if (name == "h264" && !task.publish_options.preflight_h264_formats.empty() &&
                                std::none_of(task.publish_options.preflight_h264_formats.begin(),
                                    task.publish_options.preflight_h264_formats.end(), [&](const auto& approved) {
                                        return ExactH264Format(approved,
                                            webrtc::SdpVideoFormat(codec.name, codec.parameters));
                                    })) {
                                continue;
                            }
                            if (task.publish_options.auto_hardware_h264 &&
                                name == "h264" && !IsHardwareH264FormatSupported(
                                    webrtc::SdpVideoFormat(codec.name, codec.parameters), task.screen_share)) {
                                continue;
                            }
                            preferences.push_back(codec);
                            if (codec.preferred_payload_type) {
                                selected_payload_types.insert(*codec.preferred_payload_type);
                            }
                        }
                    }
                    for (const auto& codec : capabilities.codecs) {
                        if (!codec.IsResiliencyCodec()) continue;
                        std::string name = codec.name;
                        std::transform(name.begin(), name.end(), name.begin(),
                                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                        if (name == "rtx") {
                            const auto apt = codec.parameters.find("apt");
                            if (apt == codec.parameters.end()) continue;
                            try {
                                if (!selected_payload_types.contains(std::stoi(apt->second))) continue;
                            } catch (...) {
                                continue;
                            }
                        }
                        preferences.push_back(codec);
                    }
                    return preferences;
                };

                auto preferences = get_prefs(task.publish_options.video_codec);
                if (preferences.empty()) {
                    task.room->RollbackPublishedSenderBundle(task.pc, bundle);
                    FailAwaitable(task.completion, std::make_exception_ptr(OperationError(
                        OperationKind::PublishTrack,
                        OperationErrorCode::InvalidState,
                        "apply_join_codec_policy",
                        "resolved primary codec has no sender preference")));
                    return;
                }
                const auto append_prefs = [&](const std::string& codec_name) {
                    for (const auto& codec : get_prefs(codec_name)) {
                        const auto duplicate = std::any_of(preferences.begin(), preferences.end(),
                            [&codec](const auto& current) {
                                return current.name == codec.name &&
                                    current.parameters == codec.parameters &&
                                    current.preferred_payload_type == codec.preferred_payload_type;
                            });
                        if (!duplicate) preferences.push_back(codec);
                    }
                };
                // LiveKit derives the potential upstream codecs from the
                // primary receiver's negotiated codec list. Keep the primary
                // first, but also advertise the resolved backup codecs here:
                // a subscriber can bind before the backup RTP arrives and
                // must still be able to map a later codec regression.
                for (size_t index = 1; index < task.publish_options.simulcast_codecs.size(); ++index) {
                    append_prefs(task.publish_options.simulcast_codecs[index].codec);
                }
                auto codec_status = transceiver->SetCodecPreferences(preferences);
                if (!codec_status.ok()) {
                    task.room->RollbackPublishedSenderBundle(task.pc, bundle);
                    FailAwaitable(task.completion, std::make_exception_ptr(OperationError(
                        OperationKind::PublishTrack,
                        OperationErrorCode::StateUncertain,
                        "apply_join_codec_policy",
                        codec_status.message(),
                        true)));
                    return;
                }
                try {
                    for (const auto& codec : preferences) {
                        if (auto row = VideoFormatRow(codec.name, codec.parameters,
                            diagnostic::VideoFormatSource::CodecPreference, codec.preferred_payload_type.value_or(-1))) {
                            task.room->EmitPublisherVideoFormat(*row, task.generation, {}, diagnostic::ThreadRole::Rtc);
                        }
                    }
                } catch (...) {}
                // GAP-03: Multi-Codec Simulcast & Backup Codecs Transceiver
                if (task.publish_options.simulcast_codecs.size() > 1) {
                    for (size_t c_idx = 1; c_idx < task.publish_options.simulcast_codecs.size(); ++c_idx) {
                        const auto& backup_spec = task.publish_options.simulcast_codecs[c_idx];
                        if (backup_spec.layers.empty()) {
                            task.room->RollbackPublishedSenderBundle(task.pc, bundle);
                            FailAwaitable(task.completion, std::make_exception_ptr(OperationError(
                                OperationKind::PublishTrack,
                                OperationErrorCode::InvalidState,
                                "install_backup_sender",
                                "resolved backup codec has no RTP encodings")));
                            return;
                        }

                        webrtc::RtpTransceiverInit backup_init;
                        backup_init.direction = webrtc::RtpTransceiverDirection::kSendOnly;
                        backup_init.stream_ids = {task.stream_id};
                        const bool is_active =
                            task.publish_options.backup_codec_policy !=
                            BackupCodecPolicy::PreferRegression;

                        const bool h264_backup =
                            CompareCodecName(backup_spec.codec.c_str(), "h264") == 0;
                        for (const auto& layer : backup_spec.layers) {
                            webrtc::RtpEncodingParameters encoding;
                            encoding.active = is_active;
                            encoding.rid = layer.rid;
                            encoding.scale_resolution_down_by = layer.scale_resolution_down_by;
                            if (h264_backup && layer.width > 0 && layer.height > 0) {
                                encoding.scale_resolution_down_to =
                                    webrtc::Resolution{layer.width, layer.height};
                            }
                            encoding.max_bitrate_bps = layer.max_bitrate_bps;
                            encoding.max_framerate = layer.max_fps;
                            if (CompareCodecName(backup_spec.codec.c_str(), "h264") == 0)
                                encoding.scale_resolution_down_to = webrtc::Resolution{layer.width, layer.height};
                            backup_init.send_encodings.push_back(std::move(encoding));
                        }

                        const auto backup_cid = backup_spec.cid.empty()
                            ? task.rtc_track->id() + "_backup" +
                                (c_idx == 1 ? std::string{} : "_" + std::to_string(c_idx))
                            : backup_spec.cid;
                        auto* source_track = static_cast<webrtc::VideoTrackInterface*>(
                            task.rtc_track.get());
                        webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface>
                            backup_source(source_track->GetSource());
                        auto backup_track = WebRTCManager::Instance().factory()->CreateVideoTrack(
                            std::move(backup_source), backup_cid);
                        if (!backup_track) {
                            task.room->RollbackPublishedSenderBundle(task.pc, bundle);
                            FailAwaitable(task.completion, std::make_exception_ptr(OperationError(
                                OperationKind::PublishTrack,
                                OperationErrorCode::StateUncertain,
                                "install_backup_sender",
                                "failed to create the backup native video track",
                                true)));
                            return;
                        }
                        backup_track->set_enabled(task.rtc_track->enabled());
                        auto backup_res = task.pc->AddTransceiver(
                            backup_track, backup_init);
                        if (!backup_res.ok()) {
                            task.room->RollbackPublishedSenderBundle(task.pc, bundle);
                            FailAwaitable(task.completion, std::make_exception_ptr(OperationError(
                                OperationKind::PublishTrack,
                                OperationErrorCode::StateUncertain,
                                "install_backup_sender",
                                backup_res.error().message(),
                                true)));
                            return;
                        }
                        auto backup_transceiver = backup_res.MoveValue();
                        auto backup_sender = backup_transceiver->sender();
                        if (!backup_sender) {
                            task.room->RollbackPublishedSenderBundle(task.pc, bundle);
                            FailAwaitable(task.completion, std::make_exception_ptr(OperationError(
                                OperationKind::PublishTrack,
                                OperationErrorCode::StateUncertain,
                                "install_backup_sender",
                                "WebRTC did not return a backup RTP sender",
                                true)));
                            return;
                        }
                        bundle.senders.push_back(backup_sender);
                        bundle.scalability_modes.push_back(
                            backup_spec.codec == "vp9" && backup_spec.scalability_mode.empty() &&
                            backup_spec.layers.size() > 1 ? "L1T1" : backup_spec.scalability_mode);
                        bundle.track_ids.push_back(backup_cid);
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
                        // Backup layers have no independent hardware preflight.
                        // Never let a delayed backup Create adopt fresh globals.
                        const auto installed = backup_sender->SetVideoEncoderFactory(
                            CreateLinuxPublicationVideoEncoderFactory("software", {}));
                        if (!installed.ok()) {
                            task.room->RollbackPublishedSenderBundle(task.pc, bundle);
                            FailAwaitable(task.completion, std::make_exception_ptr(OperationError(
                                OperationKind::PublishTrack, OperationErrorCode::TrackPublishRejected,
                                "install_backup_encoder_policy", "immutable backup encoder policy could not be installed")));
                            return;
                        }
#endif

                        auto backup_prefs = get_prefs(backup_spec.codec);
                        if (backup_prefs.empty()) {
                            task.room->RollbackPublishedSenderBundle(task.pc, bundle);
                            FailAwaitable(task.completion, std::make_exception_ptr(OperationError(
                                OperationKind::PublishTrack,
                                OperationErrorCode::InvalidState,
                                "apply_backup_codec_policy",
                                "resolved backup codec has no sender preference")));
                            return;
                        }
                        const auto backup_status =
                            backup_transceiver->SetCodecPreferences(backup_prefs);
                        if (!backup_status.ok()) {
                            task.room->RollbackPublishedSenderBundle(task.pc, bundle);
                            FailAwaitable(task.completion, std::make_exception_ptr(OperationError(
                                OperationKind::PublishTrack,
                                OperationErrorCode::StateUncertain,
                                "apply_backup_codec_policy",
                                backup_status.message(),
                                true)));
                            return;
                        }
                        try {
                            for (const auto& codec : backup_prefs) {
                                if (auto row = VideoFormatRow(codec.name, codec.parameters,
                                    diagnostic::VideoFormatSource::CodecPreference, codec.preferred_payload_type.value_or(-1))) {
                                    task.room->EmitPublisherVideoFormat(*row, task.generation, {}, diagnostic::ThreadRole::Rtc);
                                }
                            }
                        } catch (...) {}
                        task.room->Log("TRACK", "BACKUP_CODEC_ATTACH",
                                       "Attached backup codec transceiver to video track: Codec=" + backup_spec.codec +
                                       ", Layers=" + std::to_string(backup_spec.layers.size()) +
                                       ", Policy=" + (is_active ? "active" : "prefer-regression"));
                    }
                }
            }
            if (task.generation != task.room->session_generation_.load(std::memory_order_acquire)) {
                task.room->RollbackPublishedSenderBundle(task.pc, bundle);
                FailAwaitable(task.completion, std::make_exception_ptr(OperationError(
                    OperationKind::PublishTrack,
                    OperationErrorCode::Cancelled,
                    "install_sender",
                    "session changed during sender installation")));
                return;
            }
            if (task.require_encryption) {
                try {
                    for (const auto& protected_sender : bundle.senders) {
                        auto cryptor = task.encryption->CreateSenderCryptor(
                            WebRTCManager::Instance().signaling_thread(), task.encryption_identity,
                            protected_sender->track()->id(), task.rtc_track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind ? task.publish_options.video_codec : "opus",
                            task.generation, protected_sender == bundle.primary &&
                                task.publish_options.auto_hardware_h264 &&
                                task.publish_options.allow_vp8_fallback);
                        if (!cryptor) throw std::runtime_error("media cryptor unavailable");
                        protected_sender->SetFrameTransformer(std::move(cryptor));
                    }
                } catch (...) {
                    task.room->RollbackPublishedSenderBundle(task.pc, bundle);
                    FailAwaitable(task.completion, std::make_exception_ptr(OperationError(
                        OperationKind::PublishTrack, OperationErrorCode::EncryptionFailed,
                        "e2ee_sender_binding", "media protection could not be installed; retry with a valid key", false)));
                    return;
                }
            }
            PublishedSenderBundle completed = bundle;
            if (!CompleteAwaitable(task.completion, std::move(completed))) {
                task.room->RollbackPublishedSenderBundle(task.pc, bundle);
            }
        });

    co_return co_await WaitAwaitable<PublishedSenderBundle>(
        completion,
        operation_timeouts_.publish,
        OperationKind::PublishTrack,
        OperationErrorCode::TrackPublishTimeout,
        "install_sender");
}

ScreenShareQualityConstraint EvaluateScreenShareCodecCeiling(
    const std::string& requested_codec,
    const std::string& encoder_preference,
    int fps,
    const std::vector<std::string>& server_enabled_codecs,
    bool encrypted) {
    VideoPublishOptions opts;
    opts.source = TrackSource::ScreenShareVideo;
    opts.video_codec = requested_codec.empty() ? std::string("auto") : requested_codec;
    auto sender_codecs = LocalVideoSenderCodecs();
    if (encrypted && (opts.video_codec.empty() ||
        CompareCodecName(opts.video_codec.c_str(), "auto") == 0)) {
        std::erase_if(sender_codecs, [](const std::string& codec) {
            return CompareCodecName(codec.c_str(), "vp8") != 0 &&
                CompareCodecName(codec.c_str(), "h264") != 0 &&
                CompareCodecName(codec.c_str(), "vp9") != 0;
        });
    }
    const auto hardware = GetVideoEncoderCapabilities();
    const auto pref = NormalizePreferenceToken(encoder_preference);
    const bool prefer_hardware_h264 = HardwareH264AvailableForPreference(pref, hardware);
    const auto plan = LocalVideoTrack::ResolvePublishPlan(
        2560, 1440, opts, sender_codecs, server_enabled_codecs, prefer_hardware_h264);
    if (!plan.ok()) return {};
    if (plan.effective_codec == "h264") {
        if (pref == "software" || !prefer_hardware_h264) {
            if (const auto level = SoftwareH264SendLevel(true)) {
                return EvaluateH264ScreenShareConstraint(
                    *level, fps, ScreenShareConstraintReason::H264SoftwareLevel);
            }
        } else if (const auto level = PredictedHardwareH264ScreenShareLevel(pref, hardware)) {
            return EvaluateH264ScreenShareConstraint(
                *level, fps, ScreenShareConstraintReason::H264HardwareLevel);
        }
        return ScreenShareQualityConstraint{ScreenShareConstraintReason::None, "h264"};
    }
    if (plan.effective_codec == "h265") {
        if (const auto level = LocalH265SendLevel()) {
            return EvaluateH265ScreenShareConstraint(*level, fps);
        }
        return ScreenShareQualityConstraint{ScreenShareConstraintReason::None, "h265"};
    }
    return ScreenShareQualityConstraint{ScreenShareConstraintReason::None, plan.effective_codec};
}

asio::awaitable<ScreenShareFrameProfile> Room::ConstrainScreenShareProfileAsync(
    std::shared_ptr<LocalVideoTrack> track, ScreenShareFrameProfile profile) {
    const auto resolved = co_await ConstrainScreenShareProfileWithDetailAsync(std::move(track), profile);
    co_return resolved.profile;
}

asio::awaitable<ResolvedScreenShareQuality> Room::ConstrainScreenShareProfileWithDetailAsync(
    std::shared_ptr<LocalVideoTrack> track, ScreenShareFrameProfile profile) {
    auto admission = AdmitOperation(OperationKind::PublishTrack, "resolve_screen_quality");
    if (!track || track->Track::source() != TrackSource::ScreenShareVideo ||
        !profile.quality.valid() || profile.width < 2 || profile.height < 2)
        throw OperationError(OperationKind::PublishTrack, OperationErrorCode::InvalidState,
            "resolve_screen_quality", "invalid screen profile");
    const auto generation = session_generation_.load(std::memory_order_acquire);
    const auto revision = track->publish_options_revision();
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
    std::shared_ptr<TrackPublication> publication;
    std::vector<std::string> ids;
    std::vector<std::string> enabled_codecs;
    bool encrypted = false;
    {
        std::lock_guard lock(room_mutex_);
        if (!IsNativeGenerationCurrentLocked(generation) || connection_state_ != ConnectionState::Connected)
            throw OperationError(OperationKind::PublishTrack, OperationErrorCode::Cancelled,
                "resolve_screen_quality", "screen session retired");
        pc = publisher_pc_;
        enabled_codecs = enabled_publish_codecs_;
        encrypted = e2ee_manager_ && e2ee_manager_->enabled();
        if (local_participant_) for (const auto& [sid, item] : local_participant_->tracks()) {
            if (item && item->track() == track) { publication = item; break; }
        }
        const auto found = published_sender_track_ids_.find(track.get());
        if (found != published_sender_track_ids_.end()) ids = found->second;
    }
    if (!publication) {
        // Resolve before publication so the first encoder input already uses
        // the constrained capture profile. The final negotiated format is
        // checked independently before publication commit.
        const auto plan = ResolveVideoPublishPlan(track, enabled_codecs, encrypted);
        if (!plan.ok()) throw OperationError(OperationKind::PublishTrack,
            OperationErrorCode::CodecUnsupported, "resolve_screen_quality", "screen codec unavailable");
        if (plan.effective_codec == "h264") {
            const auto preference = GetVideoEncoderPreference();
            const auto hardware = GetVideoEncoderCapabilities();
            const bool use_software = preference == "software" ||
                !HardwareH264AvailableForPreference(preference, hardware);
            if (use_software) {
                const auto level = SoftwareH264SendLevel(true);
                const auto effective = level ? detail::ConstrainH264ScreenShareProfile(profile, *level) : std::nullopt;
                if (!effective) RejectH264Profile();
                co_return ResolvedScreenShareQuality{
                    *effective,
                    EvaluateH264ScreenShareConstraint(*level, effective->quality.fps,
                        ScreenShareConstraintReason::H264SoftwareLevel)};
            }
            if (const auto level = MaxH264FormatLevel(LocalHardwareH264Formats(true))) {
                const auto effective = detail::ConstrainH264ScreenShareProfile(profile, *level);
                if (!effective) RejectH264Profile();
                co_return ResolvedScreenShareQuality{
                    *effective,
                    EvaluateH264ScreenShareConstraint(*level, effective->quality.fps,
                        ScreenShareConstraintReason::H264HardwareLevel)};
            }
            co_return ResolvedScreenShareQuality{
                profile, ScreenShareQualityConstraint{ScreenShareConstraintReason::None, "h264"}};
        }
        if (plan.effective_codec != "h265") {
            co_return ResolvedScreenShareQuality{
                profile, ScreenShareQualityConstraint{ScreenShareConstraintReason::None, plan.effective_codec}};
        }
        const auto level = LocalH265SendLevel();
        const auto effective = level ? detail::ConstrainH265ScreenShareProfile(profile, *level) : std::nullopt;
        if (!effective) RejectH265Profile();
        co_return ResolvedScreenShareQuality{
            *effective,
            EvaluateH265ScreenShareConstraint(*level, effective->quality.fps)};
    }

    auto* signaling = WebRTCManager::Instance().signaling_thread();
    if (!pc || !signaling || signaling->IsQuitting())
        throw OperationError(OperationKind::PublishTrack, OperationErrorCode::Cancelled,
            "resolve_screen_quality", "screen publisher unavailable");
    auto completion = std::make_shared<AwaitableState<ResolvedScreenShareQuality>>(executor_);
    const auto gate = callback_gate_;
    auto cancel = gate->CancelWhenClosed([weak = std::weak_ptr(completion), weak_gate = std::weak_ptr(gate)] {
        if (auto live = weak_gate.lock()) live->PostCompletion([weak] {
            if (auto pending = weak.lock()) FailAwaitable(pending, std::make_exception_ptr(OperationError(
                OperationKind::PublishTrack, OperationErrorCode::Cancelled,
                "resolve_screen_quality", "screen session retired")));
        });
    });
    struct Task {
        std::shared_ptr<Room> room;
        std::shared_ptr<AwaitableState<ResolvedScreenShareQuality>> completion;
        std::shared_ptr<ExecutorCallbackGate> gate;
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
        std::shared_ptr<LocalVideoTrack> track;
        std::shared_ptr<TrackPublication> publication;
        std::vector<std::string> ids;
        ScreenShareFrameProfile profile;
        uint64_t generation;
        ExecutorCallbackGate::Ticket ticket;
    };
    auto* task = new Task{shared_from_this(), completion, gate, pc, track, publication,
        std::move(ids), profile, generation, AdmitOperation(OperationKind::PublishTrack, "resolve_screen_quality")};
    try {
        signaling->PostTask([task] {
            std::unique_ptr<Task> owned(task);
            try {
                {
                    std::lock_guard lock(task->completion->mutex);
                    if (task->completion->completed) return;
                }
                const auto current = [&] {
                    std::lock_guard lock(task->room->room_mutex_);
                    const auto local = task->room->local_participant_;
                    return task->room->IsNativeGenerationCurrentLocked(task->generation) &&
                        task->room->connection_state_ == ConnectionState::Connected &&
                        task->room->publisher_pc_ == task->pc && local &&
                        local->get_publication(task->publication->sid()) == task->publication &&
                        task->publication->track() == task->track;
                };
                if (!current()) throw OperationError(OperationKind::PublishTrack, OperationErrorCode::Cancelled,
                    "resolve_screen_quality", "screen publication retired");
                ResolvedScreenShareQuality resolved{task->profile, {}};
                bool found = false;
                for (const auto& sender : task->pc->GetSenders()) {
                    if (!sender || !sender->track()) continue;
                    if (sender->track() != task->track->rtc_track() &&
                        std::find(task->ids.begin(), task->ids.end(), sender->track()->id()) == task->ids.end()) continue;
                    found = true;
                    const auto parameters = sender->GetParameters();
                    if (resolved.constraint.codec.empty() && !parameters.codecs.empty()) {
                        resolved.constraint.codec = NormalizePreferenceToken(parameters.codecs.front().name);
                    }
                    if (const auto level = SenderH264Level(parameters)) {
                        auto effective_level = *level;
                        auto reason = ScreenShareConstraintReason::H264NegotiatedLevel;
                        if (GetVideoEncoderPreference() == "software") {
                            if (const auto sw_level = SoftwareH264SendLevel(true);
                                sw_level && *sw_level <= effective_level) {
                                effective_level = *sw_level;
                                reason = ScreenShareConstraintReason::H264SoftwareLevel;
                            }
                        }
                        const auto constrained = detail::ConstrainH264ScreenShareProfile(resolved.profile, effective_level);
                        if (!constrained) RejectH264Profile();
                        resolved.profile = *constrained;
                        resolved.constraint = EvaluateH264ScreenShareConstraint(
                            effective_level, resolved.profile.quality.fps, reason);
                    }
                    if (const auto level = SenderH265Level(parameters,
                            CompareCodecName(task->track->requested_publish_options().video_codec.c_str(), "h265") == 0)) {
                        const auto constrained = detail::ConstrainH265ScreenShareProfile(resolved.profile, *level);
                        if (!constrained) RejectH265Profile();
                        resolved.profile = *constrained;
                        resolved.constraint = EvaluateH265ScreenShareConstraint(
                            *level, resolved.profile.quality.fps);
                    }
                }
                if (!found || !current()) throw OperationError(OperationKind::PublishTrack, OperationErrorCode::Cancelled,
                    "resolve_screen_quality", "screen sender retired");
                task->gate->PostCompletion([completion = task->completion, resolved] {
                    CompleteAwaitable(completion, resolved);
                });
            } catch (...) {
                task->gate->PostCompletion([completion = task->completion, error = std::current_exception()] {
                    FailAwaitable(completion, error);
                });
            }
        });
    } catch (...) { delete task; throw; }
    auto effective = co_await WaitAwaitable<ResolvedScreenShareQuality>(completion, operation_timeouts_.publish,
        OperationKind::PublishTrack, OperationErrorCode::TrackPublishTimeout, "resolve_screen_quality");
    {
        std::lock_guard lock(room_mutex_);
        if (!IsNativeGenerationCurrentLocked(generation) || connection_state_ != ConnectionState::Connected ||
            publisher_pc_ != pc || !local_participant_ ||
            local_participant_->get_publication(publication->sid()) != publication || publication->track() != track ||
            revision != track->publish_options_revision())
            throw OperationError(OperationKind::PublishTrack, OperationErrorCode::Cancelled,
                "resolve_screen_quality", "screen publication changed during quality resolution");
    }
    co_return effective;
}

asio::awaitable<void> Room::ApplyScreenShareSenderParametersAsync(
    std::shared_ptr<LocalVideoTrack> track, ScreenShareFrameProfile profile) {
    auto admission = AdmitOperation(OperationKind::PublishTrack, "screen_quality");
    if (!track || track->Track::source() != TrackSource::ScreenShareVideo ||
        !profile.quality.valid() || profile.width < 2 || profile.height < 2)
        throw OperationError(OperationKind::PublishTrack, OperationErrorCode::InvalidState,
                             "screen_quality", "invalid screen profile");
    const auto generation = session_generation_.load(std::memory_order_acquire);
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
    std::shared_ptr<TrackPublication> publication;
    std::vector<std::string> ids;
    {
        std::lock_guard lock(room_mutex_);
        pc = publisher_pc_;
        if (local_participant_) for (const auto& [sid, item] : local_participant_->tracks()) {
            if (item && item->track() == track) { publication = item; break; }
        }
        auto found = published_sender_track_ids_.find(track.get());
        if (found != published_sender_track_ids_.end()) ids = found->second;
        if (!IsNativeGenerationCurrentLocked(generation) || connection_state_ != ConnectionState::Connected)
            publication.reset();
    }
    auto* signaling = WebRTCManager::Instance().signaling_thread();
    if (!pc || !publication || !signaling || signaling->IsQuitting())
        throw OperationError(OperationKind::PublishTrack, OperationErrorCode::Cancelled,
                             "screen_quality", "screen publication is unavailable");
    auto completion = std::make_shared<AwaitableState<void>>(executor_);
    auto cancel = callback_gate_->CancelWhenClosed([weak = std::weak_ptr(completion)] {
        if (auto pending = weak.lock()) FailAwaitable(pending, std::make_exception_ptr(OperationError(
            OperationKind::PublishTrack, OperationErrorCode::Cancelled, "screen_quality", "room retired")));
    });
    struct Task {
        std::shared_ptr<Room> room;
        std::shared_ptr<AwaitableState<void>> completion;
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
        std::shared_ptr<LocalVideoTrack> track;
        std::shared_ptr<TrackPublication> publication;
        std::vector<std::string> ids;
        ScreenShareFrameProfile profile;
        uint64_t generation;
        ExecutorCallbackGate::Ticket ticket;
    };
    auto* task = new Task{shared_from_this(), completion, pc, track, publication, ids, profile, generation,
        AdmitOperation(OperationKind::PublishTrack, "screen_quality")};
    try { signaling->PostTask([task] {
        std::unique_ptr<Task> owned(task);
        {
            std::lock_guard lock(task->completion->mutex);
            if (task->completion->completed) return;
        }
        auto current = [&] {
            std::lock_guard lock(task->room->room_mutex_);
            auto local = task->room->local_participant_;
            return task->room->IsNativeGenerationCurrentLocked(task->generation) &&
                task->room->connection_state_ == ConnectionState::Connected &&
                task->room->publisher_pc_ == task->pc && local &&
                local->get_publication(task->publication->sid()) == task->publication &&
                task->publication->track() == task->track;
        };
        struct Saved {
            webrtc::scoped_refptr<webrtc::RtpSenderInterface> sender;
            webrtc::RtpParameters parameters;
        };
        std::vector<Saved> changed;
        auto restore = [&] {
            if (!current()) return false;
            bool ok = true;
            for (auto& entry : changed) {
                if (!current()) return false;
                auto fresh = entry.sender->GetParameters();
                if (fresh.encodings.size() != entry.parameters.encodings.size()) { ok = false; continue; }
                for (size_t i = 0; i < fresh.encodings.size(); ++i) {
                    fresh.encodings[i].max_framerate = entry.parameters.encodings[i].max_framerate;
                    fresh.encodings[i].max_bitrate_bps = entry.parameters.encodings[i].max_bitrate_bps;
                    fresh.encodings[i].scale_resolution_down_to = entry.parameters.encodings[i].scale_resolution_down_to;
                }
                ok = entry.sender->SetParameters(fresh).ok() && ok;
            }
            return ok;
        };
        try {
            if (!current()) throw std::runtime_error("retired screen publication");
            for (auto& sender : task->pc->GetSenders()) {
                if (!sender || !sender->track()) continue;
                if (sender->track() != task->track->rtc_track() &&
                    std::find(task->ids.begin(), task->ids.end(), sender->track()->id()) == task->ids.end()) continue;
                auto parameters = sender->GetParameters();
                if (parameters.encodings.empty()) throw std::runtime_error("screen encoding unavailable");
                auto options = task->track->requested_publish_options();
                options.screen_share_fps = task->profile.quality.fps;
                if (!parameters.codecs.empty()) options.video_codec = parameters.codecs[0].name;
                // Compute the full-resolution budget independently of the frozen RID topology.
                options.simulcast = false;
                const auto h265_level = SenderH265Level(parameters, CompareCodecName(options.video_codec.c_str(), "h265") == 0);
                const auto h264_level = SenderH264Level(parameters);
                if (h265_level) options.video_codec = "h265";
                auto budget = LocalVideoTrack::ComputeSimulcastOptions(task->profile.width, task->profile.height, options).layers.front();
                if (h265_level) {
                    if (parameters.encodings.size() != 1) RejectH265Profile();
                    const auto* limits = webrtc::external_encoder::FindAmfHevcLevelLimits(*h265_level);
                    budget.max_bitrate_bps = std::min(budget.max_bitrate_bps,
                        static_cast<int>(limits->max_main_tier_bitrate_bps));
                    if (detail::CheckH265LevelConstraints(*h265_level, task->profile.width, task->profile.height,
                            budget.max_fps, budget.max_bitrate_bps) != detail::H265LevelConstraintStatus::Supported)
                        RejectH265Profile();
                }
                changed.push_back({sender, parameters});
                if (h264_level && detail::CheckH264LevelConstraints(*h264_level,
                        task->profile.width, task->profile.height, budget.max_fps) !=
                            detail::H264LevelConstraintStatus::Supported) RejectH264Profile();
                for (auto& encoding : parameters.encodings) {
                    const bool low = parameters.encodings.size() > 1 && encoding.scale_resolution_down_by.value_or(1.0) > 1.0;
                    encoding.max_framerate = low ? 3 : budget.max_fps;
                    encoding.max_bitrate_bps = low ? std::max(150000, budget.max_bitrate_bps / (4 * (task->profile.quality.fps / 3))) : budget.max_bitrate_bps;
                    if (h265_level) encoding.scale_resolution_down_to =
                        webrtc::Resolution{task->profile.width, task->profile.height};
                    else if (h264_level) {
                        const double scale = encoding.scale_resolution_down_by.value_or(1.0);
                        if (!std::isfinite(scale) || scale < 1.0) RejectH264Profile();
                        encoding.scale_resolution_down_to = webrtc::Resolution{
                            std::max(2, static_cast<int>(task->profile.width / scale) & ~1),
                            std::max(2, static_cast<int>(task->profile.height / scale) & ~1)};
                    }
                }
                if (!current()) throw std::runtime_error("screen sender retired");
                if (!sender->SetParameters(parameters).ok()) throw std::runtime_error("screen sender rejected quality");
            }
            if (changed.empty() || !current()) throw std::runtime_error("screen sender retired");
            if (!CompleteAwaitable(task->completion)) restore();
        } catch (...) {
            const bool restored = restore();
            FailAwaitable(task->completion, std::make_exception_ptr(OperationError(
                OperationKind::PublishTrack, restored ? OperationErrorCode::InvalidState : OperationErrorCode::StateUncertain,
                "screen_quality", restored ? "quality update rejected" : "quality compensation failed", !restored)));
        }
    }); } catch (...) { delete task; throw; }
    co_await WaitAwaitable<void>(completion, operation_timeouts_.publish,
        OperationKind::PublishTrack, OperationErrorCode::StateUncertain, "screen_quality");
}

asio::awaitable<void> Room::SyncScreenShareMetadataAsync(
    std::shared_ptr<LocalVideoTrack> track, ScreenShareFrameProfile profile) {
    auto admission = AdmitOperation(OperationKind::PublishTrack, "screen_quality_metadata");
    if (!track || track->Track::source() != TrackSource::ScreenShareVideo ||
        !profile.quality.valid() || profile.width < 2 || profile.height < 2)
        throw OperationError(OperationKind::PublishTrack, OperationErrorCode::InvalidState,
                             "screen_quality_metadata", "invalid screen profile");
    std::shared_ptr<SignalClient> signal;
    std::shared_ptr<TrackPublication> publication;
    const auto generation = session_generation_.load(std::memory_order_acquire);
    {
        std::lock_guard lock(room_mutex_);
        if (local_participant_) for (const auto& [sid, item] : local_participant_->tracks()) {
            if (item && item->track() == track) { publication = item; break; }
        }
        if (!IsNativeGenerationCurrentLocked(generation) || connection_state_ != ConnectionState::Connected)
            publication.reset();
        signal = signal_client_;
    }
    if (!publication || !signal || !profile.quality.valid())
        throw OperationError(OperationKind::PublishTrack, OperationErrorCode::Cancelled,
                             "screen_quality_metadata", "screen publication retired");
    proto::SignalRequest request;
    auto* update = request.mutable_update_video_track();
    update->set_track_sid(publication->sid());
    update->set_width(profile.width);
    update->set_height(profile.height);
    co_await signal->SendAsync(request);
    // Send completion is local transport delivery, not a server acknowledgement.
    std::lock_guard lock(room_mutex_);
    if (!IsNativeGenerationCurrentLocked(generation) || signal_client_ != signal ||
        !local_participant_ || local_participant_->get_publication(publication->sid()) != publication)
        throw OperationError(OperationKind::PublishTrack, OperationErrorCode::Cancelled,
                             "screen_quality_metadata", "screen publication changed during sync");
}

asio::awaitable<void> Room::ApplyPublishedSenderScalabilityModesAsync(
    PublishedSenderBundle bundle,
    uint64_t generation) {
    if (bundle.senders.size() != bundle.scalability_modes.size() ||
        bundle.senders.size() != bundle.track_ids.size()) {
        throw OperationError(OperationKind::PublishTrack,
                             OperationErrorCode::InvalidState,
                             "apply_scalability_mode",
                             "sender bundle metadata is inconsistent");
    }
    if (std::none_of(
            bundle.scalability_modes.begin(),
            bundle.scalability_modes.end(),
            [](const std::string& mode) { return !mode.empty(); })) {
        co_return;
    }

    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
    {
        std::lock_guard lock(room_mutex_);
        if (!IsNativeGenerationCurrentLocked(generation)) {
            throw OperationError(OperationKind::PublishTrack,
                                 OperationErrorCode::Cancelled,
                                 "apply_scalability_mode",
                                 "room session changed before applying SVC mode");
        }
        pc = publisher_pc_;
    }
    if (!pc || !WebRTCManager::Instance().signaling_thread()) {
        throw OperationError(OperationKind::PublishTrack,
                             OperationErrorCode::InvalidState,
                             "apply_scalability_mode",
                             "publisher peer connection is unavailable");
    }

    auto completion = std::make_shared<AwaitableState<void>>(executor_);
    auto cancel_native = callback_gate_->CancelWhenClosed(
        [weak = std::weak_ptr(completion)] {
            if (auto pending = weak.lock()) {
                FailAwaitable(pending, std::make_exception_ptr(OperationError(
                    OperationKind::PublishTrack,
                    OperationErrorCode::Cancelled,
                    "apply_scalability_mode",
                    "room is retired")));
            }
        });
    struct ApplyScalabilityTaskParams {
        std::shared_ptr<Room> room;
        std::shared_ptr<AwaitableState<void>> completion;
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
        PublishedSenderBundle bundle;
        uint64_t generation = 0;
    };
    auto* params = new ApplyScalabilityTaskParams{
        shared_from_this(), completion, pc, std::move(bundle), generation};
    WebRTCManager::Instance().signaling_thread()->PostTask([params]() {
        std::unique_ptr<ApplyScalabilityTaskParams> owned(params);
        auto& task = *owned;
        if (task.generation != task.room->session_generation_.load(
                std::memory_order_acquire)) {
            FailAwaitable(task.completion, std::make_exception_ptr(OperationError(
                OperationKind::PublishTrack,
                OperationErrorCode::Cancelled,
                "apply_scalability_mode",
                "session changed before applying SVC mode")));
            return;
        }

        for (size_t index = 0; index < task.bundle.senders.size(); ++index) {
            const auto& mode = task.bundle.scalability_modes[index];
            if (mode.empty()) continue;
            const auto& sender = task.bundle.senders[index];
            if (!sender || !sender->track()) {
                FailAwaitable(task.completion, std::make_exception_ptr(OperationError(
                    OperationKind::PublishTrack,
                    OperationErrorCode::StateUncertain,
                    "apply_scalability_mode",
                    "sender was removed before applying SVC mode",
                    true)));
                return;
            }
            auto parameters = sender->GetParameters();
            for (auto& encoding : parameters.encodings) {
                encoding.scalability_mode = mode;
            }
            const auto status = sender->SetParameters(parameters);
            if (!status.ok()) {
                FailAwaitable(task.completion, std::make_exception_ptr(OperationError(
                    OperationKind::PublishTrack,
                    OperationErrorCode::StateUncertain,
                    "apply_scalability_mode",
                    status.message(),
                    true)));
                return;
            }
        }
        if (!CompleteAwaitable(task.completion)) {
            task.room->RollbackPublishedSenderBundle(task.pc, task.bundle);
        }
    });

    co_await WaitAwaitable<void>(
        completion,
        operation_timeouts_.publish,
        OperationKind::PublishTrack,
        OperationErrorCode::TrackPublishTimeout,
        "apply_scalability_mode");
}

namespace {

struct PlannedVideoLayerLookup {
    const std::string& expected_codec;
    const std::vector<VideoLayerSetting>& layers;
};

PlannedVideoLayerLookup ResolveSenderPlannedLayerSpec(
    std::size_t sender_index,
    const VideoPublishOptions& options) {
    const auto& specs = options.simulcast_codecs;
    if (sender_index > 0 && sender_index < specs.size()) {
        return {specs[sender_index].codec, specs[sender_index].layers};
    }
    return {options.video_codec, options.layers};
}

const VideoLayerSetting* FindPlannedEncodingLayer(
    const webrtc::RtpEncodingParameters& encoding,
    std::size_t encoding_index,
    const webrtc::RtpParameters& parameters,
    const std::vector<VideoLayerSetting>& layers) {
    auto planned = layers.end();
    if (!encoding.rid.empty()) {
        planned = std::find_if(layers.begin(), layers.end(),
            [&](const auto& layer) { return layer.rid == encoding.rid; });
    }
    if (planned == layers.end() && encoding.rid.empty() &&
        layers.size() == parameters.encodings.size()) {
        planned = layers.begin() + encoding_index;
    }
    return planned == layers.end() ? nullptr : &*planned;
}

bool ComputeNegotiatedEncodingGeometry(
    std::pair<int, int> source_dimensions,
    const webrtc::RtpEncodingParameters& encoding,
    const VideoLayerSetting& planned,
    int& width,
    int& height,
    std::uint32_t& framerate) {
    width = source_dimensions.first;
    height = source_dimensions.second;
    if (width <= 0 || height <= 0) return false;
    if (encoding.scale_resolution_down_to) {
        auto bound = *encoding.scale_resolution_down_to;
        if (bound.width <= 0 || bound.height <= 0) return false;
        if ((width < height) != (bound.width < bound.height)) {
            std::swap(bound.width, bound.height);
        }
        const auto scale = std::min(
            {1.0, double(bound.width) / width, double(bound.height) / height});
        width = static_cast<int>(std::round(width * scale));
        height = static_cast<int>(std::round(height * scale));
    } else {
        const double scale =
            encoding.scale_resolution_down_by.value_or(planned.scale_resolution_down_by);
        if (!std::isfinite(scale) || scale < 1.0) return false;
        width = static_cast<int>(std::round(width / scale));
        height = static_cast<int>(std::round(height / scale));
    }
    const double fps = encoding.max_framerate.value_or(planned.max_fps);
    if (!std::isfinite(fps) || fps <= 0 ||
        fps > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    framerate = static_cast<std::uint32_t>(std::ceil(fps));
    return width > 0 && height > 0;
}

}  // namespace

asio::awaitable<void> Room::ReconcileNegotiatedVideoSendersAsync(
    PublishedSenderBundle bundle,
    VideoPublishOptions options,
    uint64_t generation,
    std::shared_ptr<LocalVideoTrack> track) {
    const bool may_use_h264 = CompareCodecName(options.video_codec.c_str(), "h264") == 0 ||
        std::any_of(options.simulcast_codecs.begin(), options.simulcast_codecs.end(),
            [](const auto& spec) { return CompareCodecName(spec.codec.c_str(), "h264") == 0; });
    if (!may_use_h264) co_return;
    auto admission = AdmitOperation(OperationKind::PublishTrack, "reconcile_video_level");
    const auto source = track ? track->source() : nullptr;
    const auto profile = track ? track->screen_share_profile() : std::nullopt;
    const auto source_dimensions = profile ? std::pair{profile->width, profile->height}
        : source ? source->dimensions() : std::pair{0, 0};
    const auto revision = track ? track->publish_options_revision() : 0;
    auto completion = std::make_shared<AwaitableState<void>>(executor_);
    const auto gate = callback_gate_;
    auto cancel = gate->CancelWhenClosed(
        [weak = std::weak_ptr(completion), weak_gate = std::weak_ptr(gate)] {
            if (auto live_gate = weak_gate.lock()) live_gate->PostCompletion([weak] {
                if (auto pending = weak.lock()) FailAwaitable(pending, std::make_exception_ptr(OperationError(
                    OperationKind::PublishTrack, OperationErrorCode::Cancelled,
                    "reconcile_video_level", "room retired before negotiated video sender reconciliation")));
            });
        });
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
    std::chrono::milliseconds timeout;
    {
        std::lock_guard lock(room_mutex_);
        pc = publisher_pc_;
        timeout = operation_timeouts_.publish;
    }
    auto* signaling = WebRTCManager::Instance().signaling_thread();
    if (!pc || !signaling || signaling->IsQuitting()) {
        throw OperationError(OperationKind::PublishTrack, OperationErrorCode::Cancelled,
            "reconcile_video_level", "publisher is unavailable for negotiated video sender reconciliation");
    }
    struct Task {
        std::shared_ptr<Room> room;
        std::shared_ptr<AwaitableState<void>> completion;
        std::shared_ptr<ExecutorCallbackGate> gate;
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
        PublishedSenderBundle bundle;
        VideoPublishOptions options;
        std::pair<int, int> source_dimensions;
        uint64_t generation;
        ExecutorCallbackGate::Ticket ticket;
    };
    auto background_admission = AdmitOperation(OperationKind::PublishTrack, "reconcile_video_level");
    auto* task = new Task{shared_from_this(), completion, gate, pc, std::move(bundle),
        std::move(options), source_dimensions, generation, std::move(background_admission)};
    try {
        signaling->PostTask([task] {
            std::unique_ptr<Task> owned(task);
            const auto finish = [&](std::exception_ptr error = {}) {
                task->gate->PostCompletion([completion = task->completion, error] {
                    if (error) FailAwaitable(completion, error);
                    else CompleteAwaitable(completion);
                });
            };
            try {
                {
                    std::lock_guard lock(task->completion->mutex);
                    if (task->completion->completed) return;
                }
                {
                    std::lock_guard lock(task->room->room_mutex_);
                    if (!task->room->IsNativeGenerationCurrentLocked(task->generation) ||
                        task->room->publisher_pc_ != task->pc) {
                        throw OperationError(OperationKind::PublishTrack, OperationErrorCode::Cancelled,
                            "reconcile_video_level", "session changed before negotiated video sender reconciliation");
                    }
                }
                for (std::size_t sender_index = 0; sender_index < task->bundle.senders.size(); ++sender_index) {
                    const auto& sender = task->bundle.senders[sender_index];
                    if (!sender) continue;
                    const auto spec = ResolveSenderPlannedLayerSpec(sender_index, task->options);
                    auto parameters = sender->GetParameters();
                    if (parameters.encodings.empty()) continue;
                    bool modified = false;
                    for (std::size_t index = 0; index < parameters.encodings.size(); ++index) {
                        auto& encoding = parameters.encodings[index];
                        const webrtc::RtpCodec* codec = encoding.codec ? &*encoding.codec :
                            (parameters.codecs.empty() ? nullptr : &parameters.codecs.front());
                        if (!codec || CompareCodecName(codec->name.c_str(), "h264") != 0) continue;
                        const auto format = detail::ProjectH264Format(codec->name, codec->parameters);
                        if (!format || !format->valid) continue;
                        const auto* planned = FindPlannedEncodingLayer(encoding, index, parameters, spec.layers);
                        if (!planned) continue;
                        int width = 0, height = 0;
                        std::uint32_t framerate = 0;
                        if (!ComputeNegotiatedEncodingGeometry(
                                task->source_dimensions, encoding, *planned, width, height, framerate)) {
                            continue;
                        }
                        const auto level = static_cast<webrtc::H264Level>(format->level);
                        if (detail::CheckH264LevelConstraints(level, width, height, framerate) ==
                            detail::H264LevelConstraintStatus::Supported) {
                            continue;
                        }
                        const auto constrained = detail::ConstrainH264Dimensions(level, width, height, framerate);
                        if (!constrained ||
                            (constrained->first == width && constrained->second == height)) {
                            continue;
                        }
                        RTC_LOG(LS_INFO)
                            << "Reconciling negotiated H264 sender layer: sender=" << sender_index
                            << " layer=" << index << " rid=" << encoding.rid
                            << " level=" << static_cast<int>(format->level)
                            << " from=" << width << "x" << height << "@" << framerate
                            << " to=" << constrained->first << "x" << constrained->second;
                        encoding.scale_resolution_down_to = webrtc::Resolution{
                            constrained->first, constrained->second};
                        modified = true;
                    }
                    if (modified) {
                        const auto status = sender->SetParameters(parameters);
                        if (!status.ok()) {
                            RTC_LOG(LS_WARNING)
                                << "Failed to apply negotiated H264 sender parameters: sender="
                                << sender_index << " error=" << status.message();
                        }
                    }
                }
                finish();
            } catch (const OperationError&) {
                finish(std::current_exception());
            } catch (...) {
                finish(std::make_exception_ptr(OperationError(OperationKind::PublishTrack,
                    OperationErrorCode::CodecUnsupported, "reconcile_video_level",
                    "negotiated video sender parameters could not be reconciled", false,
                    "h264", OperationRecoveryAction::SelectSupportedCodec)));
            }
        });
    } catch (...) {
        delete task;
        throw;
    }
    co_await WaitAwaitable<void>(completion, timeout, OperationKind::PublishTrack,
        OperationErrorCode::TrackPublishTimeout, "reconcile_video_level");
    const auto current_profile = track ? track->screen_share_profile() : std::nullopt;
    const auto current_dimensions = current_profile ? std::pair{current_profile->width, current_profile->height}
        : source ? source->dimensions() : std::pair{0, 0};
    if (!track || revision != track->publish_options_revision() || profile != current_profile ||
        source_dimensions != current_dimensions) {
        throw OperationError(OperationKind::PublishTrack, OperationErrorCode::Cancelled,
            "reconcile_video_level", "video parameters changed before negotiated sender reconciliation completed", true);
    }
}

asio::awaitable<void> Room::ValidateFinalVideoSendersAsync(
    PublishedSenderBundle bundle,
    VideoPublishOptions options,
    uint64_t generation,
    std::shared_ptr<LocalVideoTrack> track) {
    const bool may_use_h264 = CompareCodecName(options.video_codec.c_str(), "h264") == 0 ||
        std::any_of(options.simulcast_codecs.begin(), options.simulcast_codecs.end(),
            [](const auto& spec) { return CompareCodecName(spec.codec.c_str(), "h264") == 0; });
    const bool may_use_h265 = CompareCodecName(options.video_codec.c_str(), "h265") == 0 ||
        std::any_of(options.simulcast_codecs.begin(), options.simulcast_codecs.end(),
            [](const auto& spec) { return CompareCodecName(spec.codec.c_str(), "h265") == 0; });
    if (!may_use_h264 && !may_use_h265) co_return;
    auto admission = AdmitOperation(OperationKind::PublishTrack, "validate_video_level");
    const auto source = track ? track->source() : nullptr;
    const auto profile = track ? track->screen_share_profile() : std::nullopt;
    const auto source_dimensions = profile ? std::pair{profile->width, profile->height}
        : source ? source->dimensions() : std::pair{0, 0};
    const auto revision = track ? track->publish_options_revision() : 0;
    auto completion = std::make_shared<AwaitableState<void>>(executor_);
    const auto gate = callback_gate_;
    auto cancel = gate->CancelWhenClosed(
        [weak = std::weak_ptr(completion), weak_gate = std::weak_ptr(gate)] {
            if (auto live_gate = weak_gate.lock()) live_gate->PostCompletion([weak] {
                if (auto pending = weak.lock()) FailAwaitable(pending, std::make_exception_ptr(OperationError(
                    OperationKind::PublishTrack, OperationErrorCode::Cancelled,
                    "validate_video_level", "room retired before final video format validation")));
            });
        });
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
    std::chrono::milliseconds timeout;
    {
        std::lock_guard lock(room_mutex_);
        pc = publisher_pc_;
        timeout = operation_timeouts_.publish;
    }
    auto* signaling = WebRTCManager::Instance().signaling_thread();
    if (!pc || !signaling || signaling->IsQuitting()) {
        throw OperationError(OperationKind::PublishTrack, OperationErrorCode::Cancelled,
            "validate_video_level", "publisher is unavailable for final video format validation");
    }
    struct Task {
        std::shared_ptr<Room> room;
        std::shared_ptr<AwaitableState<void>> completion;
        std::shared_ptr<ExecutorCallbackGate> gate;
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
        PublishedSenderBundle bundle;
        VideoPublishOptions options;
        std::pair<int, int> source_dimensions;
        uint64_t generation;
        ExecutorCallbackGate::Ticket ticket;
    };
    auto background_admission = AdmitOperation(OperationKind::PublishTrack, "validate_video_level");
    auto* task = new Task{shared_from_this(), completion, gate, pc, std::move(bundle),
        std::move(options), source_dimensions, generation, std::move(background_admission)};
    try {
        signaling->PostTask([task] {
            std::unique_ptr<Task> owned(task);
            const auto finish = [&](std::exception_ptr error = {}) {
                task->gate->PostCompletion([completion = task->completion, error] {
                    if (error) FailAwaitable(completion, error);
                    else CompleteAwaitable(completion);
                });
            };
            try {
                {
                    std::lock_guard lock(task->completion->mutex);
                    if (task->completion->completed) return;
                }
                {
                    std::lock_guard lock(task->room->room_mutex_);
                    if (!task->room->IsNativeGenerationCurrentLocked(task->generation) ||
                        task->room->publisher_pc_ != task->pc) {
                        throw OperationError(OperationKind::PublishTrack, OperationErrorCode::Cancelled,
                            "validate_video_level", "session changed before final video format validation");
                    }
                }
                bool checking_h265 = CompareCodecName(task->options.video_codec.c_str(), "h265") == 0;
                std::uint32_t checked_bitrate = 0;
                const auto reject = [&](std::size_t layer, int width = 0, int height = 0,
                    std::uint32_t fps = 0, int status = static_cast<int>(detail::H264LevelConstraintStatus::InvalidParameters),
                    std::uint8_t level = 255) {
                    diagnostic::Event event;
                    event.kind = diagnostic::EventKind::VideoLevelRejected;
                    {
                        std::lock_guard lock(task->room->room_mutex_);
                        event.context = task->room->diagnostic_context_;
                    }
                    event.context.room_generation = task->generation;
                    event.context.has_room_generation = true;
                    event.sdp_role = diagnostic::SdpRole::Publisher;
                    event.thread_role = diagnostic::ThreadRole::Rtc;
                    event.media_kind = diagnostic::MediaKind::Video;
                    event.outcome = diagnostic::Outcome::Failure;
                    event.hardware.encoder_codecs = checking_h265 ? diagnostic::HardwareEncoderH265 : diagnostic::HardwareEncoderH264;
                    event.encoder.stage = diagnostic::EncoderFailureStage::InitLevelConstraints;
                    event.encoder_level = level;
                    event.encoder.layer = static_cast<std::uint8_t>(std::min<std::size_t>(layer, 255));
                    event.encoder.width = static_cast<std::uint16_t>(std::clamp(width, 0, 65535));
                    event.encoder.height = static_cast<std::uint16_t>(std::clamp(height, 0, 65535));
                    event.encoder.framerate = static_cast<std::uint16_t>(std::min<std::uint32_t>(fps, 65535));
                    event.encoder.bitrate_bps = checked_bitrate;
                    event.encoder.status = static_cast<std::int32_t>(status);
                    diagnostic::EmitBusinessEvent(event);
                    throw OperationError(OperationKind::PublishTrack, OperationErrorCode::CodecUnsupported,
                        "validate_video_level", "the final video format cannot support the current video parameters", false,
                        checking_h265 ? "h265" : "h264", OperationRecoveryAction::SelectSupportedCodec);
                };
                if (task->bundle.senders.empty()) reject(0);
                for (std::size_t sender_index = 0; sender_index < task->bundle.senders.size(); ++sender_index) {
                    const auto& sender = task->bundle.senders[sender_index];
                    const auto spec = ResolveSenderPlannedLayerSpec(sender_index, task->options);
                    const auto& layers = spec.layers;
                    const auto& expected_codec = spec.expected_codec;
                    const bool expected_h264 = CompareCodecName(expected_codec.c_str(), "h264") == 0;
                    const bool expected_h265 = CompareCodecName(expected_codec.c_str(), "h265") == 0;
                    checking_h265 = expected_h265;
                    checked_bitrate = 0;
                    if (!sender) { if (expected_h264 || expected_h265) reject(0); else continue; }
                    const auto parameters = sender->GetParameters();
                    if (parameters.encodings.empty()) { if (expected_h264 || expected_h265) reject(0); else continue; }
                    for (std::size_t index = 0; index < parameters.encodings.size(); ++index) {
                        checking_h265 = expected_h265;
                        checked_bitrate = 0;
                        const auto& encoding = parameters.encodings[index];
                        const webrtc::RtpCodec* codec = encoding.codec ? &*encoding.codec :
                            (parameters.codecs.empty() ? nullptr : &parameters.codecs.front());
                        if (!codec) { if (expected_h264 || expected_h265) reject(index); else continue; }
                        const bool actual_h265 = CompareCodecName(codec->name.c_str(), "h265") == 0;
                        const bool actual_h264 = CompareCodecName(codec->name.c_str(), "h264") == 0;
                        if (!actual_h264 && !actual_h265) {
                            if ((expected_h264 || expected_h265) && CompareCodecName(codec->name.c_str(), "vp8") != 0 &&
                                CompareCodecName(codec->name.c_str(), "vp9") != 0 && CompareCodecName(codec->name.c_str(), "av1") != 0 &&
                                CompareCodecName(codec->name.c_str(), "h265") != 0) reject(index);
                            continue;
                        }
                        checking_h265 = actual_h265;
                        const auto format = detail::ProjectH264Format(codec->name, codec->parameters);
                        const auto hevc_format = detail::ProjectH265Format(codec->name, codec->parameters);
                        if (checking_h265 ? (!hevc_format || !hevc_format->valid || parameters.encodings.size() != 1)
                                          : (!format || !format->valid)) reject(index);
                        const auto* planned = FindPlannedEncodingLayer(encoding, index, parameters, layers);
                        if (!planned) reject(index);
                        int width = 0, height = 0;
                        std::uint32_t framerate = 0;
                        if (!ComputeNegotiatedEncodingGeometry(
                                task->source_dimensions, encoding, *planned, width, height, framerate)) {
                            reject(index);
                        }
                        if (checking_h265) {
                            const auto bitrate = encoding.max_bitrate_bps.value_or(planned->max_bitrate_bps);
                            if (bitrate <= 0) reject(index);
                            checked_bitrate = static_cast<std::uint32_t>(bitrate);
                            const auto status = detail::CheckH265LevelConstraints(
                                hevc_format->level, width, height, framerate, checked_bitrate);
                            if (status != detail::H265LevelConstraintStatus::Supported)
                                reject(index, width, height, framerate, static_cast<int>(status), hevc_format->level);
                        } else {
                            const auto status = detail::CheckH264LevelConstraints(
                                static_cast<webrtc::H264Level>(format->level), width, height, framerate);
                            if (status != detail::H264LevelConstraintStatus::Supported)
                                reject(index, width, height, framerate, static_cast<int>(status), format->level);
                        }
                    }
                }
                finish();
            } catch (const OperationError&) {
                finish(std::current_exception());
            } catch (...) {
                finish(std::make_exception_ptr(OperationError(OperationKind::PublishTrack,
                    OperationErrorCode::CodecUnsupported, "validate_video_level",
                    "the final video sender parameters could not be validated", false,
                    CompareCodecName(task->options.video_codec.c_str(), "h265") == 0 ? "h265" : "h264",
                    OperationRecoveryAction::SelectSupportedCodec)));
            }
        });
    } catch (...) {
        delete task;
        throw;
    }
    co_await WaitAwaitable<void>(completion, timeout, OperationKind::PublishTrack,
        OperationErrorCode::TrackPublishTimeout, "validate_video_level");
    const auto current_profile = track ? track->screen_share_profile() : std::nullopt;
    const auto current_dimensions = current_profile ? std::pair{current_profile->width, current_profile->height}
        : source ? source->dimensions() : std::pair{0, 0};
    if (!track || revision != track->publish_options_revision() || profile != current_profile ||
        source_dimensions != current_dimensions) {
        throw OperationError(OperationKind::PublishTrack, OperationErrorCode::Cancelled,
            "validate_video_level", "video parameters changed before final format validation completed", true);
    }
}

asio::awaitable<void> Room::ReconcileAndValidateVideoSendersAsync(
    PublishedSenderBundle bundle,
    VideoPublishOptions options,
    uint64_t generation,
    std::shared_ptr<LocalVideoTrack> track,
    std::function<void()> validate_preflight) {
    for (int attempt = 0; ; ++attempt) {
        if (validate_preflight) validate_preflight();
        try {
            co_await ReconcileNegotiatedVideoSendersAsync(
                bundle, options, generation, track);
            co_await ValidateFinalVideoSendersAsync(
                bundle, options, generation, track);
            co_return;
        } catch (const OperationError& error) {
            if (attempt < 2 &&
                error.code() == OperationErrorCode::Cancelled &&
                error.retryable() &&
                (error.stage() == "validate_video_level" ||
                 error.stage() == "reconcile_video_level")) {
                std::lock_guard lock(room_mutex_);
                if (IsNativeGenerationCurrentLocked(generation) &&
                    connection_state_ == ConnectionState::Connected) {
                    continue;
                }
            }
            throw;
        }
    }
}

asio::awaitable<Room::PreflightedVideoPublishPlan> Room::PreflightVideoPublishPlanAsync(
    std::shared_ptr<LocalVideoTrack> track,
    ResolvedVideoPublishPlan plan,
    std::vector<std::string> enabled_codecs,
    uint64_t generation) {
    if (plan.ok() && GetVideoEncoderPreference() == "software") {
        const bool screenshare = track && track->Track::source() == TrackSource::ScreenShareVideo;
        const auto constrain = [&](auto& layers) {
            const auto level = SoftwareH264SendLevel(screenshare);
            if (!level) RejectH264Profile();
            for (auto& layer : layers) {
                const auto dimensions = detail::ConstrainH264Dimensions(*level,
                    layer.width, layer.height, layer.max_fps);
                if (!dimensions) RejectH264Profile();
                layer.scale_resolution_down_by *= double(layer.width) / dimensions->first;
                layer.width = dimensions->first;
                layer.height = dimensions->second;
            }
        };
        if (plan.effective_codec == "h264") constrain(plan.effective.layers);
        for (auto& spec : plan.effective.simulcast_codecs)
            if (CompareCodecName(spec.codec.c_str(), "h264") == 0) constrain(spec.layers);
    }
    if (plan.ok() && plan.effective_codec == "h265") {
        const auto level = LocalH265SendLevel();
        if (!level) RejectH265Profile();
        const auto* limits = webrtc::external_encoder::FindAmfHevcLevelLimits(*level);
        const auto constrain_budget = [&](auto& layers) {
            for (auto& layer : layers) {
                layer.max_bitrate_bps = std::min(layer.max_bitrate_bps,
                    static_cast<int>(limits->max_main_tier_bitrate_bps));
                if (detail::CheckH265LevelConstraints(*level, layer.width, layer.height,
                        layer.max_fps, layer.max_bitrate_bps) != detail::H265LevelConstraintStatus::Supported)
                    RejectH265Profile();
            }
        };
        constrain_budget(plan.effective.layers);
        for (auto& spec : plan.effective.simulcast_codecs) constrain_budget(spec.layers);
        co_return PreflightedVideoPublishPlan{std::move(plan), {}};
    }
    const auto preference = GetVideoEncoderPreference();
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
    plan.requested_backend = preference;
#endif
    const auto hardware = GetVideoEncoderCapabilities();
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
    plan.backend_reason = LinuxEncoderRequestReason(preference, hardware);
#endif
    const bool explicit_hardware = preference == "amf" || preference == "nvenc" || preference == "qsv"
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
        || preference == "vaapi"
#endif
        ;
    const bool hardware_available = hardware.ready &&
        (hardware.amf.h264 || hardware.nvenc.h264 || hardware.qsv.h264
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
         || hardware.vaapi.h264
#endif
        );
    if (!plan.ok()) {
        co_return PreflightedVideoPublishPlan{std::move(plan), {}
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
            , CreateLinuxPublicationVideoEncoderFactory("software", {})
#endif
            };
    }
    if (plan.effective_codec != "h264" || preference == "software" ||
        (!plan.effective.auto_hardware_h264 && !explicit_hardware && !hardware_available)) {
        const bool screenshare = track->Track::source() == TrackSource::ScreenShareVideo;
        if (const auto level = SoftwareH264SendLevel(screenshare)) {
            ConstrainH264PublishOptions(plan.effective, *level);
        }
        co_return PreflightedVideoPublishPlan{std::move(plan), {}
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
            , CreateLinuxPublicationVideoEncoderFactory("software", {})
#endif
            };
    }

    auto stale = [] {
        throw OperationError(OperationKind::PublishTrack, OperationErrorCode::Cancelled,
            "hardware_preflight", "video publication parameters changed during hardware preflight", true);
    };
    const auto revision = track->publish_options_revision();
    const auto source = track->source();
    const auto dimensions = source ? source->dimensions() : std::pair{1280, 720};
    const auto profile = track->screen_share_profile();
    const auto source_type = track->Track::source();
    std::shared_ptr<LocalParticipant> local;
    std::shared_ptr<E2eeManager> encryption;
    std::chrono::milliseconds wait_timeout;
    {
        std::lock_guard lock(room_mutex_);
        if (!IsNativeGenerationCurrentLocked(generation) ||
            connection_state_ != ConnectionState::Connected ||
            NormalizedVideoCodecs(enabled_publish_codecs_) != NormalizedVideoCodecs(enabled_codecs)) stale();
        local = local_participant_;
        encryption = e2ee_manager_;
        wait_timeout = std::max(std::chrono::milliseconds(1),
            std::min(operation_timeouts_.publish, std::chrono::milliseconds(5000)));
    }
    const bool encrypted = encryption && encryption->enabled();
    // The caller's plan may already be stale before this coroutine starts.
    // Resolve again between the same revision/geometry reads before admitting
    // any driver work; comparing only the post-resolution snapshot is unsafe.
    const auto current_plan = ResolveVideoPublishPlan(track, enabled_codecs, encrypted);
    if (current_plan.requested != plan.requested || current_plan.effective != plan.effective ||
        revision != track->publish_options_revision() || source != track->source() ||
        dimensions != (source ? source->dimensions() : std::pair{1280, 720}) ||
        profile != track->screen_share_profile() || preference != GetVideoEncoderPreference()) stale();

    VideoEncoderPreflightRequest request;
    request.preference = preference;
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
    request.linux_capabilities = hardware;
#endif
    request.screenshare = source_type == TrackSource::ScreenShareVideo;
    request.formats = LocalHardwareH264Formats(request.screenshare);
    const auto local_codecs = NormalizedVideoCodecs(LocalVideoSenderCodecs());
    const auto server_codecs = NormalizedVideoCodecs(enabled_codecs);
    const bool vp8_allowed = local_codecs.contains("vp8") &&
        (enabled_codecs.empty() || server_codecs.contains("vp8"));
    if (!request.formats.empty() && (plan.requested_codec == "h264" || !vp8_allowed)) {
        if (const auto max_hw_level = MaxH264FormatLevel(request.formats);
            max_hw_level && !H264LayersFitLevel(plan.effective.layers, *max_hw_level)) {
            ConstrainH264PublishOptions(plan.effective, *max_hw_level);
        }
    }
    for (const auto& layer : plan.effective.layers) {
        request.layers.push_back({layer.width, layer.height, layer.max_fps, layer.max_bitrate_bps});
    }
    const auto candidate_formats = request.formats;
    auto completion = std::make_shared<AwaitableState<VideoEncoderPreflightResult>>(executor_);
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    const auto gate = callback_gate_;
    auto cancel = gate->CancelWhenClosed(
        [cancelled, weak = std::weak_ptr(completion), weak_gate = std::weak_ptr(gate)] {
            cancelled->store(true, std::memory_order_release);
            if (auto live_gate = weak_gate.lock()) {
                live_gate->PostCompletion([weak] {
                    if (auto pending = weak.lock()) {
                        FailAwaitable(pending, std::make_exception_ptr(OperationError(
                            OperationKind::PublishTrack, OperationErrorCode::Cancelled,
                            "hardware_preflight", "room retired during hardware preflight")));
                    }
                });
            }
        });
    auto background_admission = AdmitOperation(OperationKind::PublishTrack, "hardware_preflight");
    VideoEncoderPreflightResult result;
    try {
        // Only the supervisor runs here. The RTC preflight owns the bounded
        // driver sequence, including retirement after cancellation/timeouts.
        asio::post(asio::system_executor(),
            [room = shared_from_this(), gate, ticket = std::move(background_admission),
             completion, cancelled, request = std::move(request)]() mutable {
                VideoEncoderPreflightResult result;
                try {
                    result = PreflightVideoEncoder(request, cancelled);
                } catch (...) {
                    result.status = VideoEncoderPreflightStatus::InitializationFailed;
                }
                // Keep the Room, execution-context lease and in-flight ticket
                // until the supervisor and its single completion have finished.
                gate->PostCompletion([room = std::move(room), completion, result = std::move(result)]() mutable {
                    CompleteAwaitable(completion, std::move(result));
                });
            });
        result = co_await WaitAwaitable<VideoEncoderPreflightResult>(completion, wait_timeout,
            OperationKind::PublishTrack, OperationErrorCode::TrackPublishTimeout, "hardware_preflight");
    } catch (const OperationError& error) {
        cancelled->store(true, std::memory_order_release);
        if (error.code() != OperationErrorCode::TrackPublishTimeout) throw;
        result.status = VideoEncoderPreflightStatus::TimedOut;
    } catch (...) {
        cancelled->store(true, std::memory_order_release);
        throw;
    }

    // Only formats actually offered to the probe can become negotiation policy.
    if (result.ok() && (result.formats.empty() ||
        std::any_of(result.formats.begin(), result.formats.end(), [&](const auto& format) {
            return std::none_of(candidate_formats.begin(), candidate_formats.end(),
                [&](const auto& candidate) { return ExactH264Format(candidate, format); });
        }))) {
        result.status = VideoEncoderPreflightStatus::UnsupportedFormat;
        result.formats.clear();
    }
    auto validate_current = [weak_room = weak_from_this(), weak_track = std::weak_ptr(track),
        local, encryption, encrypted, enabled_codecs, revision, source, dimensions, profile,
        source_type, generation, preference, requested = plan.requested,
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
        capability_generation = hardware.generation,
        approved_device = result.linux_capability.device,
#endif
        approved = result.ok() ? result.formats : std::vector<webrtc::SdpVideoFormat>{},
        approved_backend = result.backend, stale] {
        const auto room = weak_room.lock();
        const auto track = weak_track.lock();
        if (!room || !track) stale();
        {
            std::lock_guard lock(room->room_mutex_);
            if (!room->IsNativeGenerationCurrentLocked(generation) ||
                room->connection_state_ != ConnectionState::Connected ||
                room->local_participant_ != local || room->e2ee_manager_ != encryption ||
                NormalizedVideoCodecs(room->enabled_publish_codecs_) != NormalizedVideoCodecs(enabled_codecs)) stale();
        }
        auto current_requested = track->requested_publish_options();
        current_requested.preflight_h264_formats.clear();
        const auto current_dims = source ? source->dimensions() : std::pair{1280, 720};
        const bool dims_changed = profile.has_value()
            ? (dimensions != current_dims)
            : (current_dims.first <= 0 || current_dims.second <= 0 ||
               (dimensions.first < dimensions.second) != (current_dims.first < current_dims.second));
        if (revision != track->publish_options_revision() || requested != current_requested ||
            source != track->source() || source_type != track->Track::source() ||
            dims_changed ||
            profile != track->screen_share_profile() || preference != GetVideoEncoderPreference() ||
            encrypted != (encryption && encryption->enabled())) stale();
        if (!approved.empty()) {
            const auto hardware = GetVideoEncoderCapabilities();
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
            if (hardware.generation != capability_generation) stale();
            const auto* backend = approved_backend == "nvenc" ? &hardware.nvenc :
                (approved_backend == "qsv" ? &hardware.qsv :
                    (approved_backend == "vaapi" ? &hardware.vaapi : nullptr));
            if (!backend || !SameLinuxEncoderDevice(approved_device, backend->device)) stale();
#endif
            const bool available = hardware.ready &&
                ((approved_backend == "amf" && hardware.amf.h264) ||
                 (approved_backend == "nvenc" && hardware.nvenc.h264) ||
                 (approved_backend == "qsv" && hardware.qsv.h264)
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
                 || (approved_backend == "vaapi" && hardware.vaapi.h264)
#endif
                );
            if (!available) stale();
            const auto formats = LocalHardwareH264Formats(source_type == TrackSource::ScreenShareVideo);
            for (const auto& format : approved) {
                if (std::none_of(formats.begin(), formats.end(),
                    [&](const auto& candidate) { return ExactH264Format(candidate, format); })) stale();
            }
        }
    };
    validate_current();
    const auto backend = result.backend == "amf" || result.backend == "nvenc" || result.backend == "qsv"
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
        || result.backend == "vaapi"
#endif
        ? result.backend : std::string("none");
    Log("SIGNAL", "PUBLISH_HW_PREFLIGHT", std::string("status=") + VideoPreflightStatusName(result.status) +
        "; backend=" + backend + "; layer=" + std::to_string(result.failed_layer));
    const auto emit_preflight = [&](std::size_t layer_index) {
        diagnostic::Event event;
        event.kind = diagnostic::EventKind::HardwareEncoderPreflight;
        event.context.room_generation = generation;
        event.context.has_room_generation = true;
        event.media_kind = diagnostic::MediaKind::Video;
        event.outcome = result.ok() ? diagnostic::Outcome::Success : diagnostic::Outcome::Failure;
        event.hardware.vendor_id = backend == "amf" ? 0x1002 :
            (backend == "nvenc" ? 0x10de : (backend == "qsv" ? 0x8086 : 0));
        event.hardware.encoder_codecs = diagnostic::HardwareEncoderH264;
        event.rtc_error_type = result.codec_status;
        event.encoder.stage = diagnostic::EncoderFailureStage::Preflight;
        event.encoder.status = static_cast<std::int32_t>(result.status);
        event.encoder.layer = static_cast<std::uint8_t>(std::min<std::size_t>(layer_index, 255));
        if (layer_index < plan.effective.layers.size()) {
            const auto& layer = plan.effective.layers[layer_index];
            event.encoder.width = static_cast<std::uint16_t>(std::clamp(layer.width, 0, 65535));
            event.encoder.height = static_cast<std::uint16_t>(std::clamp(layer.height, 0, 65535));
            event.encoder.framerate = static_cast<std::uint16_t>(std::clamp(layer.max_fps, 0, 65535));
            event.encoder.bitrate_bps = static_cast<std::uint32_t>(std::max(layer.max_bitrate_bps, 0));
            if (!result.ok() && result.tested_bitrate_bps > 0) {
                event.encoder.bitrate_bps = result.tested_bitrate_bps;
            }
        }
        EmitDiagnostic(event);
    };
    if (result.ok()) {
        for (std::size_t layer_index = 0; layer_index < plan.effective.layers.size(); ++layer_index) {
            emit_preflight(layer_index);
        }
    } else {
        emit_preflight(result.failed_layer);
    }
    if (result.status == VideoEncoderPreflightStatus::Cancelled) {
        throw OperationError(OperationKind::PublishTrack, OperationErrorCode::Cancelled,
            "hardware_preflight", "video hardware preflight was cancelled");
    }
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
    if (result.status == VideoEncoderPreflightStatus::TimedOut ||
        result.status == VideoEncoderPreflightStatus::RetirementFailed) {
        // A driver call may still own its resources. Do not hide that state by
        // starting a software encoder in the same publication transaction.
        throw OperationError(OperationKind::PublishTrack,
            result.status == VideoEncoderPreflightStatus::TimedOut
                ? OperationErrorCode::TrackPublishTimeout
                : OperationErrorCode::TrackPublishRejected,
            "hardware_preflight", "video hardware preflight did not retire safely");
    }
#endif
    bool software_fallback = false;
    if (result.ok()) {
        try {
            for (const auto& format : result.formats) {
                if (auto row = VideoFormatRow(format.name, format.parameters,
                    diagnostic::VideoFormatSource::PreflightApproved)) {
                    EmitPublisherVideoFormat(*row, generation);
                }
            }
        } catch (...) {}
        plan.effective.preflight_h264_formats = std::move(result.formats);
    } else {
        const auto [width, height] = profile ? std::pair{profile->width, profile->height} : dimensions;
        const auto requested_snapshot = plan.requested;
        auto fallback_sender_codecs = LocalVideoSenderCodecs();
        if (encrypted) {
            std::erase_if(fallback_sender_codecs, [](const std::string& codec) {
                return CompareCodecName(codec.c_str(), "vp8") != 0 &&
                    CompareCodecName(codec.c_str(), "h264") != 0 &&
                    CompareCodecName(codec.c_str(), "vp9") != 0;
            });
        }
        plan = LocalVideoTrack::ResolveHardwarePreflightFallback(
            width, height, plan, fallback_sender_codecs, enabled_codecs);
        if (!plan.ok() && !explicit_hardware &&
            (preference.empty() || preference == "auto") &&
            plan.requested_codec == "h264" &&
            result.status != VideoEncoderPreflightStatus::RetirementFailed &&
            result.status != VideoEncoderPreflightStatus::TimedOut) {
            auto sw_plan = LocalVideoTrack::ResolvePublishPlan(
                width, height, requested_snapshot, fallback_sender_codecs,
                enabled_codecs, /*prefer_hardware_h264=*/false);
            const auto sw_level = SoftwareH264SendLevel(source_type == TrackSource::ScreenShareVideo);
            if (sw_plan.ok() &&
                CompareCodecName(sw_plan.effective_codec.c_str(), "h264") == 0 &&
                sw_level &&
                ConstrainH264PublishOptions(sw_plan.effective, *sw_level)) {
                sw_plan.fallback_reason = "hardware H264 preflight failed before publication";
                plan = std::move(sw_plan);
                software_fallback = true;
            }
        }
        if (!plan.ok()) {
            throw OperationError(OperationKind::PublishTrack, OperationErrorCode::TrackPublishRejected,
                "hardware_preflight", plan.error);
        }
    }
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
    auto encoder_factory = CreateLinuxPublicationVideoEncoderFactory(preference, hardware,
        plan.effective.preflight_h264_formats);
    if (!encoder_factory) stale();
#endif
    co_return PreflightedVideoPublishPlan{std::move(plan),
        software_fallback ? std::function<void()>{} : std::function<void()>{std::move(validate_current)}
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
        , std::move(encoder_factory)
#endif
        };
}

asio::awaitable<std::shared_ptr<TrackPublication>> Room::PublishLocalTrackAsync(
    std::shared_ptr<Track> track,
    const proto::SignalRequest& request) {
    auto operation_admission = AdmitOperation(OperationKind::PublishTrack, "PublishLocalTrackAsync");
    auto media_policy = AcquireMediaPublishPolicy();
    co_return co_await PublishLocalTrackAsync(std::move(track), request,
        session_generation_.load(std::memory_order_acquire));
}

asio::awaitable<std::shared_ptr<TrackPublication>> Room::PublishLocalTrackAsync(
    std::shared_ptr<Track> track, const proto::SignalRequest& request, uint64_t generation) {
    auto operation_admission = AdmitOperation(OperationKind::PublishTrack, "PublishLocalTrackAsync");
    if (!track || !request.has_add_track()) {
        throw OperationError(OperationKind::PublishTrack,
                             OperationErrorCode::InvalidState,
                             "publish_validate",
                             "invalid AddTrack request");
    }

    proto::SignalRequest effective_request(request);
    std::optional<ResolvedVideoPublishPlan> video_plan;
    std::optional<ResolvedAudioPublishPlan> audio_plan;
    std::function<void()> validate_video_preflight;
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
    std::shared_ptr<webrtc::VideoEncoderFactory> linux_encoder_factory;
#endif
    if (auto video = std::dynamic_pointer_cast<LocalVideoTrack>(track)) {
        std::vector<std::string> enabled_codecs;
        std::shared_ptr<LocalParticipant> planning_local;
        std::shared_ptr<E2eeManager> planning_e2ee;
        {
            std::lock_guard lock(room_mutex_);
            enabled_codecs = enabled_publish_codecs_;
            planning_local = local_participant_;
            planning_e2ee = e2ee_manager_;
        }
        if (!planning_local) {
            throw OperationError(OperationKind::PublishTrack,
                                 OperationErrorCode::InvalidState,
                                 "resolve_publish_plan",
                                 "local participant is unavailable");
        }
        video_plan = ResolveVideoPublishPlan(video, enabled_codecs,
            planning_e2ee && planning_e2ee->enabled());
        if (!video_plan->ok()) {
            throw OperationError(OperationKind::PublishTrack,
                                 OperationErrorCode::TrackPublishRejected,
                                 "resolve_publish_plan",
                                 video_plan->error);
        }
        auto preflight = co_await PreflightVideoPublishPlanAsync(
            video, std::move(*video_plan), enabled_codecs, generation);
        video_plan = std::move(preflight.plan);
        validate_video_preflight = std::move(preflight.validate_current);
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
        linux_encoder_factory = std::move(preflight.linux_encoder_factory);
#endif
        effective_request = planning_local->BuildTrackPublishRequest(
            track, &video_plan->effective);
        if (video_plan->used_fallback() || !video_plan->fallback_reason.empty()) {
            Log("SIGNAL", "PUBLISH_CODEC_FALLBACK",
                video_plan->fallback_reason + "; using " +
                    video_plan->effective_codec);
        }
    } else if (auto audio = std::dynamic_pointer_cast<LocalAudioTrack>(track)) {
        std::shared_ptr<LocalParticipant> planning_local;
        std::shared_ptr<E2eeManager> e2ee;
        {
            std::lock_guard lock(room_mutex_);
            planning_local = local_participant_;
            e2ee = e2ee_manager_;
        }
        if (!planning_local) {
            throw OperationError(OperationKind::PublishTrack,
                                 OperationErrorCode::InvalidState,
                                 "resolve_audio_publish_plan",
                                 "local participant is unavailable");
        }
        const bool media_encryption_enabled = e2ee && e2ee->enabled() &&
            e2ee->encryption_type() != EncryptionType::NONE;
        audio_plan = ResolveAudioPublishPlan(
            audio, media_encryption_enabled);
        if (!audio_plan->ok()) {
            throw OperationError(OperationKind::PublishTrack,
                                 OperationErrorCode::TrackPublishRejected,
                                 "resolve_audio_publish_plan",
                                 audio_plan->error);
        }
        effective_request = planning_local->BuildTrackPublishRequest(
            track, nullptr, &audio_plan->effective);
        if (audio_plan->red_disabled_for_encryption) {
            Log("SIGNAL", "PUBLISH_AUDIO_RED_DISABLED",
                "Audio RED disabled because media encryption is configured");
        }
    }

    // Encryption admission belongs to the resolved plan, before AddTrack or
    // native sender side effects. Never preserve an untrusted caller GCM flag.
    std::shared_ptr<E2eeManager> publish_e2ee;
    {
        std::lock_guard lock(room_mutex_);
        publish_e2ee = e2ee_manager_;
    }
    if (publish_e2ee && publish_e2ee->enabled()) {
        const auto local = local_participant();
        const auto codec = video_plan ? video_plan->effective_codec : std::string("opus");
        if ((track->kind() == TrackKind::Video && !video_plan) ||
            (codec != "opus" && codec != "vp8" && codec != "h264" && codec != "vp9") ||
            publish_e2ee->encryption_type() != EncryptionType::GCM ||
            (video_plan && video_plan->effective.simulcast_codecs.size() > 1)) {
            throw OperationError(OperationKind::PublishTrack, OperationErrorCode::EncryptionCodecUnsupported,
                "e2ee_codec_admission", "select a supported single-codec encrypted media profile", false,
                codec, OperationRecoveryAction::SelectSupportedCodec);
        }
        if (!local || !publish_e2ee->CanPublishMedia(local->identity(), codec)) {
            throw OperationError(OperationKind::PublishTrack, OperationErrorCode::EncryptionKeyUnavailable,
                "e2ee_key_admission", "install the selected media key before publishing", false,
                codec, OperationRecoveryAction::InstallEncryptionKey);
        }
        effective_request.mutable_add_track()->set_encryption(proto::Encryption::GCM);
    } else {
        effective_request.mutable_add_track()->set_encryption(proto::Encryption::NONE);
    }
    const std::string cid = effective_request.add_track().cid();
    std::shared_ptr<SignalClient> signal;
    std::shared_ptr<LocalParticipant> local;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> publisher;
    OperationTimeouts timeouts;
    std::shared_ptr<telemetry::SessionTelemetry> telemetry_owner;
    bool reconnect_republish = false;
    std::shared_ptr<telemetry::LocalVideoActivityProbe> local_video_probe;
    std::shared_ptr<telemetry::LocalAudioActivityProbe> local_audio_probe;
    std::uint64_t local_publication_epoch = 0;
    const auto publish_accepted_at = std::chrono::steady_clock::now();
    auto ack = std::make_shared<AwaitableState<proto::TrackPublishedResponse>>(executor_);
    if (validate_video_preflight) validate_video_preflight();
    {
        std::lock_guard lock(room_mutex_);
        if (connection_state_ != ConnectionState::Connected ||
            generation != session_generation_.load(std::memory_order_acquire)) {
            throw OperationError(OperationKind::PublishTrack,
                                 OperationErrorCode::InvalidState,
                                 "publish_validate",
                                 "room is not connected");
        }
        if (pending_track_publishes_.contains(cid)) {
            throw OperationError(OperationKind::PublishTrack,
                                 OperationErrorCode::InvalidState,
                                 "publish_validate",
                                 "a publish operation for the same CID is already in progress");
        }
        signal = signal_client_;
        local = local_participant_;
        publisher = publisher_pc_;
        timeouts = operation_timeouts_;
        pending_track_publishes_[cid] = ack;
        reconnect_republish = reconnect_republish_tracks_.erase(track.get()) != 0;
        telemetry_owner = session_telemetry_.lock();
    }
    if (telemetry_owner) {
        local_publication_epoch = local_publication_epoch_.fetch_add(
            1, std::memory_order_relaxed);
        if (auto video = std::dynamic_pointer_cast<LocalVideoTrack>(track)) {
            const auto rtc_track = track->rtc_track();
            local_video_probe = MakeLocalVideoActivityProbe(
                telemetry_owner,
                video,
                LocalPublishTelemetryKey(rtc_track ? rtc_track->id() : std::string{}),
                generation,
                local_publication_epoch);
            video->set_publish_telemetry_probe(local_video_probe);
        } else if (auto audio = std::dynamic_pointer_cast<LocalAudioTrack>(track)) {
            local_audio_probe = MakeLocalAudioActivityProbe(telemetry_owner);
            audio->set_publish_telemetry_probe(local_audio_probe);
        }
    }
    TelemetryOperationSpan publish_operation(
        reconnect_republish ? nullptr : telemetry_owner,
        telemetry::OperationKind::PublishTrack);
    diagnostic::Event publish_event;
    publish_event.kind = diagnostic::EventKind::MediaPublishStarted;
    publish_event.context.room_generation = generation;
    publish_event.context.has_room_generation = true;
    publish_event.context.operation_id.Assign(publish_operation.id().empty()
        ? "publish_" + std::to_string(generation) + "_" +
            std::to_string(publish_accepted_at.time_since_epoch().count())
        : publish_operation.id());
    publish_event.media_kind = track->kind() == TrackKind::Video
        ? diagnostic::MediaKind::Video : diagnostic::MediaKind::Audio;
    EmitDiagnostic(publish_event);
    const auto release_ack = [&]() {
        std::lock_guard lock(room_mutex_);
        const auto found = pending_track_publishes_.find(cid);
        if (found != pending_track_publishes_.end() && found->second == ack) {
            pending_track_publishes_.erase(found);
        }
    };

    PublishedSenderBundle sender_bundle;
    bool server_acknowledged = false;
    try {
        co_await signal->SendAsync(effective_request);
        auto response = co_await WaitAwaitable<proto::TrackPublishedResponse>(
            ack,
            timeouts.publish,
            OperationKind::PublishTrack,
            OperationErrorCode::TrackPublishTimeout,
            "wait_track_published_ack");
        server_acknowledged = true;

        release_ack();

        sender_bundle = co_await AddTrackToPublisherAsync(
            track,
            generation,
            video_plan.has_value()
                ? std::optional<VideoPublishOptions>{video_plan->effective}
                : std::nullopt,
            audio_plan.has_value()
                ? std::optional<AudioPublishPolicy>{audio_plan->effective}
                : std::nullopt,
            effective_request.add_track().encryption() == proto::Encryption::GCM
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
            , linux_encoder_factory
#endif
            );
        co_await NegotiatePublisherAsync(timeouts.negotiation, generation);
        co_await ApplyPublishedSenderScalabilityModesAsync(
            sender_bundle, generation);
        if (video_plan) {
            co_await ReconcileAndValidateVideoSendersAsync(
                sender_bundle, video_plan->effective, generation,
                std::dynamic_pointer_cast<LocalVideoTrack>(track),
                validate_video_preflight);
        }

        if (generation != session_generation_.load(std::memory_order_acquire)) {
            throw OperationError(OperationKind::PublishTrack,
                                 OperationErrorCode::Cancelled,
                                 "publish_commit",
                                 "session changed before publication commit");
        }

        auto publication = std::make_shared<TrackPublication>(
            track, response.track().sid(), response.track().name());
        {
            std::lock_guard lock(room_mutex_);
            if (!IsNativeGenerationCurrentLocked(generation) || !local || local != local_participant_) {
                throw OperationError(OperationKind::PublishTrack,
                                     OperationErrorCode::Cancelled,
                                     "publish_commit",
                                     "local participant changed before publication commit");
            }
            track->set_sid(response.track().sid());
            local->add_publication(publication);
            published_sender_track_ids_[track.get()] = sender_bundle.track_ids;
            media_publication_policies_[track.get()] = publish_e2ee
                ? publish_e2ee->AcquireMediaPublishPolicy() : nullptr;
        }
        if (telemetry_owner && local_publication_epoch != 0) {
            const auto rtc_track = track->rtc_track();
            const auto rtc_track_id = rtc_track ? rtc_track->id() : std::string{};
            telemetry::LocalVideoPublishDescriptor video_publish;
            video_publish.publication_sid = publication->sid();
            video_publish.source_kind = track->source() == TrackSource::Camera
                ? telemetry::LocalVideoSourceKind::Camera
                : (track->source() == TrackSource::ScreenShareVideo
                    ? telemetry::LocalVideoSourceKind::Screen
                    : telemetry::LocalVideoSourceKind::Unknown);
            if (video_plan.has_value()) {
                video_publish.requested_codec = video_plan->requested_codec;
                video_publish.effective_codec = video_plan->effective_codec;
                video_publish.fallback_reason = video_plan->fallback_reason;
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
                video_publish.requested_backend = video_plan->requested_backend;
                video_publish.backend_reason = video_plan->backend_reason;
#endif
                video_publish.source = video_publish.source_kind == telemetry::LocalVideoSourceKind::Camera
                    ? "camera" : (video_publish.source_kind == telemetry::LocalVideoSourceKind::Screen
                        ? "screen" : "unknown");
                video_publish.mode = !video_plan->effective.scalability_mode.empty()
                    ? "svc"
                    : (video_plan->effective.simulcast &&
                       video_plan->effective.layers.size() > 1 ? "simulcast" : "single");
                video_publish.resolved_profile = video_plan->effective_codec == "av1"
                    ? "profile0" : "negotiated";
                video_publish.resolved_scalability =
                    video_plan->effective.scalability_mode.empty()
                        ? "none" : video_plan->effective.scalability_mode;
                video_publish.sender_track_ids = sender_bundle.track_ids;
            }
            telemetry_owner->RegisterLocalPublication(
                LocalPublishTelemetryKey(rtc_track_id),
                generation,
                local_publication_epoch,
                track->kind() == TrackKind::Video
                    ? telemetry::LocalMediaKind::Video
                    : telemetry::LocalMediaKind::Audio,
                rtc_track_id,
                publish_accepted_at,
                !track->muted(),
                local_video_probe,
                std::chrono::steady_clock::now(),
                local_audio_probe,
                std::move(video_publish));
        }
        if (track->kind() == TrackKind::Video) SchedulePublisherMediaDiagnostic(generation);
        publish_operation.Finish(telemetry::OperationOutcome::Success);
        publish_event.kind = diagnostic::EventKind::MediaPublishTerminal;
        publish_event.outcome = diagnostic::Outcome::Success;
        publish_event.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - publish_accepted_at).count();
        EmitDiagnostic(publish_event);
        co_return publication;
    } catch (...) {
        const auto operation_outcome = TelemetryOutcome(std::current_exception());
        if (local_video_probe) {
            local_video_probe->active.store(false, std::memory_order_release);
            if (auto video = std::dynamic_pointer_cast<LocalVideoTrack>(track)) {
                video->set_publish_telemetry_probe({});
            }
        }
        if (local_audio_probe) {
            local_audio_probe->active.store(false, std::memory_order_release);
            if (auto audio = std::dynamic_pointer_cast<LocalAudioTrack>(track)) {
                audio->set_publish_telemetry_probe({});
            }
        }
        release_ack();
        if (!sender_bundle.empty() && WebRTCManager::Instance().signaling_thread()) {
            WebRTCManager::Instance().signaling_thread()->BlockingCall([this, publisher, sender_bundle]() {
                RollbackPublishedSenderBundle(publisher, sender_bundle);
            });
        }
        if (server_acknowledged) {
            Log("ERROR", "PUBLISH_ROLLBACK",
                "Local transaction failed after TrackPublished ACK; removed the sender and requested Publisher SDP renegotiation to reconcile server tracks");
            NegotiatePublisher(generation);
        }
        publish_operation.Finish(operation_outcome);
        publish_event.kind = diagnostic::EventKind::MediaPublishTerminal;
        publish_event.outcome = DiagnosticOutcome(operation_outcome);
        publish_event.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - publish_accepted_at).count();
        ProjectOperationError(std::current_exception(), publish_event);
        EmitDiagnostic(publish_event);
        throw;
    }
}

asio::awaitable<std::vector<std::shared_ptr<TrackPublication>>> Room::PublishLocalTracksBatchAsync(
    std::vector<LocalParticipant::BatchTrackItem> items) {
    auto operation_admission = AdmitOperation(OperationKind::PublishTrack, "PublishLocalTracksBatchAsync");
    auto media_policy = AcquireMediaPublishPolicy();
    if (items.empty()) {
        co_return std::vector<std::shared_ptr<TrackPublication>>{};
    }
    const auto generation = session_generation_.load(std::memory_order_acquire);

    std::vector<proto::SignalRequest> effective_requests;
    effective_requests.reserve(items.size());
    std::vector<std::optional<ResolvedVideoPublishPlan>> video_plans(
        items.size());
    std::vector<std::optional<ResolvedAudioPublishPlan>> audio_plans(
        items.size());
    std::vector<std::function<void()>> validate_video_preflights(items.size());
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
    std::vector<std::shared_ptr<webrtc::VideoEncoderFactory>> linux_encoder_factories(items.size());
#endif
    std::vector<std::string> enabled_codecs;
    std::shared_ptr<LocalParticipant> planning_local;
    std::shared_ptr<E2eeManager> planning_e2ee;
    {
        std::lock_guard lock(room_mutex_);
        enabled_codecs = enabled_publish_codecs_;
        planning_local = local_participant_;
        planning_e2ee = e2ee_manager_;
    }

    for (size_t index = 0; index < items.size(); ++index) {
        auto& item = items[index];
        if (!item.track || !item.request || !item.request->has_add_track()) {
            throw OperationError(OperationKind::PublishTrack,
                                 OperationErrorCode::InvalidState,
                                 "publish_validate",
                                 "invalid AddTrack request in batch item");
        }
        proto::SignalRequest eff_req(*item.request);
        if (auto video = std::dynamic_pointer_cast<LocalVideoTrack>(item.track)) {
            if (!planning_local) {
                throw OperationError(OperationKind::PublishTrack,
                                     OperationErrorCode::InvalidState,
                                     "resolve_publish_plan",
                                     "local participant is unavailable");
            }
            video_plans[index] = ResolveVideoPublishPlan(video, enabled_codecs,
                planning_e2ee && planning_e2ee->enabled());
            if (!video_plans[index]->ok()) {
                throw OperationError(OperationKind::PublishTrack,
                                     OperationErrorCode::TrackPublishRejected,
                                     "resolve_publish_plan",
                                     video_plans[index]->error);
            }
            auto preflight = co_await PreflightVideoPublishPlanAsync(
                video, std::move(*video_plans[index]), enabled_codecs, generation);
            video_plans[index] = std::move(preflight.plan);
            validate_video_preflights[index] = std::move(preflight.validate_current);
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
            linux_encoder_factories[index] = std::move(preflight.linux_encoder_factory);
#endif
            eff_req = planning_local->BuildTrackPublishRequest(
                item.track, &video_plans[index]->effective);
            if (video_plans[index]->used_fallback() || !video_plans[index]->fallback_reason.empty()) {
                Log("SIGNAL", "PUBLISH_CODEC_FALLBACK",
                    video_plans[index]->fallback_reason + "; using " +
                        video_plans[index]->effective_codec);
            }
        } else if (auto audio =
                       std::dynamic_pointer_cast<LocalAudioTrack>(item.track)) {
            if (!planning_local) {
                throw OperationError(OperationKind::PublishTrack,
                                     OperationErrorCode::InvalidState,
                                     "resolve_audio_publish_plan",
                                     "local participant is unavailable");
            }
            const bool media_encryption_enabled = planning_e2ee &&
                planning_e2ee->enabled() &&
                planning_e2ee->encryption_type() != EncryptionType::NONE;
            audio_plans[index] = ResolveAudioPublishPlan(
                audio, media_encryption_enabled);
            if (!audio_plans[index]->ok()) {
                throw OperationError(OperationKind::PublishTrack,
                                     OperationErrorCode::TrackPublishRejected,
                                     "resolve_audio_publish_plan",
                                     audio_plans[index]->error);
            }
            eff_req = planning_local->BuildTrackPublishRequest(
                item.track, nullptr, &audio_plans[index]->effective);
            if (audio_plans[index]->red_disabled_for_encryption) {
                Log("SIGNAL", "PUBLISH_AUDIO_RED_DISABLED",
                    "Audio RED disabled because media encryption is configured");
            }
        }
        if (planning_e2ee && planning_e2ee->enabled()) {
            const auto codec = video_plans[index] ? video_plans[index]->effective_codec : std::string("opus");
            if ((item.track->kind() == TrackKind::Video && !video_plans[index]) ||
                (codec != "opus" && codec != "vp8" && codec != "h264" && codec != "vp9") ||
                planning_e2ee->encryption_type() != EncryptionType::GCM ||
                (video_plans[index] && video_plans[index]->effective.simulcast_codecs.size() > 1)) {
                throw OperationError(OperationKind::PublishTrack, OperationErrorCode::EncryptionCodecUnsupported,
                    "e2ee_codec_admission", "select a supported single-codec encrypted media profile", false,
                    codec, OperationRecoveryAction::SelectSupportedCodec);
            }
            if (!planning_local || !planning_e2ee->CanPublishMedia(planning_local->identity(), codec)) {
                throw OperationError(OperationKind::PublishTrack, OperationErrorCode::EncryptionKeyUnavailable,
                    "e2ee_key_admission", "install the selected media key before publishing", false,
                    codec, OperationRecoveryAction::InstallEncryptionKey);
            }
            eff_req.mutable_add_track()->set_encryption(proto::Encryption::GCM);
        } else {
            eff_req.mutable_add_track()->set_encryption(proto::Encryption::NONE);
        }
        effective_requests.push_back(std::move(eff_req));
    }

    // A later item can await another preflight after an earlier item passed.
    // Recheck every receipt before registering or sending any batch AddTrack.
    for (const auto& validate : validate_video_preflights) {
        if (validate) validate();
    }
    std::shared_ptr<SignalClient> signal;
    std::shared_ptr<LocalParticipant> local;
    struct PendingAckInfo {
        std::string cid;
        std::shared_ptr<AwaitableState<proto::TrackPublishedResponse>> ack;
    };
    std::vector<PendingAckInfo> pending_acks;
    pending_acks.reserve(items.size());
    std::shared_ptr<telemetry::SessionTelemetry> telemetry_owner;
    std::vector<std::shared_ptr<telemetry::LocalVideoActivityProbe>>
        local_video_probes(items.size());
    std::vector<std::shared_ptr<telemetry::LocalAudioActivityProbe>>
        local_audio_probes(items.size());
    std::vector<std::uint64_t> local_publication_epochs(items.size(), 0);
    const auto publish_accepted_at = std::chrono::steady_clock::now();

    {
        std::lock_guard lock(room_mutex_);
        if (connection_state_ != ConnectionState::Connected ||
            generation != session_generation_.load(std::memory_order_acquire)) {
            throw OperationError(OperationKind::PublishTrack,
                                 OperationErrorCode::InvalidState,
                                 "publish_validate",
                                 "room is not connected");
        }
        for (const auto& eff_req : effective_requests) {
            const std::string cid = eff_req.add_track().cid();
            if (pending_track_publishes_.contains(cid)) {
                throw OperationError(OperationKind::PublishTrack,
                                     OperationErrorCode::InvalidState,
                                     "publish_validate",
                                     "a publish operation for CID " + cid + " is already in progress");
            }
            auto ack = std::make_shared<AwaitableState<proto::TrackPublishedResponse>>(executor_);
            pending_track_publishes_[cid] = ack;
            pending_acks.push_back({cid, ack});
        }
        signal = signal_client_;
        local = local_participant_;
        telemetry_owner = session_telemetry_.lock();
    }

    if (telemetry_owner) {
        for (std::size_t index = 0; index < items.size(); ++index) {
            local_publication_epochs[index] = local_publication_epoch_.fetch_add(
                1, std::memory_order_relaxed);
            if (auto video = std::dynamic_pointer_cast<LocalVideoTrack>(
                    items[index].track)) {
                const auto rtc_track = items[index].track->rtc_track();
                local_video_probes[index] = MakeLocalVideoActivityProbe(
                    telemetry_owner,
                    video,
                    LocalPublishTelemetryKey(
                        rtc_track ? rtc_track->id() : std::string{}),
                    generation,
                    local_publication_epochs[index]);
                video->set_publish_telemetry_probe(local_video_probes[index]);
            } else if (auto audio = std::dynamic_pointer_cast<LocalAudioTrack>(
                           items[index].track)) {
                local_audio_probes[index] = MakeLocalAudioActivityProbe(telemetry_owner);
                audio->set_publish_telemetry_probe(local_audio_probes[index]);
            }
        }
    }

    TelemetryOperationSpan batch_operation(
        telemetry_owner, telemetry::OperationKind::PublishBatch);
    diagnostic::Event batch_event;
    batch_event.kind = diagnostic::EventKind::MediaPublishBatchStarted;
    batch_event.context.room_generation = generation;
    batch_event.context.has_room_generation = true;
    batch_event.context.operation_id = batch_operation.id().empty()
        ? diagnostic::NewCorrelationId() : diagnostic::OpaqueId{};
    if (!batch_operation.id().empty())
        batch_event.context.operation_id.Assign(batch_operation.id());
    batch_event.batch_track_count = static_cast<std::uint32_t>(items.size());
    EmitDiagnostic(batch_event);
    std::vector<TelemetryOperationSpan> member_operations;
    member_operations.reserve(items.size());
    for (std::size_t index = 0; index < items.size(); ++index) {
        member_operations.emplace_back(
            telemetry_owner, telemetry::OperationKind::PublishTrack);
    }

    std::vector<PublishedSenderBundle> sender_bundles;
    std::vector<proto::TrackPublishedResponse> responses;
    responses.reserve(items.size());
    bool server_acknowledged_any = false;
    bool add_track_dispatched_any = false;

    try {
        for (std::size_t index = 0; index < effective_requests.size(); ++index) {
            // SendAsync itself can suspend: do not let a later member reuse a
            // receipt invalidated while an earlier member was being sent.
            if (validate_video_preflights[index]) validate_video_preflights[index]();
            co_await signal->SendAsync(effective_requests[index]);
            add_track_dispatched_any = true;
        }

        for (const auto& pa : pending_acks) {
            auto response = co_await WaitAwaitable<proto::TrackPublishedResponse>(
                pa.ack,
                operation_timeouts_.publish,
                OperationKind::PublishTrack,
                OperationErrorCode::TrackPublishTimeout,
                "wait_track_published_ack");
            server_acknowledged_any = true;
            responses.push_back(std::move(response));
        }

        {
            std::lock_guard lock(room_mutex_);
            for (const auto& pa : pending_acks) {
                pending_track_publishes_.erase(pa.cid);
            }
        }

        for (size_t index = 0; index < items.size(); ++index) {
            sender_bundles.push_back(co_await AddTrackToPublisherAsync(
                items[index].track,
                generation,
                video_plans[index].has_value()
                    ? std::optional<VideoPublishOptions>{
                        video_plans[index]->effective}
                    : std::nullopt,
                audio_plans[index].has_value()
                    ? std::optional<AudioPublishPolicy>{
                        audio_plans[index]->effective}
                    : std::nullopt,
                effective_requests[index].add_track().encryption() == proto::Encryption::GCM
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
                , linux_encoder_factories[index]
#endif
                ));
        }

        // 单次全量 SDP 重协商
        co_await NegotiatePublisherAsync(operation_timeouts_.negotiation, generation);
        for (std::size_t index = 0; index < sender_bundles.size(); ++index) {
            co_await ApplyPublishedSenderScalabilityModesAsync(
                sender_bundles[index], generation);
            if (video_plans[index]) {
                co_await ReconcileAndValidateVideoSendersAsync(
                    sender_bundles[index], video_plans[index]->effective, generation,
                    std::dynamic_pointer_cast<LocalVideoTrack>(items[index].track),
                    validate_video_preflights[index]);
            }
        }

        if (generation != session_generation_.load(std::memory_order_acquire)) {
            throw OperationError(OperationKind::PublishTrack,
                                 OperationErrorCode::Cancelled,
                                 "publish_commit",
                                 "session changed before batch publication commit");
        }

        std::vector<std::shared_ptr<TrackPublication>> publications;
        publications.reserve(items.size());
        {
            std::lock_guard lock(room_mutex_);
            if (!local || local != local_participant_) {
                throw OperationError(OperationKind::PublishTrack,
                                     OperationErrorCode::Cancelled,
                                     "publish_commit",
                                     "local participant changed before batch publication commit");
            }
            for (size_t i = 0; i < items.size(); ++i) {
                items[i].track->set_sid(responses[i].track().sid());
                auto pub = std::make_shared<TrackPublication>(
                    items[i].track, responses[i].track().sid(), responses[i].track().name());
                local->add_publication(pub);
                published_sender_track_ids_[items[i].track.get()] =
                    sender_bundles[i].track_ids;
                media_publication_policies_[items[i].track.get()] = planning_e2ee
                    ? planning_e2ee->AcquireMediaPublishPolicy() : nullptr;
                publications.push_back(pub);
            }
        }

        if (telemetry_owner) {
            const auto committed_at = std::chrono::steady_clock::now();
            for (std::size_t index = 0; index < items.size(); ++index) {
                const auto rtc_track = items[index].track->rtc_track();
                const auto rtc_track_id = rtc_track
                    ? rtc_track->id() : std::string{};
                telemetry::LocalVideoPublishDescriptor video_publish;
                video_publish.publication_sid = publications[index]->sid();
                video_publish.source_kind = items[index].track->source() == TrackSource::Camera
                    ? telemetry::LocalVideoSourceKind::Camera
                    : (items[index].track->source() == TrackSource::ScreenShareVideo
                        ? telemetry::LocalVideoSourceKind::Screen
                        : telemetry::LocalVideoSourceKind::Unknown);
                if (video_plans[index].has_value()) {
                    const auto& plan = *video_plans[index];
                    video_publish.requested_codec = plan.requested_codec;
                    video_publish.effective_codec = plan.effective_codec;
                    video_publish.fallback_reason = plan.fallback_reason;
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
                    video_publish.requested_backend = plan.requested_backend;
                    video_publish.backend_reason = plan.backend_reason;
#endif
                    video_publish.source = video_publish.source_kind == telemetry::LocalVideoSourceKind::Camera
                        ? "camera" : (video_publish.source_kind == telemetry::LocalVideoSourceKind::Screen
                            ? "screen" : "unknown");
                    video_publish.mode = !plan.effective.scalability_mode.empty()
                        ? "svc"
                        : (plan.effective.simulcast && plan.effective.layers.size() > 1
                            ? "simulcast" : "single");
                    video_publish.resolved_profile = plan.effective_codec == "av1"
                        ? "profile0" : "negotiated";
                    video_publish.resolved_scalability =
                        plan.effective.scalability_mode.empty()
                            ? "none" : plan.effective.scalability_mode;
                    video_publish.sender_track_ids = sender_bundles[index].track_ids;
                }
                telemetry_owner->RegisterLocalPublication(
                    LocalPublishTelemetryKey(rtc_track_id),
                    generation,
                    local_publication_epochs[index],
                    items[index].track->kind() == TrackKind::Video
                        ? telemetry::LocalMediaKind::Video
                        : telemetry::LocalMediaKind::Audio,
                    rtc_track_id,
                    publish_accepted_at,
                    !items[index].track->muted(),
                    local_video_probes[index],
                    committed_at,
                    local_audio_probes[index],
                    std::move(video_publish));
            }
        }
        Log("TRACK", "BATCH_PUBLISHED",
            "Local audio/video batch published: " + std::to_string(publications.size()) + " tracks in a single SDP negotiation");
        SchedulePublisherMediaDiagnostic(generation);
        for (auto& operation : member_operations) {
            operation.Finish(telemetry::OperationOutcome::Success);
        }
        batch_operation.Finish(telemetry::OperationOutcome::Success);
        batch_event.kind = diagnostic::EventKind::MediaPublishBatchTerminal;
        batch_event.outcome = diagnostic::Outcome::Success;
        batch_event.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - publish_accepted_at).count();
        EmitDiagnostic(batch_event);
        co_return publications;
    } catch (...) {
        const auto operation_outcome = TelemetryOutcome(std::current_exception());
        for (std::size_t index = 0; index < local_video_probes.size(); ++index) {
            if (!local_video_probes[index]) continue;
            local_video_probes[index]->active.store(false, std::memory_order_release);
            if (auto video = std::dynamic_pointer_cast<LocalVideoTrack>(
                    items[index].track)) {
                video->set_publish_telemetry_probe({});
            }
        }
        for (std::size_t index = 0; index < local_audio_probes.size(); ++index) {
            if (!local_audio_probes[index]) continue;
            local_audio_probes[index]->active.store(false, std::memory_order_release);
            if (auto audio = std::dynamic_pointer_cast<LocalAudioTrack>(
                    items[index].track)) {
                audio->set_publish_telemetry_probe({});
            }
        }
        {
            std::lock_guard lock(room_mutex_);
            for (const auto& pa : pending_acks) {
                pending_track_publishes_.erase(pa.cid);
            }
        }
        if (!sender_bundles.empty() && WebRTCManager::Instance().signaling_thread()) {
            webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
            {
                std::lock_guard lock(room_mutex_);
                pc = publisher_pc_;
            }
            WebRTCManager::Instance().signaling_thread()->BlockingCall([this, pc, sender_bundles]() {
                if (pc) {
                    for (auto bundle = sender_bundles.rbegin();
                         bundle != sender_bundles.rend(); ++bundle) {
                        RollbackPublishedSenderBundle(pc, *bundle);
                    }
                }
            });
        }
        if (server_acknowledged_any || add_track_dispatched_any) {
            Log("ERROR", "PUBLISH_BATCH_ROLLBACK",
                "Batch publication failed; rolled back local senders and requested Publisher SDP renegotiation to reconcile server tracks");
            NegotiatePublisher(generation);
        }
        for (auto& operation : member_operations) {
            operation.Finish(operation_outcome);
        }
        batch_operation.Finish(operation_outcome);
        batch_event.kind = diagnostic::EventKind::MediaPublishBatchTerminal;
        batch_event.outcome = DiagnosticOutcome(operation_outcome);
        batch_event.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - publish_accepted_at).count();
        ProjectOperationError(std::current_exception(), batch_event);
        EmitDiagnostic(batch_event);
        throw;
    }
}

void Room::SchedulePublisherMediaDiagnostic(uint64_t generation) {
    // Two bounded snapshots distinguish startup from stalled media. All native
    // sender inspection stays in RequestRtcStats on the signaling thread.
    static std::atomic<uint64_t> next_diagnostic_id{0};
    const auto diagnostic_id = next_diagnostic_id.fetch_add(1, std::memory_order_relaxed) + 1;
    livekit::safe_co_spawn(executor_, [self = shared_from_this(), generation, diagnostic_id]() -> asio::awaitable<void> {
        const auto started = std::chrono::steady_clock::now();
        // Retain only identity across timers so diagnostics cannot keep a native
        // PC alive after Disconnect has released it and stopped its threads.
        webrtc::PeerConnectionInterface* publisher_identity = nullptr;
        {
            std::lock_guard lock(self->room_mutex_);
            if (!self->IsNativeGenerationCurrentLocked(generation)) co_return;
            publisher_identity = self->publisher_pc_.get();
        }
        if (!publisher_identity) co_return;
        std::optional<StatsReport> previous_stats;
        std::map<std::string, VideoFrameDiagnostics> previous_sources;
        for (int sample = 1; sample <= 2; ++sample) {
            asio::steady_timer timer(self->executor_);
            timer.expires_at(started + std::chrono::seconds(sample == 1 ? 2 : 5));
            co_await timer.async_wait(asio::use_awaitable);
            webrtc::scoped_refptr<webrtc::PeerConnectionInterface> publisher;
            {
                std::lock_guard lock(self->room_mutex_);
                if (!self->IsNativeGenerationCurrentLocked(generation) ||
                    publisher_identity != self->publisher_pc_.get()) co_return;
                publisher = self->publisher_pc_;
            }

            auto stats = co_await CollectRtcStats(publisher, self->executor_);
            publisher = nullptr;
            std::vector<std::pair<std::string, std::shared_ptr<LocalVideoTrack>>> videos;
            {
                std::lock_guard lock(self->room_mutex_);
                if (!self->IsNativeGenerationCurrentLocked(generation) ||
                    publisher_identity != self->publisher_pc_.get()) co_return;
                if (self->local_participant_) {
                    for (const auto& [sid, publication] : self->local_participant_->tracks()) {
                        if (publication) {
                            if (auto video = std::dynamic_pointer_cast<LocalVideoTrack>(publication->track())) {
                                videos.emplace_back(sid, std::move(video));
                            }
                        }
                    }
                }
            }
            const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            const std::string prefix = "run=" + std::to_string(diagnostic_id) +
                ", sample=" + std::to_string(sample) +
                ", elapsed_ms=" + std::to_string(elapsed_ms);
            for (const auto& [sid, video] : videos) {
                const auto frames = video->frame_diagnostics();
                const auto previous = previous_sources.find(sid);
                const auto before = previous == previous_sources.end()
                    ? VideoFrameDiagnostics{} : previous->second;
                self->Log("MEDIA", "PUBLISH_SOURCE", prefix +
                    ", track=" + MediaDiagnosticToken(video->name()) +
                    ", sid=" + MediaDiagnosticToken(sid) +
                    ", muted=" + (video->muted() ? "yes" : "no") +
                    ", source_frames=" + MediaCounter(frames.source_available, frames.source_frames) +
                    ", source_delta=" + MediaCounterDelta(frames.source_available, frames.source_frames,
                        before.source_available, before.source_frames) +
                    ", rtc_input_frames=" + MediaCounter(frames.rtc_available, frames.rtc_input_frames) +
                    ", rtc_input_delta=" + MediaCounterDelta(frames.rtc_available, frames.rtc_input_frames,
                        before.rtc_available, before.rtc_input_frames) +
                    ", rtc_output_frames=" + MediaCounter(frames.rtc_available, frames.rtc_output_frames) +
                    ", rtc_output_delta=" + MediaCounterDelta(frames.rtc_available, frames.rtc_output_frames,
                        before.rtc_available, before.rtc_output_frames) +
                    ", rtc_dropped_frames=" + MediaCounter(frames.rtc_available, frames.rtc_dropped_frames) +
                    ", output_stage=OnFrame_submission");
                previous_sources[sid] = frames;
            }
            if (!stats) {
                self->Log("MEDIA", "PUBLISH_RTP", prefix + ", status=unavailable");
                previous_stats.reset();
                continue;
            }
            const auto totals = SummarizePublisherRtp(*stats);
            self->Log("MEDIA", "PUBLISH_RTP", prefix +
                ", outbound_streams=" + std::to_string(stats->outbound_rtp.size()) +
                ", audio_streams=" + std::to_string(totals.audio_streams) +
                ", video_streams=" + std::to_string(totals.video_streams) +
                ", unknown_kind_streams=" + std::to_string(totals.unknown_kind_streams) +
                ", audio_packets=" + MediaCounter(totals.audio_packets_available, totals.audio_packets) +
                ", video_frames_encoded=" + MediaCounter(totals.video_frames_available, totals.video_frames) +
                ", video_packets=" + MediaCounter(totals.video_packets_available, totals.video_packets) +
                ", video_bytes=" + MediaCounter(totals.video_bytes_available, totals.video_bytes) +
                ", senders=" + MediaCounter(stats->senders_available, stats->senders.size()));
            for (const auto& sender : stats->senders) {
                self->Log("MEDIA", "PUBLISH_SENDER", prefix +
                    ", track=" + MediaDiagnosticToken(sender.track_id) +
                    ", kind=" + MediaDiagnosticToken(sender.kind) +
                    ", mid=" + (sender.mid_available ? MediaDiagnosticToken(sender.mid) : "N/A") +
                    ", enabled=" + (sender.track_enabled ? "yes" : "no") +
                    ", direction=" + MediaDiagnosticToken(sender.direction) +
                    ", current_direction=" + (sender.current_direction_available
                        ? MediaDiagnosticToken(sender.current_direction) : "N/A") +
                    ", encodings=" + std::to_string(sender.encoding_count) +
                    ", active_encodings=" + MediaCounter(sender.encoding_count > 0, sender.active_encoding_count));
            }
            for (const auto& stream : stats->outbound_rtp) {
                const OutboundRtpStreamStats* before = nullptr;
                if (previous_stats && !stream.id.empty()) {
                    const auto found = std::find_if(previous_stats->outbound_rtp.begin(),
                        previous_stats->outbound_rtp.end(), [&](const auto& candidate) {
                            return candidate.id == stream.id && candidate.kind == stream.kind &&
                                candidate.mid == stream.mid && candidate.rid == stream.rid;
                        });
                    if (found != previous_stats->outbound_rtp.end()) before = &*found;
                }
                self->Log("MEDIA", "PUBLISH_RTP_STREAM", prefix +
                    ", kind=" + (stream.kind_available ? MediaDiagnosticToken(stream.kind) : "N/A") +
                    ", mid=" + (stream.mid_available ? MediaDiagnosticToken(stream.mid) : "N/A") +
                    ", rid=" + (stream.rid_available ? MediaDiagnosticToken(stream.rid) : "N/A") +
                    ", frames_encoded=" + MediaCounter(stream.frames_encoded_available, stream.frames_encoded) +
                    ", frames_encoded_delta=" + MediaCounterDelta(stream.frames_encoded_available, stream.frames_encoded,
                        before && before->frames_encoded_available, before ? before->frames_encoded : 0) +
                    ", packets=" + MediaCounter(stream.packets_sent_available, stream.packets_sent) +
                    ", packets_delta=" + MediaCounterDelta(stream.packets_sent_available, stream.packets_sent,
                        before && before->packets_sent_available, before ? before->packets_sent : 0) +
                    ", bytes=" + MediaCounter(stream.bytes_sent_available, stream.bytes_sent) +
                    ", bytes_delta=" + MediaCounterDelta(stream.bytes_sent_available, stream.bytes_sent,
                        before && before->bytes_sent_available, before ? before->bytes_sent : 0));
            }
            previous_stats = std::move(stats);
        }
    });
}

asio::awaitable<void> Room::RemoveLocalTrackFromPublisherAsync(
    std::shared_ptr<Track> track,
    uint64_t generation) {
    auto operation_admission = AdmitOperation(OperationKind::UnpublishTrack, "RemoveLocalTrackFromPublisherAsync");
    if (!track || !track->rtc_track()) {
        throw OperationError(OperationKind::UnpublishTrack,
                             OperationErrorCode::InvalidState,
                             "remove_sender_validate",
                             "local track has no native WebRTC track");
    }

    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
    std::vector<std::string> sender_track_ids;
    {
        std::lock_guard lock(room_mutex_);
        if (generation != session_generation_.load(std::memory_order_acquire) ||
            connection_state_ != ConnectionState::Connected) {
            throw OperationError(OperationKind::UnpublishTrack,
                                 OperationErrorCode::Cancelled,
                                 "remove_sender_validate",
                                 "room session changed while unpublishing");
        }
        pc = publisher_pc_;
        const auto owned = published_sender_track_ids_.find(track.get());
        if (owned != published_sender_track_ids_.end()) {
            sender_track_ids = owned->second;
        }
    }
    if (!pc || !WebRTCManager::Instance().signaling_thread()) {
        throw OperationError(OperationKind::UnpublishTrack,
                             OperationErrorCode::InvalidState,
                             "remove_sender_validate",
                             "publisher peer connection is unavailable");
    }

    auto completion = std::make_shared<AwaitableState<void>>(executor_);
    auto cancel_native = callback_gate_->CancelWhenClosed(
        [weak = std::weak_ptr(completion)] {
            if (auto pending = weak.lock()) {
                FailAwaitable(pending, std::make_exception_ptr(OperationError(
                    OperationKind::UnpublishTrack, OperationErrorCode::Cancelled,
                    "remove_sender", "room is retired")));
            }
        });
    struct RemoveTrackTaskParams {
        std::shared_ptr<Room> room;
        std::shared_ptr<AwaitableState<void>> completion;
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
        webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> rtc_track;
        std::vector<std::string> sender_track_ids;
        uint64_t generation = 0;
        bool screen_share = false;
    };
    auto* params = new RemoveTrackTaskParams{
        shared_from_this(), completion, pc, track->rtc_track(),
        std::move(sender_track_ids), generation,
        track->source() == TrackSource::ScreenShareVideo};
    WebRTCManager::Instance().signaling_thread()->PostTask([params]() {
        std::unique_ptr<RemoveTrackTaskParams> owned(params);
        auto& task = *owned;
        if (task.generation != task.room->session_generation_.load(std::memory_order_acquire)) {
            FailAwaitable(task.completion, std::make_exception_ptr(OperationError(
                OperationKind::UnpublishTrack,
                OperationErrorCode::Cancelled,
                "remove_sender",
                "session changed before sender removal")));
            return;
        }

        std::vector<webrtc::scoped_refptr<webrtc::RtpSenderInterface>> senders;
        for (const auto& sender : task.pc->GetSenders()) {
            if (!sender || !sender->track()) continue;
            if (sender->track() == task.rtc_track ||
                sender->track()->id() == task.rtc_track->id() ||
                std::find(task.sender_track_ids.begin(),
                          task.sender_track_ids.end(),
                          sender->track()->id()) != task.sender_track_ids.end()) {
                senders.push_back(sender);
            }
        }
        if (senders.empty()) {
            FailAwaitable(task.completion, std::make_exception_ptr(OperationError(
                OperationKind::UnpublishTrack,
                OperationErrorCode::InvalidState,
                "remove_sender",
                "no publisher sender owns the requested local track")));
            return;
        }

        for (const auto& sender : senders) {
            const auto result = task.pc->RemoveTrackOrError(sender);
            if (!result.ok()) {
                FailAwaitable(task.completion, std::make_exception_ptr(OperationError(
                    OperationKind::UnpublishTrack,
                    OperationErrorCode::StateUncertain,
                    "remove_sender",
                    result.message(),
                    true)));
                return;
            }
            if (task.screen_share) {
                bool found = false;
                for (const auto& transceiver : task.pc->GetTransceivers()) {
                    if (!transceiver || transceiver->sender() != sender) continue;
                    found = true;
                    const auto stopped = transceiver->StopStandard();
                    if (!stopped.ok()) {
                        FailAwaitable(task.completion, std::make_exception_ptr(OperationError(
                            OperationKind::UnpublishTrack,
                            OperationErrorCode::StateUncertain,
                            "retire_screen_transceiver", stopped.message(), true)));
                        return;
                    }
                    break;
                }
                if (!found) {
                    FailAwaitable(task.completion, std::make_exception_ptr(OperationError(
                        OperationKind::UnpublishTrack,
                        OperationErrorCode::StateUncertain,
                        "retire_screen_transceiver",
                        "screen sender has no transceiver", true)));
                    return;
                }
            }
        }
        CompleteAwaitable(task.completion);
    });

    co_await WaitAwaitable<void>(
        completion,
        operation_timeouts_.publish,
        OperationKind::UnpublishTrack,
        OperationErrorCode::TrackUnpublishTimeout,
        "remove_sender");
    {
        std::lock_guard lock(room_mutex_);
        if (IsNativeGenerationCurrentLocked(generation)) {
            published_sender_track_ids_.erase(track.get());
            media_publication_policies_.erase(track.get());
        }
    }
}

void Room::BindLocalUnpublishHandler() {
    auto self = shared_from_this();
    local_participant_->SetAsyncUnpublishTrackHandler(
        [self](std::string track_sid)
            -> asio::awaitable<std::shared_ptr<TrackPublication>> {
            co_return co_await self->UnpublishLocalTrackAsync(std::move(track_sid));
        });
}

asio::awaitable<std::shared_ptr<TrackPublication>> Room::UnpublishLocalTrackAsync(
    std::string track_sid) {
    auto operation_admission = AdmitOperation(OperationKind::UnpublishTrack, "UnpublishLocalTrackAsync");
    if (track_sid.empty()) {
        throw OperationError(OperationKind::UnpublishTrack,
                             OperationErrorCode::InvalidState,
                             "unpublish_validate",
                             "track SID is empty");
    }

    const auto generation = session_generation_.load(std::memory_order_acquire);
    std::shared_ptr<LocalParticipant> local;
    std::shared_ptr<TrackPublication> publication;
    std::shared_ptr<Track> track;
    std::shared_ptr<LocalUnpublishOverrides> instrumentation_hooks;
    std::shared_ptr<telemetry::SessionTelemetry> telemetry_owner;
    std::string local_publish_telemetry_key;
    {
        std::lock_guard lock(room_mutex_);
        if (connection_state_ != ConnectionState::Connected ||
            generation != session_generation_.load(std::memory_order_acquire)) {
            throw OperationError(OperationKind::UnpublishTrack,
                                 OperationErrorCode::InvalidState,
                                 "unpublish_validate",
                                 "room is not connected");
        }
        local = local_participant_;
        publication = local ? local->get_publication(track_sid) : nullptr;
        track = publication ? publication->track() : nullptr;
        if (!local || !publication || !track) {
            throw OperationError(OperationKind::UnpublishTrack,
                                 OperationErrorCode::InvalidState,
                                 "unpublish_validate",
                                 "track SID is not an active local publication");
        }
        if (pending_local_unpublishes_.contains(track_sid)) {
            throw OperationError(OperationKind::UnpublishTrack,
                                 OperationErrorCode::InvalidState,
                                 "unpublish_validate",
                                 "an unpublish operation for this track is already in progress");
        }
        pending_local_unpublishes_.emplace(track_sid,
                                            PendingLocalUnpublish{generation, publication, false});
        instrumentation_hooks = local_unpublish_overrides_;
        telemetry_owner = session_telemetry_.lock();
        if (const auto rtc_track = track->rtc_track()) {
            local_publish_telemetry_key = LocalPublishTelemetryKey(rtc_track->id());
        }
    }
    TelemetryOperationSpan unpublish_operation(
        telemetry_owner, telemetry::OperationKind::Unpublish);
    const auto unpublish_started_at = std::chrono::steady_clock::now();
    diagnostic::Event unpublish_event;
    unpublish_event.kind = diagnostic::EventKind::MediaUnpublishStarted;
    unpublish_event.context.room_generation = generation;
    unpublish_event.context.has_room_generation = true;
    unpublish_event.context.operation_id = unpublish_operation.id().empty()
        ? diagnostic::NewCorrelationId() : diagnostic::OpaqueId{};
    if (!unpublish_operation.id().empty())
        unpublish_event.context.operation_id.Assign(unpublish_operation.id());
    unpublish_event.media_kind = track->kind() == TrackKind::Video
        ? diagnostic::MediaKind::Video : diagnostic::MediaKind::Audio;
    EmitDiagnostic(unpublish_event);

    bool sender_removed = false;
    try {
        if (instrumentation_hooks) {
            co_await instrumentation_hooks->remove_sender(track, generation);
        } else {
            co_await RemoveLocalTrackFromPublisherAsync(track, generation);
        }
        sender_removed = true;
        {
            std::lock_guard lock(room_mutex_);
            const auto it = pending_local_unpublishes_.find(track_sid);
            if (it == pending_local_unpublishes_.end() ||
                it->second.generation != generation ||
                generation != session_generation_.load(std::memory_order_acquire)) {
                throw OperationError(OperationKind::UnpublishTrack,
                                     OperationErrorCode::Cancelled,
                                     "unpublish_negotiate",
                                     "session changed after sender removal");
            }
            it->second.sender_removed = true;
        }

        if (instrumentation_hooks) {
            co_await instrumentation_hooks->negotiate(operation_timeouts_.negotiation, generation);
        } else {
            co_await NegotiatePublisherAsync(operation_timeouts_.negotiation, generation);
        }

        std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;
        {
            std::lock_guard lock(room_mutex_);
            const auto it = pending_local_unpublishes_.find(track_sid);
            if (generation != session_generation_.load(std::memory_order_acquire) ||
                connection_state_ != ConnectionState::Connected ||
                !local || local != local_participant_ ||
                it == pending_local_unpublishes_.end() ||
                it->second.publication != publication) {
                throw OperationError(OperationKind::UnpublishTrack,
                                     OperationErrorCode::Cancelled,
                                     "unpublish_commit",
                                     "session changed before local unpublish commit");
            }

            local->remove_publication(track_sid);
            published_track_records_.erase(
                std::remove_if(published_track_records_.begin(),
                               published_track_records_.end(),
                               [&track_sid, &track](const PublishedTrackRecord& record) {
                                   return record.previous_sid == track_sid || record.track == track;
                               }),
                published_track_records_.end());
            pending_local_unpublishes_.erase(it);
            listeners_snapshot = listeners_;
        }

        for (const auto& listener : listeners_snapshot) {
            DeliverListener({generation}, listener, [&](RoomListener& target) {
                target.OnLocalTrackUnpublished(publication);
            });
        }
        publication->set_track(nullptr);
        if (telemetry_owner && !local_publish_telemetry_key.empty()) {
            const auto video = std::dynamic_pointer_cast<LocalVideoTrack>(track);
            telemetry_owner->EndLocalPublication(local_publish_telemetry_key,
                std::chrono::steady_clock::now(), video
                    ? std::optional{SampleLocalVideoPipeline(video)} : std::nullopt);
        }
        if (auto video = std::dynamic_pointer_cast<LocalVideoTrack>(track)) {
            video->set_publish_telemetry_probe({});
        }
        track->set_sid("");
        Log("TRACK", "LOCAL_UNPUBLISHED",
            "Publisher SDP Answer confirmed local track unpublication");
        unpublish_operation.Finish(telemetry::OperationOutcome::Success);
        unpublish_event.kind = diagnostic::EventKind::MediaUnpublishTerminal;
        unpublish_event.outcome = diagnostic::Outcome::Success;
        unpublish_event.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - unpublish_started_at).count();
        EmitDiagnostic(unpublish_event);
        co_return publication;
    } catch (const std::exception& error) {
        const auto operation_outcome = TelemetryOutcome(std::current_exception());
        bool state_uncertain = false;
        {
            std::lock_guard lock(room_mutex_);
            const auto it = pending_local_unpublishes_.find(track_sid);
            if (it != pending_local_unpublishes_.end()) {
                state_uncertain = sender_removed || it->second.sender_removed;
                if (!state_uncertain) {
                    pending_local_unpublishes_.erase(it);
                }
            }
        }
        if (state_uncertain) {
            Log("ERROR", "UNPUBLISH_STATE_UNCERTAIN",
                "Local sender removed without SDP Answer; recovery will reconcile with server state; " +
                    secure_log::ExceptionSummary("unpublish_negotiate"));
            unpublish_operation.Finish(telemetry::OperationOutcome::Failure);
            unpublish_event.kind = diagnostic::EventKind::MediaUnpublishTerminal;
            unpublish_event.outcome = diagnostic::Outcome::Failure;
            unpublish_event.error_code = diagnostic::ErrorCode::StateUncertain;
            unpublish_event.retryable = true;
            unpublish_event.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - unpublish_started_at).count();
            EmitDiagnostic(unpublish_event);
            throw OperationError(OperationKind::UnpublishTrack,
                                 OperationErrorCode::StateUncertain,
                                 "unpublish_negotiate",
                                 "publisher sender was removed before unpublish negotiation completed",
                                 true);
        }
        unpublish_operation.Finish(operation_outcome);
        unpublish_event.kind = diagnostic::EventKind::MediaUnpublishTerminal;
        unpublish_event.outcome = DiagnosticOutcome(operation_outcome);
        unpublish_event.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - unpublish_started_at).count();
        ProjectOperationError(std::current_exception(), unpublish_event);
        EmitDiagnostic(unpublish_event);
        throw;
    }
}

void Room::OnNegotiationFailed(uint64_t generation) {
    CompleteNegotiation("publisher negotiation failed", generation);
}

asio::awaitable<void> Room::NegotiatePublisherAsync(
    std::chrono::milliseconds timeout,
    uint64_t generation,
    bool ice_restart) {
    auto operation_admission = AdmitOperation(OperationKind::Negotiate, "NegotiatePublisherAsync");
    std::shared_ptr<AwaitableState<void>> completion;
    bool should_start = false;
    {
        std::lock_guard lock(room_mutex_);
        if (generation != session_generation_.load(std::memory_order_acquire)) {
            throw OperationError(OperationKind::Negotiate,
                                 OperationErrorCode::Cancelled,
                                 "negotiate_start",
                                 "session changed before negotiation started");
        }
        if (!publisher_pc_ || !signal_client_) {
            throw OperationError(OperationKind::Negotiate,
                                 OperationErrorCode::InvalidState,
                                 "negotiate_start",
                                 "publisher peer connection or signal client is missing");
        }
        completion = std::make_shared<AwaitableState<void>>(executor_);
        negotiation_waiters_.push_back(completion);
        negotiation_ice_restart_requested_ = negotiation_ice_restart_requested_ || ice_restart;
        if (negotiation_state_ == NegotiationState::Idle) {
            negotiation_state_ = NegotiationState::InProgress;
            should_start = true;
        } else {
            // Keep every waiter pending until the final coalesced round reaches stable.
            negotiation_state_ = NegotiationState::PendingRetry;
        }
    }
    if (should_start) ExecuteNegotiatePublisher(generation);

    try {
        co_await WaitAwaitable<void>(completion, timeout,
                                     OperationKind::Negotiate,
                                     OperationErrorCode::NegotiationFailed,
                                     "publisher_negotiation");
    } catch (...) {
        std::lock_guard lock(room_mutex_);
        // Completion/failure/cancellation removes waiters before waking them.
        // A waiter still registered here expired in WaitAwaitable.
        const bool timed_out = IsSignalGenerationCurrentLocked(generation) &&
            std::find(negotiation_waiters_.begin(), negotiation_waiters_.end(), completion) != negotiation_waiters_.end();
        negotiation_waiters_.erase(
            std::remove(negotiation_waiters_.begin(), negotiation_waiters_.end(), completion),
            negotiation_waiters_.end());
        if (timed_out && publisher_sdp_trace_) {
            publisher_sdp_trace_->Record(diagnostic::SdpAction::Callback, diagnostic::SdpPhase::Failed,
                diagnostic::SdpState::Unknown, diagnostic::SdpState::Unknown, diagnostic::SdpReason::Timeout);
            if (negotiation_waiters_.empty())
                publisher_sdp_trace_->Finish(diagnostic::Outcome::Timeout, diagnostic::SdpReason::Timeout);
        }
        throw;
    }
}

void Room::CompleteNegotiation(
    const std::string& error,
    uint64_t generation) {
    std::vector<std::shared_ptr<AwaitableState<void>>> waiters;
    {
        std::lock_guard lock(room_mutex_);
        if (!IsSignalGenerationCurrentLocked(generation)) return;
        waiters.swap(negotiation_waiters_);
        negotiation_state_ = NegotiationState::Idle;
        subscriber_negotiating_ = false;
    }
    for (const auto& completion : waiters) {
        if (error.empty()) {
            CompleteAwaitable(completion);
        } else {
            FailAwaitable(completion, std::make_exception_ptr(OperationError(
                OperationKind::Negotiate,
                OperationErrorCode::NegotiationFailed,
                "publisher_negotiation",
                error,
                true)));
        }
    }
}

void Room::ExecuteNegotiatePublisher(uint64_t generation) {
    auto operation_admission = callback_gate_->Enter();
    if (!operation_admission) return;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pub_pc;
    std::shared_ptr<SignalClient> client;
    bool ice_restart = false;
    bool unavailable = false;
    std::shared_ptr<diagnostic::SdpNegotiationTrace> trace;
    {
        std::lock_guard lock(room_mutex_);
        if (!IsSignalGenerationCurrentLocked(generation)) return;
        pub_pc = publisher_pc_;
        client = signal_client_;
        ice_restart = negotiation_ice_restart_requested_;
        negotiation_ice_restart_requested_ = false;
        if (!pub_pc || !client) {
            unavailable = true;
        } else if (pub_pc->signaling_state() != webrtc::PeerConnectionInterface::SignalingState::kStable) {
            negotiation_state_ = NegotiationState::PendingRetry;
            auto self = shared_from_this();
            auto retry_timer = std::make_shared<asio::steady_timer>(executor_, std::chrono::milliseconds(50));
            retry_timer->async_wait([self, retry_timer, generation](const asio::error_code& ec) {
                if (!ec) {
                    bool should_run = false;
                    {
                        std::lock_guard lock(self->room_mutex_);
                        if (self->IsSignalGenerationCurrentLocked(generation) &&
                            self->negotiation_state_ == NegotiationState::PendingRetry) {
                            self->negotiation_state_ = NegotiationState::InProgress;
                            should_run = true;
                        }
                    }
                    if (should_run) {
                        self->ExecuteNegotiatePublisher(generation);
                    }
                }
            });
            return;
        }
    }

    if (unavailable) {
        CompleteNegotiation(
            "publisher peer connection or signal client became unavailable",
            generation);
        return;
    }

    {
        std::lock_guard lock(room_mutex_);
        if (!IsSignalGenerationCurrentLocked(generation)) return;
        trace = SdpTraceLocked(diagnostic::SdpRole::Publisher, generation, true, ice_restart);
    }
    auto self = shared_from_this();
    WebRTCManager::Instance().CreateOffer(pub_pc, executor_,
        [self, client, pub_pc, generation, trace](const std::string& sdp, const std::string& err) {
            {
                std::lock_guard lock(self->room_mutex_);
                if (!self->IsSignalGenerationCurrentLocked(generation)) {
                    trace->Record(diagnostic::SdpAction::Callback, diagnostic::SdpPhase::Rejected,
                        diagnostic::SdpState::Unknown, diagnostic::SdpState::Unknown, diagnostic::SdpReason::GenerationExpired);
                    trace->Finish(diagnostic::Outcome::Cancelled, diagnostic::SdpReason::GenerationExpired);
                    return;
                }
            }
            if (!err.empty()) {
                self->Log("ERROR", "OFFER_FAIL", secure_log::OpaqueSummary("create_offer"));
                self->OnNegotiationFailed(generation);
                return;
            }

            self->TracePublisherSdpFormats(sdp, false, generation, trace);
            WebRTCManager::Instance().SetLocalDescription(pub_pc, "offer", sdp, self->executor_,
                [self, client, pub_pc, sdp, generation, trace](const std::string& set_local_err) {
                    {
                        std::lock_guard lock(self->room_mutex_);
                        if (!self->IsSignalGenerationCurrentLocked(generation)) {
                            trace->Record(diagnostic::SdpAction::Callback, diagnostic::SdpPhase::Rejected,
                                diagnostic::SdpState::Unknown, diagnostic::SdpState::Unknown, diagnostic::SdpReason::GenerationExpired);
                            trace->Finish(diagnostic::Outcome::Cancelled, diagnostic::SdpReason::GenerationExpired);
                            return;
                        }
                    }
                    if (!set_local_err.empty()) {
                        self->Log("ERROR", "LOCAL_DESC_FAIL",
                                  secure_log::OpaqueSummary("set_local_offer"));
                        self->OnNegotiationFailed(generation);
                        return;
                    }

                    proto::SignalRequest req;
                    auto* offer_msg = req.mutable_offer();
                    offer_msg->set_type("offer");
                    offer_msg->set_sdp(sdp);

                    // Limit bindings to active sending sections in this exact
                    // offer. A transceiver added while CreateOffer/SetLocal is
                    // pending belongs to the next negotiation round.
                    std::set<std::string> offered_send_mids;
                    std::istringstream sdp_stream(sdp);
                    std::string line;
                    std::string cur_mid;
                    bool section_active = false;
                    bool section_sending = true;
                    const auto finish_section = [&] {
                        if (section_active && section_sending && !cur_mid.empty()) {
                            offered_send_mids.insert(cur_mid);
                        }
                    };
                    while (std::getline(sdp_stream, line)) {
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        if (line.rfind("m=", 0) == 0) {
                            finish_section();
                            cur_mid.clear();
                            section_sending = true;
                            std::istringstream media_line(line.substr(2));
                            std::string media, port;
                            media_line >> media >> port;
                            section_active = !port.empty() && port != "0";
                        } else if (line.rfind("a=mid:", 0) == 0) {
                            cur_mid = line.substr(6);
                        } else if (line == "a=bundle-only") {
                            // Port zero is valid for a bundle-only section;
                            // it denotes rejection only without this attribute.
                            section_active = true;
                        } else if (line == "a=recvonly" || line == "a=inactive") {
                            section_sending = false;
                        } else if (line == "a=sendonly" || line == "a=sendrecv") {
                            section_sending = true;
                        }
                    }
                    finish_section();

                    // The native local track ID is the publication CID. SDP
                    // MSID may retain a previous sender ID after reuse/rebind;
                    // it must never overwrite this authoritative binding.
                    for (const auto& transceiver : pub_pc->GetTransceivers()) {
                        if (!transceiver) continue;
                        const auto mid = transceiver->mid();
                        const auto sender = transceiver->sender();
                        const auto track = sender ? sender->track() : nullptr;
                        if (!mid || !offered_send_mids.contains(*mid) || !track) continue;
                        (*offer_msg->mutable_mid_to_track_id())[*mid] = track->id();
                    }
                    for (const auto& [mid, cid] : offer_msg->mid_to_track_id()) {
                        self->Log("SIGNAL", "MID_TRACK_BIND", "mid=" + MediaDiagnosticToken(mid) +
                            ", cid=" + MediaDiagnosticToken(cid));
                    }

                    livekit::safe_co_spawn(self->executor_, [self, client, req = std::move(req), generation, trace]() -> asio::awaitable<void> {
                        try {
                            co_await client->SendAsync(req, trace);
                        } catch (const std::exception& error) {
                            self->CompleteNegotiation(error.what(), generation);
                        }
                    });
                    LogSdpNegotiationDetails(
                        *self, "SDP_OFFER_DETAIL", "publisher_offer_prepared", sdp);
                }, self->callback_gate_, trace);
        }, ice_restart, callback_gate_, trace);
}

void Room::SendPublishOffer() {
    auto operation_admission = callback_gate_->Enter();
    if (!operation_admission) return;
    NegotiatePublisher();
}


} // namespace livekit
