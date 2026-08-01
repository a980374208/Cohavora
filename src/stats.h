#pragma once

#include <cstdint>
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

/// @brief 综合 WebRTC Stats 报表单据
struct StatsReport {
    std::int64_t timestamp_ms{0};
    std::vector<InboundRtpStreamStats> inbound_rtp;
    std::vector<OutboundRtpStreamStats> outbound_rtp;
    std::vector<RemoteInboundRtpStreamStats> remote_inbound_rtp;
    std::vector<CandidatePairStats> candidate_pairs;
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
};

} // namespace livekit
