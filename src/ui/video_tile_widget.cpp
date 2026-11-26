#include "src/ui/video_tile_widget.h"
#include "src/ui/detail/meeting_room_helpers.h"
#include "src/ui/meeting_log_console.h"
#include "src/ui/app_icons.h"
#include "src/ui/app_theme.h"
#include "src/core/video_layout_geometry.h"

#include <QtCore/QCoreApplication>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtWidgets/QApplication>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QStyle>
#include <QtWidgets/QToolTip>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace MeetingUI {
using ColorRole = AppTheme::ColorRole;
using detail::themeColor;

// ----------------------------------------------------
// VideoTileWidget 实现
// ----------------------------------------------------

VideoTileWidget::VideoTileWidget(const QString &displayName, bool isLocal, QWidget *parent, bool isScreenShare)
	: Ui::RpWidget(parent)
	, _displayName(displayName)
	, _isLocal(isLocal) {
	_isScreenShare = isScreenShare;
	setMouseTracking(true);
	setToolTip(QStringLiteral("<qt>%1</qt>").arg(_displayName.toHtmlEscaped()));
	setAttribute(Qt::WA_OpaquePaintEvent, false);

	AppTheme::observeChanges(*this, [this] {
		if (_visualizer) _visualizer->setBarColor(themeColor(ColorRole::Success));
		invalidatePresentation();
	});
}

void VideoTileWidget::ensureVisualizer() {
	if (_visualizer) return;
	_visualizer = new AudioVisualizerWidget(this, 7);
	_visualizer->setBarColor(themeColor(ColorRole::Success));
	_visualizer->hide();
	const int w = width();
	const int h = height();
	const auto tag = bottomNameTag(rect());
	const int visualizerWidth = tag.compact ? std::min(60, std::max(0, w - 12)) : 90;
	const int visualizerHeight = tag.compact ? 14 : 24;
	const int visualizerY = tag.compact && !tag.rect.isEmpty()
		? std::max(0, tag.rect.y() - visualizerHeight - 3) : h - 36;
	_visualizer->setGeometry((w - visualizerWidth) / 2, visualizerY, visualizerWidth, visualizerHeight);
}

void VideoTileWidget::setupVolumeControls() {
	if (_pinBtn) return;
	_pinBtn = new QPushButton(QStringLiteral("…"), this);
	_pinBtn->setFixedSize(28, 28);
	_pinBtn->setToolTip(tr("More actions"));
	MeetingUI::AppTheme::setStyleVariant(*_pinBtn,
		_isPinned ? "meeting-room-window-pinbtn-2-active" : "meeting-room-window-pinbtn");
	_pinBtn->setGeometry(pinButtonRect());
	_pinBtn->hide();

	connect(_pinBtn, &QPushButton::clicked, [this] {
		emit moreRequested();
	});

	if (_isLocal || _isScreenShare) return;

	_volBtn = new QPushButton(_isLocallyMuted ? QString::fromUtf8("🔇") : QString::fromUtf8("🔊"), this);
	_volBtn->setFixedSize(28, 28);
	_volBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Adjust this participant's volume"));
	MeetingUI::AppTheme::setStyleVariant(*_volBtn, "meeting-room-window-volbtn");
	_volBtn->move(width() - 70, 10);
	_volBtn->hide();

	connect(_volBtn, &QPushButton::clicked, [this] {
		ensureVolumePopup();
		if (!_volPopup) return;
		if (_volPopup->isVisible()) {
			_volPopup->hide();
		} else {
			_volPopup->move(std::max(0, width() - _volPopup->width() - 10), 42);
			_volPopup->show();
			_volPopup->raise();
		}
	});
}

