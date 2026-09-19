#include "src/core/meeting_coordinator.h"
#include "src/net/service_endpoint_policy.h"
#include "src/ui/meeting_log_console.h"
#include "src/telemetry/log_redaction.h"

#include <QtCore/QDebug>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonDocument>
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtCore/QThread>

#include <exception>
#include <future>
#include <utility>

namespace OpenMeeting {

// 从 LiveKit Participant 中提取真实用户昵称 (优先解析 OpenMeeting metadata 中的 JSON userInfo.nickname)
static QString ResolveParticipantNickname(const livekit::ParticipantStateSnapshot &state) {
    QString identity = QString::fromStdString(state.identity).trimmed();
    QString metaStr = QString::fromStdString(state.metadata).trimmed();
    if (!metaStr.isEmpty()) {
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(metaStr.toUtf8(), &err);
        if (err.error == QJsonParseError::NoError && doc.isObject()) {
            QJsonObject obj = doc.object();
            // 1. userInfo.nickname (OpenMeeting 核心标准)
            if (obj.contains("userInfo") && obj["userInfo"].isObject()) {
                QJsonObject userObj = obj["userInfo"].toObject();
                QString nick = userObj["nickname"].toString().trimmed();
                if (!nick.isEmpty()) return nick;
                QString uName = userObj["name"].toString().trimmed();
                if (!uName.isEmpty() && uName != "participant-name") return uName;
            }
            // 2. 根级 nickname
            QString rootNick = obj["nickname"].toString().trimmed();
            if (!rootNick.isEmpty()) return rootNick;
            // 3. 根级 name (排除 participant-name)
            QString rootName = obj["name"].toString().trimmed();
            if (!rootName.isEmpty() && rootName != "participant-name") return rootName;
        }
    }
    // 4. Participant 原生 name (排除 participant-name)
    QString rawName = QString::fromStdString(state.name).trimmed();
    if (!rawName.isEmpty() && rawName != "participant-name") {
        return rawName;
    }
    // 5. 回退 identity (纯数字 ID)
    return identity;
}

static QString ResolveParticipantNickname(const std::shared_ptr<livekit::Participant> &p) {
    return p ? ResolveParticipantNickname(p->SnapshotState()) : QString();
}

static MeetingRoomInfo ToMeetingRoomInfo(const livekit::RoomInfo &info) {
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

static void CopyParticipantState(const livekit::ParticipantStateSnapshot &state,
                                 ParticipantInfo *info) {
    if (!info) return;
    info->connectionQuality = state.connection_quality;
    info->connectionQualityScore = state.connection_quality_score;
    info->permissions = state.permission;

    bool audio_paused = false;
    bool video_paused = false;
    for (const auto &publication : state.publications) {
        if (!publication.track ||
            publication.stream_state != livekit::TrackPublication::StreamState::Paused) {
            continue;
        }
        if (publication.kind == livekit::TrackKind::Audio) {
            audio_paused = true;
        } else if (publication.kind == livekit::TrackKind::Video) {
            video_paused = true;
        }
    }
    info->isAudioStreamPaused = audio_paused;
    info->isVideoStreamPaused = video_paused;
}

static void CopyParticipantState(const std::shared_ptr<livekit::Participant> &participant,
                                 ParticipantInfo *info) {
    if (participant) CopyParticipantState(participant->SnapshotState(), info);
}

static QString ParticipantKeyToken(const livekit::ParticipantKey &key) {
    return QString::number(key.native_room_generation) + QLatin1Char(':') +
        QString::number(key.incarnation) + QLatin1Char(':') +
        QString::number(static_cast<qulonglong>(key.sid.size())) + QLatin1Char(':') +
        QString::fromStdString(key.sid) + QLatin1Char(':') +
        QString::fromStdString(key.identity);
}

// ----------------------------------------------------
// CoordinatorRoomListener: LiveKit 事件监听与桥接
// ----------------------------------------------------
class MeetingCoordinator::CoordinatorRoomListener : public livekit::RoomListener {
public:
    CoordinatorRoomListener(MeetingCoordinator *c,
                            const std::shared_ptr<MeetingSessionRuntime> &session)
        : _coordinator(c),
          _session(session),
          _generation(session ? session->generation() : 0) {}

    bool ConsumesParticipantEvents() const override { return true; }

    void OnParticipantEvent(const livekit::ParticipantEvent &event) override {
        auto *coordinator = _coordinator;
        if (!coordinator || _generation == 0) return;
        const auto generation = _generation;
        QMetaObject::invokeMethod(coordinator, [coordinator, generation, event]() {
            coordinator->applyParticipantEventOnUiThread(generation, event);
        }, Qt::QueuedConnection);
    }

    void OnConnected() override {
        auto *coordinator = _coordinator;
        if (!coordinator || _generation == 0) return;
        const auto generation = _generation;
        QMetaObject::invokeMethod(coordinator, [coordinator, generation]() {
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
        }, Qt::QueuedConnection);
    }

    void OnDisconnected(livekit::RoomDisconnectReason reason,
                        const std::string &detail) override {
        auto *coordinator = _coordinator;
        if (!coordinator || _generation == 0) return;
        const auto generation = _generation;
        const QString qDetail = detail.empty()
            ? QString::fromUtf8("无附加断开说明")
            : QString::fromStdString(livekit::secure_log::OpaqueSummary("room_disconnect"));
        // 不捕获 listener 自身：DuplicateIdentity 处理会释放 _roomListener，
        // 捕获 this 会在回调执行期间留下悬垂指针风险。
        QMetaObject::invokeMethod(coordinator, [coordinator, generation, reason, qDetail]() {
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
                QString("与 LiveKit 房间连接断开: reason=%1, detail=%2")
                    .arg(QString::fromLatin1(livekit::ToString(reason)), qDetail));
            if (!current()) return;
            if (reason == livekit::RoomDisconnectReason::DuplicateIdentity) {
                owner->handleDuplicateIdentityKickOff(QString());
                return;
            }
            if (owner->_state != MeetingState::Leaving && owner->_state != MeetingState::Idle) {
                owner->setState(MeetingState::Idle, qDetail);
                if (!current() || owner->_state != MeetingState::Idle) return;
                emit owner->meetingLeft();
            }
        }, Qt::QueuedConnection);
    }

    void OnReconnecting() override {
        auto *coordinator = _coordinator;
        if (!coordinator || _generation == 0) return;
        const auto generation = _generation;
        QMetaObject::invokeMethod(coordinator, [coordinator, generation]() {
            QPointer<MeetingCoordinator> owner(coordinator);
            if (!owner->isCurrentSessionGenerationOnUiThread(generation) ||
                owner->_state == MeetingState::Leaving ||
                owner->_state == MeetingState::Idle) {
                return;
            }
            const auto nativeGeneration = owner->_nativeRoomGeneration;
            MeetingUI::LogToConsole(MeetingUI::LogCategory::Connection, "RECONNECTING",
                                    QString::fromUtf8("网络中断，正在恢复音视频连接"));
            if (!owner || !owner->isCurrentSessionGenerationOnUiThread(generation) ||
                owner->_nativeRoomGeneration != nativeGeneration ||
                owner->_state == MeetingState::Leaving || owner->_state == MeetingState::Idle) return;
            owner->setState(MeetingState::Reconnecting,
                                  QString::fromUtf8("网络中断，正在恢复连接..."));
        }, Qt::QueuedConnection);
    }

    void OnReconnected() override {
        auto *coordinator = _coordinator;
        if (!coordinator || _generation == 0) return;
        const auto generation = _generation;
        QMetaObject::invokeMethod(coordinator, [coordinator, generation]() {
            QPointer<MeetingCoordinator> owner(coordinator);
            if (!owner->isCurrentSessionGenerationOnUiThread(generation) ||
                owner->_state == MeetingState::Leaving ||
                owner->_state == MeetingState::Idle) {
                return;
            }
            const auto nativeGeneration = owner->_nativeRoomGeneration;

            if (!owner->_startupCommitted) {
                MeetingUI::LogToConsole(
                    MeetingUI::LogCategory::Connection,
                    "RECONNECTED_DURING_STARTUP",
                    QString::fromUtf8("连接已恢复，等待本地音视频启动事务提交"));
                return;
            }

            owner->setState(MeetingState::InMeeting,
                                  QString::fromUtf8("音视频连接已恢复"));
            if (!owner || !owner->isCurrentSessionGenerationOnUiThread(generation) ||
                owner->_nativeRoomGeneration != nativeGeneration ||
                owner->_state != MeetingState::InMeeting) return;
            MeetingUI::LogToConsole(MeetingUI::LogCategory::Connection, "RECONNECTED",
                                    QString::fromUtf8("音视频连接已恢复，等待值投影恢复远端轨道"));
        }, Qt::QueuedConnection);
    }

    void OnRoomMetadataChanged(const livekit::RoomInfo &room,
                               const std::string &oldMetadata,
                               const std::string &newMetadata) override {
        auto *coordinator = _coordinator;
        if (!coordinator || _generation == 0) return;
        const auto generation = _generation;
        const QString metadata = QString::fromStdString(newMetadata);
        QMetaObject::invokeMethod(coordinator, [coordinator, generation, metadata]() {
            if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
            coordinator->parseRoomMetadata(metadata.toStdString());
        }, Qt::QueuedConnection);
    }

    void OnRoomUpdated(const livekit::RoomInfo &room) override {
        auto *coordinator = _coordinator;
        if (!coordinator || _generation == 0) return;
        const auto generation = _generation;
        const MeetingRoomInfo info = ToMeetingRoomInfo(room);
        QMetaObject::invokeMethod(coordinator, [coordinator, generation, info]() {
            if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
            coordinator->_roomInfo = info;
            emit coordinator->roomInfoUpdated(info);
        }, Qt::QueuedConnection);
    }

    void OnConnectionQualityChanged(std::shared_ptr<livekit::Participant> participant,
                                    livekit::ConnectionQuality quality,
                                    float score) override {
        auto *coordinator = _coordinator;
        if (!participant || !coordinator || _generation == 0) return;
        const auto generation = _generation;
        const QString identity = QString::fromStdString(participant->identity());
        QMetaObject::invokeMethod(coordinator, [coordinator, generation, identity, quality, score]() {
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
        }, Qt::QueuedConnection);
    }

    void OnTrackStreamStateChanged(
        std::shared_ptr<livekit::Participant> participant,
        std::shared_ptr<livekit::TrackPublication> publication,
        livekit::TrackPublication::StreamState state) override {
        auto *coordinator = _coordinator;
        if (!participant || !publication || !publication->track() ||
            !coordinator || _generation == 0) {
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
            if (candidate->track()->kind() == livekit::TrackKind::Audio) {
                audioPaused = true;
            } else if (candidate->track()->kind() == livekit::TrackKind::Video) {
                videoPaused = true;
            }
        }

        QMetaObject::invokeMethod(coordinator,
                                  [coordinator, generation, identity, trackSid, isVideo, paused,
                                   audioPaused, videoPaused]() {
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
        }, Qt::QueuedConnection);
    }

    void OnParticipantPermissionsChanged(
        const livekit::ParticipantPermission &oldPermission,
        const livekit::ParticipantPermission &newPermission,
        std::shared_ptr<livekit::Participant> participant) override {
        auto *coordinator = _coordinator;
        if (!participant || !coordinator || _generation == 0) return;
        const auto generation = _generation;
        const QString identity = QString::fromStdString(participant->identity());
        QMetaObject::invokeMethod(coordinator, [coordinator, generation, identity, newPermission]() {
            if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
            const auto it = coordinator->_participants.find(identity);
            if (it == coordinator->_participants.end()) return;
            it->second.permissions = newPermission;
            coordinator->updateParticipantListAndNotify();
            emit coordinator->participantPermissionsChanged(identity, newPermission);
        }, Qt::QueuedConnection);
    }

    void OnTrackSubscriptionPermissionChanged(
        const livekit::TrackSubscriptionPermission &permission,
        std::shared_ptr<livekit::Participant> participant,
        std::shared_ptr<livekit::TrackPublication> publication) override {
        auto *coordinator = _coordinator;
        if (!coordinator || _generation == 0) return;
        const auto generation = _generation;
        const QString identity = participant
            ? QString::fromStdString(participant->identity())
            : QString();
        const QString participantSid = QString::fromStdString(permission.participant_sid);
        const QString trackSid = QString::fromStdString(permission.track_sid);
        const bool allowed = permission.allowed;
        QMetaObject::invokeMethod(coordinator,
                                  [coordinator, generation, identity, participantSid, trackSid, allowed]() {
            if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
            emit coordinator->trackSubscriptionPermissionChanged(
                identity, participantSid, trackSid, allowed);
        }, Qt::QueuedConnection);
    }

    void OnParticipantConnected(std::shared_ptr<livekit::RemoteParticipant> p) override {
        auto *coordinator = _coordinator;
        if (!p || !coordinator || _generation == 0) return;
        const auto generation = _generation;
        QString id = QString::fromStdString(p->identity());
        QString realNick = ResolveParticipantNickname(p);
        QMetaObject::invokeMethod(coordinator, [coordinator, generation, id, realNick, p]() {
            if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
            MeetingUI::LogToConsole(MeetingUI::LogCategory::Participant, "REMOTE_JOIN", QString("参会人加入: %1 (昵称: %2)").arg(id).arg(realNick));
            ParticipantInfo info;
            info.identity = id;
            info.name = realNick;
            info.isLocal = false;
            info.isHost = (id == coordinator->_meetingDetail.hostUserId || id == coordinator->_meetingDetail.creatorUserId);
            info.isAudioMuted = true;
            info.isVideoEnabled = false;
            CopyParticipantState(p, &info);
            coordinator->_participants[id] = info;
            coordinator->updateParticipantListAndNotify();
            emit coordinator->participantJoined(id, realNick);
        }, Qt::QueuedConnection);
    }

    void OnParticipantMetadataChanged(std::shared_ptr<livekit::Participant> p,
                                      const std::string &old_metadata,
                                      const std::string &new_metadata) override {
        auto *coordinator = _coordinator;
        if (!p || !coordinator || _generation == 0) return;
        const auto generation = _generation;
        QString id = QString::fromStdString(p->identity());
        QString realNick = ResolveParticipantNickname(p);
        QMetaObject::invokeMethod(coordinator, [coordinator, generation, id, realNick]() {
            if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
            MeetingUI::LogToConsole(MeetingUI::LogCategory::Participant, "METADATA_CHANGED",
                                    QString("参会人 [%1] 元数据更新，刷新昵称为: %2").arg(id).arg(realNick));
            auto it = coordinator->_participants.find(id);
            if (it != coordinator->_participants.end()) {
                if (!it->second.isLocal) {
                    it->second.name = realNick;
                }
                coordinator->updateParticipantListAndNotify();
                emit coordinator->participantJoined(id, realNick);
            }
        }, Qt::QueuedConnection);
    }

    void OnParticipantDisconnected(std::shared_ptr<livekit::RemoteParticipant> p) override {
        auto *coordinator = _coordinator;
        if (!p || !coordinator || _generation == 0) return;
        const auto generation = _generation;
        QString id = QString::fromStdString(p->identity());
        QMetaObject::invokeMethod(coordinator, [coordinator, generation, id]() {
            if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
            MeetingUI::LogToConsole(MeetingUI::LogCategory::Participant, "REMOTE_LEFT", QString("参会人离开: %1").arg(id));
            coordinator->_participants.erase(id);
            coordinator->updateParticipantListAndNotify();
            emit coordinator->participantLeft(id);

        }, Qt::QueuedConnection);
    }

    void OnTrackSubscribed(std::shared_ptr<livekit::Track> track,
                           std::shared_ptr<livekit::TrackPublication> pub,
                           std::shared_ptr<livekit::RemoteParticipant> p) override {
        auto *coordinator = _coordinator;
        if (!track || !p || !coordinator || _generation == 0) return;
        const auto generation = _generation;
        std::string identity = p->identity();

        if (track->kind() == livekit::TrackKind::Video) {
            QMetaObject::invokeMethod(coordinator, [coordinator, generation, track, identity]() {
                if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
                emit coordinator->remoteVideoTrackAvailable(QString::fromStdString(identity), track);
            }, Qt::QueuedConnection);
        }
    }

    void OnTrackUnsubscribed(std::shared_ptr<livekit::Track> track,
                             std::shared_ptr<livekit::TrackPublication> pub,
                             std::shared_ptr<livekit::RemoteParticipant> p) override {
        auto *coordinator = _coordinator;
        if (!track || !p || !coordinator || _generation == 0) return;
        const auto generation = _generation;
        QString id = QString::fromStdString(p->identity());
        bool isVideo = (track->kind() == livekit::TrackKind::Video);
        const std::string track_sid = track->sid();
        QMetaObject::invokeMethod(coordinator, [coordinator, generation, id, isVideo, track_sid]() {
            if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
            if (isVideo) {
                emit coordinator->remoteVideoTrackUnavailable(id, QString::fromStdString(track_sid));
            }
            auto it = coordinator->_participants.find(id);
            if (it != coordinator->_participants.end()) {
                if (isVideo) {
                    it->second.isVideoEnabled = false;
                } else {
                    it->second.isAudioMuted = true;
                }
                coordinator->updateParticipantListAndNotify();
            }
            emit coordinator->remoteTrackMuted(id, isVideo, true);
        }, Qt::QueuedConnection);
    }

    void OnTrackMuted(std::shared_ptr<livekit::Participant> participant,
                      std::shared_ptr<livekit::TrackPublication> publication,
                      bool muted) override {
        auto *coordinator = _coordinator;
        if (!participant || !publication || !publication->track() || !coordinator || _generation == 0) return;
        const auto generation = _generation;
        QString id = QString::fromStdString(participant->identity());
        bool isVideo = (publication->track()->kind() == livekit::TrackKind::Video);
        QMetaObject::invokeMethod(coordinator, [coordinator, generation, id, isVideo, muted]() {
            if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
            auto it = coordinator->_participants.find(id);
            if (it != coordinator->_participants.end()) {
                if (isVideo) {
                    it->second.isVideoEnabled = !muted;
                } else {
                    it->second.isAudioMuted = muted;
                }
                coordinator->updateParticipantListAndNotify();
            }
            emit coordinator->remoteTrackMuted(id, isVideo, muted);
        }, Qt::QueuedConnection);
    }

    void OnActiveSpeakersChanged(const std::vector<std::shared_ptr<livekit::Participant>> &speakers) override {
        (void)speakers;
    }

    void OnDataReceived(const std::vector<uint8_t> &payload,
                        std::shared_ptr<livekit::RemoteParticipant> participant,
                        const std::string &topic) override {
        auto *coordinator = _coordinator;
        auto session = _session.lock();
        if (!coordinator || !session) return;
        (void)payload;
        (void)participant;
    }

    void OnTextStreamOpened(std::shared_ptr<livekit::TextStreamReader> reader,
                            std::shared_ptr<livekit::Participant> participant) override {
        auto *coordinator = _coordinator;
        auto session = _session.lock();
        if (!coordinator || !session || !reader) return;
        const auto generation = _generation;
        QString pId = participant ? QString::fromStdString(participant->identity()) : QString();
        QMetaObject::invokeMethod(coordinator, [coordinator, generation, reader, pId]() {
            if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
            emit coordinator->textStreamReceived(reader, pId);
        }, Qt::QueuedConnection);
    }

    void OnByteStreamOpened(std::shared_ptr<livekit::ByteStreamReader> reader,
                            std::shared_ptr<livekit::Participant> participant) override {
        auto *coordinator = _coordinator;
        auto session = _session.lock();
        if (!coordinator || !session || !reader) return;
        const auto generation = _generation;
        QString pId = participant ? QString::fromStdString(participant->identity()) : QString();
        QMetaObject::invokeMethod(coordinator, [coordinator, generation, reader, pId]() {
            if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
            emit coordinator->byteStreamReceived(reader, pId);
        }, Qt::QueuedConnection);
    }

private:
    MeetingCoordinator *_coordinator;
    std::weak_ptr<MeetingSessionRuntime> _session;
    const uint64_t _generation;
};

std::shared_ptr<livekit::RoomListener> MeetingCoordinator::participantEventListenerForTesting(
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
        event.kind == livekit::ParticipantEventKind::TrackUnavailable;
    const bool requiresParticipantTicket =
        event.kind != livekit::ParticipantEventKind::ActiveSpeakers &&
        event.kind != livekit::ParticipantEventKind::DataReceived &&
        !participantCleanup;

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
                if (!info.isLocal) oldRemoteIdentities.push_back(identity);
                cancelInboundTransfersForParticipant(info.participantKey);
            }
            _nativeRoomGeneration = eventRoomGeneration;
            _participants.clear();
            _remoteVideoTracks.clear();
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
        _participantEventSequences.erase(sequenceKey);
        std::vector<QString> trackSids;
        const auto tracks = _remoteVideoTracks.find(identity);
        if (tracks != _remoteVideoTracks.end()) {
            for (const auto &[sid, value] : tracks->second) {
                if (value.key.participant == key) trackSids.push_back(sid);
            }
        }
        if (participantStillCurrent()) {
            _participants.erase(identity);
            _remoteVideoTracks.erase(identity);
            updateParticipantListAndNotify();
            if (!sessionStillCurrent()) return;
            for (const auto &trackSid : trackSids) {
                if (owner->_participants.find(identity) != owner->_participants.end()) return;
                emit remoteVideoTrackUnavailable(identity, trackSid);
                if (!sessionStillCurrent()) return;
            }
            if (owner->_participants.find(identity) != owner->_participants.end()) return;
            emit participantLeft(identity);
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
        if (!serverOrigin &&
            !livekit::IsParticipantTicketActive(
                event.sender.ticket, event.sender.key)) {
            return;
        }
        auto session = _sessionRuntime;
        if (session) enqueueDataReceived(session, event.data, event.sender);
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
        auto participantTracks = _remoteVideoTracks.find(identity);
        if (participantTracks == _remoteVideoTracks.end()) return;
        const auto track = participantTracks->second.find(trackSid);
        if (track == participantTracks->second.end() ||
            track->second.key != event.track_key) {
            return;
        }
        participantTracks->second.erase(track);
        if (participantTracks->second.empty()) {
            _remoteVideoTracks.erase(participantTracks);
        }
        emit remoteVideoTrackUnavailable(identity, trackSid);
        return;
    }

