#include <QtCore/QCoreApplication>
#include <QtCore/QUrl>
#include "src/ui/app_theme.h"
#include "src/ui/app_icons.h"
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
#include <QtGui/QIcon>
#include <QtGui/QPainterPath>

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

class LoginBrandBadge final : public QWidget {
public:
    explicit LoginBrandBadge(QWidget *parent = nullptr)
        : QWidget(parent) {
        setFixedSize(56, 56);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);

        QColor bgColor(QStringLiteral("#f1f7f6"));
        QColor borderColor(QStringLiteral("#d7e6e3"));
        switch (AppTheme::effectiveTheme()) {
        case AppTheme::Theme::Dark:
            bgColor = QColor(QStringLiteral("#2c3440"));
            borderColor = QColor(QStringLiteral("#3d4859"));
            break;
        case AppTheme::Theme::Teal:
            bgColor = QColor(QStringLiteral("#e4f4ee"));
            borderColor = QColor(QStringLiteral("#bfd9ce"));
            break;
        case AppTheme::Theme::Light:
        case AppTheme::Theme::System:
            break;
        }

        p.setPen(QPen(borderColor, 1.0));
        p.setBrush(bgColor);
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 15.0, 15.0);

        const QRect iconRect(10, 10, 36, 36);
        if (!_logoIcon.isNull()) {
            _logoIcon.paint(&p, iconRect, Qt::AlignCenter);
        } else {
            AppTheme::icon(AppTheme::Icon::Video, AppTheme::ColorRole::Accent).paint(&p, iconRect);
        }
    }

private:
    QIcon _logoIcon{QStringLiteral(":/meeting-ui/icons/cohavora.svg")};
};

