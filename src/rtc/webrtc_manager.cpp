#if defined(_WIN32)
#include <winsock2.h>
#endif
#include <asio.hpp>
#include "webrtc_manager.h"
#include "auto_hardware_video_encoder.h"
#include "bounded_encoder_worker.h"
#include "final_h264_level_guard.h"
#include "retained_vp8_decoder.h"
#include "telemetry/diagnostic_pipeline.h"
#include "telemetry/sdp_negotiation_trace.h"
#include "telemetry/received_decode_diagnostic.h"
#include "core/executor_lifetime.h"
#include "audio_playout_device_selection.h"
#include "audio_playout_transport.h"
#include "media/audio_apm.h"
#include "rtc_base/ssl_adapter.h"
#include "api/create_peerconnection_factory.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/video_codecs/video_encoder_factory.h"
#include "api/video_codecs/video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/h264_profile_level_id.h"
#include "api/sequence_checker.h"
#include "api/field_trials_view.h"
#include "rtc_base/logging.h"

#include "api/environment/environment_factory.h"
#include "api/audio/audio_device.h"
#include "api/audio/create_audio_device_module.h"
#include "modules/video_coding/codecs/vp8/include/vp8.h"
#include "modules/video_coding/codecs/vp9/include/vp9.h"
#include "modules/video_coding/codecs/h264/include/h264.h"
#include "modules/video_coding/codecs/av1/dav1d_decoder.h"
#include "modules/video_coding/codecs/av1/libaom_av1_encoder.h"
#include "modules/video_coding/svc/scalability_mode_util.h"
#include "media/engine/simulcast_encoder_adapter.h"
#if defined(COHAVORA_HAS_BACKEND_ENCODERS)
#include "d3d11_amf_video_encoder.h"
#include "media/engine/internal_encoder_factory.h"
#include "modules/video_coding/codecs/external/backend_video_encoder.h"
#include "modules/video_coding/codecs/external/hardware_encoder_probe.h"
#include "modules/video_coding/codecs/external/amf_video_encoder.h"
#include "modules/video_coding/codecs/external/nvenc_video_encoder.h"
#include "modules/video_coding/codecs/external/nvenc_video_encoder_backend.h"
#include "modules/video_coding/codecs/external/qsv_video_encoder.h"
#include "modules/video_coding/codecs/external/qsv_video_encoder_backend.h"
#include "modules/video_coding/codecs/external/x265_video_encoder.h"
#endif
#if defined(COHAVORA_HAS_H265_DECODER)
#include "h265_video_decoder.h"
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#if defined(_WIN32)
#include <objbase.h>
#else
#include <strings.h>
#endif

namespace webrtc {
namespace webrtc_checks_impl {
    void FatalLog(char const* file, int line) {}
}
}

namespace livekit {

namespace {

int CompareCodecName(const char* left, const char* right) {
#if defined(_WIN32)
    return _stricmp(left, right);
#else
    return strcasecmp(left, right);
#endif
}

enum class EncoderPreference { kAuto, kSoftware, kAmf, kNvenc, kQsv };
std::atomic<EncoderPreference> video_encoder_preference{EncoderPreference::kAuto};

enum VideoEncoderCapabilityFlag : unsigned {
    kCapabilitiesReady = 1u << 0,
    kAmfH264 = 1u << 1,
    kAmfH265 = 1u << 2,
    kNvencH264 = 1u << 3,
    kNvencH265 = 1u << 4,
    kQsvH264 = 1u << 5,
    kQsvH265 = 1u << 6,
};

std::atomic<unsigned> video_encoder_capability_flags{0};
std::once_flag video_encoder_capability_probe_once;

const char* PreferenceName(EncoderPreference preference) {
    switch (preference) {
    case EncoderPreference::kSoftware: return "software";
    case EncoderPreference::kAmf: return "amf";
    case EncoderPreference::kNvenc: return "nvenc";
    case EncoderPreference::kQsv: return "qsv";
    default: return "auto";
    }
}

bool CodecNameEquals(const webrtc::SdpVideoFormat& format, const char* expected) {
    return CompareCodecName(format.name.c_str(), expected) == 0;
}

void EmitEncoderFormat(const webrtc::SdpVideoFormat& format,
                       diagnostic::VideoFormatSource source) {
    const auto projected = detail::ProjectH264Format(format.name, format.parameters);
    if (!projected) return;
    diagnostic::VideoFormatDiagnosticRow row;
    row.source = source;
    row.profile_level_id = projected->profile_level_id;
    row.profile = projected->profile;
    row.level = projected->level;
    row.packetization_mode = projected->packetization_mode;
    row.level_asymmetry = projected->level_asymmetry;
    row.valid = projected->valid;
    row.profile_level_defaulted = projected->profile_level_defaulted;
    diagnostic::Event event;
    event.SetVideoFormat(row);
    event.thread_role = diagnostic::ThreadRole::Rtc;
    diagnostic::EmitBusinessEvent(event);
}

#if defined(COHAVORA_HAS_BACKEND_ENCODERS)
class EncoderPreferenceTrials final : public webrtc::FieldTrialsView {
public:
    EncoderPreferenceTrials(const webrtc::Environment& original,
                            EncoderPreference preference)
        : original_(original), preference_(preference) {}

    std::string Lookup(absl::string_view key) const override {
        if (key == "WebRTC-H264-HardwareEncoder" ||
            key == "WebRTC-H265-HardwareEncoder") {
            switch (preference_) {
            case EncoderPreference::kAmf: return "AMF";
            case EncoderPreference::kNvenc: return "NVENC";
            case EncoderPreference::kQsv: return "QSV";
            case EncoderPreference::kSoftware: return "Disabled";
            default: return "auto";
            }
        }
        if (key == "WebRTC-H265-Encoder") {
            return preference_ == EncoderPreference::kSoftware ? "x265" : "auto";
        }
        return original_.field_trials().Lookup(key);
    }

    std::unique_ptr<webrtc::FieldTrialsView> CreateCopy() const override {
        return std::make_unique<EncoderPreferenceTrials>(*this);
    }

private:
    // Keep every delegated utility alive, including field trials supplied by
    // the caller. The new environment owns this immutable overlay.
    webrtc::Environment original_;
    EncoderPreference preference_;
};

webrtc::Environment EncoderEnvironment(const webrtc::Environment& original,
                                      EncoderPreference preference) {
    webrtc::EnvironmentFactory factory(original);
    std::unique_ptr<const webrtc::FieldTrialsView> trials =
        std::make_unique<EncoderPreferenceTrials>(original, preference);
    factory.Set(std::move(trials));
    return factory.Create();
}

bool HardwareRetirementFailed() {
    return webrtc::external_encoder::HardwareEncoderProbes().CheckAny() ==
        webrtc::external_encoder::Status::kRetirementFailed;
}

const std::vector<webrtc::SdpVideoFormat>& CachedBackendFormats() {
    // Discovery may initialize hardware. Run it once per process, rather than
    // on every settings/UI capability query. Creation still checks live state.
    static const auto formats = [] {
        try {
            return webrtc::InternalEncoderFactory().GetSupportedFormats();
        } catch (...) {
            // Cache failure too: a later UI read must not retry discovery.
            return std::vector<webrtc::SdpVideoFormat>{};
        }
    }();
    return formats;
}

const std::vector<webrtc::SdpVideoFormat>& CachedSoftwareFormats() {
    static const auto formats = [] {
        try {
            auto result = webrtc::SupportedH264Codecs(true);
            // Explicit software intent must not initialize a vendor GPU backend.
            for (const char* level : {"120", "93"}) {
                webrtc::SdpVideoFormat h265("H265", {{"profile-id", "1"},
                    {"tier-flag", "0"}, {"level-id", level}, {"tx-mode", "SRST"}});
                if (webrtc::IsX265VideoEncoderAvailable(h265)) {
                    result.push_back(std::move(h265));
                    break;
                }
            }
            return result;
        } catch (...) {
            return std::vector<webrtc::SdpVideoFormat>{};
        }
    }();
    return formats;
}

std::vector<webrtc::SdpVideoFormat> BackendFormats(EncoderPreference preference) {
    if (preference == EncoderPreference::kSoftware) return CachedSoftwareFormats();
    if (HardwareRetirementFailed()) return {};
    return CachedBackendFormats();
}

bool H265SendingAvailable() {
#if defined(COHAVORA_HAS_H265_DECODER)
    return IsH265DecoderAvailable();
#else
    return false;
#endif
}

void LogBackendCreation(const webrtc::SdpVideoFormat& format,
                        EncoderPreference preference,
                        const webrtc::VideoEncoder* encoder) {
    if (!encoder) {
        RTC_LOG(LS_WARNING) << "Product encoder codec=" << format.name
            << " requested=" << PreferenceName(preference)
            << " effective=unavailable";
        return;
    }
    const auto info = encoder->GetEncoderInfo();
    RTC_LOG(LS_INFO) << "Product encoder codec=" << format.name
        << " requested=" << PreferenceName(preference)
        << " effective=" << info.implementation_name
        << " hardware=" << info.is_hardware_accelerated;
    // This is construction metadata, not an assertion that InitEncode or an
    // actual hardware frame has succeeded. SDK wrappers log runtime fallback.
}

#endif

void EmitRtcLifecycle(diagnostic::RtcStatus status) noexcept {
    diagnostic::Event event;
    event.kind = diagnostic::EventKind::RtcLifecycle;
    event.thread_role = diagnostic::ThreadRole::Rtc;
    event.rtc_status = status;
    diagnostic::EmitBusinessEvent(event);
}

class SingleStreamVideoEncoderFactory : public webrtc::VideoEncoderFactory {
public:
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        std::vector<webrtc::SdpVideoFormat> formats{
            webrtc::SdpVideoFormat("VP8")};
#if defined(COHAVORA_HAS_BACKEND_ENCODERS)
        const auto preference = video_encoder_preference.load(std::memory_order_relaxed);
        for (auto format : BackendFormats(preference)) {
            if (CodecNameEquals(format, "H264")) {
                formats.push_back(std::move(format));
            } else if (CodecNameEquals(format, "H265") &&
                       webrtc::IsBackendH265FormatSupported(format) &&
                       H265SendingAvailable()) {
                format.scalability_modes = {webrtc::ScalabilityMode::kL1T1};
                formats.push_back(std::move(format));
            }
        }
#else
        if (webrtc::H264Encoder::IsSupported()) {
            auto h264 = webrtc::SupportedH264Codecs();
            formats.insert(formats.end(), h264.begin(), h264.end());
        }
#endif
        const auto vp9 = webrtc::SupportedVP9Codecs(true);
        formats.insert(formats.end(), vp9.begin(), vp9.end());
        return formats;
    }

    CodecSupport QueryCodecSupport(
        const webrtc::SdpVideoFormat& format,
        std::optional<std::string> scalability_mode) const override {
        CodecSupport support;
#if defined(COHAVORA_HAS_BACKEND_ENCODERS)
        if (CodecNameEquals(format, "H264") || CodecNameEquals(format, "H265")) {
            if (!format.IsCodecInList(GetSupportedFormats())) return {};
            if (CodecNameEquals(format, "H265")) {
                return {.is_supported = webrtc::IsBackendH265FormatSupported(format) &&
                    (!scalability_mode || *scalability_mode == "L1T1")};
            }
            if (!scalability_mode) return {.is_supported = true};
            const auto mode = webrtc::ScalabilityModeFromString(*scalability_mode);
            return {.is_supported = mode && webrtc::H264Encoder::SupportsScalabilityMode(*mode)};
        }
#endif
        if (CompareCodecName(format.name.c_str(), "VP9") == 0) {
            if (!format.IsCodecInList(webrtc::SupportedVP9Codecs(false))) return {};
            if (!scalability_mode) return {.is_supported = true};
            const auto mode = webrtc::ScalabilityModeFromString(*scalability_mode);
            return {.is_supported = mode && webrtc::VP9Encoder::SupportsScalabilityMode(*mode)};
        }
        support.is_supported = CompareCodecName(format.name.c_str(), "VP8") == 0 ||
            (CompareCodecName(format.name.c_str(), "H264") == 0 &&
             webrtc::H264Encoder::IsSupported());
        return support;
    }

