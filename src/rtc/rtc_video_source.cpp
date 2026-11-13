#include "rtc_video_source.h"
#include "../telemetry/session_telemetry.h"
#include "api/scoped_refptr.h"
#include "api/video/i420_buffer.h"
#include "api/video/nv12_buffer.h"
#include "api/video/video_frame.h"
#include "api/video/video_rotation.h"
#include "libyuv/convert.h"
#include "libyuv/convert_argb.h"
#include "libyuv/convert_from_argb.h"
#include "libyuv/planar_functions.h"
#include "rtc_base/time_utils.h"
#include <cstring>
#include <iostream>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <vector>

namespace livekit {

namespace { std::atomic<std::uint64_t> live_rtc_video_sources{0}; }

std::uint64_t RtcVideoSource::LiveInstanceCount() noexcept {
    return live_rtc_video_sources.load(std::memory_order_relaxed);
}

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
    live_rtc_video_sources.fetch_add(1, std::memory_order_relaxed);
}

RtcVideoSource::~RtcVideoSource() {
    if (subscription_) subscription_->disconnect();
    SetTelemetryProbe({});
    live_rtc_video_sources.fetch_sub(1, std::memory_order_relaxed);
}

void RtcVideoSource::SetTelemetryProbe(
    std::shared_ptr<telemetry::LocalVideoActivityProbe> probe) noexcept {
    auto previous = std::atomic_exchange_explicit(
        &telemetry_probe_, std::move(probe), std::memory_order_acq_rel);
    if (previous) previous->active.store(false, std::memory_order_release);
}

VideoFrameDiagnostics RtcVideoSource::frame_diagnostics() const noexcept {
    VideoFrameDiagnostics result;
    result.source_available = static_cast<bool>(lk_source_);
    result.rtc_available = true;
    if (lk_source_) result.source_frames = lk_source_->captured_frame_count();
    result.rtc_input_frames = input_frames_.load(std::memory_order_relaxed);
    result.rtc_output_frames = output_frames_.load(std::memory_order_relaxed);
    result.rtc_dropped_frames = dropped_frames_.load(std::memory_order_relaxed);
    return result;
}

