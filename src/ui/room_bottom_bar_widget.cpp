#include "src/ui/room_bottom_bar_widget.h"
#include "src/ui/detail/meeting_room_helpers.h"
#include "src/ui/app_icons.h"
#include "src/ui/app_theme.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QTimer>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QToolTip>

#include <algorithm>
#include <cmath>

namespace MeetingUI {
using ColorRole = AppTheme::ColorRole;
using detail::ShowMeetingWarning;
using detail::themeColor;

namespace {

// The popup owns only a cancellable subscription, never a native driver job.
// Even cached results are queued so no action is populated before popup().
template <typename Discovery, typename Apply>
void loadDeviceMenu(QMenu *menu, Discovery &discovery, Apply apply) {
    auto gate = OpenMeeting::QtCallbackGate<QMenu>::Create(menu);
    auto request = std::make_shared<typename Discovery::Request>();
    QObject::connect(menu, &QMenu::aboutToHide, menu, [gate, request] {
        gate->Revoke();
        request->reset();
    });
    QTimer::singleShot(0, menu, [&discovery, gate, request, apply = std::move(apply)] {
        if (!gate->active()) return;
        *request = discovery.request([gate, apply](const auto &result) {
            gate->Post([apply, result](QMenu *target) { apply(*target, result); });
        });
    });
}

constexpr int kBottomBarHeight = 72;
constexpr int kBottomBarPadding = 12;
constexpr int kBottomBarGap = 6;
constexpr int kBottomBarEndWidth = 88;
constexpr int kBottomBarPreferredToolWidth = 76;

} // namespace

RoomBottomBarWidget::RoomBottomBarWidget(QWidget *parent)
	: Ui::RpWidget(parent) {
	AppTheme::observeChanges(*this, [this] { update(); });
	setMinimumHeight(kBottomBarHeight);
	setMouseTracking(true);
	setAttribute(Qt::WA_OpaquePaintEvent, false);

	_chatInput = new QLineEdit(this);
	_chatInput->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Type a message..."));
	MeetingUI::AppTheme::setStyleVariant(*_chatInput, "meeting-room-window-chatinput");

	_handBtn = new QPushButton(QString::fromUtf8("✋"), this);
	_handBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Raise Hand to Speak"));
	_handBtn->setFixedSize(32, 32);
	MeetingUI::AppTheme::setStyleVariant(*_handBtn, "meeting-room-window-handbtn");

	connect(_chatInput, &QLineEdit::returnPressed, [this] {
		if (!_chatInput->text().trimmed().isEmpty()) {
			_sendChatStream.fire_copy(_chatInput->text().trimmed());
			_chatInput->clear();
		}
	});

	connect(_handBtn, &QPushButton::clicked, [this] {
		QMessageBox::information(this, QCoreApplication::translate("MeetingUI", "Raise Hand"), QCoreApplication::translate("MeetingUI", "The host has been notified that you would like to speak."));
	});

	_chatInput->hide();
	_handBtn->hide();
	const auto accessibleTool = [this](const char *id, const QString &name) {
		auto *button = new QPushButton(this);
		button->setObjectName(QString::fromLatin1(id));
		button->setAccessibleName(name);
		button->setFlat(true);
		button->setAttribute(Qt::WA_TransparentForMouseEvents);
		button->setStyleSheet(QStringLiteral("QPushButton { background: transparent; border: 0; }"));
		return button;
	};
	_uiaParticipants = accessibleTool("meetingParticipants", QCoreApplication::translate("MeetingUI", "Participants"));
	_uiaChat = accessibleTool("meetingChat", QCoreApplication::translate("MeetingUI", "Chat"));
	_uiaWhiteboard = accessibleTool("meetingWhiteboard", QCoreApplication::translate("MeetingUI", "Whiteboard"));
	_uiaCopilot = accessibleTool("meetingCopilot", QCoreApplication::translate("MeetingUI", "Copilot"));
	_uiaAudio = accessibleTool("meetingMicrophone", QCoreApplication::translate("MeetingUI", "Microphone"));
	_uiaVideo = accessibleTool("meetingCamera", QCoreApplication::translate("MeetingUI", "Camera"));
	_uiaShare = accessibleTool("meetingShareScreen", QCoreApplication::translate("MeetingUI", "Share Screen"));
	_uiaEnd = accessibleTool("meetingLeave", QCoreApplication::translate("MeetingUI", "Leave Meeting"));
	for (auto *button : {_uiaAudio, _uiaVideo, _uiaShare}) button->setCheckable(true);
	_uiaAudio->setChecked(!_audioMuted);
	_uiaVideo->setChecked(_videoEnabled);
	connect(_uiaParticipants, &QPushButton::clicked, this, [this] { _participantsStream.fire({}); });
	connect(_uiaChat, &QPushButton::clicked, this, [this] { _chatStream.fire({}); });
	connect(_uiaWhiteboard, &QPushButton::clicked, this, [this] { _whiteboardStream.fire({}); });
	connect(_uiaCopilot, &QPushButton::clicked, this, [this] { _copilotStream.fire({}); });
	// Qt 5's accessibility Toggle action calls QAbstractButton::toggle(),
	// which emits toggled but not clicked. Commit the same business action for
	// UIA and ordinary activation. State projection updates the model first,
	// so its setChecked() notification must not submit another action.
	connect(_uiaAudio, &QPushButton::toggled, this, [this](bool enabled) {
		if (enabled == !_audioMuted) return;
		// UIA Toggle enters synchronously through COM. Device enumeration and
		// dialogs must run after that call returns, on the UI event loop.
		QMetaObject::invokeMethod(this, [this, enabled] {
			if (enabled != !_audioMuted) toggleAudio();
		}, Qt::QueuedConnection);
	});
	connect(_uiaVideo, &QPushButton::toggled, this, [this](bool enabled) {
		if (enabled == _videoEnabled) return;
		QMetaObject::invokeMethod(this, [this, enabled] {
			if (enabled != _videoEnabled) toggleVideo();
		}, Qt::QueuedConnection);
	});
	connect(_uiaShare, &QPushButton::toggled, this, [this](bool enabled) {
		if (enabled == canStopScreenShare()) return;
		QMetaObject::invokeMethod(this, [this, enabled] {
			if (!screenShareControlEnabled()) {
				_uiaShare->setChecked(canStopScreenShare());
				return;
			}
			if (enabled != canStopScreenShare()) _shareScreenStream.fire({});
		}, Qt::QueuedConnection);
	});
	connect(_uiaEnd, &QPushButton::clicked, this, [this] { _endMeetingStream.fire({}); });
}

void RoomBottomBarWidget::setAudioMuted(bool muted) {
	cancelDeviceAction(DeviceKind::Microphone);
	_audioMuted = muted;
	_uiaAudio->setEnabled(!_inRecovery);
	_uiaAudio->setChecked(!muted);
	_uiaAudio->setAccessibleName(QCoreApplication::translate("MeetingUI", muted ? "Unmute" : "Mute"));
	update();
}

void RoomBottomBarWidget::setSpeakerMuted(bool muted) {
	cancelDeviceAction(DeviceKind::Speaker);
	_speakerMuted = muted;
	update();
}

void RoomBottomBarWidget::setVideoEnabled(bool enabled) {
	cancelDeviceAction(DeviceKind::Camera);
	_videoEnabled = enabled;
	_uiaVideo->setEnabled(!_inRecovery);
	_uiaVideo->setChecked(enabled);
	_uiaVideo->setAccessibleName(QCoreApplication::translate("MeetingUI", enabled ? "Stop Video" : "Start Video"));
	update();
}

void RoomBottomBarWidget::setScreenShareState(livekit::ScreenShareState state) {
	_screenShareState = state;
	_uiaShare->setChecked(canStopScreenShare());
	_uiaShare->setAccessibleName(QCoreApplication::translate("MeetingUI",
		canStopScreenShare() ? "Stop Sharing" : "Share Screen"));
	_uiaShare->setEnabled(screenShareControlEnabled());
	update();
}

void RoomBottomBarWidget::setScreenShareAvailable(bool available) {
	_screenShareAvailable = available;
#ifndef Q_OS_WIN
    _uiaShare->setToolTip(QCoreApplication::translate("MeetingUI", available
        ? "Choose a screen or window in the system picker. Video only."
        : "Screen sharing is unavailable in this desktop session."));
#endif
	_uiaShare->setEnabled(screenShareControlEnabled());
	update();
}

void RoomBottomBarWidget::setWhiteboardHasContent(bool hasContent) {
	if (_whiteboardHasContent == hasContent) return;
	_whiteboardHasContent = hasContent;
	update();
}

void RoomBottomBarWidget::cancelDeviceAction(DeviceKind kind) {
    auto &pending = _pendingDeviceActions[static_cast<std::size_t>(kind)];
    if (pending.callbacks) pending.callbacks->Revoke();
    pending = {};
}

void RoomBottomBarWidget::requestDeviceEnable(DeviceKind kind) {
    if (_inRecovery) return;
    auto &pending = _pendingDeviceActions[static_cast<std::size_t>(kind)];
    if (pending.callbacks) return;
    auto gate = OpenMeeting::QtCallbackGate<RoomBottomBarWidget>::Create(this);
    pending.callbacks = gate;
    if (kind == DeviceKind::Microphone) {
        _uiaAudio->setChecked(!_audioMuted);
        _uiaAudio->setEnabled(false);
    } else if (kind == DeviceKind::Camera) {
        _uiaVideo->setChecked(_videoEnabled);
        _uiaVideo->setEnabled(false);
    }
    const auto complete = [gate, kind](bool available) {
        gate->Post([kind, available](RoomBottomBarWidget *bar) {
            bar->cancelDeviceAction(kind);
            if (bar->_inRecovery) return;
            if (kind == DeviceKind::Microphone) bar->_uiaAudio->setEnabled(true);
            if (kind == DeviceKind::Camera) bar->_uiaVideo->setEnabled(true);
            if (!available) {
                const bool camera = kind == DeviceKind::Camera;
                const bool speaker = kind == DeviceKind::Speaker;
                ShowMeetingWarning(bar,
                    camera ? "meetingCameraUnavailable" : speaker ? "meetingSpeakerUnavailable" : "meetingMicrophoneUnavailable",
                    camera ? "meetingCameraUnavailableDismiss" : speaker ? "meetingSpeakerUnavailableDismiss" : "meetingMicrophoneUnavailableDismiss",
                    QCoreApplication::translate("MeetingUI", camera ? "Camera Unavailable" : speaker ? "Speaker Unavailable" : "Microphone Unavailable"),
                    QCoreApplication::translate("MeetingUI", camera ? "No camera device is available. Video cannot be enabled." : speaker ?
                        "No speaker output device is available. The speaker cannot be enabled." :
                        "No microphone input device is available. The microphone cannot be enabled."));
                return;
            }
            if (kind == DeviceKind::Camera) {
                bar->setVideoEnabled(true);
                bar->_toggleVideoStream.fire_copy(true);
            } else if (kind == DeviceKind::Microphone) {
                bar->setAudioMuted(false);
                bar->_toggleAudioStream.fire_copy(false);
            } else {
                bar->setSpeakerMuted(false);
                bar->_toggleSpeakerStream.fire_copy(false);
            }
        });
    };
    if (kind == DeviceKind::Camera) {
        auto &discovery = _cameraDiscovery ? *_cameraDiscovery : CameraDeviceDiscovery::Instance();
        pending.subscription = discovery.request([complete](const auto &result) {
            complete(result.succeeded && std::any_of(result.devices->begin(), result.devices->end(),
                [](const auto &device) { return !device.path.empty(); }));
        });
    } else {
        auto &discovery = kind == DeviceKind::Speaker
            ? (_speakerDiscovery ? *_speakerDiscovery : SpeakerDeviceDiscovery())
            : (_microphoneDiscovery ? *_microphoneDiscovery : MicrophoneDeviceDiscovery());
        pending.subscription = discovery.request([complete](const auto &result) {
            complete(result.succeeded && std::any_of(result.devices->begin(), result.devices->end(),
                [](const auto &device) { return !device.id.empty(); }));
        });
    }
}

void RoomBottomBarWidget::toggleAudio() {
    if (_inRecovery) { _uiaAudio->setChecked(!_audioMuted); return; }
    if (_audioMuted) { requestDeviceEnable(DeviceKind::Microphone); return; }
    setAudioMuted(true);
    _toggleAudioStream.fire_copy(true);
}

void RoomBottomBarWidget::toggleVideo() {
    if (_inRecovery) { _uiaVideo->setChecked(_videoEnabled); return; }
    if (!_videoEnabled) { requestDeviceEnable(DeviceKind::Camera); return; }
    setVideoEnabled(false);
    _toggleVideoStream.fire_copy(false);
}

void RoomBottomBarWidget::toggleSpeaker() {
    if (_inRecovery) return;
    if (_speakerMuted) { requestDeviceEnable(DeviceKind::Speaker); return; }
    setSpeakerMuted(true);
    _toggleSpeakerStream.fire_copy(true);
}

void RoomBottomBarWidget::hideEvent(QHideEvent *event) {
    for (auto kind : {DeviceKind::Microphone, DeviceKind::Speaker, DeviceKind::Camera}) cancelDeviceAction(kind);
    if (_deviceMenu) _deviceMenu->close();
    if (_uiaAudio) _uiaAudio->setEnabled(!_inRecovery);
    if (_uiaVideo) _uiaVideo->setEnabled(!_inRecovery);
    Ui::RpWidget::hideEvent(event);
}

void RoomBottomBarWidget::setParticipantCount(int count) {
	_participantCount = count;
	update();
}

void RoomBottomBarWidget::setChatUnreadCount(int count) {
	_chatUnreadCount = count;
	update();
}

void RoomBottomBarWidget::setActiveSidebar(int sidebarId) {
	if (_activeSidebarId != sidebarId) {
		_activeSidebarId = sidebarId;
		update();
	}
}

void RoomBottomBarWidget::setInRecovery(bool inRecovery) {
	if (_inRecovery == inRecovery) return;
	_inRecovery = inRecovery;
    if (inRecovery) {
        for (auto kind : {DeviceKind::Microphone, DeviceKind::Speaker, DeviceKind::Camera}) cancelDeviceAction(kind);
        if (_deviceMenu) _deviceMenu->close();
    }
	for (auto *button : {_uiaParticipants, _uiaChat, _uiaWhiteboard, _uiaCopilot, _uiaAudio, _uiaVideo})
		button->setEnabled(!inRecovery);
	_uiaShare->setEnabled(screenShareControlEnabled());
	update();
}

int RoomBottomBarWidget::heightForWidth(int width) const {
	Q_UNUSED(width);
	return kBottomBarHeight;
}

void RoomBottomBarWidget::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	const int w = width();
	const int h = height();

