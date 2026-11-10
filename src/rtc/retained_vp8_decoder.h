#pragma once
#include <cstddef>
#include <memory>
#include <optional>
namespace webrtc { class Environment; class VideoDecoder; class VideoFrameBuffer; }
namespace livekit::render {
// Only this factory can stamp outputs; callers cannot certify arbitrary I420.
std::unique_ptr<webrtc::VideoDecoder> CreateRetainedVp8Decoder(const webrtc::Environment&);
std::optional<size_t> CertifiedVp8RetainedBytes(const webrtc::VideoFrameBuffer*);
}
