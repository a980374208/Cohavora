#include "src/ui/room_top_bar_widget.h"
#include "src/ui/detail/meeting_room_helpers.h"
#include "src/ui/meeting_layout_controls.h"
#include "src/ui/app_icons.h"
#include "src/ui/app_theme.h"
#include "src/ui/telemetry_dialogs.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QTimer>
#include <QtGui/QClipboard>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMenu>
#include <QtWidgets/QToolTip>

#include <algorithm>
#include <cmath>

namespace MeetingUI {
using ColorRole = AppTheme::ColorRole;
using detail::themeColor;

RoomTopBarWidget::RoomTopBarWidget(QWidget *parent)
	: Ui::RpWidget(parent) {
	AppTheme::observeChanges(*this, [this] { update(); });
	setMinimumHeight(44);
	setMouseTracking(true);
	setAttribute(Qt::WA_OpaquePaintEvent, false);
#if COHAVORA_SHOW_CONSOLE_BUTTON
	_uiaConsole = new QPushButton(this);
	_uiaConsole->setObjectName(QStringLiteral("meetingConsole"));
	_uiaConsole->setAccessibleName(QCoreApplication::translate("MeetingUI", "Console"));
	_uiaConsole->setFlat(true);
	_uiaConsole->setAttribute(Qt::WA_TransparentForMouseEvents);
	_uiaConsole->setStyleSheet(QStringLiteral("QPushButton { background: transparent; border: 0; }"));
	connect(_uiaConsole, &QPushButton::clicked, this, [this] { _consoleStream.fire({}); });
#endif
	_layoutButton = new MeetingLayoutButton(this);
	connect(_layoutButton, &QToolButton::clicked, this, &RoomTopBarWidget::layoutMenuRequested);
	_uiaTelemetry = new QPushButton(this);
	_uiaTelemetry->setObjectName(QStringLiteral("meetingTelemetry"));
	_uiaTelemetry->setAccessibleName(QCoreApplication::translate("MeetingUI", "Meeting telemetry"));
	_uiaTelemetry->setFlat(true);
	_uiaTelemetry->setAttribute(Qt::WA_TransparentForMouseEvents);
	_uiaTelemetry->setStyleSheet(QStringLiteral("QPushButton { background: transparent; border: 0; }"));
	connect(_uiaTelemetry, &QPushButton::clicked, this, [this] {
		emit telemetryDetailsRequested();
	});
}

void RoomTopBarWidget::updateDuration(int seconds) {
	_durationSeconds = seconds;
	update();
}

void RoomTopBarWidget::setActiveSpeaker(const QString &speakerName) {
	if (_speakerName == speakerName) return;
	_speakerName = speakerName;
	QResizeEvent ev(size(), size());
	resizeEvent(&ev);
	update();
}

void RoomTopBarWidget::setMeetingId(const QString &meetingId) {
	_meetingId = meetingId;
	if (auto *parent = parentWidget()) {
		QCoreApplication::postEvent(parent, new QResizeEvent(parent->size(), parent->size()));
	}
	QResizeEvent ev(size(), size());
	resizeEvent(&ev);
	update();
}

void RoomTopBarWidget::setTelemetrySnapshot(const QVariantMap &snapshot) {
	if (!snapshot.isEmpty() && !_telemetrySnapshot.isEmpty() &&
		snapshot.value(QStringLiteral("sessionGeneration")).toULongLong() ==
			_telemetrySnapshot.value(QStringLiteral("sessionGeneration")).toULongLong() &&
		snapshot.value(QStringLiteral("revision")).toULongLong() <
			_telemetrySnapshot.value(QStringLiteral("revision")).toULongLong()) {
		return;
	}
	_telemetrySnapshot = snapshot;
	update();
}

void RoomTopBarWidget::setLayoutSelection(VideoViewMode mode, livekit::SpeakerLayout speakerLayout) {
	_currentViewMode = mode;
	static_cast<MeetingLayoutButton *>(_layoutButton)->setSelection(
		mode == VideoViewMode::Speaker || mode == VideoViewMode::Pip
			? livekit::VideoLayoutMode::Speaker : livekit::VideoLayoutMode::Grid,
		speakerLayout);
}

void RoomTopBarWidget::setEncrypted(bool encrypted) {
	if (_encrypted != encrypted) {
		_encrypted = encrypted;
		QResizeEvent ev(size(), size());
		resizeEvent(&ev);
		update();
	}
}

void RoomTopBarWidget::setAgentStatus(
		bool online,
		const QString &state,
		const QString &agentName,
		int agentCount,
		bool localTranslatorActive,
		bool localAgentRoleActive) {
	const QString normalizedState = state.trimmed().toLower();
	const QString normalizedName = agentName.trimmed();
	const int normalizedCount = online ? std::max(1, agentCount) : 0;
	if (_agentOnline == online &&
		_agentState == normalizedState &&
		_agentName == normalizedName &&
		_agentCount == normalizedCount &&
		_localTranslatorActive == localTranslatorActive &&
		_localAgentRoleActive == localAgentRoleActive) {
		return;
	}
	_agentOnline = online;
	_agentState = normalizedState;
	_agentName = normalizedName;
	_agentCount = normalizedCount;
	_localTranslatorActive = localTranslatorActive;
	_localAgentRoleActive = localAgentRoleActive;
	QResizeEvent ev(size(), size());
	resizeEvent(&ev);
	updateGeometry();
	if (auto *parent = parentWidget()) {
		QCoreApplication::postEvent(parent, new QResizeEvent(parent->size(), parent->size()));
	}
	update();
}

QString RoomTopBarWidget::agentBadgeText() const {
	if (!_agentOnline) {
		if (_localTranslatorActive) {
			return QCoreApplication::translate("MeetingUI", "Local Translating");
		}
		return QCoreApplication::translate("MeetingUI", "Agent Offline");
	}
	QString label;
	if (_agentState == QStringLiteral("listening")) {
		label = QCoreApplication::translate("MeetingUI", "Agent Listening");
	} else if (_agentState == QStringLiteral("thinking")) {
		label = QCoreApplication::translate("MeetingUI", "Agent Thinking");
	} else if (_agentState == QStringLiteral("speaking")) {
		label = QCoreApplication::translate("MeetingUI", "Agent Speaking");
	} else if (_agentState == QStringLiteral("initializing")) {
		label = QCoreApplication::translate("MeetingUI", "Agent Starting");
	} else {
		label = QCoreApplication::translate("MeetingUI", "Agent Online");
	}
	if (_localAgentRoleActive) {
		label = QCoreApplication::translate("MeetingUI", "%1 (You)").arg(label);
	} else if (_agentCount > 1) {
		label = QStringLiteral("%1 (%2)").arg(label).arg(_agentCount);
	}
	return label;
}

QString RoomTopBarWidget::agentTooltipText() const {
	QString summary;
	if (_localAgentRoleActive) {
		summary = QCoreApplication::translate(
			"MeetingUI", "You are currently acting as the room transcription/translation Agent.");
	} else if (_agentOnline) {
		const QString base = agentBadgeText();
		summary = !_agentName.isEmpty()
			? QCoreApplication::translate("MeetingUI", "%1 - %2").arg(base, _agentName)
			: base;
	} else if (_localTranslatorActive) {
		summary = QCoreApplication::translate(
			"MeetingUI", "Local meeting translation is active (visible only to you).");
	} else {
		summary = QCoreApplication::translate(
			"MeetingUI", "No LiveKit Agent is currently online in this room.");
	}
	return summary + QLatin1Char('\n') + QCoreApplication::translate(
		"MeetingUI", "Click to manage live translation and room Agent role.");
}

void RoomTopBarWidget::setMaximized(bool maximized) {
	if (_maximized != maximized) {
		_maximized = maximized;
		update();
	}
}

bool RoomTopBarWidget::eventHook(QEvent *event) {
	const auto handled = Ui::RpWidget::eventHook(event);
	if (event->type() == QEvent::LayoutRequest && _layoutButton) {
		QResizeEvent resize(size(), size());
		resizeEvent(&resize);
		updateGeometry();
		if (auto *parent = parentWidget())
			QCoreApplication::postEvent(parent, new QResizeEvent(parent->size(), parent->size()));
		update();
	}
	return handled;
}

int RoomTopBarWidget::heightForWidth(int width) const {
	QFont f(QStringLiteral("Microsoft YaHei"));
	f.setPixelSize(12);
	const QFontMetrics metrics(f);
	_layoutButton->ensurePolished();
	const int toolsWidth = _layoutButton->sizeHint().width()
#if COHAVORA_SHOW_CONSOLE_BUTTON
		+ metrics.horizontalAdvance(QCoreApplication::translate("MeetingUI", "Console")) + 22
#endif
		+ metrics.horizontalAdvance(QCoreApplication::translate("MeetingUI", "Simulate")) + 22
		+ 132;
	const QString idSample = _meetingId.isEmpty() ? QStringLiteral("000-000-000") : _meetingId;
	const int pillWidth = 32 + metrics.horizontalAdvance(idSample) + 26 + 52 + (_encrypted ? 22 : 0);
	const int agentWidth = 33 + metrics.horizontalAdvance(agentBadgeText()) + 8;
	const int rowHeight = std::max(44, _layoutButton->sizeHint().height() + 14);
	return rowHeight * (width < toolsWidth + pillWidth + agentWidth + 40 ? 2 : 1);
}

void RoomTopBarWidget::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	const int w = width();
	const int h = height();
	_layoutButton->ensurePolished();
	const auto layoutSize = _layoutButton->sizeHint();
	const int rowHeight = std::max(44, layoutSize.height() + 14);
	const bool twoRows = heightForWidth(w) > rowHeight;