class LoginTransportGlyph final : public QWidget {
public:
    explicit LoginTransportGlyph(QWidget *parent = nullptr)
        : QWidget(parent) {
        setFixedSize(14, 14);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

    void setWarning(bool warning) {
        if (_warning == warning) return;
        _warning = warning;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        if (_warning) {
            p.scale(width() / 24.0, height() / 24.0);
            p.setPen(QPen(AppTheme::color(AppTheme::ColorRole::Warning), 2.0,
                          Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
            QPainterPath triangle;
            triangle.moveTo(12.0, 4.0);
            triangle.lineTo(20.5, 19.5);
            triangle.lineTo(3.5, 19.5);
            triangle.closeSubpath();
            p.drawPath(triangle);
            p.drawLine(QPointF(12.0, 9.5), QPointF(12.0, 13.8));
            p.drawPoint(QPointF(12.0, 16.8));
            return;
        }
        AppTheme::icon(AppTheme::Icon::Shield, AppTheme::ColorRole::TextMuted).paint(&p, rect());
    }

private:
    bool _warning = false;
};

QVBoxLayout *makeFieldGroup(const QString &labelText, QWidget *field, QWidget *parent) {
    auto *group = new QVBoxLayout();
    group->setContentsMargins(0, 0, 0, 0);
    group->setSpacing(6);
    auto *label = new QLabel(labelText, parent);
    label->setObjectName(QStringLiteral("loginFieldLabel"));
    label->setWordWrap(false);
    group->addWidget(label);
    group->addWidget(field);
    return group;
}

void repolishLoginWidget(QWidget *w) {
    if (!w) return;
    w->style()->unpolish(w);
    w->style()->polish(w);
    w->update();
}

QString compactEndpointLabel(const QString &rawUrl) {
    const QString trimmed = rawUrl.trimmed();
    if (trimmed.isEmpty()) return {};
    const QUrl parsed = QUrl::fromUserInput(trimmed);
    if (parsed.isValid() && !parsed.host().isEmpty()) {
        return parsed.port() > 0
            ? QStringLiteral("%1:%2").arg(parsed.host()).arg(parsed.port())
            : parsed.host();
    }
    return trimmed;
}

} // namespace

LoginDialog::LoginDialog(QWidget *parent)
    : LoginDialog(OpenMeeting::SessionManager::instance(), parent) {
}

LoginDialog::LoginDialog(OpenMeeting::SessionManager &session, QWidget *parent)
    : QDialog(parent), _session(session) {
    setWindowTitle(QCoreApplication::translate("MeetingUI", "Cohavora Sign In"));
    resize(512, 580);
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, true);

    initUI();
    AppTheme::makeDialogAdaptive(*this, QSize(512, 580));
    for (auto *label : findChildren<QLabel *>()) {
        const auto name = label->objectName();
        if (name != QStringLiteral("loginStatus")
            && name != QStringLiteral("loginGuestNotice")
            && name != QStringLiteral("loginRegHint")) {
            label->setWordWrap(false);
        }
    }
#ifdef Q_OS_LINUX
    const QPointer<QWidget> card = findChild<QWidget *>(QStringLiteral("loginCard"));
    LinuxWindowFrameOptions frameOptions;
    frameOptions.radius = 16;
    frameOptions.captionHitTest = [this, card](QPoint point) {
        if (!card || !card->isVisibleTo(this) || !_segmentBar) return false;
        const auto local = card->mapFrom(this, point);
        if (!card->rect().contains(local) || local.y() >= _segmentBar->mapTo(card, QPoint()).y()) return false;
        // The shared application filter also sees the title labels. Limit the
        // caption to the header and leave its Close/Server buttons independently usable.
        for (auto *child = card->childAt(local); child && child != card;
                child = child->parentWidget()) {
            if (qobject_cast<QAbstractButton *>(child) || child->focusPolicy() != Qt::NoFocus) return false;
        }
        return true;
    };
    installLinuxWindowFrame(*this, std::move(frameOptions));
#endif
    AppTheme::observeChanges(*this, [this] { refreshThemeVisuals(); });
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
        syncServerSummary();
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
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(0);

    auto *body = new QWidget(card);
    body->setObjectName(QStringLiteral("loginBody"));
    _bodyLayout = new QVBoxLayout(body);
    _bodyLayout->setContentsMargins(36, 26, 36, 22);
    _bodyLayout->setSpacing(18);

    // 顶部居中品牌头（右侧保留关闭按钮）
    auto *headerLayout = new QVBoxLayout();
    headerLayout->setContentsMargins(0, 0, 0, 2);
    headerLayout->setSpacing(8);

    auto *headerTopRow = new QGridLayout();
    headerTopRow->setContentsMargins(0, 0, 0, 0);
    headerTopRow->setSpacing(0);
    headerTopRow->setColumnMinimumWidth(0, 32);
    headerTopRow->setColumnStretch(1, 1);
    headerTopRow->setColumnMinimumWidth(2, 32);
    headerTopRow->setRowMinimumHeight(0, 60);

    _brandBadge = new LoginBrandBadge(body);
    headerTopRow->addWidget(_brandBadge, 0, 1, Qt::AlignHCenter | Qt::AlignBottom);

    _closeBtn = new QPushButton(body);
    _closeBtn->setObjectName("closeBtn");
    _closeBtn->setFixedSize(32, 32);
    _closeBtn->setIconSize(QSize(16, 16));
    _closeBtn->setCursor(Qt::PointingHandCursor);
    _closeBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Close"));
    _closeBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Close"));
    connect(_closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    headerTopRow->addWidget(_closeBtn, 0, 2, Qt::AlignRight | Qt::AlignTop);
    headerLayout->addLayout(headerTopRow);

    auto *titleBox = new QVBoxLayout();
    titleBox->setContentsMargins(0, 0, 0, 0);
    titleBox->setSpacing(3);
    auto *title = new QLabel(AppBranding::name(), body);
    title->setObjectName(QStringLiteral("loginTitle"));
    title->setAlignment(Qt::AlignHCenter);
    QFont tf = title->font();
    tf.setPixelSize(21);
    tf.setBold(true);
    title->setFont(tf);

    auto *subtitle = new QLabel(AppBranding::tagline(), body);
    subtitle->setObjectName(QStringLiteral("loginSubtitle"));
    subtitle->setAlignment(Qt::AlignHCenter);
    MeetingUI::AppTheme::setStyleVariant(*subtitle, "login-dialog-subtitle");
    subtitle->setWordWrap(false);

    titleBox->addWidget(title);
    titleBox->addWidget(subtitle);
    headerLayout->addLayout(titleBox);
    _bodyLayout->addLayout(headerLayout);

    // 胶囊分段切换栏（驱动隐藏 TabBar 的 CurrentPageTabWidget）
    _segmentBar = new QFrame(body);
    _segmentBar->setObjectName(QStringLiteral("loginSegmentBar"));
    auto segLayout = new QHBoxLayout(_segmentBar);
    segLayout->setContentsMargins(3, 3, 3, 3);
    segLayout->setSpacing(3);
    const std::array<QString, 3> tabLabels = {
        QCoreApplication::translate("MeetingUI", "Account Sign-In"),
        QCoreApplication::translate("MeetingUI", "Create Account"),
        QCoreApplication::translate("MeetingUI", "Guest Access")
    };
    for (int i = 0; i < 3; ++i) {
        auto *btn = new QPushButton(tabLabels[static_cast<size_t>(i)], _segmentBar);
        btn->setObjectName(QStringLiteral("loginSegmentBtn"));
        btn->setCheckable(true);
        btn->setFixedHeight(38);
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        btn->setCursor(Qt::PointingHandCursor);
        connect(btn, &QPushButton::clicked, this, [this, i] {
            if (_tabWidget) _tabWidget->setCurrentIndex(i);
        });
        segLayout->addWidget(btn);
        _segmentButtons[static_cast<size_t>(i)] = btn;
    }
    _bodyLayout->addWidget(_segmentBar);

    // 选项卡内容区：账号登录 vs 用户注册 vs 访客体验
    _tabWidget = new CurrentPageTabWidget(body);
    _tabWidget->setDocumentMode(true);
    _tabWidget->tabBar()->hide();

    // --- Tab 1: 账号登录 ---
    auto accountTab = new QWidget(_tabWidget);
    auto accLayout = new QVBoxLayout(accountTab);
    accLayout->setContentsMargins(0, 2, 0, 0);
    accLayout->setSpacing(14);

    // 已保存会话快速继续卡片
    _savedSessionCard = new QFrame(accountTab);
    _savedSessionCard->setObjectName(QStringLiteral("loginSavedSessionCard"));
    auto savedLayout = new QHBoxLayout(_savedSessionCard);
    savedLayout->setContentsMargins(14, 10, 14, 10);
    savedLayout->setSpacing(10);

    _savedAvatarLabel = new QLabel(QStringLiteral("U"), _savedSessionCard);
    _savedAvatarLabel->setObjectName(QStringLiteral("loginSavedAvatar"));
    _savedAvatarLabel->setFixedSize(36, 36);
    _savedAvatarLabel->setAlignment(Qt::AlignCenter);
    savedLayout->addWidget(_savedAvatarLabel, 0, Qt::AlignVCenter);

    auto savedTextLayout = new QVBoxLayout();
    savedTextLayout->setContentsMargins(0, 0, 0, 0);
    savedTextLayout->setSpacing(1);
    _savedAccountLabel = new QLabel(_savedSessionCard);
    _savedAccountLabel->setObjectName(QStringLiteral("loginSavedAccount"));
    auto savedHint = new QLabel(QCoreApplication::translate("MeetingUI", "Saved session on this device"), _savedSessionCard);
    savedHint->setObjectName(QStringLiteral("loginSavedHint"));
    savedTextLayout->addWidget(_savedAccountLabel);
    savedTextLayout->addWidget(savedHint);
    savedLayout->addLayout(savedTextLayout, 1);

    _resumeBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Continue"), _savedSessionCard);
    _resumeBtn->setObjectName("resumeSavedSession");
    _resumeBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Continue with Saved Account"));
    _resumeBtn->setMinimumHeight(32);
    _resumeBtn->setCursor(Qt::PointingHandCursor);
    savedLayout->addWidget(_resumeBtn, 0, Qt::AlignVCenter);
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
    _savedSessionCard->setVisible(false);
    accLayout->addWidget(_savedSessionCard);

    _accountInput = new QLineEdit(accountTab);
    _accountInput->setObjectName("loginAccount");
    _accountInput->setMinimumHeight(44);
    _accountInput->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Phone number or account (%1-%2 characters)")
        .arg(OpenMeeting::kMinAccountLength).arg(OpenMeeting::kMaxAccountLength));
    _accountInput->setAccessibleName(_accountInput->placeholderText());
    accLayout->addLayout(makeFieldGroup(
        QCoreApplication::translate("MeetingUI", "Account / Phone"), _accountInput, accountTab));

    _pwdBox = new QFrame(accountTab);
    _pwdBox->setObjectName(QStringLiteral("loginInputBox"));
    _pwdBox->setProperty("focused", false);
    auto pwdLayout = new QHBoxLayout(_pwdBox);
    pwdLayout->setContentsMargins(0, 0, 6, 0);
    pwdLayout->setSpacing(2);
    _passwordInput = new QLineEdit(_pwdBox);
    _passwordInput->setObjectName("loginPassword");
    _passwordInput->setMinimumHeight(42);
    _passwordInput->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Password (%1-%2 characters)")
        .arg(OpenMeeting::kMinPasswordLength).arg(OpenMeeting::kMaxPasswordLength));
    _passwordInput->setAccessibleName(_passwordInput->placeholderText());
    _passwordInput->setEchoMode(QLineEdit::Password);
    _passwordInput->installEventFilter(this);
    pwdLayout->addWidget(_passwordInput, 1);

