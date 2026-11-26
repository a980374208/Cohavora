#include "src/platform/qt_path.h"
#include "floating_member_list_window.h"
#include "shared_screen_window.h"
#include "src/ui/meeting_encryption_panel.h"
#include "telemetry_live_dialog.h"
#include "src/core/session_shutdown_service.h"
#include <QtCore/QCoreApplication>
#include "src/ui/meeting_room_window.h"
#include "src/ui/meeting_layout_controls.h"
#include "src/core/video_layout_geometry.h"
#include "src/ui/ui_timing_observer.h"
#include "src/ui/screen_share_quality_controls.h"
#include "src/ui/remote_control_ui.h"
#include "src/ui/whiteboard/accessible_combo_box.h"
#include "src/ui/app_theme.h"
#include "src/ui/app_icons.h"
#include "src/core/call_invite_controller.h"
#include "src/core/contact_catalog_controller.h"
#include "src/core/presence_watch_controller.h"
#include "src/ui/contacts/contact_picker_dialog.h"
#include "src/ui/whiteboard/annotation_overlay_window.h"
#include "src/ui/whiteboard/whiteboard_panel.h"
#include "src/ui/meeting_log_console.h"
#include "src/net/service_endpoint_policy.h"
#include "src/ui/telemetry_dialogs.h"
#include "src/telemetry/telemetry_report.h"
#include "src/telemetry/diagnostic_pipeline.h"
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDateTimeEdit>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFileDialog>
#include "directory_icon_provider.h"
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QApplication>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QProgressDialog>
#include <QtWidgets/QScrollArea>
#include "src/ui/native_child_geometry.h"
#include <QtWidgets/QStyle>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QToolTip>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QIcon>
#include <QtGui/QPainterPath>
#include <QtGui/QFont>
#include <QtGui/QClipboard>
#include <QtGui/QCursor>
#include <QtGui/QWindow>
#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QPointer>
#include <QtCore/QSettings>
#include <QtCore/QSignalBlocker>
#include <QtCore/QThread>
#include <algorithm>
#include <cmath>
#include <exception>
#include <set>

#ifdef Q_OS_LINUX
#include "src/ui/linux_window_frame.h"
#endif

#if defined(Q_OS_WIN)
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#endif

#include "src/ui/detail/meeting_room_helpers.h"

namespace MeetingUI {
using ColorRole = AppTheme::ColorRole;
using detail::ShowMeetingWarning;
using detail::themeColor;

namespace {

QSize bannerSize(QLabel &label, int availableWidth, int preferredWidth) {
 label.setWordWrap(true);
 label.ensurePolished();
 const int width = std::max(1, std::min(availableWidth - 32,
     std::max(preferredWidth, label.sizeHint().width())));
 return QSize(width, std::max(32, label.heightForWidth(width)));
}


constexpr int kVideoPagerHeight = 34;
#if defined(Q_OS_WIN)
constexpr int kNativeVerticalFramePixels = 1;
#endif

QSize screenShareSourcePixelSize(const livekit::DesktopSource &source) {
    if (source.kind == livekit::DesktopSourceKind::Screen) {
        if (const auto binding = livekit::ResolveScreenBinding(source, 1, "share-resolution-menu"))
            return QSize(binding->physical_width, binding->physical_height);
    }
#if defined(Q_OS_WIN)
    else {
        const auto window = reinterpret_cast<HWND>(source.id);
        RECT bounds{};
        if (!IsWindow(window)) return {};
        if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))) ||
            bounds.right <= bounds.left || bounds.bottom <= bounds.top)
            GetWindowRect(window, &bounds);
        if (bounds.right > bounds.left && bounds.bottom > bounds.top)
            return QSize(bounds.right - bounds.left, bounds.bottom - bounds.top);
    }
#endif
    return {};
}

bool SameViewportIntentContent(const livekit::ViewportIntent &left,
		const livekit::ViewportIntent &right) {
	const auto sameKey = [](const auto &a, const auto &b) {
		return a.has_value() == b.has_value() && (!a || *a == *b);
	};
	return left.mode == right.mode && left.page == right.page &&
		left.resolution_match == right.resolution_match &&
		left.page_size == right.page_size &&
		left.speaker_layout == right.speaker_layout &&
		left.speaking_only == right.speaking_only &&
		left.local_participant_present == right.local_participant_present &&
		left.local_screen_share_present == right.local_screen_share_present &&
		sameKey(left.page_anchor, right.page_anchor) &&
		sameKey(left.pinned, right.pinned) &&
		left.pinned_local == right.pinned_local &&
		sameKey(left.selected_share, right.selected_share) &&
		left.stage_content == right.stage_content &&
		left.stage_rect.x == right.stage_rect.x &&
		left.stage_rect.y == right.stage_rect.y &&
		left.stage_rect.width == right.stage_rect.width &&
		left.stage_rect.height == right.stage_rect.height &&
		left.paging_bar_height == right.paging_bar_height &&
		left.device_pixel_ratio == right.device_pixel_ratio &&
		left.remote_video_enabled == right.remote_video_enabled &&
        left.window_visible == right.window_visible &&
		left.minimized == right.minimized && left.detached_share == right.detached_share &&
		left.floating_strip == right.floating_strip;
}

bool isChatSenderPlaceholderName(const QString &name) {
	const auto normalized = name.trimmed().toCaseFolded();
	return normalized.isEmpty()
		|| normalized.startsWith(QStringLiteral("pa_"))
		|| normalized == QStringLiteral("participant-name")
		|| normalized == QStringLiteral("participant_name")
		|| normalized == QStringLiteral("participant name");
}

QString resolveParticipantDisplayName(
	const std::shared_ptr<OpenMeeting::MeetingCoordinator> &coordinator,
	const QString &senderIdentity,
	const QString &senderName) {
	QString displayName = senderName.trimmed();
	const auto needsParticipantLookup = isChatSenderPlaceholderName(displayName);
	if (needsParticipantLookup) {
		displayName = senderIdentity;
	}
	if (coordinator && (needsParticipantLookup || displayName == senderIdentity)) {
		if (const auto info = coordinator->participantInfo(senderIdentity)) {
			const auto participantName = info->name.trimmed();
			if (!isChatSenderPlaceholderName(participantName)) {
				displayName = participantName;
			}
		} else if (!senderName.isEmpty() && senderName != senderIdentity) {
			if (const auto byName = coordinator->participantInfo(senderName)) {
				const auto participantName = byName->name.trimmed();
				if (!isChatSenderPlaceholderName(participantName)) {
					displayName = participantName;
				}
			}
		}
	}
	return displayName.isEmpty() ? QCoreApplication::translate("MeetingUI", "Participants") : displayName;
}

} // namespace

// ----------------------------------------------------
// MeetingRoomWindow 实现
// ----------------------------------------------------


MeetingRoomWindow::MeetingRoomWindow(const Config &config,
                                     std::shared_ptr<OpenMeeting::MeetingCoordinator> coordinator,
                                     QWidget *parent)
	: Ui::RpWidget(parent)
	, _config(config)
	, _coordinator(std::move(coordinator)) {
	closeStandaloneNotices();
	setObjectName("MeetingRoomWindow");
	AppTheme::setTone(*this, AppTheme::Tone::Dark);
	AppTheme::observeChanges(*this, [this] {
		update();
		if (_stageContainer) _stageContainer->update();
	});
	if (!_coordinator) {
		_coordinator = OpenMeeting::MeetingCoordinator::create(this);
	}
	_agentCoordinator = _config.agentCoordinator;
	if (!_agentCoordinator) {
		_agentCoordinator = std::make_shared<OpenMeeting::AgentCoordinator>(
			OpenMeeting::SessionManager::instance());
	}
	_agentCoordinator->attachMeeting(_coordinator.get());
	setupMeetingTranslationServices();
	setupCameraCompletionOwner(OpenMeeting::SessionManager::instance());
	setWindowTitle(QCoreApplication::translate("MeetingUI", "Cohavora Meeting Room - %1").arg(config.displayName));
	resize(1120, 720);
	setMinimumSize(850, 560);
	if (!parent) {
		AppTheme::centerOnScreen(*this);
	}
	setMouseTracking(true);

	// Keep Qt's native frame bookkeeping consistent with the resizable HWND.
	// WM_NCCALCSIZE removes the native title bar while retaining its resize frame.
	setWindowFlags(Qt::Window | Qt::WindowSystemMenuHint | Qt::WindowMinMaxButtonsHint);
#ifdef Q_OS_LINUX
	setWindowFlag(Qt::FramelessWindowHint, true);
	installLinuxWindowFrame(*this, {
		.captionHitTest = [this](QPoint position) {
			return _topBar && _topBar->geometry().contains(position) &&
				_topBar->isWindowDragArea(_topBar->mapFrom(this, position));
		},
	});
#endif

	_persistVideoLayoutPreferences = true;
	const QSettings preferences;
	const auto savedLayout = preferences.value(QStringLiteral("meeting/videoLayout"), QStringLiteral("grid")).toString();
	const auto savedCapacity = preferences.value(QStringLiteral("meeting/gridCapacity"), 9).toUInt();
	_videoPageSize = savedCapacity == 25 || savedCapacity == 49 ? savedCapacity : 9;
	_speakingOnly = preferences.value(QStringLiteral("meeting/speakingOnly"), false).toBool();
	const std::array<QString, 5> speakerLayouts = {QStringLiteral("right"), QStringLiteral("top"),
		QStringLiteral("top-left"), QStringLiteral("bottom-left"), QStringLiteral("floating")};
	for (size_t i = 0; i < speakerLayouts.size(); ++i) {
		if (savedLayout == speakerLayouts[i]) {
			_viewMode = VideoViewMode::Speaker;
			_speakerLayout = static_cast<livekit::SpeakerLayout>(i);
			break;
		}
	}
	_preparingMedia = true;
	initLayout();
	if (auto sample = _coordinator->pageTiming())
		new FirstPaintTimingObserver(_bottomBar, sample);
	if (auto sample = _coordinator->joinTiming()) {
		connect(this, &QObject::destroyed, QCoreApplication::instance(), [sample] {
            sample->Finish(livekit::diagnostic::Outcome::Cancelled);
        });
		QPointer<MeetingRoomWindow> window(this);
		ObserveTimingBoundary(sample, [window] {
            return window && window->isVisible() && window->_coordinator &&
                window->_coordinator->state() == OpenMeeting::MeetingState::InMeeting &&
                window->_coordinator->isMeetingReady();
        });
	}
	setupCoordinatorBindings();
	_coordinator->setLocalAudioMuted(_config.audioMuted);
	_coordinator->setLocalVideoEnabled(_config.videoEnabled);
	_remoteRenderSession = std::make_unique<livekit::render::VideoRenderSession>(
		[this](const std::string &identity, const QImage &image,
		       livekit::render::VideoRenderFrame::Ptr frame) {
			receiveRenderedVideoFrame(
				image, QString::fromStdString(identity), std::move(frame));
		});
	_remoteRenderTimer = new QTimer(this);
	connect(_remoteRenderTimer, &QTimer::timeout, this, &MeetingRoomWindow::onRemoteRenderTick);
	_remoteRenderTimer->start(33);

	// 3. 复用 Coordinator 的本地音频与视频数据源，确保外设采集与 WebRTC 发送通道打通
	bindLocalMediaSources();

	const auto captureUiCallbacks = _captureUiCallbacks;
	_localAudioSource->addSink([captureUiCallbacks](const livekit::AudioFrame &frame) {
		if (!captureUiCallbacks->active()) return;
		const auto &samples = frame.data();
		if (!samples.empty()) {
			double sum = 0.0;
			for (size_t i = 0; i < samples.size(); ++i) {
				sum += static_cast<double>(samples[i]) * samples[i];
			}
			double rms = std::sqrt(sum / static_cast<double>(samples.size()));
			float level = static_cast<float>(std::clamp(rms / 2200.0, 0.0, 1.0));
			bool speaking = (level > 0.02f);
			captureUiCallbacks->Post([speaking, level](MeetingRoomWindow *window) {
				if (window->_sessionRunning.load(std::memory_order_acquire) &&
					window->_localTile && !window->_config.audioMuted) {
					window->_localTile->setSpeaking(speaking, level);
				}
			});
		}
	});


	_localTile->setVideoActive(false);
	_localTile->setAudioMuted(_config.audioMuted);
	updateVideoLayout();

	attachCoordinatorSession();
	scheduleViewportIntent(true);

	updateRecoveryStateUi(_pendingAdmission ? OpenMeeting::MeetingState::Idle :
		(_coordinator ? _coordinator->state() : OpenMeeting::MeetingState::InMeeting), {});
}

MeetingRoomWindow::MeetingRoomWindow(
		ParticipantWindowInjectionTag,
		const Config &config,
		std::shared_ptr<OpenMeeting::MeetingCoordinator> coordinator,
		bool fullLayoutForChatPrivacy,
		QWidget *parent)
:	Ui::RpWidget(parent),
	_coordinator(std::move(coordinator)),
	_config(config) {
	// Isolated presentation fixture: the bindings, tile operations and CPU
	// renderer are production paths; no device capture or account singleton.
	setObjectName("MeetingRoomWindowParticipantFixture");
	AppTheme::setTone(*this, AppTheme::Tone::Dark);
	AppTheme::observeChanges(*this, [this] {
		update();
		if (_stageContainer) _stageContainer->update();
	});
	if (fullLayoutForChatPrivacy) {
		initLayout();
	} else {
		_topBar = new RoomTopBarWidget(this);
		_stageContainer = new QWidget(this);
		_bottomBar = new RoomBottomBarWidget(this);
		_localTile = new VideoTileWidget(_config.displayName, true, _stageContainer);
		_localTile->setIdentity("local");
		bindTileInteractions(_localTile);
		_localTile->setAudioMuted(true);
		_localTile->setVideoActive(false);
		_inviteHintBanner = new QLabel(_stageContainer);
		_recoveryBanner = new QLabel(_stageContainer);
		_recoveryBanner->hide();
		setupInvitationBinding();
		setupWhiteboardBinding();
		setupVideoPagingControls();
		setupLiveCaptionBanner();
	}
	_remoteRenderSession = std::make_unique<livekit::render::VideoRenderSession>(
		[this](const std::string &identity, const QImage &image,
		       livekit::render::VideoRenderFrame::Ptr frame) {
			receiveRenderedVideoFrame(
				image, QString::fromStdString(identity), std::move(frame));
		});
	_remoteRenderSession->UseQtCpuBackend();
	resize(1120, 720);
	_stageContainer->setGeometry(0, 56, 1120, 588);
	bindLocalMediaSources();
	setupCoordinatorBindings();
	attachCoordinatorSession();
	scheduleViewportIntent(true);
}

MeetingRoomWindow::MeetingRoomWindow(
		CameraOwnerInjectionTag,
		const Config &config,
		std::shared_ptr<livekit::CameraSourceManager> cameraManager,
		OpenMeeting::SessionManager &sessionManager,
		QWidget *parent)
:	Ui::RpWidget(parent),
	_config(config),
	_cameraManager(std::move(cameraManager)) {
	setObjectName("MeetingRoomWindowCameraOwnerFixture");
	AppTheme::setTone(*this, AppTheme::Tone::Dark);
	AppTheme::observeChanges(*this, [this] {
		update();
		if (_stageContainer) _stageContainer->update();
	});
	_topBar = new RoomTopBarWidget(this);
	_stageContainer = new QWidget(this);
	_bottomBar = new RoomBottomBarWidget(this);
	_localTile = new VideoTileWidget(
		QCoreApplication::translate("MeetingUI", "%1 (Me)").arg(_config.displayName),
		true,
		_stageContainer);
	_localTile->setIdentity("local");
	_localTile->setVideoActive(_config.videoEnabled);
	setupCameraCompletionOwner(sessionManager);
	bindCameraDeviceChanges();
}

MeetingRoomWindow::~MeetingRoomWindow() {
	if (_agentCoordinator && _coordinator &&
		_agentCoordinator->attachedMeeting() == _coordinator.get()) {
		_agentCoordinator->attachMeeting(nullptr);
	}
    _remoteControlUi.reset();
	if (_nativeResizeFilterInstalled && qApp) qApp->removeNativeEventFilter(this);
	dismissDepartureNotice();
	invalidateCameraCompletion();
	stopLiveKitSession();
}

void MeetingRoomWindow::showEvent(QShowEvent *e) {
	Ui::RpWidget::showEvent(e);
	_logicalWindowVisible = true;
	updateStageGeometry();
	setupNativeWindow();
	tryActivateGpuBackend();
	scheduleViewportIntent(true);
}

void MeetingRoomWindow::hideEvent(QHideEvent *e) {
	_logicalWindowVisible = false;
	if (_layoutPopup) _layoutPopup->hide();
	if (_videoPagingControls) _videoPagingControls->hide();
	scheduleViewportIntent(true);
	updateVideoLayout();
	Ui::RpWidget::hideEvent(e);
}

void MeetingRoomWindow::changeEvent(QEvent *e) {
	Ui::RpWidget::changeEvent(e);
	if (e && e->type() == QEvent::WindowStateChange) {
		if (_topBar) {
			_topBar->setMaximized(isMaximized() || isFullScreen());
		}
		updateStageGeometry();
		scheduleViewportIntent(true);
		updateVideoLayout();
	} else if (e && e->type() == QEvent::LanguageChange) {
		if (_topBar) _topBar->update();
		if (_bottomBar) _bottomBar->update();
		update();
	}
}

void MeetingRoomWindow::closeEvent(QCloseEvent *e) {
	dismissDepartureNotice();
	if (!_closeRequested) {
		_closeRequested = true;
		const bool requestLeave = !_closingForSessionInvalidation &&
			!OpenMeeting::SessionManager::instance().isSessionInvalidating();
		stopLiveKitSession(requestLeave);
	}
	Ui::RpWidget::closeEvent(e);
}

void MeetingRoomWindow::setupNativeWindow() {
#if defined(Q_OS_WIN)
	_handle = reinterpret_cast<HWND>(winId());
	if (!_handle) return;
	if (!_nativeResizeFilterInstalled) {
		qApp->installNativeEventFilter(this);
		_nativeResizeFilterInstalled = true;
	}
	// Qt owns the fullscreen HWND style and monitor geometry. Reinstalling a
	// resize frame during restore can turn its delayed native state back normal.
	if (isFullScreen()) return;

	const LONG_PTR style = GetWindowLongPtr(_handle, GWL_STYLE);
	// Windows 10 can paint a native caption even with our thin NCCALCSIZE
	// inset. Keep the resize frame, but let only the Qt top bar own the caption.
	const LONG_PTR decoratedStyle =
		(style & ~LONG_PTR(WS_CAPTION)) | WS_THICKFRAME | WS_SYSMENU |
		WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN;
	if (style != decoratedStyle) {
		SetWindowLongPtr(_handle, GWL_STYLE, decoratedStyle);
	}

	MARGINS margins = { 1, 1, 1, 1 };
	DwmExtendFrameIntoClientArea(_handle, &margins);

	DWORD preference = 2; // DWMWCP_ROUND
	DwmSetWindowAttribute(_handle, 33, &preference, sizeof(preference));

	if (style != decoratedStyle) {
		SetWindowPos(_handle, nullptr, 0, 0, 0, 0,
			SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
	}
#endif
}

void MeetingRoomWindow::initLayout() {
	_topBar = new RoomTopBarWidget(this);
	connect(_topBar, &RoomTopBarWidget::windowDragRequested, this, [this]() {
		if (isFullScreen()) {
			return;
		}
		if (auto *handle = windowHandle()) {
			handle->startSystemMove();
		}
	});
	QString mId = _coordinator ? _coordinator->currentMeetingId() : QString();
	if (mId.isEmpty()) mId = _config.meetingId;
	if (!mId.isEmpty()) {
		_topBar->setMeetingId(mId);
		setWindowTitle(QCoreApplication::translate("MeetingUI", "Cohavora - Meeting ID: %1").arg(mId));
	}
	_stageContainer = new QWidget(this);
	MeetingUI::AppTheme::setStyleVariant(*_stageContainer, "meeting-room-window-stagecontainer");
	if ((_videoCanvas = livekit::render::CreateVideoCanvas(_stageContainer, &_renderDiagnostics))) {
		_videoCanvas->setGeometry(_stageContainer->rect());
		_videoCanvas->hide();
		connect(_videoCanvas, &livekit::render::VideoCanvas::rendererUnavailable,
		        this, &MeetingRoomWindow::fallBackToQtCpuBackend, Qt::QueuedConnection);
	} else {
		LogToConsole(LogCategory::WebRTC, "RENDER",
			QCoreApplication::translate("MeetingUI", "Qt CPU video backend: ") + livekit::render::RenderDiagnosticsSafeSummary(_renderDiagnostics));
	}
	_bottomBar = new RoomBottomBarWidget(this);

	_participantsSidebar = new OpenMeeting::ParticipantsSidebarWidget(_coordinator, this);
	_participantsSidebar->hide();
	connect(_participantsSidebar, &OpenMeeting::ParticipantsSidebarWidget::localMicrophoneRequested,
		this, &MeetingRoomWindow::requestLocalMicrophone);
	connect(_participantsSidebar, &OpenMeeting::ParticipantsSidebarWidget::localCameraRequested,
		this, &MeetingRoomWindow::requestLocalCamera);
	connect(_participantsSidebar, &OpenMeeting::ParticipantsSidebarWidget::closeRequested, this, [this]() {
		switchSidebar(ActiveSidebar::None);
	});

	_chatSidebar = new OpenMeeting::MeetingChatSidebarWidget(this);
	_chatSidebar->hide();
	connect(_chatSidebar, &OpenMeeting::MeetingChatSidebarWidget::closeRequested, this, [this]() {
		switchSidebar(ActiveSidebar::None);
	});
	_copilotSidebar = new OpenMeeting::CopilotSidebarWidget(_agentCoordinator.get(), this);
	_copilotSidebar->hide();
	connect(_copilotSidebar, &OpenMeeting::CopilotSidebarWidget::closeRequested, this, [this]() {
		if (_activeSidebar == ActiveSidebar::Copilot) {
			switchSidebar(ActiveSidebar::None);
		}
	});
	connect(_copilotSidebar, &OpenMeeting::CopilotSidebarWidget::expandToggleRequested, this, [this]() {
		_copilotExpanded = !_copilotExpanded;
		if (_copilotSidebar) {
			_copilotSidebar->setExpanded(_copilotExpanded);
		}
		if (_activeSidebar == ActiveSidebar::Copilot) {
			QResizeEvent ev(size(), size());
			resizeEvent(&ev);
		}
	});
	connect(_copilotSidebar, &OpenMeeting::CopilotSidebarWidget::openSettingsRequested, this, [this]() {
		openCopilotSettingsDialog();
	});
	connect(_copilotSidebar, &OpenMeeting::CopilotSidebarWidget::openSettingsPageRequested, this, [this](int pageIndex, int subTabIndex) {
		if (_config.openSettingsPage) {
			_config.openSettingsPage(pageIndex, subTabIndex, this);
		} else {
			openCopilotSettingsDialog();
		}
	});
	connect(_chatSidebar, &OpenMeeting::MeetingChatSidebarWidget::messageSent, this, [this](const QString &text) {
		QString msgId = QString("msg_%1_%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(qrand() % 1000);
		int64_t seq = _coordinator ? _coordinator->nextSequenceNumber() : (QDateTime::currentMSecsSinceEpoch() * 1000);
		OpenMeeting::ChatMessageItem item;
		item.id = msgId;
		item.senderIdentity = "local";
		item.senderName = QCoreApplication::translate("MeetingUI", "%1 (Me)").arg(_config.displayName);
		item.text = text;
		item.timestamp = QDateTime::currentMSecsSinceEpoch();
		item.seq = seq;
		item.isMine = true;
		item.status = OpenMeeting::MessageSendStatus::Sending;
		_chatSidebar->appendMessage(item);

		if (_coordinator) {
			_coordinator->sendChatMessage(text, msgId, seq);
		}
		if (_agentCoordinator) {
			_agentCoordinator->recordMeetingChatMessage(item.senderName, text, true);
		}
		LogToConsole(LogCategory::Participant, "CHAT_TEXT_SENT",
			QStringLiteral("bytes=%1").arg(text.toUtf8().size()));
	});

	connect(_chatSidebar, &OpenMeeting::MeetingChatSidebarWidget::imageSent, this, [this](const QString &fileName, const QByteArray &data) {
		QString msgId = QString("img_%1_%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(qrand() % 1000);
		int64_t seq = _coordinator ? _coordinator->nextSequenceNumber() : (QDateTime::currentMSecsSinceEpoch() * 1000);
		OpenMeeting::ChatMessageItem item;
		item.id = msgId;
		item.senderIdentity = "local";
		item.senderName = QCoreApplication::translate("MeetingUI", "%1 (Me)").arg(_config.displayName);
		item.type = OpenMeeting::ChatMessageType::Image;
		item.fileName = fileName;
		item.fileSize = data.size();
		item.fileData = data;
		item.timestamp = QDateTime::currentMSecsSinceEpoch();
		item.seq = seq;
		item.isMine = true;
		item.status = OpenMeeting::MessageSendStatus::Sending;
		item.progress = 0;
		_chatSidebar->appendMessage(item);

		if (_coordinator) {
			_coordinator->sendChatMediaMessage(msgId, "image", fileName, data, seq);
		}
		LogToConsole(LogCategory::Participant, "CHAT_IMAGE_SENT",
			QStringLiteral("bytes=%1").arg(data.size()));
	});

	connect(_chatSidebar, &OpenMeeting::MeetingChatSidebarWidget::fileSent, this, [this](const QString &fileName, const QByteArray &data) {
		QString msgId = QString("file_%1_%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(qrand() % 1000);
		int64_t seq = _coordinator ? _coordinator->nextSequenceNumber() : (QDateTime::currentMSecsSinceEpoch() * 1000);
		OpenMeeting::ChatMessageItem item;
		item.id = msgId;
		item.senderIdentity = "local";
		item.senderName = QCoreApplication::translate("MeetingUI", "%1 (Me)").arg(_config.displayName);
		item.type = OpenMeeting::ChatMessageType::File;
		item.fileName = fileName;
		item.fileSize = data.size();
		item.fileData = data;
		item.timestamp = QDateTime::currentMSecsSinceEpoch();
		item.seq = seq;
		item.isMine = true;
		item.status = OpenMeeting::MessageSendStatus::Sending;
		item.progress = 0;
		_chatSidebar->appendMessage(item);

		if (_coordinator) {
			_coordinator->sendChatMediaMessage(msgId, "file", fileName, data, seq);
		}
		LogToConsole(LogCategory::Participant, "CHAT_FILE_SENT",
			QStringLiteral("bytes=%1").arg(data.size()));
	});

	connect(_chatSidebar, &OpenMeeting::MeetingChatSidebarWidget::retryRequested, this, [this](const QString &msgId) {
		if (!_coordinator || !_chatSidebar) return;
		auto msg = _chatSidebar->findMessage(msgId);
		if (msg.id.isEmpty()) return;

		_chatSidebar->updateMessageStatus(msgId, OpenMeeting::MessageSendStatus::Sending, 0);

		if (msg.type == OpenMeeting::ChatMessageType::Text) {
			_coordinator->sendChatMessage(msg.text, msgId, msg.seq);
		} else if (msg.type == OpenMeeting::ChatMessageType::Image) {
			_coordinator->sendChatMediaMessage(msgId, "image", msg.fileName, msg.fileData, msg.seq);
		} else if (msg.type == OpenMeeting::ChatMessageType::File) {
			_coordinator->sendChatMediaMessage(msgId, "file", msg.fileName, msg.fileData, msg.seq);
		}
	});

	_localTile = new VideoTileWidget(QCoreApplication::translate("MeetingUI", "%1 (Me)").arg(_config.displayName), true, _stageContainer);
	_localTile->setIdentity("local");
	_localTile->show();

	bindTileInteractions(_localTile);

	_bottomBar->setAudioMuted(_config.audioMuted);
	_bottomBar->setVideoEnabled(_config.videoEnabled);
	_bottomBar->setParticipantCount(1);

	_localTile->setAudioMuted(_config.audioMuted);
	_localTile->setVideoActive(_config.videoEnabled);

	_inviteHintBanner = new QLabel(QCoreApplication::translate("MeetingUI", "Waiting for more participants..."), _stageContainer);
	_inviteHintBanner->setAlignment(Qt::AlignCenter);
	MeetingUI::AppTheme::setStyleVariant(*_inviteHintBanner, "meeting-room-window-invitehintbanner");

	_recoveryBanner = new QLabel(_stageContainer);
	_recoveryBanner->setAlignment(Qt::AlignCenter);
	_recoveryBanner->hide();
	setupVideoPagingControls();
	setupLiveCaptionBanner();

	_recoveryBannerFadeTimer = new QTimer(this);
	_recoveryBannerFadeTimer->setSingleShot(true);
	connect(_recoveryBannerFadeTimer, &QTimer::timeout, this, [this]() {
		if (_recoveryBanner) {
			_recoveryBanner->hide();
		}
	});

	_meetingTimer = new QTimer(this);
	connect(_meetingTimer, &QTimer::timeout, this, &MeetingRoomWindow::onTimerTick);
	_meetingTimer->start(1000);

	_topBar->minimizeClicked() | rpl::on_next([this] { showMinimized(); }, lifetime());
	_topBar->maximizeClicked() | rpl::on_next([this] {
		if (isMaximized()) showNormal();
		else showMaximized();
	}, lifetime());
	_topBar->closeClicked() | rpl::on_next([this] { close(); }, lifetime());
	_topBar->viewModeChanged() | rpl::on_next([this](VideoViewMode m) {
		_viewMode = m;
		_videoPage = 0;
		scheduleViewportIntent(true);
	}, lifetime());
	_topBar->consoleClicked() | rpl::on_next([this] {
		MeetingLogConsoleWindow::Instance().show();
		MeetingLogConsoleWindow::Instance().raise();
		MeetingLogConsoleWindow::Instance().activateWindow();
	}, lifetime());

	auto handleSimulate = [this](livekit::SimulateScenarioType type) {
        if (type == livekit::SimulateScenarioType::E2eeKeyRatchet && _room) {
            const auto manager = _room->e2ee_manager();
            if (manager && manager->enabled() && manager->encryption_type() != livekit::EncryptionType::NONE) {
                LogToConsole(LogCategory::General, "E2EE",
                    QCoreApplication::translate("MeetingUI", "Debug key rotation is unavailable in an encrypted meeting."));
                return;
            }
        }
		if (_room) {
			_room->SimulateScenario(type);
		}
		QString name;
		switch (type) {
		case livekit::SimulateScenarioType::SignalReconnect: name = "signalReconnect"; break;
		case livekit::SimulateScenarioType::FullReconnect: name = "fullReconnect"; break;
		case livekit::SimulateScenarioType::SpeakerUpdate: name = "speakerUpdate"; break;
		case livekit::SimulateScenarioType::NodeFailure: name = "nodeFailure"; break;
		case livekit::SimulateScenarioType::Migration: name = "migration"; break;
		case livekit::SimulateScenarioType::ServerLeave: name = "serverLeave"; break;
		case livekit::SimulateScenarioType::SwitchCandidate: name = "switchCandidate"; break;
		case livekit::SimulateScenarioType::E2eeKeyRatchet: name = "e2eeKeyRatchet"; break;
		case livekit::SimulateScenarioType::ParticipantName: name = "participantName"; break;
		case livekit::SimulateScenarioType::ParticipantMetadata: name = "participantMetadata"; break;
		case livekit::SimulateScenarioType::Clear: name = "clear"; break;
		}
		LogToConsole(LogCategory::General, "SIMULATE", QCoreApplication::translate("MeetingUI", "Scenario simulation triggered: %1").arg(name));
	};

	_topBar->simulateScenarioRequested() | rpl::on_next(handleSimulate, lifetime());
	_bottomBar->simulateScenarioRequested() | rpl::on_next(handleSimulate, lifetime());

	_bottomBar->toggleAudioRequested() | rpl::on_next([this](bool muted) {
		requestLocalMicrophone(muted);
		LogToConsole(LogCategory::Media, "AUDIO", muted ? QCoreApplication::translate("MeetingUI", "User muted the microphone") : QCoreApplication::translate("MeetingUI", "User requested to enable the microphone"));
	}, lifetime());

	_bottomBar->toggleSpeakerRequested() | rpl::on_next([this](bool muted) {
		setSpeakerOutputMuted(muted);
		LogToConsole(LogCategory::Media, "SPEAKER", muted ? QCoreApplication::translate("MeetingUI", "User muted speaker output") : QCoreApplication::translate("MeetingUI", "User requested to enable speaker output"));
	}, lifetime());

	_bottomBar->toggleVideoRequested() | rpl::on_next([this](bool enabled) {
		requestLocalCamera(enabled);
		LogToConsole(LogCategory::Media, "VIDEO", enabled ? QCoreApplication::translate("MeetingUI", "User requested to enable local video") : QCoreApplication::translate("MeetingUI", "User disabled local video"));
	}, lifetime());

	setupInvitationBinding();

	setupWhiteboardBinding();

	_bottomBar->participantsClicked() | rpl::on_next([this] {
		switchSidebar(ActiveSidebar::Participants);
	}, lifetime());

	_bottomBar->chatClicked() | rpl::on_next([this] {
		switchSidebar(ActiveSidebar::Chat);
	}, lifetime());
	_bottomBar->copilotClicked() | rpl::on_next([this] {
		switchSidebar(ActiveSidebar::Copilot);
	}, lifetime());

	if (_coordinator) {
		connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::chatMessageReceived,
			this, [this](const QString &senderIdentity, const QString &senderName, const QString &text, int64_t seq) {
			const auto dispName = resolveParticipantDisplayName(
				_coordinator, senderIdentity, senderName);

			OpenMeeting::ChatMessageItem item;
			item.id = QString::number(QDateTime::currentMSecsSinceEpoch());
			item.senderIdentity = senderIdentity;
			item.senderName = dispName;
			item.text = text;
			item.timestamp = QDateTime::currentMSecsSinceEpoch();
			item.seq = seq;
			item.isMine = false;

			if (_chatSidebar) {
				_chatSidebar->appendMessage(item);
			}

			if (_activeSidebar != ActiveSidebar::Chat && _bottomBar) {
				_bottomBar->setChatUnreadCount(_bottomBar->chatUnreadCount() + 1);
			}

			LogToConsole(LogCategory::Participant, "CHAT_TEXT_RECEIVED",
				QStringLiteral("bytes=%1").arg(text.toUtf8().size()));
		});

		connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::chatMediaReceivingStarted,
			this, [this](const QString &transferId, const QString &senderIdentity, const QString &senderName,
						 const QString &mediaType, const QString &fileName, qint64 totalSize, int64_t seq) {
			const auto dispName = resolveParticipantDisplayName(
				_coordinator, senderIdentity, senderName);

			if (_chatSidebar) {
				_chatSidebar->startReceivingMedia(transferId, senderIdentity, dispName, mediaType, fileName, totalSize, seq);
			}

			if (_activeSidebar != ActiveSidebar::Chat && _bottomBar) {
				_bottomBar->setChatUnreadCount(_bottomBar->chatUnreadCount() + 1);
			}

			LogToConsole(LogCategory::Participant, "CHAT_MEDIA_RECEIVING",
				QStringLiteral("type=%1 bytes=%2")
					.arg(mediaType == QStringLiteral("image") ? QStringLiteral("image") : QStringLiteral("file"))
					.arg(totalSize));
		});

		connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::chatMediaReceivingProgress,
			this, [this](const QString &transferId, int progress) {
			if (_chatSidebar) {
				_chatSidebar->updateReceivingProgress(transferId, progress);
			}
		});

		connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::chatMediaReceivingCompleted,
			this, [this](const QString &transferId, const QString &senderIdentity, const QString &senderName,
						 const QString &mediaType, const QString &fileName, const QByteArray &data) {
			if (_chatSidebar) {
				_chatSidebar->completeReceivingMedia(transferId, mediaType, fileName, data);
			}

			LogToConsole(LogCategory::Participant, "CHAT_MEDIA_RECEIVED",
				QStringLiteral("type=%1 bytes=%2")
					.arg(mediaType == QStringLiteral("image") ? QStringLiteral("image") : QStringLiteral("file"))
					.arg(data.size()));
		});

		connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::chatMediaReceivingFailed,
			this, [this](const QString &transferId, const QString &reason) {
			if (_chatSidebar) {
				_chatSidebar->failReceivingMedia(transferId, reason);
			}
			LogToConsole(LogCategory::Participant, "CHAT_MEDIA_FAILED",
				QStringLiteral("transfer_interrupted"));
		});

		connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::chatMessageSendProgress,
			this, [this](const QString &messageId, int progress) {
			if (_chatSidebar) {
				_chatSidebar->updateMessageStatus(messageId, OpenMeeting::MessageSendStatus::Sending, progress);
			}
		});

		connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::chatMessageSendSuccess,
			this, [this](const QString &messageId) {
			if (_chatSidebar) {
				_chatSidebar->updateMessageStatus(messageId, OpenMeeting::MessageSendStatus::Sent, 100);
			}
		});

		connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::chatMessageSendFailed,
			this, [this](const QString &messageId, const QString &error) {
			if (_chatSidebar) {
				_chatSidebar->updateMessageStatus(messageId, OpenMeeting::MessageSendStatus::Failed, 0, error);
			}
		});
	}

	_bottomBar->endMeetingClicked() | rpl::on_next([this] {
		handleEndMeetingClicked();
	}, lifetime());

	_bottomBar->sendChatRequested() | rpl::on_next([this](const QString &text) {
		_topBar->setActiveSpeaker(QString::fromUtf8("%1: %2").arg(_config.displayName).arg(text));
		LogToConsole(LogCategory::Participant, "CHAT_TEXT_SENT",
			QStringLiteral("bytes=%1").arg(text.toUtf8().size()));
		if (_coordinator) {
			_coordinator->sendChatMessage(text);
		}
	}, lifetime());

	_bottomBar->microphoneDeviceChanged() | rpl::on_next([this](const QString &devId) {
		auto &session = OpenMeeting::SessionManager::instance();
		auto preferences = session.mediaPreferences();
		if (preferences.microphoneDeviceId == devId) {
			requestMicrophoneSwitch(devId);
		} else {
			preferences.microphoneDeviceId = devId;
			session.setMediaPreferences(preferences);
		}
	}, lifetime());

	_bottomBar->speakerDeviceChanged() | rpl::on_next([this](const QString &deviceId) {
		auto &session = OpenMeeting::SessionManager::instance();
		auto preferences = session.mediaPreferences();
		if (preferences.speakerDeviceId == deviceId) {
#ifdef Q_OS_LINUX
			requestSpeakerDevice(deviceId);
#else
			selectSpeakerDevice(deviceId);
#endif
		} else {
			preferences.speakerDeviceId = deviceId;
			session.setMediaPreferences(preferences);
		}
	}, lifetime());

	bindCameraDeviceChanges();

	_localGenTimer = new QTimer(this);
	connect(_localGenTimer, &QTimer::timeout, this, &MeetingRoomWindow::onLocalVideoGenerated);
}

