#pragma once

#include <QtCore/QString>

#include <asio.hpp>

#include <cstdint>
#include <map>
#include <utility>
#include <tuple>

#include "participant_event.h"

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
                          QString localUserId)
        : _strand(context.get_executor())
        , _generation(generation)
        , _localUserId(std::move(localUserId)) {
    }

    Strand &strand() { return _strand; }
    uint64_t generation() const { return _generation; }
    const QString &localUserId() const { return _localUserId; }

    void assertOnStrand() const {
        Q_ASSERT(_strand.running_in_this_thread());
    }

    bool acceptsDataOnStrand() const {
        assertOnStrand();
        return _acceptingData;
    }

    void stopAcceptingDataOnStrand() {
        assertOnStrand();
        _acceptingData = false;
    }

    std::map<InboundTransferKey, InboundMediaTransfer> &transfersOnStrand() {
        assertOnStrand();
        return _inboundMediaTransfers;
    }

private:
    Strand _strand;
    const uint64_t _generation;
    const QString _localUserId;
    bool _acceptingData = true;
    std::map<InboundTransferKey, InboundMediaTransfer> _inboundMediaTransfers;
};

} // namespace OpenMeeting
