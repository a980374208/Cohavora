#include "detail/room_internal.h"
#include "telemetry/callback_phase_diagnostic.h"
#include "webrtc_manager.h"
#include "render/owned_i420_frame.h"
#include "render/remote_video_frame_pipeline.h"
#include "livekit_rtc.pb.h"
#include "livekit_models.pb.h"
#include "api/jsep.h"
#include "api/video/video_sink_interface.h"
#include "pc/session_description.h"
#if COHAVORA_ENABLE_MEDIA_DIAGNOSTICS
#include "api/make_ref_counted.h"
#endif

#include <algorithm>
#include <atomic>
#include <limits>

namespace livekit {

namespace {

proto::VideoQuality VideoQualityFromPublishedLayer(PublishedVideoQuality quality) {
    switch (quality) {
    case PublishedVideoQuality::Low: return proto::VideoQuality::LOW;
    case PublishedVideoQuality::Medium: return proto::VideoQuality::MEDIUM;
    case PublishedVideoQuality::High: return proto::VideoQuality::HIGH;
    }
    return proto::VideoQuality::OFF;
}

telemetry::MediaExpectationReason ToTelemetryRenderReason(
        render::RenderExpectationReason reason) {
    switch (reason) {
    case render::RenderExpectationReason::SurfaceVisible:
        return telemetry::MediaExpectationReason::SurfaceVisible;
    case render::RenderExpectationReason::WindowMinimized:
        return telemetry::MediaExpectationReason::WindowMinimized;
    case render::RenderExpectationReason::StreamPaused:
        return telemetry::MediaExpectationReason::StreamPaused;
    case render::RenderExpectationReason::BindingEnded:
        return telemetry::MediaExpectationReason::RenderBindingEnded;
    case render::RenderExpectationReason::SurfaceHidden:
        return telemetry::MediaExpectationReason::SurfaceHidden;
    }
    return telemetry::MediaExpectationReason::SurfaceHidden;
}

class TelemetryRenderSubmitObserver final : public render::RenderSubmitObserver {
public:
    TelemetryRenderSubmitObserver(
        std::weak_ptr<telemetry::SessionTelemetry> telemetry,
        std::string series_key,
        std::uint64_t room_generation,
        std::uint64_t binding_epoch,
        std::shared_ptr<telemetry::RenderActivityProbe> probe)
        : telemetry_(std::move(telemetry))
        , series_key_(std::move(series_key))
        , room_generation_(room_generation)
        , binding_epoch_(binding_epoch)
        , probe_(std::move(probe)) {}

    void SetExpected(
            bool expected,
            render::RenderExpectationReason reason,
            Clock::time_point source_time) override {
        if (!probe_ || !probe_->active.load(std::memory_order_acquire)) return;
        const bool previous = probe_->expected.exchange(
            expected, std::memory_order_acq_rel);
        if (previous == expected) return;
        // A hidden interval must never be charged as a visible render stall.
        probe_->last_submit_ns.store(0, std::memory_order_release);
        if (const auto telemetry = telemetry_.lock()) {
            telemetry->SetRemoteVideoRenderExpected(
                series_key_, room_generation_, binding_epoch_, probe_, expected,
                ToTelemetryRenderReason(reason), source_time);
        }
    }

    void OnSubmitted(
            const render::RenderFrameMetadata& metadata,
            const char* measurement_point,
            Clock::time_point source_time) override {
        if (!probe_ || !probe_->active.load(std::memory_order_acquire) ||
            metadata.series_key != series_key_ ||
            metadata.room_generation != room_generation_ ||
            metadata.binding_epoch != binding_epoch_) {
            return;
        }
        if (const auto telemetry = telemetry_.lock()) {
            telemetry->RecordRemoteVideoRenderSubmit(
                series_key_, room_generation_, binding_epoch_,
                metadata.frame_token, metadata.decoded_at,
                measurement_point ? measurement_point : "render_submit",
                probe_, source_time);
        }
    }

    void OnStageTiming(
            const render::RenderFrameMetadata& metadata,
            const char* measurement_point,
            std::chrono::microseconds duration,
            Clock::time_point) override {
        if (!probe_ || !probe_->active.load(std::memory_order_acquire) ||
            metadata.series_key != series_key_ ||
            metadata.room_generation != room_generation_ ||
            metadata.binding_epoch != binding_epoch_ || !measurement_point ||
            duration.count() < 0) {
            return;
        }
        auto* target = &probe_->draw_submit;
        const std::string_view point(measurement_point);
        if (point == "qt_cpu_convert") {
            target = &probe_->cpu_convert;
        } else if (point.find("upload") != std::string_view::npos) {
            target = &probe_->upload_submit;
        } else if (point != "qt_cpu_paint") {
            // Whole-scene GPU draw/present costs use the session canvas sink.
            return;
        }
        target->samples.fetch_add(1, std::memory_order_relaxed);
        target->total_us.fetch_add(duration.count(), std::memory_order_relaxed);
        auto maximum = target->maximum_us.load(std::memory_order_relaxed);
        while (duration.count() > maximum &&
               !target->maximum_us.compare_exchange_weak(
                   maximum, duration.count(), std::memory_order_relaxed)) {
        }
    }

    std::shared_ptr<render::CanvasRenderTimingObserver> CanvasTimingObserver(
            const render::RenderFrameMetadata& metadata) override {
        if (!probe_ || !probe_->active.load(std::memory_order_acquire) ||
            metadata.series_key != series_key_ ||
            metadata.room_generation != room_generation_ ||
            metadata.binding_epoch != binding_epoch_) {
            return {};
        }
        if (const auto telemetry = telemetry_.lock()) {
            return telemetry->canvasRenderProbe();
        }
        return {};
    }

private:
    std::weak_ptr<telemetry::SessionTelemetry> telemetry_;
    std::string series_key_;
    std::uint64_t room_generation_ = 0;
    std::uint64_t binding_epoch_ = 0;
    std::shared_ptr<telemetry::RenderActivityProbe> probe_;
};

} // namespace

void Room::SetRemoteMediaRecoveryHandler(RemoteMediaRecoveryHandler handler) {
    std::lock_guard lock(room_mutex_);
    remote_media_recovery_handler_ = std::move(handler);
}

std::optional<RemoteMediaRecoveryRequest> Room::remote_media_recovery_request() const {
    std::lock_guard lock(room_mutex_);
    if (!remote_media_recovery_pending_) return std::nullopt;
    return RemoteMediaRecoveryRequest{
        remote_media_coordinator_session_,
        installed_session_generation_,
        remote_media_catalog_revision_,
        remote_media_recovery_epoch_,
        remote_media_recovery_token_};
}

ControlApplyResult Room::ApplyRemoteMediaPlan(const RemoteMediaPlan& plan) {
    ControlApplyResult result;
    std::array<bool, 4> subscription_changes{};
    std::vector<std::pair<std::shared_ptr<RemoteTrackPublication>, uint64_t>> detach;
    result.coordinator_session = plan.coordinator_session;
    result.native_room_generation = plan.native_room_generation;
    result.catalog_revision = plan.catalog_revision;
    result.policy_revision = plan.policy_revision;
    result.recovery_epoch = plan.recovery_epoch;
    result.recovery_token = plan.recovery_token;

    {
        std::lock_guard lock(room_mutex_);
        const auto current_generation =
            session_generation_.load(std::memory_order_acquire);
        if (retired()) {
            result.reason = ControlRejectionReason::Retired;
            return result;
        }
        if (plan.native_room_generation == 0 ||
            plan.native_room_generation != current_generation ||
            plan.native_room_generation != installed_session_generation_) {
            result.reason = ControlRejectionReason::StaleRoomGeneration;
            return result;
        }
        if (remote_media_plan_active_ &&
            plan.coordinator_session != remote_media_coordinator_session_) {
            result.reason = ControlRejectionReason::StaleSession;
            return result;
        }
        if (remote_media_native_generation_ != 0 &&
            plan.native_room_generation < remote_media_native_generation_) {
            result.reason = ControlRejectionReason::StaleRoomGeneration;
            return result;
        }
        const bool new_native_generation =
            remote_media_native_generation_ != 0 &&
            plan.native_room_generation != remote_media_native_generation_;
        const auto catalog_floor = new_native_generation
            ? uint64_t{0} : remote_media_catalog_revision_;
        const auto policy_floor = new_native_generation
            ? uint64_t{0} : remote_media_policy_revision_;
        if (plan.catalog_revision < catalog_floor) {
            result.reason = ControlRejectionReason::StaleCatalog;
            return result;
        }
        if (plan.policy_revision < policy_floor) {
            result.reason = ControlRejectionReason::StalePolicy;
            return result;
        }
        if (remote_media_recovery_pending_) {
            if (plan.recovery_epoch != remote_media_recovery_epoch_ ||
                plan.recovery_token != remote_media_recovery_token_) {
                result.reason = ControlRejectionReason::StaleRecovery;
                return result;
            }
        } else if (plan.recovery_epoch != 0 || !plan.recovery_token.empty()) {
            result.reason = ControlRejectionReason::StaleRecovery;
            return result;
        }
        if (plan.video.size() > kMaximumRemoteVideoTracks) {
            result.reason = ControlRejectionReason::InvalidDemand;
            return result;
        }

        struct ResolvedPublication {
            TrackKey track_key;
            SubscriptionIntentKey key;
            std::shared_ptr<RemoteTrackPublication> publication;
        };
        struct ResolvedDemand {
            const RemoteTrackDemand* demand = nullptr;
            SubscriptionIntentKey key;
            std::shared_ptr<RemoteTrackPublication> publication;
        };
        std::vector<ResolvedPublication> known;
        std::vector<ResolvedDemand> video;
        std::vector<ResolvedDemand> audio;
        known.reserve(plan.known_publications.size());
        const auto resolve_known = [&](const TrackKey& track_key) {
            const auto participant = remote_participants_.find(
                track_key.participant.sid);
            if (participant == remote_participants_.end() ||
                !participant->second ||
                participant->second->identity() != track_key.participant.identity) {
                result.rejected.push_back(track_key);
                if (result.reason == ControlRejectionReason::None)
                    result.reason = ControlRejectionReason::MissingPublication;
                return;
            }
            const auto publication = participant->second->get_remote_publication(
                track_key.publication_sid);
            const auto membership = publication
                ? track_memberships_.find(publication.get())
                : track_memberships_.end();
            if (!publication || !publication->track() ||
                membership == track_memberships_.end() ||
                !membership->second || membership->second->key != track_key ||
                !membership->second->active.load(std::memory_order_acquire)) {
                result.rejected.push_back(track_key);
                if (result.reason == ControlRejectionReason::None)
                    result.reason = ControlRejectionReason::MissingPublication;
                return;
            }
            known.push_back({
                track_key,
                MakeSubscriptionIntentKeyLocked(
                    participant->first, participant->second->identity(),
                    publication->sid()),
                publication});
        };
        for (const auto& key : plan.known_publications) {
            if (std::any_of(known.begin(), known.end(), [&](const auto& current) {
                    return current.track_key == key;
                })) {
                result.rejected.push_back(key);
                result.reason = ControlRejectionReason::InvalidDemand;
                continue;
            }
            resolve_known(key);
        }
        if (!result.rejected.empty()) return result;

        const auto find_known = [&](const TrackKey& key) {
            return std::find_if(known.begin(), known.end(), [&](const auto& current) {
                return current.track_key == key;
            });
        };
        const auto resolve_selected = [&](const TrackKey& track_key,
                                          TrackKind expected_kind,
                                          const RemoteTrackDemand* demand,
                                          std::vector<ResolvedDemand>& target) {
            const auto resolved = find_known(track_key);
            if (resolved == known.end() ||
                resolved->publication->track()->kind() != expected_kind) {
                result.rejected.push_back(track_key);
                result.reason = ControlRejectionReason::InvalidDemand;
                return;
            }
            if (!resolved->publication->subscription_allowed()) {
                result.rejected.push_back(track_key);
                result.reason = ControlRejectionReason::PermissionDenied;
                return;
            }
            target.push_back({
                demand,
                resolved->key,
                resolved->publication});
        };

        for (const auto& demand : plan.video) {
            if (!demand.subscribed || !demand.enabled ||
                demand.quality == VideoQualityTier::None ||
                demand.width == 0 || demand.height == 0 ||
                std::any_of(video.begin(), video.end(), [&](const auto& current) {
                    return current.demand && current.demand->key == demand.key;
                })) {
                result.rejected.push_back(demand.key);
                result.reason = ControlRejectionReason::InvalidDemand;
                continue;
            }
            resolve_selected(demand.key, TrackKind::Video, &demand, video);
        }
        for (const auto& key : plan.audio) {
            if (std::any_of(audio.begin(), audio.end(), [&](const auto& current) {
                    return current.publication &&
                        track_memberships_.at(current.publication.get())->key == key;
                })) {
                result.rejected.push_back(key);
                result.reason = ControlRejectionReason::InvalidDemand;
                continue;
            }
            resolve_selected(key, TrackKind::Audio, nullptr, audio);
        }
        if (!result.rejected.empty()) return result;

        const auto contains_key = [](const auto& values,
                                     const SubscriptionIntentKey& key) {
            return std::any_of(values.begin(), values.end(), [&](const auto& value) {
                return value.key.participant_sid == key.participant_sid &&
                    value.key.participant_identity == key.participant_identity &&
                    value.key.track_sid == key.track_sid;
            });
        };
        const bool repeated_revision = remote_media_plan_active_ &&
            !new_native_generation &&
            plan.policy_revision == remote_media_policy_revision_;
        bool changed = false;

        for (const auto& scoped : known) {
            const auto& publication = scoped.publication;
            const auto& key = scoped.key;
            auto& intent = EnsureSubscriptionIntentLocked(key);
            const bool is_video =
                publication->track()->kind() == TrackKind::Video;
            const bool selected = is_video
                ? contains_key(video, key) : contains_key(audio, key);
            bool enabled = intent.enabled;
            auto quality = intent.quality;
            uint32_t width = intent.width;
            uint32_t height = intent.height;
            uint32_t max_fps = intent.max_fps;
            uint32_t priority = intent.priority;
            if (is_video) {
                if (selected) {
                    const auto found = std::find_if(
                        video.begin(), video.end(), [&](const auto& value) {
                            return value.key.participant_sid == key.participant_sid &&
                                value.key.participant_identity == key.participant_identity &&
                                value.key.track_sid == key.track_sid;
                        });
                    const auto& demand = *found->demand;
                    enabled = demand.enabled;
                    quality = demand.selected_layer_quality
                        ? VideoQualityFromPublishedLayer(*demand.selected_layer_quality)
                        : VideoQualityForTier(demand.quality);
                    width = demand.width;
                    height = demand.height;
                    max_fps = demand.max_fps.value_or(0);
                    priority = demand.priority;
                } else {
                    enabled = false;
                    quality = proto::VideoQuality::OFF;
                    width = 0;
                    height = 0;
                    max_fps = 0;
                    priority = 0;
                }
            }
            const bool subscription_changed = intent.subscribed != selected;
            const bool settings_changed = is_video && !IsVideoSettingsEqual(
                intent, enabled, quality, width, height, max_fps, priority);
            changed = changed || subscription_changed || settings_changed;
            if (repeated_revision && (subscription_changed || settings_changed)) {
                result.reason = ControlRejectionReason::StalePolicy;
                return result;
            }
        }

        remote_media_plan_active_ = true;
        remote_media_coordinator_session_ = plan.coordinator_session;
        remote_media_native_generation_ = plan.native_room_generation;
        remote_media_catalog_revision_ = plan.catalog_revision;
        remote_media_policy_revision_ = plan.policy_revision;

        if (!repeated_revision || changed) {
            for (const auto& scoped : known) {
                const auto& publication = scoped.publication;
                const auto& key = scoped.key;
                auto& intent = EnsureSubscriptionIntentLocked(key);
                const bool is_video =
                    publication->track()->kind() == TrackKind::Video;
                const bool selected = is_video
                    ? contains_key(video, key) : contains_key(audio, key);
                bool enabled = intent.enabled;
                auto quality = intent.quality;
                uint32_t width = intent.width;
                uint32_t height = intent.height;
                uint32_t max_fps = intent.max_fps;
                uint32_t priority = intent.priority;
                if (is_video) {
                    if (selected) {
                        const auto found = std::find_if(
                            video.begin(), video.end(), [&](const auto& value) {
                                return value.key.participant_sid == key.participant_sid &&
                                    value.key.participant_identity == key.participant_identity &&
                                    value.key.track_sid == key.track_sid;
                            });
                        const auto& demand = *found->demand;
                        enabled = demand.enabled;
                        quality = demand.selected_layer_quality
                            ? VideoQualityFromPublishedLayer(*demand.selected_layer_quality)
                            : VideoQualityForTier(demand.quality);
                        width = demand.width;
                        height = demand.height;
                        max_fps = demand.max_fps.value_or(0);
                        priority = demand.priority;
                    } else {
                        enabled = false;
                        quality = proto::VideoQuality::OFF;
                        width = height = max_fps = priority = 0;
                    }
                }
                const bool subscription_changed = intent.subscribed != selected;
                if (subscription_changed) {
                    const auto index = (is_video ? 2u : 0u) + (selected ? 1u : 0u);
                    subscription_changes[index] = true;
                }
                const bool settings_changed = is_video && !IsVideoSettingsEqual(
                    intent, enabled, quality, width, height, max_fps, priority);
                if (!subscription_changed && !settings_changed) continue;

                const auto existing_pending = pending_subscription_updates_.find(key);
                const bool subscribe_in_flight =
                    existing_pending != pending_subscription_updates_.end() &&
                    existing_pending->second.in_flight &&
                    existing_pending->second.subscribed;
                const bool server_may_be_subscribed =
                    intent.sent_subscribed.value_or(session_auto_subscribe_) ||
                    subscribe_in_flight;
                const auto retire_unsubscribed_binding = [&]() {
                    if (publication->track()) {
                        SetRemoteMediaExpected(
                            session_telemetry_.lock(), publication->track()->kind(),
                            key.participant_sid, publication->sid(), false,
                            telemetry::MediaExpectationReason::Unsubscribed);
                    }
                    const auto binding =
                        current_remote_binding_serials_.find(publication.get());
                    if (binding != current_remote_binding_serials_.end()) {
                        if (const auto media =
                                FindMediaBindingLocked(binding->second)) {
                            media->active.store(false, std::memory_order_release);
                        }
                        detach.push_back({publication, binding->second});
                    }
                };
                if (!selected && !server_may_be_subscribed) {
                    CancelPendingSubscriptionUpdateLocked(key);
                    intent.subscribed = false;
                    intent.enabled = false;
                    intent.quality = proto::VideoQuality::OFF;
                    intent.width = 0;
                    intent.height = 0;
                    intent.max_fps = 0;
                    intent.priority = 0;
                    intent.revision = next_subscription_revision_++;
                    intent.policy_revision = plan.policy_revision;
                    intent.subscription_dirty = false;
                    intent.settings_dirty = false;
                    intent.accepted_at = {};

                    RemotePublicationControlRequest control;
                    control.sequence = 0;
                    control.kind = RemotePublicationControlRequest::Kind::Subscription;
                    control.subscribed = false;
                    control.enabled = false;
                    control.quality = proto::VideoQuality::OFF;
                    control.width = 0;
                    control.height = 0;
                    control.priority = 0;
                    publication->CommitControl(control);
                    retire_unsubscribed_binding();
                    continue;
                }

                const bool subscription_dirty =
                    subscription_changed || intent.subscription_dirty ||
                    subscribe_in_flight ||
                    !intent.sent_subscribed.has_value() ||
                    *intent.sent_subscribed != selected;
                const bool settings_dirty = is_video &&
                    (settings_changed || intent.settings_dirty);

                intent.subscribed = selected;
                intent.enabled = enabled;
                intent.quality = quality;
                intent.width = width;
                intent.height = height;
                intent.max_fps = max_fps;
                intent.priority = priority;
                intent.revision = next_subscription_revision_++;
                intent.policy_revision = plan.policy_revision;
                intent.subscription_dirty = subscription_dirty;
                intent.settings_dirty = settings_dirty;
                intent.accepted_at = selected
                    ? std::chrono::steady_clock::now()
                    : std::chrono::steady_clock::time_point{};

                std::shared_ptr<SubscriptionTelemetryOperation> operation;
                if (subscription_changed) {
                    if (const auto sink = session_telemetry_.lock()) {
                        operation = std::make_shared<SubscriptionTelemetryOperation>(
                            sink, selected ? telemetry::OperationKind::Subscribe
                                           : telemetry::OperationKind::Unsubscribe);
                    }
                }
                QueueSubscriptionUpdateLocked(key);
                auto pending = pending_subscription_updates_.find(key);
                if (subscription_changed &&
                    pending != pending_subscription_updates_.end()) {
                    pending->second.telemetry_operation = std::move(operation);
                }

                RemotePublicationControlRequest control;
                control.sequence = 0;
                control.kind = RemotePublicationControlRequest::Kind::Subscription;
                control.subscribed = selected;
                control.enabled = enabled;
                control.quality = quality;
                control.width = width;
                control.height = height;
                control.priority = priority;
                publication->CommitControl(control);
                // A real unsubscribe -> subscribe transition is a new attempt.
                // Settings-only policy revisions must preserve server failures.
                if (subscription_changed && selected) {
                    ClearTrackSubscriptionErrorLocked(
                        remote_participants_.at(key.participant_sid), publication);
                    if (publication->has_media_binding() && publication->track()) {
                        SetRemoteMediaExpected(
                            session_telemetry_.lock(), publication->track()->kind(),
                            key.participant_sid, publication->sid(),
                            IsRemoteMediaExpected(publication->SnapshotState(), true),
                            telemetry::MediaExpectationReason::Subscribed);
                    }
                } else if (subscription_changed && !selected) {
                    if (publication->track()) {
                        SetRemoteMediaExpected(
                            session_telemetry_.lock(), publication->track()->kind(),
                            key.participant_sid, publication->sid(), false,
                            telemetry::MediaExpectationReason::Unsubscribed);
                    }
                }
                if (!selected) {
                    retire_unsubscribed_binding();
                }

            }
        }
        if (remote_media_recovery_pending_) CompleteRemoteMediaRecoveryLocked();
        result.accepted = true;
        result.pending_reconnect =
            connection_state_ == ConnectionState::Reconnecting ||
            reconnect_active_;
        result.pending_send = !pending_subscription_updates_.empty();
        ScheduleSubscriptionDrainLocked();
    }

    for (const auto& [publication, binding] : detach) {
        DetachRemotePublicationMedia(
            publication.get(), /*notify_listener=*/true, binding);
    }

    if (result.accepted) {
        for (std::size_t index = 0; index < subscription_changes.size(); ++index) {
            if (!subscription_changes[index]) continue;
            diagnostic::Event event;
            event.kind = diagnostic::EventKind::MediaSubscriptionChanged;
            event.media_kind = index >= 2 ? diagnostic::MediaKind::Video
                                          : diagnostic::MediaKind::Audio;
            event.subscription_state = index % 2 == 1
                ? diagnostic::SubscriptionState::Subscribed
                : diagnostic::SubscriptionState::Unsubscribed;
            EmitDiagnostic(event);
        }
    }

    return result;
}


namespace {

class NativeAudioTrackSink : public webrtc::AudioTrackSinkInterface {
public:
    using Clock = std::chrono::steady_clock;

