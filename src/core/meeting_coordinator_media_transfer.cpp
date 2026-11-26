#include <QtCore/QCoreApplication>
#include "src/core/detail/meeting_coordinator_internal.h"
#include "src/ui/meeting_log_console.h"
#include "src/telemetry/log_redaction.h"
#include "src/telemetry/diagnostic_pipeline.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QUuid>

#include <algorithm>
#include <exception>
#include <set>
#include <utility>

namespace OpenMeeting {
using namespace mc_detail;

namespace {
constexpr qint64 kMediaRecoveryCacheLifetimeMs = 10 * 60 * 1000;
constexpr qint64 kMediaRecoveryTimeoutMs = 60 * 1000;
constexpr qint64 kMediaRecoveryIntervalMs = 5 * 1000;
constexpr qint64 kMediaRecoveryCacheBytes = 128 * 1024 * 1024;
constexpr int kMediaMaximumChunks = 2048; // Covers the existing 15 MiB UI limit.
constexpr int kMediaRecoveryBatch = 64;

QString MediaCompletionId(const livekit::ParticipantKey &sender, uint64_t revision,
                          const QString &transferId) {
    return QString::number(sender.native_room_generation) + QLatin1Char(':') +
        QString::number(sender.incarnation) + QLatin1Char(':') +
        QString::number(sender.sid.size()) + QLatin1Char(':') +
        QString::fromStdString(sender.sid) + QLatin1Char(':') +
        QString::number(sender.identity.size()) + QLatin1Char(':') +
        QString::fromStdString(sender.identity) + QLatin1Char(':') +
        QString::number(revision) + QLatin1Char(':') + transferId;
}

QString MediaCompletionId(const livekit::SenderContext &sender, const QString &transferId) {
    return MediaCompletionId(sender.key, sender.encryption_revision, transferId);
}

void RememberMediaTerminal(MeetingSessionRuntime &session, const livekit::ParticipantKey &sender,
                           uint64_t revision, const QString &transferId, qint64 now) {
    auto &completed = session.completedTransferIdsOnStrand();
    completed[MediaCompletionId(sender, revision, transferId)] =
        {sender, revision, transferId, now + kMediaRecoveryCacheLifetimeMs};
    while (completed.size() > 1024) {
        const auto oldest = std::min_element(completed.begin(), completed.end(),
            [](const auto &a, const auto &b) { return a.second.expiresAt < b.second.expiresAt; });
        completed.erase(oldest);
    }
}

std::vector<uint8_t> MediaPacket(const QJsonObject &object) {
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    return {bytes.begin(), bytes.end()};
}

QJsonObject MediaChunkPacket(const QString &transferId, const QString &mediaType,
        const QString &fileName, qint64 totalSize, int64_t seq, int totalChunks,
        int index, const QString &base64Payload, int chunkSize) {
    return {{"om_type", "media_chunk"}, {"transferId", transferId}, {"resumeVersion", 1},
        {"mediaType", mediaType}, {"fileName", fileName}, {"totalSize", totalSize},
        {"seq", static_cast<double>(seq)}, {"totalChunks", totalChunks},
        {"chunkIndex", index}, {"chunkData", base64Payload.mid(index * chunkSize, chunkSize)}};
}

void EmitChatSendTerminal(livekit::diagnostic::Context context,
                          livekit::diagnostic::Outcome outcome,
                          std::uint64_t bytes) noexcept {
    livekit::diagnostic::Event event;
    event.kind = livekit::diagnostic::EventKind::ChatSendTerminal;
    event.thread_role = livekit::diagnostic::ThreadRole::Ui;
    event.context = context;
    event.chat_kind = livekit::diagnostic::ChatKind::Text;
    event.outcome = outcome;
    event.bytes = bytes;
    livekit::diagnostic::EmitBusinessEvent(event);
}
} // namespace

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
        EmitChatSendTerminal(diagnosticContext(), livekit::diagnostic::Outcome::Failure,
                             static_cast<std::uint64_t>(content.toUtf8().size()));
        if (!messageId.isEmpty()) {
            emit chatMessageSendFailed(messageId, QCoreApplication::translate("MeetingUI", "Not connected to a meeting room"));
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
        EmitChatSendTerminal(diagnosticContext(), ok
            ? livekit::diagnostic::Outcome::Success
            : livekit::diagnostic::Outcome::Failure,
            static_cast<std::uint64_t>(content.toUtf8().size()));
        if (ok) {
            if (!messageId.isEmpty()) {
                _sessionUiGate->Post([messageId](MeetingCoordinator* self) {
                    emit self->chatMessageSendProgress(messageId, 100);
                    emit self->chatMessageSendSuccess(messageId);
                });
            }
        } else {
            if (!messageId.isEmpty()) {
                emit chatMessageSendFailed(messageId, QCoreApplication::translate("MeetingUI", "Data channel congested. Send failed."));
            }
        }
    } catch (const std::exception &) {
        EmitChatSendTerminal(diagnosticContext(), livekit::diagnostic::Outcome::Failure,
                             static_cast<std::uint64_t>(content.toUtf8().size()));
        if (!messageId.isEmpty()) {
            emit chatMessageSendFailed(
                messageId,
                QString::fromStdString(
                    livekit::secure_log::ExceptionSummary("chat_send")));
        }
    } catch (...) {
        EmitChatSendTerminal(diagnosticContext(), livekit::diagnostic::Outcome::Failure,
                             static_cast<std::uint64_t>(content.toUtf8().size()));
        if (!messageId.isEmpty()) {
            emit chatMessageSendFailed(messageId, QCoreApplication::translate("MeetingUI", "An unknown error occurred while sending"));
        }
    }
}

