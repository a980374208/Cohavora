#include "src/ui/login_dialog.h"
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
    setWindowTitle(QString::fromUtf8("OpenMeeting 登录"));
    setFixedSize(460, 600);
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, true);

    initUI();
    loadSavedData();
    connect(_rememberBox, &QCheckBox::toggled, this, [this](bool checked) {
        _autoLoginBox->setEnabled(checked);
        if (!checked) {
            _autoLoginBox->setChecked(false);
            if (!_session.forgetSavedSession()) showError(_session.persistenceMessage());
            updateSavedSessionAction();
        }
    });
    connect(_accountInput, &QLineEdit::textChanged, this, [this] { updateSavedSessionAction(); });
    connect(_serverUrlInput, &QLineEdit::textChanged, this, [this] { updateSavedSessionAction(); });
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
    setStyleSheet(R"(
        QDialog {
            background: transparent;
        }
        #loginCard {
            background-color: #ffffff;
            border-radius: 16px;
            border: 1px solid #e1e4ea;
        }
        QLabel {
            color: #1f2329;
            font-family: "Microsoft YaHei", "Segoe UI", sans-serif;
        }
        QLineEdit {
            border: 1px solid #dcdfe6;
            border-radius: 8px;
            padding: 10px 14px;
            font-size: 13px;
            background: #f8f9fa;
        }
        QLineEdit:focus {
            border: 1px solid #1677ff;
            background: #ffffff;
        }
        QPushButton#primaryBtn {
            background-color: #1677ff;
            color: #ffffff;
            border-radius: 8px;
            padding: 10px 24px;
            font-size: 14px;
            font-weight: bold;
            border: none;
        }
        QPushButton#primaryBtn:hover {
            background-color: #4096ff;
        }
        QPushButton#primaryBtn:pressed {
            background-color: #0958d9;
        }
        QPushButton#primaryBtn:disabled {
            background-color: #b7d6ff;
        }
        QPushButton#guestBtn {
            background-color: #f2f3f5;
            color: #1f2329;
            border-radius: 8px;
            padding: 10px 24px;
            font-size: 14px;
            border: 1px solid #dcdfe6;
        }
        QPushButton#guestBtn:hover {
            background-color: #e5e6eb;
        }
        QPushButton#linkBtn {
            background: transparent;
            color: #1677ff;
            font-size: 12px;
            border: none;
            padding: 0;
            text-align: left;
        }
        QPushButton#linkBtn:hover {
            color: #4096ff;
            text-decoration: underline;
        }
        QPushButton#closeBtn {
            background: transparent;
            border: none;
            font-size: 16px;
            color: #8c8c8c;
            border-radius: 4px;
        }
        QPushButton#closeBtn:hover {
            background: #f5f5f5;
            color: #f53f3f;
        }
        QCheckBox {
            font-size: 12px;
            color: #606266;
        }
        QTabWidget::pane {
            border: none;
            background: transparent;
        }
        QTabBar::tab {
            background: transparent;
            color: #8c8c8c;
            font-size: 13px;
            font-weight: 500;
            padding: 8px 14px;
            min-width: 80px;
            border-bottom: 2px solid transparent;
        }
        QTabBar::tab:selected {
            color: #1677ff;
            border-bottom: 2px solid #1677ff;
            font-weight: bold;
        }
    )");

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

    auto subtitle = new QLabel(QString::fromUtf8("基于 LiveKit & WebRTC 的现代化音视频会议"), card);
    subtitle->setStyleSheet("color: #8c8c8c; font-size: 12px;");
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
    _accountInput->setPlaceholderText(QString::fromUtf8("请输入手机号或账号"));
    accLayout->addWidget(_accountInput);

    auto pwdLayout = new QHBoxLayout();
    _passwordInput = new QLineEdit(accountTab);
    _passwordInput->setPlaceholderText(QString::fromUtf8("请输入登录密码"));
    _passwordInput->setEchoMode(QLineEdit::Password);
    pwdLayout->addWidget(_passwordInput);

    _togglePwdBtn = new QPushButton(QString::fromUtf8("👁"), accountTab);
    _togglePwdBtn->setFixedSize(36, 36);
    _togglePwdBtn->setStyleSheet("border: 1px solid #dcdfe6; border-radius: 8px; background: #f8f9fa;");
    connect(_togglePwdBtn, &QPushButton::clicked, this, &LoginDialog::togglePasswordVisibility);
    pwdLayout->addWidget(_togglePwdBtn);
    accLayout->addLayout(pwdLayout);

    // 记住登录状态与自动登录 + 快速去注册
    auto optLayout = new QHBoxLayout();
    _rememberBox = new QCheckBox(QString::fromUtf8("记住登录状态"), accountTab);
    _rememberBox->setObjectName("rememberSession");
    _autoLoginBox = new QCheckBox(QString::fromUtf8("自动登录"), accountTab);
    _autoLoginBox->setObjectName("autoLogin");
    optLayout->addWidget(_rememberBox);
    optLayout->addWidget(_autoLoginBox);
    optLayout->addStretch();
    _toRegisterLinkBtn = new QPushButton(QString::fromUtf8("注册新账号 ➔"), accountTab);
    _toRegisterLinkBtn->setObjectName("linkBtn");
    connect(_toRegisterLinkBtn, &QPushButton::clicked, this, [this]() {
        if (_tabWidget) _tabWidget->setCurrentIndex(1);
    });
    optLayout->addWidget(_toRegisterLinkBtn);
    accLayout->addLayout(optLayout);

    _loginBtn = new QPushButton(QString::fromUtf8("登 录"), accountTab);
    _loginBtn->setObjectName("primaryBtn");
    _loginBtn->setFixedHeight(40);
    connect(_loginBtn, &QPushButton::clicked, this, &LoginDialog::onLoginClicked);
    _resumeBtn = new QPushButton(QString::fromUtf8("继续使用已保存账号"), accountTab);
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

    _tabWidget->addTab(accountTab, QString::fromUtf8("账号登录"));

    // --- Tab 2: 新用户注册 ---
    auto regTab = new QWidget(_tabWidget);
    auto regLayout = new QVBoxLayout(regTab);
    regLayout->setContentsMargins(0, 12, 0, 0);
    regLayout->setSpacing(10);

    _regAccountInput = new QLineEdit(regTab);
    _regAccountInput->setPlaceholderText(QString::fromUtf8("请输入注册账号或手机号"));
    regLayout->addWidget(_regAccountInput);

    _regNicknameInput = new QLineEdit(regTab);
    _regNicknameInput->setPlaceholderText(QString::fromUtf8("请输入用户昵称"));
    regLayout->addWidget(_regNicknameInput);

    auto regPwdLayout = new QHBoxLayout();
    _regPasswordInput = new QLineEdit(regTab);
    _regPasswordInput->setPlaceholderText(QString::fromUtf8("设置登录密码 (至少6位)"));
    _regPasswordInput->setEchoMode(QLineEdit::Password);
    regPwdLayout->addWidget(_regPasswordInput);

    _toggleRegPwdBtn = new QPushButton(QString::fromUtf8("👁"), regTab);
    _toggleRegPwdBtn->setFixedSize(36, 36);
    _toggleRegPwdBtn->setStyleSheet("border: 1px solid #dcdfe6; border-radius: 8px; background: #f8f9fa;");
    connect(_toggleRegPwdBtn, &QPushButton::clicked, this, &LoginDialog::toggleRegPasswordVisibility);
    regPwdLayout->addWidget(_toggleRegPwdBtn);
    regLayout->addLayout(regPwdLayout);

    _regConfirmPwdInput = new QLineEdit(regTab);
    _regConfirmPwdInput->setPlaceholderText(QString::fromUtf8("再次输入确认密码"));
    _regConfirmPwdInput->setEchoMode(QLineEdit::Password);
    regLayout->addWidget(_regConfirmPwdInput);

    _registerBtn = new QPushButton(QString::fromUtf8("注 册"), regTab);
    _registerBtn->setObjectName("primaryBtn");
    _registerBtn->setFixedHeight(40);
    connect(_registerBtn, &QPushButton::clicked, this, &LoginDialog::onRegisterClicked);
    regLayout->addWidget(_registerBtn);

    auto toLoginLayout = new QHBoxLayout();
    toLoginLayout->addStretch();
    _toLoginLinkBtn = new QPushButton(QString::fromUtf8("已有账号？直接登录 ➔"), regTab);
    _toLoginLinkBtn->setObjectName("linkBtn");
    connect(_toLoginLinkBtn, &QPushButton::clicked, this, [this]() {
        if (_tabWidget) _tabWidget->setCurrentIndex(0);
    });
    toLoginLayout->addWidget(_toLoginLinkBtn);
    regLayout->addLayout(toLoginLayout);

    regLayout->addStretch();

    _tabWidget->addTab(regTab, QString::fromUtf8("用户注册"));

    // --- Tab 3: 访客/调试体验 ---
    auto guestTab = new QWidget(_tabWidget);
    auto guestLayout = new QVBoxLayout(guestTab);
    guestLayout->setContentsMargins(0, 12, 0, 0);
    guestLayout->setSpacing(14);

    auto guestDesc = new QLabel(QString::fromUtf8("无需注册账号，输入昵称即可快速加入会议或进行本地 RTC 功能测试。"), guestTab);
    guestDesc->setWordWrap(true);
    guestDesc->setStyleSheet("color: #606266; font-size: 12px; line-height: 1.4;");
    guestLayout->addWidget(guestDesc);

    _guestNicknameInput = new QLineEdit(guestTab);
    _guestNicknameInput->setPlaceholderText(QString::fromUtf8("请输入参会昵称 (如: Alice)"));
    guestLayout->addWidget(_guestNicknameInput);

    guestLayout->addSpacing(10);
    _guestBtn = new QPushButton(QString::fromUtf8("以访客身份进入"), guestTab);
    _guestBtn->setObjectName("guestBtn");
    _guestBtn->setFixedHeight(40);
    connect(_guestBtn, &QPushButton::clicked, this, &LoginDialog::onGuestLoginClicked);
    guestLayout->addWidget(_guestBtn);
    guestLayout->addStretch();

    _tabWidget->addTab(guestTab, QString::fromUtf8("访客体验"));

    cardLayout->addWidget(_tabWidget);

    // 全局网络设置（登录 / 注册 / 访客通用）
    auto advToggleLayout = new QHBoxLayout();
    advToggleLayout->addStretch();
    _advancedToggleBtn = new QPushButton(QString::fromUtf8("⚙ 服务器设置 ▾"), card);
    _advancedToggleBtn->setObjectName("linkBtn");
    _advancedToggleBtn->setStyleSheet("color: #8c8c8c; font-size: 11px;");
    connect(_advancedToggleBtn, &QPushButton::clicked, this, &LoginDialog::toggleAdvancedSettings);
    advToggleLayout->addWidget(_advancedToggleBtn);
    advToggleLayout->addStretch();
    cardLayout->addLayout(advToggleLayout);

    _advancedWidget = new QWidget(card);
    auto advLayout = new QHBoxLayout(_advancedWidget);
    advLayout->setContentsMargins(0, 2, 0, 2);
    advLayout->setSpacing(6);
    auto advLabel = new QLabel(QString::fromUtf8("服务器:"), _advancedWidget);
    advLabel->setStyleSheet("color: #606266; font-size: 12px;");
    _serverUrlInput = new QLineEdit(_advancedWidget);
    _serverUrlInput->setPlaceholderText(QString::fromUtf8("如 http://123.56.225.164:11102"));
    advLayout->addWidget(advLabel);
    advLayout->addWidget(_serverUrlInput);
    _advancedWidget->setVisible(false);
    cardLayout->addWidget(_advancedWidget);

    // 错误/成功提示 Label（自适应换行，确保不会被卡片边缘截断）
    _errorLabel = new QLabel(card);
    _errorLabel->setStyleSheet("color: #f53f3f; font-size: 12px; padding: 2px 4px;");
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
    _guestNicknameInput->setText(QString::fromUtf8("访客_%1").arg(QDateTime::currentDateTime().toString("mmss")));
    updateSavedSessionAction();
    if (!session.persistenceMessage().isEmpty()) showError(session.persistenceMessage());
}

