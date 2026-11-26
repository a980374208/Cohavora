#include <QtCore/QCoreApplication>
#include "src/core/detail/meeting_coordinator_internal.h"
#include "src/ui/meeting_log_console.h"
#include "src/telemetry/log_redaction.h"
#include "src/core/whiteboard/whiteboard_protocol.h"
#include "src/core/whiteboard/whiteboard_runtime.h"

#include <QtCore/QDateTime>
#include <QtCore/QPointer>
#include <QtCore/QThread>

#include <set>
#include <utility>

namespace OpenMeeting {
using namespace mc_detail;

namespace {
MeetingRoomInfo ToMeetingRoomInfo(const livekit::RoomInfo &info) {
    MeetingRoomInfo result;
    result.sid = QString::fromStdString(info.sid);
    result.name = QString::fromStdString(info.name);
    result.metadata = QString::fromStdString(info.metadata);
    result.emptyTimeout = info.empty_timeout;
    result.departureTimeout = info.departure_timeout;
    result.maxParticipants = info.max_participants;
    result.creationTimeMs = info.creation_time_ms;
    result.numParticipants = info.num_participants;
    result.numPublishers = info.num_publishers;
    result.activeRecording = info.active_recording;
    return result;
}

QString ParticipantKeyToken(const livekit::ParticipantKey &key) {
    return QString::number(key.native_room_generation) + QLatin1Char(':') +
        QString::number(key.incarnation) + QLatin1Char(':') +
        QString::number(static_cast<qulonglong>(key.sid.size())) + QLatin1Char(':') +
        QString::fromStdString(key.sid) + QLatin1Char(':') +
        QString::fromStdString(key.identity);
}
} // namespace

// ----------------------------------------------------
// CoordinatorRoomListener: LiveKit 事件监听与桥接
// ----------------------------------------------------
MeetingCoordinator::CoordinatorRoomListener::CoordinatorRoomListener(MeetingCoordinator *c,
                        const std::shared_ptr<MeetingSessionRuntime> &session)
    : _uiGate(c->_sessionUiGate),
      _session(session),
      _room(c->_room),
      _generation(session ? session->generation() : 0),
      _authGeneration(c->_sessionManager.authGeneration()),
      _localUserId(session ? session->localUserId() : QString()) {}

bool MeetingCoordinator::CoordinatorRoomListener::ConsumesParticipantEvents() const { return true; }

void MeetingCoordinator::CoordinatorRoomListener::OnParticipantEvent(const livekit::ParticipantEvent &event) {
    if (!_uiGate || !_uiGate->active() || _generation == 0) return;
    if (livekit::PublicationCatalog::Consumes(event.kind)) {
        auto session = _session.lock();
        if (!session || !session->enqueuePublicationCatalogEvent(
                session, event,
                [gate = _uiGate,
                 weakSession = std::weak_ptr<MeetingSessionRuntime>(session),
                 room = _room] {
                    if (auto current = weakSession.lock()) {
                        MeetingCoordinator::applyRemoteMediaDemandOnStrand(
                            gate, current, room);
                    }
                })) {
            return;
        }
    }
    if (event.kind == livekit::ParticipantEventKind::ActiveSpeakers) {
        auto session = _session.lock();
        if (!session || !session->post([gate = _uiGate, session,
                                        room = _room,
                                        speakers = event.speakers] {
                if (session->updateActiveSpeakersOnStrand(speakers)) {
                    MeetingCoordinator::applyRemoteMediaDemandOnStrand(
                        gate, session, room);
                }
            })) {
            return;
        }
    }
    if (event.kind == livekit::ParticipantEventKind::DataReceived &&
        event.topic == livekit::remote_control::Topic) {
        MeetingCoordinator::enqueueRemoteControlData(_session.lock(),event.data,event.sender);
        return;
    }
    if (event.kind == livekit::ParticipantEventKind::DataReceived &&
        livekit::whiteboard::isWhiteboardTopic(event.topic)) {
        auto session = _session.lock();
        if (!session || event.sender.origin == livekit::SenderOrigin::Server ||
            !livekit::IsParticipantTicketActive(event.sender.ticket, event.sender.key)) return;
        MeetingCoordinator::enqueueWhiteboardData(session, event.data, event.topic, event.sender);
        return;
    }
    if (event.kind == livekit::ParticipantEventKind::Upsert ||
        event.kind == livekit::ParticipantEventKind::Departure) {
        if (auto session = _session.lock()) {
            const auto key = event.participant.key;
            const auto ticket = event.participant.ticket;
            const bool departure = event.kind == livekit::ParticipantEventKind::Departure;
            session->post([session, key, ticket, departure] {
                const std::string identity = key.identity.empty() ? key.sid : key.identity;
                auto &peers = session->whiteboardPeersOnStrand();
                auto &departures = session->whiteboardDeparturesOnStrand();
                if (departure) {
                    const auto current = peers.find(identity);
                    if (current == peers.end() || current->second == key) peers.erase(identity);
                    departures[identity] = key;
                    if (departures.size() > 512) departures.erase(departures.begin());
                } else if (livekit::IsParticipantTicketActive(ticket, key)) {
                    peers[identity] = key;
                    departures.erase(identity);
                } else return;
                auto runtime = session->whiteboardOnStrand();
                if (!runtime) return;
                livekit::whiteboard::PeerInstance peer{
                    identity, key.native_room_generation, key.incarnation};
                if (departure) runtime->peerLeft(peer);
                else runtime->observePeer(peer);
            });
        }
    }
    const auto generation = _generation;
    const auto authGeneration = _authGeneration;
    const auto localUserId = _localUserId;
    _uiGate->Post([generation, authGeneration, localUserId, event](MeetingCoordinator* coordinator) {
        if (coordinator->applyAccountNotificationOnUiThread(generation, authGeneration, localUserId, event)) return;
        coordinator->applyParticipantEventOnUiThread(generation, event);
    });
}

void MeetingCoordinator::CoordinatorRoomListener::OnConnected() {
    if (!_uiGate || !_uiGate->active() || _generation == 0) return;
    const auto generation = _generation;
    _uiGate->Post([generation](MeetingCoordinator* coordinator) {
        QPointer<MeetingCoordinator> owner(coordinator);
        if (!owner->isCurrentSessionGenerationOnUiThread(generation)) return;
        const auto nativeGeneration = owner->_nativeRoomGeneration;

        // ConnectAsync only establishes the Room transport.  The startup
        // transaction publishes required local tracks before it reports a
        // successful meeting join.
        // 读取 Room 的统一原生快照。JoinResponse 与后续 RoomUpdate
        // 都已经提交到这里，避免 UI 从过期 protobuf 副本读取状态。
        if (owner->_room) {
            const auto native_room_info = owner->_room->room_info();
            const auto info = ToMeetingRoomInfo(native_room_info);
            owner->_roomInfo = info;
            emit owner->roomInfoUpdated(info);
            if (!owner || !owner->isCurrentSessionGenerationOnUiThread(generation) ||
                owner->_nativeRoomGeneration != nativeGeneration) return;
            owner->parseRoomMetadata(native_room_info.metadata);

        }
    });
}

void MeetingCoordinator::CoordinatorRoomListener::OnDisconnected(livekit::RoomDisconnectReason reason,
                    const std::string &detail) {
    if (auto session = _session.lock()) {
        session->post([session] {
            if (auto share = session->screenShareOnStrand()) share->SetTransportReady(false);
            if (auto board = session->whiteboardOnStrand()) board->setTransportReady(false, 0);
            if (auto control = session->remoteControlOnStrand()) control->stop();
        });
    }
    if (!_uiGate || !_uiGate->active() || _generation == 0) return;
    const auto generation = _generation;
    const QString qDetail = detail.empty()
        ? QCoreApplication::translate("MeetingUI", "No additional disconnect details")
        : QString::fromStdString(livekit::secure_log::OpaqueSummary("room_disconnect"));
    // 不捕获 listener 自身：DuplicateIdentity 处理会释放 _roomListener，
    // 捕获 this 会在回调执行期间留下悬垂指针风险。
    _uiGate->Post([generation, reason, qDetail](MeetingCoordinator* coordinator) {
        QPointer<MeetingCoordinator> owner(coordinator);
        if (!owner->isCurrentSessionGenerationOnUiThread(generation)) return;
        const auto nativeGeneration = owner->_nativeRoomGeneration;
        const auto current = [&] {
            return owner && owner->isCurrentSessionGenerationOnUiThread(generation) &&
                owner->_nativeRoomGeneration == nativeGeneration;
        };
        MeetingUI::LogToConsole(
            MeetingUI::LogCategory::Connection,
            "DISCONNECTED",
            QCoreApplication::translate("MeetingUI", "Disconnected from the LiveKit room: reason=%1, detail=%2")
                .arg(QString::fromLatin1(livekit::ToString(reason)), qDetail));
        if (!current()) return;
        if (reason == livekit::RoomDisconnectReason::DuplicateIdentity) {
            owner->handleDuplicateIdentityKickOff(QString());
            return;
        }
        if (owner->_state != MeetingState::Leaving) {
            owner->_screenShareSnapshot = {};
            emit owner->screenShareChanged({});
            if (!current()) return;
            const auto admissionGeneration = owner->_admissionGeneration;
            owner->setState(MeetingState::Leaving, qDetail);
            if (!current()) return;
            // A server disconnect retires the native session too. Publish
            // the terminal UI state only after its managed cleanup joins.
            owner->stopRoomSession([owner, generation, admissionGeneration, qDetail] {
                const auto stillStopped = [&] {
                    return owner && owner->_nextSessionGeneration == generation &&
                        owner->_admissionGeneration == admissionGeneration &&
                        !owner->_sessionRunning.load(std::memory_order_acquire) &&
                        !owner->_sessionInvalidated;
                };
                if (!stillStopped() || owner->_state != MeetingState::Leaving) return;
                owner->setState(MeetingState::Idle, qDetail);
                if (!stillStopped() || owner->_state != MeetingState::Idle) return;
                emit owner->meetingLeft();
            });
            if (owner && owner->_nextSessionGeneration == generation &&
                owner->_admissionGeneration == admissionGeneration &&
                !owner->_sessionRunning.load(std::memory_order_acquire) &&
                !owner->_sessionInvalidated &&
                (owner->_state == MeetingState::Leaving || owner->_state == MeetingState::Idle)) {
                // Window lifetime must not depend on a native join. Keep
                // meetingLeft as the fully drained business terminal.
                emit owner->meetingDisconnected();
            }
        }
    });
}

void MeetingCoordinator::CoordinatorRoomListener::OnReconnecting() {
    if (auto session = _session.lock()) {
        session->post([session] {
            if (auto share = session->screenShareOnStrand()) share->SetTransportReady(false);
            if (auto board = session->whiteboardOnStrand()) board->setTransportReady(false, 0);
            if (auto control = session->remoteControlOnStrand()) control->stop();
        });
    }
    if (!_uiGate || !_uiGate->active() || _generation == 0) return;
    const auto generation = _generation;
    _uiGate->Post([generation](MeetingCoordinator* coordinator) {
        QPointer<MeetingCoordinator> owner(coordinator);
        if (!owner->isCurrentSessionGenerationOnUiThread(generation) ||
            owner->_state == MeetingState::Leaving ||
            owner->_state == MeetingState::Idle) {
            return;
        }
        const auto nativeGeneration = owner->_nativeRoomGeneration;
        MeetingUI::LogToConsole(MeetingUI::LogCategory::Connection, "RECONNECTING",
                                QCoreApplication::translate("MeetingUI", "Network interrupted. Restoring the audio/video connection."));
        if (!owner || !owner->isCurrentSessionGenerationOnUiThread(generation) ||
            owner->_nativeRoomGeneration != nativeGeneration ||
            owner->_state == MeetingState::Leaving || owner->_state == MeetingState::Idle) return;
        owner->_startupReconnectPending = true;
        owner->_mediaReconnectGeneration = owner->_nativeRoomGeneration;
        owner->_mediaReconnectPeers.clear();
        for (const auto &[identity, participant] : owner->_participants)
            if (!participant.isLocal) owner->_mediaReconnectPeers.emplace(identity, participant.participantKey);
        owner->setState(MeetingState::Reconnecting,
                              QCoreApplication::translate("MeetingUI", "Network interrupted. Reconnecting..."));
    });
}

void MeetingCoordinator::CoordinatorRoomListener::OnReconnected() {
    if (auto session = _session.lock()) {
        session->post([session] {
            if (auto share = session->screenShareOnStrand()) share->SetTransportReady(true);
            if (auto board = session->whiteboardOnStrand()) {
                board->setTransportReady(true, static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch()));
            }
        });
    }
    if (!_uiGate || !_uiGate->active() || _generation == 0) return;
    const auto generation = _generation;
    _uiGate->Post([generation](MeetingCoordinator* coordinator) {
        QPointer<MeetingCoordinator> owner(coordinator);
        if (!owner->isCurrentSessionGenerationOnUiThread(generation) ||
            owner->_state == MeetingState::Leaving ||
            owner->_state == MeetingState::Idle) {
            return;
        }
        const auto nativeGeneration = owner->_nativeRoomGeneration;
        if (owner->_room && !owner->_businessRoomSid.isEmpty()) {
            const auto info = owner->_room->room_info();
            if (!info.sid.empty() && QString::fromStdString(info.sid) != owner->_businessRoomSid &&
                info.metadata.empty()) {
                // DeleteRoom can race transport recovery: an old token
                // creates a fresh, metadata-free SFU room with the same
                // name. It is not the admitted business meeting.
                owner->finishLeavingMeeting(false);
                if (owner && owner->_nextSessionGeneration == generation &&
                    !owner->_sessionRunning.load(std::memory_order_acquire) &&
                    !owner->_sessionInvalidated &&
                    (owner->_state == MeetingState::Leaving || owner->_state == MeetingState::Idle)) {
                    emit owner->meetingDisconnected();
                }
                return;
            }
        }
        owner->_startupReconnectPending = false;
        owner->processMediaRecovery(true);
        if (!owner->tryCommitOperationalStateOnUiThread(
                generation, QCoreApplication::translate("MeetingUI", "Audio/video connection restored"))) {
            return;
        }
        if (!owner->_startupCommitted) {
            MeetingUI::LogToConsole(
                MeetingUI::LogCategory::Connection,
                "RECONNECTED_DURING_STARTUP",
                QCoreApplication::translate("MeetingUI", "Connection restored. Waiting for local media startup to complete."));
            return;
        }
        if (!owner || !owner->isCurrentSessionGenerationOnUiThread(generation) ||
            owner->_nativeRoomGeneration != nativeGeneration ||
            owner->_state != MeetingState::InMeeting) return;
        MeetingUI::LogToConsole(MeetingUI::LogCategory::Connection, "RECONNECTED",
                                QCoreApplication::translate("MeetingUI", "Audio/video connection restored. Waiting for state projection to restore remote tracks."));
    });
}

