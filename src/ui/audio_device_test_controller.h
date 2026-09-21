#pragma once

#include <QtCore/QMetaType>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QVector>

#include <memory>

namespace MeetingUI {

struct AudioDeviceDescriptor {
	QString id;
	QString name;
	bool isDefault = false;
	int sampleRate = 0;
	int channelCount = 0;
};

class AudioDeviceTestController final : public QObject {
	Q_OBJECT

public:
	explicit AudioDeviceTestController(QObject *parent = nullptr);
	~AudioDeviceTestController() override;

	bool isEnumerating() const noexcept;
	bool isMicrophoneTestActive() const noexcept;
	bool isSpeakerTestActive() const noexcept;

public slots:
	// Enumeration and all completion signals are delivered on this QObject's thread.
	void refreshDevices();

	// The microphone test owns an independent shared-mode capture stream. It only
	// calculates a normalized input level and never publishes or records audio.
	void startMicrophoneTest(const QString &deviceId);
	void stopMicrophoneTest();

	// Plays a short generated tone directly to the selected endpoint. The duration
	// is bounded to 250..5000 ms so an abandoned settings page cannot play forever.
	void playSpeakerTestTone(const QString &deviceId, int durationMs = 1500);
	void stopSpeakerTestTone();

	void stopAll();

signals:
	void deviceEnumerationStateChanged(bool active);
	void devicesChanged(
		const QVector<MeetingUI::AudioDeviceDescriptor> &microphones,
		const QVector<MeetingUI::AudioDeviceDescriptor> &speakers);
	void deviceEnumerationFailed(const QString &message);

	void microphoneTestStateChanged(bool active);
	void microphoneLevelChanged(float level);
	void microphoneTestFailed(const QString &message);

	void speakerTestStateChanged(bool active);
	void speakerTestFinished(bool success, const QString &message);

private:
	struct Impl;
	std::unique_ptr<Impl> _impl;

	Q_DISABLE_COPY(AudioDeviceTestController)
};

} // namespace MeetingUI

Q_DECLARE_METATYPE(MeetingUI::AudioDeviceDescriptor)
Q_DECLARE_METATYPE(QVector<MeetingUI::AudioDeviceDescriptor>)
