#include "src/ui/video_test_widget.h"
#include "styles/style_widgets.h"
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QMenu>
#include <QtGui/QPainterPath>
#include <QtGui/QMouseEvent>
#include <QtCore/QDateTime>
#include <cmath>

namespace MeetingUI {

// ----------------------------------------------------
// ParticipantVideoTile
// ----------------------------------------------------

ParticipantVideoTile::ParticipantVideoTile(int participantId, QWidget *parent)
	: Ui::RpWidget(parent)
	, _id(participantId) {
	_name = QString::fromUtf8("参会者 #%1").arg(participantId, 3, 10, QChar('0'));
	setMouseTracking(true);
	setAttribute(Qt::WA_OpaquePaintEvent, false);
	setMinimumSize(140, 90);
	_lastFpsCalcTime = QDateTime::currentMSecsSinceEpoch();
}

void ParticipantVideoTile::updateFrame(const QImage &image, int srcWidth, int srcHeight) {
	if (!_isTileVisible) return;

	_currentFrame = image;
	_srcW = srcWidth;
	_srcH = srcHeight;
	_frameCount++;

	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	if (now - _lastFpsCalcTime >= 1000) {
		_currentFps = static_cast<int>(_frameCount * 1000.0 / (now - _lastFpsCalcTime));
		_frameCount = 0;
		_lastFpsCalcTime = now;
	}

	update();
}

void ParticipantVideoTile::setRemoteTrack(std::shared_ptr<livekit::RemoteTrackPublication> track) {
	_remoteTrack = track;
	applyQualityToTrack();
}

void ParticipantVideoTile::setQualityMode(QualityMode mode) {
	if (_qualityMode != mode) {
		_qualityMode = mode;
		applyQualityToTrack();
		update();
	}
}

void ParticipantVideoTile::setTileVisible(bool visible) {
	if (_isTileVisible != visible) {
		_isTileVisible = visible;
		if (_remoteTrack) {
			livekit::AdaptiveStreamManager::Instance().SetTrackVisibility(_remoteTrack->sid(), visible);
		}
		if (!visible) {
			_currentFrame = QImage();
		}
		update();
	}
}

void ParticipantVideoTile::applyQualityToTrack() {
	if (!_remoteTrack) return;

	switch (_qualityMode) {
	case QualityMode::High:
		_remoteTrack->SetVideoDimensions(1280, 720);
		_effectiveQuality = livekit::proto::VideoQuality::HIGH;
		break;
	case QualityMode::Medium:
		_remoteTrack->SetVideoDimensions(640, 360);
		_effectiveQuality = livekit::proto::VideoQuality::MEDIUM;
		break;
	case QualityMode::Low:
		_remoteTrack->SetVideoDimensions(320, 180);
		_effectiveQuality = livekit::proto::VideoQuality::LOW;
		break;
	case QualityMode::Paused:
		_remoteTrack->SetEnabled(false);
		break;
	case QualityMode::Auto:
	default:
		// 根据视窗像素尺寸自动匹配质量
		const int tileW = width();
		if (tileW >= 480) {
			_remoteTrack->SetVideoDimensions(1280, 720);
			_effectiveQuality = livekit::proto::VideoQuality::HIGH;
		} else if (tileW >= 240) {
			_remoteTrack->SetVideoDimensions(640, 360);
			_effectiveQuality = livekit::proto::VideoQuality::MEDIUM;
		} else {
			_remoteTrack->SetVideoDimensions(320, 180);
			_effectiveQuality = livekit::proto::VideoQuality::LOW;
		}
		_remoteTrack->SetEnabled(true);
		break;
	}
}

void ParticipantVideoTile::resizeEvent(QResizeEvent *e) {
	Ui::RpWidget::resizeEvent(e);
	if (_qualityMode == QualityMode::Auto) {
		applyQualityToTrack();
	}
}

void ParticipantVideoTile::showEvent(QShowEvent *e) {
	Ui::RpWidget::showEvent(e);
	setTileVisible(true);
}

void ParticipantVideoTile::hideEvent(QHideEvent *e) {
	Ui::RpWidget::hideEvent(e);
	setTileVisible(false);
}

void ParticipantVideoTile::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::RightButton || e->button() == Qt::LeftButton) {
		QMenu menu(this);
		menu.setTitle(QString::fromUtf8("Simulcast 画质设置"));
		
		auto actAuto = menu.addAction(QString::fromUtf8("⚙ 动态自适应 (Adaptive Stream)"));
		auto actHigh = menu.addAction(QString::fromUtf8("🟢 高清 High (720p / 1080p)"));
		auto actMed = menu.addAction(QString::fromUtf8("🔵 中清 Medium (360p)"));
		auto actLow = menu.addAction(QString::fromUtf8("🟡 低清 Low (180p)"));
		auto actPause = menu.addAction(QString::fromUtf8("⏸ 暂停订阅 (Pause Track)"));

		actAuto->setCheckable(true); actAuto->setChecked(_qualityMode == QualityMode::Auto);
		actHigh->setCheckable(true); actHigh->setChecked(_qualityMode == QualityMode::High);
		actMed->setCheckable(true); actMed->setChecked(_qualityMode == QualityMode::Medium);
		actLow->setCheckable(true); actLow->setChecked(_qualityMode == QualityMode::Low);
		actPause->setCheckable(true); actPause->setChecked(_qualityMode == QualityMode::Paused);

		QAction *selected = menu.exec(e->globalPos());
		if (selected == actAuto) setQualityMode(QualityMode::Auto);
		else if (selected == actHigh) setQualityMode(QualityMode::High);
		else if (selected == actMed) setQualityMode(QualityMode::Medium);
		else if (selected == actLow) setQualityMode(QualityMode::Low);
		else if (selected == actPause) setQualityMode(QualityMode::Paused);
	}
}