void MeetingCoordinator::CoordinatorRoomListener::OnRoomMetadataChanged(const livekit::RoomInfo &room,
                           const std::string &oldMetadata,
                           const std::string &newMetadata) {
    if (!_uiGate || !_uiGate->active() || _generation == 0) return;
    const auto generation = _generation;
    const QString metadata = QString::fromStdString(newMetadata);
    _uiGate->Post([generation, metadata](MeetingCoordinator* coordinator) {
        if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
        coordinator->parseRoomMetadata(metadata.toStdString());
    });
}

void MeetingCoordinator::CoordinatorRoomListener::OnRoomUpdated(const livekit::RoomInfo &room) {
    if (!_uiGate || !_uiGate->active() || _generation == 0) return;
    const auto generation = _generation;
    const MeetingRoomInfo info = ToMeetingRoomInfo(room);
    _uiGate->Post([generation, info](MeetingCoordinator* coordinator) {
        if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
        coordinator->_roomInfo = info;
        emit coordinator->roomInfoUpdated(info);
    });
}

void MeetingCoordinator::CoordinatorRoomListener::OnConnectionQualityChanged(std::shared_ptr<livekit::Participant> participant,
                                livekit::ConnectionQuality quality,
                                float score) {
    if (!participant || (!_uiGate || !_uiGate->active()) || _generation == 0) return;
    const auto generation = _generation;
    const QString identity = QString::fromStdString(participant->identity());
    _uiGate->Post([generation, identity, quality, score](MeetingCoordinator* coordinator) {
        if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
        const auto it = coordinator->_participants.find(identity);
        if (it == coordinator->_participants.end()) return;
        if (it->second.connectionQuality == quality &&
            it->second.connectionQualityScore == score) {
            return;
        }
        it->second.connectionQuality = quality;
        it->second.connectionQualityScore = score;
        coordinator->updateParticipantListAndNotify();
        emit coordinator->participantConnectionQualityChanged(
            identity, static_cast<int>(quality), score);
    });
}

