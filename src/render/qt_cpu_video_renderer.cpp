#include "qt_cpu_video_renderer.h"

#include "libyuv/convert_argb.h"

namespace livekit::render {
namespace {

const libyuv::YuvConstants* SelectYuvConstants(const RenderColorSpace& color_space) {
    const auto matrix = color_space.matrix == RenderColorMatrix::Unspecified
        ? RenderColorMatrix::Bt601
        : color_space.matrix;
    const bool full_range = color_space.range == RenderColorRange::Full;

    switch (matrix) {
    case RenderColorMatrix::Bt709:
        return full_range ? &libyuv::kYuvF709Constants : &libyuv::kYuvH709Constants;
    case RenderColorMatrix::Bt2020:
        return full_range ? &libyuv::kYuvV2020Constants : &libyuv::kYuv2020Constants;
    case RenderColorMatrix::Bt601:
    case RenderColorMatrix::Unspecified:
    default:
        return full_range ? &libyuv::kYuvJPEGConstants : &libyuv::kYuvI601Constants;
    }
}

} // namespace

QImage QtCpuVideoRenderer::Convert(const OwnedI420Frame& frame) const {
    if (frame.width() <= 0 || frame.height() <= 0 ||
        !frame.data_y() || !frame.data_u() || !frame.data_v()) {
        return {};
    }

    // libyuv ARGB is byte-order BGRA on little-endian Windows, which matches
    // Qt's Format_ARGB32 storage contract.
    QImage image(frame.width(), frame.height(), QImage::Format_ARGB32);
    if (image.isNull()) {
        return image;
    }
    const int result = libyuv::I420ToARGBMatrix(frame.data_y(),
                                                  frame.stride_y(),
                                                  frame.data_u(),
                                                  frame.stride_u(),
                                                  frame.data_v(),
                                                  frame.stride_v(),
                                                  image.bits(),
                                                  image.bytesPerLine(),
                                                  SelectYuvConstants(frame.color_space()),
                                                  frame.width(),
                                                  frame.height());
    return result == 0 ? image : QImage();
}

} // namespace livekit::render