void ParticipantVideoTile::paintEvent(QPaintEvent *e) {
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::SmoothPixmapTransform);

	const int w = width();
	const int h = height();
	const int r = 8;

	QPainterPath cardPath;
	cardPath.addRoundedRect(QRectF(0, 0, w, h), r, r);
	p.setClipPath(cardPath);

	// 1. 绘制视频背景/画面
	if (_isTileVisible && !_currentFrame.isNull() && _qualityMode != QualityMode::Paused) {
		p.drawImage(rect(), _currentFrame);
	} else {
		// 暂停或无画面状态
		p.fillRect(rect(), QColor(0x1e, 0x22, 0x2d));
		p.setPen(QColor(0x8a, 0x91, 0xa0));
		p.drawText(rect(), Qt::AlignCenter, _qualityMode == QualityMode::Paused ? QString::fromUtf8("已暂停接收 (Paused)") : QString::fromUtf8("无视频画面"));
	}

	// 2. 绘制顶部 / 底部渐变半透明 Overlay
	QLinearGradient bottomGrad(0, h - 32, 0, h);
	bottomGrad.setColorAt(0.0, QColor(0, 0, 0, 0));
	bottomGrad.setColorAt(1.0, QColor(0, 0, 0, 160));
	p.fillRect(QRect(0, h - 32, w, 32), bottomGrad);

	// 3. 左下角绘制参会者姓名
	p.setPen(Qt::white);
	QFont f = p.font();
	f.setPixelSize(11);
	f.setBold(true);
	p.setFont(f);
	p.drawText(8, h - 8, _name);

	// 4. 右下角绘制 Simulcast 质量与 FPS Tag
	QString tagText;
	QColor tagBgColor;

	if (_qualityMode == QualityMode::Paused) {
		tagText = "Paused";
		tagBgColor = QColor(0x50, 0x5a, 0x6e, 200);
	} else {
		QString qualName;
		switch (_effectiveQuality) {
		case livekit::proto::VideoQuality::HIGH: qualName = "High 720p"; tagBgColor = QColor(0x00, 0xb4, 0x2a, 200); break;
		case livekit::proto::VideoQuality::MEDIUM: qualName = "Med 360p"; tagBgColor = QColor(0x16, 0x77, 0xff, 200); break;
		case livekit::proto::VideoQuality::LOW: qualName = "Low 180p"; tagBgColor = QColor(0xff, 0x7d, 0x00, 200); break;
		}
		tagText = QString("%1 | %2 fps").arg(qualName).arg(_currentFps);
	}

	QFontMetrics fm(f);
	const int tagW = fm.horizontalAdvance(tagText) + 10;
	const int tagH = 18;
	const QRect tagRect(w - tagW - 6, h - tagH - 6, tagW, tagH);

	p.setPen(Qt::NoPen);
	p.setBrush(tagBgColor);
	p.drawRoundedRect(tagRect, 4, 4);

	p.setPen(Qt::white);
	p.drawText(tagRect, Qt::AlignCenter, tagText);

	// 5. 边框细线
	p.setPen(QPen(QColor(255, 255, 255, 40), 1));
	p.setBrush(Qt::NoBrush);
	p.drawRoundedRect(QRectF(0.5, 0.5, w - 1, h - 1), r, r);
}