    if (!livekit::IsParticipantTicketActive(
            event.participant.ticket, key)) {
        return;
    }

    if (event.kind == livekit::ParticipantEventKind::Upsert ||
        event.kind == livekit::ParticipantEventKind::ConnectionQuality ||
        event.kind == livekit::ParticipantEventKind::TrackMuted ||
        event.kind == livekit::ParticipantEventKind::TrackStreamState ||
        event.kind == livekit::ParticipantEventKind::TrackSubscriptionPermission) {
        const auto existing = _participants.find(identity);
        const bool replacing = existing != _participants.end() &&
            existing->second.participantKey != key;
        const bool isNew = existing == _participants.end() || replacing;
        if (replacing) {
            const auto oldKey = existing->second.participantKey;
            _participants.erase(existing);
            _remoteVideoTracks.erase(identity);
            cancelInboundTransfersForParticipant(oldKey);
            updateParticipantListAndNotify();
            if (!valueStillCurrent() || owner->_participants.find(identity) != owner->_participants.end()) return;
            emit participantLeft(identity);
            if (!valueStillCurrent() || owner->_participants.find(identity) != owner->_participants.end()) return;
        }

        ParticipantInfo info;
        if (const auto current = _participants.find(identity);
            current != _participants.end()) {
            info = current->second;
        }
        info.identity = identity;
        info.name = ResolveParticipantNickname(event.participant.state);
        info.isLocal = event.participant.is_local;
        info.isHost = identity == _meetingDetail.hostUserId ||
            identity == _meetingDetail.creatorUserId;
        info.isAudioMuted = info.isLocal ? _audioMuted : true;
        info.isVideoEnabled = info.isLocal ? _videoEnabled : false;
        info.isSpeaking = event.participant.state.speaking;
        info.audioLevel = event.participant.state.audio_level;
        info.participantKey = key;
        info.participantTicket = event.participant.ticket;
        info.lastEventSequence = event.event_sequence;
        CopyParticipantState(event.participant.state, &info);
        for (const auto &publication : event.participant.state.publications) {
            if (!publication.track) continue;
            if (publication.kind == livekit::TrackKind::Audio) {
                info.isAudioMuted = publication.muted;
            } else if (publication.kind == livekit::TrackKind::Video) {
                info.isVideoEnabled = !publication.muted;
            }
        }
        _participants[identity] = info;
        updateParticipantListAndNotify();
        if (!valueStillCurrent() || !participantStillCurrent()) return;

        if (isNew && !info.isLocal) {
            emit participantJoined(identity, info.name);
            if (!valueStillCurrent() || !participantStillCurrent()) return;
        }

        if (event.kind == livekit::ParticipantEventKind::ConnectionQuality) {
            emit participantConnectionQualityChanged(
                identity,
                static_cast<int>(info.connectionQuality),
                info.connectionQualityScore);
        } else if (event.kind == livekit::ParticipantEventKind::TrackMuted) {
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
            event.publication.kind != livekit::TrackKind::Video ||
            !event.publication.track) {
            return;
        }
        const QString trackSid = QString::fromStdString(
            event.track_key.publication_sid);
        const auto participantTracks = _remoteVideoTracks.find(identity);
        if (participantTracks != _remoteVideoTracks.end()) {
            const auto existing = participantTracks->second.find(trackSid);
            if (existing != participantTracks->second.end() &&
                existing->second.key == event.track_key &&
                existing->second.track == event.publication.track) {
                return;
            }
        }
        _remoteVideoTracks[identity][trackSid] = {
            event.track_key, event.track_ticket, event.publication.track};
        emit remoteVideoTrackAvailable(identity, event.publication.track);
    }
}