    std::unique_ptr<webrtc::VideoEncoder> Create(
        const webrtc::Environment& env,
        const webrtc::SdpVideoFormat& format) override {
        EmitEncoderFormat(format, diagnostic::VideoFormatSource::EncoderLayerFactory);
        if (CompareCodecName(format.name.c_str(), "VP8") == 0) {
            return webrtc::CreateVp8Encoder(env);
        }
#if defined(COHAVORA_HAS_BACKEND_ENCODERS)
        if (CodecNameEquals(format, "H264") || CodecNameEquals(format, "H265")) {
            const auto preference = video_encoder_preference.load(std::memory_order_relaxed);
            if (!format.IsCodecInList(BackendFormats(preference)) ||
                (CodecNameEquals(format, "H265") &&
                 (!webrtc::IsBackendH265FormatSupported(format) ||
                  !H265SendingAvailable()))) {
                RTC_LOG(LS_WARNING) << "Product encoder codec=" << format.name
                    << " requested=" << PreferenceName(preference)
                    << " effective=unavailable reason=format_or_capability_unavailable";
                return nullptr;
            }
            const auto selected_env = EncoderEnvironment(env, preference);
            std::unique_ptr<webrtc::VideoEncoder> encoder;
            if (CodecNameEquals(format, "H264") &&
                preference == EncoderPreference::kSoftware) {
                encoder = webrtc::CreateH264Encoder(
                    selected_env, webrtc::H264EncoderSettings::Parse(format));
            } else {
                const auto capabilities = GetVideoEncoderCapabilities();
                const bool use_d3d11_amf =
                    !HardwareRetirementFailed() &&
                    (preference == EncoderPreference::kAmf ||
                     (preference == EncoderPreference::kAuto &&
                      ((CodecNameEquals(format, "H264") &&
                        !capabilities.nvenc.h264 && !capabilities.qsv.h264 &&
                        capabilities.amf.h264) ||
                       (CodecNameEquals(format, "H265") &&
                        !capabilities.nvenc.h265 && !capabilities.qsv.h265 &&
                        capabilities.amf.h265))));
                if (use_d3d11_amf) {
                    webrtc::external_encoder::Status probe_status =
                        webrtc::external_encoder::Status::kUnavailable;
                    encoder = CreateD3d11AmfVideoEncoder(
                        selected_env, format, &probe_status);
                    if (probe_status ==
                            webrtc::external_encoder::Status::kRetirementFailed ||
                        HardwareRetirementFailed()) {
                        encoder.reset();
                    } else if (!encoder && preference == EncoderPreference::kAuto) {
                        encoder = webrtc::InternalEncoderFactory().Create(
                            selected_env, format);
                    }
                } else {
                    // InternalEncoderFactory returns one encoder, without another
                    // SimulcastEncoderAdapter. SDK policy owns vendor and fallback.
                    encoder = webrtc::InternalEncoderFactory().Create(selected_env, format);
                }
            }
            LogBackendCreation(format, preference, encoder.get());
            if (encoder && CodecNameEquals(format, "H264"))
                return std::make_unique<detail::FinalH264LevelGuard>(std::move(encoder), format, true);
            return encoder;
        }
#else
        if (CompareCodecName(format.name.c_str(), "H264") == 0 &&
            webrtc::H264Encoder::IsSupported()) {
            return std::make_unique<detail::FinalH264LevelGuard>(webrtc::CreateH264Encoder(
                env, webrtc::H264EncoderSettings::Parse(format)), format, true);
        }
#endif
        if (CompareCodecName(format.name.c_str(), "VP9") == 0 &&
            format.IsCodecInList(webrtc::SupportedVP9Codecs(false))) {
            const auto profile = webrtc::ParseSdpForVP9Profile(format.parameters)
                .value_or(webrtc::VP9Profile::kProfile0);
            return webrtc::CreateVp9Encoder(env, {.profile = profile});
        }
        return nullptr;
    }
};

class CustomVideoEncoderFactory : public webrtc::VideoEncoderFactory {
public:
    CustomVideoEncoderFactory()
        : simulcast_factory_(std::make_unique<SingleStreamVideoEncoderFactory>()) {}

    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        std::vector<webrtc::SdpVideoFormat> formats;

        // Preserve codec preference order. VP9 single-stream/SVC passes through
        // the adapter; multiple independent RIDs can fall back to one encoder
        // per stream when native VP9 rejects unequal simulcast frame rates.
        const auto legacy_formats = simulcast_factory_->GetSupportedFormats();
        AppendFormatsNamed(formats, legacy_formats, "VP8");
        AppendFormatsNamed(formats, legacy_formats, "H264");
        AppendFormatsNamed(formats, legacy_formats, "H265");
        const auto vp9_formats = webrtc::SupportedVP9Codecs(true);
        formats.insert(formats.end(), vp9_formats.begin(), vp9_formats.end());
        // SetParameters validates an explicit mode against the advertised RTP
        // capability. Keep it consistent with QueryCodecSupport and libaom.
        auto av1 = webrtc::SdpVideoFormat::AV1Profile0();
        av1.scalability_modes.push_back(webrtc::ScalabilityMode::kL1T1);
        formats.push_back(std::move(av1));
        return formats;
    }

    std::vector<webrtc::SdpVideoFormat> GetImplementations() const override {
        // Create() owns hardware selection and same-codec software fallback.
        // Keep the RTP codec consistent with the publication's SFU metadata.
        return GetSupportedFormats();
    }

    CodecSupport QueryCodecSupport(
        const webrtc::SdpVideoFormat& format,
        std::optional<std::string> scalability_mode) const override {
        if (CodecNameEquals(format, "VP8") || CodecNameEquals(format, "H264") ||
            CodecNameEquals(format, "H265")) {
            return simulcast_factory_->QueryCodecSupport(format, scalability_mode);
        }
        if (CodecNameEquals(format, "VP9")) {
            const auto supported_formats = webrtc::SupportedVP9Codecs(false);
            if (!format.IsCodecInList(supported_formats)) return {};
            if (!scalability_mode.has_value()) return {.is_supported = true};
            const auto parsed_mode = webrtc::ScalabilityModeFromString(*scalability_mode);
            return {.is_supported = parsed_mode.has_value() &&
                webrtc::VP9Encoder::SupportsScalabilityMode(*parsed_mode)};
        }
        if (CodecNameEquals(format, "AV1")) {
            const std::vector<webrtc::SdpVideoFormat> supported{
                webrtc::SdpVideoFormat::AV1Profile0()};
            return {
                .is_supported = format.IsCodecInList(supported) &&
                    (!scalability_mode.has_value() || *scalability_mode == "L1T1")};
        }
        return {};
    }

    std::unique_ptr<webrtc::VideoEncoder> Create(
        const webrtc::Environment& env,
        const webrtc::SdpVideoFormat& format) override {
        if (CodecNameEquals(format, "H265")) {
            // HEVC backend contract is SRST/L1T1. Do not wrap it in simulcast.
            return simulcast_factory_->Create(env, format);
        }
        if (CodecNameEquals(format, "VP8") || CodecNameEquals(format, "H264")) {
            auto encoder = std::make_unique<webrtc::SimulcastEncoderAdapter>(
                env, simulcast_factory_.get(), nullptr, format);
            if (CodecNameEquals(format, "H264")) {
                EmitEncoderFormat(format, diagnostic::VideoFormatSource::EncoderFactory);
                return std::make_unique<detail::FinalH264LevelGuard>(std::move(encoder), format);
            }
            return encoder;
        }
        if (CodecNameEquals(format, "AV1")) {
            const std::vector<webrtc::SdpVideoFormat> supported{
                webrtc::SdpVideoFormat::AV1Profile0()};
            if (!format.IsCodecInList(supported)) return nullptr;
            return webrtc::CreateLibaomAv1Encoder(env);
        }
        if (CodecNameEquals(format, "VP9")) {
            const auto supported_formats = webrtc::SupportedVP9Codecs(false);
            if (!format.IsCodecInList(supported_formats)) return nullptr;
            return std::make_unique<webrtc::SimulcastEncoderAdapter>(
                env, simulcast_factory_.get(), nullptr, format);
        }
        return nullptr;
    }

private:
    static bool CodecNameEquals(
        const webrtc::SdpVideoFormat& format,
        const char* expected) {
        return CompareCodecName(format.name.c_str(), expected) == 0;
    }

    static void AppendFormatsNamed(
        std::vector<webrtc::SdpVideoFormat>& destination,
        const std::vector<webrtc::SdpVideoFormat>& source,
        const char* codec_name) {
        for (const auto& format : source) {
            if (CodecNameEquals(format, codec_name)) {
                destination.push_back(format);
            }
        }
    }

    std::unique_ptr<webrtc::VideoEncoderFactory> simulcast_factory_;
};

class CustomVideoDecoderFactory : public webrtc::VideoDecoderFactory {
public:
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        std::vector<webrtc::SdpVideoFormat> formats;
        formats.push_back(webrtc::SdpVideoFormat("VP8"));
        for (const auto& format : webrtc::SupportedVP9DecoderCodecs()) {
            formats.push_back(format);
        }
        if (webrtc::H264Decoder::IsSupported()) {
            for (const auto& f : webrtc::SupportedH264DecoderCodecs()) {
                formats.push_back(f);
            }
        }
        formats.push_back(webrtc::SdpVideoFormat::AV1Profile0());
        formats.push_back(webrtc::SdpVideoFormat::AV1Profile1());
#if defined(COHAVORA_HAS_H265_DECODER)
        if (IsH265DecoderAvailable()) {
            formats.emplace_back("H265", webrtc::CodecParameterMap{
                {"profile-id", "1"}, {"tier-flag", "0"},
                {"level-id", "120"}, {"tx-mode", "SRST"}});
        }
#endif
        return formats;
    }

    std::unique_ptr<webrtc::VideoDecoder> Create(
        const webrtc::Environment& env,
        const webrtc::SdpVideoFormat& format) override {
        const auto observed = [](std::unique_ptr<webrtc::VideoDecoder> decoder) {
#if COHAVORA_ENABLE_MEDIA_DIAGNOSTICS
            return telemetry::WithReceivedDecodeDiagnostic(std::move(decoder));
#else
            return decoder;
#endif
        };
        if (CompareCodecName(format.name.c_str(), "VP8") == 0) {
            return observed(render::CreateRetainedVp8Decoder(env));
        }
        if (CompareCodecName(format.name.c_str(), "VP9") == 0) {
            const auto supported_formats = webrtc::SupportedVP9DecoderCodecs();
            if (!format.IsCodecInList(supported_formats)) return nullptr;
            return observed(webrtc::VP9Decoder::Create());
        }
        if (CompareCodecName(format.name.c_str(), "H264") == 0) {
            return observed(webrtc::H264Decoder::Create());
        }
#if defined(COHAVORA_HAS_H265_DECODER)
        if (CodecNameEquals(format, "H265")) {
            return observed(CreateH265VideoDecoder(env, format));
        }
#endif
        if (CompareCodecName(format.name.c_str(), "AV1") == 0) {
            const std::vector<webrtc::SdpVideoFormat> supported{
                webrtc::SdpVideoFormat::AV1Profile0(),
                webrtc::SdpVideoFormat::AV1Profile1()};
            if (!format.IsCodecInList(supported)) return nullptr;
            return observed(webrtc::CreateDav1dDecoder(env));
        }
        return nullptr;
    }
};

} // namespace

namespace detail {

int32_t PlayoutAudioTransportWrapper::RecordedDataIsAvailable(
    const void* audioSamples,
    size_t nSamples,
    size_t nBytesPerSample,
    size_t nChannels,
    uint32_t samplesPerSec,
    uint32_t totalDelayMS,
    int32_t clockDrift,
    uint32_t currentMicLevel,
    bool keyPressed,
    uint32_t& newMicLevel) {
    if (inner_) {
        return inner_->RecordedDataIsAvailable(audioSamples, nSamples, nBytesPerSample,
                                               nChannels, samplesPerSec, totalDelayMS,
                                               clockDrift, currentMicLevel, keyPressed, newMicLevel);
    }
    return 0;
}

int32_t PlayoutAudioTransportWrapper::NeedMorePlayData(
    size_t nSamples,
    size_t nBytesPerSample,
    size_t nChannels,
    uint32_t samplesPerSec,
    void* audioSamples,
    size_t& nSamplesOut,
    int64_t* elapsed_time_ms,
    int64_t* ntp_time_ms) {
    if (!inner_) {
        nSamplesOut = 0;
        return -1;
    }
    int32_t res = inner_->NeedMorePlayData(nSamples, nBytesPerSample, nChannels,
                                            samplesPerSec, audioSamples, nSamplesOut,
                                            elapsed_time_ms, ntp_time_ms);
    // ADM's legacy names are misleading: nBytesPerSample is bytes per
    // interleaved frame, and nSamplesOut counts all channels' PCM samples.
    if (res == 0 && audioSamples && nSamplesOut > 0 && nChannels > 0 &&
        nBytesPerSample == sizeof(int16_t) * nChannels) {
        const size_t output_frames = std::min(nSamplesOut / nChannels, nSamples);
        const size_t output_samples = output_frames * nChannels;
        warmup_.Process(static_cast<int16_t*>(audioSamples), output_samples,
                        nChannels, samplesPerSec);

        // 将下行扬声器渲染音频作为反向参考信号送入 APM (AEC 回声消除)
        auto apm = WebRTCManager::Instance().apm_processor();
        if (apm) {
            const int16_t* pcm = static_cast<const int16_t*>(audioSamples);
            std::vector<int16_t> render_pcm(pcm, pcm + output_samples);
            AudioFrame render_frame(std::move(render_pcm),
                                    static_cast<int>(samplesPerSec),
                                    static_cast<int>(nChannels),
                                    static_cast<int>(output_frames));
            apm->ProcessRenderFrame(render_frame);
        }
    }
    return res;
}

void PlayoutAudioTransportWrapper::PullRenderData(
    int bits_per_sample,
    int sample_rate,
    size_t number_of_channels,
    size_t number_of_frames,
    void* audio_data,
    int64_t* elapsed_time_ms,
    int64_t* ntp_time_ms) {
    if (inner_) {
        inner_->PullRenderData(bits_per_sample, sample_rate, number_of_channels,
                               number_of_frames, audio_data, elapsed_time_ms, ntp_time_ms);
        if (bits_per_sample == 16 && audio_data) {
            const size_t total_samples = number_of_frames * number_of_channels;
            warmup_.Process(static_cast<int16_t*>(audio_data),
                            total_samples,
                            number_of_channels,
                            static_cast<uint32_t>(sample_rate));

            auto apm = WebRTCManager::Instance().apm_processor();
            if (apm) {
                const int16_t* pcm = static_cast<const int16_t*>(audio_data);
                std::vector<int16_t> render_pcm(pcm, pcm + total_samples);
                AudioFrame render_frame(std::move(render_pcm),
                                        sample_rate,
                                        static_cast<int>(number_of_channels),
                                        static_cast<int>(number_of_frames));
                apm->ProcessRenderFrame(render_frame);
            }
        }
    }
}

} // namespace detail