    explicit NativeAudioTrackSink(
        std::function<void(Clock::time_point, const AudioFrame&)> callback)
        : callback_(std::move(callback)) {}

    void OnData(const void* audio_data,
                int bits_per_sample,
                int sample_rate,
                size_t number_of_channels,
                size_t number_of_frames) override {
        if (!callback_ || !audio_data || bits_per_sample != 16 ||
            sample_rate <= 0 || number_of_channels == 0 || number_of_frames == 0 ||
            number_of_channels > static_cast<size_t>((std::numeric_limits<int>::max)()) ||
            number_of_frames > static_cast<size_t>((std::numeric_limits<int>::max)()) ||
            number_of_frames > static_cast<size_t>((std::numeric_limits<int>::max)()) /
                number_of_channels) {
            return;
        }
        const auto source_time = Clock::now();

        const auto num_samples = static_cast<int>(
            number_of_frames * number_of_channels);

        AudioFrame frame = AudioFrame::create(sample_rate, static_cast<int>(number_of_channels), static_cast<int>(number_of_frames));
        const int16_t* pcm_data = static_cast<const int16_t*>(audio_data);
        if (!frame.data().empty()) {
            std::copy(pcm_data, pcm_data + num_samples, frame.data().begin());
        }

        ///********/
        //double sum = 0.0;
        //for (int i = 0; i < num_samples; ++i) {
        //    sum += static_cast<double>(pcm_data[i]) * pcm_data[i];
        //}
        //double rms = std::sqrt(sum / num_samples);
        //// 打印音量诊断（大于 300 说明有明显说话声）
        //if (rms > 300.0) {
        //    std::cout << "[AUDIO ACTIVE VOICE] 正在说话! 音量 RMS = " << rms << std::endl;
        //}
        ///*********/



        callback_(source_time, frame);
    }

private:
    std::function<void(Clock::time_point, const AudioFrame&)> callback_;
};

class NativeVideoTrackSink : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    using Clock = std::chrono::steady_clock;
    using MetadataFactory =
        std::function<render::RenderFrameMetadata(Clock::time_point)>;

    explicit NativeVideoTrackSink(
        std::function<Track::I420VideoFrameDelivery(Clock::time_point, int, int, bool)> callback,
        std::shared_ptr<telemetry::VideoActivityProbe> activity_probe,
        MetadataFactory metadata_factory = {},
        std::function<void(std::chrono::nanoseconds)> duration_probe = {},
        std::function<void(std::chrono::nanoseconds, std::chrono::nanoseconds)> phase_probe = {}
#if COHAVORA_ENABLE_MEDIA_DIAGNOSTICS
        , std::shared_ptr<const E2eeManager::MediaObservation> trace_observation = {}
        , uint64_t trace_binding_serial = 0
#endif
        )
        : callback_(std::move(callback))
        , activity_probe_(std::move(activity_probe))
        , metadata_factory_(std::move(metadata_factory))
        , duration_probe_(std::move(duration_probe))
        , phase_probe_(std::move(phase_probe))
#if COHAVORA_ENABLE_MEDIA_DIAGNOSTICS
        , trace_observation_(std::move(trace_observation))
        , trace_binding_serial_(trace_binding_serial)
#endif
    {}

    ~NativeVideoTrackSink() override { Close(); }
    void SetPipeline(std::shared_ptr<render::RemoteVideoFramePipeline> pipeline,
                     std::shared_ptr<render::RemoteVideoFrameEndpoint> endpoint) {
        pipeline_ = std::move(pipeline);
        endpoint_ = std::move(endpoint);
    }
    void Close() {
        if (endpoint_) endpoint_->Close();
        endpoint_.reset();
        // RemoveSink drains ingress before native cleanup joins the last worker.
        pipeline_.reset();
    }

