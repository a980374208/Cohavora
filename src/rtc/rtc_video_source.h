#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include "media/base/adapted_video_track_source.h"
#include "api/video/video_frame.h"
#include "api/video/i420_buffer.h"
#include "rtc_base/ref_counted_object.h"
#include "rtc_base/timestamp_aligner.h"
#include "video_source.h"

namespace livekit {

namespace telemetry { struct LocalVideoActivityProbe; }

class RtcVideoSource : public webrtc::AdaptedVideoTrackSource {
public:
    static webrtc::scoped_refptr<RtcVideoSource> Create(std::shared_ptr<VideoSource> source, bool screencast = false);

    explicit RtcVideoSource(std::shared_ptr<VideoSource> source, bool screencast = false);
    ~RtcVideoSource() override;
    static std::uint64_t LiveInstanceCount() noexcept;

    // webrtc::MediaSourceInterface impl
    SourceState state() const override { return kLive; }
    bool remote() const override { return false; }
    bool is_screencast() const override { return screencast_; }
    std::optional<bool> needs_denoising() const override { return false; }

    VideoFrameDiagnostics frame_diagnostics() const noexcept;
    void SetTelemetryProbe(
        std::shared_ptr<telemetry::LocalVideoActivityProbe> probe) noexcept;

private:
    struct PendingFrame {
        VideoFrame frame;
        VideoCaptureOptions options;
        std::int64_t arrival_system_us = 0;
    };

    void EnqueueFrame(const VideoFrame& frame, const VideoCaptureOptions& options);
    void WorkerLoop();
    void ProcessFrame(const VideoFrame& frame, const VideoCaptureOptions& options,
                      std::int64_t arrival_system_us);

    std::shared_ptr<VideoSource> lk_source_;
    const bool screencast_;
    // Accessed exclusively on worker_thread_ after WorkerLoop starts.
    webrtc::TimestampAligner timestamp_aligner_;
    std::atomic<std::uint64_t> input_frames_{0};
    std::atomic<std::uint64_t> output_frames_{0};
    std::atomic<std::uint64_t> dropped_frames_{0};
    std::shared_ptr<telemetry::LocalVideoActivityProbe> telemetry_probe_;
    std::mutex mailbox_mutex_;
    std::condition_variable mailbox_cv_;
    std::optional<PendingFrame> pending_frame_;
    bool stopping_ = false;
    std::thread worker_thread_;
    std::shared_ptr<VideoSource::Subscription> subscription_;
};

} // namespace livekit