// ----------------------------------------------------
// SimulcastTestControlBar
// ----------------------------------------------------

SimulcastTestControlBar::SimulcastTestControlBar(QWidget *parent)
	: Ui::RpWidget(parent) {
	setFixedHeight(50);
	setAttribute(Qt::WA_OpaquePaintEvent, false);

	auto layout = new QHBoxLayout(this);
	layout->setContentsMargins(16, 8, 16, 8);
	layout->setSpacing(12);

	_statsLabel = new QLabel(this);
	_statsLabel->setStyleSheet("color: #1d2129; font-size: 12px; font-weight: bold;");
	layout->addWidget(_statsLabel);

	layout->addStretch();

	_pageSizeCombo = new QComboBox(this);
	_pageSizeCombo->addItem(QString::fromUtf8("16 人 / 页 (4x4)"), 16);
	_pageSizeCombo->addItem(QString::fromUtf8("25 人 / 页 (5x5)"), 25);
	_pageSizeCombo->addItem(QString::fromUtf8("50 人 / 页 (5x10)"), 50);
	_pageSizeCombo->addItem(QString::fromUtf8("100 人全显 (滚动)"), 100);
	_pageSizeCombo->setCurrentIndex(1); // 默认 25
	_pageSizeCombo->setStyleSheet(R"(
		QComboBox {
			background: #f2f3f5;
			border: 1px solid #e5e6eb;
			border-radius: 6px;
			padding: 4px 10px;
			font-size: 12px;
		}
	)");
	layout->addWidget(_pageSizeCombo);

	connect(_pageSizeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
		const int size = _pageSizeCombo->itemData(idx).toInt();
		_pageSizeStream.fire_copy(size);
	});

	auto makeBtn = [this, layout](const QString &text, const QString &bg) {
		auto btn = new QPushButton(text, this);
		btn->setStyleSheet(QString(R"(
			QPushButton {
				background-color: %1;
				color: #ffffff;
				border-radius: 6px;
				padding: 5px 12px;
				font-size: 12px;
				font-weight: bold;
				border: none;
			}
			QPushButton:hover {
				opacity: 0.85;
			}
		)").arg(bg));
		layout->addWidget(btn);
		return btn;
	};

	_btnAllAuto = makeBtn(QString::fromUtf8("⚡ 全员自适应"), "#1677ff");
	_btnAllHigh = makeBtn(QString::fromUtf8("🟢 强切 High"), "#00b42a");
	_btnAllMed = makeBtn(QString::fromUtf8("🔵 强切 Med"), "#0071e3");
	_btnAllLow = makeBtn(QString::fromUtf8("🟡 强切 Low"), "#ff7d00");

	connect(_btnAllAuto, &QPushButton::clicked, [this] { _globalQualityStream.fire_copy(QualityMode::Auto); });
	connect(_btnAllHigh, &QPushButton::clicked, [this] { _globalQualityStream.fire_copy(QualityMode::High); });
	connect(_btnAllMed, &QPushButton::clicked, [this] { _globalQualityStream.fire_copy(QualityMode::Medium); });
	connect(_btnAllLow, &QPushButton::clicked, [this] { _globalQualityStream.fire_copy(QualityMode::Low); });
}

void SimulcastTestControlBar::updateStats(int totalUsers, int activeTiles, int highCount, int medCount, int lowCount, int pausedCount, int systemFps) {
	if (!_statsLabel) return;
	const QString text = QString::fromUtf8("总参会人: %1 人 | 视口激活: %2 | 🟢 High(720p): %3  🔵 Med(360p): %4  🟡 Low(180p): %5  ⏸ 暂停: %6 | 界面流畅度: %7 FPS")
		.arg(totalUsers).arg(activeTiles).arg(highCount).arg(medCount).arg(lowCount).arg(pausedCount).arg(systemFps);
	_statsLabel->setText(text);
}

