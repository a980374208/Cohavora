#pragma once
#include <cstddef>
#include <memory>
#include <optional>
#include "api/scoped_refptr.h"
namespace webrtc { class Environment; class VideoDecoder; class VideoFrameBuffer; class I420Buffer; }
namespace livekit { class H265VideoDecoder; }
namespace livekit::render {
// Only the HEVC decoder may certify an external CPU plane owner.
class HevcFramePublisher final {
    friend class livekit::H265VideoDecoder;
    static webrtc::scoped_refptr<webrtc::VideoFrameBuffer> Publish(
        webrtc::scoped_refptr<webrtc::VideoFrameBuffer>, size_t backing_bytes);
};
std::unique_ptr<webrtc::VideoDecoder> CreateRetainedVp8Decoder(const webrtc::Environment&);
webrtc::scoped_refptr<webrtc::VideoFrameBuffer> PublishOwnedDecodedI420(
    webrtc::scoped_refptr<webrtc::I420Buffer>);
std::optional<size_t> CertifiedDecodedI420Bytes(const webrtc::VideoFrameBuffer*);
std::optional<size_t> CertifiedVp8RetainedBytes(const webrtc::VideoFrameBuffer*);
}
