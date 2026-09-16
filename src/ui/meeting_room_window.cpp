#include "src/ui/meeting_room_window.h"
#include "src/ui/meeting_log_console.h"
#include "src/media/media_converters.h"
#include "libyuv/convert_argb.h"
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QApplication>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QFont>
#include <QtGui/QClipboard>
#include <QtGui/QWindow>
#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <cmath>

#if defined(Q_OS_WIN)
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#endif

namespace MeetingUI {

// ----------------------------------------------------
// 视频帧格式转换工具 (VideoFrame -> QImage)
// ----------------------------------------------------
static QImage VideoFrameToQImage(const livekit::VideoFrame &frame) {
	const int w = frame.width();
	const int h = frame.height();
	if (w <= 0 || h <= 0 || !frame.data()) return QImage();

	if (frame.type() == livekit::VideoBufferType::RGBA) {
		return QImage(frame.data(), w, h, w * 4, QImage::Format_RGBA8888).copy();
	} else if (frame.type() == livekit::VideoBufferType::ARGB || frame.type() == livekit::VideoBufferType::BGRA) {
		return QImage(frame.data(), w, h, w * 4, QImage::Format_ARGB32).copy();
	} else if (frame.type() == livekit::VideoBufferType::RGB24) {
		std::vector<uint8_t> rgba(w * h * 4);
		livekit::MediaConverters::ConvertRGB24ToRGBA(frame.data(), rgba.data(), w, h);
		return QImage(rgba.data(), w, h, w * 4, QImage::Format_RGBA8888).copy();
	} else if (frame.type() == livekit::VideoBufferType::NV12) {
		QImage image(w, h, QImage::Format_ARGB32);
		if (image.isNull()) return image;
		const int chroma_width = (w + 1) / 2;
		const uint8_t *y_plane = frame.data();
		const uint8_t *uv_plane = y_plane + (w * h);
		return libyuv::NV12ToARGB(y_plane, w, uv_plane, chroma_width * 2,
			image.bits(), image.bytesPerLine(), w, h) == 0 ? image : QImage();
	} else if (frame.type() == livekit::VideoBufferType::I420 ||
	           frame.type() == livekit::VideoBufferType::I420A) {
		QImage image(w, h, QImage::Format_ARGB32);
		if (image.isNull()) return image;
		const int chroma_width = (w + 1) / 2;
		const int chroma_height = (h + 1) / 2;
		const uint8_t *y_plane = frame.data();
		const uint8_t *u_plane = y_plane + (w * h);
		const uint8_t *v_plane = u_plane + (chroma_width * chroma_height);
		return libyuv::I420ToARGB(y_plane, w, u_plane, chroma_width, v_plane, chroma_width,
			image.bits(), image.bytesPerLine(), w, h) == 0 ? image : QImage();
	}
	return QImage();
}

// ----------------------------------------------------
// VideoTileWidget 实现
// ----------------------------------------------------

VideoTileWidget::VideoTileWidget(const QString &displayName, bool isLocal, QWidget *parent)
	: Ui::RpWidget(parent)
	, _displayName(displayName)
	, _isLocal(isLocal) {
	setMouseTracking(true);
	setAttribute(Qt::WA_OpaquePaintEvent, false);

	_visualizer = new AudioVisualizerWidget(this, 7);
	_visualizer->setBarColor(QColor(0, 180, 42));
	_visualizer->hide();

	setupVolumeControls();
}

void VideoTileWidget::setupVolumeControls() {
	_pinBtn = new QPushButton(QString::fromUtf8("📌"), this);
	_pinBtn->setFixedSize(28, 28);
	_pinBtn->setToolTip(QString::fromUtf8("钉住此画面 (Pin) / 取消钉住"));
	_pinBtn->setStyleSheet(R"(
		QPushButton {
			background-color: rgba(0, 0, 0, 150);
			border-radius: 14px;
			color: white;
			border: none;
			font-size: 13px;
		}
		QPushButton:hover {
			background-color: rgba(22, 119, 255, 220);
		}
	)");
	_pinBtn->hide();

	connect(_pinBtn, &QPushButton::clicked, [this] {
		_isPinned = !_isPinned;
		_pinBtn->setStyleSheet(_isPinned ?
			"background-color: #1677ff; border-radius: 14px; color: white; border: none; font-size: 13px;" :
			"background-color: rgba(0, 0, 0, 150); border-radius: 14px; color: white; border: none; font-size: 13px;");
		emit pinToggled(_isPinned);
		update();
	});

	if (_isLocal) return;

	_volBtn = new QPushButton(QString::fromUtf8("🔊"), this);
	_volBtn->setFixedSize(28, 28);
	_volBtn->setToolTip(QString::fromUtf8("独立调节该参会人音量"));
	_volBtn->setStyleSheet(R"(
		QPushButton {
			background-color: rgba(0, 0, 0, 150);
			border-radius: 14px;
			color: white;
			border: none;
			font-size: 13px;
		}
		QPushButton:hover {
			background-color: rgba(22, 119, 255, 220);
		}
	)");
	_volBtn->hide();

	_volPopup = new QWidget(this);
	_volPopup->setFixedSize(190, 44);
	_volPopup->setStyleSheet(R"(
		QWidget {
			background-color: rgba(20, 24, 32, 230);
			border-radius: 8px;
			border: 1px solid rgba(255, 255, 255, 40);
		}
	)");
	_volPopup->hide();

	_muteRemoteBtn = new QPushButton(QString::fromUtf8("🔊"), _volPopup);
	_muteRemoteBtn->setFixedSize(26, 26);
	_muteRemoteBtn->setGeometry(8, 9, 26, 26);
	_muteRemoteBtn->setStyleSheet(R"(
		QPushButton {
			background: transparent;
			color: #ffffff;
			border: none;
			font-size: 13px;
		}
		QPushButton:hover {
			color: #1677ff;
		}
	)");

	_volSlider = new QSlider(Qt::Horizontal, _volPopup);
	_volSlider->setRange(0, 200);
	_volSlider->setValue(100);
	_volSlider->setGeometry(38, 12, 100, 20);
	_volSlider->setStyleSheet(R"(
		QSlider::groove:horizontal {
			height: 4px;
			background: rgba(255, 255, 255, 60);
			border-radius: 2px;
		}
		QSlider::sub-page:horizontal {
			background: #1677ff;
			border-radius: 2px;
		}
		QSlider::handle:horizontal {
			background: #ffffff;
			width: 12px;
			margin: -4px 0;
			border-radius: 6px;
		}
	)");

	_volLabel = new QLabel(QString::fromUtf8("100%"), _volPopup);
	_volLabel->setGeometry(142, 11, 40, 22);
	_volLabel->setStyleSheet("color: #ffffff; font-size: 11px; border: none; background: transparent;");

	connect(_volBtn, &QPushButton::clicked, [this] {
		if (_volPopup->isVisible()) {
			_volPopup->hide();
		} else {
			_volPopup->show();
			_volPopup->raise();
		}
	});

	connect(_muteRemoteBtn, &QPushButton::clicked, [this] {
		_isLocallyMuted = !_isLocallyMuted;
		_muteRemoteBtn->setText(_isLocallyMuted ? QString::fromUtf8("🔇") : QString::fromUtf8("🔊"));
		_muteRemoteBtn->setStyleSheet(_isLocallyMuted ? "color: #f53f3f; border: none; font-size: 13px;" : "color: #ffffff; border: none; font-size: 13px;");
		_volBtn->setText(_isLocallyMuted ? QString::fromUtf8("🔇") : QString::fromUtf8("🔊"));
		remoteLocalMuteToggled(_isLocallyMuted);
	});

	connect(_volSlider, &QSlider::valueChanged, [this](int value) {
		_remoteVolume = static_cast<float>(value) / 100.0f;
		_volLabel->setText(QString("%1%").arg(value));
		if (_isLocallyMuted && value > 0) {
			_isLocallyMuted = false;
			_muteRemoteBtn->setText(QString::fromUtf8("🔊"));
			_muteRemoteBtn->setStyleSheet("color: #ffffff; border: none; font-size: 13px;");
			_volBtn->setText(QString::fromUtf8("🔊"));
			remoteLocalMuteToggled(false);
		}
		remoteVolumeChanged(_remoteVolume);
	});
}

void VideoTileWidget::enterEventHook(QEnterEvent *e) {
	if (_pinBtn) _pinBtn->show();
	if (_volBtn) _volBtn->show();
	Ui::RpWidget::enterEventHook(e);
}

void VideoTileWidget::leaveEventHook(QEvent *e) {
	if (_pinBtn && !_isPinned) _pinBtn->hide();
	if (_volBtn && (!_volPopup || !_volPopup->isVisible())) {
		_volBtn->hide();
	}
	Ui::RpWidget::leaveEventHook(e);
}

void VideoTileWidget::resizeEvent(QResizeEvent *e) {
	Ui::RpWidget::resizeEvent(e);
	const int w = width();
	const int h = height();

	if (_pinBtn) {
		_pinBtn->move(_volBtn ? (w - 70) : (w - 36), 10);
	}
	if (_volBtn) {
		_volBtn->move(w - 36, 10);
	}
	if (_volPopup) {
		_volPopup->move(w - 200, 42);
	}
	if (_visualizer) {
		_visualizer->setGeometry((w - 90) / 2, h - 36, 90, 24);
		_visualizer->raise();
	}
}

void VideoTileWidget::setDisplayName(const QString &name) {
	_displayName = name;
	update();
}

void VideoTileWidget::setVideoActive(bool active) {
	_isVideoActive = active;
	if (!active) {
		std::lock_guard<std::mutex> lock(_frameMutex);
		_currentFrame = QImage();
	}
	update();
}

void VideoTileWidget::setAudioMuted(bool muted) {
	_isAudioMuted = muted;
	if (muted) {
		_isSpeaking = false;
		_audioLevel = 0.0f;
		if (_visualizer) {
			_visualizer->setActive(false);
			_visualizer->hide();
		}
	}
	update();
}

void VideoTileWidget::setConnectionQuality(livekit::ConnectionQuality quality) {
	if (_connectionQuality == quality) return;
	_connectionQuality = quality;
	update();
}

void VideoTileWidget::setVideoStreamPaused(bool paused) {
	if (_isVideoStreamPaused == paused) return;
	_isVideoStreamPaused = paused;
	update();
}

void VideoTileWidget::setSpeaking(bool speaking, float level) {
	_isSpeaking = speaking;
	_audioLevel = level;
	if (_visualizer) {
		_visualizer->setActive(speaking);
		if (speaking) {
			_visualizer->setAudioLevel(level);
			_visualizer->show();
			_visualizer->raise();
		} else {
			_visualizer->hide();
		}
	}
	update();
}

void VideoTileWidget::setFrame(const QImage &image) {
	{
		std::lock_guard<std::mutex> lock(_frameMutex);
		_currentFrame = image;
	}
	update();
}

void VideoTileWidget::paintEvent(QPaintEvent *e) {
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	p.setRenderHint(QPainter::TextAntialiasing);

	const QRect r = rect();

	if (_isPip) {
		QPainterPath path;
		path.addRoundedRect(r.adjusted(2, 2, -2, -2), 10, 10);
		p.setClipPath(path);
		p.fillPath(path, QColor(0x1a, 0x1d, 0x24));
	}

	if (_isVideoActive) {
		drawVideoFrame(p, r);
	} else {
		drawAvatarPlaceholder(p, r);
	}

	drawBottomNameTag(p, r);
	drawNetworkQualityBadge(p, r);

	// 画中画模式下的基础边框
	if (_isPip) {
		p.setClipping(false);
		p.setPen(QPen(_isSpeaking ? QColor(0x00, 0xb4, 0x2a) : QColor(0x86, 0x90, 0x9c), _isSpeaking ? 3.0 : 2.0));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(r.adjusted(1, 1, -1, -1), 10, 10);
	}

	// 说话中：绘制高质感双层绿色呼吸发光光圈 (Active Speaker Halo)
	if (_isSpeaking && !_isAudioMuted) {
		p.save();
		p.setClipping(false);
		p.setBrush(Qt::NoBrush);

		int alpha = static_cast<int>(60 + 150 * std::clamp(_audioLevel * 4.0f, 0.1f, 1.0f));
		QPen outerGlow(QColor(0, 180, 42, alpha / 3), 6.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
		p.setPen(outerGlow);
		p.drawRoundedRect(r.adjusted(3, 3, -3, -3), _isPip ? 10 : 8, _isPip ? 10 : 8);

		QPen innerFocus(QColor(0, 180, 42, alpha), 2.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
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

void VideoTileWidget::drawAvatarPlaceholder(QPainter &p, const QRect &r) {
	p.fillRect(r, QColor(0x18, 0x1a, 0x22));

	const int cx = r.center().x();
	const int cy = r.center().y();
	const int minDim = std::min(r.width(), r.height());
	const int outerRadius = std::clamp(minDim * 22 / 100, 28, 54);

	// 正在说话时：在头像外围绘制随音量动态扩散的声波涟漪光环
	if (_isSpeaking && !_isAudioMuted) {
		p.save();
		const int auraRadius = outerRadius + static_cast<int>(std::clamp(_audioLevel, 0.1f, 1.0f) * 14.0f);
		p.setPen(QPen(QColor(0x00, 0xb4, 0x2a, 100), 2.0));
		p.setBrush(QColor(0x00, 0xb4, 0x2a, 35));
		p.drawEllipse(QPoint(cx, cy - 14), auraRadius, auraRadius);
		p.restore();
	}

	// 质感色彩哈希渐变圆形头像
	p.save();
	auto [col1, col2] = GenerateAvatarGradient(_displayName.isEmpty() ? _identity : _displayName);
	QLinearGradient grad(cx - outerRadius, cy - 14 - outerRadius, cx + outerRadius, cy - 14 + outerRadius);
	grad.setColorAt(0.0, col1);
	grad.setColorAt(1.0, col2);

	p.setPen(Qt::NoPen);
	p.setBrush(grad);
	p.drawEllipse(QPoint(cx, cy - 14), outerRadius, outerRadius);

	QString initial = _displayName.isEmpty() ? (_identity.isEmpty() ? "U" : _identity.left(1)) : _displayName.left(1);
	if (!_displayName.isEmpty()) {
		QString clean = _displayName;
		clean.remove(" (我)");
		clean.remove(" (Host)");
		if (!clean.isEmpty()) {
			initial = clean.left(1).toUpper();
		}
	}
	QFont avatarFont("Microsoft YaHei", outerRadius * 8 / 10, QFont::Bold);
	p.setFont(avatarFont);
	p.setPen(Qt::white);
	QRect avatarRect(cx - outerRadius, cy - 14 - outerRadius, outerRadius * 2, outerRadius * 2);
	p.drawText(avatarRect, Qt::AlignCenter, initial);
	p.restore();

	// 昵称与麦克风指示
	QFont font("Microsoft YaHei", std::clamp(minDim * 6 / 100, 9, 12), QFont::Bold);
	p.setFont(font);
	QFontMetrics fm(font);
	const int textW = fm.horizontalAdvance(_displayName);
	const int totalW = textW + 24;
	const int startX = cx - totalW / 2;
	const int nameY = cy + outerRadius + 6;

	const int micX = startX + 6;
	const int micY = nameY + 6;
	p.setPen(QPen(_isAudioMuted ? QColor(0xf5, 0x3f, 0x3f) : QColor(0x00, 0xb4, 0x2a), 1.5, Qt::SolidLine, Qt::RoundCap));
	p.drawRoundedRect(QRect(micX - 3, micY - 5, 6, 8), 3, 3);
	p.drawLine(micX, micY + 3, micX, micY + 6);
	p.drawLine(micX - 4, micY + 6, micX + 4, micY + 6);
	if (_isAudioMuted) {
		p.drawLine(micX - 5, micY - 6, micX + 5, micY + 7);
	}

	p.setPen(QColor(0xf0, 0xf2, 0xf5));
	p.drawText(QRect(startX + 18, nameY - 2, textW + 10, 20), Qt::AlignLeft | Qt::AlignVCenter, _displayName);
}

void VideoTileWidget::drawNetworkQualityBadge(QPainter &p, const QRect &r) {
	p.save();
	const int bx = r.x() + 10;
	const int by = r.y() + 12;
	int bars = 0;
	QColor color(0x86, 0x90, 0x9c);
	switch (_connectionQuality) {
	case livekit::ConnectionQuality::Excellent:
		bars = 3;
		color = QColor(0x00, 0xb4, 0x2a);
		break;
	case livekit::ConnectionQuality::Good:
		bars = 2;
		color = QColor(0x52, 0xc4, 0x1a);
		break;
	case livekit::ConnectionQuality::Poor:
		bars = 1;
		color = QColor(0xe6, 0x7e, 0x22);
		break;
	case livekit::ConnectionQuality::Lost:
		bars = 1;
		color = QColor(0xf5, 0x3f, 0x3f);
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
		p.setBrush(QColor(0xe6, 0x7e, 0x22, 220));
		p.drawRoundedRect(pausedRect, 5, 5);
		p.setPen(Qt::white);
		p.setFont(QFont("Microsoft YaHei", 8, QFont::DemiBold));
		p.drawText(pausedRect, Qt::AlignCenter, QString::fromUtf8("网络暂停"));
	}

	if (_isPinned) {
		p.setFont(QFont("Segoe UI Emoji", 10));
		p.setPen(Qt::white);
		const int pinOffset = _isVideoStreamPaused ? 74 : 16;
		p.drawText(QRect(bx + pinOffset, by - 2, 16, 16), Qt::AlignCenter, QString::fromUtf8("📌"));
	}
	p.restore();
}

void VideoTileWidget::drawVideoFrame(QPainter &p, const QRect &r) {
	if (_isVideoStreamPaused) {
		p.fillRect(r, QColor(0x14, 0x16, 0x1d));
		p.setPen(QColor(0xe6, 0x7e, 0x22));
		p.setFont(QFont("Microsoft YaHei", 12));
		p.drawText(r, Qt::AlignCenter, QString::fromUtf8("视频流因网络拥塞暂停"));
		return;
	}

	QImage frameCopy;
	{
		std::lock_guard<std::mutex> lock(_frameMutex);
		frameCopy = _currentFrame;
	}

	if (frameCopy.isNull()) {
		p.fillRect(r, QColor(0x14, 0x16, 0x1d));
		p.setPen(QColor(0x86, 0x90, 0x9c));
		p.setFont(QFont("Microsoft YaHei", 12));
		p.drawText(r, Qt::AlignCenter, QString::fromUtf8("正在等待视频画面..."));
		return;
	}

	if (!_hasLoggedFirstPaint) {
		_hasLoggedFirstPaint = true;
		LogToConsole(LogCategory::WebRTC, "PAINT_FRAME", QString("VideoTileWidget [%1] 画面成功上屏绘制 (图像: %2x%3, 视口: %4x%5)")
			.arg(_displayName).arg(frameCopy.width()).arg(frameCopy.height()).arg(r.width()).arg(r.height()));
	}

	p.fillRect(r, QColor(0x0e, 0x10, 0x14));

	QImage scaled = frameCopy.scaled(r.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
	const int x = r.x() + (r.width() - scaled.width()) / 2;
	const int y = r.y() + (r.height() - scaled.height()) / 2;
	p.drawImage(x, y, scaled);
}

void VideoTileWidget::drawBottomNameTag(QPainter &p, const QRect &r) {
	if (!_isVideoActive) return;

	const int tagH = 24;
	const int margin = 12;

	QFont font("Microsoft YaHei", 10);
	p.setFont(font);
	QFontMetrics fm(font);
	const int textW = fm.horizontalAdvance(_displayName);
	const int tagW = textW + 30;

	QRect tagRect(r.x() + margin, r.bottom() - margin - tagH, tagW, tagH);

	p.save();
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0, 0, 0, 160));
	p.drawRoundedRect(tagRect, 6, 6);

	const int micX = tagRect.x() + 10;
	const int micY = tagRect.center().y();
	p.setPen(QPen(_isAudioMuted ? QColor(0xf5, 0x3f, 0x3f) : QColor(0x00, 0xb4, 0x2a), 1.4, Qt::SolidLine, Qt::RoundCap));
	p.drawRoundedRect(QRect(micX - 3, micY - 4, 6, 7), 2, 2);
	p.drawLine(micX, micY + 3, micX, micY + 5);
	p.drawLine(micX - 3, micY + 5, micX + 3, micY + 5);
	if (_isAudioMuted) {
		p.drawLine(micX - 4, micY - 5, micX + 4, micY + 6);
	}

	p.setPen(Qt::white);
	p.drawText(QRect(tagRect.x() + 20, tagRect.y(), textW + 6, tagH), Qt::AlignVCenter | Qt::AlignLeft, _displayName);
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

RoomTopBarWidget::RoomTopBarWidget(QWidget *parent)
	: Ui::RpWidget(parent) {
	setFixedHeight(44);
	setMouseTracking(true);
	setAttribute(Qt::WA_OpaquePaintEvent, false);
}

void RoomTopBarWidget::updateDuration(int seconds) {
	_durationSeconds = seconds;
	update();
}

void RoomTopBarWidget::setActiveSpeaker(const QString &speakerName) {
	_speakerName = speakerName;
	update();
}

void RoomTopBarWidget::setMeetingId(const QString &meetingId) {
	_meetingId = meetingId;
	QResizeEvent ev(size(), size());
	resizeEvent(&ev);
	update();
}

void RoomTopBarWidget::resizeEvent(QResizeEvent *e) {
	const int w = width();
	const int h = height();
	const int btnW = 38;

	_closeRect = QRect(w - btnW, 0, btnW, h);
	_maxRect = QRect(w - btnW * 2, 0, btnW, h);
	_minRect = QRect(w - btnW * 3, 0, btnW, h);

	int rightX = w - btnW * 3 - 6;

	_fullscreenRect = QRect(rightX - 26, 8, 26, 28);
	rightX -= 30;

	_settingsRect = QRect(rightX - 52, 8, 52, 28);
	rightX -= 56;

	_simulateRect = QRect(rightX - 58, 8, 58, 28);
	rightX -= 62;

	_consoleRect = QRect(rightX - 62, 8, 62, 28);
	rightX -= 66;

	_hostToolsRect = QRect(rightX - 78, 8, 78, 28);
	rightX -= 82;

	_layoutRect = QRect(rightX - 74, 8, 74, 28);
	rightX -= 78;

	const int leftInfoRight = 196;
	if (!_meetingId.isEmpty()) {
		_meetingIdRect = QRect(leftInfoRight, (h - 26) / 2, 178, 26);
	} else {
		_meetingIdRect = QRect();
	}

	const int leftBoundary = _meetingId.isEmpty() ? leftInfoRight : (leftInfoRight + 188);
	const int rightButtonsLeft = rightX;
	const int availCenterW = rightButtonsLeft - leftBoundary - 16;

	if (availCenterW >= 180) {
		const int pillW = std::min(availCenterW, 200);
		int pillX = (w - pillW) / 2;
		pillX = std::max(leftBoundary + 8, std::min(pillX, rightButtonsLeft - 8 - pillW));
		_speakerCapsuleRect = QRect(pillX, (h - 26) / 2, pillW, 26);
	} else if (availCenterW >= 110) {
		const int pillW = availCenterW;
		const int pillX = leftBoundary + 8;
		_speakerCapsuleRect = QRect(pillX, (h - 26) / 2, pillW, 26);
	} else {
		_speakerCapsuleRect = QRect(); // 窗口空间不足时自动隐藏，彻底杜绝元素重叠
	}
}

void RoomTopBarWidget::paintEvent(QPaintEvent *e) {
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::TextAntialiasing);

	const int w = width();
	const int h = height();

	p.fillRect(rect(), QColor(0xfd, 0xfd, 0xfe));
	p.setPen(QColor(0xeb, 0xed, 0xf0));
	p.drawLine(0, h - 1, w, h - 1);

	// 1. 左侧：Logo、会议名称与持续时间
	p.save();
	const int logoX = 14;
	const int logoY = h / 2;
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0x16, 0x77, 0xff));
	p.drawEllipse(QPoint(logoX, logoY), 5, 5);
	p.drawEllipse(QPoint(logoX + 7, logoY - 4), 4, 4);

	QFont font("Microsoft YaHei", 10);
	p.setFont(font);
	p.setPen(QColor(0x4e, 0x59, 0x69));
	p.drawText(QRect(logoX + 16, 0, 70, h), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8("会议"));

	const int minutes = _durationSeconds / 60;
	const int secs = _durationSeconds % 60;
	const QString timeStr = QString("%1:%2")
		.arg(minutes, 2, 10, QChar('0'))
		.arg(secs, 2, 10, QChar('0'));
	
	QFont timeFont("Microsoft YaHei", 10, QFont::DemiBold);
	p.setFont(timeFont);
	p.setPen(QColor(0x1f, 0x23, 0x29));
	p.drawText(QRect(logoX + 90, 0, 48, h), Qt::AlignVCenter | Qt::AlignLeft, timeStr);

	const int sigX = logoX + 144;
	const int sigY = h / 2 + 3;
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0x00, 0xb4, 0x2a));
	p.drawRect(sigX, sigY - 4, 2, 4);
	p.drawRect(sigX + 4, sigY - 7, 2, 7);
	p.drawRect(sigX + 8, sigY - 10, 2, 10);

