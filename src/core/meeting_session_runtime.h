#pragma once

#include <QtCore/QString>

#include <asio.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <tuple>

#include "participant_event.h"
#include "publication_catalog.h"
#include "video_demand_policy.h"
#include "video_demand_types.h"
#include "src/telemetry/session_telemetry.h"

namespace livekit {
class ScreenShareSession;
namespace whiteboard { class Runtime; }
}

namespace OpenMeeting {

struct InboundTransferKey {
    uint64_t coordinatorSession = 0;
    uint64_t nativeRoomGeneration = 0;
    uint64_t participantIncarnation = 0;
    QString wireTransferId;

    InboundTransferKey() = default;
    InboundTransferKey(uint64_t coordinator_session,
                       uint64_t native_room_generation,
                       uint64_t participant_incarnation,
                       QString wire_transfer_id)
        : coordinatorSession(coordinator_session),
          nativeRoomGeneration(native_room_generation),
          participantIncarnation(participant_incarnation),
          wireTransferId(std::move(wire_transfer_id)) {}

    // Preserves the narrow runtime test/API seam. Production callers always
    // provide the full participant-instance key.
    InboundTransferKey(QString wire_transfer_id)
        : wireTransferId(std::move(wire_transfer_id)) {}

    bool operator<(const InboundTransferKey &other) const {
        return std::tie(coordinatorSession,
                        nativeRoomGeneration,
                        participantIncarnation,
                        wireTransferId) <
            std::tie(other.coordinatorSession,
                     other.nativeRoomGeneration,
                     other.participantIncarnation,
                     other.wireTransferId);
    }

    QString uiTransferId() const {
        return QString::number(coordinatorSession) + QLatin1Char(':') +
            QString::number(nativeRoomGeneration) + QLatin1Char(':') +
            QString::number(participantIncarnation) + QLatin1Char(':') +
            QString::number(wireTransferId.size()) + QLatin1Char(':') + wireTransferId;
    }
};

// `MeetingCoordinator` keeps presentation state on the Qt thread. This class
// owns only per-room callback state and is strictly affine to its ASIO strand.
// Do not access transfersOnStrand() from Qt or WebRTC callback threads.
struct InboundMediaTransfer {
    QString mediaType;
    QString fileName;
    int totalChunks = 0;
    qint64 totalSize = 0;
    int64_t seq = 0;
    qint64 lastActiveTimestamp = 0;
    QString senderIdentity;
    QString senderName;
    livekit::ParticipantKey senderKey;
    livekit::ParticipantTicket senderTicket;
    QString wireTransferId;
    std::map<int, QString> receivedChunks;
};

class MeetingSessionRuntime final {
public:
    using Strand = asio::strand<asio::io_context::executor_type>;

    MeetingSessionRuntime(asio::io_context &context,
                          uint64_t generation,
                          QString localUserId,
                          std::shared_ptr<void> executorLifetime = {})
        : _executorLifetime(std::move(executorLifetime))
        , _strand(context.get_executor())
        , _videoDemandTimer(_strand)
        , _generation(generation)
        , _localUserId(std::move(localUserId))
        , _publicationCatalog(generation)
        , _videoDemandPolicy(generation)
        , _telemetry(std::make_shared<livekit::telemetry::SessionTelemetry>(
              _strand, generation,
              livekit::telemetry::SessionTelemetry::kDefaultQueueCapacity,
              livekit::telemetry::SessionTelemetry::Clock::now(), _executorLifetime)) {
    }

    Strand &strand() { return _strand; }
    const std::shared_ptr<void>& executorLifetime() const { return _executorLifetime; }
    // Closing the gate is serialized with the post itself, not just a racy
    // active check. The shutdown barrier is posted directly after revocation.
    template <typename F> bool post(F&& callback) {
        std::lock_guard lock(_callbackMutex);
        if (!_callbacksAccepted) return false;
        asio::post(_strand, std::forward<F>(callback));
        return true;
    }
    void revokeCallbacks() {
        std::lock_guard lock(_callbackMutex);
        _callbacksAccepted = false;
    }
    uint64_t generation() const { return _generation; }
    const QString &localUserId() const { return _localUserId; }
    const std::shared_ptr<livekit::telemetry::SessionTelemetry> &telemetry() const {
        return _telemetry;
    }

    void assertOnStrand() const {
        Q_ASSERT(_strand.running_in_this_thread());
    }

