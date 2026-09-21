#pragma once

#include "base/basic_types.h"
#include "ui/widgets/rp_window.h"
#include "src/ui/sidebar_widget.h"
#include "src/ui/action_card_widget.h"
#include "src/ui/schedule_widget.h"
#include "src/ui/meeting_entry_guard.h"
#include "src/net/meeting_types.h"
#include <QtCore/QPointer>
#include <QtWidgets/QDialog>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QLabel>

#include <memory>
#include <optional>

namespace OpenMeeting {
enum class SessionInvalidationReason;
class MeetingCatalogController;
}

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
	Q_OBJECT
public:
	explicit JoinMeetingDialog(
		QWidget *parent = nullptr,
		const QString &initialMeetingId = QString(),
		std::optional<OpenMeeting::MeetingSettings> meetingSettings = std::nullopt);
	~JoinMeetingDialog() override = default;

	QString serverUrl() const;
	QString token() const;
	QString meetingId() const;
	QString password() const;
	QString displayName() const;
	bool isAudioMuted() const;
	bool isVideoMuted() const;
	bool isManualConnection() const { return _isManualConnection; }

	void reject() override;

protected:
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void closeEvent(QCloseEvent *e) override;

private slots:
	void onJoinClicked();
	void toggleManualServer();

private:
	void setLoading(bool loading, const QString &statusText = QString());
	void showError(const QString &msg);
	void persistMediaPreferences();

	QPushButton *_closeBtn = nullptr;
	QLineEdit *_meetingIdInput = nullptr;
	QLineEdit *_passwordInput = nullptr;
	QLineEdit *_displayNameInput = nullptr;
	QCheckBox *_audioMuteBox = nullptr;
	QCheckBox *_videoMuteBox = nullptr;
	QPushButton *_joinBtn = nullptr;
	QPushButton *_cancelBtn = nullptr;
	QLabel *_statusLabel = nullptr;
	QLabel *_meetingPolicyLabel = nullptr;

	QPushButton *_manualToggleBtn = nullptr;
	QWidget *_manualWidget = nullptr;
	QLineEdit *_serverUrlInput = nullptr;
	QLineEdit *_tokenInput = nullptr;

	QString _resolvedServerUrl;
	QString _resolvedToken;
	QString _cleanMeetingId;

	bool _isLoading = false;
	bool _isCancelled = false;
	bool _isManualConnection = false;
	std::optional<OpenMeeting::MeetingSettings> _meetingSettings;

	QPoint _dragPosition;
	bool _isDragging = false;
};

class MeetingMainWindow : public Ui::RpWidget {
public:
	explicit MeetingMainWindow(QWidget *parent = nullptr);
	~MeetingMainWindow() override = default;

protected:
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;
	void showEvent(QShowEvent *e) override;
	void closeEvent(QCloseEvent *e) override;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
#else
	bool nativeEvent(const QByteArray &eventType, void *message, long *result) override;
#endif

private:
	void setupNativeWindow();
	void initLayout();
	void onCardClicked(ActionCardType type);
	void openQuickMeeting(std::unique_ptr<QObject> reservation, bool startScreenShare);
	void onSessionInvalidated(OpenMeeting::SessionInvalidationReason reason);
	void handleUserLogout();
	void closeMeetingWindows();
	void hideLogConsole();
	void showBookingDialog();
	void showMeetingListDialog();
	void showMeetingDetail(const QString &meetingId);
	void beginMeetingEntry(
		const QString &meetingId = QString(),
		std::optional<OpenMeeting::MeetingSettings> meetingSettings = std::nullopt,
		bool requireFreshDetail = false,
		bool shareScreenAfterJoin = false);
	void openJoinMeetingDialog(
		std::unique_ptr<QObject> reservation,
		const QString &meetingId,
		std::optional<OpenMeeting::MeetingSettings> meetingSettings,
		bool shareScreenAfterJoin);
	void handlePendingMeetingEntryDetail();
	void clearPendingMeetingEntry();
	void syncSchedule();

	static constexpr int kWindowCornerRadius = 12;

	SidebarWidget *_sidebar = nullptr;
	ActionGridContainer *_actionGrid = nullptr;
	ScheduleWidget *_scheduleWidget = nullptr;
	WindowControlsWidget *_windowControls = nullptr;
	OpenMeeting::MeetingCatalogController *_meetingCatalog = nullptr;
	bool _sessionInvalidationDialogActive = false;
	MeetingEntryGuard _meetingEntryGuard;
	std::unique_ptr<QObject> _pendingMeetingReservation;
	QString _pendingMeetingId;
	bool _pendingShareScreen = false;
	quint64 _pendingMeetingEntryGeneration = 0;
	QPointer<QDialog> _pendingMeetingProgress;

#if defined(Q_OS_WIN)
	HWND _handle = nullptr;
#endif
};

} // namespace MeetingUI
