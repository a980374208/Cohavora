#include <QtCore/QCoreApplication>
#include "src/ui/app_theme.h"
#include "src/ui/app_branding.h"
#include "src/ui/login_dialog.h"
#ifdef Q_OS_LINUX
#include "src/ui/linux_window_frame.h"
#include <QtWidgets/QAbstractButton>
#include <utility>
#endif
#include "src/net/account_credentials_policy.h"
#include <QtCore/QSignalBlocker>
#include "src/net/service_endpoint_policy.h"
#include "src/net/session_manager.h"
#include <QtCore/QDateTime>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGraphicsDropShadowEffect>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QApplication>
#include <QtWidgets/QStyleOption>
#include <QtWidgets/QTabBar>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QFont>
#include <QtGui/QScreen>
#include <QtGui/QShowEvent>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>

namespace MeetingUI {
namespace {

// QTabWidget's default hints include every page, even those with an Ignored
// size policy. Login forms should measure only the selected page.
class CurrentPageTabWidget final : public QTabWidget {
public:
    using QTabWidget::QTabWidget;

    QSize sizeHint() const override {
        const auto *page = currentWidget();
        return sizeWithTabs(page ? page->sizeHint().expandedTo(page->minimumSize()) : QSize(0, 0), false);
    }

    QSize minimumSizeHint() const override {
        const auto *page = currentWidget();
        return sizeWithTabs(page ? page->minimumSizeHint().expandedTo(page->minimumSize()) : QSize(0, 0), true);
    }

    bool hasHeightForWidth() const override {
        return currentWidget() && currentWidget()->hasHeightForWidth();
    }

    int heightForWidth(int width) const override {
        const auto *page = currentWidget();
        if (!page) return sizeHint().height();
        QStyleOptionTabWidgetFrame option;
        initStyleOption(&option);
        option.state = QStyle::State_None;
        const auto padding = style()->sizeFromContents(QStyle::CT_TabWidget, &option, QSize(0, 0), this);
        const int pageWidth = qMax(1, width - padding.width());
        const int pageHeight = page->hasHeightForWidth()
            ? page->heightForWidth(pageWidth) : page->sizeHint().height();
        const int barHeight = tabBar()->isHidden() ? 0 : tabBar()->sizeHint().height();
        const int height = qMax(page->minimumSize().height(), pageHeight) + barHeight + padding.height();
        return qMax(height, QApplication::globalStrut().height());
    }

private:
    QSize sizeWithTabs(QSize pageSize, bool minimum) const {
        auto bar = tabBar()->isHidden() ? QSize(0, 0)
            : minimum ? tabBar()->minimumSizeHint() : tabBar()->sizeHint();
        if (!minimum && tabBar()->usesScrollButtons()) bar = bar.boundedTo(QSize(200, 200));
        const QSize contents(qMax(pageSize.width(), bar.width()), pageSize.height() + bar.height());
        QStyleOptionTabWidgetFrame option;
        initStyleOption(&option);
        option.state = QStyle::State_None;
        return style()->sizeFromContents(QStyle::CT_TabWidget, &option, contents, this)
            .expandedTo(QApplication::globalStrut());
    }
};

} // namespace

LoginDialog::LoginDialog(QWidget *parent)
    : LoginDialog(OpenMeeting::SessionManager::instance(), parent) {
}

LoginDialog::LoginDialog(OpenMeeting::SessionManager &session, QWidget *parent)
    : QDialog(parent), _session(session) {
    setWindowTitle(QCoreApplication::translate("MeetingUI", "Cohavora Sign In"));
    resize(460, 600);
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, true);