void MeetingCoordinator::CoordinatorRoomListener::OnTrackStreamStateChanged(
    std::shared_ptr<livekit::Participant> participant,
    std::shared_ptr<livekit::TrackPublication> publication,
    livekit::TrackPublication::StreamState state) {
    if (!participant || !publication || !publication->track() ||
        (!_uiGate || !_uiGate->active()) || _generation == 0) {
        return;
    }
    const auto generation = _generation;
    const QString identity = QString::fromStdString(participant->identity());
    const QString trackSid = QString::fromStdString(publication->sid());
    const bool isVideo = publication->track()->kind() == livekit::TrackKind::Video;
    const bool paused = state == livekit::TrackPublication::StreamState::Paused;

    // A participant may have multiple tracks of the same kind. Snapshot
    // their aggregate paused state on the native callback thread so the
    // Qt projection remains correct when one of them resumes.
    bool audioPaused = false;
    bool videoPaused = false;
    for (const auto &[sid, candidate] : participant->tracks()) {
        if (!candidate || !candidate->track() ||
            candidate->stream_state() != livekit::TrackPublication::StreamState::Paused) {
            continue;
        }
        if (candidate->track()->kind() == livekit::TrackKind::Audio &&
            IsMicrophoneSource(candidate->track()->source())) {
            audioPaused = true;
        } else if (candidate->track()->kind() == livekit::TrackKind::Video) {
            videoPaused = true;
        }
    }

    _uiGate->Post([generation, identity, trackSid, isVideo, paused,
                               audioPaused, videoPaused](MeetingCoordinator* coordinator) {
        if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
        const auto it = coordinator->_participants.find(identity);
        if (it == coordinator->_participants.end()) return;
        const bool aggregate_changed =
            it->second.isAudioStreamPaused != audioPaused ||
            it->second.isVideoStreamPaused != videoPaused;
        if (aggregate_changed) {
            it->second.isAudioStreamPaused = audioPaused;
            it->second.isVideoStreamPaused = videoPaused;
            coordinator->updateParticipantListAndNotify();
        }
        // A second track of the same kind can change state while the
        // participant-level aggregate remains paused. Preserve that
        // track-level event for consumers such as a per-track renderer.
        emit coordinator->participantTrackStreamStateChanged(
            identity, trackSid, isVideo, paused);
    });
}

void MeetingCoordinator::CoordinatorRoomListener::OnParticipantPermissionsChanged(
    const livekit::ParticipantPermission &oldPermission,
    const livekit::ParticipantPermission &newPermission,
    std::shared_ptr<livekit::Participant> participant) {
    if (!participant || (!_uiGate || !_uiGate->active()) || _generation == 0) return;
    const auto generation = _generation;
    const QString identity = QString::fromStdString(participant->identity());
    _uiGate->Post([generation, identity, newPermission](MeetingCoordinator* coordinator) {
        if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
        const auto it = coordinator->_participants.find(identity);
        if (it == coordinator->_participants.end()) return;
        it->second.permissions = newPermission;
        coordinator->updateParticipantListAndNotify();
        emit coordinator->participantPermissionsChanged(identity, newPermission);
    });
}