namespace {

class PlayoutOnlyAudioDeviceModule : public webrtc::AudioDeviceModule {
public:
    explicit PlayoutOnlyAudioDeviceModule(webrtc::scoped_refptr<webrtc::AudioDeviceModule> inner)
        : inner_(inner) {}

    ~PlayoutOnlyAudioDeviceModule() override = default;

    int32_t ActiveAudioLayer(AudioLayer* audioLayer) const override {
        return inner_ ? inner_->ActiveAudioLayer(audioLayer) : -1;
    }

    int32_t RegisterAudioCallback(webrtc::AudioTransport* audioCallback) override {
        if (!inner_) return -1;
        if (audioCallback) {
            auto wrapper = std::make_unique<detail::PlayoutAudioTransportWrapper>(audioCallback);
            const int32_t result = inner_->RegisterAudioCallback(wrapper.get());
            if (result == 0) {
                transport_wrapper_ = std::move(wrapper);
            }
            return result;
        }

        // The native render thread retains this raw pointer. Unregister it
        // before releasing the wrapper, otherwise teardown can race a callback.
        const int32_t result = inner_->RegisterAudioCallback(nullptr);
        if (result == 0) {
            transport_wrapper_.reset();
        }
        return result;
    }

    int32_t Init() override {
        return inner_ ? inner_->Init() : 0;
    }

    int32_t Terminate() override {
#if !defined(_WIN32)
        // VoiceEngine terminates whenever the last PeerConnection goes away,
        // but this ADM belongs to the process-wide factory. The packaged Pulse
        // backend cannot restart its threads after Terminate/Init (quit_ stays
        // set). VoiceEngine already stops both streams and unregisters its
        // transport before this call; retain the idle native device until the
        // manager releases the factory and performs the final shutdown.
        return 0;
#else
        return inner_ ? inner_->Terminate() : 0;
#endif
    }

    int32_t ShutdownNative() {
        return inner_ ? inner_->Terminate() : 0;
    }

    bool Initialized() const override {
        return inner_ ? inner_->Initialized() : true;
    }

    // Keep native playout and the render-reference callback on both platforms.
    int16_t PlayoutDevices() override {
        return inner_ ? inner_->PlayoutDevices() : 0;
    }

    int32_t PlayoutDeviceName(uint16_t index, char name[webrtc::kAdmMaxDeviceNameSize], char guid[webrtc::kAdmMaxGuidSize]) override {
        return inner_ ? inner_->PlayoutDeviceName(index, name, guid) : -1;
    }

    int32_t SetPlayoutDevice(uint16_t index) override {
        if (transport_wrapper_) transport_wrapper_->ResetWarmup();
        return inner_ ? inner_->SetPlayoutDevice(index) : 0;
    }

    int32_t SetPlayoutDevice(WindowsDeviceType device) override {
        if (transport_wrapper_) transport_wrapper_->ResetWarmup();
        return inner_ ? inner_->SetPlayoutDevice(device) : 0;
    }

    int32_t PlayoutIsAvailable(bool* available) override {
        return inner_ ? inner_->PlayoutIsAvailable(available) : 0;
    }

    int32_t InitPlayout() override {
        return inner_ ? inner_->InitPlayout() : 0;
    }

    bool PlayoutIsInitialized() const override {
        return inner_ ? inner_->PlayoutIsInitialized() : false;
    }

    bool HasAudioCallback() const {
        return transport_wrapper_ != nullptr;
    }

    int32_t StartPlayout() override {
        // Windows ADM can copy an unfilled render buffer when no callback is
        // registered. Never start its native render thread in that state.
        if (!inner_ || !HasAudioCallback()) return -1;
        transport_wrapper_->ResetWarmup();
        return inner_->StartPlayout();
    }

    int32_t StopPlayout() override {
        return inner_ ? inner_->StopPlayout() : 0;
    }

    bool Playing() const override {
        return inner_ ? inner_->Playing() : false;
    }

    int32_t InitSpeaker() override {
        return inner_ ? inner_->InitSpeaker() : 0;
    }

    bool SpeakerIsInitialized() const override {
        return inner_ ? inner_->SpeakerIsInitialized() : false;
    }

    int32_t SpeakerVolumeIsAvailable(bool* available) override {
        return inner_ ? inner_->SpeakerVolumeIsAvailable(available) : 0;
    }

    int32_t SetSpeakerVolume(uint32_t volume) override {
        return inner_ ? inner_->SetSpeakerVolume(volume) : 0;
    }

    int32_t SpeakerVolume(uint32_t* volume) const override {
        return inner_ ? inner_->SpeakerVolume(volume) : 0;
    }

    int32_t MaxSpeakerVolume(uint32_t* maxVolume) const override {
        return inner_ ? inner_->MaxSpeakerVolume(maxVolume) : 0;
    }

    int32_t MinSpeakerVolume(uint32_t* minVolume) const override {
        return inner_ ? inner_->MinSpeakerVolume(minVolume) : 0;
    }

    int32_t SpeakerMuteIsAvailable(bool* available) override {
        return inner_ ? inner_->SpeakerMuteIsAvailable(available) : 0;
    }

    int32_t SetSpeakerMute(bool enable) override {
        return inner_ ? inner_->SetSpeakerMute(enable) : 0;
    }

    int32_t SpeakerMute(bool* enabled) const override {
        return inner_ ? inner_->SpeakerMute(enabled) : 0;
    }

    int32_t StereoPlayoutIsAvailable(bool* available) const override {
        return inner_ ? inner_->StereoPlayoutIsAvailable(available) : 0;
    }

    int32_t SetStereoPlayout(bool enable) override {
        return inner_ ? inner_->SetStereoPlayout(enable) : 0;
    }

    int32_t StereoPlayout(bool* enabled) const override {
        return inner_ ? inner_->StereoPlayout(enabled) : 0;
    }

    int32_t PlayoutDelay(uint16_t* delayMS) const override {
        return inner_ ? inner_->PlayoutDelay(delayMS) : 0;
    }

    // Windows uses the application's WASAPI capture. Linux uses the native
    // PulseAudio/ALSA ADM for microphone capture and audio-track sending.
#if defined(_WIN32)
    int16_t RecordingDevices() override { return 0; }
    int32_t RecordingDeviceName(uint16_t, char[webrtc::kAdmMaxDeviceNameSize], char[webrtc::kAdmMaxGuidSize]) override { return -1; }
    int32_t SetRecordingDevice(uint16_t) override { return 0; }
    int32_t SetRecordingDevice(WindowsDeviceType) override { return 0; }
    int32_t RecordingIsAvailable(bool* available) override { if (available) *available = false; return 0; }
    int32_t InitRecording() override { return 0; }
    bool RecordingIsInitialized() const override { return false; }
    int32_t StartRecording() override { return 0; }
    int32_t StopRecording() override { return 0; }
    bool Recording() const override { return false; }
    int32_t InitMicrophone() override { return 0; }
    bool MicrophoneIsInitialized() const override { return false; }
    int32_t MicrophoneVolumeIsAvailable(bool* available) override { if (available) *available = false; return 0; }
    int32_t SetMicrophoneVolume(uint32_t) override { return 0; }
    int32_t MicrophoneVolume(uint32_t*) const override { return 0; }
    int32_t MaxMicrophoneVolume(uint32_t*) const override { return 0; }
    int32_t MinMicrophoneVolume(uint32_t*) const override { return 0; }
    int32_t MicrophoneMuteIsAvailable(bool* available) override { if (available) *available = false; return 0; }
    int32_t SetMicrophoneMute(bool) override { return 0; }
    int32_t MicrophoneMute(bool*) const override { return 0; }
    int32_t StereoRecordingIsAvailable(bool* available) const override { if (available) *available = false; return 0; }
    int32_t SetStereoRecording(bool) override { return 0; }
    int32_t StereoRecording(bool* enabled) const override { if (enabled) *enabled = false; return 0; }
#else
    int16_t RecordingDevices() override { return inner_ ? inner_->RecordingDevices() : 0; }
    int32_t RecordingDeviceName(uint16_t index,
                                char name[webrtc::kAdmMaxDeviceNameSize],
                                char guid[webrtc::kAdmMaxGuidSize]) override {
        return inner_ ? inner_->RecordingDeviceName(index, name, guid) : -1;
    }
    int32_t SetRecordingDevice(uint16_t index) override {
        return inner_ ? inner_->SetRecordingDevice(index) : -1;
    }
    int32_t SetRecordingDevice(WindowsDeviceType device) override {
        return inner_ ? inner_->SetRecordingDevice(device) : -1;
    }
    int32_t RecordingIsAvailable(bool* available) override {
        return inner_ ? inner_->RecordingIsAvailable(available) : -1;
    }
    int32_t InitRecording() override {
        return inner_ ? recording_gate_.InitRecording(*inner_) : -1;
    }
    bool RecordingIsInitialized() const override {
        return inner_ && inner_->RecordingIsInitialized();
    }
    int32_t StartRecording() override {
        return inner_ ? recording_gate_.StartRecording(*inner_) : -1;
    }
    int32_t StopRecording() override { return inner_ ? inner_->StopRecording() : -1; }
    bool Recording() const override { return inner_ && inner_->Recording(); }
    bool StartNativeRecording() {
        return inner_ && recording_gate_.StartExplicitly(*inner_);
    }
    bool StopNativeRecording() {
        return !inner_ || recording_gate_.StopExplicitly(*inner_);
    }
    void RevokeNativeRecording() { recording_gate_.Revoke(); }
    int32_t InitMicrophone() override { return inner_ ? inner_->InitMicrophone() : -1; }
    bool MicrophoneIsInitialized() const override {
        return inner_ && inner_->MicrophoneIsInitialized();
    }
    int32_t MicrophoneVolumeIsAvailable(bool* available) override {
        return inner_ ? inner_->MicrophoneVolumeIsAvailable(available) : -1;
    }
    int32_t SetMicrophoneVolume(uint32_t volume) override {
        return inner_ ? inner_->SetMicrophoneVolume(volume) : -1;
    }
    int32_t MicrophoneVolume(uint32_t* volume) const override {
        return inner_ ? inner_->MicrophoneVolume(volume) : -1;
    }
    int32_t MaxMicrophoneVolume(uint32_t* max_volume) const override {
        return inner_ ? inner_->MaxMicrophoneVolume(max_volume) : -1;
    }
    int32_t MinMicrophoneVolume(uint32_t* min_volume) const override {
        return inner_ ? inner_->MinMicrophoneVolume(min_volume) : -1;
    }
    int32_t MicrophoneMuteIsAvailable(bool* available) override {
        return inner_ ? inner_->MicrophoneMuteIsAvailable(available) : -1;
    }
    int32_t SetMicrophoneMute(bool muted) override {
        return inner_ ? inner_->SetMicrophoneMute(muted) : -1;
    }
    int32_t MicrophoneMute(bool* muted) const override {
        return inner_ ? inner_->MicrophoneMute(muted) : -1;
    }
    int32_t StereoRecordingIsAvailable(bool* available) const override {
        return inner_ ? inner_->StereoRecordingIsAvailable(available) : -1;
    }
    int32_t SetStereoRecording(bool enabled) override {
        return inner_ ? inner_->SetStereoRecording(enabled) : -1;
    }
    int32_t StereoRecording(bool* enabled) const override {
        return inner_ ? inner_->StereoRecording(enabled) : -1;
    }
#endif

    bool BuiltInAECIsAvailable() const override { return false; }
    bool BuiltInAGCIsAvailable() const override { return false; }
    bool BuiltInNSIsAvailable() const override { return false; }
    int32_t EnableBuiltInAEC(bool) override { return 0; }
    int32_t EnableBuiltInAGC(bool) override { return 0; }
    int32_t EnableBuiltInNS(bool) override { return 0; }

private:
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> inner_;
    std::unique_ptr<detail::PlayoutAudioTransportWrapper> transport_wrapper_;
#if !defined(_WIN32)
    detail::NativeRecordingGate recording_gate_;
#endif
};

struct EnumeratedAudioDevice {
    uint16_t index = 0;
    AudioDeviceInfo info;
};

#if !defined(_WIN32)
std::string EncodeAdmDeviceName(const std::string& name) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(name.size() * 2);
    for (unsigned char byte : name) {
        encoded.push_back(kHex[byte >> 4]);
        encoded.push_back(kHex[byte & 0x0f]);
    }
    return encoded;
}
#endif

