#include "src/ui/meeting_room_window.h"
#include "src/ui/meeting_log_console.h"
#include "src/media/media_converters.h"
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QApplication>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QFont>
#include <QtGui/QClipboard>
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
		std::vector<uint8_t> rgba(w * h * 4);
		livekit::MediaConverters::ConvertNV12ToRGBA(frame.data(), rgba.data(), w, h);
		return QImage(rgba.data(), w, h, w * 4, QImage::Format_RGBA8888).copy();
	} else if (frame.type() == livekit::VideoBufferType::I420) {
		const uint8_t *y_plane = frame.data();
		const uint8_t *u_plane = y_plane + (w * h);
		const uint8_t *v_plane = u_plane + ((w / 2) * (h / 2));
		std::vector<uint8_t> rgba(w * h * 4);

		auto clamp8 = [](int val) -> uint8_t {
			return static_cast<uint8_t>(val < 0 ? 0 : (val > 255 ? 255 : val));
		};

		for (int y = 0; y < h; ++y) {
			for (int x = 0; x < w; ++x) {
				int y_val = y_plane[y * w + x];
				int u_val = u_plane[(y / 2) * (w / 2) + (x / 2)];
				int v_val = v_plane[(y / 2) * (w / 2) + (x / 2)];
				int c = y_val - 16;
				int d = u_val - 128;
				int e_val = v_val - 128;

				int r = clamp8((298 * c + 409 * e_val + 128) >> 8);
				int g = clamp8((298 * c - 100 * d - 208 * e_val + 128) >> 8);
				int b = clamp8((298 * c + 516 * d + 128) >> 8);
				int idx = (y * w + x) * 4;
				rgba[idx] = r;
				rgba[idx + 1] = g;
				rgba[idx + 2] = b;
				rgba[idx + 3] = 255;
			}
		}
		return QImage(rgba.data(), w, h, w * 4, QImage::Format_RGBA8888).copy();
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

	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0x00, 0xb4, 0x2a));
	p.drawRect(bx, by + 6, 2, 4);
	p.drawRect(bx + 4, by + 3, 2, 7);
	p.drawRect(bx + 8, by, 2, 10);

	if (_isPinned) {
		p.setFont(QFont("Segoe UI Emoji", 10));
		p.setPen(Qt::white);
		p.drawText(QRect(bx + 16, by - 2, 16, 16), Qt::AlignCenter, QString::fromUtf8("📌"));
	}
	p.restore();
}

void VideoTileWidget::drawVideoFrame(QPainter &p, const QRect &r) {
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
	const int rightButtonsLeft = rightX;
	const int availCenterW = rightButtonsLeft - leftInfoRight - 16;

	if (availCenterW >= 180) {
		const int pillW = std::min(availCenterW, 200);
		int pillX = (w - pillW) / 2;
		pillX = std::max(leftInfoRight + 8, std::min(pillX, rightButtonsLeft - 8 - pillW));
		_speakerCapsuleRect = QRect(pillX, (h - 26) / 2, pillW, 26);
	} else if (availCenterW >= 110) {
		const int pillW = availCenterW;
		const int pillX = leftInfoRight + 8;
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

	if (next != _hoverBtn) {
		_hoverBtn = next;
		update();
	}
}

void RoomTopBarWidget::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
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
		}
	}
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