void VideoTileWidget::ensureVolumePopup() {
	if (_volPopup || _isLocal || _isScreenShare) return;

	_volPopup = new QWidget(this);
	auto *volumeLayout = new QHBoxLayout(_volPopup);
	volumeLayout->setContentsMargins(8, 8, 8, 8);
	volumeLayout->setSpacing(6);
	MeetingUI::AppTheme::setStyleVariant(*_volPopup, "meeting-room-window-volpopup");
	_volPopup->hide();

	_muteRemoteBtn = new QPushButton(_isLocallyMuted ? QString::fromUtf8("🔇") : QString::fromUtf8("🔊"), _volPopup);
	_muteRemoteBtn->setFixedSize(26, 26);
	volumeLayout->addWidget(_muteRemoteBtn);
	MeetingUI::AppTheme::setStyleVariant(*_muteRemoteBtn,
		_isLocallyMuted ? "meeting-room-window-muteremotebtn-2-active" : "meeting-room-window-muteremotebtn");

	_volSlider = new QSlider(Qt::Horizontal, _volPopup);
	_volSlider->setRange(0, 200);
	const int initialVolume = std::clamp(static_cast<int>(_remoteVolume * 100.0f + 0.5f), 0, 200);
	_volSlider->setValue(initialVolume);
	_volSlider->setMinimumWidth(100);
	volumeLayout->addWidget(_volSlider, 1);
	MeetingUI::AppTheme::setStyleVariant(*_volSlider, "meeting-room-window-volslider");

	_volLabel = new QLabel(QString("%1%").arg(initialVolume), _volPopup);
	volumeLayout->addWidget(_volLabel);
	_volPopup->adjustSize();
	MeetingUI::AppTheme::setStyleVariant(*_volLabel, "meeting-room-window-vollabel");
	_volPopup->move(std::max(0, width() - _volPopup->width() - 10), 42);

	connect(_muteRemoteBtn, &QPushButton::clicked, [this] {
		_isLocallyMuted = !_isLocallyMuted;
		_muteRemoteBtn->setText(_isLocallyMuted ? QString::fromUtf8("🔇") : QString::fromUtf8("🔊"));
		MeetingUI::AppTheme::setStyleVariant(*_muteRemoteBtn, _isLocallyMuted ? "meeting-room-window-muteremotebtn-2-active" : "meeting-room-window-muteremotebtn-2-normal");
		if (_volBtn) _volBtn->setText(_isLocallyMuted ? QString::fromUtf8("🔇") : QString::fromUtf8("🔊"));
		remoteLocalMuteToggled(_isLocallyMuted);
	});

	connect(_volSlider, &QSlider::valueChanged, [this](int value) {
		_remoteVolume = static_cast<float>(value) / 100.0f;
		_volLabel->setText(QString("%1%").arg(value));
		if (_isLocallyMuted && value > 0) {
			_isLocallyMuted = false;
			_muteRemoteBtn->setText(QString::fromUtf8("🔊"));
			MeetingUI::AppTheme::setStyleVariant(*_muteRemoteBtn, "meeting-room-window-muteremotebtn-3");
			if (_volBtn) _volBtn->setText(QString::fromUtf8("🔊"));
			remoteLocalMuteToggled(false);
		}
		remoteVolumeChanged(_remoteVolume);
	});
}

void VideoTileWidget::enterEventHook(QEnterEvent *e) {
	if (!_useHardwareCanvas) {
		setupVolumeControls();
		if (_pinBtn) _pinBtn->show();
		if (_volBtn) _volBtn->show();
	}
	Ui::RpWidget::enterEventHook(e);
	invalidatePresentation();
}

void VideoTileWidget::leaveEventHook(QEvent *e) {
	if (_pinBtn) _pinBtn->hide();
	if (_volBtn && (!_volPopup || !_volPopup->isVisible())) {
		_volBtn->hide();
	}
	Ui::RpWidget::leaveEventHook(e);
	invalidatePresentation();
}

void VideoTileWidget::resizeEvent(QResizeEvent *e) {
	Ui::RpWidget::resizeEvent(e);
	const int w = width();
	const int h = height();

	if (_pinBtn) {
		_pinBtn->setGeometry(pinButtonRect());
	}
	if (_volBtn) {
		_volBtn->move(w - 70, 10);
	}
	if (_volPopup) {
		_volPopup->move(std::max(0, w - _volPopup->width() - 10), 42);
	}
	if (_visualizer) {
		const auto tag = bottomNameTag(rect());
		const int visualizerWidth = tag.compact ? std::min(60, std::max(0, w - 12)) : 90;
		const int visualizerHeight = tag.compact ? 14 : 24;
		const int visualizerY = tag.compact && !tag.rect.isEmpty()
			? std::max(0, tag.rect.y() - visualizerHeight - 3) : h - 36;
		_visualizer->setGeometry((w - visualizerWidth) / 2, visualizerY, visualizerWidth, visualizerHeight);
		_visualizer->raise();
	}
}

void VideoTileWidget::setDisplayName(const QString &name) {
	if (_displayName == name) return;
	if (QToolTip::text() == toolTip()) QToolTip::hideText();
	_displayName = name;
	setToolTip(QStringLiteral("<qt>%1</qt>").arg(_displayName.toHtmlEscaped()));
	invalidatePresentation();
}

void VideoTileWidget::setNameDisplayMode(OpenMeeting::ParticipantNameDisplayMode mode) {
	if (_nameDisplayMode == mode) return;
	_nameDisplayMode = mode;
	invalidatePresentation();
}