	const int shieldX = sigX + 22;
	p.setPen(QPen(QColor(0x86, 0x90, 0x9c), 1.3));
	p.setBrush(Qt::NoBrush);
	QPainterPath shieldPath;
	shieldPath.moveTo(shieldX, logoY - 5);
	shieldPath.lineTo(shieldX + 8, logoY - 5);
	shieldPath.lineTo(shieldX + 8, logoY);
	shieldPath.quadTo(shieldX + 4, logoY + 7, shieldX + 4, logoY + 7);
	shieldPath.quadTo(shieldX, logoY + 7, shieldX, logoY);
	shieldPath.closeSubpath();
	p.drawPath(shieldPath);
	p.restore();

	// 1.5 会议号胶囊徽标与点击复制
	if (!_meetingIdRect.isEmpty() && !_meetingId.isEmpty()) {
		p.save();
		bool isHover = _meetingIdRect.contains(mapFromGlobal(QCursor::pos()));
		p.setPen(QPen(_copiedAnim ? QColor(0x52, 0xc4, 0x1a) : (isHover ? QColor(0x16, 0x77, 0xff) : QColor(0xd9, 0xd9, 0xd9)), 1));
		p.setBrush(_copiedAnim ? QColor(0xf6, 0xff, 0xed) : (isHover ? QColor(0xf0, 0xf5, 0xff) : QColor(0xf5, 0xf7, 0xfa)));
		p.drawRoundedRect(_meetingIdRect, 6, 6);

		QFont mFont("Microsoft YaHei", 9);
		mFont.setBold(true);
		p.setFont(mFont);
		p.setPen(_copiedAnim ? QColor(0x52, 0xc4, 0x1a) : (isHover ? QColor(0x16, 0x77, 0xff) : QColor(0x4e, 0x59, 0x69)));
		QString dispText = _copiedAnim ? QString::fromUtf8("✔ 已复制会议号") : QString::fromUtf8("🆔 会议号: %1 📋").arg(_meetingId);
		p.drawText(_meetingIdRect, Qt::AlignCenter, dispText);
		p.restore();
	}

