#pragma once
#include "media_diagnostic_config.h"

#if COHAVORA_ENABLE_MEDIA_DIAGNOSTICS

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include "api/frame_transformer_interface.h"

namespace livekit::telemetry {

// Assembled-frame metadata only: no payload, participant IDs, addresses or SDP.
struct ReceivedFrameDiagnosticRecord {
    static constexpr std::size_t kMaxDependencies = 8;
    uint64_t sequence = 0;
    int64_t steady_ns = 0;
    uint32_t ssrc = 0, rtp_timestamp = 0, encoded_size = 0;
    int codec = 0;
    uint16_t width = 0, height = 0;
    bool keyframe = false, generic_present = false, dependencies_truncated = false;
    int64_t frame_id = 0;
    int spatial_index = 0, temporal_index = 0;
    std::array<int64_t, kMaxDependencies> dependencies{};
    std::size_t dependency_count = 0;
    bool vp8_present = false, vp8_layer_sync = false, vp8_non_reference = false;
    int vp8_picture_id = -1, vp8_tl0_pic_idx = -1, vp8_temporal_idx = -1;
};

class ReceivedFrameDiagnostic final {
public:
    static constexpr std::size_t kCapacity = 64;
    struct Snapshot {
        std::array<ReceivedFrameDiagnosticRecord, kCapacity> records{};
        std::size_t size = 0;
        uint64_t assembled = 0, keyframes = 0, forwarded = 0, no_callback = 0;
        uint64_t overwritten = 0, contention = 0;
        int64_t last_assembled_ns = 0;
        bool retired = false;
    };

    explicit ReceivedFrameDiagnostic(std::shared_ptr<const std::atomic<bool>> binding_active)
        : binding_active_(std::move(binding_active)) {}

    // Admission may complete concurrently with retirement, just like a native
    // sink callback. Every later admission is rejected; forwarding is unaffected.
    bool Observe(const webrtc::TransformableVideoFrameInterface& frame) noexcept {
        if (retired_.load(std::memory_order_acquire) ||
            !binding_active_ || !binding_active_->load(std::memory_order_acquire)) return false;
        ReceivedFrameDiagnosticRecord record;
        record.steady_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        bool admitted = false;
        const auto admit = [&] {
            last_assembled_ns_.store(record.steady_ns, std::memory_order_relaxed);
            if (record.keyframe) keyframes_.fetch_add(1, std::memory_order_relaxed);
            record.sequence = assembled_.fetch_add(1, std::memory_order_release) + 1;
            admitted = true;
        };
        try {
            record.ssrc = frame.GetSsrc();
            record.rtp_timestamp = frame.GetTimestamp();
            record.encoded_size = static_cast<uint32_t>(frame.GetData().size());
            record.keyframe = frame.IsKeyFrame();
            // Use the existing header by reference; never call SetMetadata/SetData
            // or construct a replacement frame in the diagnostic hook.
            const auto& header = frame.header();
            record.codec = static_cast<int>(header.codec);
            record.width = header.width;
            record.height = header.height;
            if (header.generic) {
                record.generic_present = true;
                record.frame_id = header.generic->frame_id;
                record.spatial_index = header.generic->spatial_index;
                record.temporal_index = header.generic->temporal_index;
                record.dependency_count = std::min(header.generic->dependencies.size(),
                    record.dependencies.size());
                record.dependencies_truncated = header.generic->dependencies.size() > record.dependency_count;
                std::copy_n(header.generic->dependencies.begin(), record.dependency_count,
                    record.dependencies.begin());
            }
            if (const auto* vp8 = std::get_if<webrtc::RTPVideoHeaderVP8>(&header.video_type_header)) {
                record.vp8_present = true;
                record.vp8_picture_id = vp8->pictureId;
                record.vp8_tl0_pic_idx = vp8->tl0PicIdx;
                record.vp8_temporal_idx = vp8->temporalIdx == webrtc::kNoTemporalIdx ? -1 : vp8->temporalIdx;
                record.vp8_layer_sync = vp8->layerSync;
                record.vp8_non_reference = vp8->nonReference;
            }
            std::unique_lock lock(mutex_, std::try_to_lock);
            if (!lock.owns_lock()) {
                contention_.fetch_add(1, std::memory_order_relaxed);
                admit();
                return true;
            }
            // Successful metadata and its sequence/count share the snapshot
            // lock. A reader cannot announce N while record N is still pending.
            admit();
            if (size_ == kCapacity) {
                head_ = (head_ + 1) % kCapacity;
                --size_;
                // Normal tail eviction after a snapshot is not lost metadata.
                if (since_snapshot_ >= kCapacity) overwritten_.fetch_add(1, std::memory_order_relaxed);
            }
            records_[(head_ + size_) % kCapacity] = record;
            ++size_;
            ++since_snapshot_;
        } catch (...) {
            contention_.fetch_add(1, std::memory_order_relaxed);
            if (!admitted) admit();
        }
        return true;
    }

