#pragma once

#include <memory>
#include <future>
#include "api/stats/rtc_stats_collector_callback.h"
#include "api/stats/rtc_stats_report.h"
#include "api/peer_connection_interface.h"
#include "stats.h"

namespace livekit {

class RtcStatsCollectorBridge : public webrtc::RTCStatsCollectorCallback {
public:
    static webrtc::scoped_refptr<RtcStatsCollectorBridge> Create();

    RtcStatsCollectorBridge() = default;
    ~RtcStatsCollectorBridge() override = default;

    void OnStatsDelivered(const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report) override;

    std::future<StatsReport> get_future() { return promise_.get_future(); }

private:
    std::promise<StatsReport> promise_;
};

StatsReport ParseRtcStatsReport(const webrtc::RTCStatsReport& report);

} // namespace livekit
