#include <QtCore/QCoreApplication>
#include "src/ui/app_theme.h"
#include "src/ui/login_dialog.h"
#include "src/net/service_endpoint_policy.h"
#include "src/net/session_manager.h"
#include <QtCore/QDateTime>
#include <QtCore/QPointer>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QGraphicsDropShadowEffect>
#include <QtWidgets/QMessageBox>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QFont>

namespace MeetingUI {

LoginDialog::LoginDialog(QWidget *parent)
    : LoginDialog(OpenMeeting::SessionManager::instance(), parent) {
}

LoginDialog::LoginDialog(OpenMeeting::SessionManager &session, QWidget *parent)
    : QDialog(parent), _session(session) {
    setWindowTitle(QCoreApplication::translate("MeetingUI", "OpenMeeting Sign In"));
    resize(460, 600);
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, true);

    initUI();
    AppTheme::makeDialogAdaptive(*this, QSize(460, 600));
    loadSavedData();
    connect(_rememberBox, &QCheckBox::toggled, this, [this](bool checked) {
        _autoLoginBox->setEnabled(checked &&
            OpenMeeting::serviceAllowsCredentialPersistence(_serverUrlInput->text()));
        if (!checked) {
            _autoLoginBox->setChecked(false);
            if (!_session.forgetSavedSession()) showError(_session.persistenceMessage());
            updateSavedSessionAction();
        }
    });
    connect(_accountInput, &QLineEdit::textChanged, this, [this] { updateSavedSessionAction(); });
    connect(_serverUrlInput, &QLineEdit::textChanged, this, [this] {
        updateSavedSessionAction();
        updateEndpointOptions();
    });
}

LoginDialog::~LoginDialog() {
    cancelLogin();
}

void LoginDialog::cancelLogin() {
    if (_loginInFlight && _session.authGeneration() == _loginGeneration) _session.cancelPendingLogin();
    _loginInFlight = false;
}

void LoginDialog::reject() {
    cancelLogin();
    QDialog::reject();
}

