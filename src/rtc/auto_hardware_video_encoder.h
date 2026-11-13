#pragma once

#include "api/video_codecs/video_encoder_factory.h"
#include "modules/video_coding/include/video_error_codes.h"

namespace livekit::detail {

// GetImplementations and EncoderSelector parameters are local implementation
// settings. WebRTC merges them after SDP negotiation, never into capabilities.
inline constexpr char kAutoHardwareH264Parameter[] = "cohavora-auto-hardware";

inline bool IsAutoHardwareH264(const webrtc::SdpVideoFormat& format) {
    const auto parameter = format.parameters.find(kAutoHardwareH264Parameter);
    return parameter != format.parameters.end() && parameter->second == "1";
}

class AutoHardwareH264EncoderSelector final
    : public webrtc::VideoEncoderFactory::EncoderSelectorInterface {
public:
    explicit AutoHardwareH264EncoderSelector(bool allow_vp8_fallback)
        : allow_vp8_fallback_(allow_vp8_fallback),
          current_("H264", {{"profile-level-id", "42001f"},
                            {"packetization-mode", "1"},
                            {"level-asymmetry-allowed", "1"}}) {}

    void OnCurrentEncoder(const webrtc::SdpVideoFormat& format) override {
        current_ = format;
        has_current_ = true;
        // Once a peer or a failure selects another codec, do not switch back.
        if (format.name != "H264" && format.name != "h264") finished_ = true;
    }

    std::optional<webrtc::SdpVideoFormat> OnAvailableBitrate(
        const webrtc::DataRate&) override {
        return SelectHardwareImplementation();
    }

    std::optional<webrtc::SdpVideoFormat> OnResolutionChange(
        const webrtc::RenderResolution&) override {
        return SelectHardwareImplementation();
    }

    std::optional<webrtc::SdpVideoFormat> OnEncoderBroken() override {
        if (finished_) return std::nullopt;
        finished_ = true;
        if (allow_vp8_fallback_) return webrtc::SdpVideoFormat("VP8");
        return std::nullopt;
    }

private:
    std::optional<webrtc::SdpVideoFormat> SelectHardwareImplementation() {
        // Wait for the actual negotiated profile/level, which may be lower
        // than the offer. Never replace it with the local capability defaults.
        if (!has_current_ || finished_ || switch_requested_ || IsAutoHardwareH264(current_)) {
            return std::nullopt;
        }
        switch_requested_ = true;
        auto format = current_;
        format.parameters[kAutoHardwareH264Parameter] = "1";
        return format;
    }

    bool allow_vp8_fallback_ = false;
    bool has_current_ = false;
    bool switch_requested_ = false;
    bool finished_ = false;
    webrtc::SdpVideoFormat current_;
};

// The backend's FALLBACK_SOFTWARE result normally asks for a same-codec
// wrapper. This automatic policy has negotiated VP8 instead: surface a codec
// failure so WebRTC invokes this sender's selector for creation/init/frame loss.
class CodecSwitchOnFailureEncoder final : public webrtc::VideoEncoder {
public:
    explicit CodecSwitchOnFailureEncoder(std::unique_ptr<webrtc::VideoEncoder> inner)
        : inner_(std::move(inner)) {}

    int InitEncode(const webrtc::VideoCodec* codec, const Settings& settings) override {
        return inner_->InitEncode(codec, settings);
    }
    int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) override {
        return inner_->RegisterEncodeCompleteCallback(callback);
    }
    int32_t Release() override { return inner_->Release(); }
    int32_t Encode(const webrtc::VideoFrame& frame,
                   const std::vector<webrtc::VideoFrameType>* types) override {
        const auto result = inner_->Encode(frame, types);
        return result < 0 ? WEBRTC_VIDEO_CODEC_ENCODER_FAILURE : result;
    }
    void SetRates(const RateControlParameters& rates) override { inner_->SetRates(rates); }
    void SetFecControllerOverride(webrtc::FecControllerOverride* controller) override {
        inner_->SetFecControllerOverride(controller);
    }
    void OnPacketLossRateUpdate(float loss) override { inner_->OnPacketLossRateUpdate(loss); }
    void OnRttUpdate(int64_t rtt) override { inner_->OnRttUpdate(rtt); }
    void OnLossNotification(const LossNotification& loss) override { inner_->OnLossNotification(loss); }
    EncoderInfo GetEncoderInfo() const override { return inner_->GetEncoderInfo(); }

private:
    std::unique_ptr<webrtc::VideoEncoder> inner_;
};

} // namespace livekit::detail