// ----------------------------------------------------
// MeetingCoordinator 实现
// ----------------------------------------------------

std::shared_ptr<MeetingCoordinator> MeetingCoordinator::create(QObject *parent) {
    return std::make_shared<MeetingCoordinator>(parent);
}

MeetingCoordinator::MeetingCoordinator(QObject *parent)
    : MeetingCoordinator(SessionManager::instance(),
                         makeDefaultAdmissionBackend(SessionManager::instance()),
                         parent) {
}

MeetingCoordinator::AdmissionBackend MeetingCoordinator::makeDefaultAdmissionBackend(
    SessionManager &sessionManager) {
    AdmissionBackend backend;
    backend.joinMeeting = [&sessionManager](const QString &meetingId,
                                            const QString &password,
                                            ResultCallback<bool> callback) {
        sessionManager.httpClient().joinMeeting(meetingId, password, std::move(callback));
    };
    backend.getMeetingToken = [&sessionManager](const QString &meetingId,
                                                ResultCallback<LiveKitAuthInfo> callback) {
        sessionManager.httpClient().getMeetingToken(meetingId, std::move(callback));
    };
    backend.createImmediateMeeting = [&sessionManager](const QString &title,
                                                       int durationSeconds,
                                                       ResultCallback<LiveKitAuthInfo> callback) {
        sessionManager.httpClient().createImmediateMeeting(title, durationSeconds, std::move(callback));
    };
    backend.leaveMeeting = [&sessionManager](const QString &meetingId,
                                             ResultCallback<bool> callback) {
        sessionManager.httpClient().leaveMeeting(meetingId, std::move(callback));
    };
    backend.endMeeting = [&sessionManager](const QString &meetingId,
                                           ResultCallback<bool> callback) {
        sessionManager.httpClient().endMeeting(meetingId, std::move(callback));
    };
    return backend;
}

MeetingCoordinator::MeetingCoordinator(SessionManager &sessionManager,
                                       AdmissionBackend admissionBackend,
                                       QObject *parent)
    : QObject(parent)
    , _sessionManager(sessionManager)
    , _admissionBackend(std::move(admissionBackend)) {
    qRegisterMetaType<livekit::RoomDisconnectReason>("livekit::RoomDisconnectReason");
    qRegisterMetaType<MeetingRoomInfo>("OpenMeeting::MeetingRoomInfo");
    qRegisterMetaType<livekit::ParticipantPermission>("livekit::ParticipantPermission");
    qRegisterMetaType<std::shared_ptr<livekit::TextStreamReader>>("std::shared_ptr<livekit::TextStreamReader>");
    qRegisterMetaType<std::shared_ptr<livekit::ByteStreamReader>>("std::shared_ptr<livekit::ByteStreamReader>");
    _localAudioSource = std::make_shared<livekit::AudioSource>(48000, 2);
    _localVideoSource = std::make_shared<livekit::VideoSource>(1280, 720);

    _mediaSendTimer = new QTimer(this);
    connect(_mediaSendTimer, &QTimer::timeout, this, &MeetingCoordinator::processNextMediaSendChunk);

    // 全局账号状态由 SessionManager 统一裁决。这里不发 meetingLeft，避免
    // MeetingRoomWindow 按普通离会逻辑继续调用业务 HTTP 接口。
    connect(&_sessionManager, &SessionManager::sessionInvalidated,
            this, [this](SessionInvalidationReason reason) {
                handleSessionInvalidated(reason);
            }, Qt::QueuedConnection);
}

MeetingCoordinator::~MeetingCoordinator() {
    invalidateAdmission();
    stopRoomSession();
}

QString MeetingCoordinator::stateString() const {
    switch (_state) {
        case MeetingState::Idle: return QString::fromUtf8("未就绪/闲置");
        case MeetingState::Validating: return QString::fromUtf8("准入校验中");
        case MeetingState::FetchingCredentials: return QString::fromUtf8("凭据获取中");
        case MeetingState::ConnectingRoom: return QString::fromUtf8("正在连接房间");
        case MeetingState::StartingLocalMedia: return QString::fromUtf8("正在启动本地音视频");
        case MeetingState::InMeeting: return QString::fromUtf8("会议进行中");
        case MeetingState::Reconnecting: return QString::fromUtf8("断线重连中");
        case MeetingState::Leaving: return QString::fromUtf8("退出清理中");
        case MeetingState::Failed: return QString::fromUtf8("操作失败");
    }
    return QString::fromUtf8("未知状态");
}

bool MeetingCoordinator::isHost() const {
    QString myUserId = _sessionManager.userId();
    if (myUserId.isEmpty()) return false;
    return (myUserId == _meetingDetail.hostUserId || myUserId == _meetingDetail.creatorUserId);
}

uint64_t MeetingCoordinator::beginAdmission(AdmissionStage stage) {
    ++_admissionGeneration;
    _admissionStage = stage;
    return _admissionGeneration;
}

uint64_t MeetingCoordinator::invalidateAdmission() {
    ++_admissionGeneration;
    _admissionStage = AdmissionStage::None;
    return _admissionGeneration;
}

bool MeetingCoordinator::canBeginAdmission() const {
    return _admissionStage == AdmissionStage::None ||
           _admissionStage == AdmissionStage::Consumed;
}

bool MeetingCoordinator::isAdmissionCurrent(uint64_t generation,
                                            AdmissionStage stage) const {
    return !_sessionInvalidated && !_sessionManager.isSessionInvalidating() &&
           _admissionGeneration == generation &&
           _admissionStage == stage;
}

void MeetingCoordinator::setState(MeetingState s, const QString &detail) {
    if (_state != s) {
        _state = s;
        emit stateChanged(_state, detail);
    }
}

void MeetingCoordinator::joinMeetingAsync(const QString &meetingId,
                                         const QString &password,
                                         const QString &displayName,
                                         const MediaPreferences &prefs) {
    if (_sessionInvalidated || _sessionManager.isSessionInvalidating()) {
        qInfo() << "[Coordinator] Ignore join request after session invalidation.";
        return;
    }
    if ((_state != MeetingState::Idle && _state != MeetingState::Failed) ||
        !canBeginAdmission()) {
        emit errorOccurred(QString::fromUtf8("入会错误"), QString::fromUtf8("当前已有正在执行的会议流程，请勿重复加入"));
        return;
    }

    const QString requestedMeetingId = meetingId;
    const QString requestedPassword = password;
    const QString requestedDisplayName = displayName;
    const uint64_t generation = beginAdmission(AdmissionStage::Joining);
    QPointer<MeetingCoordinator> owner(this);

    _currentMeetingId = requestedMeetingId;
    _currentPassword = requestedPassword;
    _currentDisplayName = requestedDisplayName.isEmpty() ? _sessionManager.nickname() : requestedDisplayName;
    _mediaPrefs = prefs;
    _audioMuted = !prefs.enableMicrophone;
    _videoEnabled = prefs.enableVideo;

    _participants.clear();
    ensureLocalParticipant();
    updateParticipantListAndNotify();
    if (!owner || !owner->isAdmissionCurrent(generation, AdmissionStage::Joining)) {
        return;
    }

    owner->setState(MeetingState::Validating, QString::fromUtf8("正在校验会议准入资格..."));
    if (!owner || !owner->isAdmissionCurrent(generation, AdmissionStage::Joining)) {
        return;
    }

    // 第一阶段：向后端鉴权校验密码与会议有效性
    auto joinRequest = owner->_admissionBackend.joinMeeting;
    joinRequest(requestedMeetingId, requestedPassword,
                [owner, generation, requestedMeetingId](bool ok, bool, const HttpError &err) {
        if (!owner || !owner->isAdmissionCurrent(generation, AdmissionStage::Joining)) {
            return;
        }
        if (!ok) {
            const QString detail = err.message;
            const QString message = detail.isEmpty()
                ? QString::fromUtf8("会议不存在或入会密码错误") : detail;
            owner->_admissionStage = AdmissionStage::None;
            owner->setState(MeetingState::Failed, detail);
            if (!owner || owner->_admissionGeneration != generation ||
                owner->_admissionStage != AdmissionStage::None) {
                return;
            }
            emit owner->errorOccurred(QString::fromUtf8("入会鉴权失败"), message);
            return;
        }

        // 第二阶段：换取 LiveKit 令牌与网关 URL
        owner->_admissionStage = AdmissionStage::FetchingToken;
        owner->setState(MeetingState::FetchingCredentials,
                        QString::fromUtf8("正在换取音视频会话令牌..."));
        if (!owner || !owner->isAdmissionCurrent(generation, AdmissionStage::FetchingToken)) {
            return;
        }
        auto tokenRequest = owner->_admissionBackend.getMeetingToken;
        tokenRequest(requestedMeetingId, [owner, generation](bool tokenOk,
                                                            const LiveKitAuthInfo &auth,
                                                            const HttpError &tokenErr) {
            if (!owner || !owner->isAdmissionCurrent(generation, AdmissionStage::FetchingToken)) {
                return;
            }
            if (!tokenOk || auth.url.isEmpty() || auth.token.isEmpty()) {
                const QString detail = tokenErr.message;
                const QString message = detail.isEmpty()
                    ? QString::fromUtf8("无法换取 LiveKit 房间访问凭证") : detail;
                owner->_admissionStage = AdmissionStage::None;
                owner->setState(MeetingState::Failed, detail);
                if (!owner || owner->_admissionGeneration != generation ||
                    owner->_admissionStage != AdmissionStage::None) {
                    return;
                }
                emit owner->errorOccurred(QString::fromUtf8("获取凭据失败"), message);
                return;
            }

            // 第三阶段：启动 LiveKit 房间连接与媒体发布
            const QString url = auth.url;
            const QString token = auth.token;
            owner->_admissionStage = AdmissionStage::ReadyToStart;
            owner->startRoomSession(url, token, generation);
        });
    });
}