void VideoTileWidget::resetForReuse(const QString &displayName, float remoteVolume, bool locallyMuted) {
	if (QToolTip::text() == toolTip()) QToolTip::hideText();
	_displayName = displayName;
	setToolTip(QStringLiteral("<qt>%1</qt>").arg(_displayName.toHtmlEscaped()));
	_identity.clear();
	_renderKey.clear();
	_layoutClip.reset();
	clearMask();
	setFrame({});
	_isVideoActive = false;
	_isAudioMuted = false;
	_isVideoStreamPaused = false;
	_videoSubscriptionError = livekit::TrackPublication::SubscriptionError::None;
	_connectionQuality = livekit::ConnectionQuality::Unknown;
	_isSpeaking = false;
	_audioLevel = 0.0f;
	_isPip = false;
	_remoteInputEnabled = false;
	_hasLoggedFirstPaint = false;
	_remoteVolume = remoteVolume;
	_isLocallyMuted = locallyMuted;
	if (_isPinned) {
		_isPinned = false;
		if (_pinBtn) {
			MeetingUI::AppTheme::setStyleVariant(*_pinBtn, "meeting-room-window-pinbtn-2-normal");
		}
	}
	if (_pinBtn) _pinBtn->hide();
	if (_volBtn) {
		_volBtn->setText(_isLocallyMuted ? QString::fromUtf8("🔇") : QString::fromUtf8("🔊"));
		_volBtn->hide();
	}
	if (_volPopup) {
		_volPopup->hide();
		if (_muteRemoteBtn) {
			_muteRemoteBtn->setText(_isLocallyMuted ? QString::fromUtf8("🔇") : QString::fromUtf8("🔊"));
			MeetingUI::AppTheme::setStyleVariant(*_muteRemoteBtn,
				_isLocallyMuted ? "meeting-room-window-muteremotebtn-2-active" : "meeting-room-window-muteremotebtn-2-normal");
		}
		if (_volSlider) {
			QSignalBlocker blocker(_volSlider);
			const int vol = std::clamp(static_cast<int>(_remoteVolume * 100.0f + 0.5f), 0, 200);
			_volSlider->setValue(vol);
			if (_volLabel) _volLabel->setText(QString("%1%").arg(vol));
		}
	}
	if (_visualizer) {
		_visualizer->setActive(false);
		_visualizer->hide();
	}
	_hardwareDecoration = {};
	_externalSurfaceActive = false;
	_externalSurfaceVisible = false;
	_externalSurfaceMinimized = false;
	_externalLogicalSize = {};
	updateRenderExpectation();
}

bool VideoTileWidget::nameVisible(bool hovered) const {
	switch (_nameDisplayMode) {
	case OpenMeeting::ParticipantNameDisplayMode::Always:
		return true;
	case OpenMeeting::ParticipantNameDisplayMode::OnHover:
		return hovered;
	default: {
		// The threshold follows the tile's logical size, independent of display DPI.
		const QSize logical = (_externalSurfaceActive && !_externalLogicalSize.isEmpty())
			? _externalLogicalSize : size();
		return hovered || qint64(logical.width()) * logical.height() > 250000;
	}
	}
}

void VideoTileWidget::setVideoActive(bool active) {
	if (_isVideoActive == active) return;
	_isVideoActive = active;
	livekit::render::RenderFrameMetadata::Ptr retired;
	if (!active) {
		{
			std::lock_guard<std::mutex> lock(_frameMutex);
			_currentFrame = QImage();
			retired = std::exchange(_currentRenderFrame, {});
		}
		if (retired) {
			retired->SetRenderExpected(
				false, livekit::render::RenderExpectationReason::BindingEnded);
		}
	}
	updateRenderExpectation();
	invalidatePresentation();
}

void VideoTileWidget::setAudioMuted(bool muted) {
	if (_isAudioMuted == muted) return;
	_isAudioMuted = muted;
	if (muted) {
		_isSpeaking = false;
		_audioLevel = 0.0f;
		if (_visualizer) {
			_visualizer->setActive(false);
			_visualizer->hide();
		}
	}
	invalidatePresentation();
}

void VideoTileWidget::setConnectionQuality(livekit::ConnectionQuality quality) {
	if (_connectionQuality == quality) return;
	_connectionQuality = quality;
	invalidatePresentation();
}

void VideoTileWidget::setVideoStreamPaused(bool paused) {
	if (_isVideoStreamPaused == paused) return;
	_isVideoStreamPaused = paused;
	updateRenderExpectation();
	invalidatePresentation();
}

void VideoTileWidget::setVideoSubscriptionError(
		livekit::TrackPublication::SubscriptionError error) {
	if (_videoSubscriptionError == error) return;
	_videoSubscriptionError = error;
	if (error != livekit::TrackPublication::SubscriptionError::None) setFrame({});
	updateRenderExpectation();
	invalidatePresentation();
}

VideoTileWidget::~VideoTileWidget() {
	livekit::render::RenderFrameMetadata::Ptr retired;
	{
		std::lock_guard<std::mutex> lock(_frameMutex);
		retired = std::exchange(_currentRenderFrame, {});
	}
	if (retired) {
		retired->SetRenderExpected(
			false, livekit::render::RenderExpectationReason::BindingEnded);
	}
}

void VideoTileWidget::setSpeaking(bool speaking, float level) {
	if (_isSpeaking == speaking && _audioLevel == level) return;
	_isSpeaking = speaking;
	_audioLevel = level;
	if (speaking && !_useHardwareCanvas) {
		ensureVisualizer();
	}
	if (_visualizer) {
		const bool showVisualizer = speaking && !_useHardwareCanvas;
		_visualizer->setActive(showVisualizer);
		if (showVisualizer) {
			_visualizer->setAudioLevel(level);
			_visualizer->show();
			_visualizer->raise();
		} else {
			_visualizer->hide();
		}
	}
	invalidatePresentation();
}