void MeetingRoomWindow::onTimerTick() {
	_elapsedSeconds++;
	_topBar->updateDuration(_elapsedSeconds);
	const bool encrypted = _room && _room->e2ee_manager() &&
		_room->e2ee_manager()->enabled() &&
		_room->e2ee_manager()->encryption_type() != livekit::EncryptionType::NONE;
	_topBar->setEncrypted(encrypted);
}

void MeetingRoomWindow::onRemoteRenderTick() {
	if (_remoteRenderSession && (isVideoStageVisible() || isSharedScreenVisible() || isFloatingListVisible())) {
		_remoteRenderSession->RenderLatestFrames();
		const auto nowMs = QDateTime::currentMSecsSinceEpoch();
		if (nowMs - _lastRenderTelemetrySampleMs >= 1000) {
			_lastRenderTelemetrySampleMs = nowMs;
			if (const auto telemetry = _coordinator
					? _coordinator->sessionTelemetry().lock() : nullptr) {
				const auto statistics = _remoteRenderSession->statistics();
				if (_videoCanvas) _renderDiagnostics = _videoCanvas->renderDiagnostics();
				livekit::telemetry::RenderPipelineSample sample;
				sample.router_submitted = statistics.router.submitted;
				sample.router_replaced_before_render =
					statistics.router.replaced_before_render;
				sample.router_rejected_generation =
					statistics.router.rejected_generation;
				sample.router_rejected_binding =
					statistics.router.rejected_binding;
				sample.router_dropped_invalid = statistics.router.dropped_invalid;
				sample.router_dropped_capacity = statistics.router.dropped_capacity;
				sample.delivered_to_gpu = statistics.delivered_to_gpu;
				sample.delivered_to_qt_cpu = statistics.delivered_to_qt_cpu;
				sample.qt_cpu_conversion_failures =
					statistics.qt_cpu_conversion_failures;
				sample.rejected_track_attachments =
					statistics.rejected_track_attachments;
				sample.attached_track_count = statistics.attached_track_count;
				sample.requested_backend = livekit::render::RenderBackendName(
					_renderDiagnostics.requested_backend).toStdString();
				sample.actual_backend = livekit::render::RenderBackendName(
					_renderDiagnostics.actual_backend).toStdString();
				sample.gpu_failure = livekit::render::RenderGpuFailureName(
					_renderDiagnostics.gpu_failure).toStdString();
				sample.fallback_reason = livekit::render::RenderFallbackReasonName(
					_renderDiagnostics.fallback_reason).toStdString();
				telemetry->RecordRenderPipelineSample(std::move(sample));
			}
		}
	}
	if (_localScreenPreview && localScreenPreviewVisible()) {
		using PreviewTarget = livekit::render::VideoRenderRouter::PreviewTarget;
		const auto key = _localScreenTile->renderKey();
		bool dx11 = _videoCanvas && _videoCanvas->usesDx11();
		if (isDetachedShareFrame(key)) dx11 = _sharedWindow->usesDx11();
		else if (_floatingStripRenderKeys.count(key)) dx11 = _floatingListWindow && _floatingListWindow->usesDx11();
		const bool native = dx11 && _usingGpuBackend.load(std::memory_order_acquire);
		auto frame = _localScreenPreview->TakePreview("screen", _localScreenPreview->generation(),
			native ? PreviewTarget::Dx11 : PreviewTarget::Cpu);
		if (!frame) return;
		_localScreenTile->setVideoActive(true);
		if (_remoteRenderSession) _remoteRenderSession->RenderFrame(key.toStdString(), std::move(frame));
	} else {
		updateLocalScreenPreviewDemand();
	}
}

void MeetingRoomWindow::switchSidebar(ActiveSidebar target) {
	if (_activeSidebar == target) {
		_activeSidebar = ActiveSidebar::None;
	} else {
		_activeSidebar = target;
	}

	if (_bottomBar) {
		int sidebarId = 0;
		if (_activeSidebar == ActiveSidebar::Participants) sidebarId = 5;
		else if (_activeSidebar == ActiveSidebar::Chat) sidebarId = 6;
		else if (_activeSidebar == ActiveSidebar::Copilot) sidebarId = 8;
		_bottomBar->setActiveSidebar(sidebarId);
		if (_activeSidebar == ActiveSidebar::Chat) {
			_bottomBar->setChatUnreadCount(0);
		}
	}

	QResizeEvent ev(size(), size());
	resizeEvent(&ev);
	if (_activeSidebar == ActiveSidebar::Copilot && _copilotSidebar) {
		_copilotSidebar->focusInput();
	}
}

void MeetingRoomWindow::openCopilotSettingsDialog() {
	if (_config.openCopilotSettings) {
		_config.openCopilotSettings(this);
	}
}

void MeetingRoomWindow::updateRecoveryStateUi(OpenMeeting::MeetingState state, const QString &detail) {
	if (state == OpenMeeting::MeetingState::Reconnecting) {
		setAnnotationInteractionEnabled(false);
	} else if (state == OpenMeeting::MeetingState::InMeeting) {
		setAnnotationInteractionEnabled(true);
	} else {
		closeAnnotationOverlay();
	}
	if (!_recoveryBanner) return;

	const int stageW = _stageContainer ? _stageContainer->width() : width();


	if (_preparingMedia) {
		if (_recoveryBannerFadeTimer) _recoveryBannerFadeTimer->stop();
		MeetingUI::AppTheme::setStyleVariant(*_recoveryBanner, "meeting-room-window-recoverybanner");
		_recoveryBanner->setText(QCoreApplication::translate("MeetingUI", "Preparing audio and video devices..."));
		_recoveryBanner->show();
		_recoveryBanner->raise();
		if (_bottomBar) _bottomBar->setInRecovery(true);
	} else switch (state) {
	case OpenMeeting::MeetingState::Validating:
	case OpenMeeting::MeetingState::ConnectingRoom:
	case OpenMeeting::MeetingState::StartingLocalMedia: {
		if (_recoveryBannerFadeTimer) _recoveryBannerFadeTimer->stop();
		MeetingUI::AppTheme::setStyleVariant(*_recoveryBanner, "meeting-room-window-recoverybanner");
		_recoveryBanner->setText(QCoreApplication::translate("MeetingUI", "🔄 Connecting to the meeting..."));
		_recoveryBanner->show();
		_recoveryBanner->raise();
		if (_bottomBar) _bottomBar->setInRecovery(true);
		break;
	}
	case OpenMeeting::MeetingState::Reconnecting: {
		if (_recoveryBannerFadeTimer) _recoveryBannerFadeTimer->stop();
		_wasReconnecting = true;
		MeetingUI::AppTheme::setStyleVariant(*_recoveryBanner, "meeting-room-window-recoverybanner-2");
		_recoveryBanner->setText(QCoreApplication::translate("MeetingUI", "⚠️ Connection interrupted. Reconnecting to the meeting..."));
		_recoveryBanner->show();
		_recoveryBanner->raise();
		if (_bottomBar) _bottomBar->setInRecovery(true);
		break;
	}
	case OpenMeeting::MeetingState::InMeeting: {
		if (_bottomBar) _bottomBar->setInRecovery(false);
		if (!_mediaPreparationError.isEmpty()) {
			if (_recoveryBannerFadeTimer) _recoveryBannerFadeTimer->stop();
			MeetingUI::AppTheme::setStyleVariant(*_recoveryBanner, "meeting-room-window-recoverybanner-4");
			_recoveryBanner->setText(_mediaPreparationError);
			_recoveryBanner->show();
			_recoveryBanner->raise();
		} else if (_wasReconnecting) {
			_wasReconnecting = false;
			MeetingUI::AppTheme::setStyleVariant(*_recoveryBanner, "meeting-room-window-recoverybanner-3");
			_recoveryBanner->setText(QCoreApplication::translate("MeetingUI", "✅ Meeting connection restored"));
			_recoveryBanner->show();
			_recoveryBanner->raise();
			if (_recoveryBannerFadeTimer) {
				_recoveryBannerFadeTimer->start(1500);
			}
		} else {
			if (_recoveryBannerFadeTimer) _recoveryBannerFadeTimer->stop();
			_recoveryBanner->hide();
		}
		break;
	}
	case OpenMeeting::MeetingState::Failed: {
		if (_bottomBar) _bottomBar->setInRecovery(false);
		_wasReconnecting = false;
		if (_recoveryBannerFadeTimer) _recoveryBannerFadeTimer->stop();
		MeetingUI::AppTheme::setStyleVariant(*_recoveryBanner, "meeting-room-window-recoverybanner-4");
		_recoveryBanner->setText(QCoreApplication::translate("MeetingUI", "❌ Meeting connection failed: %1").arg(detail.isEmpty() ? QCoreApplication::translate("MeetingUI", "Network or authentication error") : detail));
		_recoveryBanner->show();
		_recoveryBanner->raise();
		break;
	}
	case OpenMeeting::MeetingState::Idle:
	default: {
		if (_bottomBar) _bottomBar->setInRecovery(false);
		_wasReconnecting = false;
		if (_recoveryBannerFadeTimer) _recoveryBannerFadeTimer->stop();
		_recoveryBanner->hide();
		break;
	}
	}
 const auto size = bannerSize(*_recoveryBanner, stageW, 420);
 _recoveryBanner->setGeometry(QRect(QPoint((stageW - size.width()) / 2, 16), size));
    if (_videoCanvas) _videoCanvas->setStageOverlay(_recoveryBanner);
}

bool MeetingRoomWindow::eventFilter(QObject *watched, QEvent *event) {
	if (watched == _encryptionPanel &&
		(event->type() == QEvent::Show || event->type() == QEvent::Hide)) {
		QResizeEvent layoutEvent(size(), size());
		resizeEvent(&layoutEvent);
	}
	return Ui::RpWidget::eventFilter(watched, event);
}

void MeetingRoomWindow::resizeEvent(QResizeEvent *e) {
	const int w = width();
	const int h = height();

 const int topBarH = _topBar->heightForWidth(w);
 const int bottomBarH = _bottomBar->heightForWidth(w);
 _topBar->setGeometry(0, 0, w, topBarH);

 QWidget *sidebar = nullptr;
 if (_activeSidebar == ActiveSidebar::Participants) {
     sidebar = _participantsSidebar;
 } else if (_activeSidebar == ActiveSidebar::Chat) {
     sidebar = _chatSidebar;
 } else if (_activeSidebar == ActiveSidebar::Copilot) {
     sidebar = _copilotSidebar;
 }
 const int sidebarW = _activeSidebar != ActiveSidebar::None && sidebar
     ? ((_activeSidebar == ActiveSidebar::Copilot && _copilotExpanded)
         ? std::min(w * 2 / 3, std::max(440, sidebar->minimumSizeHint().width()))
         : std::min(w / 2, std::max(340, sidebar->minimumSizeHint().width())))
     : 0;
	const int stageW = w - sidebarW;
    const int annotationW = _annotationButton && !_annotationButton->isHidden()
        ? std::max(80, _annotationButton->sizeHint().width()) : 0;
    const int controlStopW = _remoteControlStopButton && !_remoteControlStopButton->isHidden()
        ? std::max(100, _remoteControlStopButton->sizeHint().width()) : 0;
    const int audioW = _screenAudioButton && !_screenAudioButton->isHidden()
        ? std::max(110, _screenAudioButton->sizeHint().width()) : 0;
    if (_screenShareBanner) _screenShareBanner->setContentsMargins(
        _screenQualityButton && !_screenQualityButton->isHidden() ? 136 : 12, 4,
        12 + (annotationW ? annotationW + 8 : 0) + (controlStopW ? controlStopW + 8 : 0) +
            (audioW ? audioW + 8 : 0), 4);
	const int shareBannerH = _screenShareBanner && !_screenShareBanner->isHidden() ? std::max(44, _screenShareBanner->heightForWidth(w)) : 0;
	const int encryptionH = _encryptionPanel && !_encryptionPanel->isHidden()
		? std::max(44, _encryptionPanel->sizeHint().height()) : 0;
	if (_encryptionPanel) _encryptionPanel->setGeometry(0, topBarH + shareBannerH, w, encryptionH);
	const int stageTop = topBarH + shareBannerH + encryptionH;
	const int stageH = std::max(0, h - stageTop - bottomBarH);
	if (_screenShareBanner) _screenShareBanner->setGeometry(0, topBarH, w, shareBannerH);
    if (_screenQualityButton && shareBannerH > 0) {
        _screenQualityButton->setGeometry(12, (shareBannerH - 32) / 2, 116, 32);
        _screenQualityButton->raise();
    }
	if (_annotationButton && _annotationButton->isVisible() && shareBannerH > 0) {
		_annotationButton->adjustSize();
		const auto hint = _annotationButton->sizeHint();
		const int buttonHeight = std::min(hint.height(), std::max(1, shareBannerH - 8));
		const int buttonWidth = std::min(hint.width(), std::max(1, w - 24));
		_annotationButton->setGeometry(
			std::max(12, w - buttonWidth - 12),
			std::max(0, (shareBannerH - buttonHeight) / 2),
			buttonWidth,
			buttonHeight);
		_annotationButton->raise();
	}
    if (controlStopW && shareBannerH > 0) {
        _remoteControlStopButton->setGeometry(w - 12 - (annotationW ? annotationW + 8 : 0) - controlStopW,
            (shareBannerH - 32) / 2, controlStopW, 32);
        _remoteControlStopButton->raise();
    }
    if (audioW && shareBannerH > 0) {
        _screenAudioButton->setGeometry(w - 12 - (annotationW ? annotationW + 8 : 0) -
            (controlStopW ? controlStopW + 8 : 0) - audioW,
            (shareBannerH - 32) / 2, audioW, 32);
        _screenAudioButton->raise();
    }

	_stageContainer->setGeometry(0, stageTop, stageW, stageH);
	_bottomBar->setGeometry(0, h - bottomBarH, w, bottomBarH);
	updateStageGeometry();

	if (_activeSidebar == ActiveSidebar::Participants) {
		if (_participantsSidebar) {
			_participantsSidebar->setGeometry(stageW, stageTop, sidebarW, stageH);
			_participantsSidebar->show();
			_participantsSidebar->raise();
		}
		if (_chatSidebar) {
			_chatSidebar->hide();
		}
		if (_copilotSidebar) {
			_copilotSidebar->hide();
		}
	} else if (_activeSidebar == ActiveSidebar::Chat) {
		if (_chatSidebar) {
			_chatSidebar->setGeometry(stageW, stageTop, sidebarW, stageH);
			_chatSidebar->show();
			_chatSidebar->raise();
		}
		if (_participantsSidebar) {
			_participantsSidebar->hide();
		}
		if (_copilotSidebar) {
			_copilotSidebar->hide();
		}
	} else if (_activeSidebar == ActiveSidebar::Copilot) {
		if (_copilotSidebar) {
			_copilotSidebar->setGeometry(stageW, stageTop, sidebarW, stageH);
			_copilotSidebar->show();
			_copilotSidebar->raise();
		}
		if (_participantsSidebar) {
			_participantsSidebar->hide();
		}
		if (_chatSidebar) {
			_chatSidebar->hide();
		}
	} else {
		if (_participantsSidebar) _participantsSidebar->hide();
		if (_chatSidebar) _chatSidebar->hide();
		if (_copilotSidebar) _copilotSidebar->hide();
	}

	updateVideoLayout();
	scheduleViewportIntent(false);
}

void MeetingRoomWindow::onRemoteParticipantJoined(const QString &identity, const QString &name) {
	applyRemoteParticipantJoined(identity, name, nullptr);
}

void MeetingRoomWindow::applyRemoteParticipantJoined(const QString &identity, const QString &name,
		const OpenMeeting::ParticipantPresentation *presentation, bool relayout) {
	if (identity.isEmpty()) return;
	QPointer<MeetingRoomWindow> owner(this);
	QPointer<OpenMeeting::MeetingCoordinator> coordinator(_coordinator.get());
	const auto current = [&] {
		return owner && (!presentation || (coordinator && owner->_coordinator.get() == coordinator &&
			coordinator->isParticipantPresentationCurrent(*presentation)));
	};
	if (!current()) return;

	const std::optional<OpenMeeting::ParticipantInfo> participantInfo = presentation
		? std::optional<OpenMeeting::ParticipantInfo>(presentation->participant)
		: (_coordinator ? _coordinator->participantInfo(identity) : std::nullopt);
	const QString ownAgentId = _roomAgentPublisher
		? _roomAgentPublisher->activeAgentIdentity()
		: QString();
	if ((participantInfo && participantInfo->isAgent) ||
		(!ownAgentId.isEmpty() && identity == ownAgentId)) {
		return;
	}

	QString dispName = name.isEmpty() ? identity : name;
	if (!presentation && dispName == identity && participantInfo && !participantInfo->name.isEmpty()) {
		dispName = participantInfo->name;
	}
	_remoteParticipantNames[identity] = dispName;

	auto it = _remoteTiles.find(identity);
	if (it == _remoteTiles.end()) {
		const bool hasVisibleCamera = std::any_of(
			_acceptedVideoPlan.visible_seats.begin(),
			_acceptedVideoPlan.visible_seats.end(),
			[&](const auto &seat) {
				return seat.source != livekit::TrackSource::ScreenShareVideo &&
					QString::fromStdString(seat.key.participant.identity.empty()
						? seat.key.participant.sid
						: seat.key.participant.identity) == identity;
			});
		if (!hasVisibleCamera) {
			if (_room) {
				auto itVol = _remoteVolumes.find(identity);
				if (itVol != _remoteVolumes.end()) {
					_room->SetParticipantVolume(identity.toStdString(), itVol->second);
					if (!current()) return;
				}
				if (_locallyMutedUsers.count(identity)) {
					_room->SetParticipantMuted(identity.toStdString(), true);
					if (!current()) return;
				}
			}
			_participantCount = 1 + static_cast<int>(_remoteParticipantNames.size());
			if (_bottomBar) _bottomBar->setParticipantCount(_participantCount);
			return;
		}
		const bool usingGpuCanvas = !_whiteboardVisible &&
			_usingGpuBackend.load(std::memory_order_acquire) && _videoCanvas != nullptr;
		const auto itVol = _remoteVolumes.find(identity);
		const float savedVolume = itVol != _remoteVolumes.end() ? itVol->second : 1.0f;
		const bool savedMuted = _locallyMutedUsers.count(identity) > 0;
		QPointer<VideoTileWidget> tile;
		if (!_remoteTilePool.empty()) {
			auto recycled = std::move(_remoteTilePool.back());
			_remoteTilePool.pop_back();
			recycled->resetForReuse(dispName, savedVolume, savedMuted);
			if (_cameraSessionManager) {
				recycled->setNameDisplayMode(_cameraSessionManager->mediaPreferences().participantNameDisplay);
			}
			tile = recycled.get();
			_remoteTiles[identity] = std::move(recycled);
		} else {
			tile = new VideoTileWidget(dispName, false, _stageContainer);
			if (!current()) {
				if (tile) delete tile.data();
				return;
			}
			_remoteTiles[identity].reset(tile.data());
			tile->resetForReuse(dispName, savedVolume, savedMuted);
			bindTileInteractions(tile.data());
		}
		tile->setHardwareCanvasMode(usingGpuCanvas);
		tile->setIdentity(identity);
		tile->setRenderKey(QStringLiteral("remote-camera/") + identity);
		if (!current() || !tile) return;
		tile->setVideoActive(false);
		if (!current() || !tile) return;

		connect(tile.data(), &VideoTileWidget::remoteVolumeChanged, this, [this, identity](float volume) {
			_remoteVolumes[identity] = volume;
			if (_room) {
				_room->SetParticipantVolume(identity.toStdString(), volume);
			}
		});

		connect(tile.data(), &VideoTileWidget::remoteLocalMuteToggled, this, [this, identity](bool muted) {
			if (muted) _locallyMutedUsers.insert(identity);
			else _locallyMutedUsers.erase(identity);
			if (_room) {
				_room->SetParticipantMuted(identity.toStdString(), muted);
			}
		});

		if (!usingGpuCanvas) {
			tile->show();
		}
		if (!current() || !tile) return;
	} else {
		if (!dispName.isEmpty() && it->second && it->second->displayName() != dispName) {
			it->second->setDisplayName(dispName);
			if (!current()) return;
		}
	}

	if (!current()) return;
	if (_room) {
		auto itVol = _remoteVolumes.find(identity);
		if (itVol != _remoteVolumes.end()) {
			_room->SetParticipantVolume(identity.toStdString(), itVol->second);
			if (!current()) return;
		}
		if (_locallyMutedUsers.count(identity)) {
			_room->SetParticipantMuted(identity.toStdString(), true);
			if (!current()) return;
		}
	}

	if (auto tileIt = _remoteTiles.find(identity);
		tileIt != _remoteTiles.end() && tileIt->second && participantInfo) {
		QPointer<VideoTileWidget> tile(tileIt->second.get());
		if (!current() || !tile) return;
		tile->setAudioMuted(participantInfo->isAudioMuted);
		if (!current() || !tile) return;
		tile->setConnectionQuality(participantInfo->connectionQuality);
		if (!current() || !tile) return;
	}

	_participantCount = 1 + static_cast<int>(_remoteParticipantNames.size());
	if (_bottomBar) {
		_bottomBar->setParticipantCount(_participantCount);
		if (!current()) return;
	}

	LogToConsole(LogCategory::Participant, "USER_JOIN", QCoreApplication::translate("MeetingUI", "Remote participant joined: %1 (name: %2, participants: %3)").arg(identity).arg(dispName).arg(_participantCount));
	if (!current()) return;
	if (relayout) updateVideoLayout();
}

void MeetingRoomWindow::onRemoteParticipantLeft(const QString &identity) {
	std::vector<QString> retired;
	for (const auto &[sid, binding] : _remoteVideoBindings) {
		if (binding.identity == identity) retired.push_back(sid);
	}
	for (const auto &sid : retired) {
		removeRemoteVideo(sid);
		_remoteScreenTiles.erase(sid);
	}
	_remoteParticipantNames.erase(identity);
	if (_remoteRenderSession) {
		_remoteRenderSession->RemoveTracksForIdentity(identity.toStdString());
	}
	removeUserFromCanvases((QStringLiteral("remote-camera/") + identity).toStdString());
	auto it = _remoteTiles.find(identity);
	if (it != _remoteTiles.end()) {
		auto retired = std::move(it->second);
		_remoteTiles.erase(it);
		if (retired) {
			retired->setFrame({});
			recycleRemoteCameraTile(std::move(retired));
		}
	}

	if (_pinnedRenderKey == QStringLiteral("remote-camera/") + identity) {
		_pinnedRenderKey.clear();
		_pinnedTrackKey.reset();
	}

	_participantCount = 1 + static_cast<int>(_remoteParticipantNames.size());
	if (_bottomBar) {
		_bottomBar->setParticipantCount(_participantCount);
	}

	LogToConsole(LogCategory::Participant, "USER_LEFT", QCoreApplication::translate("MeetingUI", "Remote participant left: %1 (participants: %2)").arg(identity).arg(_participantCount));
	updateVideoLayout();
}


void MeetingRoomWindow::onRemoteTrackMuted(const QString &identity, bool isVideo, bool muted) {
	if (isVideo) {
		refreshRemoteVideoPresentations();
		return;
	}
	auto it = _remoteTiles.find(identity);
	if (it == _remoteTiles.end() || !it->second) return;
	it->second->setAudioMuted(muted);
	if (muted) it->second->setSpeaking(false, 0.0f);
	updateVideoLayout();
}

void MeetingRoomWindow::updateActiveSpeakers(const std::vector<livekit::ActiveSpeakerInfo> &speakers) {
	QString primarySpeakerName;
	std::unordered_map<std::string, float> speaking_levels;
	bool localSpeaking = false;
	float localLevel = 0.0f;

	for (const auto &spk : speakers) {
		if (spk.speaking) {
			speaking_levels[spk.sid] = spk.audio_level;
			speaking_levels[spk.identity] = spk.audio_level;
			if (spk.is_local) {
				localSpeaking = true;
				localLevel = spk.audio_level;
			}
			if (primarySpeakerName.isEmpty()) {
				primarySpeakerName = QString::fromStdString(spk.identity);
			}
		}
	}

	// 1. 顶部状态栏提示更新
	if (_topBar) {
		const auto &preferences = OpenMeeting::SessionManager::instance().mediaPreferences();
		_topBar->setActiveSpeaker(preferences.showActiveSpeaker ? primarySpeakerName : QString());
	}

	// 2. 本端画框发光光圈联动
	if (_localTile) {
		if (_config.audioMuted) {
			localSpeaking = false;
		}
		_localTile->setSpeaking(localSpeaking, localLevel);
	}

	// 3. 所有远端画框发光光圈联动
	for (auto &[id, tile] : _remoteTiles) {
		if (tile) {
			bool remoteSpeaking = false;
			float remoteLevel = 0.0f;
			auto it = speaking_levels.find(id.toStdString());
			if (it != speaking_levels.end()) {
				remoteSpeaking = true;
				remoteLevel = it->second;
			}
			tile->setSpeaking(remoteSpeaking, remoteLevel);
		}
	}
}

void MeetingRoomWindow::tryActivateGpuBackend() {
	if (_whiteboardVisible || !_videoCanvas || !_remoteRenderSession || !_remoteRenderSession->active() ||
        _usingGpuBackend.load(std::memory_order_acquire)) return;
    if (_gpuBackendActivationAttempted) {
        if (!_videoCanvas->rendererReady()) return;
    } else {
        _gpuBackendActivationAttempted = true;
        setupVideoCanvasInteractions();
        _videoCanvas->setGeometry(_stageContainer->rect());
        _videoCanvas->show();
    }

	if (!_videoCanvas->rendererReady()) {
        if (_videoCanvas->rendererPending()) return; // Qt initializes the GL context asynchronously.
		_renderDiagnostics = _videoCanvas->renderDiagnostics();
		_videoCanvas->shutdownRenderer();
		_videoCanvas->hide();
		_remoteRenderSession->UseQtCpuBackend();
        if (_sharedWindow) _sharedWindow->useCpu();
        if (_floatingListWindow) _floatingListWindow->useCpu();
		_renderDiagnostics = _videoCanvas->renderDiagnostics();
		livekit::diagnostic::Event event;
		event.kind = livekit::diagnostic::EventKind::RenderBackendChanged;
		event.thread_role = livekit::diagnostic::ThreadRole::Ui;
		event.from_render_backend = livekit::diagnostic::RenderBackend::Gpu;
		event.to_render_backend = livekit::diagnostic::RenderBackend::QtCpu;
		event.render_reason = livekit::diagnostic::RenderReason::InitializationFailed;
		if (_coordinator) event.context = _coordinator->diagnosticContext();
		livekit::diagnostic::EmitBusinessEvent(event);
		LogToConsole(LogCategory::WebRTC, "RENDER", QCoreApplication::translate("MeetingUI", "GPU canvas initialization failed. Using the Qt CPU video backend: ") +
			livekit::render::RenderDiagnosticsSafeSummary(_renderDiagnostics));
		return;
	}

	_remoteRenderSession->UseGpuBackend(
        [this](const std::string& key, livekit::render::VideoRenderFrame::Ptr frame) {
            receiveGpuVideoFrame(key, std::move(frame));
        });
	_usingGpuBackend.store(true, std::memory_order_release);
	_renderDiagnostics = _videoCanvas->renderDiagnostics();
	livekit::diagnostic::Event event;
	event.kind = livekit::diagnostic::EventKind::RenderBackendChanged;
	event.thread_role = livekit::diagnostic::ThreadRole::Ui;
	event.from_render_backend = livekit::diagnostic::RenderBackend::QtCpu;
	event.to_render_backend = livekit::diagnostic::RenderBackend::Gpu;
	event.render_reason = livekit::diagnostic::RenderReason::GpuReady;
	if (_coordinator) event.context = _coordinator->diagnosticContext();
	livekit::diagnostic::EmitBusinessEvent(event);
	LogToConsole(LogCategory::WebRTC, "RENDER", QCoreApplication::translate("MeetingUI", "GPU video backend enabled: ") +
		livekit::render::RenderDiagnosticsSafeSummary(_renderDiagnostics));
	updateVideoLayout();
}