void MeetingCoordinator::createAndJoinQuickMeetingAsync(const QString &title,
                                                       int durationSeconds,
                                                       const MediaPreferences &prefs) {
    if (_sessionInvalidated || _sessionManager.isSessionInvalidating()) {
        qInfo() << "[Coordinator] Ignore quick-meeting request after session invalidation.";
        return;
    }
    if ((_state != MeetingState::Idle && _state != MeetingState::Failed) ||
        !canBeginAdmission()) {
        emit errorOccurred(QString::fromUtf8("创建错误"), QString::fromUtf8("当前已有活跃会议流程"));
        return;
    }

    const QString requestedTitle = title;
    const int requestedDurationSeconds = durationSeconds;
    const uint64_t generation = beginAdmission(AdmissionStage::Creating);
    QPointer<MeetingCoordinator> owner(this);

    _currentDisplayName = _sessionManager.nickname();
    _mediaPrefs = prefs;
    _audioMuted = !prefs.enableMicrophone;
    _videoEnabled = prefs.enableVideo;

    _participants.clear();
    ensureLocalParticipant();
    updateParticipantListAndNotify();
    if (!owner || !owner->isAdmissionCurrent(generation, AdmissionStage::Creating)) {
        return;
    }

    owner->setState(MeetingState::Validating, QString::fromUtf8("正在创建即时会议..."));
    if (!owner || !owner->isAdmissionCurrent(generation, AdmissionStage::Creating)) {
        return;
    }

    auto createRequest = owner->_admissionBackend.createImmediateMeeting;
    createRequest(requestedTitle, requestedDurationSeconds,
                  [owner, generation, requestedTitle](bool ok,
                                                      const LiveKitAuthInfo &auth,
                                                      const HttpError &err) {
        if (!owner || !owner->isAdmissionCurrent(generation, AdmissionStage::Creating)) {
            return;
        }
        if (!ok || auth.url.isEmpty() || auth.token.isEmpty()) {
            const QString detail = err.message;
            const QString message = detail.isEmpty()
                ? QString::fromUtf8("服务端未能分配会议房间") : detail;
            owner->_admissionStage = AdmissionStage::None;
            owner->setState(MeetingState::Failed, detail);
            if (!owner || owner->_admissionGeneration != generation ||
                owner->_admissionStage != AdmissionStage::None) {
                return;
            }
            emit owner->errorOccurred(QString::fromUtf8("创建即时会议失败"), message);
            return;
        }

        const QString meetingId = auth.meetingId;
        const QString url = auth.url;
        const QString token = auth.token;
        const QString userId = owner->_sessionManager.userId();
        owner->_admissionStage = AdmissionStage::ReadyToStart;
        owner->_currentMeetingId = meetingId;
        owner->_meetingDetail.meetingId = meetingId;
        owner->_meetingDetail.meetingName = requestedTitle;
        owner->_meetingDetail.hostUserId = userId;
        owner->_meetingDetail.creatorUserId = userId;
        const MeetingDetail detailSnapshot = owner->_meetingDetail;
        emit owner->meetingDetailUpdated(detailSnapshot);
        if (!owner || !owner->isAdmissionCurrent(generation, AdmissionStage::ReadyToStart)) {
            return;
        }

        MeetingUI::LogToConsole(MeetingUI::LogCategory::General, "MEETING_ID",
            QString("即时会议创建成功！会议号: %1 (其他参会人可凭此 9 位会议号加入)").arg(meetingId));

        owner->startRoomSession(url, token, generation);
    });
}

void MeetingCoordinator::connectDirectlyAsync(const QString &url,
                                             const QString &token,
                                             const QString &meetingId,
                                             const QString &displayName,
                                             const MediaPreferences &prefs) {
    invalidateAdmission();
    if (_sessionInvalidated || _sessionManager.isSessionInvalidating()) {
        qInfo() << "[Coordinator] Ignore direct-connect request after session invalidation.";
        return;
    }
    const QString requestedUrl = url;
    const QString requestedToken = token;
    const uint64_t generation = beginAdmission(AdmissionStage::ReadyToStart);
    QPointer<MeetingCoordinator> owner(this);
    _currentMeetingId = meetingId;
    _currentDisplayName = displayName;
    _mediaPrefs = prefs;
    _audioMuted = !prefs.enableMicrophone;
    _videoEnabled = prefs.enableVideo;

    _participants.clear();
    ensureLocalParticipant();
    updateParticipantListAndNotify();
    if (!owner || !owner->isAdmissionCurrent(generation, AdmissionStage::ReadyToStart)) {
        return;
    }

    owner->startRoomSession(requestedUrl, requestedToken, generation);
}

void MeetingCoordinator::leaveMeetingAsync(bool endMeetingForAll) {
    invalidateAdmission();
    if (_state == MeetingState::Idle || _state == MeetingState::Leaving) {
        return;
    }

    const QString meetingId = _currentMeetingId;
    const bool notifyBackend = !meetingId.isEmpty() && _sessionManager.isLoggedIn();
    const bool shouldEndMeeting = endMeetingForAll && isHost();
    auto backendRequest = shouldEndMeeting
        ? _admissionBackend.endMeeting : _admissionBackend.leaveMeeting;
    QPointer<MeetingCoordinator> owner(this);

    setState(MeetingState::Leaving, QString::fromUtf8("正在安全退出会议..."));
    if (!owner || owner->_state != MeetingState::Leaving ||
        owner->_admissionStage != AdmissionStage::None || owner->_sessionInvalidated) {
        return;
    }
    if (notifyBackend) {
        backendRequest(meetingId, [](bool, bool, const HttpError &) {});
        if (!owner || owner->_state != MeetingState::Leaving ||
            owner->_admissionStage != AdmissionStage::None || owner->_sessionInvalidated) {
            return;
        }
    }

    owner->stopRoomSession();
    if (!owner || owner->_state != MeetingState::Leaving ||
        owner->_admissionStage != AdmissionStage::None || owner->_sessionInvalidated) {
        return;
    }

    owner->setState(MeetingState::Idle, QString::fromUtf8("已退出会议"));
    if (!owner || owner->_state != MeetingState::Idle ||
        owner->_admissionStage != AdmissionStage::None || owner->_sessionInvalidated) {
        return;
    }
    emit owner->meetingLeft();
}

void MeetingCoordinator::handleDuplicateIdentityKickOff(const QString &detail) {
    const uint64_t generation = invalidateAdmission();
    if (_state == MeetingState::Leaving || _state == MeetingState::Idle) {
        return;
    }
    QPointer<MeetingCoordinator> owner(this);
    const auto sessionGeneration = _nextSessionGeneration;

    const QString message = detail.isEmpty()
        ? QString::fromUtf8("同一账号已在其他设备加入此会议")
        : QString::fromStdString(
            livekit::secure_log::SanitizeForOutput(detail.toStdString()));
    MeetingUI::LogToConsole(MeetingUI::LogCategory::Connection,
                            "DUPLICATE_IDENTITY",
                            QString("[Coordinator] Meeting kicked off by server: %1").arg(message));
    // Logging can replace the session or destroy its QObject. Admission
    // invalidation alone must not suppress cleanup of the original resources.
    if (!owner || owner->_nextSessionGeneration != sessionGeneration) return;

    // Room 已由服务端 LEAVE 流程断开；这里负责停止 Coordinator 所属的
    // io 线程和媒体资源。不要发出 meetingLeft，否则 UI 会在提示前关闭。
    owner->stopRoomSession();
    if (!owner || owner->_admissionGeneration != generation ||
        owner->_admissionStage != AdmissionStage::None) {
        return;
    }
    owner->setState(MeetingState::Idle, message);
    if (!owner || owner->_admissionGeneration != generation ||
        owner->_admissionStage != AdmissionStage::None || owner->_state != MeetingState::Idle) {
        return;
    }
    emit owner->meetingKickOff(livekit::RoomDisconnectReason::DuplicateIdentity);
}

void MeetingCoordinator::handleSessionInvalidated(SessionInvalidationReason reason) {
    invalidateAdmission();
    if (_sessionInvalidated) {
        return;
    }
    _sessionInvalidated = true;

    MeetingUI::LogToConsole(
        MeetingUI::LogCategory::Connection,
        "SESSION_INVALIDATED",
        QString("[Coordinator] Stop room for invalidated account session, reason=%1")
            .arg(static_cast<int>(reason)));

    if (_state == MeetingState::Idle) {
        return;
    }

    QPointer<MeetingCoordinator> owner(this);
    if (_state != MeetingState::Leaving) {
        setState(MeetingState::Leaving, QString::fromUtf8("账号登录状态已失效，正在停止会议..."));
    }
    if (!owner) {
        return;
    }
    owner->stopRoomSession();
    if (!owner) {
        return;
    }
    owner->setState(MeetingState::Idle, QString::fromUtf8("账号登录状态已失效"));
}