	// 右上角圆角窗口三键
	const int btnW = 36;
	const int btnH = 30;
	const int winY = (rowHeight - btnH) / 2;
	const int rightPad = 8;
	_closeRect = QRect(w - rightPad - btnW, winY, btnW, btnH);
	_maxRect = QRect(_closeRect.left() - btnW, winY, btnW, btnH);
	_minRect = QRect(_maxRect.left() - btnW, winY, btnW, btnH);

	int rightX = twoRows ? (w - 12) : (_minRect.left() - 14);
	const int rowY = (twoRows ? rowHeight : 0) + (rowHeight - 28) / 2;

	QFont btnFont(QStringLiteral("Microsoft YaHei"));
	btnFont.setPixelSize(12);
	const QFontMetrics metrics(btnFont);
	const auto buttonWidth = [&](const QString &text) { return metrics.horizontalAdvance(text) + 20; };

	const int simulateW = buttonWidth(QCoreApplication::translate("MeetingUI", "Simulate"));
	_simulateRect = QRect(rightX - simulateW, rowY, simulateW, 28);
	rightX -= simulateW + 6;

#if COHAVORA_SHOW_CONSOLE_BUTTON
	const int consoleW = buttonWidth(QCoreApplication::translate("MeetingUI", "Console"));
	_consoleRect = QRect(rightX - consoleW, rowY, consoleW, 28);
	_uiaConsole->setGeometry(_consoleRect);
	rightX -= consoleW + 6;
#else
	_consoleRect = QRect();
#endif