void VideoTileWidget::invalidatePresentation() {
	_hardwareDecoration = {};
	if (!_useHardwareCanvas) update();
	emit presentationChanged();
}

void VideoTileWidget::setPinned(bool pinned) {
	if (_isPinned == pinned) return;
	_isPinned = pinned;
	if (_pinBtn) {
		MeetingUI::AppTheme::setStyleVariant(*_pinBtn, pinned ? "meeting-room-window-pinbtn-2-active" : "meeting-room-window-pinbtn-2-normal");
		_pinBtn->setVisible(underMouse() && !_useHardwareCanvas);
	}
	invalidatePresentation();
}

void VideoTileWidget::setPipMode(bool pip) {
	if (_isPip == pip) return;
	_isPip = pip;
	invalidatePresentation();
}

void VideoTileWidget::setLayoutClip(const QRect &visible) {
	const auto clipped = visible.intersected(rect());
	_layoutClip = clipped == rect() ? std::nullopt : std::optional(clipped);
	if (!_useHardwareCanvas && _layoutClip && !clipped.isEmpty()) setMask(QRegion(clipped));
	else clearMask();
}

QRect VideoTileWidget::pinButtonRect() const {
	return QRect(width() - 36, 10, 28, 28);
}

QImage VideoTileWidget::hardwareDecoration(const QSize &pixels, bool hasFrame, bool hovered) {
	if (pixels.isEmpty() || size().isEmpty()) return {};
	const auto themeRevision = AppTheme::revision();
	if (!_hardwareDecoration.isNull() && _hardwareDecoration.size() == pixels &&
		_decorationLogicalSize == size() && _decorationHasFrame == hasFrame &&
		_decorationHovered == hovered && _decorationThemeRevision == themeRevision) return _hardwareDecoration;
	_hardwareDecoration = QImage(pixels, QImage::Format_RGBA8888_Premultiplied);
	_hardwareDecoration.fill(Qt::transparent);
	_decorationLogicalSize = size();
	_decorationHasFrame = hasFrame;
	_decorationHovered = hovered;
	_decorationThemeRevision = themeRevision;
	QPainter painter(&_hardwareDecoration);
	painter.scale(double(pixels.width()) / width(), double(pixels.height()) / height());
	paintCard(painter, true, hasFrame, hovered);
	painter.end();
	return _hardwareDecoration;
}

void VideoTileWidget::paintOnExternalSurface(QPainter &p, const QRect &targetRect, bool hovered) {
	if (targetRect.isEmpty()) return;
	if (size() != targetRect.size()) {
		resize(targetRect.size());
	}
	p.save();
	p.translate(targetRect.topLeft());
	p.setClipRect(QRect(QPoint(0, 0), targetRect.size()), Qt::IntersectClip);
	paintCard(p, false, false, hovered);
	p.restore();
}

void VideoTileWidget::setExternalCpuSurfaceState(
		bool active, bool visible, bool minimized, const QSize &logicalSize) {
	if (_externalSurfaceActive == active &&
		_externalSurfaceVisible == visible &&
		_externalSurfaceMinimized == minimized &&
		_externalLogicalSize == logicalSize) {
		return;
	}
	_externalSurfaceActive = active;
	_externalSurfaceVisible = visible;
	_externalSurfaceMinimized = minimized;
	_externalLogicalSize = logicalSize;
	updateRenderExpectation();
}

void VideoTileWidget::setHardwareCanvasMode(bool enabled) {
	if (_useHardwareCanvas == enabled) return;
	_useHardwareCanvas = enabled;
	if (enabled) {
		if (_pinBtn) _pinBtn->hide();
		if (_volBtn) _volBtn->hide();
		if (_volPopup) _volPopup->hide();
		if (_visualizer) {
			_visualizer->setActive(false);
			_visualizer->hide();
		}
		clearMask();
	} else if (_layoutClip && !_layoutClip->isEmpty()) {
		setMask(QRegion(*_layoutClip));
	}
	updateRenderExpectation();
	if (!enabled) update();
}

void VideoTileWidget::setFrame(
		const QImage &image,
		livekit::render::VideoRenderFrame::Ptr renderFrame) {
	livekit::render::RenderFrameMetadata::Ptr retired;
	livekit::render::RenderFrameMetadata::Ptr current;
	{
		std::lock_guard<std::mutex> lock(_frameMutex);
		_currentFrame = image;
		retired = std::exchange(_currentRenderFrame, renderFrame ? renderFrame->presentationMetadata() : nullptr);
		current = _currentRenderFrame;
	}
	if (retired && retired != current &&
		(!current || !retired->same_binding_as(*current))) {
		retired->SetRenderExpected(
			false, livekit::render::RenderExpectationReason::BindingEnded);
	}
	updateRenderExpectation();
	update();
}