	// 方案 1：4 组悬浮胶囊岛 Dock（媒体 / 协作 / 侧栏 / 离开），与 Stage 同色融合底槽
	_toolItems = {
		// Group 0: Media
		{ 1, QCoreApplication::translate("MeetingUI", "Unmute"), QCoreApplication::translate("MeetingUI", "Mute"), QRect(), true },
		{ 11, QCoreApplication::translate("MeetingUI", "Enable Speaker"), QCoreApplication::translate("MeetingUI", "Speaker"), QRect(), true },
		{ 2, QCoreApplication::translate("MeetingUI", "Start Video"), QCoreApplication::translate("MeetingUI", "Stop Video"), QRect(), true },
		// Group 1: Collaboration
		{ 3, QCoreApplication::translate("MeetingUI", "Share Screen"), QCoreApplication::translate("MeetingUI", "Share Screen"), QRect(), false },
		{ 7, QCoreApplication::translate("MeetingUI", "Whiteboard"), QCoreApplication::translate("MeetingUI", "Whiteboard"), QRect(), false },
		{ 4, QCoreApplication::translate("MeetingUI", "Invite"), QCoreApplication::translate("MeetingUI", "Invite"), QRect(), false },
		{ 10, QCoreApplication::translate("MeetingUI", "Simulate Scenario"), QCoreApplication::translate("MeetingUI", "Simulate Scenario"), QRect(), false },
		// Group 2: Sidebars
		{ 5, QCoreApplication::translate("MeetingUI", "Participants (%1)").arg(_participantCount), QCoreApplication::translate("MeetingUI", "Participants (%1)").arg(_participantCount), QRect(), false },
		{ 6, QCoreApplication::translate("MeetingUI", "Chat"), QCoreApplication::translate("MeetingUI", "Chat"), QRect(), false },
		{ 8, QCoreApplication::translate("MeetingUI", "Copilot"), QCoreApplication::translate("MeetingUI", "Copilot"), QRect(), false },
	};