    _togglePwdBtn = new QPushButton(_pwdBox);
    _togglePwdBtn->setObjectName(QStringLiteral("loginEyeBtn"));
    _togglePwdBtn->setFixedSize(32, 32);
    _togglePwdBtn->setIconSize(QSize(16, 16));
    _togglePwdBtn->setCursor(Qt::PointingHandCursor);
    _togglePwdBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Show or hide password"));
    _togglePwdBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Show or hide password"));
    connect(_togglePwdBtn, &QPushButton::clicked, this, &LoginDialog::togglePasswordVisibility);
    pwdLayout->addWidget(_togglePwdBtn, 0, Qt::AlignVCenter);
    accLayout->addLayout(makeFieldGroup(
        QCoreApplication::translate("MeetingUI", "Sign-in Password"), _pwdBox, accountTab));

    // 记住登录状态与自动登录
    auto optLayout = new QHBoxLayout();
    optLayout->setContentsMargins(0, 2, 0, 2);
    optLayout->setSpacing(10);
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
    optLayout->addStretch();
    optLayout->addWidget(_autoLoginBox);
    accLayout->addLayout(optLayout);

    _loginBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Sign In"), accountTab);
    _loginBtn->setObjectName("primaryBtn");
    _loginBtn->setMinimumHeight(46);
    _loginBtn->setCursor(Qt::PointingHandCursor);
    connect(_loginBtn, &QPushButton::clicked, this, &LoginDialog::onLoginClicked);
    accLayout->addWidget(_loginBtn);
    accLayout->addStretch();

