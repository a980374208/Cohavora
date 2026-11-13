#pragma once

#include "h264_level_constraints.h"
#include "video_format_diagnostics.h"
#include "telemetry/diagnostic_pipeline.h"
#include "api/video_codecs/video_encoder.h"
#include "modules/video_coding/include/video_error_codes.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace livekit::detail {

// Enclose the whole simulcast/fallback adapter. A same-codec software fallback
// must obey the negotiated level just as the hardware implementation does.
class FinalH264LevelGuard final : public webrtc::VideoEncoder {
public:
    FinalH264LevelGuard(std::unique_ptr<webrtc::VideoEncoder> inner,
                        webrtc::SdpVideoFormat format, bool single_encoder = false)
        : inner_(std::move(inner)), format_(std::move(format)), single_encoder_(single_encoder) {}

    int InitEncode(const webrtc::VideoCodec* codec, const Settings& settings) override {
        rejected_ = true;
        if (!codec || codec->numberOfSimulcastStreams > webrtc::kMaxSimulcastStreams)
            return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
        if (!Validate(*codec, 0, diagnostic::EncoderFailureStage::InitLevelConstraints)) {
            if (initialized_) ReleaseInner();
            return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
        }
        codec_ = *codec;
        const auto result = inner_->InitEncode(codec, settings);
        initialized_ = result >= 0;
        rejected_ = !initialized_;
        return result;
    }
    int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) override {
        return inner_->RegisterEncodeCompleteCallback(callback);
    }
    int32_t Release() override {
        initialized_ = false;
        rejected_ = false;
        return inner_->Release();
    }
    int32_t Encode(const webrtc::VideoFrame& frame,
                   const std::vector<webrtc::VideoFrameType>* types) override {
        return rejected_ ? WEBRTC_VIDEO_CODEC_ERR_PARAMETER : inner_->Encode(frame, types);
    }
    void SetRates(const RateControlParameters& rates) override {
        if (rejected_) return;
        // Nonpositive targets mean unavailable, according to the encoder API.
        // The adapter retains the initialized frame-rate limit in that case.
        if (initialized_ && !(rates.framerate_fps <= 0) &&
            !Validate(codec_, rates.framerate_fps,
                      diagnostic::EncoderFailureStage::RateLevelConstraints)) {
            rejected_ = true;
            return;
        }
        inner_->SetRates(rates);
    }
    void SetFecControllerOverride(webrtc::FecControllerOverride* controller) override {
        inner_->SetFecControllerOverride(controller);
    }
    void OnPacketLossRateUpdate(float loss) override { inner_->OnPacketLossRateUpdate(loss); }
    void OnRttUpdate(int64_t rtt) override { inner_->OnRttUpdate(rtt); }
    void OnLossNotification(const LossNotification& loss) override { inner_->OnLossNotification(loss); }
    EncoderInfo GetEncoderInfo() const override { return inner_->GetEncoderInfo(); }

private:
    bool Check(const webrtc::SdpVideoFormat& format, int width, int height,
               double fps, unsigned layer, std::uint32_t bitrate_kbps,
               diagnostic::EncoderFailureStage stage) const {
        const auto projected = ProjectH264Format(format.name, format.parameters);
        if (!projected) return true; // A mixed-codec simulcast layer has its own contract.
        const bool valid_fps = std::isfinite(fps) && fps > 0 &&
            fps <= std::numeric_limits<std::uint32_t>::max();
        const auto rounded_fps = valid_fps ? static_cast<std::uint32_t>(std::ceil(fps)) : 0;
        const auto status = projected->valid
            ? CheckH264LevelConstraints(static_cast<webrtc::H264Level>(projected->level),
                width, height, rounded_fps)
            : H264LevelConstraintStatus::InvalidParameters;
        if (status == H264LevelConstraintStatus::Supported) return true;
        diagnostic::Event event;
        event.kind = diagnostic::EventKind::VideoLevelRejected;
        event.thread_role = diagnostic::ThreadRole::Rtc;
        event.outcome = diagnostic::Outcome::Failure;
        event.hardware.encoder_codecs = diagnostic::HardwareEncoderH264;
        event.encoder.stage = stage;
        event.encoder_level = projected->level;
        event.encoder.width = static_cast<std::uint16_t>(std::clamp(width, 0, 65535));
        event.encoder.height = static_cast<std::uint16_t>(std::clamp(height, 0, 65535));
        event.encoder.framerate = static_cast<std::uint16_t>(std::min(rounded_fps, 65535u));
        event.encoder.layer = static_cast<std::uint8_t>(layer);
        event.encoder.bitrate_bps = static_cast<std::uint32_t>(std::min<std::uint64_t>(
            std::uint64_t(bitrate_kbps) * 1000, std::numeric_limits<std::uint32_t>::max()));
        event.encoder.status = static_cast<std::int32_t>(status);
        event.rtc_error_type = WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
        diagnostic::EmitBusinessEvent(event);
        return false;
    }
    bool Validate(const webrtc::VideoCodec& codec, double updated_fps,
                  diagnostic::EncoderFailureStage stage) const {
        if (!std::isfinite(updated_fps))
            return Check(format_, codec.width, codec.height, updated_fps, 0, codec.startBitrate, stage);
        // SEA passes the whole codec unchanged in its single-stream bypass.
        if (codec.numberOfSimulcastStreams <= 1) {
            return Check(format_, codec.width, codec.height,
                updated_fps == 0 ? codec.maxFramerate : updated_fps,
                0, codec.startBitrate, stage);
        }
        for (unsigned index = 0; index < codec.numberOfSimulcastStreams; ++index) {
            const auto& stream = codec.simulcastStream[index];
            // Validate inactive declarations too: subsequent allocation may
            // enable them, and a software encoder can support native simulcast.
            // A native simulcast encoder receives one factory format and the
            // global FPS for all layers. SEA's split path applies per-layer
            // formats/FPS instead. Check both boundaries in their own wrapper.
            const double fps = single_encoder_
                ? (updated_fps == 0 ? codec.maxFramerate : updated_fps)
                : (updated_fps == 0 ? stream.maxFramerate
                   : std::min(updated_fps, double(stream.maxFramerate)));
            if (!Check(single_encoder_ ? format_ : stream.format.value_or(format_), stream.width, stream.height,
                       fps, index, codec.startBitrate, stage)) return false;
        }
        return true;
    }
    void ReleaseInner() {
        inner_->Release();
        initialized_ = false;
    }
    std::unique_ptr<webrtc::VideoEncoder> inner_;
    const webrtc::SdpVideoFormat format_;
    const bool single_encoder_;
    webrtc::VideoCodec codec_{};
    bool initialized_ = false;
    bool rejected_ = false;
};

} // namespace livekit::detail