void MeetingRoomWindow::fallBackToQtCpuBackend() {
    if (_sharedWindow) _sharedWindow->useCpu();
    if (_floatingListWindow) _floatingListWindow->useCpu();
	const bool was_using_gpu = _usingGpuBackend.exchange(false, std::memory_order_acq_rel);
	if (_remoteRenderSession) {
		_remoteRenderSession->UseQtCpuBackend();
	}
	if (_videoCanvas) {
		_renderDiagnostics = _videoCanvas->renderDiagnostics();
		_videoCanvas->shutdownRenderer();
		_videoCanvas->hide();
		_renderDiagnostics = _videoCanvas->renderDiagnostics();
	}
	if (was_using_gpu) {
		livekit::diagnostic::Event event;
		event.kind = livekit::diagnostic::EventKind::RenderBackendChanged;
		event.thread_role = livekit::diagnostic::ThreadRole::Ui;
		event.from_render_backend = livekit::diagnostic::RenderBackend::Gpu;
		event.to_render_backend = livekit::diagnostic::RenderBackend::QtCpu;
		event.render_reason = livekit::diagnostic::RenderReason::DeviceFailure;
		if (_coordinator) event.context = _coordinator->diagnosticContext();
		livekit::diagnostic::EmitBusinessEvent(event);
		LogToConsole(LogCategory::Error, "RENDER", QCoreApplication::translate("MeetingUI", "GPU presentation or device failure. Switched to the Qt CPU video backend: ") +
			livekit::render::RenderDiagnosticsSafeSummary(_renderDiagnostics));
	}
	updateVideoLayout();
}

void MeetingRoomWindow::syncVideoCanvasLayout(const std::vector<VideoTileWidget*> &tiles) {
	if (_whiteboardVisible || !_usingGpuBackend.load(std::memory_order_acquire) || !_videoCanvas) {
		return;
	}

	std::vector<livekit::render::VideoTileRect> canvas_tiles;
	canvas_tiles.reserve(tiles.size());
	for (auto *tile : tiles) {
		if (!tile) continue;
		const QRect geometry = tile->geometry();
		const auto visible = tile->layoutClipRect().translated(geometry.topLeft());
		canvas_tiles.push_back({
			tile->renderKey().toStdString(),
			geometry.x(), geometry.y(), geometry.width(), geometry.height(),
			tile->isSpeaking(), tile->audioLevel(), tile->isVideoActive() && !tile->isVideoStreamPaused(),
			livekit::render::VideoClipRect{visible.x(), visible.y(), visible.width(), visible.height()}
		});
		QPointer<VideoTileWidget> guarded(tile);
		_videoCanvas->setTileDecoration(tile->renderKey().toStdString(),
			[guarded](const QSize &pixels, bool hasFrame, bool hovered) {
				return guarded ? guarded->hardwareDecoration(pixels, hasFrame, hovered) : QImage();
			}, tile->pinButtonRect(), [guarded] {
				return guarded ? guarded->toolTip() : QString();
			});
		tile->setHardwareCanvasMode(true);
		tile->hide();
	}

	_videoCanvas->setGeometry(_stageContainer->rect());
	_videoCanvas->setTilesLayout(canvas_tiles);
	_videoCanvas->show();
	_videoCanvas->raise();
	if (_remoteControlUi) _remoteControlUi->raiseEntry();
	if (_videoPagingControls && _videoPagingControls->isVisible()) {
		_videoPagingControls->raise();
	}
	if (_inviteHintBanner) _inviteHintBanner->hide();
	if (_recoveryBanner && _recoveryBanner->isVisible()) {
		_recoveryBanner->raise();
	}
}

void MeetingRoomWindow::setupVideoCanvasInteractions() {
    _videoCanvas->setStageOverlay(_recoveryBanner);
    connect(_videoCanvas, &livekit::render::VideoCanvas::rendererInitialized,
        this, &MeetingRoomWindow::tryActivateGpuBackend,
        static_cast<Qt::ConnectionType>(Qt::QueuedConnection | Qt::UniqueConnection));
	connect(_videoCanvas, &livekit::render::VideoCanvas::tileDoubleClicked,
		this, &MeetingRoomWindow::togglePinForRenderKey, Qt::UniqueConnection);
	connect(_videoCanvas, &livekit::render::VideoCanvas::tilePinRequested,
		this, [this](const QString &key) { showVideoTileMenu(key); }, Qt::UniqueConnection);
}

void MeetingRoomWindow::bindTileInteractions(VideoTileWidget *tile) {
	if (_cameraSessionManager) {
		tile->setNameDisplayMode(_cameraSessionManager->mediaPreferences().participantNameDisplay);
	}
    connect(tile, &VideoTileWidget::moreRequested, this, [this, tile] { showVideoTileMenu(tile->renderKey()); });
	connect(tile, &VideoTileWidget::tileDoubleClicked, this, [this, tile] {
		setPinnedTile(tile->renderKey(), !tile->isPinned());
	});
	connect(tile, &VideoTileWidget::pinToggled, this, [this, tile](bool pinned) {
		setPinnedTile(tile->renderKey(), pinned);
	});
	connect(tile, &VideoTileWidget::presentationChanged, this, [this, tile] {
		const auto key = tile->renderKey().toStdString();
		const bool hasVideo = tile->isVideoActive() && !tile->isVideoStreamPaused();
		if (_floatingStripRenderKeys.count(tile->renderKey())) {
			if (_floatingListWindow) _floatingListWindow->updateTilePresentation(key, hasVideo);
		} else if (_videoCanvas) {
			_videoCanvas->updateTilePresentation(key, hasVideo);
		}
	});
}

void MeetingRoomWindow::setPinnedTile(const QString &renderKey, bool pinned) {
	if (pinned) {
		std::optional<livekit::LocalVideoTarget> local;
		if (_localTile && renderKey == _localTile->renderKey()) {
			local = livekit::LocalVideoTarget::Camera;
		} else if (_localScreenTile && renderKey == _localScreenTile->renderKey()) {
			local = livekit::LocalVideoTarget::ScreenShare;
		}
		const auto remote = local ? std::nullopt : trackKeyForRenderKey(renderKey);
		if (!local && !remote) return;
		_pinnedRenderKey = renderKey;
		_pinnedTrackKey = remote;
		_pinnedLocalTarget = local;
	} else if (_pinnedRenderKey == renderKey) {
		_pinnedRenderKey.clear();
		_pinnedTrackKey.reset();
		_pinnedLocalTarget.reset();
	}
	scheduleViewportIntent(true);
}

void MeetingRoomWindow::togglePinForRenderKey(const QString &renderKey) {
	const auto toggle = [&](VideoTileWidget *tile) {
		if (!tile || tile->renderKey() != renderKey) return false;
		setPinnedTile(tile->renderKey(), _pinnedRenderKey != tile->renderKey());
		return true;
	};
	if (toggle(_localTile) || toggle(_localScreenTile.get())) return;
	for (const auto &[id, tile] : _remoteTiles) if (toggle(tile.get())) return;
	for (const auto &[sid, tile] : _remoteScreenTiles) if (toggle(tile.get())) return;
}

void MeetingRoomWindow::setupWhiteboardBinding() {
	_bottomBar->whiteboardClicked() | rpl::on_next([this] {
		setWhiteboardVisible(!_whiteboardVisible);
	}, lifetime());
}

void MeetingRoomWindow::setupRemoteControl() {
    if (_remoteControlUi || !_coordinator || !_stageContainer) return;
    _remoteControlUi = std::make_unique<RemoteControlUi>(_stageContainer,_coordinator.get(),[this] {
        std::vector<RemoteControlTarget> targets;
        if (!_coordinator || _coordinator->state() != OpenMeeting::MeetingState::InMeeting) return targets;
        for (const auto& [sid,binding] : _remoteVideoBindings) {
            if (!binding.screen || binding.track.expired() ||
                !livekit::IsTrackTicketActive(binding.ticket, binding.key)) continue;
            auto* tile = remoteVideoTile(sid);
            QWidget* widget = nullptr;
            QRectF content;
            std::optional<QRectF> inputClip;
            if (!_whiteboardVisible && tile && canRenderRemoteVideo(sid) &&
                !(_detachedShareIntent && _detachedShareIntent->track == binding.key) &&
                !_floatingStripRenderKeys.count(tile->renderKey())) {
                if (_usingGpuBackend.load() && _videoCanvas && _videoCanvas->isVisible()) {
                    widget = _videoCanvas;
                    content = _videoCanvas->videoContentRect(tile->renderKey().toStdString());
                } else if (tile->isVisible()) {
                    widget = tile;
                    content = tile->remoteControlContentRect();
                }
                if (widget) {
                    const auto visible = tile->layoutClipRect();
                    inputClip = QRectF(widget->mapFromGlobal(tile->mapToGlobal(visible.topLeft())), visible.size());
                    if (widget == _videoCanvas) inputClip = inputClip->intersected(_videoCanvas->rect());
                    else inputClip = inputClip->intersected(QRectF(
                        tile->mapFrom(_stageContainer, QPoint()), _stageContainer->size()));
                }
            }
            const auto menuRect = tile && widget ? QRectF(tile->pinButtonRect().translated(
                widget->mapFromGlobal(tile->mapToGlobal(QPoint())))) : QRectF{};
            targets.push_back({binding.identity,sid,tile ? tile->displayName() : binding.identity,
                widget,content,inputClip,menuRect});
        }
        return targets;
    },[this](bool blocked) {
        if (_annotationOverlay) _annotationOverlay->setInteractionEnabled(!blocked && _coordinator &&
            _coordinator->state() == OpenMeeting::MeetingState::InMeeting);
        if (_annotationButton) _annotationButton->setEnabled(!blocked && _annotationBinding.has_value());
        syncScreenShareControls();
    },[this](const QString& status) { updateRemoteControlStatus(status); });
    _remoteControlUi->setTileMenuMode(true);
    connect(_coordinator.get(),&OpenMeeting::MeetingCoordinator::remoteControlChanged,this,
        [this](const livekit::remote_control::Projection& p) {
            std::string key;
            if (p.state == livekit::remote_control::State::Controlling)
                if (auto* tile = remoteVideoTile(QString::fromStdString(p.track))) key = tile->renderKey().toStdString();
            if (_videoCanvas) _videoCanvas->setRemoteInputKey(std::move(key));
            for (const auto& [sid,tile] : _remoteScreenTiles) tile->setRemoteInputEnabled(
                p.state == livekit::remote_control::State::Controlling && sid.toStdString() == p.track);
        });
}

void MeetingRoomWindow::setupVideoPagingControls() {
	if (_videoPagingControls || !_stageContainer) return;
	// A sibling row reserves real space below the native video stage.
	_videoPagingControls = new MeetingVideoPager(this);
	_videoPagingControls->hide();
	_videoPagingControls->pageRequested = [this](uint32_t page) {
		if (page >= _acceptedVideoPlan.page_count) return;
		_videoPage = page;
		scheduleViewportIntent(true);
	};
	_layoutPopup = new MeetingLayoutPopup(this);
	_topBar->setLayoutSelection(_viewMode, _speakerLayout);
	_layoutPopup->layoutSelected = [this](livekit::VideoLayoutMode mode, livekit::SpeakerLayout layout) {
		_viewMode = mode == livekit::VideoLayoutMode::Grid ? VideoViewMode::Grid : VideoViewMode::Speaker;
		if (_viewMode == VideoViewMode::Grid) {
			_pinnedTrackKey.reset();
			_pinnedLocalTarget.reset();
			_pinnedRenderKey.clear();
		}
		_speakerLayout = layout;
		_videoPage = 0;
		saveVideoLayoutPreferences();
		scheduleViewportIntent(true);
	};
	_layoutPopup->gridCapacitySelected = [this](uint32_t capacity) {
		if (capacity != 9 && capacity != 25 && capacity != 49) return;
		_viewMode = VideoViewMode::Grid;
		_pinnedTrackKey.reset();
		_pinnedLocalTarget.reset();
		_pinnedRenderKey.clear();
		_videoPageSize = capacity;
		_videoPage = 0;
		saveVideoLayoutPreferences();
		scheduleViewportIntent(true);
	};
	_layoutPopup->speakingOnlyChanged = [this](bool enabled) {
		_speakingOnly = enabled;
		_videoPage = 0;
		saveVideoLayoutPreferences();
		scheduleViewportIntent(true);
	};
	connect(_topBar, &RoomTopBarWidget::layoutMenuRequested, this, [this] {
		_layoutPopup->setSelection(_viewMode == VideoViewMode::Grid ? livekit::VideoLayoutMode::Grid
			: livekit::VideoLayoutMode::Speaker, _speakerLayout, _videoPageSize, _speakingOnly);
		_layoutPopup->showAnchored(_topBar->layoutButton());
	});
	connect(_topBar, &RoomTopBarWidget::agentMenuRequested, this, &MeetingRoomWindow::showAgentControlMenu);
	_viewportIntentTimer = new QTimer(this);
	_viewportIntentTimer->setSingleShot(true);
	connect(_viewportIntentTimer, &QTimer::timeout,
		this, &MeetingRoomWindow::submitViewportIntent);
}

void MeetingRoomWindow::saveVideoLayoutPreferences() {
	if (!_persistVideoLayoutPreferences) return;
	const std::array<QString, 5> layouts = {QStringLiteral("right"), QStringLiteral("top"),
		QStringLiteral("top-left"), QStringLiteral("bottom-left"), QStringLiteral("floating")};
	QSettings preferences;
	preferences.setValue(QStringLiteral("meeting/videoLayout"), _viewMode == VideoViewMode::Grid
		? QStringLiteral("grid") : layouts[static_cast<size_t>(_speakerLayout)]);
	preferences.setValue(QStringLiteral("meeting/gridCapacity"), _videoPageSize);
	preferences.setValue(QStringLiteral("meeting/speakingOnly"), _speakingOnly);
}

void MeetingRoomWindow::setupLiveCaptionBanner() {
	if (_liveCaptionBanner) {
		return;
	}
	_liveCaptionBanner = new QLabel(this);
	_liveCaptionBanner->setObjectName(QStringLiteral("MeetingLiveCaptionBanner"));
	_liveCaptionBanner->setTextFormat(Qt::PlainText);
	_liveCaptionBanner->setWordWrap(false);
	_liveCaptionBanner->setAlignment(Qt::AlignCenter);
	MeetingUI::AppTheme::setStyleVariant(*_liveCaptionBanner, "meeting-room-window-captionbanner");
	_liveCaptionBanner->hide();

	_liveCaptionFadeTimer = new QTimer(this);
	_liveCaptionFadeTimer->setSingleShot(true);
	connect(_liveCaptionFadeTimer, &QTimer::timeout, this, [this] {
		clearLiveCaption();
	});
}

void MeetingRoomWindow::onMeetingTranscriptionReceived(const OpenMeeting::MeetingTranscriptItem &item) {
	if (!_liveCaptionBanner || _closeRequested) {
		return;
	}
	const auto &preferences = _cameraSessionManager
		? _cameraSessionManager->mediaPreferences()
		: OpenMeeting::SessionManager::instance().mediaPreferences();
	if (!preferences.showLiveCaptions) {
		clearLiveCaption();
		return;
	}
	const QString trimmedText = item.text.trimmed();
	const QString trimmedSource = item.sourceText.trimmed();
	if (trimmedText.isEmpty() && trimmedSource.isEmpty()) {
		return;
	}
	QString speaker = item.speakerName.trimmed();
	if (speaker.isEmpty()) {
		speaker = item.speakerIdentity.trimmed();
	}
	QString prefix;
	if (item.isAgentGenerated) {
		prefix = speaker.isEmpty()
			? QCoreApplication::translate("MeetingUI", "[AI]")
			: QCoreApplication::translate("MeetingUI", "[AI] %1").arg(speaker);
	} else {
		prefix = speaker;
	}
	const bool hasDistinctTranslation = item.isTranslated &&
		!trimmedSource.isEmpty() &&
		!trimmedText.isEmpty() &&
		trimmedSource != trimmedText;
	_liveCaptionSecondaryText.clear();
	if (hasDistinctTranslation) {
		switch (preferences.liveCaptionDisplayMode) {
		case OpenMeeting::LiveCaptionDisplayMode::Bilingual:
			_liveCaptionFullText = prefix.isEmpty()
				? trimmedSource
				: QStringLiteral("%1: %2").arg(prefix, trimmedSource);
			_liveCaptionSecondaryText = trimmedText;
			break;
		case OpenMeeting::LiveCaptionDisplayMode::OriginalOnly:
			_liveCaptionFullText = prefix.isEmpty()
				? trimmedSource
				: QStringLiteral("%1: %2").arg(prefix, trimmedSource);
			break;
		case OpenMeeting::LiveCaptionDisplayMode::TranslatedOnly:
		default:
			_liveCaptionFullText = prefix.isEmpty()
				? trimmedText
				: QStringLiteral("%1: %2").arg(prefix, trimmedText);
			break;
		}
	} else {
		const QString display = trimmedText.isEmpty() ? trimmedSource : trimmedText;
		_liveCaptionFullText = prefix.isEmpty()
			? display
			: QStringLiteral("%1: %2").arg(prefix, display);
	}

	if (_liveCaptionFadeTimer) {
		_liveCaptionFadeTimer->start(6000);
	}
	const QSize previousStageSize = _stageContainer ? _stageContainer->size() : QSize();
	updateStageGeometry();
	if (_stageContainer && _stageContainer->size() != previousStageSize) {
		updateVideoLayout();
	}
}

void MeetingRoomWindow::clearLiveCaption() {
	if (_liveCaptionFadeTimer) {
		_liveCaptionFadeTimer->stop();
	}
	const bool hadCaption = !_liveCaptionFullText.isEmpty() || !_liveCaptionSecondaryText.isEmpty();
	_liveCaptionFullText.clear();
	_liveCaptionSecondaryText.clear();
	if (_liveCaptionBanner) {
		_liveCaptionBanner->clear();
		_liveCaptionBanner->hide();
	}
	if (hadCaption && !_closeRequested && _stageContainer) {
		const QSize previousStageSize = _stageContainer->size();
		updateStageGeometry();
		if (_stageContainer->size() != previousStageSize && _sessionRunning.load()) {
			updateVideoLayout();
		}
	}
}

void MeetingRoomWindow::updateStageGeometry() {
	if (!_stageContainer || !_topBar || !_bottomBar) return;
	const int stageTop = _topBar->geometry().bottom() + 1
		+ (_screenShareBanner && !_screenShareBanner->isHidden() ? _screenShareBanner->height() : 0)
		+ (_encryptionPanel && !_encryptionPanel->isHidden() ? _encryptionPanel->height() : 0);
	const int contentHeight = std::max(0, height() - stageTop - _bottomBar->height());
	const bool pagingOnStage = _videoPagingControls && _logicalWindowVisible && !isMinimized()
		&& !_whiteboardVisible && !isFloatingStripModeActive() && _acceptedVideoPlan.page_count > 1;
	const int pagerHeight = pagingOnStage ? std::min(kVideoPagerHeight, contentHeight) : 0;
	const bool captions = _liveCaptionBanner && !_liveCaptionFullText.isEmpty()
		&& _logicalWindowVisible && !isMinimized() && !_whiteboardVisible;
	const bool twoLines = captions && !_liveCaptionSecondaryText.isEmpty();
	const int captionHeight = captions ? std::min(twoLines ? 54 : 36, std::max(0, contentHeight - pagerHeight)) : 0;
	_stageContainer->setGeometry(0, stageTop, _stageContainer->width(),
		std::max(0, contentHeight - pagerHeight - captionHeight));
	if (_liveCaptionBanner) {
		const int bannerW = _stageContainer->width();
		_liveCaptionBanner->setGeometry(0, stageTop + contentHeight - pagerHeight - captionHeight,
			bannerW, captionHeight);
		if (captions && captionHeight > 0) {
			const int maxTextW = std::max(80, bannerW - 32);
			const auto fm = _liveCaptionBanner->fontMetrics();
			if (twoLines) {
				const QString line1 = fm.elidedText(_liveCaptionFullText, Qt::ElideLeft, maxTextW);
				const QString line2 = fm.elidedText(_liveCaptionSecondaryText, Qt::ElideLeft, maxTextW);
				_liveCaptionBanner->setText(line1 + QLatin1Char('\n') + line2);
				_liveCaptionBanner->setAccessibleName(_liveCaptionFullText + QLatin1Char(' ') + _liveCaptionSecondaryText);
			} else {
				_liveCaptionBanner->setText(fm.elidedText(_liveCaptionFullText, Qt::ElideLeft, maxTextW));
				_liveCaptionBanner->setAccessibleName(_liveCaptionFullText);
			}
			_liveCaptionBanner->show();
			_liveCaptionBanner->raise();
		} else {
			_liveCaptionBanner->hide();
		}
	}
	if (_videoPagingControls) {
		_videoPagingControls->setGeometry(0, stageTop + contentHeight - pagerHeight,
			_stageContainer->width(), pagerHeight);
		_videoPagingControls->setVisible(pagingOnStage);
	}
}

void MeetingRoomWindow::scheduleViewportIntent(bool immediate) {
	if (!_viewportIntentTimer) return;
	if (immediate) {
		_viewportIntentTimer->stop();
		submitViewportIntent();
	} else {
		_viewportIntentTimer->start(200);
	}
}

void MeetingRoomWindow::submitViewportIntent() {
	if (_topBar) _topBar->setLayoutSelection(_viewMode, _speakerLayout);
	syncFloatingListWindowVisibility();
	if (!_coordinator || !_stageContainer || _closeRequested) return;
	livekit::ViewportIntent intent;
	if (_cameraSessionManager) {
		intent.resolution_match = _cameraSessionManager->mediaPreferences().receiveResolutionMatch;
	}
	switch (_viewMode) {
	case VideoViewMode::Auto: intent.mode = livekit::VideoLayoutMode::Auto; break;
	case VideoViewMode::Grid: intent.mode = livekit::VideoLayoutMode::Grid; break;
	case VideoViewMode::Pip: intent.mode = livekit::VideoLayoutMode::PictureInPicture; break;
	case VideoViewMode::Speaker: intent.mode = livekit::VideoLayoutMode::Speaker; break;
	}
	intent.page = _videoPage;
	intent.page_size = _videoPageSize;
	intent.speaker_layout = _speakerLayout;
	intent.speaking_only = _speakingOnly;
	intent.local_participant_present = _localTile != nullptr;
	intent.local_screen_share_present = _localScreenTile != nullptr;
	intent.pinned = _pinnedTrackKey;
	intent.pinned_local = _pinnedLocalTarget;
	intent.stage_content = _whiteboardVisible
		? livekit::StageContent::Whiteboard
		: livekit::StageContent::Video;
	intent.stage_rect = {0, 0, _stageContainer->width(),
		std::max(0, height() - _stageContainer->y() - _bottomBar->height())};
	intent.paging_bar_height = kVideoPagerHeight;
	intent.device_pixel_ratio = devicePixelRatioF();
    intent.remote_video_enabled = _receiveRemoteVideo;
	intent.window_visible = _logicalWindowVisible;
	intent.minimized = isMinimized();
    if (_detachedShareIntent && _sharedWindow) {
        auto share = _sharedWindow->viewport();
        share.surface_id = _detachedShareIntent->surface_id;
        share.track = _detachedShareIntent->track;
        share.local = _detachedShareIntent->local;
        intent.detached_share = std::move(share);
    }
    if (_speakerLayout == livekit::SpeakerLayout::Floating) {
        if (_floatingSurfaceId == 0) ++_floatingSurfaceId;
        livekit::FloatingStripViewport strip = _floatingListWindow
            ? _floatingListWindow->viewport()
            : livekit::FloatingStripViewport{};
        strip.surface_id = _floatingSurfaceId;
        intent.floating_strip = std::move(strip);
    }
	if (_lastViewportIntent &&
		SameViewportIntentContent(*_lastViewportIntent, intent)) {
		return;
	}
	intent.view_revision = ++_viewportRevision;
	if (intent.view_revision == 0) intent.view_revision = ++_viewportRevision;
	_lastViewportIntent = intent;
	_coordinator->submitViewportIntent(std::move(intent));
}

void MeetingRoomWindow::setWhiteboardVisible(bool visible) {
	if (visible && !_whiteboardPanel) {
		_whiteboardPanel = new WhiteboardPanel(_stageContainer);
		connect(_whiteboardPanel, &WhiteboardPanel::closeRequested, this, [this] {
			setWhiteboardVisible(false);
		});
		connect(_whiteboardPanel->canvas(), &WhiteboardCanvas::documentChanged, this, [this] {
			_bottomBar->setWhiteboardHasContent(_whiteboardPanel->document().hasContent());
		});
		if (_coordinator) {
			connect(_whiteboardPanel, &WhiteboardPanel::commandProposed,
				_coordinator.get(), &OpenMeeting::MeetingCoordinator::submitWhiteboardCommand);
			connect(_whiteboardPanel, &WhiteboardPanel::imageProposed,
				_coordinator.get(), &OpenMeeting::MeetingCoordinator::submitWhiteboardImage);
			connect(_whiteboardPanel, &WhiteboardPanel::lockRequested,
				_coordinator.get(), &OpenMeeting::MeetingCoordinator::setWhiteboardLocked);
			connect(_whiteboardPanel, &WhiteboardPanel::writersOpenRequested,
				_coordinator.get(), &OpenMeeting::MeetingCoordinator::setWhiteboardWritersOpen);
			connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::whiteboardProjectionChanged,
				_whiteboardPanel, &WhiteboardPanel::applyProjection);
			connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::whiteboardCommandRejected,
				_whiteboardPanel->canvas(), [canvas = _whiteboardPanel->canvas()](const QString &id) {
					canvas->rejectCommand(id.toStdString());
				});
			if (_coordinator->state() == OpenMeeting::MeetingState::InMeeting ||
				_coordinator->state() == OpenMeeting::MeetingState::Reconnecting) {
				_whiteboardPanel->enableCollaboration();
				_coordinator->activateWhiteboard();
			}
		}
	}
	_whiteboardVisible = visible;
	updateStageGeometry();
	if (!visible && _whiteboardPanel) _whiteboardPanel->hide();
	scheduleViewportIntent(true);
	updateVideoLayout();
	if (visible) _whiteboardPanel->canvas()->setFocus(Qt::OtherFocusReason);
	else tryActivateGpuBackend();
}

void MeetingRoomWindow::showVideoTileMenu(const QString& renderKey, const QPoint& globalAnchor) {
    if (!_coordinator || _coordinator->state() != OpenMeeting::MeetingState::InMeeting || _closeRequested) return;
    VideoTileWidget* tile = nullptr;
    bool local = false, screenShare = false;
    if (_localTile && _localTile->renderKey() == renderKey) { tile = _localTile; local = true; }
    if (_localScreenTile && _localScreenTile->renderKey() == renderKey) {
        tile = _localScreenTile.get(); local = true; screenShare = true;
    }
    const auto key = local ? std::optional<livekit::TrackKey>{} : trackKeyForRenderKey(renderKey);
    if (key) {
        tile = remoteVideoTile(*key);
        const auto* seat = _acceptedVideoPlan.FindMediaSeat(*key);
        screenShare = seat && seat->source == livekit::TrackSource::ScreenShareVideo;
    }
    if (!tile) return;
    QPointer<VideoTileWidget> guarded(tile);
    const auto currentParticipant = [this, local, key]() -> std::optional<OpenMeeting::ParticipantInfo> {
        if (!_coordinator) return std::nullopt;
        for (const auto& person : _coordinator->participants()) {
            if ((local && person.isLocal) || (!local && key && person.participantKey == key->participant)) return person;
        }
        return std::nullopt;
    };
    const auto current = [this, guarded, renderKey, key, local] {
        return guarded && _coordinator && !_closeRequested &&
            _coordinator->state() == OpenMeeting::MeetingState::InMeeting &&
            (local || trackKeyForRenderKey(renderKey) == key);
    };
    const auto target = currentParticipant();
    const auto canManage = [this, local] { return local || (_coordinator && _coordinator->isHost()); };
    auto* menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("videoTileActions"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    AppTheme::styleMenu(*menu, AppTheme::Tone::Dark);
    const auto add = [this, menu, current](const QString& text, const char* id, auto action) {
        auto* entry = menu->addAction(text); entry->setObjectName(QString::fromLatin1(id));
        connect(entry, &QAction::triggered, this, [current, action] { if (current()) action(); });
    };
    if (target && canManage()) {
        const bool muted = local ? _pendingAudioMuted.value_or(_config.audioMuted) : target->isAudioMuted;
        if (!muted || target->permissions.can_publish) {
            add(muted ? (local ? tr("Unmute") : tr("Request unmute")) : tr("Mute"), "tileMicrophone",
                [this, local, muted, currentParticipant, canManage] {
                    const auto now = currentParticipant();
                    if (!now || !canManage() || (muted && !now->permissions.can_publish)) return;
                    if (local) requestLocalMicrophone(!muted);
                    else _coordinator->requestParticipantMicrophone(now->identity, muted);
                });
        }
        const bool enabled = local ? _pendingVideoEnabled.value_or(_config.videoEnabled) : target->isVideoEnabled;
        if (!screenShare && (enabled || target->permissions.can_publish)) {
            add(enabled ? tr("Stop video") : (local ? tr("Start video") : tr("Request video")), "tileCamera",
                [this, local, enabled, currentParticipant, canManage] {
                    const auto now = currentParticipant();
                    if (!now || !canManage() || (!enabled && !now->permissions.can_publish)) return;
                    if (local) requestLocalCamera(!enabled);
                    else _coordinator->requestParticipantCamera(now->identity, !enabled);
                });
        }
    }
    if (!local && key && target && screenShare && _remoteControlUi) {
        const auto identity = target->identity;
        const auto sid = QString::fromStdString(key->publication_sid);
        if (_remoteControlUi->controlsTarget(identity, sid)) {
            add(tr("End remote control"), "tileRemoteControl", [this, identity, sid] {
                if (_remoteControlUi && _remoteControlUi->controlsTarget(identity, sid)) _remoteControlUi->stop();
            });
        } else if (target->permissions.can_publish_data && _remoteControlUi->canRequestTarget(identity, sid)) {
            add(tr("Remote control"), "tileRemoteControl", [this, identity, sid, currentParticipant] {
                const auto now = currentParticipant();
                if (now && now->permissions.can_publish_data && _remoteControlUi) _remoteControlUi->requestTarget(identity, sid);
            });
        } else if (const auto hint = _remoteControlUi->capabilityHint(); !hint.isEmpty()) {
            auto* unavailable = menu->addAction(tr("Remote control"));
            unavailable->setObjectName(QStringLiteral("tileRemoteControlUnavailable"));
            unavailable->setEnabled(false);
            unavailable->setToolTip(hint);
            menu->setToolTipsVisible(true);
        }
    }
    if (local && screenShare && _remoteControlUi) {
        if (const auto hint = _remoteControlUi->capabilityHint(true); !hint.isEmpty()) {
            auto* unavailable = menu->addAction(hint);
            unavailable->setObjectName(QStringLiteral("tileRemoteControlHostUnavailable"));
            unavailable->setEnabled(false);
        }
    }
    const bool supported = !_videoCanvas ||
        _videoCanvas->renderDiagnostics().requested_backend != livekit::render::RenderBackend::OpenGL;
    const auto mayReceive = [this] {
        for (const auto& person : _coordinator->participants())
            if (person.isLocal) return person.permissions.can_subscribe;
        return true;
    };
    const bool permittedShare = local || (mayReceive() && key && remoteVideoSubscriptionError(*key) ==
        livekit::TrackPublication::SubscriptionError::None);
    if (screenShare && supported && permittedShare && !_sharedReturnPending &&
        (!_remoteControlUi || !_remoteControlUi->active())) {
        add(tr("Open in separate window"), "tilePopout", [this, key, local, mayReceive] {
            if (local || (mayReceive() && key && remoteVideoSubscriptionError(*key) ==
                livekit::TrackPublication::SubscriptionError::None)) detachSharedScreen(key, local);
        });
    }
    if (!menu->isEmpty()) menu->addSeparator();
    menu->addSection(tr("For my layout"));
    const bool pinned = _pinnedRenderKey == renderKey;
    add(pinned ? tr("Unlock this view") : tr("Lock this view"), "tilePin", [this, renderKey, pinned] {
        setPinnedTile(renderKey, !pinned);
    });
    if (_receiveRemoteVideo || mayReceive()) add(_receiveRemoteVideo ? tr("Stop receiving all other video") : tr("Resume receiving other video"),
        "tileReceiveVideo", [this, mayReceive] {
            if (!_receiveRemoteVideo && !mayReceive()) return;
            _receiveRemoteVideo = !_receiveRemoteVideo;
            scheduleViewportIntent(true);
        });
    // Rebuild on the next open if authority/lifetime changes while the menu is
    // visible. Each callback also revalidates the exact target and authority.
    connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::hostRoleChanged, menu, &QMenu::close);
    connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::participantPermissionsChanged, menu, &QMenu::close);
    connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::participantLeft, menu, &QMenu::close);
    connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::remoteVideoTrackUnavailable, menu, &QMenu::close);
    const QPoint popupPos = !globalAnchor.isNull()
        ? globalAnchor
        : (tile->isVisible()
            ? tile->mapToGlobal(tile->pinButtonRect().bottomLeft())
            : (_stageContainer
                ? _stageContainer->mapToGlobal(tile->geometry().topLeft() + tile->pinButtonRect().bottomLeft())
                : QCursor::pos()));
    menu->popup(popupPos);
}