    _tabWidget->addTab(accountTab, tabLabels[0]);

    // --- Tab 2: 新用户注册 ---
    auto regTab = new QWidget(_tabWidget);
    auto regLayout = new QVBoxLayout(regTab);
    regLayout->setContentsMargins(0, 2, 0, 0);
    regLayout->setSpacing(12);

    auto *regTopRow = new QHBoxLayout();
    regTopRow->setContentsMargins(0, 0, 0, 0);
    regTopRow->setSpacing(12);

    _regAccountInput = new QLineEdit(regTab);
    _regAccountInput->setObjectName("registerAccount");
    _regAccountInput->setMinimumHeight(42);
    _regAccountInput->setPlaceholderText(QCoreApplication::translate("MeetingUI", "%1-%2 characters")
        .arg(OpenMeeting::kMinAccountLength).arg(OpenMeeting::kMaxAccountLength));
    _regAccountInput->setAccessibleName(QCoreApplication::translate("MeetingUI", "Account or phone number (%1-%2 characters)")
        .arg(OpenMeeting::kMinAccountLength).arg(OpenMeeting::kMaxAccountLength));
    regTopRow->addLayout(makeFieldGroup(
        QCoreApplication::translate("MeetingUI", "Account / Phone"), _regAccountInput, regTab), 1);

    _regNicknameInput = new QLineEdit(regTab);
    _regNicknameInput->setObjectName("registerNickname");
    _regNicknameInput->setMinimumHeight(42);
    _regNicknameInput->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Name shown in meetings"));
    _regNicknameInput->setAccessibleName(QCoreApplication::translate("MeetingUI", "Display Nickname"));
    regTopRow->addLayout(makeFieldGroup(
        QCoreApplication::translate("MeetingUI", "Display Nickname"), _regNicknameInput, regTab), 1);
    regLayout->addLayout(regTopRow);

