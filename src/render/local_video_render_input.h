#pragma once

#include "render/video_render_frame.h"
#include <atomic>
#include <mutex>
#include <utility>

namespace livekit::render {

// One local camera binding, owned/consumed by the UI. Capture only locks weak
// mailbox state and replaces one immutable frame. No Qt or GPU on capture threads.
class LocalVideoRenderInput final {
public:
    LocalVideoRenderInput() = default;
    LocalVideoRenderInput(const LocalVideoRenderInput&) = delete;
    LocalVideoRenderInput& operator=(const LocalVideoRenderInput&) = delete;
    ~LocalVideoRenderInput() { Detach(); }
    void Attach(const std::shared_ptr<VideoSource>& source) {
        if (source && source_.lock() == source && state_) return;
        Detach();
        if (!source) return;
        state_ = std::make_shared<State>();
        source_ = source;
        std::weak_ptr<State> weak = state_;
        subscription_ = source->subscribe([weak](const VideoFrame& frame, const VideoCaptureOptions& options) {
            auto state = weak.lock();
            if (!state || !state->active.load(std::memory_order_acquire)) return;
            auto owned = VideoRenderFrame::CopyFrom(frame, options);
            if (!owned) return;
            std::lock_guard lock(state->mutex);
            if (state->active.load(std::memory_order_relaxed)) state->latest = std::move(owned);
        });
    }
    VideoRenderFrame::Ptr TakeLatest() {
        if (!state_) return {};
        std::lock_guard lock(state_->mutex);
        return std::exchange(state_->latest, {});
    }
    void Detach() {
        // Invalidating the mailbox precedes disconnect's in-flight barrier.
        auto state = std::move(state_);
        if (state) state->active.store(false, std::memory_order_release);
        subscription_.reset();
        source_.reset();
    }
private:
    struct State {
        std::atomic<bool> active{true};
        std::mutex mutex;
        VideoRenderFrame::Ptr latest;
    };
    std::shared_ptr<State> state_;
    std::weak_ptr<VideoSource> source_;
    std::shared_ptr<VideoSource::Subscription> subscription_;
};

} // namespace livekit::render
