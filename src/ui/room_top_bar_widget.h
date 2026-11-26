#pragma once

#include "ui/widgets/rp_window.h"
#include "src/core/room.h"
#include "src/core/video_demand_types.h"

#include <QtCore/QVariantMap>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QToolButton>

class ParticipantWindowInspectionAccess;

namespace MeetingUI {

enum class VideoViewMode {
	Auto,       // 根据参会人数自动选择
	Grid,       // 宫格分屏并排
	Pip,        // 画中画悬浮窗
	Speaker     // 演讲者单人聚焦
};

// ----------------------------------------------------
// RoomTopBarWidget: 顶部状态栏与工具按钮
// ----------------------------------------------------
class RoomTopBarWidget : public Ui::RpWidget {
	Q_OBJECT
public:
	explicit RoomTopBarWidget(QWidget *parent = nullptr);
	~RoomTopBarWidget() override = default;
	int heightForWidth(int width) const override;
	bool isWindowDragArea(const QPoint &position) const;

	void updateDuration(int seconds);
	void setActiveSpeaker(const QString &speakerName);
	void setMeetingId(const QString &meetingId);
	void setTelemetrySnapshot(const QVariantMap &snapshot);
	void setLayoutSelection(VideoViewMode mode, livekit::SpeakerLayout speakerLayout);
	void setEncrypted(bool encrypted);
	void setAgentStatus(
		bool online,
		const QString &state = QString(),
		const QString &agentName = QString(),
		int agentCount = 0,
		bool localTranslatorActive = false,
		bool localAgentRoleActive = false);
	void setMaximized(bool maximized);
	QToolButton *layoutButton() const { return _layoutButton; }

	rpl::producer<VideoViewMode> viewModeChanged() const { return _viewModeStream.events(); }
	rpl::producer<> consoleClicked() const { return _consoleStream.events(); }
	rpl::producer<> minimizeClicked() const { return _minStream.events(); }
	rpl::producer<> maximizeClicked() const { return _maxStream.events(); }
	rpl::producer<> closeClicked() const { return _closeStream.events(); }
	rpl::producer<livekit::SimulateScenarioType> simulateScenarioRequested() const { return _simulateScenarioStream.events(); }

	void showSimulateScenarioMenu(const QPoint &globalPos);

signals:
	void telemetryDetailsRequested();
	void layoutMenuRequested();
	void agentMenuRequested(const QPoint &globalPos);
	// Native child HWNDs (such as Dx11VideoCanvas) may prevent the top-level
	// WM_NCHITTEST path from reaching this QWidget. Blank title-bar presses
	// therefore request a system drag explicitly as a reliable fallback.
	void windowDragRequested();

protected:
	bool eventHook(QEvent *event) override;
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	friend class ::ParticipantWindowInspectionAccess;
	enum class HoverBtn {
		None, Quality, Agent, Layout, Console, Simulate, Min, Max, Close
	};

	HoverBtn _hoverBtn = HoverBtn::None;
	int _durationSeconds = 0;
	QString _speakerName;
	QString _meetingId;
	QVariantMap _telemetrySnapshot;
	bool _copiedAnim = false;
	bool _encrypted = false;
	bool _agentOnline = false;
	bool _localTranslatorActive = false;
	bool _localAgentRoleActive = false;
	QString _agentState;
	QString _agentName;
	int _agentCount = 0;
	bool _maximized = false;
	VideoViewMode _currentViewMode = VideoViewMode::Grid;

	QString agentBadgeText() const;
	QString agentTooltipText() const;

	QRect _layoutRect;
	QToolButton *_layoutButton = nullptr;
	QRect _infoPillRect;
	QRect _agentPillRect;
	QRect _qualityRect;
	QPushButton *_uiaTelemetry = nullptr;
	QRect _consoleRect;
	QPushButton *_uiaConsole = nullptr;
	QRect _simulateRect;
	QRect _minRect;
	QRect _maxRect;
	QRect _closeRect;
	QRect _speakerCapsuleRect;
	QRect _meetingIdRect;

	rpl::event_stream<VideoViewMode> _viewModeStream;
	rpl::event_stream<> _consoleStream;
	rpl::event_stream<> _minStream;
	rpl::event_stream<> _maxStream;
	rpl::event_stream<> _closeStream;
	rpl::event_stream<livekit::SimulateScenarioType> _simulateScenarioStream;
};

} // namespace MeetingUI