    _regPwdBox = new QFrame(regTab);
    _regPwdBox->setObjectName(QStringLiteral("loginInputBox"));
    _regPwdBox->setProperty("focused", false);
    auto regPwdLayout = new QHBoxLayout(_regPwdBox);
    regPwdLayout->setContentsMargins(0, 0, 6, 0);
    regPwdLayout->setSpacing(2);
    _regPasswordInput = new QLineEdit(_regPwdBox);
    _regPasswordInput->setObjectName("registerPassword");
    _regPasswordInput->setMinimumHeight(40);
    _regPasswordInput->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Password (%1-%2 characters)")
        .arg(OpenMeeting::kMinPasswordLength).arg(OpenMeeting::kMaxPasswordLength));
    _regPasswordInput->setEchoMode(QLineEdit::Password);
    _regPasswordInput->installEventFilter(this);
    regPwdLayout->addWidget(_regPasswordInput, 1);

    _toggleRegPwdBtn = new QPushButton(_regPwdBox);
    _toggleRegPwdBtn->setObjectName(QStringLiteral("loginEyeBtn"));
    _toggleRegPwdBtn->setFixedSize(32, 32);
    _toggleRegPwdBtn->setIconSize(QSize(16, 16));
    _toggleRegPwdBtn->setCursor(Qt::PointingHandCursor);
    _toggleRegPwdBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Show or hide password"));
    _toggleRegPwdBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Show or hide password"));
    connect(_toggleRegPwdBtn, &QPushButton::clicked, this, &LoginDialog::toggleRegPasswordVisibility);
    regPwdLayout->addWidget(_toggleRegPwdBtn, 0, Qt::AlignVCenter);
    regLayout->addLayout(makeFieldGroup(
        QCoreApplication::translate("MeetingUI", "Set Password"), _regPwdBox, regTab));

    _regConfirmPwdInput = new QLineEdit(regTab);
    _regConfirmPwdInput->setObjectName("registerConfirmPassword");
    _regConfirmPwdInput->setMinimumHeight(42);
    _regConfirmPwdInput->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Confirm password"));
    _regConfirmPwdInput->setEchoMode(QLineEdit::Password);
    regLayout->addLayout(makeFieldGroup(
        QCoreApplication::translate("MeetingUI", "Confirm Password"), _regConfirmPwdInput, regTab));

    _registerBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Sign Up"), regTab);
    _registerBtn->setObjectName("primaryBtn");
    _registerBtn->setMinimumHeight(44);
    _registerBtn->setCursor(Qt::PointingHandCursor);
    connect(_registerBtn, &QPushButton::clicked, this, &LoginDialog::onRegisterClicked);
    regLayout->addWidget(_registerBtn);
    regLayout->addStretch();

    _tabWidget->addTab(regTab, tabLabels[1]);

    // --- Tab 3: 访客/调试体验 ---
    auto guestTab = new QWidget(_tabWidget);
    auto guestLayout = new QVBoxLayout(guestTab);
    guestLayout->setContentsMargins(0, 2, 0, 0);
    guestLayout->setSpacing(16);

    auto guestDesc = new QLabel(QCoreApplication::translate("MeetingUI", "Enter a display name to join a meeting or test local RTC features without creating an account."), guestTab);
    guestDesc->setObjectName(QStringLiteral("loginGuestNotice"));
    guestDesc->setWordWrap(true);
    MeetingUI::AppTheme::setStyleVariant(*guestDesc, "login-dialog-guestdesc");
    guestLayout->addWidget(guestDesc);

    _guestNicknameInput = new QLineEdit(guestTab);
    _guestNicknameInput->setMinimumHeight(44);
    _guestNicknameInput->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Display name (e.g. Alice)"));
    _guestNicknameInput->setAccessibleName(_guestNicknameInput->placeholderText());
    guestLayout->addLayout(makeFieldGroup(
        QCoreApplication::translate("MeetingUI", "Guest Display Name"), _guestNicknameInput, guestTab));

    _guestBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Continue as Guest"), guestTab);
    _guestBtn->setObjectName("guestBtn");
    _guestBtn->setMinimumHeight(46);
    _guestBtn->setCursor(Qt::PointingHandCursor);
    connect(_guestBtn, &QPushButton::clicked, this, &LoginDialog::onGuestLoginClicked);
    guestLayout->addWidget(_guestBtn);
    guestLayout->addStretch();

    _tabWidget->addTab(guestTab, tabLabels[2]);

    _bodyLayout->addWidget(_tabWidget);

    // 错误/成功提示 Label（自适应换行，确保不会被卡片边缘截断）
    _errorLabel = new QLabel(body);
    _errorLabel->setObjectName("loginStatus");
    MeetingUI::AppTheme::setStyleVariant(*_errorLabel, "login-dialog-errorlabel");
    _errorLabel->setAlignment(Qt::AlignCenter);
    _errorLabel->setWordWrap(true);
    _errorLabel->setMinimumHeight(34);
    _errorLabel->setVisible(false);
    _bodyLayout->addWidget(_errorLabel);

    cardLayout->addWidget(body);

    // 独立页脚带（服务节点配置 + 传输安全状态胶囊）
    _footerBar = new QFrame(card);
    _footerBar->setObjectName(QStringLiteral("loginFooterBar"));
    _footerLayout = new QVBoxLayout(_footerBar);
    _footerLayout->setContentsMargins(28, 12, 28, 12);
    _footerLayout->setSpacing(10);

    auto *footerTopRow = new QHBoxLayout();
    footerTopRow->setContentsMargins(0, 0, 0, 0);
    footerTopRow->setSpacing(8);
    _advancedToggleBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Server Settings"), _footerBar);
    _advancedToggleBtn->setObjectName("serverSettingsToggle");
    _advancedToggleBtn->setIconSize(QSize(14, 14));
    _advancedToggleBtn->setCursor(Qt::PointingHandCursor);
    MeetingUI::AppTheme::setStyleVariant(*_advancedToggleBtn, "login-dialog-advancedtogglebtn");
    connect(_advancedToggleBtn, &QPushButton::clicked, this, &LoginDialog::toggleAdvancedSettings);
    footerTopRow->addWidget(_advancedToggleBtn);
    footerTopRow->addStretch();

    _transportBadge = new QFrame(_footerBar);
    _transportBadge->setObjectName(QStringLiteral("loginTransportBadge"));
    auto *badgeLayout = new QHBoxLayout(_transportBadge);
    badgeLayout->setContentsMargins(8, 3, 8, 3);
    badgeLayout->setSpacing(4);
    auto *glyph = new LoginTransportGlyph(_transportBadge);
    _transportGlyph = glyph;
    badgeLayout->addWidget(_transportGlyph, 0, Qt::AlignVCenter);
    _transportBadgeLabel = new QLabel(_transportBadge);
    _transportBadgeLabel->setObjectName(QStringLiteral("loginTransportBadgeLabel"));
    badgeLayout->addWidget(_transportBadgeLabel, 0, Qt::AlignVCenter);
    footerTopRow->addWidget(_transportBadge, 0, Qt::AlignVCenter);
    _footerLayout->addLayout(footerTopRow);

    auto *advCard = new QFrame(_footerBar);
    advCard->setObjectName(QStringLiteral("loginAdvancedCard"));
    _advancedWidget = advCard;
    auto advLayout = new QGridLayout(_advancedWidget);
    advLayout->setContentsMargins(14, 12, 14, 12);
    advLayout->setHorizontalSpacing(10);
    advLayout->setVerticalSpacing(8);
    auto advLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Meeting service:"), _advancedWidget);
    MeetingUI::AppTheme::setStyleVariant(*advLabel, "login-dialog-advlabel");
    _serverUrlInput = new QLineEdit(_advancedWidget);
    _serverUrlInput->setObjectName("serverBaseUrl");
    _serverUrlInput->setMinimumHeight(34);
    _serverUrlInput->setPlaceholderText(QString::fromUtf8("https://api.example.com"));
    _serverUrlInput->setAccessibleName(QCoreApplication::translate("MeetingUI", "Meeting service"));
    advLayout->addWidget(advLabel, 0, 0);
    advLayout->addWidget(_serverUrlInput, 0, 1);

    auto registrationLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Registration service:"), _advancedWidget);
    MeetingUI::AppTheme::setStyleVariant(*registrationLabel, "login-dialog-advlabel");
    _registrationServerUrlInput = new QLineEdit(_advancedWidget);
    _registrationServerUrlInput->setObjectName("registrationServerBaseUrl");
    _registrationServerUrlInput->setMinimumHeight(34);
    _registrationServerUrlInput->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Leave empty to use the default"));
    advLayout->addWidget(registrationLabel, 1, 0);
    advLayout->addWidget(_registrationServerUrlInput, 1, 1);
    advLayout->setColumnStretch(1, 1);

    auto registrationHint = new QLabel(QCoreApplication::translate("MeetingUI",
        "Default registration port: 11022. You can set a separate service or reverse-proxy base URL."), _advancedWidget);
    registrationHint->setObjectName(QStringLiteral("loginRegHint"));
    MeetingUI::AppTheme::setStyleVariant(*registrationHint, "login-dialog-advlabel");
    registrationHint->setWordWrap(true);
    advLayout->addWidget(registrationHint, 2, 0, 1, 2);
    _advancedWidget->setVisible(false);
    _footerLayout->addWidget(_advancedWidget);

    cardLayout->addWidget(_footerBar);

    // Only the current page's submit action handles Return. In a QDialog,
    // autoDefault otherwise allows auxiliary buttons (including close) to take
    // over. Use one default-button path, not additional returnPressed handlers.
    for (auto *button : findChildren<QPushButton *>()) {
        button->setAutoDefault(false);
        button->setDefault(false);
    }
    const auto updateDefaultButton = [this](int page) {
        syncSegmentButtons(page);
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
    syncServerSummary();
    refreshThemeVisuals();
}

