#pragma once

#include <QtCore/QString>

#include <asio.hpp>

#include <cstdint>
#include <map>
#include <utility>

namespace OpenMeeting {

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

    std::map<QString, InboundMediaTransfer> &transfersOnStrand() {
        assertOnStrand();
        return _inboundMediaTransfers;
    }

private:
    Strand _strand;
    const uint64_t _generation;
    const QString _localUserId;
    bool _acceptingData = true;
    std::map<QString, InboundMediaTransfer> _inboundMediaTransfers;
};

} // namespace OpenMeeting