    void OnFrame(const webrtc::VideoFrame& rtc_frame) override {
        if (!callback_) return;
        const auto source_time = Clock::now();
        telemetry::callback_diagnostic::FrameScope diagnostic_trace(source_time);
        using DiagnosticPoint = telemetry::callback_diagnostic::FramePoint;

        // Do not run an I420-to-RGBA pixel loop on WebRTC's media worker.
        // CopyFrom preserves stride-correct planes and metadata in a frame that
        // can safely outlive the decoder buffer.
        telemetry::callback_diagnostic::Mark(DiagnosticPoint::MetadataBegin);
        auto metadata = metadata_factory_
            ? metadata_factory_(source_time) : render::RenderFrameMetadata{};
        telemetry::callback_diagnostic::Mark(DiagnosticPoint::MetadataEnd);
        diagnostic_trace.Identity(metadata.room_generation, metadata.binding_epoch,
            metadata.frame_token, rtc_frame.width(), rtc_frame.height());
        Clock::time_point copied_at{};
        const bool retained = endpoint_ && endpoint_->Offer(rtc_frame, metadata, activity_probe_);
        auto delivery = callback_(source_time, rtc_frame.width(), rtc_frame.height(), retained);
        activity_probe_->admission_rejected_rate.fetch_add(delivery.rejected_rate, std::memory_order_relaxed);
        activity_probe_->admission_rejected_inactive.fetch_add(delivery.rejected_inactive, std::memory_order_relaxed);
        activity_probe_->admission_rejected_busy.fetch_add(delivery.rejected_busy, std::memory_order_relaxed);
        if (delivery.empty()) {
            if (!retained) activity_probe_->precopy_skipped.fetch_add(1, std::memory_order_relaxed);
        } else {
            activity_probe_->copy_attempts.fetch_add(1, std::memory_order_relaxed);
            telemetry::callback_diagnostic::Mark(DiagnosticPoint::CopyBegin);
            if (auto frame = render::OwnedI420Frame::CopyFrom(
                    rtc_frame, std::move(metadata))) {
                telemetry::callback_diagnostic::Mark(DiagnosticPoint::CopyEnd);
                activity_probe_->delivered_frame_count.fetch_add(1, std::memory_order_relaxed);
                activity_probe_->copied_bytes.fetch_add(
                    uint64_t(frame->width()) * frame->height() +
                    2 * uint64_t(frame->chroma_width()) * frame->chroma_height(), std::memory_order_relaxed);
                if (phase_probe_) copied_at = Clock::now();
#if COHAVORA_ENABLE_MEDIA_DIAGNOSTICS
                if (trace_observation_ && trace_observation_->frame_trace &&
                    !trace_observation_->retired.load(std::memory_order_acquire) &&
                    (!trace_observation_->binding_active ||
                     trace_observation_->binding_active->load(std::memory_order_acquire))) {
                    MediaFrameTraceRecord record;
                    record.phase = MediaFrameTracePhase::CopiedForDelivery;
                    record.binding_id = trace_observation_->binding_id;
                    record.generation = trace_observation_->generation;
                    record.room_binding_serial = trace_binding_serial_;
                    record.install_epoch = trace_observation_->install_epoch->load(std::memory_order_acquire);
                    record.rtp_timestamp = rtc_frame.rtp_timestamp();
                    record.timestamp_us = rtc_frame.timestamp_us();
                    record.width = rtc_frame.width();
                    record.height = rtc_frame.height();
                    trace_observation_->frame_trace->Record(record);
                }
#endif
                telemetry::callback_diagnostic::Mark(DiagnosticPoint::DispatchBegin);
                delivery.Deliver(frame);
                telemetry::callback_diagnostic::Mark(DiagnosticPoint::DispatchEnd);
            } else {
                activity_probe_->copy_failures.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (duration_probe_ || phase_probe_ || diagnostic_trace) {
            const auto finished_at = Clock::now();
            diagnostic_trace.Complete(finished_at);
            if (duration_probe_) duration_probe_(finished_at - source_time);
            if (phase_probe_ && copied_at != Clock::time_point{})
                phase_probe_(copied_at - source_time, finished_at - copied_at);
        }
    }

private:
    std::function<Track::I420VideoFrameDelivery(Clock::time_point, int, int, bool)> callback_;
    std::shared_ptr<render::RemoteVideoFramePipeline> pipeline_;
    std::shared_ptr<render::RemoteVideoFrameEndpoint> endpoint_;
    std::shared_ptr<telemetry::VideoActivityProbe> activity_probe_;
    MetadataFactory metadata_factory_;
    std::function<void(std::chrono::nanoseconds)> duration_probe_;
    std::function<void(std::chrono::nanoseconds, std::chrono::nanoseconds)> phase_probe_;
#if COHAVORA_ENABLE_MEDIA_DIAGNOSTICS
    std::shared_ptr<const E2eeManager::MediaObservation> trace_observation_;
    uint64_t trace_binding_serial_ = 0;
#endif
};

struct VideoFrameTelemetryGate {
    std::atomic<bool> first_frame_submitted{false};
    std::atomic<std::uint64_t> recovery_epoch{0};
    std::atomic<std::int64_t> recovery_first_ns{0};
    std::atomic<std::int64_t> recovery_last_good_ns{0};
    std::atomic<bool> recovery_stable_submitted{false};
};

struct AudioFrameTelemetryGate {
    std::atomic<bool> first_frame_submitted{false};
    std::atomic<std::uint64_t> recovery_epoch{0};
    std::atomic<std::int64_t> recovery_first_ns{0};
    std::atomic<std::int64_t> recovery_last_good_ns{0};
    std::atomic<bool> recovery_stable_submitted{false};
};

} // namespace

void Room::DetachRemoteTrackSinks(std::vector<RemoteTrackSinkBinding> bindings) noexcept {
    for (auto& binding : bindings) {
        if (binding.media_binding) {
            binding.media_binding->active.store(false, std::memory_order_release);
        }
#if COHAVORA_ENABLE_MEDIA_DIAGNOSTICS
        if (binding.received_frame_diagnostic) binding.received_frame_diagnostic->Retire();
#endif
        if (binding.retire_crypto) {
            try { binding.retire_crypto(); }
            catch (...) {
                diagnostic::EmitBusinessEvent(
                    diagnostic::Event::Issue(diagnostic::IssueCode::NativeCleanupFailed));
            }
        }
        if (binding.telemetry_probe) {
            binding.telemetry_probe->active.store(false, std::memory_order_release);
        }
        if (binding.audio_telemetry_probe) {
            binding.audio_telemetry_probe->active.store(false, std::memory_order_release);
        }
        if (binding.render_telemetry_observer) {
            binding.render_telemetry_observer->SetExpected(
                false,
                render::RenderExpectationReason::BindingEnded,
                std::chrono::steady_clock::now());
        }
        if (binding.render_telemetry_probe) {
            binding.render_telemetry_probe->active.store(false, std::memory_order_release);
        }
        if (!binding.detach) continue;
        try {
            auto& manager = WebRTCManager::Instance();
            auto* detach_thread = binding.detach_thread == RemoteTrackSinkThread::Signaling
                ? manager.signaling_thread()
                : manager.worker_thread();
            if (detach_thread) {
                detach_thread->BlockingCall([detach = std::move(binding.detach)]() mutable {
                    detach();
                });
            } else {
                binding.detach();
            }
        } catch (const std::exception&) {
            diagnostic::EmitBusinessEvent(
                diagnostic::Event::Issue(diagnostic::IssueCode::NativeCleanupFailed));
        } catch (...) {
            diagnostic::EmitBusinessEvent(
                diagnostic::Event::Issue(diagnostic::IssueCode::NativeCleanupFailed));
        }
    }
}

std::vector<Room::RemoteTrackSinkBinding> Room::TakeRemoteTrackSinksForTrackKeys(
    const std::vector<TrackKey>& track_keys) {
    std::vector<RemoteTrackSinkBinding> removed;
    if (track_keys.empty()) return removed;

    std::lock_guard lock(room_mutex_);
    for (auto it = remote_track_sinks_.begin(); it != remote_track_sinks_.end();) {
        if (std::find(track_keys.begin(), track_keys.end(), it->track_key) ==
            track_keys.end()) {
            ++it;
            continue;
        }
        if (it->media_binding) {
            it->media_binding->active.store(false, std::memory_order_release);
        }
        removed.push_back(std::move(*it));
        it = remote_track_sinks_.erase(it);
    }
    for (const auto& binding : removed) {
        const bool still_bound = std::any_of(
            remote_track_sinks_.begin(), remote_track_sinks_.end(),
            [&binding](const RemoteTrackSinkBinding& candidate) {
                return candidate.rtc_track_id == binding.rtc_track_id;
            });
        if (!still_bound) processed_remote_track_ids_.erase(binding.rtc_track_id);
    }
    return removed;
}

std::vector<Room::RemoteTrackSinkBinding> Room::TakeRemoteTrackSinkForBindingSerial(
    uint64_t binding_serial) {
    std::vector<RemoteTrackSinkBinding> removed;
    if (binding_serial == 0) return removed;

    std::lock_guard lock(room_mutex_);
    const auto binding = std::find_if(
        remote_track_sinks_.begin(), remote_track_sinks_.end(),
        [binding_serial](const RemoteTrackSinkBinding& candidate) {
            return candidate.binding_serial == binding_serial;
        });
    if (binding == remote_track_sinks_.end()) return removed;
    if (binding->media_binding) {
        binding->media_binding->active.store(false, std::memory_order_release);
    }
    const std::string rtc_track_id = binding->rtc_track_id;
    removed.push_back(std::move(*binding));
    remote_track_sinks_.erase(binding);
    const bool still_bound = std::any_of(
        remote_track_sinks_.begin(), remote_track_sinks_.end(),
        [&rtc_track_id](const RemoteTrackSinkBinding& candidate) {
            return candidate.rtc_track_id == rtc_track_id;
        });
    if (!still_bound) processed_remote_track_ids_.erase(rtc_track_id);
    return removed;
}

Room::SubscriptionIntentKey Room::MakeSubscriptionIntentKeyLocked(
    const std::string& participant_sid,
    const std::string& participant_identity,
    const std::string& track_sid) const {
    return {participant_sid, participant_identity, track_sid};
}

Room::RemoteSubscriptionIntent& Room::EnsureSubscriptionIntentLocked(
    const SubscriptionIntentKey& key) {
    const auto [it, inserted] = subscription_intents_.try_emplace(key);
    if (inserted) {
        it->second.subscribed = session_auto_subscribe_;
        it->second.revision = next_subscription_revision_++;
        it->second.sent_subscribed = session_auto_subscribe_;
        if (it->second.subscribed) {
            it->second.accepted_at = std::chrono::steady_clock::now();
        }
    }
    return it->second;
}

const Room::RemoteSubscriptionIntent* Room::FindSubscriptionIntentLocked(
    const SubscriptionIntentKey& key) const {
    const auto it = subscription_intents_.find(key);
    return it == subscription_intents_.end() ? nullptr : &it->second;
}

proto::VideoQuality Room::VideoQualityForTier(VideoQualityTier quality) {
    switch (quality) {
    case VideoQualityTier::None: return proto::VideoQuality::OFF;
    case VideoQualityTier::P180: return proto::VideoQuality::LOW;
    case VideoQualityTier::P360: return proto::VideoQuality::MEDIUM;
    case VideoQualityTier::P720:
    case VideoQualityTier::P1080:
    case VideoQualityTier::P1440:
    case VideoQualityTier::P2160: return proto::VideoQuality::HIGH;
    }
    return proto::VideoQuality::OFF;
}

bool Room::IsVideoSettingsEqual(
    const RemoteSubscriptionIntent& intent,
    bool enabled,
    proto::VideoQuality quality,
    uint32_t width,
    uint32_t height,
    uint32_t max_fps,
    uint32_t priority) {
    return intent.enabled == enabled && intent.quality == quality &&
        intent.width == width && intent.height == height &&
        intent.max_fps == max_fps && intent.priority == priority;
}

void Room::QueueSubscriptionUpdateLocked(const SubscriptionIntentKey& key) {
    const auto* intent = FindSubscriptionIntentLocked(key);
    if (!intent || (!intent->subscription_dirty && !intent->settings_dirty)) return;
    std::shared_ptr<SubscriptionTelemetryOperation> preserved_operation;
    const auto existing = pending_subscription_updates_.find(key);
    if (existing != pending_subscription_updates_.end()) {
        if (existing->second.subscribed == intent->subscribed &&
            existing->second.enabled == intent->enabled &&
            existing->second.quality == intent->quality &&
            existing->second.width == intent->width &&
            existing->second.height == intent->height &&
            existing->second.max_fps == intent->max_fps &&
            existing->second.priority == intent->priority &&
            existing->second.revision == intent->revision) {
            existing->second.subscription_dirty =
                existing->second.subscription_dirty || intent->subscription_dirty;
            existing->second.settings_dirty =
                existing->second.settings_dirty || intent->settings_dirty;
            ScheduleSubscriptionDrainLocked();
            return;
        }
        if (!existing->second.in_flight && intent->subscription_dirty &&
            existing->second.subscribed == intent->subscribed) {
            preserved_operation =
                std::move(existing->second.telemetry_operation);
        }
        FinishSubscriptionOperationLocked(
            existing->second, telemetry::OperationOutcome::Cancelled);
    }
    PendingSubscriptionUpdate update;
    update.subscribed = intent->subscribed;
    update.enabled = intent->enabled;
    update.quality = intent->quality;
    update.width = intent->width;
    update.height = intent->height;
    update.max_fps = intent->max_fps;
    update.priority = intent->priority;
    update.revision = intent->revision;
    update.policy_revision = intent->policy_revision;
    update.subscription_dirty = intent->subscription_dirty;
    update.settings_dirty = intent->settings_dirty;
    update.telemetry_operation = std::move(preserved_operation);
    pending_subscription_updates_[key] = std::move(update);
    ScheduleSubscriptionDrainLocked();
}

void Room::FinishSubscriptionOperationLocked(
    const PendingSubscriptionUpdate& update,
    telemetry::OperationOutcome outcome) {
    if (update.telemetry_operation) {
        update.telemetry_operation->Finish(outcome);
    }
}

void Room::CancelPendingSubscriptionUpdateLocked(
    const SubscriptionIntentKey& key) {
    const auto pending = pending_subscription_updates_.find(key);
    if (pending == pending_subscription_updates_.end()) return;
    if (!pending->second.in_flight) {
        FinishSubscriptionOperationLocked(
            pending->second, telemetry::OperationOutcome::Cancelled);
    }
    pending_subscription_updates_.erase(pending);
}

void Room::ScheduleSubscriptionDrainLocked() {
    if (subscription_sender_active_ || pending_subscription_updates_.empty() ||
        subscription_recovery_barrier_ ||
        connection_state_ != ConnectionState::Connected || !signal_client_) {
        return;
    }
    subscription_sender_active_ = true;
    subscription_sender_session_ = subscription_session_generation_;
    subscription_sender_operation_ = next_subscription_sender_operation_++;
    const auto logical_session = subscription_sender_session_;
    const auto sender_operation = subscription_sender_operation_;
    const auto signal = signal_client_;
    std::weak_ptr<Room> weak = weak_from_this();
    callback_gate_->Post([weak, logical_session, sender_operation, signal] {
        if (const auto room = weak.lock()) {
            livekit::safe_co_spawn(room->executor_,
                [room, logical_session, sender_operation, signal]() -> asio::awaitable<void> {
                co_await room->DrainSubscriptionUpdates(
                    logical_session, sender_operation, signal);
            });
        }
    });
}

asio::awaitable<void> Room::DrainSubscriptionUpdates(
    uint64_t logical_session,
    uint64_t sender_operation,
    std::shared_ptr<SignalClient> signal) {
    struct BatchItem {
        SubscriptionIntentKey key;
        PendingSubscriptionUpdate update;
    };
    const auto same_settings = [](const PendingSubscriptionUpdate& left,
                                  const PendingSubscriptionUpdate& right) {
        return left.enabled == right.enabled &&
            left.quality == right.quality &&
            left.width == right.width &&
            left.height == right.height &&
            left.max_fps == right.max_fps &&
            left.priority == right.priority;
    };
    for (;;) {
        std::vector<BatchItem> batch;
        int send_phase = 5;
        std::shared_ptr<ConnectAttemptInstrumentation> instrumentation_hooks;
        {
            std::lock_guard lock(room_mutex_);
            if (logical_session != subscription_session_generation_ ||
                !subscription_sender_active_ ||
                subscription_sender_session_ != logical_session ||
                subscription_sender_operation_ != sender_operation ||
                subscription_recovery_barrier_ || signal_client_ != signal) {
                co_return;
            }
            if (connection_state_ != ConnectionState::Connected || !signal_client_) {
                subscription_sender_active_ = false;
                co_return;
            }
            if (pending_subscription_updates_.empty()) {
                subscription_sender_active_ = false;
                co_return;
            }
            const auto phase = [&](const SubscriptionIntentKey& candidate_key,
                                   const PendingSubscriptionUpdate& candidate) {
                if (!candidate.subscribed && candidate.settings_dirty) return 0;
                if (!candidate.subscribed && candidate.subscription_dirty) return 1;
                const auto current = FindSubscriptionIntentLocked(candidate_key);
                const bool retained = current && current->sent_subscribed.value_or(
                    session_auto_subscribe_);
                if (candidate.subscribed && candidate.settings_dirty && retained) return 2;
                if (candidate.subscribed && candidate.settings_dirty) return 3;
                if (candidate.subscribed && candidate.subscription_dirty) return 4;
                return 5;
            };
            int selected_phase = 6;
            for (auto candidate = pending_subscription_updates_.begin();
                 candidate != pending_subscription_updates_.end();) {
                const auto* intent = FindSubscriptionIntentLocked(candidate->first);
                const auto& pending_update = candidate->second;
                if (!intent || intent->revision != pending_update.revision ||
                    intent->subscribed != pending_update.subscribed ||
                    intent->enabled != pending_update.enabled ||
                    intent->quality != pending_update.quality ||
                    intent->width != pending_update.width ||
                    intent->height != pending_update.height ||
                    intent->max_fps != pending_update.max_fps ||
                    intent->priority != pending_update.priority) {
                    FinishSubscriptionOperationLocked(
                        pending_update, telemetry::OperationOutcome::Cancelled);
                    candidate = pending_subscription_updates_.erase(candidate);
                    continue;
                }
                const auto candidate_phase = phase(candidate->first, pending_update);
                if (candidate_phase < selected_phase) {
                    selected_phase = candidate_phase;
                }
                ++candidate;
            }
            if (selected_phase >= 5) {
                subscription_sender_active_ = false;
                co_return;
            }
            send_phase = selected_phase;
            const bool batch_settings =
                send_phase == 0 || send_phase == 2 || send_phase == 3;
            for (auto candidate = pending_subscription_updates_.begin();
                 candidate != pending_subscription_updates_.end(); ++candidate) {
                if (phase(candidate->first, candidate->second) != send_phase) {
                    continue;
                }
                if (batch_settings && !batch.empty() &&
                    !same_settings(batch.front().update, candidate->second)) {
                    continue;
                }
                candidate->second.in_flight = true;
                batch.push_back({candidate->first, candidate->second});
            }
            if (batch.empty()) {
                subscription_sender_active_ = false;
                co_return;
            }
            instrumentation_hooks = connect_attempt_instrumentation_;
        }

        const bool send_settings =
            send_phase == 0 || send_phase == 2 || send_phase == 3;
        proto::SignalRequest request;
        if (send_settings) {
            auto* settings = request.mutable_track_setting();
            const auto& first = batch.front().update;
            settings->set_disabled(!first.enabled);
            settings->set_quality(first.quality);
            settings->set_width(first.width);
            settings->set_height(first.height);
            settings->set_fps(first.max_fps);
            settings->set_priority(first.priority);
            for (const auto& item : batch) {
                settings->add_track_sids(item.key.track_sid);
            }
        } else {
            auto* subscription = request.mutable_subscription();
            subscription->set_subscribe(batch.front().update.subscribed);
            std::map<std::string, std::vector<std::string>> by_participant;
            for (const auto& item : batch) {
                by_participant[item.key.participant_sid].push_back(item.key.track_sid);
            }
            for (const auto& [participant_sid, track_sids] : by_participant) {
                auto* participant_tracks = subscription->add_participant_tracks();
                participant_tracks->set_participant_sid(participant_sid);
                for (const auto& track_sid : track_sids) {
                    subscription->add_track_sids(track_sid);
                    participant_tracks->add_track_sids(track_sid);
                }
            }
        }
        try {
            if (!send_settings && instrumentation_hooks && instrumentation_hooks->before_subscription_send) {
                for (const auto& item : batch) {
                    co_await instrumentation_hooks->before_subscription_send(
                        item.update.subscribed, item.update.revision);
                }
            }
            {
                std::lock_guard lock(room_mutex_);
                if (logical_session != subscription_session_generation_ ||
                    !subscription_sender_active_ ||
                    subscription_sender_session_ != logical_session ||
                    subscription_sender_operation_ != sender_operation ||
                    subscription_recovery_barrier_ || signal_client_ != signal ||
                    connection_state_ != ConnectionState::Connected) {
                    if (logical_session == subscription_session_generation_) {
                        for (const auto& item : batch) {
                            const auto pending = pending_subscription_updates_.find(item.key);
                            if (pending != pending_subscription_updates_.end() &&
                                pending->second.revision == item.update.revision) {
                                pending->second.in_flight = false;
                            }
                        }
                    }
                    co_return;
                }
                if (!send_settings && batch.front().update.subscribed) {
                    const auto send_started_at = std::chrono::steady_clock::now();
                    for (const auto& item : batch) {
                        auto intent = subscription_intents_.find(item.key);
                        if (intent != subscription_intents_.end() &&
                            intent->second.revision == item.update.revision &&
                            intent->second.subscribed) {
                            intent->second.accepted_at = send_started_at;
                        }
                    }
                }
            }
            co_await signal->SendAsync(request);
            {
                std::lock_guard lock(room_mutex_);
                for (const auto& item : batch) {
                    const auto pending = pending_subscription_updates_.find(item.key);
                    const bool current_pending =
                        pending != pending_subscription_updates_.end() &&
                        pending->second.revision == item.update.revision &&
                        pending->second.telemetry_operation ==
                            item.update.telemetry_operation;
                    if (!send_settings) {
                        FinishSubscriptionOperationLocked(
                            item.update, telemetry::OperationOutcome::Success);
                    } else if (!current_pending && item.update.telemetry_operation) {
                        FinishSubscriptionOperationLocked(
                            item.update, telemetry::OperationOutcome::Cancelled);
                    }
                    auto intent = subscription_intents_.find(item.key);
                    if (current_pending) {
                        if (send_settings) {
                            pending->second.settings_dirty = false;
                            if (intent != subscription_intents_.end() &&
                                intent->second.revision == item.update.revision) {
                                intent->second.settings_dirty = false;
                            }
                        } else {
                            pending->second.subscription_dirty = false;
                            pending->second.telemetry_operation.reset();
                            if (intent != subscription_intents_.end() &&
                                intent->second.revision == item.update.revision) {
                                intent->second.subscription_dirty = false;
                                intent->second.sent_subscribed = item.update.subscribed;
                            }
                        }
                        pending->second.in_flight = false;
                        if (!pending->second.settings_dirty &&
                            !pending->second.subscription_dirty) {
                            pending_subscription_updates_.erase(pending);
                        }
                    } else if (!send_settings &&
                               logical_session == subscription_session_generation_ &&
                               intent != subscription_intents_.end()) {
                        intent->second.sent_subscribed = item.update.subscribed;
                    }
                }
            }
        } catch (...) {
            const auto outcome = TelemetryOutcome(std::current_exception());
            std::lock_guard lock(room_mutex_);
            for (const auto& item : batch) {
                if (item.update.telemetry_operation) {
                    FinishSubscriptionOperationLocked(item.update, outcome);
                }
            }
            if (logical_session == subscription_session_generation_ &&
                subscription_sender_active_ &&
                subscription_sender_session_ == logical_session &&
                subscription_sender_operation_ == sender_operation &&
                signal_client_ == signal) {
                for (const auto& item : batch) {
                    const auto pending = pending_subscription_updates_.find(item.key);
                    const bool newer_update_pending =
                        pending != pending_subscription_updates_.end() &&
                        (pending->second.revision != item.update.revision ||
                         pending->second.telemetry_operation !=
                             item.update.telemetry_operation);
                    if (!newer_update_pending) {
                        if (pending != pending_subscription_updates_.end()) {
                            pending_subscription_updates_.erase(pending);
                        }
                        if (FindSubscriptionIntentLocked(item.key)) {
                            QueueSubscriptionUpdateLocked(item.key);
                        }
                    }
                }
                subscription_sender_active_ = false;
            }
            co_return;
        }
    }
}

void Room::PauseSubscriptionSendingLocked() {
    subscription_recovery_barrier_ = true;
    subscription_sender_active_ = false;
    subscription_sender_operation_ = next_subscription_sender_operation_++;
    for (auto& [key, intent] : subscription_intents_) {
        const auto pending = pending_subscription_updates_.find(key);
        const bool in_flight =
            pending != pending_subscription_updates_.end() &&
            pending->second.in_flight;
        const bool server_may_differ =
            intent.subscription_dirty ||
            in_flight ||
            intent.subscribed != session_auto_subscribe_ ||
            intent.sent_subscribed.value_or(session_auto_subscribe_) != intent.subscribed;
        if (!server_may_differ && !intent.subscribed) {
            CancelPendingSubscriptionUpdateLocked(key);
            intent.settings_dirty = false;
            continue;
        }
        intent.subscription_dirty = server_may_differ;
        const auto participant = remote_participants_.find(key.participant_sid);
        const auto publication = participant != remote_participants_.end() &&
                participant->second
            ? participant->second->get_publication(key.track_sid) : nullptr;
        if (intent.subscribed && publication && publication->track() &&
            publication->track()->kind() == TrackKind::Video) {
            intent.settings_dirty = true;
        }
        QueueSubscriptionUpdateLocked(key);
    }
}

RemoteMediaRecoveryRequest Room::BeginRemoteMediaRecoveryLocked() {
    ++remote_media_recovery_epoch_;
    if (remote_media_recovery_epoch_ == 0) ++remote_media_recovery_epoch_;
    remote_media_recovery_token_ =
        std::to_string(subscription_session_generation_) + ":" +
        std::to_string(installed_session_generation_) + ":" +
        std::to_string(remote_media_recovery_epoch_);
    remote_media_recovery_pending_ = true;
    return RemoteMediaRecoveryRequest{
        remote_media_coordinator_session_,
        installed_session_generation_,
        remote_media_catalog_revision_,
        remote_media_recovery_epoch_,
        remote_media_recovery_token_};
}

void Room::CompleteRemoteMediaRecoveryLocked() {
    remote_media_recovery_pending_ = false;
    remote_media_recovery_token_.clear();
}

void Room::FinishSubscriptionRecoveryLocked(
    const SubscriptionSyncSnapshot& snapshot,
    const std::shared_ptr<SignalClient>& signal) {
    if (snapshot.logical_session != subscription_session_generation_ ||
        signal_client_ != signal) {
        return;
    }
    for (const auto& [key, revision] : snapshot.revisions) {
        const auto current = subscription_intents_.find(key);
        const auto pending = pending_subscription_updates_.find(key);
        if (current != subscription_intents_.end() &&
            current->second.revision == revision &&
            pending != pending_subscription_updates_.end() &&
            pending->second.revision == revision) {
            if (pending->second.subscription_dirty) {
                FinishSubscriptionOperationLocked(
                    pending->second, telemetry::OperationOutcome::Success);
                current->second.subscription_dirty = false;
                current->second.sent_subscribed = current->second.subscribed;
                pending->second.subscription_dirty = false;
                pending->second.telemetry_operation.reset();
            }
            pending->second.in_flight = false;
            if (!pending->second.settings_dirty) {
                pending_subscription_updates_.erase(pending);
            }
        }
    }
    subscription_recovery_barrier_ = false;
}

void Room::ResetSubscriptionSessionLocked(bool auto_subscribe) {
    for (const auto& [_, update] : pending_subscription_updates_) {
        FinishSubscriptionOperationLocked(
            update, telemetry::OperationOutcome::Cancelled);
    }
    ++subscription_session_generation_;
    next_subscription_revision_ = 1;
    session_auto_subscribe_ = auto_subscribe;
    subscription_recovery_barrier_ = false;
    subscription_room_sid_.clear();
    subscription_intents_.clear();
    pending_subscription_updates_.clear();
    subscription_sender_active_ = false;
    subscription_sender_session_ = subscription_session_generation_;
    subscription_sender_operation_ = next_subscription_sender_operation_++;
    remote_media_native_generation_ = 0;
    remote_media_coordinator_session_ = 0;
    remote_media_catalog_revision_ = 0;
    remote_media_policy_revision_ = 0;
    remote_media_recovery_epoch_ = 0;
    remote_media_plan_active_ = false;
    remote_media_recovery_pending_ = false;
    remote_media_recovery_token_.clear();
}

void Room::ClearSubscriptionSessionLocked() {
    ResetSubscriptionSessionLocked(true);
}

void Room::PruneSubscriptionIntentsLocked() {
    for (auto it = subscription_intents_.begin(); it != subscription_intents_.end();) {
        const auto participant = remote_participants_.find(it->first.participant_sid);
        if (participant == remote_participants_.end() || !participant->second ||
            participant->second->identity() != it->first.participant_identity ||
            !participant->second->get_publication(it->first.track_sid)) {
            CancelPendingSubscriptionUpdateLocked(it->first);
            it = subscription_intents_.erase(it);
        } else {
            ++it;
        }
    }
}

std::shared_ptr<MediaBindingState> Room::FindMediaBindingLocked(
    uint64_t binding_serial) const {
    const auto binding = std::find_if(
        remote_track_sinks_.begin(), remote_track_sinks_.end(),
        [binding_serial](const RemoteTrackSinkBinding& candidate) {
            return candidate.binding_serial == binding_serial;
        });
    return binding == remote_track_sinks_.end() ? nullptr : binding->media_binding;
}

std::shared_ptr<RemoteTrackPublication> Room::CreateRemoteTrackPublication(
    std::shared_ptr<Track> track,
    const std::string& participant_sid,
    const std::string& participant_identity,
    const std::string& track_sid,
    const std::string& name,
    proto::TrackType type) {
    const auto generation = session_generation_.load(std::memory_order_acquire);
    std::weak_ptr<Room> weak_room = weak_from_this();
    auto controller = [weak_room, participant_sid, generation](
                          RemoteTrackPublication* publication,
                          const RemotePublicationControlRequest& request) {
        const auto room = weak_room.lock();
        if (!room) return RemotePublicationControlDispatch::Rejected;
        return room->QueueRemotePublicationControl(
            publication, participant_sid, generation, request);
    };
    const auto key = MakeSubscriptionIntentKeyLocked(
        participant_sid, participant_identity, track_sid);
    auto& intent = EnsureSubscriptionIntentLocked(key);
    // Full restart preserves desired state/revision, but request sequences
    // belong to each publication object. The successor starts at sequence 1;
    // generation and canonical-pointer checks still reject the old controller.
    intent.last_request_sequence = 0;
    auto publication = std::make_shared<RemoteTrackPublication>(
        std::move(track), track_sid, name, type, generation,
        std::move(controller), intent.subscribed);
    RemotePublicationControlRequest restored;
    restored.kind = RemotePublicationControlRequest::Kind::Settings;
    restored.enabled = intent.enabled;
    restored.quality = intent.quality;
    restored.width = intent.width;
    restored.height = intent.height;
    restored.priority = intent.priority;
    publication->CommitControl(restored);
    return publication;
}

RemotePublicationControlDispatch Room::QueueRemotePublicationControl(
    RemoteTrackPublication* publication,
    const std::string& participant_sid,
    uint64_t generation,
    const RemotePublicationControlRequest& request) {
    if (!publication) return RemotePublicationControlDispatch::Rejected;

    if (request.kind == RemotePublicationControlRequest::Kind::Subscription &&
        request.subscribed.has_value()) {
        std::shared_ptr<RemoteTrackPublication> canonical;
        uint64_t binding_serial = 0;
        {
            std::lock_guard lock(room_mutex_);
            if ((connection_state_ != ConnectionState::Connected &&
                 connection_state_ != ConnectionState::Reconnecting) ||
                generation != session_generation_.load(std::memory_order_acquire)) {
                return RemotePublicationControlDispatch::Rejected;
            }
            const auto participant = remote_participants_.find(participant_sid);
            if (participant == remote_participants_.end() || !participant->second) {
                return RemotePublicationControlDispatch::Rejected;
            }
            canonical = participant->second->get_remote_publication(publication->sid());
            if (!canonical || canonical.get() != publication ||
                canonical->session_generation() != generation) {
                return RemotePublicationControlDispatch::Rejected;
            }

            const auto key = MakeSubscriptionIntentKeyLocked(
                participant_sid, participant->second->identity(), canonical->sid());
            auto& intent = EnsureSubscriptionIntentLocked(key);
            if (request.sequence <= intent.last_request_sequence) {
                return RemotePublicationControlDispatch::Rejected;
            }
            intent.last_request_sequence = request.sequence;
            intent.subscribed = *request.subscribed;
            const bool is_video = canonical->track() &&
                canonical->track()->kind() == TrackKind::Video;
            if (is_video) {
                intent.enabled = intent.subscribed;
                intent.settings_dirty = true;
            }
            intent.revision = next_subscription_revision_++;
            intent.subscription_dirty = true;
            intent.accepted_at = intent.subscribed
                ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point{};

            CancelPendingSubscriptionUpdateLocked(key);
            std::shared_ptr<SubscriptionTelemetryOperation> telemetry_operation;
            if (const auto sink = session_telemetry_.lock()) {
                telemetry_operation =
                    std::make_shared<SubscriptionTelemetryOperation>(
                        sink,
                        intent.subscribed ? telemetry::OperationKind::Subscribe
                                          : telemetry::OperationKind::Unsubscribe);
            }
            QueueSubscriptionUpdateLocked(key);
            const auto pending = pending_subscription_updates_.find(key);
            if (pending != pending_subscription_updates_.end()) {
                pending->second.telemetry_operation =
                    std::move(telemetry_operation);
            }

            auto committed = request;
            if (is_video) committed.enabled = intent.enabled;
            canonical->CommitControl(committed);
            if (intent.subscribed) {
                // An explicit SetSubscribed(true), including a retry of the
                // same intent, starts a fresh attempt on this publication.
                ClearTrackSubscriptionErrorLocked(participant->second, canonical);
            }

            if (!intent.subscribed) {
                if (canonical->track()) {
                    SetRemoteMediaExpected(
                        session_telemetry_.lock(), canonical->track()->kind(),
                        participant_sid, canonical->sid(), false,
                        telemetry::MediaExpectationReason::Unsubscribed);
                }
                const auto current = current_remote_binding_serials_.find(canonical.get());
                if (current != current_remote_binding_serials_.end()) {
                    binding_serial = current->second;
                    if (const auto media_binding = FindMediaBindingLocked(binding_serial)) {
                        media_binding->active.store(false, std::memory_order_release);
                    }
                }
            }
            ScheduleSubscriptionDrainLocked();
        }

        if (binding_serial != 0) {
            DetachRemotePublicationMedia(
                canonical.get(), /*notify_listener=*/true, binding_serial);
        }
        return RemotePublicationControlDispatch::Queued;
    }

    std::lock_guard lock(room_mutex_);
    if ((connection_state_ != ConnectionState::Connected &&
         connection_state_ != ConnectionState::Reconnecting) ||
        generation != session_generation_.load(std::memory_order_acquire)) {
        return RemotePublicationControlDispatch::Rejected;
    }
    const auto participant = remote_participants_.find(participant_sid);
    if (participant == remote_participants_.end() || !participant->second) {
        return RemotePublicationControlDispatch::Rejected;
    }
    const auto canonical =
        participant->second->get_remote_publication(publication->sid());
    if (!canonical || canonical.get() != publication ||
        canonical->session_generation() != generation || !canonical->track() ||
        canonical->track()->kind() != TrackKind::Video) {
        return RemotePublicationControlDispatch::Rejected;
    }

    const auto key = MakeSubscriptionIntentKeyLocked(
        participant_sid, participant->second->identity(), canonical->sid());
    auto& intent = EnsureSubscriptionIntentLocked(key);
    if (request.sequence <= intent.last_request_sequence) {
        return RemotePublicationControlDispatch::Rejected;
    }
    intent.last_request_sequence = request.sequence;
    intent.enabled = request.enabled.value_or(intent.enabled);
    intent.quality = request.quality.value_or(intent.quality);
    intent.width = request.width.value_or(intent.width);
    intent.height = request.height.value_or(intent.height);
    intent.priority = request.priority.value_or(intent.priority);
    intent.revision = next_subscription_revision_++;
    intent.settings_dirty = true;
    QueueSubscriptionUpdateLocked(key);
    canonical->CommitControl(request);
    ScheduleSubscriptionDrainLocked();
    return RemotePublicationControlDispatch::Queued;
}

void Room::RemoveRemoteMediaTrackReferences(
    const std::vector<std::shared_ptr<Track>>& removed_tracks) {
    if (removed_tracks.empty()) return;
    const auto remove_tracks = [&removed_tracks](std::vector<std::weak_ptr<Track>>& tracks) {
        tracks.erase(std::remove_if(tracks.begin(), tracks.end(), [&removed_tracks](const std::weak_ptr<Track>& weak) {
            const auto track = weak.lock();
            return !track || std::find(removed_tracks.begin(), removed_tracks.end(), track) !=
                removed_tracks.end();
        }), tracks.end());
    };

    std::lock_guard lock(remote_media_mutex_);
    remove_tracks(remote_video_tracks_);
    remove_tracks(remote_audio_tracks_);
}

void Room::ClearRemotePublicationMediaBindingsLocked() {
    for (auto& binding : remote_track_sinks_) {
        if (binding.media_binding) {
            binding.media_binding->active.store(false, std::memory_order_release);
        }
    }
    // Retire the owner's shared media index before releasing room_mutex_. Slow
    // native detach must never clear a replacement's newly installed entries.
    {
        std::lock_guard media_lock(remote_media_mutex_);
        remote_video_tracks_.clear();
        remote_audio_tracks_.clear();
    }
    for (const auto& [participant_sid, participant] : remote_participants_) {
        if (!participant) continue;
        for (const auto& [track_sid, publication] : participant->tracks()) {
            if (const auto remote_publication =
                    std::dynamic_pointer_cast<RemoteTrackPublication>(publication)) {
                remote_publication->ClearMediaBinding();
            }
        }
    }
    current_remote_binding_serials_.clear();
}

void Room::DetachRemotePublicationMedia(RemoteTrackPublication* publication,
                                        bool notify_listener,
                                        uint64_t binding_serial) {
    if (!publication) return;

    std::shared_ptr<RemoteParticipant> participant;
    std::shared_ptr<RemoteTrackPublication> canonical;
    std::shared_ptr<Track> track;
    webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> detached_rtc_track;
    std::vector<std::shared_ptr<RoomListener>> listeners;
    std::shared_ptr<MediaBindingState> media_binding;
    uint64_t generation = 0;
    {
        std::lock_guard lock(room_mutex_);
        for (const auto& [participant_sid, candidate] : remote_participants_) {
            const auto remote_publication = candidate->get_remote_publication(publication->sid());
            if (remote_publication && remote_publication.get() == publication) {
                participant = candidate;
                canonical = remote_publication;
                break;
            }
        }
        if (!participant || !canonical || !canonical->has_media_binding()) return;
        const auto current_serial = current_remote_binding_serials_.find(canonical.get());
        if (current_serial == current_remote_binding_serials_.end() ||
            current_serial->second != binding_serial) {
            return;
        }
        media_binding = FindMediaBindingLocked(binding_serial);
        if (media_binding) {
            media_binding->active.store(false, std::memory_order_release);
        }

        track = canonical->track();
        const auto track_membership = track_memberships_.find(canonical.get());
        if (track_membership == track_memberships_.end()) return;
        current_remote_binding_serials_.erase(current_serial);
        canonical->ClearMediaBinding();
        // Retire this binding's logical media references in the same Room
        // transaction as its serial. A concurrent/reentrant attach may reuse
        // the canonical Track after this point. Keep the RTC reference alive
        // so setting nullptr cannot destroy an RTC object under room_mutex_.
        // set_rtc_track(nullptr) performs no RTC set_enabled call.
        if (track) {
            detached_rtc_track = track->rtc_track();
            track->set_rtc_track(nullptr);
        }
        // Matches retain_binding's Room -> remote_media_mutex_ lock order.
        RemoveRemoteMediaTrackReferences({track});
        auto unavailable = MakeTrackEventLocked(
            ParticipantEventKind::TrackUnavailable,
            participant,
            canonical,
            false);
        if (media_binding) {
            unavailable.media_binding_key = media_binding->key;
            unavailable.media_binding_ticket = media_binding;
        }
        EnqueueParticipantEventLocked(std::move(unavailable));
        listeners = listeners_;
        generation = session_generation_.load(std::memory_order_acquire);
    }

    // Move physical sinks out of Room before calling WebRTC RemoveSink. The
    // publication is already marked detached, making repeated unsubscribe and
    // replacement calls idempotent.
    DetachRemoteTrackSinks(TakeRemoteTrackSinkForBindingSerial(binding_serial));

    if (notify_listener && track) {
        for (const auto& listener : listeners) {
            DeliverListener({generation}, listener, [&](RoomListener& target) {
                bool current_cleanup = false;
                {
                    std::lock_guard lock(room_mutex_);
                    const auto current_participant =
                        remote_participants_.find(participant->sid());
                    const auto current_binding =
                        current_remote_binding_serials_.find(canonical.get());
                    current_cleanup =
                        current_participant != remote_participants_.end() &&
                        current_participant->second.get() == participant.get() &&
                        participant->get_remote_publication(canonical->sid()) == canonical &&
                        (current_binding == current_remote_binding_serials_.end() ||
                         current_binding->second == binding_serial);
                }
                if (current_cleanup) {
                    target.OnTrackUnsubscribed(track, canonical, participant);
                }
            }, true);
        }
    }
}


std::pair<std::string, std::string> Room::UnpackStreamId(const std::string& packed) {
    auto sep_pos = packed.find('|');
    if (sep_pos != std::string::npos) {
        return {packed.substr(0, sep_pos), packed.substr(sep_pos + 1)};
    }
    if (packed.rfind("PA_", 0) == 0) {
        return {packed, ""};
    }
    return {"", ""};
}

void Room::RemoveExpiredPendingTracks() {
    auto now = std::chrono::steady_clock::now();
    for (auto it = pending_track_queue_.begin(); it != pending_track_queue_.end();) {
        auto& list = it->second;
        list.erase(std::remove_if(list.begin(), list.end(), [now](const PendingTrack& pt) {
            return now > pt.expires_at;
        }), list.end());
        if (list.empty()) {
            it = pending_track_queue_.erase(it);
        } else {
            ++it;
        }
    }
}

void Room::AttachRemoteTrackToParticipant(
    std::shared_ptr<RemoteParticipant> participant,
    webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track,
    webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
    const std::string& track_sid, uint64_t generation) {
    if (!participant || !track) return;
    const auto rtc_track_id = track->id();

    TrackKind kind = (track->kind() == webrtc::MediaStreamTrackInterface::kAudioKind) ? TrackKind::Audio : TrackKind::Video;

    // 优先使用传入的有效 track_sid (以 TR_ 开头)，若不是以 TR_ 开头，尝试从 participant 已有的 publications 中查找对应类型的有效 SID
    std::string track_id = track_sid;
    if (track_id.rfind("TR_", 0) != 0) {
        for (const auto& [sid, p] : participant->tracks()) {
            if (p && p->track() && p->track()->kind() == kind && sid.rfind("TR_", 0) == 0) {
                track_id = sid;
                break;
            }
        }
    }
    if (track_id.empty()) {
        track_id = rtc_track_id;
    }

    std::shared_ptr<TrackPublication> pub;
    std::shared_ptr<Track> r_track;
    std::shared_ptr<RemoteTrackPublication> remote_publication;
    uint64_t replaced_binding_serial = 0;
    ParticipantKey participant_key;
    TrackKey track_key;
    uint64_t binding_serial = 0;
    SubscriptionIntentKey subscription_key;
    uint64_t subscription_revision = 0;
    std::chrono::steady_clock::time_point subscription_accepted_at;
    std::shared_ptr<MediaBindingState> media_binding;
    std::shared_ptr<SignalClient> signal;
    std::shared_ptr<E2eeManager> receive_e2ee;
    webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> retired_rtc_track;
    double native_volume = 1.0;
    bool initial_output_muted = false;
    uint64_t delivery_generation = 0;

    {
        std::lock_guard lock(room_mutex_);
        if (generation != 0 && !IsNativeGenerationCurrentLocked(generation)) return;
        delivery_generation = session_generation_.load(std::memory_order_acquire);
        const auto canonical = remote_participants_.find(participant->sid());
        const auto membership = FindMembershipLocked(participant);
        if (connection_state_ == ConnectionState::Disconnected ||
            canonical == remote_participants_.end() ||
            canonical->second.get() != participant.get() ||
            !membership ||
            !membership->active.load(std::memory_order_acquire)) {
            return;
        }
        // 查找已有 publication
        pub = participant->get_publication(track_id);
        if (!pub && track_id.rfind("TR_", 0) != 0) {
            // 尝试按类型查找未绑定 rtc_track 的已有 publication
            for (const auto& [sid, p] : participant->tracks()) {
                if (p && p->track() && p->track()->kind() == kind && !p->track()->rtc_track()) {
                    pub = p;
                    track_id = sid;
                    break;
                }
            }
        }

        receive_e2ee = e2ee_manager_;
        if (receive_e2ee && receive_e2ee->enabled() &&
            (!pub || pub->encryption() != TrackEncryption::Gcm)) {
            // Do not infer encryption from the presence of an RTP track. Keep
            // the native channel/gate closed until TrackInfo declares GCM.
            RemoveExpiredPendingTracks();
            auto& pending = pending_track_queue_[participant->sid()];
            const auto duplicate = std::any_of(pending.begin(), pending.end(), [&](const PendingTrack& item) {
                return item.generation == generation && item.receiver == receiver && item.track_sid == track_id;
            });
            if (!duplicate) pending.push_back({generation, track, receiver, participant->sid(), track_id,
                std::chrono::steady_clock::now() + std::chrono::seconds(15)});
            if (pub && pub->encryption() != TrackEncryption::Unknown &&
                pub->subscription_error() != TrackPublication::SubscriptionError::EncryptionRequired) {
                pub->set_subscription_error(TrackPublication::SubscriptionError::EncryptionRequired);
                EnqueueParticipantEventLocked(MakeTrackEventLocked(
                    ParticipantEventKind::TrackSubscriptionError, participant, pub, false));
            }
            return;
        }

        if (!pub) {
            r_track = std::make_shared<Track>(track_id, (kind == TrackKind::Video ? "camera_video" : "microphone_audio"), kind);
            if (kind == TrackKind::Audio && audio_output_muted_) {
                r_track->set_output_muted(true);
            }
            pub = CreateRemoteTrackPublication(
                r_track,
                participant->sid(),
                participant->identity(),
                track_id,
                r_track->name(),
                kind == TrackKind::Audio ? proto::TrackType::AUDIO : proto::TrackType::VIDEO);
            participant->add_publication(pub);
        } else {
            r_track = pub->track();
            if (!r_track) {
                r_track = std::make_shared<Track>(track_id, pub->name().empty() ? (kind == TrackKind::Video ? "camera_video" : "microphone_audio") : pub->name(), kind);
                if (kind == TrackKind::Audio && audio_output_muted_) {
                    r_track->set_output_muted(true);
                }
                pub->set_track(r_track);
            }
        }
        remote_publication = std::dynamic_pointer_cast<RemoteTrackPublication>(pub);
        const auto track_membership = EnsureTrackMembershipLocked(participant, pub);
        if (!track_membership) return;
        participant_key = membership->key;
        track_key = track_membership->key;
        subscription_key = MakeSubscriptionIntentKeyLocked(
            participant->sid(), participant->identity(), track_id);
        auto& subscription_intent = EnsureSubscriptionIntentLocked(subscription_key);
        if (!subscription_intent.subscribed ||
            pub->subscription_error() != TrackPublication::SubscriptionError::None ||
            (remote_publication && !remote_publication->is_subscribed())) {
            return;
        }
        subscription_revision = subscription_intent.revision;
        subscription_accepted_at = subscription_intent.accepted_at;
        initial_output_muted = audio_output_muted_;
        receive_e2ee = e2ee_manager_;
        binding_serial = next_remote_track_binding_serial_++;
        media_binding = std::make_shared<MediaBindingState>(
            MediaBindingKey{track_key, binding_serial});
        if (remote_publication && remote_publication->has_media_binding() &&
            remote_publication->media_track_id() != rtc_track_id) {
            const auto existing_serial =
                current_remote_binding_serials_.find(remote_publication.get());
            if (existing_serial != current_remote_binding_serials_.end()) {
                replaced_binding_serial = existing_serial->second;
            }
        }
    }

    // A replaced WebRTC receiver must detach its previous raw sink before the
    // canonical Track points at the new receiver. This keeps one sink binding
    // per remote publication/SID rather than accumulating stale decoders.
    if (replaced_binding_serial != 0 && remote_publication) {
        DetachRemotePublicationMedia(
            remote_publication.get(), /*notify_listener=*/false, replaced_binding_serial);
    }

    std::function<void()> retire_crypto;
#if COHAVORA_ENABLE_MEDIA_DIAGNOSTICS
    std::shared_ptr<const E2eeManager::MediaObservation> receive_trace_observation;
    std::shared_ptr<telemetry::ReceivedFrameDiagnostic> received_frame_diagnostic;
#endif
    bool receiver_committed = false;
    const auto crypto_rollback = std::shared_ptr<void>(nullptr, [&](void*) {
#if COHAVORA_ENABLE_MEDIA_DIAGNOSTICS
        if (!receiver_committed && received_frame_diagnostic) received_frame_diagnostic->Retire();
#endif
        if (!receiver_committed && retire_crypto) {
            try { retire_crypto(); }
            catch (...) {
                diagnostic::EmitBusinessEvent(
                    diagnostic::Event::Issue(diagnostic::IssueCode::NativeCleanupFailed));
            }
        }
    });
    if (receive_e2ee && receive_e2ee->enabled()) {
        if (!receiver) return;
        auto gate = EnsureReceiverCryptoGate(receiver, generation);
        if (!gate) return;
        auto active = std::shared_ptr<const std::atomic<bool>>(media_binding, &media_binding->active);
        auto cryptor = receive_e2ee->CreateReceiverCryptor(
            WebRTCManager::Instance().signaling_thread(), participant->identity(), track_id,
            kind == TrackKind::Video, generation, std::move(active), &retire_crypto
#if COHAVORA_ENABLE_MEDIA_DIAGNOSTICS
            , &receive_trace_observation
#endif
            );
        if (!cryptor) {
            gate->SetTarget(binding_serial, nullptr);
            return;
        }
#if COHAVORA_ENABLE_MEDIA_DIAGNOSTICS
        if (receive_trace_observation) {
            MediaFrameTraceRecord record;
            record.phase = MediaFrameTracePhase::BindingMapped;
            record.binding_id = receive_trace_observation->binding_id;
            record.generation = receive_trace_observation->generation;
            record.room_binding_serial = binding_serial;
            record.install_epoch = receive_trace_observation->install_epoch->load(std::memory_order_acquire);
            receive_trace_observation->frame_trace->Record(record);
        }
#endif
        if (!gate->SetTarget(binding_serial, std::move(cryptor))) return;
    }
#if COHAVORA_ENABLE_MEDIA_DIAGNOSTICS
    else if (receiver && kind == TrackKind::Video) {
        std::shared_ptr<MediaReceiverGate> diagnostic_gate;
        bool install_hook = false;
        {
            std::lock_guard lock(room_mutex_);
            // Never replace a crypto gate, even when an old encryption owner
            // is already retired. Unknown/GCM/custom publications stay untouched.
            const bool crypto_hook_present = std::any_of(
                receiver_crypto_gates_.begin(), receiver_crypto_gates_.end(),
                [&](const auto& entry) {
                    return entry.first.second == receiver.get() && !entry.second.expired();
                });
            if (received_frame_diagnostics_enabled_ && !crypto_hook_present &&
                e2ee_manager_ == receive_e2ee && (!receive_e2ee || !receive_e2ee->enabled()) &&
                pub->encryption() == TrackEncryption::None &&
                (generation == 0 || IsNativeGenerationCurrentLocked(generation))) {
                std::erase_if(receiver_diagnostic_gates_, [](const auto& entry) { return entry.second.expired(); });
                auto& slot = receiver_diagnostic_gates_[{generation, receiver.get()}];
                diagnostic_gate = slot.lock();
                if (!diagnostic_gate) {
                    diagnostic_gate = CreateMediaReceiverGate();
                    slot = diagnostic_gate;
                    install_hook = true;
                }
                received_frame_diagnostic = std::make_shared<telemetry::ReceivedFrameDiagnostic>(
                    std::shared_ptr<const std::atomic<bool>>(media_binding, &media_binding->active));
            }
        }
        if (diagnostic_gate) {
            auto passthrough = webrtc::make_ref_counted<telemetry::ReceivedFrameDiagnosticPassthrough>(
                received_frame_diagnostic);
            if (diagnostic_gate->SetTarget(binding_serial, std::move(passthrough))) {
                // RtpReceiver dispatches registration to its worker, exactly as
                // EnsureReceiverCryptoGate does. No Room lock crosses that call.
                if (install_hook) receiver->SetFrameTransformer(diagnostic_gate->Hook());
            } else {
                received_frame_diagnostic->Retire();
                received_frame_diagnostic.reset();
            }
        }
    }
#endif

    uint64_t superseded_binding_serial = 0;
    const auto retain_binding = [&](RemoteTrackSinkBinding& binding) {
        binding.retire_crypto = retire_crypto;
        binding.receiver = receiver;
#if COHAVORA_ENABLE_MEDIA_DIAGNOSTICS
        binding.received_frame_diagnostic = received_frame_diagnostic;
#endif
        std::lock_guard lock(room_mutex_);
        if (e2ee_manager_ != receive_e2ee) return false;
        if (generation != 0 && !IsNativeGenerationCurrentLocked(generation)) return false;
        const auto canonical = remote_participants_.find(participant->sid());
        const auto membership = FindMembershipLocked(participant);
        const auto track_membership = track_memberships_.find(pub.get());
        const auto* subscription_intent = FindSubscriptionIntentLocked(subscription_key);
        if (connection_state_ == ConnectionState::Disconnected ||
            canonical == remote_participants_.end() ||
            canonical->second.get() != participant.get() ||
            participant->get_publication(track_id) != pub ||
            !membership || membership->key != participant_key ||
            track_membership == track_memberships_.end() ||
            track_membership->second->key != track_key ||
            !track_membership->second->active.load(std::memory_order_acquire) ||
            !subscription_intent || !subscription_intent->subscribed ||
            subscription_intent->revision != subscription_revision ||
            pub->subscription_error() != TrackPublication::SubscriptionError::None ||
            !remote_publication || !remote_publication->is_subscribed()) {
            return false;
        }

        retired_rtc_track = r_track->ExchangeRtcTrackBinding(
            track, kind == TrackKind::Audio && audio_output_muted_);
        native_volume = r_track->volume();
        {
            std::lock_guard media_lock(remote_media_mutex_);
            auto& media_tracks = kind == TrackKind::Video
                ? remote_video_tracks_
                : remote_audio_tracks_;
            const auto existing = std::find_if(
                media_tracks.begin(), media_tracks.end(), [&r_track](const std::weak_ptr<Track>& weak) {
                    return weak.lock() == r_track;
                });
            if (existing == media_tracks.end()) media_tracks.push_back(r_track);
        }
        processed_remote_track_ids_.insert(rtc_track_id);
        signal = signal_client_;
        if (kind == TrackKind::Video && binding.telemetry_probe) {
            const bool expected_receive = subscription_intent->subscribed &&
                !r_track->muted() &&
                pub->stream_state() == TrackPublication::StreamState::Active &&
                pub->subscription_allowed();
            if (const auto telemetry_owner = session_telemetry_.lock()) {
                telemetry_owner->RegisterRemoteVideoBinding(
                    RemoteVideoTelemetryKey(participant->sid(), track_id),
                    track_key.participant.native_room_generation,
                    binding_serial,
                    subscription_accepted_at,
                    expected_receive,
                    r_track->source() != TrackSource::ScreenShareVideo,
                    binding.telemetry_probe,
                    std::chrono::steady_clock::now(),
                    RemoteMediaRecoveryIdentity(*participant, *pub));
                if (binding.render_telemetry_probe) {
                    telemetry_owner->RegisterRemoteVideoRenderBinding(
                        RemoteVideoRenderTelemetryKey(participant->sid(), track_id),
                        track_key.participant.native_room_generation,
                        binding_serial,
                        subscription_accepted_at,
                        false,
                        r_track->source() != TrackSource::ScreenShareVideo,
                        std::chrono::milliseconds(33),
                        binding.render_telemetry_probe,
                        std::chrono::steady_clock::now(),
                        RemoteMediaRecoveryIdentity(*participant, *pub));
                }
            }
        } else if (kind == TrackKind::Audio && binding.audio_telemetry_probe) {
            const bool expected_receive = subscription_intent->subscribed &&
                !r_track->muted() && !r_track->playout_muted() &&
                native_volume > 0.001 &&
                pub->stream_state() == TrackPublication::StreamState::Active &&
                pub->subscription_allowed();
            if (const auto telemetry_owner = session_telemetry_.lock()) {
                telemetry_owner->RegisterRemoteAudioBinding(
                    RemoteAudioTelemetryKey(participant->sid(), track_id),
                    track_key.participant.native_room_generation,
                    binding_serial,
                    subscription_accepted_at,
                    expected_receive,
                    binding.audio_telemetry_probe,
                    std::chrono::steady_clock::now(),
                    RemoteMediaRecoveryIdentity(*participant, *pub));
            }
        }
        media_binding->active.store(true, std::memory_order_release);
        remote_track_sinks_.push_back(std::move(binding));
        receiver_committed = true;
        if (remote_publication) {
            const auto existing_serial =
                current_remote_binding_serials_.find(remote_publication.get());
            if (existing_serial != current_remote_binding_serials_.end() &&
                existing_serial->second != binding_serial) {
                superseded_binding_serial = existing_serial->second;
            }
            current_remote_binding_serials_[remote_publication.get()] = binding_serial;
            std::weak_ptr<Room> weak_room = weak_from_this();
            remote_publication->SetMediaBinding(
                rtc_track_id,
                [weak_room, binding_serial](RemoteTrackPublication* publication,
                                            bool notify_listener) {
                    if (const auto room = weak_room.lock()) {
                        room->DetachRemotePublicationMedia(
                            publication, notify_listener, binding_serial);
                    }
                });
        }
        QueueSubscriptionUpdateLocked(subscription_key);
        return true;
    };
    const auto configure_native = [&]() {
        const bool effective_enabled =
            !r_track->muted() && !r_track->playout_muted() && native_volume > 0.001;
        track->set_enabled(effective_enabled);
        if (kind == TrackKind::Audio) {
            static_cast<webrtc::AudioTrackInterface*>(track.get())->SetVolume(native_volume);
        }
    };

    if (kind == TrackKind::Audio) {
        auto audio_track = static_cast<webrtc::AudioTrackInterface*>(track.get());
        if (audio_track) {
            audio_track->set_enabled(
                !r_track->muted() && !r_track->playout_muted() &&
                !initial_output_muted && r_track->volume() > 0.001);
        }
        auto has_logged = std::make_shared<std::atomic<bool>>(false);
        auto last_voice_log_time = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
        auto audio_telemetry_probe = std::make_shared<telemetry::AudioActivityProbe>();
        auto audio_telemetry_gate = std::make_shared<AudioFrameTelemetryGate>();
        const auto audio_telemetry_key =
            RemoteAudioTelemetryKey(participant->sid(), track_id);
        std::weak_ptr<telemetry::SessionTelemetry> weak_telemetry = session_telemetry_;
        const auto telemetry_room_generation =
            track_key.participant.native_room_generation;
        std::weak_ptr<Room> weak_room = weak_from_this();
        auto sink = std::make_shared<NativeAudioTrackSink>(
            [r_track, has_logged, last_voice_log_time, weak_room, participant,
             media_binding, audio_telemetry_probe, audio_telemetry_gate,
             weak_telemetry, audio_telemetry_key, telemetry_room_generation,
             binding_serial](std::chrono::steady_clock::time_point source_time,
                             const AudioFrame& frame) {
            if (!media_binding->active.load(std::memory_order_acquire)) return;
            const auto frame_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                source_time.time_since_epoch()).count();
            const auto previous_frame_ns = audio_telemetry_probe->last_frame_ns.exchange(
                frame_ns, std::memory_order_acq_rel);
            audio_telemetry_probe->sample_rate.store(
                static_cast<std::uint32_t>((std::max)(0, frame.sampleRate())),
                std::memory_order_release);
            audio_telemetry_probe->channels.store(
                static_cast<std::uint32_t>((std::max)(0, frame.numChannels())),
                std::memory_order_release);
            if (const auto telemetry_owner = weak_telemetry.lock()) {
                const bool first_frame = !audio_telemetry_gate->first_frame_submitted.load(
                    std::memory_order_acquire);
                const auto recovery_epoch = telemetry_owner->ActiveRecoveryEpoch();
                const auto observed_epoch = audio_telemetry_gate->recovery_epoch.load(
                    std::memory_order_acquire);
                if (recovery_epoch != observed_epoch) {
                    audio_telemetry_gate->recovery_epoch.store(
                        recovery_epoch, std::memory_order_release);
                    audio_telemetry_gate->recovery_first_ns.store(
                        0, std::memory_order_release);
                    audio_telemetry_gate->recovery_last_good_ns.store(
                        previous_frame_ns, std::memory_order_release);
                    audio_telemetry_gate->recovery_stable_submitted.store(
                        false, std::memory_order_release);
                }
                const auto recovery_first_ns =
                    audio_telemetry_gate->recovery_first_ns.load(
                        std::memory_order_acquire);
                const bool recovery_first = recovery_epoch != 0 && recovery_first_ns == 0;
                const bool recovery_stable = recovery_epoch != 0 &&
                    recovery_first_ns != 0 &&
                    !audio_telemetry_gate->recovery_stable_submitted.load(
                        std::memory_order_acquire) &&
                    frame_ns - recovery_first_ns >=
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            telemetry::SessionTelemetry::kRecoveryStableWindow).count();
                if ((first_frame || recovery_first || recovery_stable) &&
                    telemetry_owner->RecordRemoteAudioFrame(
                        audio_telemetry_key,
                        telemetry_room_generation,
                        binding_serial,
                        recovery_epoch,
                        recovery_stable,
                        static_cast<std::uint32_t>((std::max)(0, frame.sampleRate())),
                        static_cast<std::uint32_t>((std::max)(0, frame.numChannels())),
                        source_time,
                        recovery_first &&
                                audio_telemetry_gate->recovery_last_good_ns.load(
                                    std::memory_order_acquire) > 0
                            ? std::chrono::steady_clock::time_point(
                                std::chrono::nanoseconds(
                                    audio_telemetry_gate->recovery_last_good_ns.load(
                                        std::memory_order_acquire)))
                            : std::chrono::steady_clock::time_point{})) {
                    if (first_frame) {
                        audio_telemetry_gate->first_frame_submitted.store(
                            true, std::memory_order_release);
                    }
                    if (recovery_first) {
                        audio_telemetry_gate->recovery_first_ns.store(
                            frame_ns, std::memory_order_release);
                    }
                    if (recovery_stable) {
                        audio_telemetry_gate->recovery_stable_submitted.store(
                            true, std::memory_order_release);
                    }
                }
            }
            if (r_track) {
                r_track->notifyAudioFrame(frame);
            }
            if (!has_logged->exchange(true)) {
                if (auto room = weak_room.lock()) {
                    room->Log("WEBRTC", "AUDIO_FRAME", "First remote audio frame received (sample rate: " + std::to_string(frame.sampleRate()) + "Hz, channels: " + std::to_string(frame.numChannels()) + ", samples per frame: " + std::to_string(frame.totalSamples()) + ")");
                }
            }
            // 计算音频能量 RMS 并进行周期性语音诊断
            const auto& samples = frame.data();
            if (!samples.empty()) {
                double sum = 0.0;
                for (size_t i = 0; i < samples.size(); ++i) {
                    sum += static_cast<double>(samples[i]) * samples[i];
                }
                double rms = std::sqrt(sum / static_cast<double>(samples.size()));
                if (rms > 250.0) {
                    auto now = std::chrono::steady_clock::now();
                    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - *last_voice_log_time).count() > 2500) {
                        *last_voice_log_time = now;
                        if (auto room = weak_room.lock()) {
                            room->Log("MEDIA", "AUDIO_VOICE", "Remote audio activity detected");
                        }
                    }
                }
            }
        });
        audio_track->AddSink(sink.get());
        RemoteTrackSinkBinding binding{
            track_key,
            binding_serial,
            media_binding,
            {},
            audio_telemetry_probe,
            {},
            {},
            track->id(),
            RemoteTrackSinkThread::Signaling,
            [track, sink]() {
                auto* attached_track = static_cast<webrtc::AudioTrackInterface*>(track.get());
                if (attached_track) attached_track->RemoveSink(sink.get());
            }};
        const bool retained = retain_binding(binding);
        if (!retained) {
            if (binding.detach) binding.detach();
            return;
        }
        configure_native();
        if (superseded_binding_serial != 0) {
            DetachRemoteTrackSinks(
                TakeRemoteTrackSinkForBindingSerial(superseded_binding_serial));
        }
        Log("WEBRTC", "AUDIO_ATTACH", "Remote audio track sink attached");

        Log("SIGNAL", "AUDIO_TRACK_ACTIVE", "SFU downstream audio subscription reconciled");
    } else {
        auto video_track = static_cast<webrtc::VideoTrackInterface*>(track.get());
        auto has_logged = std::make_shared<std::atomic<bool>>(false);
        auto telemetry_probe = std::make_shared<telemetry::VideoActivityProbe>();
        auto render_telemetry_probe =
            std::make_shared<telemetry::RenderActivityProbe>();
        auto telemetry_gate = std::make_shared<VideoFrameTelemetryGate>();
        const auto telemetry_key = RemoteVideoTelemetryKey(participant->sid(), track_id);
        const auto render_telemetry_key =
            RemoteVideoRenderTelemetryKey(participant->sid(), track_id);
        std::weak_ptr<telemetry::SessionTelemetry> weak_telemetry = session_telemetry_;
        const auto telemetry_room_generation =
            track_key.participant.native_room_generation;
        auto render_telemetry_observer =
            std::make_shared<TelemetryRenderSubmitObserver>(
                weak_telemetry,
                render_telemetry_key,
                telemetry_room_generation,
                binding_serial,
                render_telemetry_probe);
        auto render_frame_token = std::make_shared<std::atomic<std::uint64_t>>(0);
        std::weak_ptr<Room> weak_room = weak_from_this();
        std::function<void(std::chrono::nanoseconds)> duration_probe;
        std::function<void(std::chrono::nanoseconds, std::chrono::nanoseconds)> phase_probe;
        {
            std::lock_guard lock(room_mutex_);
            if (connect_attempt_instrumentation_) {
                duration_probe = connect_attempt_instrumentation_->on_native_video_callback_duration;
                phase_probe = connect_attempt_instrumentation_->on_native_video_callback_phase_durations;
            }
        }
        auto sink = std::make_shared<NativeVideoTrackSink>(
            [r_track, has_logged, weak_room, media_binding, telemetry_probe,
             telemetry_gate, weak_telemetry, telemetry_key,
             telemetry_room_generation, binding_serial](
                std::chrono::steady_clock::time_point source_time,
                int width, int height, bool retained) -> Track::I420VideoFrameDelivery {
            if (!media_binding->active.load(std::memory_order_acquire)) {
                Track::I420VideoFrameDelivery rejected;
                rejected.rejected_inactive = 1;
                return rejected;
            }
            const auto frame_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                source_time.time_since_epoch()).count();
            const auto previous_frame_ns = telemetry_probe->last_frame_ns.exchange(
                frame_ns, std::memory_order_acq_rel);
            telemetry_probe->width.store(
                static_cast<std::uint32_t>(width), std::memory_order_release);
            telemetry_probe->height.store(
                static_cast<std::uint32_t>(height), std::memory_order_release);

            if (const auto telemetry_owner = weak_telemetry.lock()) {
                const bool first_frame =
                    !telemetry_gate->first_frame_submitted.load(std::memory_order_acquire);
                const auto recovery_epoch = telemetry_owner->ActiveRecoveryEpoch();
                auto observed_epoch = telemetry_gate->recovery_epoch.load(
                    std::memory_order_acquire);
                if (recovery_epoch != observed_epoch) {
                    telemetry_gate->recovery_epoch.store(
                        recovery_epoch, std::memory_order_release);
                    telemetry_gate->recovery_first_ns.store(0, std::memory_order_release);
                    telemetry_gate->recovery_last_good_ns.store(
                        previous_frame_ns, std::memory_order_release);
                    telemetry_gate->recovery_stable_submitted.store(
                        false, std::memory_order_release);
                    observed_epoch = recovery_epoch;
                }

                const auto recovery_first_ns = telemetry_gate->recovery_first_ns.load(
                    std::memory_order_acquire);
                const bool recovery_first = recovery_epoch != 0 && recovery_first_ns == 0;
                const bool recovery_stable = recovery_epoch != 0 &&
                    recovery_first_ns != 0 &&
                    !telemetry_gate->recovery_stable_submitted.load(
                        std::memory_order_acquire) &&
                    frame_ns - recovery_first_ns >=
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            telemetry::SessionTelemetry::kRecoveryStableWindow).count();
                if ((first_frame || recovery_first || recovery_stable) &&
                    telemetry_owner->RecordRemoteVideoFrame(
                        telemetry_key,
                        telemetry_room_generation,
                        binding_serial,
                        recovery_epoch,
                        recovery_stable,
                        static_cast<std::uint32_t>(width),
                        static_cast<std::uint32_t>(height),
                        source_time,
                        recovery_first &&
                                telemetry_gate->recovery_last_good_ns.load(
                                    std::memory_order_acquire) > 0
                            ? std::chrono::steady_clock::time_point(
                                std::chrono::nanoseconds(
                                    telemetry_gate->recovery_last_good_ns.load(
                                        std::memory_order_acquire)))
                            : std::chrono::steady_clock::time_point{})) {
                    if (first_frame) {
                        telemetry_gate->first_frame_submitted.store(
                            true, std::memory_order_release);
                    }
                    if (recovery_first) {
                        telemetry_gate->recovery_first_ns.store(
                            frame_ns, std::memory_order_release);
                    }
                    if (recovery_stable) {
                        telemetry_gate->recovery_stable_submitted.store(
                            true, std::memory_order_release);
                    }
                }
            }
            if (!has_logged->exchange(true)) {
                if (auto room = weak_room.lock()) {
                    room->Log("WEBRTC", "VIDEO_FRAME", "WebRTC decoder started producing remote video (" + std::to_string(width) + "x" + std::to_string(height) + ")");
                }
            }
            auto delivery = r_track ? r_track->prepareI420VideoDelivery(source_time, retained)
                                    : Track::I420VideoFrameDelivery{};
            delivery.binding_valid = [media_binding] {
                return media_binding->active.load(std::memory_order_acquire);
            };
            return delivery;
        }, telemetry_probe,
            [render_telemetry_key, telemetry_room_generation, binding_serial,
             render_telemetry_observer, render_frame_token, telemetry_probe](
                    std::chrono::steady_clock::time_point decoded_at) {
                render::RenderFrameMetadata metadata;
                metadata.series_key = render_telemetry_key;
                metadata.room_generation = telemetry_room_generation;
                metadata.binding_epoch = binding_serial;
                metadata.frame_token = render_frame_token->fetch_add(
                    1, std::memory_order_relaxed) + 1;
                telemetry_probe->on_frame_count.store(
                    metadata.frame_token, std::memory_order_relaxed);
                telemetry_probe->last_on_frame_ns.store(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        decoded_at.time_since_epoch()).count(),
                    std::memory_order_release);
                metadata.decoded_at = decoded_at;
                metadata.observer = render_telemetry_observer;
                return metadata;
            }, std::move(duration_probe), std::move(phase_probe)
