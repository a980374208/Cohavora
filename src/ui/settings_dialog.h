#pragma once

#include "src/net/session_manager.h"

#include <QtCore/QHash>
#include <QtCore/QPoint>
#include <QtCore/QString>
#include <QtCore/QVector>
#include <QtWidgets/QDialog>

class QCheckBox;
class QComboBox;
class QFrame;
class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QStackedWidget;

namespace MeetingUI {

class AudioDeviceTestController;
class CameraPreviewWidget;

class SettingsDialog final : public QDialog {
	Q_OBJECT

public:
	enum class Page {
		General = 0,
		Video,
		Audio,
		About,
	};

	explicit SettingsDialog(
		OpenMeeting::SessionManager &session,
		QWidget *parent = nullptr);
	~SettingsDialog() override;

	OpenMeeting::MediaPreferences preferences() const;
	QString selectedCameraDeviceId() const;
	QString selectedMicrophoneDeviceId() const;
	QString selectedSpeakerDeviceId() const;
	QWidget *videoPreviewHost() const;

	void showPage(Page page);
	void setPreferences(const OpenMeeting::MediaPreferences &preferences);
	void setVideoPreviewWidget(QWidget *previewWidget);

public slots:
	void refreshDevices();
	void setMicrophoneLevel(float normalizedLevel);
	void setMicrophoneTestActive(bool active);
	void setSpeakerTestActive(bool active);
	void setVideoPreviewMessage(const QString &message, bool error = false);

signals:
	void preferencesEdited(const OpenMeeting::MediaPreferences &preferences);
	void cameraSelectionChanged(
		const QString &deviceId,
		int captureWidth,
		int captureHeight,
		int captureFps);
	void cameraPreviewRequested(
		const QString &deviceId,
		int captureWidth,
		int captureHeight,
		int captureFps);
	void cameraPreviewStopped();
	void microphoneSelectionChanged(const QString &deviceId);
	void speakerSelectionChanged(const QString &deviceId);
	void microphoneTestStarted(const QString &deviceId);
	void microphoneTestStopped();
	void speakerTestRequested(const QString &deviceId);

protected:
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;

private:
	struct VideoFormat {
		int width = 0;
		int height = 0;
		int fps = 0;
	};

	void buildUi();
	QWidget *buildGeneralPage();
	QWidget *buildVideoPage();
	QWidget *buildAudioPage();
	QWidget *buildAboutPage();
	void connectPreferenceControls();
	void connectDeviceControllers();
	void refreshVideoDevices(const OpenMeeting::MediaPreferences &preferences);
	void rebuildResolutionChoices(const OpenMeeting::MediaPreferences &preferences);
	int defaultFormatIndex(const QVector<VideoFormat> &formats) const;
	VideoFormat selectedVideoFormat() const;
	void syncJoinPreferenceControls(QCheckBox *source);
	void updateMirrorControls();
	void updateHighDefinitionControls();
	void commitPreferences();
	void emitCameraSelection();
	void requestPreviewIfVisible();

	OpenMeeting::SessionManager &_session;
	QListWidget *_navigation = nullptr;
	QStackedWidget *_pages = nullptr;

	QCheckBox *_generalCamera = nullptr;
	QCheckBox *_generalMicrophone = nullptr;
	QCheckBox *_generalSpeaker = nullptr;
	QCheckBox *_quitOnClose = nullptr;
	QCheckBox *_showActiveSpeaker = nullptr;
	QCheckBox *_stayWhenLocked = nullptr;

	QFrame *_previewHost = nullptr;
	QLabel *_previewMessage = nullptr;
	CameraPreviewWidget *_cameraPreview = nullptr;
	QComboBox *_cameraCombo = nullptr;
	QComboBox *_resolutionCombo = nullptr;
	QCheckBox *_videoCamera = nullptr;
	QCheckBox *_highDefinition = nullptr;
	QCheckBox *_mirrorEnabled = nullptr;
	QRadioButton *_localMirror = nullptr;
	QRadioButton *_remoteMirror = nullptr;

	QComboBox *_speakerCombo = nullptr;
	QComboBox *_microphoneCombo = nullptr;
	QPushButton *_speakerTestButton = nullptr;
	QPushButton *_microphoneTestButton = nullptr;
	QProgressBar *_microphoneLevel = nullptr;
	QLabel *_audioStatus = nullptr;
	QCheckBox *_audioMicrophone = nullptr;
	QCheckBox *_audioSpeaker = nullptr;
	QCheckBox *_pushToTalk = nullptr;
	QCheckBox *_echoCancellation = nullptr;
	QCheckBox *_noiseSuppression = nullptr;
	QCheckBox *_autoGainControl = nullptr;
	AudioDeviceTestController *_audioTestController = nullptr;

	QHash<QString, QVector<VideoFormat>> _cameraFormats;
	bool _usingExternalPreview = false;
	bool _updatingUi = false;
	bool _microphoneTestActive = false;
	bool _speakerTestActive = false;
	bool _dragging = false;
	QPoint _dragOffset;
};

} // namespace MeetingUI