std::vector<EnumeratedAudioDevice> EnumerateAudioDevices(
        webrtc::AudioDeviceModule& device, bool recording) {
    std::vector<EnumeratedAudioDevice> result;
    const int16_t count = recording ? device.RecordingDevices() : device.PlayoutDevices();
    for (int16_t index = 0; index < count; ++index) {
        char name[webrtc::kAdmMaxDeviceNameSize] = {};
        char guid[webrtc::kAdmMaxGuidSize] = {};
        const auto status = recording
            ? device.RecordingDeviceName(static_cast<uint16_t>(index), name, guid)
            : device.PlayoutDeviceName(static_cast<uint16_t>(index), name, guid);
        if (status != 0 || !name[0]) continue;
        AudioDeviceInfo info;
        info.name = name;
#if defined(_WIN32)
        info.id = guid;
#else
        // Linux PulseAudio and ALSA return empty GUIDs. Encode raw ADM name
        // bytes as ASCII so Qt's UTF-8 conversion cannot change the ID.
        if (index != 0) {
            info.id = "adm-index:" + std::to_string(index) + ":" +
                EncodeAdmDeviceName(info.name);
        }
#endif
        result.push_back({static_cast<uint16_t>(index), std::move(info)});
    }
    return result;
}

#if !defined(_WIN32)
std::optional<uint16_t> FindAudioDeviceIndex(
        webrtc::AudioDeviceModule& device, bool recording,
        const std::string& device_id) {
    for (const auto& entry : EnumerateAudioDevices(device, recording)) {
        if (entry.info.id == device_id) return entry.index;
    }
    return std::nullopt;
}

std::optional<std::string> AudioDeviceIdentityAtIndex(
        webrtc::AudioDeviceModule& device, bool recording, uint16_t index) {
    for (const auto& entry : EnumerateAudioDevices(device, recording)) {
        if (entry.index == index) {
            // The public default ID is empty; rollback still needs its name.
            return "adm-index:" + std::to_string(index) + ":" +
                EncodeAdmDeviceName(entry.info.name);
        }
    }
    return std::nullopt;
}
#endif

} // namespace

void SetVideoEncoderPreference(const std::string& preference) {
    auto selected = EncoderPreference::kAuto;
    if (CompareCodecName(preference.c_str(), "software") == 0) selected = EncoderPreference::kSoftware;
    else if (CompareCodecName(preference.c_str(), "amf") == 0) selected = EncoderPreference::kAmf;
    else if (CompareCodecName(preference.c_str(), "nvenc") == 0) selected = EncoderPreference::kNvenc;
    else if (CompareCodecName(preference.c_str(), "qsv") == 0) selected = EncoderPreference::kQsv;
    const auto previous = video_encoder_preference.exchange(selected, std::memory_order_relaxed);
    if (previous != selected) {
        RTC_LOG(LS_INFO) << "Product video encoder preference=" << PreferenceName(selected)
            << " appliesTo=new_encoders";
    }
}

std::string GetVideoEncoderPreference() {
    return PreferenceName(video_encoder_preference.load(std::memory_order_relaxed));
}

VideoEncoderCapabilities GetVideoEncoderCapabilities() {
    VideoEncoderCapabilities result;
    const auto flags = video_encoder_capability_flags.load(std::memory_order_acquire);
    result.ready = (flags & kCapabilitiesReady) != 0;
    if (!result.ready) return result;
#if defined(COHAVORA_HAS_BACKEND_ENCODERS)
    // Match the factory's fail-closed rule after unresolved SDK cleanup.
    // This only reads coordinator state; it never waits for probe completion.
    if (HardwareRetirementFailed()) return result;
#endif
    result.amf = {(flags & kAmfH264) != 0, (flags & kAmfH265) != 0};
    result.nvenc = {(flags & kNvencH264) != 0, (flags & kNvencH265) != 0};
    result.qsv = {(flags & kQsvH264) != 0, (flags & kQsvH265) != 0};
    return result;
}

void ProbeVideoEncoderCapabilities() {
    std::call_once(video_encoder_capability_probe_once, [] {
        unsigned flags = kCapabilitiesReady;
#if defined(COHAVORA_HAS_BACKEND_ENCODERS)
        try {
            // Settings also reads these capability caches. Initialize both here
            // so rebuilding the codec choices never starts discovery on the UI.
            (void)CachedBackendFormats();
            (void)CachedSoftwareFormats();
            const webrtc::SdpVideoFormat h264("H264", {
                {"level-asymmetry-allowed", "1"},
                {"packetization-mode", "1"},
                {"profile-level-id", "42001f"}});
            const webrtc::SdpVideoFormat h265("H265", {
                {"profile-id", "1"}, {"tier-flag", "0"},
                {"level-id", "93"}, {"tx-mode", "SRST"}});
            const bool h265_sending = H265SendingAvailable();
            const auto probe = [&](auto probe_backend, unsigned h264_flag, unsigned h265_flag) {
                // Probe APIs initialize only the named vendor, without software or
                // another vendor as fallback. Coordinator calls are serialized.
                if (HardwareRetirementFailed()) return;
                if (probe_backend(h264, nullptr) == webrtc::external_encoder::Status::kOk) {
                    flags |= h264_flag;
                }
                if (h265_sending && !HardwareRetirementFailed() &&
                    probe_backend(h265, nullptr) == webrtc::external_encoder::Status::kOk) {
                    flags |= h265_flag;
                }
            };
            probe(webrtc::ProbeAmfVideoEncoderAvailability, kAmfH264, kAmfH265);
            probe(webrtc::ProbeNvencVideoEncoderAvailability, kNvencH264, kNvencH265);
            probe(webrtc::ProbeQsvVideoEncoderAvailability, kQsvH264, kQsvH265);
            if (HardwareRetirementFailed()) flags = kCapabilitiesReady;
        } catch (...) {
            // Complete pending UI requests even if discovery could not finish.
            flags = kCapabilitiesReady;
        }
#endif
        // Publish a complete snapshot. UI readers keep seeing ready=false
        // while initialization or driver cleanup is in progress.
        video_encoder_capability_flags.store(flags, std::memory_order_release);
    });
}

namespace {

using PreflightStatus = VideoEncoderPreflightStatus;

bool PreflightCancelled(const std::shared_ptr<std::atomic<bool>>& cancelled) {
    return cancelled && cancelled->load(std::memory_order_acquire);
}

std::optional<std::size_t> InvalidPreflightLayer(
        const VideoEncoderPreflightRequest& request) {
    if (request.layers.empty() || request.layers.size() > webrtc::kMaxSimulcastStreams) {
        return 0;
    }
    for (std::size_t index = 0; index < request.layers.size(); ++index) {
        const auto& layer = request.layers[index];
        if (layer.width <= 0 || layer.height <= 0 || layer.width % 2 || layer.height % 2 ||
            layer.width > std::numeric_limits<std::uint16_t>::max() ||
            layer.height > std::numeric_limits<std::uint16_t>::max() ||
            std::int64_t(layer.width) * layer.height > std::numeric_limits<int>::max() ||
            layer.max_fps <= 0 || layer.max_bitrate_bps <= 0) {
            return index;
        }
    }
    return std::nullopt;
}

bool IsPreflightH264Format(const webrtc::SdpVideoFormat& format) {
    if (!CodecNameEquals(format, "H264")) return false;
    const auto mode = format.parameters.find("packetization-mode");
    const auto profile = webrtc::ParseSdpForH264ProfileLevelId(format.parameters);
    return mode != format.parameters.end() && mode->second == "1" && profile &&
        profile->profile == webrtc::H264Profile::kProfileBaseline &&
        profile->level != webrtc::H264Level::kLevel1_b;
}

struct PreflightEncoderProgress {
    // A request has at most three layers and int-valued positive bitrate caps.
    // Their rounded-up kbps values fit 22 bits. Keep the complete codec status,
    // actual tested bitrate, layer and phase in one coherent timeout snapshot.
    static constexpr std::uint64_t kBitrateMask = (std::uint64_t{1} << 22) - 1;
    std::atomic<std::uint64_t> snapshot{0};

    void Publish(std::size_t layer, int status, VideoEncoderPreflightPhase phase,
                 std::uint32_t bitrate_bps) noexcept {
        snapshot.store(static_cast<std::uint32_t>(status) |
            (std::uint64_t(bitrate_bps / 1000) << 32) |
            (std::uint64_t(layer) << 54) |
            (std::uint64_t(phase == VideoEncoderPreflightPhase::Ceiling) << 56),
            std::memory_order_release);
    }

    void ApplyTo(VideoEncoderPreflightResult& result) const noexcept {
        const auto value = snapshot.load(std::memory_order_acquire);
        result.codec_status = static_cast<std::int32_t>(static_cast<std::uint32_t>(value));
        result.tested_bitrate_bps =
            static_cast<std::uint32_t>((value >> 32) & kBitrateMask) * 1000;
        result.failed_layer = static_cast<std::size_t>((value >> 54) & 3);
        result.phase = (value >> 56) & 1 ? VideoEncoderPreflightPhase::Ceiling
                                        : VideoEncoderPreflightPhase::Startup;
    }
};

// Constructed empty on the caller, then only accessed by the worker. In
// particular, a blocked InitEncode/Release retains all of its sibling sessions
// here until that same owner can finish retirement and destroy the group.
class PreflightEncoderGroup final {
public:
    explicit PreflightEncoderGroup(
            std::shared_ptr<PreflightEncoderProgress> progress = {})
        : progress_(std::move(progress)) {}
    ~PreflightEncoderGroup() { Retire(); }

    VideoEncoderPreflightResult Check(
            const VideoEncoderPreflightRequest& request,
            const webrtc::SdpVideoFormat& format,
            const std::string& backend,
            const detail::VideoEncoderPreflightFactory& factory,
            const std::shared_ptr<std::atomic<bool>>& cancelled) {
        Retire();
        if (result_.status == PreflightStatus::RetirementFailed) return result_;
        result_ = {};
        result_.backend = backend;
        if (PreflightCancelled(cancelled)) {
            result_.status = PreflightStatus::Cancelled;
            return result_;
        }
        if (const auto invalid = InvalidPreflightLayer(request)) {
            result_.status = PreflightStatus::InvalidParameters;
            result_.failed_layer = *invalid;
            return result_;
        }
        if (!IsPreflightH264Format(format)) {
            result_.status = PreflightStatus::UnsupportedFormat;
            return result_;
        }
        if (!factory) return result_;

        try {
            encoders_.reserve(request.layers.size());
            result_.status = PreflightStatus::Ready;
            for (const auto phase : {VideoEncoderPreflightPhase::Startup,
                                     VideoEncoderPreflightPhase::Ceiling}) {
                result_.phase = phase;
                for (std::size_t index = 0; index < request.layers.size(); ++index) {
                    result_.failed_layer = index;
                    const auto& layer = request.layers[index];
                    const auto ceiling_kbps = static_cast<unsigned>(
                        (std::uint64_t(layer.max_bitrate_bps) + 999) / 1000);
                    const auto startup_bps = std::min(30000, layer.max_bitrate_bps);
                    const auto start_kbps = phase == VideoEncoderPreflightPhase::Startup
                        ? static_cast<unsigned>((std::uint64_t(startup_bps) + 999) / 1000)
                        : ceiling_kbps;
                    result_.tested_bitrate_bps = start_kbps * 1000;
                    PublishProgress(index, WEBRTC_VIDEO_CODEC_OK, phase,
                                    result_.tested_bitrate_bps);
                    if (PreflightCancelled(cancelled)) {
                        result_.status = PreflightStatus::Cancelled;
                        break;
                    }
                    auto encoder = factory(format);
                    if (!encoder) {
                        result_.status = PreflightStatus::Unavailable;
                        break;
                    }
                    encoders_.push_back({std::move(encoder), result_.tested_bitrate_bps});
                    webrtc::VideoCodec codec;
                    codec.codecType = webrtc::kVideoCodecH264;
                    codec.width = static_cast<std::uint16_t>(layer.width);
                    codec.height = static_cast<std::uint16_t>(layer.height);
                    codec.maxFramerate = static_cast<std::uint32_t>(layer.max_fps);
                    // Each group uses a fresh InitEncode, with the real ceiling
                    // retained while separately checking the startup floor.
                    codec.startBitrate = start_kbps;
                    codec.maxBitrate = ceiling_kbps;
                    codec.minBitrate = 1;
                    codec.active = true;
                    codec.numberOfSimulcastStreams = 0;
                    codec.simulcastStream[0].numberOfTemporalLayers = 1;
                    codec.mode = request.screenshare ? webrtc::VideoCodecMode::kScreensharing
                                                     : webrtc::VideoCodecMode::kRealtimeVideo;
                    codec.SetScalabilityMode(webrtc::ScalabilityMode::kL1T1);
                    *codec.H264() = webrtc::VideoEncoder::GetDefaultH264Settings();
                    codec.H264()->numberOfTemporalLayers = 1;
                    const webrtc::VideoEncoder::Settings settings(
                        webrtc::VideoEncoder::Capabilities(false), 1, 1200);
                    result_.codec_status = encoders_.back().encoder->InitEncode(&codec, settings);
                    PublishProgress(index, result_.codec_status, phase,
                                    result_.tested_bitrate_bps);
                    if (result_.codec_status != WEBRTC_VIDEO_CODEC_OK) {
                        result_.status = PreflightStatus::InitializationFailed;
                        break;
                    }
                    if (!encoders_.back().encoder->GetEncoderInfo().is_hardware_accelerated) {
                        result_.status = PreflightStatus::NotHardware;
                        break;
                    }
                }
                // Retain the complete concurrent group through its last Init,
                // then retire every session before the next group is created.
                Retire();
                if (result_.status != PreflightStatus::RetirementFailed &&
                    PreflightCancelled(cancelled)) {
                    result_.status = PreflightStatus::Cancelled;
                }
                if (!result_.ok()) break;
            }
        } catch (...) {
            result_.status = PreflightStatus::InitializationFailed;
        }

        // An exception during creation/initialization still retires the partial
        // group, including an encoder whose InitEncode did not succeed.
        Retire();
        if (result_.status != PreflightStatus::RetirementFailed &&
            PreflightCancelled(cancelled)) {
            result_.status = PreflightStatus::Cancelled;
        }
        if (result_.ok()) result_.formats = {format};
        return result_;
    }

