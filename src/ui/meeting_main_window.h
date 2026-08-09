#pragma once

#include "base/basic_types.h"
#include "ui/widgets/rp_window.h"
#include "src/ui/sidebar_widget.h"
#include "src/ui/action_card_widget.h"
#include "src/ui/schedule_widget.h"
#include <QtWidgets/QDialog>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QCheckBox>

namespace MeetingUI {

class WindowControlsWidget : public Ui::RpWidget {
public:
	explicit WindowControlsWidget(QWidget *parent = nullptr);
	~WindowControlsWidget() override = default;

	rpl::producer<> minimizeClicked() const { return _minClicks.events(); }
	rpl::producer<> maximizeClicked() const { return _maxClicks.events(); }
	rpl::producer<> closeClicked() const { return _closeClicks.events(); }

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	enum class HoverBtn { None, Min, Max, Close };
	HoverBtn _hoverBtn = HoverBtn::None;
	QRect _minRect;
	QRect _maxRect;
	QRect _closeRect;

	rpl::event_stream<> _minClicks;
	rpl::event_stream<> _maxClicks;
	rpl::event_stream<> _closeClicks;
};

// 加入会议弹窗
class JoinMeetingDialog : public QDialog {
public:
	explicit JoinMeetingDialog(QWidget *parent = nullptr);
	~JoinMeetingDialog() override = default;

	QString meetingId() const;
	QString displayName() const;
	bool isAudioMuted() const;
	bool isVideoMuted() const;

private:
	QLineEdit *_meetingIdInput = nullptr;
	QLineEdit *_nameInput = nullptr;
	QCheckBox *_audioMuteBox = nullptr;
	QCheckBox *_videoMuteBox = nullptr;
	QPushButton *_joinBtn = nullptr;
	QPushButton *_cancelBtn = nullptr;
};

class MeetingMainWindow : public Ui::RpWidget {
public:
	explicit MeetingMainWindow(QWidget *parent = nullptr);
	~MeetingMainWindow() override = default;

protected:
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;
	void showEvent(QShowEvent *e) override;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
#else
	bool nativeEvent(const QByteArray &eventType, void *message, long *result) override;
#endif

private:
	void setupNativeWindow();
	void initLayout();
	void onCardClicked(ActionCardType type);

	static constexpr int kWindowCornerRadius = 12;

	SidebarWidget *_sidebar = nullptr;
	ActionGridContainer *_actionGrid = nullptr;
	ScheduleWidget *_scheduleWidget = nullptr;
	WindowControlsWidget *_windowControls = nullptr;

#if defined(Q_OS_WIN)
	HWND _handle = nullptr;
#endif
};

} // namespace MeetingUI
