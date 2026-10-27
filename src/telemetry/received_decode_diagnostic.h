#pragma once
#include "media_diagnostic_config.h"

#if COHAVORA_ENABLE_MEDIA_DIAGNOSTICS

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <new>
#include <vector>
#include "api/video_codecs/video_decoder.h"
#include "rtc_base/time_utils.h"

namespace livekit::telemetry {

// Metadata only. Packet receive timestamps belong to WebRTC's clock; the
// bracketed rtc_now_us/steady_ns pair explicitly relates it to the probe clock.
struct ReceivedDecodeDiagnosticRecord {
    enum Event : uint8_t { Configure = 1, Input = 2, Return = 3, Output = 4, Release = 5 };
    uint64_t sequence = 0, decoder_id = 0, epoch = 0, decode_call_id = 0;
    Event event = Input;
    int64_t steady_ns = 0, rtc_now_us = 0, clock_uncertainty_ns = 0;
    uint32_t rtp_timestamp = 0, ssrc = 0;
    bool ssrc_known = false;
    int64_t render_time_ms = 0;
    int32_t return_code = 0;
    bool return_code_known = false;
    uint8_t decode_overload = 0, callback_overload = 0;
    uint64_t packet_count = 0;
    bool packet_times_available = false, packet_metadata_truncated = false;
    bool packet_rtp_mismatch = false;
    int64_t first_received_us = 0, last_received_us = 0;
};

inline bool ReceivedDecodeDiagnosticsEnabled() noexcept {
    // Factory/fixture path only. The normal product build compiles this out.
    const char* value = std::getenv("COHAVORA_RECEIVED_DECODE_DIAGNOSTIC");
    return value && value[0] == '1' && value[1] == '\0';
}

class ReceivedDecodeDiagnostic final {
public:
    static constexpr std::size_t kCapacity = 256;
    static constexpr std::size_t kMaxPacketInfos = 4096;
    struct Snapshot {
        uint64_t decoder_id = 0, epoch = 0;
        bool released = false, destroyed = false;
        uint64_t configure_calls = 0, decode_calls = 0, decode_returns = 0;
        uint64_t raw_outputs = 0, no_callback_outputs = 0;
        uint64_t metadata_overwritten = 0, metadata_contention = 0;
        uint64_t ssrc_unknown = 0, ssrc_ambiguous = 0;
        uint64_t ambiguous_outputs = 0, unmatched_outputs = 0, pending_mapping_overwritten = 0;
        std::array<ReceivedDecodeDiagnosticRecord, kCapacity> records{};
        std::size_t size = 0;
    };

    explicit ReceivedDecodeDiagnostic(uint64_t id) : decoder_id_(id) {}

    uint64_t BeginConfigure() noexcept {
        configure_calls_.fetch_add(1, std::memory_order_relaxed);
        released_.store(false, std::memory_order_relaxed);
        return epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
    }
    void Configured(uint64_t epoch, bool success) noexcept {
        auto record = NewRecord(ReceivedDecodeDiagnosticRecord::Configure);
        record.epoch = epoch;
        record.return_code = success ? 1 : 0;
        record.return_code_known = true;
        Append(record);
    }

    ReceivedDecodeDiagnosticRecord Input(const webrtc::EncodedImage& image,
            int64_t render_time_ms, uint8_t overload) noexcept {
        auto record = NewRecord(ReceivedDecodeDiagnosticRecord::Input);
        record.decode_call_id = decode_calls_.fetch_add(1, std::memory_order_relaxed) + 1;
        record.rtp_timestamp = image.RtpTimestamp();
        record.render_time_ms = render_time_ms;
        record.decode_overload = overload;
        const auto before = NowNs();
        record.rtc_now_us = webrtc::TimeMicros();
        const auto after = NowNs();
        record.steady_ns = before + (after - before) / 2;
        record.clock_uncertainty_ns = (after - before + 1) / 2;
        const auto& packets = image.PacketInfos();
        record.packet_count = packets.size();
        record.packet_metadata_truncated = packets.size() > kMaxPacketInfos;
        bool times = !packets.empty() && !record.packet_metadata_truncated;
        bool ssrc = !packets.empty() && !record.packet_metadata_truncated;
        bool ambiguous = false, first_time = true;
        const auto count = std::min(packets.size(), kMaxPacketInfos);
        for (std::size_t i = 0; i < count; ++i) {
            const auto& packet = packets[i];
            if (i == 0) record.ssrc = packet.ssrc();
            else if (record.ssrc != packet.ssrc()) ambiguous = true;
            if (packet.ssrc() == 0) ssrc = false;
            if (packet.rtp_timestamp() != record.rtp_timestamp) record.packet_rtp_mismatch = true;
            if (!packet.receive_time().IsFinite()) { times = false; continue; }
            const auto time = packet.receive_time().us();
            if (first_time) record.first_received_us = record.last_received_us = time;
            else {
                record.first_received_us = std::min(record.first_received_us, time);
                record.last_received_us = std::max(record.last_received_us, time);
            }
            first_time = false;
        }
        record.ssrc_known = ssrc && !ambiguous && !record.packet_rtp_mismatch;
        record.packet_times_available = times && !first_time && !record.packet_rtp_mismatch;
        if (!record.ssrc_known) ssrc_unknown_.fetch_add(1, std::memory_order_relaxed);
        if (ambiguous) ssrc_ambiguous_.fetch_add(1, std::memory_order_relaxed);
        try {
            std::unique_lock lock(mutex_, std::try_to_lock);
            if (!lock.owns_lock()) {
                metadata_contention_.fetch_add(1, std::memory_order_relaxed);
                return record;
            }
            auto& pending = pending_[pending_head_];
            if (pending.call_id && !pending.output_seen)
                pending_mapping_overwritten_.fetch_add(1, std::memory_order_relaxed);
            pending = {record.decode_call_id, record.epoch, record.rtp_timestamp,
                record.ssrc, record.ssrc_known, record.render_time_ms, false};
            pending_head_ = (pending_head_ + 1) % kCapacity;
            AppendLocked(record);
        } catch (...) {
            metadata_contention_.fetch_add(1, std::memory_order_relaxed);
        }
        return record;
    }