    VideoEncoderPreflightResult Close() {
        Retire();
        return result_;
    }

private:
    void PublishProgress(std::size_t layer, int status,
                         VideoEncoderPreflightPhase phase,
                         std::uint32_t bitrate_bps) noexcept {
        if (!progress_) return;
        progress_->Publish(layer, status, phase, bitrate_bps);
    }

    void Retire() noexcept {
        for (std::size_t index = 0; index < encoders_.size(); ++index) {
            int status = WEBRTC_VIDEO_CODEC_ERROR;
            const auto bitrate_bps = encoders_[index].bitrate_bps;
            PublishProgress(index, WEBRTC_VIDEO_CODEC_OK, result_.phase, bitrate_bps);
            try { status = encoders_[index].encoder->Release(); } catch (...) {}
            PublishProgress(index, status, result_.phase, bitrate_bps);
            if (status != WEBRTC_VIDEO_CODEC_OK &&
                result_.status != PreflightStatus::RetirementFailed) {
                result_.status = PreflightStatus::RetirementFailed;
                result_.failed_layer = index;
                result_.codec_status = status;
                result_.tested_bitrate_bps = bitrate_bps;
                result_.formats.clear();
            }
        }
        encoders_.clear();
    }

    struct OwnedEncoder {
        std::unique_ptr<webrtc::VideoEncoder> encoder;
        std::uint32_t bitrate_bps;
    };
    std::vector<OwnedEncoder> encoders_;
    VideoEncoderPreflightResult result_;
    std::shared_ptr<PreflightEncoderProgress> progress_;
};

} // namespace

VideoEncoderPreflightResult detail::CheckVideoEncoderPreflightFormat(
        const VideoEncoderPreflightRequest& request,
        const webrtc::SdpVideoFormat& format,
        const std::string& backend,
        const VideoEncoderPreflightFactory& factory,
        std::shared_ptr<std::atomic<bool>> cancelled) {
    PreflightEncoderGroup group;
    return group.Check(request, format, backend, factory, cancelled);
}

VideoEncoderPreflightResult PreflightVideoEncoder(
        const VideoEncoderPreflightRequest& request,
        std::shared_ptr<std::atomic<bool>> cancelled) {
    VideoEncoderPreflightResult result;
    if (PreflightCancelled(cancelled)) {
        result.status = PreflightStatus::Cancelled;
        return result;
    }
    if (const auto invalid = InvalidPreflightLayer(request)) {
        result.status = PreflightStatus::InvalidParameters;
        result.failed_layer = *invalid;
        return result;
    }
    std::vector<webrtc::SdpVideoFormat> formats;
    for (const auto& format : request.formats) {
        if (IsPreflightH264Format(format)) formats.push_back(format);
    }
    if (formats.empty()) {
        result.status = PreflightStatus::UnsupportedFormat;
        return result;
    }
#if defined(COHAVORA_HAS_BACKEND_ENCODERS)
    if (HardwareRetirementFailed()) {
        result.status = PreflightStatus::RetirementFailed;
        return result;
    }
    const auto capabilities = GetVideoEncoderCapabilities();
    if (!capabilities.ready) return result;
    using Vendor = webrtc::external_encoder::HardwareEncoderVendor;
    struct Candidate {
        Vendor vendor;
        const char* name;
        bool available;
        detail::VideoEncoderPreflightFactory factory;
    };
    const std::vector<Candidate> candidates{
        {Vendor::kNvenc, "nvenc", capabilities.nvenc.h264,
         [](const webrtc::SdpVideoFormat& format) {
             return webrtc::CreateBackendVideoEncoder(
                 format, webrtc::external_encoder::CreateNvencH264Backend());
         }},
        {Vendor::kQsv, "qsv", capabilities.qsv.h264,
         [](const webrtc::SdpVideoFormat& format) {
             return webrtc::CreateBackendVideoEncoder(
                 format, webrtc::external_encoder::CreateQsvH264Backend());
         }},
        {Vendor::kAmf, "amf", capabilities.amf.h264,
         [](const webrtc::SdpVideoFormat& format) {
             return CreateD3d11AmfHardwareEncoder(format);
         }}
    };
    const bool automatic = request.preference.empty() ||
        CompareCodecName(request.preference.c_str(), "auto") == 0;
    if (!automatic &&
        std::none_of(candidates.begin(), candidates.end(), [&](const auto& candidate) {
            return CompareCodecName(request.preference.c_str(), candidate.name) == 0;
        })) {
        return result;
    }

    using Worker = detail::BoundedEncoderWorker<PreflightEncoderGroup,
                                               VideoEncoderPreflightResult>;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    const auto remaining = [&] {
        return std::max(std::chrono::milliseconds::zero(),
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()));
    };
    const auto quarantine = [](const Candidate& candidate) {
        webrtc::external_encoder::HardwareEncoderProbes().RecordRetirementFailure(
            candidate.vendor, "hardware parameter preflight retirement unconfirmed");
    };
    try {
        const auto progress = std::make_shared<PreflightEncoderProgress>();
        Worker worker(std::make_unique<PreflightEncoderGroup>(progress),
            [](PreflightEncoderGroup& group) { return group.Close(); });
        const Candidate* attempted = nullptr;
        for (const auto& candidate : candidates) {
            if (!candidate.available || (!automatic &&
                CompareCodecName(request.preference.c_str(), candidate.name) != 0)) {
                continue;
            }
            for (const auto& format : formats) {
                if (PreflightCancelled(cancelled)) {
                    result.status = PreflightStatus::Cancelled;
                    return result;
                }
                if (HardwareRetirementFailed()) {
                    result.status = PreflightStatus::RetirementFailed;
                    return result;
                }
                if (remaining() <= std::chrono::milliseconds::zero()) {
                    if (attempted) quarantine(*attempted);
                    result.status = PreflightStatus::TimedOut;
                    return result;
                }
                // Every capture is owned; a deadline cannot leave references to
                // the caller's request, cancellation state, or stack frame.
                attempted = &candidate;
                progress->snapshot.store(0, std::memory_order_release);
                auto reply = worker.Invoke(
                    [request, format, backend = std::string(candidate.name),
                     vendor = candidate.vendor, factory = candidate.factory,
                     cancelled](PreflightEncoderGroup& group) {
                        // Serialize the complete real-parameter session group
                        // with discovery probes, including its final cleanup.
                        // Run is called only here, never inside a backend.
                        VideoEncoderPreflightResult checked;
                        const auto status =
                            webrtc::external_encoder::HardwareEncoderProbes().Run(
                                vendor, nullptr, [&](std::string*) {
                                    checked = group.Check(
                                        request, format, backend, factory, cancelled);
                                    using Status = webrtc::external_encoder::Status;
                                    return checked.status == PreflightStatus::RetirementFailed
                                        ? Status::kRetirementFailed
                                        : (checked.ok() ? Status::kOk : Status::kUnavailable);
                                });
                        if (status == webrtc::external_encoder::Status::kRetirementFailed) {
                            checked.status = PreflightStatus::RetirementFailed;
                            checked.backend = backend;
                            checked.formats.clear();
                        }
                        return checked;
                    }, remaining());
                if (reply.wait != Worker::Wait::kCompleted || !reply.value) {
                    quarantine(candidate);
                    result.backend = candidate.name;
                    progress->ApplyTo(result);
                    result.status = reply.wait == Worker::Wait::kTimedOut
                        ? PreflightStatus::TimedOut : PreflightStatus::RetirementFailed;
                    return result;
                }
                result = std::move(*reply.value);
                if (result.status == PreflightStatus::RetirementFailed) {
                    quarantine(candidate);
                    return result;
                }
                if (HardwareRetirementFailed()) {
                    result.status = PreflightStatus::RetirementFailed;
                    result.formats.clear();
                    return result;
                }
                if (result.ok() || result.status == PreflightStatus::Cancelled) {
                    const auto closed = worker.Close(remaining());
                    if (closed.wait != Worker::Wait::kCompleted || !closed.value ||
                        closed.value->status == PreflightStatus::RetirementFailed) {
                        quarantine(candidate);
                        result.status = closed.wait == Worker::Wait::kTimedOut
                            ? PreflightStatus::TimedOut : PreflightStatus::RetirementFailed;
                        result.formats.clear();
                    } else if (HardwareRetirementFailed()) {
                        result.status = PreflightStatus::RetirementFailed;
                        result.formats.clear();
                    } else if (PreflightCancelled(cancelled)) {
                        result.status = PreflightStatus::Cancelled;
                        result.formats.clear();
                    }
                    return result;
                }
            }
            // Mirror the backend selected by the runtime's cached capability
            // order. Trying another vendor here could approve AMF/QSV while
            // the actual Auto factory still creates NVENC. The caller owns
            // the codec fallback after this selected backend's formats fail.
            break;
        }
        // Ordinary failures already retired their complete encoder group. Also
        // await worker destruction before returning an exhausted-candidate result.
        const auto closed = worker.Close(remaining());
        if (closed.wait != Worker::Wait::kCompleted || !closed.value ||
            closed.value->status == PreflightStatus::RetirementFailed) {
            if (attempted) quarantine(*attempted);
            result.status = closed.wait == Worker::Wait::kTimedOut
                ? PreflightStatus::TimedOut : PreflightStatus::RetirementFailed;
            result.formats.clear();
        } else if (HardwareRetirementFailed()) {
            result.status = PreflightStatus::RetirementFailed;
            result.formats.clear();
        } else if (PreflightCancelled(cancelled)) {
            result.status = PreflightStatus::Cancelled;
            result.formats.clear();
        }
    } catch (...) {
        result.status = PreflightStatus::Unavailable;
        result.formats.clear();
    }
#endif
    return result;
}

std::unique_ptr<webrtc::VideoEncoderFactory> CreateVideoEncoderFactory() {
    return std::make_unique<CustomVideoEncoderFactory>();
}

bool IsHardwareH264FormatSupported(const webrtc::SdpVideoFormat& format) {
#if defined(COHAVORA_HAS_BACKEND_ENCODERS)
    return webrtc::IsBackendH264FormatSupported(format);
#else
    return false;
#endif
}

std::unique_ptr<webrtc::VideoEncoderFactory::EncoderSelectorInterface>
CreateAutoHardwareH264EncoderSelector(bool allow_vp8_fallback) {
    return std::make_unique<detail::AutoHardwareH264EncoderSelector>(allow_vp8_fallback);
}

std::unique_ptr<webrtc::VideoDecoderFactory> CreateVideoDecoderFactory() {
    return std::make_unique<CustomVideoDecoderFactory>();
}

bool IsVideoEncoderFormatSupported(
    const std::string& codec_name,
    const std::string& scalability_mode) {
    auto factory = CreateVideoEncoderFactory();
    const auto formats = factory->GetSupportedFormats();
    for (const auto& format : formats) {
        if (CompareCodecName(format.name.c_str(), codec_name.c_str()) != 0) continue;
        const auto mode = scalability_mode.empty()
            ? std::optional<std::string>{}
            : std::optional<std::string>{scalability_mode};
        if (factory->QueryCodecSupport(format, mode).is_supported) return true;
    }
    return false;
}

WebRTCManager& WebRTCManager::Instance() {
    static WebRTCManager instance;
    return instance;
}

WebRTCManager::~WebRTCManager() {
    Deinitialize();
}

