#pragma once

#include "src/render/owned_i420_frame.h"

namespace livekit::dx11 {

// Matches the normalized I420 samples used by the D3D shaders. Rows are RGB,
// columns are Y/U/V/unused; offsets are applied before the matrix multiply.
// Keeping this policy outside the shader source makes the default and every
// supported WebRTC colour-space mapping deterministic and unit-testable.
struct YuvColorConversion {
    float yuv_to_rgb[3][4]{};
    float yuv_offset[4]{};
};

inline YuvColorConversion MakeYuvColorConversion(const render::RenderColorSpace& color_space) {
    const auto matrix = color_space.matrix == render::RenderColorMatrix::Unspecified
        ? render::RenderColorMatrix::Bt601
        : color_space.matrix;
    const bool full_range = color_space.range == render::RenderColorRange::Full;

    YuvColorConversion conversion{};
    conversion.yuv_offset[0] = full_range ? 0.0f : -16.0f / 255.0f;
    conversion.yuv_offset[1] = -0.5f;
    conversion.yuv_offset[2] = -0.5f;

    switch (matrix) {
    case render::RenderColorMatrix::Bt709:
        conversion.yuv_to_rgb[0][0] = full_range ? 1.0f : 1.164383f;
        conversion.yuv_to_rgb[0][2] = full_range ? 1.574800f : 1.792741f;
        conversion.yuv_to_rgb[1][0] = full_range ? 1.0f : 1.164383f;
        conversion.yuv_to_rgb[1][1] = full_range ? -0.187324f : -0.213249f;
        conversion.yuv_to_rgb[1][2] = full_range ? -0.468124f : -0.532909f;
        conversion.yuv_to_rgb[2][0] = full_range ? 1.0f : 1.164383f;
        conversion.yuv_to_rgb[2][1] = full_range ? 1.855600f : 2.112402f;
        break;
    case render::RenderColorMatrix::Bt2020:
        conversion.yuv_to_rgb[0][0] = full_range ? 1.0f : 1.164383f;
        conversion.yuv_to_rgb[0][2] = full_range ? 1.474600f : 1.678674f;
        conversion.yuv_to_rgb[1][0] = full_range ? 1.0f : 1.164383f;
        conversion.yuv_to_rgb[1][1] = full_range ? -0.164553f : -0.187326f;
        conversion.yuv_to_rgb[1][2] = full_range ? -0.571353f : -0.650424f;
        conversion.yuv_to_rgb[2][0] = full_range ? 1.0f : 1.164383f;
        conversion.yuv_to_rgb[2][1] = full_range ? 1.881400f : 2.141772f;
        break;
    case render::RenderColorMatrix::Bt601:
    case render::RenderColorMatrix::Unspecified:
    default:
        conversion.yuv_to_rgb[0][0] = full_range ? 1.0f : 1.164383f;
        conversion.yuv_to_rgb[0][2] = full_range ? 1.402000f : 1.596027f;
        conversion.yuv_to_rgb[1][0] = full_range ? 1.0f : 1.164383f;
        conversion.yuv_to_rgb[1][1] = full_range ? -0.344136f : -0.391762f;
        conversion.yuv_to_rgb[1][2] = full_range ? -0.714136f : -0.812968f;
        conversion.yuv_to_rgb[2][0] = full_range ? 1.0f : 1.164383f;
        conversion.yuv_to_rgb[2][1] = full_range ? 1.772000f : 2.017232f;
        break;
    }
    return conversion;
}

} // namespace livekit::dx11