bool LoginDialog::eventFilter(QObject *watched, QEvent *event) {
    if (event && (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)) {
        const bool focused = event->type() == QEvent::FocusIn;
        if (watched == _passwordInput && _pwdBox) {
            _pwdBox->setProperty("focused", focused);
            repolishLoginWidget(_pwdBox);
        } else if (watched == _regPasswordInput && _regPwdBox) {
            _regPwdBox->setProperty("focused", focused);
            repolishLoginWidget(_regPwdBox);
        }
    }
    return QDialog::eventFilter(watched, event);
}

void LoginDialog::syncSegmentButtons(int index) {
    for (int i = 0; i < static_cast<int>(_segmentButtons.size()); ++i) {
        if (auto *btn = _segmentButtons[static_cast<size_t>(i)]) {
            const QSignalBlocker blocker(btn);
            btn->setChecked(i == index);
        }
    }
}

void LoginDialog::syncServerSummary() {
    const bool expanded = _advancedWidget && !_advancedWidget->isHidden();
    if (_advancedToggleBtn) {
        const QString host = compactEndpointLabel(_serverUrlInput ? _serverUrlInput->text() : QString());
        const QString baseText = host.isEmpty()
            ? QCoreApplication::translate("MeetingUI", "Server Settings")
            : QCoreApplication::translate("MeetingUI", "Server: %1").arg(host);
        _advancedToggleBtn->setText(baseText + (expanded ? QStringLiteral(" ▴") : QStringLiteral(" ▾")));
        _advancedToggleBtn->setProperty("expanded", expanded);
        repolishLoginWidget(_advancedToggleBtn);
    }
    refreshThemeVisuals();
}

