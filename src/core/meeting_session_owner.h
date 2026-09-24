#pragma once

#include "src/core/meeting_session_runtime.h"
#include "src/core/room.h"

#include <memory>
#include <thread>
#include <vector>

namespace OpenMeeting {

// No QObject/native callback captures the Coordinator to keep these resources
// alive. The shutdown service retains this owner until native retirement and
// telemetry complete and joins the I/O thread before releasing its lease.
struct MeetingSessionOwner final {
    std::shared_ptr<asio::io_context> context = std::make_shared<asio::io_context>();
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> work =
        std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(
            context->get_executor());
    std::shared_ptr<MeetingSessionRuntime> runtime;
    std::shared_ptr<livekit::Room> room;
    std::shared_ptr<livekit::RoomListener> listener;
    // Qt drops its presentation references immediately. Keep native track and
    // source references here so their final release also runs on the worker.
    std::vector<std::shared_ptr<void>> retiredMedia;
    std::thread thread;
};

} // namespace OpenMeeting