void VideoTileWidget::updateRenderExpectation() {
	livekit::render::RenderFrameMetadata::Ptr frame;
	{
		std::lock_guard<std::mutex> lock(_frameMutex);
		frame = _currentRenderFrame;
	}
	if (!frame) return;
	const bool minimized = _externalSurfaceActive
		? _externalSurfaceMinimized
		: (window() && window()->isMinimized());
	const QSize effectiveSize = _externalSurfaceActive ? _externalLogicalSize : size();
	const bool surfaceVisible = _externalSurfaceActive ? _externalSurfaceVisible : isVisible();
	const bool expected = !_useHardwareCanvas && _isVideoActive &&
		_videoSubscriptionError == livekit::TrackPublication::SubscriptionError::None &&
		!_isVideoStreamPaused && surfaceVisible && !minimized &&
		effectiveSize.width() > 0 && effectiveSize.height() > 0;
	frame->SetRenderExpected(
		expected,
		expected ? livekit::render::RenderExpectationReason::SurfaceVisible
			: minimized ? livekit::render::RenderExpectationReason::WindowMinimized
				: _isVideoStreamPaused
					? livekit::render::RenderExpectationReason::StreamPaused
					: livekit::render::RenderExpectationReason::SurfaceHidden);
}

void VideoTileWidget::showEvent(QShowEvent *e) {
	Ui::RpWidget::showEvent(e);
	updateRenderExpectation();
}

void VideoTileWidget::hideEvent(QHideEvent *e) {
	Ui::RpWidget::hideEvent(e);
	updateRenderExpectation();
}

void VideoTileWidget::changeEvent(QEvent *e) {
	Ui::RpWidget::changeEvent(e);
	if (e->type() == QEvent::WindowStateChange ||
		e->type() == QEvent::ParentChange) {
		updateRenderExpectation();
	}
}

void VideoTileWidget::paintEvent(QPaintEvent *e) {
	QPainter p(this);
	paintCard(p, false, false, underMouse());
}