void MeetingRoomWindow::detachSharedScreen(std::optional<livekit::TrackKey> key, bool local) {
    if (!_coordinator || _closeRequested ||
        _coordinator->state() != OpenMeeting::MeetingState::InMeeting ||
        (_remoteControlUi && _remoteControlUi->active()) ||
        (local ? (!_localScreenTile || key.has_value()) : !key.has_value())) return;
    if (_sharedReturnPending) return;
    if (_detachedShareIntent && _detachedShareIntent->local == local && _detachedShareIntent->track == key) {
        if (_sharedWindow) _sharedWindow->locate();
        return;
    }
    QString name = tr("You");
    if (key) {
        const auto* seat = _acceptedVideoPlan.FindMediaSeat(*key);
        if (!seat || seat->source != livekit::TrackSource::ScreenShareVideo) return;
        const auto identity = QString::fromStdString(key->participant.identity);
        const auto found = _remoteParticipantNames.find(identity);
        name = found != _remoteParticipantNames.end() ? found->second : identity;
    }
    // DX11 uses an independent canvas/device. Never switch the meeting to CPU
    // merely because a second surface is requested. ANGLE multi-owner support
    // is a separate backend capability; do not silently degrade it here.
    const auto backend = _videoCanvas ? _videoCanvas->renderDiagnostics().requested_backend
        : livekit::render::RenderBackend::QtCpu;
    if (backend == livekit::render::RenderBackend::OpenGL) return;
    if (!_sharedWindow) {
        const bool gpu = _videoCanvas && backend == livekit::render::RenderBackend::Dx11 &&
            (!_gpuBackendActivationAttempted || _videoCanvas->rendererReady() || _videoCanvas->rendererPending());
        _sharedWindow = std::make_unique<SharedScreenWindow>(gpu);
        _sharedWindow->setAttribute(Qt::WA_DontShowOnScreen, testAttribute(Qt::WA_DontShowOnScreen));
        connect(_sharedWindow.get(), &SharedScreenWindow::returnRequested,
            this, &MeetingRoomWindow::returnSharedScreen, Qt::QueuedConnection);
        connect(_sharedWindow.get(), &SharedScreenWindow::viewportChanged,
            this, [this] {
                updateLocalScreenPreviewDemand();
                scheduleViewportIntent();
            });
        connect(_sharedWindow.get(), &SharedScreenWindow::rendererUnavailable,
            this, &MeetingRoomWindow::fallBackToQtCpuBackend, Qt::QueuedConnection);
        connect(_sharedWindow.get(), &SharedScreenWindow::shareQualityRequested, this, [this] {
            if (_detachedShareIntent && _detachedShareIntent->local && _screenQualityButton)
                _screenQualityButton->click();
        });
        connect(_sharedWindow.get(), &SharedScreenWindow::annotationRequested, this, [this] {
            if (_detachedShareIntent && _detachedShareIntent->local && _annotationButton)
                _annotationButton->click();
        });
        const auto* targetScreen = screen();
        if (targetScreen && !_sharedWindow->restoredGeometry()) {
            const auto area = targetScreen->availableGeometry();
            _sharedWindow->resize(std::min(960, area.width()), std::min(600, area.height()));
            _sharedWindow->move(area.center() - _sharedWindow->rect().center());
        }
    }
    livekit::DetachedShareViewport intent;
    intent.surface_id = ++_nextSharedSurfaceId;
    if (!intent.surface_id) intent.surface_id = ++_nextSharedSurfaceId;
    intent.track = std::move(key);
    intent.local = local;
    _detachedShareIntent = std::move(intent);
    _sharedWindow->setSharingName(name);
    _sharedWindow->setStatus(tr("Waiting for shared screen…"), true);
    syncScreenShareControls();
    _sharedWindow->show();
    _sharedWindow->raise();
    scheduleViewportIntent(true);
}

void MeetingRoomWindow::returnSharedScreen() {
    if (!_detachedShareIntent || _closeRequested) return;
    // The old accepted route remains active until the single replacement plan
    // has returned the publication to the main stage.
    const auto key = _detachedShareIntent->track;
    const bool local = _detachedShareIntent->local;
    _detachedShareIntent.reset();
    _sharedReturnPending = true;
    syncScreenShareControls();
    if (key) {
        _pinnedTrackKey = key;
        _pinnedLocalTarget.reset();
        _pinnedRenderKey = QStringLiteral("remote-screen/") + QString::fromStdString(key->publication_sid);
    }
    else if (local && _localScreenTile) {
        _pinnedTrackKey.reset();
        _pinnedLocalTarget = livekit::LocalVideoTarget::ScreenShare;
        _pinnedRenderKey = _localScreenTile->renderKey();
    }
    if (isMinimized()) showNormal();
    else show();
    raise();
    activateWindow();
    if (_whiteboardVisible) setWhiteboardVisible(false);
    scheduleViewportIntent(true);
}

bool MeetingRoomWindow::isSharedScreenVisible() const {
    return _sharedWindow && _sharedWindow->viewing() &&
        (!_detachedShareIntent || _detachedShareIntent->surface_id == _acceptedVideoPlan.detached_surface_id) &&
        _acceptedVideoPlan.detached_surface_id &&
        _displayedSharedSurfaceId == _acceptedVideoPlan.detached_surface_id;
}

bool MeetingRoomWindow::isDetachedShareFrame(const QString& key) const {
    if (!isSharedScreenVisible()) return false;
    if (_acceptedVideoPlan.show_detached_local && _localScreenTile)
        return key == _localScreenTile->renderKey();
    return _acceptedVideoPlan.detached_share &&
        key.toStdString() == _acceptedVideoPlan.detached_share->key.publication_sid;
}

void MeetingRoomWindow::syncSharedScreenPlan() {
    if (!_sharedWindow) return;
    const auto& plan = _acceptedVideoPlan;
    if (!plan.detached_surface_id) {
        _displayedSharedSurfaceId = 0;
        if (_sharedReturnPending) {
            _sharedReturnPending = false;
            _sharedWindow->hide();
        }
        return;
    }
    if (_displayedSharedSurfaceId != plan.detached_surface_id) {
        _sharedWindow->clearFrame();
        _displayedSharedSurfaceId = plan.detached_surface_id;
    }
    if (!plan.detached_share && !plan.detached_local) {
        _sharedWindow->setStatus(tr("Screen sharing has ended."), true);
    } else if (plan.detached_share && std::find(plan.selected_video.begin(), plan.selected_video.end(),
                   plan.detached_share->key) == plan.selected_video.end()) {
        _sharedWindow->setStatus(tr("Waiting for shared screen…"), true);
    }
}

void MeetingRoomWindow::removeUserFromCanvases(const std::string& key) {
    if (_videoCanvas) _videoCanvas->removeUser(key);
    if (_floatingListWindow) _floatingListWindow->removeUser(key);
}

bool MeetingRoomWindow::isFloatingStripModeActive() const {
    if (_acceptedVideoPlan.coordinator_session == 0) {
        return _viewMode == VideoViewMode::Speaker &&
            _speakerLayout == livekit::SpeakerLayout::Floating;
    }
    return _acceptedVideoPlan.floating_strip_active;
}

bool MeetingRoomWindow::isFloatingListVisible() const {
    return isFloatingStripModeActive() && _floatingListWindow && _floatingListWindow->viewing();
}

void MeetingRoomWindow::dockFloatingMemberList() {
    if (_closeRequested) return;
    _viewMode = VideoViewMode::Speaker;
    _speakerLayout = livekit::SpeakerLayout::Right;
    _videoPage = 0;
    saveVideoLayoutPreferences();
    syncFloatingListWindowVisibility();
    updateStageGeometry();
    scheduleViewportIntent(true);
    updateVideoLayout();
}

void MeetingRoomWindow::syncFloatingListWindowVisibility() {
    const bool hostAlive = !_closeRequested && !_closingForSessionInvalidation &&
        (!isHidden() || isMinimized());
    const bool explicitFloating = _viewMode == VideoViewMode::Speaker &&
        _speakerLayout == livekit::SpeakerLayout::Floating;
    const bool shouldShow = hostAlive && (explicitFloating || _acceptedVideoPlan.floating_strip_active);

    if (!shouldShow) {
        if (_floatingListWindow && _floatingListWindow->isVisible()) {
            _floatingListWindow->hide();
        }
        return;
    }

    if (!_floatingListWindow) {
        const auto backend = _videoCanvas
            ? _videoCanvas->renderDiagnostics().requested_backend
            : livekit::render::RenderBackend::QtCpu;
        bool gpu = false;
#if defined(Q_OS_WIN)
        if (backend == livekit::render::RenderBackend::OpenGL) {
            // Windows EGL/ANGLE serializes a single context owner process-wide.
            // Switch cleanly to QtCpu so all surfaces share one mutually-exclusive backend.
            fallBackToQtCpuBackend();
        } else if (backend == livekit::render::RenderBackend::Dx11) {
            gpu = _videoCanvas &&
                (!_gpuBackendActivationAttempted || _videoCanvas->rendererReady() || _videoCanvas->rendererPending());
        }
#else
        if (backend == livekit::render::RenderBackend::OpenGL ||
            backend == livekit::render::RenderBackend::Dx11) {
            gpu = _videoCanvas &&
                (!_gpuBackendActivationAttempted || _videoCanvas->rendererReady() || _videoCanvas->rendererPending());
        }
#endif
        _floatingListWindow = std::make_unique<FloatingMemberListWindow>(gpu, _persistVideoLayoutPreferences);
        _floatingListWindow->setAttribute(Qt::WA_DontShowOnScreen, testAttribute(Qt::WA_DontShowOnScreen));
        if (++_floatingSurfaceId == 0) ++_floatingSurfaceId;

        connect(_floatingListWindow.get(), &FloatingMemberListWindow::dockRequested,
            this, &MeetingRoomWindow::dockFloatingMemberList, Qt::QueuedConnection);
        connect(_floatingListWindow.get(), &FloatingMemberListWindow::rendererUnavailable,
            this, &MeetingRoomWindow::fallBackToQtCpuBackend, Qt::QueuedConnection);
        connect(_floatingListWindow.get(), &FloatingMemberListWindow::pageRequested,
            this, [this](uint32_t page) {
                if (page >= _acceptedVideoPlan.page_count) return;
                _videoPage = page;
                scheduleViewportIntent(true);
            });
        connect(_floatingListWindow.get(), &FloatingMemberListWindow::tileDoubleClicked,
            this, &MeetingRoomWindow::togglePinForRenderKey);
        connect(_floatingListWindow.get(), &FloatingMemberListWindow::tileMenuRequested,
            this, [this](const QString& renderKey, const QPoint& globalPos) {
                showVideoTileMenu(renderKey, globalPos);
            });
        connect(_floatingListWindow.get(), &FloatingMemberListWindow::viewportChanged,
            this, [this] {
                if (!_floatingListWindow) return;
                const bool collapsed = _floatingListWindow->isCollapsed();
                const bool minimized = _floatingListWindow->isMinimized();
                const bool stateChanged = (collapsed != _lastFloatingCollapsed) ||
                    (minimized != _lastFloatingMinimized);
                _lastFloatingCollapsed = collapsed;
                _lastFloatingMinimized = minimized;
                updateLocalScreenPreviewDemand();
                scheduleViewportIntent(stateChanged);
            });

        _floatingListWindow->placeBeside(this);
        _lastFloatingCollapsed = _floatingListWindow->isCollapsed();
        _lastFloatingMinimized = _floatingListWindow->isMinimized();
    }

    if (!_floatingListWindow->isVisible()) {
        _floatingListWindow->setPage(_acceptedVideoPlan.page,
            std::max<uint32_t>(1, _acceptedVideoPlan.page_count));
        _floatingListWindow->show();
        _floatingListWindow->raise();
    }
}

void MeetingRoomWindow::updateFloatingListLayout(VideoTileWidget* pinnedTile) {
    const bool floatingMode = isFloatingStripModeActive();
    if (!floatingMode || !_floatingListWindow) {
        if (!_floatingStripRenderKeys.empty()) {
            _floatingStripRenderKeys.clear();
            if (_localTile) _localTile->setExternalCpuSurfaceState(false, false, false, {});
            if (_localScreenTile) _localScreenTile->setExternalCpuSurfaceState(false, false, false, {});
            for (auto& [_, tile] : _remoteTiles) {
                if (tile) tile->setExternalCpuSurfaceState(false, false, false, {});
            }
            for (auto& [_, tile] : _remoteScreenTiles) {
                if (tile) tile->setExternalCpuSurfaceState(false, false, false, {});
            }
        }
        if (_floatingListWindow) _floatingListWindow->clearFrames();
        return;
    }

    _floatingListWindow->setPage(_acceptedVideoPlan.page,
        std::max<uint32_t>(1, _acceptedVideoPlan.page_count));

    const bool awaitingPlan = _acceptedVideoPlan.coordinator_session == 0;
    const bool stripViewing = _floatingListWindow->viewing();
    const bool stripGpu = _floatingListWindow->usesGpu() &&
        _usingGpuBackend.load(std::memory_order_acquire);

    struct StripItem {
        VideoTileWidget* tile = nullptr;
        QRect rect;
    };
    std::vector<StripItem> items;
    if (!awaitingPlan && stripViewing) {
        const auto toQRect = [](const livekit::LogicalViewport& vp) {
            return QRect(vp.x, vp.y, std::max(0, vp.width), std::max(0, vp.height));
        };
        if (_localTile && _acceptedVideoPlan.show_local_participant &&
            _acceptedVideoPlan.local_participant_in_floating_strip &&
            _acceptedVideoPlan.local_participant_rect) {
            items.push_back({_localTile, toQRect(*_acceptedVideoPlan.local_participant_rect)});
        }
        for (const auto& seat : _acceptedVideoPlan.visible_seats) {
            if (seat.role == livekit::VideoSeatRole::Main) continue;
            if (auto* tile = remoteVideoTile(seat.key)) {
                items.push_back({tile, toQRect(seat.rect)});
            }
        }
        if (_localScreenTile && _acceptedVideoPlan.show_local_screen_share &&
            _acceptedVideoPlan.local_screen_share_in_floating_strip &&
            _acceptedVideoPlan.local_screen_share_rect) {
            items.push_back({_localScreenTile.get(), toQRect(*_acceptedVideoPlan.local_screen_share_rect)});
        }
    }

    std::set<QString> nextKeys;
    std::vector<FloatingMemberSlot> memberSlots;
    memberSlots.reserve(items.size());
    for (const auto& item : items) {
        auto* tile = item.tile;
        if (!tile || item.rect.isEmpty()) continue;
        nextKeys.insert(tile->renderKey());
        if (_videoCanvas) _videoCanvas->removeUser(tile->renderKey().toStdString());
        tile->setPinned(tile == pinnedTile);
        tile->setPipMode(true);
        tile->resize(item.rect.size());
        tile->setLayoutClip(QRect(QPoint(0, 0), item.rect.size()));
        tile->setHardwareCanvasMode(stripGpu);
        tile->setExternalCpuSurfaceState(
            !stripGpu, stripViewing, _floatingListWindow->isMinimized(), item.rect.size());
        if (tile->isVisible()) tile->hide();

        QPointer<VideoTileWidget> guarded(tile);
        FloatingMemberSlot slot;
        slot.renderKey = tile->renderKey();
        slot.rect = item.rect;
        slot.isSpeaking = tile->isSpeaking();
        slot.audioLevel = tile->audioLevel();
        slot.hasVideo = tile->isVideoActive() && !tile->isVideoStreamPaused();
        slot.pinRect = tile->pinButtonRect();
        slot.decorationPainter = [guarded](const QSize& pixels, bool hasFrame, bool hovered) {
            return guarded ? guarded->hardwareDecoration(pixels, hasFrame, hovered) : QImage();
        };
        slot.toolTipProvider = [guarded] {
            return guarded ? guarded->toolTip() : QString();
        };
        slot.cpuPainter = [guarded](QPainter& painter, const QRect& targetRect, bool hovered) {
            if (guarded) guarded->paintOnExternalSurface(painter, targetRect, hovered);
        };
        memberSlots.push_back(std::move(slot));
    }

    const auto resetIfRemoved = [&](VideoTileWidget* tile) {
        if (tile && !nextKeys.count(tile->renderKey())) {
            tile->setExternalCpuSurfaceState(false, false, false, {});
        }
    };
    resetIfRemoved(_localTile);
    resetIfRemoved(_localScreenTile.get());
    for (auto& [_, tile] : _remoteTiles) resetIfRemoved(tile.get());
    for (auto& [_, tile] : _remoteScreenTiles) resetIfRemoved(tile.get());
    _floatingStripRenderKeys = std::move(nextKeys);

    int totalListCandidates = static_cast<int>(_remoteParticipantNames.size()) +
        static_cast<int>(_remoteScreenTiles.size()) +
        (_localTile ? 1 : 0) + (_localScreenTile ? 1 : 0);
    if (isVideoStageVisible() && focusedVideoTile() != nullptr && totalListCandidates > 0) {
        --totalListCandidates;
    }
    _floatingListWindow->setMemberCount(
        _speakingOnly
            ? static_cast<int>(memberSlots.size())
            : std::max<int>(static_cast<int>(memberSlots.size()), totalListCandidates));

    const QString emptyHint = memberSlots.empty()
        ? (_speakingOnly
            ? tr("No speaking members right now")
            : tr("No other video participants"))
        : QString();
    _floatingListWindow->setSlots(std::move(memberSlots), emptyHint);
}

bool MeetingRoomWindow::isVideoStageVisible() const {
	return _logicalWindowVisible && !isMinimized() && !_whiteboardVisible &&
		_stageContainer && _stageContainer->width() > 0 &&
		_stageContainer->height() > 0;
}

bool MeetingRoomWindow::localScreenPreviewVisible() const {
    return _localScreenTile &&
        ((isVideoStageVisible() && (_acceptedVideoPlan.coordinator_session == 0 ||
          (_acceptedVideoPlan.show_local_screen_share &&
           !_acceptedVideoPlan.local_screen_share_in_floating_strip))) ||
         (isFloatingListVisible() && _acceptedVideoPlan.show_local_screen_share &&
          _acceptedVideoPlan.local_screen_share_in_floating_strip) ||
         (isSharedScreenVisible() && _acceptedVideoPlan.show_detached_local));
}

void MeetingRoomWindow::updateLocalScreenPreviewDemand() {
    if (_localScreenPreview && !localScreenPreviewVisible()) {
        _localScreenPreview->DiscardLatest("screen", _localScreenPreview->generation());
    }
}

void MeetingRoomWindow::updateVideoLayout() {
    syncFloatingListWindowVisibility();
    updateLocalScreenPreviewDemand();
	const bool stageVisible = isVideoStageVisible();
	const bool awaitingPlan = _acceptedVideoPlan.coordinator_session == 0;
	const bool floatingMode = isFloatingStripModeActive();
	const bool floatingVisible = isFloatingListVisible();
	const bool showLocalOnStage = stageVisible && (awaitingPlan
		|| (_acceptedVideoPlan.show_local_participant &&
		    (!floatingMode || !_acceptedVideoPlan.local_participant_in_floating_strip)));
	const bool showLocalInStrip = floatingMode && floatingVisible && !awaitingPlan &&
		_acceptedVideoPlan.show_local_participant &&
		_acceptedVideoPlan.local_participant_in_floating_strip;
	const bool showLocalScreenOnStage = stageVisible && ((awaitingPlan && (!floatingMode || !showLocalOnStage))
		|| (!awaitingPlan && _acceptedVideoPlan.show_local_screen_share &&
		    (!floatingMode || !_acceptedVideoPlan.local_screen_share_in_floating_strip)));
    if (_remoteRenderSession && _localVideoSource) {
        if (_config.videoEnabled && (showLocalOnStage || showLocalInStrip)) {
            _remoteRenderSession->AttachLocalSource(_localVideoSource);
        } else {
            _remoteRenderSession->DetachLocalSource();
            removeUserFromCanvases("local");
            if (_localTile) _localTile->setFrame({});
        }
    }

	VideoTileWidget *pinnedTile = nullptr;
	if (_acceptedVideoPlan.pinned_local == livekit::LocalVideoTarget::Camera) {
		pinnedTile = _localTile;
	} else if (_acceptedVideoPlan.pinned_local == livekit::LocalVideoTarget::ScreenShare) {
		pinnedTile = _localScreenTile.get();
	} else if (_acceptedVideoPlan.pinned) {
		pinnedTile = remoteVideoTile(*_acceptedVideoPlan.pinned);
	}
	updateFloatingListLayout(pinnedTile);

	const int stageW = _stageContainer->width();
	const int stageH = _stageContainer->height();
	if (stageW <= 0 || stageH <= 0) return;

	std::vector<VideoTileWidget*> allTiles;
	if (_localTile && showLocalOnStage) {
		allTiles.push_back(_localTile);
	} else if (_localTile) {
		_localTile->hide();
		if (_videoCanvas) _videoCanvas->removeUser(_localTile->renderKey().toStdString());
	}
	const bool gallery = !floatingMode && (_acceptedVideoPlan.mode == livekit::VideoLayoutMode::Grid ||
		_acceptedVideoPlan.mode == livekit::VideoLayoutMode::Auto);
	if (_localScreenTile && showLocalScreenOnStage && gallery) {
		allTiles.push_back(_localScreenTile.get());
	} else if (_localScreenTile && !showLocalScreenOnStage) {
		_localScreenTile->hide();
		if (_videoCanvas) _videoCanvas->removeUser(_localScreenTile->renderKey().toStdString());
	}
	for (const auto &seat : _acceptedVideoPlan.visible_seats) {
		if (floatingMode && seat.role != livekit::VideoSeatRole::Main) continue;
		if (auto *tile = remoteVideoTile(seat.key);
			tile && std::find(allTiles.begin(), allTiles.end(), tile) == allTiles.end()) {
			allTiles.push_back(tile);
		}
	}
	if (_localScreenTile && showLocalScreenOnStage && !gallery) allTiles.push_back(_localScreenTile.get());
	for (auto &[_, tile] : _remoteTiles) {
		if (tile && std::find(allTiles.begin(), allTiles.end(), tile.get()) == allTiles.end()) {
			tile->hide();
			if (_videoCanvas) _videoCanvas->removeUser(tile->renderKey().toStdString());
		}
	}
	for (auto &[_, tile] : _remoteScreenTiles) {
		if (tile && std::find(allTiles.begin(), allTiles.end(), tile.get()) == allTiles.end()) {
			tile->hide();
			if (_videoCanvas) _videoCanvas->removeUser(tile->renderKey().toStdString());
		}
	}
	if (!isVideoStageVisible()) {
		for (auto *tile : allTiles) if (tile) tile->hide();
		for (auto &[_, tile] : _remoteTiles) if (tile) tile->hide();
		for (auto &[_, tile] : _remoteScreenTiles) if (tile) tile->hide();
		if (_localTile) _localTile->hide();
		if (_localScreenTile) _localScreenTile->hide();
		if (_videoCanvas) _videoCanvas->hide();
		if (_inviteHintBanner) _inviteHintBanner->hide();
		if (_videoPagingControls) _videoPagingControls->hide();
		if (_whiteboardVisible && _whiteboardPanel) {
			_whiteboardPanel->setGeometry(_stageContainer->rect());
			_whiteboardPanel->show();
			_whiteboardPanel->raise();
		}
		if (_recoveryBanner && _recoveryBanner->isVisible()) {
			const auto size = bannerSize(*_recoveryBanner, stageW, 420);
			_recoveryBanner->setGeometry((stageW - size.width()) / 2, 16, size.width(), size.height());
			_recoveryBanner->raise();
		}
		return;
	}
	if (_whiteboardPanel) _whiteboardPanel->hide();
	const bool usingGpuCanvas = !_whiteboardVisible &&
		_usingGpuBackend.load(std::memory_order_acquire) && _videoCanvas != nullptr;
	for (auto *tile : allTiles) {
		tile->setPinned(tile == pinnedTile);
		tile->setHardwareCanvasMode(usingGpuCanvas);
	}

	const int N = static_cast<int>(allTiles.size());
	if (N == 0) {
		syncVideoCanvasLayout({});
		return;
	}

	const bool hasRemote = !_remoteTiles.empty();
	const bool localActive = _localTile && _localTile->isVideoActive();

 const auto inviteSize = bannerSize(*_inviteHintBanner, stageW, 220);
 const int bannerW = inviteSize.width();
 const int bannerH = inviteSize.height();
	_inviteHintBanner->setGeometry((stageW - bannerW) / 2, stageH - bannerH - 12, bannerW, bannerH);
	_inviteHintBanner->setVisible(!usingGpuCanvas && !hasRemote && !localActive);

	if (_recoveryBanner && _recoveryBanner->isVisible()) {
  const auto recoverySize = bannerSize(*_recoveryBanner, stageW, 420);
  const int recBannerW = recoverySize.width();
  const int recBannerH = recoverySize.height();
		_recoveryBanner->setGeometry((stageW - recBannerW) / 2, 16, recBannerW, recBannerH);
		_recoveryBanner->raise();
	}
	if (_missedCallBanner && _missedCallBanner->isVisible()) {
		const auto missedSize = bannerSize(*_missedCallBanner, stageW, 380);
		_missedCallBanner->setGeometry(
			std::max(12, stageW - missedSize.width() - 16),
			16,
			missedSize.width(),
			missedSize.height());
		_missedCallBanner->raise();
	}

	// Both subscription pixel demand and QWidget/GPU placement use this geometry.
	size_t mainIndex = 0;
	if (auto *focused = focusedVideoTile()) {
		const auto found = std::find(allTiles.begin(), allTiles.end(), focused);
		if (found != allTiles.end()) mainIndex = static_cast<size_t>(found - allTiles.begin());
	}
	if (!gallery && mainIndex != 0) {
		const auto main = allTiles.begin() + mainIndex;
		std::rotate(allTiles.begin(), main, main + 1);
		mainIndex = 0;
	}
	const auto effectiveMode = floatingMode ? livekit::VideoLayoutMode::Speaker : _acceptedVideoPlan.mode;
	const auto effectiveSpeakerLayout = floatingMode ? livekit::SpeakerLayout::Floating : _acceptedVideoPlan.speaker_layout;
	const auto geometry = livekit::CalculateVideoLayout(effectiveMode,
		effectiveSpeakerLayout, {0, 0, stageW, stageH},
		_acceptedVideoPlan.page_size ? _acceptedVideoPlan.page_size : 9, allTiles.size(), mainIndex);
	const auto clips = livekit::CalculateVideoLayoutClips(effectiveMode,
		effectiveSpeakerLayout, {0, 0, stageW, stageH}, allTiles.size(), mainIndex);
	for (size_t i = 0; i < allTiles.size(); ++i) {
		const auto &rect = geometry[i];
		auto *tile = allTiles[i];
		tile->setPipMode(!gallery && i != mainIndex &&
			_acceptedVideoPlan.mode == livekit::VideoLayoutMode::PictureInPicture);
		tile->setGeometry(rect.x, rect.y, rect.width, rect.height);
		const auto &clip = clips[i];
		tile->setLayoutClip(QRect(clip.x, clip.y, clip.width, clip.height).translated(-tile->pos()));
		if (usingGpuCanvas) {
			if (tile->isVisible()) tile->hide();
		} else {
			tile->setVisible(rect.width > 0 && rect.height > 0 && !tile->layoutClipRect().isEmpty());
			if (!gallery) tile->raise();
		}
	}
	syncVideoCanvasLayout(allTiles);
}

void MeetingRoomWindow::paintEvent(QPaintEvent *e) {
	QPainter p(this);
	p.fillRect(rect(), themeColor(ColorRole::Stage));
}

void MeetingRoomWindow::receiveRenderedVideoFrame(
		const QImage& image,
		const QString& key,
		livekit::render::VideoRenderFrame::Ptr renderFrame) {
    if (isDetachedShareFrame(key)) {
        if (_acceptedVideoPlan.detached_local || canRenderRemoteVideo(key))
            _sharedWindow->setFrame(image, std::move(renderFrame));
        return;
    }
    if (_remoteControlUi) _remoteControlUi->noteFrame(key);
    if (key == QStringLiteral("local")) {
        if (_config.videoEnabled) {
            receiveLocalVideoFrame(image, std::move(renderFrame));
            if (_localTile && _floatingStripRenderKeys.count(_localTile->renderKey()) && _floatingListWindow) {
                _floatingListWindow->requestCpuRepaint();
            }
        }
    } else if (_localScreenTile && key == _localScreenTile->renderKey()) {
        _localScreenTile->setFrame(image, std::move(renderFrame));
        if (_floatingStripRenderKeys.count(_localScreenTile->renderKey()) && _floatingListWindow) {
            _floatingListWindow->requestCpuRepaint();
        }
    } else {
        receiveRemoteVideoFrame(image, key, std::move(renderFrame));
        if (auto* tile = remoteVideoTile(key)) {
            if (_floatingStripRenderKeys.count(tile->renderKey()) && _floatingListWindow) {
                _floatingListWindow->requestCpuRepaint();
            }
        }
    }
}

void MeetingRoomWindow::receiveGpuVideoFrame(const std::string& key, livekit::render::VideoRenderFrame::Ptr frame) {
    const auto routeKey = QString::fromStdString(key);
    if (isDetachedShareFrame(routeKey)) {
        if (_acceptedVideoPlan.detached_local || canRenderRemoteVideo(routeKey))
            _sharedWindow->setGpuFrame(std::move(frame));
        return;
    }
    if (_remoteControlUi) _remoteControlUi->noteFrame(QString::fromStdString(key));
    if (!_usingGpuBackend.load(std::memory_order_acquire)) return;
    const auto qkey = QString::fromStdString(key);
    VideoTileWidget* tile = nullptr;
    if (qkey == QStringLiteral("local")) {
        if (!_config.videoEnabled) return;
        tile = _localTile;
    } else if (_localScreenTile && qkey == _localScreenTile->renderKey()) {
        tile = _localScreenTile.get();
    } else {
        if (!canRenderRemoteVideo(qkey)) return;
        tile = remoteVideoTile(qkey);
        if (tile && !tile->isVideoActive()) { tile->setVideoActive(true); }
    }
    if (!tile) return;
    const std::string tileKey = tile->renderKey().toStdString();
    if (_floatingStripRenderKeys.count(tile->renderKey())) {
        if (_floatingListWindow) _floatingListWindow->updateGpuFrame(tileKey, std::move(frame));
    } else if (_videoCanvas) {
        _videoCanvas->updateFrame(tileKey, std::move(frame));
    }
}

void MeetingRoomWindow::receiveRemoteVideoFrame(
		const QImage &frame,
		const QString &trackSid,
		livekit::render::VideoRenderFrame::Ptr renderFrame) {
	if (frame.isNull() || !canRenderRemoteVideo(trackSid)) return;
	if (auto *tile = remoteVideoTile(trackSid)) {
		tile->setFrame(frame, std::move(renderFrame));
		if (!tile->isVideoActive()) {
			tile->setVideoActive(true);
			updateVideoLayout();
		}
	}
}

void MeetingRoomWindow::receiveLocalVideoFrame(
		const QImage &frame,
		livekit::render::VideoRenderFrame::Ptr renderFrame) {
	if (_localTile) {
		_localTile->setFrame(frame, std::move(renderFrame));
	}
}



void MeetingRoomWindow::onLocalVideoGenerated() {
	if (!_localVideoSource) return;

	_localFrameStep++;
	const int w = 1280;
	const int h = 720;

	livekit::VideoFrame frame = livekit::VideoFrame::create(w, h, livekit::VideoBufferType::RGBA);
	uint8_t *data = frame.data();
	if (!data) return;

	const int shift = (_localFrameStep * 4) % w;
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			int idx = (y * w + x) * 4;
			int col_sec = ((x + shift) * 7) / w;
			uint8_t r = 0, g = 0, b = 0;
			switch (col_sec % 7) {
			case 0: r = 38; g = 110; b = 240; break;
			case 1: r = 0; g = 180; b = 136; break;
			case 2: r = 255; g = 125; b = 0; break;
			case 3: r = 245; g = 63; b = 63; break;
			case 4: r = 114; g = 46; b = 209; break;
			case 5: r = 22, g = 93, b = 255; break;
			case 6: r = 20, g = 201, b = 201; break;
			}
			if (y > h * 4 / 5) {
				int bar_x = (_localFrameStep * 8) % w;
				if (std::abs(x - bar_x) < 24) {
					r = 255; g = 255; b = 255;
				} else {
					r = (x * 200) / w;
					g = (y * 200) / h;
					b = 100;
				}
			}
			data[idx] = r;
			data[idx + 1] = g;
			data[idx + 2] = b;
			data[idx + 3] = 255;
		}
	}

	livekit::VideoCaptureOptions opts;
	opts.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	opts.rotation = livekit::VideoRotation::VIDEO_ROTATION_0;

	_localVideoSource->captureFrame(frame, opts);
}

void MeetingRoomWindow::applyParticipantPresentation(
		const OpenMeeting::ParticipantPresentation &presentation) {
	QPointer<MeetingRoomWindow> owner(this);
	QPointer<OpenMeeting::MeetingCoordinator> coordinator(_coordinator.get());
	const auto current = [&](const OpenMeeting::RemoteVideoTrackPresentation *track = nullptr) {
		return owner && coordinator && owner->_coordinator.get() == coordinator &&
			coordinator->isParticipantPresentationCurrent(presentation, track);
	};
	const QString ownAgentId = _roomAgentPublisher
		? _roomAgentPublisher->activeAgentIdentity()
		: QString();
	if (presentation.participant.isLocal ||
		presentation.participant.isAgent ||
		(!ownAgentId.isEmpty() && presentation.participant.identity == ownAgentId) ||
		!current()) return;
	applyRemoteParticipantJoined(presentation.participant.identity,
		presentation.participant.name, &presentation, presentation.videoTracks.empty());
	if (!current()) return;
	bool attachedAny = false;
	for (const auto &track : presentation.videoTracks) {
		if (!current()) return;
		if (!current(&track)) continue;
		owner->attachRemoteVideo(presentation, track, false, false);
		attachedAny = true;
	}
	if (!current()) return;
	if (attachedAny) {
		owner->reconcileRemoteRenderSelection();
	}
	if (owner && !presentation.videoTracks.empty()) {
		owner->updateVideoLayout();
	}
}

