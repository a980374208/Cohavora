// Opt-in S8c L3 harness. Two independent processes connect to a real LiveKit
// service. The publisher emits synthetic I420 markers and the receiver only
// acknowledges markers that reached the render-submit boundary.
#include "core/local_video_track.h"
#include "core/participant.h"
#include "core/remote_track_publication.h"
#include "core/room.h"
#include "render/video_render_frame.h"
#include "rtc/video_frame.h"
#include "rtc/video_source.h"
#include "rtc/webrtc_manager.h"
#include "telemetry/e2e_measurement.h"
#include "telemetry/e2e_media_marker.h"
#include "telemetry/stats.h"

#include <asio.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

enum class Role { Publisher, Receiver };

struct Config {
    Role role = Role::Receiver;
    std::string url;
    std::string token;
    std::string session_id;
    std::string local_peer_id;
    std::string remote_peer_id;
    std::string phase_id = "s8c";
    std::string codec = "vp8";
    std::string expected_codec;
    std::string receiver_publish_codec;
    std::string source = "camera";
    std::string scalability_mode;
    std::string backup_codec;
    livekit::BackupCodecPolicy backup_policy =
        livekit::BackupCodecPolicy::PreferRegression;
    bool auto_backup_codec = false;
    int width = 1280;
    int height = 720;
    bool simulcast = true;
    livekit::proto::VideoQuality quality = livekit::proto::VideoQuality::HIGH;
    std::size_t probes = 10;
    int probe_timeout_ms = 3000;
    int announcement_lead_ms = 150;
    int marker_repeat_frames = 5;
    bool shared_clock_ground_truth = false;
};

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

const char* QualityName(livekit::proto::VideoQuality quality) {
    switch (quality) {
    case livekit::proto::VideoQuality::LOW: return "low";
    case livekit::proto::VideoQuality::MEDIUM: return "medium";
    case livekit::proto::VideoQuality::HIGH: return "high";
    default: return "unknown";
    }
}

const char* BackupPolicyName(livekit::BackupCodecPolicy policy) {
    switch (policy) {
    case livekit::BackupCodecPolicy::PreferRegression: return "prefer-regression";
    case livekit::BackupCodecPolicy::Simulcast: return "simulcast";
    case livekit::BackupCodecPolicy::Regression: return "regression";
    }
    return "prefer-regression";
}

const char* ClockStateName(livekit::telemetry::ClockCalibrationState state) {
    using State = livekit::telemetry::ClockCalibrationState;
    switch (state) {
    case State::WarmingUp: return "warming_up";
    case State::Valid: return "valid";
    case State::Uncertain: return "uncertain";
    case State::Stale: return "stale";
    case State::Invalid: return "invalid";
    }
    return "invalid";
}

void Usage(const char* executable) {
    std::cout
        << "Usage: " << executable
        << " --role publisher|receiver --url <ws-url>"
        << " (--token <jwt> | --token-env <name>)"
        << " --session <id> --local-peer <identity> --remote-peer <identity>"
        << " [--phase-id <id>] [--codec auto|vp8|h264|vp9|av1]"
        << " [--expected-codec vp8|h264|vp9|av1]"
        << " [--receiver-publish-codec vp8|h264|vp9|av1]"
        << " [--source camera|screen] [--scalability-mode <mode>]"
        << " [--backup-codec none|vp8|h264]"
        << " [--backup-policy prefer-regression|simulcast|regression]"
        << " [--auto-backup true|false]"
        << " [--width <pixels>] [--height <pixels>]"
        << " [--simulcast true|false] [--quality high|medium|low]"
        << " [--probes <1..64>] [--probe-timeout-ms <1000..15000>]"
        << " [--announcement-lead-ms <50..2000>]"
        << " [--marker-repeat-frames <1..30>]"
        << " [--shared-clock-ground-truth true|false]\n"
        << "Run the receiver first with the same matrix settings. Tokens are"
        << " never printed. The shared-clock option is valid only for two"
        << " processes on the same Windows host. This target is opt-in and is"
        << " not registered with CTest. Use --list-codecs or --self-test for"
        << " deterministic local checks that do not require a server.\n";
}