void VideoTileWidget::paintCard(QPainter &p, bool decorationOnly, bool hasFrame, bool hovered) {
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	p.setRenderHint(QPainter::TextAntialiasing);

	const QRect r = rect();
	const bool showName = nameVisible(hovered);

	if (_isPip && !decorationOnly) {
		QPainterPath path;
		path.addRoundedRect(r.adjusted(2, 2, -2, -2), 10, 10);
		p.setClipPath(path);
		p.fillPath(path, themeColor(ColorRole::Tile));
	}

	if (_videoSubscriptionError != livekit::TrackPublication::SubscriptionError::None) {
		drawVideoPlaceholder(p, r);
	} else if (_isVideoActive) {
		if (!decorationOnly) drawVideoFrame(p, r);
		else if (_isVideoStreamPaused || !hasFrame) drawVideoPlaceholder(p, r);
	} else {
		drawAvatarPlaceholder(p, r, showName);
	}

	if (_remoteInputEnabled && !decorationOnly) return;
	if (showName) drawBottomNameTag(p, r);
	drawNetworkQualityBadge(p, r);
	if (decorationOnly && hovered) {
		p.save();
		const auto button = pinButtonRect();
		p.setPen(Qt::NoPen);
		p.setBrush(_isPinned ? themeColor(ColorRole::Accent) : themeColor(ColorRole::Surface, 235));
		p.drawRoundedRect(button, 14, 14);
		p.setPen(themeColor(_isPinned ? ColorRole::OnAccent : ColorRole::Text));
		p.setFont(QFont("Segoe UI Emoji", 10));
		p.drawText(button, Qt::AlignCenter, QStringLiteral("…"));
		p.restore();
	}

	// 画中画模式下的基础边框
	if (_isPip) {
		p.setClipping(false);
		p.setPen(QPen(_isSpeaking ? themeColor(ColorRole::Success) : themeColor(ColorRole::TextMuted), _isSpeaking ? 3.0 : 2.0));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(r.adjusted(1, 1, -1, -1), 10, 10);
	}

	// 说话中：绘制高质感双层绿色呼吸发光光圈 (Active Speaker Halo)
	if (_isSpeaking && !_isAudioMuted) {
		p.save();
		p.setClipping(false);
		p.setBrush(Qt::NoBrush);

		int alpha = static_cast<int>(60 + 150 * std::clamp(_audioLevel * 4.0f, 0.1f, 1.0f));
		QPen outerGlow(themeColor(ColorRole::Success, alpha / 3), 6.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
		p.setPen(outerGlow);
		p.drawRoundedRect(r.adjusted(3, 3, -3, -3), _isPip ? 10 : 8, _isPip ? 10 : 8);

		QPen innerFocus(themeColor(ColorRole::Success, alpha), 2.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
		p.setPen(innerFocus);
		p.drawRoundedRect(r.adjusted(1, 1, -1, -1), _isPip ? 10 : 8, _isPip ? 10 : 8);

		p.restore();
	}
}

static std::pair<QColor, QColor> GenerateAvatarGradient(const QString &str) {
	uint32_t hash = 5381;
	QByteArray ba = str.toUtf8();
	for (char c : ba) {
		hash = ((hash << 5) + hash) + static_cast<uint8_t>(c);
	}
	static const std::vector<std::pair<QColor, QColor>> gradients = {
		{ QColor(0x3a, 0x7b, 0xd5), QColor(0x3a, 0x60, 0x73) }, // Sea Blue
		{ QColor(0x6a, 0x11, 0xcb), QColor(0x25, 0x75, 0xfc) }, // Purple-Blue
		{ QColor(0x11, 0x99, 0x8e), QColor(0x38, 0xef, 0x7d) }, // Emerald Green
		{ QColor(0xf1, 0x27, 0x11), QColor(0xf5, 0xaf, 0x19) }, // Sunset Amber
		{ QColor(0x8e, 0x2d, 0xe2), QColor(0x4a, 0x00, 0xe0) }, // Royal Purple
		{ QColor(0x00, 0xb4, 0xd8), QColor(0x00, 0x77, 0xb6) }, // Ocean Cyan
		{ QColor(0xe0, 0x56, 0xfd), QColor(0x68, 0x6d, 0xe0) }, // Magenta
		{ QColor(0xeb, 0x3b, 0x5a), QColor(0xfa, 0x82, 0x31) }, // Coral
	};
	return gradients[hash % gradients.size()];
}

void VideoTileWidget::drawAvatarPlaceholder(QPainter &p, const QRect &r, bool showName) {
	p.fillRect(r, themeColor(ColorRole::Tile));

	const int cx = r.center().x();
	const int cy = r.center().y();
	const int minDim = std::min(r.width(), r.height());
	const auto nameTag = bottomNameTag(r);
	const int outerRadius = nameTag.compact
		? std::clamp(minDim * 22 / 100, 8, 36)
		: std::clamp(minDim * 22 / 100, 28, 54);
	const int avatarY = nameTag.compact && !nameTag.rect.isEmpty()
		? r.y() + std::max(0, nameTag.rect.y() - r.y() - 6) / 2
		: cy - 14;

	// 正在说话时：在头像外围绘制随音量动态扩散的声波涟漪光环
	if (_isSpeaking && !_isAudioMuted) {
		p.save();
		const int auraRadius = outerRadius + static_cast<int>(std::clamp(_audioLevel, 0.1f, 1.0f) * 14.0f);
		p.setPen(QPen(themeColor(ColorRole::Success, 100), 2.0));
		p.setBrush(themeColor(ColorRole::Success, 35));
		p.drawEllipse(QPoint(cx, avatarY), auraRadius, auraRadius);
		p.restore();
	}

	// 质感色彩哈希渐变圆形头像
	p.save();
	auto [col1, col2] = GenerateAvatarGradient(_displayName.isEmpty() ? _identity : _displayName);
	QLinearGradient grad(cx - outerRadius, avatarY - outerRadius, cx + outerRadius, avatarY + outerRadius);
	grad.setColorAt(0.0, col1);
	grad.setColorAt(1.0, col2);

	p.setPen(Qt::NoPen);
	p.setBrush(grad);
	p.drawEllipse(QPoint(cx, avatarY), outerRadius, outerRadius);

	QString initial = _displayName.isEmpty() ? (_identity.isEmpty() ? "U" : _identity.left(1)) : _displayName.left(1);
	if (!_displayName.isEmpty()) {
		QString clean = _displayName;
		clean.remove(QCoreApplication::translate("MeetingUI", " (Me)"));
		clean.remove(" (Host)");
		if (!clean.isEmpty()) {
			initial = clean.left(1).toUpper();
		}
	}
	QFont avatarFont("Microsoft YaHei", outerRadius * 8 / 10, QFont::Bold);
	p.setFont(avatarFont);
	p.setPen(Qt::white);
	QRect avatarRect(cx - outerRadius, avatarY - outerRadius, outerRadius * 2, outerRadius * 2);
	p.drawText(avatarRect, Qt::AlignCenter, initial);
	p.restore();
	// Small avatar and video cards share the compact bottom tag.
	if (nameTag.compact || !showName) return;

	// 昵称与麦克风指示
	QFont font("Microsoft YaHei", std::clamp(minDim * 6 / 100, 9, 12), QFont::Bold);
	p.setFont(font);
	QFontMetrics fm(font);
	const auto label = fm.elidedText(_displayName, Qt::ElideRight, std::max(0, r.width() - 40));
	const int textW = fm.horizontalAdvance(label);
	const int totalW = textW + 24;
	const int startX = cx - totalW / 2;
	const int nameY = cy + outerRadius + 6;

	const int micX = startX + 6;
	const int micY = nameY + 6;
	p.setPen(QPen(_isAudioMuted ? themeColor(ColorRole::Danger) : themeColor(ColorRole::Success), 1.5, Qt::SolidLine, Qt::RoundCap));
	p.drawRoundedRect(QRect(micX - 3, micY - 5, 6, 8), 3, 3);
	p.drawLine(micX, micY + 3, micX, micY + 6);
	p.drawLine(micX - 4, micY + 6, micX + 4, micY + 6);
	if (_isAudioMuted) {
		p.drawLine(micX - 5, micY - 6, micX + 5, micY + 7);
	}

	p.setPen(themeColor(ColorRole::Text));
	p.drawText(QRect(startX + 18, nameY - 2, textW + 10, 20), Qt::AlignLeft | Qt::AlignVCenter, label);
}

void VideoTileWidget::drawNetworkQualityBadge(QPainter &p, const QRect &r) {
	p.save();
	const int bx = r.x() + 10;
	const int by = r.y() + 12;
	int bars = 0;
	QColor color = themeColor(ColorRole::TextMuted);
	switch (_connectionQuality) {
	case livekit::ConnectionQuality::Excellent:
		bars = 3;
		color = themeColor(ColorRole::Success);
		break;
	case livekit::ConnectionQuality::Good:
		bars = 2;
		color = themeColor(ColorRole::Success);
		break;
	case livekit::ConnectionQuality::Poor:
		bars = 1;
		color = themeColor(ColorRole::Warning);
		break;
	case livekit::ConnectionQuality::Lost:
		bars = 1;
		color = themeColor(ColorRole::Danger);
		break;
	case livekit::ConnectionQuality::Unknown:
		break;
	}

	p.setPen(Qt::NoPen);
	p.setBrush(color);
	if (bars >= 1) p.drawRect(bx, by + 6, 2, 4);
	if (bars >= 2) p.drawRect(bx + 4, by + 3, 2, 7);
	if (bars >= 3) p.drawRect(bx + 8, by, 2, 10);

	if (_isVideoStreamPaused) {
		const QRect pausedRect(bx + 16, by - 3, 54, 16);
		p.setBrush(themeColor(ColorRole::Warning, 220));
		p.drawRoundedRect(pausedRect, 5, 5);
		p.setPen(themeColor(ColorRole::OnAccent));
		p.setFont(QFont("Microsoft YaHei", 8, QFont::DemiBold));
		p.drawText(pausedRect, Qt::AlignCenter, QCoreApplication::translate("MeetingUI", "Paused: Network"));
	}

	if (_isPinned) {
		p.setFont(QFont("Segoe UI Emoji", 10));
		p.setPen(themeColor(ColorRole::Text));
		const int pinOffset = _isVideoStreamPaused ? 74 : 16;
		p.drawText(QRect(bx + pinOffset, by - 2, 16, 16), Qt::AlignCenter, QString::fromUtf8("📌"));
	}
	p.restore();
}

void VideoTileWidget::drawVideoPlaceholder(QPainter &p, const QRect &r) {
	using Error = livekit::TrackPublication::SubscriptionError;
	p.fillRect(r, themeColor(ColorRole::Tile));
	p.setPen(_isVideoStreamPaused || _videoSubscriptionError != Error::None
		? themeColor(ColorRole::Warning) : themeColor(ColorRole::TextMuted));
	p.setFont(QFont("Microsoft YaHei", 12));
	QString label;
	switch (_videoSubscriptionError) {
	case Error::CodecUnsupported:
		label = QCoreApplication::translate("MeetingUI", "Cannot receive this video format");
		break;
	case Error::TrackNotFound:
		label = QCoreApplication::translate("MeetingUI", "Video is no longer available");
		break;
	case Error::Unknown:
		label = QCoreApplication::translate("MeetingUI", "Video subscription failed");
		break;
	case Error::None:
		label = _isVideoStreamPaused
			? QCoreApplication::translate("MeetingUI", "Video paused due to network congestion")
			: QCoreApplication::translate("MeetingUI", "Waiting for video...");
		break;
	}
	p.drawText(r.adjusted(12, 0, -12, 0), Qt::AlignCenter,
		p.fontMetrics().elidedText(label, Qt::ElideRight, std::max(0, r.width() - 24)));
}

void VideoTileWidget::setRemoteInputEnabled(bool enabled) {
    _remoteInputEnabled = enabled;
    if (_pinBtn) _pinBtn->setVisible(underMouse());
    update();
}
QRectF VideoTileWidget::remoteControlContentRect() {
    std::lock_guard<std::mutex> lock(_frameMutex);
    if (!_isVideoActive || _isVideoStreamPaused || _currentFrame.isNull()) return {};
    const auto fitted = _currentFrame.size().scaled(size(),Qt::KeepAspectRatio);
    return QRectF((width()-fitted.width())/2,(height()-fitted.height())/2,fitted.width(),fitted.height());
}

void VideoTileWidget::drawVideoFrame(QPainter &p, const QRect &r) {

	QImage frameCopy;
	livekit::render::RenderFrameMetadata::Ptr renderFrame;
	{
		std::lock_guard<std::mutex> lock(_frameMutex);
		frameCopy = _currentFrame;
		renderFrame = _currentRenderFrame;
	}

	if (_isVideoStreamPaused || frameCopy.isNull()) {
		drawVideoPlaceholder(p, r);
		return;
	}

	if (!_hasLoggedFirstPaint) {
		_hasLoggedFirstPaint = true;
		LogToConsole(LogCategory::WebRTC, "PAINT_FRAME", QStringLiteral("first_frame image=%1x%2 viewport=%3x%4")
			.arg(frameCopy.width()).arg(frameCopy.height()).arg(r.width()).arg(r.height()));
	}

	const auto paintStartedAt = std::chrono::steady_clock::now();
	p.fillRect(r, themeColor(ColorRole::Letterbox));

	QImage scaled = frameCopy.scaled(r.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
	const int x = r.x() + (r.width() - scaled.width()) / 2;
	const int y = r.y() + (r.height() - scaled.height()) / 2;
	p.drawImage(x, y, scaled);
	if (renderFrame && p.isActive()) {
		renderFrame->NotifyRenderStage(
			"qt_cpu_paint",
			std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - paintStartedAt));
		renderFrame->NotifyRendered("qt_cpu_paint");
	}
}

VideoTileWidget::NameTagPresentation VideoTileWidget::bottomNameTag(const QRect &r) const {
	NameTagPresentation result;
	result.compact = r.width() < 320 || r.height() < 180;
	result.font = QFont("Microsoft YaHei", 10);
	if (result.compact) result.font.setPixelSize(12);
	const QFontMetrics fm(result.font);
	const int margin = result.compact ? (r.width() < 200 ? 4 : 6) : 12;
	const int height = std::max(result.compact ? 18 : 24, fm.height() + 2);
	const int padding = result.compact ? 22 : 30;
	const int available = std::max(0, r.width() - margin * 2);
	const int maximum = result.compact ? std::min(available, r.width() * 65 / 100) : available;
	if (maximum < padding || r.height() < height + margin * 2) return result;
	result.text = fm.elidedText(_displayName, Qt::ElideRight, maximum - padding);
	const int width = std::min(maximum, fm.horizontalAdvance(result.text) + padding);
	result.rect = QRect(r.x() + margin, r.y() + r.height() - margin - height, width, height);
	return result;
}

void VideoTileWidget::drawBottomNameTag(QPainter &p, const QRect &r) {
	const auto tag = bottomNameTag(r);
	if (tag.rect.isEmpty() || (!_isVideoActive && !tag.compact &&
		_videoSubscriptionError == livekit::TrackPublication::SubscriptionError::None)) return;
	const auto &tagRect = tag.rect;
	const bool darkTheme = themeColor(ColorRole::Text).lightness() > 128;
	p.save();
	p.setFont(tag.font);
	p.setPen(Qt::NoPen);
	// Reuse the theme's dark/bright pair so the translucent compact badge has
	// consistent contrast over video in Light, Dark, Teal and System themes.
	p.setBrush(tag.compact
		? themeColor(darkTheme ? ColorRole::Surface : ColorRole::Text, 180)
		: themeColor(ColorRole::Surface, 235));
	p.drawRoundedRect(tagRect, tag.compact ? 4 : 6, tag.compact ? 4 : 6);

	const int micX = tagRect.x() + (tag.compact ? 8 : 10);
	const int micY = tagRect.center().y();
	auto micColor = themeColor(_isAudioMuted ? ColorRole::Danger : ColorRole::Success);
	if (tag.compact && !darkTheme) micColor = micColor.lighter(170);
	p.setPen(QPen(micColor, tag.compact ? 1.2 : 1.4, Qt::SolidLine, Qt::RoundCap));
	const int micHalfWidth = tag.compact ? 2 : 3;
	p.drawRoundedRect(_isScreenShare
		? QRect(micX - (tag.compact ? 4 : 5), micY - 4, tag.compact ? 8 : 10, tag.compact ? 6 : 7)
		: QRect(micX - micHalfWidth, micY - 4, micHalfWidth * 2, tag.compact ? 6 : 7), 2, 2);
	p.drawLine(micX, micY + 2, micX, micY + 5);
	p.drawLine(micX - micHalfWidth, micY + 5, micX + micHalfWidth, micY + 5);
	if (_isAudioMuted && !_isScreenShare) {
		p.drawLine(micX - micHalfWidth - 1, micY - 5, micX + micHalfWidth + 1, micY + 5);
	}

	p.setPen(themeColor(tag.compact && !darkTheme ? ColorRole::Surface : ColorRole::Text));
	const int textOffset = tag.compact ? 17 : 20;
	p.drawText(tagRect.adjusted(textOffset, 0, -5, 0), Qt::AlignVCenter | Qt::AlignLeft, tag.text);
	p.restore();
}

void VideoTileWidget::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		emit tileClicked();
	}
	Ui::RpWidget::mousePressEvent(e);
}

void VideoTileWidget::mouseDoubleClickEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		emit tileDoubleClicked();
	}
	Ui::RpWidget::mouseDoubleClickEvent(e);
}

// ----------------------------------------------------
// RoomTopBarWidget 实现
// ----------------------------------------------------


} // namespace MeetingUI