void MeetingCoordinator::startRoomSession(const QString &url,
                                          const QString &token,
                                          uint64_t admissionGeneration) {
    if (!isAdmissionCurrent(admissionGeneration, AdmissionStage::ReadyToStart)) {
        qInfo() << "[Coordinator] Refuse to start room after session invalidation.";
        return;
    }
    _admissionStage = AdmissionStage::Starting;
    QPointer<MeetingCoordinator> owner(this);
    stopRoomSession(); // 确保前序会话已释放
    if (!owner || !owner->isAdmissionCurrent(admissionGeneration, AdmissionStage::Starting)) {
        return;
    }

    owner->setState(MeetingState::ConnectingRoom, QString::fromUtf8("正在建立 WebRTC 连接..."));
    if (!owner || !owner->isAdmissionCurrent(admissionGeneration, AdmissionStage::Starting)) {
        return;
    }
    owner->_admissionStage = AdmissionStage::Consumed;
    auto roomStartHook = owner->_roomStartHook;
    if (roomStartHook) {
        roomStartHook(url, token);
        return;
    }
    _startupCommitted = false;

    _sessionRunning = true;
    _ioContext = std::make_unique<asio::io_context>();
    _workGuard = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(_ioContext->get_executor());
    _sessionRuntime = std::make_shared<MeetingSessionRuntime>(
        *_ioContext,
        ++_nextSessionGeneration,
        _sessionManager.userId());

    _room = livekit::Room::Create(_ioContext->get_executor());
    _room->SetLogHandler([](const std::string &cat, const std::string &tag, const std::string &msg) {
        MeetingUI::LogCategory c = MeetingUI::LogCategory::General;
        if (cat == "WEBRTC") c = MeetingUI::LogCategory::WebRTC;
        else if (cat == "SIGNAL") c = MeetingUI::LogCategory::Signal;
        else if (cat == "TRACK") c = MeetingUI::LogCategory::Track;
        else if (cat == "ERROR") c = MeetingUI::LogCategory::Error;
        else if (cat == "MEDIA") c = MeetingUI::LogCategory::Media;
        MeetingUI::LogToConsole(c, QString::fromStdString(tag), QString::fromStdString(msg));
    });

    _roomListener = std::make_shared<CoordinatorRoomListener>(this, _sessionRuntime);
    _room->AddListener(_roomListener);

    // 确保本地音频与视频源就绪（复用已有实例，避免重复创建断开外设采集绑定）
    if (!_localAudioSource) {
        _localAudioSource = std::make_shared<livekit::AudioSource>(48000, 2);
    }
    if (!_localVideoSource) {
        _localVideoSource = std::make_shared<livekit::VideoSource>(1280, 720);
    }

    const std::string urlStr = url.toStdString();
    const std::string tokenStr = token.toStdString();
    auto *ioContext = _ioContext.get();
    auto room = _room;
    auto session = _sessionRuntime;
    const uint64_t sessionGeneration = session->generation();
    auto audioSource = _localAudioSource;
    auto videoSource = _localVideoSource;
    const bool audioMuted = _audioMuted;
    const bool videoEnabled = _videoEnabled;
    const bool allowInsecureTransport = isDebugHttpTransportEnabled();

    _ioThread = std::thread([this, ioContext, room = std::move(room), session = std::move(session),
                             audioSource = std::move(audioSource), videoSource = std::move(videoSource),
                             audioMuted, videoEnabled, allowInsecureTransport,
                             urlStr, tokenStr, sessionGeneration] {
        livekit::SignalOptions opts;
        opts.auto_subscribe = true;
        opts.connect_timeout = std::chrono::seconds(10);
        opts.allow_insecure_transport = allowInsecureTransport;

        asio::co_spawn(*ioContext,
                        [this, room = std::move(room), session = std::move(session),
                         audioSource = std::move(audioSource), videoSource = std::move(videoSource),
                         audioMuted, videoEnabled, urlStr, tokenStr, opts, sessionGeneration]() mutable -> asio::awaitable<void> {
            MeetingStartupTransaction startup;
            try {
                if (urlStr.empty() || tokenStr.empty()) {
                    throw std::runtime_error("LiveKit URL 或访问令牌为空");
                }

                co_await room->ConnectAsync(urlStr, tokenStr, opts);
                if (!startup.markRoomConnected()) {
                    throw std::runtime_error("本地媒体启动事务状态无效");
                }
                QMetaObject::invokeMethod(this, [this, sessionGeneration]() {
                    if (!isCurrentSessionGenerationOnUiThread(sessionGeneration)) {
                        return;
                    }
                    // 房间底层信令与下行通道已就绪，立即进入 InMeeting 状态以秒级呈现远端画面
                    setState(MeetingState::InMeeting,
                             QString::fromUtf8("已成功连入会议房间，正在激活本地音视频..."));
                    emit meetingJoinedSuccessfully(_currentMeetingId);
                }, Qt::QueuedConnection);

                auto local = room->local_participant();
                if (!local) {
                    throw std::runtime_error("LiveKit 房间连接完成后未创建本地参会者");
                }

                auto audioTrack = livekit::LocalAudioTrack::createLocalAudioTrack("simple_audio", audioSource);
                audioTrack->set_muted(audioMuted);

                livekit::VideoPublishOptions vopts;
                vopts.video_codec = "vp8";
                auto videoTrack = livekit::LocalVideoTrack::createLocalVideoTrack(
                    "camera_video", videoSource, livekit::TrackSource::Camera, vopts);
                videoTrack->set_muted(!videoEnabled);

                // 【核心优化】：将本地音视频打包，发起批量发布与单次全量 SDP 协商
                std::vector<std::shared_ptr<livekit::Track>> tracksToPublish;
                tracksToPublish.push_back(audioTrack);
                tracksToPublish.push_back(videoTrack);

                auto pubs = co_await local->PublishTracksBatchAsync(std::move(tracksToPublish));
                if (!startup.markMediaBatchPublished()) {
                    throw std::runtime_error("本地媒体批量发布事务状态无效");
                }
                MeetingUI::LogToConsole(MeetingUI::LogCategory::Track, "PUBLISH",
                    QString("本地音视频批量发布成功 (共 %1 条轨，合并单次 SDP 协商完成)").arg(pubs.size()));

                QMetaObject::invokeMethod(this,
                                          [this, sessionGeneration, audioTrack = std::move(audioTrack),
                                           videoTrack = std::move(videoTrack)]() mutable {
                    completeRoomStartupOnUiThread(sessionGeneration, std::move(audioTrack), std::move(videoTrack));
                }, Qt::QueuedConnection);
            } catch (const std::exception &) {
                const QString err = QString::fromStdString(
                    livekit::secure_log::ExceptionSummary("meeting_startup"));
                const bool mediaBegan = startup.mediaStartupBegan();
                const QString title = mediaBegan
                    ? QString::fromUtf8("本地媒体启动失败")
                    : QString::fromUtf8("连接房间失败");
                if (startup.beginRollback()) {
                    startup.completeRollback();
                }
                MeetingUI::LogToConsole(MeetingUI::LogCategory::Error, "STARTUP_TRANSACTION",
                                        QString("%1: %2").arg(title, err));
                if (mediaBegan) {
                    // 房间本身连接正常，仅本地媒体硬件发布异常：降级为无媒体参会，不强制断开会议
                    QMetaObject::invokeMethod(this, [this, sessionGeneration, title, err]() {
                        if (!isCurrentSessionGenerationOnUiThread(sessionGeneration)) {
                            return;
                        }
                        _startupCommitted = true;
                        emit errorOccurred(title, QString::fromUtf8("%1 (已自动切换为仅收听收看模式)").arg(err));
                    }, Qt::QueuedConnection);
                } else {
                    // 连接房间本身失败：执行回滚并清理
                    QMetaObject::invokeMethod(this, [this, sessionGeneration, title, err]() {
                        failRoomStartupOnUiThread(sessionGeneration, title, err);
                    }, Qt::QueuedConnection);
                }
            }
        }, asio::detached);

        ioContext->run();
    });
}

void MeetingCoordinator::completeRoomStartupOnUiThread(
    uint64_t sessionGeneration,
    std::shared_ptr<livekit::LocalAudioTrack> audioTrack,
    std::shared_ptr<livekit::LocalVideoTrack> videoTrack) {
    if (!isCurrentSessionGenerationOnUiThread(sessionGeneration)) {
        return;
    }

    _localAudioTrack = std::move(audioTrack);
    _localVideoTrack = std::move(videoTrack);
    _startupCommitted = true;
    setState(MeetingState::InMeeting, QString::fromUtf8("本地音视频已就绪，已成功连入会议房间"));
    emit localAudioMuteChanged(_audioMuted);
    emit localVideoEnableChanged(_videoEnabled);
}

void MeetingCoordinator::failRoomStartupOnUiThread(
    uint64_t sessionGeneration,
    const QString &title,
    const QString &detail) {
    if (!isCurrentSessionGenerationOnUiThread(sessionGeneration)) {
        return;
    }

    // The target protocol has no local media-Unpublish request.  A full Room
    // disconnect is therefore the only server-visible rollback that cannot
    // leave an audio-only or video-only startup publication behind.
    setState(MeetingState::Leaving, QString::fromUtf8("本地媒体启动失败，正在回滚房间会话..."));
    stopRoomSession();
    setState(MeetingState::Failed, detail);
    emit errorOccurred(title, detail);
}

void MeetingCoordinator::stopRoomSession() {
    const bool was_running = _sessionRunning.exchange(false);
    if (!was_running) {
        return;
    }
    _startupCommitted = false;
    QPointer<MeetingCoordinator> owner(this);
    const auto stoppedGeneration = _nextSessionGeneration;
    std::vector<QString> failedOutboundMessages;
    QString transferCleanupError;

    if (_mediaSendTimer && _mediaSendTimer->isActive()) {
        _mediaSendTimer->stop();
    }
    for (const auto &task : _mediaSendQueue) {
        if (!task.messageId.isEmpty()) {
            failedOutboundMessages.push_back(task.messageId);
        }
    }
    _mediaSendQueue.clear();

    std::vector<QString> failedInboundTransfers;
    auto session = _sessionRuntime;
    if (session && _ioContext && _ioThread.joinable()) {
        // Serialize the admission barrier and transfer cleanup behind any
        // in-flight DataChannel callback before this thread stops the executor.
        auto cleanup = std::make_shared<std::promise<std::vector<QString>>>();
        auto completed = cleanup->get_future();
        asio::post(session->strand(), [session, cleanup]() {
            try {
                session->assertOnStrand();
                session->stopAcceptingDataOnStrand();

                std::vector<QString> failed;
                auto &transfers = session->transfersOnStrand();
                failed.reserve(transfers.size());
                for (const auto &[transferId, _] : transfers) {
                    failed.push_back(transferId.uiTransferId());
                }
                transfers.clear();
                cleanup->set_value(std::move(failed));
            } catch (...) {
                cleanup->set_exception(std::current_exception());
            }
        });
        try {
            failedInboundTransfers = completed.get();
        } catch (const std::exception &) {
            transferCleanupError = QString::fromStdString(
                livekit::secure_log::ExceptionSummary("transfer_cleanup"));
        }
    }
    for (const auto &[uiTransferId, entry] : _inboundTransferLedger) {
        if (entry.second) {
            failedInboundTransfers.erase(std::remove(failedInboundTransfers.begin(),
                failedInboundTransfers.end(), uiTransferId), failedInboundTransfers.end());
            continue;
        }
        if (std::find(failedInboundTransfers.begin(), failedInboundTransfers.end(), uiTransferId) ==
            failedInboundTransfers.end()) {
            failedInboundTransfers.push_back(uiTransferId);
        }
    }
    _inboundTransferLedger.clear();

    if (_wasapiCap) {
        _wasapiCap->Stop();
        _wasapiCap.reset();
    }
    if (_dshowCap) {
        _dshowCap->Stop();
        _dshowCap.reset();
    }

    if (_room) {
        // External owners may keep Room alive after this Coordinator. Revoke
        // its bridge registration before disconnect; the worker join below
        // still completes any listener batch already taken by the dispatcher.
        if (_roomListener) _room->RemoveListener(_roomListener);
        _room->Disconnect();
    }

    if (_workGuard) {
        _workGuard->reset();
    }
    if (_ioContext) {
        _ioContext->stop();
    }
    if (_ioThread.joinable()) {
        _ioThread.join();
    }

    // A MeetingSessionRuntime contains an asio::strand. Destroy every owner
    // of that runtime while its io_context and strand service still exist.
    // In particular, no queued Qt callback may retain the runtime; those
    // callbacks carry only the immutable session generation.
    _roomListener.reset();
    _room.reset();
    _sessionRuntime.reset();
    session.reset();
    _workGuard.reset();
    _ioContext.reset();
    _localAudioTrack.reset();
    _localVideoTrack.reset();
    _localAudioSource.reset();
    _localVideoSource.reset();
    _remoteVideoTracks.clear();
    _participantEventSequences.clear();
    _nativeRoomGeneration = 0;

    // Finish resource cleanup before notifications can synchronously delete
    // the owner or start another session. Never clean up that successor.
    const auto stillStopped = [&] {
        return owner && owner->_nextSessionGeneration == stoppedGeneration &&
            !owner->_sessionRunning.load(std::memory_order_acquire);
    };
    if (!transferCleanupError.isEmpty()) {
        MeetingUI::LogToConsole(MeetingUI::LogCategory::Error,
            "SESSION_TRANSFER_CLEANUP", transferCleanupError);
        if (!stillStopped()) return;
    }
    for (const auto &messageId : failedOutboundMessages) {
        emit owner->chatMessageSendFailed(messageId, QString::fromUtf8("会议已退出"));
        if (!stillStopped()) return;
    }
    for (const auto &transferId : failedInboundTransfers) {
        emit owner->chatMediaReceivingFailed(transferId, QString::fromUtf8("本地已离开会议"));
        if (!stillStopped()) return;
    }
}

void MeetingCoordinator::setLocalAudioMuted(bool muted) {
    _audioMuted = muted;
    if (_localAudioTrack) {
        _localAudioTrack->set_muted(muted);
    }
    for (auto &[id, p] : _participants) {
        if (p.isLocal) {
            p.isAudioMuted = muted;
            break;
        }
    }
    updateParticipantListAndNotify();
    emit localAudioMuteChanged(muted);
}

void MeetingCoordinator::setLocalVideoEnabled(bool enabled) {
    _videoEnabled = enabled;
    if (_localVideoTrack) {
        _localVideoTrack->set_muted(!enabled);
    }
    for (auto &[id, p] : _participants) {
        if (p.isLocal) {
            p.isVideoEnabled = enabled;
            break;
        }
    }
    updateParticipantListAndNotify();
    emit localVideoEnableChanged(enabled);
}

void MeetingCoordinator::sendNotifyData(const openmeeting::meeting::NotifyMeetingData &data, bool reliable) {
    if (!_room || _state != MeetingState::InMeeting) return;
    std::string bytes;
    if (!data.SerializeToString(&bytes)) return;
    std::vector<uint8_t> payload(bytes.begin(), bytes.end());
    _room->PublishData(payload, reliable);
}

int64_t MeetingCoordinator::nextSequenceNumber() {
    int64_t now = QDateTime::currentMSecsSinceEpoch() * 1000;
    int64_t counter = ++_msgSequenceCounter;
    return now + (counter % 1000);
}