MeetingRoomWindow::MeetingRoomWindow(const Config &config, QWidget *parent)
	: Ui::RpWidget(parent)
	, _config(config) {
	setObjectName("MeetingRoomWindow");
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

	// 3. 创建本地音频与视频数据源
	_localAudioSource = std::make_shared<livekit::AudioSource>(48000, 2);
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

	_localVideoSource = std::make_shared<livekit::VideoSource>(1280, 720);
	_localVideoSource->addSink([this](const livekit::VideoFrame &frame, const livekit::VideoCaptureOptions &) {
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
	_stageContainer = new QWidget(this);
	_stageContainer->setStyleSheet("background-color: #12141a;");
	_bottomBar = new RoomBottomBarWidget(this);

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
		QString userList = QString::fromUtf8("当前参会成员列表 (%1人)：\n1. %2 (我 - 本地)")
			.arg(_participantCount).arg(_config.displayName);
		int uIdx = 2;
		for (const auto &[id, tile] : _remoteTiles) {
			if (tile) {
				userList += QString::fromUtf8("\n%1. %2 (远端参会人)").arg(uIdx++).arg(tile->displayName());
			}
		}
		QMessageBox::information(this, QString::fromUtf8("参会成员"), userList);
	}, lifetime());

	_bottomBar->chatClicked() | rpl::on_next([this] {
		QMessageBox::information(this, QString::fromUtf8("会议聊天"),
			QString::fromUtf8("聊天通道已激活，可在左下角输入框发送快捷弹幕或消息。"));
	}, lifetime());

	_bottomBar->recordClicked() | rpl::on_next([this] {
		QMessageBox::information(this, QString::fromUtf8("云端录制"),
			QString::fromUtf8("正在将本次会议视频流实时转码存档至高可用存储。"));
	}, lifetime());

	_bottomBar->appsClicked() | rpl::on_next([this] {
		QMessageBox::information(this, QString::fromUtf8("会议应用"),
			QString::fromUtf8("可用应用：互动白板、投票调查、同声传译、计时器。"));
	}, lifetime());

	_bottomBar->endMeetingClicked() | rpl::on_next([this] {
		if (QMessageBox::question(this, QString::fromUtf8("离开会议"),
			QString::fromUtf8("您确定要离开或结束当前会议吗？"),
			QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
			close();
		}
	}, lifetime());

	_bottomBar->sendChatRequested() | rpl::on_next([this](const QString &text) {
		_topBar->setActiveSpeaker(QString::fromUtf8("%1: %2").arg(_config.displayName).arg(text));
		LogToConsole(LogCategory::Participant, "CHAT", QString("%1: %2").arg(_config.displayName).arg(text));
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

void MeetingRoomWindow::resizeEvent(QResizeEvent *e) {
	const int w = width();
	const int h = height();

	_topBar->setGeometry(0, 0, w, 44);
	_stageContainer->setGeometry(0, 44, w, h - 44 - 76);
	_bottomBar->setGeometry(0, h - 76, w, 76);

	updateVideoLayout();
}

void MeetingRoomWindow::onRemoteParticipantJoined(const QString &identity) {
	if (identity.isEmpty()) return;

	auto it = _remoteTiles.find(identity);
	if (it == _remoteTiles.end()) {
		auto tile = std::make_unique<VideoTileWidget>(identity, false, _stageContainer);
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

	_participantCount = 1 + static_cast<int>(_remoteTiles.size());
	if (_bottomBar) {
		_bottomBar->setParticipantCount(_participantCount);
	}
	if (_topBar) {
		_topBar->setActiveSpeaker(QString::fromUtf8("%1 已加入").arg(identity));
	}

	LogToConsole(LogCategory::Participant, "USER_JOIN", QString("远端参会人已加入: %1 (当前房间总人数: %2)").arg(identity).arg(_participantCount));
	updateVideoLayout();
}

void MeetingRoomWindow::onRemoteParticipantLeft(const QString &identity) {
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
	if (_topBar) {
		_topBar->setActiveSpeaker(QString());
	}

	LogToConsole(LogCategory::Participant, "USER_LEFT", QString("远端参会人已离开: %1 (当前房间总人数: %2)").arg(identity).arg(_participantCount));
	updateVideoLayout();
}

void MeetingRoomWindow::onRemoteTrackMuted(bool isVideo, bool muted) {
	for (auto &[id, tile] : _remoteTiles) {
		if (tile) {
			if (isVideo) {
				tile->setVideoActive(!muted);
				if (muted) tile->setFrame(QImage());
			} else {
				tile->setAudioMuted(muted);
				if (muted) tile->setSpeaking(false, 0.0f);
			}
		}
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
	_inviteHintBanner->setVisible(!hasRemote && !localActive);

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

void MeetingRoomWindow::startLiveKitSession() {
	if (_config.serverUrl.isEmpty()) {
		LogToConsole(LogCategory::General, "SESSION", "未指定服务器地址，运行在单机演示模式");
		return;
	}

	_sessionRunning = true;
	_ioContext = std::make_unique<asio::io_context>();
	_workGuard = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(_ioContext->get_executor());

	_room = livekit::Room::Create(_ioContext->get_executor());

	_room->SetLogHandler([](const std::string &cat, const std::string &tag, const std::string &msg) {
		LogCategory c = LogCategory::General;
		if (cat == "WEBRTC") c = LogCategory::WebRTC;
		else if (cat == "SIGNAL") c = LogCategory::Signal;
		else if (cat == "TRACK") c = LogCategory::Track;
		else if (cat == "ERROR") c = LogCategory::Error;
		else if (cat == "MEDIA") c = LogCategory::Media;
		LogToConsole(c, QString::fromStdString(tag), QString::fromStdString(msg));
	});

	LogToConsole(LogCategory::Connection, "CONNECTING", QString("正在连接 LiveKit 服务器: %1 ...").arg(_config.serverUrl));

	class AppRoomListener : public livekit::RoomListener {
	public:
		explicit AppRoomListener(MeetingRoomWindow *w) : _window(w) {}

		void OnConnected() override {
			LogToConsole(LogCategory::Connection, "CONNECTED", "LiveKit 房间连接成功！");
			if (!_window) return;

			if (_window->_room) {
				auto remotes = _window->_room->remote_participants();
				for (const auto &[sid, p] : remotes) {
					if (p) {
						QString id = QString::fromStdString(p->identity());
						QMetaObject::invokeMethod(_window, [this, id]() {
							_window->onRemoteParticipantJoined(id);
						}, Qt::QueuedConnection);
					}
				}
			}
		}

		void OnParticipantConnected(std::shared_ptr<livekit::RemoteParticipant> p) override {
			if (p && _window) {
				QString id = QString::fromStdString(p->identity());
				LogToConsole(LogCategory::Participant, "REMOTE_JOIN", QString("参会人加入: %1 (SID: %2)").arg(id).arg(QString::fromStdString(p->sid())));
				QMetaObject::invokeMethod(_window, [this, id]() {
					_window->onRemoteParticipantJoined(id);
				}, Qt::QueuedConnection);
			}
		}

		void OnParticipantDisconnected(std::shared_ptr<livekit::RemoteParticipant> p) override {
			if (p && _window) {
				QString id = QString::fromStdString(p->identity());
				LogToConsole(LogCategory::Participant, "REMOTE_LEFT", QString("参会人离开: %1").arg(id));
				QMetaObject::invokeMethod(_window, [this, id]() {
					_window->onRemoteParticipantLeft(id);
				}, Qt::QueuedConnection);
			}
		}

		void OnTrackSubscribed(std::shared_ptr<livekit::Track> track,
		                       std::shared_ptr<livekit::TrackPublication> publication,
		                       std::shared_ptr<livekit::RemoteParticipant> participant) override {
			if (!track || !participant || !_window) return;

			std::string identity = participant->identity();
			LogToConsole(LogCategory::Track, "SUBSCRIBED", QString("订阅到用户 %1 的轨道: %2 (%3)")
				.arg(QString::fromStdString(identity))
				.arg(QString::fromStdString(track->name()))
				.arg(track->kind() == livekit::TrackKind::Video ? "VIDEO" : "AUDIO"));

			if (track->kind() == livekit::TrackKind::Video) {
				auto has_logged = std::make_shared<std::atomic<bool>>(false);
				track->addVideoSink([this, identity, has_logged](const livekit::VideoFrame &frame, const livekit::VideoCaptureOptions &) {
					if (!has_logged->exchange(true)) {
						LogToConsole(LogCategory::WebRTC, "SINK_FRAME", QString("收到来自 [%1] 的远端视频画面 (%2x%3 RGBA)")
							.arg(QString::fromStdString(identity)).arg(frame.width()).arg(frame.height()));
					}
					QImage img = VideoFrameToQImage(frame);
					if (!img.isNull() && _window) {
						QMetaObject::invokeMethod(_window, [this, img = std::move(img), id = QString::fromStdString(identity)]() {
							_window->receiveRemoteVideoFrame(img, id);
						}, Qt::QueuedConnection);
					}
				});
			} else if (track->kind() == livekit::TrackKind::Audio) {
				LogToConsole(LogCategory::Media, "AUDIO", QString("参会人 [%1] 音频轨已挂载至 WebRTC 原生混音播放管线 (WASAPI Playout ADM 活跃中)").arg(QString::fromStdString(identity)));
			}
		}

		void OnTrackUnsubscribed(std::shared_ptr<livekit::Track> track,
		                         std::shared_ptr<livekit::TrackPublication> publication,
		                         std::shared_ptr<livekit::RemoteParticipant> participant) override {
			if (!track || !_window) return;
			LogToConsole(LogCategory::Track, "UNSUBSCRIBED", QString("取消订阅轨道: %1").arg(QString::fromStdString(track->name())));
			if (track->kind() == livekit::TrackKind::Video) {
				QMetaObject::invokeMethod(_window, [this]() {
					_window->onRemoteTrackMuted(true, true);
				}, Qt::QueuedConnection);
			}
		}

		void OnTrackUnpublished(std::shared_ptr<livekit::RemoteParticipant> participant,
		                        std::shared_ptr<livekit::TrackPublication> publication) override {
			if (!publication || !_window) return;
			LogToConsole(LogCategory::Track, "UNPUBLISHED", QString("远端用户取消发布轨道: %1").arg(QString::fromStdString(publication->sid())));
			if (publication->track() && publication->track()->kind() == livekit::TrackKind::Video) {
				QMetaObject::invokeMethod(_window, [this]() {
					_window->onRemoteTrackMuted(true, true);
				}, Qt::QueuedConnection);
			}
		}

		void OnTrackMuted(std::shared_ptr<livekit::Participant> participant,
		                  std::shared_ptr<livekit::TrackPublication> publication,
		                  bool muted) override {
			if (!publication || !publication->track() || !_window) return;
			const bool isVideo = (publication->track()->kind() == livekit::TrackKind::Video);
			QMetaObject::invokeMethod(_window, [this, isVideo, muted]() {
				_window->onRemoteTrackMuted(isVideo, muted);
			}, Qt::QueuedConnection);
		}

		void OnActiveSpeakersChanged(const std::vector<std::shared_ptr<livekit::Participant>> &speakers) override {
			if (!_window) return;
			QMetaObject::invokeMethod(_window, [this, speakers]() {
				_window->updateActiveSpeakers(speakers);
			}, Qt::QueuedConnection);
		}

	private:
		MeetingRoomWindow *_window;
	};

	_roomListener = std::make_shared<AppRoomListener>(this);
	_room->AddListener(_roomListener);

	const std::string urlStr = _config.serverUrl.toStdString();
	const std::string tokenStr = _config.token.toStdString();

	_ioThread = std::thread([this, urlStr, tokenStr] {
		livekit::SignalOptions opts;
		opts.auto_subscribe = true;
		opts.connect_timeout = std::chrono::seconds(10);

		asio::co_spawn(*_ioContext, [this, urlStr, tokenStr, opts]() -> asio::awaitable<void> {
			try {
				if (!urlStr.empty() && !tokenStr.empty()) {
                    co_await _room->ConnectAsync(urlStr, tokenStr, opts);
                    {
                        auto local = _room->local_participant();
                        if (local) {
							// 自动发布本地音频轨
							_localAudioTrack = livekit::LocalAudioTrack::createLocalAudioTrack("simple_audio", _localAudioSource);
							_localAudioTrack->set_muted(_config.audioMuted);
                            co_await local->PublishTrackAsync(_localAudioTrack);
							LogToConsole(LogCategory::Track, "PUBLISH", QString("已向房间发布 LocalAudioTrack (simple_audio, 初始: %1)").arg(_config.audioMuted ? "静音" : "开麦"));

							// 自动发布本地视频轨
							livekit::VideoPublishOptions vopts;
							vopts.video_codec = _config.videoCodec.toStdString();
							if (!_config.backupCodec.isEmpty()) {
								vopts.backup_codec = _config.backupCodec.toStdString();
							}
							vopts.backup_codec_policy = _config.backupCodecPolicy;
							vopts.auto_backup_codec = true;

							_localVideoTrack = livekit::LocalVideoTrack::createLocalVideoTrack("camera_video", _localVideoSource, livekit::TrackSource::Camera, vopts);
							_localVideoTrack->set_muted(!_config.videoEnabled);
                            co_await local->PublishTrackAsync(_localVideoTrack);
							LogToConsole(LogCategory::Track, "PUBLISH", QString("已向房间发布 LocalVideoTrack (camera_video, 编码: %1, 备用: %2, 初始: %3)")
								.arg(_config.videoCodec)
								.arg(_config.backupCodec.isEmpty() ? "None" : _config.backupCodec)
								.arg(_config.videoEnabled ? "开启" : "关闭"));
						}
                    }
				}
			} catch (const std::exception &ex) {
				LogToConsole(LogCategory::Error, "EXCEPTION", QString("连接异常: %1").arg(ex.what()));
			}
		}, asio::detached);

		_ioContext->run();
	});
}

void MeetingRoomWindow::stopLiveKitSession() {
	if (!_sessionRunning.exchange(false)) {
		return; // 已经停止或正在停止，防止 closeEvent 和析构函数重复执行
	}

	if (_meetingTimer) _meetingTimer->stop();

	if (_wasapiCap) {
		_wasapiCap->Stop();
		_wasapiCap.reset();
	}
	if (_dshowCap) {
		_dshowCap->Stop();
		_dshowCap.reset();
	}

	if (_room) {
		_room->Disconnect();
		LogToConsole(LogCategory::Connection, "DISCONNECT", "已断开 LiveKit 房间连接并释放媒体资源");
	}
	if (_workGuard) {
		_workGuard->reset();
	}
	if (_ioContext) {
		_ioContext->stop();
	}
	if (_ioThread.joinable()) {
		_ioThread.join();
	}
	_room.reset();
	_ioContext.reset();
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
	HWND handle = msg->hwnd ? msg->hwnd : _handle;

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