std::optional<int> ParseInt(std::string_view value) {
    try {
        std::size_t consumed = 0;
        const int parsed = std::stoi(std::string(value), &consumed);
        if (consumed != value.size()) return std::nullopt;
        return parsed;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<bool> ParseBool(std::string_view value) {
    const auto normalized = Lower(std::string(value));
    if (normalized == "true" || normalized == "1") return true;
    if (normalized == "false" || normalized == "0") return false;
    return std::nullopt;
}

std::optional<livekit::proto::VideoQuality> ParseQuality(std::string_view value) {
    const auto normalized = Lower(std::string(value));
    if (normalized == "low") return livekit::proto::VideoQuality::LOW;
    if (normalized == "medium") return livekit::proto::VideoQuality::MEDIUM;
    if (normalized == "high") return livekit::proto::VideoQuality::HIGH;
    return std::nullopt;
}

std::optional<Config> Parse(int argc, char** argv) {
    Config config;
    std::string role;
    std::string token_env;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--help" || argument == "-h") return std::nullopt;
        if (i + 1 >= argc) return std::nullopt;
        const std::string value = argv[++i];
        if (argument == "--role") role = value;
        else if (argument == "--url") config.url = value;
        else if (argument == "--token") config.token = value;
        else if (argument == "--token-env") token_env = value;
        else if (argument == "--session") config.session_id = value;
        else if (argument == "--local-peer") config.local_peer_id = value;
        else if (argument == "--remote-peer") config.remote_peer_id = value;
        else if (argument == "--phase-id") config.phase_id = value;
        else if (argument == "--codec") config.codec = Lower(value);
        else if (argument == "--expected-codec") config.expected_codec = Lower(value);
        else if (argument == "--receiver-publish-codec") config.receiver_publish_codec = Lower(value);
        else if (argument == "--source") config.source = Lower(value);
        else if (argument == "--scalability-mode") config.scalability_mode = value;
        else if (argument == "--backup-codec") {
            config.backup_codec = Lower(value);
            if (config.backup_codec == "none") config.backup_codec.clear();
        } else if (argument == "--backup-policy") {
            const auto policy = Lower(value);
            if (policy == "prefer-regression") {
                config.backup_policy = livekit::BackupCodecPolicy::PreferRegression;
            } else if (policy == "simulcast") {
                config.backup_policy = livekit::BackupCodecPolicy::Simulcast;
            } else if (policy == "regression") {
                config.backup_policy = livekit::BackupCodecPolicy::Regression;
            } else {
                return std::nullopt;
            }
        } else if (argument == "--auto-backup") {
            const auto parsed = ParseBool(value);
            if (!parsed) return std::nullopt;
            config.auto_backup_codec = *parsed;
        }
        else if (argument == "--width") {
            const auto parsed = ParseInt(value);
            if (!parsed) return std::nullopt;
            config.width = *parsed;
        } else if (argument == "--height") {
            const auto parsed = ParseInt(value);
            if (!parsed) return std::nullopt;
            config.height = *parsed;
        } else if (argument == "--simulcast") {
            const auto parsed = ParseBool(value);
            if (!parsed) return std::nullopt;
            config.simulcast = *parsed;
        } else if (argument == "--quality") {
            const auto parsed = ParseQuality(value);
            if (!parsed) return std::nullopt;
            config.quality = *parsed;
        } else if (argument == "--probes") {
            const auto parsed = ParseInt(value);
            if (!parsed || *parsed < 1) return std::nullopt;
            config.probes = static_cast<std::size_t>(*parsed);
        } else if (argument == "--probe-timeout-ms") {
            const auto parsed = ParseInt(value);
            if (!parsed) return std::nullopt;
            config.probe_timeout_ms = *parsed;
        } else if (argument == "--announcement-lead-ms") {
            const auto parsed = ParseInt(value);
            if (!parsed) return std::nullopt;
            config.announcement_lead_ms = *parsed;
        } else if (argument == "--marker-repeat-frames") {
            const auto parsed = ParseInt(value);
            if (!parsed) return std::nullopt;
            config.marker_repeat_frames = *parsed;
        } else if (argument == "--shared-clock-ground-truth") {
            const auto parsed = ParseBool(value);
            if (!parsed) return std::nullopt;
            config.shared_clock_ground_truth = *parsed;
        } else {
            return std::nullopt;
        }
    }

    if (role == "publisher") config.role = Role::Publisher;
    else if (role == "receiver") config.role = Role::Receiver;
    else return std::nullopt;

    if (!token_env.empty()) {
        if (!config.token.empty()) return std::nullopt;
        const char* value = std::getenv(token_env.c_str());
        if (value) config.token = value;
    }
    if (config.url.empty() || config.token.empty() || config.session_id.empty() ||
        config.local_peer_id.empty() || config.remote_peer_id.empty() ||
        config.local_peer_id == config.remote_peer_id || config.phase_id.empty() ||
        (config.codec != "auto" && config.codec != "vp8" &&
         config.codec != "h264" && config.codec != "vp9" &&
         config.codec != "av1") ||
        (!config.expected_codec.empty() && config.expected_codec != "vp8" &&
         config.expected_codec != "h264" && config.expected_codec != "vp9" &&
         config.expected_codec != "av1") ||
        (!config.receiver_publish_codec.empty() &&
         (config.role != Role::Receiver ||
          (config.receiver_publish_codec != "vp8" && config.receiver_publish_codec != "h264" &&
           config.receiver_publish_codec != "vp9" && config.receiver_publish_codec != "av1"))) ||
        (config.source != "camera" && config.source != "screen") ||
        (!config.backup_codec.empty() && config.backup_codec != "vp8" &&
         config.backup_codec != "h264") ||
        config.width < 160 || config.width > 3840 ||
        config.height < 90 || config.height > 2160 ||
        (config.width % 2) != 0 || (config.height % 2) != 0 ||
        config.probes > 64 || config.probe_timeout_ms < 1000 ||
        config.probe_timeout_ms > 15000 || config.announcement_lead_ms < 50 ||
        config.announcement_lead_ms > 2000 ||
        config.marker_repeat_frames < 1 || config.marker_repeat_frames > 30) {
        return std::nullopt;
    }
    return config;
}

bool IsLoopbackDevelopmentUrl(std::string_view url) {
    return url.starts_with("ws://127.0.0.1") ||
        url.starts_with("ws://localhost") || url.starts_with("ws://[::1]");
}

std::int64_t NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now().time_since_epoch()).count();
}

std::string RandomNonce(std::uint64_t& probe_id) {
    std::random_device random;
    probe_id = (static_cast<std::uint64_t>(random()) << 32U) ^ random();
    if (probe_id == 0) probe_id = 1;
    std::ostringstream text;
    text << "probe-" << std::hex << std::setw(16) << std::setfill('0')
         << probe_id;
    return text.str();
}

livekit::telemetry::E2eMeasurementSession MakeMeasurementSession(
        const Config& config) {
    livekit::telemetry::E2eMeasurementSessionConfig session;
    session.session_id = config.session_id;
    session.local_peer_id = config.local_peer_id;
    session.remote_peer_id = config.remote_peer_id;
    session.local_capabilities = {{1}, true, true, true};
    session.clock_policy.minimum_samples = 5;
    session.clock_policy.maximum_uncertainty_us = 10'000;
    session.maximum_pending_media_probes = std::max<std::size_t>(16, config.probes);
    return livekit::telemetry::E2eMeasurementSession(std::move(session));
}

template <typename Value>
Value Percentile(std::vector<Value> values, double percentile) {
    if (values.empty()) return {};
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(std::ceil(
        percentile * static_cast<double>(values.size()))) - 1;
    return values[std::min(index, values.size() - 1)];
}

template <typename Value>
double Mean(const std::vector<Value>& values) {
    if (values.empty()) return 0.0;
    long double sum = 0.0L;
    for (const auto value : values) sum += static_cast<long double>(value);
    return static_cast<double>(sum / values.size());
}

bool CodecMatches(std::string value, std::string_view codec) {
    value = Lower(std::move(value));
    const auto slash = value.find('/');
    if (slash != std::string::npos) value.erase(0, slash + 1);
    return value == codec;
}

std::vector<std::string> CodecNames(
        const webrtc::RtpCapabilities& capabilities) {
    std::set<std::string> unique;
    for (const auto& codec : capabilities.codecs) {
        if (!codec.IsMediaCodec()) continue;
        auto name = Lower(codec.name);
        const auto slash = name.find('/');
        if (slash != std::string::npos) name.erase(0, slash + 1);
        if (!name.empty()) unique.insert(std::move(name));
    }
    return {unique.begin(), unique.end()};
}

std::string Join(const std::vector<std::string>& values) {
    std::string result;
    for (const auto& value : values) {
        if (!result.empty()) result += ',';
        result += value;
    }
    return result;
}