void RtcVideoSource::OnVideoFrame(const VideoFrame& frame, const VideoCaptureOptions& options) {
    input_frames_.fetch_add(1, std::memory_order_relaxed);
    int width = frame.width();
    int height = frame.height();
    if (width <= 0 || height <= 0 || frame.dataSize() == 0) {
        dropped_frames_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Match the native SDK's capture-clock -> WebRTC-clock alignment. Camera
    // timestamps can be relative to a capture session and reset on restart.
    // Mixing them with a system-clock fallback makes the encoder discard every
    // subsequent frame as old after a reset or a missing capture timestamp.
    const int64_t system_timestamp_us = webrtc::TimeMicros();
    const int64_t capture_timestamp_us = options.timestamp_us > 0
        ? options.timestamp_us : system_timestamp_us;
    const int64_t timestamp_us = timestamp_aligner_.TranslateTimestamp(
        capture_timestamp_us, system_timestamp_us);

    int target_w = width;
    int target_h = height;
    if (!screencast_) {
        const bool landscape = width >= height;
        // Preserve ordinary-video sources through the supported 4K ceiling.
        // Smaller published layers are selected by simulcast/subscription.
        const int max_width = landscape ? 3840 : 2160;
        const int max_height = landscape ? 2160 : 3840;

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
    }

    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> frame_buffer;

    if (frame.hasNativeBuffer()) {
        auto native_buf = frame.videoFrameBuffer();
        if (native_buf) {
            if (target_w != width || target_h != height) {
                frame_buffer = native_buf->Scale(target_w, target_h);
            } else {
                frame_buffer = std::move(native_buf);
            }
        }
    }

    if (!frame_buffer && frame.type() == VideoBufferType::NV12) {
        webrtc::scoped_refptr<webrtc::NV12BufferInterface> nv12_src = frame.nv12Buffer();
        if (!nv12_src) {
            auto copied = webrtc::NV12Buffer::Create(width, height);
            const auto planes = frame.planeInfos();
            if (planes.size() >= 2) {
                libyuv::NV12Copy(
                    reinterpret_cast<const uint8_t*>(planes[0].data_ptr),
                    static_cast<int>(planes[0].stride),
                    reinterpret_cast<const uint8_t*>(planes[1].data_ptr),
                    static_cast<int>(planes[1].stride),
                    copied->MutableDataY(), copied->StrideY(),
                    copied->MutableDataUV(), copied->StrideUV(),
                    width, height);
            } else {
                const uint8_t* y_src = frame.data();
                const uint8_t* uv_src = y_src + static_cast<std::size_t>(width) * height;
                const int uv_stride = ((width + 1) / 2) * 2;
                libyuv::NV12Copy(
                    y_src, width, uv_src, uv_stride,
                    copied->MutableDataY(), copied->StrideY(),
                    copied->MutableDataUV(), copied->StrideUV(),
                    width, height);
            }
            nv12_src = std::move(copied);
        }

        if (target_w != width || target_h != height) {
            auto scaled = webrtc::NV12Buffer::Create(target_w, target_h);
            scaled->CropAndScaleFrom(*nv12_src, 0, 0, width, height);
            frame_buffer = std::move(scaled);
        } else {
            frame_buffer = std::move(nv12_src);
        }
    } else if (!frame_buffer &&
               (frame.type() == VideoBufferType::I420 ||
                frame.type() == VideoBufferType::I420A)) {
        webrtc::scoped_refptr<webrtc::I420Buffer> i420_buffer =
            webrtc::I420Buffer::Create(width, height);
        const auto planes = frame.planeInfos();
        if (planes.size() >= 3) {
            libyuv::I420Copy(
                reinterpret_cast<const uint8_t*>(planes[0].data_ptr),
                static_cast<int>(planes[0].stride),
                reinterpret_cast<const uint8_t*>(planes[1].data_ptr),
                static_cast<int>(planes[1].stride),
                reinterpret_cast<const uint8_t*>(planes[2].data_ptr),
                static_cast<int>(planes[2].stride),
                i420_buffer->MutableDataY(), i420_buffer->StrideY(),
                i420_buffer->MutableDataU(), i420_buffer->StrideU(),
                i420_buffer->MutableDataV(), i420_buffer->StrideV(),
                width, height);
        }

        if (target_w != width || target_h != height) {
            webrtc::scoped_refptr<webrtc::I420Buffer> scaled_buffer =
                webrtc::I420Buffer::Create(target_w, target_h);
            scaled_buffer->ScaleFrom(*i420_buffer);
            i420_buffer = std::move(scaled_buffer);
        }
        frame_buffer = std::move(i420_buffer);
    } else if (!frame_buffer) {
        auto nv12_buffer = webrtc::NV12Buffer::Create(width, height);
        const uint8_t* src = frame.data();
        if (frame.type() == VideoBufferType::BGRA) {
            libyuv::ARGBToNV12(
                src, width * 4,
                nv12_buffer->MutableDataY(), nv12_buffer->StrideY(),
                nv12_buffer->MutableDataUV(), nv12_buffer->StrideUV(),
                width, height);
        } else if (frame.type() == VideoBufferType::RGBA) {
            libyuv::ABGRToNV12(
                src, width * 4,
                nv12_buffer->MutableDataY(), nv12_buffer->StrideY(),
                nv12_buffer->MutableDataUV(), nv12_buffer->StrideUV(),
                width, height);
        } else {
            std::vector<uint8_t> temp_argb(static_cast<std::size_t>(width) * height * 4);
            if (frame.type() == VideoBufferType::ARGB) {
                libyuv::BGRAToARGB(src, width * 4, temp_argb.data(), width * 4, width, height);
            } else if (frame.type() == VideoBufferType::ABGR) {
                libyuv::RGBAToARGB(src, width * 4, temp_argb.data(), width * 4, width, height);
            } else if (frame.type() == VideoBufferType::RGB24) {
                libyuv::RAWToARGB(src, width * 3, temp_argb.data(), width * 4, width, height);
            }
            libyuv::ARGBToNV12(
                temp_argb.data(), width * 4,
                nv12_buffer->MutableDataY(), nv12_buffer->StrideY(),
                nv12_buffer->MutableDataUV(), nv12_buffer->StrideUV(),
                width, height);
        }

        if (target_w != width || target_h != height) {
            auto scaled = webrtc::NV12Buffer::Create(target_w, target_h);
            scaled->CropAndScaleFrom(*nv12_buffer, 0, 0, width, height);
            frame_buffer = std::move(scaled);
        } else {
            frame_buffer = std::move(nv12_buffer);
        }
    }

    webrtc::VideoRotation rtc_rotation = webrtc::kVideoRotation_0;
    if (options.rotation == VideoRotation::VIDEO_ROTATION_90) rtc_rotation = webrtc::kVideoRotation_90;
    else if (options.rotation == VideoRotation::VIDEO_ROTATION_180) rtc_rotation = webrtc::kVideoRotation_180;
    else if (options.rotation == VideoRotation::VIDEO_ROTATION_270) rtc_rotation = webrtc::kVideoRotation_270;

    webrtc::VideoFrame rtc_frame = webrtc::VideoFrame::Builder()
                                       .set_video_frame_buffer(frame_buffer)
                                       .set_rotation(rtc_rotation)
                                       .set_timestamp_us(timestamp_us)
                                       .build();

    // This counts API submissions, even when the bridge has no encoder sink.
    output_frames_.fetch_add(1, std::memory_order_relaxed);
    if (const auto probe = std::atomic_load_explicit(
            &telemetry_probe_, std::memory_order_acquire);
        probe && probe->active.load(std::memory_order_acquire)) {
        const auto injected_at = telemetry::SessionTelemetry::Clock::now();
        const auto injected_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            injected_at.time_since_epoch()).count();
        const auto submitted_width = static_cast<std::uint32_t>(rtc_frame.width());
        const auto submitted_height = static_cast<std::uint32_t>(rtc_frame.height());
        const auto previous_width = probe->width.exchange(
            submitted_width, std::memory_order_acq_rel);
        const auto previous_height = probe->height.exchange(
            submitted_height, std::memory_order_acq_rel);
        if (previous_width != 0 && previous_height != 0 &&
            (previous_width != submitted_width ||
             previous_height != submitted_height)) {
            probe->format_changes.fetch_add(1, std::memory_order_relaxed);
        }
        if (options.timestamp_us > 0) {
            const auto previous_timestamp = probe->capture_timestamp_us.exchange(
                options.timestamp_us, std::memory_order_acq_rel);
            if (previous_timestamp > 0 && options.timestamp_us < previous_timestamp) {
                probe->clock_resets.fetch_add(1, std::memory_order_relaxed);
            }
        }
        probe->last_frame_ns.store(injected_ns, std::memory_order_release);
        probe->frame_count.fetch_add(1, std::memory_order_relaxed);
        auto unset = std::int64_t{0};
        if (probe->first_injected_ns.compare_exchange_strong(
                unset, injected_ns, std::memory_order_acq_rel)) {
            bool not_submitted = false;
            if (probe->first_event_submitted.compare_exchange_strong(
                    not_submitted, true, std::memory_order_acq_rel)) {
                if (const auto owner = probe->telemetry.lock()) {
                    owner->RecordLocalVideoFrameInjected(
                        probe->series_key,
                        probe->room_generation,
                        probe->publication_epoch,
                        injected_at);
                }
            }
        }
    }
    OnFrame(rtc_frame);
}

} // namespace livekit