	// 2. 中间：正在讲话提示胶囊
	if (!_speakerCapsuleRect.isEmpty()) {
		p.save();
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0xe8, 0xf3, 0xff));
		p.drawRoundedRect(_speakerCapsuleRect, 6, 6);

		QFont speakerFont("Microsoft YaHei", 9);
		p.setFont(speakerFont);
		p.setPen(QColor(0x16, 0x77, 0xff));
		QString speakerText = _speakerName.isEmpty() ? QString::fromUtf8("正在讲话: 无") : QString::fromUtf8("正在讲话: %1").arg(_speakerName);
		QFontMetrics fm(speakerFont);
		QString elided = fm.elidedText(speakerText, Qt::ElideMiddle, _speakerCapsuleRect.width() - 12);
		p.drawText(_speakerCapsuleRect, Qt::AlignCenter, elided);
		p.restore();
	}

	// 3. 右侧工具按钮
	auto drawTextBtn = [&](const QRect &r, const QString &text, bool hovered, bool hasArrow = false, const QColor &customColor = QColor(0x4e, 0x59, 0x69)) {
		p.save();
		if (hovered) {
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(0xf2, 0xf3, 0xf5));
			p.drawRoundedRect(r, 6, 6);
		}
		p.setFont(QFont("Microsoft YaHei", 9));
		p.setPen(customColor);
		if (hasArrow) {
			p.drawText(r.adjusted(4, 0, -12, 0), Qt::AlignCenter, text);
			p.setPen(QPen(QColor(0x86, 0x90, 0x9c), 1.3));
			const int ax = r.right() - 10;
			const int ay = r.center().y();
			p.drawLine(ax - 3, ay - 1, ax, ay + 2);
			p.drawLine(ax, ay + 2, ax + 3, ay - 1);
		} else {
			p.drawText(r, Qt::AlignCenter, text);
		}
		p.restore();
	};

	QString layoutStr = (_currentViewMode == VideoViewMode::Grid) ? QString::fromUtf8("宫格布局") : QString::fromUtf8("画中画");
	drawTextBtn(_layoutRect, layoutStr, _hoverBtn == HoverBtn::Layout, true);
	drawTextBtn(_hostToolsRect, QString::fromUtf8("主持人工具"), _hoverBtn == HoverBtn::HostTools, true);
	drawTextBtn(_consoleRect, QString::fromUtf8("控制台 📋"), _hoverBtn == HoverBtn::Console, false, QColor(0x16, 0x77, 0xff));
	drawTextBtn(_simulateRect, QString::fromUtf8("🐛 模拟"), _hoverBtn == HoverBtn::Simulate, false, QColor(0xe6, 0x7e, 0x22));
	drawTextBtn(_settingsRect, QString::fromUtf8("设置 ⚙"), _hoverBtn == HoverBtn::Settings, false);

	p.save();
	if (_hoverBtn == HoverBtn::Fullscreen) {
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0xf2, 0xf3, 0xf5));
		p.drawRoundedRect(_fullscreenRect, 6, 6);
	}
	p.setPen(QPen(QColor(0x4e, 0x59, 0x69), 1.3));
	const int fx = _fullscreenRect.center().x();
	const int fy = _fullscreenRect.center().y();
	p.drawRect(fx - 5, fy - 5, 10, 10);
	p.restore();

	// 4. 窗口控制按钮
	p.save();
	if (_hoverBtn == HoverBtn::Min) p.fillRect(_minRect, QColor(0xe5, 0xe8, 0xef));
	if (_hoverBtn == HoverBtn::Max) p.fillRect(_maxRect, QColor(0xe5, 0xe8, 0xef));
	if (_hoverBtn == HoverBtn::Close) p.fillRect(_closeRect, QColor(0xf5, 0x3f, 0x3f));

	p.setPen(QPen((_hoverBtn == HoverBtn::Close) ? Qt::white : QColor(0x60, 0x62, 0x66), 1.2));
	p.drawLine(_minRect.center().x() - 5, _minRect.center().y(), _minRect.center().x() + 5, _minRect.center().y());
	p.drawRect(_maxRect.center().x() - 5, _maxRect.center().y() - 5, 10, 10);
	p.drawLine(_closeRect.center().x() - 5, _closeRect.center().y() - 5, _closeRect.center().x() + 5, _closeRect.center().y() + 5);
	p.drawLine(_closeRect.center().x() + 5, _closeRect.center().y() - 5, _closeRect.center().x() - 5, _closeRect.center().y() + 5);
	p.restore();
}

void RoomTopBarWidget::mouseMoveEvent(QMouseEvent *e) {
	const QPoint pos = e->pos();
	HoverBtn next = HoverBtn::None;

	if (_closeRect.contains(pos)) next = HoverBtn::Close;
	else if (_maxRect.contains(pos)) next = HoverBtn::Max;
	else if (_minRect.contains(pos)) next = HoverBtn::Min;
	else if (_fullscreenRect.contains(pos)) next = HoverBtn::Fullscreen;
	else if (_settingsRect.contains(pos)) next = HoverBtn::Settings;
	else if (_simulateRect.contains(pos)) next = HoverBtn::Simulate;
	else if (_consoleRect.contains(pos)) next = HoverBtn::Console;
	else if (_hostToolsRect.contains(pos)) next = HoverBtn::HostTools;
	else if (_layoutRect.contains(pos)) next = HoverBtn::Layout;

	if (!_meetingIdRect.isEmpty() && _meetingIdRect.contains(pos)) {
		setCursor(Qt::PointingHandCursor);
		setToolTip(QString::fromUtf8("点击复制会议号: %1").arg(_meetingId));
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
		if (_minRect.contains(e->pos())) {
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
			_currentViewMode = (_currentViewMode == VideoViewMode::Grid) ? VideoViewMode::Pip : VideoViewMode::Grid;
			_viewModeStream.fire_copy(_currentViewMode);
			update();
		} else if (_settingsRect.contains(e->pos())) {
			_settingsStream.fire({});
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
	menu.setStyleSheet(R"(
		QMenu {
			background-color: #1a1a1f;
			border: 1px solid #2e2e38;
			border-radius: 8px;
			padding: 8px 4px;
			font-family: "Segoe UI", "Microsoft YaHei";
			color: #e4e4e8;
		}
		QMenu::item {
			padding: 7px 28px 7px 18px;
			border-radius: 6px;
			font-size: 13px;
			font-weight: 500;
			color: #e4e4e8;
		}
		QMenu::item:selected {
			background-color: #2b2b36;
			color: #ffffff;
		}
		QMenu::item:disabled {
			color: #8c8c9a;
			font-size: 14px;
			font-weight: bold;
			padding: 8px 18px 6px 18px;
		}
		QMenu::separator {
			height: 1px;
			background-color: #2e2e38;
			margin: 4px 8px;
		}
	)");

	QAction *header = menu.addAction(QString::fromUtf8("Simulate Scenario"));
	header->setEnabled(false);
	menu.addSeparator();

	struct ScenarioEntry {
		QString name;
		livekit::SimulateScenarioType type;
	};

	const std::vector<ScenarioEntry> entries = {
		{ "signalReconnect", livekit::SimulateScenarioType::SignalReconnect },
		{ "fullReconnect", livekit::SimulateScenarioType::FullReconnect },
		{ "speakerUpdate", livekit::SimulateScenarioType::SpeakerUpdate },
		{ "nodeFailure", livekit::SimulateScenarioType::NodeFailure },
		{ "migration", livekit::SimulateScenarioType::Migration },
		{ "serverLeave", livekit::SimulateScenarioType::ServerLeave },
		{ "switchCandidate", livekit::SimulateScenarioType::SwitchCandidate },
		{ "e2eeKeyRatchet", livekit::SimulateScenarioType::E2eeKeyRatchet },
		{ "participantName", livekit::SimulateScenarioType::ParticipantName },
		{ "participantMetadata", livekit::SimulateScenarioType::ParticipantMetadata },
		{ "clear", livekit::SimulateScenarioType::Clear }
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

RoomBottomBarWidget::RoomBottomBarWidget(QWidget *parent)
	: Ui::RpWidget(parent) {
	setFixedHeight(76);
	setMouseTracking(true);
	setAttribute(Qt::WA_OpaquePaintEvent, false);

	_chatInput = new QLineEdit(this);
	_chatInput->setPlaceholderText(QString::fromUtf8("说点什么..."));
	_chatInput->setStyleSheet(R"(
		QLineEdit {
			background-color: #f2f3f5;
			border-radius: 16px;
			border: 1px solid transparent;
			padding: 4px 14px;
			font-size: 12px;
			color: #1f2329;
		}
		QLineEdit:focus {
			background-color: #ffffff;
			border: 1px solid #1677ff;
		}
	)");

	_handBtn = new QPushButton(QString::fromUtf8("✋"), this);
	_handBtn->setToolTip(QString::fromUtf8("举手发言"));
	_handBtn->setFixedSize(32, 32);
	_handBtn->setStyleSheet(R"(
		QPushButton {
			background-color: #f2f3f5;
			border-radius: 16px;
			border: none;
			font-size: 15px;
		}
		QPushButton:hover {
			background-color: #e5e6eb;
		}
	)");

	connect(_chatInput, &QLineEdit::returnPressed, [this] {
		if (!_chatInput->text().trimmed().isEmpty()) {
			_sendChatStream.fire_copy(_chatInput->text().trimmed());
			_chatInput->clear();
		}
	});

	connect(_handBtn, &QPushButton::clicked, [this] {
		QMessageBox::information(this, QString::fromUtf8("举手"), QString::fromUtf8("您已向主持人举手申请发言！"));
	});
}

void RoomBottomBarWidget::setAudioMuted(bool muted) {
	_audioMuted = muted;
	update();
}

void RoomBottomBarWidget::setSpeakerMuted(bool muted) {
	_speakerMuted = muted;
	update();
}

void RoomBottomBarWidget::setVideoEnabled(bool enabled) {
	_videoEnabled = enabled;
	update();
}

void RoomBottomBarWidget::setParticipantCount(int count) {
	_participantCount = count;
	update();
}

void RoomBottomBarWidget::setChatUnreadCount(int count) {
	_chatUnreadCount = count;
	update();
}

bool RoomBottomBarWidget::HasAvailableAudioDevice() {
	try {
		auto mics = livekit::WasapiEnumerator::EnumerateInputDevices();
		auto defMic = livekit::WasapiEnumerator::GetDefaultInputDevice();
		return !mics.empty() && !defMic.id.empty();
	} catch (...) {
		return false;
	}
}

bool RoomBottomBarWidget::HasAvailableSpeakerDevice() {
	try {
		auto spks = livekit::WasapiEnumerator::EnumerateOutputDevices();
		return !spks.empty();
	} catch (...) {
		return false;
	}
}

bool RoomBottomBarWidget::HasAvailableVideoDevice() {
	try {
		auto cams = livekit::DShowEnumerator::EnumerateVideoDevices();
		auto defCam = livekit::DShowEnumerator::GetDefaultVideoDevice();
		return !cams.empty() && !defCam.path.empty();
	} catch (...) {
		return false;
	}
}

void RoomBottomBarWidget::resizeEvent(QResizeEvent *e) {
	const int w = width();
	const int h = height();

	_toolItems = {
		{ 1, QString::fromUtf8("解除静音"), QString::fromUtf8("静音"), QRect(), true },
		{ 11, QString::fromUtf8("开启扬声器"), QString::fromUtf8("扬声器"), QRect(), true },
		{ 2, QString::fromUtf8("开启视频"), QString::fromUtf8("停止视频"), QRect(), true },
		{ 3, QString::fromUtf8("共享屏幕"), QString::fromUtf8("共享屏幕"), QRect(), true },
		{ 4, QString::fromUtf8("邀请"), QString::fromUtf8("邀请"), QRect(), true },
		{ 5, QString::fromUtf8("成员(%1)").arg(_participantCount), QString::fromUtf8("成员(%1)").arg(_participantCount), QRect(), false },
		{ 6, QString::fromUtf8("聊天"), QString::fromUtf8("聊天"), QRect(), false },
		{ 7, QString::fromUtf8("录制"), QString::fromUtf8("停止录制"), QRect(), true },
		{ 9, QString::fromUtf8("应用"), QString::fromUtf8("应用"), QRect(), false },
		{ 10, QString::fromUtf8("场景模拟"), QString::fromUtf8("场景模拟"), QRect(), true },
	};

	int itemW = 56;
	int gap = 6;
	if (w < 880) {
		itemW = 46;
		gap = 2;
	} else if (w < 1020) {
		itemW = 50;
		gap = 4;
	}

	const int itemH = 60;
	const int numItems = static_cast<int>(_toolItems.size());
	const int totalItemsW = numItems * itemW + (numItems - 1) * gap;

	_endMeetingRect = QRect(w - 92, 12, 78, 52);

	int startX = (w - totalItemsW) / 2;
	if (startX + totalItemsW > w - 100) {
		startX = w - 100 - totalItemsW;
	}

	const int leftSpace = startX - 16;
	if (leftSpace >= 170) {
		const int chatW = std::min(130, leftSpace - 32 - 12);
		_chatInput->setVisible(true);
		_chatInput->setGeometry(16, 20, chatW, 32);
		_handBtn->setVisible(true);
		_handBtn->setGeometry(16 + chatW + 8, 20, 32, 32);
	} else if (leftSpace >= 110) {
		const int chatW = leftSpace - 32 - 10;
		_chatInput->setVisible(true);
		_chatInput->setGeometry(16, 20, chatW, 32);
		_handBtn->setVisible(true);
		_handBtn->setGeometry(16 + chatW + 6, 20, 32, 32);
	} else if (leftSpace >= 40) {
		_chatInput->setVisible(false);
		_handBtn->setVisible(true);
		_handBtn->setGeometry(16, 20, 32, 32);
	} else {
		_chatInput->setVisible(false);
		_handBtn->setVisible(false);
	}

	const int startY = 8;
	for (size_t i = 0; i < _toolItems.size(); ++i) {
		_toolItems[i].rect = QRect(startX + static_cast<int>(i) * (itemW + gap), startY, itemW, itemH);
	}
}

void RoomBottomBarWidget::paintEvent(QPaintEvent *e) {
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::TextAntialiasing);

	const int w = width();
	const int h = height();

	p.fillRect(rect(), Qt::white);
	p.setPen(QColor(0xeb, 0xed, 0xf0));
	p.drawLine(0, 0, w, 0);

	for (const auto &item : _toolItems) {
		const QRect r = item.rect;
		const bool hovered = (_hoveredId == item.id);

		p.save();
		if (hovered) {
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(0xf2, 0xf3, 0xf5));
			p.drawRoundedRect(r, 8, 8);
		}

		const int cx = r.center().x();
		const int cy = r.y() + 18;

		if (item.id == 1) {
			const bool muted = _audioMuted;
			p.setPen(QPen(muted ? QColor(0xf5, 0x3f, 0x3f) : QColor(0x1f, 0x23, 0x29), 1.8, Qt::SolidLine, Qt::RoundCap));
			p.setBrush(Qt::NoBrush);
			p.drawRoundedRect(QRect(cx - 4, cy - 7, 8, 11), 3, 3);
			p.drawLine(cx, cy + 4, cx, cy + 8);
			p.drawLine(cx - 4, cy + 8, cx + 4, cy + 8);
			if (muted) {
				p.drawLine(cx - 7, cy - 8, cx + 7, cy + 9);
			}
		} else if (item.id == 11) {
			const bool muted = _speakerMuted;
			p.setPen(QPen(muted ? QColor(0xf5, 0x3f, 0x3f) : QColor(0x1f, 0x23, 0x29), 1.8, Qt::SolidLine, Qt::RoundCap));
			p.setBrush(Qt::NoBrush);

			// 喇叭后腔方块
			p.drawRoundedRect(QRect(cx - 7, cy - 3, 4, 7), 1, 1);
			// 喇叭扩音锥形
			QPainterPath hornPath;
			hornPath.moveTo(cx - 3, cy - 3);
			hornPath.lineTo(cx + 1, cy - 7);
			hornPath.lineTo(cx + 1, cy + 7);
			hornPath.lineTo(cx - 3, cy + 3);
			hornPath.closeSubpath();
			p.drawPath(hornPath);

			if (muted) {
				// 静音状态绘制红色斜线
				p.drawLine(cx - 8, cy - 8, cx + 8, cy + 9);
			} else {
				// 开启状态绘制两道流畅声波弧线
				p.drawArc(QRect(cx - 2, cy - 5, 8, 11), -55 * 16, 110 * 16);
				p.drawArc(QRect(cx - 3, cy - 8, 13, 17), -55 * 16, 110 * 16);
			}
		} else if (item.id == 2) {
			const bool closed = !_videoEnabled;
			p.setPen(QPen(closed ? QColor(0xf5, 0x3f, 0x3f) : QColor(0x1f, 0x23, 0x29), 1.8, Qt::SolidLine, Qt::RoundCap));
			p.setBrush(Qt::NoBrush);
			p.drawRoundedRect(QRect(cx - 8, cy - 6, 11, 12), 2, 2);
			QPainterPath camPath;
			camPath.moveTo(cx + 3, cy - 2);
			camPath.lineTo(cx + 8, cy - 6);
			camPath.lineTo(cx + 8, cy + 6);
			camPath.lineTo(cx + 3, cy + 2);
			camPath.closeSubpath();
			p.drawPath(camPath);
			if (closed) {
				p.drawLine(cx - 9, cy - 8, cx + 9, cy + 9);
			}
		} else if (item.id == 3) {
			p.setPen(QPen(QColor(0x00, 0xb4, 0x2a), 1.8, Qt::SolidLine, Qt::RoundCap));
			p.setBrush(Qt::NoBrush);
			p.drawRoundedRect(QRect(cx - 8, cy - 7, 16, 12), 2, 2);
			p.drawLine(cx, cy + 2, cx, cy - 3);
			p.drawLine(cx - 3, cy - 1, cx, cy - 4);
			p.drawLine(cx + 3, cy - 1, cx, cy - 4);
		} else if (item.id == 4) {
			p.setPen(QPen(QColor(0x1f, 0x23, 0x29), 1.6));
			p.drawEllipse(QPoint(cx - 3, cy - 4), 3, 3);
			p.drawArc(cx - 7, cy, 8, 8, 0, 180 * 16);
			p.setPen(QPen(QColor(0x16, 0x77, 0xff), 1.8));
			p.drawLine(cx + 4, cy - 2, cx + 8, cy - 2);
			p.drawLine(cx + 6, cy - 4, cx + 6, cy);
		} else if (item.id == 5) {
			p.setPen(QPen(QColor(0x1f, 0x23, 0x29), 1.6));
			p.drawEllipse(QPoint(cx - 3, cy - 4), 3, 3);
			p.drawArc(cx - 5, cy, 10, 8, 0, 180 * 16);
		} else if (item.id == 6) {
			p.setPen(QPen(QColor(0x1f, 0x23, 0x29), 1.6));
			p.drawRoundedRect(QRect(cx - 7, cy - 6, 14, 11), 3, 3);
			p.drawLine(cx - 3, cy - 2, cx + 3, cy - 2);
			p.drawLine(cx - 3, cy + 1, cx + 1, cy + 1);

			if (_chatUnreadCount > 0) {
				p.save();
				p.setPen(Qt::NoPen);
				p.setBrush(QColor(0xf5, 0x3f, 0x3f));
				if (_chatUnreadCount > 99) {
					p.drawRoundedRect(QRect(cx + 4, cy - 10, 16, 10), 5, 5);
					p.setFont(QFont("Microsoft YaHei", 6, QFont::Bold));
					p.setPen(Qt::white);
					p.drawText(QRect(cx + 4, cy - 10, 16, 10), Qt::AlignCenter, "99+");
				} else if (_chatUnreadCount > 9) {
					p.drawRoundedRect(QRect(cx + 4, cy - 10, 14, 10), 5, 5);
					p.setFont(QFont("Microsoft YaHei", 6, QFont::Bold));
					p.setPen(Qt::white);
					p.drawText(QRect(cx + 4, cy - 10, 14, 10), Qt::AlignCenter, QString::number(_chatUnreadCount));
				} else {
					p.drawEllipse(QPoint(cx + 8, cy - 6), 4, 4);
				}
				p.restore();
			}
		} else if (item.id == 7) {
			p.setPen(QPen(QColor(0x1f, 0x23, 0x29), 1.6));
			p.drawEllipse(QPoint(cx, cy), 6, 6);
			p.setBrush(QColor(0x1f, 0x23, 0x29));
			p.drawEllipse(QPoint(cx, cy), 3, 3);
		} else if (item.id == 9) {
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(0x1f, 0x23, 0x29));
			for (int row = -1; row <= 1; ++row) {
				for (int col = -1; col <= 1; ++col) {
					p.drawRect(cx + col * 4 - 1, cy + row * 4 - 1, 2, 2);
				}
			}
		} else if (item.id == 10) {
			const QColor bugCol = hovered ? QColor(0x16, 0x77, 0xff) : QColor(0x1f, 0x23, 0x29);
			p.setPen(QPen(bugCol, 1.6, Qt::SolidLine, Qt::RoundCap));
			p.setBrush(Qt::NoBrush);
			p.drawRoundedRect(QRect(cx - 5, cy - 4, 10, 11), 4, 4);
			p.drawArc(QRect(cx - 3, cy - 8, 6, 6), 0, 180 * 16);
			p.drawLine(cx - 2, cy - 7, cx - 5, cy - 10);
			p.drawLine(cx + 2, cy - 7, cx + 5, cy - 10);
			p.drawLine(cx - 5, cy - 2, cx - 9, cy - 4);
			p.drawLine(cx + 5, cy - 2, cx + 9, cy - 4);
			p.drawLine(cx - 5, cy + 2, cx - 10, cy + 2);
			p.drawLine(cx + 5, cy + 2, cx + 10, cy + 2);
			p.drawLine(cx - 5, cy + 6, cx - 9, cy + 8);
			p.drawLine(cx + 5, cy + 6, cx + 9, cy + 8);
			p.drawLine(cx, cy - 4, cx, cy + 7);
		}

		QString title = item.title;
		if (item.id == 1) title = _audioMuted ? QString::fromUtf8("解除静音") : QString::fromUtf8("静音");
		else if (item.id == 11) title = _speakerMuted ? QString::fromUtf8("开启扬声器") : QString::fromUtf8("扬声器");
		else if (item.id == 2) title = _videoEnabled ? QString::fromUtf8("停止视频") : QString::fromUtf8("开启视频");
		else if (item.id == 5) title = QString::fromUtf8("成员(%1)").arg(_participantCount);

		QFont font("Microsoft YaHei", r.width() < 50 ? 8 : 9);
		p.setFont(font);
		p.setPen(QColor(0x4e, 0x59, 0x69));
		p.drawText(QRect(r.x(), r.bottom() - 18, r.width(), 16), Qt::AlignCenter, title);

		if (item.hasDropdown) {
			p.setPen(QPen(QColor(0x86, 0x90, 0x9c), 1.4));
			const int ax = r.right() - 7;
			const int ay = r.y() + 10;
			p.drawLine(ax - 3, ay, ax, ay + 3);
			p.drawLine(ax, ay + 3, ax + 3, ay);
		}

		p.restore();
	}

	p.save();
	if (_endHovered) {
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0xff, 0xec, 0xe8));
		p.drawRoundedRect(_endMeetingRect, 8, 8);
	}

	const int ecx = _endMeetingRect.center().x();
	const int ecy = _endMeetingRect.y() + 16;

	p.setPen(QPen(QColor(0xf5, 0x3f, 0x3f), 1.8, Qt::SolidLine, Qt::RoundCap));
	p.drawLine(ecx - 6, ecy - 7, ecx + 2, ecy - 7);
	p.drawLine(ecx + 2, ecy - 7, ecx + 2, ecy + 7);
	p.drawLine(ecx + 2, ecy + 7, ecx - 6, ecy + 7);

	p.drawLine(ecx - 8, ecy, ecx - 1, ecy);
	p.drawLine(ecx - 4, ecy - 3, ecx - 1, ecy);
	p.drawLine(ecx - 4, ecy + 3, ecx - 1, ecy);

	p.setFont(QFont("Microsoft YaHei", 10, QFont::Bold));
	p.setPen(QColor(0xf5, 0x3f, 0x3f));
	p.drawText(QRect(_endMeetingRect.x(), _endMeetingRect.bottom() - 20, _endMeetingRect.width(), 18), Qt::AlignCenter, QString::fromUtf8("结束会议"));
	p.restore();
}