int RunLocalMode(bool self_test) {
    auto& manager = livekit::WebRTCManager::Instance();
    if (!manager.Initialize() || !manager.factory()) {
        std::cout << "CODEC_RUNTIME_LOCAL FAIL reason=webrtc_initialization_failed\n";
        return EXIT_FAILURE;
    }
    const auto sender = CodecNames(manager.factory()->GetRtpSenderCapabilities(
        webrtc::MediaType::VIDEO));
    const auto receiver = CodecNames(manager.factory()->GetRtpReceiverCapabilities(
        webrtc::MediaType::VIDEO));
    std::cout << "CODEC_RUNTIME_CAPABILITIES send=" << Join(sender)
              << " receive=" << Join(receiver)
              << " h265_provider="
              << (livekit::IsVideoEncoderFormatSupported("h265") ? "available" : "unavailable")
              << '\n';
    bool passed = true;
    if (self_test) {
        livekit::VideoPublishOptions automatic;
        automatic.video_codec = "auto";
        automatic.auto_backup_codec = false;
        const auto auto_plan = livekit::LocalVideoTrack::ResolvePublishPlan(
            1280, 720, automatic, sender, {});
        passed = auto_plan.ok() && auto_plan.requested_codec == "auto" &&
            auto_plan.effective_codec == "vp8" && !auto_plan.used_fallback();

        livekit::VideoPublishOptions vp9;
        vp9.video_codec = "vp9";
        vp9.simulcast = false;
        vp9.scalability_mode = "L3T3_KEY";
        vp9.auto_backup_codec = false;
        const auto vp9_plan = livekit::LocalVideoTrack::ResolvePublishPlan(
            1280, 720, vp9, sender, {});
        passed = passed && vp9_plan.ok() &&
            vp9_plan.effective.scalability_mode == "L3T3_KEY";

        livekit::VideoPublishOptions av1;
        av1.video_codec = "av1";
        av1.simulcast = false;
        av1.scalability_mode = "L1T1";
        av1.auto_backup_codec = false;
        const auto av1_plan = livekit::LocalVideoTrack::ResolvePublishPlan(
            1280, 720, av1, sender, {});
        av1.scalability_mode = "L2T1";
        const auto rejected_av1 = livekit::LocalVideoTrack::ResolvePublishPlan(
            1280, 720, av1, sender, {});
        passed = passed && av1_plan.ok() && !rejected_av1.ok();

        auto frame = livekit::VideoFrame::create(
            160, 90, livekit::VideoBufferType::I420);
        const livekit::telemetry::E2eMediaMarker marker{42, 7};
        const bool embedded = livekit::telemetry::EmbedE2eMediaMarker(frame, marker);
        const auto decoded = livekit::telemetry::DecodeE2eMediaMarker(frame);
        passed = passed && embedded && decoded && decoded->probe_id == marker.probe_id &&
            decoded->sequence == marker.sequence &&
            std::find(sender.begin(), sender.end(), "h265") == sender.end() &&
            std::find(receiver.begin(), receiver.end(), "h265") == receiver.end() &&
            !livekit::IsVideoEncoderFormatSupported("h265");
        std::cout << "CODEC_RUNTIME_SELF_TEST auto_effective="
                  << (auto_plan.ok() ? auto_plan.effective_codec : "unavailable")
                  << " vp9_svc=" << (vp9_plan.ok() ? "supported" : "unsupported")
                  << " av1_l1t1=" << (av1_plan.ok() ? "supported" : "unsupported")
                  << " av1_l2t1=" << (rejected_av1.ok() ? "unexpected" : "rejected")
                  << " marker_roundtrip=" << (embedded && decoded ? "pass" : "fail")
                  << '\n';
    }
    manager.Deinitialize();
    std::cout << "CODEC_RUNTIME_LOCAL " << (passed ? "PASS" : "FAIL") << '\n';
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

struct ProbeRecord {
    std::size_t index = 0;
    std::int64_t publication_start_us = 0;
    std::int64_t capture_us = 0;
    bool acknowledged = false;
};

class E2eMediaRuntime final
    : public livekit::RoomListener,
      public std::enable_shared_from_this<E2eMediaRuntime> {
public:
    E2eMediaRuntime(asio::io_context& io, Config config)
        : io_(io), strand_(asio::make_strand(io)), config_(std::move(config)),
          room_(livekit::Room::Create(strand_)),
          measurement_(MakeMeasurementSession(config_)), deadline_(strand_),
          capability_timer_(strand_), finish_timer_(strand_) {}

    void Start() {
        room_->AddListener(shared_from_this());
        const auto timeout = std::chrono::seconds(
            30 + static_cast<int>(config_.probes) *
                (config_.probe_timeout_ms / 1000 + 2));
        deadline_.expires_after(timeout);
        deadline_.async_wait([self = shared_from_this()](const std::error_code& error) {
            if (!error && !self->finished_) self->Finish(false, "runtime_timeout");
        });
        ScheduleCapabilities();

        livekit::SignalOptions options;
        options.auto_subscribe = true;
        options.single_peer_connection = true;
        options.connect_timeout = 10s;
        options.allow_insecure_transport = IsLoopbackDevelopmentUrl(config_.url);
        asio::co_spawn(strand_,
            [self = shared_from_this(), options]() -> asio::awaitable<void> {
                try {
                    co_await self->room_->ConnectAsync(
                        self->config_.url, self->config_.token, options);
                } catch (const std::exception&) {
                    self->Finish(false, "connect_failed");
                }
            }, asio::detached);
    }

    bool success() const noexcept { return success_.load(); }

    void OnConnected() override {
        asio::post(strand_, [self = shared_from_this()] {
            ++self->connection_events_;
            self->connected_ = true;
            if (!self->config_.receiver_publish_codec.empty() && !self->receiver_publish_started_) {
                self->receiver_publish_started_ = true;
                asio::co_spawn(self->strand_,
                    [self]() -> asio::awaitable<void> {
                        co_await self->PublishReceiverVideo();
                    }, asio::detached);
            }
            self->SendCapabilities();
        });
    }

    void OnDisconnected(const std::string&) override { PostPeerLeft(); }

    void OnParticipantConnected(
            std::shared_ptr<livekit::RemoteParticipant> participant) override {
        if (!participant || participant->identity() != config_.remote_peer_id) return;
        asio::post(strand_, [self = shared_from_this()] { self->SendCapabilities(); });
    }

    void OnParticipantDisconnected(
            std::shared_ptr<livekit::RemoteParticipant> participant) override {
        if (!participant || participant->identity() != config_.remote_peer_id) return;
        PostPeerLeft();
    }

    void OnLocalTrackRepublished(
            const std::string&,
            std::shared_ptr<livekit::TrackPublication>) override {
        asio::post(strand_, [self = shared_from_this()] {
            ++self->republish_events_;
        });
    }

    void OnDataReceived(const std::vector<std::uint8_t>& payload,
                        std::shared_ptr<livekit::RemoteParticipant> participant,
                        const std::string& topic) override {
        if (topic != livekit::telemetry::kE2eMeasurementTopic || !participant ||
            participant->identity() != config_.remote_peer_id) return;
        std::string bytes(payload.begin(), payload.end());
        const auto received_us = NowUs();
        asio::post(strand_, [self = shared_from_this(), bytes = std::move(bytes),
                             received_us] {
            self->HandleMessage(bytes, received_us);
        });
    }

    void OnTrackSubscribed(
            std::shared_ptr<livekit::Track> track,
            std::shared_ptr<livekit::TrackPublication> publication,
            std::shared_ptr<livekit::RemoteParticipant> participant) override {
        if (config_.role != Role::Receiver || !track ||
            track->kind() != livekit::TrackKind::Video || !participant ||
            participant->identity() != config_.remote_peer_id) return;
        asio::post(strand_, [self = shared_from_this(), track = std::move(track),
                             publication = std::move(publication)] {
            const auto track_id = track->sid();
            self->remote_publication_ =
                std::dynamic_pointer_cast<livekit::RemoteTrackPublication>(publication);
            if (!self->remote_publication_) {
                self->Finish(false, "remote_publication_control_unavailable");
                return;
            }
            self->quality_control_accepted_ =
                self->remote_publication_->SetVideoQuality(self->config_.quality);
            if (!self->quality_control_accepted_) {
                self->Finish(false, "video_quality_control_rejected");
                return;
            }
            auto subscription = track->subscribeI420VideoFrames(
                [weak = self->weak_from_this(), track_id](auto frame) {
                    if (auto owner = weak.lock()) {
                        const auto decoded_us = NowUs();
                        asio::post(owner->strand_,
                            [owner, track_id, frame = std::move(frame), decoded_us] {
                                owner->HandleDecodedFrame(track_id, frame, decoded_us);
                            });
                    }
                });
            self->subscriptions_.push_back(std::move(subscription));
        });
    }

    void OnTrackUnsubscribed(
            std::shared_ptr<livekit::Track> track,
            std::shared_ptr<livekit::TrackPublication>,
            std::shared_ptr<livekit::RemoteParticipant>) override {
        if (!track) return;
        const auto track_id = track->sid();
        asio::post(strand_, [self = shared_from_this(), track_id] {
            self->measurement_.CancelMediaTrack(track_id);
            self->remote_publication_.reset();
        });
    }

    void PostRenderSubmit(std::string track_id, std::uint64_t render_token,
                          std::int64_t submit_us, std::string measurement_point,
                          std::uint32_t frame_width,
                          std::uint32_t frame_height) {
        asio::post(strand_,
            [self = shared_from_this(), track_id = std::move(track_id),
             render_token, submit_us, measurement_point = std::move(measurement_point),
             frame_width, frame_height]() mutable {
                auto acknowledgement =
                    self->measurement_.BuildMediaAckAfterRenderSubmit(
                        track_id, render_token, submit_us,
                        std::move(measurement_point), frame_width, frame_height);
                if (!acknowledgement.accepted() || !acknowledgement.message) return;
                const auto encoded = livekit::telemetry::EncodeE2eMessage(
                    *acknowledgement.message);
                if (!encoded || !self->SendBytes(*encoded)) {
                    self->Finish(false, "media_ack_send_failed");
                    return;
                }
                ++self->acks_sent_;
                if (self->acks_sent_ == self->config_.probes) {
                    self->Finish(true, "receiver_all_media_acks_sent");
                }
            });
    }

private:
    class AckObserver final : public livekit::render::RenderSubmitObserver {
    public:
        AckObserver(std::weak_ptr<E2eMediaRuntime> owner, std::string track_id,
                    std::uint32_t frame_width, std::uint32_t frame_height)
            : owner_(std::move(owner)), track_id_(std::move(track_id)),
              frame_width_(frame_width), frame_height_(frame_height) {}

        void SetExpected(bool, livekit::render::RenderExpectationReason,
                         Clock::time_point) override {}

        void OnSubmitted(const livekit::render::RenderFrameMetadata& metadata,
                         const char* measurement_point,
                         Clock::time_point source_time) override {
            if (auto owner = owner_.lock()) {
                owner->PostRenderSubmit(
                    track_id_, metadata.frame_token,
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        source_time.time_since_epoch()).count(),
                    measurement_point ? measurement_point : "render_submit",
                    frame_width_, frame_height_);
            }
        }

    private:
        std::weak_ptr<E2eMediaRuntime> owner_;
        std::string track_id_;
        std::uint32_t frame_width_ = 0;
        std::uint32_t frame_height_ = 0;
    };

    void PostPeerLeft() {
        asio::post(strand_, [self = shared_from_this()] {
            if (self->finished_) return;
            const bool complete = self->config_.role == Role::Receiver &&
                self->acks_sent_ == self->config_.probes;
            self->Finish(complete, complete ? "receiver_complete" :
                "measurement_peer_left");
        });
    }

    asio::awaitable<void> Delay(std::chrono::milliseconds duration) {
        asio::steady_timer timer(strand_);
        timer.expires_after(duration);
        co_await timer.async_wait(asio::use_awaitable);
    }

    void ScheduleCapabilities() {
        capability_timer_.expires_after(500ms);
        capability_timer_.async_wait(
            [self = shared_from_this()](const std::error_code& error) {
                if (error || self->finished_) return;
                // Capabilities have no acknowledgement message. Keep the
                // idempotent advertisement alive so a peer that joined later
                // cannot remain permanently unnegotiated.
                self->SendCapabilities();
                self->ScheduleCapabilities();
            });
    }

    void SendCapabilities() {
        if (!connected_) return;
        if (!config_.receiver_publish_codec.empty() && !receiver_publish_ready_) return;
        const auto encoded = livekit::telemetry::EncodeE2eMessage(
            measurement_.BuildCapabilities(next_sequence_++));
        if (encoded) SendBytes(*encoded);
    }

    bool SendBytes(std::string_view bytes) {
        livekit::proto::DataPacket packet;
        auto* user = packet.mutable_user();
        user->set_topic(std::string(
            livekit::telemetry::kE2eMeasurementTopic));
        user->set_payload(bytes.data(), bytes.size());
        user->add_destination_identities(config_.remote_peer_id);
        return room_->PublishDataPacket(packet, true);
    }

    void HandleMessage(std::string_view bytes, std::int64_t received_us) {
        const auto message = livekit::telemetry::DecodeE2eMessage(
            livekit::telemetry::kE2eMeasurementTopic, bytes);
        if (!message) return;
        using Kind = livekit::telemetry::E2eMessageKind;
        switch (message->kind) {
        case Kind::Capabilities:
            if (measurement_.AcceptCapabilities(*message).accepted())
                MaybeStartPublisher();
            break;
        case Kind::ClockRequest: {
            auto response = measurement_.AnswerClockProbe(*message, received_us, NowUs());
            if (response.accepted() && response.message) {
                if (const auto encoded = livekit::telemetry::EncodeE2eMessage(
                        *response.message)) SendBytes(*encoded);
            }
            break;
        }
        case Kind::ClockResponse:
            if (measurement_.AcceptClockResponse(*message, received_us).accepted())
                ++clock_responses_;
            break;
        case Kind::MediaProbe: {
            const auto accepted = measurement_.AcceptMediaProbe(*message);
            if (accepted.accepted()) ++probe_announcements_accepted_;
            else if (accepted.status ==
                    livekit::telemetry::E2eActionStatus::DuplicateOrUnknown)
                ++probe_announcement_duplicates_;
            break;
        }
        case Kind::MediaAck:
            HandleMediaAck(*message, received_us);
            break;
        }
    }

    void HandleMediaAck(const livekit::telemetry::E2eMessage& message,
                        std::int64_t received_us) {
        const auto result = measurement_.AcceptMediaAck(message, received_us);
        if (!result.accepted() || !result.media_measurement) {
            if (result.status ==
                livekit::telemetry::E2eActionStatus::DuplicateOrUnknown)
                ++duplicate_acks_;
            else
                ++invalid_acks_;
            return;
        }
        auto found = outgoing_probes_.find(message.probe_id);
        if (found == outgoing_probes_.end() || found->second.acknowledged) {
            ++invalid_acks_;
            return;
        }
        found->second.acknowledged = true;
        acknowledged_probes_.insert(message.probe_id);

        const auto& value = *result.media_measurement;
        ack_rtt_us_.push_back(value.ack_round_trip_us);
        if (value.clock_state == livekit::telemetry::ClockCalibrationState::Valid) {
            ++clock_valid_measurements_;
            uncertainty_us_.push_back(value.uncertainty_us);
        }
        if (value.publication_to_decode_us)
            e2e02_us_.push_back(*value.publication_to_decode_us);
        if (value.capture_to_render_submit_us)
            e2e03_us_.push_back(*value.capture_to_render_submit_us);

        std::optional<std::int64_t> e2e02_error;
        std::optional<std::int64_t> e2e03_error;
        if (config_.shared_clock_ground_truth) {
            if (value.publication_to_decode_us &&
                message.remote_decode_us >= found->second.publication_start_us) {
                const auto truth = message.remote_decode_us -
                    found->second.publication_start_us;
                e2e02_error = std::llabs(*value.publication_to_decode_us - truth);
                e2e02_error_us_.push_back(*e2e02_error);
            }
            if (value.capture_to_render_submit_us &&
                message.remote_render_submit_us >= found->second.capture_us) {
                const auto truth = message.remote_render_submit_us -
                    found->second.capture_us;
                e2e03_error = std::llabs(
                    *value.capture_to_render_submit_us - truth);
                e2e03_error_us_.push_back(*e2e03_error);
            }
        }

        const auto mapping = measurement_.clock_mapping(received_us);
        std::cout << std::fixed << std::setprecision(3)
                  << "E2E_PROBE phase_id=" << config_.phase_id
                  << " codec=" << config_.codec
                  << " source_width=" << config_.width
                  << " source_height=" << config_.height
                  << " simulcast=" << (config_.simulcast ? "true" : "false")
                  << " quality=" << QualityName(config_.quality)
                  << " probe_index=" << found->second.index
                  << " status=matched"
                  << " ack_rtt_us=" << value.ack_round_trip_us
                  << " clock_state=" << ClockStateName(value.clock_state)
                  << " clock_offset_us=" << mapping.offset_us
                  << " clock_drift_ppm=" << mapping.drift_ppm
                  << " clock_uncertainty_us=" << value.uncertainty_us
                  << " clock_age_us=" << mapping.age_us;
        if (value.publication_to_decode_us)
            std::cout << " e2e02_us=" << *value.publication_to_decode_us;
        else
            std::cout << " e2e02_us=UNAVAILABLE";
        if (value.capture_to_render_submit_us)
            std::cout << " e2e03_us=" << *value.capture_to_render_submit_us;
        else
            std::cout << " e2e03_us=UNAVAILABLE";
        if (value.remote_frame_width && value.remote_frame_height) {
            received_widths_.push_back(*value.remote_frame_width);
            received_heights_.push_back(*value.remote_frame_height);
            std::cout << " received_width=" << *value.remote_frame_width
                      << " received_height=" << *value.remote_frame_height;
        } else {
            std::cout << " received_width=UNAVAILABLE received_height=UNAVAILABLE";
        }
        if (e2e02_error && e2e03_error) {
            std::cout << " e2e02_error_us=" << *e2e02_error
                      << " e2e03_error_us=" << *e2e03_error;
        } else {
            std::cout << " e2e02_error_us=UNAVAILABLE e2e03_error_us=UNAVAILABLE";
        }
        std::cout << " one_way_reason=" << value.one_way_reason << '\n';
    }

    void MaybeStartPublisher() {
        if (config_.role != Role::Publisher || publisher_started_ || !connected_ ||
            measurement_.negotiation().mode !=
                livekit::telemetry::E2eMeasurementMode::MediaCorrelated) return;
        publisher_started_ = true;
        asio::co_spawn(strand_,
            [self = shared_from_this()]() -> asio::awaitable<void> {
                co_await self->RunPublisher();
            }, asio::detached);
    }

    livekit::VideoPublishOptions RequestedVideoOptions() const {
        livekit::VideoPublishOptions options;
        options.source = config_.source == "screen"
            ? livekit::TrackSource::ScreenShareVideo
            : livekit::TrackSource::Camera;
        options.video_codec = config_.codec;
        options.simulcast = config_.simulcast;
        options.scalability_mode = config_.scalability_mode;
        options.auto_backup_codec = config_.auto_backup_codec;
        options.backup_codec_policy = config_.backup_policy;
        if (!config_.backup_codec.empty()) options.backup_codec = config_.backup_codec;
        return options;
    }

    livekit::ResolvedVideoPublishPlan ResolveRequestedPlan() const {
        const auto factory = livekit::WebRTCManager::Instance().factory();
        if (!factory) {
            livekit::ResolvedVideoPublishPlan plan;
            plan.error = "peer connection factory unavailable";
            return plan;
        }
        const auto capabilities = factory->GetRtpSenderCapabilities(webrtc::MediaType::VIDEO);
        std::vector<std::string> local;
        for (const auto& codec : capabilities.codecs) {
            if (codec.IsMediaCodec()) local.push_back(codec.name);
        }
        return livekit::LocalVideoTrack::ResolvePublishPlan(
            config_.width, config_.height, RequestedVideoOptions(), local,
            room_->enabled_publish_codecs());
    }

    // Exercise the production single-PC ordering: finish a local publication
    // before allowing the other peer to publish a different codec downstream.
    asio::awaitable<void> PublishReceiverVideo() {
        try {
            auto source = std::make_shared<livekit::VideoSource>(320, 180);
            livekit::VideoPublishOptions options;
            options.video_codec = config_.receiver_publish_codec;
            options.simulcast = false;
            options.auto_backup_codec = false;
            if (options.video_codec == "vp9" || options.video_codec == "av1") {
                options.scalability_mode = "L1T1";
            }
            const auto local = room_->local_participant();
            if (!local) {
                Finish(false, "receiver_local_participant_unavailable");
                co_return;
            }
            auto track = livekit::LocalVideoTrack::createLocalVideoTrack(
                "mixed-codec-uplink", source, livekit::TrackSource::Camera, options);
            const auto publication = co_await local->PublishTrackAsync(track);
            if (!publication) {
                Finish(false, "receiver_uplink_publish_failed");
                co_return;
            }
            receiver_publish_ready_ = true;
            std::cout << "E2E_RECEIVER_PUBLISHED codec="
                      << config_.receiver_publish_codec << std::endl;
            SendCapabilities();
            auto frame = livekit::VideoFrame::create(320, 180, livekit::VideoBufferType::I420);
            std::fill(frame.data(), frame.data() + frame.dataSize(), 128);
            int frames = 0;
            while (!finished_) {
                livekit::VideoCaptureOptions capture;
                capture.timestamp_us = NowUs();
                source->captureFrame(frame, capture);
                if (++frames % 30 == 0 && !receiver_uplink_verified_) {
                    const auto report = co_await room_->GetStats();
                    for (const auto& peer : report.reports) {
                        for (const auto& stream : peer.outbound_rtp) {
                            if (!stream.kind_available || stream.kind != "video" ||
                                !stream.codec_id_available || !stream.packets_sent_available ||
                                stream.packets_sent == 0) continue;
                            for (const auto& codec : peer.codecs) {
                                if (codec.id == stream.codec_id && codec.mime_type_available &&
                                    CodecMatches(codec.mime_type, config_.receiver_publish_codec)) {
                                    receiver_uplink_verified_ = true;
                                }
                            }
                        }
                    }
                }
                co_await Delay(33ms);
            }
        } catch (const std::exception&) {
            Finish(false, "receiver_uplink_failed");
        }
    }

    asio::awaitable<bool> CollectPublisherStats() {
        const auto report = co_await room_->GetStats();
        std::set<std::string> codecs;
        std::set<std::string> implementations;
        std::set<std::string> scalability;
        encoded_frames_ = 0;
        sent_frames_ = 0;
        sent_packets_ = 0;
        for (const auto& peer : report.reports) {
            const auto codec_name = [&peer](const std::string& id) {
                const auto found = std::find_if(
                    peer.codecs.begin(), peer.codecs.end(),
                    [&](const livekit::CodecStats& codec) { return codec.id == id; });
                if (found == peer.codecs.end() || !found->mime_type_available) {
                    return std::string{};
                }
                auto name = Lower(found->mime_type);
                const auto slash = name.find('/');
                if (slash != std::string::npos) name.erase(0, slash + 1);
                return name;
            };
            for (const auto& stream : peer.outbound_rtp) {
                if (!stream.kind_available || stream.kind != "video") continue;
                if (stream.codec_id_available) {
                    const auto name = codec_name(stream.codec_id);
                    if (!name.empty()) codecs.insert(name);
                }
                if (stream.encoder_implementation_available &&
                    !stream.encoder_implementation.empty()) {
                    implementations.insert(stream.encoder_implementation);
                }
                if (stream.scalability_mode_available &&
                    !stream.scalability_mode.empty()) {
                    scalability.insert(stream.scalability_mode);
                } else if (stream.rid_available) {
                    scalability.insert("rid-present");
                }
                if (stream.frames_encoded_available) {
                    encoded_frames_ += stream.frames_encoded;
                }
                if (stream.frames_sent_available) sent_frames_ += stream.frames_sent;
                if (stream.packets_sent_available) sent_packets_ += stream.packets_sent;
            }
        }
        observed_codecs_.assign(codecs.begin(), codecs.end());
        observed_implementations_.assign(
            implementations.begin(), implementations.end());
        observed_scalability_.assign(scalability.begin(), scalability.end());
        const bool codec_observed = std::find(
            observed_codecs_.begin(), observed_codecs_.end(),
            resolved_effective_codec_) != observed_codecs_.end();
        co_return report.successful_peer_connection_count > 0 && codec_observed &&
            encoded_frames_ > 0 && sent_packets_ > 0;
    }

    livekit::VideoFrame MakeFrame(
            std::optional<livekit::telemetry::E2eMediaMarker> marker =
                std::nullopt) const {
        auto frame = livekit::VideoFrame::create(
            config_.width, config_.height, livekit::VideoBufferType::I420);
        std::fill(frame.data(), frame.data() + frame.dataSize(), 128);
        if (marker && !livekit::telemetry::EmbedE2eMediaMarker(frame, *marker))
            return {};
        return frame;
    }

    void Capture(const livekit::VideoFrame& frame) {
        livekit::VideoCaptureOptions options;
        options.timestamp_us = NowUs();
        video_source_->captureFrame(frame, options);
    }

    asio::awaitable<void> SendNeutralFrames(int count) {
        const auto neutral = MakeFrame();
        for (int i = 0; i < count && !finished_; ++i) {
            Capture(neutral);
            co_await Delay(33ms);
        }
    }

    asio::awaitable<void> RunPublisher() {
        try {
            const auto plan = ResolveRequestedPlan();
            if (!plan.ok()) {
                Finish(false, "publish_plan_unavailable");
                co_return;
            }
            resolved_requested_codec_ = plan.requested_codec;
            resolved_effective_codec_ = plan.effective_codec;
            fallback_reason_ = plan.fallback_reason;
            resolved_mode_ = !plan.effective.scalability_mode.empty()
                ? "svc"
                : (plan.effective.simulcast && plan.effective.layers.size() > 1
                    ? "simulcast" : "single");
            if (!config_.expected_codec.empty() &&
                config_.expected_codec != resolved_effective_codec_) {
                Finish(false, "unexpected_effective_codec");
                co_return;
            }
            std::cout << "E2E_PLAN phase_id=" << config_.phase_id
                      << " requested_codec=" << resolved_requested_codec_
                      << " effective_codec=" << resolved_effective_codec_
                      << " fallback=" << (fallback_reason_.empty() ? "none" : "applied")
                      << " source=" << config_.source
                      << " mode=" << resolved_mode_
                      << " scalability=" << (plan.effective.scalability_mode.empty()
                            ? "none" : plan.effective.scalability_mode)
                      << " backup_policy=" << BackupPolicyName(config_.backup_policy)
                      << " backup_codec=" << (plan.effective.backup_codec
                            ? *plan.effective.backup_codec : "none") << '\n';
            const auto participant = room_->local_participant();
            if (!participant) {
                Finish(false, "local_participant_unavailable");
                co_return;
            }
            video_source_ = std::make_shared<livekit::VideoSource>(
                config_.width, config_.height);
            auto options = RequestedVideoOptions();
            auto track = livekit::LocalVideoTrack::createLocalVideoTrack(
                "e2e-controlled-marker", video_source_,
                options.source, options);
            const auto publication_started_us = NowUs();
            const auto publication = co_await participant->PublishTrackAsync(track);
            if (!publication || publication->sid().empty()) {
                Finish(false, "controlled_track_publish_failed");
                co_return;
            }
            published_track_id_ = publication->sid();

            // Flow RTP during clock warm-up and allow the receiver's real
            // RemoteTrackPublication quality request to settle.
            for (int sample = 0; sample < 16 && !finished_; ++sample) {
                const auto nonce = "clock-" + std::to_string(next_sequence_);
                auto request = measurement_.BeginClockProbe(
                    nonce, next_sequence_++, NowUs());
                if (request.accepted() && request.message) {
                    if (const auto encoded = livekit::telemetry::EncodeE2eMessage(
                            *request.message)) SendBytes(*encoded);
                }
                co_await SendNeutralFrames(8);
            }

            for (std::size_t index = 1;
                 index <= config_.probes && !finished_; ++index) {
                std::uint64_t probe_id = 0;
                const auto nonce = RandomNonce(probe_id);
                const auto sequence = next_sequence_++;

                // Announce before measuring so the data-channel message is in
                // receiver state before the single marker frame is captured.
                livekit::telemetry::E2eMessage announcement;
                announcement.kind = livekit::telemetry::E2eMessageKind::MediaProbe;
                announcement.session_id = config_.session_id;
                announcement.sender_peer_id = config_.local_peer_id;
                announcement.destination_peer_id = config_.remote_peer_id;
                announcement.sequence = sequence;
                announcement.protocol_version = measurement_.negotiation().protocol_version;
                announcement.nonce = nonce;
                announcement.track_id = published_track_id_;
                announcement.probe_id = probe_id;
                const auto encoded = livekit::telemetry::EncodeE2eMessage(announcement);
                if (!encoded || !SendBytes(*encoded)) {
                    Finish(false, "media_probe_announcement_failed");
                    co_return;
                }
                ++probe_announcements_sent_;
                co_await SendNeutralFrames(std::max(1, config_.announcement_lead_ms / 33));

                const auto capture_us = NowUs();
                auto probe = measurement_.BeginMediaProbe(
                    published_track_id_, nonce, probe_id, sequence,
                    {publication_started_us, capture_us, capture_us});
                if (!probe.accepted()) {
                    Finish(false, "media_probe_start_failed");
                    co_return;
                }
                outgoing_probes_.emplace(probe_id, ProbeRecord{
                    index, publication_started_us, capture_us, false});
                const auto marker = MakeFrame(livekit::telemetry::E2eMediaMarker{
                    probe_id, static_cast<std::uint32_t>(sequence)});
                if (!marker.data()) {
                    Finish(false, "media_marker_embedding_failed");
                    co_return;
                }
                for (int repeat = 0;
                     repeat < config_.marker_repeat_frames && !finished_ &&
                        !acknowledged_probes_.contains(probe_id);
                     ++repeat) {
                    Capture(marker);
                    ++marker_frames_sent_;
                    co_await Delay(33ms);
                }

                const auto probe_deadline = Clock::now() +
                    std::chrono::milliseconds(config_.probe_timeout_ms);
                const auto neutral = MakeFrame();
                while (!finished_ && !acknowledged_probes_.contains(probe_id) &&
                       Clock::now() < probe_deadline) {
                    co_await Delay(33ms);
                    Capture(neutral);
                }
                if (!acknowledged_probes_.contains(probe_id)) {
                    ++probe_timeouts_;
                    std::cout << "E2E_PROBE phase_id=" << config_.phase_id
                              << " codec=" << config_.codec
                              << " source_width=" << config_.width
                              << " source_height=" << config_.height
                              << " simulcast=" << (config_.simulcast ? "true" : "false")
                              << " quality=" << QualityName(config_.quality)
                              << " probe_index=" << index << " status=timeout\n";
                }
                co_await SendNeutralFrames(3);
            }

            if (finished_) co_return;
            const bool stats_complete = co_await CollectPublisherStats();
            PrintPublisherSummary();
            const bool complete = acknowledged_probes_.size() == config_.probes &&
                clock_valid_measurements_ == config_.probes &&
                stats_complete &&
                (!config_.shared_clock_ground_truth ||
                    (e2e02_error_us_.size() == config_.probes &&
                     e2e03_error_us_.size() == config_.probes));
            Finish(complete, complete ? "publisher_matrix_complete" :
                "publisher_matrix_incomplete");
        } catch (const std::exception&) {
            Finish(false, "publisher_failed");
        }
    }

    void HandleDecodedFrame(const std::string& track_id,
            const livekit::render::OwnedI420Frame::Ptr& frame,
            std::int64_t decoded_us) {
        if (finished_ || config_.role != Role::Receiver || !frame) return;
        ++decoded_frames_;
        last_received_width_ = frame->width();
        last_received_height_ = frame->height();
        const auto marker = livekit::telemetry::DecodeE2eMediaMarker(*frame);
        if (!marker) return;
        ++markers_decoded_;
        const auto token = next_render_token_++;
        auto observed = measurement_.ObserveDecodedMediaMarker(
            track_id, *marker, decoded_us, token);
        if (!observed.accepted()) {
            if (observed.status ==
                livekit::telemetry::E2eActionStatus::DuplicateOrUnknown)
                ++marker_duplicates_or_unknown_;
            else
                ++marker_invalid_context_;
            return;
        }
        ++markers_matched_;

        livekit::render::RenderFrameMetadata metadata;
        metadata.series_key = "e2e_runtime/" + track_id;
        metadata.room_generation = 1;
        metadata.binding_epoch = 1;
        metadata.frame_token = token;
        metadata.decoded_at = Clock::now();
        metadata.observer = std::make_shared<AckObserver>(
            weak_from_this(), track_id, static_cast<std::uint32_t>(frame->width()),
            static_cast<std::uint32_t>(frame->height()));
        auto copied = livekit::render::OwnedI420Frame::CopyFromPlanes(
            frame->width(), frame->height(), frame->data_y(), frame->stride_y(),
            frame->data_u(), frame->stride_u(), frame->data_v(), frame->stride_v(),
            frame->timestamp_us(), frame->rotation(), frame->color_space(),
            std::move(metadata));
        auto submitted = livekit::render::VideoRenderFrame::FromI420(std::move(copied));
        if (submitted)
            submitted->NotifyRendered("runtime_software_submit", Clock::now());
    }

    static void PrintDistribution(std::string_view name,
                                  const std::vector<std::int64_t>& values) {
        if (values.empty()) {
            std::cout << ' ' << name << "_samples=0"
                      << ' ' << name << "_mean_us=UNAVAILABLE"
                      << ' ' << name << "_p95_us=UNAVAILABLE"
                      << ' ' << name << "_max_us=UNAVAILABLE";
            return;
        }
        std::cout << ' ' << name << "_samples=" << values.size()
                  << ' ' << name << "_mean_us=" << Mean(values)
                  << ' ' << name << "_p95_us=" << Percentile(values, 0.95)
                  << ' ' << name << "_max_us="
                  << *std::max_element(values.begin(), values.end());
    }

    void PrintPublisherSummary() const {
        const double success_rate = static_cast<double>(acknowledged_probes_.size()) /
            static_cast<double>(config_.probes);
        std::cout << std::fixed << std::setprecision(3)
                  << "E2E_SUMMARY role=publisher"
                  << " phase_id=" << config_.phase_id
                  << " codec=" << config_.codec
                  << " requested_codec=" << resolved_requested_codec_
                  << " effective_codec=" << resolved_effective_codec_
                  << " observed_codecs=" << (observed_codecs_.empty()
                        ? "UNAVAILABLE" : Join(observed_codecs_))
                  << " fallback_reason=" << (fallback_reason_.empty()
                        ? "none" : "local_server_intersection")
                  << " source=" << config_.source
                  << " mode=" << resolved_mode_
                  << " scalability=" << (observed_scalability_.empty()
                        ? "UNAVAILABLE" : Join(observed_scalability_))
                  << " encoder_implementations=" << (observed_implementations_.empty()
                        ? "UNAVAILABLE" : Join(observed_implementations_))
                  << " backup_policy=" << BackupPolicyName(config_.backup_policy)
                  << " encoded_frames=" << encoded_frames_
                  << " sent_frames=" << sent_frames_
                  << " sent_packets=" << sent_packets_
                  << " connection_events=" << connection_events_
                  << " republish_events=" << republish_events_
                  << " reconnect_state=" << (republish_events_ > 0
                        ? "republished" : "not_observed")
                  << " source_width=" << config_.width
                  << " source_height=" << config_.height
                  << " simulcast=" << (config_.simulcast ? "true" : "false")
                  << " quality=" << QualityName(config_.quality)
                  << " probes=" << config_.probes
                  << " marker_repeat_frames=" << config_.marker_repeat_frames
                  << " probe_announcements_sent=" << probe_announcements_sent_
                  << " marker_frames_sent=" << marker_frames_sent_
                  << " marker_matched=" << acknowledged_probes_.size()
                  << " marker_timeouts=" << probe_timeouts_
                  << " marker_success_rate=" << success_rate
                  << " duplicate_acks=" << duplicate_acks_
                  << " invalid_acks=" << invalid_acks_
                  << " clock_responses=" << clock_responses_
                  << " clock_valid_measurements=" << clock_valid_measurements_;
        PrintDistribution("ack_rtt", ack_rtt_us_);
        PrintDistribution("clock_uncertainty", uncertainty_us_);
        PrintDistribution("e2e02", e2e02_us_);
        PrintDistribution("e2e03", e2e03_us_);
        PrintDistribution("e2e02_error", e2e02_error_us_);
        PrintDistribution("e2e03_error", e2e03_error_us_);
        PrintRange("received_width", received_widths_);
        PrintRange("received_height", received_heights_);
        std::cout << '\n';
    }

    static void PrintRange(std::string_view name,
                           const std::vector<std::int64_t>& values) {
        if (values.empty()) {
            std::cout << ' ' << name << "_min=UNAVAILABLE"
                      << ' ' << name << "_max=UNAVAILABLE";
            return;
        }
        const auto [minimum, maximum] = std::minmax_element(
            values.begin(), values.end());
        std::cout << ' ' << name << "_min=" << *minimum
                  << ' ' << name << "_max=" << *maximum;
    }

    void PrintReceiverSummary() const {
        std::cout << "E2E_SUMMARY role=receiver"
                  << " phase_id=" << config_.phase_id
                  << " codec=" << config_.codec
                  << " source=" << config_.source
                  << " simulcast=" << (config_.simulcast ? "true" : "false")
                  << " quality=" << QualityName(config_.quality)
                  << " probes_expected=" << config_.probes
                  << " probe_announcements=" << probe_announcements_accepted_
                  << " announcement_duplicates=" << probe_announcement_duplicates_
                  << " decoded_frames=" << decoded_frames_
                  << " first_frame_detected=" << (decoded_frames_ > 0 ? "true" : "false")
                  << " local_publish_codec=" << (config_.receiver_publish_codec.empty()
                        ? "none" : config_.receiver_publish_codec)
                  << " local_uplink_verified=" << (receiver_uplink_verified_ ? "true" : "false")
                  << " markers_decoded=" << markers_decoded_
                  << " markers_matched=" << markers_matched_
                  << " marker_duplicates_or_unknown=" << marker_duplicates_or_unknown_
                  << " marker_invalid_context=" << marker_invalid_context_
                  << " acks_sent=" << acks_sent_
                  << " quality_control_accepted="
                  << (quality_control_accepted_ ? "true" : "false")
                  << " received_width=" << last_received_width_
                  << " received_height=" << last_received_height_ << '\n';
    }

    void Finish(bool success, std::string reason) {
        if (finished_) return;
        if (success && !config_.receiver_publish_codec.empty() && !receiver_uplink_verified_) {
            success = false;
            reason = "receiver_uplink_not_verified";
        }
        finished_ = true;
        success_.store(success);
        if (config_.role == Role::Receiver) PrintReceiverSummary();
        std::cout << "E2E_RUNTIME " << (success ? "PASS" : "FAIL")
                  << " role=" << (config_.role == Role::Publisher ? "publisher" : "receiver")
                  << " phase_id=" << config_.phase_id << " reason=" << reason << '\n';
        std::error_code ignored;
        deadline_.cancel(ignored);
        capability_timer_.cancel(ignored);
        finish_timer_.expires_after(success ? 1000ms : 0ms);
        finish_timer_.async_wait([self = shared_from_this()](const std::error_code&) {
            self->room_->Disconnect();
            self->io_.stop();
        });
    }

    asio::io_context& io_;
    asio::strand<asio::io_context::executor_type> strand_;
    Config config_;
    std::shared_ptr<livekit::Room> room_;
    livekit::telemetry::E2eMeasurementSession measurement_;
    asio::steady_timer deadline_;
    asio::steady_timer capability_timer_;
    asio::steady_timer finish_timer_;
    std::vector<livekit::Track::I420VideoFrameSubscription> subscriptions_;
    std::shared_ptr<livekit::RemoteTrackPublication> remote_publication_;
    std::shared_ptr<livekit::VideoSource> video_source_;
    std::string published_track_id_;
    std::unordered_map<std::uint64_t, ProbeRecord> outgoing_probes_;
    std::unordered_set<std::uint64_t> acknowledged_probes_;
    std::vector<std::int64_t> ack_rtt_us_, uncertainty_us_, e2e02_us_, e2e03_us_;
    std::vector<std::int64_t> e2e02_error_us_, e2e03_error_us_;
    std::vector<std::int64_t> received_widths_, received_heights_;
    std::vector<std::string> observed_codecs_;
    std::vector<std::string> observed_implementations_;
    std::vector<std::string> observed_scalability_;
    std::string resolved_requested_codec_;
    std::string resolved_effective_codec_;
    std::string fallback_reason_;
    std::string resolved_mode_;
    std::uint64_t next_sequence_ = 1;
    std::uint64_t next_render_token_ = 1;
    std::size_t clock_responses_ = 0;
    std::size_t probe_announcements_sent_ = 0;
    std::size_t probe_announcements_accepted_ = 0;
    std::size_t probe_announcement_duplicates_ = 0;
    std::size_t marker_frames_sent_ = 0;
    std::size_t probe_timeouts_ = 0;
    std::size_t duplicate_acks_ = 0;
    std::size_t invalid_acks_ = 0;
    std::size_t clock_valid_measurements_ = 0;
    std::size_t decoded_frames_ = 0;
    std::size_t markers_decoded_ = 0;
    std::size_t markers_matched_ = 0;
    std::size_t marker_duplicates_or_unknown_ = 0;
    std::size_t marker_invalid_context_ = 0;
    std::size_t acks_sent_ = 0;
    std::uint64_t encoded_frames_ = 0;
    std::uint64_t sent_frames_ = 0;
    std::uint64_t sent_packets_ = 0;
    std::size_t connection_events_ = 0;
    std::size_t republish_events_ = 0;
    int last_received_width_ = 0;
    int last_received_height_ = 0;
    bool connected_ = false;
    bool quality_control_accepted_ = false;
    bool publisher_started_ = false;
    bool receiver_publish_started_ = false;
    bool receiver_publish_ready_ = false;
    bool receiver_uplink_verified_ = false;
    bool finished_ = false;
    std::atomic<bool> success_{false};
};

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--list-codecs") {
        return RunLocalMode(false);
    }
    if (argc == 2 && std::string_view(argv[1]) == "--self-test") {
        return RunLocalMode(true);
    }
    if (argc == 2 && (std::string_view(argv[1]) == "--help" ||
                      std::string_view(argv[1]) == "-h")) {
        Usage(argv[0]);
        return EXIT_SUCCESS;
    }
    const auto config = Parse(argc, argv);
    if (!config) {
        Usage(argv[0]);
        return argc > 1 ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    asio::io_context io;
    auto runtime = std::make_shared<E2eMediaRuntime>(io, *config);
    runtime->Start();
    io.run();
    return runtime->success() ? EXIT_SUCCESS : EXIT_FAILURE;
}