    void Returned(const ReceivedDecodeDiagnosticRecord& input, int32_t result) noexcept {
        decode_returns_.fetch_add(1, std::memory_order_relaxed);
        auto record = NewRecord(ReceivedDecodeDiagnosticRecord::Return);
        record.epoch = input.epoch;
        record.decode_call_id = input.decode_call_id;
        record.rtp_timestamp = input.rtp_timestamp;
        record.ssrc = input.ssrc;
        record.ssrc_known = input.ssrc_known;
        record.render_time_ms = input.render_time_ms;
        record.decode_overload = input.decode_overload;
        record.return_code = result;
        record.return_code_known = true;
        Append(record);
    }

    void Output(const webrtc::VideoFrame& frame, uint8_t overload, bool has_callback) noexcept {
        raw_outputs_.fetch_add(1, std::memory_order_relaxed);
        if (!has_callback) no_callback_outputs_.fetch_add(1, std::memory_order_relaxed);
        auto record = NewRecord(ReceivedDecodeDiagnosticRecord::Output);
        record.rtp_timestamp = frame.rtp_timestamp();
        record.callback_overload = overload;
        try {
            std::unique_lock lock(mutex_, std::try_to_lock);
            if (!lock.owns_lock()) {
                metadata_contention_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            Pending* match = nullptr;
            std::size_t matches = 0;
            for (auto& pending : pending_) {
                if (pending.call_id && pending.rtp_timestamp == record.rtp_timestamp) {
                    ++matches;
                    match = &pending;
                }
            }
            if (matches == 1) {
                record.decode_call_id = match->call_id;
                record.epoch = match->epoch;
                record.ssrc = match->ssrc;
                record.ssrc_known = match->ssrc_known;
                record.render_time_ms = match->render_time_ms;
                match->output_seen = true;
            } else if (matches == 0) {
                unmatched_outputs_.fetch_add(1, std::memory_order_relaxed);
            } else {
                ambiguous_outputs_.fetch_add(1, std::memory_order_relaxed);
            }
            AppendLocked(record);
        } catch (...) {
            metadata_contention_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void Released(int32_t result) noexcept {
        auto record = NewRecord(ReceivedDecodeDiagnosticRecord::Release);
        record.return_code = result;
        record.return_code_known = true;
        Append(record);
        released_.store(result == 0, std::memory_order_release);
        // New callbacks without a unique retained input cannot silently inherit
        // an old decoder epoch. Matched late output retains its input epoch.
        epoch_.fetch_add(1, std::memory_order_acq_rel);
    }
    void Destroyed() noexcept { destroyed_.store(true, std::memory_order_release); }

    Snapshot Read() {
        Snapshot value;
        std::lock_guard lock(mutex_);
        value.decoder_id = decoder_id_;
        value.epoch = epoch_.load(std::memory_order_acquire);
        value.released = released_.load(std::memory_order_acquire);
        value.destroyed = destroyed_.load(std::memory_order_acquire);
        value.configure_calls = configure_calls_.load(std::memory_order_relaxed);
        value.decode_calls = decode_calls_.load(std::memory_order_relaxed);
        value.decode_returns = decode_returns_.load(std::memory_order_relaxed);
        value.raw_outputs = raw_outputs_.load(std::memory_order_relaxed);
        value.no_callback_outputs = no_callback_outputs_.load(std::memory_order_relaxed);
        value.metadata_overwritten = metadata_overwritten_.load(std::memory_order_relaxed);
        value.metadata_contention = metadata_contention_.load(std::memory_order_relaxed);
        value.ssrc_unknown = ssrc_unknown_.load(std::memory_order_relaxed);
        value.ssrc_ambiguous = ssrc_ambiguous_.load(std::memory_order_relaxed);
        value.ambiguous_outputs = ambiguous_outputs_.load(std::memory_order_relaxed);
        value.unmatched_outputs = unmatched_outputs_.load(std::memory_order_relaxed);
        value.pending_mapping_overwritten = pending_mapping_overwritten_.load(std::memory_order_relaxed);
        value.size = size_;
        for (std::size_t i = 0; i < size_; ++i) value.records[i] = records_[(head_ + i) % kCapacity];
        since_snapshot_ = 0;
        return value;
    }

private:
    struct Pending {
        uint64_t call_id = 0, epoch = 0;
        uint32_t rtp_timestamp = 0, ssrc = 0;
        bool ssrc_known = false;
        int64_t render_time_ms = 0;
        bool output_seen = false;
    };
    static int64_t NowNs() noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    ReceivedDecodeDiagnosticRecord NewRecord(ReceivedDecodeDiagnosticRecord::Event event) const noexcept {
        ReceivedDecodeDiagnosticRecord value;
        value.decoder_id = decoder_id_;
        value.epoch = epoch_.load(std::memory_order_acquire);
        value.event = event;
        value.steady_ns = NowNs();
        return value;
    }
    void Append(ReceivedDecodeDiagnosticRecord value) noexcept {
        try {
            std::unique_lock lock(mutex_, std::try_to_lock);
            if (lock.owns_lock()) AppendLocked(value);
            else metadata_contention_.fetch_add(1, std::memory_order_relaxed);
        } catch (...) {
            metadata_contention_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void AppendLocked(ReceivedDecodeDiagnosticRecord value) noexcept {
        value.sequence = ++sequence_;
        if (size_ == kCapacity) {
            head_ = (head_ + 1) % kCapacity;
            --size_;
            if (since_snapshot_ >= kCapacity) metadata_overwritten_.fetch_add(1, std::memory_order_relaxed);
        }
        records_[(head_ + size_) % kCapacity] = value;
        ++size_;
        ++since_snapshot_;
    }
    const uint64_t decoder_id_;
    std::atomic<uint64_t> epoch_{0}, configure_calls_{0}, decode_calls_{0}, decode_returns_{0};
    std::atomic<uint64_t> raw_outputs_{0}, no_callback_outputs_{0};
    std::atomic<uint64_t> metadata_overwritten_{0}, metadata_contention_{0};
    std::atomic<uint64_t> ssrc_unknown_{0}, ssrc_ambiguous_{0};
    std::atomic<uint64_t> ambiguous_outputs_{0}, unmatched_outputs_{0}, pending_mapping_overwritten_{0};
    std::atomic<bool> released_{false}, destroyed_{false};
    std::mutex mutex_;
    std::array<ReceivedDecodeDiagnosticRecord, kCapacity> records_{};
    std::array<Pending, kCapacity> pending_{};
    std::size_t head_ = 0, size_ = 0, since_snapshot_ = 0, pending_head_ = 0;
    uint64_t sequence_ = 0;
};

struct ReceivedDecodeRegistrySnapshot {
    static constexpr std::size_t kCapacity = 128;
    bool enabled = false;
    std::size_t capacity = kCapacity;
    uint64_t evicted_decoders = 0;
    std::vector<ReceivedDecodeDiagnostic::Snapshot> decoders;
};

class ReceivedDecodeDiagnosticRegistry final {
public:
    static ReceivedDecodeDiagnosticRegistry& Instance() {
        static ReceivedDecodeDiagnosticRegistry value;
        return value;
    }
    std::shared_ptr<ReceivedDecodeDiagnostic> Create() {
        std::lock_guard lock(mutex_);
        auto probe = std::make_shared<ReceivedDecodeDiagnostic>(++next_id_);
        if (decoders_.size() == ReceivedDecodeRegistrySnapshot::kCapacity) {
            decoders_.erase(decoders_.begin());
            ++evicted_decoders_;
        }
        decoders_.push_back(probe);
        return probe;
    }
    ReceivedDecodeRegistrySnapshot Read() {
        ReceivedDecodeRegistrySnapshot value;
        value.enabled = true;
        std::vector<std::shared_ptr<ReceivedDecodeDiagnostic>> probes;
        {
            std::lock_guard lock(mutex_);
            probes = decoders_;
            value.evicted_decoders = evicted_decoders_;
        }
        value.decoders.reserve(probes.size());
        for (const auto& probe : probes) value.decoders.push_back(probe->Read());
        return value;
    }
private:
    std::mutex mutex_;
    std::vector<std::shared_ptr<ReceivedDecodeDiagnostic>> decoders_;
    uint64_t next_id_ = 0, evicted_decoders_ = 0;
};

inline ReceivedDecodeRegistrySnapshot ReadReceivedDecodeDiagnostics() {
    if (!ReceivedDecodeDiagnosticsEnabled()) return {};
    return ReceivedDecodeDiagnosticRegistry::Instance().Read();
}

// Keep the original decoder's registration, overload selection and callback
// results intact. Its API owner still serializes Configure/Register/Release;
// raw callback lifetime is the same WebRTC contract as without this wrapper.
class ReceivedDecodeDiagnosticDecoder final : public webrtc::VideoDecoder,
                                             private webrtc::DecodedImageCallback {
public:
    ReceivedDecodeDiagnosticDecoder(std::unique_ptr<webrtc::VideoDecoder> inner,
            std::shared_ptr<ReceivedDecodeDiagnostic> probe) noexcept
        : inner_(std::move(inner)), probe_(std::move(probe)) {}
    ~ReceivedDecodeDiagnosticDecoder() override {
        callback_.store(nullptr, std::memory_order_release);
        // Inner destruction may touch its registered bridge. Destroy it while
        // this bridge and every probe/callback member still exist, with no
        // additional Release call introduced by diagnostics.
        inner_.reset();
        probe_->Destroyed();
    }
    bool Configure(const Settings& settings) override {
        const auto epoch = probe_->BeginConfigure();
        const bool result = inner_->Configure(settings);
        probe_->Configured(epoch, result);
        return result;
    }
    int32_t Decode(const webrtc::EncodedImage& image, int64_t render_time_ms) override {
        const auto input = probe_->Input(image, render_time_ms, 1);
        const auto result = inner_->Decode(image, render_time_ms);
        probe_->Returned(input, result);
        return result;
    }
    int32_t Decode(const webrtc::EncodedImage& image, bool missing_frames,
                   int64_t render_time_ms) override {
        const auto input = probe_->Input(image, render_time_ms, 2);
        const auto result = inner_->Decode(image, missing_frames, render_time_ms);
        probe_->Returned(input, result);
        return result;
    }
    int32_t RegisterDecodeCompleteCallback(webrtc::DecodedImageCallback* callback) override {
        const auto prior = callback_.exchange(callback, std::memory_order_acq_rel);
        const auto result = inner_->RegisterDecodeCompleteCallback(callback ? this : nullptr);
        if (result != 0) callback_.store(prior, std::memory_order_release);
        return result;
    }
    int32_t Release() override {
        const auto result = inner_->Release();
        probe_->Released(result);
        return result;
    }
    DecoderInfo GetDecoderInfo() const override { return inner_->GetDecoderInfo(); }
    const char* ImplementationName() const override { return inner_->ImplementationName(); }

private:
    int32_t Decoded(webrtc::VideoFrame& frame) override {
        const auto callback = callback_.load(std::memory_order_acquire);
        probe_->Output(frame, 1, callback != nullptr);
        return callback ? callback->Decoded(frame) : 0;
    }
    int32_t Decoded(webrtc::VideoFrame& frame, int64_t decode_time_ms) override {
        const auto callback = callback_.load(std::memory_order_acquire);
        probe_->Output(frame, 2, callback != nullptr);
        return callback ? callback->Decoded(frame, decode_time_ms) : 0;
    }
    void Decoded(webrtc::VideoFrame& frame, std::optional<int32_t> decode_time_ms,
                 std::optional<uint8_t> qp) override {
        const auto callback = callback_.load(std::memory_order_acquire);
        probe_->Output(frame, 3, callback != nullptr);
        if (callback) callback->Decoded(frame, decode_time_ms, qp);
    }
    std::unique_ptr<webrtc::VideoDecoder> inner_;
    const std::shared_ptr<ReceivedDecodeDiagnostic> probe_;
    std::atomic<webrtc::DecodedImageCallback*> callback_{nullptr};
};

inline std::unique_ptr<webrtc::VideoDecoder> WithReceivedDecodeDiagnostic(
        std::unique_ptr<webrtc::VideoDecoder> inner) {
    if (!inner || !ReceivedDecodeDiagnosticsEnabled()) return inner;
    std::shared_ptr<ReceivedDecodeDiagnostic> probe;
    try { probe = ReceivedDecodeDiagnosticRegistry::Instance().Create(); }
    catch (...) { return inner; }
    // Allocation failure preserves the actual decoder and leaves an explicitly
    // empty registry entry rather than preventing media startup.
    auto wrapper = new (std::nothrow) ReceivedDecodeDiagnosticDecoder(std::move(inner), probe);
    if (!wrapper) { probe->Destroyed(); return inner; }
    return std::unique_ptr<webrtc::VideoDecoder>(wrapper);
}

} // namespace livekit::telemetry
#endif
