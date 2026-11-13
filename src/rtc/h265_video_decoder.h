#pragma once

#include <memory>

namespace webrtc {
class Environment;
class VideoDecoder;
struct SdpVideoFormat;
}

namespace livekit {

// Checks the pinned runtime in <executable directory>/media-codecs, including
// an actual HEVC decoder lookup and open. The result is cached for this process;
// installing/replacing the runtime requires restarting the application.
bool IsH265DecoderAvailable();
const char* H265DecoderAvailabilityError();

// Main / 8-bit / 4:2:0, Main tier, SRST. Each decoder owns its FFmpeg context.
// Configure, callback registration, Decode and Release belong to one sequence.
// Delivery is synchronous and owns its I420 pixels; Release discards queued
// output and clears the callback. No task or FFmpeg memory escapes the decoder.
std::unique_ptr<webrtc::VideoDecoder> CreateH265VideoDecoder(
    const webrtc::Environment& environment,
    const webrtc::SdpVideoFormat& format);

}