void MeetingCoordinator::sendChatMediaMessage(const QString &messageId, const QString &mediaType, const QString &fileName, const QByteArray &data, int64_t seq) {
    if (_pendingEncryptionRecovery) {
        emit chatMessageSendFailed(messageId, tr("Encryption recovery in progress; send the file again"));
        return;
    }
    if (data.isEmpty()) {
        EmitTransferTerminal(diagnosticContext(), mediaType,
            livekit::diagnostic::TransferDirection::Send,
            livekit::diagnostic::Outcome::Failure, 0);
        if (!messageId.isEmpty()) {
            emit chatMessageSendFailed(messageId, QCoreApplication::translate("MeetingUI", "No data to send"));
        }
        return;
    }
    if (!_room || _state != MeetingState::InMeeting) {
        EmitTransferTerminal(diagnosticContext(), mediaType,
            livekit::diagnostic::TransferDirection::Send,
            livekit::diagnostic::Outcome::Failure,
            static_cast<std::uint64_t>(data.size()));
        if (!messageId.isEmpty()) {
            emit chatMessageSendFailed(messageId, QCoreApplication::translate("MeetingUI", "Not connected to a meeting room"));
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
    startObj["resumeVersion"] = 1;
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
    for (const auto &[identity, participant] : _participants) {
        if (!participant.isLocal && !identity.isEmpty() &&
            livekit::IsParticipantTicketActive(participant.participantTicket, participant.participantKey))
            task.recipients.insert(identity);
    }

    _mediaSendQueue.push_back(std::move(task));

    if (_mediaSendTimer && !_mediaSendTimer->isActive()) {
        _mediaSendTimer->start(20); // 每 20ms 推送一个分片
    }
}

void MeetingCoordinator::processNextMediaSendChunk() {
    if (_room && (_state == MeetingState::Reconnecting || _startupReconnectPending)) return;
    if (_mediaSendQueue.empty() && _mediaReplayQueue.empty()) {
        if (_mediaSendTimer && _mediaSendTimer->isActive()) {
            _mediaSendTimer->stop();
        }
        return;
    }

    if (!_room || _state != MeetingState::InMeeting) {
        _mediaReplayQueue.clear();
        QPointer<MeetingCoordinator> owner(this);
        while (!_mediaSendQueue.empty()) {
            auto task = _mediaSendQueue.front();
            _mediaSendQueue.pop_front();
            EmitTransferTerminal(diagnosticContext(), task.mediaType,
                livekit::diagnostic::TransferDirection::Send,
                livekit::diagnostic::Outcome::Failure,
                static_cast<std::uint64_t>(task.totalSize));
            if (!task.messageId.isEmpty()) {
                emit chatMessageSendFailed(task.messageId, QCoreApplication::translate("MeetingUI", "Network disconnected"));
                if (!owner) return;
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

    if (!_mediaReplayQueue.empty()) {
        auto &replay = _mediaReplayQueue.front();
        if (!isSenderContextCurrentOnUiThread(replay.recipient) || replay.chunks.empty()) {
            _mediaReplayQueue.pop_front();
            return;
        }
        const MediaSendChunkTask *source = nullptr;
        const auto cached = _mediaSendCache.find(replay.transferId);
        if (cached != _mediaSendCache.end() && cached->second.expiresAt > QDateTime::currentMSecsSinceEpoch())
            source = &cached->second;
        if (!source) {
            const auto active = std::find_if(_mediaSendQueue.begin(), _mediaSendQueue.end(),
                [&](const auto &task) { return task.transferId == replay.transferId; });
            if (active != _mediaSendQueue.end()) source = &*active;
        }
        const QString recipient = QString::fromStdString(replay.recipient.key.identity);
        if (!source || !source->recipients.count(recipient)) {
            _mediaReplayQueue.pop_front();
            return;
        }
        const int index = *replay.chunks.begin();
        bool accepted = false;
        try {
            accepted = _room->PublishData(MediaPacket(MediaChunkPacket(source->transferId,
                source->mediaType, source->fileName, source->totalSize, source->seq,
                source->totalChunks, index, source->base64Payload, source->chunkSize)),
                true, {replay.recipient.key.identity}, "chat");
        } catch (...) { /* The receiver retries the bounded missing set. */ }
        if (accepted) {
            replay.chunks.erase(index);
            if (replay.chunks.empty()) _mediaReplayQueue.pop_front();
        }
        return;
    }

    auto &task = _mediaSendQueue.front();
    // The last progress signal may run a nested event loop before its caller
    // commits success. Keep that task pending for leave/key-change cancellation.
    if (task.currentChunk >= task.totalChunks) return;
    int i = task.currentChunk;
    QString chunkStr = task.base64Payload.mid(i * task.chunkSize, task.chunkSize);
    QJsonObject obj;
    obj["om_type"] = "media_chunk";
    obj["resumeVersion"] = 1;
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
        const auto mediaType = task.mediaType;
        const auto totalSize = task.totalSize;
        QString msgId = task.messageId;
        _mediaSendQueue.pop_front();
        EmitTransferTerminal(diagnosticContext(), mediaType,
            livekit::diagnostic::TransferDirection::Send,
            livekit::diagnostic::Outcome::Failure,
            static_cast<std::uint64_t>(totalSize));
        if (!msgId.isEmpty()) {
            emit chatMessageSendFailed(
                msgId,
                QString::fromStdString(
                    livekit::secure_log::ExceptionSummary("data_packet_send")));
        }
        return;
    } catch (...) {
        const auto mediaType = task.mediaType;
        const auto totalSize = task.totalSize;
        QString msgId = task.messageId;
        _mediaSendQueue.pop_front();
        EmitTransferTerminal(diagnosticContext(), mediaType,
            livekit::diagnostic::TransferDirection::Send,
            livekit::diagnostic::Outcome::Failure,
            static_cast<std::uint64_t>(totalSize));
        if (!msgId.isEmpty()) {
            emit chatMessageSendFailed(msgId, QCoreApplication::translate("MeetingUI", "An unknown error occurred while delivering the packet"));
        }
        return;
    }

    if (!ok) {
        // 底层 Send 拒绝 (网络缓冲区已满)，不推进 currentChunk，保留给下一轮 tick 重试
        return;
    }

    ++task.currentChunk;
    const int progress = std::min(100, (task.currentChunk * 100) / task.totalChunks);
    const QString msgId = task.messageId;
    const auto mediaType = task.mediaType;
    const auto totalSize = task.totalSize;
    const auto transferId = task.transferId;
    const bool completed = task.currentChunk >= task.totalChunks;
    QPointer<MeetingCoordinator> owner(this);
    const auto generation = _nextSessionGeneration;
    if (!msgId.isEmpty()) emit chatMessageSendProgress(msgId, progress);
    if (!owner || owner->_nextSessionGeneration != generation || !owner->_sessionRunning) return;
    if (completed) {
        if (_mediaSendQueue.empty() || _mediaSendQueue.front().transferId != transferId) return;
        auto completedTask = std::move(_mediaSendQueue.front());
        _mediaSendQueue.pop_front();
        completedTask.expiresAt = QDateTime::currentMSecsSinceEpoch() + kMediaRecoveryCacheLifetimeMs;
        _mediaSendCache[transferId] = std::move(completedTask);
        pruneMediaSendCache(QDateTime::currentMSecsSinceEpoch());
        EmitTransferTerminal(diagnosticContext(), mediaType,
            livekit::diagnostic::TransferDirection::Send,
            livekit::diagnostic::Outcome::Success,
            static_cast<std::uint64_t>(totalSize));
        if (!msgId.isEmpty()) {
            emit chatMessageSendSuccess(msgId);
        }
    }
}

void MeetingCoordinator::pruneMediaSendCache(qint64 now) {
    qint64 bytes = 0;
    for (auto it = _mediaSendCache.begin(); it != _mediaSendCache.end();) {
        if (it->second.expiresAt <= now) it = _mediaSendCache.erase(it);
        else {
            bytes += qint64(it->second.base64Payload.size()) * sizeof(QChar);
            ++it;
        }
    }
    while (bytes > kMediaRecoveryCacheBytes || _mediaSendCache.size() > 128) {
        const auto oldest = std::min_element(_mediaSendCache.begin(), _mediaSendCache.end(),
            [](const auto &a, const auto &b) { return a.second.expiresAt < b.second.expiresAt; });
        bytes -= qint64(oldest->second.base64Payload.size()) * sizeof(QChar);
        _mediaSendCache.erase(oldest);
    }
}

void MeetingCoordinator::handleMediaResumeRequest(
    const QJsonObject &request, const livekit::SenderContext &sender) {
    if (!_room || _state != MeetingState::InMeeting || _pendingEncryptionRecovery ||
        sender.origin != livekit::SenderOrigin::Remote || sender.key.identity.empty() ||
        !isSenderContextCurrentOnUiThread(sender)) return;
    const QString id = request.value("transferId").toString();
    const auto missing = request.value("missing").toArray();
    if (id.isEmpty() || id.size() > 256 || missing.isEmpty() || missing.size() > kMediaRecoveryBatch) return;
    pruneMediaSendCache(QDateTime::currentMSecsSinceEpoch());
    const MediaSendChunkTask *source = nullptr;
    if (const auto cached = _mediaSendCache.find(id); cached != _mediaSendCache.end()) source = &cached->second;
    if (!source) {
        const auto active = std::find_if(_mediaSendQueue.begin(), _mediaSendQueue.end(),
            [&](const auto &task) { return task.transferId == id; });
        if (active != _mediaSendQueue.end()) source = &*active;
    }
    if (!source) {
        try {
            _room->PublishData(MediaPacket({{"om_type", "media_unavailable"}, {"transferId", id}}),
                true, {sender.key.identity}, "chat");
        } catch (...) {}
        return;
    }
    if (!source->recipients.count(QString::fromStdString(sender.key.identity))) return;
    std::set<int> chunks;
    for (const auto &value : missing) {
        const int index = value.toInt(-1);
        if (!value.isDouble() || value.toDouble() != index || index < 0 || index >= source->totalChunks) return;
        chunks.insert(index);
    }
    const auto existing = std::find_if(_mediaReplayQueue.begin(), _mediaReplayQueue.end(),
        [&](const auto &task) { return task.transferId == id && task.recipient.key.identity == sender.key.identity; });
    if (existing != _mediaReplayQueue.end()) {
        if (existing->recipient.key != sender.key) existing->chunks.clear();
        existing->recipient = sender;
        for (int index : chunks) {
            if (existing->chunks.size() == kMediaRecoveryBatch) break;
            existing->chunks.insert(index);
        }
    } else {
        if (_mediaReplayQueue.size() >= 128) return;
        _mediaReplayQueue.push_back({id, sender, std::move(chunks)});
    }
    if (_mediaSendTimer && !_mediaSendTimer->isActive()) _mediaSendTimer->start(20);
}

void MeetingCoordinator::enqueueDataReceived(const std::shared_ptr<MeetingSessionRuntime> &session,
                                             const std::vector<uint8_t> &data,
                                             const livekit::SenderContext &sender) {
    if (!session || !_sessionRunning.load(std::memory_order_acquire)) {
        return;
    }

    session->post( [gate = _sessionUiGate, session, data, sender]() {
        handleDataReceivedOnSessionStrand(gate, session, data, sender);
    });
}

void MeetingCoordinator::failInboundMediaTransferOnStrand(
    const std::shared_ptr<QtCallbackGate<MeetingCoordinator>> &gate,
    const std::shared_ptr<MeetingSessionRuntime> &session,
    const InboundTransferKey &key, const QString &reason) {
    auto &transfers = session->transfersOnStrand();
    const auto it = transfers.find(key);
    if (it == transfers.end()) return;
    const auto uiId = MediaPresentationId(key, it->second);
    const auto mediaType = it->second.mediaType;
    const auto size = it->second.totalSize;
    RememberMediaTerminal(*session, it->second.senderKey, key.encryptionRevision,
        key.wireTransferId, QDateTime::currentMSecsSinceEpoch());
    transfers.erase(it);
    gate->Post([generation = session->generation(), key, uiId, mediaType, size, reason](MeetingCoordinator *self) {
        if (!self->isCurrentSessionGenerationOnUiThread(generation)) return;
        const auto ledger = self->_inboundTransferLedger.find(uiId);
        if (ledger == self->_inboundTransferLedger.end() || ledger->second.second ||
            ledger->second.first < key || key < ledger->second.first) return;
        self->_inboundTransferLedger.erase(ledger);
        EmitTransferTerminal(self->diagnosticContext(), mediaType,
            livekit::diagnostic::TransferDirection::Receive, livekit::diagnostic::Outcome::Failure,
            static_cast<std::uint64_t>((std::max)(qint64{0}, size)));
        emit self->chatMediaReceivingFailed(uiId, reason);
    });
}

void MeetingCoordinator::restoreInboundTransfersForParticipant(
    const livekit::ParticipantKey &participantKey, const livekit::ParticipantTicket &ticket) {
    const auto session = _sessionRuntime;
    if (!session || !_sessionRunning || participantKey.sid.empty() || participantKey.identity.empty()) return;
    const auto interruptedGeneration = _mediaReconnectGeneration;
    const auto previous = _mediaReconnectPeers.find(QString::fromStdString(participantKey.identity));
    if (previous == _mediaReconnectPeers.end() || previous->second.sid != participantKey.sid ||
        previous->second.native_room_generation > interruptedGeneration ||
        participantKey.native_room_generation <= interruptedGeneration) return;
    const auto previousKey = previous->second;
    session->post([gate = _sessionUiGate, session, participantKey, ticket, interruptedGeneration, previousKey] {
        if (!session->acceptsDataOnStrand() || interruptedGeneration == 0 ||
            !livekit::IsParticipantTicketActive(ticket, participantKey)) return;
        auto &transfers = session->transfersOnStrand();
        // Only the participant present when this local reconnect began may
        // carry terminal history into its replacement native Room instance.
        auto &terminals = session->completedTransferIdsOnStrand();
        std::vector<InboundMediaTerminal> restoredTerminals;
        for (auto it = terminals.begin(); it != terminals.end();) {
            if (it->second.senderKey == previousKey) {
                auto terminal = it->second;
                terminal.senderKey = participantKey;
                restoredTerminals.push_back(std::move(terminal));
                it = terminals.erase(it);
            } else ++it;
        }
        for (auto &terminal : restoredTerminals)
            terminals[MediaCompletionId(terminal.senderKey, terminal.encryptionRevision,
                terminal.wireTransferId)] = std::move(terminal);
        std::vector<InboundTransferKey> keys;
        for (const auto &[key, transfer] : transfers) {
            if (transfer.senderKey == previousKey)
                keys.push_back(key);
        }
        for (const auto &oldKey : keys) {
            auto node = transfers.extract(oldKey);
            auto &transfer = node.mapped();
            const auto uiId = MediaPresentationId(oldKey, transfer);
            transfer.uiTransferId = uiId;
            transfer.senderKey = participantKey;
            transfer.senderTicket = ticket;
            transfer.lastActiveTimestamp = QDateTime::currentMSecsSinceEpoch();
            transfer.lastRecoveryTimestamp = 0;
            transfer.completionQueued = false;
            const InboundTransferKey newKey{oldKey.coordinatorSession,
                participantKey.native_room_generation, participantKey.incarnation,
                oldKey.wireTransferId, oldKey.encryptionRevision};
            node.key() = newKey;
            const auto senderIdentity = transfer.senderIdentity;
            const auto senderName = transfer.senderName;
            const auto mediaType = transfer.mediaType;
            const auto fileName = transfer.fileName;
            const auto totalSize = transfer.totalSize;
            const auto seq = transfer.seq;
            transfers.insert(std::move(node));
            gate->Post([generation = session->generation(), oldKey, newKey, uiId,
                        participantKey, ticket, senderIdentity, senderName, mediaType,
                        fileName, totalSize, seq](MeetingCoordinator *self) {
                if (!self->isCurrentSessionGenerationOnUiThread(generation) ||
                    self->_pendingEncryptionRecovery ||
                    !livekit::IsParticipantTicketActive(ticket, participantKey)) return;
                const auto ledger = self->_inboundTransferLedger.find(uiId);
                if (ledger != self->_inboundTransferLedger.end() && !ledger->second.second &&
                    !(oldKey < ledger->second.first) && !(ledger->second.first < oldKey))
                    ledger->second.first = newKey;
                if (ledger == self->_inboundTransferLedger.end()) {
                    self->_inboundTransferLedger[uiId] = {newKey, false};
                    QPointer<MeetingCoordinator> owner(self);
                    emit self->chatMediaReceivingStarted(uiId, senderIdentity, senderName,
                        mediaType, fileName, totalSize, seq);
                    if (!owner || !owner->isCurrentSessionGenerationOnUiThread(generation)) return;
                }
                self->processMediaRecovery(true);
            });
        }
    });
}

void MeetingCoordinator::processMediaRecovery(bool force) {
    const auto now = QDateTime::currentMSecsSinceEpoch();
    pruneMediaSendCache(now);
    const auto session = _sessionRuntime;
    if (!session || !_room || !_sessionRunning || _pendingEncryptionRecovery ||
        (!force && (_state != MeetingState::InMeeting || _startupReconnectPending)) ||
        (force && _state != MeetingState::InMeeting && _state != MeetingState::Reconnecting)) return;
    // Native roster delivery and the lifecycle callback use different queues.
    // A sender present here must wait for its new immutable ticket to reach Qt.
    std::map<std::string, std::string> nativePeers;
    for (const auto &[_, participant] : _room->remote_participants())
        if (participant) nativePeers[participant->identity()] = participant->sid();
    session->post([gate = _sessionUiGate, session, force, now, nativePeers = std::move(nativePeers)] {
        if (!session->acceptsDataOnStrand()) return;
        auto &terminals = session->completedTransferIdsOnStrand();
        for (auto it = terminals.begin(); it != terminals.end();)
            if (it->second.expiresAt <= now) it = terminals.erase(it); else ++it;
        auto &transfers = session->transfersOnStrand();
        std::vector<std::pair<InboundTransferKey, QString>> failures;
        for (auto &[key, transfer] : transfers) {
            const auto native = nativePeers.find(transfer.senderKey.identity);
            if (force && (native == nativePeers.end() || native->second != transfer.senderKey.sid)) {
                failures.emplace_back(key, QCoreApplication::translate("MeetingUI", "The sender has left the meeting"));
                continue;
            }
            if (!livekit::IsParticipantTicketActive(transfer.senderTicket, transfer.senderKey)) {
                // A queued roster upsert will rebind a locally replaced Room.
                if (!force && now - transfer.lastActiveTimestamp >= kMediaRecoveryTimeoutMs)
                    failures.emplace_back(key, QCoreApplication::translate("MeetingUI", "File recovery timed out. Ask the sender to send it again."));
                continue;
            }
            if (force) transfer.lastActiveTimestamp = now;
            if (!force && now - transfer.lastActiveTimestamp >= kMediaRecoveryTimeoutMs) {
                failures.emplace_back(key, QCoreApplication::translate("MeetingUI", "File recovery timed out. Ask the sender to send it again."));
                continue;
            }
            if (!transfer.supportsResume) continue;
            if (!force && (now - transfer.lastActiveTimestamp < kMediaRecoveryIntervalMs ||
                now - transfer.lastRecoveryTimestamp < kMediaRecoveryIntervalMs)) continue;
            if (transfer.totalChunks <= 0 || transfer.totalChunks > kMediaMaximumChunks) continue;
            QJsonArray missing;
            for (int index = 0; index < transfer.totalChunks && missing.size() < kMediaRecoveryBatch; ++index)
                if (!transfer.receivedChunks.count(index)) missing.append(index);
            // Assembly may have finished before its old Qt callback was revoked.
            // One duplicate chunk supplies a fresh receipt context for delivery.
            if (missing.isEmpty()) missing.append(transfer.totalChunks - 1);
            transfer.lastRecoveryTimestamp = now;
            const auto uiId = MediaPresentationId(key, transfer);
            const auto senderKey = transfer.senderKey;
            const auto ticket = transfer.senderTicket;
            const QJsonObject request{{"om_type", "media_resume"}, {"transferId", key.wireTransferId}, {"missing", missing}};
            gate->Post([generation = session->generation(), key, uiId, senderKey, ticket, request](MeetingCoordinator *self) {
                if (!self->isCurrentSessionGenerationOnUiThread(generation) || !self->_room ||
                    self->_state != MeetingState::InMeeting || self->_pendingEncryptionRecovery ||
                    !livekit::IsParticipantTicketActive(ticket, senderKey)) return;
                const auto ledger = self->_inboundTransferLedger.find(uiId);
                if (ledger == self->_inboundTransferLedger.end() || ledger->second.second ||
                    ledger->second.first < key || key < ledger->second.first) return;
                const auto participant = self->_participants.find(QString::fromStdString(senderKey.identity));
                if (participant == self->_participants.end() || participant->second.participantKey != senderKey) return;
                try { self->_room->PublishData(MediaPacket(request), true, {senderKey.identity}, "chat"); }
                catch (...) { /* Retried by the recovery timer. */ }
            });
        }
        for (const auto &[key, reason] : failures)
            failInboundMediaTransferOnStrand(gate, session, key, reason);
    });
}

void MeetingCoordinator::cancelInboundTransfersForParticipant(
    const livekit::ParticipantKey &participantKey) {
    auto session = _sessionRuntime;
    if (!session || !_sessionRunning.load(std::memory_order_acquire)) {
        return;
    }
    const uint64_t sessionGeneration = session->generation();

    session->post( [gate = _sessionUiGate, session, sessionGeneration, participantKey]() {
        if (!session->acceptsDataOnStrand()) {
            return;
        }

        std::map<QString, std::pair<QString, std::uint64_t>> failedDetails;
        auto &transfers = session->transfersOnStrand();
        for (auto it = transfers.begin(); it != transfers.end();) {
            if (it->second.senderKey == participantKey) {
                const auto id = MediaPresentationId(it->first, it->second);
                failedDetails.emplace(id, std::make_pair(it->second.mediaType,
                    static_cast<std::uint64_t>((std::max)(qint64{0}, it->second.totalSize))));
                RememberMediaTerminal(*session, it->second.senderKey, it->first.encryptionRevision,
                    it->first.wireTransferId, QDateTime::currentMSecsSinceEpoch());
                it = transfers.erase(it);
            } else {
                ++it;
            }
        }
        gate->Post([sessionGeneration, participantKey,
                    failedDetails = std::move(failedDetails)](MeetingCoordinator* self) mutable {
            if (!self->isCurrentSessionGenerationOnUiThread(sessionGeneration)) {
                return;
            }
            QPointer<MeetingCoordinator> owner(self);
            // Only a published placeholder needs a failure projection. A
            // retired receipt may still be assembled on the strand while its
            // start/completion Qt callbacks were rejected before publication.
            std::vector<QString> failedTransfers;
            for (auto it = self->_inboundTransferLedger.begin();
                 it != self->_inboundTransferLedger.end();) {
                if (it->second.first.nativeRoomGeneration == participantKey.native_room_generation &&
                    it->second.first.participantIncarnation == participantKey.incarnation &&
                    it->second.first.coordinatorSession == sessionGeneration) {
                    if (!it->second.second) failedTransfers.push_back(it->first);
                    it = self->_inboundTransferLedger.erase(it);
                } else {
                    ++it;
                }
            }
            for (const auto &transferId : failedTransfers) {
                const auto detail = failedDetails.find(transferId);
                EmitTransferTerminal(owner->diagnosticContext(),
                    detail == failedDetails.end() ? QString{} : detail->second.first,
                    livekit::diagnostic::TransferDirection::Receive,
                    livekit::diagnostic::Outcome::Failure,
                    detail == failedDetails.end() ? 0 : detail->second.second);
                emit owner->chatMediaReceivingFailed(transferId, QCoreApplication::translate("MeetingUI", "The sender has left the meeting"));
                if (!owner || !owner->isCurrentSessionGenerationOnUiThread(sessionGeneration)) return;
            }
        });
    });
}

void MeetingCoordinator::handleDataReceivedOnSessionStrand(
    const std::shared_ptr<QtCallbackGate<MeetingCoordinator>>& gate,
    const std::shared_ptr<MeetingSessionRuntime> &session,
    const std::vector<uint8_t> &data,
    const livekit::SenderContext &sender) {
    session->assertOnStrand();
    if (!session->acceptsDataOnStrand() || !sender.encryptionCurrent()) {
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
                wireTransferId, sender.encryption_revision};
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
                gate->Post([sessionGeneration, sender, id, name, text, seq](MeetingCoordinator* self) {
                    if (!self->isCurrentSessionGenerationOnUiThread(sessionGeneration) ||
                        !self->isSenderContextCurrentOnUiThread(sender)) return;
                    emit self->chatMessageReceived(id, name, text, seq);
                });
                return;
            }

            if (omType == "media_resume") {
                gate->Post([sessionGeneration, sender, request = std::move(jObj)](MeetingCoordinator *self) {
                    if (self->isCurrentSessionGenerationOnUiThread(sessionGeneration))
                        self->handleMediaResumeRequest(request, sender);
                });
                return;
            }
            if (omType == "media_unavailable") {
                failInboundMediaTransferOnStrand(gate, session,
                    makeTransferKey(jObj.value("transferId").toString()),
                    QCoreApplication::translate("MeetingUI", "File recovery is unavailable. Ask the sender to send it again."));
                return;
            }

            if (omType == "media_start" || omType == "media_chunk") {
                const QString transferId = jObj.value("transferId").toString();
                const int totalChunks = jObj.value("totalChunks").toInt();
                const int chunkIdx = jObj.value("chunkIndex").toInt(-1);
                const QString mType = jObj.value("mediaType").toString();
                const QString fName = jObj.value("fileName").toString();
                const qint64 totalSize = jObj.value("totalSize").toVariant().toLongLong();
                const QString chunkData = jObj.value("chunkData").toString();
                const bool isChunk = omType == "media_chunk";
                if (transferId.isEmpty() || transferId.size() > 256 || totalChunks <= 0 ||
                    totalChunks > kMediaMaximumChunks || totalSize <= 0 || totalSize > 15 * 1024 * 1024 ||
                    (mType != "file" && mType != "image") ||
                    (isChunk && (chunkIdx < 0 || chunkIdx >= totalChunks || chunkData.isEmpty() || chunkData.size() > 10240))) return;
                const auto now = QDateTime::currentMSecsSinceEpoch();
                const auto completionId = MediaCompletionId(sender, transferId);
                auto &terminals = session->completedTransferIdsOnStrand();
                if (const auto terminal = terminals.find(completionId); terminal != terminals.end()) {
                    if (terminal->second.expiresAt > now) return;
                    terminals.erase(terminal);
                }
                const auto transferKey = makeTransferKey(transferId);
                auto existing = transfers.find(transferKey);
                if (existing == transfers.end()) {
                    if (transfers.size() >= 64) return;
                    InboundMediaTransfer value;
                    value.uiTransferId = transferKey.uiTransferId();
                    value.mediaType = mType;
                    value.fileName = fName;
                    value.totalChunks = totalChunks;
                    value.totalSize = totalSize;
                    value.seq = jObj.value("seq").toVariant().toLongLong();
                    if (value.seq <= 0) value.seq = now * 1000;
                    value.senderIdentity = id;
                    value.senderName = name;
                    value.senderKey = sender.key;
                    value.senderTicket = sender.ticket;
                    value.wireTransferId = transferId;
                    value.supportsResume = jObj.value("resumeVersion").toInt() == 1;
                    value.lastActiveTimestamp = now;
                    existing = transfers.emplace(transferKey, std::move(value)).first;
                }
                auto &transfer = existing->second;
                if (transfer.senderKey != sender.key ||
                    !livekit::IsParticipantTicketActive(transfer.senderTicket, transfer.senderKey) ||
                    transfer.totalChunks != totalChunks || transfer.totalSize != totalSize ||
                    transfer.mediaType != mType || transfer.fileName != fName) return;
                const auto uiTransferId = MediaPresentationId(transferKey, transfer);
                const auto seq = transfer.seq;
                // A prior start can be revoked while the native roster is replaced.
                // Ensure the stable placeholder exists before projecting progress.
                gate->Post([sessionGeneration, sender, transferKey, uiTransferId,
                            id, name, mType, fName, totalSize, seq](MeetingCoordinator *self) {
                    if (!self->isCurrentSessionGenerationOnUiThread(sessionGeneration) ||
                        !self->isSenderContextCurrentOnUiThread(sender)) return;
                    if (self->_inboundTransferLedger.count(uiTransferId)) return;
                    self->_inboundTransferLedger[uiTransferId] = {transferKey, false};
                    emit self->chatMediaReceivingStarted(uiTransferId, id, name, mType, fName, totalSize, seq);
                });
                if (!isChunk) return;
                if (transfer.receivedChunks.emplace(chunkIdx, chunkData).second)
                    transfer.lastActiveTimestamp = now;
                const int progress = std::min(99, (static_cast<int>(transfer.receivedChunks.size()) * 100) / totalChunks);
                gate->Post([sessionGeneration, sender, transferKey, uiTransferId, progress](MeetingCoordinator *self) {
                    if (!self->isCurrentSessionGenerationOnUiThread(sessionGeneration) ||
                        !self->isSenderContextCurrentOnUiThread(sender)) return;
                    const auto ledger = self->_inboundTransferLedger.find(uiTransferId);
                    if (ledger == self->_inboundTransferLedger.end() || ledger->second.second ||
                        ledger->second.first < transferKey || transferKey < ledger->second.first) return;
                    emit self->chatMediaReceivingProgress(uiTransferId, progress);
                });
                if (static_cast<int>(transfer.receivedChunks.size()) == totalChunks) {
                    if (transfer.completionQueued) return;
                    transfer.completionQueued = true;
                    QString fullBase64;
                    fullBase64.reserve(totalChunks * 10240);
                    for (int index = 0; index < totalChunks; ++index)
                        fullBase64.append(transfer.receivedChunks.at(index));
                    const auto completeData = QByteArray::fromBase64(fullBase64.toLatin1());
                    if (completeData.size() != transfer.totalSize) {
                        failInboundMediaTransferOnStrand(gate, session, transferKey,
                            QCoreApplication::translate("MeetingUI", "The received file is incomplete. Ask the sender to send it again."));
                        return;
                    }
                    // Retain assembly until Qt commits its terminal result. A
                    // full reconnect can revoke this receipt before UI delivery.
                    gate->Post([gate, weakSession = std::weak_ptr<MeetingSessionRuntime>(session),
                                sessionGeneration, sender, transferKey, uiTransferId,
                                id, name, mType, fName, completeData](MeetingCoordinator *self) {
                        QPointer<MeetingCoordinator> owner(self);
                        auto valid = [&](bool terminal) {
                            if (!owner || !owner->isCurrentSessionGenerationOnUiThread(sessionGeneration) ||
                                !owner->isSenderContextCurrentOnUiThread(sender)) return false;
                            const auto ledger = owner->_inboundTransferLedger.find(uiTransferId);
                            return ledger != owner->_inboundTransferLedger.end() &&
                                !(ledger->second.first < transferKey) && !(transferKey < ledger->second.first) &&
                                ledger->second.second == terminal;
                        };
                        const auto retryDelivery = [&] {
                            if (auto runtime = weakSession.lock()) {
                                runtime->post([runtime, transferKey] {
                                    const auto transfer = runtime->transfersOnStrand().find(transferKey);
                                    if (transfer != runtime->transfersOnStrand().end())
                                        transfer->second.completionQueued = false;
                                });
                            }
                        };
                        if (!valid(false)) { retryDelivery(); return; }
                        emit owner->chatMediaReceivingProgress(uiTransferId, 100);
                        if (!valid(false)) { retryDelivery(); return; }
                        owner->_inboundTransferLedger.at(uiTransferId).second = true;
                        if (auto runtime = weakSession.lock()) {
                            runtime->post([gate, runtime, transferKey, uiTransferId, senderKey = sender.key] {
                                if (!runtime->acceptsDataOnStrand()) return;
                                runtime->transfersOnStrand().erase(transferKey);
                                RememberMediaTerminal(*runtime, senderKey, transferKey.encryptionRevision,
                                    transferKey.wireTransferId, QDateTime::currentMSecsSinceEpoch());
                                gate->Post([generation = runtime->generation(), transferKey, uiTransferId](MeetingCoordinator *current) {
                                    if (!current->isCurrentSessionGenerationOnUiThread(generation)) return;
                                    const auto ledger = current->_inboundTransferLedger.find(uiTransferId);
                                    if (ledger != current->_inboundTransferLedger.end() && ledger->second.second &&
                                        !(ledger->second.first < transferKey) && !(transferKey < ledger->second.first))
                                        current->_inboundTransferLedger.erase(ledger);
                                });
                            });
                        }
                        EmitTransferTerminal(owner->diagnosticContext(), mType,
                            livekit::diagnostic::TransferDirection::Receive, livekit::diagnostic::Outcome::Success,
                            static_cast<std::uint64_t>(completeData.size()));
                        emit owner->chatMediaReceivingCompleted(uiTransferId, id, name, mType, fName, completeData);
                        if (!valid(true)) return;
                        // Keep the committed ledger until the strand acknowledges
                        // it, so a duplicate queued completion cannot recreate it.
                        emit owner->chatMediaMessageReceived(id, name, mType, fName, completeData);
                    });
                }
                return;
            }
        }

        // 2. 向下兼容：若不是 JSON 协议，作为普通文本聊天广播
        QString text = QString::fromUtf8(reinterpret_cast<const char *>(data.data()), static_cast<int>(data.size()));
        int64_t seq = QDateTime::currentMSecsSinceEpoch() * 1000;
        gate->Post([sessionGeneration, sender, id, name, text, seq](MeetingCoordinator* self) {
            if (!self->isCurrentSessionGenerationOnUiThread(sessionGeneration) ||
                !self->isSenderContextCurrentOnUiThread(sender)) return;
            emit self->chatMessageReceived(id, name, text, seq);
        });
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
                ? QCoreApplication::translate("MeetingUI", "No additional details")
                : QString::fromStdString(
                    livekit::secure_log::OpaqueSummary("kick_reason"));
            int code = static_cast<int>(kick.reasoncode());
            gate->Post([sessionGeneration, sender, reason, safeReason, code, isServerOrigin](MeetingCoordinator* self) {
                QPointer<MeetingCoordinator> owner(self);
                const auto valid = [&] {
                    return owner && owner->isCurrentSessionGenerationOnUiThread(sessionGeneration) &&
                        owner->isSenderContextCurrentOnUiThread(sender);
                };
                if (!valid()) return;
                MeetingUI::LogToConsole(
                    MeetingUI::LogCategory::Participant,
                    "KICK_OFF",
                    QCoreApplication::translate("MeetingUI", "Removal signal received: %1 (code: %2)").arg(safeReason).arg(code));
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
                    // Trusted account notifications are consumed on their first
                    // Qt delivery, with the listener's authentication generation.
                    return;
                }
                emit owner->kickedOff(reason, code);
                if (!valid()) return;
                owner->leaveMeetingAsync(false);
                });
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
                if (op.has_cameraonentry()) {
                    const bool camEnable = op.cameraonentry();
                    gate->Post([sessionGeneration, sender, camEnable, opUser](MeetingCoordinator* self) {
                        if (!self->isCurrentSessionGenerationOnUiThread(sessionGeneration) ||
                            !self->isSenderContextCurrentOnUiThread(sender)) return;
                        emit self->remoteMuteRequested(true, !camEnable, opUser);
                    });
                }

                if (op.has_microphoneonentry()) {
                    const bool micEnable = op.microphoneonentry();
                    gate->Post([sessionGeneration, sender, micEnable, opUser](MeetingCoordinator* self) {
                        if (!self->isCurrentSessionGenerationOnUiThread(sessionGeneration) ||
                            !self->isSenderContextCurrentOnUiThread(sender)) return;
                        emit self->remoteMuteRequested(false, !micEnable, opUser);
                    });
                }
            }
        }
    }

    // Host notifications precede the server metadata transaction and are also
    // client-sendable. Only committed room metadata / HTTP success changes roles.
}

} // namespace OpenMeeting
