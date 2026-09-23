#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <chrono>

namespace livekit {

/// @brief 基础 WebRTC Stats 数据结构
struct RtcStatsData {
    std::string id;
    std::int64_t timestamp_ms{0};
};

/// @brief 下行/接收 RTP 媒体流统计 (Inbound RTP)
struct InboundRtpStreamStats {
    std::string id;
    std::string kind; // "audio" or "video"
    std::string ssrc;
    std::uint64_t bytes_received{0};
    std::uint64_t packets_received{0};
    std::int64_t packets_lost{0};
    double jitter{0.0};
    std::uint32_t frames_decoded{0};
    std::uint32_t frames_dropped{0};
    std::uint32_t frame_width{0};
    std::uint32_t frame_height{0};
    double frames_per_second{0.0};

    // A missing native member is not a measured zero.
    bool kind_available{false};
    bool frames_decoded_available{false};
    bool frames_dropped_available{false};
    bool frame_width_available{false};
    bool frame_height_available{false};
    bool frames_per_second_available{false};
    std::uint32_t freeze_count{0};
    double total_freezes_duration{0.0};
    double total_decode_time{0.0};
    bool freeze_count_available{false};
    bool total_freezes_duration_available{false};
    bool total_decode_time_available{false};

    // Audio-only cumulative counters. Consumers must take deltas per stats ID
    // before calculating ratios or average jitter-buffer delay.
    std::uint64_t total_samples_received{0};
    std::uint64_t concealed_samples{0};
    std::uint64_t silent_concealed_samples{0};
    std::uint64_t concealment_events{0};
    std::uint64_t inserted_samples_for_deceleration{0};
    std::uint64_t removed_samples_for_acceleration{0};
    double jitter_buffer_delay{0.0};
    double jitter_buffer_target_delay{0.0};
    double jitter_buffer_minimum_delay{0.0};
    std::uint64_t jitter_buffer_emitted_count{0};
    double audio_level{0.0};
    bool total_samples_received_available{false};
    bool concealed_samples_available{false};
    bool silent_concealed_samples_available{false};
    bool concealment_events_available{false};
    bool inserted_samples_for_deceleration_available{false};
    bool removed_samples_for_acceleration_available{false};
    bool jitter_buffer_delay_available{false};
    bool jitter_buffer_target_delay_available{false};
    bool jitter_buffer_minimum_delay_available{false};
    bool jitter_buffer_emitted_count_available{false};
    bool audio_level_available{false};
};

struct AudioPlayoutStats {
    std::string id;
    std::uint64_t synthesized_samples_events{0};
    double synthesized_samples_duration{0.0};
    double total_playout_delay{0.0};
    std::uint64_t total_samples_count{0};
    bool synthesized_samples_events_available{false};
    bool synthesized_samples_duration_available{false};
    bool total_playout_delay_available{false};
    bool total_samples_count_available{false};
};

/// @brief 上行/发送 RTP 媒体流统计 (Outbound RTP)
struct OutboundRtpStreamStats {
    std::string id;
    std::string kind; // "audio" or "video"
    std::string ssrc;
    std::uint64_t bytes_sent{0};
    std::uint64_t packets_sent{0};
    std::uint32_t frames_encoded{0};
    double frames_per_second{0.0};

    // Preserve the numeric defaults for existing consumers, but diagnostic
    // consumers must check availability before interpreting a value as zero.
    bool kind_available{false};
    bool bytes_sent_available{false};
    bool packets_sent_available{false};
    bool frames_encoded_available{false};
    std::string mid;
    std::string rid;
    bool mid_available{false};
    bool rid_available{false};
};

/// @brief 远端接收端反馈的 RTCP 统计 (Remote Inbound RTP)
struct RemoteInboundRtpStreamStats {
    std::string id;
    std::string ssrc;
    double round_trip_time{0.0}; // RTT 单位：秒
    double fraction_lost{0.0};   // 丢包百分比 (0.0 - 1.0)
};

/// @brief ICE 候选者对与网络连接质量统计 (Candidate Pair)
struct CandidatePairStats {
    std::string id;
    std::string state;
    bool current_pair{false};
    double current_round_trip_time{0.0}; // RTT 单位：秒
    double available_outgoing_bitrate{0.0}; // 可用上行估计码率 (bps)
    double available_incoming_bitrate{0.0}; // 可用下行估计码率 (bps)
};

// Plain-data sender state captured on WebRTC's signaling thread. No native
// track, transceiver or sender reference crosses into the room executor.
struct RtpSenderDiagnostic {
    std::string track_id;
    std::string kind;
    std::string mid;
    std::string direction;
    std::string current_direction;
    bool mid_available{false};
    bool current_direction_available{false};
    bool track_enabled{false};
    // An empty encoding list means the encoder state is not yet available.
    std::size_t encoding_count{0};
    std::size_t active_encoding_count{0};
};

/// @brief 综合 WebRTC Stats 报表单据
struct StatsReport {
    std::int64_t timestamp_ms{0};
    std::vector<InboundRtpStreamStats> inbound_rtp;
    std::vector<OutboundRtpStreamStats> outbound_rtp;
    std::vector<RemoteInboundRtpStreamStats> remote_inbound_rtp;
    std::vector<CandidatePairStats> candidate_pairs;
    std::vector<AudioPlayoutStats> audio_playout;
    std::vector<RtpSenderDiagnostic> senders;
    bool senders_available{false};
};

/// @brief LiveKit 房间级汇总统计快照 (RoomStatsReport)
struct RoomStatsReport {
    std::int64_t timestamp_ms{0};
    double publisher_rtt_ms{0.0};
    double subscriber_rtt_ms{0.0};
    std::uint64_t total_bytes_sent{0};
    std::uint64_t total_bytes_received{0};
    double available_outgoing_bitrate{0.0};
    std::vector<StatsReport> reports;
    // Counts physical PeerConnection instances, not publisher/subscriber
    // logical roles. In Single-PC mode publisher and subscriber count once.
    std::uint32_t actual_peer_connection_count{0};
    std::uint32_t successful_peer_connection_count{0};
    std::uint32_t timed_out_peer_connection_count{0};
    std::uint32_t rejected_peer_connection_count{0};
};

} // namespace livekit