    initUI();
    AppTheme::makeDialogAdaptive(*this, QSize(460, 600));
#ifdef Q_OS_LINUX
    const QPointer<QWidget> card = findChild<QWidget *>(QStringLiteral("loginCard"));
    LinuxWindowFrameOptions frameOptions;
    frameOptions.radius = 16;
    frameOptions.captionHitTest = [this, card](QPoint point) {
        if (!card || !card->isVisibleTo(this) || !_tabWidget) return false;
        const auto local = card->mapFrom(this, point);
        if (!card->rect().contains(local) || local.y() >= _tabWidget->mapTo(card, QPoint()).y()) return false;
        // The shared application filter also sees the title labels. Limit the
        // caption to the header and leave its Close button independently usable.
        for (auto *child = card->childAt(local); child && child != card;
                child = child->parentWidget()) {
            if (qobject_cast<QAbstractButton *>(child) || child->focusPolicy() != Qt::NoFocus) return false;
        }
        return true;
    };
    installLinuxWindowFrame(*this, std::move(frameOptions));
#endif
    loadSavedData();
    connect(_rememberBox, &QCheckBox::toggled, this, [this](bool checked) {
        _autoLoginBox->setEnabled(checked &&
            OpenMeeting::credentialPersistenceSupported() && OpenMeeting::serviceAllowsCredentialPersistence(_serverUrlInput->text()));
        if (!checked) {
            _autoLoginBox->setChecked(false);
            const QPointer<LoginDialog> self(this);
#ifdef Q_OS_LINUX
            _session.forgetSavedSessionAsync([self](bool cleared) {
                if (self && !cleared) self->showError(self->_session.persistenceMessage());
            });
#else
            const bool cleared = _session.forgetSavedSession();
            if (self) self->refreshPersistenceState();
            if (self && !cleared) self->showError(self->_session.persistenceMessage());
#endif
        }
    });
    connect(&_session, &OpenMeeting::SessionManager::persistenceStateChanged,
            this, &LoginDialog::refreshPersistenceState);
    connect(_accountInput, &QLineEdit::textChanged, this, [this] { updateSavedSessionAction(); });
    const auto clearEndpointError = [this](QLineEdit *input) {
        if (_endpointErrorInput == input) showError(QString());
    };
    connect(_registrationServerUrlInput, &QLineEdit::textChanged, this, [this, clearEndpointError] {
        clearEndpointError(_registrationServerUrlInput);
    });
    connect(_serverUrlInput, &QLineEdit::textChanged, this, [this, clearEndpointError] {
        clearEndpointError(_serverUrlInput);
        _registrationServerUrlInput->setText(
            _session.registrationServerBaseUrl(_serverUrlInput->text()));
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
#ifdef Q_OS_LINUX
    rootLayout->setContentsMargins(0, 0, 0, 0);
#else
    rootLayout->setContentsMargins(12, 12, 12, 12);
#endif

    auto card = new QWidget(this);
    card->setObjectName("loginCard");
#ifdef Q_OS_LINUX
    // LinuxWindowFrame owns the outer round clipping, border and shadow.
    card->setProperty("linuxWindowFrameSurface", true);
#endif
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
    auto title = new QLabel(AppBranding::name(), card);
#ifdef Q_OS_LINUX
    title->setObjectName(QStringLiteral("loginTitle"));
#endif
    QFont tf = title->font();
    tf.setPixelSize(22);
    tf.setBold(true);
    title->setFont(tf);
    title->setAlignment(Qt::AlignCenter);

    auto subtitle = new QLabel(AppBranding::tagline(), card);
#ifdef Q_OS_LINUX
    subtitle->setObjectName(QStringLiteral("loginSubtitle"));
#endif
    MeetingUI::AppTheme::setStyleVariant(*subtitle, "login-dialog-subtitle");
    subtitle->setAlignment(Qt::AlignCenter);
    subtitle->setWordWrap(true);

    titleBox->addWidget(title);
    titleBox->addWidget(subtitle);
    cardLayout->addLayout(titleBox);
    cardLayout->addSpacing(8);

    // 选项卡：账号登录 vs 用户注册 vs 访客体验
    _tabWidget = new CurrentPageTabWidget(card);
    _tabWidget->setDocumentMode(true);

    // --- Tab 1: 账号登录 ---
    auto accountTab = new QWidget(_tabWidget);
    auto accLayout = new QVBoxLayout(accountTab);
    accLayout->setContentsMargins(0, 12, 0, 0);
    accLayout->setSpacing(12);

    _accountInput = new QLineEdit(accountTab);
    _accountInput->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Phone number or account (%1-%2 characters)")
        .arg(OpenMeeting::kMinAccountLength).arg(OpenMeeting::kMaxAccountLength));
    _accountInput->setAccessibleName(_accountInput->placeholderText());
    accLayout->addWidget(_accountInput);

    auto pwdLayout = new QHBoxLayout();
    _passwordInput = new QLineEdit(accountTab);
    _passwordInput->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Password (%1-%2 characters)")
        .arg(OpenMeeting::kMinPasswordLength).arg(OpenMeeting::kMaxPasswordLength));
    _passwordInput->setAccessibleName(_passwordInput->placeholderText());
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
    if (!OpenMeeting::credentialPersistenceSupported()) {
        const auto reason = QCoreApplication::translate("MeetingUI",
            "Remembering sign-in is not available on this platform yet.");
        _rememberBox->setToolTip(reason);
        _autoLoginBox->setToolTip(reason);
    }
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
        // Until unlock succeeds the stored choice is unknown. An unchecked,
        // disabled checkbox is not a request to rewrite that choice.
        const auto automaticChoice = _session.hasSavedSession()
            ? std::optional<bool>(_autoLoginBox->isChecked()) : std::nullopt;
        const QPointer<LoginDialog> self(this);
        setLoading(true);
        _loginInFlight = true;
        auto finished = [self](bool resumed) {
            if (!self) return;
            self->_loginInFlight = false;
            self->setLoading(false);
            if (resumed && self->_session.isLoggedIn()) self->acceptAuthenticatedSession();
            else {
                self->showError(self->_session.persistenceMessage());
                self->updateSavedSessionAction();
            }
        };
#ifdef Q_OS_LINUX
        _session.resumeSavedSessionAsync(false, automaticChoice, std::move(finished));
        if (self) self->_loginGeneration = self->_session.authGeneration();
#else
        const auto previousGeneration = _session.authGeneration();
        const auto resumeGeneration = previousGeneration + 1;
        _loginGeneration = resumeGeneration;
        const bool resumed = _session.resumeSavedSession(false, automaticChoice);
        if (!self || !self->_loginInFlight || self->_loginGeneration != resumeGeneration) return;
        const auto currentGeneration = self->_session.authGeneration();
        // A rejected request need not reset authentication. A later generation
        // belongs to a reentrant login/logout and must keep its own UI state.
        if (currentGeneration != previousGeneration && currentGeneration != resumeGeneration) return;
        self->_loginGeneration = currentGeneration;
        self->refreshPersistenceState();
        if (!self || !self->_loginInFlight || self->_loginGeneration != currentGeneration ||
            self->_session.authGeneration() != currentGeneration) return;
        finished(resumed);
#endif
    });
    accLayout->addWidget(_loginBtn);

    _tabWidget->addTab(accountTab, QCoreApplication::translate("MeetingUI", "Account Sign-In"));

    // --- Tab 2: 新用户注册 ---
    auto regTab = new QWidget(_tabWidget);
    auto regLayout = new QVBoxLayout(regTab);
    regLayout->setContentsMargins(0, 12, 0, 0);
    regLayout->setSpacing(10);

    _regAccountInput = new QLineEdit(regTab);
    _regAccountInput->setObjectName("registerAccount");
    _regAccountInput->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Account or phone number (%1-%2 characters)")
        .arg(OpenMeeting::kMinAccountLength).arg(OpenMeeting::kMaxAccountLength));
    regLayout->addWidget(_regAccountInput);

    _regNicknameInput = new QLineEdit(regTab);
    _regNicknameInput->setObjectName("registerNickname");
    _regNicknameInput->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Display name"));
    regLayout->addWidget(_regNicknameInput);

    auto regPwdLayout = new QHBoxLayout();
    _regPasswordInput = new QLineEdit(regTab);
    _regPasswordInput->setObjectName("registerPassword");
    _regPasswordInput->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Password (%1-%2 characters)")
        .arg(OpenMeeting::kMinPasswordLength).arg(OpenMeeting::kMaxPasswordLength));
    _regPasswordInput->setEchoMode(QLineEdit::Password);
    regPwdLayout->addWidget(_regPasswordInput);

    _toggleRegPwdBtn = new QPushButton(QString::fromUtf8("👁"), regTab);
    _toggleRegPwdBtn->setFixedSize(36, 36);
    MeetingUI::AppTheme::setStyleVariant(*_toggleRegPwdBtn, "login-dialog-toggleregpwdbtn");
    connect(_toggleRegPwdBtn, &QPushButton::clicked, this, &LoginDialog::toggleRegPasswordVisibility);
    regPwdLayout->addWidget(_toggleRegPwdBtn);
    regLayout->addLayout(regPwdLayout);

    _regConfirmPwdInput = new QLineEdit(regTab);
    _regConfirmPwdInput->setObjectName("registerConfirmPassword");
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
    _guestNicknameInput->setAccessibleName(_guestNicknameInput->placeholderText());
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
    _advancedToggleBtn->setObjectName("serverSettingsToggle");
    MeetingUI::AppTheme::setStyleVariant(*_advancedToggleBtn, "login-dialog-advancedtogglebtn");
    connect(_advancedToggleBtn, &QPushButton::clicked, this, &LoginDialog::toggleAdvancedSettings);
    advToggleLayout->addWidget(_advancedToggleBtn);
    advToggleLayout->addStretch();
    cardLayout->addLayout(advToggleLayout);

    _advancedWidget = new QWidget(card);
    auto advLayout = new QGridLayout(_advancedWidget);
    advLayout->setContentsMargins(0, 2, 0, 2);
    advLayout->setSpacing(6);
    auto advLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Meeting service:"), _advancedWidget);
    MeetingUI::AppTheme::setStyleVariant(*advLabel, "login-dialog-advlabel");
    _serverUrlInput = new QLineEdit(_advancedWidget);
    _serverUrlInput->setPlaceholderText(QString::fromUtf8("https://api.example.com"));
    _serverUrlInput->setAccessibleName(QCoreApplication::translate("MeetingUI", "Meeting service"));
    advLayout->addWidget(advLabel, 0, 0);
    advLayout->addWidget(_serverUrlInput, 0, 1);

    auto registrationLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Registration service:"), _advancedWidget);
    MeetingUI::AppTheme::setStyleVariant(*registrationLabel, "login-dialog-advlabel");
    _registrationServerUrlInput = new QLineEdit(_advancedWidget);
    _registrationServerUrlInput->setObjectName("registrationServerBaseUrl");
    _registrationServerUrlInput->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Leave empty to use the default"));
    advLayout->addWidget(registrationLabel, 1, 0);
    advLayout->addWidget(_registrationServerUrlInput, 1, 1);
    advLayout->setColumnStretch(1, 1);

    auto registrationHint = new QLabel(QCoreApplication::translate("MeetingUI",
        "Default registration port: 11022. You can set a separate service or reverse-proxy base URL."), _advancedWidget);
    MeetingUI::AppTheme::setStyleVariant(*registrationHint, "login-dialog-advlabel");
    registrationHint->setWordWrap(true);
    advLayout->addWidget(registrationHint, 2, 0, 1, 2);
    _advancedWidget->setVisible(false);
    cardLayout->addWidget(_advancedWidget);

    // 错误/成功提示 Label（自适应换行，确保不会被卡片边缘截断）
    _errorLabel = new QLabel(card);
    _errorLabel->setObjectName("loginStatus");
    MeetingUI::AppTheme::setStyleVariant(*_errorLabel, "login-dialog-errorlabel");
    _errorLabel->setAlignment(Qt::AlignCenter);
    _errorLabel->setWordWrap(true);
    _errorLabel->setMinimumHeight(32);
    _errorLabel->setVisible(false);
    cardLayout->addWidget(_errorLabel);

    // Only the current page's submit action handles Return. In a QDialog,
    // autoDefault otherwise allows auxiliary buttons (including close) to take
    // over. Use one default-button path, not additional returnPressed handlers.
    for (auto *button : findChildren<QPushButton *>()) {
        button->setAutoDefault(false);
        button->setDefault(false);
    }
    const auto updateDefaultButton = [this](int page) {
        _loginBtn->setDefault(false);
        _registerBtn->setDefault(false);
        _guestBtn->setDefault(false);
        auto *submit = page == 1 ? _registerBtn : page == 2 ? _guestBtn : _loginBtn;
        submit->setDefault(true);
        // Hidden pages must not keep the current form as tall or wide as the
        // largest page, especially after switching away from registration.
        for (int i = 0; i < _tabWidget->count(); ++i) {
            _tabWidget->widget(i)->setSizePolicy(i == page
                ? QSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred)
                : QSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored));
        }
        _tabWidget->updateGeometry();
        scheduleFormFit();
    };
    connect(_tabWidget, &QTabWidget::currentChanged, this, updateDefaultButton);
    updateDefaultButton(_tabWidget->currentIndex());
}