    bool acceptsDataOnStrand() const {
        assertOnStrand();
        return _acceptingData;
    }

    void stopAcceptingDataOnStrand(std::function<void()> onStopped = {}) {
        assertOnStrand();
        _acceptingData = false;
        if (onStopped) onStopped();
    }

    void stopTelemetryOnStrand(std::function<void()> onStopped = {}) {
        assertOnStrand();
        _telemetry->StopOnStrand(std::move(onStopped));
    }

    std::map<InboundTransferKey, InboundMediaTransfer> &transfersOnStrand() {
        assertOnStrand();
        return _inboundMediaTransfers;
    }

    std::shared_ptr<livekit::ScreenShareSession> &screenShareOnStrand() {
        assertOnStrand();
        return _screenShare;
    }

    std::shared_ptr<livekit::whiteboard::Runtime> &whiteboardOnStrand() {
        assertOnStrand();
        return _whiteboard;
    }

    std::map<std::string, livekit::ParticipantKey> &whiteboardPeersOnStrand() {
        assertOnStrand();
        return _whiteboardPeers;
    }

    std::map<std::string, livekit::ParticipantKey> &whiteboardDeparturesOnStrand() {
        assertOnStrand();
        return _whiteboardDepartures;
    }

    livekit::CatalogApplyResult updatePublicationCatalogOnStrand(
        const livekit::ParticipantEvent& event) {
        assertOnStrand();
        const auto result = _publicationCatalog.Apply(event);
        if (result == livekit::CatalogApplyResult::Applied) {
            _videoDemandPolicy.UpdateCatalog(_publicationCatalog.snapshot());
            completePendingRemoteMediaRecoveryOnStrand();
        }
        return result;
    }

    const livekit::PublicationCatalogSnapshot& publicationCatalogOnStrand() const {
        assertOnStrand();
        return _publicationCatalog.snapshot();
    }

    bool updateViewportIntentOnStrand(
        const livekit::ViewportIntent& intent,
        livekit::VideoDemandPolicy::TimePoint now =
            livekit::VideoDemandPolicy::Clock::now()) {
        assertOnStrand();
        const bool accepted = _videoDemandPolicy.UpdateViewport(intent, now);
        if (accepted) _hasViewportIntent = true;
        return accepted;
    }

    bool hasViewportIntentOnStrand() const {
        assertOnStrand();
        return _hasViewportIntent;
    }

    bool updateActiveSpeakersOnStrand(
        const std::vector<livekit::ActiveSpeakerInfo>& speakers,
        livekit::VideoDemandPolicy::TimePoint now =
            livekit::VideoDemandPolicy::Clock::now()) {
        assertOnStrand();
        return _videoDemandPolicy.UpdateSpeakers(speakers, now);
    }

    const livekit::VideoDemandPlan& reconcileVideoDemandOnStrand(
        livekit::VideoDemandPolicy::TimePoint now =
            livekit::VideoDemandPolicy::Clock::now()) {
        assertOnStrand();
        return _videoDemandPolicy.Reconcile(now);
    }

    const livekit::VideoDemandPlan& videoDemandPlanOnStrand() const {
        assertOnStrand();
        return _videoDemandPolicy.plan();
    }

