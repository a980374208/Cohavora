#include "rtc_video_source.h"
#include "telemetry.h"
#include "api/scoped_refptr.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video/video_rotation.h"
#include <cstring>
#include <chrono>
#include <iostream>
#include <atomic>
#include <algorithm>
#include <cstdint>

namespace livekit {

webrtc::scoped_refptr<RtcVideoSource> RtcVideoSource::Create(std::shared_ptr<VideoSource> source, bool screencast) {
    return webrtc::make_ref_counted<RtcVideoSource>(source, screencast);
}

RtcVideoSource::RtcVideoSource(std::shared_ptr<VideoSource> source, bool screencast)
    : lk_source_(source), screencast_(screencast) {
    if (lk_source_) {
        subscription_ = lk_source_->subscribe([this](const VideoFrame& frame, const VideoCaptureOptions& options) {
            OnVideoFrame(frame, options);
        });
    }
}

RtcVideoSource::~RtcVideoSource() {
    if (subscription_) subscription_->disconnect();
}

void RtcVideoSource::OnVideoFrame(const VideoFrame& frame, const VideoCaptureOptions& options) {
    int width = frame.width();
    int height = frame.height();
    if (width <= 0 || height <= 0) {
        return;
    }

    webrtc::scoped_refptr<webrtc::I420Buffer> i420_buffer = webrtc::I420Buffer::Create(width, height);
    
    if (frame.type() == VideoBufferType::I420) {
        auto planes = frame.planeInfos();
        if (planes.size() >= 3) {
            const uint8_t* y_data = reinterpret_cast<const uint8_t*>(planes[0].data_ptr);
            const uint8_t* u_data = reinterpret_cast<const uint8_t*>(planes[1].data_ptr);
            const uint8_t* v_data = reinterpret_cast<const uint8_t*>(planes[2].data_ptr);

            for (int r = 0; r < height; ++r) {
                std::memcpy(i420_buffer->MutableDataY() + r * i420_buffer->StrideY(),
                            y_data + r * planes[0].stride,
                            width);
            }
            int chroma_h = (height + 1) / 2;
            int chroma_w = (width + 1) / 2;
            for (int r = 0; r < chroma_h; ++r) {
                std::memcpy(i420_buffer->MutableDataU() + r * i420_buffer->StrideU(),
                            u_data + r * planes[1].stride,
                            chroma_w);
                std::memcpy(i420_buffer->MutableDataV() + r * i420_buffer->StrideV(),
                            v_data + r * planes[2].stride,
                            chroma_w);
            }
        }
    } else if (frame.type() == VideoBufferType::NV12) {
        const uint8_t* y_src = frame.data();
        const uint8_t* uv_src = frame.data() + (width * height);
        uint8_t* y_dst = i420_buffer->MutableDataY();
        uint8_t* u_dst = i420_buffer->MutableDataU();
        uint8_t* v_dst = i420_buffer->MutableDataV();
        int y_stride = i420_buffer->StrideY();
        int u_stride = i420_buffer->StrideU();
        int v_stride = i420_buffer->StrideV();

        for (int r = 0; r < height; ++r) {
            std::memcpy(y_dst + r * y_stride, y_src + r * width, width);
        }
        int chroma_h = (height + 1) / 2;
        int chroma_w = (width + 1) / 2;
        for (int r = 0; r < chroma_h; ++r) {
            for (int c = 0; c < chroma_w; ++c) {
                u_dst[r * u_stride + c] = uv_src[r * width + c * 2];
                v_dst[r * v_stride + c] = uv_src[r * width + c * 2 + 1];
            }
        }
    } else {
        const uint8_t* rgba = frame.data();
        uint8_t* y_plane = i420_buffer->MutableDataY();
        uint8_t* u_plane = i420_buffer->MutableDataU();
        uint8_t* v_plane = i420_buffer->MutableDataV();

        int y_stride = i420_buffer->StrideY();
        int u_stride = i420_buffer->StrideU();
        int v_stride = i420_buffer->StrideV();

        for (int i = 0; i < height; ++i) {
            for (int j = 0; j < width; ++j) {
                int rgba_idx = (i * width + j) * 4;
                uint8_t r = rgba[rgba_idx];
                uint8_t g = rgba[rgba_idx + 1];
                uint8_t b = rgba[rgba_idx + 2];

                y_plane[i * y_stride + j] = static_cast<uint8_t>(
                    ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);

                if (i % 2 == 0 && j % 2 == 0) {
                    u_plane[(i / 2) * u_stride + (j / 2)] = static_cast<uint8_t>(
                        ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
                    v_plane[(i / 2) * v_stride + (j / 2)] = static_cast<uint8_t>(
                        ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
                }
            }
        }
    }

    if (!screencast_) {
        const bool landscape = width >= height;
        const int max_width = landscape ? 1920 : 1080;
        const int max_height = landscape ? 1080 : 1920;
        int target_w = width;
        int target_h = height;

        if (width > max_width || height > max_height) {
            if (static_cast<int64_t>(max_width) * height <=
                static_cast<int64_t>(max_height) * width) {
                target_w = max_width;
                target_h = static_cast<int>(
                    static_cast<int64_t>(height) * max_width / width);
            } else {
                target_h = max_height;
                target_w = static_cast<int>(
                    static_cast<int64_t>(width) * max_height / height);
            }

            // Chroma planes are most interoperable with even output dimensions.
            target_w = (std::max)(2, target_w & ~1);
            target_h = (std::max)(2, target_h & ~1);
        }

        if (target_w != width || target_h != height) {
            webrtc::scoped_refptr<webrtc::I420Buffer> scaled_buffer =
                webrtc::I420Buffer::Create(target_w, target_h);
            scaled_buffer->ScaleFrom(*i420_buffer);
            i420_buffer = scaled_buffer;
        }
    }

    webrtc::VideoRotation rtc_rotation = webrtc::kVideoRotation_0;
    if (options.rotation == VideoRotation::VIDEO_ROTATION_90) rtc_rotation = webrtc::kVideoRotation_90;
    else if (options.rotation == VideoRotation::VIDEO_ROTATION_180) rtc_rotation = webrtc::kVideoRotation_180;
    else if (options.rotation == VideoRotation::VIDEO_ROTATION_270) rtc_rotation = webrtc::kVideoRotation_270;

    int64_t timestamp_us = options.timestamp_us > 0 ? options.timestamp_us : (std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());

    webrtc::VideoFrame rtc_frame = webrtc::VideoFrame::Builder()
                                       .set_video_frame_buffer(i420_buffer)
                                       .set_rotation(rtc_rotation)
                                       .set_timestamp_us(timestamp_us)
                                       .build();

    Telemetry::Instance().OnFirstVideoFrameInjected();
    OnFrame(rtc_frame);
}

} // namespace livekit