#if COHAVORA_ENABLE_MEDIA_DIAGNOSTICS
            , receive_trace_observation, binding_serial
#endif
            );
        std::shared_ptr<render::RemoteVideoFramePipeline> pipeline;
        {
            std::lock_guard lock(room_mutex_);
            pipeline = remote_video_pipeline_.lock();
            if (!pipeline) {
                pipeline = std::make_shared<render::RemoteVideoFramePipeline>();
                remote_video_pipeline_ = pipeline;
            }
        }
        auto endpoint = pipeline->Bind([media_binding, weak_track = std::weak_ptr<Track>(r_track)] {
            const auto track = weak_track.lock();
            return media_binding->active.load(std::memory_order_acquire) && track && !track->muted();
        });
        if (r_track) r_track->setRemoteVideoEndpoint(endpoint);
        sink->SetPipeline(std::move(pipeline), std::move(endpoint));
        video_track->AddOrUpdateSink(sink.get(), webrtc::VideoSinkWants());
        RemoteTrackSinkBinding binding{
            track_key,
            binding_serial,
            media_binding,
            telemetry_probe,
            {},
            render_telemetry_probe,
            render_telemetry_observer,
            track->id(),
            RemoteTrackSinkThread::MediaWorker,
            [track, sink]() {
                auto* attached_track = static_cast<webrtc::VideoTrackInterface*>(track.get());
                if (attached_track) attached_track->RemoveSink(sink.get());
                sink->Close();
            }};
        const bool retained = retain_binding(binding);
        if (!retained) {
            if (binding.detach) binding.detach();
            return;
        }
        configure_native();
        if (superseded_binding_serial != 0) {
            DetachRemoteTrackSinks(
                TakeRemoteTrackSinkForBindingSerial(superseded_binding_serial));
        }
        Log("WEBRTC", "VIDEO_ATTACH", "Remote video track sink attached");

        if (signal) {
            Log("SIGNAL", "TRACK_ACTIVE", "SFU downstream video binding uses the current Room media plan");
        }
    }

    // 触发 RoomListener 回调 (OnTrackPublished 已在 UpdateParticipants 中派发，此处仅派发 OnTrackSubscribed)
    std::vector<std::shared_ptr<RoomListener>> listeners;
    bool still_current = false;
    {
        std::lock_guard lock(room_mutex_);
        const auto canonical = remote_participants_.find(participant->sid());
        const auto membership = FindMembershipLocked(participant);
        const auto track_membership = track_memberships_.find(pub.get());
        const auto* subscription_intent = FindSubscriptionIntentLocked(subscription_key);
        still_current = (generation == 0 || IsNativeGenerationCurrentLocked(generation)) &&
            connection_state_ != ConnectionState::Disconnected &&
            canonical != remote_participants_.end() &&
            canonical->second.get() == participant.get() &&
            participant->get_publication(track_id) == pub &&
            membership && membership->key == participant_key &&
            track_membership != track_memberships_.end() &&
            track_membership->second->key == track_key &&
            track_membership->second->active.load(std::memory_order_acquire) &&
            subscription_intent && subscription_intent->subscribed &&
            subscription_intent->revision == subscription_revision &&
            pub->subscription_error() == TrackPublication::SubscriptionError::None &&
            media_binding->active.load(std::memory_order_acquire) &&
            (!remote_publication ||
             (current_remote_binding_serials_.contains(remote_publication.get()) &&
              current_remote_binding_serials_.at(remote_publication.get()) == binding_serial));
        if (still_current) {
            auto available = MakeTrackEventLocked(
                ParticipantEventKind::TrackAvailable,
                participant,
                pub,
                false);
            available.media_binding_key = media_binding->key;
            available.media_binding_ticket = media_binding;
            EnqueueParticipantEventLocked(std::move(available));
            listeners = listeners_;
        }
    }
    if (!still_current) {
        if (remote_publication) {
            DetachRemotePublicationMedia(
                remote_publication.get(), /*notify_listener=*/false, binding_serial);
        } else {
            DetachRemoteTrackSinks(TakeRemoteTrackSinkForBindingSerial(binding_serial));
        }
        return;
    }
    for (const auto& l : listeners) {
        if (!media_binding->active.load(std::memory_order_acquire)) break;
        DeliverListener({delivery_generation, {}, generation != 0}, l, [&](RoomListener& target) {
            if (media_binding->active.load(std::memory_order_acquire)) {
                target.OnTrackSubscribed(r_track, pub, participant);
            }
        }, true);
    }
}

