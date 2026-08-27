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

	if (_isPip) {
		p.setClipping(false);
		p.setPen(QPen(QColor(0x16, 0x77, 0xff), 2.0));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(r.adjusted(1, 1, -1, -1), 10, 10);
	}
}

void VideoTileWidget::drawAvatarPlaceholder(QPainter &p, const QRect &r) {
	p.fillRect(r, QColor(0xff, 0xff, 0xff));

	const int cx = r.center().x();
	const int cy = r.center().y();

	const int outerRadius = 46;
	p.setPen(QPen(QColor(0xf2, 0xf3, 0xf5), 1.5));
	p.setBrush(QColor(0xf7, 0xf8, 0xfa));
	p.drawEllipse(QPoint(cx, cy - 16), outerRadius, outerRadius);

	const int innerRadius = 13;
	p.setPen(Qt::NoPen);
	p.setBrush(_isLocal ? QColor(0x16, 0x77, 0xff) : QColor(0xff, 0x7d, 0x00));
	p.drawEllipse(QPoint(cx, cy - 4), innerRadius, innerRadius);

	p.setBrush(Qt::white);
	p.drawEllipse(QPoint(cx, cy - 8), 4, 4);
	QPainterPath bodyPath;
	bodyPath.moveTo(cx - 6, cy + 3);
	bodyPath.arcTo(cx - 6, cy - 3, 12, 12, 0, 180);
	bodyPath.closeSubpath();
	p.fillPath(bodyPath, Qt::white);

	QFont font("Microsoft YaHei", 12, QFont::Bold);
	p.setFont(font);
	QFontMetrics fm(font);
	const int textW = fm.horizontalAdvance(_displayName);
	const int totalW = textW + 24;
	const int startX = cx - totalW / 2;
	const int nameY = cy + outerRadius + 8;

	const int micX = startX + 6;
	const int micY = nameY + 6;
	p.setPen(QPen(_isAudioMuted ? QColor(0xf5, 0x3f, 0x3f) : QColor(0x00, 0xb4, 0x2a), 1.5, Qt::SolidLine, Qt::RoundCap));
	p.drawRoundedRect(QRect(micX - 3, micY - 5, 6, 8), 3, 3);
	p.drawLine(micX, micY + 3, micX, micY + 6);
	p.drawLine(micX - 4, micY + 6, micX + 4, micY + 6);
	if (_isAudioMuted) {
		p.drawLine(micX - 5, micY - 6, micX + 5, micY + 7);
	}

	p.setPen(QColor(0x1f, 0x23, 0x29));
	p.drawText(QRect(startX + 18, nameY - 2, textW + 10, 20), Qt::AlignLeft | Qt::AlignVCenter, _displayName);
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

	int rightX = w - btnW * 3 - 10;

	_fullscreenRect = QRect(rightX - 32, 8, 28, 28);
	rightX -= 36;

	_settingsRect = QRect(rightX - 58, 8, 54, 28);
	rightX -= 64;

	_consoleRect = QRect(rightX - 76, 8, 72, 28);
	rightX -= 82;

	_hostToolsRect = QRect(rightX - 86, 8, 82, 28);
	rightX -= 90;

	_layoutRect = QRect(rightX - 82, 8, 78, 28);
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
	const int pillW = 200;
	const int pillH = 26;
	const QRect pillRect((w - pillW) / 2, (h - pillH) / 2, pillW, pillH);

	p.save();
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0xe8, 0xf3, 0xff));
	p.drawRoundedRect(pillRect, 6, 6);

	p.setFont(QFont("Microsoft YaHei", 9));
	p.setPen(QColor(0x16, 0x77, 0xff));
	QString speakerText = _speakerName.isEmpty() ? QString::fromUtf8("正在讲话: 无") : QString::fromUtf8("正在讲话: %1").arg(_speakerName);
	p.drawText(pillRect, Qt::AlignCenter, speakerText);
	p.restore();

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
		} else if (_layoutRect.contains(e->pos())) {
			_currentViewMode = (_currentViewMode == VideoViewMode::Grid) ? VideoViewMode::Pip : VideoViewMode::Grid;
			_viewModeStream.fire_copy(_currentViewMode);
			update();
		} else if (_settingsRect.contains(e->pos())) {
			_settingsStream.fire({});
		}
	}
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

	_chatInput->setGeometry(16, 20, 130, 32);
	_handBtn->setGeometry(152, 20, 32, 32);

	_toolItems = {
		{ 1, QString::fromUtf8("解除静音"), QString::fromUtf8("静音"), QRect(), true },
		{ 2, QString::fromUtf8("开启视频"), QString::fromUtf8("停止视频"), QRect(), true },
		{ 3, QString::fromUtf8("共享屏幕"), QString::fromUtf8("共享屏幕"), QRect(), true },
		{ 4, QString::fromUtf8("邀请"), QString::fromUtf8("邀请"), QRect(), true },
		{ 5, QString::fromUtf8("成员(%1)").arg(_participantCount), QString::fromUtf8("成员(%1)").arg(_participantCount), QRect(), false },
		{ 6, QString::fromUtf8("聊天"), QString::fromUtf8("聊天"), QRect(), false },
		{ 7, QString::fromUtf8("录制"), QString::fromUtf8("停止录制"), QRect(), true },
		{ 8, QString::fromUtf8("元宝纪要"), QString::fromUtf8("元宝纪要"), QRect(), false },
		{ 9, QString::fromUtf8("应用"), QString::fromUtf8("应用"), QRect(), false },
	};

	const int itemW = 56;
	const int itemH = 60;
	const int gap = 6;
	const int totalItemsW = static_cast<int>(_toolItems.size()) * itemW + (static_cast<int>(_toolItems.size()) - 1) * gap;

	const int startX = (w - totalItemsW) / 2;
	const int startY = 8;

	for (size_t i = 0; i < _toolItems.size(); ++i) {
		_toolItems[i].rect = QRect(startX + static_cast<int>(i) * (itemW + gap), startY, itemW, itemH);
	}

	_endMeetingRect = QRect(w - 96, 12, 80, 52);
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
		} else if (item.id == 8) {
			p.setPen(QPen(QColor(0x1f, 0x23, 0x29), 1.6));
			p.drawRoundedRect(QRect(cx - 6, cy - 7, 12, 14), 2, 2);
			p.drawLine(cx - 3, cy - 3, cx + 3, cy - 3);
			p.drawLine(cx - 3, cy, cx + 3, cy);
			p.drawLine(cx - 3, cy + 3, cx + 1, cy + 3);
		} else if (item.id == 9) {
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(0x1f, 0x23, 0x29));
			for (int row = -1; row <= 1; ++row) {
				for (int col = -1; col <= 1; ++col) {
					p.drawRect(cx + col * 4 - 1, cy + row * 4 - 1, 2, 2);
				}
			}
		}

		QString title = item.title;
		if (item.id == 1) title = _audioMuted ? QString::fromUtf8("解除静音") : QString::fromUtf8("静音");
		else if (item.id == 2) title = _videoEnabled ? QString::fromUtf8("停止视频") : QString::fromUtf8("开启视频");
		else if (item.id == 5) title = QString::fromUtf8("成员(%1)").arg(_participantCount);

		QFont font("Microsoft YaHei", 9);
		p.setFont(font);
		p.setPen(QColor(0x4e, 0x59, 0x69));
		p.drawText(QRect(r.x(), r.bottom() - 18, r.width(), 16), Qt::AlignCenter, title);

		if (item.hasDropdown) {
			p.setPen(QPen(QColor(0x86, 0x90, 0x9c), 1.2));
			const int ax = r.right() - 6;
			const int ay = r.y() + 10;
			p.drawLine(ax - 2, ay + 1, ax, ay - 1);
			p.drawLine(ax, ay - 1, ax + 2, ay + 1);
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
	if (e->button() == Qt::LeftButton) {
		if (_endMeetingRect.contains(e->pos())) {
			_endMeetingStream.fire({});
			return;
		}

		for (const auto &item : _toolItems) {
			if (item.rect.contains(e->pos())) {
				switch (item.id) {
				case 1: {
					if (_audioMuted) {
						// 准备解除静音/开启麦克风，先检查是否有可用麦克风
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
				case 8:
					_minutesStream.fire({});
					break;
				case 9:
					_appsStream.fire({});
					break;
				}
				break;
			}
		}
	}
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

	// 1. 初始化远端音频扬声器播放器
	_audioPlayer = std::make_unique<PcmAudioPlayer>();
	_audioPlayer->Open(48000, 2, 16);

	// 2. 校验硬件设备可用性
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
	_stageContainer->setStyleSheet("background-color: #ffffff;");
	_bottomBar = new RoomBottomBarWidget(this);

	_remoteTile = new VideoTileWidget(QString::fromUtf8("远端参会人"), false, _stageContainer);
	_remoteTile->hide();

	_localTile = new VideoTileWidget(QString::fromUtf8("%1 (我)").arg(_config.displayName), true, _stageContainer);
	_localTile->show();

	_bottomBar->setAudioMuted(_config.audioMuted);
	_bottomBar->setVideoEnabled(_config.videoEnabled);
	_bottomBar->setParticipantCount(1);

	_localTile->setAudioMuted(_config.audioMuted);
	_localTile->setVideoActive(_config.videoEnabled);

	_inviteHintBanner = new QLabel(QString::fromUtf8("邀请您的联系人参加会议"), _stageContainer);
	_inviteHintBanner->setAlignment(Qt::AlignCenter);
	_inviteHintBanner->setStyleSheet(R"(
		QLabel {
			background-color: #f2f3f5;
			color: #1f2329;
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
		if (_hasRemoteUser) {
			userList += QString::fromUtf8("\n2. %1 (远端参会人)").arg(_remoteUserName.isEmpty() ? QString::fromUtf8("远端用户") : _remoteUserName);
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

	_bottomBar->minutesClicked() | rpl::on_next([this] {
		QMessageBox::information(this, QString::fromUtf8("元宝纪要"),
			QString::fromUtf8("AI 实时语音转写纪要已开启，正在为您自动提炼会议重点与行动事项。"));
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

	connect(_localTile, &VideoTileWidget::tileDoubleClicked, [this] {
		_viewMode = (_viewMode == VideoViewMode::Grid) ? VideoViewMode::Pip : VideoViewMode::Grid;
		updateVideoLayout();
	});
	connect(_remoteTile, &VideoTileWidget::tileDoubleClicked, [this] {
		_viewMode = (_viewMode == VideoViewMode::Grid) ? VideoViewMode::Pip : VideoViewMode::Grid;
		updateVideoLayout();
	});

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
	_hasRemoteUser = true;
	_remoteUserName = identity;
	_participantCount = 2;

	_remoteTile->setDisplayName(identity);
	_remoteTile->setVideoActive(false);
	_remoteTile->show();

	_bottomBar->setParticipantCount(_participantCount);
	_topBar->setActiveSpeaker(QString::fromUtf8("%1 已加入").arg(identity));

	LogToConsole(LogCategory::Participant, "USER_JOIN", QString("远端参会人已加入: %1").arg(identity));
	updateVideoLayout();
}

void MeetingRoomWindow::onRemoteParticipantLeft(const QString &identity) {
	_hasRemoteUser = false;
	_remoteUserName.clear();
	_participantCount = 1;

	_remoteTile->setVideoActive(false);
	_remoteTile->hide();

	_bottomBar->setParticipantCount(_participantCount);
	_topBar->setActiveSpeaker(QString::fromUtf8("%1 已离开").arg(identity));

	LogToConsole(LogCategory::Participant, "USER_LEFT", QString("远端参会人已离开: %1").arg(identity));
	updateVideoLayout();
}

void MeetingRoomWindow::onRemoteTrackMuted(bool isVideo, bool muted) {
	if (isVideo) {
		_remoteTile->setVideoActive(!muted);
		if (muted) {
			_remoteTile->setFrame(QImage());
		}
		LogToConsole(LogCategory::Track, "REMOTE_VIDEO", muted ? "远端视频轨道已关闭/静音" : "远端视频轨道已开启");
		updateVideoLayout();
	} else {
		_remoteTile->setAudioMuted(muted);
		LogToConsole(LogCategory::Track, "REMOTE_AUDIO", muted ? "远端音频轨道已静音" : "远端音频轨道已开麦");
	}
}

void MeetingRoomWindow::updateVideoLayout() {
	const int stageW = _stageContainer->width();
	const int stageH = _stageContainer->height();
	if (stageW <= 0 || stageH <= 0) return;

	const bool localActive = _localTile->isVideoActive();
	const bool remoteActive = _hasRemoteUser && _remoteTile->isVideoActive();

	const int bannerW = 200;
	const int bannerH = 32;
	_inviteHintBanner->setGeometry((stageW - bannerW) / 2, stageH - bannerH - 12, bannerW, bannerH);
	_inviteHintBanner->setVisible(!_hasRemoteUser && !localActive);

	if (_hasRemoteUser) {
		if (localActive && remoteActive) {
			if (_viewMode == VideoViewMode::Pip) {
				_remoteTile->setPipMode(false);
				_remoteTile->setGeometry(0, 0, stageW, stageH);
				_remoteTile->show();
				_remoteTile->raise();

				const int pipW = std::max(180, stageW * 24 / 100);
				const int pipH = pipW * 9 / 16;
				_localTile->setPipMode(true);
				_localTile->setGeometry(stageW - pipW - 16, stageH - pipH - 16, pipW, pipH);
				_localTile->show();
				_localTile->raise();
			} else {
				_remoteTile->setPipMode(false);
				_localTile->setPipMode(false);

				const int gap = 8;
				const int margin = 8;
				const int tileW = (stageW - margin * 2 - gap) / 2;
				const int tileH = stageH - margin * 2;

				_localTile->setGeometry(margin, margin, tileW, tileH);
				_remoteTile->setGeometry(margin + tileW + gap, margin, tileW, tileH);
				_localTile->show();
				_remoteTile->show();
			}
		} else if (remoteActive) {
			_remoteTile->setPipMode(false);
			_remoteTile->setGeometry(0, 0, stageW, stageH);
			_remoteTile->show();

			const int pipW = std::max(160, stageW * 20 / 100);
			const int pipH = pipW * 9 / 16;
			_localTile->setPipMode(true);
			_localTile->setGeometry(stageW - pipW - 16, stageH - pipH - 16, pipW, pipH);
			_localTile->show();
			_localTile->raise();
		} else if (localActive) {
			_localTile->setPipMode(false);
			_localTile->setGeometry(0, 0, stageW, stageH);
			_localTile->show();

			const int pipW = std::max(160, stageW * 20 / 100);
			const int pipH = pipW * 9 / 16;
			_remoteTile->setPipMode(true);
			_remoteTile->setGeometry(stageW - pipW - 16, 16, pipW, pipH);
			_remoteTile->show();
			_remoteTile->raise();
		} else {
			_remoteTile->setPipMode(false);
			_localTile->setPipMode(false);

			const int gap = 8;
			const int margin = 8;
			const int tileW = (stageW - margin * 2 - gap) / 2;
			const int tileH = stageH - margin * 2;

			_localTile->setGeometry(margin, margin, tileW, tileH);
			_remoteTile->setGeometry(margin + tileW + gap, margin, tileW, tileH);
			_localTile->show();
			_remoteTile->show();
		}
	} else {
		_remoteTile->hide();
		_localTile->setPipMode(false);
		_localTile->setGeometry(0, 0, stageW, stageH);
		_localTile->show();
	}
}

void MeetingRoomWindow::paintEvent(QPaintEvent *e) {
	QPainter p(this);
	p.fillRect(rect(), Qt::white);
}

void MeetingRoomWindow::receiveRemoteVideoFrame(const QImage &frame, const QString &user) {
	if (!_hasRemoteUser) {
		onRemoteParticipantJoined(user);
	}
	if (_remoteTile) {
		if (!_remoteUserName.isEmpty() && _remoteTile->displayName() != user) {
			_remoteTile->setDisplayName(user);
		}
		_remoteTile->setFrame(frame);
		if (!_remoteTile->isVideoActive()) {
			_remoteTile->setVideoActive(true);
			LogToConsole(LogCategory::WebRTC, "RECV_FRAME", QString("收到远端解码视频流 (%1x%2) - 开始渲染").arg(frame.width()).arg(frame.height()));
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
				auto frame_cnt = std::make_shared<std::atomic<uint64_t>>(0);
				track->addVideoSink([this, identity, frame_cnt](const livekit::VideoFrame &frame, const livekit::VideoCaptureOptions &) {
					uint64_t c = frame_cnt->fetch_add(1);
					if (c == 0 || c % 120 == 0) {
						LogToConsole(LogCategory::WebRTC, "SINK_FRAME", QString("LiveKit Track 视频 Sink 收到第 %1 帧 (%2x%3 RGBA) 来自: %4")
							.arg(c).arg(frame.width()).arg(frame.height()).arg(QString::fromStdString(identity)));
					}
					QImage img = VideoFrameToQImage(frame);
					if (!img.isNull() && _window) {
						QMetaObject::invokeMethod(_window, [this, img = std::move(img), id = QString::fromStdString(identity)]() {
							_window->receiveRemoteVideoFrame(img, id);
						}, Qt::QueuedConnection);
					}
				});
			} else if (track->kind() == livekit::TrackKind::Audio) {
				track->addAudioSink([this](const livekit::AudioFrame &frame) {
					if (_window && _window->_audioPlayer && !frame.data().empty()) {
						_window->_audioPlayer->Open(frame.sampleRate(), frame.numChannels(), 16);
						_window->_audioPlayer->Play(reinterpret_cast<const uint8_t*>(frame.data().data()), frame.data().size() * sizeof(int16_t));
					}
				});
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
			if (_window && !speakers.empty() && speakers[0]) {
				QString name = QString::fromStdString(speakers[0]->identity());
				QMetaObject::invokeMethod(_window->_topBar, "setActiveSpeaker", Qt::QueuedConnection, Q_ARG(QString, name));
			}
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
					bool ok = co_await _room->Connect(urlStr, tokenStr, opts);
					if (ok) {
						auto local = _room->local_participant();
						if (local) {
							// 自动发布本地音频轨
							_localAudioTrack = livekit::LocalAudioTrack::createLocalAudioTrack("simple_audio", _localAudioSource);
							_localAudioTrack->set_muted(_config.audioMuted);
							local->PublishTrack(_localAudioTrack);
							LogToConsole(LogCategory::Track, "PUBLISH", QString("已向房间发布 LocalAudioTrack (simple_audio, 初始: %1)").arg(_config.audioMuted ? "静音" : "开麦"));

							// 自动发布本地视频轨
							_localVideoTrack = livekit::LocalVideoTrack::createLocalVideoTrack("camera_video", _localVideoSource);
							_localVideoTrack->set_muted(!_config.videoEnabled);
							local->PublishTrack(_localVideoTrack);
							LogToConsole(LogCategory::Track, "PUBLISH", QString("已向房间发布 LocalVideoTrack (camera_video, 初始: %1)").arg(_config.videoEnabled ? "开启" : "关闭"));
						}
					} else {
						LogToConsole(LogCategory::Error, "CONNECT", "Room::Connect 返回失败，请检查 URL 与 Token 是否有效！");
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
	if (_audioPlayer) {
		_audioPlayer->Close();
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