#include "rtc_base/logging.h"
bool WebRTCManager::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) {
        return true;
    }

    // 确保当前线程启用 COM 多线程环境
#if defined(_WIN32)
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif

    // 屏蔽 WebRTC 原生日志输出
    webrtc::LogMessage::LogToDebug(webrtc::LS_NONE);
    webrtc::LogMessage::SetLogToStderr(false);

    EmitRtcLifecycle(diagnostic::RtcStatus::Starting);
    if (!webrtc::InitializeSSL()) {
        EmitRtcLifecycle(diagnostic::RtcStatus::SslFailed);
        return false;
    }
    ssl_initialized_ = true;

    network_thread_ = webrtc::Thread::CreateWithSocketServer();
    worker_thread_ = webrtc::Thread::Create();
    signaling_thread_ = webrtc::Thread::Create();

    const bool network_started = network_thread_ && network_thread_->Start();
    const bool worker_started = network_started && worker_thread_ && worker_thread_->Start();
    const bool signaling_started = worker_started && signaling_thread_ && signaling_thread_->Start();
    if (!signaling_started) {
        EmitRtcLifecycle(diagnostic::RtcStatus::ThreadsFailed);
        if (worker_started) worker_thread_->Stop();
        if (network_started) network_thread_->Stop();
        signaling_thread_.reset();
        worker_thread_.reset();
        network_thread_.reset();
        webrtc::CleanupSSL();
        ssl_initialized_ = false;
        return false;
    }

    auto audio_encoder_factory = webrtc::CreateBuiltinAudioEncoderFactory();
    auto audio_decoder_factory = webrtc::CreateBuiltinAudioDecoderFactory();
    auto video_encoder_factory = CreateVideoEncoderFactory();
    auto video_decoder_factory = CreateVideoDecoderFactory();

    auto env = webrtc::CreateEnvironment();
    // Windows ADM owns a ScopedCOMInitializer. Construct it on the same
    // worker that runs Init/Terminate and releases the final ADM reference.
    // Creating it on a transient caller thread unbalances COM at teardown.
    worker_thread_->BlockingCall([this, &env]() {
        auto raw_adm = webrtc::CreateAudioDeviceModule(
            env, webrtc::AudioDeviceModule::kPlatformDefaultAudio);
        if (raw_adm) {
            adm_ = webrtc::make_ref_counted<PlayoutOnlyAudioDeviceModule>(raw_adm);
            if (adm_) {
                const bool ready = adm_->Init() == 0;
#if defined(_WIN32)
                // Match the Windows default playback role used by settings.
                const bool playout_selected = ready &&
                    adm_->SetPlayoutDevice(webrtc::AudioDeviceModule::kDefaultDevice) == 0;
#else
                // Linux ADM has no WindowsDeviceType overload. Index zero is
                // the PulseAudio/ALSA system default for each direction.
                const bool playout_selected = ready &&
                    adm_->SetPlayoutDevice(uint16_t{0}) == 0;
                const bool recording_selected = ready &&
                    adm_->SetRecordingDevice(uint16_t{0}) == 0;
                recording_device_index_ = 0;
                recording_device_identity_ = recording_selected
                    ? AudioDeviceIdentityAtIndex(*adm_, true, 0) : std::nullopt;
                recording_device_selected_ = recording_device_identity_.has_value();
                playout_device_identity_ = playout_selected
                    ? AudioDeviceIdentityAtIndex(*adm_, false, 0) : std::nullopt;
#endif
                if (!playout_selected) {
                    EmitRtcLifecycle(diagnostic::RtcStatus::PlayoutDeviceFailed);
                }
#if defined(_WIN32)
                playout_device_selected_ = playout_selected;
#else
                playout_device_selected_ = playout_device_identity_.has_value();
#endif
                playout_device_index_ = 0;
            }
        }
    });

    factory_ = webrtc::CreatePeerConnectionFactory(
        network_thread_.get(),
        worker_thread_.get(),
        signaling_thread_.get(),
        adm_,
        audio_encoder_factory,
        audio_decoder_factory,
        std::move(video_encoder_factory),
        std::move(video_decoder_factory),
        webrtc::scoped_refptr<webrtc::AudioMixer>(), 
        webrtc::scoped_refptr<webrtc::AudioProcessing>()  
    );

    if (!factory_) {
        EmitRtcLifecycle(diagnostic::RtcStatus::FactoryFailed);
        DeinitializeLocked();
        return false;
    }

    // This SDK initializes VoiceEngine and registers its AudioTransport when
    // the first PeerConnection is created, not when the factory is created.
    // Keep output closed until the native receive path or EnsurePlayout starts
    // it after that registration; otherwise Windows can play unfilled PCM.

    initialized_ = true;
    EmitRtcLifecycle(diagnostic::RtcStatus::Ready);
    return true;
}

#if !defined(_WIN32)
bool WebRTCManager::RefreshDefaultPlayoutOnWorker() {
    if (!playout_device_is_default_) return playout_device_selected_;
    const auto identity = AudioDeviceIdentityAtIndex(*adm_, false, 0);
    if (playout_device_selected_ && identity &&
        identity == playout_device_identity_) return true;

    // Force a real switch when index zero now names a different default.
    const auto previous = playout_device_selected_
        ? std::optional<uint16_t>(playout_device_index_) : std::nullopt;
    const auto result = detail::ApplyPlayoutDeviceWithRollback(
        *adm_, uint16_t{0}, previous, [this] { ResetApmProcessor(); },
        [](uint16_t) { return false; });
    if (!result.applied || !identity ||
        AudioDeviceIdentityAtIndex(*adm_, false, 0) != identity) {
        adm_->StopPlayout();
        playout_device_selected_ = false;
        playout_device_identity_.reset();
        return false;
    }
    playout_device_index_ = 0;
    playout_device_identity_ = identity;
    playout_device_selected_ = true;
    return true;
}

bool WebRTCManager::RefreshDefaultRecordingOnWorker() {
    if (!recording_device_is_default_) return recording_device_selected_;
    const auto identity = AudioDeviceIdentityAtIndex(*adm_, true, 0);
    if (recording_device_selected_ && identity &&
        identity == recording_device_identity_) return true;

    auto* native = static_cast<PlayoutOnlyAudioDeviceModule*>(adm_.get());
    const bool was_recording = recording_device_selected_ && adm_->Recording();
    const auto fail = [this, native]() {
        native->RevokeNativeRecording();
        adm_->StopRecording();
        recording_device_selected_ = false;
        recording_device_identity_.reset();
        return false;
    };
    native->RevokeNativeRecording();
    if ((adm_->Recording() || adm_->RecordingIsInitialized()) &&
        (adm_->StopRecording() != 0 || adm_->Recording())) return fail();
    if (!identity || adm_->SetRecordingDevice(uint16_t{0}) != 0 ||
        AudioDeviceIdentityAtIndex(*adm_, true, 0) != identity) return fail();
    recording_device_index_ = 0;
    recording_device_identity_ = identity;
    recording_device_selected_ = true;
    if (was_recording && !native->StartNativeRecording()) return fail();
    if (AudioDeviceIdentityAtIndex(*adm_, true, 0) != identity) return fail();
    ResetApmProcessor();
    return true;
}
#endif

bool WebRTCManager::SetPlayoutDevice(uint16_t index) {
#if !defined(_WIN32)
    std::optional<std::string> device_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!adm_ || !worker_thread_) return false;
        device_id = worker_thread_->BlockingCall([this, index]() -> std::optional<std::string> {
            for (const auto& device : EnumerateAudioDevices(*adm_, false)) {
                if (device.index == index) return device.info.id;
            }
            return std::nullopt;
        });
    }
    return device_id && SwitchPlayoutDeviceById(*device_id).applied;
#else
    std::lock_guard<std::mutex> lock(mutex_);
    if (!adm_ || !worker_thread_) return false;
    return worker_thread_->BlockingCall([this, index]() {
        return detail::ApplyPlayoutDevice(*adm_, index, [this] { ResetApmProcessor(); });
    });
#endif
}

bool WebRTCManager::SetPlayoutDeviceById(const std::string& device_id) {
    return SwitchPlayoutDeviceById(device_id).applied;
}

PlayoutSwitchResult WebRTCManager::SwitchPlayoutDeviceById(
        const std::string& device_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!adm_ || !worker_thread_) return {};
    // BlockingCall completes before device_id goes out of scope. Keep its
    // closure small and trivially copyable across the packaged WebRTC ABI.
    return worker_thread_->BlockingCall([this, &device_id]() {
#if defined(_WIN32)
        const bool applied = detail::SelectPlayoutDeviceById(
            *adm_, device_id, [this] { ResetApmProcessor(); });
        return PlayoutSwitchResult{
            applied, applied ? PlayoutSelection::Requested : PlayoutSelection::Unknown,
            adm_->Playing(), adm_->PlayoutIsInitialized()};
#else
        if (playout_device_is_default_ &&
            (playout_device_selected_ || device_id.empty()) &&
            !RefreshDefaultPlayoutOnWorker()) {
            if (device_id.empty() || adm_->Playing()) {
                return PlayoutSwitchResult{false, PlayoutSelection::Unknown,
                    adm_->Playing(), adm_->PlayoutIsInitialized(),
                    PlayoutFailureStage::Rollback};
            }
        }
        const auto previous_current = [this]() {
            return playout_device_selected_ && playout_device_identity_ &&
                AudioDeviceIdentityAtIndex(*adm_, false, playout_device_index_) ==
                    playout_device_identity_;
        };
        const auto forget_previous = [this]() {
            playout_device_selected_ = false;
            playout_device_identity_.reset();
        };
        if (playout_device_selected_ && !previous_current()) forget_previous();
        const auto unresolved = [this, &previous_current]() {
            if (!playout_device_selected_ &&
                (adm_->Playing() || adm_->PlayoutIsInitialized()) &&
                (adm_->StopPlayout() != 0 || adm_->Playing())) {
                return PlayoutSwitchResult{false, PlayoutSelection::Unknown,
                    adm_->Playing(), adm_->PlayoutIsInitialized(), PlayoutFailureStage::Stop};
            }
            return PlayoutSwitchResult{false, previous_current()
                ? PlayoutSelection::Previous : PlayoutSelection::Unknown,
                adm_->Playing(), adm_->PlayoutIsInitialized(), PlayoutFailureStage::Resolve};
        };
        const auto index = FindAudioDeviceIndex(*adm_, false, device_id);
        if (!index) return unresolved();
        const auto requested_identity = AudioDeviceIdentityAtIndex(*adm_, false, *index);
        if (!requested_identity ||
            (!device_id.empty() && *requested_identity != device_id)) {
            return unresolved();
        }
        const auto previous = playout_device_selected_
            ? std::optional<uint16_t>(playout_device_index_) : std::nullopt;
        auto result = detail::ApplyPlayoutDeviceWithRollback(
            *adm_, *index, previous, [this] { ResetApmProcessor(); },
            [this](uint16_t old_index) {
                return playout_device_identity_ &&
                    AudioDeviceIdentityAtIndex(*adm_, false, old_index) ==
                        playout_device_identity_;
            });
        if (result.selection == PlayoutSelection::Requested) {
            if (AudioDeviceIdentityAtIndex(*adm_, false, *index) == requested_identity) {
                playout_device_index_ = *index;
                playout_device_identity_ = requested_identity;
                playout_device_selected_ = true;
                playout_device_is_default_ = device_id.empty();
            } else {
                adm_->StopPlayout();
                forget_previous();
                result = {false, PlayoutSelection::Unknown, adm_->Playing(),
                          adm_->PlayoutIsInitialized(), PlayoutFailureStage::Rollback};
            }
        } else if (result.selection == PlayoutSelection::Previous && !previous_current()) {
            adm_->StopPlayout();
            forget_previous();
            result = {false, PlayoutSelection::Unknown, adm_->Playing(),
                      adm_->PlayoutIsInitialized(), PlayoutFailureStage::Rollback};
        } else if (result.selection == PlayoutSelection::Unknown) {
            adm_->StopPlayout();
            forget_previous();
            result.playing = adm_->Playing();
            result.initialized = adm_->PlayoutIsInitialized();
        }
        return result;
#endif
    });
}

std::vector<AudioDeviceInfo> WebRTCManager::ListRecordingDevices() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!adm_ || !worker_thread_) return {};
    return worker_thread_->BlockingCall([this]() {
        std::vector<AudioDeviceInfo> result;
        for (auto& entry : EnumerateAudioDevices(*adm_, true)) {
            result.push_back(std::move(entry.info));
        }
        return result;
    });
}

std::vector<AudioDeviceInfo> WebRTCManager::ListPlayoutDevices() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!adm_ || !worker_thread_) return {};
    return worker_thread_->BlockingCall([this]() {
        std::vector<AudioDeviceInfo> result;
        for (auto& entry : EnumerateAudioDevices(*adm_, false)) {
            result.push_back(std::move(entry.info));
        }
        return result;
    });
}