void MeetingRoomWindow::restoreParticipantPresentations() {
	QPointer<MeetingRoomWindow> owner(this);
	QPointer<OpenMeeting::MeetingCoordinator> coordinator(_coordinator.get());
	if (!coordinator) return;
	_lastSyncedVideoTracksRevision = coordinator->remoteVideoTracksRevision();
	const auto presentations = coordinator->participantPresentations();
	const QString ownAgentId = owner->_roomAgentPublisher
		? owner->_roomAgentPublisher->activeAgentIdentity()
		: QString();
	bool attachedAny = false;
	bool joinedAny = false;
	for (const auto &presentation : presentations) {
		if (!owner || !coordinator || owner->_coordinator.get() != coordinator ||
			coordinator->state() != OpenMeeting::MeetingState::InMeeting) return;
		if (presentation.participant.isLocal ||
			presentation.participant.isAgent ||
			(!ownAgentId.isEmpty() && presentation.participant.identity == ownAgentId) ||
			!coordinator->isParticipantPresentationCurrent(presentation)) continue;
		owner->applyRemoteParticipantJoined(presentation.participant.identity,
			presentation.participant.name, &presentation, false);
		joinedAny = true;
		for (const auto &track : presentation.videoTracks) {
			if (!owner || !coordinator || owner->_coordinator.get() != coordinator ||
				coordinator->state() != OpenMeeting::MeetingState::InMeeting) return;
			if (!coordinator->isParticipantPresentationCurrent(presentation, &track)) continue;
			owner->attachRemoteVideo(presentation, track, false, false);
			attachedAny = true;
		}
	}
	if (!owner || !coordinator || owner->_coordinator.get() != coordinator ||
		coordinator->state() != OpenMeeting::MeetingState::InMeeting) return;
	if (attachedAny) {
		owner->reconcileRemoteRenderSelection();
	}
	if (owner && (joinedAny || attachedAny)) {
		owner->updateVideoLayout();
	}
}

void MeetingRoomWindow::requestScreenShare() {
	if (!_coordinator) return;
	using State = livekit::ScreenShareState;
	const auto state = _coordinator->screenShareSnapshot().state;
	if (state == State::AwaitingPermission || state == State::Starting ||
		state == State::Active || state == State::StopFailed) {
		_coordinator->stopScreenShare();
	} else if (state == State::Idle || state == State::Failed) {
		if (auto *picker = findChild<QDialog *>(QStringLiteral("screen-share-picker"))) {
			picker->raise();
			return;
		}
		_coordinator->requestScreenShareSources();
	}
}

void MeetingRoomWindow::requestDefaultScreenShare() {
	if (!_coordinator) return;
	using State = livekit::ScreenShareState;
	const auto state = _coordinator->screenShareSnapshot().state;
	if (state != State::Idle && state != State::Failed) return;
	_defaultScreenSharePending = true;
	_coordinator->requestScreenShareSources();
}

void MeetingRoomWindow::handleScreenShareSources(
		const std::vector<livekit::DesktopSource> &sources) {
	if (sources.empty()) {
		_defaultScreenSharePending = false;
		QMessageBox::warning(this, QCoreApplication::translate("MeetingUI", "Screen Sharing"),
			QCoreApplication::translate("MeetingUI", "No screens or windows available to share"));
		return;
	}
	if (_defaultScreenSharePending) {
		_defaultScreenSharePending = false;
		auto source = std::find_if(sources.begin(), sources.end(), [](const auto &candidate) {
			return candidate.kind == livekit::DesktopSourceKind::Screen;
		});
		if (source == sources.end()) source = sources.begin();
		if (_coordinator) _coordinator->startScreenShare(*source, livekit::ScreenShareQuality{
			livekit::ScreenShareResolution::Auto,
			OpenMeeting::SessionManager::instance().mediaPreferences().screenShareFps});
		return;
	}

	auto *dialog = new QDialog(this);
	dialog->setObjectName(QStringLiteral("screen-share-picker"));
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->setWindowFlag(Qt::WindowContextHelpButtonHint, false);
	dialog->setWindowTitle(QCoreApplication::translate("MeetingUI", "Select a Source to Share"));
	AppTheme::setTone(*dialog, AppTheme::Tone::Dark);
	auto *layout = new QVBoxLayout(dialog);
	auto *sourceLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Select a screen or window (video only):"), dialog);
#ifdef Q_OS_LINUX
	sourceLabel->setText(QCoreApplication::translate("MeetingUI",
		"Choose screen or window, then allow sharing in the system picker."));
#endif
	layout->addWidget(sourceLabel);
	auto *sourceCombo = createElidedAccessibleComboBox(dialog);
	sourceCombo->setObjectName(QStringLiteral("screenShareSource"));
	sourceCombo->setAccessibleName(QCoreApplication::translate("MeetingUI", "Share source"));
	sourceLabel->setBuddy(sourceCombo);
	for (size_t i = 0; i < sources.size(); ++i) {
		const auto &source = sources[i];
		const auto kind = source.kind == livekit::DesktopSourceKind::Screen
			? QCoreApplication::translate("MeetingUI", "Screen") : QCoreApplication::translate("MeetingUI", "Window");
#ifdef Q_OS_LINUX
		sourceCombo->addItem(kind);
#else
		sourceCombo->addItem(QString::number(i + 1) + QStringLiteral(". ") + kind +
			QStringLiteral(" — ") + QString::fromStdString(source.title));
#endif
	}
	layout->addWidget(sourceCombo);
	auto *resolutionLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Screen share resolution"), dialog);
	layout->addWidget(resolutionLabel);
	auto *resolutionCombo = createElidedAccessibleComboBox(dialog);
	resolutionCombo->setObjectName(QStringLiteral("screenShareStartResolution"));
	resolutionCombo->setAccessibleName(resolutionLabel->text());
	resolutionLabel->setBuddy(resolutionCombo);
	layout->addWidget(resolutionCombo);
	auto *resolutionHint = new QLabel(dialog);
	resolutionHint->setObjectName(QStringLiteral("screenShareStartResolutionHint"));
	resolutionHint->setWordWrap(true);
	resolutionHint->setVisible(false);
	layout->addWidget(resolutionHint);
	auto *fpsLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Screen share frame rate"), dialog);
	layout->addWidget(fpsLabel);
	auto *fpsCombo = createElidedAccessibleComboBox(dialog);
	fpsCombo->setObjectName(QStringLiteral("screenShareStartFps"));
	fpsCombo->setAccessibleName(fpsLabel->text());
	fpsLabel->setBuddy(fpsCombo);
	for (int fps : {15, 20, 30}) fpsCombo->addItem(QString::number(fps) + QStringLiteral(" FPS"), fps);
	const auto preferences = OpenMeeting::SessionManager::instance().mediaPreferences();
	const int preferredFps = preferences.screenShareFps;
	const int preferredIndex = fpsCombo->findData(preferredFps);
	fpsCombo->setCurrentIndex(preferredIndex >= 0 ? preferredIndex : fpsCombo->findData(20));
	layout->addWidget(fpsCombo);
	const auto updateResolutions = [this, sourceCombo, resolutionCombo, resolutionHint, fpsCombo, sources] {
		const int selected = resolutionCombo->currentData().isValid()
			? resolutionCombo->currentData().toInt() : int(livekit::ScreenShareResolution::Auto);
		const int index = sourceCombo->currentIndex();
		const auto pixels = index >= 0 && index < int(sources.size())
			? screenShareSourcePixelSize(sources[index]) : QSize{};
		populateScreenShareResolutionChoices(*resolutionCombo, pixels);
		selectScreenShareResolution(*resolutionCombo, selected);
		const int fps = fpsCombo->currentData().isValid() ? fpsCombo->currentData().toInt() : 20;
		const auto ceiling = _coordinator
			? _coordinator->evaluateScreenShareCodecCeiling(fps)
			: livekit::ScreenShareQualityConstraint{};
		annotateScreenShareResolutionChoices(*resolutionCombo, pixels, ceiling);
		const auto currentRes = static_cast<livekit::ScreenShareResolution>(resolutionCombo->currentData().toInt());
		const auto hint = screenShareConstraintHintText(pixels, currentRes, ceiling);
		resolutionHint->setText(hint);
		resolutionHint->setVisible(!hint.isEmpty());
	};
	updateResolutions(); // A newly opened picker always starts at Auto.
	connect(sourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), dialog,
		[updateResolutions](int) { updateResolutions(); });
	connect(fpsCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), dialog,
		[updateResolutions](int) { updateResolutions(); });
	connect(resolutionCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), dialog,
		[this, sourceCombo, resolutionCombo, resolutionHint, fpsCombo, sources](int) {
			const int index = sourceCombo->currentIndex();
			const auto pixels = index >= 0 && index < int(sources.size())
				? screenShareSourcePixelSize(sources[index]) : QSize{};
			const int fps = fpsCombo->currentData().isValid() ? fpsCombo->currentData().toInt() : 20;
			const auto ceiling = _coordinator
				? _coordinator->evaluateScreenShareCodecCeiling(fps)
				: livekit::ScreenShareQualityConstraint{};
			const auto currentRes = static_cast<livekit::ScreenShareResolution>(resolutionCombo->currentData().toInt());
			const auto hint = screenShareConstraintHintText(pixels, currentRes, ceiling);
			resolutionHint->setText(hint);
			resolutionHint->setVisible(!hint.isEmpty());
		});

    QCheckBox *shareAudio = nullptr;
    QComboBox *audioOutput = nullptr;
#ifdef Q_OS_LINUX
    shareAudio = new QCheckBox(QCoreApplication::translate("MeetingUI", "Share system audio (preview)"), dialog);
    shareAudio->setObjectName(QStringLiteral("screenShareSystemAudio"));
    shareAudio->setChecked(false);
    layout->addWidget(shareAudio);
    audioOutput = createElidedAccessibleComboBox(dialog);
    audioOutput->setObjectName(QStringLiteral("screenShareAudioOutput"));
    audioOutput->setAccessibleName(QCoreApplication::translate("MeetingUI", "System audio output to share"));
    audioOutput->addItem(QCoreApplication::translate("MeetingUI", "System default output (follow changes)"), QString());
    for (const auto &output : _coordinator->screenShareAudioOutputs()) {
        if (output.id.empty()) continue;
        audioOutput->addItem(QString::fromStdString(output.name), QString::fromStdString(output.id));
    }
    const bool audioAvailable = _coordinator->systemAudioSharingAvailable() && audioOutput->count() > 1;
    shareAudio->setEnabled(audioAvailable);
    audioOutput->setEnabled(false);
    connect(shareAudio, &QCheckBox::toggled, audioOutput, &QWidget::setEnabled);
    layout->addWidget(audioOutput);
    auto *audioHint = new QLabel(QCoreApplication::translate("MeetingUI",
        "Shares other applications playing through the selected output. Cohavora and streams with no known application are excluded. Audio is independent of the selected screen or window."), dialog);
    audioHint->setWordWrap(true);
    if (!audioAvailable) audioHint->setText(_coordinator->systemAudioSharingAvailable()
        ? QCoreApplication::translate("MeetingUI", "No supported audio outputs are available. Video sharing is available.")
        : QCoreApplication::translate("MeetingUI", "System audio sharing is unavailable in this build. Video sharing is available."));
    layout->addWidget(audioHint);
#endif
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
	layout->addWidget(buttons);
	buttons->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("screenShareAccept"));
	buttons->button(QDialogButtonBox::Cancel)->setObjectName(QStringLiteral("screenShareCancel"));
	AppTheme::setStyleVariant(*dialog, "screen-share-picker");
	AppTheme::makeDialogAdaptive(*dialog, QSize(520, shareAudio ? 500 : 360));
	dialog->setMaximumWidth(560);
	connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
	QPointer<OpenMeeting::MeetingCoordinator> coordinator(_coordinator.get());
	const std::weak_ptr<livekit::Room> room = _coordinator->room();
	connect(dialog, &QDialog::accepted, this,
		[coordinator, room, sources, sourceCombo, resolutionCombo, fpsCombo, shareAudio, audioOutput] {
			const int index = sourceCombo->currentIndex();
			livekit::ScreenShareAudioOptions audio;
            if (shareAudio && shareAudio->isEnabled() && shareAudio->isChecked() && audioOutput &&
                audioOutput->currentIndex() >= 0) {
                audio.enabled = true;
                audio.output_id = audioOutput->currentData().toString().toStdString();
                audio.output_name = audioOutput->currentText().toStdString();
            }
			if (coordinator && !room.expired() && coordinator->room() == room.lock() &&
				index >= 0 && index < static_cast<int>(sources.size()))
				coordinator->startScreenShare(sources[index], livekit::ScreenShareQuality{
					static_cast<livekit::ScreenShareResolution>(resolutionCombo->currentData().toInt()),
					fpsCombo->currentData().toInt()}, std::move(audio));
		});
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::stateChanged,
		dialog, [dialog](OpenMeeting::MeetingState state, const QString &) {
			if (state != OpenMeeting::MeetingState::InMeeting) dialog->reject();
		});
	dialog->open();
}

void MeetingRoomWindow::openScreenShareAudioPicker(uint64_t run, uint64_t audioEpoch) {
    using AudioState = livekit::ScreenShareAudioState;
    if (!_coordinator || !_coordinator->isMeetingReady() || !_coordinator->systemAudioSharingAvailable()) return;
    const auto snapshot = _coordinator->screenShareSnapshot();
    if (snapshot.state != livekit::ScreenShareState::Active || snapshot.run_id != run ||
        snapshot.audio_epoch != audioEpoch ||
        (snapshot.audio_state != AudioState::Disabled && snapshot.audio_state != AudioState::Failed)) return;
    if (auto *existing = findChild<QDialog*>(QStringLiteral("screen-share-audio-picker"))) {
        existing->raise();
        existing->activateWindow();
        return;
    }
    auto *dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("screen-share-audio-picker"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    dialog->setWindowTitle(QCoreApplication::translate("MeetingUI", "Share system audio"));
    AppTheme::setTone(*dialog, AppTheme::Tone::Dark);
    AppTheme::setStyleVariant(*dialog, "screen-share-picker");
    auto *layout = new QVBoxLayout(dialog);
    auto *label = new QLabel(QCoreApplication::translate("MeetingUI", "System audio output to share"), dialog);
    layout->addWidget(label);
    auto *output = createElidedAccessibleComboBox(dialog);
    output->setObjectName(QStringLiteral("screenShareAudioRestartOutput"));
    output->setAccessibleName(label->text());
    label->setBuddy(output);
    layout->addWidget(output);
    auto *status = new QLabel(dialog);
    status->setObjectName(QStringLiteral("screenShareAudioOutputStatus"));
    status->setWordWrap(true);
    layout->addWidget(status);
    auto *hint = new QLabel(QCoreApplication::translate("MeetingUI",
        "Shares other applications playing through the selected output. Cohavora and streams with no known application are excluded. Audio is independent of the selected screen or window."), dialog);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *refresh = new QPushButton(QCoreApplication::translate("MeetingUI", "Refresh audio outputs"), dialog);
    refresh->setObjectName(QStringLiteral("screenShareAudioRefresh"));
    layout->addWidget(refresh);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    auto *start = buttons->button(QDialogButtonBox::Ok);
    start->setObjectName(QStringLiteral("screenShareAudioStart"));
    start->setText(QCoreApplication::translate("MeetingUI", "Start sharing audio"));
    start->setEnabled(false);
    buttons->button(QDialogButtonBox::Cancel)->setObjectName(QStringLiteral("screenShareAudioCancel"));
    layout->addWidget(buttons);
    QPointer<OpenMeeting::MeetingCoordinator> coordinator(_coordinator.get());
    const std::weak_ptr<livekit::Room> room = _coordinator->room();
    const auto request = std::make_shared<uint64_t>(0);
    const auto isCurrent = [coordinator, room, run, audioEpoch] {
        if (!coordinator || room.expired() || coordinator->room() != room.lock() || !coordinator->isMeetingReady()) return false;
        const auto value = coordinator->screenShareSnapshot();
        return value.state == livekit::ScreenShareState::Active && value.run_id == run &&
            value.audio_epoch == audioEpoch &&
            (value.audio_state == AudioState::Disabled || value.audio_state == AudioState::Failed);
    };
    const auto refreshOutputs = [coordinator, dialog, output, status, start, refresh, request, run, audioEpoch, isCurrent] {
        if (!isCurrent()) { dialog->reject(); return; }
        output->clear();
        output->setEnabled(false);
        start->setEnabled(false);
        refresh->setEnabled(false);
        status->setText(QCoreApplication::translate("MeetingUI", "Loading audio outputs…"));
        *request = coordinator->requestScreenShareAudioOutputs(run, audioEpoch);
        if (!*request) dialog->reject();
    };
    connect(coordinator.data(), &OpenMeeting::MeetingCoordinator::screenShareAudioOutputsReady, dialog,
        [dialog, output, status, start, refresh, request, run, audioEpoch,
         previousOutput = snapshot.audio_output_id, isCurrent](OpenMeeting::ScreenShareAudioOutputsSnapshot value) {
            if (value.request_id != *request || value.run_id != run || value.audio_epoch != audioEpoch) return;
            if (!isCurrent()) { dialog->reject(); return; }
            output->clear();
            output->addItem(QCoreApplication::translate("MeetingUI", "System default output (follow changes)"), QString());
            for (const auto &device : value.outputs) {
                if (!device.id.empty())
                    output->addItem(QString::fromStdString(device.name), QString::fromStdString(device.id));
            }
            const bool available = output->count() > 1;
            const int previous = output->findData(QString::fromStdString(previousOutput));
            output->setCurrentIndex(available ? previous : -1);
            output->setEnabled(available);
            start->setEnabled(available && previous >= 0);
            refresh->setEnabled(true);
            status->setText(!available
                ? QCoreApplication::translate("MeetingUI", "No supported audio outputs are available. Video sharing is available.")
                : previous < 0
                ? QCoreApplication::translate("MeetingUI", "The previous audio output is unavailable. Select another output.")
                : QString());
        }, Qt::QueuedConnection);
    connect(output, QOverload<int>::of(&QComboBox::currentIndexChanged), dialog, [output, start](int index) {
        start->setEnabled(output->isEnabled() && output->count() > 1 && index >= 0);
    });
    connect(refresh, &QPushButton::clicked, dialog, refreshOutputs);
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(dialog, &QDialog::accepted, this, [coordinator, output, run, audioEpoch, isCurrent] {
        if (!isCurrent() || !output->isEnabled() || output->currentIndex() < 0) return;
        livekit::ScreenShareAudioOptions options;
        options.enabled = true;
        options.output_id = output->currentData().toString().toStdString();
        options.output_name = output->currentText().toStdString();
        coordinator->startScreenShareAudio(std::move(options), run, audioEpoch);
    });
    connect(coordinator.data(), &OpenMeeting::MeetingCoordinator::screenShareChanged, dialog,
        [dialog, isCurrent](livekit::ScreenShareSnapshot) { if (!isCurrent()) dialog->reject(); });
    connect(coordinator.data(), &OpenMeeting::MeetingCoordinator::stateChanged, dialog,
        [dialog](OpenMeeting::MeetingState state, const QString&) {
            if (state != OpenMeeting::MeetingState::InMeeting) dialog->reject();
        });
    AppTheme::makeDialogAdaptive(*dialog, QSize(520, 330));
    dialog->setMaximumWidth(560);
    dialog->open();
    refreshOutputs();
}

bool MeetingRoomWindow::isTrackVisible(const livekit::TrackKey &key) const {
    if (_acceptedVideoPlan.detached_share && _acceptedVideoPlan.detached_share->key == key &&
        _acceptedVideoPlan.detached_share->width && _acceptedVideoPlan.detached_share->height)
        return true;
	return std::any_of(
		_acceptedVideoPlan.visible_seats.begin(),
		_acceptedVideoPlan.visible_seats.end(),
		[&](const auto &seat) { return seat.key == key; });
}

std::optional<livekit::TrackKey> MeetingRoomWindow::trackKeyForRenderKey(
		const QString &renderKey) const {
	if (renderKey.isEmpty()) return std::nullopt;
	for (const auto &seat : _acceptedVideoPlan.visible_seats) {
		const auto *tile = remoteVideoTile(seat.key);
		if (tile && tile->renderKey() == renderKey) return seat.key;
	}
	return std::nullopt;
}

VideoTileWidget *MeetingRoomWindow::focusedVideoTile() const {
	if (_acceptedVideoPlan.focused_local == livekit::LocalVideoTarget::Camera) {
		return _localTile;
	}
	if (_acceptedVideoPlan.focused_local == livekit::LocalVideoTarget::ScreenShare) {
		return _localScreenTile.get();
	}
	return _acceptedVideoPlan.focused ? remoteVideoTile(*_acceptedVideoPlan.focused) : nullptr;
}

VideoTileWidget *MeetingRoomWindow::ensureRemoteCameraTile(
		const QString &identity,
		const QString &name) {
	applyRemoteParticipantJoined(identity, name, nullptr, false);
	const auto found = _remoteTiles.find(identity);
	return found == _remoteTiles.end() ? nullptr : found->second.get();
}

void MeetingRoomWindow::recycleRemoteCameraTile(std::unique_ptr<VideoTileWidget> tile) {
	if (!tile || _closeRequested ||
		_remoteTilePool.size() >= livekit::kMaximumRemoteVideoTracks) {
		return;
	}
	disconnect(tile.get(), &VideoTileWidget::remoteVolumeChanged, this, nullptr);
	disconnect(tile.get(), &VideoTileWidget::remoteLocalMuteToggled, this, nullptr);
	tile->hide();
	tile->resetForReuse(QString());
	_remoteTilePool.push_back(std::move(tile));
}

VideoTileWidget *MeetingRoomWindow::ensureRemoteScreenTile(
		const livekit::VideoSeat &seat,
		const QString &name) {
	const auto sid = QString::fromStdString(seat.key.publication_sid);
	if (sid.isEmpty()) return nullptr;
	const auto existing = _remoteScreenTiles.find(sid);
	if (existing != _remoteScreenTiles.end()) return existing->second.get();
	auto tile = std::make_unique<VideoTileWidget>(
		QCoreApplication::translate("MeetingUI", "%1 · Screen Share").arg(name),
		false, _stageContainer, true);
	tile->setIdentity(QStringLiteral("remote-screen/") + sid);
	bindTileInteractions(tile.get());
	auto *result = tile.get();
	_remoteScreenTiles.emplace(sid, std::move(tile));
	return result;
}

VideoTileWidget *MeetingRoomWindow::remoteVideoTile(
		const livekit::TrackKey &key) const {
	const auto sid = QString::fromStdString(key.publication_sid);
	const auto binding = _remoteVideoBindings.find(sid);
	if (binding != _remoteVideoBindings.end()) return remoteVideoTile(sid);
	const auto seat = std::find_if(
		_acceptedVideoPlan.visible_seats.begin(),
		_acceptedVideoPlan.visible_seats.end(),
		[&](const auto &value) { return value.key == key; });
	if (seat == _acceptedVideoPlan.visible_seats.end()) return nullptr;
	if (seat->source == livekit::TrackSource::ScreenShareVideo) {
		const auto tile = _remoteScreenTiles.find(sid);
		return tile == _remoteScreenTiles.end() ? nullptr : tile->second.get();
	}
	const auto identity = QString::fromStdString(
		key.participant.identity.empty() ? key.participant.sid : key.participant.identity);
	const auto tile = _remoteTiles.find(identity);
	return tile == _remoteTiles.end() ? nullptr : tile->second.get();
}

void MeetingRoomWindow::applyAcceptedVideoDemandPlan(
		livekit::VideoDemandPlan plan) {
    const auto requestedSurface = _detachedShareIntent ? _detachedShareIntent->surface_id : 0;
    if (_sharedWindow && _lastViewportIntent &&
        plan.view_revision < _lastViewportIntent->view_revision &&
        plan.detached_surface_id != requestedSurface) return;
	if (_acceptedVideoPlan.coordinator_session == plan.coordinator_session &&
		plan.policy_revision < _acceptedVideoPlan.policy_revision) {
		return;
	}
	if (_acceptedVideoPlan.coordinator_session != 0 &&
		_acceptedVideoPlan.coordinator_session != plan.coordinator_session) {
		_activeRemoteRenderLeases.clear();
		_lastViewportIntent.reset();
	}
	_acceptedVideoPlan = std::move(plan);
    syncSharedScreenPlan();
	// An older accepted plan must not overwrite a newer pin/unpin request.
	if (!_lastViewportIntent ||
		_acceptedVideoPlan.view_revision >= _lastViewportIntent->view_revision) {
		_pinnedTrackKey = _acceptedVideoPlan.pinned;
		_pinnedLocalTarget = _acceptedVideoPlan.pinned_local;
		if (!_pinnedTrackKey && !_pinnedLocalTarget) _pinnedRenderKey.clear();
	}
	// Display accepted state; only a response to the latest request may update pending input.
	if (!_lastViewportIntent || _acceptedVideoPlan.view_revision >= _lastViewportIntent->view_revision) {
		if ((_acceptedVideoPlan.stage_content == livekit::StageContent::Video ||
			 _acceptedVideoPlan.floating_strip_active) &&
			_acceptedVideoPlan.reason != livekit::VideoDemandReason::Hidden) {
			_videoPage = _acceptedVideoPlan.page;
		}
		if (_acceptedVideoPlan.mode == livekit::VideoLayoutMode::Grid &&
			(_acceptedVideoPlan.page_size == 4 || _acceptedVideoPlan.page_size == 9 ||
			 _acceptedVideoPlan.page_size == 16 || _acceptedVideoPlan.page_size == 25 ||
			 _acceptedVideoPlan.page_size == 49)) {
			_videoPageSize = _acceptedVideoPlan.page_size;
		}
	}
	syncFloatingListWindowVisibility();
	if (_videoPagingControls) _videoPagingControls->setPage(_acceptedVideoPlan.page,
		std::max<uint32_t>(1, _acceptedVideoPlan.page_count));
	if (_floatingListWindow) _floatingListWindow->setPage(_acceptedVideoPlan.page,
		std::max<uint32_t>(1, _acceptedVideoPlan.page_count));
	const auto previousStageSize = _stageContainer->size();
	updateStageGeometry();
	if (previousStageSize != _stageContainer->size()) scheduleViewportIntent(false);
	syncVisibleRemoteTiles();
	// Presentations can arrive before the accepted plan creates their tiles.
	// Replay current media state so new tiles do not depend on a first frame.
	QPointer<MeetingRoomWindow> owner(this);
	refreshRemoteVideoPresentations();
	if (owner) owner->updateVideoLayout();
}

void MeetingRoomWindow::syncVisibleRemoteTiles() {
	std::set<QString> cameraIdentities;
	std::set<QString> screenSids;
	for (const auto &seat : _acceptedVideoPlan.visible_seats) {
		if (cameraIdentities.size() + screenSids.size() >= livekit::kMaximumRemoteVideoTracks) break;
		const auto identity = QString::fromStdString(
			seat.key.participant.identity.empty()
				? seat.key.participant.sid
				: seat.key.participant.identity);
		const auto knownName = _remoteParticipantNames.find(identity);
		const auto name = knownName == _remoteParticipantNames.end() || knownName->second.isEmpty()
			? identity : knownName->second;
		if (seat.source == livekit::TrackSource::ScreenShareVideo) {
			const auto sid = QString::fromStdString(seat.key.publication_sid);
			screenSids.insert(sid);
			if (auto *tile = ensureRemoteScreenTile(seat, name)) {
				tile->setVideoSubscriptionError(seat.subscription_error);
			}
		} else {
			cameraIdentities.insert(identity);
			auto *tile = ensureRemoteCameraTile(identity, name);
			if (tile) tile->setVideoSubscriptionError(seat.subscription_error);
			if (tile && seat.IsParticipantPlaceholder()) {
				tile->setFrame({});
				tile->setVideoActive(false);
				tile->setVideoStreamPaused(false);
			}
		}
	}

	for (auto it = _remoteTiles.begin(); it != _remoteTiles.end();) {
		if (cameraIdentities.count(it->first)) {
			++it;
			continue;
		}
		auto retired = std::move(it->second);
		it = _remoteTiles.erase(it);
		if (retired) {
			removeUserFromCanvases(retired->renderKey().toStdString());
			retired->setFrame({});
			recycleRemoteCameraTile(std::move(retired));
		}
	}
	for (auto it = _remoteScreenTiles.begin(); it != _remoteScreenTiles.end();) {
		if (screenSids.count(it->first)) {
			++it;
			continue;
		}
		if (it->second) {
			removeUserFromCanvases(it->second->renderKey().toStdString());
			it->second->setFrame({});
		}
		it = _remoteScreenTiles.erase(it);
	}
}

void MeetingRoomWindow::reconcileRemoteRenderSelection() {
	if (!_remoteRenderSession) return;
	std::vector<livekit::render::VideoRenderSession::RemoteTrackSelection> selection;
	selection.reserve(_acceptedVideoPlan.selected_video.size());
	for (const auto &key : _acceptedVideoPlan.selected_video) {
		const auto sid = QString::fromStdString(key.publication_sid);
		const auto binding = _remoteVideoBindings.find(sid);
		if (binding == _remoteVideoBindings.end() || binding->second.key != key ||
			binding->second.muted || binding->second.paused ||
			remoteVideoSubscriptionError(key) != livekit::TrackPublication::SubscriptionError::None) {
			continue;
		}
		const auto track = binding->second.track.lock();
		if (!track ||
			!livekit::IsTrackTicketActive(binding->second.ticket, key) ||
			!livekit::IsMediaBindingTicketActive(
				binding->second.mediaBindingTicket,
				binding->second.mediaBindingKey)) {
			continue;
		}
		livekit::render::VideoRenderSession::RemoteTrackSelection value;
		value.key = key;
		value.ticket = binding->second.ticket;
		value.media_binding_key = binding->second.mediaBindingKey;
		value.media_binding_ticket = binding->second.mediaBindingTicket;
		value.track = std::move(track);
		value.identity = binding->second.identity.toStdString();
		value.render_key = key.publication_sid;
        if (const auto* seat = _acceptedVideoPlan.FindMediaSeat(key)) {
            value.max_fps = livekit::VideoSubscriptionMaxFps(
                seat->subscription_width, seat->subscription_height);
        }
		selection.push_back(std::move(value));
	}
	const auto result = _remoteRenderSession->ApplySelection(
		_acceptedVideoPlan.coordinator_session,
		_acceptedVideoPlan.policy_revision,
		selection);
	livekit::VideoRenderSelectionObservation observation;
	observation.coordinator_session = _acceptedVideoPlan.coordinator_session;
	observation.catalog_revision = _acceptedVideoPlan.catalog_revision;
	observation.policy_revision = _acceptedVideoPlan.policy_revision;
	observation.actual_video.reserve(selection.size());
	observation.bound_video.reserve(result.attached);
	for (const auto &value : selection) {
		observation.actual_video.push_back(value.key);
	}
	std::map<QString, livekit::MediaBindingKey> active;
	for (std::size_t index = 0; index < result.tracks.size() &&
		index < selection.size(); ++index) {
		const auto status = result.tracks[index].second;
		if (status == livekit::render::VideoRenderSession::AttachResult::Attached ||
			status == livekit::render::VideoRenderSession::AttachResult::AlreadyBound) {
			active[QString::fromStdString(selection[index].key.publication_sid)] =
				selection[index].media_binding_key;
			observation.bound_video.push_back(selection[index].key);
		}
	}
	for (const auto &[sid, oldBinding] : _activeRemoteRenderLeases) {
		const auto current = active.find(sid);
		if (current != active.end() && current->second == oldBinding) continue;
        if (isDetachedShareFrame(sid)) _sharedWindow->setStatus(tr("Waiting for shared screen…"), true);
		if (auto *tile = remoteVideoTile(sid)) {
			removeUserFromCanvases(tile->renderKey().toStdString());
			tile->setFrame({});
		}
	}
	_activeRemoteRenderLeases = std::move(active);
	if (_coordinator) {
		_coordinator->reportVideoRenderSelection(std::move(observation));
	}
}

VideoTileWidget *MeetingRoomWindow::remoteVideoTile(const QString &trackSid) const {
	const auto found = _remoteVideoBindings.find(trackSid);
	if (found == _remoteVideoBindings.end()) return nullptr;
	if (found->second.screen) {
		const auto tile = _remoteScreenTiles.find(trackSid);
		return tile == _remoteScreenTiles.end() ? nullptr : tile->second.get();
	}
	const auto tile = _remoteTiles.find(found->second.identity);
	return tile == _remoteTiles.end() ? nullptr : tile->second.get();
}

bool MeetingRoomWindow::canRenderRemoteVideo(const QString &trackSid) const {
	const auto found = _remoteVideoBindings.find(trackSid);
	if (found == _remoteVideoBindings.end()) return false;
	const auto &binding = found->second;
	const auto lease = _activeRemoteRenderLeases.find(trackSid);
	if (lease == _activeRemoteRenderLeases.end() ||
		lease->second != binding.mediaBindingKey ||
		!isTrackVisible(binding.key)) {
		return false;
	}
	const auto track = binding.track.lock();
	return track && !track->muted() && !binding.muted && !binding.paused &&
		remoteVideoSubscriptionError(binding.key) == livekit::TrackPublication::SubscriptionError::None &&
		livekit::IsMediaBindingTicketActive(binding.mediaBindingTicket, binding.mediaBindingKey);
}

livekit::TrackPublication::SubscriptionError MeetingRoomWindow::remoteVideoSubscriptionError(
		const livekit::TrackKey &key) const {
    if (const auto* seat = _acceptedVideoPlan.FindMediaSeat(key)) return seat->subscription_error;
	return livekit::TrackPublication::SubscriptionError::None;
}

void MeetingRoomWindow::refreshRemoteVideoPresentations() {
	QPointer<MeetingRoomWindow> owner(this);
	QPointer<OpenMeeting::MeetingCoordinator> coordinator(_coordinator.get());
	if (!coordinator) return;
	_lastSyncedVideoTracksRevision = coordinator->remoteVideoTracksRevision();
	const auto presentations = coordinator->participantPresentations();
	bool needsRelayout = false;
	for (const auto &presentation : presentations) {
		for (const auto &video : presentation.videoTracks) {
			if (!owner || !coordinator || owner->_coordinator.get() != coordinator) return;
			needsRelayout = owner->attachRemoteVideo(presentation, video, false, false) || needsRelayout;
		}
	}
	// All presentations share one accepted plan. Publish its final selection
	// once, rather than a telemetry snapshot for every replayed track.
	if (owner && coordinator && owner->_coordinator.get() == coordinator) {
		owner->reconcileRemoteRenderSelection();
		if (owner && needsRelayout) {
			owner->updateVideoLayout();
		}
	}
}

bool MeetingRoomWindow::attachRemoteVideo(const OpenMeeting::ParticipantPresentation &presentation,
		const OpenMeeting::RemoteVideoTrackPresentation &value, bool reconcile, bool relayout) {
	if (!_remoteRenderSession || !_coordinator ||
		!_coordinator->isParticipantPresentationCurrent(presentation, &value)) return false;
	const auto &identity = presentation.participant.identity;
	const auto sid = QString::fromStdString(value.key.publication_sid);
	const bool screen = value.track->source() == livekit::TrackSource::ScreenShareVideo;
	QPointer<MeetingRoomWindow> owner(this);
	QPointer<OpenMeeting::MeetingCoordinator> coordinator(_coordinator.get());
	const auto current = [&] {
		return owner && coordinator && owner->_coordinator.get() == coordinator &&
			coordinator->isParticipantPresentationCurrent(presentation, &value);
	};
	if (const auto old = _remoteVideoBindings.find(sid);
		old != _remoteVideoBindings.end() && old->second.screen != screen) removeRemoteVideo(sid, reconcile);
	if (!current()) return false;
	const auto old = _remoteVideoBindings.find(sid);
	const bool resetFrame = old == _remoteVideoBindings.end() ||
		old->second.identity != identity || old->second.track.lock() != value.track ||
		old->second.mediaBindingKey != value.mediaBindingKey ||
		old->second.muted != value.muted || old->second.paused != value.paused;
	_remoteVideoBindings[sid] = {identity, screen, value.track,
		value.key, value.ticket, value.mediaBindingKey, value.mediaBindingTicket,
		value.muted, value.paused};
	bool screenTileCreated = false;
	if (screen && isTrackVisible(value.key) && !_remoteScreenTiles.count(sid)) {
		const auto seat = std::find_if(
			_acceptedVideoPlan.visible_seats.begin(),
			_acceptedVideoPlan.visible_seats.end(),
			[&](const auto &candidate) { return candidate.key == value.key; });
		if (seat != _acceptedVideoPlan.visible_seats.end()) {
			ensureRemoteScreenTile(*seat, presentation.participant.name);
			screenTileCreated = true;
		}
	}
	if (!current()) return false;
	QPointer<VideoTileWidget> tile(remoteVideoTile(sid));
	if (resetFrame) {
		// Revoke the render subscription before clearing either backend. This also
		// rejects callbacks already in flight when the same Track is rebound.
		_remoteRenderSession->RemoveTrack(sid.toStdString());
		_activeRemoteRenderLeases.erase(sid);
        if (isDetachedShareFrame(sid)) _sharedWindow->setStatus(tr("Waiting for shared screen…"), true);
		if (tile) {
			removeUserFromCanvases(tile->renderKey().toStdString());
			tile->setFrame({});
		}
	}
	if (tile) {
		tile->setVideoActive(!value.muted);
		tile->setVideoStreamPaused(!value.muted && value.paused);
		tile->setVideoSubscriptionError(remoteVideoSubscriptionError(value.key));
		tile->setConnectionQuality(presentation.participant.connectionQuality);
		if (screen) tile->setDisplayName(QCoreApplication::translate("MeetingUI", "%1 · Screen Share").arg(presentation.participant.name));
	}
	if (!current()) return false;
	const bool needsRelayout = resetFrame || screenTileCreated;
	if (reconcile) reconcileRemoteRenderSelection();
	if (relayout && needsRelayout) updateVideoLayout();
	if (_remoteControlUi) _remoteControlUi->refreshTargets();
	return needsRelayout;
}

void MeetingRoomWindow::removeRemoteVideo(const QString &trackSid, bool reconcile) {
    if (isDetachedShareFrame(trackSid)) _sharedWindow->setStatus(tr("Screen sharing has ended."), true);
	const auto binding = _remoteVideoBindings.find(trackSid);
	if (binding != _remoteVideoBindings.end() && _pinnedTrackKey &&
		binding->second.key == *_pinnedTrackKey) {
		_pinnedTrackKey.reset();
		_pinnedRenderKey.clear();
	}
	if (_remoteRenderSession) _remoteRenderSession->RemoveTrack(trackSid.toStdString());
	_activeRemoteRenderLeases.erase(trackSid);
	if (auto *tile = remoteVideoTile(trackSid)) {
		removeUserFromCanvases(tile->renderKey().toStdString());
		tile->setFrame({});
		tile->setVideoActive(false);
		tile->setVideoStreamPaused(false);
		if (_remoteScreenTiles.count(trackSid) && _pinnedRenderKey == tile->renderKey()) _pinnedRenderKey.clear();
	}
	_remoteScreenTiles.erase(trackSid);
	_remoteVideoBindings.erase(trackSid);
	if (_remoteControlUi) _remoteControlUi->refreshTargets();
	syncVisibleRemoteTiles();
	if (reconcile) reconcileRemoteRenderSelection();
	updateVideoLayout();
}

void MeetingRoomWindow::updateRemoteControlStatus(const QString& status) {
    if (_remoteControlStatus == status) return;
    _remoteControlStatus = status;
    if (!_screenShareBanner) return;
    _remoteControlStopButton->setVisible(!status.isEmpty());
    syncScreenShareControls();
    QResizeEvent layout(size(), size());
    resizeEvent(&layout);
}

void MeetingRoomWindow::syncScreenShareControls() {
    const bool detached = _screenShareActive && _sharedWindow &&
        _detachedShareIntent && _detachedShareIntent->local;
    if (_sharedWindow) {
        _sharedWindow->setLocalShareControls(detached ? _screenQualityButton : nullptr,
            detached ? _annotationButton : nullptr, _screenShareDetails);
    }
    if (!_screenShareBanner) return;
    const bool visible = _screenShareBannerNeeded && (!detached || !_remoteControlStatus.isEmpty() ||
        !_screenShareAudioStatus.isEmpty() || _screenShareAudioControlNeeded);
    const bool relayout = _screenShareBanner->isHidden() == visible;
    _screenShareBanner->setVisible(visible);
    _screenShareBanner->setText(detached ? _screenShareAudioStatus +
        (_remoteControlStatus.isEmpty() ? QString() : "\n" + _remoteControlStatus) :
        _screenShareText + (_remoteControlStatus.isEmpty() ? QString() : "\n" + _remoteControlStatus));
    _screenQualityButton->setVisible(_screenShareActive && !detached);
    _annotationButton->setVisible(_screenShareActive && !detached);
    _screenAudioButton->setVisible(_screenShareAudioControlNeeded);
    if (relayout) {
        QResizeEvent layout(size(), size());
        resizeEvent(&layout);
    }
}

void MeetingRoomWindow::applyScreenShareSnapshot(livekit::ScreenShareSnapshot snapshot) {
	using State = livekit::ScreenShareState;
	_bottomBar->setScreenShareState(snapshot.state);
	if (!_screenShareBanner) {
		_screenShareBanner = new QLabel(this);
        _screenShareBanner->setObjectName(QStringLiteral("screenShareBanner"));
		_screenShareBanner->setTextFormat(Qt::PlainText);
		_screenShareBanner->setWordWrap(true);
		_screenShareBanner->setAlignment(Qt::AlignCenter);
        _screenShareBanner->setContentsMargins(136, 0, 110, 0);
        _screenQualityButton = new QPushButton(QCoreApplication::translate("MeetingUI", "Share quality"), _screenShareBanner);
        _screenQualityButton->setObjectName(QStringLiteral("screenShareQuality"));
        connect(_screenQualityButton, &QPushButton::clicked, this, [this] {
            if (!_coordinator || _coordinator->state() != OpenMeeting::MeetingState::InMeeting) return;
            const auto state = _coordinator->screenShareSnapshot();
            QWidget* owner = _sharedWindow && _detachedShareIntent && _detachedShareIntent->local
                ? static_cast<QWidget*>(_sharedWindow.get()) : this;
            auto* dialog = new QDialog(owner);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->setObjectName(QStringLiteral("screenShareQualityDialog"));
            dialog->setWindowTitle(QCoreApplication::translate("MeetingUI", "Screen share quality"));
            AppTheme::setTone(*dialog, AppTheme::Tone::Dark);
            auto* layout = new QVBoxLayout(dialog);
            layout->addWidget(new QLabel(QCoreApplication::translate("MeetingUI", "Resolution (keeps aspect ratio; smaller sources are not enlarged)"), dialog));
            auto* resolution = createElidedAccessibleComboBox(dialog);
            resolution->setObjectName(QStringLiteral("activeScreenShareResolution"));
            resolution->setAccessibleName(QCoreApplication::translate("MeetingUI", "Screen share resolution"));
            const auto pixels = state.source_kind == livekit::DesktopSourceKind::Screen && state.annotation_binding
                ? QSize(state.annotation_binding->physical_width, state.annotation_binding->physical_height)
                : state.applied_quality
                    ? QSize(state.applied_quality->source_width, state.applied_quality->source_height)
                    : screenShareDisplayPixelSize(screen());
            populateScreenShareResolutionChoices(*resolution, pixels);
            selectScreenShareResolution(*resolution, static_cast<int>(state.requested_quality.resolution));
            layout->addWidget(resolution);
            auto* fps = createElidedAccessibleComboBox(dialog);
            fps->setObjectName(QStringLiteral("activeScreenShareFps"));
            for (int rate : {15, 20, 30}) fps->addItem(QString::number(rate) + " FPS", rate);
            fps->setCurrentIndex(fps->findData(state.requested_quality.fps));
            layout->addWidget(fps);
            auto* qualityHint = new QLabel(dialog);
            qualityHint->setObjectName(QStringLiteral("activeScreenShareQualityHint"));
            qualityHint->setWordWrap(true);
            qualityHint->setVisible(false);
            layout->addWidget(qualityHint);
            const auto updateActiveQualityHints = [this, state, pixels, resolution, fps, qualityHint] {
                const int selectedFps = fps->currentData().isValid() ? fps->currentData().toInt() : state.requested_quality.fps;
                const auto ceiling = _coordinator
                    ? _coordinator->evaluateActiveScreenShareCodecCeiling(selectedFps)
                    : livekit::ScreenShareQualityConstraint{};
                annotateScreenShareResolutionChoices(*resolution, pixels, ceiling);
                QStringList notes;
                if (state.quality_downgraded() && state.applied_quality) {
                    const auto downgrade = screenShareDowngradeSummary(
                        state.requested_quality.resolution, state.quality_constraint);
                    notes.push_back(QCoreApplication::translate("MeetingUI", "Currently active: %1 × %2, %3 FPS%4")
                        .arg(state.applied_quality->width)
                        .arg(state.applied_quality->height)
                        .arg(state.applied_quality->quality.fps)
                        .arg(downgrade));
                }
                const auto selectedRes = static_cast<livekit::ScreenShareResolution>(resolution->currentData().toInt());
                const auto hint = screenShareConstraintHintText(pixels, selectedRes, ceiling);
                if (!hint.isEmpty()) notes.push_back(hint);
                const auto combined = notes.join(QLatin1Char('\n'));
                qualityHint->setText(combined);
                qualityHint->setVisible(!combined.isEmpty());
            };
            updateActiveQualityHints();
            connect(resolution, QOverload<int>::of(&QComboBox::currentIndexChanged), dialog,
                [updateActiveQualityHints](int) { updateActiveQualityHints(); });
            connect(fps, QOverload<int>::of(&QComboBox::currentIndexChanged), dialog,
                [updateActiveQualityHints](int) { updateActiveQualityHints(); });
            auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close, dialog);
            layout->addWidget(buttons);
            buttons->button(QDialogButtonBox::Apply)->setObjectName(QStringLiteral("screenShareQualityApply"));
            AppTheme::setStyleVariant(*dialog, "screen-share-picker");
            AppTheme::makeDialogAdaptive(*dialog, QSize(420, 260));
            dialog->setMaximumWidth(560);
            connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, dialog, [this, resolution, fps, dialog] {
                if (_coordinator) _coordinator->setScreenShareQuality({static_cast<livekit::ScreenShareResolution>(resolution->currentData().toInt()), fps->currentData().toInt()});
                dialog->accept();
            });
            connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
            dialog->open();
        });
		MeetingUI::AppTheme::setStyleVariant(*_screenShareBanner, "meeting-room-window-screensharebanner");
		_annotationButton = new QPushButton(QCoreApplication::translate("MeetingUI", "Annotate"), _screenShareBanner);
		_annotationButton->setObjectName(QStringLiteral("screenShareAnnotation"));
		connect(_annotationButton, &QPushButton::clicked,
			this, &MeetingRoomWindow::openAnnotationOverlay);
        _remoteControlStopButton = new QPushButton(tr("终止控制"), _screenShareBanner);
        _remoteControlStopButton->setObjectName(QStringLiteral("remoteControlStop"));
        connect(_remoteControlStopButton, &QPushButton::clicked, this, [this] {
            if (_remoteControlUi) _remoteControlUi->stop();
        });
        _screenAudioButton = new QPushButton(QCoreApplication::translate("MeetingUI", "Shared audio"), _screenShareBanner);
        _screenAudioButton->setObjectName(QStringLiteral("screenShareAudioControls"));
        connect(_screenAudioButton, &QPushButton::clicked, this, [this] {
            if (!_coordinator) return;
            const auto state = _coordinator->screenShareSnapshot();
            const std::weak_ptr<livekit::Room> room = _coordinator->room();
            using AudioState = livekit::ScreenShareAudioState;
            auto *menu = new QMenu(_screenAudioButton);
            menu->setObjectName(QStringLiteral("screenShareAudioMenu"));
            AppTheme::styleMenu(*menu, AppTheme::Tone::Dark);
            connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
            if (state.state == livekit::ScreenShareState::Active &&
                (state.audio_state == AudioState::Disabled || state.audio_state == AudioState::Failed)) {
                auto *start = menu->addAction(QCoreApplication::translate("MeetingUI",
                    state.audio_state == AudioState::Failed ? "Retry shared audio…" : "Share system audio…"));
                start->setObjectName(QStringLiteral("screenShareAudioEnable"));
                start->setEnabled(_coordinator->isMeetingReady() && _coordinator->systemAudioSharingAvailable());
                connect(start, &QAction::triggered, this, [this, room, run = state.run_id, epoch = state.audio_epoch] {
                    if (_coordinator && !room.expired() && _coordinator->room() == room.lock())
                        openScreenShareAudioPicker(run, epoch);
                });
            }
            if (state.audio_state == AudioState::Active) {
                auto *mute = menu->addAction(QCoreApplication::translate("MeetingUI",
                    state.audio_muted ? "Unmute shared audio" : "Mute shared audio"));
                mute->setObjectName(QStringLiteral("screenShareAudioMute"));
                mute->setEnabled(_coordinator->state() == OpenMeeting::MeetingState::InMeeting);
                connect(mute, &QAction::triggered, this, [this, room, run = state.run_id,
                        epoch = state.audio_epoch, muted = !state.audio_muted] {
                    if (_coordinator && !room.expired() && _coordinator->room() == room.lock())
                        _coordinator->setScreenShareAudioMuted(muted, run, epoch);
                });
            }
            if (state.audio_state != AudioState::Disabled && state.audio_state != AudioState::Failed) {
                auto *stop = menu->addAction(QCoreApplication::translate("MeetingUI",
                    state.audio_state == AudioState::StopFailed ? "Retry stopping shared audio" : "Stop sharing audio"));
                stop->setObjectName(QStringLiteral("screenShareAudioStop"));
                stop->setEnabled(state.audio_state != AudioState::Stopping);
                connect(stop, &QAction::triggered, this, [this, room, run = state.run_id, epoch = state.audio_epoch] {
                    if (_coordinator && !room.expired() && _coordinator->room() == room.lock())
                        _coordinator->stopScreenShareAudio(run, epoch);
                });
            }
            menu->popup(_screenAudioButton->mapToGlobal(QPoint(0, _screenAudioButton->height())));
        });
	}
	const bool active = snapshot.state == State::Active;
    _screenShareActive = active;
    _screenShareBannerNeeded = active || snapshot.state == State::AwaitingPermission ||
        snapshot.state == State::Starting ||
        snapshot.state == State::Stopping || snapshot.state == State::StopFailed;
	std::optional<livekit::ScreenBinding> nextBinding;
	if (active && snapshot.source_kind == livekit::DesktopSourceKind::Screen &&
		snapshot.annotation_binding) {
		nextBinding = snapshot.annotation_binding;
	}
	if (_annotationOverlay && (!nextBinding || !_annotationBinding ||
		*nextBinding != *_annotationBinding)) {
		closeAnnotationOverlay();
	}
	_annotationBinding = std::move(nextBinding);
	const auto shareTitle =