void Room::FlushPendingTracks(const std::string& participant_sid, uint64_t generation) {
    std::vector<PendingTrack> pending_to_flush;
    std::shared_ptr<RemoteParticipant> participant;
    {
        std::lock_guard lock(room_mutex_);
        if (generation != 0 && !IsNativeGenerationCurrentLocked(generation)) return;
        RemoveExpiredPendingTracks();
        auto pit = remote_participants_.find(participant_sid);
        if (pit == remote_participants_.end()) return;
        participant = pit->second;

        // 仅精确匹配该参会人的暂存队列
        auto qit = pending_track_queue_.find(participant_sid);
        if (qit != pending_track_queue_.end()) {
            pending_to_flush = std::move(qit->second);
            pending_track_queue_.erase(qit);
        }
    }

    if (!pending_to_flush.empty()) {
        Log("TRACK", "FLUSH_PENDING", "Flushing " +
            std::to_string(pending_to_flush.size()) + " pending media tracks");
        for (const auto& item : pending_to_flush) {
            AttachRemoteTrackToParticipant(participant, item.track, item.receiver, item.track_sid, item.generation);
        }
    }
}

void Room::OnRemoteTrackAdded(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver, webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track) {
    OnRemoteTrackAdded(receiver, track, session_generation_.load(std::memory_order_acquire));
}