void LoginDialog::initUI() {
    MeetingUI::AppTheme::setStyleVariant(*this, "login-dialog-this");

    auto rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(12, 12, 12, 12);

    auto card = new QWidget(this);
    card->setObjectName("loginCard");
    rootLayout->addWidget(card);

    auto cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(28, 20, 28, 24);
    cardLayout->setSpacing(12);

    // 顶部操作栏（关闭按钮）
    auto topBar = new QHBoxLayout();
    topBar->addStretch();
    auto closeBtn = new QPushButton(QString::fromUtf8("✕"), card);
    closeBtn->setObjectName("closeBtn");
    closeBtn->setFixedSize(28, 28);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    topBar->addWidget(closeBtn);
    cardLayout->addLayout(topBar);

    // 标题区域
    auto titleBox = new QVBoxLayout();
    titleBox->setSpacing(4);
    auto title = new QLabel(QString::fromUtf8("OpenMeeting"), card);
    QFont tf = title->font();
    tf.setPixelSize(22);
    tf.setBold(true);
    title->setFont(tf);
    title->setAlignment(Qt::AlignCenter);

    auto subtitle = new QLabel(QCoreApplication::translate("MeetingUI", "Video meetings powered by LiveKit & WebRTC"), card);
    MeetingUI::AppTheme::setStyleVariant(*subtitle, "login-dialog-subtitle");
    subtitle->setAlignment(Qt::AlignCenter);

    titleBox->addWidget(title);
    titleBox->addWidget(subtitle);
    cardLayout->addLayout(titleBox);
    cardLayout->addSpacing(8);

    // 选项卡：账号登录 vs 用户注册 vs 访客体验
    _tabWidget = new QTabWidget(card);
    _tabWidget->setDocumentMode(true);

    // --- Tab 1: 账号登录 ---
    auto accountTab = new QWidget(_tabWidget);
    auto accLayout = new QVBoxLayout(accountTab);
    accLayout->setContentsMargins(0, 12, 0, 0);
    accLayout->setSpacing(12);

    _accountInput = new QLineEdit(accountTab);
    _accountInput->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Phone number or account"));
    accLayout->addWidget(_accountInput);

    auto pwdLayout = new QHBoxLayout();
    _passwordInput = new QLineEdit(accountTab);
    _passwordInput->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Password"));
    _passwordInput->setEchoMode(QLineEdit::Password);
    pwdLayout->addWidget(_passwordInput);

    _togglePwdBtn = new QPushButton(QString::fromUtf8("👁"), accountTab);
    _togglePwdBtn->setFixedSize(36, 36);
    MeetingUI::AppTheme::setStyleVariant(*_togglePwdBtn, "login-dialog-togglepwdbtn");
    connect(_togglePwdBtn, &QPushButton::clicked, this, &LoginDialog::togglePasswordVisibility);
    pwdLayout->addWidget(_togglePwdBtn);
    accLayout->addLayout(pwdLayout);

    // 记住登录状态与自动登录 + 快速去注册
    auto optLayout = new QHBoxLayout();
    _rememberBox = new QCheckBox(QCoreApplication::translate("MeetingUI", "Remember sign-in"), accountTab);
    _rememberBox->setObjectName("rememberSession");
    _autoLoginBox = new QCheckBox(QCoreApplication::translate("MeetingUI", "Sign in automatically"), accountTab);
    _autoLoginBox->setObjectName("autoLogin");
    optLayout->addWidget(_rememberBox);
    optLayout->addWidget(_autoLoginBox);
    optLayout->addStretch();
    _toRegisterLinkBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Create an account ➔"), accountTab);
    _toRegisterLinkBtn->setObjectName("linkBtn");
    connect(_toRegisterLinkBtn, &QPushButton::clicked, this, [this]() {
        if (_tabWidget) _tabWidget->setCurrentIndex(1);
    });
    optLayout->addWidget(_toRegisterLinkBtn);
    accLayout->addLayout(optLayout);

    _loginBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Sign In"), accountTab);
    _loginBtn->setObjectName("primaryBtn");
    _loginBtn->setMinimumHeight(40);
    connect(_loginBtn, &QPushButton::clicked, this, &LoginDialog::onLoginClicked);
    _resumeBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Continue with Saved Account"), accountTab);
    _resumeBtn->setObjectName("resumeSavedSession");
    accLayout->addWidget(_resumeBtn);
    connect(_resumeBtn, &QPushButton::clicked, this, [this] {
        if (_accountInput->text().trimmed() != _session.savedAccount() ||
            OpenMeeting::canonicalServiceUrl(_serverUrlInput->text()) != _session.serverBaseUrl()) return;
        const QPointer<LoginDialog> self(this);
        const bool resumed = _session.resumeSavedSession(false, _autoLoginBox->isChecked());
        if (!self) return;
        if (resumed) acceptAuthenticatedSession();
        else {
            showError(_session.persistenceMessage());
            updateSavedSessionAction();
        }
    });
    accLayout->addWidget(_loginBtn);

    _tabWidget->addTab(accountTab, QCoreApplication::translate("MeetingUI", "Account Sign-In"));

    // --- Tab 2: 新用户注册 ---
    auto regTab = new QWidget(_tabWidget);
    auto regLayout = new QVBoxLayout(regTab);
    regLayout->setContentsMargins(0, 12, 0, 0);
    regLayout->setSpacing(10);

    _regAccountInput = new QLineEdit(regTab);
    _regAccountInput->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Account or phone number"));
    regLayout->addWidget(_regAccountInput);

    _regNicknameInput = new QLineEdit(regTab);
    _regNicknameInput->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Display name"));
    regLayout->addWidget(_regNicknameInput);

    auto regPwdLayout = new QHBoxLayout();
    _regPasswordInput = new QLineEdit(regTab);
    _regPasswordInput->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Password (at least 6 characters)"));
    _regPasswordInput->setEchoMode(QLineEdit::Password);
    regPwdLayout->addWidget(_regPasswordInput);

    _toggleRegPwdBtn = new QPushButton(QString::fromUtf8("👁"), regTab);
    _toggleRegPwdBtn->setFixedSize(36, 36);
    MeetingUI::AppTheme::setStyleVariant(*_toggleRegPwdBtn, "login-dialog-toggleregpwdbtn");
    connect(_toggleRegPwdBtn, &QPushButton::clicked, this, &LoginDialog::toggleRegPasswordVisibility);
    regPwdLayout->addWidget(_toggleRegPwdBtn);
    regLayout->addLayout(regPwdLayout);

    _regConfirmPwdInput = new QLineEdit(regTab);
    _regConfirmPwdInput->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Confirm password"));
    _regConfirmPwdInput->setEchoMode(QLineEdit::Password);
    regLayout->addWidget(_regConfirmPwdInput);

    _registerBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Sign Up"), regTab);
    _registerBtn->setObjectName("primaryBtn");
    _registerBtn->setMinimumHeight(40);
    connect(_registerBtn, &QPushButton::clicked, this, &LoginDialog::onRegisterClicked);
    regLayout->addWidget(_registerBtn);

    auto toLoginLayout = new QHBoxLayout();
    toLoginLayout->addStretch();
    _toLoginLinkBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Already have an account? Sign in ➔"), regTab);
    _toLoginLinkBtn->setObjectName("linkBtn");
    connect(_toLoginLinkBtn, &QPushButton::clicked, this, [this]() {
        if (_tabWidget) _tabWidget->setCurrentIndex(0);
    });
    toLoginLayout->addWidget(_toLoginLinkBtn);
    regLayout->addLayout(toLoginLayout);

    regLayout->addStretch();

    _tabWidget->addTab(regTab, QCoreApplication::translate("MeetingUI", "Create Account"));

    // --- Tab 3: 访客/调试体验 ---
    auto guestTab = new QWidget(_tabWidget);
    auto guestLayout = new QVBoxLayout(guestTab);
    guestLayout->setContentsMargins(0, 12, 0, 0);
    guestLayout->setSpacing(14);

    auto guestDesc = new QLabel(QCoreApplication::translate("MeetingUI", "Enter a display name to join a meeting or test local RTC features without creating an account."), guestTab);
    guestDesc->setWordWrap(true);
    MeetingUI::AppTheme::setStyleVariant(*guestDesc, "login-dialog-guestdesc");
    guestLayout->addWidget(guestDesc);

    _guestNicknameInput = new QLineEdit(guestTab);
    _guestNicknameInput->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Display name (e.g. Alice)"));
    guestLayout->addWidget(_guestNicknameInput);

    guestLayout->addSpacing(10);
    _guestBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Continue as Guest"), guestTab);
    _guestBtn->setObjectName("guestBtn");
    _guestBtn->setMinimumHeight(40);
    connect(_guestBtn, &QPushButton::clicked, this, &LoginDialog::onGuestLoginClicked);
    guestLayout->addWidget(_guestBtn);
    guestLayout->addStretch();

    _tabWidget->addTab(guestTab, QCoreApplication::translate("MeetingUI", "Guest Access"));

    cardLayout->addWidget(_tabWidget);

    // 全局网络设置（登录 / 注册 / 访客通用）
    auto advToggleLayout = new QHBoxLayout();
    advToggleLayout->addStretch();
    _advancedToggleBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "⚙ Server Settings ▾"), card);
    _advancedToggleBtn->setObjectName("linkBtn");
    MeetingUI::AppTheme::setStyleVariant(*_advancedToggleBtn, "login-dialog-advancedtogglebtn");
    connect(_advancedToggleBtn, &QPushButton::clicked, this, &LoginDialog::toggleAdvancedSettings);
    advToggleLayout->addWidget(_advancedToggleBtn);
    advToggleLayout->addStretch();
    cardLayout->addLayout(advToggleLayout);

    _advancedWidget = new QWidget(card);
    auto advLayout = new QHBoxLayout(_advancedWidget);
    advLayout->setContentsMargins(0, 2, 0, 2);
    advLayout->setSpacing(6);
    auto advLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Server:"), _advancedWidget);
    MeetingUI::AppTheme::setStyleVariant(*advLabel, "login-dialog-advlabel");
    _serverUrlInput = new QLineEdit(_advancedWidget);
    _serverUrlInput->setPlaceholderText(QString::fromUtf8("https://api.example.com"));
    advLayout->addWidget(advLabel);
    advLayout->addWidget(_serverUrlInput);
    _advancedWidget->setVisible(false);
    cardLayout->addWidget(_advancedWidget);

    // 错误/成功提示 Label（自适应换行，确保不会被卡片边缘截断）
    _errorLabel = new QLabel(card);
    MeetingUI::AppTheme::setStyleVariant(*_errorLabel, "login-dialog-errorlabel");
    _errorLabel->setAlignment(Qt::AlignCenter);
    _errorLabel->setWordWrap(true);
    _errorLabel->setMinimumHeight(32);
    _errorLabel->setVisible(false);
    cardLayout->addWidget(_errorLabel);

    // 支持回车快捷操作
    connect(_passwordInput, &QLineEdit::returnPressed, this, &LoginDialog::onLoginClicked);
    connect(_regConfirmPwdInput, &QLineEdit::returnPressed, this, &LoginDialog::onRegisterClicked);
    connect(_guestNicknameInput, &QLineEdit::returnPressed, this, &LoginDialog::onGuestLoginClicked);
}