	_chatInput->hide();
	_handBtn->hide();

	constexpr int dockH = 46;
	constexpr int btnH = 34;
	constexpr int groupPad = 6;
	constexpr int itemGap = 4;
	constexpr int groupGap = 10;
	const int dockY = (h - dockH) / 2;
	const int btnY = dockY + (dockH - btnH) / 2;

	const auto widthForItem = [](int id, bool compact) -> int {
		switch (id) {
		case 1: return compact ? 52 : 88;
		case 11: return compact ? 42 : 48;
		case 2: return compact ? 52 : 88;
		case 3: return compact ? 44 : 84;
		case 7: return compact ? 38 : 68;
		case 4: return compact ? 38 : 64;
		case 10: return 32;
		case 5: return compact ? 50 : 60;
		case 6: return 38;
		case 8: return compact ? 46 : 62;
		default: return 40;
		}
	};

	const auto computeTotal = [&](bool compact) -> int {
		int sum = 0;
		for (const auto &it : _toolItems) sum += widthForItem(it.id, compact);
		sum += 7 * itemGap + 3 * (groupPad * 2);
		sum += (compact ? 56 : 86);
		sum += 3 * groupGap;
		return sum;
	};

	_compactDockLabels = (w - 24) < computeTotal(false);
	const int desiredTotal = computeTotal(_compactDockLabels);
	const int availW = std::max(240, w - 24);
	const qreal scale = desiredTotal > availW ? (static_cast<qreal>(availW) / desiredTotal) : 1.0;

	const auto scaledW = [scale](int v) { return std::max(26, static_cast<int>(std::round(v * scale))); };
	const int scaledGap = std::max(2, static_cast<int>(std::round(itemGap * scale)));
	const int scaledGroupGap = std::max(4, static_cast<int>(std::round(groupGap * scale)));
	const int scaledPad = std::max(4, static_cast<int>(std::round(groupPad * scale)));