void RoomBottomBarWidget::showAudioDeviceMenu(const QPoint &globalPos) {
	QMenu menu(this);
	menu.setStyleSheet(R"(
		QMenu {
			background-color: #ffffff;
			border: 1px solid #e5e6eb;
			border-radius: 8px;
			padding: 6px;
			font-size: 13px;
			color: #1f2329;
		}
		QMenu::item {
			padding: 6px 24px 6px 20px;
			border-radius: 4px;
		}
		QMenu::item:selected {
			background-color: #f2f3f5;
			color: #1677ff;
		}
		QMenu::separator {
			height: 1px;
			background-color: #e5e6eb;
			margin: 6px 8px;
		}
	)");

	// 1. 麦克风输入设备
	QAction *micHeader = menu.addAction(QString::fromUtf8("🎤 选择麦克风 (输入设备)"));
	micHeader->setEnabled(false);

	auto inputDevices = livekit::WasapiEnumerator::EnumerateInputDevices();
	auto defInput = livekit::WasapiEnumerator::GetDefaultInputDevice();

	QActionGroup *micGroup = new QActionGroup(&menu);
	for (const auto &dev : inputDevices) {
		QString title = QString::fromStdString(dev.name);
		if (dev.is_default) {
			title += QString::fromUtf8(" (系统默认)");
		}
		QAction *act = menu.addAction(title);
		act->setCheckable(true);
		if (_currentMicId.isEmpty()) {
			if (dev.is_default) act->setChecked(true);
		} else if (_currentMicId == QString::fromStdString(dev.id)) {
			act->setChecked(true);
		}
		micGroup->addAction(act);

		connect(act, &QAction::triggered, [this, devId = QString::fromStdString(dev.id)] {
			_currentMicId = devId;
			_micDeviceStream.fire_copy(devId);
		});
	}

	menu.addSeparator();

	// 2. 扬声器输出设备
	QAction *spkHeader = menu.addAction(QString::fromUtf8("🔊 选择扬声器 (输出设备)"));
	spkHeader->setEnabled(false);

	auto outputDevices = livekit::WasapiEnumerator::EnumerateOutputDevices();
	QActionGroup *spkGroup = new QActionGroup(&menu);
	for (size_t i = 0; i < outputDevices.size(); ++i) {
		const auto &dev = outputDevices[i];
		QString title = QString::fromStdString(dev.name);
		if (dev.is_default) {
			title += QString::fromUtf8(" (系统默认)");
		}
		QAction *act = menu.addAction(title);
		act->setCheckable(true);
		if (static_cast<int>(i) == _currentSpeakerIndex) {
			act->setChecked(true);
		}
		spkGroup->addAction(act);

		connect(act, &QAction::triggered, [this, idx = static_cast<int>(i)] {
			_currentSpeakerIndex = idx;
			_speakerDeviceStream.fire_copy(idx);
		});
	}

	menu.exec(globalPos);
}

void RoomBottomBarWidget::showSpeakerDeviceMenu(const QPoint &globalPos) {
	QMenu menu(this);
	menu.setStyleSheet(R"(
		QMenu {
			background-color: #ffffff;
			border: 1px solid #e5e6eb;
			border-radius: 8px;
			padding: 6px;
			font-size: 13px;
			color: #1f2329;
		}
		QMenu::item {
			padding: 6px 24px 6px 20px;
			border-radius: 4px;
		}
		QMenu::item:selected {
			background-color: #f2f3f5;
			color: #1677ff;
		}
		QMenu::separator {
			height: 1px;
			background-color: #e5e6eb;
			margin: 6px 8px;
		}
	)");

	QAction *spkHeader = menu.addAction(QString::fromUtf8("🔊 选择扬声器 (输出设备)"));
	spkHeader->setEnabled(false);

	auto outputDevices = livekit::WasapiEnumerator::EnumerateOutputDevices();
	QActionGroup *spkGroup = new QActionGroup(&menu);
	for (size_t i = 0; i < outputDevices.size(); ++i) {
		const auto &dev = outputDevices[i];
		QString title = QString::fromStdString(dev.name);
		if (dev.is_default) {
			title += QString::fromUtf8(" (系统默认)");
		}
		QAction *act = menu.addAction(title);
		act->setCheckable(true);
		if (static_cast<int>(i) == _currentSpeakerIndex) {
			act->setChecked(true);
		}
		spkGroup->addAction(act);

		connect(act, &QAction::triggered, [this, idx = static_cast<int>(i)] {
			_currentSpeakerIndex = idx;
			_speakerDeviceStream.fire_copy(idx);
		});
	}

	menu.addSeparator();

	QAction *toggleMuteAct = menu.addAction(_speakerMuted ? QString::fromUtf8("🔊 开启扬声器输出") : QString::fromUtf8("🔇 静音扬声器输出"));
	connect(toggleMuteAct, &QAction::triggered, [this] {
		_speakerMuted = !_speakerMuted;
		_toggleSpeakerStream.fire_copy(_speakerMuted);
		update();
	});

	menu.exec(globalPos);
}

void RoomBottomBarWidget::mouseMoveEvent(QMouseEvent *e) {
	const QPoint pos = e->pos();
	int nextId = -1;

	for (const auto &item : _toolItems) {
		if (item.rect.contains(pos)) {
			nextId = item.id;
			break;
		}
	}

	const bool nextEnd = _endMeetingRect.contains(pos);

	if (nextId != _hoveredId || nextEnd != _endHovered) {
		_hoveredId = nextId;
		_endHovered = nextEnd;
		update();
	}
}