#ifdef Q_OS_LINUX
		QCoreApplication::translate("MeetingUI",
			snapshot.source_kind == livekit::DesktopSourceKind::Window ? "Window" : "Screen");
#else
		QString::fromStdString(snapshot.source_title).isEmpty()
			? QCoreApplication::translate("MeetingUI", "Screen")
			: QString::fromStdString(snapshot.source_title);
#endif
	_screenShareText = active ? QCoreApplication::translate("MeetingUI", "Sharing: %1").arg(
		shareTitle) :
		snapshot.state == State::AwaitingPermission ? QCoreApplication::translate("MeetingUI", "Choose a screen or window in the system picker. Waiting for permission…") :
		snapshot.state == State::Starting ? QCoreApplication::translate("MeetingUI", "Starting screen sharing...") :
		snapshot.state == State::StopFailed ? QCoreApplication::translate("MeetingUI", "Capture stopped, but unpublishing failed. Try stopping sharing again.") :
		QCoreApplication::translate("MeetingUI", "Stopping screen sharing...");
    _screenQualityButton->setVisible(active);
    _screenQualityButton->setEnabled(active && _coordinator && _coordinator->state() == OpenMeeting::MeetingState::InMeeting &&
        snapshot.quality_status != livekit::ScreenShareQualityStatus::Degraded);
    QString qualityText;
    if (snapshot.applied_quality) qualityText = QCoreApplication::translate("MeetingUI", "%1 × %2, %3 FPS").arg(snapshot.applied_quality->width).arg(snapshot.applied_quality->height).arg(snapshot.applied_quality->quality.fps);
    const auto dimensionsLength = qualityText.size();
    const bool showDowngradeReason = snapshot.quality_downgraded() &&
        (snapshot.quality_status == livekit::ScreenShareQualityStatus::Applied ||
         snapshot.quality_status == livekit::ScreenShareQualityStatus::MetadataPending);
    if (showDowngradeReason) {
        qualityText += screenShareDowngradeSummary(
            snapshot.requested_quality.resolution, snapshot.quality_constraint);
    }
    switch (snapshot.quality_status) {
    case livekit::ScreenShareQualityStatus::Pending: qualityText += QCoreApplication::translate("MeetingUI", " — Applying…"); break;
    case livekit::ScreenShareQualityStatus::Rejected: qualityText += QCoreApplication::translate("MeetingUI", " — Change failed; previous quality restored"); break;
    case livekit::ScreenShareQualityStatus::MetadataPending: qualityText += QCoreApplication::translate("MeetingUI", " — Applied locally; synchronisation pending"); break;
    case livekit::ScreenShareQualityStatus::Degraded: qualityText += QCoreApplication::translate("MeetingUI", " — Quality state uncertain; stop sharing to recover"); break;
    default: break;
    }
    _screenShareDetails = snapshot.applied_quality
        ? QCoreApplication::translate("MeetingUI", "%1 FPS").arg(snapshot.applied_quality->quality.fps)
        : QString();
    _screenShareDetails += qualityText.mid(dimensionsLength);
    _screenQualityButton->setToolTip(qualityText);
    if (active) _screenShareText += "  " + qualityText;
    using AudioState = livekit::ScreenShareAudioState;
    _screenShareAudioStatus.clear();
    switch (snapshot.audio_state) {
    case AudioState::Disabled:
#ifdef Q_OS_LINUX
        if (active) _screenShareAudioStatus = QCoreApplication::translate("MeetingUI", "System audio off");
#endif
        break;
    case AudioState::Starting:
        _screenShareAudioStatus = QCoreApplication::translate("MeetingUI", "Starting shared audio…"); break;
    case AudioState::Active:
        _screenShareAudioStatus = QCoreApplication::translate("MeetingUI", snapshot.audio_muted
            ? "Shared audio muted" : "Sharing other application audio"); break;
    case AudioState::Stopping:
        _screenShareAudioStatus = QCoreApplication::translate("MeetingUI", "Stopping shared audio…"); break;
    case AudioState::Failed:
        _screenShareAudioStatus = active
            ? QCoreApplication::translate("MeetingUI", "System audio failed; video sharing continues")
            : QCoreApplication::translate("MeetingUI", "System audio failed"); break;
    case AudioState::StopFailed:
        _screenShareAudioStatus = QCoreApplication::translate("MeetingUI", "Audio capture stopped; retry removing its publication"); break;
    default: break;
    }
    if (!_screenShareAudioStatus.isEmpty() && !snapshot.audio_output_name.empty())
        _screenShareAudioStatus += QStringLiteral(" — ") + QString::fromStdString(snapshot.audio_output_name);
    if (!_screenShareAudioStatus.isEmpty()) _screenShareText += "\n" + _screenShareAudioStatus;
    _screenShareAudioControlNeeded = snapshot.audio_state == AudioState::Starting ||
        snapshot.audio_state == AudioState::Active || snapshot.audio_state == AudioState::Stopping ||
        snapshot.audio_state == AudioState::StopFailed;
#ifdef Q_OS_LINUX
    _screenShareAudioControlNeeded = _screenShareAudioControlNeeded || active;
#endif
    const bool canStartAudio = active && _coordinator && _coordinator->systemAudioSharingAvailable() &&
        _coordinator->isMeetingReady();
    const bool canStopAudio = snapshot.audio_state == AudioState::Starting ||
        snapshot.audio_state == AudioState::Active || snapshot.audio_state == AudioState::StopFailed;
    _screenAudioButton->setEnabled(canStartAudio || canStopAudio);
    _screenAudioButton->setToolTip(active && _coordinator && !_coordinator->systemAudioSharingAvailable()
        ? QCoreApplication::translate("MeetingUI", "System audio sharing is unavailable in this build. Video sharing is available.")
        : _screenShareAudioStatus);
    _screenShareBanner->setText(_screenShareText + (_remoteControlStatus.isEmpty() ? QString() : "\n" + _remoteControlStatus));
    _remoteControlStopButton->setVisible(active && !_remoteControlStatus.isEmpty());
	_annotationButton->setVisible(active);
#ifdef Q_OS_LINUX
	if (active) {
		_annotationButton->setToolTip(QCoreApplication::translate(
			"MeetingUI", "Annotation is not available for Wayland sharing yet."));
	}
#else
	if (active && snapshot.source_kind == livekit::DesktopSourceKind::Window) {
		_annotationButton->setToolTip(QCoreApplication::translate(
			"MeetingUI", "Only full-screen sharing can be annotated."));
	} else if (active && !_annotationBinding) {
		_annotationButton->setToolTip(QCoreApplication::translate(
			"MeetingUI", "The shared screen could not be mapped reliably."));
	} else {
		_annotationButton->setToolTip(QCoreApplication::translate(
			"MeetingUI", "Annotate the shared screen"));
	}
#endif
	setAnnotationInteractionEnabled(
		_coordinator && _coordinator->state() == OpenMeeting::MeetingState::InMeeting);
    auto nextPreview = active ? snapshot.preview : nullptr;
    if (_localScreenPreview && _localScreenPreview != nextPreview) {
        _localScreenPreview->DiscardLatest("screen", _localScreenPreview->generation());
    }
    _localScreenPreview = std::move(nextPreview);
	if (active && !_localScreenTile) {
		_localScreenTile = std::make_unique<VideoTileWidget>(QCoreApplication::translate("MeetingUI", "My Screen Share"), true, _stageContainer, true);
		_localScreenTile->setIdentity(QStringLiteral("local-screen"));
		_localScreenTile->setVideoActive(true);
		bindTileInteractions(_localScreenTile.get());
	} else if (!active && _localScreenTile) {
        if (_detachedShareIntent && _detachedShareIntent->local) {
            _detachedShareIntent.reset();
            _displayedSharedSurfaceId = 0;
            _sharedWindow->hide();
        }
		removeUserFromCanvases(_localScreenTile->renderKey().toStdString());
		if (_pinnedLocalTarget == livekit::LocalVideoTarget::ScreenShare) {
			_pinnedLocalTarget.reset();
			_pinnedRenderKey.clear();
		}
		_localScreenTile.reset();
	}
    syncScreenShareControls();
	QResizeEvent layout(size(), size());
	resizeEvent(&layout);
	scheduleViewportIntent(true);
}

void MeetingRoomWindow::openAnnotationOverlay() {
    if (_remoteControlUi && _remoteControlUi->active()) return;
	if (_annotationOverlay || !_annotationBinding || !_coordinator ||
		_coordinator->state() != OpenMeeting::MeetingState::InMeeting) {
		return;
	}
	try {
		auto overlay = std::make_unique<AnnotationOverlayWindow>(
			*_annotationBinding, !_annotationOffscreenForDiagnostics);
		auto *raw = overlay.get();
		const QPointer<AnnotationOverlayWindow> guard(raw);
		connect(raw, &AnnotationOverlayWindow::closeRequested, this, [this, guard] {
			QMetaObject::invokeMethod(this, [this, guard] {
				if (_annotationOverlay.get() == guard.data()) closeAnnotationOverlay();
			}, Qt::QueuedConnection);
		});
		connect(raw, &AnnotationOverlayWindow::bindingInvalidated, this, [this, guard] {
			if (_annotationOverlay.get() != guard.data()) return;
			auto retired = std::move(_annotationOverlay);
			retired.release()->deleteLater();
			_annotationBinding.reset();
			if (_annotationButton) {
				_annotationButton->setEnabled(false);
				_annotationButton->setToolTip(QCoreApplication::translate(
					"MeetingUI", "The shared screen could not be mapped reliably."));
			}
            syncScreenShareControls();
			// The overlay's coordinates are invalid. Native ScreenShareSession
			// still validates source identity and owns capture termination.
		});
		_annotationOverlay = std::move(overlay);
	} catch (const std::exception &) {
		_annotationBinding.reset();
		if (_annotationButton) {
			_annotationButton->setEnabled(false);
			_annotationButton->setToolTip(QCoreApplication::translate(
				"MeetingUI", "The shared screen could not be mapped reliably."));
		}
        syncScreenShareControls();
		QMessageBox::warning(this,
			QCoreApplication::translate("MeetingUI", "Screen annotation"),
			QCoreApplication::translate("MeetingUI", "The shared screen could not be mapped reliably."));
	}
}

void MeetingRoomWindow::closeAnnotationOverlay() {
	if (!_annotationOverlay) return;
	_annotationOverlay->closeOverlay();
	_annotationOverlay.reset();
}

void MeetingRoomWindow::setAnnotationInteractionEnabled(bool enabled) {
    enabled = enabled && !(_remoteControlUi && _remoteControlUi->active());
    if (_screenQualityButton) _screenQualityButton->setEnabled(enabled && _coordinator && _coordinator->screenShareSnapshot().quality_status != livekit::ScreenShareQualityStatus::Degraded);
	if (_annotationOverlay) _annotationOverlay->setInteractionEnabled(enabled);
	if (_annotationButton) _annotationButton->setEnabled(enabled && _annotationBinding.has_value());
    syncScreenShareControls();
}

