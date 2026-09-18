#pragma once

#include <QtWidgets/QDialog>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QTabWidget>

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
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;

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
    void updateEndpointOptions();
    void cancelLogin();
    void acceptAuthenticatedSession();
    void setLoading(bool loading, const QString &text = QString());
    void showError(const QString &msg);
    void showSuccess(const QString &msg);

    QTabWidget *_tabWidget = nullptr;

    // 账号密码登录
    QLineEdit *_accountInput = nullptr;
    QLineEdit *_passwordInput = nullptr;
    QPushButton *_togglePwdBtn = nullptr;
    QCheckBox *_rememberBox = nullptr;
    QCheckBox *_autoLoginBox = nullptr;
    QPushButton *_toRegisterLinkBtn = nullptr;
    QPushButton *_loginBtn = nullptr;
    QPushButton *_resumeBtn = nullptr;
    OpenMeeting::SessionManager &_session;
    bool _loginInFlight = false;
    quint64 _loginGeneration = 0;

    // 用户注册
    QLineEdit *_regAccountInput = nullptr;
    QLineEdit *_regNicknameInput = nullptr;
    QLineEdit *_regPasswordInput = nullptr;
    QLineEdit *_regConfirmPwdInput = nullptr;
    QPushButton *_toggleRegPwdBtn = nullptr;
    QPushButton *_registerBtn = nullptr;
    QPushButton *_toLoginLinkBtn = nullptr;

    // 高级设置
    QWidget *_advancedWidget = nullptr;
    QLineEdit *_serverUrlInput = nullptr;
    QPushButton *_advancedToggleBtn = nullptr;

    // 访客入会
    QLineEdit *_guestNicknameInput = nullptr;
    QPushButton *_guestBtn = nullptr;

    // 状态提示
    QLabel *_errorLabel = nullptr;

    // 窗口拖动
    QPoint _dragPosition;
    bool _isDragging = false;
};

} // namespace MeetingUI