void RoomBottomBarWidget::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::RightButton) {
		for (const auto &item : _toolItems) {
			if (item.rect.contains(e->pos())) {
				if (item.id == 1) {
					showAudioDeviceMenu(mapToGlobal(QPoint(item.rect.left(), item.rect.top() - 10)));
					return;
				} else if (item.id == 11) {
					showSpeakerDeviceMenu(mapToGlobal(QPoint(item.rect.left(), item.rect.top() - 10)));
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
					if (_audioMuted) {
						if (!HasAvailableAudioDevice()) {
							QMessageBox::warning(this, QString::fromUtf8("麦克风不可用"),
								QString::fromUtf8("未检测到可用的麦克风输入设备，无法开启麦克风！"));
							break;
						}
						_audioMuted = false;
					} else {
						_audioMuted = true;
					}
					_toggleAudioStream.fire_copy(_audioMuted);
					update();
					break;
				}
				case 11: {
					// 如果点击的是右侧下拉小三角区域 (宽 16px)
					if (e->pos().x() > item.rect.right() - 16) {
						showSpeakerDeviceMenu(mapToGlobal(QPoint(item.rect.left(), item.rect.top() - 10)));
						break;
					}
					if (_speakerMuted) {
						if (!HasAvailableSpeakerDevice()) {
							QMessageBox::warning(this, QString::fromUtf8("扬声器不可用"),
								QString::fromUtf8("未检测到可用的扬声器输出设备，无法开启扬声器！"));
							break;
						}
						_speakerMuted = false;
					} else {
						_speakerMuted = true;
					}
					_toggleSpeakerStream.fire_copy(_speakerMuted);
					update();
					break;
				}
				case 2: {
					if (!_videoEnabled) {
						// 准备开启视频，先检查是否有可用摄像头
						if (!HasAvailableVideoDevice()) {
							QMessageBox::warning(this, QString::fromUtf8("摄像头不可用"),
								QString::fromUtf8("未检测到可用的摄像头设备，无法开启视频！"));
							break;
						}
						_videoEnabled = true;
					} else {
						_videoEnabled = false;
					}
					_toggleVideoStream.fire_copy(_videoEnabled);
					update();
					break;
				}
				case 3:
					_shareScreenStream.fire({});
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
					_isRecording = !_isRecording;
					_recordStream.fire({});
					update();
					break;
				case 9:
					_appsStream.fire({});
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
	menu.setStyleSheet(R"(
		QMenu {
			background-color: #1a1a1f;
			border: 1px solid #2e2e38;
			border-radius: 8px;
			padding: 8px 4px;
			font-family: "Segoe UI", "Microsoft YaHei";
			color: #e4e4e8;
		}
		QMenu::item {
			padding: 7px 28px 7px 18px;
			border-radius: 6px;
			font-size: 13px;
			font-weight: 500;
			color: #e4e4e8;
		}
		QMenu::item:selected {
			background-color: #2b2b36;
			color: #ffffff;
		}
		QMenu::item:disabled {
			color: #8c8c9a;
			font-size: 14px;
			font-weight: bold;
			padding: 8px 18px 6px 18px;
		}
		QMenu::separator {
			height: 1px;
			background-color: #2e2e38;
			margin: 4px 8px;
		}
	)");

	QAction *header = menu.addAction(QString::fromUtf8("Simulate Scenario"));
	header->setEnabled(false);
	menu.addSeparator();

	struct ScenarioEntry {
		QString name;
		livekit::SimulateScenarioType type;
	};

	const std::vector<ScenarioEntry> entries = {
		{ "signalReconnect", livekit::SimulateScenarioType::SignalReconnect },
		{ "fullReconnect", livekit::SimulateScenarioType::FullReconnect },
		{ "speakerUpdate", livekit::SimulateScenarioType::SpeakerUpdate },
		{ "nodeFailure", livekit::SimulateScenarioType::NodeFailure },
		{ "migration", livekit::SimulateScenarioType::Migration },
		{ "serverLeave", livekit::SimulateScenarioType::ServerLeave },
		{ "switchCandidate", livekit::SimulateScenarioType::SwitchCandidate },
		{ "e2eeKeyRatchet", livekit::SimulateScenarioType::E2eeKeyRatchet },
		{ "participantName", livekit::SimulateScenarioType::ParticipantName },
		{ "participantMetadata", livekit::SimulateScenarioType::ParticipantMetadata },
		{ "clear", livekit::SimulateScenarioType::Clear }
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

// ----------------------------------------------------
// MeetingRoomWindow 实现
// ----------------------------------------------------

MeetingRoomWindow::MeetingRoomWindow(const Config &config,
                                     std::shared_ptr<OpenMeeting::MeetingCoordinator> coordinator,
                                     QWidget *parent)
	: Ui::RpWidget(parent)
	, _config(config)
	, _coordinator(std::move(coordinator)) {
	setObjectName("MeetingRoomWindow");
	if (!_coordinator) {
		_coordinator = OpenMeeting::MeetingCoordinator::create(this);
	}
	setWindowTitle(QString::fromUtf8("LiveKit 会议室 - %1").arg(config.displayName));
	resize(1120, 720);
	setMinimumSize(850, 560);
	setMouseTracking(true);

	setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowSystemMenuHint | Qt::WindowMinMaxButtonsHint);

	// 1. 校验硬件设备可用性
	if (!_config.audioMuted) {
		if (!RoomBottomBarWidget::HasAvailableAudioDevice()) {
			_config.audioMuted = true;
			LogToConsole(LogCategory::Media, "AUDIO", "未检测到可用的麦克风设备，麦克风已自动置为静音状态");
		}
	}
	if (_config.videoEnabled) {
		if (!RoomBottomBarWidget::HasAvailableVideoDevice()) {
			_config.videoEnabled = false;
			LogToConsole(LogCategory::Media, "VIDEO", "未检测到可用的摄像头设备，摄像头已自动置为关闭状态");
		}
	}

	initLayout();
	setupCoordinatorBindings();
	_remoteRenderSession = std::make_unique<livekit::render::VideoRenderSession>(
		[this](const std::string &identity, const QImage &image) {
			receiveRemoteVideoFrame(image, QString::fromStdString(identity));
		});
	_remoteRenderTimer = new QTimer(this);
	connect(_remoteRenderTimer, &QTimer::timeout, this, &MeetingRoomWindow::onRemoteRenderTick);
	_remoteRenderTimer->start(33);

	// 3. 复用 Coordinator 的本地音频与视频数据源，确保外设采集与 WebRTC 发送通道打通
	if (_coordinator) {
		_localAudioSource = _coordinator->localAudioSource();
		_localVideoSource = _coordinator->localVideoSource();
	}
	if (!_localAudioSource) {
		_localAudioSource = std::make_shared<livekit::AudioSource>(48000, 2);
	}
	if (!_localVideoSource) {
		_localVideoSource = std::make_shared<livekit::VideoSource>(1280, 720);
	}
	_localVideoSource->addSink([this](const livekit::VideoFrame &frame, const livekit::VideoCaptureOptions &) {
		if (_usingDx11Backend.load(std::memory_order_acquire) && _dx11Canvas) {
			_dx11Canvas->updateFrame("local", frame);
		}
	});
	_localAudioSource->addSink([this](const livekit::AudioFrame &frame) {
		if (_config.audioMuted) return;
		const auto &samples = frame.data();
		if (!samples.empty()) {
			double sum = 0.0;
			for (size_t i = 0; i < samples.size(); ++i) {
				sum += static_cast<double>(samples[i]) * samples[i];
			}
			double rms = std::sqrt(sum / static_cast<double>(samples.size()));
			float level = static_cast<float>(std::clamp(rms / 2200.0, 0.0, 1.0));
			bool speaking = (level > 0.02f);
			QMetaObject::invokeMethod(this, [this, speaking, level]() {
				if (_localTile && !_config.audioMuted) {
					_localTile->setSpeaking(speaking, level);
				}
			}, Qt::QueuedConnection);
		}
	});

	_localVideoSource->addSink([this](const livekit::VideoFrame &frame, const livekit::VideoCaptureOptions &) {
		if (_usingDx11Backend.load(std::memory_order_acquire)) {
			return;
		}
		QImage img = VideoFrameToQImage(frame);
		if (!img.isNull()) {
			QMetaObject::invokeMethod(this, [this, img = std::move(img)]() {
				receiveLocalVideoFrame(img);
			}, Qt::QueuedConnection);
		}
	});

	// 4. 启动物理麦克风 WASAPI 采集
	_wasapiCap = livekit::WasapiAudioCapture::Create();
	_wasapiCap->EnableApm();
	livekit::WasapiCaptureConfig acfg;
	acfg.type = livekit::WasapiCaptureType::Microphone;
	acfg.target_sample_rate = 48000;
	acfg.target_channels = 2;
	if (_wasapiCap->Init(acfg, _localAudioSource) && _wasapiCap->Start()) {
		_wasapiCap->SetMute(_config.audioMuted);
		LogToConsole(LogCategory::Media, "WASAPI", QString("成功启动物理麦克风音频采集 (48kHz 双声道, 初始状态: %1)").arg(_config.audioMuted ? "静音" : "开启"));
	} else {
		LogToConsole(LogCategory::Error, "WASAPI", "物理麦克风初始化或启动失败");
	}

	// 5. 启动物理摄像头 DirectShow 采集
	try {
		auto defaultDev = livekit::DShowEnumerator::GetDefaultVideoDevice();
		if (!defaultDev.path.empty()) {
			_dshowCap = livekit::DShowVideoCapture::Create();
			livekit::DShowCaptureConfig vcfg;
			vcfg.device_path = defaultDev.path;
			vcfg.width = 1280;
			vcfg.height = 720;
			vcfg.fps = 30;
			vcfg.output_format = livekit::VideoBufferType::NV12;
			if (_dshowCap->Init(vcfg, _localVideoSource) && _dshowCap->Start()) {
				_usingRealCamera = true;
				LogToConsole(LogCategory::Media, "DSHOW", QString("成功启动物理摄像头: %1 (1280x720@30fps NV12)").arg(QString::fromStdString(defaultDev.name)));
			}
		}
	} catch (const std::exception &ex) {
		LogToConsole(LogCategory::Error, "DSHOW", QString("摄像头初始化异常: %1").arg(ex.what()));
	}

	_localTile->setVideoActive(_config.videoEnabled && _usingRealCamera);
	_localTile->setAudioMuted(_config.audioMuted);
	updateVideoLayout();

	startLiveKitSession();

	// 自动弹出控制台便于测试观察
	MeetingLogConsoleWindow::Instance().show();
	MeetingLogConsoleWindow::Instance().raise();
}

MeetingRoomWindow::~MeetingRoomWindow() {
	stopLiveKitSession();
}

void MeetingRoomWindow::showEvent(QShowEvent *e) {
	Ui::RpWidget::showEvent(e);
	setupNativeWindow();
	tryActivateDx11Backend();
}

void MeetingRoomWindow::closeEvent(QCloseEvent *e) {
	stopLiveKitSession();
	Ui::RpWidget::closeEvent(e);
}

void MeetingRoomWindow::setupNativeWindow() {
#if defined(Q_OS_WIN)
	if (!_handle) {
		_handle = reinterpret_cast<HWND>(winId());
	}
	if (!_handle) return;

	LONG_PTR style = GetWindowLongPtr(_handle, GWL_STYLE);
	SetWindowLongPtr(_handle, GWL_STYLE, style | WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);

	MARGINS margins = { 1, 1, 1, 1 };
	DwmExtendFrameIntoClientArea(_handle, &margins);

	DWORD preference = 2; // DWMWCP_ROUND
	DwmSetWindowAttribute(_handle, 33, &preference, sizeof(preference));

	SetWindowPos(_handle, nullptr, 0, 0, 0, 0,
		SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
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
		setWindowTitle(QString::fromUtf8("会议 - 会议号: %1").arg(mId));
	}
	_stageContainer = new QWidget(this);
	_stageContainer->setStyleSheet("background-color: #12141a;");
	if (livekit::dx11::Dx11VideoCanvas::IsHardwareBackendAllowed()) {
		_dx11Canvas = new livekit::dx11::Dx11VideoCanvas(_stageContainer);
		_dx11Canvas->setGeometry(_stageContainer->rect());
		_dx11Canvas->hide();
		connect(_dx11Canvas, &livekit::dx11::Dx11VideoCanvas::rendererUnavailable,
		        this, &MeetingRoomWindow::fallBackToQtCpuBackend);
		connect(_dx11Canvas, &livekit::dx11::Dx11VideoCanvas::tileDoubleClicked,
		        this, [this](const QString &identity) {
			if (identity.isEmpty()) return;
			if (_pinnedIdentity == identity) _pinnedIdentity.clear();
			else _pinnedIdentity = identity;
			updateVideoLayout();
		});
	}
	_bottomBar = new RoomBottomBarWidget(this);

	_participantsSidebar = new OpenMeeting::ParticipantsSidebarWidget(_coordinator, this);
	_participantsSidebar->hide();
	connect(_participantsSidebar, &OpenMeeting::ParticipantsSidebarWidget::closeRequested, this, [this]() {
		switchSidebar(ActiveSidebar::None);
	});

	_chatSidebar = new OpenMeeting::MeetingChatSidebarWidget(this);
	_chatSidebar->hide();
	connect(_chatSidebar, &OpenMeeting::MeetingChatSidebarWidget::closeRequested, this, [this]() {
		switchSidebar(ActiveSidebar::None);
	});
	connect(_chatSidebar, &OpenMeeting::MeetingChatSidebarWidget::messageSent, this, [this](const QString &text) {
		QString msgId = QString("msg_%1_%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(qrand() % 1000);
		int64_t seq = _coordinator ? _coordinator->nextSequenceNumber() : (QDateTime::currentMSecsSinceEpoch() * 1000);
		OpenMeeting::ChatMessageItem item;
		item.id = msgId;
		item.senderIdentity = "local";
		item.senderName = QString::fromUtf8("%1 (我)").arg(_config.displayName);
		item.text = text;
		item.timestamp = QDateTime::currentMSecsSinceEpoch();
		item.seq = seq;
		item.isMine = true;
		item.status = OpenMeeting::MessageSendStatus::Sending;
		_chatSidebar->appendMessage(item);

		if (_coordinator) {
			_coordinator->sendChatMessage(text, msgId, seq);
		}
		LogToConsole(LogCategory::Participant, "CHAT", QString("我: %1").arg(text));
	});

	connect(_chatSidebar, &OpenMeeting::MeetingChatSidebarWidget::imageSent, this, [this](const QString &fileName, const QByteArray &data) {
		QString msgId = QString("img_%1_%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(qrand() % 1000);
		int64_t seq = _coordinator ? _coordinator->nextSequenceNumber() : (QDateTime::currentMSecsSinceEpoch() * 1000);
		OpenMeeting::ChatMessageItem item;
		item.id = msgId;
		item.senderIdentity = "local";
		item.senderName = QString::fromUtf8("%1 (我)").arg(_config.displayName);
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
		LogToConsole(LogCategory::Participant, "CHAT", QString("我 发送了图片: %1 (%2)").arg(fileName).arg(OpenMeeting::ChatBubbleWidget::formatFileSize(data.size())));
	});

	connect(_chatSidebar, &OpenMeeting::MeetingChatSidebarWidget::fileSent, this, [this](const QString &fileName, const QByteArray &data) {
		QString msgId = QString("file_%1_%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(qrand() % 1000);
		int64_t seq = _coordinator ? _coordinator->nextSequenceNumber() : (QDateTime::currentMSecsSinceEpoch() * 1000);
		OpenMeeting::ChatMessageItem item;
		item.id = msgId;
		item.senderIdentity = "local";
		item.senderName = QString::fromUtf8("%1 (我)").arg(_config.displayName);
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
		LogToConsole(LogCategory::Participant, "CHAT", QString("我 发送了文件: %1 (%2)").arg(fileName).arg(OpenMeeting::ChatBubbleWidget::formatFileSize(data.size())));
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

	_localTile = new VideoTileWidget(QString::fromUtf8("%1 (我)").arg(_config.displayName), true, _stageContainer);
	_localTile->setIdentity("local");
	_localTile->show();

	connect(_localTile, &VideoTileWidget::tileDoubleClicked, [this] {
		if (_pinnedIdentity == "local") _pinnedIdentity.clear();
		else _pinnedIdentity = "local";
		updateVideoLayout();
	});

	connect(_localTile, &VideoTileWidget::pinToggled, [this](bool pinned) {
		if (pinned) _pinnedIdentity = "local";
		else if (_pinnedIdentity == "local") _pinnedIdentity.clear();
		updateVideoLayout();
	});

	_bottomBar->setAudioMuted(_config.audioMuted);
	_bottomBar->setVideoEnabled(_config.videoEnabled);
	_bottomBar->setParticipantCount(1);

	_localTile->setAudioMuted(_config.audioMuted);
	_localTile->setVideoActive(_config.videoEnabled);

	_inviteHintBanner = new QLabel(QString::fromUtf8("等待更多参会人加入会议..."), _stageContainer);
	_inviteHintBanner->setAlignment(Qt::AlignCenter);
	_inviteHintBanner->setStyleSheet(R"(
		QLabel {
			background-color: rgba(255, 255, 255, 25);
			color: #e5e6eb;
			border-radius: 8px;
			font-size: 13px;
			font-family: "Microsoft YaHei";
			padding: 6px 14px;
		}
	)");

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
		updateVideoLayout();
	}, lifetime());
	_topBar->consoleClicked() | rpl::on_next([this] {
		MeetingLogConsoleWindow::Instance().show();
		MeetingLogConsoleWindow::Instance().raise();
		MeetingLogConsoleWindow::Instance().activateWindow();
	}, lifetime());
	_topBar->settingsClicked() | rpl::on_next([this] {
		QMessageBox::information(this, QString::fromUtf8("会议设置"),
			QString::fromUtf8("音视频设置已开启：默认音频 48kHz 立体声降噪，视频分辨率自适应 (VP8/H264 Simulcast)。"));
	}, lifetime());

	auto handleSimulate = [this](livekit::SimulateScenarioType type) {
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
		LogToConsole(LogCategory::General, "SIMULATE", QString("已触发场景模拟: %1").arg(name));
	};

	_topBar->simulateScenarioRequested() | rpl::on_next(handleSimulate, lifetime());
	_bottomBar->simulateScenarioRequested() | rpl::on_next(handleSimulate, lifetime());

	_bottomBar->toggleAudioRequested() | rpl::on_next([this](bool muted) {
		_config.audioMuted = muted;
		_localTile->setAudioMuted(muted);
		if (_coordinator) {
			_coordinator->setLocalAudioMuted(muted);
		}
		if (_wasapiCap) {
			_wasapiCap->SetMute(muted);
		}
		if (_localAudioTrack) {
			_localAudioTrack->set_muted(muted);
		}
		if (_room) {
			auto local = _room->local_participant();
			if (local && _localAudioTrack) {
				local->SetMuted(_localAudioTrack->sid(), muted);
			}
		}
		LogToConsole(LogCategory::Media, "AUDIO", muted ? "用户点击静音麦克风" : "用户点击开启/解除麦克风静音");
	}, lifetime());

	_bottomBar->toggleSpeakerRequested() | rpl::on_next([this](bool muted) {
		if (_room) {
			_room->SetAudioOutputMuted(muted);
		}
		LogToConsole(LogCategory::Media, "SPEAKER", muted ? "用户点击静音扬声器 (关闭声音输出)" : "用户点击开启扬声器 (恢复声音输出)");
	}, lifetime());

	_bottomBar->toggleVideoRequested() | rpl::on_next([this](bool enabled) {
		_config.videoEnabled = enabled;
		_localTile->setVideoActive(enabled && _usingRealCamera);
		if (_coordinator) {
			_coordinator->setLocalVideoEnabled(enabled);
		}
		if (_localVideoTrack) {
			_localVideoTrack->set_muted(!enabled);
		}
		if (_room) {
			auto local = _room->local_participant();
			if (local && _localVideoTrack) {
				local->SetMuted(_localVideoTrack->sid(), !enabled);
			}
		}
		updateVideoLayout();
		LogToConsole(LogCategory::Media, "VIDEO", enabled ? "用户点击开启本地视频" : "用户点击关闭本地视频");
	}, lifetime());

	_bottomBar->shareScreenClicked() | rpl::on_next([this] {
		QMessageBox::information(this, QString::fromUtf8("屏幕共享"),
			QString::fromUtf8("已开启桌面与窗口采集选择器，您可以选择任意应用进行全高清共享。"));
	}, lifetime());

	_bottomBar->inviteClicked() | rpl::on_next([this] {
		QString inviteText = QString::fromUtf8("【LiveKit 会议邀请】\n服务器地址: %1\nToken: %2\n请使用会议客户端连接入会！")
			.arg(_config.serverUrl).arg(_config.token.isEmpty() ? "(空)" : _config.token);
		QApplication::clipboard()->setText(inviteText);
		LogToConsole(LogCategory::General, "INVITE", "会议邀请信息已复制到剪贴板");
		QMessageBox::information(this, QString::fromUtf8("邀请信息已复制"),
			QString::fromUtf8("会议邀请信息已复制到剪贴板，您可以直接粘贴发送给其他参会人！"));
	}, lifetime());

	_bottomBar->participantsClicked() | rpl::on_next([this] {
		switchSidebar(ActiveSidebar::Participants);
	}, lifetime());

	_bottomBar->chatClicked() | rpl::on_next([this] {
		switchSidebar(ActiveSidebar::Chat);
	}, lifetime());

	if (_coordinator) {
		connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::chatMessageReceived,
			this, [this](const QString &senderIdentity, const QString &senderName, const QString &text, int64_t seq) {
			QString dispName = senderName.trimmed();
			if (dispName.isEmpty() || dispName.startsWith("PA_")) {
				dispName = senderIdentity;
			}
			if (_coordinator && (dispName.isEmpty() || dispName == senderIdentity || dispName.startsWith("PA_"))) {
				for (const auto &p : _coordinator->participants()) {
					if (p.identity == senderIdentity || p.identity == senderName) {
						if (!p.name.isEmpty()) {
							dispName = p.name;
						}
						break;
					}
				}
			}
			if (dispName.isEmpty()) {
				dispName = QString::fromUtf8("参会人");
			}

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

			LogToConsole(LogCategory::Participant, "CHAT", QString("%1: %2").arg(dispName).arg(text));
		});

		connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::chatMediaReceivingStarted,
			this, [this](const QString &transferId, const QString &senderIdentity, const QString &senderName,
						 const QString &mediaType, const QString &fileName, qint64 totalSize, int64_t seq) {
			QString dispName = senderName.trimmed();
			if (dispName.isEmpty() || dispName.startsWith("PA_")) {
				dispName = senderIdentity;
			}
			if (_coordinator && (dispName.isEmpty() || dispName == senderIdentity || dispName.startsWith("PA_"))) {
				for (const auto &p : _coordinator->participants()) {
					if (p.identity == senderIdentity || p.identity == senderName) {
						if (!p.name.isEmpty()) {
							dispName = p.name;
						}
						break;
					}
				}
			}
			if (dispName.isEmpty()) {
				dispName = QString::fromUtf8("参会人");
			}

			if (_chatSidebar) {
				_chatSidebar->startReceivingMedia(transferId, senderIdentity, dispName, mediaType, fileName, totalSize, seq);
			}

			if (_activeSidebar != ActiveSidebar::Chat && _bottomBar) {
				_bottomBar->setChatUnreadCount(_bottomBar->chatUnreadCount() + 1);
			}

			LogToConsole(LogCategory::Participant, "CHAT", QString("正在接收 %1 发送的%2: %3 (%4)...")
				.arg(dispName)
				.arg(mediaType == "image" ? "图片" : "文件")
				.arg(fileName)
				.arg(OpenMeeting::ChatBubbleWidget::formatFileSize(totalSize)));
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

			QString dispName = senderName.trimmed();
			if (dispName.isEmpty() || dispName.startsWith("PA_")) dispName = senderIdentity;
			LogToConsole(LogCategory::Participant, "CHAT", QString("%1 发送的%2已接收完成: %3 (%4)")
				.arg(dispName)
				.arg(mediaType == "image" ? "图片" : "文件")
				.arg(fileName)
				.arg(OpenMeeting::ChatBubbleWidget::formatFileSize(data.size())));
		});

		connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::chatMediaReceivingFailed,
			this, [this](const QString &transferId, const QString &reason) {
			if (_chatSidebar) {
				_chatSidebar->failReceivingMedia(transferId, reason);
			}
			LogToConsole(LogCategory::Participant, "CHAT", QString("多媒体接收中断: %1").arg(reason));
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

	_bottomBar->recordClicked() | rpl::on_next([this] {
		QMessageBox::information(this, QString::fromUtf8("云端录制"),
			QString::fromUtf8("正在将本次会议视频流实时转码存档至高可用存储。"));
	}, lifetime());

	_bottomBar->appsClicked() | rpl::on_next([this] {
		QMessageBox::information(this, QString::fromUtf8("会议应用"),
			QString::fromUtf8("可用应用：互动白板、投票调查、同声传译、计时器。"));
	}, lifetime());

	_bottomBar->endMeetingClicked() | rpl::on_next([this] {
		handleEndMeetingClicked();
	}, lifetime());

	_bottomBar->sendChatRequested() | rpl::on_next([this](const QString &text) {
		_topBar->setActiveSpeaker(QString::fromUtf8("%1: %2").arg(_config.displayName).arg(text));
		LogToConsole(LogCategory::Participant, "CHAT", QString("%1: %2").arg(_config.displayName).arg(text));
		if (_coordinator) {
			_coordinator->sendChatMessage(text);
		}
	}, lifetime());

	_bottomBar->microphoneDeviceChanged() | rpl::on_next([this](const QString &devId) {
		if (_wasapiCap) {
			_wasapiCap->SwitchDevice(devId.toStdString());
			LogToConsole(LogCategory::Media, "DEVICE", QString("麦克风设备已切换为: %1").arg(devId.isEmpty() ? "(系统默认)" : devId));
		}
	}, lifetime());

	_bottomBar->speakerDeviceChanged() | rpl::on_next([this](int idx) {
		livekit::WebRTCManager::Instance().SetPlayoutDevice(static_cast<uint16_t>(idx));
		LogToConsole(LogCategory::Media, "DEVICE", QString("扬声器播放设备已切换为索引: %1").arg(idx));
	}, lifetime());

	_localGenTimer = new QTimer(this);
	connect(_localGenTimer, &QTimer::timeout, this, &MeetingRoomWindow::onLocalVideoGenerated);
}

void MeetingRoomWindow::onTimerTick() {
	_elapsedSeconds++;
	_topBar->updateDuration(_elapsedSeconds);
}

void MeetingRoomWindow::onRemoteRenderTick() {
	if (_remoteRenderSession) {
		_remoteRenderSession->RenderLatestFrames();
	}
}

void MeetingRoomWindow::switchSidebar(ActiveSidebar target) {
	if (_activeSidebar == target) {
		_activeSidebar = ActiveSidebar::None;
	} else {
		_activeSidebar = target;
	}

	if (_activeSidebar == ActiveSidebar::Chat && _bottomBar) {
		_bottomBar->setChatUnreadCount(0);
	}

	QResizeEvent ev(size(), size());
	resizeEvent(&ev);
}

void MeetingRoomWindow::resizeEvent(QResizeEvent *e) {
	const int w = width();
	const int h = height();

	_topBar->setGeometry(0, 0, w, 44);

	constexpr int kSidebarWidth = 340;
	const int sidebarW = (_activeSidebar != ActiveSidebar::None) ? kSidebarWidth : 0;
	const int stageW = w - sidebarW;
	const int stageH = h - 44 - 76;

	_stageContainer->setGeometry(0, 44, stageW, stageH);

	if (_activeSidebar == ActiveSidebar::Participants) {
		if (_participantsSidebar) {
			_participantsSidebar->setGeometry(stageW, 44, sidebarW, stageH);
			_participantsSidebar->show();
			_participantsSidebar->raise();
		}
		if (_chatSidebar) {
			_chatSidebar->hide();
		}
	} else if (_activeSidebar == ActiveSidebar::Chat) {
		if (_chatSidebar) {
			_chatSidebar->setGeometry(stageW, 44, sidebarW, stageH);
			_chatSidebar->show();
			_chatSidebar->raise();
		}
		if (_participantsSidebar) {
			_participantsSidebar->hide();
		}
	} else {
		if (_participantsSidebar) _participantsSidebar->hide();
		if (_chatSidebar) _chatSidebar->hide();
	}

	_bottomBar->setGeometry(0, h - 76, w, 76);

	updateVideoLayout();
}

void MeetingRoomWindow::onRemoteParticipantJoined(const QString &identity, const QString &name) {
	if (identity.isEmpty()) return;

	QString dispName = name.isEmpty() ? identity : name;
	if (dispName == identity && _coordinator) {
		for (const auto &p : _coordinator->participants()) {
			if (p.identity == identity && !p.name.isEmpty()) {
				dispName = p.name;
				break;
			}
		}
	}

	auto it = _remoteTiles.find(identity);
	if (it == _remoteTiles.end()) {
		auto tile = std::make_unique<VideoTileWidget>(dispName, false, _stageContainer);
		tile->setIdentity(identity);
		tile->setVideoActive(false);

		connect(tile.get(), &VideoTileWidget::tileDoubleClicked, [this, identity] {
			if (_pinnedIdentity == identity) _pinnedIdentity.clear();
			else _pinnedIdentity = identity;
			updateVideoLayout();
		});

		connect(tile.get(), &VideoTileWidget::pinToggled, [this, identity](bool pinned) {
			if (pinned) _pinnedIdentity = identity;
			else if (_pinnedIdentity == identity) _pinnedIdentity.clear();
			updateVideoLayout();
		});

		connect(tile.get(), &VideoTileWidget::remoteVolumeChanged, [this, identity](float volume) {
			_remoteVolumes[identity] = volume;
			if (_room) {
				_room->SetParticipantVolume(identity.toStdString(), volume);
			}
		});

		connect(tile.get(), &VideoTileWidget::remoteLocalMuteToggled, [this, identity](bool muted) {
			if (muted) _locallyMutedUsers.insert(identity);
			else _locallyMutedUsers.erase(identity);
			if (_room) {
				_room->SetParticipantMuted(identity.toStdString(), muted);
			}
		});

		tile->show();
		_remoteTiles[identity] = std::move(tile);
	} else {
		if (!dispName.isEmpty() && it->second && it->second->displayName() != dispName) {
			it->second->setDisplayName(dispName);
		}
	}

	if (_room) {
		auto itVol = _remoteVolumes.find(identity);
		if (itVol != _remoteVolumes.end()) {
			_room->SetParticipantVolume(identity.toStdString(), itVol->second);
		}
		if (_locallyMutedUsers.count(identity)) {
			_room->SetParticipantMuted(identity.toStdString(), true);
		}
	}

	if (auto tileIt = _remoteTiles.find(identity);
		tileIt != _remoteTiles.end() && tileIt->second && _coordinator) {
		for (const auto &participant : _coordinator->participants()) {
			if (participant.identity != identity) continue;
			tileIt->second->setConnectionQuality(participant.connectionQuality);
			tileIt->second->setVideoStreamPaused(participant.isVideoStreamPaused);
			break;
		}
	}

	_participantCount = 1 + static_cast<int>(_remoteTiles.size());
	if (_bottomBar) {
		_bottomBar->setParticipantCount(_participantCount);
	}

	LogToConsole(LogCategory::Participant, "USER_JOIN", QString("远端参会人已加入: %1 (姓名: %2, 当前房间总人数: %3)").arg(identity).arg(dispName).arg(_participantCount));
	updateVideoLayout();
}

void MeetingRoomWindow::onRemoteParticipantLeft(const QString &identity) {
	if (_remoteRenderSession) {
		_remoteRenderSession->RemoveTracksForIdentity(identity.toStdString());
	}
	if (_dx11Canvas) {
		_dx11Canvas->removeUser(identity.toStdString());
	}
	auto it = _remoteTiles.find(identity);
	if (it != _remoteTiles.end()) {
		it->second->hide();
		_remoteTiles.erase(it);
	}

	if (_pinnedIdentity == identity) {
		_pinnedIdentity.clear();
	}

	_participantCount = 1 + static_cast<int>(_remoteTiles.size());
	if (_bottomBar) {
		_bottomBar->setParticipantCount(_participantCount);
	}

	LogToConsole(LogCategory::Participant, "USER_LEFT", QString("远端参会人已离开: %1 (当前房间总人数: %2)").arg(identity).arg(_participantCount));
	updateVideoLayout();
}


void MeetingRoomWindow::onRemoteTrackMuted(const QString &identity, bool isVideo, bool muted) {
	auto it = _remoteTiles.find(identity);
	if (it == _remoteTiles.end() || !it->second) {
		return;
	}
	if (isVideo) {
		it->second->setVideoActive(!muted);
		if (muted) it->second->setFrame(QImage());
	} else {
		it->second->setAudioMuted(muted);
		if (muted) it->second->setSpeaking(false, 0.0f);
	}
	updateVideoLayout();
}

void MeetingRoomWindow::updateActiveSpeakers(const std::vector<std::shared_ptr<livekit::Participant>> &speakers) {
	QString primarySpeakerName;
	std::unordered_map<std::string, float> speaking_levels;

	for (const auto &spk : speakers) {
		if (spk && spk->is_speaking()) {
			speaking_levels[spk->sid()] = spk->audio_level();
			speaking_levels[spk->identity()] = spk->audio_level();
			if (primarySpeakerName.isEmpty()) {
				primarySpeakerName = QString::fromStdString(spk->identity());
			}
		}
	}

	// 1. 顶部状态栏提示更新
	if (_topBar) {
		_topBar->setActiveSpeaker(primarySpeakerName);
	}

	// 2. 本端画框发光光圈联动
	if (_localTile && _room) {
		auto local = _room->local_participant();
		bool localSpeaking = false;
		float localLevel = 0.0f;
		if (local) {
			auto it = speaking_levels.find(local->sid());
			if (it != speaking_levels.end()) {
				localSpeaking = true;
				localLevel = it->second;
			} else {
				auto it2 = speaking_levels.find(local->identity());
				if (it2 != speaking_levels.end()) {
					localSpeaking = true;
					localLevel = it2->second;
				}
			}
		}
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

void MeetingRoomWindow::tryActivateDx11Backend() {
	if (_dx11BackendActivationAttempted || !_dx11Canvas || !_remoteRenderSession ||
		!livekit::dx11::Dx11VideoCanvas::IsHardwareBackendAllowed()) {
		return;
	}
	_dx11BackendActivationAttempted = true;
	_dx11Canvas->setGeometry(_stageContainer->rect());
	_dx11Canvas->show(); // showEvent performs the UI-thread device probe.

	if (!_dx11Canvas->rendererReady()) {
		_dx11Canvas->hide();
		_remoteRenderSession->UseQtCpuBackend();
		LogToConsole(LogCategory::WebRTC, "DX11", "DX11 Canvas 初始化失败，已使用 Qt CPU 视频后端");
		return;
	}

	_remoteRenderSession->UseDx11Backend(
		[this](const std::string &identity, livekit::render::OwnedI420Frame::Ptr frame) {
			const QString participant = QString::fromStdString(identity);
			if (_remoteTiles.find(participant) == _remoteTiles.end()) {
				onRemoteParticipantJoined(participant);
			}
			auto it = _remoteTiles.find(participant);
			if (it != _remoteTiles.end() && it->second && !it->second->isVideoActive()) {
				it->second->setVideoActive(true);
				updateVideoLayout();
			}
			if (_usingDx11Backend.load(std::memory_order_acquire) && _dx11Canvas) {
				_dx11Canvas->updateI420Frame(identity, std::move(frame));
			}
		});
	_usingDx11Backend.store(true, std::memory_order_release);
	LogToConsole(LogCategory::WebRTC, "DX11", "已启用 I420 直渲染后端");
	updateVideoLayout();
}

void MeetingRoomWindow::fallBackToQtCpuBackend() {
	const bool was_using_dx11 = _usingDx11Backend.exchange(false, std::memory_order_acq_rel);
	if (_remoteRenderSession) {
		_remoteRenderSession->UseQtCpuBackend();
	}
	if (_dx11Canvas) {
		_dx11Canvas->clearUsers();
		_dx11Canvas->hide();
	}
	if (was_using_dx11) {
		LogToConsole(LogCategory::Error, "DX11", "DX11 Present/设备失败，已切换到 Qt CPU 视频后端");
	}
	updateVideoLayout();
}

void MeetingRoomWindow::syncDx11CanvasLayout(const std::vector<VideoTileWidget*> &tiles) {
	if (!_usingDx11Backend.load(std::memory_order_acquire) || !_dx11Canvas) {
		return;
	}

	std::vector<livekit::dx11::TileRect> dx11_tiles;
	dx11_tiles.reserve(tiles.size());
	for (auto *tile : tiles) {
		if (!tile) continue;
		const QRect geometry = tile->geometry();
		dx11_tiles.push_back({
			tile->identity().toStdString(),
			geometry.x(), geometry.y(), geometry.width(), geometry.height(),
			tile->isSpeaking(), tile->audioLevel(), tile->isVideoActive()
		});
		tile->setHardwareCanvasMode(true);
		tile->hide();
	}

	_dx11Canvas->setGeometry(_stageContainer->rect());
	_dx11Canvas->setTilesLayout(dx11_tiles);
	_dx11Canvas->show();
	_dx11Canvas->raise();
	if (_inviteHintBanner) _inviteHintBanner->hide();
}

void MeetingRoomWindow::updateVideoLayout() {
	const int stageW = _stageContainer->width();
	const int stageH = _stageContainer->height();
	if (stageW <= 0 || stageH <= 0) return;

	std::vector<VideoTileWidget*> allTiles;
	if (_localTile) {
		allTiles.push_back(_localTile);
	}
	for (auto &[id, tile] : _remoteTiles) {
		if (tile) {
			allTiles.push_back(tile.get());
		}
	}

	const int N = static_cast<int>(allTiles.size());
	if (N == 0) return;

	const bool hasRemote = !_remoteTiles.empty();
	const bool localActive = _localTile && _localTile->isVideoActive();

	const int bannerW = 220;
	const int bannerH = 32;
	_inviteHintBanner->setGeometry((stageW - bannerW) / 2, stageH - bannerH - 12, bannerW, bannerH);
	_inviteHintBanner->setVisible(!_usingDx11Backend.load(std::memory_order_acquire) && !hasRemote && !localActive);

	// 1. 画中画模式 (PiP)
	if (_viewMode == VideoViewMode::Pip && N >= 2) {
		VideoTileWidget *mainTile = allTiles[1];
		if (_pinnedIdentity == "local") {
			mainTile = _localTile;
		} else if (!_pinnedIdentity.isEmpty()) {
			auto it = _remoteTiles.find(_pinnedIdentity);
			if (it != _remoteTiles.end()) mainTile = it->second.get();
		}

		mainTile->setPipMode(false);
		mainTile->setGeometry(0, 0, stageW, stageH);
		mainTile->show();
		mainTile->lower();

		const int pipW = std::clamp(stageW * 22 / 100, 160, 260);
		const int pipH = pipW * 9 / 16;
		int pipRightOffset = 16;

		for (auto *t : allTiles) {
			if (t == mainTile) continue;
			t->setPipMode(true);
			t->setGeometry(stageW - pipW - pipRightOffset, stageH - pipH - 16, pipW, pipH);
			t->show();
			t->raise();
			pipRightOffset += pipW + 10;
		}
		syncDx11CanvasLayout(allTiles);
		return;
	}

	// 2. 演讲者聚焦模式 (Speaker / Focus Mode)
	if ((_viewMode == VideoViewMode::Speaker || !_pinnedIdentity.isEmpty()) && N >= 2) {
		VideoTileWidget *focusTile = allTiles[0];
		if (_pinnedIdentity == "local") {
			focusTile = _localTile;
		} else if (!_pinnedIdentity.isEmpty()) {
			auto it = _remoteTiles.find(_pinnedIdentity);
			if (it != _remoteTiles.end()) focusTile = it->second.get();
		} else {
			for (auto *t : allTiles) {
				if (t->isSpeaking()) {
					focusTile = t;
					break;
				}
			}
			if (focusTile == _localTile && allTiles.size() > 1) {
				focusTile = allTiles[1];
			}
		}

		std::vector<VideoTileWidget*> otherTiles;
		for (auto *t : allTiles) {
			if (t != focusTile) otherTiles.push_back(t);
		}

		const int margin = 8;
		const int gap = 8;
		const int filmstripW = std::clamp(stageW * 24 / 100, 180, 260);
		const int mainW = stageW - filmstripW - gap - margin * 2;
		const int mainH = stageH - margin * 2;

		focusTile->setPipMode(false);
		focusTile->setGeometry(margin, margin, mainW, mainH);
		focusTile->show();

		const int numOthers = static_cast<int>(otherTiles.size());
		const int smallH = (mainH - (numOthers - 1) * gap) / std::max(1, numOthers);
		const int clampedH = std::clamp(smallH, 90, filmstripW * 9 / 16);

		for (int i = 0; i < numOthers; ++i) {
			otherTiles[i]->setPipMode(false);
			otherTiles[i]->setGeometry(margin + mainW + gap, margin + i * (clampedH + gap), filmstripW, clampedH);
			otherTiles[i]->show();
		}
		syncDx11CanvasLayout(allTiles);
		return;
	}

	// 3. 自适应 16:9 最优画廊宫格模式 (Optimal Gallery Grid)
	const int margin = 8;
	const int gap = 8;
	int bestRows = 1, bestCols = 1;
	int bestTileW = 0, bestTileH = 0;
	double maxArea = 0.0;

	for (int cols = 1; cols <= N; ++cols) {
		int rows = (N + cols - 1) / cols;
		int availW = stageW - margin * 2 - (cols - 1) * gap;
		int availH = stageH - margin * 2 - (rows - 1) * gap;
		if (availW <= 0 || availH <= 0) continue;

		int maxW = availW / cols;
		int maxH = availH / rows;

		int tW = maxW;
		int tH = maxH;
		if (static_cast<double>(maxW) / maxH > 16.0 / 9.0) {
			tW = static_cast<int>(maxH * 16.0 / 9.0);
			tH = maxH;
		} else {
			tW = maxW;
			tH = static_cast<int>(maxW * 9.0 / 16.0);
		}

		double area = static_cast<double>(tW) * tH;
		if (area > maxArea) {
			maxArea = area;
			bestRows = rows;
			bestCols = cols;
			bestTileW = tW;
			bestTileH = tH;
		}
	}

	const int totalGridH = bestRows * bestTileH + (bestRows - 1) * gap;
	const int startY = (stageH - totalGridH) / 2;

	int tileIdx = 0;
	for (int r = 0; r < bestRows && tileIdx < N; ++r) {
		int itemsInRow = std::min(bestCols, N - r * bestCols);
		int rowW = itemsInRow * bestTileW + (itemsInRow - 1) * gap;
		int startX = (stageW - rowW) / 2;

		for (int c = 0; c < itemsInRow && tileIdx < N; ++c) {
			QRect geom(startX + c * (bestTileW + gap), startY + r * (bestTileH + gap), bestTileW, bestTileH);
			allTiles[tileIdx]->setPipMode(false);
			allTiles[tileIdx]->setGeometry(geom);
			allTiles[tileIdx]->show();
			++tileIdx;
		}
	}
	syncDx11CanvasLayout(allTiles);
}

void MeetingRoomWindow::paintEvent(QPaintEvent *e) {
	QPainter p(this);
	p.fillRect(rect(), QColor(0x12, 0x14, 0x1a));
}

void MeetingRoomWindow::receiveRemoteVideoFrame(const QImage &frame, const QString &user) {
	if (user.isEmpty()) return;
	if (_remoteTiles.find(user) == _remoteTiles.end()) {
		onRemoteParticipantJoined(user);
	}
	auto it = _remoteTiles.find(user);
	if (it != _remoteTiles.end() && it->second) {
		it->second->setFrame(frame);
		if (!it->second->isVideoActive()) {
			it->second->setVideoActive(true);
			LogToConsole(LogCategory::WebRTC, "RECV_FRAME", QString("收到远端 [%1] 解码视频流 (%2x%3) - 开始渲染").arg(user).arg(frame.width()).arg(frame.height()));
			updateVideoLayout();
		}
	}
}

void MeetingRoomWindow::receiveLocalVideoFrame(const QImage &frame) {
	if (_localTile) {
		_localTile->setFrame(frame);
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

void MeetingRoomWindow::setupCoordinatorBindings() {
	if (!_coordinator) return;

	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::remoteVideoTrackAvailable,
	        this, [this](const QString &id, std::shared_ptr<livekit::Track> track) {
			onRemoteParticipantJoined(id, id);
			if (_remoteRenderSession) {
				_remoteRenderSession->AttachRemoteTrack(track, id.toStdString());
			}
		});
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::remoteVideoTrackUnavailable,
	        this, [this](const QString &identity, const QString &trackSid) {
			if (_remoteRenderSession) {
				_remoteRenderSession->RemoveTrack(trackSid.toStdString());
			}
			if (_dx11Canvas) {
				_dx11Canvas->removeUser(identity.toStdString());
			}
		});
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::participantJoined,
	        this, [this](const QString &id, const QString &name) {
		onRemoteParticipantJoined(id, name);
	});
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::participantLeft,
	        this, &MeetingRoomWindow::onRemoteParticipantLeft);
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::remoteTrackMuted,
	        this, [this](const QString &id, bool isVideo, bool muted) {
		onRemoteTrackMuted(id, isVideo, muted);
	});
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::activeSpeakersChanged,
	        this, &MeetingRoomWindow::updateActiveSpeakers);
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
			? info.name
			: QString::fromUtf8("%1 - 会议号: %2").arg(info.name, meetingId));
	});
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::participantsUpdated,
	        this, [this](const std::vector<OpenMeeting::ParticipantInfo> &list) {
		_participantCount = static_cast<int>(list.size());
		_bottomBar->setParticipantCount(_participantCount);
		for (const auto &p : list) {
			if (p.isLocal) {
				if (_localTile) {
					_localTile->setConnectionQuality(p.connectionQuality);
					_localTile->setVideoStreamPaused(p.isVideoStreamPaused);
				}
				continue;
			}
			auto it = _remoteTiles.find(p.identity);
			if (it != _remoteTiles.end() && it->second) {
				if (!p.name.isEmpty() && it->second->displayName() != p.name) {
					it->second->setDisplayName(p.name);
				}
				it->second->setConnectionQuality(p.connectionQuality);
				it->second->setVideoStreamPaused(p.isVideoStreamPaused);
			}
		}
	});
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::trackSubscriptionPermissionChanged,
	        this, [this](const QString &identity, const QString &participantSid,
	                     const QString &trackSid, bool allowed) {
		LogToConsole(LogCategory::Connection, "SUBSCRIPTION_PERMISSION",
			QString("订阅权限更新: participant=%1 sid=%2 track=%3 allowed=%4")
				.arg(identity.isEmpty() ? QString::fromUtf8("未知") : identity,
					 participantSid, trackSid, allowed ? QString::fromUtf8("是") : QString::fromUtf8("否")));
	});
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::localAudioMuteChanged,
	        this, [this](bool muted) {
		_config.audioMuted = muted;
		_bottomBar->setAudioMuted(muted);
		_localTile->setAudioMuted(muted);
		if (_wasapiCap) {
			_wasapiCap->SetMute(muted);
		}
		if (_localAudioTrack) {
			_localAudioTrack->set_muted(muted);
		}
		if (_room) {
			auto local = _room->local_participant();
			if (local && _localAudioTrack) {
				local->SetMuted(_localAudioTrack->sid(), muted);
			}
		}
	});
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::localVideoEnableChanged,
	        this, [this](bool enabled) {
		_config.videoEnabled = enabled;
		_bottomBar->setVideoEnabled(enabled);
		_localTile->setVideoActive(enabled && _usingRealCamera);
		if (_localVideoTrack) {
			_localVideoTrack->set_muted(!enabled);
		}
		if (_room) {
			auto local = _room->local_participant();
			if (local && _localVideoTrack) {
				local->SetMuted(_localVideoTrack->sid(), !enabled);
			}
		}
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
	        this, [this](OpenMeeting::MeetingState state, const QString &) {
		if (state == OpenMeeting::MeetingState::InMeeting) {
			_room = _coordinator->room();
			if (_room) {
				auto remotes = _room->remote_participants();
				for (const auto &[sid, p] : remotes) {
					if (!p) continue;
					const QString pId = QString::fromStdString(p->identity());
					onRemoteParticipantJoined(pId, QString::fromStdString(p->name()));
					if (_remoteRenderSession) {
						for (const auto &[tsid, pub] : p->tracks()) {
							if (pub && pub->track() && pub->track()->kind() == livekit::TrackKind::Video) {
								_remoteRenderSession->AttachRemoteTrack(pub->track(), p->identity());
							}
						}
					}
				}
			}
		}
	});
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::errorOccurred,
	        this, [this](const QString &title, const QString &message) {
		if (!_coordinator || _coordinator->state() != OpenMeeting::MeetingState::Failed) {
			return;
		}
		LogToConsole(LogCategory::Error, "SESSION_STARTUP", QString("%1: %2").arg(title, message));
		QMessageBox::critical(this, title, message);
		close();
	});
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::meetingLeft,
	        this, [this]() {
		close();
	});
	connect(&OpenMeeting::SessionManager::instance(),
	        &OpenMeeting::SessionManager::sessionInvalidated,
	        this,
	        &MeetingRoomWindow::onSessionInvalidated,
	        Qt::QueuedConnection);

	if (!_coordinator->currentMeetingId().isEmpty()) {
		if (_topBar) _topBar->setMeetingId(_coordinator->currentMeetingId());
		setWindowTitle(QString::fromUtf8("会议 - 会议号: %1").arg(_coordinator->currentMeetingId()));
	}
}