void LoginDialog::loadSavedData() {
    auto &session = _session;

    _accountInput->setText(session.savedAccount());
    _passwordInput->clear();
    _passwordInput->setObjectName("loginPassword");
    _accountInput->setObjectName("loginAccount");
    _serverUrlInput->setObjectName("serverBaseUrl");
    _rememberBox->setChecked(session.isRememberSession());
    _autoLoginBox->setChecked(session.isAutoLogin());
    _autoLoginBox->setEnabled(session.isRememberSession());
    _serverUrlInput->setText(session.serverBaseUrl());
    _guestNicknameInput->setText(QCoreApplication::translate("MeetingUI", "Guest_%1").arg(QDateTime::currentDateTime().toString("mmss")));
    updateSavedSessionAction();
    updateEndpointOptions();
    if (session.serverBaseUrl().isEmpty()) {
        _advancedWidget->setVisible(true);
        _advancedToggleBtn->setText(QCoreApplication::translate("MeetingUI", "⚙ Server Settings ▴"));
    }
    if (!session.persistenceMessage().isEmpty()) showError(session.persistenceMessage());
}

void LoginDialog::updateSavedSessionAction() {
    _resumeBtn->setVisible(_session.hasSavedSession() &&
        _accountInput->text().trimmed() == _session.savedAccount() &&
        OpenMeeting::canonicalServiceUrl(_serverUrlInput->text()) == _session.serverBaseUrl());
}

