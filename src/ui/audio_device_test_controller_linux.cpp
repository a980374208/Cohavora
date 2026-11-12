#include "audio_device_test_controller.h"
#include "src/media/audio_device_enumerator.h"
#include "src/core/session_shutdown_service.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QThread>

namespace MeetingUI {
struct AudioDeviceTestController::Impl {
    std::shared_ptr<OpenMeeting::QtCallbackGate<AudioDeviceTestController>> callbacks;
    bool enumerating = false;
};
AudioDeviceTestController::AudioDeviceTestController(QObject* parent)
    : QObject(parent), _impl(std::make_unique<Impl>()) {
    qRegisterMetaType<AudioDeviceDescriptor>();
    qRegisterMetaType<QVector<AudioDeviceDescriptor>>();
    _impl->callbacks = OpenMeeting::QtCallbackGate<AudioDeviceTestController>::Create(this);
}
AudioDeviceTestController::~AudioDeviceTestController() { _impl->callbacks->Revoke(); }
bool AudioDeviceTestController::isEnumerating() const noexcept { return _impl->enumerating; }
bool AudioDeviceTestController::isMicrophoneTestActive() const noexcept { return false; }
bool AudioDeviceTestController::isSpeakerTestActive() const noexcept { return false; }
void AudioDeviceTestController::refreshDevices() {
    Q_ASSERT(thread() == QThread::currentThread());
    if (_impl->enumerating) return;
    _impl->enumerating = true;
    emit deviceEnumerationStateChanged(true);
    const auto callbacks = _impl->callbacks;
    const bool submitted = OpenMeeting::SessionShutdownService::Instance().Submit([callbacks] {
        QVector<AudioDeviceDescriptor> microphones, speakers;
        bool success = true;
        const auto convert = [](const std::vector<livekit::AudioCaptureDeviceInfo>& devices) {
            QVector<AudioDeviceDescriptor> result;
            for (const auto& device : devices) {
                result.push_back({QString::fromStdString(device.id), QString::fromStdString(device.name),
                                  device.is_default, int(device.default_sample_rate), int(device.default_channels)});
            }
            return result;
        };
        try {
            microphones = convert(livekit::AudioDeviceEnumerator::EnumerateInputDevices());
            speakers = convert(livekit::AudioDeviceEnumerator::EnumerateOutputDevices());
        } catch (...) { success = false; }
        callbacks->Post([microphones, speakers, success](AudioDeviceTestController* self) {
            self->_impl->enumerating = false;
            emit self->deviceEnumerationStateChanged(false);
            if (success) emit self->devicesChanged(microphones, speakers);
            else emit self->deviceEnumerationFailed(QCoreApplication::translate("MeetingUI", "Unable to enumerate audio devices"));
        });
    });
    if (!submitted) {
        _impl->enumerating = false;
        emit deviceEnumerationStateChanged(false);
        emit deviceEnumerationFailed(QCoreApplication::translate("MeetingUI", "Unable to enumerate audio devices"));
    }
}
void AudioDeviceTestController::startMicrophoneTest(const QString&) {
    emit microphoneTestFailed(QCoreApplication::translate("MeetingUI", "Microphone testing is not available on Linux yet."));
}
void AudioDeviceTestController::stopMicrophoneTest() {}
void AudioDeviceTestController::playSpeakerTestTone(const QString&, int) {
    emit speakerTestFinished(false, QCoreApplication::translate("MeetingUI", "Speaker testing is not available on Linux yet."));
}
void AudioDeviceTestController::stopSpeakerTestTone() {}
void AudioDeviceTestController::stopAll() {}
}