void MeetingRoomWindow::onKickedOff(const QString &reason, int reasonCode) {
	if (_closingForSessionInvalidation ||
		OpenMeeting::SessionManager::instance().isSessionInvalidating()) {
		return;
	}
	QMessageBox::warning(this, QString::fromUtf8("移出会议"),
	                     QString::fromUtf8("您已被主持人移出会议。\n原因: %1 (代码: %2)")
	                     .arg(reason.isEmpty() ? QString::fromUtf8("未指定") : reason).arg(reasonCode));
	close();
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

	LogToConsole(LogCategory::Connection, "DUPLICATE_IDENTITY",
	             "[UI] Show duplicate login dialog");
	// QMessageBox::warning 是模态调用；用户确认前不会关闭会议窗口，避免
	// 服务器主动踢出表现为无提示的窗口消失。
	QMessageBox::warning(this,
	                     QString::fromUtf8("会议已退出"),
	                     QString::fromUtf8("您的账号已在其他设备加入此会议，当前客户端已被强制退出。"));
	close();
}

void MeetingRoomWindow::onSessionInvalidated(OpenMeeting::SessionInvalidationReason reason) {
	if (_closingForSessionInvalidation) {
		return;
	}
	_closingForSessionInvalidation = true;
	LogToConsole(LogCategory::Connection, "SESSION_INVALIDATED",
	             QString("[UI] Close meeting window for invalidated account session, reason=%1")
	                 .arg(static_cast<int>(reason)));

	// 不在会议窗口重复弹窗；全局提示和重新登录由 MeetingMainWindow 统一负责。
	close();
}

