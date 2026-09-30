#pragma once

// Test-only references are the actual I420 frames entering ScreenShareSession.
// A visual frame ID pairs decoded pixels with their source without depending
// on SFU timestamp rewriting. No source or decoded pixels are persisted.
#include "video_frame.h"
#include "render/owned_i420_frame.h"
#include "libyuv/compare.h"
#include <nlohmann/json.hpp>
#include <deque>
#include <mutex>
#include <optional>
#include <algorithm>

class ShareQualityProbe {
public:
    explicit ShareQualityProbe(int width = 800, int height = 600) : width_(width), height_(height) {}
    int FrameId(const uint8_t* y, int stride) const {
        int id = 0;
        for (int bit = 0; bit < 12; ++bit) {
            const int value = y[(16 * height_ / 600) * stride + (36 + bit * 12) * width_ / 800];
            if (value > 180) id |= 1 << bit;
            else if (value >= 70) return -1;
        }
        return id;
    }
    void Capture(const livekit::VideoFrame& frame) {
        if (frame.width() != width_ || frame.height() != height_) return;
        const int id = FrameId(frame.data(), frame.width());
        if (id < 0) return;
        auto owned = std::make_shared<livekit::VideoFrame>(frame);
        std::lock_guard lock(mutex_);
        references_.emplace_back(id, std::move(owned));
        while (references_.size() > 64) references_.pop_front();
    }
    void Receive(livekit::render::OwnedI420Frame::Ptr frame) {
        std::lock_guard lock(mutex_);
        latest_ = std::move(frame);
    }
    nlohmann::json Sample() {
        livekit::render::OwnedI420Frame::Ptr decoded;
        std::shared_ptr<livekit::VideoFrame> reference;
        int id = -1;
        {
            std::lock_guard lock(mutex_);
            decoded = latest_;
            if (decoded && decoded->width() == width_ && decoded->height() == height_) {
                id = FrameId(decoded->data_y(), decoded->stride_y());
                for (auto it = references_.rbegin(); it != references_.rend(); ++it)
                    if (it->first == id) { reference = it->second; break; }
            }
        }
        if (!reference || id < 0 || id == last_id_) {
            ++unmatched_;
            return {{"status", "FAIL"}, {"reason", "unmatched_or_stale_frame"}, {"frame_id", id}};
        }
        last_id_ = id;
        const auto* y = reference->data();
        const auto* u = y + width_ * height_;
        const auto* v = u + width_ * height_ / 4;
        const double psnr = libyuv::I420Psnr(y, width_, u, width_ / 2, v, width_ / 2,
            decoded->data_y(), decoded->stride_y(), decoded->data_u(), decoded->stride_u(),
            decoded->data_v(), decoded->stride_v(), width_, height_);
        const double ssim = libyuv::CalcFrameSsim(y, width_, decoded->data_y(),
            decoded->stride_y(), width_, height_);
        const double text_psnr = libyuv::CalcFramePsnr(y + (40 * height_ / 600) * width_, width_,
            decoded->data_y() + (40 * height_ / 600) * decoded->stride_y(), decoded->stride_y(), width_, 190 * height_ / 600);
        min_psnr_ = std::min(min_psnr_, psnr);
        min_ssim_ = std::min(min_ssim_, ssim);
        min_text_psnr_ = std::min(min_text_psnr_, text_psnr);
        ++samples_;
        return {{"status", psnr >= 30 && ssim >= 0.95 && text_psnr >= 30 ? "PASS" : "FAIL"},
                {"frame_id", id}, {"i420_psnr_db", psnr},
                {"luma_ssim", ssim}, {"text_luma_psnr_db", text_psnr}};
    }
    nlohmann::json Summary() const {
        const bool pass = samples_ >= 25 && unmatched_ == 0 && min_psnr_ >= 30.0 &&
            min_ssim_ >= 0.95 && min_text_psnr_ >= 30.0;
        return {{"status", pass ? "PASS" : "FAIL"}, {"warmup_seconds", 15}, {"samples", samples_},
            {"unmatched_or_stale", unmatched_}, {"min_i420_psnr_db", min_psnr_},
            {"min_luma_ssim", min_ssim_}, {"min_text_luma_psnr_db", min_text_psnr_},
            {"thresholds", {{"psnr_db", 30}, {"luma_ssim", 0.95}, {"text_psnr_db", 30}}},
            {"width", width_}, {"height", height_},
            {"scope", "owned text/color/scrolling pattern stretched to capture dimensions; production capture/publish/decode"}};
    }
private:
    int width_, height_;
    std::mutex mutex_;
    std::deque<std::pair<int, std::shared_ptr<livekit::VideoFrame>>> references_;
    livekit::render::OwnedI420Frame::Ptr latest_;
    int last_id_ = -1, samples_ = 0, unmatched_ = 0;
    double min_psnr_ = 128, min_ssim_ = 1, min_text_psnr_ = 128;
};