void LoginDialog::updateEndpointOptions() {
    const auto policy = OpenMeeting::evaluateServiceEndpoint(_serverUrlInput->text());
    const bool persistent = OpenMeeting::serviceAllowsCredentialPersistence(_serverUrlInput->text());
    _rememberBox->setEnabled(persistent);
    _autoLoginBox->setEnabled(persistent && _rememberBox->isChecked());
    _rememberBox->setText(policy.isDebugHttp()
        ? QCoreApplication::translate("MeetingUI", "Remember sign-in (debug HTTP traffic is unencrypted)")
        : QCoreApplication::translate("MeetingUI", "Remember sign-in"));
}

void LoginDialog::acceptAuthenticatedSession() {
    const QPointer<LoginDialog> self(this);
    const auto generation = _session.authGeneration();
    const auto warning = _session.persistenceMessage();
    if (!warning.isEmpty()) QMessageBox::warning(this, QCoreApplication::translate("MeetingUI", "Sign-In Status"), warning);
    if (!self || _session.authGeneration() != generation || !_session.isLoggedIn()) return;
    _passwordInput->clear();
    accept();
}

void LoginDialog::toggleAdvancedSettings() {
    bool isVisible = _advancedWidget->isVisible();
    _advancedWidget->setVisible(!isVisible);
    _advancedToggleBtn->setText(!isVisible ? QCoreApplication::translate("MeetingUI", "⚙ Server Settings ▴") : QCoreApplication::translate("MeetingUI", "⚙ Server Settings ▾"));
}