void MeetingCoordinator::CoordinatorRoomListener::OnTrackSubscriptionPermissionChanged(
    const livekit::TrackSubscriptionPermission &permission,
    std::shared_ptr<livekit::Participant> participant,
    std::shared_ptr<livekit::TrackPublication> publication) {
    if (!_uiGate || !_uiGate->active() || _generation == 0) return;
    const auto generation = _generation;
    const QString identity = participant
        ? QString::fromStdString(participant->identity())
        : QString();
    const QString participantSid = QString::fromStdString(permission.participant_sid);
    const QString trackSid = QString::fromStdString(permission.track_sid);
    const bool allowed = permission.allowed;
    _uiGate->Post([generation, identity, participantSid, trackSid, allowed](MeetingCoordinator* coordinator) {
        if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
        emit coordinator->trackSubscriptionPermissionChanged(
            identity, participantSid, trackSid, allowed);
    });
}

void MeetingCoordinator::CoordinatorRoomListener::OnParticipantConnected(std::shared_ptr<livekit::RemoteParticipant> p) {
    if (!p || (!_uiGate || !_uiGate->active()) || _generation == 0) return;
    const auto generation = _generation;
    QString id = QString::fromStdString(p->identity());
    QString realNick = ResolveParticipantNickname(p);
    _uiGate->Post([generation, id, realNick, p](MeetingCoordinator* coordinator) {
        if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
        ParticipantInfo info;
        info.identity = id;
        info.name = realNick;
        info.isLocal = false;
        info.isHost = (id == coordinator->_meetingDetail.effectiveHostUserId());
        info.isAudioMuted = true;
        info.isVideoEnabled = false;
        CopyParticipantState(p, &info);
        if (!coordinator->_ignoredTranscriptionSenderIdentity.isEmpty() &&
            id == coordinator->_ignoredTranscriptionSenderIdentity) {
            info.isAgent = true;
        }
        coordinator->_participants[id] = info;
        coordinator->updateParticipantListAndNotify();
        if (!info.isAgent) {
            MeetingUI::LogToConsole(MeetingUI::LogCategory::Participant, "REMOTE_JOIN", QCoreApplication::translate("MeetingUI", "Participant joined: %1 (display name: %2)").arg(id).arg(realNick));
            emit coordinator->participantJoined(id, realNick);
        }
    });
}

void MeetingCoordinator::CoordinatorRoomListener::OnParticipantMetadataChanged(std::shared_ptr<livekit::Participant> p,
                                  const std::string &old_metadata,
                                  const std::string &new_metadata) {
    if (!p || (!_uiGate || !_uiGate->active()) || _generation == 0) return;
    const auto generation = _generation;
    QString id = QString::fromStdString(p->identity());
    QString realNick = ResolveParticipantNickname(p);
    _uiGate->Post([generation, id, realNick](MeetingCoordinator* coordinator) {
        if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
        MeetingUI::LogToConsole(MeetingUI::LogCategory::Participant, "METADATA_CHANGED",
                                QCoreApplication::translate("MeetingUI", "Participant [%1] metadata updated. Display name: %2").arg(id).arg(realNick));
        auto it = coordinator->_participants.find(id);
        if (it != coordinator->_participants.end()) {
            if (!it->second.isLocal) {
                it->second.name = realNick;
            }
            const bool isAgent = it->second.isAgent ||
                (!coordinator->_ignoredTranscriptionSenderIdentity.isEmpty() &&
                 id == coordinator->_ignoredTranscriptionSenderIdentity);
            coordinator->updateParticipantListAndNotify();
            if (!isAgent) {
                emit coordinator->participantJoined(id, realNick);
            }
        }
    });
}

void MeetingCoordinator::CoordinatorRoomListener::OnParticipantDisconnected(std::shared_ptr<livekit::RemoteParticipant> p) {
    if (!p || (!_uiGate || !_uiGate->active()) || _generation == 0) return;
    const auto generation = _generation;
    QString id = QString::fromStdString(p->identity());
    _uiGate->Post([generation, id](MeetingCoordinator* coordinator) {
        if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
        const auto it = coordinator->_participants.find(id);
        const bool wasAgent = (it != coordinator->_participants.end() && it->second.isAgent) ||
            (!coordinator->_ignoredTranscriptionSenderIdentity.isEmpty() &&
             id == coordinator->_ignoredTranscriptionSenderIdentity);
        coordinator->_participants.erase(id);
        coordinator->updateParticipantListAndNotify();
        if (!wasAgent) {
            MeetingUI::LogToConsole(MeetingUI::LogCategory::Participant, "REMOTE_LEFT", QCoreApplication::translate("MeetingUI", "Participant left: %1").arg(id));
            emit coordinator->participantLeft(id);
        }
    });
}

void MeetingCoordinator::CoordinatorRoomListener::OnTrackSubscribed(std::shared_ptr<livekit::Track> track,
                       std::shared_ptr<livekit::TrackPublication> pub,
                       std::shared_ptr<livekit::RemoteParticipant> p) {
    if (!track || !p || (!_uiGate || !_uiGate->active()) || _generation == 0) return;
    const auto generation = _generation;
    std::string identity = p->identity();

    if (track->kind() == livekit::TrackKind::Video) {
        _uiGate->Post([generation, track, identity](MeetingCoordinator* coordinator) {
            if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
            emit coordinator->remoteVideoTrackAvailable(QString::fromStdString(identity), track);
        });
    }
}

void MeetingCoordinator::CoordinatorRoomListener::OnTrackUnsubscribed(std::shared_ptr<livekit::Track> track,
                         std::shared_ptr<livekit::TrackPublication> pub,
                         std::shared_ptr<livekit::RemoteParticipant> p) {
    if (!track || !p || (!_uiGate || !_uiGate->active()) || _generation == 0) return;
    const auto generation = _generation;
    QString id = QString::fromStdString(p->identity());
    bool isVideo = (track->kind() == livekit::TrackKind::Video);
    const bool isMicrophone = track->kind() == livekit::TrackKind::Audio &&
        IsMicrophoneSource(track->source());
    const std::string track_sid = track->sid();
    _uiGate->Post([generation, id, isVideo, isMicrophone, track_sid](MeetingCoordinator* coordinator) {
        if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
        if (isVideo) {
            emit coordinator->remoteVideoTrackUnavailable(id, QString::fromStdString(track_sid));
        }
        auto it = coordinator->_participants.find(id);
        if (it != coordinator->_participants.end()) {
            if (isVideo) {
                it->second.isVideoEnabled = false;
            } else if (isMicrophone) {
                it->second.isAudioMuted = true;
            }
            coordinator->updateParticipantListAndNotify();
        }
        if (isVideo || isMicrophone)
            emit coordinator->remoteTrackMuted(id, isVideo, true);
    });
}

