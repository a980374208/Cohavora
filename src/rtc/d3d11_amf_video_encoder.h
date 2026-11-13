#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "api/environment/environment.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"
#include "modules/video_coding/codecs/external/backend_video_encoder.h"
#include "modules/video_coding/codecs/external/video_encoder_backend.h"

namespace livekit {

struct D3d11AmfEncoderDiagnostics {
    std::uint64_t dx11_native_frames_submitted = 0;
    std::uint64_t host_frames_submitted = 0;
    std::uint64_t simulcast_gpu_scales = 0;
    std::uint64_t cross_device_shared_opens = 0;
};

D3d11AmfEncoderDiagnostics GetD3d11AmfEncoderDiagnostics() noexcept;
void ResetD3d11AmfEncoderDiagnostics() noexcept;

// Creates a D3D11-native-capable AMF hardware VideoEncoder (without software
// fallback wrapper), suitable for automatic hardware H.264/H.265 paths.
std::unique_ptr<webrtc::VideoEncoder> CreateD3d11AmfHardwareEncoder(
    const webrtc::SdpVideoFormat& format,
    webrtc::BackendFailurePolicy failure_policy =
        webrtc::BackendFailurePolicy::kDefault);

// Creates a D3D11-native-capable AMF VideoEncoder wrapped with the same-codec
// software fallback (OpenH264 for H.264, x265 for H.265), matching
// webrtc::CreateAmfVideoEncoder semantics while supporting kNative
// D3d11Nv12FrameBuffer inputs via CreateSurfaceFromDX11Native without CPU
// readback or re-upload.
std::unique_ptr<webrtc::VideoEncoder> CreateD3d11AmfVideoEncoder(
    const webrtc::Environment& env,
    const webrtc::SdpVideoFormat& format,
    webrtc::external_encoder::Status* probe_status = nullptr);

}  // namespace livekit
