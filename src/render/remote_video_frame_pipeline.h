#pragma once
#include "latest_video_frame_worker.h"
#include "retained_video_frame.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

namespace livekit::telemetry { struct VideoActivityProbe; }
namespace livekit::render {
// Native Room sinks own the pipeline. Qt handles own neither worker nor input.
class RemoteVideoFrameConsumer final {
public:
    ~RemoteVideoFrameConsumer();
    bool active() const;
    OwnedI420Frame::Ptr Take();
    void SetMaxFps(uint32_t fps);
    void Retire();
private:
    friend class RemoteVideoFrameEndpoint;
    std::weak_ptr<LatestVideoFrameCore> core_;
    LatestVideoFrameCore::ConsumerToken token_;
    std::shared_ptr<std::atomic<bool>> binding_active_;
    std::atomic<bool> active_{true};
    std::function<bool()> valid_;
};
class RemoteVideoFrameEndpoint final {
public:
    std::shared_ptr<RemoteVideoFrameConsumer> Subscribe(uint32_t fps, std::function<bool()> valid);
    // True means certified input was handled (including budget rejection).
    // False requires the existing synchronous, demand-gated copy path.
    bool Offer(const webrtc::VideoFrame&, RenderFrameMetadata,
               const std::shared_ptr<telemetry::VideoActivityProbe>&);
    // Native cleanup only, after RemoveSink has drained callback ingress.
    void Close();
private:
    friend class RemoteVideoFramePipeline;
    std::weak_ptr<LatestVideoFrameCore> core_;
    LatestVideoFrameCore::BindingToken binding_;
    std::shared_ptr<std::atomic<bool>> active_ = std::make_shared<std::atomic<bool>>(true);
    std::function<bool()> valid_;
    std::mutex mutex_;
    std::vector<std::weak_ptr<RemoteVideoFrameConsumer>> consumers_;
    std::atomic<bool> used_retained_{false};
    std::atomic<uint64_t> sequence_{0};
};
class RemoteVideoFramePipeline final {
public:
    RemoteVideoFramePipeline();
    ~RemoteVideoFramePipeline();
    std::shared_ptr<RemoteVideoFrameEndpoint> Bind(std::function<bool()> valid);
    LatestVideoFrameCore::Statistics statistics() const { return core_->statistics(); }
private:
    std::shared_ptr<LatestVideoFrameCore> core_;
    LatestVideoFrameWorker worker_;
};
}
