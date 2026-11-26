#include "src/ui/meeting_room_window.h"
#include "src/ui/detail/meeting_room_helpers.h"
#include "src/ui/app_theme.h"
#include "src/ui/meeting_log_console.h"
#include "src/rtc/webrtc_manager.h"
#include "src/telemetry/diagnostic_pipeline.h"
#include "src/telemetry/session_telemetry.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QSignalBlocker>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>

#include <algorithm>
#include <atomic>
#include <memory>

namespace MeetingUI {
using detail::ShowMeetingWarning;

// Only the native worker touches these resources until Qt accepts completion.
// Cancellation is cooperative: an in-flight driver call finishes on its owner
// thread, then queued cleanup releases everything without blocking the window.
struct MeetingRoomWindow::MediaPreparation {
	std::atomic_bool cancelled{false};
	std::atomic_bool microphoneWanted{false};
	std::atomic_bool cameraWanted{false};
	bool attemptedMicrophone = false;
	bool attemptedCamera = false;
	std::shared_ptr<livekit::MicrophoneCapture> microphone;
	std::shared_ptr<livekit::CameraSourceManager> camera;
	bool cameraAvailable = false;
	livekit::CameraStartResult cameraStartResult = livekit::CameraStartResult::InitFailed;
	QString cameraPath;
};

void MeetingRoomWindow::prepareMediaAndJoin(std::function<void()> admission) {
	Q_ASSERT(thread() == QThread::currentThread());
	if (_mediaPreparationStarted || !_sessionRunning || _closeRequested ||
		_closingForSessionInvalidation || !admission) return;
	_mediaPreparationStarted = true;
	setMediaPreparationError({});
	_pendingAdmission = std::move(admission);
	_pendingAudioMuted = _config.audioMuted;
	_pendingVideoEnabled = _config.videoEnabled;
	beginMediaPreparation(!_config.audioMuted, _config.videoEnabled);
}

void MeetingRoomWindow::beginMediaPreparation(bool microphone, bool camera) {
	Q_ASSERT(thread() == QThread::currentThread());
	if (_mediaPreparation || !_sessionRunning || _closeRequested ||
		_closingForSessionInvalidation) return;
	_preparingMedia = true;
	_mediaPreparation = std::make_shared<MediaPreparation>();
	_mediaPreparation->microphoneWanted.store(microphone);
	_mediaPreparation->cameraWanted.store(camera);
	_preparationCallbacks = OpenMeeting::QtCallbackGate<MeetingRoomWindow>::Create(this);
	updateMediaPreparationFeedback();
	updateRecoveryStateUi(OpenMeeting::MeetingState::Idle, {});
	const auto preparation = _mediaPreparation;
	const auto callbacks = _preparationCallbacks;
	const auto preferences = _cameraSessionManager
		? _cameraSessionManager->mediaPreferences()
		: OpenMeeting::SessionManager::instance().mediaPreferences();
	const auto audioSource = _localAudioSource;
	const auto videoSource = _localVideoSource;
	const bool prepareMicrophone = microphone && !_wasapiCap;
	const bool prepareCamera = camera && !_cameraManager;
	const auto preparationOverride = _mediaPreparationOverride;
	// This application-owned MTA queue also participates in shutdown draining.
	// WASAPI initializes on its capture thread; DirectShow uses its graph owner.
	// No job reads a QWidget, QPointer or SessionManager.
	const bool accepted = OpenMeeting::SessionShutdownService::Instance().Submit(
		[preparation, callbacks, preferences, audioSource, videoSource,
		 prepareMicrophone, prepareCamera, preparationOverride] {
			if (preparation->cancelled.load()) return;
			if (prepareMicrophone && preparation->microphoneWanted.load()) {
				preparation->attemptedMicrophone = true;
				if (preparationOverride) preparationOverride(true, false);
				else try {
					auto microphone = livekit::MicrophoneCapture::Create();
					preparation->microphone = microphone;
					microphone->EnableApm();
					const auto processor = microphone->apm_processor();
					if (processor) {
						auto config = processor->GetConfig();
						config.enable_aec = preferences.echoCancellation;
						config.enable_ans = preferences.noiseSuppression;
						config.enable_agc = preferences.autoGainControl;
						processor->ApplyConfig(config);
					}
					// Set intent before Start: muted entry must not briefly capture unmuted.
					// Keep preparation silent until the Qt owner commits the request.
					microphone->SetMute(true);
					livekit::MicrophoneCaptureConfig config;
					config.type = livekit::AudioCaptureType::Microphone;
					config.device_id = preferences.microphoneDeviceId.toStdString();
					config.target_sample_rate = 48000;
					config.target_channels = 2;
					if (!preparation->cancelled.load() && microphone->Init(config, audioSource)) {
						if (!preparation->cancelled.load()) microphone->Start();
					}
				} catch (...) {
					// Device absence/failure degrades that medium, not admission.
				}
			}
			if (preparation->cancelled.load()) return;
			if (prepareCamera && preparation->cameraWanted.load()) {
				preparation->attemptedCamera = true;
				if (preparationOverride) preparationOverride(false, true);
				else try {
					// One snapshot supplies default selection, saved selection and capabilities.
					const auto devices = livekit::CameraEnumerator::EnumerateVideoDevices();
					if (preparation->cancelled.load()) return;
					auto selected = std::find_if(devices.begin(), devices.end(),
						[](const livekit::CameraDeviceInfo &device) { return device.is_default; });
					if (selected == devices.end()) selected = devices.begin();
					if (!preferences.cameraDeviceId.isEmpty()) {
						const auto id = preferences.cameraDeviceId.toStdString();
						const auto saved = std::find_if(devices.begin(), devices.end(),
							[&](const livekit::CameraDeviceInfo &device) {
								return device.path == id || device.name == id;
							});
						if (saved != devices.end()) selected = saved;
					}
					if (selected != devices.end() && !selected->path.empty()) {
						const auto resolutions = livekit::CameraSourceManager::GetSupportedResolutions(*selected);
						const auto requested = std::find_if(resolutions.begin(), resolutions.end(),
							[&](const livekit::CameraResolution &resolution) {
								return resolution.width == preferences.videoCaptureWidth &&
									resolution.height == preferences.videoCaptureHeight;
							});
						const auto resolution = requested != resolutions.end()
							? std::optional<livekit::CameraResolution>(*requested)
							: livekit::CameraSourceManager::SelectDefaultResolution(resolutions);
						livekit::CameraCaptureConfig config;
						config.device_path = selected->path;
						config.output_format = livekit::VideoBufferType::NV12;
						if (resolution) {
							config.width = resolution->width;
							config.height = resolution->height;
							const int fps = requested != resolutions.end() ? preferences.videoCaptureFps : 30;
							config.fps = resolution->max_fps > 0 ? (std::min)(fps, resolution->max_fps) : fps;
						}
						if (preparation->cancelled.load()) return;
						preparation->camera = livekit::CameraSourceManager::Create(videoSource);
						preparation->cameraStartResult = preparation->camera->StartAndWaitForFirstFrame(config);
						preparation->cameraAvailable = preparation->cameraStartResult == livekit::CameraStartResult::Ready;
						if (preparation->cameraAvailable) preparation->cameraPath = QString::fromStdString(selected->path);
					}
				} catch (...) {
					preparation->cameraAvailable = false;
				}
			}
		}, [callbacks](std::exception_ptr failure) {
			callbacks->Post([failure](MeetingRoomWindow *window) {
				if (failure) {
					window->cancelMediaPreparation();
					window->updateRecoveryStateUi(OpenMeeting::MeetingState::Failed,
						QCoreApplication::translate("MeetingUI", "Unable to prepare audio and video devices."));
					return;
				}
				window->finishMediaPreparation();
			});
		});
	if (!accepted) {
		// No worker ran and no native resources exist when admission is rejected.
		_mediaPreparation.reset();
		cancelMediaPreparation();
		updateRecoveryStateUi(OpenMeeting::MeetingState::Failed,
			QCoreApplication::translate("MeetingUI", "Unable to prepare audio and video devices."));
	}
}

void MeetingRoomWindow::finishMediaPreparation() {
	Q_ASSERT(thread() == QThread::currentThread());
	if (!_mediaPreparation || _mediaPreparation->cancelled.load() ||
		!_sessionRunning || _closeRequested || _closingForSessionInvalidation) return;
	const bool requestedMuted = _pendingAudioMuted.value_or(_config.audioMuted);
	const bool requestedVideo = _pendingVideoEnabled.value_or(_config.videoEnabled);
	auto preparation = std::move(_mediaPreparation);
	// Keep WASAPI's hot-plug listener even when its first device start failed.
	const bool newMicrophone = !requestedMuted && bool(preparation->microphone);
	if (newMicrophone) _wasapiCap = std::move(preparation->microphone);
	if (requestedVideo && preparation->camera && preparation->cameraAvailable) {
		_cameraManager = std::move(preparation->camera);
		_usingRealCamera = true;
		_currentCameraPath = preparation->cameraPath;
	}
	if (!_cameraManager) _usingRealCamera = false;
	const bool nextMicrophone = !requestedMuted && !_wasapiCap &&
		!preparation->attemptedMicrophone;
	const bool nextCamera = requestedVideo && !_cameraManager &&
		!preparation->attemptedCamera;
	QString preparationError = _mediaPreparationError;
	if (!requestedMuted && preparation->attemptedMicrophone &&
		(!_wasapiCap || !_wasapiCap->IsRunning())) {
		preparationError = QCoreApplication::translate("MeetingUI", "Unable to start the microphone. Check the device and system permissions.");
	}
	if (requestedVideo && preparation->attemptedCamera && !_usingRealCamera) {
		if (!preparationError.isEmpty()) preparationError += QLatin1Char('\n');
		preparationError += preparation->cameraStartResult == livekit::CameraStartResult::FirstFrameTimeout
			? QCoreApplication::translate("MeetingUI", "The camera did not produce video within 3 seconds. Check the device and enable the camera to retry.")
			: QCoreApplication::translate("MeetingUI", "Unable to start the camera. Check the device and system permissions.");
	}
	if (preparationError != _mediaPreparationError) setMediaPreparationError(preparationError);
	// Failed cameras and cancelled devices are retired on the cleanup worker.
	if (preparation->microphone || preparation->camera) {
		OpenMeeting::SessionShutdownService::Instance().SubmitCleanup([preparation] {
			if (preparation->microphone) preparation->microphone->Stop();
			if (preparation->camera) preparation->camera->Stop();
		});
	}
	_preparingMedia = false;
	if (newMicrophone) {
		bindMicrophoneCaptureState();
		livekit::WebRTCManager::Instance().SetApmProcessor(_wasapiCap->apm_processor());
	}
	if (_cameraSessionManager) setupAudioPreferencesBinding(*_cameraSessionManager);
	// Availability projection may synchronously emit the previous mute state.
	// Retain the user's current intent before changing coordinator availability.
	const bool audioMuted = requestedMuted || !_wasapiCap || !_wasapiCap->IsRunning();
	const bool videoEnabled = requestedVideo && _usingRealCamera;
	applyMicrophoneAvailability(_wasapiCap && _wasapiCap->IsRunning());
	_config.audioMuted = audioMuted;
	_config.videoEnabled = videoEnabled;
	if (_coordinator) {
		_coordinator->setLocalVideoAvailable(_usingRealCamera);
		_coordinator->setLocalAudioMuted(audioMuted);
		_coordinator->setLocalVideoEnabled(videoEnabled);
	}
	if (_wasapiCap) queueMicrophoneMute(audioMuted);
	if (_bottomBar) {
		_bottomBar->setAudioMuted(_config.audioMuted);
		_bottomBar->setVideoEnabled(_config.videoEnabled);
	}
	if (_localTile) {
		_localTile->setVideoActive(_config.videoEnabled && _usingRealCamera);
		_localTile->setAudioMuted(_config.audioMuted);
	}
	LogToConsole(LogCategory::Media, "CAPTURE",
		QStringLiteral("devices_prepared microphone=%1 camera=%2").arg(_microphoneAvailable).arg(_usingRealCamera));
	if (nextMicrophone || nextCamera) {
		// Only unattempted work is carried forward. A failed device requires a
		// new explicit request; the worker never retries it in a loop.
		_pendingAudioMuted = nextMicrophone ? std::optional<bool>(false) : std::nullopt;
		_pendingVideoEnabled = nextCamera ? std::optional<bool>(true) : std::nullopt;
		beginMediaPreparation(nextMicrophone, nextCamera);
		return;
	}
	_pendingAudioMuted.reset();
	_pendingVideoEnabled.reset();
	updateMediaPreparationFeedback();
	updateRecoveryStateUi(_pendingAdmission ? OpenMeeting::MeetingState::ConnectingRoom :
		(_coordinator ? _coordinator->state() : OpenMeeting::MeetingState::InMeeting), {});
	auto admission = std::move(_pendingAdmission);
	if (admission) admission();
}

void MeetingRoomWindow::requestLocalMicrophone(bool muted) {
	if (!_sessionRunning || _closeRequested || _closingForSessionInvalidation) return;
	setMediaPreparationError({});
	if (_mediaPreparation) {
		_pendingAudioMuted = muted;
		_mediaPreparation->microphoneWanted.store(!muted);
		if (muted && _coordinator) _coordinator->setLocalAudioMuted(true);
		updateMediaPreparationFeedback();
		return;
	}
	if (!muted && !_wasapiCap) {
		_pendingAudioMuted = false;
		beginMediaPreparation(true, false);
		return;
	}
#ifdef Q_OS_LINUX
    if (!muted) {
        queueMicrophoneMute(false, true);
        return;
    }
#endif
	if (!muted && !_wasapiCap->IsRunning()) {
		setMediaPreparationError(QCoreApplication::translate("MeetingUI", "Unable to start the microphone. Check the device and system permissions."));
	}
	updateMediaPreparationFeedback();
	updateRecoveryStateUi(_coordinator ? _coordinator->state() : OpenMeeting::MeetingState::InMeeting, {});
	if (_coordinator) _coordinator->setLocalAudioMuted(muted);
}

void MeetingRoomWindow::requestLocalCamera(bool enabled) {
	if (!_sessionRunning || _closeRequested || _closingForSessionInvalidation) return;
	setMediaPreparationError({});
	if (_mediaPreparation) {
		_pendingVideoEnabled = enabled;
		_mediaPreparation->cameraWanted.store(enabled);
		if (!enabled && _coordinator) _coordinator->setLocalVideoEnabled(false);
		updateMediaPreparationFeedback();
		return;
	}
	if (enabled && !_cameraManager) {
		_pendingVideoEnabled = true;
		beginMediaPreparation(false, true);
		return;
	}
	updateMediaPreparationFeedback();
	updateRecoveryStateUi(_coordinator ? _coordinator->state() : OpenMeeting::MeetingState::InMeeting, {});
	if (_coordinator) _coordinator->setLocalVideoEnabled(enabled);
}

void MeetingRoomWindow::setMediaPreparationError(const QString &error) {
	_mediaPreparationError = error;
	if (_mediaPreparationErrorTimer) _mediaPreparationErrorTimer->stop();
	if (!error.isEmpty()) {
		if (!_mediaPreparationErrorTimer) {
			_mediaPreparationErrorTimer = new QTimer(this);
			_mediaPreparationErrorTimer->setSingleShot(true);
			connect(_mediaPreparationErrorTimer, &QTimer::timeout, this, [this] {
				// Retire the shared feedback state so a later UI refresh cannot
				// resurrect the notice. Leave connection/recovery banners intact.
				if (_recoveryBanner && _recoveryBanner->text() == _mediaPreparationError) {
					_recoveryBanner->hide();
				}
				_mediaPreparationError.clear();
				updateMediaPreparationFeedback();
			});
		}
		_mediaPreparationErrorTimer->start(5000);
	}
	updateMediaPreparationFeedback();
}

void MeetingRoomWindow::updateMediaPreparationFeedback() {
	if (_participantsSidebar) {
		_participantsSidebar->setLocalMediaPreparation(
			_pendingAudioMuted, _pendingVideoEnabled, _mediaPreparationError);
	}
}

void MeetingRoomWindow::cancelMediaPreparation() {
	Q_ASSERT(thread() == QThread::currentThread());
	_pendingAdmission = {};
	_preparingMedia = false;
	_pendingAudioMuted.reset();
	_pendingVideoEnabled.reset();
	updateMediaPreparationFeedback();
	if (_preparationCallbacks) _preparationCallbacks->Revoke();
	if (auto preparation = std::move(_mediaPreparation)) {
		preparation->cancelled.store(true);
		// FIFO with preparation: never destroy a device while Start is using it.
		OpenMeeting::SessionShutdownService::Instance().SubmitCleanup(
			[preparation = std::move(preparation)]() mutable {
				if (preparation->microphone) preparation->microphone->Stop();
				if (preparation->camera) preparation->camera->Stop();
				preparation.reset();
			});
	}
}

void MeetingRoomWindow::applyAudioProcessingPreferences(
		const OpenMeeting::MediaPreferences &preferences) {
	if (!_wasapiCap) return;
	const auto processor = _wasapiCap->apm_processor();
	if (!processor) return;
	auto config = processor->GetConfig();
	if (config.enable_aec == preferences.echoCancellation
		&& config.enable_ans == preferences.noiseSuppression
		&& config.enable_agc == preferences.autoGainControl) return;
	config.enable_aec = preferences.echoCancellation;
	config.enable_ans = preferences.noiseSuppression;
	config.enable_agc = preferences.autoGainControl;
	// Keep the capture and render-reference paths on the same APM instance.
	// ApplyConfig serializes changes with audio callbacks internally.
	processor->ApplyConfig(config);
}

std::uint64_t MeetingRoomWindow::beginDeviceSwitchTelemetry(
		DeviceSwitchTelemetry &operation,
		livekit::telemetry::OperationKind kind) {
	if (!operation.operationId.empty()) {
		finishDeviceSwitchTelemetry(
			operation, operation.serial,
			livekit::telemetry::OperationOutcome::Cancelled);
	}
	++operation.serial;
	operation.kind = kind;
	operation.telemetry = _coordinator
		? _coordinator->sessionTelemetry()
		: std::weak_ptr<livekit::telemetry::SessionTelemetry>{};
	if (const auto telemetry = operation.telemetry.lock()) {
		operation.operationId = telemetry->StartOperation(kind);
	}
	return operation.serial;
}

void MeetingRoomWindow::finishDeviceSwitchTelemetry(
		DeviceSwitchTelemetry &operation,
		std::uint64_t serial,
		livekit::telemetry::OperationOutcome outcome) {
	if (serial == 0 || operation.serial != serial) return;
	if (const auto telemetry = operation.telemetry.lock();
		telemetry && !operation.operationId.empty()) {
		telemetry->FinishOperation(operation.operationId, operation.kind, outcome);
	}
	operation.operationId.clear();
	operation.telemetry.reset();
}

void MeetingRoomWindow::cancelDeviceSwitchTelemetry() {
	finishDeviceSwitchTelemetry(
		_cameraSwitchTelemetry, _cameraSwitchTelemetry.serial,
		livekit::telemetry::OperationOutcome::Cancelled);
	finishDeviceSwitchTelemetry(
		_microphoneSwitchTelemetry, _microphoneSwitchTelemetry.serial,
		livekit::telemetry::OperationOutcome::Cancelled);
	finishDeviceSwitchTelemetry(
		_speakerSwitchTelemetry, _speakerSwitchTelemetry.serial,
		livekit::telemetry::OperationOutcome::Cancelled);
	_pendingMicrophoneDeviceGeneration = 0;
}

#ifdef Q_OS_LINUX
void MeetingRoomWindow::requestSpeakerDevice(const QString &deviceId,
		bool fallbackToDefault, std::function<void(bool)> completed) {
	if (!_room || !_sessionRunning || _closeRequested || _closingForSessionInvalidation) return;
	if (!_captureUiCallbacks) {
		_captureUiCallbacks = OpenMeeting::QtCallbackGate<MeetingRoomWindow>::Create(this);
	}
	const auto callbacks = _captureUiCallbacks;
	const auto room = _room;
	const auto intent = _speakerDeviceIntent;
	const auto serial = ++*intent;
	++*_speakerMuteIntent;
	const auto operation = beginDeviceSwitchTelemetry(
		_speakerSwitchTelemetry,
		livekit::telemetry::OperationKind::SpeakerDeviceSwitch);
	_speakerDevicePending = true;
	const auto finish = [weakRoom = std::weak_ptr<livekit::Room>(room), intent, serial, operation,
		completed = std::move(completed)](MeetingRoomWindow *window,
			bool selected, bool available, const QString &selectedId) {
		if (intent->load() != serial || !window->_room || window->_room != weakRoom.lock() || !window->_sessionRunning ||
			window->_closeRequested || window->_closingForSessionInvalidation) return;
		window->_speakerDevicePending = false;
		window->_speakerAvailable = available;
		if (selected && window->_bottomBar) window->_bottomBar->setSpeakerDeviceId(selectedId);
		window->applyEffectiveSpeakerMute();
		window->finishDeviceSwitchTelemetry(window->_speakerSwitchTelemetry, operation,
			selected && available ? livekit::telemetry::OperationOutcome::Success
				: livekit::telemetry::OperationOutcome::Failure);
		if (!selected || !available) {
			LogToConsole(LogCategory::Error, "AUDIO_OUTPUT", "Unable to select speaker output device");
		}
		if (completed) completed(selected && available);
	};
	const bool accepted = OpenMeeting::SessionShutdownService::Instance().Submit(
		[callbacks, intent, serial, room, deviceId, fallbackToDefault, finish] {
			if (intent->load() != serial || !callbacks->active()) return;
			auto &manager = livekit::WebRTCManager::Instance();
			bool selected = false;
			bool available = false;
			QString selectedId = deviceId;
			try {
				selected = manager.SetPlayoutDeviceById(deviceId.toStdString());
				if (intent->load() != serial || !callbacks->active()) return;
				if (!selected && fallbackToDefault && !deviceId.isEmpty()) {
					selected = manager.SetPlayoutDeviceById({});
					selectedId.clear();
				}
				if (intent->load() != serial || !callbacks->active()) return;
				available = manager.EnsurePlayout();
			} catch (...) {
				selected = false;
			}
			callbacks->Post([finish, selected, available, selectedId](MeetingRoomWindow *window) {
				finish(window, selected, available, selectedId);
			});
		});
	if (!accepted) {
		callbacks->Post([finish, deviceId](MeetingRoomWindow *window) {
			finish(window, false, false, deviceId);
		});
	}
}
#else
bool MeetingRoomWindow::selectSpeakerDevice(const QString &deviceId) {
	const auto operation = beginDeviceSwitchTelemetry(
		_speakerSwitchTelemetry,
		livekit::telemetry::OperationKind::SpeakerDeviceSwitch);
	auto &manager = livekit::WebRTCManager::Instance();
	const bool selected = manager.SetPlayoutDeviceById(deviceId.toStdString());
	_speakerAvailable = manager.EnsurePlayout();
	applyEffectiveSpeakerMute();
	if (!selected || !_speakerAvailable) {
		finishDeviceSwitchTelemetry(
			_speakerSwitchTelemetry, operation,
			livekit::telemetry::OperationOutcome::Failure);
		LogToConsole(LogCategory::Error, "AUDIO_OUTPUT", "Unable to select speaker output device");
		return false;
	}
	if (_bottomBar) _bottomBar->setSpeakerDeviceId(deviceId);
	finishDeviceSwitchTelemetry(
		_speakerSwitchTelemetry, operation,
		livekit::telemetry::OperationOutcome::Success);
	return true;
}
#endif

void MeetingRoomWindow::queueMicrophoneMute(bool muted, bool commitRequestedState) {
#ifdef Q_OS_LINUX
    if (_microphoneStateCommit || !_wasapiCap || !_sessionRunning ||
        _closeRequested || _closingForSessionInvalidation) return;
    const auto capture = _wasapiCap;
    if (!commitRequestedState && !_microphoneMutePending &&
        capture->IsMuted() == muted) return;
    if (!_captureUiCallbacks) {
        _captureUiCallbacks = OpenMeeting::QtCallbackGate<MeetingRoomWindow>::Create(this);
    }
    const auto callbacks = _captureUiCallbacks;
    const auto intent = _microphoneMuteIntent;
    const auto serial = ++*intent;
    _microphoneMutePending = true;
    const bool accepted = OpenMeeting::SessionShutdownService::Instance().Submit(
        [capture, callbacks, intent, serial, muted, commitRequestedState] {
            if (intent->load() != serial || !callbacks->active()) return;
            capture->SetMute(muted);
            const bool available = capture->IsRunning();
            callbacks->Post([capture, intent, serial, muted, commitRequestedState, available](MeetingRoomWindow* window) {
                if (intent->load() != serial || window->_wasapiCap != capture ||
                    !window->_sessionRunning || window->_closeRequested || window->_closingForSessionInvalidation) return;
                const QPointer<MeetingRoomWindow> guard(window);
                window->_microphoneMutePending = false;
                window->_microphoneStateCommit = true;
                const bool currentAvailable = available && capture->IsRunning();
                window->applyMicrophoneAvailability(currentAvailable);
                if (!guard) return;
                if (commitRequestedState && window->_coordinator) {
                    window->_coordinator->setLocalAudioMuted(muted || !currentAvailable);
                    if (!guard) return;
                }
                window->_microphoneStateCommit = false;
                if (!muted && !currentAvailable) {
                    window->setMediaPreparationError(QCoreApplication::translate("MeetingUI", "Unable to start the microphone. Check the device and system permissions."));
                }
                window->updateMediaPreparationFeedback();
                if (!guard) return;
                // Admission/host policy may still require mute even after the
                // native device opened. Apply that final intent on the worker.
                if (window->_coordinator && window->_coordinator->isLocalAudioMuted() != capture->IsMuted()) {
                    window->queueMicrophoneMute(window->_coordinator->isLocalAudioMuted());
                }
            });
        });
    if (!accepted) {
        _microphoneMutePending = false;
        applyMicrophoneAvailability(false);
    }
#else
    if (_wasapiCap) _wasapiCap->SetMute(muted);
    if (commitRequestedState && _coordinator) _coordinator->setLocalAudioMuted(muted);
#endif
}

void MeetingRoomWindow::requestMicrophoneSwitch(const QString &deviceId) {
	const auto operation = beginDeviceSwitchTelemetry(
		_microphoneSwitchTelemetry,
		livekit::telemetry::OperationKind::MicrophoneDeviceSwitch);
	if (!_wasapiCap) {
		finishDeviceSwitchTelemetry(
			_microphoneSwitchTelemetry, operation,
			livekit::telemetry::OperationOutcome::Failure);
		return;
	}
#ifdef Q_OS_LINUX
    const auto capture = _wasapiCap;
    const auto callbacks = _captureUiCallbacks;
    const auto intent = _microphoneSwitchIntent;
    const auto serial = ++*intent;
    _microphoneSwitchPending = true;
    const bool accepted = OpenMeeting::SessionShutdownService::Instance().Submit(
        [capture, callbacks, intent, serial, operation, id = deviceId.toStdString()] {
            if (intent->load() != serial || !callbacks || !callbacks->active()) return;
            std::uint64_t generation = 0;
            try { generation = capture->SwitchDeviceTracked(id); } catch (...) {}
            const bool available = capture->IsRunning();
            callbacks->Post([capture, intent, serial, operation, generation, available](MeetingRoomWindow* window) {
                if (intent->load() != serial || window->_wasapiCap != capture || !window->_sessionRunning ||
                    window->_closeRequested || window->_closingForSessionInvalidation) return;
                const QPointer<MeetingRoomWindow> guard(window);
                window->_microphoneSwitchPending = false;
                if (!window->_microphoneMutePending) window->applyMicrophoneAvailability(available && capture->IsRunning());
                if (!guard) return;
                window->finishDeviceSwitchTelemetry(window->_microphoneSwitchTelemetry, operation,
                    generation != 0 ? livekit::telemetry::OperationOutcome::Success : livekit::telemetry::OperationOutcome::Failure);
                if (generation == 0) window->setMediaPreparationError(
                    QCoreApplication::translate("MeetingUI", "Unable to switch the microphone. Check the selected device and system permissions."));
            });
        });
    if (!accepted) {
        _microphoneSwitchPending = false;
        finishDeviceSwitchTelemetry(_microphoneSwitchTelemetry, operation, livekit::telemetry::OperationOutcome::Failure);
    }
    return;
#else
	const auto generation = _wasapiCap->SwitchDeviceTracked(deviceId.toStdString());
	if (generation == 0) {
		finishDeviceSwitchTelemetry(
			_microphoneSwitchTelemetry, operation,
			livekit::telemetry::OperationOutcome::Failure);
		return;
	}
	_pendingMicrophoneDeviceGeneration = generation;
	if (_wasapiCap->IsRunning() &&
		_wasapiCap->activeDeviceGeneration() == generation) {
		finishDeviceSwitchTelemetry(
			_microphoneSwitchTelemetry, operation,
			livekit::telemetry::OperationOutcome::Success);
		_pendingMicrophoneDeviceGeneration = 0;
		return;
	}
	QPointer<MeetingRoomWindow> owner(this);
	QTimer::singleShot(5000, this, [owner, operation, generation] {
		if (!owner || owner->_pendingMicrophoneDeviceGeneration != generation) return;
		owner->_pendingMicrophoneDeviceGeneration = 0;
		owner->finishDeviceSwitchTelemetry(
			owner->_microphoneSwitchTelemetry, operation,
			livekit::telemetry::OperationOutcome::Timeout);
	});
#endif
}

void MeetingRoomWindow::applyEffectiveSpeakerMute() {
	const bool effectiveMuted = _speakerRequestedMuted || !_speakerAvailable;
	if (_bottomBar) _bottomBar->setSpeakerMuted(effectiveMuted);
	if (_room) _room->SetAudioOutputMuted(effectiveMuted);
}

void MeetingRoomWindow::setSpeakerOutputMuted(bool muted) {
	_speakerRequestedMuted = muted;
	_speakerStateInitialized = true;
#ifdef Q_OS_LINUX
	const auto intent = _speakerMuteIntent;
	const auto serial = ++*intent;
	// Muting is immediate; an older native readiness completion cannot unmute.
	if (_bottomBar) _bottomBar->setSpeakerMuted(true);
	if (_room) _room->SetAudioOutputMuted(true);
	if (muted || _speakerDevicePending || !_room || !_sessionRunning ||
		_closeRequested || _closingForSessionInvalidation) return;
	if (!_captureUiCallbacks) {
		_captureUiCallbacks = OpenMeeting::QtCallbackGate<MeetingRoomWindow>::Create(this);
	}
	const auto callbacks = _captureUiCallbacks;
	const auto room = _room;
	const bool accepted = OpenMeeting::SessionShutdownService::Instance().Submit(
		[callbacks, intent, serial, room] {
			if (intent->load() != serial || !callbacks->active()) return;
			bool available = false;
			try { available = livekit::WebRTCManager::Instance().EnsurePlayout(); } catch (...) {}
			callbacks->Post([intent, serial, weakRoom = std::weak_ptr<livekit::Room>(room), available](MeetingRoomWindow *window) {
				if (intent->load() != serial || !window->_room || window->_room != weakRoom.lock() || !window->_sessionRunning ||
					window->_closeRequested || window->_closingForSessionInvalidation) return;
				window->_speakerAvailable = available;
				window->applyEffectiveSpeakerMute();
			});
		});
	if (!accepted) _speakerAvailable = false;
#else
	if (!muted) {
		_speakerAvailable = livekit::WebRTCManager::Instance().EnsurePlayout();
	}
	applyEffectiveSpeakerMute();
#endif
}

void MeetingRoomWindow::applyMicrophoneAvailability(bool available) {
	_microphoneAvailable = available;
	if (_coordinator) _coordinator->setLocalAudioAvailable(available);
}

void MeetingRoomWindow::bindMicrophoneCaptureState() {
	const std::weak_ptr<livekit::MicrophoneCapture> capture = _wasapiCap;
	if (!_captureUiCallbacks) {
		_captureUiCallbacks = OpenMeeting::QtCallbackGate<MeetingRoomWindow>::Create(this);
	}
	const auto callbacks = _captureUiCallbacks;
	_wasapiCap->SetCaptureStateCallback([callbacks, capture](bool available) {
		callbacks->Post([capture, available](MeetingRoomWindow *window) mutable {
			if (!window->_sessionRunning || window->_closingForSessionInvalidation) return;
			const auto source = capture.lock();
			if (!source || window->_wasapiCap != source) return;
#ifdef Q_OS_LINUX
            if (window->_microphoneMutePending || window->_microphoneSwitchPending) return;
            // Native notifications can queue across a later command. Project
            // the latest atomic snapshot, never resurrect the older event.
            available = source->IsRunning();
#endif
			window->applyMicrophoneAvailability(available);
		});
	});
	_wasapiCap->SetDeviceFrameCallback([callbacks, capture](std::uint64_t generation) {
		callbacks->Post([capture, generation](MeetingRoomWindow *window) {
			if (!window->_sessionRunning ||
				window->_closingForSessionInvalidation) return;
			const auto source = capture.lock();
			if (!source || window->_wasapiCap != source ||
				window->_pendingMicrophoneDeviceGeneration != generation) return;
			window->_pendingMicrophoneDeviceGeneration = 0;
			window->finishDeviceSwitchTelemetry(
				window->_microphoneSwitchTelemetry,
				window->_microphoneSwitchTelemetry.serial,
				livekit::telemetry::OperationOutcome::Success);
		});
	});
}

void MeetingRoomWindow::setupAudioPreferencesBinding(
		OpenMeeting::SessionManager &sessionManager) {
	applyAudioProcessingPreferences(sessionManager.mediaPreferences());
	scheduleViewportIntent(true);
	if (_audioPreferencesBound) return;
	_audioPreferencesBound = true;
	if (_bottomBar) {
		_bottomBar->setSpeakerDeviceId(sessionManager.mediaPreferences().speakerDeviceId);
		_bottomBar->setMicrophoneDeviceId(sessionManager.mediaPreferences().microphoneDeviceId);
	}
	connect(&sessionManager, &OpenMeeting::SessionManager::preferencesChanged, this,
		[this, session = QPointer<OpenMeeting::SessionManager>(&sessionManager),
			previousMicrophone = sessionManager.mediaPreferences().microphoneDeviceId,
			previousSpeaker = sessionManager.mediaPreferences().speakerDeviceId,
			confirmedSpeaker = std::make_shared<QString>(sessionManager.mediaPreferences().speakerDeviceId)]
		(const OpenMeeting::MediaPreferences &preferences) mutable {
			// Submit before device-switch branches, which may return on failure.
			// Unchanged video preferences are filtered by viewport deduplication.
			scheduleViewportIntent(true);
			applyAudioProcessingPreferences(preferences);
			if (previousMicrophone != preferences.microphoneDeviceId) {
				previousMicrophone = preferences.microphoneDeviceId;
				if (_bottomBar) _bottomBar->setMicrophoneDeviceId(previousMicrophone);
				requestMicrophoneSwitch(previousMicrophone);
			}
			if (previousSpeaker != preferences.speakerDeviceId) {
				if (_room && _coordinator
					&& _coordinator->state() == OpenMeeting::MeetingState::InMeeting) {
#ifdef Q_OS_LINUX
					requestSpeakerDevice(preferences.speakerDeviceId, false,
						[session, requested = preferences.speakerDeviceId, confirmedSpeaker](bool success) {
							if (!session || session->mediaPreferences().speakerDeviceId != requested) return;
							if (success) {
									*confirmedSpeaker = requested;
								return;
							}
							auto restored = session->mediaPreferences();
							restored.speakerDeviceId = *confirmedSpeaker;
							session->setMediaPreferences(restored);
						});
					previousSpeaker = preferences.speakerDeviceId;
					return;
#else
					if (!selectSpeakerDevice(preferences.speakerDeviceId)) {
						// Restore the previous choice after the current settings commit
						// finishes; never overwrite a newer user selection.
						QMetaObject::invokeMethod(this,
							[session, requested = preferences.speakerDeviceId, previousSpeaker] {
								if (!session || session->mediaPreferences().speakerDeviceId != requested) return;
								auto restored = session->mediaPreferences();
								restored.speakerDeviceId = previousSpeaker;
								session->setMediaPreferences(restored);
							}, Qt::QueuedConnection);
						return;
					}
#endif
				}
				previousSpeaker = preferences.speakerDeviceId;
				*confirmedSpeaker = preferences.speakerDeviceId;
			}
		});
}

void MeetingRoomWindow::applyParticipantNameDisplayPreferences(
		const OpenMeeting::MediaPreferences &preferences) {
	const auto apply = [mode = preferences.participantNameDisplay](VideoTileWidget *tile) {
		if (tile) tile->setNameDisplayMode(mode);
	};
	apply(_localTile);
	apply(_localScreenTile.get());
	for (const auto &[identity, tile] : _remoteTiles) apply(tile.get());
	for (const auto &tile : _remoteTilePool) apply(tile.get());
	for (const auto &[sid, tile] : _remoteScreenTiles) apply(tile.get());
	if (!preferences.showLiveCaptions) {
		clearLiveCaption();
	}
	if (_meetingTranslator) {
		OpenMeeting::MeetingTranslatorConfig cfg;
		cfg.translateEnabled = preferences.enableTranslationOutput;
		cfg.targetLanguage = preferences.translationTargetLanguage;
		cfg.translationProfileId = preferences.translationProfileId;
		cfg.translationModelOverride = preferences.translationModelOverride;
		cfg.translationSystemPrompt = preferences.translationSystemPrompt;
		cfg.includeLocalMic = preferences.translateLocalMicrophone;
		cfg.maxConcurrentStreamingAsr = preferences.maxConcurrentStreamingAsr;
		cfg.streamingIdleReleaseMs = preferences.streamingIdleReleaseMs;
		_meetingTranslator->updateConfig(cfg);
	}
}

void MeetingRoomWindow::setupCameraCompletionOwner(
		OpenMeeting::SessionManager &sessionManager) {
	Q_ASSERT(thread() == QThread::currentThread());
	Q_ASSERT(sessionManager.thread() == thread());
	_cameraSessionManager = &sessionManager;
	applyParticipantNameDisplayPreferences(sessionManager.mediaPreferences());
	connect(&sessionManager, &OpenMeeting::SessionManager::preferencesChanged, this,
		&MeetingRoomWindow::applyParticipantNameDisplayPreferences);
	if (!_cameraLogEffect) {
		_cameraLogEffect = [](bool error, const QString &tag, const QString &message) {
			LogToConsole(error ? LogCategory::Error : LogCategory::Media, tag, message);
		};
	}
	if (!_cameraWarningEffect) {
		_cameraWarningEffect = [](QWidget *parent, const QString &title, const QString &message) {
			QMessageBox::warning(parent, title, message);
		};
	}

	QPointer<MeetingRoomWindow> window(this);
	_cameraCompletionOwner = std::make_unique<CameraSwitchCompletionOwner>(
		this,
		[window](const CameraSwitchCompletionOwner::Ticket &ticket,
		         const QString &devicePath,
		         bool success,
		         const std::string &error) {
			if (window) {
				window->handleCameraSwitchResult(ticket, devicePath, success, error);
			}
		});

	connect(&sessionManager,
	        &OpenMeeting::SessionManager::sessionInvalidated,
	        this,
	        [this](OpenMeeting::SessionInvalidationReason) {
			cancelMediaPreparation();
			invalidateCameraCompletion();
		});
	connect(&sessionManager,
	        &OpenMeeting::SessionManager::sessionInvalidated,
	        this,
	        &MeetingRoomWindow::onSessionInvalidated,
	        Qt::QueuedConnection);
	connect(&sessionManager, &OpenMeeting::SessionManager::loggedOut, this,
		[this, &sessionManager]() {
			cancelMediaPreparation();
			invalidateCameraCompletion();
			if (!sessionManager.isSessionInvalidating())
				onSessionInvalidated(OpenMeeting::SessionInvalidationReason::UserLogout);
		});
}

void MeetingRoomWindow::bindCameraDeviceChanges() {
	Q_ASSERT(_bottomBar != nullptr);
	_bottomBar->videoDeviceChanged() | rpl::on_next([this](const QString &devicePath) {
		requestCameraSwitch(devicePath);
	}, lifetime());
}

void MeetingRoomWindow::requestCameraSwitch(const QString &devicePath) {
	Q_ASSERT(thread() == QThread::currentThread());
	if (!_cameraCompletionOwner || !_cameraSessionManager) return;
	if (!_cameraManager) {
		// Selecting a device while capture is deferred must not open hardware.
		if (_cameraSessionManager->isSessionInvalidating()) return;
		auto preferences = _cameraSessionManager->mediaPreferences();
		preferences.cameraDeviceId = devicePath;
		_cameraSessionManager->setMediaPreferences(preferences);
		_currentCameraPath = devicePath;
		return;
	}
	if (_cameraSessionManager->isSessionInvalidating()) {
		invalidateCameraCompletion();
		return;
	}

	_currentCameraPath = devicePath;
	const auto telemetryOperation = beginDeviceSwitchTelemetry(
		_cameraSwitchTelemetry,
		livekit::telemetry::OperationKind::CameraDeviceSwitch);
	const auto ticket = _cameraCompletionOwner->beginRequest(devicePath);
	if (!ticket) {
		finishDeviceSwitchTelemetry(
			_cameraSwitchTelemetry, telemetryOperation,
			livekit::telemetry::OperationOutcome::Failure);
		return;
	}
	auto manager = _cameraManager;
	auto callback = _cameraCompletionOwner->makeCallback(ticket);
	const auto logEffect = _cameraLogEffect;
	QPointer<MeetingRoomWindow> guard(this);
	if (logEffect) {
		logEffect(false, "CAMERA_SWITCH",
			QStringLiteral("camera_switch_requested device=%1")
				.arg(devicePath.isEmpty() ? QStringLiteral("default") : QStringLiteral("selected")));
	}
	if (!guard || !ticket.isCurrent()) {
		finishDeviceSwitchTelemetry(
			_cameraSwitchTelemetry, telemetryOperation,
			livekit::telemetry::OperationOutcome::Cancelled);
		return;
	}
	if (!guard->_cameraSessionManager
		|| guard->_cameraSessionManager->isSessionInvalidating()) {
		guard->invalidateCameraCompletion();
		return;
	}
	manager->SwitchDeviceAsync(
		devicePath.toStdString(),
		3000,
		std::move(callback));
}

void MeetingRoomWindow::handleCameraSwitchResult(
		const CameraSwitchCompletionOwner::Ticket &ticket,
		const QString &devicePath,
		bool success,
		const std::string &error) {
	Q_ASSERT(thread() == QThread::currentThread());
	if (!ticket.isCurrent() || !_cameraSessionManager) {
		return;
	}
	if (_cameraSessionManager->isSessionInvalidating()) {
		invalidateCameraCompletion();
		return;
	}

	const auto logEffect = _cameraLogEffect;
	const auto warningEffect = _cameraWarningEffect;
	QPointer<MeetingRoomWindow> guard(this);
	if (success) {
		finishDeviceSwitchTelemetry(
			_cameraSwitchTelemetry, _cameraSwitchTelemetry.serial,
			livekit::telemetry::OperationOutcome::Success);
		_usingRealCamera = true;
		if (_coordinator) _coordinator->setLocalVideoAvailable(true);
		if (_localTile) {
			_localTile->setVideoActive(_config.videoEnabled && _usingRealCamera);
		}
		if (logEffect) {
			logEffect(false, "CAMERA_SWITCH",
				QStringLiteral("camera_switch_succeeded device=%1")
					.arg(devicePath.isEmpty() ? QStringLiteral("default") : QStringLiteral("selected")));
		}
		return;
	}

	const auto errorText = QString::fromStdString(error);
	finishDeviceSwitchTelemetry(
		_cameraSwitchTelemetry, _cameraSwitchTelemetry.serial,
		errorText.contains(QStringLiteral("timeout"), Qt::CaseInsensitive)
			? livekit::telemetry::OperationOutcome::Timeout
			: livekit::telemetry::OperationOutcome::Failure);
	if (logEffect) {
		logEffect(true, "CAMERA_SWITCH",
			QStringLiteral("camera_switch_failed previous_device_restored"));
	}
	if (!guard || !ticket.isCurrent() || !guard->_cameraSessionManager
		|| guard->_cameraSessionManager->isSessionInvalidating()) {
		return;
	}
	if (warningEffect) {
		warningEffect(
			guard,
			QCoreApplication::translate("MeetingUI", "Camera Switch Failed"),
			QCoreApplication::translate("MeetingUI", "Unable to start the selected camera. Capture continues on the previous device.\nReason: %1")
				.arg(errorText));
	}
}

void MeetingRoomWindow::invalidateCameraCompletion() {
	if (_cameraCompletionOwner) {
		_cameraCompletionOwner->invalidate();
	}
	finishDeviceSwitchTelemetry(
		_cameraSwitchTelemetry, _cameraSwitchTelemetry.serial,
		livekit::telemetry::OperationOutcome::Cancelled);
}

void MeetingRoomWindow::retireLocalCapture() {
	Q_ASSERT(thread() == QThread::currentThread());
	_speakerConfiguredRoom.reset();
	_speakerStateInitialized = false;
	_speakerRequestedMuted = true;
#ifdef Q_OS_LINUX
    ++*_microphoneMuteIntent;
    ++*_microphoneSwitchIntent;
    ++*_speakerDeviceIntent;
    ++*_speakerMuteIntent;
    _speakerDevicePending = false;
    _microphoneMutePending = false;
    _microphoneSwitchPending = false;
#endif
	auto microphone = std::move(_wasapiCap);
	auto manager = std::move(_cameraManager);
	auto legacyCapture = std::move(_dshowCap);
	auto audioSource = std::move(_localAudioSource);
	auto videoSource = std::move(_localVideoSource);
	if (!microphone && !manager && !legacyCapture && !audioSource && !videoSource) return;
	// The application-owned worker keeps the devices and their output sources
	// alive through Stop, even if the meeting window has already been deleted.
	OpenMeeting::SessionShutdownService::Instance().SubmitCleanup(
		[microphone = std::move(microphone), manager = std::move(manager),
		 legacyCapture = std::move(legacyCapture), audioSource = std::move(audioSource),
		 videoSource = std::move(videoSource)]() mutable {
			// SessionShutdownService runs and releases native owners in an MTA.
			if (microphone) microphone->Stop();
			if (manager) manager->Stop();
			if (legacyCapture) legacyCapture->Stop();
			microphone.reset();
			manager.reset();
			legacyCapture.reset();
			audioSource.reset();
			videoSource.reset();
		});
}


} // namespace MeetingUI
