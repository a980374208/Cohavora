#pragma once

#include "base/basic_types.h"
#include "ui/rp_widget.h"
#include "src/rtc/video_frame.h"
#include "src/core/remote_track_publication.h"
#include "src/core/adaptive_stream_manager.h"

#include <QtWidgets/QWidget>
#include <QtWidgets/QDialog>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLabel>
#include <QtCore/QTimer>
#include <QtCore/QTime>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <memory>
#include <vector>

namespace MeetingUI {

enum class QualityMode {
	Auto,
	High,
	Medium,
	Low,
	Paused
};

// ----------------------------------------------------
// ParticipantVideoTile: 单参会者视频画框组件
// ----------------------------------------------------
class ParticipantVideoTile : public Ui::RpWidget {
	Q_OBJECT
public:
	explicit ParticipantVideoTile(int participantId, QWidget *parent = nullptr);
	~ParticipantVideoTile() override = default;

	int participantId() const { return _id; }
	QString participantName() const { return _name; }

	// 更新视频帧 (或模拟生成的 Image)
	void updateFrame(const QImage &image, int srcWidth, int srcHeight);

	// 设置画质与发布轨道关联
	void setRemoteTrack(std::shared_ptr<livekit::RemoteTrackPublication> track);
	void setQualityMode(QualityMode mode);
	QualityMode qualityMode() const { return _qualityMode; }

	bool isTileVisible() const { return _isTileVisible; }
	void setTileVisible(bool visible);

	// 性能统计
	int renderFps() const { return _currentFps; }
	int srcWidth() const { return _srcW; }
	int srcHeight() const { return _srcH; }

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void showEvent(QShowEvent *e) override;
	void hideEvent(QHideEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;

private:
	void applyQualityToTrack();

	int _id = 0;
	QString _name;
	QImage _currentFrame;
	int _srcW = 0;
	int _srcH = 0;

	QualityMode _qualityMode = QualityMode::Auto;
	livekit::proto::VideoQuality _effectiveQuality = livekit::proto::VideoQuality::HIGH;
	bool _isTileVisible = true;

	std::shared_ptr<livekit::RemoteTrackPublication> _remoteTrack;

	// FPS 计算
	int _frameCount = 0;
	int _currentFps = 0;
	qint64 _lastFpsCalcTime = 0;
};

// ----------------------------------------------------
// SimulcastTestControlBar: 性能监控与测试控制看板
// ----------------------------------------------------
class SimulcastTestControlBar : public Ui::RpWidget {
	Q_OBJECT
public:
	explicit SimulcastTestControlBar(QWidget *parent = nullptr);
	~SimulcastTestControlBar() override = default;

	void updateStats(int totalUsers, int activeTiles, int highCount, int medCount, int lowCount, int pausedCount, int systemFps);

	rpl::producer<QualityMode> globalQualityRequested() const {
		return _globalQualityStream.events();
	}
	rpl::producer<int> pageSizeRequested() const {
		return _pageSizeStream.events();
	}
	rpl::producer<> refreshRequested() const {
		return _refreshStream.events();
	}

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	QLabel *_statsLabel = nullptr;
	QPushButton *_btnAllHigh = nullptr;
	QPushButton *_btnAllMed = nullptr;
	QPushButton *_btnAllLow = nullptr;
	QPushButton *_btnAllAuto = nullptr;
	QComboBox *_pageSizeCombo = nullptr;

	rpl::event_stream<QualityMode> _globalQualityStream;
	rpl::event_stream<int> _pageSizeStream;
	rpl::event_stream<> _refreshStream;
};

// ----------------------------------------------------
// HundredParticipantContainer: 百人会议网格与流管理面板
// ----------------------------------------------------
class HundredParticipantContainer : public Ui::RpWidget {
	Q_OBJECT
public:
	explicit HundredParticipantContainer(QWidget *parent = nullptr);
	~HundredParticipantContainer() override;

	void setParticipantCount(int count);
	int participantCount() const { return _totalParticipants; }

	void setPageSize(int sizePerPage);
	int pageSize() const { return _pageSize; }

	void setGlobalQualityMode(QualityMode mode);

	// 性能指标查询
	void getStats(int &outTotal, int &outActive, int &outHigh, int &outMed, int &outLow, int &outPaused);

protected:
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;

private:
	void rebuildGrid();
	void updateVisibilityAndAdaptiveStream();
	void onSimulatedFrameTimer();
	QImage generateDummyFrame(int pId, int width, int height, livekit::proto::VideoQuality quality, int step);

	int _totalParticipants = 100;
	int _pageSize = 25; // 默认每页 25 人 (5x5 宫格)
	int _currentPage = 0;

	std::vector<ParticipantVideoTile*> _tiles;
	std::vector<std::shared_ptr<livekit::RemoteTrackPublication>> _remoteTracks;

	QWidget *_gridContent = nullptr;
	QScrollArea *_scrollArea = nullptr;

	QTimer *_simTimer = nullptr;
	int _simFrameStep = 0;
};

// ----------------------------------------------------
// MeetingTestWindow: 音视频与 Simulcast 测试主窗口
// ----------------------------------------------------
class MeetingTestWindow : public QDialog {
	Q_OBJECT
public:
	explicit MeetingTestWindow(QWidget *parent = nullptr);
	~MeetingTestWindow() override = default;

	rpl::lifetime &lifetime() { return _lifetime; }

protected:
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;

private:
	void initUi();
	void onFpsTimer();

	rpl::lifetime _lifetime;

	SimulcastTestControlBar *_controlBar = nullptr;
	HundredParticipantContainer *_participantContainer = nullptr;
	QLabel *_titleLabel = nullptr;
	QPushButton *_closeBtn = nullptr;

	QTimer *_fpsTimer = nullptr;
	int _fpsCounter = 0;
	int _lastSystemFps = 60;
	qint64 _lastTime = 0;
};

} // namespace MeetingUI