bool WebRTCManager::SetRecordingDeviceById(const std::string& device_id) {
#if defined(_WIN32)
    (void)device_id;
    return false;
#else
    std::lock_guard<std::mutex> lock(mutex_);
    if (!adm_ || !worker_thread_) return false;
    return worker_thread_->BlockingCall([this, &device_id]() {
        if (recording_device_is_default_ &&
            (recording_device_selected_ || device_id.empty()) &&
            !RefreshDefaultRecordingOnWorker() &&
            (device_id.empty() || adm_->Recording())) return false;
        auto* native = static_cast<PlayoutOnlyAudioDeviceModule*>(adm_.get());
        const auto previous_current = [this]() {
            return recording_device_selected_ && recording_device_identity_ &&
                AudioDeviceIdentityAtIndex(*adm_, true, recording_device_index_) ==
                    recording_device_identity_;
        };
        const auto fail_closed = [this, native]() {
            native->RevokeNativeRecording();
            const bool stopped = (!adm_->Recording() && !adm_->RecordingIsInitialized()) ||
                (adm_->StopRecording() == 0 && !adm_->Recording());
            recording_device_selected_ = false;
            recording_device_identity_.reset();
            return stopped;
        };
        const auto selected = FindAudioDeviceIndex(*adm_, true, device_id);
        const bool restart_after_stale = recording_device_selected_ &&
            !previous_current() && adm_->Recording();
        if (recording_device_selected_ && !previous_current() && !fail_closed()) {
            return false;
        }
        if (!recording_device_selected_ && adm_->Recording() && !fail_closed()) {
            return false;
        }
        if (!selected) return false;
        const auto requested_identity = AudioDeviceIdentityAtIndex(*adm_, true, *selected);
        if (!requested_identity ||
            (!device_id.empty() && *requested_identity != device_id)) {
            return false;
        }

        const bool was_initialized = adm_->RecordingIsInitialized();
        const bool was_recording = adm_->Recording();
        const auto previous_index = recording_device_index_;
        const bool had_selection = recording_device_selected_;
        if (had_selection && *selected == previous_index &&
            requested_identity == recording_device_identity_) return true;
        if ((was_initialized || was_recording) && adm_->StopRecording() != 0) {
            if (!previous_current()) fail_closed();
            return false;
        }

        const auto restore_previous = [&]() {
            if (!had_selection || !previous_current() ||
                adm_->SetRecordingDevice(previous_index) != 0) {
                fail_closed();
                return;
            }
            if (was_initialized || was_recording) {
                if (adm_->InitRecording() != 0 ||
                    (was_recording &&
                     (adm_->StartRecording() != 0 || !adm_->Recording()))) {
                    fail_closed();
                    return;
                }
            }
            if (!previous_current()) fail_closed();
        };

        if (adm_->SetRecordingDevice(*selected) != 0) {
            restore_previous();
            return false;
        }
        if ((was_initialized || was_recording) &&
            (adm_->InitRecording() != 0 ||
             (was_recording && (adm_->StartRecording() != 0 || !adm_->Recording())))) {
            adm_->StopRecording();
            restore_previous();
            return false;
        }
        if (AudioDeviceIdentityAtIndex(*adm_, true, *selected) != requested_identity) {
            fail_closed();
            return false;
        }
        recording_device_index_ = *selected;
        recording_device_identity_ = requested_identity;
        recording_device_selected_ = true;
        recording_device_is_default_ = device_id.empty();
        if (restart_after_stale &&
            (!native->StartNativeRecording() ||
             AudioDeviceIdentityAtIndex(*adm_, true, *selected) != requested_identity)) {
            fail_closed();
            return false;
        }
        ResetApmProcessor();
        return true;
    });
#endif
}

bool WebRTCManager::EnsureRecording() {
#if defined(_WIN32)
    return false;
#else
    std::lock_guard<std::mutex> lock(mutex_);
    if (!adm_ || !worker_thread_) return false;
    return worker_thread_->BlockingCall([this]() {
        auto* native = static_cast<PlayoutOnlyAudioDeviceModule*>(adm_.get());
        if (recording_device_is_default_ && !RefreshDefaultRecordingOnWorker()) {
            return false;
        }
        if (!recording_device_is_default_ && recording_device_selected_ &&
            (!recording_device_identity_ ||
             AudioDeviceIdentityAtIndex(*adm_, true, recording_device_index_) !=
                 recording_device_identity_)) {
            native->RevokeNativeRecording();
            adm_->StopRecording();
            recording_device_selected_ = false;
            recording_device_identity_.reset();
            return false;
        }
        if (!recording_device_selected_ && adm_->Recording()) {
            native->RevokeNativeRecording();
            adm_->StopRecording();
            return false;
        }
        if (!recording_device_selected_) return false;
        return native->StartNativeRecording();
    });
#endif
}

bool WebRTCManager::RecordingActive() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!adm_ || !worker_thread_) return false;
    return worker_thread_->BlockingCall([this]() {
#if !defined(_WIN32)
        if (recording_device_is_default_ && recording_device_selected_ &&
            !RefreshDefaultRecordingOnWorker()) return false;
        if (!recording_device_is_default_ && recording_device_selected_ &&
            (!recording_device_identity_ ||
             AudioDeviceIdentityAtIndex(*adm_, true, recording_device_index_) !=
                 recording_device_identity_)) {
            static_cast<PlayoutOnlyAudioDeviceModule*>(adm_.get())
                ->RevokeNativeRecording();
            adm_->StopRecording();
            recording_device_selected_ = false;
            recording_device_identity_.reset();
            return false;
        }
        if (!recording_device_selected_) {
            if (adm_->Recording()) {
                static_cast<PlayoutOnlyAudioDeviceModule*>(adm_.get())
                    ->RevokeNativeRecording();
                adm_->StopRecording();
            }
            return false;
        }
#endif
        return adm_->Recording();
    });
}

bool WebRTCManager::StopRecording() {
#if defined(_WIN32)
    return true;
#else
    std::lock_guard<std::mutex> lock(mutex_);
    if (!adm_) return true;
    if (!worker_thread_) return false;
    return worker_thread_->BlockingCall([this]() {
        return static_cast<PlayoutOnlyAudioDeviceModule*>(adm_.get())
            ->StopNativeRecording();
    });
#endif
}

bool WebRTCManager::EnsurePlayout() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!adm_ || !worker_thread_) return false;
    return worker_thread_->BlockingCall([this]() {
#if !defined(_WIN32)
        if (playout_device_is_default_ && !RefreshDefaultPlayoutOnWorker()) {
            return false;
        }
        if (!playout_device_is_default_ && playout_device_selected_ &&
            (!playout_device_identity_ ||
             AudioDeviceIdentityAtIndex(*adm_, false, playout_device_index_) !=
                 playout_device_identity_)) {
            adm_->StopPlayout();
            playout_device_selected_ = false;
            playout_device_identity_.reset();
            return false;
        }
        if (!playout_device_selected_) {
            if (adm_->Playing()) adm_->StopPlayout();
            return false;
        }
#endif
        // The ADM wrapper tracks AudioTransport registration on both platforms.
        // Keep output closed until a PeerConnection binds that callback.
        const auto* playout = static_cast<PlayoutOnlyAudioDeviceModule*>(adm_.get());
        if (!playout->HasAudioCallback()) return false;
        if (adm_->Playing()) return true;
        if (!adm_->PlayoutIsInitialized()) {
#if defined(_WIN32)
            if (adm_->InitSpeaker() != 0) return false;
#endif
            if (adm_->InitPlayout() != 0) return false;
        }
        return adm_->StartPlayout() == 0 && adm_->Playing();
    });
}

void WebRTCManager::Deinitialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    DeinitializeLocked();
}

void WebRTCManager::DeinitializeLocked() {
    if (!ssl_initialized_ && !initialized_) return;

    // Stop native callbacks before destroying the VoiceEngine/AudioTransport.
    if (adm_) {
        if (worker_thread_) {
            worker_thread_->BlockingCall([this]() {
#if !defined(_WIN32)
                static_cast<PlayoutOnlyAudioDeviceModule*>(adm_.get())
                    ->RevokeNativeRecording();
#endif
                if (adm_->Playing()) {
                    adm_->StopPlayout();
                }
                if (adm_->Recording()) {
                    adm_->StopRecording();
                }
            });
        } else {
#if !defined(_WIN32)
            static_cast<PlayoutOnlyAudioDeviceModule*>(adm_.get())
                ->RevokeNativeRecording();
#endif
            if (adm_->Playing()) {
                adm_->StopPlayout();
            }
            if (adm_->Recording()) {
                adm_->StopRecording();
            }
        }
    }

    factory_ = nullptr;

    if (adm_) {
        if (worker_thread_) {
            worker_thread_->BlockingCall([this]() {
                adm_->RegisterAudioCallback(nullptr);
                static_cast<PlayoutOnlyAudioDeviceModule*>(adm_.get())
                    ->ShutdownNative();
                adm_ = nullptr;
            });
        } else {
            adm_->RegisterAudioCallback(nullptr);
            static_cast<PlayoutOnlyAudioDeviceModule*>(adm_.get())
                ->ShutdownNative();
            adm_ = nullptr;
        }
    }

    if (worker_thread_) {
        worker_thread_->BlockingCall([]() {});
    }
    if (signaling_thread_) {
        signaling_thread_->BlockingCall([]() {});
    }
    if (network_thread_) {
        network_thread_->BlockingCall([]() {});
    }

    if (signaling_thread_) {
        signaling_thread_->Stop();
        signaling_thread_.reset();
    }
    if (worker_thread_) {
        worker_thread_->Stop();
        worker_thread_.reset();
    }
    if (network_thread_) {
        network_thread_->Stop();
        network_thread_.reset();
    }

    if (ssl_initialized_) webrtc::CleanupSSL();
    ssl_initialized_ = false;
    initialized_ = false;
    playout_device_selected_ = false;
    playout_device_index_ = 0;
    recording_device_selected_ = false;
    recording_device_index_ = 0;
#if !defined(_WIN32)
    playout_device_identity_.reset();
    recording_device_identity_.reset();
    playout_device_is_default_ = true;
    recording_device_is_default_ = true;
#endif
    EmitRtcLifecycle(diagnostic::RtcStatus::Stopped);
}

using CreateSdpCompletion = CancellableExecutorCallback<std::string, std::string>;
using SetSdpCompletion = CancellableExecutorCallback<std::string>;

// All state reads are on the WebRTC signaling thread, at the actual call/callback.
diagnostic::SdpState SdpStateOf(webrtc::PeerConnectionInterface* pc) {
    using P = webrtc::PeerConnectionInterface;
    using S = diagnostic::SdpState;
    switch (pc->signaling_state()) {
    case P::kStable: return S::Stable;
    case P::kHaveLocalOffer: return S::HaveLocalOffer;
    case P::kHaveLocalPrAnswer: return S::HaveLocalPranswer;
    case P::kHaveRemoteOffer: return S::HaveRemoteOffer;
    case P::kHaveRemotePrAnswer: return S::HaveRemotePranswer;
    case P::kClosed: return S::Closed;
    }
    return S::Unknown;
}

class RtcSdpOperation final {
public:
    RtcSdpOperation(webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,
                    std::shared_ptr<diagnostic::SdpNegotiationTrace> trace,
                    diagnostic::SdpAction action,
                    diagnostic::SdpDescription description = diagnostic::SdpDescription::Unknown)
        : pc_(std::move(pc)), trace_(std::move(trace)), action_(action), description_(description), before_(SdpStateOf(pc_.get())) {
        if (trace_) trace_->Record(action_, diagnostic::SdpPhase::Started,
            before_, before_, diagnostic::SdpReason::None, 0, diagnostic::ThreadRole::Rtc, description_);
    }
    void Complete(diagnostic::SdpPhase phase, diagnostic::SdpReason reason = diagnostic::SdpReason::None,
                  int error = 0) {
        if (!trace_) return;
        trace_->Record(action_, phase, before_, SdpStateOf(pc_.get()), reason, error,
            diagnostic::ThreadRole::Rtc, description_);
        if (phase == diagnostic::SdpPhase::Failed)
            trace_->Finish(diagnostic::Outcome::Failure, reason);
        if (phase == diagnostic::SdpPhase::Rejected)
            trace_->Finish(diagnostic::Outcome::Cancelled, reason);
    }
    bool traced() const { return bool(trace_); }
private:
    const webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc_;
    const std::shared_ptr<diagnostic::SdpNegotiationTrace> trace_;
    const diagnostic::SdpAction action_;
    const diagnostic::SdpDescription description_;
    const diagnostic::SdpState before_;
};

class CreateSdpObserverProxy : public webrtc::CreateSessionDescriptionObserver {
public:
    static webrtc::scoped_refptr<CreateSdpObserverProxy> Create(
        std::shared_ptr<CreateSdpCompletion> completion,
        diagnostic::Stage stage, std::shared_ptr<RtcSdpOperation> operation) {
        return webrtc::make_ref_counted<CreateSdpObserverProxy>(
            std::move(completion), stage, std::move(operation));
    }

    CreateSdpObserverProxy(
        std::shared_ptr<CreateSdpCompletion> completion,
        diagnostic::Stage stage, std::shared_ptr<RtcSdpOperation> operation)
        : completion_(std::move(completion)), stage_(stage), operation_(std::move(operation)) {}

