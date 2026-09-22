#include "stats_collector.h"
#include "webrtc_manager.h"
#include "api/stats/rtcstats_objects.h"
#include <iostream>
#include <type_traits>

namespace livekit {

namespace {

const char* DirectionName(webrtc::RtpTransceiverDirection direction) {
    switch (direction) {
    case webrtc::RtpTransceiverDirection::kSendRecv: return "sendrecv";
    case webrtc::RtpTransceiverDirection::kSendOnly: return "sendonly";
    case webrtc::RtpTransceiverDirection::kRecvOnly: return "recvonly";
    case webrtc::RtpTransceiverDirection::kInactive: return "inactive";
    case webrtc::RtpTransceiverDirection::kStopped: return "stopped";
    }
    return "unknown";
}

std::vector<RtpSenderDiagnostic> CollectSenderDiagnostics(
    webrtc::PeerConnectionInterface& peer) {
    std::vector<RtpSenderDiagnostic> result;
    for (const auto& transceiver : peer.GetTransceivers()) {
        if (!transceiver) continue;
        const auto sender = transceiver->sender();
        if (!sender) continue;
        const auto track = sender->track();
        if (!track) continue;
        RtpSenderDiagnostic item;
        item.track_id = track->id();
        item.kind = track->kind();
        item.track_enabled = track->enabled();
        item.direction = DirectionName(transceiver->direction());
        const auto mid = transceiver->mid();
        item.mid_available = mid.has_value();
        if (mid) item.mid = *mid;
        const auto current_direction = transceiver->current_direction();
        item.current_direction_available = current_direction.has_value();
        if (current_direction) item.current_direction = DirectionName(*current_direction);
        const auto parameters = sender->GetParameters();
        item.encoding_count = parameters.encodings.size();
        for (const auto& encoding : parameters.encodings) {
            if (encoding.active) ++item.active_encoding_count;
        }
        result.push_back(std::move(item));
    }
    return result;
}

} // namespace

webrtc::scoped_refptr<RtcStatsCollectorBridge> RtcStatsCollectorBridge::Create(std::shared_ptr<RtcStatsState> state) {
    return webrtc::make_ref_counted<RtcStatsCollectorBridge>(std::move(state));
}

void RtcStatsCollectorBridge::OnStatsDelivered(const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report) {
    if (!state_) return;

    std::optional<StatsReport> parsed;
    if (report) {
        parsed = ParseRtcStatsReport(*report);
    }

    std::function<void(std::optional<StatsReport>)> completion;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->done) {
            return;
        }
        if (parsed) {
            parsed->senders = state_->senders;
            parsed->senders_available = state_->senders_available;
            state_->report = *parsed;
        }
        state_->done = true;
        completion = std::move(state_->completion);
    }
    state_->cv.notify_all();
    if (completion) {
        completion(std::move(parsed));
    }
}

StatsReport ParseRtcStatsReport(const webrtc::RTCStatsReport& report) {
    StatsReport result;
    result.timestamp_ms = report.timestamp().ms();

    for (const auto& stats : report) {
        if (stats.type() == webrtc::RTCInboundRtpStreamStats::kType) {
            const auto& inbound = static_cast<const webrtc::RTCInboundRtpStreamStats&>(stats);
            InboundRtpStreamStats item;
            item.id = inbound.id();
            if (inbound.kind.has_value()) item.kind = *inbound.kind;
            if (inbound.ssrc.has_value()) item.ssrc = std::to_string(*inbound.ssrc);
            if (inbound.bytes_received.has_value()) item.bytes_received = *inbound.bytes_received;
            if (inbound.packets_received.has_value()) item.packets_received = *inbound.packets_received;
            if (inbound.packets_lost.has_value()) item.packets_lost = *inbound.packets_lost;
            if (inbound.jitter.has_value()) item.jitter = *inbound.jitter;
            if (inbound.frames_decoded.has_value()) item.frames_decoded = *inbound.frames_decoded;
            if (inbound.frames_dropped.has_value()) item.frames_dropped = *inbound.frames_dropped;
            if (inbound.frame_width.has_value()) item.frame_width = *inbound.frame_width;
            if (inbound.frame_height.has_value()) item.frame_height = *inbound.frame_height;
            if (inbound.frames_per_second.has_value()) item.frames_per_second = *inbound.frames_per_second;

            result.inbound_rtp.push_back(item);
        } else if (stats.type() == webrtc::RTCOutboundRtpStreamStats::kType) {
            const auto& outbound = static_cast<const webrtc::RTCOutboundRtpStreamStats&>(stats);
            OutboundRtpStreamStats item;
            item.id = outbound.id();
            item.kind_available = outbound.kind.has_value();
            item.bytes_sent_available = outbound.bytes_sent.has_value();
            item.packets_sent_available = outbound.packets_sent.has_value();
            item.frames_encoded_available = outbound.frames_encoded.has_value();
            item.mid_available = outbound.mid.has_value();
            item.rid_available = outbound.rid.has_value();
            if (item.kind_available) item.kind = *outbound.kind;
            if (outbound.ssrc.has_value()) item.ssrc = std::to_string(*outbound.ssrc);
            if (item.bytes_sent_available) item.bytes_sent = *outbound.bytes_sent;
            if (item.packets_sent_available) item.packets_sent = *outbound.packets_sent;
            if (item.frames_encoded_available) item.frames_encoded = *outbound.frames_encoded;
            if (item.mid_available) item.mid = *outbound.mid;
            if (item.rid_available) item.rid = *outbound.rid;
            if (outbound.frames_per_second.has_value()) item.frames_per_second = *outbound.frames_per_second;

            result.outbound_rtp.push_back(item);
        } else if (stats.type() == webrtc::RTCRemoteInboundRtpStreamStats::kType) {
            const auto& remote_inbound = static_cast<const webrtc::RTCRemoteInboundRtpStreamStats&>(stats);
            RemoteInboundRtpStreamStats item;
            item.id = remote_inbound.id();
            if (remote_inbound.ssrc.has_value()) item.ssrc = std::to_string(*remote_inbound.ssrc);
            if (remote_inbound.round_trip_time.has_value()) item.round_trip_time = *remote_inbound.round_trip_time;
            if (remote_inbound.fraction_lost.has_value()) item.fraction_lost = *remote_inbound.fraction_lost;

            result.remote_inbound_rtp.push_back(item);
        } else if (stats.type() == webrtc::RTCIceCandidatePairStats::kType) {
            const auto& pair = static_cast<const webrtc::RTCIceCandidatePairStats&>(stats);
            CandidatePairStats item;
            item.id = pair.id();
            if (pair.state.has_value()) item.state = *pair.state;
            if (pair.nominated.has_value()) item.current_pair = *pair.nominated;
            if (pair.current_round_trip_time.has_value()) item.current_round_trip_time = *pair.current_round_trip_time;
            if (pair.available_outgoing_bitrate.has_value()) item.available_outgoing_bitrate = *pair.available_outgoing_bitrate;
            if (pair.available_incoming_bitrate.has_value()) item.available_incoming_bitrate = *pair.available_incoming_bitrate;

            result.candidate_pairs.push_back(item);
        }
    }

    return result;
}