    bool recordAcceptedVideoDemandOnStrand(
        const livekit::VideoDemandPlan& plan,
        uint64_t nativeRoomGeneration) {
        assertOnStrand();
        if (plan.coordinator_session != _generation ||
            nativeRoomGeneration == 0 ||
            plan.policy_revision == 0 ||
            (_videoPolicyTelemetry.policy_revision != 0 &&
             (nativeRoomGeneration < _videoPolicyTelemetry.native_room_generation ||
              (nativeRoomGeneration == _videoPolicyTelemetry.native_room_generation &&
               (plan.catalog_revision < _videoPolicyTelemetry.catalog_revision ||
                plan.policy_revision < _videoPolicyTelemetry.policy_revision))))) {
            livekit::telemetry::VideoPolicySample stale;
            stale.coordinator_session = plan.coordinator_session;
            stale.native_room_generation = nativeRoomGeneration;
            stale.catalog_revision = plan.catalog_revision;
            stale.policy_revision = plan.policy_revision;
            return _telemetry->RecordVideoPolicySampleOnStrand(std::move(stale));
        }

        const bool advances = plan.policy_revision !=
                _videoPolicyTelemetry.policy_revision ||
            plan.catalog_revision != _videoPolicyTelemetry.catalog_revision ||
            nativeRoomGeneration != _videoPolicyTelemetry.native_room_generation;
        _videoPolicyTelemetry.coordinator_session = _generation;
        _videoPolicyTelemetry.native_room_generation = nativeRoomGeneration;
        _videoPolicyTelemetry.catalog_revision = plan.catalog_revision;
        _videoPolicyTelemetry.policy_revision = plan.policy_revision;
        _videoPolicyTelemetry.stage_content = plan.stage_content;
        _videoPolicyTelemetry.reason = plan.reason;
        _videoPolicyTelemetry.requested = KeysForSeats(plan.visible_seats);
        _videoPolicyTelemetry.selected = UniqueKeys(plan.selected_video);
        _videoPolicyTelemetry.retired = false;
        if (advances) {
            _videoPolicyTelemetry.actual.clear();
            _videoPolicyTelemetry.bound.clear();
        }
        PublishVideoPolicyTelemetryOnStrand();
        return true;
    }

    bool recordVideoRenderSelectionOnStrand(
        livekit::VideoRenderSelectionObservation observation) {
        assertOnStrand();
        if (!_acceptingData || _videoPolicyTelemetry.retired ||
            observation.coordinator_session != _generation ||
            observation.coordinator_session !=
                _videoPolicyTelemetry.coordinator_session ||
            observation.native_room_generation !=
                _videoPolicyTelemetry.native_room_generation ||
            observation.catalog_revision !=
                _videoPolicyTelemetry.catalog_revision ||
            observation.policy_revision !=
                _videoPolicyTelemetry.policy_revision) {
            livekit::telemetry::VideoPolicySample stale;
            stale.coordinator_session = observation.coordinator_session;
            stale.native_room_generation = observation.native_room_generation;
            stale.catalog_revision = observation.catalog_revision;
            stale.policy_revision = observation.policy_revision;
            return _telemetry->RecordVideoPolicySampleOnStrand(std::move(stale));
        }
        _videoPolicyTelemetry.actual = UniqueKeys(observation.actual_video);
        _videoPolicyTelemetry.bound = UniqueKeys(observation.bound_video);
        PublishVideoPolicyTelemetryOnStrand();
        return true;
    }

    void scheduleVideoDemandReconcileOnStrand(
        std::function<void()> reconcile,
        livekit::VideoDemandPolicy::TimePoint now =
            livekit::VideoDemandPolicy::Clock::now()) {
        assertOnStrand();
        std::error_code ignored;
        _videoDemandTimer.cancel(ignored);
        const auto next = _videoDemandPolicy.NextReconcileAt(now);
        if (!next || !_acceptingData || !reconcile) return;
        _videoDemandTimer.expires_at(*next);
        _videoDemandTimer.async_wait(asio::bind_executor(
            _strand,
            [reconcile = std::move(reconcile)](const std::error_code& error) {
                if (!error) reconcile();
            }));
    }

    livekit::RemoteMediaPlan buildRemoteMediaPlanOnStrand(
        std::optional<livekit::RemoteMediaRecoveryRequest> recovery = std::nullopt,
        livekit::VideoDemandPolicy::TimePoint now =
            livekit::VideoDemandPolicy::Clock::now()) {
        assertOnStrand();
        const auto& demand = _videoDemandPolicy.Reconcile(now);
        const auto& catalog = _publicationCatalog.snapshot();
        livekit::RemoteMediaPlan result;
        result.coordinator_session = _generation;
        result.native_room_generation = catalog.native_room_generation;
        result.catalog_revision = catalog.catalog_revision;
        result.policy_revision = demand.policy_revision;
        if (recovery) {
            result.recovery_epoch = recovery->recovery_epoch;
            result.recovery_token = recovery->recovery_token;
        }
        for (const auto& participant : catalog.participants) {
            result.known_publications.reserve(
                result.known_publications.size() + participant.publications.size());
            for (const auto& publication : participant.publications) {
                result.known_publications.push_back(publication.key);
            }
        }
        result.audio = demand.selected_audio;
        result.video.reserve(demand.selected_video.size());
        for (const auto& key : demand.selected_video) {
            const auto seat = std::find_if(
                demand.visible_seats.begin(), demand.visible_seats.end(),
                [&](const auto& candidate) { return candidate.key == key; });
            if (seat == demand.visible_seats.end()) continue;
            livekit::RemoteTrackDemand track;
            track.key = key;
            track.policy_revision = demand.policy_revision;
            track.subscribed = true;
            track.enabled = true;
            track.width = seat->width;
            track.height = seat->height;
            track.quality = seat->quality;
            track.max_fps = seat->quality == livekit::VideoQualityTier::P180
                ? std::optional<uint32_t>{15}
                : std::optional<uint32_t>{30};
            track.priority = seat->priority;
            result.video.push_back(std::move(track));
        }
        return result;
    }