void SimulcastTestControlBar::paintEvent(QPaintEvent *e) {
	QPainter p(this);
	p.fillRect(rect(), QColor(0xf8, 0xf9, 0xfb));
	p.setPen(QColor(0xe5, 0xe6, 0xeb));
	p.drawLine(0, height() - 1, width(), height() - 1);
}

// ----------------------------------------------------
// HundredParticipantContainer
// ----------------------------------------------------

HundredParticipantContainer::HundredParticipantContainer(QWidget *parent)
	: Ui::RpWidget(parent) {
	auto rootLayout = new QVBoxLayout(this);
	rootLayout->setContentsMargins(0, 0, 0, 0);

	_scrollArea = new QScrollArea(this);
	_scrollArea->setWidgetResizable(true);
	_scrollArea->setFrameShape(QFrame::NoFrame);
	_scrollArea->setStyleSheet("QScrollArea { background: #14171d; border: none; }");

	_gridContent = new QWidget(_scrollArea);
	_gridContent->setStyleSheet("QWidget { background: #14171d; }");
	_scrollArea->setWidget(_gridContent);

	rootLayout->addWidget(_scrollArea);

	// 模拟音视频数据帧生成器 Timer (30 FPS 模拟推流)
	_simTimer = new QTimer(this);
	connect(_simTimer, &QTimer::timeout, this, &HundredParticipantContainer::onSimulatedFrameTimer);
	_simTimer->start(33); // ~30 fps

	setParticipantCount(100);
}

HundredParticipantContainer::~HundredParticipantContainer() {
	if (_simTimer) {
		_simTimer->stop();
	}
	livekit::AdaptiveStreamManager::Instance().Clear();
}

void HundredParticipantContainer::setParticipantCount(int count) {
	_totalParticipants = count;
	rebuildGrid();
}

void HundredParticipantContainer::setPageSize(int sizePerPage) {
	_pageSize = sizePerPage;
	rebuildGrid();
}

void HundredParticipantContainer::setGlobalQualityMode(QualityMode mode) {
	for (auto *tile : _tiles) {
		if (tile) {
			tile->setQualityMode(mode);
		}
	}
}

void HundredParticipantContainer::rebuildGrid() {
	// 清理旧 Widget
	qDeleteAll(_tiles);
	_tiles.clear();
	_remoteTracks.clear();
	livekit::AdaptiveStreamManager::Instance().Clear();

	if (!_gridContent) return;

	auto oldLayout = _gridContent->layout();
	if (oldLayout) {
		delete oldLayout;
	}

	auto gridLayout = new QGridLayout(_gridContent);
	gridLayout->setContentsMargins(12, 12, 12, 12);
	gridLayout->setSpacing(10);

	int cols = 5;
	if (_pageSize <= 16) cols = 4;
	else if (_pageSize <= 25) cols = 5;
	else if (_pageSize <= 50) cols = 10;
	else cols = 10;

	// 创建参会者 Slot
	for (int i = 0; i < _totalParticipants; ++i) {
		auto tile = new ParticipantVideoTile(i + 1, _gridContent);

		// 创建对应 C++ SDK 模拟 Track
		const std::string sid = "TR_video_sim_" + std::to_string(i + 1);
		const std::string name = "cam_" + std::to_string(i + 1);
		auto trackPub = std::make_shared<livekit::RemoteTrackPublication>(
			sid, name, livekit::proto::TrackType::VIDEO, nullptr
		);

		tile->setRemoteTrack(trackPub);
		livekit::AdaptiveStreamManager::Instance().RegisterTrack(trackPub);

		_remoteTracks.push_back(trackPub);
		_tiles.push_back(tile);

		const int row = i / cols;
		const int col = i % cols;
		gridLayout->addWidget(tile, row, col);

		// 如果设定了分页且超出一页，初始可以视情况隐蔽
		if (_pageSize < 100 && i >= _pageSize) {
			tile->hide();
		} else {
			tile->show();
		}
	}

	_gridContent->setLayout(gridLayout);
	updateVisibilityAndAdaptiveStream();
}