void MeetingRoomWindow::onRemoteMuteRequested(bool isVideo, bool mute, const QString &operatorId) {
	if (isVideo) {
		if (mute) {
			_bottomBar->setVideoEnabled(false);
			_config.videoEnabled = false;
			_localTile->setVideoActive(false);
			if (_coordinator) _coordinator->setLocalVideoEnabled(false);
			updateVideoLayout();
			LogToConsole(LogCategory::Media, "VIDEO", QString("主持人 [%1] 已关闭您的摄像头").arg(operatorId));
		} else {
			if (QMessageBox::question(this, QString::fromUtf8("开启摄像头请求"),
				QString::fromUtf8("主持人 [%1] 邀请您开启摄像头，是否同意？").arg(operatorId),
				QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
				_bottomBar->setVideoEnabled(true);
				_config.videoEnabled = true;
				_localTile->setVideoActive(_usingRealCamera);
				if (_coordinator) _coordinator->setLocalVideoEnabled(true);
				updateVideoLayout();
			}
		}
	} else {
		if (mute) {
			_bottomBar->setAudioMuted(true);
			_config.audioMuted = true;
			_localTile->setAudioMuted(true);
			if (_wasapiCap) _wasapiCap->SetMute(true);
			if (_coordinator) _coordinator->setLocalAudioMuted(true);
			LogToConsole(LogCategory::Media, "AUDIO", QString("主持人 [%1] 已将您静音").arg(operatorId));
		} else {
			if (QMessageBox::question(this, QString::fromUtf8("解除静音请求"),
				QString::fromUtf8("主持人 [%1] 邀请您开启麦克风发言，是否同意？").arg(operatorId),
				QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
				_bottomBar->setAudioMuted(false);
				_config.audioMuted = false;
				_localTile->setAudioMuted(false);
				if (_wasapiCap) _wasapiCap->SetMute(false);
				if (_coordinator) _coordinator->setLocalAudioMuted(false);
			}
		}
	}
}

void MeetingRoomWindow::onMeetingDetailUpdated(const OpenMeeting::MeetingDetail &detail) {
	if (!detail.meetingName.isEmpty()) {
		setWindowTitle(QString::fromUtf8("%1 - 会议号: %2").arg(detail.meetingName).arg(detail.meetingId));
	}
	if (_topBar && !detail.meetingId.isEmpty()) {
		_topBar->setMeetingId(detail.meetingId);
	}
	if (_participantsSidebar) {
		_participantsSidebar->updateParticipants(_coordinator ? _coordinator->participants() : std::vector<OpenMeeting::ParticipantInfo>{});
	}
}

void MeetingRoomWindow::onHostRoleChanged(const QString &newHostId, const QString &operatorName) {
	LogToConsole(LogCategory::Participant, "HOST", QString("主持人身份已移交至: %1 (操作人: %2)").arg(newHostId).arg(operatorName));
	QMessageBox::information(this, QString::fromUtf8("主持人变更"),
	                         QString::fromUtf8("参会人 [%1] 已成为新的主持人").arg(newHostId));
}

void MeetingRoomWindow::handleEndMeetingClicked() {
	if (_coordinator && _coordinator->isHost()) {
		QMessageBox box(this);
		box.setWindowTitle(QString::fromUtf8("结束会议"));
		box.setText(QString::fromUtf8("您是本次会议的主持人，请选择退出方式："));
		auto *leaveBtn = box.addButton(QString::fromUtf8("仅离开会议"), QMessageBox::ActionRole);
		auto *endBtn = box.addButton(QString::fromUtf8("结束全体会议"), QMessageBox::DestructiveRole);
		auto *cancelBtn = box.addButton(QString::fromUtf8("取消"), QMessageBox::RejectRole);
		box.exec();
		if (box.clickedButton() == leaveBtn) {
			_coordinator->leaveMeetingAsync(false);
			close();
		} else if (box.clickedButton() == endBtn) {
			_coordinator->leaveMeetingAsync(true);
			close();
		}
	} else {
		if (QMessageBox::question(this, QString::fromUtf8("离开会议"),
			QString::fromUtf8("您确定要离开当前会议吗？"),
			QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
			if (_coordinator) {
				_coordinator->leaveMeetingAsync(false);
			}
			close();
		}
	}
}

void MeetingRoomWindow::startLiveKitSession() {
	if (!_coordinator) return;

	_sessionRunning = true;

	if (_coordinator->state() != OpenMeeting::MeetingState::Idle) {
		// Coordinator 已经在执行入会流程中 (Validating, ConnectingRoom, InMeeting 等)，
		// 严禁在此处重复发起连接打断已有流程！
		_room = _coordinator->room();
		if (_room && _coordinator->state() == OpenMeeting::MeetingState::InMeeting) {
			auto remotes = _room->remote_participants();
			for (const auto &[sid, p] : remotes) {
				if (!p) continue;
				onRemoteParticipantJoined(QString::fromStdString(p->identity()), QString::fromStdString(p->name()));
				if (_remoteRenderSession) {
					for (const auto &[track_sid, publication] : p->tracks()) {
						if (publication && publication->track() &&
							publication->track()->kind() == livekit::TrackKind::Video) {
							_remoteRenderSession->AttachRemoteTrack(publication->track(), p->identity());
						}
					}
				}
			}
		}
		return;
	}

	if (_config.serverUrl.isEmpty()) {
		LogToConsole(LogCategory::General, "SESSION", "未指定服务器地址，运行在单机演示模式");
		return;
	}

	OpenMeeting::MediaPreferences prefs;
	prefs.enableMicrophone = !_config.audioMuted;
	prefs.enableVideo = _config.videoEnabled;

	_coordinator->connectDirectlyAsync(_config.serverUrl, _config.token, "direct", _config.displayName, prefs);
	_room = _coordinator->room();
}

void MeetingRoomWindow::stopLiveKitSession() {
	if (!_sessionRunning.exchange(false)) {
		return;
	}

	if (_meetingTimer) _meetingTimer->stop();
	if (_remoteRenderTimer) _remoteRenderTimer->stop();
	if (_remoteRenderSession) _remoteRenderSession->Deactivate();
	_usingDx11Backend.store(false, std::memory_order_release);
	if (_dx11Canvas) {
		_dx11Canvas->clearUsers();
		_dx11Canvas->hide();
	}

	if (_wasapiCap) {
		_wasapiCap->Stop();
		_wasapiCap.reset();
	}
	if (_dshowCap) {
		_dshowCap->Stop();
		_dshowCap.reset();
	}

	if (_coordinator && _coordinator->state() != OpenMeeting::MeetingState::Failed) {
		_coordinator->leaveMeetingAsync(false);
	}
	LogToConsole(LogCategory::Connection, "DISCONNECT", "已退出会议视窗并停止媒体采集");
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
	case WM_NCCALCSIZE: {
		if (msg->wParam == TRUE) {
			*result = 0;
			return true;
		}
	} break;

	case WM_NCHITTEST: {
		if (!handle) break;

		POINT p{ GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam) };
		ScreenToClient(handle, &p);

		const qreal ratio = devicePixelRatioF();
		const int x = static_cast<int>(p.x / ratio);
		const int y = static_cast<int>(p.y / ratio);

		const int w = width();
		const int h = height();
		const int border = 8;

		if (!isMaximized() && !isFullScreen()) {
			const bool left = (x < border);
			const bool right = (x >= w - border);
			const bool top = (y < border);
			const bool bottom = (y >= h - border);

			if (top && left) { *result = HTTOPLEFT; return true; }
			if (top && right) { *result = HTTOPRIGHT; return true; }
			if (bottom && left) { *result = HTBOTTOMLEFT; return true; }
			if (bottom && right) { *result = HTBOTTOMRIGHT; return true; }
			if (left) { *result = HTLEFT; return true; }
			if (right) { *result = HTRIGHT; return true; }
			if (top) { *result = HTTOP; return true; }
			if (bottom) { *result = HTBOTTOM; return true; }
		}

		if (y < 44 && x < w - 420 && (x < (w - 220) / 2 || x > (w + 220) / 2)) {
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

} // namespace MeetingUI
