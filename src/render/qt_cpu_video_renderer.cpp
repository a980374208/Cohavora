#include "qt_cpu_video_renderer.h"

#include "libyuv/convert_argb.h"
#include <QtGui/QTransform>

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
    if (result != 0) return {};
    if (frame.rotation() != VideoRotation::VIDEO_ROTATION_0) {
        image = image.transformed(QTransform().rotate(static_cast<int>(frame.rotation())));
    }
    return image;
}

QImage QtCpuVideoRenderer::Convert(const VideoRenderFrame& frame) const {
    if (frame.i420Owner()) return Convert(*frame.i420Owner());
    const auto& v = frame.view();
    QImage image;
    if (v.format == LK_RENDER_RGBA8) {
        image = QImage(v.planes[0].data, int(v.width), int(v.height),
            int(v.planes[0].stride_bytes), QImage::Format_RGBA8888).copy();
    } else {
        image = QImage(int(v.width), int(v.height), QImage::Format_ARGB32);
        if (image.isNull()) return {};
        int status = -1;
        if (v.format == LK_RENDER_I420) {
            status = libyuv::I420ToARGBMatrix(v.planes[0].data, int(v.planes[0].stride_bytes),
                v.planes[1].data, int(v.planes[1].stride_bytes), v.planes[2].data, int(v.planes[2].stride_bytes),
                image.bits(), image.bytesPerLine(), SelectYuvConstants(frame.colorSpace()), int(v.width), int(v.height));
        } else if (v.format == LK_RENDER_NV12) {
            status = libyuv::NV12ToARGBMatrix(v.planes[0].data, int(v.planes[0].stride_bytes),
                v.planes[1].data, int(v.planes[1].stride_bytes), image.bits(), image.bytesPerLine(),
                SelectYuvConstants(frame.colorSpace()), int(v.width), int(v.height));
        }
        if (status != 0) return {};
    }
    if (v.rotation_degrees) image = image.transformed(QTransform().rotate(v.rotation_degrees));
    return image;
}

} // namespace livekit::render