	const int layoutW = layoutSize.width();
	_layoutRect = QRect(
		rightX - layoutW,
		(twoRows ? rowHeight : 0) + (rowHeight - layoutSize.height()) / 2,
		layoutW,
		layoutSize.height());
	_layoutButton->setGeometry(_layoutRect);
	rightX -= layoutW + 8;

	// 左侧统一会议信息胶囊：[ 信号质量 | 会议号 + 复制 | 时长 | E2EE 盾牌 ]
	const int pillY = (rowHeight - 30) / 2;
	const int pillLeft = 14;
	const int maxPillW = std::max(110, (twoRows ? (_minRect.left() - 16) : rightX) - pillLeft - 8);

	QFont idFont(QStringLiteral("Microsoft YaHei"));
	idFont.setPixelSize(12);
	idFont.setBold(true);
	const QFontMetrics idFm(idFont);

	const int qualityW = 24;
	const int timeW = 48;
	const int shieldW = _encrypted ? 22 : 0;
	int idBlockW = 0;
	if (!_meetingId.isEmpty()) {
		const QString copiedText = QCoreApplication::translate("MeetingUI", "Copied");
		const int textW = std::max(idFm.horizontalAdvance(_meetingId), idFm.horizontalAdvance(copiedText));
		idBlockW = std::min(textW + 24, std::max(60, maxPillW - qualityW - timeW - shieldW - 18));
	}
	const int pillW = std::min(maxPillW, 10 + qualityW + (idBlockW > 0 ? idBlockW + 4 : 0) + timeW + shieldW + 8);
	_infoPillRect = QRect(pillLeft, pillY, pillW, 30);