    using RemoteMediaRecoveryCompletion =
        std::function<void(livekit::RemoteMediaPlan)>;
    void requestRemoteMediaRecoveryOnStrand(
        livekit::RemoteMediaRecoveryRequest request,
        RemoteMediaRecoveryCompletion completion) {
        assertOnStrand();
        if (!completion || request.coordinator_session != _generation ||
            request.native_room_generation == 0) {
            return;
        }
        const auto current_generation =
            _publicationCatalog.snapshot().native_room_generation;
        if (current_generation == request.native_room_generation) {
            completion(buildRemoteMediaPlanOnStrand(request));
            return;
        }
        if (current_generation > request.native_room_generation) return;
        if (_pendingRemoteMediaRecovery &&
            request.recovery_epoch <=
                _pendingRemoteMediaRecovery->request.recovery_epoch) {
            return;
        }
        _pendingRemoteMediaRecovery = PendingRemoteMediaRecovery{
            std::move(request), std::move(completion)};
    }

    void stopVideoDemandOnStrand() {
        assertOnStrand();
        std::error_code ignored;
        _videoDemandTimer.cancel(ignored);
        _pendingRemoteMediaRecovery.reset();
        _hasViewportIntent = false;
        if (_videoPolicyTelemetry.coordinator_session != 0 &&
            !_videoPolicyTelemetry.retired) {
            _videoPolicyTelemetry.retired = true;
            _videoPolicyTelemetry.requested.clear();
            _videoPolicyTelemetry.selected.clear();
            _videoPolicyTelemetry.actual.clear();
            _videoPolicyTelemetry.bound.clear();
            PublishVideoPolicyTelemetryOnStrand();
        }
        _videoDemandPolicy.Retire();
        _publicationCatalog.Retire();
    }

private:
    struct VideoPolicyTelemetryState {
        uint64_t coordinator_session = 0;
        uint64_t native_room_generation = 0;
        uint64_t catalog_revision = 0;
        uint64_t policy_revision = 0;
        livekit::StageContent stage_content = livekit::StageContent::Video;
        livekit::VideoDemandReason reason = livekit::VideoDemandReason::Hidden;
        bool retired = false;
        std::vector<livekit::TrackKey> requested;
        std::vector<livekit::TrackKey> selected;
        std::vector<livekit::TrackKey> actual;
        std::vector<livekit::TrackKey> bound;
    };

    static std::vector<livekit::TrackKey> UniqueKeys(
        const std::vector<livekit::TrackKey>& values) {
        std::vector<livekit::TrackKey> result;
        result.reserve(values.size());
        for (const auto& value : values) {
            if (std::find(result.begin(), result.end(), value) == result.end()) {
                result.push_back(value);
            }
        }
        return result;
    }

    static std::vector<livekit::TrackKey> KeysForSeats(
        const std::vector<livekit::VideoSeat>& seats) {
        std::vector<livekit::TrackKey> result;
        result.reserve(seats.size());
        for (const auto& seat : seats) {
            if (!seat.IsParticipantPlaceholder()) result.push_back(seat.key);
        }
        return UniqueKeys(result);
    }

    static std::uint64_t DifferenceCount(
        const std::vector<livekit::TrackKey>& left,
        const std::vector<livekit::TrackKey>& right) {
        return static_cast<std::uint64_t>(std::count_if(
            left.begin(), left.end(), [&](const auto& key) {
                return std::find(right.begin(), right.end(), key) == right.end();
            }));
    }

    static const char* StageContentName(livekit::StageContent content) {
        return content == livekit::StageContent::Whiteboard
            ? "whiteboard" : "video";
    }

