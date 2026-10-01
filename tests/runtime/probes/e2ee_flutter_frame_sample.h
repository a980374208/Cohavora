#pragma once

#include "flutter_common.h"
#include "flutter_webrtc_base.h"
#include "rtc_video_frame.h"
#include "rtc_video_renderer.h"
#include "rtc_video_track.h"
#include "e2ee_frame_signature.h"
#include <condition_variable>
#include <chrono>
#include <thread>
#include <atomic>

namespace flutter_webrtc_plugin {
// Installed only in the task-owned interop plugin. The existing captureFrame
// API writes PNG files; this sampler retains no frame and performs no file I/O.
class E2eeMemoryFrameSample final
    : public libwebrtc::RTCVideoRenderer<libwebrtc::scoped_refptr<libwebrtc::RTCVideoFrame>> {
public:
    ~E2eeMemoryFrameSample() { if (owner_) owner_->RemoveRenderer(this); }
    void OnFrame(libwebrtc::scoped_refptr<libwebrtc::RTCVideoFrame> frame) override {
        std::lock_guard<std::mutex> lock(mutex_);
        value_ = e2ee_frame_test::SampleLuma(frame->DataY(), frame->StrideY(), frame->width(), frame->height());
        ++sequence_;
        ready_.notify_one();
    }
    void Read(libwebrtc::RTCVideoTrack* track, std::unique_ptr<MethodResultProxy> result) {
        if (owner_.get() != track) {
            if (owner_) owner_->RemoveRenderer(this);
            { std::lock_guard<std::mutex> lock(mutex_); value_.reset(); sequence_ = consumed_ = 0; }
            owner_ = libwebrtc::scoped_refptr<libwebrtc::RTCVideoTrack>(track);
            owner_->AddRenderer(this);
        }
        EncodableList cells;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait_for(lock, std::chrono::seconds(2), [&] { return sequence_ > consumed_; });
            if (sequence_ > consumed_ && value_) {
                for (int value : *value_) cells.emplace_back(value);
                consumed_ = sequence_;
            }
        }
        if (cells.empty()) { result->Error("frame_unavailable", "No new valid video frame"); return; }
        result->Success(EncodableValue(cells));
    }
private:
    libwebrtc::scoped_refptr<libwebrtc::RTCVideoTrack> owner_;
    std::mutex mutex_;
    std::condition_variable ready_;
    uint64_t sequence_ = 0, consumed_ = 0;
    std::optional<e2ee_frame_test::Signature> value_;
};
// One owned worker; never block the platform thread waiting for a frame.
// The method-result proxy marshals completion to the platform task runner.
class E2eeMemoryFrameWorker final {
public:
    ~E2eeMemoryFrameWorker() { if (worker_.joinable()) worker_.join(); }
    void Start(libwebrtc::RTCVideoTrack* track, std::unique_ptr<MethodResultProxy> result) {
        if (busy_.exchange(true)) { result->Error("sample_busy", "Sampling already active"); return; }
        if (worker_.joinable()) worker_.join();
        worker_ = std::thread([this, owner = libwebrtc::scoped_refptr<libwebrtc::RTCVideoTrack>(track),
                              reply = std::move(result)]() mutable {
            sampler_.Read(owner.get(), std::move(reply));
            busy_.store(false);
        });
    }
private:
    E2eeMemoryFrameSample sampler_;
    std::thread worker_;
    std::atomic<bool> busy_{false};
};
} // namespace flutter_webrtc_plugin