void LoginDialog::loadSavedData() {
    auto &session = _session;

    _accountInput->setText(session.savedAccount());
    _passwordInput->clear();
    _passwordInput->setObjectName("loginPassword");
    _accountInput->setObjectName("loginAccount");
    _serverUrlInput->setObjectName("serverBaseUrl");
    const QSignalBlocker rememberSignals(_rememberBox);
    const QSignalBlocker automaticSignals(_autoLoginBox);
    _rememberBox->setChecked(session.isRememberSession());
    _autoLoginBox->setChecked(session.isAutoLogin());
    _autoLoginBox->setEnabled(session.isRememberSession());
    _serverUrlInput->setText(session.serverBaseUrl());
    _registrationServerUrlInput->setText(session.registrationServerBaseUrl());
    _guestNicknameInput->setText(QCoreApplication::translate("MeetingUI", "Guest_%1").arg(QDateTime::currentDateTime().toString("mmss")));
    updateSavedSessionAction();
    updateEndpointOptions();
    if (session.serverBaseUrl().isEmpty()) {
        _advancedWidget->setVisible(true);
        _advancedToggleBtn->setText(QCoreApplication::translate("MeetingUI", "⚙ Server Settings ▴"));
    }
    if (!session.persistenceMessage().isEmpty()) showError(session.persistenceMessage());
}