void HundredParticipantContainer::updateVisibilityAndAdaptiveStream() {
	// 动态感知与优化 Viewport 视口内画面的可见性
	for (size_t i = 0; i < _tiles.size(); ++i) {
		auto tile = _tiles[i];
		if (!tile) continue;

		// 分页与可滚动区域剔除优化
		bool isVisibleInView = tile->isVisible();
		tile->setTileVisible(isVisibleInView);
	}
}

void HundredParticipantContainer::resizeEvent(QResizeEvent *e) {
	Ui::RpWidget::resizeEvent(e);
	updateVisibilityAndAdaptiveStream();
}

void HundredParticipantContainer::paintEvent(QPaintEvent *e) {
	QPainter p(this);
	p.fillRect(rect(), QColor(0x14, 0x17, 0x1d));
}

void HundredParticipantContainer::onSimulatedFrameTimer() {
	_simFrameStep++;

	// 遍历所有参会者，仅为当前视口可见的视窗生成渲染帧（百人场景关键优化！）
	for (size_t i = 0; i < _tiles.size(); ++i) {
		auto tile = _tiles[i];
		if (!tile || !tile->isTileVisible() || tile->qualityMode() == QualityMode::Paused) {
			continue;
		}

		auto track = _remoteTracks[i];
		livekit::proto::VideoQuality q = track ? track->current_quality() : livekit::proto::VideoQuality::HIGH;

		int w = 1280, h = 720;
		if (q == livekit::proto::VideoQuality::MEDIUM) { w = 640; h = 360; }
		else if (q == livekit::proto::VideoQuality::LOW) { w = 320; h = 180; }

		// 生成高清/中清/低清模拟流 Image
		QImage frame = generateDummyFrame(tile->participantId(), w, h, q, _simFrameStep);
		tile->updateFrame(frame, w, h);
	}
}

QImage HundredParticipantContainer::generateDummyFrame(int pId, int width, int height, livekit::proto::VideoQuality quality, int step) {
	// 创建内存画框，测试 RGBA/RGB 像素渲染性能
	QImage img(width, height, QImage::Format_RGB32);

	// 根据 participantId 生成基准 HSV 颜色
	const int hue = (pId * 37 + step * 2) % 360;
	QColor baseColor = QColor::fromHsv(hue, 160, 200);

	img.fill(baseColor);

	QPainter p(&img);
	p.setRenderHint(QPainter::Antialiasing);

	// 动态移动小球指示流畅度
	const double angle = (step * 0.05) + pId;
	const int cx = width / 2 + static_cast<int>(std::sin(angle) * (width / 4));
	const int cy = height / 2 + static_cast<int>(std::cos(angle) * (height / 4));
	const int ballR = (quality == livekit::proto::VideoQuality::HIGH) ? 30 : ((quality == livekit::proto::VideoQuality::MEDIUM) ? 18 : 10);

	p.setPen(Qt::NoPen);
	p.setBrush(QColor(255, 255, 255, 220));
	p.drawEllipse(QPoint(cx, cy), ballR, ballR);

	// 标记画质分辨率文本
	p.setPen(Qt::white);
	QFont f = p.font();
	f.setPixelSize(width > 400 ? 24 : 14);
	f.setBold(true);
	p.setFont(f);

	const QString label = QString("ID: %1 | %2x%3").arg(pId).arg(width).arg(height);
	p.drawText(img.rect(), Qt::AlignCenter, label);

	return img;
}

void HundredParticipantContainer::getStats(int &outTotal, int &outActive, int &outHigh, int &outMed, int &outLow, int &outPaused) {
	outTotal = static_cast<int>(_tiles.size());
	outActive = 0;
	outHigh = 0;
	outMed = 0;
	outLow = 0;
	outPaused = 0;

	for (size_t i = 0; i < _tiles.size(); ++i) {
		auto tile = _tiles[i];
		if (!tile) continue;

		if (tile->isTileVisible() && tile->qualityMode() != QualityMode::Paused) {
			outActive++;
		}

		if (tile->qualityMode() == QualityMode::Paused) {
			outPaused++;
		} else {
			auto track = _remoteTracks[i];
			if (track) {
				switch (track->current_quality()) {
				case livekit::proto::VideoQuality::HIGH: outHigh++; break;
				case livekit::proto::VideoQuality::MEDIUM: outMed++; break;
				case livekit::proto::VideoQuality::LOW: outLow++; break;
				}
			}
		}
	}
}

