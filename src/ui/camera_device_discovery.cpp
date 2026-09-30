#include "src/ui/camera_device_discovery.h"
#include "src/core/session_shutdown_service.h"
#include "src/media/dshow_enumerator.h"
#include "src/telemetry/diagnostic_pipeline.h"

namespace MeetingUI {

CameraDeviceDiscovery &CameraDeviceDiscovery::Instance() {
    static CameraDeviceDiscovery instance(livekit::DShowEnumerator::EnumerateVideoDevices,
        [](std::function<void()> job) {
            return OpenMeeting::SessionShutdownService::Instance().Submit(std::move(job));
        });
    return instance;
}

CameraDeviceDiscovery::CameraDeviceDiscovery(Enumerate enumerate, Submit submit, Now now)
    : CachedDeviceDiscovery(std::move(enumerate), std::move(submit), std::move(now),
        [](const Result &result, std::uint64_t elapsed) {
            using namespace livekit::diagnostic;
            EmitBusinessEvent(Event::SettingsProbe(MediaKind::Video, elapsed,
                result.cancelled ? Outcome::Cancelled :
                result.succeeded ? Outcome::Success : Outcome::Failure,
                result.devices ? static_cast<std::uint32_t>(result.devices->size()) : 0, result.cacheHit));
        }) {}

} // namespace MeetingUI
