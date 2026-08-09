#include "stats_collector.h"
#include "api/stats/rtcstats_objects.h"
#include <iostream>

namespace livekit {

webrtc::scoped_refptr<RtcStatsCollectorBridge> RtcStatsCollectorBridge::Create(std::shared_ptr<RtcStatsState> state) {
    return webrtc::make_ref_counted<RtcStatsCollectorBridge>(std::move(state));
}

void RtcStatsCollectorBridge::OnStatsDelivered(const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report) {
    if (!state_) return;
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (report) {
        state_->report = ParseRtcStatsReport(*report);
    }
    state_->done = true;
    state_->cv.notify_all();
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
            if (outbound.kind.has_value()) item.kind = *outbound.kind;
            if (outbound.ssrc.has_value()) item.ssrc = std::to_string(*outbound.ssrc);
            if (outbound.bytes_sent.has_value()) item.bytes_sent = *outbound.bytes_sent;
            if (outbound.packets_sent.has_value()) item.packets_sent = *outbound.packets_sent;
            if (outbound.frames_encoded.has_value()) item.frames_encoded = *outbound.frames_encoded;
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

} // namespace livekit