bool RequestRtcStats(
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection,
    std::shared_ptr<RtcStatsState> state) {
    auto* signaling = WebRTCManager::Instance().signaling_thread();
    if (!peer_connection || !state || !signaling || signaling->IsQuitting()) {
        return false;
    }

    // The bundled WebRTC and the application's Abseil headers use different
    // AnyInvocable move/dispose operation values. Only a small trivial closure
    // may cross PostTask's ABI boundary: its manager ignores those values.
    // Keep ref-counted ownership in an explicit payload and destroy it here,
    // rather than in the type-erased task manager (as for SDP/media tasks).
    struct StatsTaskParams {
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peer;
        std::shared_ptr<RtcStatsState> state;
    };
    auto* params = new StatsTaskParams{std::move(peer_connection), std::move(state)};
    auto task = [params] {
        std::unique_ptr<StatsTaskParams> owned(params);
        {
            std::lock_guard lock(owned->state->mutex);
            if (owned->state->done) return;
        }
        std::vector<RtpSenderDiagnostic> senders;
        bool senders_available = false;
        try {
            // All native accesses and temporary ref-counted objects stay on
            // signaling; do not hold the completion mutex across native calls.
            senders = CollectSenderDiagnostics(*owned->peer);
            senders_available = true;
        } catch (...) {
            // Optional diagnostics must not change publication or prevent the
            // existing RTP collection when a sender snapshot cannot be read.
        }
        {
            std::lock_guard lock(owned->state->mutex);
            if (owned->state->done) return;
            owned->state->senders = std::move(senders);
            owned->state->senders_available = senders_available;
        }
        auto callback = RtcStatsCollectorBridge::Create(owned->state);
        // WebRTC retains the callback until delivery. The task itself owns no
        // callback reference whose destruction could cross the ABI boundary.
        owned->peer->GetStats(callback.get());
    };
    static_assert(std::is_trivially_copyable_v<decltype(task)>);
    static_assert(sizeof(task) <= 2 * sizeof(void*));
    signaling->PostTask(task);
    return true;
}

asio::awaitable<std::optional<StatsReport>> CollectRtcStats(
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection,
    asio::any_io_executor executor,
    std::chrono::milliseconds timeout) {
    if (!peer_connection) {
        co_return std::nullopt;
    }

    co_return co_await asio::async_initiate<
        decltype(asio::use_awaitable),
        void(std::optional<StatsReport>)>(
        [peer_connection = std::move(peer_connection), executor, timeout](auto handler) mutable {
            using Handler = decltype(handler);
            auto handler_ptr = std::make_shared<Handler>(std::move(handler));
            auto state = std::make_shared<RtcStatsState>();
            auto timer = std::make_shared<asio::steady_timer>(executor, timeout);

            state->completion = [executor, timer, handler_ptr](
                                    std::optional<StatsReport> report) mutable {
                asio::post(executor, [timer, handler_ptr, report = std::move(report)]() mutable {
                    std::error_code ignored;
                    timer->cancel(ignored);
                    (*handler_ptr)(std::move(report));
                });
            };

            timer->async_wait([state, handler_ptr](const std::error_code& error) mutable {
                if (error) {
                    return;
                }
                {
                    std::lock_guard lock(state->mutex);
                    if (state->done) {
                        return;
                    }
                    state->done = true;
                    state->completion = {};
                }
                (*handler_ptr)(std::nullopt);
            });

            if (!RequestRtcStats(std::move(peer_connection), state)) {
                // Use the same single-completion path, including cancellation
                // of the timeout, when the signaling thread is unavailable.
                RtcStatsCollectorBridge::Create(state)->OnStatsDelivered(nullptr);
            }
        },
        asio::use_awaitable);
}

} // namespace livekit