	constexpr std::array<std::pair<size_t, size_t>, 3> groupRanges = {{
		{ 0, 3 }, { 3, 7 }, { 7, 10 }
	}};
	std::array<int, 3> groupWidths{};
	int totalW = 0;
	for (size_t g = 0; g < 3; ++g) {
		int gw = scaledPad * 2;
		for (size_t i = groupRanges[g].first; i < groupRanges[g].second; ++i) {
			gw += scaledW(widthForItem(_toolItems[i].id, _compactDockLabels));
			if (i + 1 < groupRanges[g].second) gw += scaledGap;
		}
		groupWidths[g] = gw;
		totalW += gw;
	}
	const int endW = scaledW(_compactDockLabels ? 56 : 86);
	totalW += endW + 3 * scaledGroupGap;

	int cursorX = std::max(12, (w - totalW) / 2);
	for (size_t g = 0; g < 3; ++g) {
		_dockGroupRects[g] = QRect(cursorX, dockY, groupWidths[g], dockH);
		int ix = cursorX + scaledPad;
		for (size_t i = groupRanges[g].first; i < groupRanges[g].second; ++i) {
			const int iw = scaledW(widthForItem(_toolItems[i].id, _compactDockLabels));
			_toolItems[i].rect = QRect(ix, btnY, iw, btnH);
			switch (_toolItems[i].id) {
			case 1: _uiaAudio->setGeometry(_toolItems[i].rect); break;
			case 2: _uiaVideo->setGeometry(_toolItems[i].rect); break;
			case 3: _uiaShare->setGeometry(_toolItems[i].rect); break;
			case 5: _uiaParticipants->setGeometry(_toolItems[i].rect); break;
			case 6: _uiaChat->setGeometry(_toolItems[i].rect); break;
			case 7: _uiaWhiteboard->setGeometry(_toolItems[i].rect); break;
			case 8: _uiaCopilot->setGeometry(_toolItems[i].rect); break;
			default: break;
			}
			ix += iw + scaledGap;
		}
		cursorX += groupWidths[g] + scaledGroupGap;
	}

	_endMeetingRect = QRect(cursorX, dockY, endW, dockH);
	_uiaEnd->setGeometry(_endMeetingRect);
}