	_qualityRect = QRect(_infoPillRect.left() + 6, _infoPillRect.top() + 3, qualityW, 24);
	_uiaTelemetry->setGeometry(_qualityRect);

	if (idBlockW > 0) {
		_meetingIdRect = QRect(_qualityRect.right() + 4, _infoPillRect.top() + 3, idBlockW, 24);
	} else {
		_meetingIdRect = QRect();
	}

	// 1e. LiveKit Agent 在线状态胶囊（紧跟在会议信息胶囊右侧）
	const int agentLeft = _infoPillRect.right() + 8;
	const int agentRightLimit = (twoRows ? (_minRect.left() - 12) : (rightX - 8));
	const int availForAgent = agentRightLimit - agentLeft;
	const QString agentText = agentBadgeText();
	const int idealAgentW = 10 + 7 + 6 + metrics.horizontalAdvance(agentText) + 10;
	if (availForAgent >= 68) {
		const int agentW = std::min(availForAgent, idealAgentW);
		_agentPillRect = QRect(agentLeft, (rowHeight - 26) / 2, agentW, 26);
	} else {
		_agentPillRect = QRect();
	}

	// 中间发言人提示胶囊（仅当有人发言且空间足够时绘制）
	const int leftContentRight = _agentPillRect.isEmpty() ? _infoPillRect.right() : _agentPillRect.right();
	const int leftBoundary = twoRows ? 14 : (leftContentRight + 10);
	const int rightButtonsLeft = rightX;
	const int availCenterW = rightButtonsLeft - leftBoundary - 12;
	if (!_speakerName.isEmpty() && availCenterW >= 120) {
		const int spW = std::min(availCenterW, 210);
		int spX = (w - spW) / 2;
		spX = std::max(leftBoundary, std::min(spX, rightButtonsLeft - spW));
		_speakerCapsuleRect = QRect(spX, (twoRows ? rowHeight : 0) + (rowHeight - 26) / 2, spW, 26);
	} else {
		_speakerCapsuleRect = QRect();
	}
}