void LoginDialog::togglePasswordVisibility() {
    if (_passwordInput->echoMode() == QLineEdit::Password) {
        _passwordInput->setEchoMode(QLineEdit::Normal);
        _togglePwdBtn->setText(QString::fromUtf8("🔒"));
    } else {
        _passwordInput->setEchoMode(QLineEdit::Password);
        _togglePwdBtn->setText(QString::fromUtf8("👁"));
    }
}

void LoginDialog::setLoading(bool loading, const QString &text) {
    const bool persistent = OpenMeeting::serviceAllowsCredentialPersistence(
        _serverUrlInput->text());
    _rememberBox->setEnabled(!loading && persistent);
    _autoLoginBox->setEnabled(!loading && persistent && _rememberBox->isChecked());
    _serverUrlInput->setEnabled(!loading);
    _resumeBtn->setEnabled(!loading);
    _loginBtn->setEnabled(!loading);
    _guestBtn->setEnabled(!loading);
    _accountInput->setEnabled(!loading);
    _passwordInput->setEnabled(!loading);
    if (_registerBtn) _registerBtn->setEnabled(!loading);
    if (_regAccountInput) _regAccountInput->setEnabled(!loading);
    if (_regNicknameInput) _regNicknameInput->setEnabled(!loading);
    if (_regPasswordInput) _regPasswordInput->setEnabled(!loading);
    if (_regConfirmPwdInput) _regConfirmPwdInput->setEnabled(!loading);

    if (loading) {
        if (!text.isEmpty()) {
            if (_tabWidget && _tabWidget->currentIndex() == 1) {
                if (_registerBtn) _registerBtn->setText(text);
            } else {
                _loginBtn->setText(text);
            }
        } else {
            _loginBtn->setText(QCoreApplication::translate("MeetingUI", "Signing in..."));
            if (_registerBtn) _registerBtn->setText(QCoreApplication::translate("MeetingUI", "Creating account..."));
        }
        _errorLabel->setVisible(false);
    } else {
        _loginBtn->setText(QCoreApplication::translate("MeetingUI", "Sign In"));
        if (_registerBtn) _registerBtn->setText(QCoreApplication::translate("MeetingUI", "Sign Up"));
    }
}

void LoginDialog::showError(const QString &msg) {
    MeetingUI::AppTheme::setStyleVariant(*_errorLabel, "login-dialog-errorlabel-2");
    _errorLabel->setText(msg);
    _errorLabel->setVisible(!msg.isEmpty());
}

void LoginDialog::showSuccess(const QString &msg) {
    MeetingUI::AppTheme::setStyleVariant(*_errorLabel, "login-dialog-errorlabel-3");
    _errorLabel->setText(msg);
    _errorLabel->setVisible(!msg.isEmpty());
}

void LoginDialog::toggleRegPasswordVisibility() {
    if (!_regPasswordInput || !_toggleRegPwdBtn) return;
    if (_regPasswordInput->echoMode() == QLineEdit::Password) {
        _regPasswordInput->setEchoMode(QLineEdit::Normal);
        if (_regConfirmPwdInput) _regConfirmPwdInput->setEchoMode(QLineEdit::Normal);
        _toggleRegPwdBtn->setText(QString::fromUtf8("🔒"));
    } else {
        _regPasswordInput->setEchoMode(QLineEdit::Password);
        if (_regConfirmPwdInput) _regConfirmPwdInput->setEchoMode(QLineEdit::Password);
        _toggleRegPwdBtn->setText(QString::fromUtf8("👁"));
    }
}