void LoginDialog::refreshThemeVisuals() {
    if (_brandBadge) _brandBadge->update();
    if (_transportGlyph) _transportGlyph->update();
    const bool serverExpanded = _advancedWidget && !_advancedWidget->isHidden();
    if (_closeBtn) {
        _closeBtn->setIcon(AppTheme::icon(AppTheme::Icon::Close, AppTheme::ColorRole::TextMuted));
    }
    if (_advancedToggleBtn) {
        const auto toggleRole = serverExpanded ? AppTheme::ColorRole::Accent : AppTheme::ColorRole::TextSecondary;
        _advancedToggleBtn->setIcon(AppTheme::icon(AppTheme::Icon::Server, toggleRole));
    }
    if (_togglePwdBtn && _passwordInput) {
        const bool visible = _passwordInput->echoMode() != QLineEdit::Password;
        _togglePwdBtn->setIcon(AppTheme::icon(
            visible ? AppTheme::Icon::EyeOff : AppTheme::Icon::Eye,
            visible ? AppTheme::ColorRole::Accent : AppTheme::ColorRole::TextMuted));
    }
    if (_toggleRegPwdBtn && _regPasswordInput) {
        const bool visible = _regPasswordInput->echoMode() != QLineEdit::Password;
        _toggleRegPwdBtn->setIcon(AppTheme::icon(
            visible ? AppTheme::Icon::EyeOff : AppTheme::Icon::Eye,
            visible ? AppTheme::ColorRole::Accent : AppTheme::ColorRole::TextMuted));
    }
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
    }
    syncServerSummary();
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
    const bool canResume = _session.canResumeSavedSession() &&
        _accountInput->text().trimmed() == _session.savedAccount() &&
        OpenMeeting::canonicalServiceUrl(_serverUrlInput->text()) == _session.serverBaseUrl();
    _resumeBtn->setEnabled(!_loading && !_session.credentialOperationPending());
    if (_savedSessionCard) {
        if (canResume) {
            const QString account = _session.savedAccount().trimmed();
            if (_savedAccountLabel) _savedAccountLabel->setText(account);
            if (_savedAvatarLabel) {
                const QString initial = account.isEmpty() ? QStringLiteral("U") : account.left(1).toUpper();
                _savedAvatarLabel->setText(initial);
            }
        }
        _savedSessionCard->setVisible(canResume);
    } else {
        _resumeBtn->setVisible(canResume);
    }
    scheduleFormFit();
}