    static const char* VideoDemandReasonName(livekit::VideoDemandReason reason) {
        switch (reason) {
        case livekit::VideoDemandReason::Visible: return "visible";
        case livekit::VideoDemandReason::Pinned: return "pinned";
        case livekit::VideoDemandReason::ActiveSpeaker: return "active_speaker";
        case livekit::VideoDemandReason::ScreenShare: return "screen_share";
        case livekit::VideoDemandReason::Hidden: return "hidden";
        case livekit::VideoDemandReason::Whiteboard: return "whiteboard";
        case livekit::VideoDemandReason::PermissionDenied: return "permission_denied";
        case livekit::VideoDemandReason::Muted: return "muted";
        }
        return "hidden";
    }

    void PublishVideoPolicyTelemetryOnStrand() {
        livekit::telemetry::VideoPolicySample sample;
        sample.coordinator_session = _videoPolicyTelemetry.coordinator_session;
        sample.native_room_generation =
            _videoPolicyTelemetry.native_room_generation;
        sample.catalog_revision = _videoPolicyTelemetry.catalog_revision;
        sample.policy_revision = _videoPolicyTelemetry.policy_revision;
        sample.stage_content = StageContentName(
            _videoPolicyTelemetry.stage_content);
        sample.policy_reason = VideoDemandReasonName(_videoPolicyTelemetry.reason);
        sample.retired = _videoPolicyTelemetry.retired;
        sample.requested = _videoPolicyTelemetry.requested.size();
        sample.selected = _videoPolicyTelemetry.selected.size();
        sample.actual = _videoPolicyTelemetry.actual.size();
        sample.bound = _videoPolicyTelemetry.bound.size();
        sample.selected_not_requested = DifferenceCount(
            _videoPolicyTelemetry.selected, _videoPolicyTelemetry.requested);
        sample.selected_not_actual = DifferenceCount(
            _videoPolicyTelemetry.selected, _videoPolicyTelemetry.actual);
        sample.actual_not_selected = DifferenceCount(
            _videoPolicyTelemetry.actual, _videoPolicyTelemetry.selected);
        sample.selected_not_bound = DifferenceCount(
            _videoPolicyTelemetry.selected, _videoPolicyTelemetry.bound);
        sample.bound_not_selected = DifferenceCount(
            _videoPolicyTelemetry.bound, _videoPolicyTelemetry.selected);
        (void)_telemetry->RecordVideoPolicySampleOnStrand(std::move(sample));
    }

    struct PendingRemoteMediaRecovery {
        livekit::RemoteMediaRecoveryRequest request;
        RemoteMediaRecoveryCompletion completion;
    };

    void completePendingRemoteMediaRecoveryOnStrand() {
        assertOnStrand();
        if (!_pendingRemoteMediaRecovery) return;
        const auto generation =
            _publicationCatalog.snapshot().native_room_generation;
        if (generation <
            _pendingRemoteMediaRecovery->request.native_room_generation) {
            return;
        }
        auto pending = std::move(*_pendingRemoteMediaRecovery);
        _pendingRemoteMediaRecovery.reset();
        if (generation == pending.request.native_room_generation &&
            pending.completion) {
            pending.completion(buildRemoteMediaPlanOnStrand(pending.request));
        }
    }

    const std::shared_ptr<void> _executorLifetime;
    std::mutex _callbackMutex;
    bool _callbacksAccepted = true;
    Strand _strand;
    asio::steady_timer _videoDemandTimer;
    const uint64_t _generation;
    const QString _localUserId;
    livekit::PublicationCatalog _publicationCatalog;
    livekit::VideoDemandPolicy _videoDemandPolicy;
    VideoPolicyTelemetryState _videoPolicyTelemetry;
    bool _hasViewportIntent = false;
    std::optional<PendingRemoteMediaRecovery> _pendingRemoteMediaRecovery;
    std::shared_ptr<livekit::telemetry::SessionTelemetry> _telemetry;
    bool _acceptingData = true;
    std::map<InboundTransferKey, InboundMediaTransfer> _inboundMediaTransfers;
    std::shared_ptr<livekit::ScreenShareSession> _screenShare;
    std::shared_ptr<livekit::whiteboard::Runtime> _whiteboard;
    std::map<std::string, livekit::ParticipantKey> _whiteboardPeers;
    std::map<std::string, livekit::ParticipantKey> _whiteboardDepartures;
};

} // namespace OpenMeeting