    void Forwarded() noexcept { forwarded_.fetch_add(1, std::memory_order_relaxed); }
    void NoCallback() noexcept { no_callback_.fetch_add(1, std::memory_order_relaxed); }
    void Retire() noexcept { retired_.store(true, std::memory_order_release); }

    // Fixture/owner thread only. Preserve the latest tail across status writes:
    // an observer may not read every status. Readers deduplicate by sequence.
    // More than 64 admissions between snapshots or any contention is explicit.
    Snapshot Read() {
        Snapshot result;
        {
            std::lock_guard lock(mutex_);
            result.size = size_;
            for (std::size_t i = 0; i < size_; ++i)
                result.records[i] = records_[(head_ + i) % kCapacity];
            since_snapshot_ = 0;
            result.assembled = assembled_.load(std::memory_order_acquire);
            result.keyframes = keyframes_.load(std::memory_order_relaxed);
            result.forwarded = forwarded_.load(std::memory_order_relaxed);
            result.no_callback = no_callback_.load(std::memory_order_relaxed);
            result.overwritten = overwritten_.load(std::memory_order_relaxed);
            result.contention = contention_.load(std::memory_order_relaxed);
            result.last_assembled_ns = last_assembled_ns_.load(std::memory_order_relaxed);
            result.retired = retired_.load(std::memory_order_acquire);
        }
        return result;
    }

private:
    const std::shared_ptr<const std::atomic<bool>> binding_active_;
    std::atomic<bool> retired_{false};
    std::atomic<uint64_t> assembled_{0}, keyframes_{0}, forwarded_{0}, no_callback_{0};
    std::atomic<uint64_t> overwritten_{0}, contention_{0};
    std::atomic<int64_t> last_assembled_ns_{0};
    std::mutex mutex_;
    std::array<ReceivedFrameDiagnosticRecord, kCapacity> records_{};
    std::size_t head_ = 0, size_ = 0, since_snapshot_ = 0;
};

// Installed only on explicitly plaintext video receivers. Retirement disables
// observation, not delivery. The receiver owns the hook until normal teardown.
class ReceivedFrameDiagnosticPassthrough : public webrtc::FrameTransformerInterface {
public:
    explicit ReceivedFrameDiagnosticPassthrough(std::shared_ptr<ReceivedFrameDiagnostic> probe)
        : probe_(std::move(probe)) {}

    void Transform(std::unique_ptr<webrtc::TransformableFrameInterface> frame) override {
        if (!frame) return;
        webrtc::scoped_refptr<webrtc::TransformedFrameCallback> callback;
        {
            std::lock_guard lock(callback_mutex_);
            const auto sink = sinks_.find(frame->GetSsrc());
            callback = sink != sinks_.end() ? sink->second : callback_;
        }
        // This hook is registered only for video, and WebRTC supplies its
        // TransformableVideoReceiverFrame here. Preserve that exact object.
        const bool observed = frame->GetDirection() == webrtc::TransformableFrameInterface::Direction::kReceiver &&
            probe_->Observe(static_cast<const webrtc::TransformableVideoFrameInterface&>(*frame));
        if (!callback) {
            if (observed) probe_->NoCallback();
            return;
        }
        callback->OnTransformedFrame(std::move(frame));
        if (observed) probe_->Forwarded();
    }

    void RegisterTransformedFrameCallback(webrtc::scoped_refptr<webrtc::TransformedFrameCallback> callback) override {
        std::lock_guard lock(callback_mutex_);
        callback_ = std::move(callback);
    }
    void RegisterTransformedFrameSinkCallback(webrtc::scoped_refptr<webrtc::TransformedFrameCallback> callback,
                                             uint32_t ssrc) override {
        std::lock_guard lock(callback_mutex_);
        sinks_[ssrc] = std::move(callback);
    }
    void UnregisterTransformedFrameCallback() override {
        std::lock_guard lock(callback_mutex_);
        callback_ = nullptr;
    }
    void UnregisterTransformedFrameSinkCallback(uint32_t ssrc) override {
        std::lock_guard lock(callback_mutex_);
        sinks_.erase(ssrc);
    }

private:
    const std::shared_ptr<ReceivedFrameDiagnostic> probe_;
    std::mutex callback_mutex_;
    webrtc::scoped_refptr<webrtc::TransformedFrameCallback> callback_;
    std::map<uint32_t, webrtc::scoped_refptr<webrtc::TransformedFrameCallback>> sinks_;
};

} // namespace livekit::telemetry

#endif