void MeetingRoomWindow::setupCoordinatorBindings() {
    setupRemoteControl();
	if (!_coordinator) return;
    connect(_topBar, &RoomTopBarWidget::telemetryDetailsRequested, this, [this] {
        if (_telemetryDialog) {
            _telemetryDialog->showNormal(); _telemetryDialog->raise(); _telemetryDialog->activateWindow();
            return;
        }
        const auto context = _coordinator->diagnosticContext();
        _telemetryDialog = OpenLiveTelemetryDialog(this,
            {std::string(context.anonymous_session_id.View()), context.session_generation},
            livekit::telemetry::InstalledTelemetryHistoryStore());
    });
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::sessionStopping,
		this, [this] {
            stopLiveKitSession(false);
        }, Qt::DirectConnection);
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::telemetrySnapshotChanged,
		this, [this](const QVariantMap &snapshot) {
			_topBar->setTelemetrySnapshot(snapshot);
		});
	_bottomBar->shareScreenClicked() | rpl::on_next([this] { requestScreenShare(); }, lifetime());
	_bottomBar->setScreenShareAvailable(_coordinator->canStartScreenShare());
	_bottomBar->setWhiteboardHasContent(_coordinator->whiteboardHasContent());
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::whiteboardContentChanged,
		_bottomBar, &RoomBottomBarWidget::setWhiteboardHasContent);
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::screenShareAvailabilityChanged,
		_bottomBar, &RoomBottomBarWidget::setScreenShareAvailable);
	applyScreenShareSnapshot(_coordinator->screenShareSnapshot());
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::screenShareChanged,
		this, [this](livekit::ScreenShareSnapshot snapshot) {
			applyScreenShareSnapshot(snapshot);
            const auto previousAudioError = _lastScreenAudioError;
            _lastScreenAudioError = snapshot.audio_error;
            if (snapshot.state == livekit::ScreenShareState::Active &&
                snapshot.audio_error != livekit::ScreenShareAudioError::None &&
                snapshot.audio_error != previousAudioError) {
                const auto message = snapshot.audio_error == livekit::ScreenShareAudioError::Unpublish
                    ? QCoreApplication::translate("MeetingUI", "Shared audio capture has stopped, but removal of its publication is not confirmed. Retry stopping shared audio or leave the meeting. Video sharing continues.")
                    : snapshot.audio_error == livekit::ScreenShareAudioError::Unavailable
                    ? QCoreApplication::translate("MeetingUI", "System audio sharing is unavailable. Video sharing continues.")
                    : QCoreApplication::translate("MeetingUI", "Shared audio could not be started or its selected output became unavailable. Audio capture has stopped. Video sharing continues.");
                ShowMeetingWarning(this, "meetingScreenShareAudioFailure", "meetingScreenShareAudioFailureDismiss",
                    QCoreApplication::translate("MeetingUI", "Shared audio"), message);
            }
			if (snapshot.error == livekit::ScreenShareError::None) return;
			QString message;
			if (snapshot.error == livekit::ScreenShareError::PermissionDenied)
				message = QCoreApplication::translate("MeetingUI", "Screen sharing permission was denied. You can try again and allow access in the system picker.");
			else if (snapshot.error == livekit::ScreenShareError::Capture)
				message = QCoreApplication::translate("MeetingUI", "The selected screen or window is no longer available. Sharing has stopped. You can select another source.");
			else if (snapshot.error == livekit::ScreenShareError::Publish)
				message = QCoreApplication::translate("MeetingUI", "Unable to publish the screen share. Capture has stopped. Check your connection and publishing permissions, then try again.");
			else message = QCoreApplication::translate("MeetingUI", "Local capture has stopped, but remote unpublishing is not yet confirmed. Try stopping again or leave the meeting.");
			ShowMeetingWarning(this, "meetingScreenShareFailure", "meetingScreenShareFailureDismiss",
				QCoreApplication::translate("MeetingUI", "Screen Sharing"), message);
		});
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::screenShareSourcesReady,
		this, [this](const std::vector<livekit::DesktopSource> &sources) {
			handleScreenShareSources(sources);
		});

	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::remoteVideoTrackAvailable,
	        this, [this](const QString &id, std::shared_ptr<livekit::Track> track) {
			if (!_coordinator) return;
			const auto presentation = _coordinator->participantPresentation(id);
			if (!presentation) return;
			for (const auto &value : presentation->videoTracks) {
				if (value.track != track ||
					!_coordinator->isParticipantPresentationCurrent(*presentation, &value)) continue;
				const bool isScreen = track && track->source() == livekit::TrackSource::ScreenShareVideo;
				const bool cameraTileAlreadyPlaced = !isScreen &&
					_usingGpuBackend.load(std::memory_order_acquire) &&
					_remoteTiles.find(id) != _remoteTiles.end();
				attachRemoteVideo(*presentation, value, true, !cameraTileAlreadyPlaced);
				return;
			}
		});
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::videoDemandPlanAccepted,
		this, &MeetingRoomWindow::applyAcceptedVideoDemandPlan);
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::remoteVideoTrackUnavailable,
	        this, [this](const QString &, const QString &trackSid) { removeRemoteVideo(trackSid); });
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::participantJoined,
	        this, [this](const QString &id, const QString &) {
		if (!_coordinator) return;
		if (const auto presentation = _coordinator->participantPresentation(id)) {
			applyParticipantPresentation(*presentation);
		}
	});
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::participantLeft,
	        this, &MeetingRoomWindow::onRemoteParticipantLeft);
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::remoteTrackMuted,
	        this, [this](const QString &id, bool isVideo, bool muted) {
		onRemoteTrackMuted(id, isVideo, muted);
	});
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::activeSpeakersChanged,
	        this, &MeetingRoomWindow::updateActiveSpeakers);
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::transcriptionReceived,
	        this, &MeetingRoomWindow::onMeetingTranscriptionReceived);
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::meetingDetailUpdated,
	        this, &MeetingRoomWindow::onMeetingDetailUpdated);
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::roomInfoUpdated,
	        this, [this](const OpenMeeting::MeetingRoomInfo &info) {
		if (!_coordinator || info.name.isEmpty() ||
			!_coordinator->meetingDetail().meetingName.isEmpty()) {
			return;
		}
		const QString meetingId = _coordinator->currentMeetingId();
		setWindowTitle(meetingId.isEmpty()
			? QCoreApplication::translate("MeetingUI", "Cohavora - %1").arg(info.name)
			: QCoreApplication::translate("MeetingUI", "Cohavora - %1 - Meeting ID: %2").arg(info.name, meetingId));
	});
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::participantsUpdated,
	        this, [this](const std::vector<OpenMeeting::ParticipantInfo> &list) {
		const QString ownAgentId = _roomAgentPublisher
			? _roomAgentPublisher->activeAgentIdentity()
			: QString();
		int agentCount = 0;
		int visibleParticipantCount = 0;
		std::vector<QString> staleAgentIdentities;
		QString primaryAgentName;
		QString bestAgentState;
		int bestAgentPriority = -1;
		const auto agentStatePriority = [](const QString &state) -> int {
			const QString lower = state.trimmed().toLower();
			if (lower == QStringLiteral("speaking")) return 4;
			if (lower == QStringLiteral("thinking")) return 3;
			if (lower == QStringLiteral("listening")) return 2;
			if (lower == QStringLiteral("initializing")) return 1;
			return 0;
		};
		for (const auto &p : list) {
			const bool hiddenAgent = p.isAgent ||
				(!ownAgentId.isEmpty() && p.identity == ownAgentId);
			if (hiddenAgent) {
				if (ownAgentId.isEmpty() || p.identity != ownAgentId) {
					++agentCount;
					const QString display = p.name.trimmed().isEmpty() ? p.identity : p.name.trimmed();
					if (primaryAgentName.isEmpty()) {
						primaryAgentName = display;
					}
					const int prio = agentStatePriority(p.agentState);
					if (prio > bestAgentPriority) {
						bestAgentPriority = prio;
						bestAgentState = p.agentState;
						if (!display.isEmpty()) {
							primaryAgentName = display;
						}
					}
				}
				_remoteParticipantNames.erase(p.identity);
				if (_remoteTiles.count(p.identity) > 0) {
					staleAgentIdentities.push_back(p.identity);
				}
				continue;
			}
			++visibleParticipantCount;
			if (p.isLocal) {
				if (_localTile) {
					_localTile->setConnectionQuality(p.connectionQuality);
				}
				continue;
			}
			_remoteParticipantNames[p.identity] =
				p.name.isEmpty() ? p.identity : p.name;
			auto it = _remoteTiles.find(p.identity);
			if (it != _remoteTiles.end() && it->second) {
				if (!p.name.isEmpty() && it->second->displayName() != p.name) {
					it->second->setDisplayName(p.name);
				}
				it->second->setAudioMuted(p.isAudioMuted);
				it->second->setConnectionQuality(p.connectionQuality);
			}
		}
		for (const auto &agentId : staleAgentIdentities) {
			onRemoteParticipantLeft(agentId);
		}
		_participantCount = visibleParticipantCount;
		_bottomBar->setParticipantCount(_participantCount);
		_hasRemoteRoomAgent = (agentCount > 0);
		_remoteRoomAgentName = primaryAgentName;
		_remoteRoomAgentState = bestAgentState;
		_remoteRoomAgentCount = agentCount;
		refreshTopBarAgentStatus();
		if (_coordinator && _coordinator->remoteVideoTracksRevision() != _lastSyncedVideoTracksRevision) {
			refreshRemoteVideoPresentations();
		}
	});
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::trackSubscriptionPermissionChanged,
	        this, [this](const QString &identity, const QString &participantSid,
	                     const QString &trackSid, bool allowed) {
		LogToConsole(LogCategory::Connection, "SUBSCRIPTION_PERMISSION",
			QCoreApplication::translate("MeetingUI", "Subscription permission updated: participant=%1 sid=%2 track=%3 allowed=%4")
				.arg(identity.isEmpty() ? QCoreApplication::translate("MeetingUI", "Unknown") : identity,
					 participantSid, trackSid, allowed ? QCoreApplication::translate("MeetingUI", "Yes") : QCoreApplication::translate("MeetingUI", "No")));
	});
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::localAudioMuteChanged,
	        this, [this](bool muted) {
		_config.audioMuted = muted;
		_bottomBar->setAudioMuted(muted);
		_localTile->setAudioMuted(muted);
		if (_wasapiCap) {
			queueMicrophoneMute(muted);
		}
	});
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::localVideoEnableChanged,
	        this, [this](bool enabled) {
		_config.videoEnabled = enabled;
		_bottomBar->setVideoEnabled(enabled);
		_localTile->setVideoActive(enabled && _usingRealCamera);
		updateVideoLayout();
	});
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::kickedOff,
	        this, &MeetingRoomWindow::onKickedOff);
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::meetingKickOff,
	        this, &MeetingRoomWindow::onMeetingKickOff);
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::remoteMuteRequested,
	        this, &MeetingRoomWindow::onRemoteMuteRequested);
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::hostRoleChanged,
	        this, &MeetingRoomWindow::onHostRoleChanged);
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::meetingDetailUpdated,
	        this, &MeetingRoomWindow::onMeetingDetailUpdated);
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::stateChanged,
	        this, [this](OpenMeeting::MeetingState state, const QString &detail) {
		_bottomBar->setScreenShareAvailable(_coordinator->canStartScreenShare());
		if (state != OpenMeeting::MeetingState::InMeeting) {
			_defaultScreenSharePending = false;
		}
		if (state == OpenMeeting::MeetingState::Leaving
			|| state == OpenMeeting::MeetingState::Failed) {
			invalidateCameraCompletion();
			applyScreenShareSnapshot({});
		}
		QPointer<MeetingRoomWindow> guard(this);
		updateRecoveryStateUi(state, detail);
		if (!guard) {
			return;
		}
		if (state == OpenMeeting::MeetingState::InMeeting) {
			_room = _coordinator->room();
			const auto preferences = OpenMeeting::SessionManager::instance().mediaPreferences();
			// Apply stored join preferences only once; subsequent Reconnecting -> InMeeting
			// transitions (both SignalReconnect and FullReconnect) must preserve the user's
			// in-meeting speaker mute state.
			if (!_speakerStateInitialized) {
				_speakerRequestedMuted = !preferences.enableSpeaker;
				_speakerStateInitialized = true;
			}
			const bool muted = _speakerRequestedMuted;
			if (_speakerConfiguredRoom.lock() != _room) {
				_speakerConfiguredRoom = _room;
#ifdef Q_OS_LINUX
				requestSpeakerDevice(preferences.speakerDeviceId, true);
#else
				if (!selectSpeakerDevice(preferences.speakerDeviceId)
					&& !preferences.speakerDeviceId.isEmpty()) {
					selectSpeakerDevice(QString());
				}
#endif
				setSpeakerOutputMuted(muted);
			} else {
#ifdef Q_OS_LINUX
				if (!_speakerDevicePending) applyEffectiveSpeakerMute();
#else
				applyEffectiveSpeakerMute();
#endif
			}
			restoreParticipantPresentations();
			maybeAutoStartMeetingTranslation();
		}
	});
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::errorOccurred,
	        this, [this](const QString &title, const QString &message) {
		if (_closingForSessionInvalidation ||
			OpenMeeting::SessionManager::instance().isSessionInvalidating() ||
			!_coordinator) {
			return;
		}
		const auto state = _coordinator->state();
		if (state == OpenMeeting::MeetingState::Failed) {
			invalidateCameraCompletion();
			QPointer<MeetingRoomWindow> guard(this);
			LogToConsole(LogCategory::Error, "SESSION_STARTUP", QString("%1: %2").arg(title, message));
			if (!guard) {
				return;
			}
			showDepartureNotice(title, message, QMessageBox::Critical);
			return;
		}
		LogToConsole(LogCategory::Error, "SESSION_OPERATION", QString("%1: %2").arg(title, message));
		if (state == OpenMeeting::MeetingState::Leaving ||
			state == OpenMeeting::MeetingState::Idle ||
			!isVisible()) {
			showStandaloneNotice(
				QStringLiteral("meetingOperationError"),
				QMessageBox::Warning,
				title,
				message);
			return;
		}
		auto *notice = new QMessageBox(QMessageBox::Warning, title, message,
			QMessageBox::Ok, this);
		notice->setObjectName(QStringLiteral("meetingOperationError"));
		notice->setAttribute(Qt::WA_DeleteOnClose);
		notice->open();
	});
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::meetingDisconnected,
	        this, [this](livekit::RoomDisconnectReason reason) {
		if (_closingForSessionInvalidation ||
			OpenMeeting::SessionManager::instance().isSessionInvalidating()) {
			stopLiveKitSession(false);
			close();
			return;
		}
		if (_departureNotice) {
			stopLiveKitSession(false);
			return;
		}
		if (!_pendingDepartureTitle.isEmpty()) {
			const auto title = _pendingDepartureTitle;
			const auto message = _pendingDepartureMessage;
			_pendingDepartureTitle.clear();
			_pendingDepartureMessage.clear();
			if (!_standaloneDepartureNoticeShown) {
				_standaloneDepartureNoticeShown = true;
				closeAnnotationOverlay();
				_annotationBinding.reset();
				showStandaloneNotice(
					QStringLiteral("meetingDepartureNotice"),
					QMessageBox::Warning,
					title,
					message);
			}
			stopLiveKitSession(false);
			close();
			return;
		}
		if (reason != livekit::RoomDisconnectReason::UserLeave &&
			!_standaloneDepartureNoticeShown) {
			_standaloneDepartureNoticeShown = true;
			closeAnnotationOverlay();
			_annotationBinding.reset();
			QMessageBox::Icon icon = QMessageBox::Warning;
			QString title;
			QString message;
			switch (reason) {
			case livekit::RoomDisconnectReason::NetworkError:
				title = QCoreApplication::translate("MeetingUI", "Connection Lost");
				message = QCoreApplication::translate(
					"MeetingUI",
					"The meeting connection was lost and could not be restored. Please check your network and join again.");
				break;
			case livekit::RoomDisconnectReason::RoomDeleted:
			case livekit::RoomDisconnectReason::RoomClosed:
				icon = QMessageBox::Information;
				title = QCoreApplication::translate("MeetingUI", "Meeting Ended");
				message = QCoreApplication::translate(
					"MeetingUI",
					"This meeting has ended and you have been disconnected.");
				break;
			case livekit::RoomDisconnectReason::ServerShutdown:
				title = QCoreApplication::translate("MeetingUI", "Server Disconnected");
				message = QCoreApplication::translate(
					"MeetingUI",
					"The meeting server closed the connection. Please try joining again later.");
				break;
			case livekit::RoomDisconnectReason::ParticipantRemoved:
				title = QCoreApplication::translate("MeetingUI", "Removed from Meeting");
				message = QCoreApplication::translate(
					"MeetingUI",
					"You have been removed from the meeting.");
				break;
			case livekit::RoomDisconnectReason::Unknown:
			case livekit::RoomDisconnectReason::DuplicateIdentity:
			case livekit::RoomDisconnectReason::UserLeave:
				title = QCoreApplication::translate("MeetingUI", "Disconnected from Meeting");
				message = QCoreApplication::translate(
					"MeetingUI",
					"You were disconnected from the meeting unexpectedly. Please try joining again.");
				break;
			}
			showStandaloneNotice(
				QStringLiteral("meetingDepartureNotice"),
				icon,
				title,
				message);
		}
		stopLiveKitSession(false);
		close();
	});
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::meetingLeft,
	        this, [this]() {
		invalidateCameraCompletion();
		if (!_pendingDepartureTitle.isEmpty()) {
			// Capture callbacks were revoked by sessionStopping; the non-modal
			// notice remains responsive while native cleanup finishes separately.
			stopLiveKitSession();
			const auto title = _pendingDepartureTitle;
			const auto message = _pendingDepartureMessage;
			_pendingDepartureTitle.clear();
			_pendingDepartureMessage.clear();
			showDepartureNotice(title, message);
			return;
		}
		if (_departureNotice) {
			// Revoke presentation immediately while capture retirement completes.
			stopLiveKitSession();
			return;
		}
		stopLiveKitSession(false);
		close();
	});

	if (!_coordinator->currentMeetingId().isEmpty()) {
		if (_topBar) _topBar->setMeetingId(_coordinator->currentMeetingId());
		setWindowTitle(QCoreApplication::translate("MeetingUI", "Cohavora - Meeting ID: %1").arg(_coordinator->currentMeetingId()));
	}

	updateRecoveryStateUi(_coordinator->state());
}

void MeetingRoomWindow::closeStandaloneNotices() {
	const auto widgets = QApplication::topLevelWidgets();
	for (QWidget *widget : widgets) {
		auto *notice = qobject_cast<QMessageBox *>(widget);
		if (notice && !notice->parentWidget() &&
			notice->property("meetingStandaloneNotice").toBool()) {
			notice->close();
		}
	}
}

void MeetingRoomWindow::showStandaloneNotice(
		const QString &objectName,
		QMessageBox::Icon icon,
		const QString &title,
		const QString &message) {
	auto *notice = new QMessageBox(icon, title, message, QMessageBox::Ok, nullptr);
	notice->setObjectName(objectName);
	notice->setProperty("meetingStandaloneNotice", true);
	notice->setAttribute(Qt::WA_DeleteOnClose);
	notice->setAttribute(Qt::WA_QuitOnClose, false);
	notice->setWindowModality(Qt::NonModal);
	notice->setWindowFlag(Qt::WindowStaysOnTopHint, true);
	notice->setMinimumSize(460, 180);
	AppTheme::setTone(*notice, AppTheme::Tone::Light);
	notice->show();
	notice->raise();
	notice->activateWindow();
}

void MeetingRoomWindow::showDepartureNotice(const QString &title, const QString &message,
		QMessageBox::Icon icon) {
	if (_departureNotice || _closeRequested || _closingForSessionInvalidation ||
		OpenMeeting::SessionManager::instance().isSessionInvalidating()) {
		return;
	}
	// A screen-sized TOPMOST annotation surface can otherwise remain above the
	// meeting-owned dialog during the short kick/leave transition.
	closeAnnotationOverlay();
	_annotationBinding.reset();
	// Do not retain the retired meeting HWND as the native owner.  In particular,
	// QDialog::open() creates a window-modal owner chain while Room teardown is
	// changing that owner's native state.  A standalone non-modal notice keeps
	// its input HWND enabled and still avoids a nested event loop.
	auto *notice = new QMessageBox(icon, title, message, QMessageBox::Ok, nullptr);
	notice->setObjectName("meetingDepartureNotice");
	notice->setAttribute(Qt::WA_DeleteOnClose);
	notice->setAttribute(Qt::WA_QuitOnClose, false);
	notice->setWindowModality(Qt::NonModal);
	notice->setWindowFlag(Qt::WindowStaysOnTopHint, true);
	notice->setMinimumSize(460, 180);
	AppTheme::setTone(*notice, AppTheme::Tone::Light);
	_departureNotice = notice;
	connect(notice, &QDialog::finished, this, [this]() {
		_departureNotice.clear();
		_pendingDepartureTitle.clear();
		_pendingDepartureMessage.clear();
		close();
	});
	notice->show();
	notice->raise();
	notice->activateWindow();
}

void MeetingRoomWindow::dismissDepartureNotice() {
	_pendingDepartureTitle.clear();
	_pendingDepartureMessage.clear();
	if (!_departureNotice) return;
	auto *notice = _departureNotice.data();
	_departureNotice.clear();
	QObject::disconnect(notice, nullptr, this, nullptr);
	notice->close();
}

void MeetingRoomWindow::onKickedOff(const QString &reason, int reasonCode) {
	if (_closingForSessionInvalidation ||
		OpenMeeting::SessionManager::instance().isSessionInvalidating()) {
		return;
	}
	invalidateCameraCompletion();
	if (_pendingDepartureTitle.isEmpty()) {
		_pendingDepartureTitle = QCoreApplication::translate("MeetingUI", "Remove from Meeting");
		_pendingDepartureMessage = QCoreApplication::translate(
			"MeetingUI", "You were removed from the meeting by the host.\nReason: %1 (code: %2)")
			.arg(reason.isEmpty() ? QCoreApplication::translate("MeetingUI", "Not Specified") : reason)
			.arg(reasonCode);
	}
}

void MeetingRoomWindow::onMeetingKickOff(livekit::RoomDisconnectReason reason) {
	if (reason != livekit::RoomDisconnectReason::DuplicateIdentity) {
		return;
	}
	if (_closingForSessionInvalidation ||
		OpenMeeting::SessionManager::instance().isSessionInvalidating()) {
		LogToConsole(LogCategory::Connection, "DUPLICATE_IDENTITY_SUPPRESSED",
		             "[UI] Suppress room-level dialog while account session is invalidating");
		return;
	}

	invalidateCameraCompletion();
	QPointer<MeetingRoomWindow> guard(this);
	LogToConsole(LogCategory::Connection, "DUPLICATE_IDENTITY",
	             "[UI] Show duplicate login dialog");
	if (!guard) return;
	showDepartureNotice(
	                     QCoreApplication::translate("MeetingUI", "Meeting Left"),
	                     QCoreApplication::translate("MeetingUI", "Your account joined this meeting on another device. This client has been disconnected."));
}

void MeetingRoomWindow::onSessionInvalidated(OpenMeeting::SessionInvalidationReason reason) {
	invalidateCameraCompletion();
	dismissDepartureNotice();
	closeStandaloneNotices();
	if (_closingForSessionInvalidation) {
		return;
	}
	_closingForSessionInvalidation = true;
	QPointer<MeetingRoomWindow> guard(this);
	LogToConsole(LogCategory::Connection, "SESSION_INVALIDATED",
	             QString("[UI] Close meeting window for invalidated account session, reason=%1")
	                 .arg(static_cast<int>(reason)));

	// 不在会议窗口重复弹窗；全局提示和重新登录由 MeetingMainWindow 统一负责。
	if (guard) {
		guard->close();
	}
}

void MeetingRoomWindow::onRemoteMuteRequested(bool isVideo, bool mute, const QString &operatorId) {
	const auto operatorName = resolveParticipantDisplayName(_coordinator, operatorId, QString());
	if (isVideo) {
		if (mute) {
			requestLocalCamera(false);
			updateVideoLayout();
			LogToConsole(LogCategory::Media, "VIDEO", QCoreApplication::translate("MeetingUI", "Host [%1] turned off your camera").arg(operatorName));
		} else {
			if (QMessageBox::question(this, QCoreApplication::translate("MeetingUI", "Request to Enable Camera"),
				QCoreApplication::translate("MeetingUI", "Host [%1] would like you to turn on your camera. Allow?").arg(operatorName),
				QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
				requestLocalCamera(true);
				updateVideoLayout();
			}
		}
	} else {
		if (mute) {
			requestLocalMicrophone(true);
			LogToConsole(LogCategory::Media, "AUDIO", QCoreApplication::translate("MeetingUI", "Host [%1] muted you").arg(operatorName));
		} else {
			if (QMessageBox::question(this, QCoreApplication::translate("MeetingUI", "Request to Unmute"),
				QCoreApplication::translate("MeetingUI", "Host [%1] would like you to unmute your microphone. Allow?").arg(operatorName),
				QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
				requestLocalMicrophone(false);
			}
		}
	}
}

void MeetingRoomWindow::onMeetingDetailUpdated(const OpenMeeting::MeetingDetail &detail) {
	if (!detail.meetingName.isEmpty()) {
		setWindowTitle(QCoreApplication::translate("MeetingUI", "Cohavora - %1 - Meeting ID: %2").arg(detail.meetingName).arg(detail.meetingId));
	}
	if (_topBar && !detail.meetingId.isEmpty()) {
		_topBar->setMeetingId(detail.meetingId);
	}
	if (_participantsSidebar) {
		_participantsSidebar->updateParticipants(_coordinator ? _coordinator->participants() : std::vector<OpenMeeting::ParticipantInfo>{});
	}
}

void MeetingRoomWindow::onHostRoleChanged(const QString &newHostId, const QString &operatorName) {
	LogToConsole(LogCategory::Participant, "HOST", QCoreApplication::translate("MeetingUI", "Host transferred to: %1 (by: %2)").arg(newHostId).arg(operatorName));
	QMessageBox::information(this, QCoreApplication::translate("MeetingUI", "Host Changed"),
	                         QCoreApplication::translate("MeetingUI", "Participant [%1] is now the host").arg(newHostId));
}

void MeetingRoomWindow::beginLeaveTimingObservation() {
	if (!_coordinator) return;
	auto sample = _coordinator->beginLeaveTiming();
	QPointer<MeetingRoomWindow> room(this);
	QPointer<QWidget> before;
	for (auto *widget : QApplication::topLevelWidgets()) {
		if (widget->objectName() == QStringLiteral("MeetingMainWindow")) { before = widget; break; }
	}
	ObserveTimingBoundary(sample, [room, before] {
		return (!room || !room->isVisible()) && before && before->isVisible();
	});
}

void MeetingRoomWindow::handleEndMeetingClicked() {
	if (_preparingMedia) { close(); return; }
	if (_coordinator && _coordinator->isHost()) {
		QMessageBox box(this);
		box.setObjectName(QStringLiteral("meetingLeaveConfirmation"));
		box.setWindowTitle(QCoreApplication::translate("MeetingUI", "End Meeting"));
		box.setText(QCoreApplication::translate("MeetingUI", "You are the host. How would you like to leave?"));
		auto *leaveBtn = box.addButton(QCoreApplication::translate("MeetingUI", "Leave Only"), QMessageBox::ActionRole);
		auto *endBtn = box.addButton(QCoreApplication::translate("MeetingUI", "End for Everyone"), QMessageBox::DestructiveRole);
		auto *cancelBtn = box.addButton(QCoreApplication::translate("MeetingUI", "Cancel"), QMessageBox::RejectRole);
		leaveBtn->setObjectName(QStringLiteral("meetingLeaveConfirm"));
		endBtn->setObjectName(QStringLiteral("meetingEndForEveryone"));
		cancelBtn->setObjectName(QStringLiteral("meetingLeaveCancel"));
		box.exec();
		if (box.clickedButton() == leaveBtn) {
			beginLeaveTimingObservation();
			QPointer<MeetingRoomWindow> guard(this);
			stopLiveKitSession(false);
			if (!guard) return;
			_coordinator->leaveMeetingAsync(false);
			if (guard) guard->close();
		} else if (box.clickedButton() == endBtn) {
			beginLeaveTimingObservation();
			auto coordinator = _coordinator;
			QPointer<MeetingRoomWindow> guard(this);
			// Hide before retiring presentation; neither HTTP nor native cleanup
			// should keep the host on an empty meeting window.
			hide();
			stopLiveKitSession(false);
			if (!guard) return;
			coordinator->leaveMeetingAsync(true,
				[owner = QPointer<OpenMeeting::MeetingCoordinator>(coordinator.get())](
					bool ok, bool, const OpenMeeting::HttpError &) {
				if (ok) return;
				// A host transfer can reject the end request before it starts.
				if (owner && (owner->state() == OpenMeeting::MeetingState::InMeeting ||
					owner->state() == OpenMeeting::MeetingState::Reconnecting))
					owner->leaveMeetingAsync(false);
				auto *notice = new QMessageBox(QMessageBox::Warning,
					QCoreApplication::translate("MeetingUI", "End Meeting"),
					QCoreApplication::translate("MeetingUI", "Could not end the meeting. Please try again."),
					QMessageBox::Ok, nullptr);
				notice->setObjectName(QStringLiteral("meetingEndError"));
				notice->setProperty("meetingStandaloneNotice", true);
				notice->setAttribute(Qt::WA_DeleteOnClose);
				notice->setAttribute(Qt::WA_QuitOnClose, false);
				notice->setWindowModality(Qt::NonModal);
				AppTheme::setTone(*notice, AppTheme::Tone::Light);
				notice->show();
				notice->raise();
				notice->activateWindow();
			});
			if (guard) guard->close();
		}
	} else {
		QMessageBox box(QMessageBox::Question,
			QCoreApplication::translate("MeetingUI", "Leave Meeting"),
			QCoreApplication::translate("MeetingUI", "Are you sure you want to leave this meeting?"),
			QMessageBox::Yes | QMessageBox::No, this);
		box.setObjectName(QStringLiteral("meetingLeaveConfirmation"));
		box.button(QMessageBox::Yes)->setObjectName(QStringLiteral("meetingLeaveConfirm"));
		box.button(QMessageBox::No)->setObjectName(QStringLiteral("meetingLeaveCancel"));
		if (box.exec() == QMessageBox::Yes) {
			beginLeaveTimingObservation();
			QPointer<MeetingRoomWindow> guard(this);
			stopLiveKitSession(false);
			if (!guard) return;
			if (_coordinator) {
				_coordinator->leaveMeetingAsync(false);
			}
			if (guard) guard->close();
		}
	}
}

void MeetingRoomWindow::setupInvitationBinding() {
	if (_bottomBar) {
		_bottomBar->inviteClicked() | rpl::on_next([this] {
			handleInviteClicked();
		}, lifetime());
	}
	if (_config.callInvite) {
		connect(_config.callInvite, &OpenMeeting::CallInviteController::silentMissedCallWhileInMeeting,
			this, [this](const OpenMeeting::CallSession &session) {
				showSilentMissedCallBanner(session);
			});
	}
}

void MeetingRoomWindow::showSilentMissedCallBanner(const OpenMeeting::CallSession &session) {
	if (!_stageContainer || _closeRequested) return;
	if (!_missedCallBanner) {
		_missedCallBanner = new QLabel(_stageContainer);
		_missedCallBanner->setObjectName(QStringLiteral("MeetingMissedCallBanner"));
		_missedCallBanner->setTextFormat(Qt::PlainText);
		_missedCallBanner->setWordWrap(true);
		_missedCallBanner->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
		_missedCallBanner->setStyleSheet(QStringLiteral(
			"background: rgba(24, 24, 28, 225); color: #ffffff; border-radius: 8px; padding: 8px 14px; font-size: 12px;"));
		_missedCallBanner->hide();
	}
	if (!_missedCallBannerTimer) {
		_missedCallBannerTimer = new QTimer(this);
		_missedCallBannerTimer->setSingleShot(true);
		connect(_missedCallBannerTimer, &QTimer::timeout, this, [this] {
			if (_missedCallBanner) _missedCallBanner->hide();
		});
	}
	const QString caller = !session.callerNickname.trimmed().isEmpty()
		? session.callerNickname.trimmed()
		: session.callerUserId;
	_missedCallBanner->setText(QCoreApplication::translate(
		"MeetingUI", "Missed call from %1 (auto-replied Busy while in meeting)").arg(caller));
	_missedCallBanner->adjustSize();
	const int stageW = _stageContainer->width();
	const int bannerW = std::min(std::max(240, _missedCallBanner->sizeHint().width() + 8), std::max(240, stageW - 32));
	const int bannerH = std::max(34, _missedCallBanner->heightForWidth(bannerW));
	_missedCallBanner->setGeometry(std::max(12, stageW - bannerW - 16), 16, bannerW, bannerH);
	_missedCallBanner->show();
	_missedCallBanner->raise();
	_missedCallBannerTimer->start(4000);
}

void MeetingRoomWindow::handleInviteClicked() {
	if (_config.invitationMode != InvitationMode::BusinessMeetingId) {
		copyMeetingIdInvitation();
		return;
	}
	const auto ready = _coordinator
		&& _coordinator->state() == OpenMeeting::MeetingState::InMeeting
		&& !_coordinator->currentMeetingId().trimmed().isEmpty();
	if (ready && _config.contacts && _config.callInvite) {
		auto *menu = new QMenu(this);
		menu->setAttribute(Qt::WA_DeleteOnClose);
		AppTheme::styleMenu(
			*menu,
			AppTheme::effectiveTheme() == AppTheme::Theme::Dark
				? AppTheme::Tone::Dark
				: AppTheme::Tone::Light);
		auto *inviteContactsAct = menu->addAction(
			AppTheme::icon(AppTheme::Icon::Plus),
			QCoreApplication::translate("MeetingUI", "Invite Contacts..."));
		auto *copyIdAct = menu->addAction(
			AppTheme::icon(AppTheme::Icon::Copy),
			QCoreApplication::translate("MeetingUI", "Copy Meeting ID Invitation"));
		connect(inviteContactsAct, &QAction::triggered, this, [this] {
			openInMeetingContactInviteDialog();
		}, Qt::QueuedConnection);
		connect(copyIdAct, &QAction::triggered, this, [this] {
			copyMeetingIdInvitation();
		}, Qt::QueuedConnection);
		menu->popup(QCursor::pos());
		return;
	}
	copyMeetingIdInvitation();
}

void MeetingRoomWindow::openInMeetingContactInviteDialog() {
	if (!_config.contacts || !_config.callInvite || !_coordinator) return;
	QSet<QString> excluded;
	const auto selfId = OpenMeeting::SessionManager::instance().userId();
	if (!selfId.isEmpty()) excluded.insert(selfId);
	for (const auto &p : _coordinator->participants()) {
		if (!p.identity.isEmpty()) excluded.insert(p.identity);
	}

	ContactPickerDialog picker(
		*_config.contacts,
		_config.presence,
		QCoreApplication::translate("MeetingUI", "Invite Contacts to Meeting"),
		QCoreApplication::translate("MeetingUI", "Send Invite"),
		{},
		excluded,
		this);
	if (picker.exec() != QDialog::Accepted) return;

	const auto selected = picker.selectedUserIds();
	if (selected.isEmpty()) return;

	int sentCount = 0;
	for (const auto &uid : selected) {
		if (_config.callInvite->inviteToActiveMeeting(uid)) {
			++sentCount;
		}
	}
	if (sentCount > 0) {
		LogToConsole(LogCategory::General, "INVITE",
			QStringLiteral("in_meeting_contact_invites_dispatched=%1").arg(sentCount));
		showInvitationNotice(
			true,
			QCoreApplication::translate("MeetingUI", "Invitations Sent"),
			QCoreApplication::translate("MeetingUI", "Sent real-time meeting invitations to %1 contact(s).").arg(sentCount));
	}
}

void MeetingRoomWindow::copyMeetingIdInvitation() {
	if (_config.invitationMode != InvitationMode::BusinessMeetingId) {
		QPointer<MeetingRoomWindow> guard(this);
		LogToConsole(LogCategory::General, "INVITE", QCoreApplication::translate("MeetingUI", "This connection does not support meeting ID invitations"));
		if (guard) {
			guard->showInvitationNotice(false, QCoreApplication::translate("MeetingUI", "Unable to Copy Invitation"),
				QCoreApplication::translate("MeetingUI", "This connection does not support meeting ID invitations. Ask the organizer to provide another way for participants to join."));
		}
		return;
	}

	const auto ready = _coordinator
		&& _coordinator->state() == OpenMeeting::MeetingState::InMeeting;
	const auto meetingId = ready ? _coordinator->currentMeetingId() : QString();
	bool validMeetingId = !meetingId.isEmpty();
	for (const QChar character : meetingId) {
		if (character.isSpace() || !character.isPrint()) {
			validMeetingId = false;
			break;
		}
	}
	if (!validMeetingId) {
		QPointer<MeetingRoomWindow> guard(this);
		LogToConsole(LogCategory::General, "INVITE", QCoreApplication::translate("MeetingUI", "The meeting is not ready. No invitation was copied."));
		if (guard) {
			guard->showInvitationNotice(false, QCoreApplication::translate("MeetingUI", "Invitation Unavailable"),
				QCoreApplication::translate("MeetingUI", "The meeting is not ready. The invitation cannot be copied yet."));
		}
		return;
	}

	const auto inviteText = QCoreApplication::translate("MeetingUI", "[Cohavora Meeting Invitation]\nMeeting ID: %1\nSign in with your own account in a client configured for the same meeting service, then join using this meeting ID.")
		.arg(meetingId);
	QPointer<MeetingRoomWindow> guard(this);
	QApplication::clipboard()->setText(inviteText);
	if (!guard) return;
	LogToConsole(LogCategory::General, "INVITE", QCoreApplication::translate("MeetingUI", "Meeting invitation copied to the clipboard"));
	if (guard) {
		guard->showInvitationNotice(true, QCoreApplication::translate("MeetingUI", "Invitation Copied"),
			QCoreApplication::translate("MeetingUI", "The meeting invitation has been copied to the clipboard. You can send it to other participants."));
	}
}

void MeetingRoomWindow::showInvitationNotice(
		bool success,
		const QString &title,
		const QString &message) {
	if (_invitationNoticeEffect) {
		auto effect = _invitationNoticeEffect;
		effect(success, title, message);
		return;
	}
	if (success) {
		QMessageBox::information(this, title, message);
	} else {
		QMessageBox::warning(this, title, message);
	}
}

void MeetingRoomWindow::bindLocalMediaSources() {
	if (!_captureUiCallbacks) {
		_captureUiCallbacks = OpenMeeting::QtCallbackGate<MeetingRoomWindow>::Create(this);
	}
	if (_coordinator) {
		_localAudioSource = _coordinator->localAudioSource();
		_localVideoSource = _coordinator->localVideoSource();
	}
	if (!_localAudioSource) {
		_localAudioSource = std::make_shared<livekit::AudioSource>(48000, 2);
	}
	if (_meetingTranslator) {
		_meetingTranslator->setLocalAudioSource(_localAudioSource);
	}
	if (!_localVideoSource) {
		_localVideoSource = std::make_shared<livekit::VideoSource>(1280, 720);
	}
	if (_remoteRenderSession && _config.videoEnabled) {
		_remoteRenderSession->AttachLocalSource(_localVideoSource);
	}
}

void MeetingRoomWindow::attachCoordinatorSession() {
	if (!_coordinator) return;
    if (!_encryptionPanel) {
        _encryptionPanel = new MeetingEncryptionPanel(this);
        _encryptionPanel->installEventFilter(this);
        _encryptionPanel->setRecoveryHandler([weak = std::weak_ptr<OpenMeeting::MeetingCoordinator>(_coordinator)](
            std::shared_ptr<livekit::MeetingSecretHandle> secret) {
            if (const auto owner = weak.lock()) owner->recoverEncryptionKey(std::move(secret));
            else if (secret) secret->Revoke();
        });
        const auto refresh = [this] {
            _encryptionPanel->setSession(_coordinator->requiresEncryption(),
                _coordinator->state() == OpenMeeting::MeetingState::InMeeting,
                _coordinator->canRecoverEncryptionKey());
        };
        connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::stateChanged, this,
            [refresh](OpenMeeting::MeetingState, const QString&) { refresh(); });
        // Native generation may arrive after InMeeting; it is published before
        // participantsUpdated, so refresh recovery availability at that boundary.
        connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::participantsUpdated, this,
            [refresh](const auto&) { refresh(); });
        connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::encryptionKeyRecoveryFinished, this,
            [this](bool installed) {
                _encryptionPanel->setInstallResult(installed, _coordinator->canRecoverEncryptionKey());
            });
        connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::encryptionMediaStatusChanged, this,
            [this](const livekit::MediaEncryptionStatus& status) { _encryptionPanel->setMediaStatus(status); });
        // Bounded display sampling, never a recovery retry or formal acceptance.
        auto* encryptionStatusTimer = new QTimer(_encryptionPanel);
        encryptionStatusTimer->setInterval(1000);
        connect(encryptionStatusTimer, &QTimer::timeout, this,
            [this] { _coordinator->refreshEncryptionMediaStatus(); });
        encryptionStatusTimer->start();
        refresh();
        _encryptionPanel->show();
        QResizeEvent layoutEvent(size(), size());
        resizeEvent(&layoutEvent);
    }


	// The entry owner starts the coordinator after constructing this window.
	// Connecting here as well would restart that session, replace its sources,
	// and leave the capture devices feeding only this window's old sources.
	// Arm cleanup before asynchronous preparation or admission begins.
	_sessionRunning = true;
	_room = _coordinator->room();
	if (_coordinator->state() == OpenMeeting::MeetingState::InMeeting) {
		restoreParticipantPresentations();
	}
}