    void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
        const bool pending = completion_->pending();
        operation_->Complete(pending ? diagnostic::SdpPhase::Completed : diagnostic::SdpPhase::Rejected,
            pending ? diagnostic::SdpReason::None : diagnostic::SdpReason::GenerationExpired);
        std::string sdp;
        desc->ToString(&sdp);
        delete desc;
        completion_->Complete(std::move(sdp), "");
    }

    void OnFailure(webrtc::RTCError error) override {
        const bool pending = completion_->pending();
        operation_->Complete(pending ? diagnostic::SdpPhase::Failed : diagnostic::SdpPhase::Rejected,
            pending ? diagnostic::SdpReason::RtcError : diagnostic::SdpReason::GenerationExpired,
            static_cast<int>(error.type()));
        diagnostic::Event event;
        event.kind = diagnostic::EventKind::RtcSdpFailed;
        event.thread_role = diagnostic::ThreadRole::Rtc;
        event.stage = stage_;
        event.error_layer = diagnostic::ErrorLayer::Rtc;
        event.rtc_error_type = static_cast<int>(error.type());
        if (!operation_->traced()) diagnostic::EmitBusinessEvent(event);
        std::string err_msg = error.message();
        completion_->Complete("", std::move(err_msg));
    }

private:
    std::shared_ptr<CreateSdpCompletion> completion_;
    diagnostic::Stage stage_;
    std::shared_ptr<RtcSdpOperation> operation_;
};

class SetSdpObserverProxy : public webrtc::SetSessionDescriptionObserver {
public:
    static webrtc::scoped_refptr<SetSdpObserverProxy> Create(
        std::shared_ptr<SetSdpCompletion> completion,
        diagnostic::Stage stage, std::shared_ptr<RtcSdpOperation> operation) {
        return webrtc::make_ref_counted<SetSdpObserverProxy>(
            std::move(completion), stage, std::move(operation));
    }

    SetSdpObserverProxy(
        std::shared_ptr<SetSdpCompletion> completion,
        diagnostic::Stage stage, std::shared_ptr<RtcSdpOperation> operation)
        : completion_(std::move(completion)), stage_(stage), operation_(std::move(operation)) {}

    void OnSuccess() override {
        const bool pending = completion_->pending();
        operation_->Complete(pending ? diagnostic::SdpPhase::Completed : diagnostic::SdpPhase::Rejected,
            pending ? diagnostic::SdpReason::None : diagnostic::SdpReason::GenerationExpired);
        completion_->Complete("");
    }

    void OnFailure(webrtc::RTCError error) override {
        const bool pending = completion_->pending();
        operation_->Complete(pending ? diagnostic::SdpPhase::Failed : diagnostic::SdpPhase::Rejected,
            pending ? diagnostic::SdpReason::RtcError : diagnostic::SdpReason::GenerationExpired,
            static_cast<int>(error.type()));
        diagnostic::Event event;
        event.kind = diagnostic::EventKind::RtcSdpFailed;
        event.thread_role = diagnostic::ThreadRole::Rtc;
        event.stage = stage_;
        event.error_layer = diagnostic::ErrorLayer::Rtc;
        event.rtc_error_type = static_cast<int>(error.type());
        if (!operation_->traced()) diagnostic::EmitBusinessEvent(event);
        std::string err_msg = error.message();
        completion_->Complete(std::move(err_msg));
    }

private:
    std::shared_ptr<SetSdpCompletion> completion_;
    diagnostic::Stage stage_;
    std::shared_ptr<RtcSdpOperation> operation_;
};

void WebRTCManager::CreateOffer(
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,
    asio::any_io_executor executor,
    std::function<void(const std::string& sdp, const std::string& error)> callback,
    bool ice_restart,
    std::shared_ptr<ExecutorCallbackGate> callback_gate,
    std::shared_ptr<diagnostic::SdpNegotiationTrace> trace) {
    if (!callback_gate) callback_gate = std::make_shared<ExecutorCallbackGate>(executor);
    auto completion = CreateSdpCompletion::Create(
        std::move(callback_gate), std::move(callback), "", "operation cancelled");
    
    struct TaskParams {
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
        std::shared_ptr<CreateSdpCompletion> completion;
        std::shared_ptr<diagnostic::SdpNegotiationTrace> trace;
        bool ice_restart;
    };
    auto* p = new TaskParams{pc, std::move(completion), std::move(trace), ice_restart};
    // Keep the cross-library task capture pointer-only, as in the existing ABI boundary.
    signaling_thread_->PostTask([p]() {
        std::unique_ptr<TaskParams> owned(p);
        const auto& trace = p->trace;
        if (!p->completion->pending()) {
            if (trace) {
                trace->Record(diagnostic::SdpAction::CreateOffer, diagnostic::SdpPhase::Rejected,
                    diagnostic::SdpState::Unknown, diagnostic::SdpState::Unknown,
                    diagnostic::SdpReason::GenerationExpired, 0, diagnostic::ThreadRole::Rtc);
                trace->Finish(diagnostic::Outcome::Cancelled, diagnostic::SdpReason::GenerationExpired);
            }
            return;
        }
        auto operation = std::make_shared<RtcSdpOperation>(p->pc, trace, diagnostic::SdpAction::CreateOffer);
        auto observer = CreateSdpObserverProxy::Create(
            p->completion, diagnostic::Stage::CreateOffer, operation);
        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
        options.ice_restart = p->ice_restart;
        p->pc->CreateOffer(observer.get(), options);
    });
}

void WebRTCManager::CreateAnswer(
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,
    asio::any_io_executor executor,
    std::function<void(const std::string& sdp, const std::string& error)> callback,
    std::shared_ptr<ExecutorCallbackGate> callback_gate,
    std::shared_ptr<diagnostic::SdpNegotiationTrace> trace) {
    if (!callback_gate) callback_gate = std::make_shared<ExecutorCallbackGate>(executor);
    auto completion = CreateSdpCompletion::Create(
        std::move(callback_gate), std::move(callback), "", "operation cancelled");
    
    struct TaskParams {
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
        std::shared_ptr<CreateSdpCompletion> completion;
        std::shared_ptr<diagnostic::SdpNegotiationTrace> trace;
    };
    auto* p = new TaskParams{pc, std::move(completion), std::move(trace)};
    // Keep the cross-library task capture pointer-only, as in the existing ABI boundary.
    signaling_thread_->PostTask([p]() {
        std::unique_ptr<TaskParams> owned(p);
        const auto& trace = p->trace;
        if (!p->completion->pending()) {
            if (trace) {
                trace->Record(diagnostic::SdpAction::CreateAnswer, diagnostic::SdpPhase::Rejected,
                    diagnostic::SdpState::Unknown, diagnostic::SdpState::Unknown,
                    diagnostic::SdpReason::GenerationExpired, 0, diagnostic::ThreadRole::Rtc);
                trace->Finish(diagnostic::Outcome::Cancelled, diagnostic::SdpReason::GenerationExpired);
            }
            return;
        }
        auto operation = std::make_shared<RtcSdpOperation>(p->pc, trace, diagnostic::SdpAction::CreateAnswer);
        auto observer = CreateSdpObserverProxy::Create(
            p->completion, diagnostic::Stage::CreateAnswer, operation);
        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
        p->pc->CreateAnswer(observer.get(), options);
    });
}

void WebRTCManager::SetRemoteDescription(
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,
    const std::string& type,
    const std::string& sdp,
    asio::any_io_executor executor,
    std::function<void(const std::string& error)> callback,
    std::shared_ptr<ExecutorCallbackGate> callback_gate,
    std::shared_ptr<diagnostic::SdpNegotiationTrace> trace) {
    if (!callback_gate) callback_gate = std::make_shared<ExecutorCallbackGate>(executor);
    auto completion = SetSdpCompletion::Create(
        std::move(callback_gate), std::move(callback), "operation cancelled");
    
    struct TaskParams {
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
        std::string type;
        std::string sdp;
        std::shared_ptr<SetSdpCompletion> completion;
        std::shared_ptr<diagnostic::SdpNegotiationTrace> trace;
    };
    auto* p = new TaskParams{pc, type, sdp, std::move(completion), std::move(trace)};
    // Keep the cross-library task capture pointer-only, as in the existing ABI boundary.
    signaling_thread_->PostTask([p]() {
        std::unique_ptr<TaskParams> owned(p);
        const auto& trace = p->trace;
        if (!p->completion->pending()) {
            if (trace) {
                trace->Record(diagnostic::SdpAction::SetRemote, diagnostic::SdpPhase::Rejected,
                    diagnostic::SdpState::Unknown, diagnostic::SdpState::Unknown,
                    diagnostic::SdpReason::GenerationExpired, 0, diagnostic::ThreadRole::Rtc);
                trace->Finish(diagnostic::Outcome::Cancelled, diagnostic::SdpReason::GenerationExpired);
            }
            return;
        }
        auto operation = std::make_shared<RtcSdpOperation>(p->pc, trace, diagnostic::SdpAction::SetRemote,
            p->type == "answer" ? diagnostic::SdpDescription::Answer : diagnostic::SdpDescription::Offer);
        webrtc::SdpParseError err;
        webrtc::SdpType sdp_type = (p->type == "answer") ? webrtc::SdpType::kAnswer : webrtc::SdpType::kOffer;
        std::unique_ptr<webrtc::SessionDescriptionInterface> session_desc =
            webrtc::CreateSessionDescription(sdp_type, p->sdp, &err);
        
        if (!session_desc) {
            operation->Complete(diagnostic::SdpPhase::Failed, diagnostic::SdpReason::ParseError);
            std::string err_msg = err.description;
            p->completion->Complete(std::move(err_msg));
            return;
        }

        auto observer = SetSdpObserverProxy::Create(
            p->completion, diagnostic::Stage::SetRemoteDescription, operation);
        p->pc->SetRemoteDescription(observer.get(), session_desc.release());
    });
}

void WebRTCManager::SetLocalDescription(
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,
    const std::string& type,
    const std::string& sdp,
    asio::any_io_executor executor,
    std::function<void(const std::string& error)> callback,
    std::shared_ptr<ExecutorCallbackGate> callback_gate,
    std::shared_ptr<diagnostic::SdpNegotiationTrace> trace) {
    if (!callback_gate) callback_gate = std::make_shared<ExecutorCallbackGate>(executor);
    auto completion = SetSdpCompletion::Create(
        std::move(callback_gate), std::move(callback), "operation cancelled");
    
    struct TaskParams {
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
        std::string type;
        std::string sdp;
        std::shared_ptr<SetSdpCompletion> completion;
        std::shared_ptr<diagnostic::SdpNegotiationTrace> trace;
    };
    auto* p = new TaskParams{pc, type, sdp, std::move(completion), std::move(trace)};
    // Keep the cross-library task capture pointer-only, as in the existing ABI boundary.
    signaling_thread_->PostTask([p]() {
        std::unique_ptr<TaskParams> owned(p);
        const auto& trace = p->trace;
        if (!p->completion->pending()) {
            if (trace) {
                trace->Record(diagnostic::SdpAction::SetLocal, diagnostic::SdpPhase::Rejected,
                    diagnostic::SdpState::Unknown, diagnostic::SdpState::Unknown,
                    diagnostic::SdpReason::GenerationExpired, 0, diagnostic::ThreadRole::Rtc);
                trace->Finish(diagnostic::Outcome::Cancelled, diagnostic::SdpReason::GenerationExpired);
            }
            return;
        }
        auto operation = std::make_shared<RtcSdpOperation>(p->pc, trace, diagnostic::SdpAction::SetLocal,
            p->type == "answer" ? diagnostic::SdpDescription::Answer : diagnostic::SdpDescription::Offer);
        webrtc::SdpParseError err;
        webrtc::SdpType sdp_type = (p->type == "answer") ? webrtc::SdpType::kAnswer : webrtc::SdpType::kOffer;
        std::unique_ptr<webrtc::SessionDescriptionInterface> session_desc =
            webrtc::CreateSessionDescription(sdp_type, p->sdp, &err);
        
        if (!session_desc) {
            operation->Complete(diagnostic::SdpPhase::Failed, diagnostic::SdpReason::ParseError);
            std::string err_msg = err.description;
            p->completion->Complete(std::move(err_msg));
            return;
        }

        auto observer = SetSdpObserverProxy::Create(
            p->completion, diagnostic::Stage::SetLocalDescription, operation);
        p->pc->SetLocalDescription(observer.get(), session_desc.release());
    });
}

void WebRTCManager::SetApmProcessor(std::shared_ptr<AudioApmProcessor> processor) {
    std::lock_guard<std::mutex> lock(apm_mutex_);
    apm_processor_ = std::move(processor);
}

std::shared_ptr<AudioApmProcessor> WebRTCManager::apm_processor() const {
    std::lock_guard<std::mutex> lock(apm_mutex_);
    return apm_processor_;
}

void WebRTCManager::ResetApmProcessor() {
    auto apm = apm_processor();
    if (apm) {
        apm->Reset();
    }
}

} // namespace livekit