void RoomBottomBarWidget::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::TextAntialiasing);

	// 1. 与视频舞台无缝同色，呈现悬浮岛视觉且零覆盖 DX11 画布
	p.fillRect(rect(), themeColor(ColorRole::Stage));

	// 2. 绘制 3 个悬浮胶囊岛容器
	const QColor islandShadow(0, 0, 0, AppTheme::effectiveTheme() == AppTheme::Theme::Dark ? 45 : 18);
	for (const auto &gr : _dockGroupRects) {
		if (gr.isEmpty()) continue;
		p.save();
		p.setPen(Qt::NoPen);
		p.setBrush(islandShadow);
		p.drawRoundedRect(gr.translated(0, 2), 15, 15);
		p.setPen(QPen(themeColor(ColorRole::Border), 1.0));
		p.setBrush(themeColor(ColorRole::Surface));
		p.drawRoundedRect(gr, 15, 15);
		p.restore();
	}

	QFont labelFont(QStringLiteral("Microsoft YaHei"));
	labelFont.setPixelSize(12);
	labelFont.setBold(false);

	// 3. 绘制每个胶囊内工具项
	for (const auto &item : _toolItems) {
		const QRect r = item.rect;
		if (r.isEmpty()) continue;
		const bool available = item.id == 3 ? screenShareControlEnabled() : !_inRecovery;
		const bool hovered = available && (_hoveredId == item.id);

		p.save();
		if (!available) {
			p.setOpacity(0.42);
		}

		QColor fill = Qt::transparent;
		QColor border = Qt::transparent;
		QColor fg = themeColor(ColorRole::TextSecondary);
		ColorRole iconRole = ColorRole::TextSecondary;
		QString shortLabel;
		bool boldLabel = false;

		if (item.id == 1) {
			if (_audioMuted) {
				fill = themeColor(ColorRole::Danger, hovered ? 52 : 34);
				border = themeColor(ColorRole::Danger, 95);
				fg = themeColor(ColorRole::Danger);
				iconRole = ColorRole::Danger;
			} else {
				fill = themeColor(hovered ? ColorRole::Hover : ColorRole::SurfaceAlt);
				fg = themeColor(ColorRole::Text);
				iconRole = ColorRole::Text;
			}
			if (!_compactDockLabels) {
				shortLabel = _audioMuted
					? QCoreApplication::translate("MeetingUI", "Muted")
					: QCoreApplication::translate("MeetingUI", "Mute");
				boldLabel = true;
			}
		} else if (item.id == 11) {
			if (_speakerMuted) {
				fill = themeColor(ColorRole::Danger, hovered ? 52 : 34);
				fg = themeColor(ColorRole::Danger);
				iconRole = ColorRole::Danger;
			} else if (hovered) {
				fill = themeColor(ColorRole::Hover);
				fg = themeColor(ColorRole::Text);
				iconRole = ColorRole::Text;
			}
		} else if (item.id == 2) {
			if (!_videoEnabled) {
				fill = themeColor(ColorRole::Danger, hovered ? 52 : 34);
				border = themeColor(ColorRole::Danger, 95);
				fg = themeColor(ColorRole::Danger);
				iconRole = ColorRole::Danger;
			} else {
				fill = themeColor(hovered ? ColorRole::Hover : ColorRole::SurfaceAlt);
				fg = themeColor(ColorRole::Text);
				iconRole = ColorRole::Text;
			}
			if (!_compactDockLabels) {
				shortLabel = _videoEnabled
					? QCoreApplication::translate("MeetingUI", "Video")
					: QCoreApplication::translate("MeetingUI", "Off");
				boldLabel = true;
			}
		} else if (item.id == 3) {
			using State = livekit::ScreenShareState;
			const bool active = _screenShareState == State::Active ||
				_screenShareState == State::Starting ||
				_screenShareState == State::AwaitingPermission;
			if (active) {
				fill = themeColor(hovered ? ColorRole::AccentHover : ColorRole::Accent);
				fg = themeColor(ColorRole::OnAccent);
				iconRole = ColorRole::OnAccent;
			} else {
				fill = themeColor(hovered ? ColorRole::Accent : ColorRole::AccentSoft);
				fg = themeColor(hovered ? ColorRole::OnAccent : ColorRole::Accent);
				iconRole = hovered ? ColorRole::OnAccent : ColorRole::Accent;
			}
			if (!_compactDockLabels) {
				if (_screenShareState == State::Active) shortLabel = QCoreApplication::translate("MeetingUI", "Stop");
				else if (_screenShareState == State::Starting || _screenShareState == State::AwaitingPermission)
					shortLabel = QCoreApplication::translate("MeetingUI", "Cancel");
				else if (_screenShareState == State::Stopping) shortLabel = QCoreApplication::translate("MeetingUI", "Stopping");
				else if (_screenShareState == State::StopFailed) shortLabel = QCoreApplication::translate("MeetingUI", "Retry");
				else shortLabel = QCoreApplication::translate("MeetingUI", "Share");
				boldLabel = true;
			}
		} else if (item.id == 7) {
			if (_whiteboardHasContent) {
				fill = themeColor(ColorRole::AccentSoft);
				fg = themeColor(ColorRole::Accent);
				iconRole = ColorRole::Accent;
			} else if (hovered) {
				fill = themeColor(ColorRole::Hover);
				fg = themeColor(ColorRole::Text);
				iconRole = ColorRole::Text;
			}
			if (!_compactDockLabels) shortLabel = QCoreApplication::translate("MeetingUI", "Board");
		} else if (item.id == 4) {
			if (hovered) {
				fill = themeColor(ColorRole::Hover);
				fg = themeColor(ColorRole::Text);
				iconRole = ColorRole::Text;
			}
			if (!_compactDockLabels) shortLabel = QCoreApplication::translate("MeetingUI", "Invite");
		} else if (item.id == 10) {
			if (hovered) {
				fill = themeColor(ColorRole::Hover);
				fg = themeColor(ColorRole::Text);
				iconRole = ColorRole::Text;
			}
		} else if (item.id == 5) {
			const bool active = (_activeSidebarId == 5);
			if (active) {
				fill = themeColor(ColorRole::AccentSoft);
				fg = themeColor(ColorRole::Accent);
				iconRole = ColorRole::Accent;
			} else if (hovered) {
				fill = themeColor(ColorRole::Hover);
				fg = themeColor(ColorRole::Text);
				iconRole = ColorRole::Text;
			}
			shortLabel = QString::number(_participantCount);
			boldLabel = active;
		} else if (item.id == 6) {
			const bool active = (_activeSidebarId == 6);
			if (active) {
				fill = themeColor(ColorRole::AccentSoft);
				fg = themeColor(ColorRole::Accent);
				iconRole = ColorRole::Accent;
			} else if (hovered) {
				fill = themeColor(ColorRole::Hover);
				fg = themeColor(ColorRole::Text);
				iconRole = ColorRole::Text;
			}
		} else if (item.id == 8) {
			const bool active = (_activeSidebarId == 8);
			if (active) {
				fill = themeColor(hovered ? ColorRole::AccentHover : ColorRole::Accent);
				fg = themeColor(ColorRole::OnAccent);
				iconRole = ColorRole::OnAccent;
			} else {
				fill = themeColor(ColorRole::AccentSoft);
				border = hovered ? themeColor(ColorRole::Accent) : Qt::transparent;
				fg = themeColor(ColorRole::Accent);
				iconRole = ColorRole::Accent;
			}
			shortLabel = QStringLiteral("AI");
			boldLabel = true;
		}

		if (fill.alpha() > 0 || border.alpha() > 0) {
			p.setPen(border.alpha() > 0 ? QPen(border, 1.0) : Qt::NoPen);
			p.setBrush(fill);
			p.drawRoundedRect(r, 10, 10);
		}

		const int arrowW = item.hasDropdown ? 14 : 0;
		const QRect contentRect = r.adjusted(4, 0, -(4 + arrowW), 0);
		constexpr int iconSize = 15;
		QFont f = labelFont;
		f.setBold(boldLabel);
		p.setFont(f);
		const int textW = shortLabel.isEmpty() ? 0 : QFontMetrics(f).horizontalAdvance(shortLabel);
		const int gap = shortLabel.isEmpty() ? 0 : 5;
		const int totalContentW = iconSize + gap + textW;
		const int startX = contentRect.left() + std::max(0, (contentRect.width() - totalContentW) / 2);
		const QRect iconRect(startX, contentRect.center().y() - iconSize / 2, iconSize, iconSize);

		if (item.id == 1) {
			AppTheme::icon(_audioMuted ? AppTheme::Icon::MicrophoneOff : AppTheme::Icon::Microphone, iconRole)
				.paint(&p, iconRect);
		} else if (item.id == 11) {
			AppTheme::icon(AppTheme::Icon::Audio, iconRole).paint(&p, iconRect);
			if (_speakerMuted) {
				p.setPen(QPen(fg, 1.7, Qt::SolidLine, Qt::RoundCap));
				p.drawLine(iconRect.topLeft() + QPoint(1, 1), iconRect.bottomRight() - QPoint(1, 1));
			}
		} else if (item.id == 2) {
			AppTheme::icon(_videoEnabled ? AppTheme::Icon::Video : AppTheme::Icon::VideoOff, iconRole)
				.paint(&p, iconRect);
		} else if (item.id == 3) {
			AppTheme::icon(AppTheme::Icon::ScreenShare, iconRole).paint(&p, iconRect);
		} else if (item.id == 7) {
			p.setPen(QPen(fg, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
			p.setBrush(Qt::NoBrush);
			p.drawRoundedRect(iconRect.adjusted(1, 2, -1, -3), 2, 2);
			p.drawLine(iconRect.left() + 4, iconRect.center().y() + 1, iconRect.center().x(), iconRect.center().y() - 2);
			p.drawLine(iconRect.center().x(), iconRect.center().y() - 2, iconRect.right() - 4, iconRect.center().y() + 1);
		} else if (item.id == 4) {
			p.setPen(QPen(fg, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
			p.setBrush(Qt::NoBrush);
			p.drawEllipse(QPointF(iconRect.left() + 5.5, iconRect.top() + 4.5), 2.5, 2.5);
			p.drawArc(QRectF(iconRect.left() + 1.5, iconRect.top() + 8.5, 8.0, 6.0), 0, 180 * 16);
			p.drawLine(QPointF(iconRect.right() - 3.0, iconRect.top() + 4.5), QPointF(iconRect.right() - 3.0, iconRect.top() + 9.5));
			p.drawLine(QPointF(iconRect.right() - 5.5, iconRect.top() + 7.0), QPointF(iconRect.right() - 0.5, iconRect.top() + 7.0));
		} else if (item.id == 5) {
			p.setPen(QPen(fg, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
			p.setBrush(Qt::NoBrush);
			p.drawEllipse(QPointF(iconRect.left() + 5.5, iconRect.top() + 4.5), 2.5, 2.5);
			p.drawArc(QRectF(iconRect.left() + 1.0, iconRect.top() + 8.5, 9.0, 6.5), 0, 180 * 16);
			p.drawEllipse(QPointF(iconRect.right() - 4.0, iconRect.top() + 5.0), 2.0, 2.0);
			p.drawArc(QRectF(iconRect.right() - 7.5, iconRect.top() + 9.0, 6.5, 5.5), 0, 140 * 16);
		} else if (item.id == 6) {
			AppTheme::icon(AppTheme::Icon::ChatMode, iconRole).paint(&p, iconRect);
			if (_chatUnreadCount > 0) {
				p.save();
				p.setPen(Qt::NoPen);
				p.setBrush(themeColor(ColorRole::Danger));
				const QRect badge(r.right() - 14, r.top() + 3, 12, 12);
				p.drawEllipse(badge);
				QFont bf(QStringLiteral("Microsoft YaHei"));
				bf.setPixelSize(9);
				bf.setBold(true);
				p.setFont(bf);
				p.setPen(themeColor(ColorRole::OnAccent));
				p.drawText(badge, Qt::AlignCenter, _chatUnreadCount > 9 ? QStringLiteral("9+") : QString::number(_chatUnreadCount));
				p.restore();
			}
		} else if (item.id == 8) {
			AppTheme::icon(AppTheme::Icon::Sparkle, iconRole).paint(&p, iconRect);
		} else if (item.id == 10) {
			AppTheme::icon(AppTheme::Icon::MoreHorizontal, iconRole).paint(&p, iconRect);
		}

		if (!shortLabel.isEmpty()) {
			p.setPen(fg);
			const QRect textRect(iconRect.right() + gap, contentRect.top(),
				std::max(0, contentRect.right() - iconRect.right() - gap + 1), contentRect.height());
			const QString visible = QFontMetrics(f).elidedText(shortLabel, Qt::ElideRight, textRect.width());
			p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, visible);
		}

		if (item.hasDropdown) {
			p.setPen(QPen(fg, 1.35, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
			const int ax = r.right() - 9;
			const int ay = r.center().y();
			p.drawLine(ax - 3, ay - 1, ax, ay + 2);
			p.drawLine(ax, ay + 2, ax + 3, ay - 1);
		}

		p.restore();
	}

	// 4. 岛 4：独立实心红离开胶囊按钮
	if (!_endMeetingRect.isEmpty()) {
		p.save();
		p.setPen(Qt::NoPen);
		p.setBrush(islandShadow);
		p.drawRoundedRect(_endMeetingRect.translated(0, 2), 15, 15);

		QColor dangerBg = themeColor(ColorRole::Danger);
		if (_endHovered) {
			dangerBg = AppTheme::effectiveTheme() == AppTheme::Theme::Dark
				? dangerBg.lighter(112)
				: dangerBg.darker(110);
		}
		p.setBrush(dangerBg);
		p.drawRoundedRect(_endMeetingRect, 15, 15);

		const QColor onDanger(255, 255, 255);
		const QString leaveLabel = _compactDockLabels ? QString() : QCoreApplication::translate("MeetingUI", "Leave");
		QFont endFont(QStringLiteral("Microsoft YaHei"));
		endFont.setPixelSize(12);
		endFont.setBold(true);
		p.setFont(endFont);
		const int iconW = 14;
		const int textW = leaveLabel.isEmpty() ? 0 : QFontMetrics(endFont).horizontalAdvance(leaveLabel);
		const int gap = leaveLabel.isEmpty() ? 0 : 6;
		const int startX = _endMeetingRect.left() + std::max(6, (_endMeetingRect.width() - (iconW + gap + textW)) / 2);
		const int cy = _endMeetingRect.center().y();

		p.setPen(QPen(onDanger, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		p.setBrush(Qt::NoBrush);
		p.drawLine(startX + 4, cy - 6, startX + 10, cy - 6);
		p.drawLine(startX + 10, cy - 6, startX + 10, cy + 6);
		p.drawLine(startX + 10, cy + 6, startX + 4, cy + 6);
		p.drawLine(startX, cy, startX + 7, cy);
		p.drawLine(startX + 4, cy - 3, startX + 7, cy);
		p.drawLine(startX + 4, cy + 3, startX + 7, cy);

		if (!leaveLabel.isEmpty()) {
			p.setPen(onDanger);
			p.drawText(QRect(startX + iconW + gap, _endMeetingRect.top(),
				std::max(0, _endMeetingRect.right() - (startX + iconW + gap) - 4), _endMeetingRect.height()),
				Qt::AlignVCenter | Qt::AlignLeft, leaveLabel);
		}
		p.restore();
	}
}

QMenu *RoomBottomBarWidget::createDeviceMenu(const char *name) {
    if (_deviceMenu) _deviceMenu->close();
    auto *menu = new QMenu(this);
    menu->setObjectName(QString::fromLatin1(name));
    _deviceMenu = menu;
    AppTheme::styleMenu(
        *menu,
        AppTheme::effectiveTheme() == AppTheme::Theme::Dark
            ? AppTheme::Tone::Dark
            : AppTheme::Tone::Light);
    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    return menu;
}

void RoomBottomBarWidget::showAudioDeviceMenu(const QPoint &globalPos) {
    showAudioEndpointMenu(globalPos, false);
}

void RoomBottomBarWidget::showSpeakerDeviceMenu(const QPoint &globalPos) {
    showAudioEndpointMenu(globalPos, true);
}

void RoomBottomBarWidget::showAudioEndpointMenu(const QPoint &globalPos, bool speaker) {
    if (_inRecovery) return;
    auto *menu = createDeviceMenu(speaker ? "meetingSpeakerDeviceMenu" : "meetingMicrophoneDeviceMenu");
    const auto populate = [this, speaker](QMenu &target, const AudioDeviceDiscovery::Result *result) {
        target.clear();
        target.addAction(QCoreApplication::translate("MeetingUI", speaker ?
            "🔊 Select Speaker (Output)" : "🎤 Select Microphone (Input)"))->setEnabled(false);
        auto *group = new QActionGroup(&target);
        const auto add = [&](const QString &title, const QString &id) {
            auto *action = target.addAction(title);
            action->setData(id);
            action->setCheckable(true);
            action->setChecked(id == (speaker ? _currentSpeakerId : _currentMicId));
            group->addAction(action);
            connect(action, &QAction::triggered, this, [this, speaker, id] {
                if (_inRecovery) return;
                if (speaker) _speakerDeviceStream.fire_copy(id);
                else _micDeviceStream.fire_copy(id);
            });
        };
        add(QCoreApplication::translate("MeetingUI", "System Default"), QString());
        if (result && result->succeeded) {
            for (const auto &device : *result->devices) {
                if (device.id.empty()) continue; // The ADM default uses the existing empty-ID entry.
                auto title = QString::fromStdString(device.name);
                if (device.is_default) title += QCoreApplication::translate("MeetingUI", " (System Default)");
                add(title, QString::fromStdString(device.id));
            }
        } else {
            target.addAction(QCoreApplication::translate("MeetingUI", result ?
                "Failed to list audio devices" : "Loading audio devices..."))->setEnabled(false);
        }
        if (speaker) {
            target.addSeparator();
            auto *toggle = target.addAction(QCoreApplication::translate("MeetingUI", _speakerMuted ?
                "🔊 Enable Speaker Output" : "🔇 Mute Speaker Output"));
            connect(toggle, &QAction::triggered, this, &RoomBottomBarWidget::toggleSpeaker);
        }
    };
    populate(*menu, nullptr);
    auto &discovery = speaker ? (_speakerDiscovery ? *_speakerDiscovery : SpeakerDeviceDiscovery())
        : (_microphoneDiscovery ? *_microphoneDiscovery : MicrophoneDeviceDiscovery());
    loadDeviceMenu(menu, discovery, [populate](QMenu &target, const auto &result) { populate(target, &result); });
    menu->popup(globalPos);
}

void RoomBottomBarWidget::showVideoDeviceMenu(const QPoint &globalPos) {
    if (_inRecovery) return;
    auto *menu = createDeviceMenu("meetingCameraDeviceMenu");
    const auto populate = [this](QMenu &target, const CameraDeviceDiscovery::Result *result) {
        target.clear();
        target.addAction(QCoreApplication::translate("MeetingUI", "📷 Select Camera"))->setEnabled(false);
        if (result && result->succeeded) {
            const auto &devices = *result->devices;
            auto defaultDevice = std::find_if(devices.begin(), devices.end(),
                [](const auto &device) { return device.is_default; });
            if (defaultDevice == devices.end()) defaultDevice = devices.begin();
            auto *group = new QActionGroup(&target);
            for (const auto &device : devices) {
                const bool isDefault = &device == &*defaultDevice;
                const auto id = QString::fromStdString(device.path);
                auto title = QString::fromStdString(device.name);
                if (isDefault) title += QCoreApplication::translate("MeetingUI", " (System Default)");
                auto *action = target.addAction(title);
                action->setData(id);
                action->setCheckable(true);
                action->setChecked(_currentCameraPath.isEmpty() ? isDefault : _currentCameraPath == id);
                group->addAction(action);
                connect(action, &QAction::triggered, this, [this, id] {
                    if (_inRecovery) return;
                    _currentCameraPath = id;
                    _videoDeviceStream.fire_copy(id);
                });
            }
            if (devices.empty()) target.addAction(QCoreApplication::translate("MeetingUI", "No camera available"))->setEnabled(false);
        } else {
            target.addAction(QCoreApplication::translate("MeetingUI", result ?
                "Failed to list cameras" : "Loading cameras..."))->setEnabled(false);
        }
        target.addSeparator();
        auto *toggle = target.addAction(QCoreApplication::translate("MeetingUI", _videoEnabled ?
            "🚫 Stop Camera Video" : "📷 Start Camera Video"));
        connect(toggle, &QAction::triggered, this, &RoomBottomBarWidget::toggleVideo);
    };
    populate(*menu, nullptr);
    auto &discovery = _cameraDiscovery ? *_cameraDiscovery : CameraDeviceDiscovery::Instance();
    loadDeviceMenu(menu, discovery, [populate](QMenu &target, const auto &result) { populate(target, &result); });
    menu->popup(globalPos);
}

void RoomBottomBarWidget::mouseMoveEvent(QMouseEvent *e) {
	const QPoint pos = e->pos();
	int nextId = -1;
	QString tip;

	if (!_inRecovery || canStopScreenShare()) {
		for (const auto &item : _toolItems) {
			if ((item.id == 3 ? screenShareControlEnabled() : !_inRecovery) && item.rect.contains(pos)) {
				nextId = item.id;
				switch (item.id) {
				case 1: tip = _audioMuted ? QCoreApplication::translate("MeetingUI", "Unmute") : QCoreApplication::translate("MeetingUI", "Mute"); break;
				case 11: tip = _speakerMuted ? QCoreApplication::translate("MeetingUI", "Enable Speaker") : QCoreApplication::translate("MeetingUI", "Speaker"); break;
				case 2: tip = _videoEnabled ? QCoreApplication::translate("MeetingUI", "Stop Video") : QCoreApplication::translate("MeetingUI", "Start Video"); break;
				case 3: tip = canStopScreenShare() ? QCoreApplication::translate("MeetingUI", "Stop Sharing") : QCoreApplication::translate("MeetingUI", "Share Screen"); break;
				case 4: tip = QCoreApplication::translate("MeetingUI", "Invite"); break;
				case 5: tip = QCoreApplication::translate("MeetingUI", "Participants (%1)").arg(_participantCount); break;
				case 6: tip = QCoreApplication::translate("MeetingUI", "Chat"); break;
				case 7: tip = QCoreApplication::translate("MeetingUI", "Whiteboard"); break;
				case 8: tip = QCoreApplication::translate("MeetingUI", "AI Copilot"); break;
				case 10: tip = QCoreApplication::translate("MeetingUI", "Simulate Scenario"); break;
				default: break;
				}
				break;
			}
		}
	}

	const bool nextEnd = _endMeetingRect.contains(pos);
	if (nextEnd) {
		tip = QCoreApplication::translate("MeetingUI", "Leave Meeting");
	}
	setCursor((nextId >= 0 || nextEnd) ? Qt::PointingHandCursor : Qt::ArrowCursor);
	if (toolTip() != tip) {
		setToolTip(tip);
	}

	if (nextId != _hoveredId || nextEnd != _endHovered) {
		_hoveredId = nextId;
		_endHovered = nextEnd;
		update();
	}
}

void RoomBottomBarWidget::mousePressEvent(QMouseEvent *e) {
	if (_inRecovery) {
		// Capture can always be stopped, including while transport recovery
		// disables other operations. Network unpublish converges after recovery.
		if (e->button() == Qt::LeftButton && canStopScreenShare()) {
			for (const auto &item : _toolItems) {
				if (item.id == 3 && item.rect.contains(e->pos())) {
					_shareScreenStream.fire({});
					return;
				}
			}
		}
		if (e->button() == Qt::LeftButton && _endMeetingRect.contains(e->pos())) {
			_endMeetingStream.fire({});
		}
		return;
	}

	if (e->button() == Qt::RightButton) {
		for (const auto &item : _toolItems) {
			if (item.rect.contains(e->pos())) {
				if (item.id == 1) {
					showAudioDeviceMenu(mapToGlobal(QPoint(item.rect.left(), item.rect.top() - 10)));
					return;
				} else if (item.id == 11) {
					showSpeakerDeviceMenu(mapToGlobal(QPoint(item.rect.left(), item.rect.top() - 10)));
					return;
				} else if (item.id == 2) {
					showVideoDeviceMenu(mapToGlobal(QPoint(item.rect.left(), item.rect.top() - 10)));
					return;
				}
			}
		}
	}

	if (e->button() == Qt::LeftButton) {
		if (_endMeetingRect.contains(e->pos())) {
			_endMeetingStream.fire({});
			return;
		}

		for (const auto &item : _toolItems) {
			if (item.rect.contains(e->pos())) {
				switch (item.id) {
				case 1: {
					// 如果点击的是右侧下拉小三角区域 (宽 16px)
					if (e->pos().x() > item.rect.right() - 16) {
						showAudioDeviceMenu(mapToGlobal(QPoint(item.rect.left(), item.rect.top() - 10)));
						break;
					}
					toggleAudio();
					break;
				}
				case 11: {
					// 如果点击的是右侧下拉小三角区域 (宽 16px)
					if (e->pos().x() > item.rect.right() - 16) {
						showSpeakerDeviceMenu(mapToGlobal(QPoint(item.rect.left(), item.rect.top() - 10)));
						break;
					}
					toggleSpeaker();
					break;
				}
				case 2: {
					// 如果点击的是右侧下拉小三角区域 (宽 16px)
					if (e->pos().x() > item.rect.right() - 16) {
						showVideoDeviceMenu(mapToGlobal(QPoint(item.rect.left(), item.rect.top() - 10)));
						break;
					}
					toggleVideo();
					break;
				}
				case 3:
					if (screenShareControlEnabled()) _shareScreenStream.fire({});
					break;
				case 4:
					_inviteStream.fire({});
					break;
				case 5:
					_participantsStream.fire({});
					break;
				case 6:
					_chatStream.fire({});
					break;
				case 7:
					_whiteboardStream.fire({});
					break;
				case 8:
					_copilotStream.fire({});
					break;
				case 10:
					showSimulateScenarioMenu(mapToGlobal(QPoint(item.rect.left(), item.rect.top() - 10)));
					break;
				}
				break;
			}
		}
	}
}

void RoomBottomBarWidget::showSimulateScenarioMenu(const QPoint &globalPos) {
	QMenu menu(this);
	AppTheme::styleMenu(
		menu,
		AppTheme::effectiveTheme() == AppTheme::Theme::Dark
			? AppTheme::Tone::Dark
			: AppTheme::Tone::Light);

	QAction *header = menu.addAction(QCoreApplication::translate("MeetingUI", "Simulate Scenario"));
	header->setEnabled(false);
	menu.addSeparator();

	struct ScenarioEntry {
		QString name;
		livekit::SimulateScenarioType type;
	};

	const std::vector<ScenarioEntry> entries = {
		{ QCoreApplication::translate("MeetingUI", "Signal reconnect"), livekit::SimulateScenarioType::SignalReconnect },
		{ QCoreApplication::translate("MeetingUI", "Full reconnect"), livekit::SimulateScenarioType::FullReconnect },
		{ QCoreApplication::translate("MeetingUI", "Speaker update"), livekit::SimulateScenarioType::SpeakerUpdate },
		{ QCoreApplication::translate("MeetingUI", "Node failure"), livekit::SimulateScenarioType::NodeFailure },
		{ QCoreApplication::translate("MeetingUI", "Server migration"), livekit::SimulateScenarioType::Migration },
		{ QCoreApplication::translate("MeetingUI", "Server disconnect"), livekit::SimulateScenarioType::ServerLeave },
		{ QCoreApplication::translate("MeetingUI", "Switch ICE candidate"), livekit::SimulateScenarioType::SwitchCandidate },
		{ QCoreApplication::translate("MeetingUI", "Rotate E2EE key"), livekit::SimulateScenarioType::E2eeKeyRatchet },
		{ QCoreApplication::translate("MeetingUI", "Update participant name"), livekit::SimulateScenarioType::ParticipantName },
		{ QCoreApplication::translate("MeetingUI", "Update participant metadata"), livekit::SimulateScenarioType::ParticipantMetadata },
		{ QCoreApplication::translate("MeetingUI", "Clear simulation"), livekit::SimulateScenarioType::Clear }
	};

	for (const auto &entry : entries) {
		QAction *act = menu.addAction(entry.name);
		connect(act, &QAction::triggered, [this, type = entry.type] {
			_simulateScenarioStream.fire_copy(type);
		});
	}

	menu.exec(globalPos);
}

void RoomBottomBarWidget::leaveEventHook(QEvent *e) {
	_hoveredId = -1;
	_endHovered = false;
	update();
	Ui::RpWidget::leaveEventHook(e);
}

} // namespace MeetingUI