void Room::OnRemoteTrackAdded(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver, webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track, uint64_t generation) {
    if (!track) return;
    
    std::string track_id = track->id();
    std::function<void(bool)> duplicate_probe;
    bool duplicate = false;
    bool same_track_object = false;
    {
        std::lock_guard lock(room_mutex_);
        if (processed_remote_track_ids_.find(track_id) != processed_remote_track_ids_.end()) {
            duplicate = true;
            if (connect_attempt_instrumentation_) {
                duplicate_probe = connect_attempt_instrumentation_->on_remote_track_duplicate;
                if (duplicate_probe) {
                    for (const auto& [_, participant] : remote_participants_) {
                        if (!participant) continue;
                        for (const auto& [sid, publication] : participant->tracks()) {
                            (void)sid;
                            if (publication && publication->track()) {
                                same_track_object |=
                                    publication->track()->rtc_track().get() == track.get();
                            }
                        }
                    }
                }
            }
        }
    }
    if (duplicate) {
        if (duplicate_probe) duplicate_probe(same_track_object);
        // A reused RTC id is not an object identity. Resolve against the active
        // binding below so a replacement receiver can take over its gate.
    }

    // 从 receiver 的 stream_ids 解包 (msid: <participantSid>|<trackSid>)
    std::string stream_id;
    if (receiver) {
        auto streams = receiver->stream_ids();
        if (!streams.empty()) {
            stream_id = streams[0];
        }
    }

    auto [participant_sid, track_sid] = UnpackStreamId(stream_id);
    
    // 如果解出来的 participant_sid 为空 (说明是本地预分配的无主临时虚拟轨，尚未绑定远端 SSRC/MSID)，直接忽略
    if (participant_sid.empty()) {
        Log("WEBRTC", "ON_TRACK_IGNORE", "Ignoring local virtual track without remote SSRC/MSID: TrackID=" + track_id + ", StreamID=" + stream_id + ", Kind=" + std::string(track->kind()));
        return;
    }

    if (track_sid.empty()) {
        track_sid = track_id;
    }

    OnRemoteTrackResolved(receiver, track, std::move(participant_sid),
                          std::move(track_sid), generation);
}