void LoginDialog::updateEndpointOptions() {
    const auto policy = OpenMeeting::evaluateServiceEndpoint(_serverUrlInput->text());
    const bool persistent = OpenMeeting::credentialPersistenceSupported() && OpenMeeting::serviceAllowsCredentialPersistence(_serverUrlInput->text());
    _rememberBox->setEnabled(!_loading && persistent && !_session.credentialOperationPending());
    _autoLoginBox->setEnabled(!_loading && persistent && !_session.credentialOperationPending() && _rememberBox->isChecked());
    _rememberBox->setText(QCoreApplication::translate("MeetingUI", "Remember sign-in"));
    if (_transportBadge && _transportBadgeLabel && _transportGlyph) {
        auto *glyph = static_cast<LoginTransportGlyph *>(_transportGlyph);
        if (policy.status == OpenMeeting::ServiceEndpointStatus::DebugHttpAllowed) {
            _transportBadge->setProperty("mode", QStringLiteral("warning"));
            glyph->setWarning(true);
            _transportBadgeLabel->setText(QCoreApplication::translate("MeetingUI", "Debug HTTP (Unencrypted)"));
            _transportBadge->setToolTip(QCoreApplication::translate("MeetingUI", "Debug HTTP traffic is unencrypted."));
            _transportBadge->setVisible(true);
        } else if (policy.status == OpenMeeting::ServiceEndpointStatus::HttpsAllowed) {
            _transportBadge->setProperty("mode", QStringLiteral("secure"));
            glyph->setWarning(false);
            _transportBadgeLabel->setText(QCoreApplication::translate("MeetingUI", "HTTPS Encrypted"));
            _transportBadge->setToolTip(QCoreApplication::translate("MeetingUI", "Sign-in traffic uses HTTPS encryption."));
            _transportBadge->setVisible(true);
        } else {
            _transportBadge->setVisible(false);
        }
        repolishLoginWidget(_transportBadge);
        repolishLoginWidget(_transportBadgeLabel);
    }
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
    syncServerSummary();
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
        card->layout()->setContentsMargins(0, 0, 0, 0);
        card->layout()->setSpacing(0);
        if (_bodyLayout) {
            _bodyLayout->setContentsMargins(compact ? 24 : 36, compact ? 16 : 26,
                                            compact ? 24 : 36, compact ? 16 : 22);
            _bodyLayout->setSpacing(compact ? 12 : 18);
        }
        if (_footerLayout) {
            _footerLayout->setContentsMargins(compact ? 24 : 28, compact ? 10 : 12,
                                              compact ? 24 : 28, compact ? 10 : 12);
            _footerLayout->setSpacing(compact ? 8 : 10);
        }
        for (int i = 0; i < _tabWidget->count(); ++i) {
            auto *pageLayout = _tabWidget->widget(i)->layout();
            pageLayout->setContentsMargins(0, 2, 0, 0);
            pageLayout->setSpacing(compact ? (i == 1 ? 8 : 10) : (i == 1 ? 12 : i == 2 ? 16 : 14));
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
        const int width = qMin(available.width(), qMax(512, minimum.width()) + scrollBarWidth);
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
    desired.setHeight(qMin(available.height(), qMax(440, desired.height())));
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
    } else {
        _passwordInput->setEchoMode(QLineEdit::Password);
    }
    refreshThemeVisuals();
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
    if (_togglePwdBtn) _togglePwdBtn->setEnabled(!loading);
    if (_registerBtn) _registerBtn->setEnabled(!loading);
    if (_regAccountInput) _regAccountInput->setEnabled(!loading);
    if (_regNicknameInput) _regNicknameInput->setEnabled(!loading);
    if (_regPasswordInput) _regPasswordInput->setEnabled(!loading);
    if (_regConfirmPwdInput) _regConfirmPwdInput->setEnabled(!loading);
    if (_toggleRegPwdBtn) _toggleRegPwdBtn->setEnabled(!loading);
    for (auto *btn : _segmentButtons) {
        if (btn) btn->setEnabled(!loading);
    }

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
    } else {
        _regPasswordInput->setEchoMode(QLineEdit::Password);
        if (_regConfirmPwdInput) _regConfirmPwdInput->setEchoMode(QLineEdit::Password);
    }
    refreshThemeVisuals();
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
