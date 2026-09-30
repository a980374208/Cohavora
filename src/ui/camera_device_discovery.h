#pragma once

#include "src/media/dshow_types.h"
#include "src/ui/cached_device_discovery.h"

namespace MeetingUI {

// Settings and meeting controls share the same immutable camera snapshot.
class CameraDeviceDiscovery final
    : public CachedDeviceDiscovery<std::vector<livekit::DShowDeviceInfo>> {
public:
    static CameraDeviceDiscovery &Instance();
    explicit CameraDeviceDiscovery(Enumerate enumerate, Submit submit, Now now = Clock::now);
};

} // namespace MeetingUI