void LoginDialog::updateSavedSessionAction() {
    _resumeBtn->setVisible(_session.hasSavedSession() &&
        _accountInput->text().trimmed() == _session.savedAccount() &&
        OpenMeeting::canonicalServiceUrl(_serverUrlInput->text()) == _session.serverBaseUrl());
}

void LoginDialog::acceptAuthenticatedSession() {
    const QPointer<LoginDialog> self(this);
    const auto generation = _session.authGeneration();
    const auto warning = _session.persistenceMessage();
    if (!warning.isEmpty()) QMessageBox::warning(this, QString::fromUtf8("登录状态"), warning);
    if (!self || _session.authGeneration() != generation || !_session.isLoggedIn()) return;
    _passwordInput->clear();
    accept();
}

void LoginDialog::toggleAdvancedSettings() {
    bool isVisible = _advancedWidget->isVisible();
    _advancedWidget->setVisible(!isVisible);
    _advancedToggleBtn->setText(!isVisible ? QString::fromUtf8("⚙ 服务器设置 ▴") : QString::fromUtf8("⚙ 服务器设置 ▾"));
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
    _rememberBox->setEnabled(!loading);
    _autoLoginBox->setEnabled(!loading && _rememberBox->isChecked());
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
            _loginBtn->setText(QString::fromUtf8("正在登录..."));
            if (_registerBtn) _registerBtn->setText(QString::fromUtf8("正在注册..."));
        }
        _errorLabel->setVisible(false);
    } else {
        _loginBtn->setText(QString::fromUtf8("登 录"));
        if (_registerBtn) _registerBtn->setText(QString::fromUtf8("注 册"));
    }
}

