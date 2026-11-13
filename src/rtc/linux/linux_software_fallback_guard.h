#pragma once

#include "api/video_codecs/video_encoder.h"
#include "modules/video_coding/include/video_error_codes.h"

#include <memory>
#include <utility>

namespace livekit::detail {

bool LinuxHardwareEncoderRetirementFailed();

// Only automatic fallback owns this adapter. Explicit Software keeps the raw
// encoder and Windows never compiles this header. A poisoned hardware session
// must not be hidden by a successful software Init inside WebRTC's wrapper.
class LinuxSoftwareFallbackGuard final : public webrtc::VideoEncoder {
public:
    explicit LinuxSoftwareFallbackGuard(std::unique_ptr<webrtc::VideoEncoder> encoder)
        : encoder_(std::move(encoder)) {}

    int InitEncode(const webrtc::VideoCodec* codec, const Settings& settings) override {
        return LinuxHardwareEncoderRetirementFailed() ? WEBRTC_VIDEO_CODEC_ERROR
                                                     : encoder_->InitEncode(codec, settings);
    }
    int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) override {
        return encoder_->RegisterEncodeCompleteCallback(callback);
    }
    int32_t Release() override {
        // Cleanup must execute even when another backend poisoned the process.
        const auto status = encoder_->Release();
        return status != WEBRTC_VIDEO_CODEC_OK ? status :
            (LinuxHardwareEncoderRetirementFailed() ? WEBRTC_VIDEO_CODEC_ERROR : WEBRTC_VIDEO_CODEC_OK);
    }
    int32_t Encode(const webrtc::VideoFrame& frame,
                   const std::vector<webrtc::VideoFrameType>* types) override {
        return LinuxHardwareEncoderRetirementFailed() ? WEBRTC_VIDEO_CODEC_ERROR
                                                     : encoder_->Encode(frame, types);
    }
    void SetRates(const RateControlParameters& rates) override {
        if (!LinuxHardwareEncoderRetirementFailed()) encoder_->SetRates(rates);
    }
    void SetFecControllerOverride(webrtc::FecControllerOverride* controller) override {
        encoder_->SetFecControllerOverride(controller);
    }
    void OnPacketLossRateUpdate(float loss) override { encoder_->OnPacketLossRateUpdate(loss); }
    void OnRttUpdate(int64_t rtt) override { encoder_->OnRttUpdate(rtt); }
    void OnLossNotification(const LossNotification& loss) override { encoder_->OnLossNotification(loss); }
    EncoderInfo GetEncoderInfo() const override { return encoder_->GetEncoderInfo(); }

private:
    std::unique_ptr<webrtc::VideoEncoder> encoder_;
};

} // namespace livekit::detail
