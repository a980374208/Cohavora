#pragma once

#include "src/core/meeting_coordinator.h"
#include "src/telemetry/diagnostic_pipeline.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace OpenMeeting {

namespace mc_detail {

inline QString MediaPresentationId(const InboundTransferKey &key,
                                   const InboundMediaTransfer &transfer) {
    return transfer.uiTransferId.isEmpty() ? key.uiTransferId() : transfer.uiTransferId;
}

inline void EmitTransferTerminal(livekit::diagnostic::Context context,
                                 const QString &mediaType,
                                 livekit::diagnostic::TransferDirection direction,
                                 livekit::diagnostic::Outcome outcome,
                                 std::uint64_t bytes) noexcept {
    livekit::diagnostic::Event event;
    event.kind = livekit::diagnostic::EventKind::TransferTerminal;
    event.thread_role = livekit::diagnostic::ThreadRole::Ui;
    event.context = context;
    event.transfer_kind = mediaType == QStringLiteral("image")
        ? livekit::diagnostic::TransferKind::Image
        : mediaType == QStringLiteral("file")
            ? livekit::diagnostic::TransferKind::File
            : livekit::diagnostic::TransferKind::Unknown;
    event.transfer_direction = direction;
    event.outcome = outcome;
    event.bytes = bytes;
    livekit::diagnostic::EmitBusinessEvent(event);
}

// 从 LiveKit Participant 中提取真实用户昵称 (优先解析 OpenMeeting metadata 中的 JSON userInfo.nickname)
inline QString ResolveParticipantNickname(const livekit::ParticipantStateSnapshot &state) {
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

inline QString ResolveParticipantNickname(const std::shared_ptr<livekit::Participant> &p) {
    return p ? ResolveParticipantNickname(p->SnapshotState()) : QString();
}

inline bool IsMicrophoneSource(livekit::TrackSource source) {
    // Older peers can omit the source. An explicitly identified share-audio
    // publication must never change the microphone indicator.
    return source == livekit::TrackSource::Microphone ||
        source == livekit::TrackSource::Unknown;
}

inline void CopyParticipantState(const livekit::ParticipantStateSnapshot &state,
                                 ParticipantInfo *info) {
    if (!info) return;
    info->kind = state.kind;
    info->isAgent = livekit::IsRoomAgentParticipant(state);
    const auto agentStateIt = state.attributes.find("lk.agent.state");
    info->agentState = agentStateIt != state.attributes.end()
        ? QString::fromStdString(agentStateIt->second).trimmed()
        : QString();
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
        if (publication.kind == livekit::TrackKind::Audio &&
            IsMicrophoneSource(publication.source)) {
            audio_paused = true;
        } else if (publication.kind == livekit::TrackKind::Video) {
            video_paused = true;
        }
    }
    info->isAudioStreamPaused = audio_paused;
    info->isVideoStreamPaused = video_paused;
}

inline void CopyParticipantState(const std::shared_ptr<livekit::Participant> &participant,
                                 ParticipantInfo *info) {
    if (participant) CopyParticipantState(participant->SnapshotState(), info);
}

} // namespace mc_detail

class MeetingCoordinator::CoordinatorRoomListener : public livekit::RoomListener {
public:
    CoordinatorRoomListener(MeetingCoordinator *c,
                            const std::shared_ptr<MeetingSessionRuntime> &session);

    bool ConsumesParticipantEvents() const override;

    void OnParticipantEvent(const livekit::ParticipantEvent &event) override;

    void OnConnected() override;

    void OnDisconnected(livekit::RoomDisconnectReason reason,
                        const std::string &detail) override;

    void OnReconnecting() override;

    void OnReconnected() override;

    void OnRoomMetadataChanged(const livekit::RoomInfo &room,
                               const std::string &oldMetadata,
                               const std::string &newMetadata) override;

    void OnRoomUpdated(const livekit::RoomInfo &room) override;

    void OnConnectionQualityChanged(std::shared_ptr<livekit::Participant> participant,
                                    livekit::ConnectionQuality quality,
                                    float score) override;

    void OnTrackStreamStateChanged(
        std::shared_ptr<livekit::Participant> participant,
        std::shared_ptr<livekit::TrackPublication> publication,
        livekit::TrackPublication::StreamState state) override;

    void OnParticipantPermissionsChanged(
        const livekit::ParticipantPermission &oldPermission,
        const livekit::ParticipantPermission &newPermission,
        std::shared_ptr<livekit::Participant> participant) override;

    void OnTrackSubscriptionPermissionChanged(
        const livekit::TrackSubscriptionPermission &permission,
        std::shared_ptr<livekit::Participant> participant,
        std::shared_ptr<livekit::TrackPublication> publication) override;

    void OnParticipantConnected(std::shared_ptr<livekit::RemoteParticipant> p) override;

    void OnParticipantMetadataChanged(std::shared_ptr<livekit::Participant> p,
                                      const std::string &old_metadata,
                                      const std::string &new_metadata) override;

    void OnParticipantDisconnected(std::shared_ptr<livekit::RemoteParticipant> p) override;

    void OnTrackSubscribed(std::shared_ptr<livekit::Track> track,
                           std::shared_ptr<livekit::TrackPublication> pub,
                           std::shared_ptr<livekit::RemoteParticipant> p) override;

    void OnTrackUnsubscribed(std::shared_ptr<livekit::Track> track,
                             std::shared_ptr<livekit::TrackPublication> pub,
                             std::shared_ptr<livekit::RemoteParticipant> p) override;

    void OnTrackMuted(std::shared_ptr<livekit::Participant> participant,
                      std::shared_ptr<livekit::TrackPublication> publication,
                      bool muted) override;

    void OnActiveSpeakersChanged(
        const std::vector<std::shared_ptr<livekit::Participant>> &speakers) override;

    void OnDataReceived(const std::vector<uint8_t> &payload,
                        std::shared_ptr<livekit::RemoteParticipant> participant,
                        const std::string &topic) override;

    void OnTextStreamOpened(std::shared_ptr<livekit::TextStreamReader> reader,
                            std::shared_ptr<livekit::Participant> participant) override;

    void OnByteStreamOpened(std::shared_ptr<livekit::ByteStreamReader> reader,
                            std::shared_ptr<livekit::Participant> participant) override;

private:
    std::shared_ptr<QtCallbackGate<MeetingCoordinator>> _uiGate;
    std::weak_ptr<MeetingSessionRuntime> _session;
    std::weak_ptr<livekit::Room> _room;
    const uint64_t _generation;
    const quint64 _authGeneration;
    const QString _localUserId;
};

} // namespace OpenMeeting
