#pragma once

#include "src/net/session_manager.h"
#include "src/ui/camera_device_discovery.h"

#include <QtCore/QHash>
#include <QtCore/QPoint>
#include <QtCore/QString>
#include <QtCore/QVector>
#include <QtWidgets/QDialog>

class QCheckBox;
class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QStackedWidget;

namespace OpenMeeting { template <typename T> class QtCallbackGate; }

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
		Security,
		About,
	};

	explicit SettingsDialog(
		OpenMeeting::SessionManager &session,
		QWidget *parent = nullptr,
		CameraDeviceDiscovery::Clock::time_point clickedAt = CameraDeviceDiscovery::Clock::now());
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
	void done(int result) override;
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
	bool eventFilter(QObject *watched, QEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;

private:
	friend class SettingsDialogTestAccess;
	struct VideoFormat {
		int width = 0;
		int height = 0;
		int fps = 0;
	};

	void buildUi();
	QWidget *buildGeneralPage();
	QWidget *buildVideoPage();
	QWidget *buildAudioPage();
	QWidget *buildSecurityPage();
	QWidget *buildAboutPage();
	void syncSecurityControls();
	void commitSecurityPreferences();
	void updateSecurityKeyControls();
	void saveSecurityKey();
	void clearSecurityEditor();
	void connectPreferenceControls();
	void connectDeviceControllers();
	void prepareDevicePlaceholders();
	void startDeviceDiscovery(bool forceRefresh);
	void applyVideoDevices(const CameraDeviceDiscovery::Devices &devices);
	void cancelDeviceDiscovery();
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
	CameraDeviceDiscovery *_cameraDiscovery = &CameraDeviceDiscovery::Instance();
	CameraDeviceDiscovery::Request _cameraRequest;
	std::shared_ptr<OpenMeeting::QtCallbackGate<SettingsDialog>> _cameraCallbacks;
	CameraDeviceDiscovery::Clock::time_point _clickedAt;
	bool _firstPaintScheduled = false;
	bool _firstPaintReported = false;
	bool _forceDeviceRefresh = false;
	bool _videoDevicesReady = false;
	bool _closed = false;
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
	QComboBox *_cameraCodecCombo = nullptr;
	QComboBox *_screenShareCodecCombo = nullptr;
	QComboBox *_screenShareResolutionCombo = nullptr;
	QComboBox *_screenShareFpsCombo = nullptr;
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

	QCheckBox *_quickE2ee = nullptr;
	QCheckBox *_screenShareE2ee = nullptr;
	QCheckBox *_meetingDetailsE2ee = nullptr;
	QCheckBox *_allE2ee = nullptr;
	QLineEdit *_e2eeKey = nullptr;
	QPushButton *_e2eeSaveKey = nullptr;
	QPushButton *_e2eeClearKey = nullptr;
	QLabel *_e2eeStatus = nullptr;

	QHash<QString, QVector<VideoFormat>> _cameraFormats;
	bool _usingExternalPreview = false;
	bool _updatingUi = false;
	bool _microphoneTestActive = false;
	bool _speakerTestActive = false;
	bool _dragging = false;
	QPoint _dragOffset;
};

} // namespace MeetingUI