void LoginDialog::refreshPersistenceState() {
    const QSignalBlocker rememberSignals(_rememberBox);
    const QSignalBlocker automaticSignals(_autoLoginBox);
    _rememberBox->setChecked(_session.isRememberSession());
    _autoLoginBox->setChecked(_session.isAutoLogin());
    updateSavedSessionAction();
    updateEndpointOptions();
    if (!_loading && !_session.credentialOperationPending() && !_session.persistenceMessage().isEmpty())
        showError(_session.persistenceMessage());
}

void LoginDialog::updateSavedSessionAction() {
    _resumeBtn->setEnabled(!_loading && !_session.credentialOperationPending());
    _resumeBtn->setVisible(_session.canResumeSavedSession() &&
        _accountInput->text().trimmed() == _session.savedAccount() &&
        OpenMeeting::canonicalServiceUrl(_serverUrlInput->text()) == _session.serverBaseUrl());
    scheduleFormFit();
}

void LoginDialog::updateEndpointOptions() {
    const auto policy = OpenMeeting::evaluateServiceEndpoint(_serverUrlInput->text());
    const bool persistent = OpenMeeting::credentialPersistenceSupported() && OpenMeeting::serviceAllowsCredentialPersistence(_serverUrlInput->text());
    _rememberBox->setEnabled(!_loading && persistent && !_session.credentialOperationPending());
    _autoLoginBox->setEnabled(!_loading && persistent && !_session.credentialOperationPending() && _rememberBox->isChecked());
    _rememberBox->setText(policy.isDebugHttp()
        ? QCoreApplication::translate("MeetingUI", "Remember sign-in (debug HTTP traffic is unencrypted)")
        : QCoreApplication::translate("MeetingUI", "Remember sign-in"));
    scheduleFormFit();
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
    bool isVisible = !_advancedWidget->isHidden();
    _advancedWidget->setVisible(!isVisible);
    _advancedToggleBtn->setText(!isVisible ? QCoreApplication::translate("MeetingUI", "⚙ Server Settings ▴") : QCoreApplication::translate("MeetingUI", "⚙ Server Settings ▾"));
    scheduleFormFit();
}

