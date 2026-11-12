#include "src/ui/audio_device_discovery.h"
#include "src/core/session_shutdown_service.h"
#include "src/media/audio_device_enumerator.h"

namespace MeetingUI {
namespace {
bool submit(std::function<void()> job) {
	return OpenMeeting::SessionShutdownService::Instance().Submit(std::move(job));
}
}

AudioDeviceDiscovery &MicrophoneDeviceDiscovery() {
	static AudioDeviceDiscovery instance(livekit::AudioDeviceEnumerator::EnumerateInputDevices, submit);
	return instance;
}

AudioDeviceDiscovery &SpeakerDeviceDiscovery() {
	static AudioDeviceDiscovery instance(livekit::AudioDeviceEnumerator::EnumerateOutputDevices, submit);
	return instance;
}

} // namespace MeetingUI
