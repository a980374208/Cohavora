#pragma once

namespace OpenMeeting {

// Models the coordinator-owned startup commit point.  It deliberately does
// not own Room or media resources: resource rollback remains on the Qt owner
// thread, while this state machine prevents a partially published session from
// being reported as joined.
class MeetingStartupTransaction final {
public:
    enum class Phase {
        ConnectingRoom,
        StartingLocalMedia,
        AudioPublished,
        VideoPublished,
        Committed,
        RollingBack,
        Failed,
        Cancelled,
    };

    [[nodiscard]] Phase phase() const { return _phase; }
    [[nodiscard]] bool mediaStartupBegan() const {
        return _phase != Phase::ConnectingRoom;
    }
    [[nodiscard]] bool readyToCommit() const {
        return _phase == Phase::VideoPublished || _phase == Phase::Committed;
    }
    [[nodiscard]] bool isTerminal() const {
        return _phase == Phase::Committed ||
            _phase == Phase::Failed ||
            _phase == Phase::Cancelled;
    }

    bool markRoomConnected() {
        return transition(Phase::ConnectingRoom, Phase::StartingLocalMedia);
    }

    bool markAudioPublished() {
        return transition(Phase::StartingLocalMedia, Phase::AudioPublished);
    }

    bool markVideoPublished() {
        return transition(Phase::AudioPublished, Phase::VideoPublished);
    }

    bool markMediaBatchPublished() {
        return transition(Phase::StartingLocalMedia, Phase::Committed);
    }

    bool commit() {
        return transition(Phase::VideoPublished, Phase::Committed);
    }

    bool beginRollback() {
        if (isTerminal() || _phase == Phase::RollingBack) {
            return false;
        }
        _phase = Phase::RollingBack;
        return true;
    }

    bool completeRollback() {
        return transition(Phase::RollingBack, Phase::Failed);
    }

    void cancel() {
        if (!isTerminal()) {
            _phase = Phase::Cancelled;
        }
    }

private:
    bool transition(Phase expected, Phase next) {
        if (_phase != expected) {
            return false;
        }
        _phase = next;
        return true;
    }

    Phase _phase = Phase::ConnectingRoom;
};

} // namespace OpenMeeting