void Room::OnRemoteTrackResolved(
    webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
    webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track,
    std::string participant_sid, std::string track_sid, uint64_t generation) {
    if (!track || participant_sid.empty()) return;
    if (track_sid.empty()) track_sid = track->id();

    std::vector<std::pair<std::shared_ptr<RemoteTrackPublication>, uint64_t>> old_bindings;
    {
        std::lock_guard lock(room_mutex_);
        if (!IsNativeGenerationCurrentLocked(generation)) return;
        for (const auto& [owner_sid, owner] : remote_participants_) {
            if (!owner) continue;
            for (const auto& [sid, publication] : owner->tracks()) {
                auto remote = std::dynamic_pointer_cast<RemoteTrackPublication>(publication);
                if (!remote || !remote->has_media_binding() ||
                    remote->media_track_id() != track->id()) continue;
                const auto current = current_remote_binding_serials_.find(remote.get());
                if (owner_sid == participant_sid && sid == track_sid &&
                    remote->track() && remote->track()->rtc_track().get() == track.get() &&
                    current != current_remote_binding_serials_.end()) {
                    const auto binding = std::find_if(remote_track_sinks_.begin(),
                        remote_track_sinks_.end(), [&](const auto& candidate) {
                            return candidate.binding_serial == current->second &&
                                candidate.receiver.get() == receiver.get() &&
                                candidate.media_binding &&
                                candidate.media_binding->active.load(std::memory_order_acquire);
                        });
                    if (binding != remote_track_sinks_.end()) {
                        if (remote->track()->kind() == TrackKind::Audio) {
                            remote->track()->set_output_muted(audio_output_muted_);
                        } else {
                            remote->track()->UpdateRtcTrackEnabled();
                        }
                        return;
                    }
                }
                if (current != current_remote_binding_serials_.end()) {
                    old_bindings.emplace_back(std::move(remote), current->second);
                }
            }
        }
    }
    for (const auto& [publication, serial] : old_bindings) {
        DetachRemotePublicationMedia(publication.get(), true, serial);
    }

    Log("WEBRTC", "ON_TRACK_RESOLVE", "Downstream track resolved: Kind=" +
        std::string(track->kind()));

    std::shared_ptr<RemoteParticipant> participant;
    BeforeNativeEventCommit(generation);
    {
        std::lock_guard lock(room_mutex_);
        if (!IsNativeGenerationCurrentLocked(generation)) return;
        auto it = remote_participants_.find(participant_sid);
        if (it != remote_participants_.end()) {
            participant = it->second;
        }

        if (!participant) {
            // 参会人尚未建立（信令在路上）：放入以 participant_sid 为精确 Key 的 PendingTrackQueue 暂存！
            RemoveExpiredPendingTracks();
            PendingTrack pt;
            pt.generation = generation;
            pt.track = track;
            pt.receiver = receiver;
            pt.participant_sid = participant_sid;
            pt.track_sid = track_sid;
            pt.expires_at = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            pending_track_queue_[participant_sid].push_back(pt);
        }
    }
    if (!participant) {
        Log("TRACK", "ENQUEUE_PENDING", "Participant is not ready; media track saved in pending queue");
        return;
    }

    // 参会人已就绪，立即绑定挂载
    AttachRemoteTrackToParticipant(participant, track, receiver, track_sid, generation);
}