void MeetingCoordinator::sendChatMessage(const QString &content, const QString &messageId, int64_t seq) {
    if (content.isEmpty()) return;
    if (!_room || _state != MeetingState::InMeeting) {
        if (!messageId.isEmpty()) {
            emit chatMessageSendFailed(messageId, QString::fromUtf8("未连入会议房间"));
        }
        return;
    }
    if (seq <= 0) {
        seq = nextSequenceNumber();
    }

    QJsonObject obj;
    obj["om_type"] = "chat_text";
    obj["seq"] = static_cast<double>(seq);
    obj["msgId"] = messageId;
    obj["text"] = content;

    QJsonDocument doc(obj);
    QByteArray jsonBytes = doc.toJson(QJsonDocument::Compact);
    std::vector<uint8_t> payload(jsonBytes.begin(), jsonBytes.end());
    try {
        bool ok = _room->PublishData(payload, true, {}, "chat");
        if (ok) {
            if (!messageId.isEmpty()) {
                QMetaObject::invokeMethod(this, [this, messageId]() {
                    emit chatMessageSendProgress(messageId, 100);
                    emit chatMessageSendSuccess(messageId);
                }, Qt::QueuedConnection);
            }
        } else {
            if (!messageId.isEmpty()) {
                emit chatMessageSendFailed(messageId, QString::fromUtf8("数据通道拥塞，发送失败"));
            }
        }
    } catch (const std::exception &) {
        if (!messageId.isEmpty()) {
            emit chatMessageSendFailed(
                messageId,
                QString::fromStdString(
                    livekit::secure_log::ExceptionSummary("chat_send")));
        }
    } catch (...) {
        if (!messageId.isEmpty()) {
            emit chatMessageSendFailed(messageId, QString::fromUtf8("发送遇到未知异常"));
        }
    }
}

