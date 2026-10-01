#pragma once

#include "src/core/meeting_coordinator.h"
#include <functional>

namespace MeetingUI {

// Arm before admission. Media startup and reconnect may complete in either
// order; a video toggle is not a command-readiness notification.
inline void ArmAutomaticScreenShare(OpenMeeting::MeetingCoordinator* coordinator,
    QObject* context, std::function<void()> request) {
    QObject::connect(coordinator, &OpenMeeting::MeetingCoordinator::screenShareAvailabilityChanged,
        context, [coordinator, request = std::move(request), pending = true](bool available) mutable {
            if (!pending || !available || !coordinator->canStartScreenShare()) return;
            pending = false; // The request can re-enter signals or destroy its window.
            request();
        });
}

} // namespace MeetingUI