void RoomTopBarWidget::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::TextAntialiasing);

	const int w = width();
	const int h = height();

	p.fillRect(rect(), themeColor(ColorRole::Surface));
	p.setPen(themeColor(ColorRole::Border));
	p.drawLine(0, h - 1, w, h - 1);

	// 1. 左侧统一会议信息胶囊
	if (!_infoPillRect.isEmpty()) {
		p.save();
		const bool idHovered = !_meetingIdRect.isEmpty() && _meetingIdRect.contains(mapFromGlobal(QCursor::pos()));
		p.setPen(QPen(_copiedAnim ? themeColor(ColorRole::Success) : themeColor(ColorRole::Border), 1.0));
		p.setBrush(_copiedAnim ? themeColor(ColorRole::Success, 20) : themeColor(ColorRole::SurfaceAlt));
		p.drawRoundedRect(_infoPillRect, 15, 15);

		// 1a. 信号质量 3 格指示条
		const QString availability = _telemetrySnapshot.value(
			QStringLiteral("availability"), QStringLiteral("UNKNOWN")).toString();
		QColor qualityColor = themeColor(ColorRole::TextMuted);
		if (availability == QStringLiteral("VALID")) qualityColor = themeColor(ColorRole::Success);
		else if (availability == QStringLiteral("WARMING_UP")) qualityColor = themeColor(ColorRole::Accent);
		else if (availability == QStringLiteral("STALE")) qualityColor = themeColor(ColorRole::Warning);
		else if (availability == QStringLiteral("TIMEOUT") ||
			availability == QStringLiteral("INVALID") ||
			availability == QStringLiteral("UNSUPPORTED")) qualityColor = themeColor(ColorRole::Danger);

		if (_hoverBtn == HoverBtn::Quality) {
			p.setPen(Qt::NoPen);
			p.setBrush(themeColor(ColorRole::Hover));
			p.drawRoundedRect(_qualityRect, 10, 10);
		}
		const double coverage = _telemetrySnapshot.value(QStringLiteral("coverage"), 0.0).toDouble();
		const int activeBars = availability == QStringLiteral("VALID")
			? (coverage >= 0.66 ? 3 : (coverage >= 0.33 ? 2 : 1))
			: (availability == QStringLiteral("WARMING_UP") ? 1 : 0);
		const int sigX = _qualityRect.left() + 6;
		const int sigY = _qualityRect.center().y() + 5;
		p.setPen(Qt::NoPen);
		for (int bar = 0; bar != 3; ++bar) {
			p.setBrush(bar < activeBars ? qualityColor : themeColor(ColorRole::Border));
			const int barHeight = 4 + bar * 3;
			p.drawRoundedRect(QRectF(sigX + bar * 4.5, sigY - barHeight, 2.5, barHeight), 1.0, 1.0);
		}

		// 1b. 会议号 + 线性复制图标
		int cursorX = _qualityRect.right() + 4;
		if (!_meetingIdRect.isEmpty() && !_meetingId.isEmpty()) {
			if (idHovered && !_copiedAnim) {
				p.setPen(Qt::NoPen);
				p.setBrush(themeColor(ColorRole::Hover));
				p.drawRoundedRect(_meetingIdRect, 10, 10);
			}
			QFont idFont(QStringLiteral("Microsoft YaHei"));
			idFont.setPixelSize(12);
			idFont.setBold(true);
			p.setFont(idFont);
			const QColor idColor = _copiedAnim
				? themeColor(ColorRole::Success)
				: (idHovered ? themeColor(ColorRole::Accent) : themeColor(ColorRole::Text));
			p.setPen(idColor);
			const QString disp = _copiedAnim
				? QCoreApplication::translate("MeetingUI", "Copied")
				: QFontMetrics(idFont).elidedText(_meetingId, Qt::ElideRight, std::max(10, _meetingIdRect.width() - 22));
			p.drawText(_meetingIdRect.adjusted(6, 0, -18, 0), Qt::AlignVCenter | Qt::AlignLeft, disp);

			const QIcon copyIc = AppTheme::icon(
				_copiedAnim ? AppTheme::Icon::Check : AppTheme::Icon::Copy,
				_copiedAnim ? ColorRole::Success : (idHovered ? ColorRole::Accent : ColorRole::TextMuted));
			copyIc.paint(&p, QRect(_meetingIdRect.right() - 16, _meetingIdRect.center().y() - 6, 12, 12));
			cursorX = _meetingIdRect.right() + 4;
		}

		// 1c. 会议计时 MM:SS
		const int minutes = _durationSeconds / 60;
		const int secs = _durationSeconds % 60;
		const QString timeStr = QStringLiteral("%1:%2")
			.arg(minutes, 2, 10, QChar('0'))
			.arg(secs, 2, 10, QChar('0'));
		QFont timeFont(QStringLiteral("Microsoft YaHei"));
		timeFont.setPixelSize(12);
		p.setFont(timeFont);
		p.setPen(themeColor(ColorRole::TextMuted));
		const int timeRight = _encrypted ? (_infoPillRect.right() - 26) : (_infoPillRect.right() - 8);
		if (timeRight > cursorX + 20) {
			p.drawText(QRect(cursorX, _infoPillRect.top(), timeRight - cursorX, _infoPillRect.height()),
				Qt::AlignCenter, timeStr);
		}

		// 1d. E2EE 加密盾牌徽标
		if (_encrypted) {
			const QIcon shieldIc = AppTheme::icon(AppTheme::Icon::Shield, ColorRole::Accent);
			shieldIc.paint(&p, QRect(_infoPillRect.right() - 22, _infoPillRect.center().y() - 7, 14, 14));
		}
		p.restore();
	}

	// 1e. LiveKit Agent / 本地转译状态胶囊（可点击打开热切换菜单）
	if (!_agentPillRect.isEmpty()) {
		p.save();
		const bool pillActive = _agentOnline || _localTranslatorActive;
		const bool activeSpeechState = (_agentOnline &&
			(_agentState == QStringLiteral("speaking") || _agentState == QStringLiteral("thinking"))) ||
			(!_agentOnline && _localTranslatorActive);
		const ColorRole accentRole = !pillActive
			? ColorRole::TextMuted
			: (activeSpeechState ? ColorRole::Accent : ColorRole::Success);
		const bool hovered = (_hoverBtn == HoverBtn::Agent);
		const QColor pillBg = pillActive
			? themeColor(accentRole, hovered ? 36 : 22)
			: (hovered ? themeColor(ColorRole::Hover) : themeColor(ColorRole::SurfaceAlt));
		const QColor pillBorder = pillActive
			? themeColor(accentRole, hovered ? 140 : 95)
			: (hovered ? themeColor(ColorRole::Accent, 110) : themeColor(ColorRole::Border));
		p.setPen(QPen(pillBorder, 1.0));
		p.setBrush(pillBg);
		p.drawRoundedRect(_agentPillRect, 13, 13);

		// 状态指示圆点
		p.setPen(Qt::NoPen);
		p.setBrush(themeColor(accentRole));
		const qreal dotX = _agentPillRect.left() + 10.0;
		const qreal dotY = _agentPillRect.center().y() - 3.0;
		p.drawEllipse(QRectF(dotX, dotY, 7.0, 7.0));

		QFont agentFont(QStringLiteral("Microsoft YaHei"));
		agentFont.setPixelSize(12);
		agentFont.setBold(pillActive);
		p.setFont(agentFont);
		p.setPen(pillActive ? themeColor(accentRole) : themeColor(ColorRole::TextSecondary));
		const QRect textRect = _agentPillRect.adjusted(23, 0, -9, 0);
		const QString elided = QFontMetrics(agentFont).elidedText(
			agentBadgeText(), Qt::ElideRight, std::max(10, textRect.width()));
		p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, elided);
		p.restore();
	}

	// 2. 中间：仅有人发言时呈现柔和发言人胶囊
	if (!_speakerCapsuleRect.isEmpty() && !_speakerName.isEmpty()) {
		p.save();
		p.setPen(Qt::NoPen);
		p.setBrush(themeColor(ColorRole::AccentSoft));
		p.drawRoundedRect(_speakerCapsuleRect, 13, 13);

		const QIcon micIc = AppTheme::icon(AppTheme::Icon::Microphone, ColorRole::Accent);
		micIc.paint(&p, QRect(_speakerCapsuleRect.left() + 8, _speakerCapsuleRect.center().y() - 6, 12, 12));

		QFont speakerFont(QStringLiteral("Microsoft YaHei"));
		speakerFont.setPixelSize(12);
		p.setFont(speakerFont);
		p.setPen(themeColor(ColorRole::Accent));
		const QString speakerText = QCoreApplication::translate("MeetingUI", "Speaking: %1").arg(_speakerName);
		const QString elided = QFontMetrics(speakerFont).elidedText(
			speakerText, Qt::ElideRight, std::max(10, _speakerCapsuleRect.width() - 32));
		p.drawText(_speakerCapsuleRect.adjusted(24, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft, elided);
		p.restore();
	}

	// 3. 右侧紧凑工具按钮（无 emoji）
	auto drawChipBtn = [&](const QRect &r, const QString &text, bool hovered, ColorRole fgRole) {
		if (r.isEmpty()) return;
		p.save();
		p.setPen(QPen(hovered ? themeColor(ColorRole::Border) : Qt::transparent, 1.0));
		p.setBrush(hovered ? themeColor(ColorRole::Hover) : themeColor(ColorRole::SurfaceAlt));
		p.drawRoundedRect(r, 7, 7);
		QFont f(QStringLiteral("Microsoft YaHei"));
		f.setPixelSize(12);
		p.setFont(f);
		p.setPen(themeColor(fgRole));
		p.drawText(r, Qt::AlignCenter, text);
		p.restore();
	};

#if COHAVORA_SHOW_CONSOLE_BUTTON
	drawChipBtn(_consoleRect, QCoreApplication::translate("MeetingUI", "Console"),
		_hoverBtn == HoverBtn::Console, ColorRole::TextSecondary);
#endif
	drawChipBtn(_simulateRect, QCoreApplication::translate("MeetingUI", "Simulate"),
		_hoverBtn == HoverBtn::Simulate, ColorRole::TextSecondary);

	// 分割线
	if (_minRect.left() > _simulateRect.right() + 6) {
		const int divX = _minRect.left() - 7;
		p.setPen(themeColor(ColorRole::Border));
		p.drawLine(divX, _minRect.center().y() - 8, divX, _minRect.center().y() + 8);
	}

	// 4. 圆角窗口控制三键（与新主界面保持统一）
	p.save();
	const auto paintWinHover = [&p](const QRect &r, const QColor &fill) {
		QPainterPath path;
		path.addRoundedRect(r.adjusted(2, 2, -2, -2), 6, 6);
		p.fillPath(path, fill);
	};
	if (_hoverBtn == HoverBtn::Min) paintWinHover(_minRect, themeColor(ColorRole::Hover));
	if (_hoverBtn == HoverBtn::Max) paintWinHover(_maxRect, themeColor(ColorRole::Hover));
	if (_hoverBtn == HoverBtn::Close) paintWinHover(_closeRect, themeColor(ColorRole::Danger));

	p.setPen(QPen(themeColor(ColorRole::TextSecondary), 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	p.drawLine(_minRect.center().x() - 5, _minRect.center().y(), _minRect.center().x() + 5, _minRect.center().y());

	const QPoint mc = _maxRect.center();
	p.setBrush(Qt::NoBrush);
	if (_maximized) {
		p.drawRoundedRect(QRectF(mc.x() - 5.0, mc.y() - 3.0, 8.0, 8.0), 1.2, 1.2);
		QPainterPath back;
		back.moveTo(mc.x() - 3.0, mc.y() - 3.0);
		back.lineTo(mc.x() - 3.0, mc.y() - 5.0);
		back.lineTo(mc.x() + 5.0, mc.y() - 5.0);
		back.lineTo(mc.x() + 5.0, mc.y() + 3.0);
		back.lineTo(mc.x() + 3.0, mc.y() + 3.0);
		p.drawPath(back);
	} else {
		p.drawRoundedRect(QRectF(mc.x() - 5.0, mc.y() - 5.0, 10.0, 10.0), 1.5, 1.5);
	}

	p.setPen(QPen((_hoverBtn == HoverBtn::Close) ? themeColor(ColorRole::OnAccent) : themeColor(ColorRole::TextSecondary),
		1.35, Qt::SolidLine, Qt::RoundCap));
	const QPoint cc = _closeRect.center();
	p.drawLine(cc.x() - 4, cc.y() - 4, cc.x() + 4, cc.y() + 4);
	p.drawLine(cc.x() + 4, cc.y() - 4, cc.x() - 4, cc.y() + 4);
	p.restore();
}

void RoomTopBarWidget::mouseMoveEvent(QMouseEvent *e) {
	const QPoint pos = e->pos();
	HoverBtn next = HoverBtn::None;

	if (_closeRect.contains(pos)) next = HoverBtn::Close;
	else if (_maxRect.contains(pos)) next = HoverBtn::Max;
	else if (_minRect.contains(pos)) next = HoverBtn::Min;
	else if (_qualityRect.contains(pos)) next = HoverBtn::Quality;
	else if (!_agentPillRect.isEmpty() && _agentPillRect.contains(pos)) next = HoverBtn::Agent;
	else if (_simulateRect.contains(pos)) next = HoverBtn::Simulate;
	else if (_consoleRect.contains(pos)) next = HoverBtn::Console;
	else if (_layoutRect.contains(pos)) next = HoverBtn::Layout;

	if (!_meetingIdRect.isEmpty() && _meetingIdRect.contains(pos)) {
		setCursor(Qt::PointingHandCursor);
		setToolTip(QCoreApplication::translate("MeetingUI", "Click to copy meeting ID: %1").arg(_meetingId));
	} else if (next == HoverBtn::Quality) {
		setCursor(Qt::PointingHandCursor);
		const auto availability = _telemetrySnapshot.value(
			QStringLiteral("availability"), QStringLiteral("UNKNOWN")).toString();
		const auto age = _telemetrySnapshot.value(
			QStringLiteral("sampleAgeMs"), -1).toLongLong();
		const bool statsSamplingEnabled = _telemetrySnapshot.value(
			QStringLiteral("statsSamplingEnabled"), false).toBool();
		QString tooltip;
		if (!statsSamplingEnabled && age < 0) {
			tooltip = QCoreApplication::translate(
				"MeetingUI",
				"Telemetry: Continuous stats sampling is off (enable in Settings)");
		} else {
			tooltip = age >= 0
				? QCoreApplication::translate("MeetingUI", "Telemetry: %1, age %2 ms")
					.arg(LocalizeTelemetryDisplayText(availability)).arg(age)
				: QCoreApplication::translate("MeetingUI", "Telemetry: %1")
					.arg(LocalizeTelemetryDisplayText(availability));
			if (!statsSamplingEnabled) {
				tooltip += QStringLiteral("\n") + QCoreApplication::translate(
					"MeetingUI",
					"Continuous stats sampling is off (enable in Settings)");
			}
		}
		const auto reconnectVideo = _telemetrySnapshot.value(
			QStringLiteral("reconnectVideoAvailability"), QStringLiteral("UNKNOWN")).toString();
		if (reconnectVideo == QStringLiteral("WARMING_UP") ||
			reconnectVideo == QStringLiteral("TIMEOUT")) {
			tooltip += QStringLiteral("\n") +
				QCoreApplication::translate("MeetingUI", "Video recovery: %1 (%2/%3 stable)")
					.arg(LocalizeTelemetryDisplayText(reconnectVideo),
						 _telemetrySnapshot.value(QStringLiteral("reconnectVideoRecovered")).toString(),
						 _telemetrySnapshot.value(QStringLiteral("reconnectVideoExpected")).toString());
		}
		setToolTip(tooltip);
	} else if (next == HoverBtn::Agent) {
		setCursor(Qt::PointingHandCursor);
		setToolTip(agentTooltipText());
	} else if (next != HoverBtn::None) {
		setCursor(Qt::PointingHandCursor);
		setToolTip(QString());
	} else {
		setCursor(Qt::ArrowCursor);
		setToolTip(QString());
	}

	if (next != _hoverBtn) {
		_hoverBtn = next;
		update();
	}
}

bool RoomTopBarWidget::isWindowDragArea(const QPoint &position) const {
	return rect().contains(position) &&
		!_qualityRect.contains(position) && !_meetingIdRect.contains(position) &&
		!_agentPillRect.contains(position) &&
		!_minRect.contains(position) && !_maxRect.contains(position) &&
		!_closeRect.contains(position) && !_consoleRect.contains(position) &&
		!_simulateRect.contains(position) && !_layoutRect.contains(position);
}

void RoomTopBarWidget::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		if (!_meetingIdRect.isEmpty() && _meetingIdRect.contains(e->pos())) {
			if (!_meetingId.isEmpty()) {
				QApplication::clipboard()->setText(_meetingId);
				_copiedAnim = true;
				update();
				QTimer::singleShot(1800, this, [this]() {
					_copiedAnim = false;
					update();
				});
			}
			return;
		}
		if (_qualityRect.contains(e->pos())) {
			emit telemetryDetailsRequested();
		} else if (!_agentPillRect.isEmpty() && _agentPillRect.contains(e->pos())) {
			emit agentMenuRequested(mapToGlobal(QPoint(_agentPillRect.left(), _agentPillRect.bottom() + 4)));
		} else if (_minRect.contains(e->pos())) {
			_minStream.fire({});
		} else if (_maxRect.contains(e->pos())) {
			_maxStream.fire({});
		} else if (_closeRect.contains(e->pos())) {
			_closeStream.fire({});
		} else if (_consoleRect.contains(e->pos())) {
			_consoleStream.fire({});
		} else if (_simulateRect.contains(e->pos())) {
			showSimulateScenarioMenu(mapToGlobal(QPoint(_simulateRect.left(), _simulateRect.bottom() + 4)));
		} else if (_layoutRect.contains(e->pos())) {
			emit layoutMenuRequested();
		} else {
			emit windowDragRequested();
			e->accept();
			return;
		}
	}
	Ui::RpWidget::mousePressEvent(e);
}

void RoomTopBarWidget::showSimulateScenarioMenu(const QPoint &globalPos) {
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

void RoomTopBarWidget::leaveEventHook(QEvent *e) {
	_hoverBtn = HoverBtn::None;
	update();
	Ui::RpWidget::leaveEventHook(e);
}

// ----------------------------------------------------
// RoomBottomBarWidget 实现
// ----------------------------------------------------


} // namespace MeetingUI