void LoginDialog::showError(const QString &msg) {
    _errorLabel->setStyleSheet("color: #f53f3f; font-size: 12px; padding: 2px 0;");
    _errorLabel->setText(msg);
    _errorLabel->setVisible(!msg.isEmpty());
}

void LoginDialog::showSuccess(const QString &msg) {
    _errorLabel->setStyleSheet("color: #00b578; font-size: 12px; padding: 2px 0; font-weight: bold;");
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
        showError(QString::fromUtf8("请输入注册账号或手机号"));
        _regAccountInput->setFocus();
        return;
    }
    if (nickname.isEmpty()) {
        showError(QString::fromUtf8("请输入用户昵称"));
        _regNicknameInput->setFocus();
        return;
    }
    if (pwd.isEmpty()) {
        showError(QString::fromUtf8("请设置登录密码"));
        _regPasswordInput->setFocus();
        return;
    }
    if (pwd.length() < 6) {
        showError(QString::fromUtf8("密码长度不能少于 6 位"));
        _regPasswordInput->setFocus();
        return;
    }
    if (pwd != confirmPwd) {
        showError(QString::fromUtf8("两次输入的密码不一致，请重新确认"));
        _regConfirmPwdInput->setFocus();
        return;
    }

    auto &session = _session;
    if (!session.setServerBaseUrl(serverUrl)) {
        showError(QString::fromUtf8("请输入有效的服务器地址。"));
        return;
    }

    setLoading(true, QString::fromUtf8("正在注册..."));

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
                self->showSuccess(QString::fromUtf8("注册成功！已自动填充账号密码，请点击登录"));
            } else {
                self->showError(errMsg.isEmpty() ? QString::fromUtf8("注册失败，请稍后重试") : errMsg);
            }
        });
}

void LoginDialog::onLoginClicked() {
    const QString account = _accountInput ? _accountInput->text().trimmed() : QString();
    const QString password = _passwordInput ? _passwordInput->text() : QString();
    const QString serverUrl = _serverUrlInput ? _serverUrlInput->text().trimmed() : QString();

    if (account.isEmpty()) {
        showError(QString::fromUtf8("请输入账号或手机号"));
        if (_accountInput) _accountInput->setFocus();
        return;
    }
    if (password.isEmpty()) {
        showError(QString::fromUtf8("请输入登录密码"));
        if (_passwordInput) _passwordInput->setFocus();
        return;
    }

    auto &session = _session;
    if (!session.setServerBaseUrl(serverUrl)) {
        showError(QString::fromUtf8("请输入有效的服务器地址。"));
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
                self->showError(errMsg.isEmpty() ? QString::fromUtf8("登录失败，请检查账号密码或服务器连接") : errMsg);
            }
        });
    if (self) _loginGeneration = session.authGeneration();
}

void LoginDialog::onGuestLoginClicked() {
    QString nickname = _guestNicknameInput->text().trimmed();
    if (nickname.isEmpty()) {
        nickname = QString::fromUtf8("访客用户");
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