void MeetingRoomWindow::stopLiveKitSession(bool requestLeave) {
	Q_ASSERT(thread() == QThread::currentThread());
    _detachedShareIntent.reset();
    _displayedSharedSurfaceId = 0;
    _sharedReturnPending = false;
    if (_sharedWindow) {
        disconnect(_sharedWindow.get(), nullptr, this, nullptr);
        _sharedWindow.reset();
    }
    _floatingStripRenderKeys.clear();
    _floatingSurfaceId = 0;
    if (_floatingListWindow) {
        disconnect(_floatingListWindow.get(), nullptr, this, nullptr);
        _floatingListWindow.reset();
    }
	// The Coordinator's cleanup owner retains native resources through drain.
	// A hidden/departure-notice window must not retain the retired Room and keys.
	_room.reset();
	CloseLiveTelemetryDialog(_telemetryDialog);
	const bool wasRunning = _sessionRunning.exchange(false);
	if (_whiteboardPanel) _whiteboardPanel->canvas()->setEditingEnabled(false);
	cancelMediaPreparation();
	if (_captureUiCallbacks) _captureUiCallbacks->Revoke();
	invalidateCameraCompletion();
	cancelDeviceSwitchTelemetry();
	closeAnnotationOverlay();
	_annotationBinding.reset();
    if (_localScreenPreview) {
        _localScreenPreview->DiscardLatest("screen", _localScreenPreview->generation());
    }
	_localScreenPreview.reset();
	if (_remoteRenderSession) _remoteRenderSession->Deactivate();
	if (_meetingTimer) _meetingTimer->stop();
	if (_remoteRenderTimer) _remoteRenderTimer->stop();
	if (_localGenTimer) _localGenTimer->stop();
	if (_recoveryBannerFadeTimer) _recoveryBannerFadeTimer->stop();
	if (_roomAgentPublisher) _roomAgentPublisher->disableAgentRole();
	if (_meetingTranslator) _meetingTranslator->stop();
	_hasRemoteRoomAgent = false;
	_remoteRoomAgentName.clear();
	_remoteRoomAgentState.clear();
	_remoteRoomAgentCount = 0;
	_autoTranslationEvaluated = false;
	_e2eeAgentBroadcastAuthorized = false;
	refreshTopBarAgentStatus();
	clearLiveCaption();
	_usingGpuBackend.store(false, std::memory_order_release);
	if (_mediaPreparationErrorTimer) _mediaPreparationErrorTimer->stop();
	if (_videoCanvas) {
		_videoCanvas->shutdownRenderer();
		_videoCanvas->hide();
	}
	_remoteTilePool.clear();

	if (_wasapiCap) {
#ifdef Q_OS_WIN
		_wasapiCap->SetMute(true);
#endif
		_wasapiCap->SetCaptureStateCallback({});
		_wasapiCap->SetDeviceFrameCallback({});
		auto &manager = livekit::WebRTCManager::Instance();
		if (manager.apm_processor() == _wasapiCap->apm_processor()) {
			manager.SetApmProcessor(nullptr);
		}
	}
	retireLocalCapture();

	if (wasRunning && requestLeave && _coordinator &&
		_coordinator->state() != OpenMeeting::MeetingState::Failed) {
		_coordinator->leaveMeetingAsync(false);
	}
	if (wasRunning) {
		LogToConsole(LogCategory::Connection, "DISCONNECT", QCoreApplication::translate("MeetingUI", "Stopping"));
	}
}

#if defined(Q_OS_WIN)
int MeetingRoomWindow::nativeResizeHitTest(LPARAM position) const {
	if (!_handle || isMaximized() || isFullScreen()) return HTCLIENT;
	POINT point{ GET_X_LPARAM(position), GET_Y_LPARAM(position) };
	RECT client{};
	if (!ScreenToClient(_handle, &point) || !GetClientRect(_handle, &client) ||
		!PtInRect(&client, point)) return HTCLIENT;
	const int sideBorder = std::max(1, qRound(8 * devicePixelRatioF()));
	// The native frame already provides an outer resize edge. Only a narrow
	// inner vertical band is needed beside the top/bottom controls.
	const int verticalBorder = std::max(1,
		qRound(4 * devicePixelRatioF()) - kNativeVerticalFramePixels);
	const bool left = point.x < sideBorder;
	const bool right = point.x >= client.right - sideBorder;
	const bool top = point.y < verticalBorder;
	const bool bottom = point.y >= client.bottom - verticalBorder;
	if (top && left) return HTTOPLEFT;
	if (top && right) return HTTOPRIGHT;
	if (bottom && left) return HTBOTTOMLEFT;
	if (bottom && right) return HTBOTTOMRIGHT;
	if (left) return HTLEFT;
	if (right) return HTRIGHT;
	if (top) return HTTOP;
	if (bottom) return HTBOTTOM;
	return HTCLIENT;
}
#endif

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
bool MeetingRoomWindow::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result)
#else
bool MeetingRoomWindow::nativeEventFilter(const QByteArray &eventType, void *message, long *result)
#endif
{
	Q_UNUSED(eventType);
#if defined(Q_OS_WIN)
	const auto msg = static_cast<MSG*>(message);
	if (msg && msg->message == WM_NCHITTEST && _handle && msg->hwnd != _handle &&
		IsChild(_handle, msg->hwnd) && nativeResizeHitTest(msg->lParam) != HTCLIENT) {
		// The GPU canvas and its paging overlay have native child HWNDs.
		// Let Windows continue hit-testing to the meeting's top-level HWND at
		// resize edges. Returning HTLEFT/etc. here would resize the child itself.
		*result = HTTRANSPARENT;
		return true;
	}
#endif
	return false;
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
bool MeetingRoomWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
#else
bool MeetingRoomWindow::nativeEvent(const QByteArray &eventType, void *message, long *result)
#endif
{
#if defined(Q_OS_WIN)
	auto msg = reinterpret_cast<MSG*>(message);
	if (!msg) {
		return Ui::RpWidget::nativeEvent(eventType, message, result);
	}
	// Once the DX11 Canvas creates a native child HWND, Qt may forward a native
	// message whose hwnd is that child. Hit-testing and resizing are defined in
	// MeetingRoomWindow coordinates, so never use the child as the conversion
	// origin here.
	HWND handle = _handle ? _handle : msg->hwnd;

	switch (msg->message) {
	case WM_ERASEBKGND: {
		if (_handle && msg->hwnd == _handle) {
			// Qt's backing store paints the opaque client area in one pass.
			*result = 1;
			return true;
		}
	} break;

	case WM_NCACTIVATE: {
		if (_handle && msg->hwnd == _handle) {
			// Preserve activation state without drawing a native caption over Qt.
			*result = DefWindowProcW(_handle, msg->message, msg->wParam, -1);
			return true;
		}
	} break;

	case WM_NCCALCSIZE: {
		if (msg->hwnd == handle && msg->wParam == TRUE) {
			auto *parameters = reinterpret_cast<NCCALCSIZE_PARAMS *>(msg->lParam);
			const RECT proposed = parameters->rgrc[0];
			DefWindowProcW(handle, msg->message, msg->wParam, msg->lParam);
			// Keep a native frame on all four sides and replace the caption with
			// our Qt top bar. A thin top/bottom edge avoids a visible system band
			// without taking over the entire non-client area beside GPU HWNDs.
			const int resizeBorder = proposed.bottom - parameters->rgrc[0].bottom;
			const auto style = GetWindowLongPtrW(handle, GWL_STYLE);
			if ((style & WS_THICKFRAME) && !(style & WS_MAXIMIZE) &&
				!isFullScreen() && resizeBorder > 0) {
				// NCCALCSIZE can precede Qt's screen/DPI notification. A single
				// physical pixel keeps this frame stable across that transition.
				const int thinBorder = std::min(resizeBorder, kNativeVerticalFramePixels);
				parameters->rgrc[0].top = proposed.top + thinBorder;
				parameters->rgrc[0].bottom = proposed.bottom - thinBorder;
			} else {
				// Maximized windows still need the system's off-work-area inset.
				parameters->rgrc[0].top = proposed.top + resizeBorder;
			}
			*result = 0;
			return true;
		}
	} break;

	case WM_NCHITTEST: {
		if (!handle) break;
		POINT nativePoint{ GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam) };
		RECT client{};
		if (ScreenToClient(handle, &nativePoint) && GetClientRect(handle, &client) &&
			!PtInRect(&client, nativePoint)) {
			*result = DefWindowProcW(handle, msg->message, msg->wParam, msg->lParam);
			return true;
		}
		const int resizeHit = nativeResizeHitTest(msg->lParam);
		if (resizeHit != HTCLIENT) {
			*result = resizeHit;
			return true;
		}

		POINT p{ GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam) };
		ScreenToClient(handle, &p);

		const qreal ratio = devicePixelRatioF();
		const int x = static_cast<int>(p.x / ratio);
		const int y = static_cast<int>(p.y / ratio);

		// Alien title-bar widgets share this HWND. Only its actual empty area
		// may act as a caption; its controls must receive client mouse events.
		if (_topBar && _topBar->isWindowDragArea(
			_topBar->mapFrom(this, QPoint(x, y)))) {
			*result = HTCAPTION;
			return true;
		}

		*result = HTCLIENT;
		return true;
	} break;
	}
#endif
	return Ui::RpWidget::nativeEvent(eventType, message, result);
}

void MeetingRoomWindow::setupMeetingTranslationServices() {
	if (!_meetingTranslator) {
		_meetingTranslator = new OpenMeeting::MeetingTranslatorService(
			_agentCoordinator.get(), _coordinator.get(), nullptr, nullptr, this);
	}
	if (!_roomAgentPublisher) {
		_roomAgentPublisher = new OpenMeeting::RoomAgentPublisher(
			_coordinator.get(), &OpenMeeting::OpenMeetingHttpClient::instance(), {}, this);
	}
	connect(_meetingTranslator, &OpenMeeting::MeetingTranslatorService::stateChanged,
		this, [this](OpenMeeting::MeetingTranslatorState) {
			refreshTopBarAgentStatus();
		});
	connect(_meetingTranslator, &OpenMeeting::MeetingTranslatorService::activityChanged,
		this, [this](bool busy) {
			if (_roomAgentPublisher && _roomAgentPublisher->isActive()) {
				_roomAgentPublisher->setAgentActivityState(
					busy ? QStringLiteral("thinking") : QStringLiteral("listening"));
			}
			refreshTopBarAgentStatus();
		});
	connect(_meetingTranslator, &OpenMeeting::MeetingTranslatorService::transcriptProduced,
		this, [this](const OpenMeeting::MeetingTranscriptItem &item) {
			if (_roomAgentPublisher && _roomAgentPublisher->isActive()) {
				_roomAgentPublisher->publishTranscript(item);
			}
		});
	connect(_meetingTranslator, &OpenMeeting::MeetingTranslatorService::errorOccurred,
		this, [this](const QString &safeError) {
			LogToConsole(LogCategory::Error, "TRANSLATOR", safeError);
			if (_meetingTranslator && _meetingTranslator->state() == OpenMeeting::MeetingTranslatorState::Error) {
				if (_roomAgentPublisher) {
					_roomAgentPublisher->disableAgentRole();
				}
				ShowMeetingWarning(this, "meetingTranslatorError", "meetingTranslatorErrorDismiss",
					QCoreApplication::translate("MeetingUI", "Live Translation"), safeError);
			}
		});

	connect(_roomAgentPublisher, &OpenMeeting::RoomAgentPublisher::stateChanged,
		this, [this](OpenMeeting::RoomAgentRoleState) {
			refreshTopBarAgentStatus();
		});
	connect(_roomAgentPublisher, &OpenMeeting::RoomAgentPublisher::conflictDetected,
		this, [this](const OpenMeeting::ExistingRoomAgentInfo &existing) {
			const QString holder = !existing.name.trimmed().isEmpty()
				? existing.name.trimmed()
				: existing.identity;
			if (isCurrentUserMeetingHost()) {
				auto *box = new QMessageBox(
					QMessageBox::Question,
					QCoreApplication::translate("MeetingUI", "Room Agent Already Online"),
					QCoreApplication::translate(
						"MeetingUI",
						"This meeting room already has an active Agent (%1).\n\n"
						"As the host, would you like to take over and replace it with your client?")
						.arg(holder.isEmpty() ? QCoreApplication::translate("MeetingUI", "Unknown") : holder),
					QMessageBox::Yes | QMessageBox::No,
					this);
				box->setAttribute(Qt::WA_DeleteOnClose);
				AppTheme::setTone(*box, AppTheme::Tone::Dark);
				connect(box, &QMessageBox::finished, this, [this](int result) {
					if (result == QMessageBox::Yes) {
						enableRoomAgentRoleInteractive(true);
					} else if (_roomAgentPublisher) {
						_roomAgentPublisher->disableAgentRole();
					}
				});
				box->open();
			} else {
				ShowMeetingWarning(this, "meetingAgentConflict", "meetingAgentConflictDismiss",
					QCoreApplication::translate("MeetingUI", "Room Agent Already Online"),
					QCoreApplication::translate(
						"MeetingUI",
						"This meeting room already has an active Agent (%1). "
						"Your client will stay in local-only translation mode.")
						.arg(holder.isEmpty() ? QCoreApplication::translate("MeetingUI", "Unknown") : holder));
			}
		});
	connect(_roomAgentPublisher, &OpenMeeting::RoomAgentPublisher::takeoverRemoved,
		this, [this](const QString &reason) {
			LogToConsole(LogCategory::Connection, "AGENT_ROLE", reason);
			ShowMeetingWarning(this, "meetingAgentTakeover", "meetingAgentTakeoverDismiss",
				QCoreApplication::translate("MeetingUI", "Room Agent Role Released"), reason);
		});
	connect(_roomAgentPublisher, &OpenMeeting::RoomAgentPublisher::errorOccurred,
		this, [this](const QString &safeError) {
			LogToConsole(LogCategory::Error, "AGENT_ROLE", safeError);
			ShowMeetingWarning(this, "meetingAgentError", "meetingAgentErrorDismiss",
				QCoreApplication::translate("MeetingUI", "Room Agent"), safeError);
		});
}

bool MeetingRoomWindow::isCurrentUserMeetingHost() const {
	if (!_coordinator) return false;
	const QString myId = OpenMeeting::SessionManager::instance().userId();
	if (myId.isEmpty()) return false;
	const auto &detail = _coordinator->meetingDetail();
	return detail.effectiveHostUserId() == myId || detail.creatorUserId == myId;
}

void MeetingRoomWindow::refreshTopBarAgentStatus() {
	if (!_topBar) return;
	const bool localTranslatorActive = _meetingTranslator && _meetingTranslator->isRunning();
	const bool localAgentActive = _roomAgentPublisher && _roomAgentPublisher->isActive();
	const bool localAgentStarting = _roomAgentPublisher &&
		(_roomAgentPublisher->state() == OpenMeeting::RoomAgentRoleState::RequestingToken ||
		 _roomAgentPublisher->state() == OpenMeeting::RoomAgentRoleState::Connecting ||
		 _roomAgentPublisher->state() == OpenMeeting::RoomAgentRoleState::Reconnecting);

	if (localAgentActive || localAgentStarting) {
		const QString stateStr = localAgentStarting
			? QStringLiteral("initializing")
			: ((_meetingTranslator && _meetingTranslator->isBusy())
				? QStringLiteral("thinking")
				: QStringLiteral("listening"));
		_topBar->setAgentStatus(
			true,
			stateStr,
			_config.displayName,
			1 + _remoteRoomAgentCount,
			localTranslatorActive,
			true);
		return;
	}
	_topBar->setAgentStatus(
		_hasRemoteRoomAgent,
		_remoteRoomAgentState,
		_remoteRoomAgentName,
		_remoteRoomAgentCount,
		localTranslatorActive,
		false);
}

void MeetingRoomWindow::maybeAutoStartMeetingTranslation() {
	if (_autoTranslationEvaluated || !_coordinator ||
		_coordinator->state() != OpenMeeting::MeetingState::InMeeting) {
		return;
	}
	_autoTranslationEvaluated = true;
	const auto &prefs = _cameraSessionManager
		? _cameraSessionManager->mediaPreferences()
		: OpenMeeting::SessionManager::instance().mediaPreferences();
	if (!prefs.enableLocalMeetingTranslation) {
		return;
	}
	if (prefs.preferRoomAgentTranscript && _hasRemoteRoomAgent) {
		return;
	}
	// Never silently exfiltrate decrypted audio in an E2EE meeting without explicit user action.
	if (_coordinator->requiresEncryption() &&
		(!_agentCoordinator || !_agentCoordinator->isE2eeContextAuthorized())) {
		return;
	}
	if (!startLocalMeetingTranslation(false)) {
		return;
	}
	// E2EE rooms always require explicit interactive confirmation before broadcasting plaintext captions.
	if (prefs.meetingTranslationRoleMode == OpenMeeting::MeetingTranslationRoleMode::RoomAgentIfAvailable &&
		!_hasRemoteRoomAgent &&
		!_coordinator->requiresEncryption()) {
		enableRoomAgentRoleInteractive(false);
	}
}

bool MeetingRoomWindow::startLocalMeetingTranslation(bool interactive) {
	if (!_meetingTranslator || !_coordinator ||
		_coordinator->state() != OpenMeeting::MeetingState::InMeeting) {
		return false;
	}
	if (_coordinator->requiresEncryption() && _agentCoordinator &&
		!_agentCoordinator->isE2eeContextAuthorized()) {
		if (!interactive) {
			return false;
		}
		auto *box = new QMessageBox(
			QMessageBox::Warning,
			QCoreApplication::translate("MeetingUI", "Encrypted Meeting Audio Notice"),
			QCoreApplication::translate(
				"MeetingUI",
				"This meeting uses end-to-end encryption (E2EE). Starting local translation will send "
				"decrypted speech segments to your configured Speech-to-Text / Copilot API endpoint.\n\n"
				"Continue enabling local translation for this meeting?"),
			QMessageBox::Yes | QMessageBox::No,
			this);
		box->setAttribute(Qt::WA_DeleteOnClose);
		AppTheme::setTone(*box, AppTheme::Tone::Dark);
		if (box->exec() != QMessageBox::Yes) {
			return false;
		}
		_agentCoordinator->authorizeE2eeContextForCurrentMeeting(true);
	}

	if (_localAudioSource) {
		_meetingTranslator->setLocalAudioSource(_localAudioSource);
	}
	const auto &prefs = _cameraSessionManager
		? _cameraSessionManager->mediaPreferences()
		: OpenMeeting::SessionManager::instance().mediaPreferences();
	OpenMeeting::MeetingTranslatorConfig cfg;
	cfg.translateEnabled = prefs.enableTranslationOutput;
	cfg.targetLanguage = prefs.translationTargetLanguage;
	cfg.translationProfileId = prefs.translationProfileId;
	cfg.translationModelOverride = prefs.translationModelOverride;
	cfg.translationSystemPrompt = prefs.translationSystemPrompt;
	cfg.includeLocalMic = prefs.translateLocalMicrophone;
	cfg.maxConcurrentStreamingAsr = prefs.maxConcurrentStreamingAsr;
	cfg.streamingIdleReleaseMs = prefs.streamingIdleReleaseMs;
	const bool ok = _meetingTranslator->start(cfg);
	if (!ok && interactive) {
		ShowMeetingWarning(this, "meetingTranslatorStartFail", "meetingTranslatorStartFailDismiss",
			QCoreApplication::translate("MeetingUI", "Live Translation"),
			_meetingTranslator->lastError());
	}
	refreshTopBarAgentStatus();
	return ok;
}

void MeetingRoomWindow::stopLocalMeetingTranslation() {
	if (_roomAgentPublisher && _roomAgentPublisher->isBusyOrActive()) {
		_roomAgentPublisher->disableAgentRole();
	}
	if (_meetingTranslator) {
		_meetingTranslator->stop();
	}
	refreshTopBarAgentStatus();
}

void MeetingRoomWindow::enableRoomAgentRoleInteractive(bool forceTakeover) {
	if (!_roomAgentPublisher || !_coordinator ||
		_coordinator->state() != OpenMeeting::MeetingState::InMeeting) {
		return;
	}
	if (_coordinator->requiresEncryption() && !_e2eeAgentBroadcastAuthorized) {
		QMessageBox box(
			QMessageBox::Warning,
			QCoreApplication::translate("MeetingUI", "Confirm Plaintext Caption Broadcast (E2EE)"),
			QCoreApplication::translate(
				"MeetingUI",
				"This meeting is end-to-end encrypted (E2EE). Acting as the room Agent will broadcast "
				"recognized and translated captions in plaintext via LiveKit lk.transcription to all "
				"participants in this room.\n\n"
				"Do you confirm broadcasting captions to the room?"),
			QMessageBox::Yes | QMessageBox::No,
			this);
		AppTheme::setTone(box, AppTheme::Tone::Dark);
		if (box.exec() != QMessageBox::Yes) {
			return;
		}
		_e2eeAgentBroadcastAuthorized = true;
	}

	if (!_meetingTranslator || !_meetingTranslator->isRunning()) {
		if (!startLocalMeetingTranslation(true)) {
			return;
		}
	}

	const auto &prefs = _cameraSessionManager
		? _cameraSessionManager->mediaPreferences()
		: OpenMeeting::SessionManager::instance().mediaPreferences();
	OpenMeeting::RoomAgentRoleConfig roleCfg;
	roleCfg.meetingId = _coordinator->currentMeetingId();
	roleCfg.role = prefs.enableTranslationOutput
		? QStringLiteral("translator")
		: QStringLiteral("transcriber");
	roleCfg.displayName = QCoreApplication::translate("MeetingUI", "AI Agent (%1)")
		.arg(_config.displayName.trimmed().isEmpty()
			? OpenMeeting::SessionManager::instance().userId()
			: _config.displayName.trimmed());
	roleCfg.sourceLanguage = (_agentCoordinator && !_agentCoordinator->dictationLanguage().trimmed().isEmpty())
		? _agentCoordinator->dictationLanguage().trimmed()
		: QStringLiteral("auto");
	roleCfg.targetLanguage = prefs.enableTranslationOutput
		? prefs.translationTargetLanguage
		: QString();
	roleCfg.allowInsecureTransport = OpenMeeting::isDebugHttpTransportEnabled();

	_roomAgentPublisher->enableAgentRole(roleCfg, forceTakeover);
	refreshTopBarAgentStatus();
}

void MeetingRoomWindow::disableRoomAgentRole() {
	if (_roomAgentPublisher) {
		_roomAgentPublisher->disableAgentRole();
	}
	refreshTopBarAgentStatus();
}

void MeetingRoomWindow::showAgentControlMenu(const QPoint &globalPos) {
	QMenu menu(this);
	AppTheme::styleMenu(
		menu,
		AppTheme::effectiveTheme() == AppTheme::Theme::Dark
			? AppTheme::Tone::Dark
			: AppTheme::Tone::Light);

	const bool inMeeting = _coordinator &&
		_coordinator->state() == OpenMeeting::MeetingState::InMeeting;
	const bool localRunning = _meetingTranslator && _meetingTranslator->isRunning();
	const bool localAgentActive = _roomAgentPublisher && _roomAgentPublisher->isBusyOrActive();

	QString statusLine;
	if (localAgentActive) {
		statusLine = QCoreApplication::translate("MeetingUI", "Room Agent: Active (This Device)");
	} else if (_hasRemoteRoomAgent) {
		statusLine = QCoreApplication::translate("MeetingUI", "Room Agent: Online (%1)")
			.arg(_remoteRoomAgentName.isEmpty()
				? QCoreApplication::translate("MeetingUI", "Remote")
				: _remoteRoomAgentName);
	} else {
		statusLine = QCoreApplication::translate("MeetingUI", "Room Agent: Offline");
	}
	QAction *header = menu.addAction(statusLine);
	header->setEnabled(false);
	menu.addSeparator();

	// 1. Toggle local meeting translation on/off anytime
	QAction *toggleTranslatorAct = menu.addAction(
		localRunning
			? QCoreApplication::translate("MeetingUI", "Stop Live Translation")
			: QCoreApplication::translate("MeetingUI", "Start Live Translation (Local)"));
	toggleTranslatorAct->setEnabled(inMeeting);
	connect(toggleTranslatorAct, &QAction::triggered, this, [this, localRunning] {
		if (localRunning) {
			stopLocalMeetingTranslation();
		} else {
			startLocalMeetingTranslation(true);
		}
	});

	// 2. Claim / release room Agent role anytime
	if (localAgentActive) {
		QAction *releaseAgentAct = menu.addAction(
			QCoreApplication::translate("MeetingUI", "Release Room Agent Role (Keep Local Only)"));
		connect(releaseAgentAct, &QAction::triggered, this, [this] {
			disableRoomAgentRole();
		});
	} else if (_hasRemoteRoomAgent) {
		if (isCurrentUserMeetingHost()) {
			QAction *takeoverAct = menu.addAction(
				QCoreApplication::translate("MeetingUI", "Take Over as Room Agent (Host)"));
			takeoverAct->setEnabled(inMeeting);
			connect(takeoverAct, &QAction::triggered, this, [this] {
				QMessageBox box(
					QMessageBox::Question,
					QCoreApplication::translate("MeetingUI", "Take Over Room Agent"),
					QCoreApplication::translate(
						"MeetingUI",
						"Disconnect the current room Agent (%1) and broadcast captions from this device instead?")
						.arg(_remoteRoomAgentName.isEmpty()
							? QCoreApplication::translate("MeetingUI", "Remote")
							: _remoteRoomAgentName),
					QMessageBox::Yes | QMessageBox::No,
					this);
				AppTheme::setTone(box, AppTheme::Tone::Dark);
				if (box.exec() == QMessageBox::Yes) {
					enableRoomAgentRoleInteractive(true);
				}
			});
		} else {
			QAction *occupiedAct = menu.addAction(
				QCoreApplication::translate("MeetingUI", "Room Agent Occupied (Host Can Take Over)"));
			occupiedAct->setEnabled(false);
		}
	} else {
		QAction *claimAgentAct = menu.addAction(
			QCoreApplication::translate("MeetingUI", "Act as Room Agent (Broadcast Captions)"));
		claimAgentAct->setEnabled(inMeeting);
		connect(claimAgentAct, &QAction::triggered, this, [this] {
			enableRoomAgentRoleInteractive(false);
		});
	}

	menu.addSeparator();

	// 3. Target language quick submenu
	auto &session = _cameraSessionManager ? *_cameraSessionManager : OpenMeeting::SessionManager::instance();
	const auto prefs = session.mediaPreferences();

	QMenu *langMenu = menu.addMenu(QCoreApplication::translate("MeetingUI", "Translation Target"));
	AppTheme::styleMenu(
		*langMenu,
		AppTheme::effectiveTheme() == AppTheme::Theme::Dark
			? AppTheme::Tone::Dark
			: AppTheme::Tone::Light);
	struct LangChoice {
		QString label;
		QString code;
		bool enableTranslate;
	};
	const std::vector<LangChoice> choices = {
		{ QCoreApplication::translate("MeetingUI", "Original Only (No Translation)"), QString(), false },
		{ QCoreApplication::translate("MeetingUI", "Chinese (zh)"), QStringLiteral("zh"), true },
		{ QCoreApplication::translate("MeetingUI", "English (en)"), QStringLiteral("en"), true },
		{ QCoreApplication::translate("MeetingUI", "Japanese (ja)"), QStringLiteral("ja"), true },
		{ QCoreApplication::translate("MeetingUI", "Korean (ko)"), QStringLiteral("ko"), true },
		{ QCoreApplication::translate("MeetingUI", "French (fr)"), QStringLiteral("fr"), true },
		{ QCoreApplication::translate("MeetingUI", "German (de)"), QStringLiteral("de"), true },
		{ QCoreApplication::translate("MeetingUI", "Spanish (es)"), QStringLiteral("es"), true },
	};
	bool matchedPreset = false;
	for (const auto &choice : choices) {
		QAction *a = langMenu->addAction(choice.label);
		a->setCheckable(true);
		const bool checked = (!choice.enableTranslate && !prefs.enableTranslationOutput) ||
			(choice.enableTranslate && prefs.enableTranslationOutput &&
			 prefs.translationTargetLanguage.compare(choice.code, Qt::CaseInsensitive) == 0);
		if (choice.enableTranslate && checked) {
			matchedPreset = true;
		}
		a->setChecked(checked);
		connect(a, &QAction::triggered, this, [this, &session, choice] {
			auto updated = session.mediaPreferences();
			updated.enableTranslationOutput = choice.enableTranslate;
			if (choice.enableTranslate && !choice.code.isEmpty()) {
				updated.translationTargetLanguage = choice.code;
			}
			session.setMediaPreferences(updated);
		});
	}
	if (prefs.enableTranslationOutput && !prefs.translationTargetLanguage.trimmed().isEmpty() && !matchedPreset) {
		const QString customLang = prefs.translationTargetLanguage.trimmed();
		QAction *customAct = langMenu->addAction(
			QCoreApplication::translate("MeetingUI", "Custom (%1)").arg(customLang));
		customAct->setCheckable(true);
		customAct->setChecked(true);
	}

	// 4. Quick toggle for showing live captions
	QAction *captionToggleAct = menu.addAction(QCoreApplication::translate("MeetingUI", "Show live captions"));
	captionToggleAct->setCheckable(true);
	captionToggleAct->setChecked(prefs.showLiveCaptions);
	connect(captionToggleAct, &QAction::toggled, this, [&session](bool checked) {
		auto updated = session.mediaPreferences();
		updated.showLiveCaptions = checked;
		session.setMediaPreferences(updated);
	});

	menu.addSeparator();
	QAction *settingsAct = menu.addAction(QCoreApplication::translate("MeetingUI", "Translation & Copilot Settings..."));
	connect(settingsAct, &QAction::triggered, this, [this] {
		openCopilotSettingsDialog();
	});

	menu.exec(globalPos);
}

} // namespace MeetingUI