void MeetingCoordinator::CoordinatorRoomListener::OnTrackMuted(std::shared_ptr<livekit::Participant> participant,
                  std::shared_ptr<livekit::TrackPublication> publication,
                  bool muted) {
    if (!participant || !publication || !publication->track() || (!_uiGate || !_uiGate->active()) || _generation == 0) return;
    const auto generation = _generation;
    QString id = QString::fromStdString(participant->identity());
    bool isVideo = (publication->track()->kind() == livekit::TrackKind::Video);
    const bool isMicrophone = publication->track()->kind() == livekit::TrackKind::Audio &&
        IsMicrophoneSource(publication->track()->source());
    _uiGate->Post([generation, id, isVideo, isMicrophone, muted](MeetingCoordinator* coordinator) {
        if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
        auto it = coordinator->_participants.find(id);
        if (it != coordinator->_participants.end()) {
            if (isVideo) {
                it->second.isVideoEnabled = !muted;
            } else if (isMicrophone) {
                it->second.isAudioMuted = muted;
            }
            coordinator->updateParticipantListAndNotify();
        }
        if (isVideo || isMicrophone)
            emit coordinator->remoteTrackMuted(id, isVideo, muted);
    });
}

void MeetingCoordinator::CoordinatorRoomListener::OnActiveSpeakersChanged(const std::vector<std::shared_ptr<livekit::Participant>> &speakers) {
    (void)speakers;
}

void MeetingCoordinator::CoordinatorRoomListener::OnDataReceived(const std::vector<uint8_t> &payload,
                    std::shared_ptr<livekit::RemoteParticipant> participant,
                    const std::string &topic) {
    auto session = _session.lock();
    if (!_uiGate || !_uiGate->active() || !session) return;
    (void)payload;
    (void)participant;
}

void MeetingCoordinator::CoordinatorRoomListener::OnTextStreamOpened(std::shared_ptr<livekit::TextStreamReader> reader,
                        std::shared_ptr<livekit::Participant> participant) {
    auto session = _session.lock();
    if (!_uiGate || !_uiGate->active() || !session || !reader) return;
    const auto generation = _generation;
    QString pId = participant ? QString::fromStdString(participant->identity()) : QString();
    _uiGate->Post([generation, reader, pId](MeetingCoordinator* coordinator) {
        if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
        emit coordinator->textStreamReceived(reader, pId);
    });
}

void MeetingCoordinator::CoordinatorRoomListener::OnByteStreamOpened(std::shared_ptr<livekit::ByteStreamReader> reader,
                        std::shared_ptr<livekit::Participant> participant) {
    auto session = _session.lock();
    if (!_uiGate || !_uiGate->active() || !session || !reader) return;
    const auto generation = _generation;
    QString pId = participant ? QString::fromStdString(participant->identity()) : QString();
    _uiGate->Post([generation, reader, pId](MeetingCoordinator* coordinator) {
        if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
        emit coordinator->byteStreamReceived(reader, pId);
    });
}

bool MeetingCoordinator::applyAccountNotificationOnUiThread(uint64_t sessionGeneration, quint64 authGeneration,
        const QString &localUserId, const livekit::ParticipantEvent &event) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (event.kind != livekit::ParticipantEventKind::DataReceived ||
        event.sender.origin != livekit::SenderOrigin::Server) return false;
    openmeeting::meeting::NotifyMeetingData notify;
    if (!notify.ParseFromArray(event.data.data(), static_cast<int>(event.data.size())) ||
        !notify.has_kickoffmeetingdata() ||
        notify.kickoffmeetingdata().reasoncode() != openmeeting::meeting::KickOffReason::DuplicatedLogin) return false;

    // Account state belongs to this Qt owner. Do not send an accepted account
    // notification through Qt -> strand -> Qt: the server sends RemoveParticipant
    // immediately after it, which can retire the room before that round trip ends.
    if (!isCurrentSessionGenerationOnUiThread(sessionGeneration) ||
        _sessionManager.authGeneration() != authGeneration || !_sessionManager.isLoggedIn() ||
        _sessionManager.userId() != localUserId ||
        QString::fromStdString(notify.kickoffmeetingdata().userid()) != localUserId) return true;
    QPointer<MeetingCoordinator> owner(this);
    MeetingUI::LogToConsole(MeetingUI::LogCategory::Connection, "DUPLICATED_LOGIN",
        "[Coordinator] Apply trusted server duplicated-login event");
    if (!owner || !owner->isCurrentSessionGenerationOnUiThread(sessionGeneration) ||
        owner->_sessionManager.authGeneration() != authGeneration) return true;
    owner->_sessionManager.invalidateSession(SessionInvalidationReason::DuplicatedLogin);
    return true;
}

std::shared_ptr<livekit::RoomListener> MeetingCoordinator::participantEventListenerForInspection(
    const std::shared_ptr<MeetingSessionRuntime> &session, bool retainForOwnedSession) {
    auto listener = std::make_shared<CoordinatorRoomListener>(this, session);
    if (retainForOwnedSession) _roomListener = listener;
    return listener;
}