void MeetingCoordinator::sendChatMediaMessage(const QString &messageId, const QString &mediaType, const QString &fileName, const QByteArray &data, int64_t seq) {
    if (data.isEmpty()) {
        if (!messageId.isEmpty()) {
            emit chatMessageSendFailed(messageId, QString::fromUtf8("发送数据为空"));
        }
        return;
    }
    if (!_room || _state != MeetingState::InMeeting) {
        if (!messageId.isEmpty()) {
            emit chatMessageSendFailed(messageId, QString::fromUtf8("未连入会议房间"));
        }
        return;
    }
    if (seq <= 0) {
        seq = nextSequenceNumber();
    }

    QString base64 = QString::fromLatin1(data.toBase64());
    const int chunkSize = 10 * 1024; // 每分片 10KB 字符，确保加上 JSON 协议头后小于 15KB，避免触发 DataStream 二次切片与竞争
    const int totalLen = base64.length();
    const int totalChunks = (totalLen + chunkSize - 1) / chunkSize;
    const QString transferId = QString("media_%1_%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(qrand() % 10000);

    // 1. 发送轻量预告包 media_start 瞬时建立接收端占位 (不等待切片，秒送达)
    QJsonObject startObj;
    startObj["om_type"] = "media_start";
    startObj["transferId"] = transferId;
    startObj["seq"] = static_cast<double>(seq);
    startObj["mediaType"] = mediaType;
    startObj["fileName"] = fileName;
    startObj["totalSize"] = static_cast<qint64>(data.size());
    startObj["totalChunks"] = totalChunks;

    QJsonDocument startDoc(startObj);
    QByteArray startBytes = startDoc.toJson(QJsonDocument::Compact);
    std::vector<uint8_t> startPayload(startBytes.begin(), startBytes.end());
    try {
        _room->PublishData(startPayload, true, {}, "chat");
    } catch (...) {
        // 忽略预告包偶发抛错，后续第一个 chunk 也会兜底建立占位
    }

    // 2. 将数据切片入队由 20ms 平滑流控调度
    MediaSendChunkTask task;
    task.messageId = messageId;
    task.mediaType = mediaType;
    task.fileName = fileName;
    task.transferId = transferId;
    task.totalSize = static_cast<qint64>(data.size());
    task.seq = seq;
    task.currentChunk = 0;
    task.totalChunks = totalChunks;
    task.base64Payload = base64;
    task.chunkSize = chunkSize;

    _mediaSendQueue.push_back(std::move(task));

    if (_mediaSendTimer && !_mediaSendTimer->isActive()) {
        _mediaSendTimer->start(20); // 每 20ms 推送一个分片
    }
}

void MeetingCoordinator::processNextMediaSendChunk() {
    if (_mediaSendQueue.empty()) {
        if (_mediaSendTimer && _mediaSendTimer->isActive()) {
            _mediaSendTimer->stop();
        }
        return;
    }

    if (!_room || _state != MeetingState::InMeeting) {
        while (!_mediaSendQueue.empty()) {
            auto task = _mediaSendQueue.front();
            _mediaSendQueue.pop_front();
            if (!task.messageId.isEmpty()) {
                emit chatMessageSendFailed(task.messageId, QString::fromUtf8("网络已断开"));
            }
        }
        if (_mediaSendTimer && _mediaSendTimer->isActive()) {
            _mediaSendTimer->stop();
        }
        return;
    }

    // === DataChannel 背压流控 (Backpressure Flow Control) ===
    // 检查底层 WebRTC SCTP 待发送缓冲区水位，门限设为 64KB
    // 若当前积压大于 64KB，暂停本轮推送，让底层网络充分排空，彻底避免打爆 SCTP 缓冲区与丢包
    uint64_t buffered = _room->GetDataChannelBufferedAmount(true);
    if (buffered > 64 * 1024) {
        return; // 等待下一个 20ms tick 再次检测
    }

    auto &task = _mediaSendQueue.front();
    int i = task.currentChunk;
    QString chunkStr = task.base64Payload.mid(i * task.chunkSize, task.chunkSize);
    QJsonObject obj;
    obj["om_type"] = "media_chunk";
    obj["transferId"] = task.transferId;
    obj["seq"] = static_cast<double>(task.seq);
    obj["chunkIndex"] = i;
    obj["totalChunks"] = task.totalChunks;
    obj["mediaType"] = task.mediaType;
    obj["fileName"] = task.fileName;
    obj["totalSize"] = task.totalSize;
    obj["chunkData"] = chunkStr;

    QJsonDocument doc(obj);
    QByteArray jsonBytes = doc.toJson(QJsonDocument::Compact);
    std::vector<uint8_t> payload(jsonBytes.begin(), jsonBytes.end());
    bool ok = false;
    try {
        ok = _room->PublishData(payload, true, {}, "chat");
    } catch (const std::exception &) {
        QString msgId = task.messageId;
        _mediaSendQueue.pop_front();
        if (!msgId.isEmpty()) {
            emit chatMessageSendFailed(
                msgId,
                QString::fromStdString(
                    livekit::secure_log::ExceptionSummary("data_packet_send")));
        }
        return;
    } catch (...) {
        QString msgId = task.messageId;
        _mediaSendQueue.pop_front();
        if (!msgId.isEmpty()) {
            emit chatMessageSendFailed(msgId, QString::fromUtf8("数据包投递遇到未知异常"));
        }
        return;
    }

    if (!ok) {
        // 底层 Send 拒绝 (网络缓冲区已满)，不推进 currentChunk，保留给下一轮 tick 重试
        return;
    }

    task.currentChunk++;
    int progress = std::min(100, (task.currentChunk * 100) / task.totalChunks);
    if (!task.messageId.isEmpty()) {
        emit chatMessageSendProgress(task.messageId, progress);
    }

    if (task.currentChunk >= task.totalChunks) {
        QString msgId = task.messageId;
        _mediaSendQueue.pop_front();
        if (!msgId.isEmpty()) {
            emit chatMessageSendSuccess(msgId);
        }
    }
}

void MeetingCoordinator::requestParticipantMute(const QString &targetUserId, bool isVideo, bool mute) {
    openmeeting::meeting::NotifyMeetingData notify;
    notify.set_operatoruserid(SessionManager::instance().userId().toStdString());

    auto *streamOp = notify.mutable_streamoperatedata();
    auto *op = streamOp->add_operation();
    op->set_userid(targetUserId.toStdString());
    if (isVideo) {
        op->set_cameraonentry(!mute);
    } else {
        op->set_microphoneonentry(!mute);
    }

    sendNotifyData(notify, true);
}

void MeetingCoordinator::requestParticipantCamera(const QString &targetUserId, bool enable) {
    requestParticipantMute(targetUserId, true, !enable);
}

void MeetingCoordinator::requestParticipantMicrophone(const QString &targetUserId, bool enable) {
    requestParticipantMute(targetUserId, false, !enable);
}

void MeetingCoordinator::muteAllParticipants(bool muteMic, bool /*allowSelfUnmute*/) {
    if (!_room || _state != MeetingState::InMeeting) return;
    openmeeting::meeting::NotifyMeetingData notify;
    notify.set_operatoruserid(SessionManager::instance().userId().toStdString());

    auto *streamOp = notify.mutable_streamoperatedata();
    for (const auto &[uid, info] : _participants) {
        if (!info.isLocal) {
            auto *op = streamOp->add_operation();
            op->set_userid(uid.toStdString());
            op->set_cameraonentry(info.isVideoEnabled);
            op->set_microphoneonentry(!muteMic);
        }
    }

    sendNotifyData(notify, true);
}

void MeetingCoordinator::transferHost(const QString &newHostUserId) {
    if (!_room || _state != MeetingState::InMeeting) return;
    openmeeting::meeting::NotifyMeetingData notify;
    notify.set_operatoruserid(SessionManager::instance().userId().toStdString());

    auto *hostData = notify.mutable_meetinghostdata();
    hostData->set_userid(newHostUserId.toStdString());
    hostData->set_operatornickname(SessionManager::instance().nickname().toStdString());
    hostData->set_hosttype("host");

    sendNotifyData(notify, true);

    _meetingDetail.hostUserId = newHostUserId;
    for (auto &[id, p] : _participants) {
        p.isHost = (id == newHostUserId);
    }
    updateParticipantListAndNotify();
    emit hostRoleChanged(newHostUserId, SessionManager::instance().nickname());
    emit meetingDetailUpdated(_meetingDetail);
}

void MeetingCoordinator::kickParticipant(const QString &targetUserId, const QString &reason) {
    openmeeting::meeting::NotifyMeetingData notify;
    notify.set_operatoruserid(SessionManager::instance().userId().toStdString());

    auto *kick = notify.mutable_kickoffmeetingdata();
    kick->set_userid(targetUserId.toStdString());
    kick->set_reason(reason.toStdString());
    kick->set_reasoncode(openmeeting::meeting::KickOffReason::Logout);

    sendNotifyData(notify, true);
}

void MeetingCoordinator::ensureLocalParticipant() {
    QString myId = _sessionManager.userId();
    if (myId.isEmpty() && _room && _room->local_participant()) {
        myId = QString::fromStdString(_room->local_participant()->identity());
    }
    if (myId.isEmpty()) {
        myId = _currentDisplayName.isEmpty() ? QString("local_user") : _currentDisplayName;
    }

    ParticipantInfo localInfo;
    localInfo.identity = myId;
    QString resolvedNick;
    if (_room && _room->local_participant()) {
        resolvedNick = ResolveParticipantNickname(_room->local_participant());
    }
    if (!resolvedNick.isEmpty() && resolvedNick != myId) {
        localInfo.name = resolvedNick;
    } else if (!_currentDisplayName.isEmpty()) {
        localInfo.name = _currentDisplayName;
    } else if (!_sessionManager.nickname().isEmpty()) {
        localInfo.name = _sessionManager.nickname();
    } else {
        localInfo.name = myId;
    }
    localInfo.isLocal = true;
    localInfo.isHost = isHost();
    localInfo.isAudioMuted = _audioMuted;
    localInfo.isVideoEnabled = _videoEnabled;
    if (_room && _room->local_participant()) {
        CopyParticipantState(_room->local_participant(), &localInfo);
    }

    // 清除旧的可能存在的本地项，避免重复
    for (auto it = _participants.begin(); it != _participants.end();) {
        if (it->second.isLocal) {
            it = _participants.erase(it);
        } else {
            ++it;
        }
    }
    _participants[myId] = localInfo;
}

std::vector<ParticipantInfo> MeetingCoordinator::participants() const {
    std::vector<ParticipantInfo> list;
    list.reserve(_participants.size());
    for (const auto &[_, info] : _participants) {
        list.push_back(info);
    }
    return list;
}

std::vector<ParticipantPresentation> MeetingCoordinator::participantPresentations() const {
    Q_ASSERT(QThread::currentThread() == thread());
    std::vector<ParticipantPresentation> result;
    result.reserve(_participants.size());
    for (const auto &[identity, info] : _participants) {
        ParticipantPresentation presentation;
        presentation.participant = info;
        presentation.coordinatorSession = _nextSessionGeneration;
        if (!isParticipantPresentationCurrent(presentation)) continue;
        const auto tracks = _remoteVideoTracks.find(identity);
        if (tracks != _remoteVideoTracks.end()) {
            for (const auto &[trackSid, value] : tracks->second) {
                if (isParticipantPresentationCurrent(presentation, &value)) {
                    presentation.videoTracks.push_back(value);
                }
            }
        }
        result.push_back(std::move(presentation));
    }
    return result;
}

bool MeetingCoordinator::isParticipantPresentationCurrent(
    const ParticipantPresentation &presentation,
    const RemoteVideoTrackPresentation *track) const {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto &info = presentation.participant;
    if (!isCurrentSessionGenerationOnUiThread(presentation.coordinatorSession) ||
        _state == MeetingState::Idle || _state == MeetingState::Leaving ||
        _state == MeetingState::Failed ||
        info.participantKey.native_room_generation != _nativeRoomGeneration ||
        !livekit::IsParticipantTicketActive(info.participantTicket, info.participantKey)) {
        return false;
    }
    const auto current = _participants.find(info.identity);
    if (current == _participants.end() || current->second.participantKey != info.participantKey ||
        current->second.lastEventSequence != info.lastEventSequence) return false;
    if (!track) return true;
    if (!track->track || track->key.participant != info.participantKey ||
        !livekit::IsTrackTicketActive(track->ticket, track->key)) return false;
    const auto tracks = _remoteVideoTracks.find(info.identity);
    if (tracks == _remoteVideoTracks.end()) return false;
    const auto currentTrack = tracks->second.find(QString::fromStdString(track->key.publication_sid));
    return currentTrack != tracks->second.end() && currentTrack->second.key == track->key &&
        currentTrack->second.track == track->track;
}

void MeetingCoordinator::updateParticipantListAndNotify() {
    auto list = participants();
    emit participantsUpdated(list);
}

void MeetingCoordinator::parseRoomMetadata(const std::string &metadata) {
    QPointer<MeetingCoordinator> owner(this);
    const auto sessionGeneration = _nextSessionGeneration;
    const auto nativeGeneration = _nativeRoomGeneration;
    const auto current = [&] {
        return owner && owner->isCurrentSessionGenerationOnUiThread(sessionGeneration) &&
            owner->_nativeRoomGeneration == nativeGeneration;
    };
    if (metadata.empty()) {
        _meetingDetail = MeetingDetail{};
        for (auto &[identity, participant] : _participants) {
            participant.isHost = false;
        }
        updateParticipantListAndNotify();
        if (!current()) return;
        const auto detail = owner->_meetingDetail;
        emit owner->meetingDetailUpdated(detail);
        return;
    }
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromRawData(metadata.data(), static_cast<int>(metadata.size())), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return;

    QJsonObject root = doc.object();
    QJsonObject detail = root.value("detail").toObject();
    if (detail.isEmpty()) return;

    QJsonObject info = detail.value("info").toObject();
    QJsonObject setting = detail.value("setting").toObject();

    _meetingDetail.meetingId = info.value("meetingID").toString();
    _meetingDetail.meetingName = info.value("meetingName").toString();
    _meetingDetail.creatorUserId = info.value("creatorUserID").toString();
    _meetingDetail.hostUserId = info.value("hostUserID").toString();
    _meetingDetail.startTime = info.value("startTime").toVariant().toLongLong();
    _meetingDetail.endTime = info.value("endTime").toVariant().toLongLong();

    _meetingDetail.disableMicrophoneOnJoin = setting.value("disableMicrophoneOnJoin").toBool();
    _meetingDetail.disableCameraOnJoin = setting.value("disableCameraOnJoin").toBool();
    _meetingDetail.lockMeeting = setting.value("lockMeeting").toBool();
    _meetingDetail.canJoinEarly = setting.value("canParticipantJoinMeetingEarly").toBool(true);

    for (auto &[identity, participant] : _participants) {
        participant.isHost = (identity == _meetingDetail.hostUserId ||
                              identity == _meetingDetail.creatorUserId);
    }

    updateParticipantListAndNotify();
    if (!current()) return;
    const auto updatedDetail = owner->_meetingDetail;
    emit owner->meetingDetailUpdated(updatedDetail);
}

void MeetingCoordinator::enqueueDataReceived(const std::shared_ptr<MeetingSessionRuntime> &session,
                                             const std::vector<uint8_t> &data,
                                             const livekit::SenderContext &sender) {
    if (!session || !_sessionRunning.load(std::memory_order_acquire)) {
        return;
    }

    asio::post(session->strand(), [this, session, data, sender]() {
        handleDataReceivedOnSessionStrand(session, data, sender);
    });
}

bool MeetingCoordinator::isCurrentSessionGenerationOnUiThread(uint64_t sessionGeneration) const {
    return sessionGeneration != 0 &&
        _sessionRunning.load(std::memory_order_acquire) &&
        _sessionRuntime &&
        _nextSessionGeneration == sessionGeneration &&
        _sessionRuntime->generation() == sessionGeneration;
}

bool MeetingCoordinator::isSenderContextCurrentOnUiThread(
    const livekit::SenderContext &sender) const {
    if (sender.origin == livekit::SenderOrigin::Server) return true;
    if (sender.origin == livekit::SenderOrigin::Unresolved ||
        !livekit::IsParticipantTicketActive(sender.ticket, sender.key)) {
        return false;
    }
    if (sender.origin == livekit::SenderOrigin::Local) return true;
    const QString identity = QString::fromStdString(
        sender.key.identity.empty() ? sender.key.sid : sender.key.identity);
    const auto participant = _participants.find(identity);
    return participant != _participants.end() &&
        participant->second.participantKey == sender.key;
}

void MeetingCoordinator::cancelInboundTransfersForParticipant(
    const livekit::ParticipantKey &participantKey) {
    auto session = _sessionRuntime;
    if (!session || !_sessionRunning.load(std::memory_order_acquire)) {
        return;
    }
    const uint64_t sessionGeneration = session->generation();

    asio::post(session->strand(), [this, session, sessionGeneration, participantKey]() {
        if (!session->acceptsDataOnStrand()) {
            return;
        }

        std::vector<QString> failedTransfers;
        auto &transfers = session->transfersOnStrand();
        for (auto it = transfers.begin(); it != transfers.end();) {
            if (it->second.senderKey == participantKey) {
                failedTransfers.push_back(it->first.uiTransferId());
                it = transfers.erase(it);
            } else {
                ++it;
            }
        }
        QMetaObject::invokeMethod(this, [this, sessionGeneration, participantKey,
                                         failedTransfers = std::move(failedTransfers)]() mutable {
            if (!isCurrentSessionGenerationOnUiThread(sessionGeneration)) {
                return;
            }
            QPointer<MeetingCoordinator> owner(this);
            for (auto it = _inboundTransferLedger.begin();
                 it != _inboundTransferLedger.end();) {
                if (it->second.first.nativeRoomGeneration == participantKey.native_room_generation &&
                    it->second.first.participantIncarnation == participantKey.incarnation &&
                    it->second.first.coordinatorSession == sessionGeneration) {
                    if (it->second.second) {
                        failedTransfers.erase(std::remove(failedTransfers.begin(),
                            failedTransfers.end(), it->first), failedTransfers.end());
                    } else if (std::find(failedTransfers.begin(), failedTransfers.end(), it->first) ==
                        failedTransfers.end()) {
                        failedTransfers.push_back(it->first);
                    }
                    it = _inboundTransferLedger.erase(it);
                } else {
                    ++it;
                }
            }
            for (const auto &transferId : failedTransfers) {
                emit owner->chatMediaReceivingFailed(transferId, QString::fromUtf8("发送方已离会"));
                if (!owner || !owner->isCurrentSessionGenerationOnUiThread(sessionGeneration)) return;
            }
        }, Qt::QueuedConnection);
    });
}

void MeetingCoordinator::handleDataReceivedOnSessionStrand(
    const std::shared_ptr<MeetingSessionRuntime> &session,
    const std::vector<uint8_t> &data,
    const livekit::SenderContext &sender) {
    session->assertOnStrand();
    if (!session->acceptsDataOnStrand()) {
        return;
    }
    const uint64_t sessionGeneration = session->generation();
    if (sender.origin == livekit::SenderOrigin::Unresolved ||
        (sender.origin != livekit::SenderOrigin::Server &&
         !livekit::IsParticipantTicketActive(sender.ticket, sender.key))) {
        return;
    }
    const std::string participantSid = sender.key.sid.empty()
        ? sender.transport_sid
        : sender.key.sid;
    const std::string participantIdentity = sender.key.identity.empty()
        ? sender.transport_identity
        : sender.key.identity;
    const QString participantName = QString::fromStdString(sender.display_name);

    openmeeting::meeting::NotifyMeetingData notify;
    if (!notify.ParseFromArray(data.data(), static_cast<int>(data.size()))) {
        QString id = !participantIdentity.empty() ? QString::fromStdString(participantIdentity) : QString::fromStdString(participantSid);
        QString name = participantName;
        if (name.isEmpty() || name == id) {
            // Participant presentation state belongs to the Qt thread. The
            // Room callback already supplied the best available name snapshot.
            name = id;
        }
        auto &transfers = session->transfersOnStrand();
        const auto makeTransferKey = [&](const QString &wireTransferId) {
            return InboundTransferKey{
                sessionGeneration,
                sender.key.native_room_generation,
                sender.key.incarnation,
                wireTransferId};
        };

        // 1. 尝试解析为 JSON 消息协议 (chat_text, media_start, media_chunk)
        QJsonParseError jErr;
        QJsonDocument jDoc = QJsonDocument::fromJson(QByteArray::fromRawData(reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size())), &jErr);
        if (jErr.error == QJsonParseError::NoError && jDoc.isObject()) {
            QJsonObject jObj = jDoc.object();
            QString omType = jObj.value("om_type").toString();

            // 1.1 文本聊天消息
            if (omType == "chat_text") {
                QString text = jObj.value("text").toString();
                int64_t seq = jObj.value("seq").toVariant().toLongLong();
                if (seq <= 0) seq = QDateTime::currentMSecsSinceEpoch() * 1000;
                QMetaObject::invokeMethod(this, [this, sessionGeneration, sender, id, name, text, seq]() {
                    if (!isCurrentSessionGenerationOnUiThread(sessionGeneration) ||
                        !isSenderContextCurrentOnUiThread(sender)) return;
                    emit chatMessageReceived(id, name, text, seq);
                }, Qt::QueuedConnection);
                return;
            }

            // 1.2 多媒体传输轻量预告包 (建立气泡占位)
            if (omType == "media_start") {
                QString transferId = jObj.value("transferId").toString();
                int64_t seq = jObj.value("seq").toVariant().toLongLong();
                if (seq <= 0) seq = QDateTime::currentMSecsSinceEpoch() * 1000;
                int totalChunks = jObj.value("totalChunks").toInt();
                QString mType = jObj.value("mediaType").toString();
                QString fName = jObj.value("fileName").toString();
                qint64 totalSize = jObj.value("totalSize").toVariant().toLongLong();
                const auto transferKey = makeTransferKey(transferId);
                const QString uiTransferId = transferKey.uiTransferId();

                auto &transfer = transfers[transferKey];
                transfer.mediaType = mType;
                transfer.fileName = fName;
                transfer.totalChunks = totalChunks;
                transfer.totalSize = totalSize;
                transfer.seq = seq;
                transfer.senderIdentity = id;
                transfer.senderName = name;
                transfer.senderKey = sender.key;
                transfer.senderTicket = sender.ticket;
                transfer.wireTransferId = transferId;
                transfer.lastActiveTimestamp = QDateTime::currentMSecsSinceEpoch();

                QMetaObject::invokeMethod(this, [this, sessionGeneration, sender, transferKey, uiTransferId,
                                                  id, name, mType, fName, totalSize, seq]() {
                    if (!isCurrentSessionGenerationOnUiThread(sessionGeneration) ||
                        !isSenderContextCurrentOnUiThread(sender)) return;
                    _inboundTransferLedger[uiTransferId] = {transferKey, false};
                    emit chatMediaReceivingStarted(uiTransferId, id, name, mType, fName, totalSize, seq);
                }, Qt::QueuedConnection);
                return;
            }

            // 1.3 多媒体数据分片
            if (omType == "media_chunk") {
                QString transferId = jObj.value("transferId").toString();
                int chunkIdx = jObj.value("chunkIndex").toInt();
                int totalChunks = jObj.value("totalChunks").toInt();
                QString mType = jObj.value("mediaType").toString();
                QString fName = jObj.value("fileName").toString();
                qint64 totalSize = jObj.value("totalSize").toVariant().toLongLong();
                int64_t seq = jObj.value("seq").toVariant().toLongLong();
                if (seq <= 0) seq = QDateTime::currentMSecsSinceEpoch() * 1000;
                QString chunkData = jObj.value("chunkData").toString();
                const auto transferKey = makeTransferKey(transferId);
                const QString uiTransferId = transferKey.uiTransferId();

                bool isFirstChunk = (transfers.find(transferKey) == transfers.end());
                auto &transfer = transfers[transferKey];
                if (isFirstChunk) {
                    transfer.mediaType = mType;
                    transfer.fileName = fName;
                    transfer.totalChunks = totalChunks;
                    transfer.totalSize = totalSize;
                    transfer.seq = seq;
                    transfer.senderIdentity = id;
                    transfer.senderName = name;
                    transfer.senderKey = sender.key;
                    transfer.senderTicket = sender.ticket;
                    transfer.wireTransferId = transferId;
                    transfer.lastActiveTimestamp = QDateTime::currentMSecsSinceEpoch();

                    QMetaObject::invokeMethod(this, [this, sessionGeneration, sender, transferKey, uiTransferId,
                                                      id, name, mType, fName, totalSize, seq]() {
                        if (!isCurrentSessionGenerationOnUiThread(sessionGeneration) ||
                            !isSenderContextCurrentOnUiThread(sender)) return;
                        _inboundTransferLedger[uiTransferId] = {transferKey, false};
                        emit chatMediaReceivingStarted(uiTransferId, id, name, mType, fName, totalSize, seq);
                    }, Qt::QueuedConnection);
                }

                if (transfer.senderKey != sender.key ||
                    !livekit::IsParticipantTicketActive(
                        transfer.senderTicket, transfer.senderKey)) {
                    transfers.erase(transferKey);
                    return;
                }

                transfer.lastActiveTimestamp = QDateTime::currentMSecsSinceEpoch();
                transfer.receivedChunks[chunkIdx] = chunkData;

                int progress = std::min(99, (static_cast<int>(transfer.receivedChunks.size()) * 100) / totalChunks);
                QMetaObject::invokeMethod(this, [this, sessionGeneration, sender, transferKey, uiTransferId, progress]() {
                    if (!isCurrentSessionGenerationOnUiThread(sessionGeneration) ||
                        !isSenderContextCurrentOnUiThread(sender)) return;
                    const auto ledger = _inboundTransferLedger.find(uiTransferId);
                    if (ledger == _inboundTransferLedger.end() ||
                        ledger->second.first < transferKey ||
                        transferKey < ledger->second.first ||
                        ledger->second.second) return;
                    emit chatMediaReceivingProgress(uiTransferId, progress);
                }, Qt::QueuedConnection);

                if (static_cast<int>(transfer.receivedChunks.size()) == totalChunks) {
                    QString fullBase64;
                    fullBase64.reserve(totalChunks * 10240);
                    for (int i = 0; i < totalChunks; ++i) {
                        fullBase64.append(transfer.receivedChunks[i]);
                    }
                    QByteArray completeData = QByteArray::fromBase64(fullBase64.toLatin1());
                    transfers.erase(transferKey);

                    QMetaObject::invokeMethod(this, [this, sessionGeneration, sender, transferKey, uiTransferId,
                                                      id, name, mType, fName, completeData]() {
                        QPointer<MeetingCoordinator> owner(this);
                        auto valid = [&](bool terminal) {
                            if (!owner ||
                                !owner->isCurrentSessionGenerationOnUiThread(sessionGeneration) ||
                                !owner->isSenderContextCurrentOnUiThread(sender)) return false;
                            const auto ledger = owner->_inboundTransferLedger.find(uiTransferId);
                            return ledger != owner->_inboundTransferLedger.end() &&
                                !(ledger->second.first < transferKey) &&
                                !(transferKey < ledger->second.first) &&
                                ledger->second.second == terminal;
                        };
                        if (!valid(false)) return;
                        emit owner->chatMediaReceivingProgress(uiTransferId, 100);
                        if (!valid(false)) return;
                        // Completion is the terminal linearization point. A
                        // synchronous leave/retire must not report failure too.
                        owner->_inboundTransferLedger.at(uiTransferId).second = true;
                        emit owner->chatMediaReceivingCompleted(uiTransferId, id, name, mType, fName, completeData);
                        if (!valid(true)) return;
                        owner->_inboundTransferLedger.erase(uiTransferId);
                        emit owner->chatMediaMessageReceived(id, name, mType, fName, completeData);
                    }, Qt::QueuedConnection);
                }
                return;
            }
        }

        // 2. 向下兼容：若不是 JSON 协议，作为普通文本聊天广播
        QString text = QString::fromUtf8(reinterpret_cast<const char *>(data.data()), static_cast<int>(data.size()));
        int64_t seq = QDateTime::currentMSecsSinceEpoch() * 1000;
        QMetaObject::invokeMethod(this, [this, sessionGeneration, sender, id, name, text, seq]() {
            if (!isCurrentSessionGenerationOnUiThread(sessionGeneration) ||
                !isSenderContextCurrentOnUiThread(sender)) return;
            emit chatMessageReceived(id, name, text, seq);
        }, Qt::QueuedConnection);
        return;
    }

    const QString &localUserId = session->localUserId();
    const bool isServerOrigin = sender.origin == livekit::SenderOrigin::Server;

    // 1. KickOff 踢出信令
    if (notify.has_kickoffmeetingdata()) {
        const auto &kick = notify.kickoffmeetingdata();
        QString targetUser = QString::fromStdString(kick.userid());
        if (targetUser == localUserId) {
            QString reason = QString::fromStdString(kick.reason());
            const QString safeReason = reason.isEmpty()
                ? QString::fromUtf8("无附加说明")
                : QString::fromStdString(
                    livekit::secure_log::OpaqueSummary("kick_reason"));
            int code = static_cast<int>(kick.reasoncode());
            QMetaObject::invokeMethod(
                this,
                [this, sessionGeneration, sender, reason, safeReason, code, isServerOrigin]() {
                QPointer<MeetingCoordinator> owner(this);
                const auto valid = [&] {
                    return owner && owner->isCurrentSessionGenerationOnUiThread(sessionGeneration) &&
                        owner->isSenderContextCurrentOnUiThread(sender);
                };
                if (!valid()) return;
                MeetingUI::LogToConsole(
                    MeetingUI::LogCategory::Participant,
                    "KICK_OFF",
                    QString("收到踢出信令: %1 (代码: %2)").arg(safeReason).arg(code));
                if (!valid()) return;
                if (code == static_cast<int>(openmeeting::meeting::KickOffReason::DuplicatedLogin)) {
                    // DuplicatedLogin 是全局账号事件，不能伪装成 LiveKit 的
                    // DuplicateIdentity 房间事件。只有没有参会者来源的服务端
                    // DataPacket 才拥有清理本地账号会话的权限。
                    if (!isServerOrigin) {
                        MeetingUI::LogToConsole(
                            MeetingUI::LogCategory::Error,
                            "UNTRUSTED_DUPLICATED_LOGIN",
                            "[Coordinator] Ignore DuplicatedLogin from a participant data message");
                        return;
                    }
                    MeetingUI::LogToConsole(
                        MeetingUI::LogCategory::Connection,
                        "DUPLICATED_LOGIN",
                        "[Coordinator] Receive trusted server duplicated-login event");
                    if (!valid()) return;
                    SessionManager::instance().invalidateSession(SessionInvalidationReason::DuplicatedLogin);
                    return;
                }
                emit owner->kickedOff(reason, code);
                if (!valid()) return;
                owner->leaveMeetingAsync(false);
                },
                Qt::QueuedConnection);
            return;
        }
    }

    // 2. StreamOperateData 远端流控信令 (开/关麦、开/关摄)
    if (notify.has_streamoperatedata()) {
        const auto &streamData = notify.streamoperatedata();
        QString opUser = QString::fromStdString(notify.operatoruserid());
        for (int i = 0; i < streamData.operation_size(); ++i) {
            const auto &op = streamData.operation(i);
            if (QString::fromStdString(op.userid()) == localUserId) {
                bool camEnable = op.cameraonentry();
                QMetaObject::invokeMethod(this, [this, sessionGeneration, sender, camEnable, opUser]() {
                    if (!isCurrentSessionGenerationOnUiThread(sessionGeneration) ||
                        !isSenderContextCurrentOnUiThread(sender)) return;
                    emit remoteMuteRequested(true, !camEnable, opUser);
                }, Qt::QueuedConnection);

                bool micEnable = op.microphoneonentry();
                QMetaObject::invokeMethod(this, [this, sessionGeneration, sender, micEnable, opUser]() {
                    if (!isCurrentSessionGenerationOnUiThread(sessionGeneration) ||
                        !isSenderContextCurrentOnUiThread(sender)) return;
                    emit remoteMuteRequested(false, !micEnable, opUser);
                }, Qt::QueuedConnection);
            }
        }
    }

    // 3. MeetingHostData 主持人角色变更信令
    if (notify.has_meetinghostdata()) {
        const auto &hostData = notify.meetinghostdata();
        QString newHost = QString::fromStdString(hostData.userid());
        QString opNick = QString::fromStdString(hostData.operatornickname());
        QMetaObject::invokeMethod(this, [this, sessionGeneration, sender, newHost, opNick]() {
            QPointer<MeetingCoordinator> owner(this);
            const auto valid = [&] {
                return owner && owner->isCurrentSessionGenerationOnUiThread(sessionGeneration) &&
                    owner->isSenderContextCurrentOnUiThread(sender);
            };
            if (!valid()) return;
            _meetingDetail.hostUserId = newHost;
            for (auto &[id, p] : _participants) {
                p.isHost = (id == newHost);
            }
            updateParticipantListAndNotify();
            if (!valid()) return;
            emit owner->hostRoleChanged(newHost, opNick);
            if (!valid()) return;
            emit owner->meetingDetailUpdated(owner->_meetingDetail);
        }, Qt::QueuedConnection);
    }
}

std::shared_ptr<livekit::TextStreamWriter> MeetingCoordinator::createTextStreamWriter(
    const QString &topic,
    const std::map<std::string, std::string> &attributes,
    const QString &streamId,
    std::optional<size_t> totalSize,
    const QString &replyToId,
    const std::vector<std::string> &destinationIdentities) {
    if (!_room) return nullptr;
    return _room->CreateTextStreamWriter(
        topic.toStdString(), attributes, streamId.toStdString(),
        totalSize, replyToId.toStdString(), destinationIdentities);
}

std::shared_ptr<livekit::ByteStreamWriter> MeetingCoordinator::createByteStreamWriter(
    const QString &name,
    const QString &topic,
    const std::map<std::string, std::string> &attributes,
    const QString &streamId,
    std::optional<size_t> totalSize,
    const QString &mimeType,
    const std::vector<std::string> &destinationIdentities) {
    if (!_room) return nullptr;
    return _room->CreateByteStreamWriter(
        name.toStdString(), topic.toStdString(), attributes, streamId.toStdString(),
        totalSize, mimeType.toStdString(), destinationIdentities);
}

} // namespace OpenMeeting
