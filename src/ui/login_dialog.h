#pragma once

#include <QtWidgets/QDialog>
#include <QtWidgets/QFrame>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QTabWidget>
#include <array>

class QVBoxLayout;

namespace OpenMeeting { class SessionManager; }

namespace MeetingUI {

class LoginDialog : public QDialog {
    Q_OBJECT
public:
    explicit LoginDialog(QWidget *parent = nullptr);
    explicit LoginDialog(OpenMeeting::SessionManager &session, QWidget *parent = nullptr);
    ~LoginDialog() override;
    void reject() override;

protected:
    void showEvent(QShowEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onLoginClicked();
    void onRegisterClicked();
    void onGuestLoginClicked();
    void toggleAdvancedSettings();
    void togglePasswordVisibility();
    void toggleRegPasswordVisibility();

private:
    void initUI();
    void loadSavedData();
    void updateSavedSessionAction();
    void refreshPersistenceState();
    void updateEndpointOptions();
    void refreshThemeVisuals();
    void syncSegmentButtons(int index);
    void syncServerSummary();
    void scheduleFormFit();
    void fitFormToScreen();
    void cancelLogin();
    void acceptAuthenticatedSession();
    void setLoading(bool loading, const QString &text = QString());
    void showError(const QString &msg, QLineEdit *endpointInput = nullptr);
    void showSuccess(const QString &msg);

    QWidget *_brandBadge = nullptr;
    QPushButton *_closeBtn = nullptr;
    QVBoxLayout *_bodyLayout = nullptr;
    QVBoxLayout *_footerLayout = nullptr;
    QFrame *_footerBar = nullptr;
    QFrame *_transportBadge = nullptr;
    QWidget *_transportGlyph = nullptr;
    QLabel *_transportBadgeLabel = nullptr;
    QFrame *_segmentBar = nullptr;
    std::array<QPushButton *, 3> _segmentButtons{};
    QTabWidget *_tabWidget = nullptr;

    // 账号密码登录
    QFrame *_savedSessionCard = nullptr;
    QLabel *_savedAvatarLabel = nullptr;
    QLabel *_savedAccountLabel = nullptr;
    QLineEdit *_accountInput = nullptr;
    QFrame *_pwdBox = nullptr;
    QLineEdit *_passwordInput = nullptr;
    QPushButton *_togglePwdBtn = nullptr;
    QCheckBox *_rememberBox = nullptr;
    QCheckBox *_autoLoginBox = nullptr;
    QPushButton *_loginBtn = nullptr;
    QPushButton *_resumeBtn = nullptr;
    OpenMeeting::SessionManager &_session;
    bool _loginInFlight = false;
    bool _loading = false;
    quint64 _loginGeneration = 0;

    // 用户注册
    QLineEdit *_regAccountInput = nullptr;
    QLineEdit *_regNicknameInput = nullptr;
    QFrame *_regPwdBox = nullptr;
    QLineEdit *_regPasswordInput = nullptr;
    QLineEdit *_regConfirmPwdInput = nullptr;
    QPushButton *_toggleRegPwdBtn = nullptr;
    QPushButton *_registerBtn = nullptr;

    // 高级设置
    QWidget *_advancedWidget = nullptr;
    QLineEdit *_serverUrlInput = nullptr;
    QLineEdit *_registrationServerUrlInput = nullptr;
    QPushButton *_advancedToggleBtn = nullptr;

    // 访客入会
    QLineEdit *_guestNicknameInput = nullptr;
    QPushButton *_guestBtn = nullptr;

    // 状态提示
    QLabel *_errorLabel = nullptr;
    QLineEdit *_endpointErrorInput = nullptr;
    bool _formFitPending = false;

    // 窗口拖动
    QPoint _dragPosition;
    bool _isDragging = false;
};

} // namespace MeetingUI