void MeetingCoordinator::applyParticipantEventOnUiThread(
    uint64_t coordinatorGeneration,
    const livekit::ParticipantEvent &event) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!isCurrentSessionGenerationOnUiThread(coordinatorGeneration)) return;
    // Every signal below may synchronously destroy the receiver or deliver a
    // replacement roster. Establish the weak owner before the first effect.
    QPointer<MeetingCoordinator> owner(this);
    const auto eventRoomGeneration = event.participant.key.native_room_generation;
    auto sessionStillCurrent = [&]() {
        return owner &&
            owner->isCurrentSessionGenerationOnUiThread(coordinatorGeneration) &&
            (eventRoomGeneration == 0 ||
             owner->_nativeRoomGeneration == eventRoomGeneration);
    };

    const bool participantCleanup =
        event.kind == livekit::ParticipantEventKind::Departure;
    const bool trackCleanup =
        event.kind == livekit::ParticipantEventKind::TrackUnavailable ||
        event.kind == livekit::ParticipantEventKind::DataTrackUnpublished;
    const bool requiresParticipantTicket =
        event.kind != livekit::ParticipantEventKind::ActiveSpeakers &&
        event.kind != livekit::ParticipantEventKind::DataReceived &&
        event.kind != livekit::ParticipantEventKind::TranscriptionReceived &&
        !participantCleanup &&
        !trackCleanup;

    if (requiresParticipantTicket &&
        !livekit::IsParticipantTicketActive(
            event.participant.ticket, event.participant.key)) {
        return;
    }

    if (!participantCleanup && !trackCleanup &&
        event.participant.key.native_room_generation != 0) {
        if (_nativeRoomGeneration != 0 &&
            eventRoomGeneration < _nativeRoomGeneration) {
            return;
        }
        if (_nativeRoomGeneration == 0 ||
            eventRoomGeneration > _nativeRoomGeneration) {
            std::vector<QString> oldRemoteIdentities;
            for (const auto &[identity, info] : _participants) {
                if (!info.isLocal && !info.isAgent) oldRemoteIdentities.push_back(identity);
                if (_mediaReconnectGeneration == 0 ||
                    eventRoomGeneration <= _mediaReconnectGeneration ||
                    info.participantKey.native_room_generation > _mediaReconnectGeneration)
                    cancelInboundTransfersForParticipant(info.participantKey);
            }
            _nativeRoomGeneration = eventRoomGeneration;
            _participants.clear();
            if (!_remoteVideoTracks.empty()) {
                _remoteVideoTracks.clear();
                ++_remoteVideoTracksRevision;
            }
            _participantEventSequences.clear();
            updateParticipantListAndNotify();
            if (!sessionStillCurrent()) return;
            for (const auto &identity : oldRemoteIdentities) {
                // A reentrant roster may already have installed its successor.
                if (owner->_participants.find(identity) != owner->_participants.end()) continue;
                emit participantLeft(identity);
                if (!sessionStillCurrent()) return;
            }
        }
    }

    const auto key = event.participant.key;
    const QString identity = QString::fromStdString(
        key.identity.empty() ? key.sid : key.identity);
    const QString sequenceKey = ParticipantKeyToken(key);
    if (!sequenceKey.isEmpty() && key.incarnation != 0) {
        auto &last = _participantEventSequences[sequenceKey];
        if (event.event_sequence <= last) return;
        last = event.event_sequence;
    }

    auto participantStillCurrent = [&]() {
        if (!sessionStillCurrent()) return false;
        const auto it = owner->_participants.find(identity);
        return it != owner->_participants.end() &&
            it->second.participantKey == key;
    };
    auto valueStillCurrent = [&]() {
        if (!sessionStillCurrent() ||
            !livekit::IsParticipantTicketActive(event.participant.ticket, key)) return false;
        const auto sequence = owner->_participantEventSequences.find(sequenceKey);
        return sequence != owner->_participantEventSequences.end() &&
            sequence->second == event.event_sequence;
    };

    if (event.kind == livekit::ParticipantEventKind::Departure) {
        // Resource cancellation belongs to the retired key and must be queued
        // before any UI effect can replace the session or delete this owner.
        cancelInboundTransfersForParticipant(key);
        if (const auto reconnectPeer = _mediaReconnectPeers.find(identity);
            reconnectPeer != _mediaReconnectPeers.end() && reconnectPeer->second == key)
            _mediaReconnectPeers.erase(reconnectPeer);
        _participantEventSequences.erase(sequenceKey);
        std::vector<QString> trackSids;
        const auto tracks = _remoteVideoTracks.find(identity);
        if (tracks != _remoteVideoTracks.end()) {
            for (const auto &[sid, value] : tracks->second) {
                if (value.key.participant == key) trackSids.push_back(sid);
            }
        }
        if (participantStillCurrent()) {
            const auto existing = _participants.find(identity);
            const bool wasAgent = (existing != _participants.end() && existing->second.isAgent) ||
                (!_ignoredTranscriptionSenderIdentity.isEmpty() &&
                 identity == _ignoredTranscriptionSenderIdentity);
            _participants.erase(identity);
            removeRemoteAudioTracksForParticipant(identity);
            if (_remoteVideoTracks.erase(identity) > 0) {
                ++_remoteVideoTracksRevision;
            }
            updateParticipantListAndNotify();
            if (!sessionStillCurrent()) return;
            for (const auto &trackSid : trackSids) {
                if (owner->_participants.find(identity) != owner->_participants.end()) return;
                emit remoteVideoTrackUnavailable(identity, trackSid);
                if (!sessionStillCurrent()) return;
            }
            if (owner->_participants.find(identity) != owner->_participants.end()) return;
            if (!wasAgent) {
                emit participantLeft(identity);
            }
        }
        return;
    }

    if (event.kind == livekit::ParticipantEventKind::ActiveSpeakers) {
        for (auto &[id, participant] : _participants) {
            participant.isSpeaking = false;
            participant.audioLevel = 0.0f;
        }
        std::vector<livekit::ActiveSpeakerInfo> accepted;
        for (const auto &speaker : event.speakers) {
            if (!livekit::IsParticipantTicketActive(speaker.ticket, speaker.key)) continue;
            const QString speakerIdentity = QString::fromStdString(
                speaker.key.identity.empty() ? speaker.key.sid : speaker.key.identity);
            const auto it = _participants.find(speakerIdentity);
            if (it == _participants.end() ||
                it->second.participantKey != speaker.key) {
                continue;
            }
            it->second.isSpeaking = speaker.speaking;
            it->second.audioLevel = speaker.audio_level;
            if (it->second.isLocal && !_localAudioLevelSubscription) {
                const float lvl = std::clamp(speaker.audio_level, 0.0f, 1.0f);
                if (std::abs(_localAudioLevel - lvl) > 0.005f) {
                    _localAudioLevel = lvl;
                    emit localAudioLevelChanged(lvl);
                }
            }
            accepted.push_back(speaker);
        }
        updateParticipantListAndNotify();
        if (!sessionStillCurrent()) return;
        accepted.erase(std::remove_if(accepted.begin(), accepted.end(), [&](const auto &speaker) {
            if (!livekit::IsParticipantTicketActive(speaker.ticket, speaker.key)) return true;
            const QString id = QString::fromStdString(
                speaker.key.identity.empty() ? speaker.key.sid : speaker.key.identity);
            const auto current = owner->_participants.find(id);
            return current == owner->_participants.end() ||
                current->second.participantKey != speaker.key;
        }), accepted.end());
        emit activeSpeakersChanged(accepted);
        return;
    }

    if (event.kind == livekit::ParticipantEventKind::DataReceived) {
        const bool serverOrigin = event.sender.origin == livekit::SenderOrigin::Server;
        // Named application/stream channels must not fall through to the
        // legacy text-chat parser. Empty topic retains the existing protocol;
        // server-origin control notifications keep their separate trust path.
        if (!serverOrigin && !event.topic.empty() && event.topic != "chat") return;
        if (!serverOrigin &&
            !livekit::IsParticipantTicketActive(
                event.sender.ticket, event.sender.key)) {
            return;
        }
        auto session = _sessionRuntime;
        if (session) enqueueDataReceived(session, event.data, event.sender);
        return;
    }

    if (event.kind == livekit::ParticipantEventKind::TranscriptionReceived) {
        if (!event.sender.encryptionCurrent()) return;
        const bool serverOrigin = event.sender.origin == livekit::SenderOrigin::Server;
        const bool resolvedOrigin = event.sender.origin == livekit::SenderOrigin::Unresolved;
        if (!serverOrigin && !resolvedOrigin &&
            !livekit::IsParticipantTicketActive(
                event.sender.ticket, event.sender.key)) {
            return;
        }
        if (!event.transcription.has_value() || event.transcription->segments.empty()) {
            return;
        }
        constexpr int kMaxStoredMeetingTranscripts = 120;
        const auto &tr = *event.transcription;
        const QString senderId = QString::fromStdString(tr.sender_identity);
        if (!_ignoredTranscriptionSenderIdentity.isEmpty() &&
            senderId == _ignoredTranscriptionSenderIdentity) {
            return;
        }
        const QString speakerIdentity = QString::fromStdString(
            !tr.transcribed_participant_identity.empty()
                ? tr.transcribed_participant_identity
                : tr.sender_identity);
        QString speakerName = speakerIdentity;
        bool isLocalSpeaker = (!speakerIdentity.isEmpty() && speakerIdentity == _sessionManager.userId());
        if (const auto pit = _participants.find(speakerIdentity); pit != _participants.end()) {
            isLocalSpeaker = pit->second.isLocal;
            if (!pit->second.name.trimmed().isEmpty()) {
                speakerName = pit->second.name.trimmed();
            }
        } else if (speakerIdentity.isEmpty() && !senderId.isEmpty()) {
            speakerName = senderId;
        }
        if (speakerName.isEmpty()) {
            speakerName = QCoreApplication::translate("MeetingUI", "Participant");
        }

        const QString trackSid = QString::fromStdString(tr.track_sid);
        const auto segments = tr.segments;
        for (const auto &seg : segments) {
            if (!sessionStillCurrent()) return;
            MeetingTranscriptItem item;
            item.segmentId = QString::fromStdString(seg.id);
            item.senderIdentity = senderId;
            item.speakerIdentity = speakerIdentity;
            item.speakerName = speakerName;
            item.trackSid = trackSid;
            item.text = QString::fromStdString(seg.text);
            item.sourceText = QString::fromStdString(seg.source_text);
            item.sourceLanguage = QString::fromStdString(seg.source_language);
            item.language = QString::fromStdString(seg.language);
            item.timestampMs = seg.last_received_time_ms > 0
                ? static_cast<qint64>(seg.last_received_time_ms)
                : QDateTime::currentMSecsSinceEpoch();
            item.isFinal = seg.is_final;
            item.isLocalSpeaker = isLocalSpeaker;
            item.isAgentGenerated = tr.generated;
            item.isTranslated = seg.is_translated;

            if (!item.text.trimmed().isEmpty()) {
                if (item.isLocalSpeaker) {
                    _roomHasLocalTranscription = true;
                }
                int existingIdx = -1;
                if (!item.segmentId.isEmpty()) {
                    for (int idx = _recentTranscripts.size() - 1; idx >= 0; --idx) {
                        const auto &existing = _recentTranscripts.at(idx);
                        if (existing.segmentId == item.segmentId &&
                            existing.speakerIdentity == item.speakerIdentity) {
                            existingIdx = idx;
                            break;
                        }
                    }
                }
                if (existingIdx >= 0) {
                    _recentTranscripts[existingIdx] = item;
                } else {
                    _recentTranscripts.push_back(item);
                    while (_recentTranscripts.size() > kMaxStoredMeetingTranscripts) {
                        _recentTranscripts.removeFirst();
                    }
                }
            }
            emit transcriptionReceived(item);
            if (!sessionStillCurrent()) return;
        }
        return;
    }

    if (event.kind == livekit::ParticipantEventKind::TextStreamOpened ||
        event.kind == livekit::ParticipantEventKind::ByteStreamOpened) {
        if (!livekit::IsParticipantTicketActive(
                event.sender.ticket, event.sender.key)) {
            return;
        }
        const QString senderIdentity = QString::fromStdString(
            event.sender.key.identity.empty()
                ? event.sender.key.sid
                : event.sender.key.identity);
        if (event.kind == livekit::ParticipantEventKind::TextStreamOpened &&
            event.text_reader) {
            emit textStreamReceived(event.text_reader, senderIdentity);
        } else if (event.byte_reader) {
            emit byteStreamReceived(event.byte_reader, senderIdentity);
        }
        return;
    }

    if (event.kind == livekit::ParticipantEventKind::TrackUnavailable) {
        const QString trackSid = QString::fromStdString(
            event.track_key.publication_sid);
        if (const auto audioIt = _remoteAudioTracks.find(trackSid);
            audioIt != _remoteAudioTracks.end()) {
            _remoteAudioTracks.erase(audioIt);
        }
        auto participantTracks = _remoteVideoTracks.find(identity);
        if (participantTracks == _remoteVideoTracks.end()) return;
        const auto track = participantTracks->second.find(trackSid);
        if (track == participantTracks->second.end() ||
            track->second.key != event.track_key ||
            (event.media_binding_key.serial != 0 &&
             track->second.mediaBindingKey != event.media_binding_key)) {
            return;
        }
        participantTracks->second.erase(track);
        if (participantTracks->second.empty()) {
            _remoteVideoTracks.erase(participantTracks);
        }
        ++_remoteVideoTracksRevision;
        emit remoteVideoTrackUnavailable(identity, trackSid);
        return;
    }

    if (event.kind == livekit::ParticipantEventKind::DataTrackUnpublished) {
        if (_nativeRoomGeneration != 0 && eventRoomGeneration != 0 &&
            eventRoomGeneration < _nativeRoomGeneration) {
            return;
        }
        const QString trackSid = QString::fromStdString(event.data_track_sid);
        if (!identity.isEmpty() && !trackSid.isEmpty()) {
            emit dataTrackUnpublished(identity, trackSid);
        }
        return;
    }

    if (!livekit::IsParticipantTicketActive(
            event.participant.ticket, key)) {
        return;
    }

    if (event.kind == livekit::ParticipantEventKind::DataTrackPublished) {
        if (!identity.isEmpty() && event.data_track) {
            emit dataTrackPublished(identity, event.data_track);
        }
        return;
    }

    if (event.kind == livekit::ParticipantEventKind::Upsert ||
        event.kind == livekit::ParticipantEventKind::ConnectionQuality ||
        event.kind == livekit::ParticipantEventKind::TrackMuted ||
        event.kind == livekit::ParticipantEventKind::TrackStreamState ||
        event.kind == livekit::ParticipantEventKind::TrackSubscriptionPermission ||
        event.kind == livekit::ParticipantEventKind::TrackSubscriptionError) {
        const auto existing = _participants.find(identity);
        const bool replacing = existing != _participants.end() &&
            existing->second.participantKey != key;
        const bool prevWasAgent = (existing != _participants.end() && !replacing)
            ? existing->second.isAgent : false;
        const bool replacedWasAgent = replacing && existing->second.isAgent;
        const bool isNew = existing == _participants.end() || replacing;
        if (replacing) {
            const auto oldKey = existing->second.participantKey;
            _participants.erase(existing);
            if (_remoteVideoTracks.erase(identity) > 0) {
                ++_remoteVideoTracksRevision;
            }
            cancelInboundTransfersForParticipant(oldKey);
            updateParticipantListAndNotify();
            if (!valueStillCurrent() || owner->_participants.find(identity) != owner->_participants.end()) return;
            if (!replacedWasAgent) {
                emit participantLeft(identity);
                if (!valueStillCurrent() || owner->_participants.find(identity) != owner->_participants.end()) return;
            }
        }

        ParticipantInfo info;
        if (const auto current = _participants.find(identity);
            current != _participants.end()) {
            info = current->second;
        }
        info.identity = identity;
        info.name = ResolveParticipantNickname(event.participant.state);
        info.isLocal = event.participant.is_local;
        info.isHost = identity == _meetingDetail.effectiveHostUserId();
        info.isAudioMuted = info.isLocal ? _audioMuted : true;
        info.hasScreenShareAudio = false;
        info.isScreenShareAudioMuted = true;
        info.isScreenShareAudioStreamPaused = false;
        info.isVideoEnabled = info.isLocal ? _videoEnabled : false;
        info.isSpeaking = event.participant.state.speaking;
        info.audioLevel = event.participant.state.audio_level;
        info.participantKey = key;
        info.participantTicket = event.participant.ticket;
        info.lastEventSequence = event.event_sequence;
        CopyParticipantState(event.participant.state, &info);
        if (!_ignoredTranscriptionSenderIdentity.isEmpty() &&
            identity == _ignoredTranscriptionSenderIdentity) {
            info.isAgent = true;
        }
        // Local effective state includes device readiness and newer UI intent.
        // An older publication snapshot must not re-enable a failed device.
        for (const auto &publication : event.participant.state.publications) {
            if (publication.kind == livekit::TrackKind::Audio) {
                if (!info.isLocal && IsMicrophoneSource(publication.source))
                    info.isAudioMuted = info.isAudioMuted && publication.muted;
                if (publication.source == livekit::TrackSource::ScreenShareAudio) {
                    info.hasScreenShareAudio = true;
                    info.isScreenShareAudioMuted = info.isScreenShareAudioMuted && publication.muted;
                    info.isScreenShareAudioStreamPaused = info.isScreenShareAudioStreamPaused ||
                        publication.stream_state == livekit::TrackPublication::StreamState::Paused;
                }
            } else if (publication.track && publication.kind == livekit::TrackKind::Video) {
                if (!info.isLocal) info.isVideoEnabled = !publication.muted;
                const auto participantTracks = _remoteVideoTracks.find(identity);
                if (participantTracks != _remoteVideoTracks.end()) {
                    const auto video = participantTracks->second.find(QString::fromStdString(publication.sid));
                    if (video != participantTracks->second.end() &&
                        video->second.key.participant == key && video->second.track == publication.track) {
                        const bool paused = publication.stream_state == livekit::TrackPublication::StreamState::Paused;
                        if (video->second.muted != publication.muted || video->second.paused != paused) {
                            video->second.muted = publication.muted;
                            video->second.paused = paused;
                            ++_remoteVideoTracksRevision;
                        }
                    }
                }
            }
        }
        _participants[identity] = info;
        if (_mediaReconnectGeneration != 0 && !info.isLocal &&
            key.native_room_generation > _mediaReconnectGeneration)
            restoreInboundTransfersForParticipant(key, event.participant.ticket);
        updateParticipantListAndNotify();
        if (!valueStillCurrent() || !participantStillCurrent()) return;

        if (info.isAgent) {
            if (!isNew && !prevWasAgent) {
                emit participantLeft(identity);
                if (!valueStillCurrent() || !participantStillCurrent()) return;
            }
        } else if ((isNew && !info.isLocal) || (!isNew && prevWasAgent)) {
            emit participantJoined(identity, info.name);
            if (!valueStillCurrent() || !participantStillCurrent()) return;
        }

        if (event.kind == livekit::ParticipantEventKind::ConnectionQuality) {
            emit participantConnectionQualityChanged(
                identity,
                static_cast<int>(info.connectionQuality),
                info.connectionQualityScore);
        } else if (event.kind == livekit::ParticipantEventKind::TrackMuted &&
                   (event.publication.kind == livekit::TrackKind::Video ||
                    (event.publication.kind == livekit::TrackKind::Audio &&
                     IsMicrophoneSource(event.publication.source)))) {
            emit remoteTrackMuted(
                identity,
                event.publication.kind == livekit::TrackKind::Video,
                event.publication.muted);
        } else if (event.kind == livekit::ParticipantEventKind::TrackStreamState) {
            emit participantTrackStreamStateChanged(
                identity,
                QString::fromStdString(event.track_key.publication_sid),
                event.publication.kind == livekit::TrackKind::Video,
                event.publication.stream_state ==
                    livekit::TrackPublication::StreamState::Paused);
        } else if (event.kind == livekit::ParticipantEventKind::TrackSubscriptionPermission) {
            emit trackSubscriptionPermissionChanged(
                identity,
                QString::fromStdString(key.sid),
                QString::fromStdString(event.track_key.publication_sid),
                event.publication.subscription_allowed);
        }
    }

    if (event.kind == livekit::ParticipantEventKind::TrackAvailable) {
        if (!participantStillCurrent() ||
            !livekit::IsTrackTicketActive(
                event.track_ticket, event.track_key) ||
            !livekit::IsMediaBindingTicketActive(
                event.media_binding_ticket, event.media_binding_key) ||
            !event.publication.track) {
            return;
        }
        const QString trackSid = QString::fromStdString(
            event.track_key.publication_sid);
        if (event.publication.kind == livekit::TrackKind::Audio) {
            if (!event.participant.is_local &&
                IsMicrophoneSource(event.publication.source)) {
                const auto pit = _participants.find(identity);
                const bool isAgent = (pit != _participants.end() && pit->second.isAgent);
                if (!isAgent) {
                    QString speakerName = identity;
                    if (pit != _participants.end() && !pit->second.name.trimmed().isEmpty()) {
                        speakerName = pit->second.name.trimmed();
                    }
                    auto &entry = _remoteAudioTracks[trackSid];
                    entry.subscription.reset();
                    entry.speakerIdentity = identity;
                    entry.speakerName = speakerName;
                    entry.trackSid = trackSid;
                    entry.track = event.publication.track;
                    bindRemoteAudioTrackSubscription(entry);
                }
            }
            return;
        }
        if (event.publication.kind != livekit::TrackKind::Video) {
            return;
        }
        const auto participantTracks = _remoteVideoTracks.find(identity);
        if (participantTracks != _remoteVideoTracks.end()) {
            const auto existing = participantTracks->second.find(trackSid);
            if (existing != participantTracks->second.end() &&
                existing->second.key == event.track_key &&
                existing->second.mediaBindingKey == event.media_binding_key &&
                existing->second.track == event.publication.track) {
                return;
            }
        }
        _remoteVideoTracks[identity][trackSid] = {
            event.track_key,
            event.track_ticket,
            event.media_binding_key,
            event.media_binding_ticket,
            event.publication.track,
            event.publication.muted,
            event.publication.stream_state == livekit::TrackPublication::StreamState::Paused};
        ++_remoteVideoTracksRevision;
        emit remoteVideoTrackAvailable(identity, event.publication.track);
    }
}

} // namespace OpenMeeting
