#pragma once

#include <memory>
#include <future>
#include <functional>
#include <optional>
#include <asio.hpp>
#include "api/stats/rtc_stats_collector_callback.h"
#include "api/stats/rtc_stats_report.h"
#include "api/peer_connection_interface.h"
#include "stats.h"

#include <mutex>
#include <condition_variable>

namespace livekit {

struct RtcStatsState {
    std::mutex mutex;
    std::condition_variable cv;
    bool done{false};
    StatsReport report;
    std::vector<RtpSenderDiagnostic> senders;
    bool senders_available{false};
    std::function<void(std::optional<StatsReport>)> completion;
};

class RtcStatsCollectorBridge : public webrtc::RTCStatsCollectorCallback {
public:
    static webrtc::scoped_refptr<RtcStatsCollectorBridge> Create(std::shared_ptr<RtcStatsState> state);

    explicit RtcStatsCollectorBridge(std::shared_ptr<RtcStatsState> state)
        : state_(std::move(state)) {}
    ~RtcStatsCollectorBridge() override = default;

    void OnStatsDelivered(const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report) override;

private:
    std::shared_ptr<RtcStatsState> state_;
};

StatsReport ParseRtcStatsReport(const webrtc::RTCStatsReport& report);

// Queue collection using the project's WebRTC task ABI boundary. The manager
// must remain alive until its signaling queue drains. False means no task was
// queued; the caller must complete or skip its wait. An expired state is ignored.
bool RequestRtcStats(
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection,
    std::shared_ptr<RtcStatsState> state);

// Non-blocking bridge for Room::GetStats(). The timeout only completes the
// awaiting coroutine; WebRTC may still deliver and release its callback later.
asio::awaitable<std::optional<StatsReport>> CollectRtcStats(
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection,
    asio::any_io_executor executor,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(1500));

} // namespace livekit
