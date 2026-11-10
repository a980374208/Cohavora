#include "retained_vp8_decoder.h"
#include "api/make_ref_counted.h"
#include "api/video/video_frame_buffer.h"
#include "api/video_codecs/video_decoder.h"
#include "modules/video_coding/codecs/vp8/include/vp8.h"
#include <mutex>
#include <unordered_map>

namespace livekit::render {
namespace {
struct Registry {
    std::mutex mutex;
    std::unordered_map<const webrtc::VideoFrameBuffer*, size_t> buffers;
};
std::shared_ptr<Registry> RegistryOwner() {
    static const auto owner = std::make_shared<Registry>();
    return owner;
}
// The pinned libvpx VP8 decoder returns exactly VideoFrameBufferPool's CPU
// I420 allocation. Keep its ref, not its pool/decoder. Identity survives the
// WebRTC delivery queue without retaining any entry in a global strong cache.
class CertifiedBuffer : public webrtc::I420BufferInterface {
public:
    explicit CertifiedBuffer(webrtc::scoped_refptr<webrtc::VideoFrameBuffer> input)
        : input_(std::move(input)), registry_(RegistryOwner()) {
        const size_t bytes = size_t(StrideY()) * height() +
            size_t(StrideU() + StrideV()) * ((height() + 1) / 2);
        std::lock_guard lock(registry_->mutex);
        registry_->buffers.emplace(this, bytes);
    }
    ~CertifiedBuffer() override {
        std::lock_guard lock(registry_->mutex);
        registry_->buffers.erase(this);
        // input_ releases only after this lock has gone out of scope.
    }
    int width() const override { return input_->width(); }
    int height() const override { return input_->height(); }
    int StrideY() const override { return input_->GetI420()->StrideY(); }
    int StrideU() const override { return input_->GetI420()->StrideU(); }
    int StrideV() const override { return input_->GetI420()->StrideV(); }
    const uint8_t* DataY() const override { return input_->GetI420()->DataY(); }
    const uint8_t* DataU() const override { return input_->GetI420()->DataU(); }
    const uint8_t* DataV() const override { return input_->GetI420()->DataV(); }
private:
    const webrtc::scoped_refptr<webrtc::VideoFrameBuffer> input_;
    const std::shared_ptr<Registry> registry_;
};

class Decoder final : public webrtc::VideoDecoder, private webrtc::DecodedImageCallback {
public:
    explicit Decoder(const webrtc::Environment& env) : decoder_(webrtc::CreateVp8Decoder(env)) {}
    bool Configure(const Settings& s) override { return decoder_->Configure(s); }
    int32_t Decode(const webrtc::EncodedImage& f, int64_t t) override { return decoder_->Decode(f, t); }
    int32_t Decode(const webrtc::EncodedImage& f, bool missing, int64_t t) override {
        return decoder_->Decode(f, missing, t);
    }
    int32_t RegisterDecodeCompleteCallback(webrtc::DecodedImageCallback* callback) override {
        callback_ = callback;
        return decoder_->RegisterDecodeCompleteCallback(callback ? this : nullptr);
    }
    int32_t Release() override { return decoder_->Release(); }
    DecoderInfo GetDecoderInfo() const override { return decoder_->GetDecoderInfo(); }
    const char* ImplementationName() const override { return decoder_->ImplementationName(); }
private:
    static webrtc::VideoFrame Stamp(const webrtc::VideoFrame& input) {
        auto frame = input;
        // No ToI420: format mismatch fails closed and follows the old path.
        const auto buffer = input.video_frame_buffer();
        if (buffer && buffer->type() == webrtc::VideoFrameBuffer::Type::kI420) {
            frame.set_video_frame_buffer(webrtc::make_ref_counted<CertifiedBuffer>(buffer));
        }
        return frame;
    }
    int32_t Decoded(webrtc::VideoFrame& f) override {
        auto stamped = Stamp(f);
        return callback_ ? callback_->Decoded(stamped) : 0;
    }
    int32_t Decoded(webrtc::VideoFrame& f, int64_t time) override {
        auto stamped = Stamp(f);
        return callback_ ? callback_->Decoded(stamped, time) : 0;
    }
    void Decoded(webrtc::VideoFrame& f, std::optional<int32_t> time,
                 std::optional<uint8_t> qp) override {
        auto stamped = Stamp(f);
        if (callback_) callback_->Decoded(stamped, time, qp);
    }
    std::unique_ptr<webrtc::VideoDecoder> decoder_;
    webrtc::DecodedImageCallback* callback_ = nullptr;
};
}
std::unique_ptr<webrtc::VideoDecoder> CreateRetainedVp8Decoder(const webrtc::Environment& env) {
#if defined(COHAVORA_CERTIFIED_VP8) && COHAVORA_CERTIFIED_VP8
    return std::make_unique<Decoder>(env);
#else
    return webrtc::CreateVp8Decoder(env);
#endif
}
std::optional<size_t> CertifiedVp8RetainedBytes(const webrtc::VideoFrameBuffer* buffer) {
    const auto registry = RegistryOwner();
    std::lock_guard lock(registry->mutex);
    const auto found = registry->buffers.find(buffer);
    return found == registry->buffers.end() ? std::nullopt : std::optional<size_t>(found->second);
}
}