void LoginDialog::showEvent(QShowEvent *e) {
    QDialog::showEvent(e);
    scheduleFormFit();
}

void LoginDialog::scheduleFormFit() {
    if (!isVisible() || _formFitPending) return;
    _formFitPending = true;
    // Wait for the tab, text and visibility changes to finish their layouts.
    QTimer::singleShot(0, this, [this] {
        _formFitPending = false;
        if (isVisible()) fitFormToScreen();
    });
}

void LoginDialog::fitFormToScreen() {
#ifdef Q_OS_LINUX
    // Let the compositor own maximized/fullscreen geometry. A queued form-fit
    // from Show must not resize the surface underneath its configured state.
    if (isMaximized() || isFullScreen()) return;
#endif
    auto *scroll = findChild<QScrollArea *>(QStringLiteral("adaptiveDialogScroll"));
    if (!scroll || !scroll->widget()) return;
    auto *content = scroll->widget();
    auto *card = findChild<QWidget *>(QStringLiteral("loginCard"));
    if (!card || !content->layout() || !card->layout()) return;
    auto *targetScreen = screen();
    const auto available = targetScreen
        ? targetScreen->availableGeometry().adjusted(16, 32, -16, -32)
        : QRect(pos(), QSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX));
    const auto applyDensity = [&](bool compact) {
#ifdef Q_OS_LINUX
        content->layout()->setContentsMargins(0, 0, 0, 0);
#else
        content->layout()->setContentsMargins(compact ? 8 : 12, compact ? 8 : 12,
                                              compact ? 8 : 12, compact ? 8 : 12);
#endif
        card->layout()->setContentsMargins(compact ? 20 : 28, compact ? 12 : 20,
                                           compact ? 20 : 28, compact ? 16 : 24);
        card->layout()->setSpacing(compact ? 8 : 12);
        for (int i = 0; i < _tabWidget->count(); ++i) {
            auto *pageLayout = _tabWidget->widget(i)->layout();
            pageLayout->setContentsMargins(0, compact ? 8 : 12, 0, 0);
            pageLayout->setSpacing(compact ? (i == 1 ? 6 : 8) : (i == 1 ? 10 : i == 2 ? 14 : 12));
        }
        const auto layouts = content->findChildren<QLayout *>();
        for (auto *childLayout : layouts) childLayout->invalidate();
        // A pasted endpoint can change the HTTP notice's minimum width.
        // Refresh inner layouts before measuring their ancestors, then expose
        // the current page's new hint through the custom tab widget.
        for (auto it = layouts.crbegin(); it != layouts.crend(); ++it) (*it)->activate();
        _tabWidget->updateGeometry();
        card->layout()->invalidate();
        card->layout()->activate();
        content->layout()->invalidate();
        content->layout()->activate();
    };
    const auto measuredSize = [&](int scrollBarWidth) {
        auto *formLayout = content->layout();
        const auto minimum = formLayout->totalMinimumSize();
        const int width = qMin(available.width(), qMax(460, minimum.width()) + scrollBarWidth);
        const int formWidth = qMax(1, width - scrollBarWidth);
        // QLabel wrapping depends on the final viewport width; sizeHint alone
        // can still describe the old, wider form after adding an error message.
        const int height = formLayout->hasHeightForWidth()
            ? formLayout->totalHeightForWidth(formWidth) : formLayout->totalSizeHint().height();
        return QSize(width, qMax(minimum.height(), height));
    };
    applyDensity(false);
    auto desired = measuredSize(0);
    if (desired.height() > available.height()) {
        applyDensity(true);
        desired = measuredSize(0);
    }
    const bool contentFits = desired.height() <= available.height();
    if (desired.height() > available.height()) {
        // Truly small screens still scroll, without introducing horizontal
        // overflow just because the vertical scroll bar occupies some width.
        desired = measuredSize(scroll->verticalScrollBar()->sizeHint().width());
    }
    desired.setHeight(qMin(available.height(), qMax(600, desired.height())));
    const auto scrollPolicy = scroll->verticalScrollBarPolicy();
    if (contentFits) {
        // Qt can retain a scroll bar by measuring wrapping at the old, narrower
        // viewport. Restore the full width before applying a size that fits;
        // AsNeeded still handles genuine overflow after the layout settles.
        scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    }
    if (size() != desired) resize(desired);
    if (contentFits) {
        layout()->activate();
        content->layout()->activate();
        scroll->setVerticalScrollBarPolicy(scrollPolicy);
    }
    if (targetScreen) {
        move(qBound(available.left(), x(), available.right() - width() + 1),
             qBound(available.top(), y(), available.bottom() - height() + 1));
    }
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
    _loading = loading;
    const bool persistent = OpenMeeting::credentialPersistenceSupported() && OpenMeeting::serviceAllowsCredentialPersistence(
        _serverUrlInput->text());
    _rememberBox->setEnabled(!loading && persistent && !_session.credentialOperationPending());
    _autoLoginBox->setEnabled(!loading && persistent && !_session.credentialOperationPending() && _rememberBox->isChecked());
    _serverUrlInput->setEnabled(!loading);
    _registrationServerUrlInput->setEnabled(!loading);
    _resumeBtn->setEnabled(!loading && !_session.credentialOperationPending());
    _loginBtn->setEnabled(!loading);
    _guestBtn->setEnabled(!loading);
    _accountInput->setEnabled(!loading);
    _passwordInput->setEnabled(!loading);
    if (_registerBtn) _registerBtn->setEnabled(!loading);
    if (_regAccountInput) _regAccountInput->setEnabled(!loading);
    if (_regNicknameInput) _regNicknameInput->setEnabled(!loading);
    if (_regPasswordInput) _regPasswordInput->setEnabled(!loading);
    if (_regConfirmPwdInput) _regConfirmPwdInput->setEnabled(!loading);
    if (_toggleRegPwdBtn) _toggleRegPwdBtn->setEnabled(!loading);

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
        _endpointErrorInput = nullptr;
        _errorLabel->setVisible(false);
    } else {
        _loginBtn->setText(QCoreApplication::translate("MeetingUI", "Sign In"));
        if (_registerBtn) _registerBtn->setText(QCoreApplication::translate("MeetingUI", "Sign Up"));
    }
}

