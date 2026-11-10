#include "remote_video_frame_pipeline.h"
#include "telemetry/session_telemetry.h"
#include <algorithm>

namespace livekit::render {
namespace {
class ObservedInput final : public FrameInput {
public:
    ObservedInput(RetainedVideoFrame::Ptr input, std::function<bool()> valid,
                  std::shared_ptr<telemetry::VideoActivityProbe> probe)
        : input_(std::move(input)), valid_(std::move(valid)), probe_(std::move(probe)) {}
    size_t RetainedBytes() const noexcept override { return input_->RetainedBytes(); }
    OwnedI420Frame::Ptr Materialize() const override {
        if (valid_ && !valid_()) return {};
        probe_->copy_attempts.fetch_add(1, std::memory_order_relaxed);
        auto frame = input_->Materialize();
        if (!frame) probe_->copy_failures.fetch_add(1, std::memory_order_relaxed);
        else {
            probe_->delivered_frame_count.fetch_add(1, std::memory_order_relaxed);
            probe_->copied_bytes.fetch_add(uint64_t(frame->width()) * frame->height() +
                2 * uint64_t(frame->chroma_width()) * frame->chroma_height(), std::memory_order_relaxed);
        }
        return frame;
    }
private:
    RetainedVideoFrame::Ptr input_;
    std::function<bool()> valid_;
    std::shared_ptr<telemetry::VideoActivityProbe> probe_;
};
}
RemoteVideoFrameConsumer::~RemoteVideoFrameConsumer() { Retire(); }
bool RemoteVideoFrameConsumer::active() const {
    return active_.load(std::memory_order_acquire) && binding_active_ &&
        binding_active_->load(std::memory_order_acquire) && !core_.expired();
}
void RemoteVideoFrameConsumer::Retire() {
    if (active_.exchange(false, std::memory_order_acq_rel))
        if (const auto core = core_.lock()) core->RetireConsumer(token_);
}
void RemoteVideoFrameConsumer::SetMaxFps(uint32_t fps) {
    if (active()) if (const auto core = core_.lock()) core->UpdateConsumerFps(token_, fps);
}
OwnedI420Frame::Ptr RemoteVideoFrameConsumer::Take() {
    if (!active() || (valid_ && !valid_())) return {};
    const auto core = core_.lock();
    if (!core) return {};
    const auto ready = core->TakeReadyAndRequest(token_, LatestVideoFrameCore::Clock::now());
    return ready && active() && (!valid_ || valid_()) ? ready->frame : OwnedI420Frame::Ptr{};
}
std::shared_ptr<RemoteVideoFrameConsumer> RemoteVideoFrameEndpoint::Subscribe(
        uint32_t fps, std::function<bool()> valid) {
    const auto core = core_.lock();
    if (!core || !active_->load(std::memory_order_acquire)) return {};
    auto consumer = std::make_shared<RemoteVideoFrameConsumer>();
    const auto token = core->AddConsumer(binding_, fps, LatestVideoFrameCore::Clock::now());
    if (!token) return {};
    consumer->core_ = core;
    consumer->token_ = *token;
    consumer->binding_active_ = active_;
    consumer->valid_ = std::move(valid);
    std::lock_guard lock(mutex_);
    std::erase_if(consumers_, [](const auto& entry) { return entry.expired(); });
    consumers_.push_back(consumer);
    return consumer;
}
bool RemoteVideoFrameEndpoint::Offer(const webrtc::VideoFrame& frame, RenderFrameMetadata metadata,
        const std::shared_ptr<telemetry::VideoActivityProbe>& probe) {
    if (!active_->load(std::memory_order_acquire) || (valid_ && !valid_())) return false;
    auto retained = RetainedVideoFrame::TryRetain(frame, std::move(metadata));
    if (!retained) {
        // Codec/source changes must not let a delayed VP8 result overwrite a
        // newer fallback frame. Re-enable only with a new native media binding.
        if (used_retained_.load(std::memory_order_acquire)) Close();
        return false;
    }
    const auto core = core_.lock();
    if (!core) return false;
    std::vector<std::shared_ptr<RemoteVideoFrameConsumer>> consumers;
    {
        std::lock_guard lock(mutex_);
        for (const auto& entry : consumers_) if (auto value = entry.lock()) consumers.push_back(std::move(value));
    }
    bool demand = false;
    for (const auto& consumer : consumers) {
        if (!consumer->active()) continue;
        if (consumer->valid_ && !consumer->valid_()) { consumer->Retire(); continue; }
        demand = true;
    }
    if (!demand) return false;
    used_retained_.store(true, std::memory_order_release);
    const auto active = active_;
    const auto valid = valid_;
    auto input = std::make_shared<ObservedInput>(std::move(retained), [active, valid] {
        return active->load(std::memory_order_acquire) && (!valid || valid());
    }, probe);
    const auto result = core->Offer(binding_, input, sequence_.fetch_add(1, std::memory_order_relaxed) + 1);
    if (result == LatestVideoFrameCore::OfferResult::Closed ||
        result == LatestVideoFrameCore::OfferResult::InvalidBinding) {
        Close();
        return false; // Scheduler failure must preserve the synchronous path.
    }
    if (result == LatestVideoFrameCore::OfferResult::NoDemand) return false;
    if (result == LatestVideoFrameCore::OfferResult::Accepted)
        probe->retained_submitted.fetch_add(1, std::memory_order_relaxed);
    else {
        probe->precopy_skipped.fetch_add(1, std::memory_order_relaxed);
        if (result == LatestVideoFrameCore::OfferResult::CapacityExceeded)
            probe->retained_capacity_rejected.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}
void RemoteVideoFrameEndpoint::Close() {
    if (active_->exchange(false, std::memory_order_acq_rel))
        if (const auto core = core_.lock()) core->RemoveBinding(binding_);
}
RemoteVideoFramePipeline::RemoteVideoFramePipeline()
    : core_(std::make_shared<LatestVideoFrameCore>()), worker_(core_) {}
RemoteVideoFramePipeline::~RemoteVideoFramePipeline() { worker_.StopAndDrain(); }
std::shared_ptr<RemoteVideoFrameEndpoint> RemoteVideoFramePipeline::Bind(std::function<bool()> valid) {
    auto endpoint = std::make_shared<RemoteVideoFrameEndpoint>();
    const auto token = core_->AddBinding();
    if (!token) return {};
    endpoint->core_ = core_;
    endpoint->binding_ = *token;
    endpoint->valid_ = std::move(valid);
    return endpoint;
}
}