void LoginDialog::onRegisterClicked() {
    const QString account = _regAccountInput->text().trimmed();
    const QString nickname = _regNicknameInput->text().trimmed();
    const QString pwd = _regPasswordInput->text();
    const QString confirmPwd = _regConfirmPwdInput->text();
    const QString serverUrl = _serverUrlInput->text().trimmed();

    if (account.isEmpty()) {
        showError(QCoreApplication::translate("MeetingUI", "Account or phone number"));
        _regAccountInput->setFocus();
        return;
    }
    if (nickname.isEmpty()) {
        showError(QCoreApplication::translate("MeetingUI", "Display name"));
        _regNicknameInput->setFocus();
        return;
    }
    if (pwd.isEmpty()) {
        showError(QCoreApplication::translate("MeetingUI", "Enter a password"));
        _regPasswordInput->setFocus();
        return;
    }
    if (pwd.length() < 6) {
        showError(QCoreApplication::translate("MeetingUI", "The password must contain at least 6 characters"));
        _regPasswordInput->setFocus();
        return;
    }
    if (pwd != confirmPwd) {
        showError(QCoreApplication::translate("MeetingUI", "The passwords do not match. Please try again."));
        _regConfirmPwdInput->setFocus();
        return;
    }

    auto &session = _session;
    if (!session.setServerBaseUrl(serverUrl)) {
        showError(OpenMeeting::serviceEndpointErrorMessage(
            OpenMeeting::evaluateServiceEndpoint(serverUrl).status));
        return;
    }

    setLoading(true, QCoreApplication::translate("MeetingUI", "Creating account..."));

    QPointer<LoginDialog> self = this;
    session.registerUser(account, pwd, nickname,
        [self, account, pwd](bool success, const QString &errMsg, const OpenMeeting::UserInfo &user) {
            Q_UNUSED(user);
            if (!self) return;
            self->setLoading(false);
            if (success) {
                if (self->_accountInput) self->_accountInput->setText(account);
                if (self->_passwordInput) self->_passwordInput->setText(pwd);
                if (self->_tabWidget) {
                    self->_tabWidget->setCurrentIndex(0);
                }
                self->showSuccess(QCoreApplication::translate("MeetingUI", "Account created! Your credentials are filled in. Click Sign In to continue."));
            } else {
                self->showError(errMsg.isEmpty() ? QCoreApplication::translate("MeetingUI", "Unable to create the account. Please try again later.") : errMsg);
            }
        });
}

void LoginDialog::onLoginClicked() {
    const QString account = _accountInput ? _accountInput->text().trimmed() : QString();
    const QString password = _passwordInput ? _passwordInput->text() : QString();
    const QString serverUrl = _serverUrlInput ? _serverUrlInput->text().trimmed() : QString();

    if (account.isEmpty()) {
        showError(QCoreApplication::translate("MeetingUI", "Enter an account or phone number"));
        if (_accountInput) _accountInput->setFocus();
        return;
    }
    if (password.isEmpty()) {
        showError(QCoreApplication::translate("MeetingUI", "Password"));
        if (_passwordInput) _passwordInput->setFocus();
        return;
    }

    auto &session = _session;
    if (!session.setServerBaseUrl(serverUrl)) {
        showError(OpenMeeting::serviceEndpointErrorMessage(
            OpenMeeting::evaluateServiceEndpoint(serverUrl).status));
        return;
    }

    setLoading(true);

    QPointer<LoginDialog> self = this;
    _loginInFlight = true;
    session.loginWithPassword(account, password, _rememberBox && _rememberBox->isChecked(), _autoLoginBox && _autoLoginBox->isChecked(),
        [self](bool success, const QString &errMsg) {
            if (!self) return;
            self->_loginInFlight = false;
            self->setLoading(false);
            if (success) {
                self->acceptAuthenticatedSession();
            } else {
                self->showError(errMsg.isEmpty() ? QCoreApplication::translate("MeetingUI", "Sign-in failed. Check your credentials and server connection.") : errMsg);
            }
        });
    if (self) _loginGeneration = session.authGeneration();
}

void LoginDialog::onGuestLoginClicked() {
    QString nickname = _guestNicknameInput->text().trimmed();
    if (nickname.isEmpty()) {
        nickname = QCoreApplication::translate("MeetingUI", "Guest User");
    }

    auto &session = _session;
    session.loginAsGuest(nickname);
    accept();
}

void LoginDialog::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton) {
        _isDragging = true;
        _dragPosition = e->globalPos() - frameGeometry().topLeft();
        e->accept();
    }
}

void LoginDialog::mouseMoveEvent(QMouseEvent *e) {
    if (_isDragging && (e->buttons() & Qt::LeftButton)) {
        move(e->globalPos() - _dragPosition);
        e->accept();
    }
}

} // namespace MeetingUI
