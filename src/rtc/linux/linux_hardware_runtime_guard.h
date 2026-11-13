#pragma once

#include "api/video_codecs/video_encoder.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "linux_encoder_capability_types.h"
#include <memory>
#include <utility>

namespace livekit {
void InvalidateVideoEncoderCapabilities(LinuxEncoderCapabilityReason reason);
VideoEncoderCapabilities GetVideoEncoderCapabilities();
namespace detail {
bool LinuxHardwareEncoderRetirementFailed();

// Driver/identity errors invalidate discovery once, without probing or hopping
// threads from the encoder sequence. The Qt owner schedules a later refresh.
class LinuxHardwareRuntimeGuard final : public webrtc::VideoEncoder {
public:
    LinuxHardwareRuntimeGuard(std::unique_ptr<webrtc::VideoEncoder> encoder, std::uint64_t generation)
        : encoder_(std::move(encoder)), generation_(generation) {}
    int InitEncode(const webrtc::VideoCodec* codec, const Settings& settings) override {
        if (!initialized_ && GetVideoEncoderCapabilities().generation != generation_)
            return WEBRTC_VIDEO_CODEC_ERROR;
        initialized_ = true;
        return Observe(encoder_->InitEncode(codec, settings));
    }
    int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) override {
        return encoder_->RegisterEncodeCompleteCallback(callback);
    }
    int32_t Release() override { return Observe(encoder_->Release()); }
    int32_t Encode(const webrtc::VideoFrame& frame, const std::vector<webrtc::VideoFrameType>* types) override {
        return Observe(encoder_->Encode(frame, types));
    }
    void SetRates(const RateControlParameters& rates) override { encoder_->SetRates(rates); }
    void SetFecControllerOverride(webrtc::FecControllerOverride* controller) override {
        encoder_->SetFecControllerOverride(controller);
    }
    void OnPacketLossRateUpdate(float loss) override { encoder_->OnPacketLossRateUpdate(loss); }
    void OnRttUpdate(int64_t rtt) override { encoder_->OnRttUpdate(rtt); }
    void OnLossNotification(const LossNotification& loss) override { encoder_->OnLossNotification(loss); }
    EncoderInfo GetEncoderInfo() const override { return encoder_->GetEncoderInfo(); }
private:
    int Observe(int status) {
        if (status < 0 && !invalidated_) {
            invalidated_ = true;
            InvalidateVideoEncoderCapabilities(LinuxHardwareEncoderRetirementFailed()
                ? LinuxEncoderCapabilityReason::RetirementFailed : LinuxEncoderCapabilityReason::UnknownUnavailable);
        }
        return status;
    }
    std::unique_ptr<webrtc::VideoEncoder> encoder_;
    bool invalidated_ = false;
    bool initialized_ = false;
    std::uint64_t generation_ = 0;
};
} // namespace detail
} // namespace livekit
