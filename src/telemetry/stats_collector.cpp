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

    std::function<void(RtcStatsCollectionResult)> completion;
    RtcStatsLateCompletion late_completion;
    RtcStatsCollectionResult result;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->done) {
            if (state_->timed_out && !state_->late_reported) {
                state_->late_reported = true;
                late_completion = state_->late_completion;
            }
        } else {
            if (parsed) {
                parsed->senders = state_->senders;
                parsed->senders_available = state_->senders_available;
                state_->report = *parsed;
                state_->status = RtcStatsCollectionStatus::Success;
            } else {
                state_->status = RtcStatsCollectionStatus::Failed;
            }
            state_->done = true;
            result.status = state_->status;
            result.report = std::move(parsed);
            completion = std::move(state_->completion);
        }
    }
    state_->cv.notify_all();
    if (completion) {
        completion(std::move(result));
    }
    if (late_completion) {
        late_completion();
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
            item.kind_available = inbound.kind.has_value();
            item.frames_decoded_available = inbound.frames_decoded.has_value();
            item.frames_dropped_available = inbound.frames_dropped.has_value();
            item.frame_width_available = inbound.frame_width.has_value();
            item.frame_height_available = inbound.frame_height.has_value();
            item.frames_per_second_available = inbound.frames_per_second.has_value();
            item.freeze_count_available = inbound.freeze_count.has_value();
            item.total_freezes_duration_available =
                inbound.total_freezes_duration.has_value();
            item.total_decode_time_available = inbound.total_decode_time.has_value();
            item.total_samples_received_available =
                inbound.total_samples_received.has_value();
            item.concealed_samples_available = inbound.concealed_samples.has_value();
            item.silent_concealed_samples_available =
                inbound.silent_concealed_samples.has_value();
            item.concealment_events_available = inbound.concealment_events.has_value();
            item.inserted_samples_for_deceleration_available =
                inbound.inserted_samples_for_deceleration.has_value();
            item.removed_samples_for_acceleration_available =
                inbound.removed_samples_for_acceleration.has_value();
            item.jitter_buffer_delay_available = inbound.jitter_buffer_delay.has_value();
            item.jitter_buffer_target_delay_available =
                inbound.jitter_buffer_target_delay.has_value();
            item.jitter_buffer_minimum_delay_available =
                inbound.jitter_buffer_minimum_delay.has_value();
            item.jitter_buffer_emitted_count_available =
                inbound.jitter_buffer_emitted_count.has_value();
            item.audio_level_available = inbound.audio_level.has_value();
            if (item.kind_available) item.kind = *inbound.kind;
            if (inbound.ssrc.has_value()) item.ssrc = std::to_string(*inbound.ssrc);
            if (inbound.bytes_received.has_value()) item.bytes_received = *inbound.bytes_received;
            if (inbound.packets_received.has_value()) item.packets_received = *inbound.packets_received;
            if (inbound.packets_lost.has_value()) item.packets_lost = *inbound.packets_lost;
            if (inbound.jitter.has_value()) item.jitter = *inbound.jitter;
            if (item.frames_decoded_available) item.frames_decoded = *inbound.frames_decoded;
            if (item.frames_dropped_available) item.frames_dropped = *inbound.frames_dropped;
            if (item.frame_width_available) item.frame_width = *inbound.frame_width;
            if (item.frame_height_available) item.frame_height = *inbound.frame_height;
            if (item.frames_per_second_available) {
                item.frames_per_second = *inbound.frames_per_second;
            }
            if (item.freeze_count_available) item.freeze_count = *inbound.freeze_count;
            if (item.total_freezes_duration_available) {
                item.total_freezes_duration = *inbound.total_freezes_duration;
            }
            if (item.total_decode_time_available) {
                item.total_decode_time = *inbound.total_decode_time;
            }
            if (item.total_samples_received_available) {
                item.total_samples_received = *inbound.total_samples_received;
            }
            if (item.concealed_samples_available) {
                item.concealed_samples = *inbound.concealed_samples;
            }
            if (item.silent_concealed_samples_available) {
                item.silent_concealed_samples = *inbound.silent_concealed_samples;
            }
            if (item.concealment_events_available) {
                item.concealment_events = *inbound.concealment_events;
            }
            if (item.inserted_samples_for_deceleration_available) {
                item.inserted_samples_for_deceleration =
                    *inbound.inserted_samples_for_deceleration;
            }
            if (item.removed_samples_for_acceleration_available) {
                item.removed_samples_for_acceleration =
                    *inbound.removed_samples_for_acceleration;
            }
            if (item.jitter_buffer_delay_available) {
                item.jitter_buffer_delay = *inbound.jitter_buffer_delay;
            }
            if (item.jitter_buffer_target_delay_available) {
                item.jitter_buffer_target_delay = *inbound.jitter_buffer_target_delay;
            }
            if (item.jitter_buffer_minimum_delay_available) {
                item.jitter_buffer_minimum_delay = *inbound.jitter_buffer_minimum_delay;
            }
            if (item.jitter_buffer_emitted_count_available) {
                item.jitter_buffer_emitted_count = *inbound.jitter_buffer_emitted_count;
            }
            if (item.audio_level_available) item.audio_level = *inbound.audio_level;

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
        } else if (stats.type() == webrtc::RTCAudioPlayoutStats::kType) {
            const auto& playout =
                static_cast<const webrtc::RTCAudioPlayoutStats&>(stats);
            AudioPlayoutStats item;
            item.id = playout.id();
            item.synthesized_samples_events_available =
                playout.synthesized_samples_events.has_value();
            item.synthesized_samples_duration_available =
                playout.synthesized_samples_duration.has_value();
            item.total_playout_delay_available =
                playout.total_playout_delay.has_value();
            item.total_samples_count_available =
                playout.total_samples_count.has_value();
            if (item.synthesized_samples_events_available) {
                item.synthesized_samples_events = *playout.synthesized_samples_events;
            }
            if (item.synthesized_samples_duration_available) {
                item.synthesized_samples_duration = *playout.synthesized_samples_duration;
            }
            if (item.total_playout_delay_available) {
                item.total_playout_delay = *playout.total_playout_delay;
            }
            if (item.total_samples_count_available) {
                item.total_samples_count = *playout.total_samples_count;
            }
            result.audio_playout.push_back(std::move(item));
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

asio::awaitable<RtcStatsCollectionResult> CollectRtcStatsDetailed(
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection,
    asio::any_io_executor executor,
    std::chrono::milliseconds timeout,
    RtcStatsLateCompletion late_completion) {
    if (!peer_connection) {
        co_return RtcStatsCollectionResult{
            RtcStatsCollectionStatus::Rejected, std::nullopt};
    }

    co_return co_await asio::async_initiate<
        decltype(asio::use_awaitable),
        void(RtcStatsCollectionResult)>(
        [peer_connection = std::move(peer_connection), executor, timeout,
         late_completion = std::move(late_completion)](auto handler) mutable {
            using Handler = decltype(handler);
            auto handler_ptr = std::make_shared<Handler>(std::move(handler));
            auto state = std::make_shared<RtcStatsState>();
            auto timer = std::make_shared<asio::steady_timer>(executor, timeout);

            if (late_completion) {
                state->late_completion = std::move(late_completion);
            }
            state->completion = [executor, timer, handler_ptr](RtcStatsCollectionResult result) mutable {
                asio::post(executor, [timer, handler_ptr, result = std::move(result)]() mutable {
                    std::error_code ignored;
                    timer->cancel(ignored);
                    (*handler_ptr)(std::move(result));
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
                    state->timed_out = true;
                    state->status = RtcStatsCollectionStatus::Timeout;
                    state->completion = {};
                }
                (*handler_ptr)(RtcStatsCollectionResult{
                    RtcStatsCollectionStatus::Timeout, std::nullopt});
            });

            if (!RequestRtcStats(std::move(peer_connection), state)) {
                std::function<void(RtcStatsCollectionResult)> completion;
                {
                    std::lock_guard lock(state->mutex);
                    if (!state->done) {
                        state->done = true;
                        state->status = RtcStatsCollectionStatus::Rejected;
                        completion = std::move(state->completion);
                    }
                }
                state->cv.notify_all();
                if (completion) {
                    completion(RtcStatsCollectionResult{
                        RtcStatsCollectionStatus::Rejected, std::nullopt});
                }
            }
        },
        asio::use_awaitable);
}

asio::awaitable<std::optional<StatsReport>> CollectRtcStats(
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection,
    asio::any_io_executor executor,
    std::chrono::milliseconds timeout) {
    auto result = co_await CollectRtcStatsDetailed(
        std::move(peer_connection), executor, timeout);
    if (result.status != RtcStatsCollectionStatus::Success) {
        co_return std::nullopt;
    }
    co_return std::move(result.report);
}

} // namespace livekit
