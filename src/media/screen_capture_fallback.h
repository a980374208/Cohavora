#pragma once

#include "modules/desktop_capture/desktop_capturer.h"
#include "modules/desktop_capture/desktop_frame.h"
#include <functional>
#include <chrono>
#include <vector>

namespace livekit {

// Synchronous Windows capturers only. Create and destroy backends on the
// capture worker, and never destroy one while its callback is on the stack.
class ScreenCaptureFallback final : public webrtc::DesktopCapturer,
                                    private webrtc::DesktopCapturer::Callback {
public:
    using Factory = std::function<std::unique_ptr<webrtc::DesktopCapturer>()>;
    struct Backend { Factory create; bool retry_temporary = false; };
    explicit ScreenCaptureFallback(std::vector<Backend> factories)
        : factories_(std::move(factories)) {}
    bool GetSourceList(SourceList*) override { return false; }
    bool SelectSource(SourceId id) override {
        current_.reset();
        next_ = 0;
        source_ = id;
        return Advance();
    }
    void Start(webrtc::DesktopCapturer::Callback* callback) override {
        callback_ = callback;
    }
    void SetMaxFrameRate(uint32_t rate) override { rate_ = rate; }
    void CaptureFrame() override {
        if (!callback_) return;
        while (current_) {
            result_ = Result::ERROR_PERMANENT;
            frame_.reset();
            try {
                if (!started_) {
                    current_->Start(this);
                    current_->SetMaxFrameRate(rate_);
                    started_ = true;
                }
                current_->CaptureFrame();
            } catch (...) {
                result_ = Result::ERROR_PERMANENT;
                frame_.reset();
            }
            if (result_ == Result::SUCCESS && frame_) {
                last_success_ = std::chrono::steady_clock::now();
                callback_->OnCaptureResult(result_, std::move(frame_));
                return;
            }
            // WGC can initially have no frame yet. DXGI errors fall through
            // immediately; later backends retain the existing idle tolerance.
            if (result_ == Result::ERROR_TEMPORARY && factories_[next_ - 1].retry_temporary &&
                std::chrono::steady_clock::now() - last_success_ < std::chrono::seconds(5)) {
                callback_->OnCaptureResult(result_, nullptr);
                return;
            }
            // No backend oscillation; preserve the selected monitor id.
            if (!Advance()) break;
        }
        callback_->OnCaptureResult(Result::ERROR_PERMANENT, nullptr);
    }
private:
    bool Advance() {
        current_.reset();
        started_ = false;
        while (next_ < factories_.size()) {
            const auto index = next_++;
            try {
                current_ = factories_[index].create();
                if (current_ && current_->SelectSource(source_)) {
                    last_success_ = std::chrono::steady_clock::now();
                    return true;
                }
            } catch (...) {}
            current_.reset();
        }
        return false;
    }
    void OnCaptureResult(Result result, std::unique_ptr<webrtc::DesktopFrame> frame) override {
        result_ = result;
        frame_ = std::move(frame);
    }
    std::vector<Backend> factories_;
    std::unique_ptr<webrtc::DesktopCapturer> current_;
    std::unique_ptr<webrtc::DesktopFrame> frame_;
    webrtc::DesktopCapturer::Callback* callback_ = nullptr;
    SourceId source_ = 0;
    std::size_t next_ = 0;
    uint32_t rate_ = 15;
    bool started_ = false;
    Result result_ = Result::ERROR_PERMANENT;
    std::chrono::steady_clock::time_point last_success_;
};
} // namespace livekit