void LoginDialog::showError(const QString &msg, QLineEdit *endpointInput) {
    _endpointErrorInput = msg.isEmpty() ? nullptr : endpointInput;
    MeetingUI::AppTheme::setStyleVariant(*_errorLabel, "login-dialog-errorlabel-2");
    _errorLabel->setText(msg);
    _errorLabel->setVisible(!msg.isEmpty());
    scheduleFormFit();
}

void LoginDialog::showSuccess(const QString &msg) {
    _endpointErrorInput = nullptr;
    MeetingUI::AppTheme::setStyleVariant(*_errorLabel, "login-dialog-errorlabel-3");
    _errorLabel->setText(msg);
    _errorLabel->setVisible(!msg.isEmpty());
    scheduleFormFit();
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

    const auto validation = OpenMeeting::validateAccountCredentials(account, pwd);
    if (validation != OpenMeeting::CredentialValidationError::None) {
        showError(OpenMeeting::credentialValidationMessage(validation));
        (validation == OpenMeeting::CredentialValidationError::AccountLength
            ? _regAccountInput : _regPasswordInput)->setFocus();
        return;
    }
    if (nickname.isEmpty()) {
        showError(QCoreApplication::translate("MeetingUI", "Display name"));
        _regNicknameInput->setFocus();
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
            OpenMeeting::evaluateServiceEndpoint(serverUrl).status), _serverUrlInput);
        return;
    }
    const QString registrationUrl = _registrationServerUrlInput->text().trimmed();
    if (!session.setRegistrationServerBaseUrl(registrationUrl)) {
        showError(OpenMeeting::serviceEndpointErrorMessage(
            OpenMeeting::evaluateServiceEndpoint(registrationUrl).status), _registrationServerUrlInput);
        _registrationServerUrlInput->setFocus();
        return;
    }
    _registrationServerUrlInput->setText(session.registrationServerBaseUrl());

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
    // Changing the endpoint publishes persistence state synchronously. Keep
    // this click's choices before that projection refreshes the controls.
    const bool remember = _rememberBox && _rememberBox->isChecked();
    const bool automatic = _autoLoginBox && _autoLoginBox->isChecked();
    const QPointer<LoginDialog> self(this);

    const auto validation = OpenMeeting::validateAccountCredentials(account, password);
    if (validation != OpenMeeting::CredentialValidationError::None) {
        showError(OpenMeeting::credentialValidationMessage(validation));
        auto *input = validation == OpenMeeting::CredentialValidationError::AccountLength
            ? _accountInput : _passwordInput;
        if (input) input->setFocus();
        return;
    }

    auto &session = _session;
    const bool endpointAccepted = session.setServerBaseUrl(serverUrl);
    if (!self) return;
    if (!endpointAccepted) {
        showError(OpenMeeting::serviceEndpointErrorMessage(
            OpenMeeting::evaluateServiceEndpoint(serverUrl).status), _serverUrlInput);
        return;
    }

    setLoading(true);

    _loginInFlight = true;
    session.loginWithPassword(account, password, remember, automatic,
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
#ifdef Q_OS_LINUX
    QDialog::mousePressEvent(e);
#else
    if (e->button() == Qt::LeftButton) {
        _isDragging = true;
        _dragPosition = e->globalPos() - frameGeometry().topLeft();
        e->accept();
    }
#endif
}

void LoginDialog::mouseMoveEvent(QMouseEvent *e) {
#ifdef Q_OS_LINUX
    QDialog::mouseMoveEvent(e);
#else
    if (_isDragging && (e->buttons() & Qt::LeftButton)) {
        move(e->globalPos() - _dragPosition);
        e->accept();
    }
#endif
}

} // namespace MeetingUI