void Room::ClearTrackSubscriptionErrorLocked(
    const std::shared_ptr<RemoteParticipant>& participant,
    const std::shared_ptr<RemoteTrackPublication>& publication) {
    if (!participant || !publication ||
        publication->subscription_error() == TrackPublication::SubscriptionError::None) {
        return;
    }
    if (e2ee_manager_ && e2ee_manager_->enabled() && publication->encryption() != TrackEncryption::Gcm &&
        publication->subscription_error() == TrackPublication::SubscriptionError::EncryptionRequired) return;
    publication->set_subscription_error(TrackPublication::SubscriptionError::None);
    EnqueueParticipantEventLocked(MakeTrackEventLocked(
        ParticipantEventKind::TrackSubscriptionError, participant, publication, false));
}

namespace {

struct NegotiatedRemoteTrack {
    webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver;
    webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track;
    std::string stream_id;
};

std::vector<NegotiatedRemoteTrack> SnapshotNegotiatedRemoteTracks(
    const webrtc::scoped_refptr<webrtc::PeerConnectionInterface>& pc) {
    std::vector<NegotiatedRemoteTrack> result;
    if (!pc) return result;
    auto* signaling_thread = WebRTCManager::Instance().signaling_thread();
    if (!signaling_thread) return result;
    signaling_thread->BlockingCall([&] {
        const auto* remote = pc->remote_description();
        const auto* description = remote ? remote->description() : nullptr;
        if (!description) return;
        for (const auto& transceiver : pc->GetTransceivers()) {
            if (!transceiver || !transceiver->receiver() || !transceiver->mid()) continue;
            const auto* content = description->GetContentByName(*transceiver->mid());
            if (!content || content->rejected || !content->media_description()) continue;
            const auto& streams = content->media_description()->streams();
            if (streams.size() != 1 || streams.front().first_stream_id().empty()) continue;
            auto track = transceiver->receiver()->track();
            if (track) {
                result.push_back({transceiver->receiver(), std::move(track),
                                  streams.front().first_stream_id()});
            }
        }
    });
    return result;
}

} // namespace

void Room::ReconcileRemoteReceivers(
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,
    uint64_t generation) {
    for (auto& negotiated : SnapshotNegotiatedRemoteTracks(pc)) {
        auto [participant_sid, track_sid] = UnpackStreamId(negotiated.stream_id);
        if (participant_sid.empty()) continue;
        callback_gate_->Post(
            [weak = weak_from_this(), receiver = std::move(negotiated.receiver),
             track = std::move(negotiated.track), participant_sid = std::move(participant_sid),
             track_sid = std::move(track_sid), generation]() mutable {
                if (auto room = weak.lock()) {
                    room->OnRemoteTrackResolved(
                        std::move(receiver), std::move(track),
                        std::move(participant_sid), std::move(track_sid), generation);
                }
            });
    }
}

void Room::UpdateTrackSubscriptionError(
    const proto::SubscriptionResponse& response,
    uint64_t event_generation) {
    using Error = TrackPublication::SubscriptionError;
    // SubscriptionResponse carries SubscriptionError, not a success ACK.
    // In particular, SE_UNKNOWN must not clear a previous failure.
    Error error = Error::Unknown;
    if (response.err() == proto::SE_CODEC_UNSUPPORTED) error = Error::CodecUnsupported;
    else if (response.err() == proto::SE_TRACK_NOTFOUND) error = Error::TrackNotFound;

    std::shared_ptr<RemoteTrackPublication> publication;
    uint64_t binding_serial = 0;
    bool changed = false;
    {
        std::lock_guard lock(room_mutex_);
        if (!IsSignalGenerationCurrentLocked(event_generation)) return;
        for (const auto& [participant_sid, participant] : remote_participants_) {
            if (!participant) continue;
            auto candidate = participant->get_remote_publication(response.track_sid());
            if (!candidate) continue;
            const auto key = MakeSubscriptionIntentKeyLocked(
                participant_sid, participant->identity(), candidate->sid());
            const auto* intent = FindSubscriptionIntentLocked(key);
            if (!intent || !intent->subscribed || !candidate->is_subscribed()) return;
            publication = std::move(candidate);
            const auto binding = current_remote_binding_serials_.find(publication.get());
            if (binding != current_remote_binding_serials_.end()) {
                binding_serial = binding->second;
                // Invalidate queued TrackAvailable and frame callbacks before
                // releasing Room ownership or detaching the physical sink.
                if (const auto state = FindMediaBindingLocked(binding_serial)) {
                    state->active.store(false, std::memory_order_release);
                }
            }
            if (publication->subscription_error() != error) {
                changed = true;
                publication->set_subscription_error(error);
                EnqueueParticipantEventLocked(MakeTrackEventLocked(
                    ParticipantEventKind::TrackSubscriptionError,
                    participant, publication, false));
            }
            break;
        }
    }
    if (publication && binding_serial != 0) {
        DetachRemotePublicationMedia(publication.get(), true, binding_serial);
    }
    if (changed && publication && publication->track()) {
        diagnostic::Event event;
        event.kind = diagnostic::EventKind::MediaSubscriptionChanged;
        event.media_kind = publication->track()->kind() == TrackKind::Audio
            ? diagnostic::MediaKind::Audio : diagnostic::MediaKind::Video;
        event.subscription_state = diagnostic::SubscriptionState::Blocked;
        event.error_code = error == Error::CodecUnsupported
            ? diagnostic::ErrorCode::CodecUnsupported
            : error == Error::TrackNotFound
                ? diagnostic::ErrorCode::TrackNotFound
                : diagnostic::ErrorCode::Unknown;
        EmitDiagnostic(event);
    }
}

void Room::UpdateTrackSubscriptionPermission(
    const proto::SubscriptionPermissionUpdate& update,
    uint64_t event_generation) {
    const TrackSubscriptionPermission permission{
        update.participant_sid(), update.track_sid(), update.allowed()};
    std::shared_ptr<Participant> participant;
    std::shared_ptr<TrackPublication> publication;
    std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;
    bool changed = false;

    {
        std::lock_guard lock(room_mutex_);
        if (!IsSignalGenerationCurrentLocked(event_generation)) return;
        event_generation = session_generation_.load(std::memory_order_acquire);
        const auto key = std::make_pair(permission.participant_sid,
                                        permission.track_sid);
        const auto existing = track_subscription_permissions_.find(key);
        changed = existing == track_subscription_permissions_.end() ||
            existing->second != permission.allowed;
        if (!changed) {
            return;
        }
        track_subscription_permissions_[key] = permission.allowed;

        if (local_participant_ &&
            local_participant_->sid() == permission.participant_sid) {
            participant = local_participant_;
        } else {
            const auto remote = remote_participants_.find(permission.participant_sid);
            if (remote != remote_participants_.end()) {
                participant = remote->second;
            }
        }
        if (participant) {
            publication = participant->get_publication(permission.track_sid);
            if (publication) {
                publication->set_subscription_allowed(permission.allowed);
                EnqueueParticipantEventLocked(MakeTrackEventLocked(
                    ParticipantEventKind::TrackSubscriptionPermission,
                    participant,
                    publication,
                    participant.get() == local_participant_.get()));
            }
        }
        listeners_snapshot = listeners_;
    }

    if (participant && participant != local_participant_ && publication &&
        publication->track()) {
        if (!permission.allowed) {
            diagnostic::Event event;
            event.kind = diagnostic::EventKind::MediaSubscriptionChanged;
            event.media_kind = publication->track()->kind() == TrackKind::Audio
                ? diagnostic::MediaKind::Audio : diagnostic::MediaKind::Video;
            event.subscription_state = diagnostic::SubscriptionState::Blocked;
            event.error_code = diagnostic::ErrorCode::PermissionDenied;
            EmitDiagnostic(event);
        }
        const auto snapshot = publication->SnapshotState();
        if (const auto telemetry_owner = session_telemetry_.lock()) {
            SetRemoteMediaExpected(
                telemetry_owner,
                snapshot.kind,
                participant->sid(), publication->sid(),
                IsRemoteMediaExpected(
                    snapshot, IsRemotePublicationSubscribed(publication)),
                permission.allowed
                    ? telemetry::MediaExpectationReason::PermissionAllowed
                    : telemetry::MediaExpectationReason::PermissionDenied);
        }
    }
    for (const auto& listener : listeners_snapshot) {
        DeliverListener({event_generation}, listener, [&](RoomListener& target) {
            target.OnTrackSubscriptionPermissionChanged(permission,
                                                           participant,
                                                           publication);
        }, true);
    }
}


} // namespace livekit