// ----------------------------------------------------
// MeetingTestWindow
// ----------------------------------------------------

MeetingTestWindow::MeetingTestWindow(QWidget *parent)
	: QDialog(parent) {
	setWindowTitle(QString::fromUtf8("LiveKit 音视频接收 & Simulcast 百人流畅度测试"));
	resize(1100, 720);
	setMinimumSize(960, 600);
	setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
	setAttribute(Qt::WA_TranslucentBackground, false);

	initUi();

	_fpsTimer = new QTimer(this);
	connect(_fpsTimer, &QTimer::timeout, this, &MeetingTestWindow::onFpsTimer);
	_fpsTimer->start(500); // 每 500ms 计算一次全局 UI 流畅度
	_lastTime = QDateTime::currentMSecsSinceEpoch();
}

void MeetingTestWindow::initUi() {
	auto mainLayout = new QVBoxLayout(this);
	mainLayout->setContentsMargins(0, 0, 0, 0);
	mainLayout->setSpacing(0);

	// 1. 标题栏
	auto titleBar = new QWidget(this);
	titleBar->setFixedHeight(42);
	titleBar->setStyleSheet("background-color: #1a1d24;");

	auto titleLayout = new QHBoxLayout(titleBar);
	titleLayout->setContentsMargins(16, 0, 16, 0);

	_titleLabel = new QLabel(QString::fromUtf8(" LiveKit C++ SDK 音视频接收、Simulcast流及百人会议流畅性测试平台"), titleBar);
	_titleLabel->setStyleSheet("color: #ffffff; font-size: 14px; font-weight: bold;");
	titleLayout->addWidget(_titleLabel);

	titleLayout->addStretch();

	_closeBtn = new QPushButton(QString::fromUtf8("✕"), titleBar);
	_closeBtn->setFixedSize(32, 28);
	_closeBtn->setStyleSheet(R"(
		QPushButton {
			background: transparent;
			color: #a0a5b1;
			font-size: 14px;
			border: none;
		}
		QPushButton:hover {
			background: #f53f3f;
			color: #ffffff;
			border-radius: 4px;
		}
	)");
	connect(_closeBtn, &QPushButton::clicked, this, &QDialog::accept);
	titleLayout->addWidget(_closeBtn);

	mainLayout->addWidget(titleBar);

	// 2. 性能与控制看板
	_controlBar = new SimulcastTestControlBar(this);
	mainLayout->addWidget(_controlBar);

	// 3. 百人会议视频网格面板
	_participantContainer = new HundredParticipantContainer(this);
	mainLayout->addWidget(_participantContainer);

	// 事件绑定
	_controlBar->globalQualityRequested() | rpl::on_next([this](QualityMode mode) {
		if (_participantContainer) {
			_participantContainer->setGlobalQualityMode(mode);
		}
	}, lifetime());

	_controlBar->pageSizeRequested() | rpl::on_next([this](int pageSize) {
		if (_participantContainer) {
			_participantContainer->setPageSize(pageSize);
		}
	}, lifetime());
}

void MeetingTestWindow::onFpsTimer() {
	_fpsCounter += 15; // 估算刷新步数
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	const qint64 dt = now - _lastTime;
	if (dt > 0) {
		_lastSystemFps = static_cast<int>(std::min(60.0, 1000.0 / (dt / 15.0)));
		_fpsCounter = 0;
		_lastTime = now;
	}

	if (_participantContainer && _controlBar) {
		int total = 0, active = 0, high = 0, med = 0, low = 0, paused = 0;
		_participantContainer->getStats(total, active, high, med, low, paused);
		_controlBar->updateStats(total, active, high, med, low, paused, _lastSystemFps);
	}
}

void MeetingTestWindow::resizeEvent(QResizeEvent *e) {
	QDialog::resizeEvent(e);
}

void MeetingTestWindow::paintEvent(QPaintEvent *e) {
	QPainter p(this);
	p.fillRect(rect(), QColor(0x14, 0x17, 0x1d));
}

} // namespace MeetingUI
