#include <cstdlib>
#include <iostream>

#include "src/core/meeting_startup_transaction.h"

namespace {

void Require(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        std::abort();
    }
}

} // namespace

int main() {
    using Phase = OpenMeeting::MeetingStartupTransaction::Phase;

    {
        OpenMeeting::MeetingStartupTransaction startup;
        Require(startup.phase() == Phase::ConnectingRoom, "transaction must start before room connection");
        Require(startup.markRoomConnected(), "room connection must begin local media startup");
        Require(startup.markAudioPublished(), "audio publish must follow room connection");
        Require(startup.markVideoPublished(), "video publish must follow audio publish");
        Require(startup.readyToCommit(), "both required local tracks must be published before commit");
        Require(startup.commit(), "fully published startup must commit");
        Require(startup.phase() == Phase::Committed, "successful startup must become committed");
        Require(!startup.beginRollback(), "committed startup must not roll back");
    }

    {
        OpenMeeting::MeetingStartupTransaction startup;
        Require(startup.markRoomConnected(), "room connection must be recorded");
        Require(startup.markAudioPublished(), "audio publish must be recorded");
        Require(startup.beginRollback(), "partial audio publication must require rollback");
        Require(startup.phase() == Phase::RollingBack, "partial startup must remain rolling back before cleanup");
        Require(startup.completeRollback(), "rollback must finish in failed state");
        Require(startup.phase() == Phase::Failed, "rollback completion must report failure");
        Require(!startup.commit(), "failed startup must never commit");
    }

    {
        OpenMeeting::MeetingStartupTransaction startup;
        Require(!startup.markVideoPublished(), "video may not publish before audio");
        startup.cancel();
        Require(startup.phase() == Phase::Cancelled, "cancellation must be terminal");
        Require(!startup.markRoomConnected(), "cancelled startup must reject stale progress");
    }

    {
        OpenMeeting::MeetingStartupTransaction startup;
        Require(startup.markRoomConnected(), "room connection must transition to StartingLocalMedia");
        Require(startup.markMediaBatchPublished(), "batch publish must commit directly");
        Require(startup.readyToCommit(), "batch published startup must be ready to commit");
        Require(startup.phase() == Phase::Committed, "batch published startup must be committed");
        Require(!startup.beginRollback(), "committed batch startup must not roll back");
    }

    return 0;
}
