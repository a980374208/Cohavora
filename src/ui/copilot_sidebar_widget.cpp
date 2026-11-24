#include "src/ui/copilot_sidebar_widget.h"

#include "src/core/session_shutdown_service.h"
#include "src/ui/app_icons.h"
#include "src/ui/app_theme.h"
#include "src/ui/copilot_chat_canvas_widget.h"
#include "src/ui/whiteboard/accessible_combo_box.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMimeData>
#include <QtCore/QSet>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QClipboard>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QDragMoveEvent>
#include <QtGui/QDropEvent>
#include <QtGui/QFontMetrics>
#include <QtGui/QGuiApplication>
#include <QtGui/QIcon>
#include <QtGui/QKeyEvent>
#include <QtGui/QKeySequence>
#include <QtGui/QLinearGradient>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QShowEvent>
#include <QtGui/QTextCursor>
#include <QtWidgets/QActionGroup>
#include <QtWidgets/QApplication>
#include <QtWidgets/QDialog>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QStyle>

#include <algorithm>
#include <cmath>

namespace OpenMeeting {
namespace Theme = MeetingUI::AppTheme;
namespace {

constexpr int kMinInputHeight = 38;
constexpr int kMaxInputHeight = 140;
constexpr int kLeftEdgeShadowWidth = 14;
constexpr int kCompletionKindRole = Qt::UserRole + 1;
constexpr int kCompletionValueRole = Qt::UserRole + 2;
constexpr int kCompletionTokenLenRole = Qt::UserRole + 3;
constexpr int kHistorySessionIdRole = Qt::UserRole + 1;

Theme::Tone resolvedCurrentTone(const QWidget *widget) {
	if (Theme::effectiveTheme() == Theme::Theme::Dark) {
		return Theme::Tone::Dark;
	}
	if (widget && widget->window() &&
		widget->window()->palette().color(QPalette::Window).lightness() < 128 &&
		Theme::effectiveTheme() != Theme::Theme::Light &&
		Theme::effectiveTheme() != Theme::Theme::Teal) {
		return Theme::Tone::Dark;
	}
	return Theme::Tone::Light;
}

class LeftEdgeShadowOverlay final : public QWidget {
public:
	explicit LeftEdgeShadowOverlay(QWidget *parent)
		: QWidget(parent) {
		setObjectName(QStringLiteral("CopilotLeftEdgeShadow"));
		setAttribute(Qt::WA_TransparentForMouseEvents, true);
		setAttribute(Qt::WA_NoSystemBackground, true);
		setAttribute(Qt::WA_TranslucentBackground, true);
		setFocusPolicy(Qt::NoFocus);
		Theme::observeChanges(*this, [this] { update(); });
	}

protected:
	void paintEvent(QPaintEvent *) override {
		if (width() <= 0 || height() <= 0) {
			return;
		}
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing, false);

		const bool isDarkSurface = Theme::color(Theme::ColorRole::Surface).lightness() < 128;
		const QColor baseColor = isDarkSurface ? QColor(0, 0, 0) : QColor(15, 23, 42);
		const int maxAlpha = isDarkSurface ? 96 : 42;

		const auto stopColor = [&baseColor, maxAlpha](double weight) {
			QColor c = baseColor;
			c.setAlpha(qBound(0, static_cast<int>(std::lround(maxAlpha * weight)), 255));
			return c;
		};

		QLinearGradient grad(0.0, 0.0, static_cast<qreal>(width()), 0.0);
		grad.setColorAt(0.00, stopColor(0.00));
		grad.setColorAt(0.35, stopColor(0.07));
		grad.setColorAt(0.62, stopColor(0.22));
		grad.setColorAt(0.82, stopColor(0.45));
		grad.setColorAt(0.94, stopColor(0.74));
		grad.setColorAt(1.00, stopColor(1.00));
		p.fillRect(rect(), grad);
	}
};

// High-density symmetric vertical bar waveform UI placeholder for Dictation.
class DictationWaveformWidget final : public QWidget {
public:
	explicit DictationWaveformWidget(QWidget *parent = nullptr)
		: QWidget(parent) {
		setObjectName(QStringLiteral("CopilotDictationWaveform"));
		setAttribute(Qt::WA_TransparentForMouseEvents, true);
		setFixedHeight(30);
		_timer.setInterval(33);
		connect(&_timer, &QTimer::timeout, this, [this] {
			_phase += _listening ? 0.18 : 0.08;
			if (_phase > 628.318) {
				_phase -= 628.318;
			}
			const float floorLevel = _listening ? 0.04f : 0.0f;
			const float target = std::max(floorLevel, _targetLevel);
			const float alpha = (target > _smoothedLevel) ? 0.45f : 0.16f;
			_smoothedLevel = _smoothedLevel + alpha * (target - _smoothedLevel);
			_targetLevel *= 0.85f;
			for (int k = kRadialBins - 1; k > 0; --k) {
				_radialEnergy[k] = _radialEnergy[k] * 0.52f + _radialEnergy[k - 1] * 0.48f;
			}
			_radialEnergy[0] = _smoothedLevel;
			update();
		});
		Theme::observeChanges(*this, [this] { update(); });
	}

	void setListening(bool active, bool transcribing) {
		_listening = active;
		_transcribing = transcribing;
		if (!active) {
			_targetLevel = 0.0f;
		}
		update();
	}

	void setAudioLevel(float level) {
		_targetLevel = std::clamp(level, 0.0f, 1.0f);
		if (isVisible()) {
			update();
		}
	}

protected:
	void showEvent(QShowEvent *event) override {
		QWidget::showEvent(event);
		if (!_timer.isActive()) {
			_timer.start();
		}
	}

	void hideEvent(QHideEvent *event) override {
		_timer.stop();
		_targetLevel = 0.0f;
		_smoothedLevel = 0.0f;
		std::fill(std::begin(_radialEnergy), std::end(_radialEnergy), 0.0f);
		QWidget::hideEvent(event);
	}

	void paintEvent(QPaintEvent *) override {
		const int w = width();
		const int h = height();
		if (w <= 8 || h <= 6) {
			return;
		}
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing, true);
		p.setPen(Qt::NoPen);

		constexpr int kBarWidth = 2;
		constexpr int kMinGap = 3;
		const int barCount = std::clamp((w - 8) / (kBarWidth + kMinGap), 25, 51) | 1;
		const double step = static_cast<double>(w - kBarWidth) / std::max(1, barCount - 1);
		const double mid = (barCount - 1) / 2.0;
		const double maxBarH = std::max(4.0, h - 4.0);

		const QColor idlePeakColor = Theme::color(Theme::ColorRole::Text);
		const QColor activePeakColor = _transcribing
			? Theme::color(Theme::ColorRole::Accent)
			: Theme::color(Theme::ColorRole::Text);
		const QColor edgeColor = Theme::color(Theme::ColorRole::TextMuted);
		const double breathe = _listening
			? (0.06 + 0.03 * std::sin(_phase * 0.7))
			: (0.04 + 0.02 * std::sin(_phase * 0.5));

		for (int i = 0; i < barCount; ++i) {
			const double dist = (mid > 0.0) ? (std::abs(i - mid) / mid) : 0.0;
			const int binIdx = std::clamp(
				static_cast<int>(std::round(dist * (kRadialBins - 1))),
				0,
				kRadialBins - 1);
			const double radial = static_cast<double>(_radialEnergy[binIdx]);
			const double envelope = std::exp(-std::pow(dist * 1.9, 2.0));
			const double ripple = 0.78 + 0.22 * std::sin(_phase * 1.6 - dist * 5.2);
			const double amp = breathe + envelope * std::clamp(radial * 1.35 * ripple, 0.0, 0.92);
			const double barH = std::clamp(amp * maxBarH, 2.5, maxBarH);
			const double x = i * step;
			const double y = (h - barH) * 0.5;

			QColor col = (envelope > 0.25 && radial > 0.07) ? activePeakColor : ((envelope > 0.25) ? idlePeakColor : edgeColor);
			col.setAlphaF(std::clamp(0.28 + 0.68 * envelope, 0.25, 0.96));
			p.setBrush(col);
			p.drawRoundedRect(QRectF(x, y, kBarWidth, barH), 1.0, 1.0);
		}
	}

private:
	static constexpr int kRadialBins = 26;
	QTimer _timer;
	double _phase = 0.0;
	bool _listening = false;
	bool _transcribing = false;
	float _targetLevel = 0.0f;
	float _smoothedLevel = 0.0f;
	float _radialEnergy[kRadialBins] = {};
};

struct ContextChipDescriptor {
	AgentContextChipKind kind;
	const char *hashTag;
	const char *displayLabel;
	const char *description;
};

const ContextChipDescriptor kContextChipDescriptors[] = {
	{ AgentContextChipKind::MeetingChat, "#meeting-chat",
	  QT_TRANSLATE_NOOP("MeetingUI", "#meeting-chat"),
	  QT_TRANSLATE_NOOP("MeetingUI", "Attach recent meeting chat messages") },
	{ AgentContextChipKind::Transcript, "#transcript",
	  QT_TRANSLATE_NOOP("MeetingUI", "#transcript"),
	  QT_TRANSLATE_NOOP("MeetingUI", "Attach recent meeting AI voice transcript") },
	{ AgentContextChipKind::Telemetry, "#telemetry",
	  QT_TRANSLATE_NOOP("MeetingUI", "#telemetry"),
	  QT_TRANSLATE_NOOP("MeetingUI", "Attach A/V quality and network telemetry") },
	{ AgentContextChipKind::Participants, "#participants",
	  QT_TRANSLATE_NOOP("MeetingUI", "#participants"),
	  QT_TRANSLATE_NOOP("MeetingUI", "Attach current participant list and media state") },
	{ AgentContextChipKind::Schedule, "#schedule",
	  QT_TRANSLATE_NOOP("MeetingUI", "#schedule"),
	  QT_TRANSLATE_NOOP("MeetingUI", "Attach upcoming scheduled meetings") },
};

struct SlashCommandDescriptor {
	const char *command;
	const char *description;
	const char *promptTemplate;
	bool attachMeetingChat = false;
	bool attachTranscript = false;
	bool attachTelemetry = false;
	bool attachSchedule = false;
	bool immediateAction = false;
};

const SlashCommandDescriptor kSlashCommandDescriptors[] = {
	{ "/summary",
	  QT_TRANSLATE_NOOP("MeetingUI", "Summarize meeting chat, voice transcript, and action items"),
	  QT_TRANSLATE_NOOP("MeetingUI", "Summarize key points, decisions, and action items from the recent meeting."),
	  true, true, false, false, false },
	{ "/transcript",
	  QT_TRANSLATE_NOOP("MeetingUI", "Summarize live AI voice transcript"),
	  QT_TRANSLATE_NOOP("MeetingUI", "Summarize the recent AI voice transcript and highlight speakers and decisions."),
	  false, true, false, false, false },
	{ "/diagnose",
	  QT_TRANSLATE_NOOP("MeetingUI", "Diagnose A/V quality and network issues"),
	  QT_TRANSLATE_NOOP("MeetingUI", "Analyze current audio/video telemetry and network quality and suggest fixes."),
	  false, false, true, false, false },
	{ "/schedule",
	  QT_TRANSLATE_NOOP("MeetingUI", "Check upcoming scheduled meetings"),
	  QT_TRANSLATE_NOOP("MeetingUI", "List my upcoming scheduled meetings and highlight the next one."),
	  false, false, false, true, false },
	{ "/prefs",
	  QT_TRANSLATE_NOOP("MeetingUI", "Inspect client & join media preferences"),
	  QT_TRANSLATE_NOOP("MeetingUI", "Show my client preferences and join media defaults."),
	  false, false, false, false, false },
	{ "/ask",
	  QT_TRANSLATE_NOOP("MeetingUI", "Switch to Ask (read-only Q&A) mode"),
	  "", false, false, false, false, true },
	{ "/agent",
	  QT_TRANSLATE_NOOP("MeetingUI", "Switch to Agent (tool actions) mode"),
	  "", false, false, false, false, true },
	{ "/clear",
	  QT_TRANSLATE_NOOP("MeetingUI", "Clear current Copilot conversation"),
	  "", false, false, false, false, true },
};

bool hasMeetingScopedChip(const QVector<AgentContextChip> &chips) {
	return std::any_of(chips.cbegin(), chips.cend(), [](const AgentContextChip &chip) {
		return chip.kind == AgentContextChipKind::MeetingChat ||
			chip.kind == AgentContextChipKind::Transcript ||
			chip.kind == AgentContextChipKind::Telemetry ||
			chip.kind == AgentContextChipKind::Participants;
	});
}

QString localizedChipLabel(AgentContextChipKind kind) {
	for (const auto &desc : kContextChipDescriptors) {
		if (desc.kind == kind) {
			return QCoreApplication::translate("MeetingUI", desc.displayLabel);
		}
	}
	return QStringLiteral("#context");
}

bool isSupportedImageFilePath(const QString &filePath) {
	const QString ext = QFileInfo(filePath).suffix().trimmed().toLower();
	return ext == QStringLiteral("png") ||
		ext == QStringLiteral("jpg") ||
		ext == QStringLiteral("jpeg") ||
		ext == QStringLiteral("webp") ||
		ext == QStringLiteral("gif");
}

bool hasSupportedImageUrls(const QList<QUrl> &urls) {
	for (const QUrl &url : urls) {
		if (url.isLocalFile() && isSupportedImageFilePath(url.toLocalFile())) {
			return true;
		}
	}
	return false;
}

QString shortModelDisplayName(const QString &modelId) {
	const QString clean = modelId.trimmed();
	const int slash = clean.lastIndexOf(QLatin1Char('/'));
	return (slash >= 0 && slash + 1 < clean.size()) ? clean.mid(slash + 1) : clean;
}

} // namespace

CopilotSidebarWidget::CopilotSidebarWidget(AgentCoordinator *coordinator, QWidget *parent)
	: QWidget(parent)
	, _coordinator(coordinator)
	, _callbackGate(QtCallbackGate<CopilotSidebarWidget>::Create(this)) {
	setObjectName(QStringLiteral("CopilotSidebar"));
	setAttribute(Qt::WA_StyledBackground, true);
	setAcceptDrops(true);
	installEventFilter(this);
	Theme::setStyleVariant(*this, "copilot-sidebar-widget");
	_speechService = new AgentSpeechService(
		_coordinator,
		_coordinator ? _coordinator->attachedMeeting() : nullptr,
		nullptr,
		{},
		this);
	_dictationAutoSendTimer = new QTimer(this);
	_dictationAutoSendTimer->setSingleShot(true);
	connect(_dictationAutoSendTimer, &QTimer::timeout, this, [this] {
		if (!_speechService || !_speechService->isDictationActive()) {
			return;
		}
		if (!_coordinator || !_coordinator->isConfigured() || _coordinator->isStreaming()) {
			return;
		}
		if (_inputEdit && !_inputEdit->toPlainText().trimmed().isEmpty()) {
			onSendOrStopClicked();
		}
	});
	buildUi();
	bindSpeechService();
	Theme::observeChanges(*this, [this] {
		syncThemeVisuals();
	});
	bindCoordinator();
	syncHeaderAndState();
	rebuildMessages();
	syncActionButton();
}

CopilotSidebarWidget::~CopilotSidebarWidget() {
	if (_dictationAutoSendTimer) {
		_dictationAutoSendTimer->stop();
	}
	if (_speechService) {
		_speechService->stopDictation();
	}
	if (_callbackGate) {
		_callbackGate->Revoke();
	}
	_micDiscoveryRequest.reset();
	if (_leftEdgeShadow) {
		_leftEdgeShadow->deleteLater();
		_leftEdgeShadow.clear();
	}
}

void CopilotSidebarWidget::setLeftEdgeShadowEnabled(bool enabled) {
	if (_leftEdgeShadowEnabled == enabled) {
		return;
	}
	_leftEdgeShadowEnabled = enabled;
	syncLeftEdgeShadow();
}

void CopilotSidebarWidget::setExpanded(bool expanded) {
	if (_expanded == expanded) {
		return;
	}
	_expanded = expanded;
	if (_expandBtn) {
		_expandBtn->setIcon(Theme::icon(_expanded ? Theme::Icon::Collapse : Theme::Icon::Expand));
		_expandBtn->setToolTip(_expanded
			? QCoreApplication::translate("MeetingUI", "Restore default panel width")
			: QCoreApplication::translate("MeetingUI", "Expand panel width"));
	}
}

void CopilotSidebarWidget::setCoordinator(AgentCoordinator *coordinator) {
	if (_coordinator == coordinator) {
		return;
	}
	_coordinator = coordinator;
	bindCoordinator();
	syncHeaderAndState();
	rebuildMessages();
	syncActionButton();
}

void CopilotSidebarWidget::focusInput() {
	if (_historyVisible) {
		setHistoryViewVisible(false);
	}
	if (_inputEdit && _inputEdit->isEnabled()) {
		_inputEdit->setFocus();
	}
}

void CopilotSidebarWidget::buildUi() {
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(0, 0, 0, 0);
	root->setSpacing(0);

	// 1. Layer 1: Primary Tab & Action Bar
	auto *primaryHeader = new QWidget(this);
	primaryHeader->setObjectName(QStringLiteral("CopilotHeader"));
	auto *primaryLayout = new QHBoxLayout(primaryHeader);
	primaryLayout->setContentsMargins(12, 4, 8, 4);
	primaryLayout->setSpacing(4);

	_chatTabLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Chat"), primaryHeader);
	_chatTabLabel->setObjectName(QStringLiteral("CopilotChatTabLabel"));
	primaryLayout->addWidget(_chatTabLabel);

	_moreActionsBtn = new QPushButton(primaryHeader);
	_moreActionsBtn->setObjectName(QStringLiteral("CopilotMoreBtn"));
	_moreActionsBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "More Copilot actions"));
	_moreActionsBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Clear, export, or open settings"));
	_moreActionsBtn->setIcon(Theme::icon(Theme::Icon::MoreHorizontal, Theme::ColorRole::TextSecondary));
	_moreActionsBtn->setIconSize(QSize(15, 15));
	_moreActionsBtn->setFixedSize(26, 26);
	_moreActionsBtn->setCursor(Qt::PointingHandCursor);
	_moreActionsBtn->setAutoDefault(false);
	connect(_moreActionsBtn, &QPushButton::clicked, this, &CopilotSidebarWidget::onMoreMenuClicked);
	primaryLayout->addWidget(_moreActionsBtn);

	primaryLayout->addStretch(1);

	_newChatBtn = new QPushButton(primaryHeader);
	_newChatBtn->setObjectName(QStringLiteral("CopilotNewChatBtn"));
	_newChatBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "New chat"));
	_newChatBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Start a new Copilot conversation"));
	_newChatBtn->setIcon(Theme::icon(Theme::Icon::Plus, Theme::ColorRole::TextSecondary));
	_newChatBtn->setIconSize(QSize(15, 15));
	_newChatBtn->setFixedSize(26, 26);
	_newChatBtn->setCursor(Qt::PointingHandCursor);
	_newChatBtn->setAutoDefault(false);
	connect(_newChatBtn, &QPushButton::clicked, this, &CopilotSidebarWidget::onNewChatClicked);
	primaryLayout->addWidget(_newChatBtn);

	_settingsBtn = new QPushButton(primaryHeader);
	_settingsBtn->setObjectName(QStringLiteral("CopilotSettingsBtn"));
	_settingsBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Open Copilot settings"));
	_settingsBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Open Copilot settings"));
	_settingsBtn->setIcon(Theme::icon(Theme::Icon::Settings, Theme::ColorRole::TextSecondary));
	_settingsBtn->setIconSize(QSize(15, 15));
	_settingsBtn->setFixedSize(26, 26);
	_settingsBtn->setCursor(Qt::PointingHandCursor);
	_settingsBtn->setAutoDefault(false);
	connect(_settingsBtn, &QPushButton::clicked, this, &CopilotSidebarWidget::openSettingsRequested);
	primaryLayout->addWidget(_settingsBtn);

	_expandBtn = new QPushButton(primaryHeader);
	_expandBtn->setObjectName(QStringLiteral("CopilotExpandBtn"));
	_expandBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Expand panel width"));
	_expandBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Expand panel width"));
	_expandBtn->setIcon(Theme::icon(Theme::Icon::Expand, Theme::ColorRole::TextSecondary));
	_expandBtn->setIconSize(QSize(14, 14));
	_expandBtn->setFixedSize(26, 26);
	_expandBtn->setCursor(Qt::PointingHandCursor);
	_expandBtn->setAutoDefault(false);
	connect(_expandBtn, &QPushButton::clicked, this, &CopilotSidebarWidget::expandToggleRequested);
	primaryLayout->addWidget(_expandBtn);

	_closeBtn = new QPushButton(primaryHeader);
	_closeBtn->setObjectName(QStringLiteral("CopilotCloseBtn"));
	_closeBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Close Copilot"));
	_closeBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Close Copilot"));
	_closeBtn->setIcon(Theme::icon(Theme::Icon::Close, Theme::ColorRole::TextSecondary));
	_closeBtn->setIconSize(QSize(14, 14));
	_closeBtn->setFixedSize(26, 26);
	_closeBtn->setCursor(Qt::PointingHandCursor);
	_closeBtn->setAutoDefault(false);
	connect(_closeBtn, &QPushButton::clicked, this, &CopilotSidebarWidget::closeRequested);
	primaryLayout->addWidget(_closeBtn);

	root->addWidget(primaryHeader);

	// 2. Layer 2: Session Sub-header (Back + Session Title + History Drawer toggle)
	auto *sessionHeader = new QWidget(this);
	sessionHeader->setObjectName(QStringLiteral("CopilotSessionHeader"));
	auto *sessionLayout = new QHBoxLayout(sessionHeader);
	sessionLayout->setContentsMargins(10, 4, 8, 4);
	sessionLayout->setSpacing(6);

	_backSessionsBtn = new QPushButton(sessionHeader);
	_backSessionsBtn->setObjectName(QStringLiteral("CopilotBackBtn"));
	_backSessionsBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Toggle conversation history"));
	_backSessionsBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Browse saved conversations"));
	_backSessionsBtn->setIcon(Theme::icon(Theme::Icon::Back, Theme::ColorRole::TextSecondary));
	_backSessionsBtn->setIconSize(QSize(14, 14));
	_backSessionsBtn->setFixedSize(24, 24);
	_backSessionsBtn->setCursor(Qt::PointingHandCursor);
	_backSessionsBtn->setAutoDefault(false);
	connect(_backSessionsBtn, &QPushButton::clicked, this, &CopilotSidebarWidget::onHistoryViewToggled);
	sessionLayout->addWidget(_backSessionsBtn);

	_sessionTitleLabel = new QLabel(QCoreApplication::translate("MeetingUI", "AI Copilot"), sessionHeader);
	_sessionTitleLabel->setObjectName(QStringLiteral("CopilotSessionTitle"));
	_sessionTitleLabel->setTextFormat(Qt::PlainText);
	_sessionTitleLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	sessionLayout->addWidget(_sessionTitleLabel, 1);

	_sessionsBtn = new QPushButton(sessionHeader);
	_sessionsBtn->setObjectName(QStringLiteral("CopilotSessionsBtn"));
	_sessionsBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Chat history and actions"));
	_sessionsBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Switch saved conversations"));
	_sessionsBtn->setIcon(Theme::icon(Theme::Icon::SidebarRight, Theme::ColorRole::TextSecondary));
	_sessionsBtn->setIconSize(QSize(15, 15));
	_sessionsBtn->setFixedSize(24, 24);
	_sessionsBtn->setCursor(Qt::PointingHandCursor);
	_sessionsBtn->setAutoDefault(false);
	connect(_sessionsBtn, &QPushButton::clicked, this, &CopilotSidebarWidget::onHistoryViewToggled);
	sessionLayout->addWidget(_sessionsBtn);

	root->addWidget(sessionHeader);

	// 3. E2EE authorization gate (shown only when an encrypted meeting awaits permission)
	_contextBar = new QWidget(this);
	_contextBar->setObjectName(QStringLiteral("CopilotContextBar"));
	auto *contextLayout = new QVBoxLayout(_contextBar);
	contextLayout->setContentsMargins(12, 6, 12, 6);
	contextLayout->setSpacing(6);

	_e2eeBanner = new QWidget(_contextBar);
	_e2eeBanner->setObjectName(QStringLiteral("CopilotE2eeBanner"));
	auto *e2eeLayout = new QHBoxLayout(_e2eeBanner);
	e2eeLayout->setContentsMargins(8, 6, 8, 6);
	e2eeLayout->setSpacing(8);

	_e2eeBannerLabel = new QLabel(
		QCoreApplication::translate("MeetingUI", "Encrypted meeting: context is withheld until allowed."),
		_e2eeBanner);
	_e2eeBannerLabel->setObjectName(QStringLiteral("CopilotE2eeLabel"));
	_e2eeBannerLabel->setWordWrap(true);
	e2eeLayout->addWidget(_e2eeBannerLabel, 1);

	_e2eeAuthorizeBtn = new QPushButton(
		QCoreApplication::translate("MeetingUI", "Allow"),
		_e2eeBanner);
	_e2eeAuthorizeBtn->setObjectName(QStringLiteral("CopilotAllowE2eeBtn"));
	_e2eeAuthorizeBtn->setCursor(Qt::PointingHandCursor);
	_e2eeAuthorizeBtn->setAutoDefault(false);
	connect(_e2eeAuthorizeBtn, &QPushButton::clicked, this, [this] {
		if (_coordinator) {
			_coordinator->authorizeE2eeContextForCurrentMeeting(true);
		}
	});
	e2eeLayout->addWidget(_e2eeAuthorizeBtn);

	_e2eeBanner->hide();
	contextLayout->addWidget(_e2eeBanner);
	_contextBar->hide();
	root->addWidget(_contextBar);

	// 4. Body stack: Page 0 = Chat stream, Page 1 = Inline Session History
	_bodyStack = new QStackedWidget(this);
	_bodyStack->setObjectName(QStringLiteral("CopilotBodyStack"));

	_chatStreamPage = new QWidget(_bodyStack);
	auto *streamPageLayout = new QVBoxLayout(_chatStreamPage);
	streamPageLayout->setContentsMargins(0, 0, 0, 0);
	streamPageLayout->setSpacing(0);

	_scrollArea = new QScrollArea(_chatStreamPage);
	_scrollArea->setObjectName(QStringLiteral("CopilotMessageScroll"));
	_scrollArea->setWidgetResizable(true);
	_scrollArea->setFrameShape(QFrame::NoFrame);
	_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

	_scrollContent = new QWidget(_scrollArea);
	_scrollContent->setObjectName(QStringLiteral("CopilotScrollContent"));
	auto *contentRoot = new QVBoxLayout(_scrollContent);
	contentRoot->setContentsMargins(12, 12, 12, 12);
	contentRoot->setSpacing(10);

	// Unconfigured CTA card
	_unconfiguredCard = new QWidget(_scrollContent);
	_unconfiguredCard->setObjectName(QStringLiteral("CopilotUnconfiguredCard"));
	auto *unconfLayout = new QVBoxLayout(_unconfiguredCard);
	unconfLayout->setContentsMargins(14, 14, 14, 14);
	unconfLayout->setSpacing(8);

	_unconfiguredTitle = new QLabel(
		QCoreApplication::translate("MeetingUI", "Connect your Agent API"),
		_unconfiguredCard);
	_unconfiguredTitle->setObjectName(QStringLiteral("CopilotCardTitle"));
	_unconfiguredTitle->setWordWrap(true);

	_unconfiguredBody = new QLabel(
		QCoreApplication::translate("MeetingUI",
			"Configure an OpenAI-compatible API, custom REST webhook, or local model server to chat with AI Copilot."),
		_unconfiguredCard);
	_unconfiguredBody->setObjectName(QStringLiteral("CopilotCardBody"));
	_unconfiguredBody->setWordWrap(true);

	_configureBtn = new QPushButton(
		QCoreApplication::translate("MeetingUI", "Configure Copilot"),
		_unconfiguredCard);
	_configureBtn->setObjectName(QStringLiteral("CopilotConfigureBtn"));
	_configureBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Configure Copilot"));
	_configureBtn->setCursor(Qt::PointingHandCursor);
	_configureBtn->setAutoDefault(false);
	connect(_configureBtn, &QPushButton::clicked, this, &CopilotSidebarWidget::openSettingsRequested);

	unconfLayout->addWidget(_unconfiguredTitle);
	unconfLayout->addWidget(_unconfiguredBody);
	unconfLayout->addWidget(_configureBtn, 0, Qt::AlignLeft);
	contentRoot->addWidget(_unconfiguredCard);

	// Configured empty hint card
	_emptyCard = new QWidget(_scrollContent);
	_emptyCard->setObjectName(QStringLiteral("CopilotEmptyCard"));
	auto *emptyLayout = new QVBoxLayout(_emptyCard);
	emptyLayout->setContentsMargins(12, 10, 12, 10);
	_emptyLabel = new QLabel(
		QCoreApplication::translate("MeetingUI",
			"Ask AI Copilot to summarize notes, draft agendas, or answer technical questions."),
		_emptyCard);
	_emptyLabel->setObjectName(QStringLiteral("CopilotCardBody"));
	_emptyLabel->setWordWrap(true);
	emptyLayout->addWidget(_emptyLabel);
	contentRoot->addWidget(_emptyCard);

	_chatCanvas = new CopilotChatCanvasWidget(_scrollContent);
	connect(_chatCanvas, &CopilotChatCanvasWidget::retryRequested, this, [this] {
		if (_coordinator && _coordinator->canRetryLastTurn()) {
			clearRequestError();
			_coordinator->retryLastTurn();
		}
	});
	connect(_chatCanvas, &CopilotChatCanvasWidget::openSettingsRequested,
		this, &CopilotSidebarWidget::openSettingsRequested);
	connect(_chatCanvas, &CopilotChatCanvasWidget::followupClicked, this, [this](const QString &prompt) {
		if (_coordinator && _coordinator->isConfigured() && !_coordinator->isStreaming()) {
			clearRequestError();
			_coordinator->sendUserMessage(prompt);
		}
	});
	connect(_chatCanvas, &CopilotChatCanvasWidget::internalActionTriggered,
		this, &CopilotSidebarWidget::handleInternalActionUrl);
	connect(_chatCanvas, &CopilotChatCanvasWidget::editMessageRequested,
		this, &CopilotSidebarWidget::beginEditingMessage);
	connect(_chatCanvas, &CopilotChatCanvasWidget::regenerateMessageRequested, this,
		[this](const QString &assistantMessageId) {
			if (_coordinator && !_coordinator->isStreaming()) {
				clearRequestError();
				cancelEditingMessage();
				_coordinator->regenerateFromAssistantMessage(assistantMessageId);
			}
		});
	connect(_chatCanvas, &CopilotChatCanvasWidget::exportConversationRequested,
		this, &CopilotSidebarWidget::onExportConversationClicked);
	contentRoot->addWidget(_chatCanvas);
	contentRoot->addStretch(1);

	_scrollArea->setWidget(_scrollContent);
	_scrollArea->setAcceptDrops(true);
	_scrollArea->installEventFilter(this);
	if (_scrollArea->viewport()) {
		_scrollArea->viewport()->setAcceptDrops(true);
		_scrollArea->viewport()->installEventFilter(this);
	}
	if (QScrollBar *vbar = _scrollArea->verticalScrollBar()) {
		connect(vbar, &QScrollBar::valueChanged, this, [this] {
			updateScrollToBottomButton();
		});
		connect(vbar, &QScrollBar::rangeChanged, this, [this] {
			updateScrollToBottomButton();
		});
	}
	streamPageLayout->addWidget(_scrollArea, 1);

	// Floating jump-to-bottom pill button over the scroll viewport
	_scrollToBottomBtn = new QPushButton(_scrollArea);
	_scrollToBottomBtn->setObjectName(QStringLiteral("CopilotScrollBottomBtn"));
	_scrollToBottomBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Jump to latest message"));
	_scrollToBottomBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Jump to bottom"));
	_scrollToBottomBtn->setIcon(Theme::icon(Theme::Icon::ArrowDown, Theme::ColorRole::Accent));
	_scrollToBottomBtn->setIconSize(QSize(15, 15));
	_scrollToBottomBtn->setFixedSize(30, 30);
	_scrollToBottomBtn->setCursor(Qt::PointingHandCursor);
	_scrollToBottomBtn->setAutoDefault(false);
	connect(_scrollToBottomBtn, &QPushButton::clicked, this, [this] {
		scrollToBottomIfFollowing(true);
	});
	_scrollToBottomBtn->hide();

	_bodyStack->addWidget(_chatStreamPage);

	// History page
	_historyPage = new QWidget(_bodyStack);
	_historyPage->setObjectName(QStringLiteral("CopilotHistoryPage"));
	auto *historyLayout = new QVBoxLayout(_historyPage);
	historyLayout->setContentsMargins(12, 10, 12, 10);
	historyLayout->setSpacing(8);

	auto *historyHdr = new QHBoxLayout();
	historyHdr->setContentsMargins(0, 0, 0, 0);
	historyHdr->setSpacing(6);
	auto *historyTitle = new QLabel(QCoreApplication::translate("MeetingUI", "Conversations"), _historyPage);
	historyTitle->setObjectName(QStringLiteral("CopilotHistoryHeaderLabel"));
	historyHdr->addWidget(historyTitle, 1);

	_historyNewBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "+ New Chat"), _historyPage);
	_historyNewBtn->setObjectName(QStringLiteral("CopilotHistoryNewBtn"));
	_historyNewBtn->setCursor(Qt::PointingHandCursor);
	_historyNewBtn->setAutoDefault(false);
	connect(_historyNewBtn, &QPushButton::clicked, this, &CopilotSidebarWidget::onNewChatClicked);
	historyHdr->addWidget(_historyNewBtn);
	historyLayout->addLayout(historyHdr);

	_historyList = new QListWidget(_historyPage);
	_historyList->setObjectName(QStringLiteral("CopilotHistoryList"));
	_historyList->setFrameShape(QFrame::NoFrame);
	_historyList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	connect(_historyList, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
		if (!_coordinator || !item) {
			return;
		}
		const QString sid = item->data(kHistorySessionIdRole).toString();
		if (!sid.isEmpty()) {
			cancelEditingMessage();
			_coordinator->switchSession(sid);
			setHistoryViewVisible(false);
		}
	});
	historyLayout->addWidget(_historyList, 1);

	auto *historyActions = new QHBoxLayout();
	historyActions->setContentsMargins(0, 0, 0, 0);
	historyActions->setSpacing(6);

	_historyRenameBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Rename..."), _historyPage);
	_historyRenameBtn->setObjectName(QStringLiteral("CopilotHistoryActionBtn"));
	_historyRenameBtn->setCursor(Qt::PointingHandCursor);
	_historyRenameBtn->setAutoDefault(false);
	connect(_historyRenameBtn, &QPushButton::clicked, this, &CopilotSidebarWidget::onRenameActiveSession);
	historyActions->addWidget(_historyRenameBtn);

	_historyDeleteBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Delete"), _historyPage);
	_historyDeleteBtn->setObjectName(QStringLiteral("CopilotHistoryActionBtn"));
	_historyDeleteBtn->setCursor(Qt::PointingHandCursor);
	_historyDeleteBtn->setAutoDefault(false);
	connect(_historyDeleteBtn, &QPushButton::clicked, this, &CopilotSidebarWidget::onDeleteActiveSession);
	historyActions->addWidget(_historyDeleteBtn);

	historyActions->addStretch(1);

	_historyExportBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Export .md"), _historyPage);
	_historyExportBtn->setObjectName(QStringLiteral("CopilotHistoryActionBtn"));
	_historyExportBtn->setCursor(Qt::PointingHandCursor);
	_historyExportBtn->setAutoDefault(false);
	connect(_historyExportBtn, &QPushButton::clicked, this, &CopilotSidebarWidget::onExportConversationClicked);
	historyActions->addWidget(_historyExportBtn);
	historyLayout->addLayout(historyActions);

	_bodyStack->addWidget(_historyPage);
	root->addWidget(_bodyStack, 1);

	// 5. Bottom Dock: Banners + Dictation Card + Unified Composer Card + Status/Mode Footer
	_inputPanel = new QWidget(this);
	_inputPanel->setObjectName(QStringLiteral("CopilotInputPanel"));
	_inputPanel->setAcceptDrops(true);
	_inputPanel->installEventFilter(this);
	auto *inputLayout = new QVBoxLayout(_inputPanel);
	inputLayout->setContentsMargins(10, 8, 10, 6);
	inputLayout->setSpacing(6);

	_errorBanner = new QWidget(_inputPanel);
	_errorBanner->setObjectName(QStringLiteral("CopilotErrorBanner"));
	auto *errLayout = new QHBoxLayout(_errorBanner);
	errLayout->setContentsMargins(8, 6, 8, 6);
	errLayout->setSpacing(6);

	_errorBannerLabel = new QLabel(_errorBanner);
	_errorBannerLabel->setObjectName(QStringLiteral("CopilotErrorLabel"));
	_errorBannerLabel->setTextFormat(Qt::PlainText);
	_errorBannerLabel->setWordWrap(true);
	errLayout->addWidget(_errorBannerLabel, 1);

	_errorSettingsBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Settings"), _errorBanner);
	_errorSettingsBtn->setObjectName(QStringLiteral("CopilotErrorSettingsBtn"));
	_errorSettingsBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Open Copilot settings"));
	_errorSettingsBtn->setCursor(Qt::PointingHandCursor);
	_errorSettingsBtn->setAutoDefault(false);
	_errorSettingsBtn->hide();
	connect(_errorSettingsBtn, &QPushButton::clicked, this, &CopilotSidebarWidget::openSettingsRequested);
	errLayout->addWidget(_errorSettingsBtn);

	_errorDismissBtn = new QPushButton(_errorBanner);
	_errorDismissBtn->setObjectName(QStringLiteral("CopilotErrorDismissBtn"));
	_errorDismissBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Dismiss error"));
	_errorDismissBtn->setIcon(Theme::icon(Theme::Icon::Close, Theme::ColorRole::TextSecondary));
	_errorDismissBtn->setIconSize(QSize(12, 12));
	_errorDismissBtn->setFixedSize(20, 20);
	_errorDismissBtn->setCursor(Qt::PointingHandCursor);
	_errorDismissBtn->setAutoDefault(false);
	connect(_errorDismissBtn, &QPushButton::clicked, this, &CopilotSidebarWidget::clearRequestError);
	errLayout->addWidget(_errorDismissBtn);

	_errorBanner->hide();
	inputLayout->addWidget(_errorBanner);

	_editingBanner = new QWidget(_inputPanel);
	_editingBanner->setObjectName(QStringLiteral("CopilotEditingBanner"));
	auto *editBannerLayout = new QHBoxLayout(_editingBanner);
	editBannerLayout->setContentsMargins(8, 5, 8, 5);
	editBannerLayout->setSpacing(6);

	_editingBannerLabel = new QLabel(
		QCoreApplication::translate("MeetingUI", "Editing previous message — sending will re-run from here"),
		_editingBanner);
	_editingBannerLabel->setObjectName(QStringLiteral("CopilotEditingLabel"));
	_editingBannerLabel->setTextFormat(Qt::PlainText);
	_editingBannerLabel->setWordWrap(true);
	editBannerLayout->addWidget(_editingBannerLabel, 1);

	_editingCancelBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Cancel"), _editingBanner);
	_editingCancelBtn->setObjectName(QStringLiteral("CopilotEditingCancelBtn"));
	_editingCancelBtn->setCursor(Qt::PointingHandCursor);
	_editingCancelBtn->setAutoDefault(false);
	connect(_editingCancelBtn, &QPushButton::clicked, this, &CopilotSidebarWidget::cancelEditingMessage);
	editBannerLayout->addWidget(_editingCancelBtn);

	_editingBanner->hide();
	inputLayout->addWidget(_editingBanner);

	// Dictation floating card (UI placeholder with real mic device selector & high-density symmetric waveform)
	_dictationCard = new QFrame(_inputPanel);
	_dictationCard->setObjectName(QStringLiteral("CopilotDictationCard"));
	_dictationCard->setFrameShape(QFrame::NoFrame);
	auto *dictLayout = new QVBoxLayout(_dictationCard);
	dictLayout->setContentsMargins(12, 10, 10, 10);
	dictLayout->setSpacing(8);

	auto *dictHeaderRow = new QHBoxLayout();
	dictHeaderRow->setContentsMargins(0, 0, 0, 0);
	dictHeaderRow->setSpacing(6);
	auto *dictTitle = new QLabel(QCoreApplication::translate("MeetingUI", "Dictation"), _dictationCard);
	dictTitle->setObjectName(QStringLiteral("CopilotDictationTitle"));
	dictHeaderRow->addWidget(dictTitle, 1);

	_dictationCloseBtn = new QPushButton(_dictationCard);
	_dictationCloseBtn->setObjectName(QStringLiteral("CopilotDictationCloseBtn"));
	_dictationCloseBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Close Dictation"));
	_dictationCloseBtn->setIcon(Theme::icon(Theme::Icon::Close, Theme::ColorRole::TextSecondary));
	_dictationCloseBtn->setIconSize(QSize(13, 13));
	_dictationCloseBtn->setFixedSize(22, 22);
	_dictationCloseBtn->setCursor(Qt::PointingHandCursor);
	_dictationCloseBtn->setAutoDefault(false);
	connect(_dictationCloseBtn, &QPushButton::clicked, this, [this] {
		setDictationVisible(false);
	});
	dictHeaderRow->addWidget(_dictationCloseBtn);
	dictLayout->addLayout(dictHeaderRow);

	_dictationSubtitle = new QLabel(
		QCoreApplication::translate(
			"MeetingUI",
			"Speak and it becomes text. Adjust <a href=\"cohavora://settings/copilot/asr\">settings</a> or <a href=\"cohavora://dictation/info\">how it works</a>."),
		_dictationCard);
	_dictationSubtitle->setObjectName(QStringLiteral("CopilotDictationSubtitle"));
	_dictationSubtitle->setTextFormat(Qt::RichText);
	_dictationSubtitle->setOpenExternalLinks(false);
	_dictationSubtitle->setWordWrap(true);
	connect(_dictationSubtitle, &QLabel::linkActivated, this, [this](const QString &link) {
		handleInternalActionUrl(QUrl(link));
	});
	dictLayout->addWidget(_dictationSubtitle);

	_dictationMicCombo = MeetingUI::createAccessibleComboBox(_dictationCard);
	_dictationMicCombo->setObjectName(QStringLiteral("CopilotDictationMicCombo"));
	_dictationMicCombo->setAccessibleName(QCoreApplication::translate("MeetingUI", "Dictation microphone"));
	_dictationMicCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	_dictationMicCombo->setMinimumContentsLength(10);
	_dictationMicCombo->addItem(
		Theme::icon(Theme::Icon::Microphone, Theme::ColorRole::TextSecondary),
		QCoreApplication::translate("MeetingUI", "System Default Microphone"));
	connect(_dictationMicCombo, static_cast<void (QComboBox::*)(int)>(&QComboBox::activated),
		this, [this](int) {
			onDictationMicActivated();
		});
	dictLayout->addWidget(_dictationMicCombo);

	_dictationWaveform = new DictationWaveformWidget(_dictationCard);
	dictLayout->addWidget(_dictationWaveform);

	_dictationStatusHint = new QLabel(_dictationCard);
	_dictationStatusHint->setObjectName(QStringLiteral("CopilotDictationHint"));
	_dictationStatusHint->setTextFormat(Qt::PlainText);
	_dictationStatusHint->setProperty("error", false);
	_dictationStatusHint->setWordWrap(true);
	_dictationStatusHint->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
	_dictationStatusHint->hide();
	dictLayout->addWidget(_dictationStatusHint);

	_dictationCard->hide();
	inputLayout->addWidget(_dictationCard);

	// Unified Composer Card
	_composerCard = new QFrame(_inputPanel);
	_composerCard->setObjectName(QStringLiteral("CopilotComposerCard"));
	_composerCard->setFrameShape(QFrame::NoFrame);
	_composerCard->setProperty("focused", false);
	auto *composerLayout = new QVBoxLayout(_composerCard);
	composerLayout->setContentsMargins(10, 8, 8, 6);
	composerLayout->setSpacing(6);

	_contextChipsStrip = new QWidget(_composerCard);
	_contextChipsStrip->setObjectName(QStringLiteral("CopilotContextChipsStrip"));
	_contextChipsRowsLayout = new QVBoxLayout(_contextChipsStrip);
	_contextChipsRowsLayout->setContentsMargins(0, 0, 0, 0);
	_contextChipsRowsLayout->setSpacing(4);
	_contextChipsStrip->hide();
	composerLayout->addWidget(_contextChipsStrip);

	_attachmentStrip = new QWidget(_composerCard);
	_attachmentStrip->setObjectName(QStringLiteral("CopilotAttachmentStrip"));
	auto *stripLayout = new QVBoxLayout(_attachmentStrip);
	stripLayout->setContentsMargins(6, 5, 6, 5);
	stripLayout->setSpacing(4);

	auto *cardsRow = new QHBoxLayout();
	cardsRow->setContentsMargins(0, 0, 0, 0);
	cardsRow->setSpacing(6);
	_attachmentCardsLayout = new QHBoxLayout();
	_attachmentCardsLayout->setContentsMargins(0, 0, 0, 0);
	_attachmentCardsLayout->setSpacing(6);
	cardsRow->addLayout(_attachmentCardsLayout);
	cardsRow->addStretch(1);

	_attachmentSummaryLabel = new QLabel(_attachmentStrip);
	_attachmentSummaryLabel->setObjectName(QStringLiteral("CopilotAttachmentSummary"));
	_attachmentSummaryLabel->setTextFormat(Qt::PlainText);
	cardsRow->addWidget(_attachmentSummaryLabel, 0, Qt::AlignVCenter);
	stripLayout->addLayout(cardsRow);

	_attachmentWarningLabel = new QLabel(_attachmentStrip);
	_attachmentWarningLabel->setObjectName(QStringLiteral("CopilotAttachmentWarning"));
	_attachmentWarningLabel->setTextFormat(Qt::PlainText);
	_attachmentWarningLabel->setWordWrap(true);
	_attachmentWarningLabel->hide();
	stripLayout->addWidget(_attachmentWarningLabel);

	_attachmentStrip->hide();
	composerLayout->addWidget(_attachmentStrip);

	_inputEdit = new QPlainTextEdit(_composerCard);
	_inputEdit->setObjectName(QStringLiteral("CopilotInputEdit"));
	_inputEdit->setAccessibleName(QCoreApplication::translate("MeetingUI", "Copilot message input"));
	_inputEdit->setPlaceholderText(QCoreApplication::translate(
		"MeetingUI",
		"Chat with Copilot (/ commands, # context)"));
	_inputEdit->setFixedHeight(kMinInputHeight);
	_inputEdit->setAcceptDrops(true);
	_inputEdit->installEventFilter(this);
	if (_inputEdit->viewport()) {
		_inputEdit->viewport()->setAcceptDrops(true);
		_inputEdit->viewport()->installEventFilter(this);
	}
	connect(_inputEdit, &QPlainTextEdit::textChanged, this, [this] {
		_lastAttachmentError.clear();
		clearRequestError();
		if (!_applyingDictationEdit && _speechService && _speechService->isDictationActive()) {
			_dictationTracker.onManualUserEdit(_inputEdit->textCursor().position());
			if (_dictationAutoSendTimer) {
				_dictationAutoSendTimer->stop();
			}
		}
		updateInputEditHeight();
		updateCompletionPopup();
		syncActionButton();
	});
	connect(_inputEdit, &QPlainTextEdit::cursorPositionChanged, this, [this] {
		if (!_applyingDictationEdit && _speechService && _speechService->isDictationActive()) {
			_dictationTracker.onManualUserEdit(_inputEdit->textCursor().position());
		}
	});
	composerLayout->addWidget(_inputEdit);

	auto *composerToolbar = new QHBoxLayout();
	composerToolbar->setContentsMargins(0, 0, 0, 0);
	composerToolbar->setSpacing(4);

	_addContextBtn = new QPushButton(_composerCard);
	_addContextBtn->setObjectName(QStringLiteral("CopilotAddContextBtn"));
	_addContextBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Attach context"));
	_addContextBtn->setToolTip(QCoreApplication::translate(
		"MeetingUI",
		"Attach meeting chat, telemetry, participants, schedule, or images (or type #)"));
	_addContextBtn->setIcon(Theme::icon(Theme::Icon::Plus, Theme::ColorRole::TextSecondary));
	_addContextBtn->setIconSize(QSize(15, 15));
	_addContextBtn->setFixedSize(26, 26);
	_addContextBtn->setCursor(Qt::PointingHandCursor);
	_addContextBtn->setAutoDefault(false);
	connect(_addContextBtn, &QPushButton::clicked, this, &CopilotSidebarWidget::onAddContextClicked);
	composerToolbar->addWidget(_addContextBtn);

	_modelSwitchBtn = new QPushButton(_composerCard);
	_modelSwitchBtn->setObjectName(QStringLiteral("CopilotModelSwitchBtn"));
	_modelSwitchBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Switch AI model and reasoning"));
	_modelSwitchBtn->setIcon(Theme::icon(Theme::Icon::Sparkle, Theme::ColorRole::Accent));
	_modelSwitchBtn->setIconSize(QSize(14, 14));
	_modelSwitchBtn->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
	_modelSwitchBtn->setMinimumHeight(26);
	_modelSwitchBtn->setCursor(Qt::PointingHandCursor);
	_modelSwitchBtn->setAutoDefault(false);
	connect(_modelSwitchBtn, &QPushButton::clicked, this, &CopilotSidebarWidget::onModelSwitchClicked);
	composerToolbar->addWidget(_modelSwitchBtn, 1);

	_dictationBtn = new QPushButton(_composerCard);
	_dictationBtn->setObjectName(QStringLiteral("CopilotDictationBtn"));
	_dictationBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Toggle voice dictation"));
	_dictationBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Voice dictation (speak into message box)"));
	_dictationBtn->setIcon(Theme::icon(Theme::Icon::Microphone, Theme::ColorRole::TextSecondary));
	_dictationBtn->setIconSize(QSize(15, 15));
	_dictationBtn->setFixedSize(26, 26);
	_dictationBtn->setCursor(Qt::PointingHandCursor);
	_dictationBtn->setAutoDefault(false);
	_dictationBtn->setProperty("active", false);
	connect(_dictationBtn, &QPushButton::clicked, this, &CopilotSidebarWidget::onDictationToggled);
	composerToolbar->addWidget(_dictationBtn);

	_actionBtn = new QPushButton(_composerCard);
	_actionBtn->setObjectName(QStringLiteral("CopilotActionBtn"));
	_actionBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Send"));
	_actionBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Send message (Enter)"));
	_actionBtn->setIcon(Theme::icon(Theme::Icon::Send, Theme::ColorRole::OnAccent));
	_actionBtn->setIconSize(QSize(15, 15));
	_actionBtn->setFixedSize(28, 28);
	_actionBtn->setCursor(Qt::PointingHandCursor);
	_actionBtn->setAutoDefault(false);
	connect(_actionBtn, &QPushButton::clicked, this, &CopilotSidebarWidget::onSendOrStopClicked);
	composerToolbar->addWidget(_actionBtn);

	composerLayout->addLayout(composerToolbar);
	inputLayout->addWidget(_composerCard);

	// Bottom Mode / Permission & Connection Status Footer
	auto *footerBar = new QWidget(_inputPanel);
	footerBar->setObjectName(QStringLiteral("CopilotFooterBar"));
	auto *footerLayout = new QHBoxLayout(footerBar);
	footerLayout->setContentsMargins(2, 0, 2, 0);
	footerLayout->setSpacing(6);

	_modePermBtn = new QPushButton(footerBar);
	_modePermBtn->setObjectName(QStringLiteral("CopilotModePermBtn"));
	_modePermBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Copilot mode and tool permissions"));
	_modePermBtn->setIcon(Theme::icon(Theme::Icon::ChatMode, Theme::ColorRole::TextSecondary));
	_modePermBtn->setIconSize(QSize(13, 13));
	_modePermBtn->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
	_modePermBtn->setMinimumHeight(22);
	_modePermBtn->setCursor(Qt::PointingHandCursor);
	_modePermBtn->setAutoDefault(false);
	connect(_modePermBtn, &QPushButton::clicked, this, &CopilotSidebarWidget::onModePermClicked);
	footerLayout->addWidget(_modePermBtn, 1);

	_statusIndicatorBtn = new QPushButton(footerBar);
	_statusIndicatorBtn->setObjectName(QStringLiteral("CopilotStatusIndicatorBtn"));
	_statusIndicatorBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Copilot connection status"));
	_statusIndicatorBtn->setIcon(Theme::icon(Theme::Icon::Antenna, Theme::ColorRole::TextMuted));
	_statusIndicatorBtn->setIconSize(QSize(14, 14));
	_statusIndicatorBtn->setFixedSize(22, 22);
	_statusIndicatorBtn->setCursor(Qt::PointingHandCursor);
	_statusIndicatorBtn->setAutoDefault(false);
	connect(_statusIndicatorBtn, &QPushButton::clicked, this, &CopilotSidebarWidget::openSettingsRequested);
	footerLayout->addWidget(_statusIndicatorBtn);

	inputLayout->addWidget(footerBar);
	root->addWidget(_inputPanel);

	// Non-activating inline completion popup for /commands and #context chips
	_completionPopup = new QFrame(this);
	_completionPopup->setObjectName(QStringLiteral("CopilotCompletionPopup"));
	_completionPopup->setFrameShape(QFrame::NoFrame);
	auto *popupLayout = new QVBoxLayout(_completionPopup);
	popupLayout->setContentsMargins(4, 4, 4, 4);
	popupLayout->setSpacing(0);
	_completionList = new QListWidget(_completionPopup);
	_completionList->setObjectName(QStringLiteral("CopilotCompletionList"));
	_completionList->setFrameShape(QFrame::NoFrame);
	_completionList->setFocusPolicy(Qt::NoFocus);
	_completionList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	connect(_completionList, &QListWidget::itemClicked, this, &CopilotSidebarWidget::applyCompletionItem);
	popupLayout->addWidget(_completionList);
	_completionPopup->hide();

	updateChoiceCombosTheme();
}

void CopilotSidebarWidget::bindCoordinator() {
	for (const auto &conn : _coordinatorConnections) {
		disconnect(conn);
	}
	_coordinatorConnections.clear();

	if (_speechService) {
		_speechService->setAgentCoordinator(_coordinator);
		_speechService->setMeetingCoordinator(_coordinator ? _coordinator->attachedMeeting() : nullptr);
	}

	if (!_coordinator) {
		return;
	}

	_coordinatorConnections.push_back(connect(_coordinator, &AgentCoordinator::profileChanged, this, [this] {
		clearRequestError();
		syncHeaderAndState();
		syncActionButton();
	}));
	_coordinatorConnections.push_back(connect(_coordinator, &AgentCoordinator::contextStateChanged, this, [this] {
		if (_coordinator && (!_coordinator->isEncryptedMeeting() || _coordinator->isE2eeContextAuthorized())) {
			clearRequestError();
		}
		if (_speechService) {
			_speechService->setMeetingCoordinator(_coordinator ? _coordinator->attachedMeeting() : nullptr);
		}
		syncHeaderAndState();
	}));
	_coordinatorConnections.push_back(connect(_coordinator, &AgentCoordinator::chatModeChanged, this, [this] {
		syncHeaderAndState();
	}));
	_coordinatorConnections.push_back(connect(_coordinator, &AgentCoordinator::permissionModeChanged, this, [this] {
		syncHeaderAndState();
	}));
	_coordinatorConnections.push_back(connect(_coordinator, &AgentCoordinator::modelsFetched, this,
		[this](bool success, const QStringList &, const QString &safeError) {
			syncHeaderAndState();
			if (_pendingModelsRefreshMenu) {
				_pendingModelsRefreshMenu = false;
				if (!success && !safeError.isEmpty()) {
					showRequestError(safeError);
				} else if (success && _modelSwitchBtn && isVisible()) {
					onModelSwitchClicked();
				}
			}
		}));
	_coordinatorConnections.push_back(connect(_coordinator, &AgentCoordinator::messageAdded, this,
		[this](const AgentChatMessage &msg) {
			const bool follow = isNearBottom();
			if (_chatCanvas) {
				_chatCanvas->appendOrUpdateMessage(msg);
			}
			syncHeaderAndState();
			scrollToBottomIfFollowing(follow);
		}));
	_coordinatorConnections.push_back(connect(_coordinator, &AgentCoordinator::messageUpdated, this,
		[this](const AgentChatMessage &msg) {
			const bool follow = isNearBottom();
			if (_chatCanvas) {
				_chatCanvas->appendOrUpdateMessage(msg);
			}
			syncHeaderAndState();
			scrollToBottomIfFollowing(follow);
		}));
	_coordinatorConnections.push_back(connect(_coordinator, &AgentCoordinator::messageRemoved, this,
		[this](const QString &messageId) {
			if (_chatCanvas) {
				_chatCanvas->removeMessage(messageId);
			}
			syncHeaderAndState();
			updateScrollToBottomButton();
		}));
	_coordinatorConnections.push_back(connect(_coordinator, &AgentCoordinator::historyCleared, this, [this] {
		clearRequestError();
		cancelEditingMessage();
		rebuildMessages();
		syncHeaderAndState();
	}));
	_coordinatorConnections.push_back(connect(_coordinator, &AgentCoordinator::sessionsChanged, this, [this] {
		syncHeaderAndState();
		if (_historyVisible) {
			rebuildHistoryList();
		}
	}));
	_coordinatorConnections.push_back(connect(_coordinator, &AgentCoordinator::streamingStateChanged, this, [this](bool streaming) {
		if (_chatCanvas) {
			_chatCanvas->setActionsEnabled(!streaming);
		}
		syncActionButton();
		syncHeaderAndState();
	}));
	_coordinatorConnections.push_back(connect(_coordinator, &AgentCoordinator::requestFailed, this,
		[this](const QString &safeError) {
			showRequestError(safeError);
		}));
	_coordinatorConnections.push_back(connect(_coordinator, &AgentCoordinator::pendingActionAdded, this,
		[this](const AgentPendingToolAction &action) {
			const bool follow = isNearBottom();
			appendPendingActionUi(action);
			scrollToBottomIfFollowing(follow);
		}));
	_coordinatorConnections.push_back(connect(_coordinator, &AgentCoordinator::pendingActionUpdated, this,
		[this](const AgentPendingToolAction &action) {
			updatePendingActionUi(action);
		}));
}

void CopilotSidebarWidget::syncThemeVisuals() {
	updateChoiceCombosTheme();
	if (_chatCanvas) {
		_chatCanvas->update();
	}
	if (_dictationWaveform) {
		_dictationWaveform->update();
	}
	if (_leftEdgeShadow) {
		_leftEdgeShadow->update();
	}
	syncHeaderAndState();
	syncActionButton();
}

void CopilotSidebarWidget::updateSessionTitleText() {
	if (!_sessionTitleLabel) {
		return;
	}
	if (_historyVisible) {
		_sessionTitleLabel->setText(QCoreApplication::translate("MeetingUI", "Conversation History"));
		_sessionTitleLabel->setToolTip(QString());
		return;
	}
	const int availW = std::max(40, _sessionTitleLabel->width());
	const QString elided = _sessionTitleLabel->fontMetrics().elidedText(
		_fullSessionTitle,
		Qt::ElideRight,
		availW);
	_sessionTitleLabel->setText(elided);
	_sessionTitleLabel->setToolTip(_fullSessionTitle);
}

void CopilotSidebarWidget::updateModelSwitchLabel() {
	if (!_modelSwitchBtn) {
		return;
	}
	const int availW = std::max(48, _modelSwitchBtn->width() - 28);
	const QString elided = _modelSwitchBtn->fontMetrics().elidedText(
		_fullModelDisplayText,
		Qt::ElideRight,
		availW);
	_modelSwitchBtn->setText(elided);
}

void CopilotSidebarWidget::updateModePermLabel() {
	if (!_modePermBtn) {
		return;
	}
	const int availW = std::max(60, _modePermBtn->width() - 24);
	const QString elided = _modePermBtn->fontMetrics().elidedText(
		_fullModePermText,
		Qt::ElideRight,
		availW);
	_modePermBtn->setText(elided);
}

void CopilotSidebarWidget::syncHeaderAndState() {
	const bool hasCoord = !_coordinator.isNull();
	const bool configured = hasCoord && _coordinator->isConfigured();

	// 1. Session title
	if (!hasCoord) {
		_fullSessionTitle = QCoreApplication::translate("MeetingUI", "AI Copilot");
	} else {
		QString activeTitle;
		const QString activeId = _coordinator->activeSessionId();
		for (const auto &s : _coordinator->sessionSummaries()) {
			if (s.sessionId == activeId) {
				activeTitle = s.title.trimmed();
				break;
			}
		}
		if (activeTitle.isEmpty()) {
			activeTitle = QCoreApplication::translate("MeetingUI", "New Chat");
		}
		_fullSessionTitle = QStringLiteral("%1 · %2").arg(
			QCoreApplication::translate("MeetingUI", "Copilot"),
			activeTitle);
	}
	updateSessionTitleText();

	// 2. Model selector button & context capacity label
	if (!hasCoord) {
		_fullModelDisplayText = QCoreApplication::translate("MeetingUI", "Unavailable");
		if (_modelSwitchBtn) {
			_modelSwitchBtn->setEnabled(false);
			_modelSwitchBtn->setToolTip(QString());
		}
	} else if (!configured) {
		_fullModelDisplayText = QCoreApplication::translate("MeetingUI", "Configure Model");
		if (_modelSwitchBtn) {
			_modelSwitchBtn->setEnabled(true);
			_modelSwitchBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Configure AI Copilot endpoint and model"));
		}
	} else {
		const auto &p = _coordinator->profile();
		const QString shortModel = shortModelDisplayName(p.model);
		const QString displayPrefix = p.name.trimmed().isEmpty() || p.name.trimmed() == QStringLiteral("Custom Agent")
			? shortModel
			: QStringLiteral("%1 · %2").arg(p.name.trimmed(), shortModel);
		const auto caps = _coordinator->resolveModelCapabilities(p.model);
		const QString ctxLabel = formatAgentContextWindowLabel(caps.contextLengthTokens);
		_fullModelDisplayText = ctxLabel.isEmpty()
			? displayPrefix
			: QStringLiteral("%1 | %2").arg(displayPrefix, ctxLabel);

		if (_modelSwitchBtn) {
			_modelSwitchBtn->setEnabled(true);
			QStringList tips;
			tips.push_back(QStringLiteral("%1: %2").arg(QCoreApplication::translate("MeetingUI", "Model"), p.model));
			if (!ctxLabel.isEmpty()) {
				tips.push_back(QStringLiteral("%1: %2").arg(QCoreApplication::translate("MeetingUI", "Context window"), ctxLabel));
			}
			const QString effortLabel = reasoningEffortShortLabel(p.reasoningEffort);
			if (!effortLabel.isEmpty()) {
				tips.push_back(QStringLiteral("%1: %2").arg(QCoreApplication::translate("MeetingUI", "Reasoning"), effortLabel));
			}
			tips.push_back(caps.supportsVision
				? QCoreApplication::translate("MeetingUI", "Image input: supported.")
				: QCoreApplication::translate("MeetingUI", "Image input: not listed by server metadata."));
			_modelSwitchBtn->setToolTip(tips.join(QLatin1Char('\n')));
		}
	}
	updateModelSwitchLabel();

	// 3. Mode & Permission footer + status antenna
	if (!hasCoord) {
		_fullModePermText = QCoreApplication::translate("MeetingUI", "Interactive | Manual permissions");
		if (_modePermBtn) _modePermBtn->setEnabled(false);
		if (_statusIndicatorBtn) {
			_statusIndicatorBtn->setIcon(Theme::icon(Theme::Icon::Antenna, Theme::ColorRole::TextMuted));
			_statusIndicatorBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Copilot unavailable"));
		}
	} else {
		const bool isAsk = (_coordinator->chatMode() == AgentChatMode::Ask);
		const bool confirmAll = (_coordinator->permissionMode() == AgentPermissionMode::ConfirmAll);
		if (isAsk) {
			_fullModePermText = QCoreApplication::translate("MeetingUI", "Ask | Read-only");
		} else if (confirmAll) {
			_fullModePermText = QCoreApplication::translate("MeetingUI", "Interactive | Confirm all");
		} else {
			_fullModePermText = QCoreApplication::translate("MeetingUI", "Interactive | Manual permissions");
		}
		if (_modePermBtn) {
			_modePermBtn->setEnabled(configured);
			_modePermBtn->setToolTip(QCoreApplication::translate(
				"MeetingUI",
				"Switch interaction mode (Agent / Ask) and tool approval policy"));
		}
		const bool needE2eeGate = _coordinator->isInMeeting() &&
			_coordinator->isEncryptedMeeting() &&
			!_coordinator->isE2eeContextAuthorized();
		if (_statusIndicatorBtn) {
			const Theme::ColorRole indRole = !configured
				? Theme::ColorRole::TextMuted
				: (needE2eeGate ? Theme::ColorRole::Warning : Theme::ColorRole::Success);
			_statusIndicatorBtn->setIcon(Theme::icon(Theme::Icon::Antenna, indRole));
			if (!configured) {
				_statusIndicatorBtn->setToolTip(QCoreApplication::translate("MeetingUI", "API not configured — click to open settings"));
			} else if (needE2eeGate) {
				_statusIndicatorBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Encrypted meeting: context sharing requires approval"));
			} else {
				_statusIndicatorBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Connected & ready — click to open Copilot settings"));
			}
		}
	}
	updateModePermLabel();

	const bool hasMessages = hasCoord && !_coordinator->messages().isEmpty();
	if (_newChatBtn) {
		_newChatBtn->setEnabled(hasCoord && (hasMessages || _coordinator->isStreaming()));
	}
	if (_sessionsBtn) {
		_sessionsBtn->setEnabled(hasCoord);
	}
	if (_backSessionsBtn) {
		_backSessionsBtn->setEnabled(hasCoord);
	}

	_unconfiguredCard->setVisible(!configured);
	_emptyCard->setVisible(configured && !hasMessages);

	if (hasCoord) {
		const bool needE2eeGate = _coordinator->isInMeeting() &&
			_coordinator->isEncryptedMeeting() &&
			!_coordinator->isE2eeContextAuthorized();
		_e2eeBanner->setVisible(needE2eeGate);
		_contextBar->setVisible(needE2eeGate);
	} else {
		_e2eeBanner->hide();
		_contextBar->hide();
	}

	_inputEdit->setEnabled(configured);
	if (_addContextBtn) {
		_addContextBtn->setEnabled(configured);
	}
	if (_dictationBtn) {
		_dictationBtn->setEnabled(configured);
	}
	if (!configured && _dictationVisible) {
		setDictationVisible(false);
	}
	rebuildContextChipsStrip();
	rebuildAttachmentPreviewStrip();

	if (_historyVisible) {
		rebuildHistoryList();
	}
}

void CopilotSidebarWidget::syncActionButton() {
	if (!_actionBtn) {
		return;
	}
	const bool configured = _coordinator && _coordinator->isConfigured();
	const bool streaming = _coordinator && _coordinator->isStreaming();
	_actionBtn->setProperty("stopMode", streaming);
	if (auto *st = _actionBtn->style()) {
		st->unpolish(_actionBtn);
		st->polish(_actionBtn);
	}

	if (streaming) {
		_actionBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Stop"));
		_actionBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Stop generating"));
		_actionBtn->setIcon(Theme::icon(Theme::Icon::Stop, Theme::ColorRole::OnAccent));
		_actionBtn->setEnabled(true);
	} else {
		_actionBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Send"));
		_actionBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Send message (Enter)"));
		_actionBtn->setIcon(Theme::icon(Theme::Icon::Send, Theme::ColorRole::OnAccent));
		const bool hasText = _inputEdit && !_inputEdit->toPlainText().trimmed().isEmpty();
		_actionBtn->setEnabled(configured && (hasText || !_pendingImages.isEmpty() || !_pendingContextChips.isEmpty()));
	}
}

void CopilotSidebarWidget::rebuildMessages() {
	if (_chatCanvas) {
		_chatCanvas->clearMessages();
	}

	for (auto it = _actionWidgets.begin(); it != _actionWidgets.end(); ++it) {
		if (it.value().card) {
			it.value().card->deleteLater();
		}
	}
	_actionWidgets.clear();

	if (!_coordinator || !_chatCanvas) {
		return;
	}

	QHash<QString, AgentPendingToolAction> actionsById;
	for (const auto &action : _coordinator->pendingActions()) {
		actionsById.insert(action.actionId, action);
	}
	QSet<QString> placedActions;

	for (const auto &msg : _coordinator->messages()) {
		if (msg.role == AgentChatMessage::Role::Tool) {
			if (!msg.pendingActionId.isEmpty() && actionsById.contains(msg.pendingActionId)) {
				appendPendingActionUi(actionsById.value(msg.pendingActionId));
				placedActions.insert(msg.pendingActionId);
			}
			continue;
		}
		_chatCanvas->appendOrUpdateMessage(msg);
	}

	for (const auto &action : _coordinator->pendingActions()) {
		if (!placedActions.contains(action.actionId)) {
			appendPendingActionUi(action);
		}
	}
	scrollToBottomIfFollowing(true);
}

void CopilotSidebarWidget::rebuildHistoryList() {
	if (!_historyList || !_coordinator) {
		return;
	}
	_historyList->clear();
	const auto sessions = _coordinator->sessionSummaries();
	const QString activeId = _coordinator->activeSessionId();
	for (const auto &s : sessions) {
		if (s.messageCount <= 0 && s.sessionId != activeId) {
			continue;
		}
		const QString title = s.title.trimmed().isEmpty()
			? QCoreApplication::translate("MeetingUI", "New Chat")
			: s.title.trimmed();
		const QString timeStr = s.updatedAt.isValid()
			? s.updatedAt.toString(QStringLiteral("MM-dd HH:mm"))
			: QString();
		const QString label = timeStr.isEmpty()
			? QStringLiteral("%1 (%2)").arg(title).arg(s.messageCount)
			: QStringLiteral("%1\n%2 · %3")
				.arg(title)
				.arg(QCoreApplication::translate("MeetingUI", "%1 messages").arg(s.messageCount))
				.arg(timeStr);
		auto *item = new QListWidgetItem(label, _historyList);
		item->setData(kHistorySessionIdRole, s.sessionId);
		if (s.sessionId == activeId) {
			item->setSelected(true);
			_historyList->setCurrentItem(item);
		}
	}
	if (_historyRenameBtn) {
		_historyRenameBtn->setEnabled(!activeId.isEmpty());
	}
	if (_historyDeleteBtn) {
		_historyDeleteBtn->setEnabled(!activeId.isEmpty() && (sessions.size() > 1 || !_coordinator->messages().isEmpty()));
	}
	if (_historyExportBtn) {
		_historyExportBtn->setEnabled(!_coordinator->messages().isEmpty());
	}
}

void CopilotSidebarWidget::appendPendingActionUi(const AgentPendingToolAction &action) {
	if (_actionWidgets.contains(action.actionId)) {
		updatePendingActionUi(action);
		return;
	}

	QWidget *cardParent = _chatCanvas ? static_cast<QWidget *>(_chatCanvas) : _scrollContent;
	auto *card = new QWidget(cardParent);
	card->setObjectName(QStringLiteral("CopilotActionCard"));
	auto *cardLayout = new QVBoxLayout(card);
	cardLayout->setContentsMargins(12, 10, 12, 10);
	cardLayout->setSpacing(6);

	auto *titleLabel = new QLabel(action.title, card);
	titleLabel->setObjectName(QStringLiteral("CopilotActionTitle"));
	titleLabel->setTextFormat(Qt::PlainText);
	titleLabel->setWordWrap(true);
	cardLayout->addWidget(titleLabel);

	auto *summaryLabel = new QLabel(action.summary, card);
	summaryLabel->setObjectName(QStringLiteral("CopilotActionSummary"));
	summaryLabel->setTextFormat(Qt::PlainText);
	summaryLabel->setWordWrap(true);
	cardLayout->addWidget(summaryLabel);

	auto *btnRow = new QHBoxLayout();
	btnRow->setContentsMargins(0, 2, 0, 0);
	btnRow->setSpacing(8);

	auto *confirmBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Confirm"), card);
	confirmBtn->setObjectName(QStringLiteral("CopilotActionConfirmBtn"));
	confirmBtn->setCursor(Qt::PointingHandCursor);
	confirmBtn->setAutoDefault(false);

	auto *rejectBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Cancel"), card);
	rejectBtn->setObjectName(QStringLiteral("CopilotActionRejectBtn"));
	rejectBtn->setCursor(Qt::PointingHandCursor);
	rejectBtn->setAutoDefault(false);

	const QString actionId = action.actionId;
	connect(confirmBtn, &QPushButton::clicked, this, [this, actionId] {
		if (_coordinator) {
			_coordinator->approvePendingAction(actionId);
		}
	});
	connect(rejectBtn, &QPushButton::clicked, this, [this, actionId] {
		if (_coordinator) {
			_coordinator->rejectPendingAction(actionId);
		}
	});

	btnRow->addWidget(confirmBtn);
	btnRow->addWidget(rejectBtn);
	btnRow->addStretch(1);
	cardLayout->addLayout(btnRow);

	auto *statusLabel = new QLabel(card);
	statusLabel->setObjectName(QStringLiteral("CopilotActionStatus"));
	statusLabel->setTextFormat(Qt::PlainText);
	statusLabel->setWordWrap(true);
	cardLayout->addWidget(statusLabel);

	_actionWidgets.insert(action.actionId, PendingActionUi{
		card, titleLabel, summaryLabel, confirmBtn, rejectBtn, statusLabel
	});
	updatePendingActionUi(action);
}

void CopilotSidebarWidget::updatePendingActionUi(const AgentPendingToolAction &action) {
	const auto it = _actionWidgets.find(action.actionId);
	if (it == _actionWidgets.end()) {
		return;
	}
	PendingActionUi &ui = it.value();
	const bool isPending = action.status == AgentPendingActionStatus::Pending;
	ui.confirmBtn->setVisible(isPending);
	ui.confirmBtn->setEnabled(isPending);
	ui.rejectBtn->setVisible(isPending);
	ui.rejectBtn->setEnabled(isPending);

	ui.statusLabel->setVisible(!isPending && !action.resultMessage.isEmpty());
	ui.statusLabel->setText(action.resultMessage);
	if (_chatCanvas && ui.card) {
		_chatCanvas->appendOrUpdateActionCard(action.actionId, ui.card);
	}
}

bool CopilotSidebarWidget::isNearBottom() const {
	if (!_scrollArea) {
		return true;
	}
	const QScrollBar *bar = _scrollArea->verticalScrollBar();
	return !bar || (bar->maximum() - bar->value() <= 48);
}

void CopilotSidebarWidget::scrollToBottomIfFollowing(bool force) {
	if (!force && !isNearBottom()) {
		return;
	}
	QTimer::singleShot(0, this, [this] {
		if (!_scrollArea) {
			return;
		}
		if (QScrollBar *bar = _scrollArea->verticalScrollBar()) {
			bar->setValue(bar->maximum());
		}
	});
}

void CopilotSidebarWidget::onSendOrStopClicked() {
	if (!_coordinator) {
		return;
	}
	if (_coordinator->isStreaming()) {
		_coordinator->stopActiveRequest();
		return;
	}
	const QString prompt = _inputEdit->toPlainText().trimmed();
	if ((prompt.isEmpty() && _pendingImages.isEmpty() && _pendingContextChips.isEmpty()) ||
		!_coordinator->isConfigured()) {
		return;
	}
	if (_coordinator->isInMeeting() &&
		_coordinator->isEncryptedMeeting() &&
		!_coordinator->isE2eeContextAuthorized()) {
		if (!_pendingImages.isEmpty()) {
			_lastAttachmentError = QCoreApplication::translate(
				"MeetingUI",
				"End-to-end encrypted meeting: please authorize AI sharing in the banner before sending images.");
			syncHeaderAndState();
			return;
		}
		if (hasMeetingScopedChip(_pendingContextChips)) {
			showRequestError(QCoreApplication::translate(
				"MeetingUI",
				"End-to-end encrypted meeting: please authorize AI sharing in the banner before attaching meeting context."));
			return;
		}
	}

	if (_historyVisible) {
		setHistoryViewVisible(false);
	}
	const QVector<AgentImageAttachment> imagesToSend = _pendingImages;
	const QVector<AgentContextChip> chipsToSend = _pendingContextChips;
	const QString editingId = _editingMessageId;
	_lastAttachmentError.clear();
	clearRequestError();
	hideCompletionPopup();
	cancelEditingMessage();
	if (_dictationAutoSendTimer) {
		_dictationAutoSendTimer->stop();
	}
	_applyingDictationEdit = true;
	_inputEdit->clear();
	_applyingDictationEdit = false;
	_dictationTracker.beginSession(0);
	clearPendingImages();
	clearPendingContextChips();
	if (!editingId.isEmpty()) {
		_coordinator->editAndResendUserMessage(editingId, prompt, &chipsToSend);
	} else {
		_coordinator->sendUserMessage(prompt, imagesToSend, chipsToSend);
	}
}

void CopilotSidebarWidget::onClearClicked() {
	cancelEditingMessage();
	if (_coordinator) {
		_coordinator->clearHistory();
	}
	clearPendingContextChips();
	hideCompletionPopup();
}

void CopilotSidebarWidget::onAttachImageClicked() {
	if (!_coordinator || !_coordinator->isConfigured()) {
		return;
	}
	const QStringList files = QFileDialog::getOpenFileNames(
		this,
		QCoreApplication::translate("MeetingUI", "Select Images"),
		QString(),
		QCoreApplication::translate("MeetingUI", "Images (*.png *.jpg *.jpeg *.webp *.gif)"),
		nullptr,
		QFileDialog::DontUseNativeDialog);
	if (files.isEmpty()) {
		return;
	}
	_lastAttachmentError.clear();
	for (const QString &file : files) {
		QString err;
		if (!addPendingImageFromFile(file, &err)) {
			if (!err.isEmpty()) {
				_lastAttachmentError = err;
			}
			break;
		}
	}
	syncHeaderAndState();
	syncActionButton();
}

bool CopilotSidebarWidget::addPendingImageFromFile(const QString &filePath, QString *outError) {
	_lastAttachmentError.clear();
	if (_pendingImages.size() >= kMaxAgentImagesPerMessage) {
		const QString err = QCoreApplication::translate(
			"MeetingUI", "Up to %1 images can be attached per message.").arg(kMaxAgentImagesPerMessage);
		_lastAttachmentError = err;
		if (outError) {
			*outError = err;
		}
		rebuildAttachmentPreviewStrip();
		return false;
	}
	const QFileInfo fi(filePath);
	if (!fi.exists() || !fi.isFile()) {
		const QString err = QCoreApplication::translate(
			"MeetingUI", "Unsupported or corrupted image file (only PNG, JPEG, WebP, and GIF are allowed).");
		_lastAttachmentError = err;
		if (outError) {
			*outError = err;
		}
		rebuildAttachmentPreviewStrip();
		return false;
	}
	if (fi.size() > kMaxRawAgentImageBytes) {
		const QString err = QCoreApplication::translate("MeetingUI", "Image file exceeds the 10 MB limit.");
		_lastAttachmentError = err;
		if (outError) {
			*outError = err;
		}
		rebuildAttachmentPreviewStrip();
		return false;
	}
	QFile f(filePath);
	if (!f.open(QIODevice::ReadOnly)) {
		const QString err = QCoreApplication::translate(
			"MeetingUI", "Unsupported or corrupted image file (only PNG, JPEG, WebP, and GIF are allowed).");
		_lastAttachmentError = err;
		if (outError) {
			*outError = err;
		}
		rebuildAttachmentPreviewStrip();
		return false;
	}
	const QByteArray raw = f.read(kMaxRawAgentImageBytes + 1);
	QString err;
	const AgentImageAttachment att = prepareAgentImageFromRawBytes(raw, fi.fileName(), &err);
	if (!att.isValid()) {
		_lastAttachmentError = err;
		if (outError) {
			*outError = err;
		}
		rebuildAttachmentPreviewStrip();
		return false;
	}
	const QImage preview = QImage::fromData(att.encodedBytes);
	const QPixmap thumb = makeRoundedCopilotThumbnail(
		preview, QSize(56, 56), std::max<qreal>(1.0, devicePixelRatioF()), 6);
	_pendingImages.push_back(att);
	_pendingThumbnails.push_back(thumb);
	rebuildAttachmentPreviewStrip();
	syncActionButton();
	return true;
}

bool CopilotSidebarWidget::addPendingImageFromQImage(
		const QImage &image,
		const QString &sourceName,
		QString *outError) {
	_lastAttachmentError.clear();
	if (_pendingImages.size() >= kMaxAgentImagesPerMessage) {
		const QString err = QCoreApplication::translate(
			"MeetingUI", "Up to %1 images can be attached per message.").arg(kMaxAgentImagesPerMessage);
		_lastAttachmentError = err;
		if (outError) {
			*outError = err;
		}
		rebuildAttachmentPreviewStrip();
		return false;
	}
	QString err;
	const AgentImageAttachment att = prepareAgentImageFromQImage(image, sourceName, 0, &err);
	if (!att.isValid()) {
		_lastAttachmentError = err;
		if (outError) {
			*outError = err;
		}
		rebuildAttachmentPreviewStrip();
		return false;
	}
	const QPixmap thumb = makeRoundedCopilotThumbnail(
		image, QSize(56, 56), std::max<qreal>(1.0, devicePixelRatioF()), 6);
	_pendingImages.push_back(att);
	_pendingThumbnails.push_back(thumb);
	rebuildAttachmentPreviewStrip();
	syncActionButton();
	return true;
}

void CopilotSidebarWidget::removePendingImageAt(int index) {
	if (index < 0 || index >= _pendingImages.size()) {
		return;
	}
	_lastAttachmentError.clear();
	_pendingImages.removeAt(index);
	if (index < _pendingThumbnails.size()) {
		_pendingThumbnails.removeAt(index);
	}
	syncHeaderAndState();
	syncActionButton();
}

void CopilotSidebarWidget::clearPendingImages() {
	_pendingImages.clear();
	_pendingThumbnails.clear();
	_lastAttachmentError.clear();
	rebuildAttachmentPreviewStrip();
	syncActionButton();
}

void CopilotSidebarWidget::rebuildAttachmentPreviewStrip() {
	if (!_attachmentStrip || !_attachmentCardsLayout || !_attachmentSummaryLabel || !_attachmentWarningLabel) {
		return;
	}
	while (auto *item = _attachmentCardsLayout->takeAt(0)) {
		if (auto *w = item->widget()) {
			w->deleteLater();
		}
		delete item;
	}

	if (_pendingImages.isEmpty()) {
		_attachmentSummaryLabel->clear();
		_attachmentSummaryLabel->hide();
		if (_lastAttachmentError.isEmpty()) {
			_attachmentWarningLabel->clear();
			_attachmentWarningLabel->hide();
			_attachmentStrip->hide();
		} else {
			_attachmentWarningLabel->setText(_lastAttachmentError);
			_attachmentWarningLabel->show();
			_attachmentStrip->show();
		}
		return;
	}

	qint64 totalBytes = 0;
	for (int i = 0; i < _pendingImages.size(); ++i) {
		const auto &att = _pendingImages[i];
		totalBytes += att.encodedBytes.size();

		auto *card = new QWidget(_attachmentStrip);
		card->setObjectName(QStringLiteral("CopilotAttachmentCard"));
		card->setFixedSize(56, 56);

		auto *thumbBtn = new QPushButton(card);
		thumbBtn->setObjectName(QStringLiteral("CopilotAttachmentThumbBtn"));
		thumbBtn->setGeometry(0, 0, 56, 56);
		thumbBtn->setCursor(Qt::PointingHandCursor);
		thumbBtn->setAutoDefault(false);
		thumbBtn->setAccessibleName(att.fileName);
		thumbBtn->setToolTip(att.fileName);
		if (i < _pendingThumbnails.size() && !_pendingThumbnails[i].isNull()) {
			thumbBtn->setIcon(QIcon(_pendingThumbnails[i]));
			thumbBtn->setIconSize(QSize(56, 56));
		}
		const QByteArray bytesCopy = att.encodedBytes;
		const QString nameCopy = att.fileName;
		connect(thumbBtn, &QPushButton::clicked, this, [this, bytesCopy, nameCopy] {
			const QImage preview = QImage::fromData(bytesCopy);
			if (!preview.isNull()) {
				showCopilotImagePreviewDialog(preview, nameCopy, this);
			}
		});

		auto *removeBtn = new QPushButton(QStringLiteral("✕"), card);
		removeBtn->setObjectName(QStringLiteral("CopilotAttachmentRemoveBtn"));
		removeBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Remove image"));
		removeBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Remove image"));
		removeBtn->setCursor(Qt::PointingHandCursor);
		removeBtn->setAutoDefault(false);
		removeBtn->setGeometry(36, 2, 18, 18);
		removeBtn->raise();
		connect(removeBtn, &QPushButton::clicked, this, [this, i] {
			removePendingImageAt(i);
		});

		_attachmentCardsLayout->addWidget(card);
	}

	_attachmentSummaryLabel->setText(QStringLiteral("%1/%2 · %3 KB")
		.arg(_pendingImages.size())
		.arg(kMaxAgentImagesPerMessage)
		.arg(qMax<qint64>(1, totalBytes / 1024)));
	_attachmentSummaryLabel->show();

	if (!_lastAttachmentError.isEmpty()) {
		_attachmentWarningLabel->setText(_lastAttachmentError);
		_attachmentWarningLabel->show();
	} else if (_coordinator && _coordinator->isConfigured()) {
		const auto caps = _coordinator->resolveModelCapabilities(_coordinator->profile().model);
		if (!caps.supportsVision) {
			_attachmentWarningLabel->setText(QCoreApplication::translate(
				"MeetingUI",
				"Current model (%1) may not support image input.").arg(_coordinator->profile().model));
			_attachmentWarningLabel->show();
		} else {
			_attachmentWarningLabel->clear();
			_attachmentWarningLabel->hide();
		}
	} else {
		_attachmentWarningLabel->clear();
		_attachmentWarningLabel->hide();
	}

	_attachmentStrip->show();
}

void CopilotSidebarWidget::showRequestError(const QString &safeError) {
	if (!_errorBanner || !_errorBannerLabel) {
		return;
	}
	const QString clean = safeError.trimmed();
	if (clean.isEmpty()) {
		clearRequestError();
		return;
	}
	const bool needsSettingsAction = !_coordinator || !_coordinator->isConfigured() ||
		clean.contains(QStringLiteral("Settings"), Qt::CaseInsensitive) ||
		clean.contains(QStringLiteral("API key"), Qt::CaseInsensitive) ||
		clean.contains(QStringLiteral("HTTP 401"), Qt::CaseInsensitive) ||
		clean.contains(QStringLiteral("HTTP 403"), Qt::CaseInsensitive);
	bool alreadyDisplayedInBubble = false;
	if (_coordinator && !_coordinator->messages().isEmpty()) {
		const auto &last = _coordinator->messages().last();
		alreadyDisplayedInBubble = (last.role == AgentChatMessage::Role::Assistant &&
			last.status == AgentChatMessage::Status::Failed &&
			last.errorMessage == clean);
	}
	if (alreadyDisplayedInBubble) {
		return;
	}
	_errorBannerLabel->setText(clean);
	if (_errorSettingsBtn) {
		_errorSettingsBtn->setVisible(needsSettingsAction);
	}
	_errorBanner->show();
}

void CopilotSidebarWidget::clearRequestError() {
	if (_errorBannerLabel) {
		_errorBannerLabel->clear();
	}
	if (_errorSettingsBtn) {
		_errorSettingsBtn->hide();
	}
	if (_errorBanner) {
		_errorBanner->hide();
	}
}

void CopilotSidebarWidget::onMoreMenuClicked() {
	if (!_moreActionsBtn) {
		return;
	}
	QMenu menu(this);
	Theme::styleMenu(menu, resolvedCurrentTone(this));

	const bool hasCoord = !_coordinator.isNull();
	const bool hasMessages = hasCoord && !_coordinator->messages().isEmpty();

	QAction *newAct = menu.addAction(
		Theme::icon(Theme::Icon::Plus, Theme::ColorRole::TextSecondary),
		QCoreApplication::translate("MeetingUI", "New Chat"));
	newAct->setEnabled(hasCoord && (hasMessages || _coordinator->isStreaming()));
	connect(newAct, &QAction::triggered, this, &CopilotSidebarWidget::onNewChatClicked);

	QAction *clearAct = menu.addAction(
		Theme::icon(Theme::Icon::Trash, Theme::ColorRole::TextSecondary),
		QCoreApplication::translate("MeetingUI", "Clear Conversation"));
	clearAct->setEnabled(hasCoord && (hasMessages || _coordinator->isStreaming()));
	connect(clearAct, &QAction::triggered, this, &CopilotSidebarWidget::onClearClicked);

	QAction *exportAct = menu.addAction(QCoreApplication::translate("MeetingUI", "Export Conversation (.md)"));
	exportAct->setEnabled(hasCoord && hasMessages);
	connect(exportAct, &QAction::triggered, this, &CopilotSidebarWidget::onExportConversationClicked);

	menu.addSeparator();
	QAction *settingsAct = menu.addAction(
		Theme::icon(Theme::Icon::Settings, Theme::ColorRole::TextSecondary),
		QCoreApplication::translate("MeetingUI", "Copilot Settings..."));
	connect(settingsAct, &QAction::triggered, this, &CopilotSidebarWidget::openSettingsRequested);

	menu.exec(_moreActionsBtn->mapToGlobal(QPoint(0, _moreActionsBtn->height())));
}

void CopilotSidebarWidget::onModelSwitchClicked() {
	if (!_modelSwitchBtn) {
		return;
	}
	if (!_coordinator || !_coordinator->isConfigured()) {
		emit openSettingsRequested();
		return;
	}

	QMenu menu(this);
	Theme::styleMenu(menu, resolvedCurrentTone(this));

	const auto &profile = _coordinator->profile();
	const auto &allProfiles = _coordinator->profiles();
	if (allProfiles.size() > 1) {
		QMenu *profileMenu = menu.addMenu(QCoreApplication::translate("MeetingUI", "Switch Profile"));
		Theme::styleMenu(*profileMenu, resolvedCurrentTone(this));
		auto *profileGroup = new QActionGroup(profileMenu);
		profileGroup->setExclusive(true);
		for (const auto &prof : allProfiles) {
			const QString baseLabel = prof.name.trimmed().isEmpty() ? prof.id : prof.name.trimmed();
			const QString label = prof.model.trimmed().isEmpty()
				? baseLabel
				: QStringLiteral("%1  (%2)").arg(baseLabel, prof.model.trimmed());
			QAction *pAct = profileMenu->addAction(label);
			pAct->setCheckable(true);
			pAct->setChecked(prof.id == profile.id);
			profileGroup->addAction(pAct);
			const QString targetId = prof.id;
			connect(pAct, &QAction::triggered, this, [this, targetId] {
				if (_coordinator) {
					_coordinator->switchProfile(targetId);
				}
			});
		}
		menu.addSeparator();
	}

	QStringList modelList = _coordinator->cachedModels();
	if (!profile.model.trimmed().isEmpty() && !modelList.contains(profile.model.trimmed())) {
		modelList.prepend(profile.model.trimmed());
	}

	if (!modelList.isEmpty()) {
		auto *modelGroup = new QActionGroup(&menu);
		modelGroup->setExclusive(true);
		for (const QString &m : modelList) {
			const auto caps = _coordinator->resolveModelCapabilities(m);
			const QString ctxLabel = formatAgentContextWindowLabel(caps.contextLengthTokens);
			const QString label = ctxLabel.isEmpty()
				? m
				: QStringLiteral("%1  (%2)").arg(m, ctxLabel);
			QAction *act = menu.addAction(label);
			act->setCheckable(true);
			act->setChecked(m == profile.model);
			modelGroup->addAction(act);
			connect(act, &QAction::triggered, this, [this, m] {
				if (_coordinator) {
					_coordinator->switchModel(m);
				}
			});
		}
		menu.addSeparator();
	}

	// Reasoning Effort submenu
	if (profile.protocol != AgentProtocolDialect::CustomWebhookJson) {
		QMenu *reasonMenu = menu.addMenu(QCoreApplication::translate("MeetingUI", "Reasoning Effort"));
		Theme::styleMenu(*reasonMenu, resolvedCurrentTone(this));
		auto *reasonGroup = new QActionGroup(reasonMenu);
		reasonGroup->setExclusive(true);

		const struct {
			AgentReasoningEffort effort;
			const char *label;
		} kEfforts[] = {
			{ AgentReasoningEffort::Default, QT_TRANSLATE_NOOP("MeetingUI", "Default") },
			{ AgentReasoningEffort::Low, QT_TRANSLATE_NOOP("MeetingUI", "Low") },
			{ AgentReasoningEffort::Medium, QT_TRANSLATE_NOOP("MeetingUI", "Medium") },
			{ AgentReasoningEffort::High, QT_TRANSLATE_NOOP("MeetingUI", "High") },
			{ AgentReasoningEffort::Extra, QT_TRANSLATE_NOOP("MeetingUI", "Extra (xhigh)") },
		};
		for (const auto &entry : kEfforts) {
			QAction *rAct = reasonMenu->addAction(QCoreApplication::translate("MeetingUI", entry.label));
			rAct->setCheckable(true);
			rAct->setChecked(profile.reasoningEffort == entry.effort);
			reasonGroup->addAction(rAct);
			const auto targetEffort = entry.effort;
			connect(rAct, &QAction::triggered, this, [this, targetEffort] {
				if (!_coordinator) {
					return;
				}
				AgentProfile next = _coordinator->profile();
				if (next.reasoningEffort != targetEffort) {
					next.reasoningEffort = targetEffort;
					_coordinator->setProfile(next);
				}
			});
		}
	}

	QAction *refreshAct = menu.addAction(
		Theme::icon(Theme::Icon::Refresh, Theme::ColorRole::TextSecondary),
		_coordinator->isFetchingModels()
			? QCoreApplication::translate("MeetingUI", "Refreshing models...")
			: QCoreApplication::translate("MeetingUI", "Refresh Available Models"));
	refreshAct->setEnabled(!_coordinator->isFetchingModels());
	connect(refreshAct, &QAction::triggered, this, [this] {
		if (_coordinator) {
			_pendingModelsRefreshMenu = true;
			_coordinator->fetchModels(_coordinator->profile());
		}
	});

	menu.addSeparator();
	QAction *cfgAct = menu.addAction(
		Theme::icon(Theme::Icon::Settings, Theme::ColorRole::TextSecondary),
		QCoreApplication::translate("MeetingUI", "Configure Endpoint & API Key..."));
	connect(cfgAct, &QAction::triggered, this, &CopilotSidebarWidget::openSettingsRequested);

	menu.exec(_modelSwitchBtn->mapToGlobal(QPoint(0, -menu.sizeHint().height())));
}

void CopilotSidebarWidget::onModePermClicked() {
	if (!_coordinator || !_modePermBtn) {
		return;
	}
	QMenu menu(this);
	Theme::styleMenu(menu, resolvedCurrentTone(this));

	auto *modeGroup = new QActionGroup(&menu);
	modeGroup->setExclusive(true);

	QAction *agentAct = menu.addAction(QCoreApplication::translate("MeetingUI", "Interactive (Agent tools)"));
	agentAct->setCheckable(true);
	agentAct->setChecked(_coordinator->chatMode() == AgentChatMode::Agent);
	modeGroup->addAction(agentAct);
	connect(agentAct, &QAction::triggered, this, [this] {
		if (_coordinator) {
			_coordinator->setChatMode(AgentChatMode::Agent);
		}
	});

	QAction *askAct = menu.addAction(QCoreApplication::translate("MeetingUI", "Ask (Read-only Q&A)"));
	askAct->setCheckable(true);
	askAct->setChecked(_coordinator->chatMode() == AgentChatMode::Ask);
	modeGroup->addAction(askAct);
	connect(askAct, &QAction::triggered, this, [this] {
		if (_coordinator) {
			_coordinator->setChatMode(AgentChatMode::Ask);
		}
	});

	menu.addSeparator();
	auto *permGroup = new QActionGroup(&menu);
	permGroup->setExclusive(true);
	const bool inAgentMode = (_coordinator->chatMode() == AgentChatMode::Agent);

	QAction *manualPermAct = menu.addAction(
		QCoreApplication::translate("MeetingUI", "Manual permissions (confirm actions)"));
	manualPermAct->setCheckable(true);
	manualPermAct->setEnabled(inAgentMode);
	manualPermAct->setChecked(_coordinator->permissionMode() == AgentPermissionMode::Manual);
	permGroup->addAction(manualPermAct);
	connect(manualPermAct, &QAction::triggered, this, [this] {
		if (_coordinator) {
			_coordinator->setPermissionMode(AgentPermissionMode::Manual);
		}
	});

	QAction *confirmAllAct = menu.addAction(
		QCoreApplication::translate("MeetingUI", "Confirm all tools (strict)"));
	confirmAllAct->setCheckable(true);
	confirmAllAct->setEnabled(inAgentMode);
	confirmAllAct->setChecked(_coordinator->permissionMode() == AgentPermissionMode::ConfirmAll);
	permGroup->addAction(confirmAllAct);
	connect(confirmAllAct, &QAction::triggered, this, [this] {
		if (_coordinator) {
			_coordinator->setPermissionMode(AgentPermissionMode::ConfirmAll);
		}
	});

	menu.exec(_modePermBtn->mapToGlobal(QPoint(0, -menu.sizeHint().height())));
}

void CopilotSidebarWidget::onDictationToggled() {
	setDictationVisible(!_dictationVisible);
}

void CopilotSidebarWidget::setDictationVisible(bool visible) {
	if (_dictationVisible == visible) {
		return;
	}
	_dictationVisible = visible;
	if (_dictationCard) {
		_dictationCard->setVisible(_dictationVisible);
	}
	if (_dictationBtn) {
		_dictationBtn->setProperty("active", _dictationVisible);
		_dictationBtn->setIcon(Theme::icon(
			Theme::Icon::Microphone,
			_dictationVisible ? Theme::ColorRole::Accent : Theme::ColorRole::TextSecondary));
		if (auto *st = _dictationBtn->style()) {
			st->unpolish(_dictationBtn);
			st->polish(_dictationBtn);
		}
	}
	if (_dictationVisible) {
		_dictationInfoShown = false;
		refreshDictationMicrophones();
		if (_inputEdit) {
			_dictationTracker.beginSession(_inputEdit->textCursor().position());
			_inputEdit->setFocus();
		} else {
			_dictationTracker.reset();
		}
		if (_speechService) {
			const QString comboDev = _dictationMicCombo ? _dictationMicCombo->currentData().toString() : QString();
			const QString deviceId = !comboDev.isEmpty()
				? comboDev
				: (_coordinator ? _coordinator->dictationDeviceId() : QString());
			_speechService->startDictation(deviceId);
		}
	} else {
		_dictationInfoShown = false;
		if (_dictationAutoSendTimer) {
			_dictationAutoSendTimer->stop();
		}
		if (_speechService) {
			_speechService->stopDictation();
		}
		_dictationTracker.reset();
		if (auto *wf = static_cast<DictationWaveformWidget *>(_dictationWaveform)) {
			wf->setListening(false, false);
		}
		updateDictationStatusHint();
	}
	if (_completionPopup && _completionPopup->isVisible()) {
		updateCompletionPopup();
	}
}

void CopilotSidebarWidget::refreshDictationMicrophones() {
	if (!_dictationMicCombo || !_callbackGate) {
		return;
	}
	const auto gate = _callbackGate;
	_micDiscoveryRequest = MeetingUI::MicrophoneDeviceDiscovery().request(
		[gate](const MeetingUI::AudioDeviceDiscovery::Result &result) {
			if (!gate || !result.succeeded || !result.devices) {
				return;
			}
			const auto devices = *result.devices;
			gate->Post([devices](CopilotSidebarWidget *self) {
				if (!self->_dictationMicCombo) {
					return;
				}
				const QString savedId = self->_coordinator ? self->_coordinator->dictationDeviceId() : QString();
				const QString previousId = !self->_dictationMicCombo->currentData().toString().isEmpty()
					? self->_dictationMicCombo->currentData().toString()
					: savedId;
				const QSignalBlocker blocker(self->_dictationMicCombo);
				self->_dictationMicCombo->clear();
				const QIcon micIcon = Theme::icon(Theme::Icon::Microphone, Theme::ColorRole::TextSecondary);
				if (devices.empty()) {
					self->_dictationMicCombo->addItem(
						micIcon,
						QCoreApplication::translate("MeetingUI", "No microphone detected"));
					return;
				}
				int selectedIdx = 0;
				bool matchedPrevious = false;
				for (int i = 0; i < static_cast<int>(devices.size()); ++i) {
					const auto &dev = devices[static_cast<size_t>(i)];
					const QString devId = QString::fromStdString(dev.id);
					const QString devName = QString::fromStdString(dev.name);
					self->_dictationMicCombo->addItem(micIcon, devName, devId);
					if (!previousId.isEmpty() && devId == previousId) {
						selectedIdx = i;
						matchedPrevious = true;
					} else if (!matchedPrevious && dev.is_default) {
						selectedIdx = i;
					}
				}
				self->_dictationMicCombo->setCurrentIndex(selectedIdx);
			});
		});
}

void CopilotSidebarWidget::bindSpeechService() {
	if (!_speechService) {
		return;
	}
	connect(_speechService, &AgentSpeechService::audioLevelChanged, this, [this](float level) {
		if (auto *wf = static_cast<DictationWaveformWidget *>(_dictationWaveform)) {
			wf->setAudioLevel(level);
		}
	});
	connect(_speechService, &AgentSpeechService::dictationActiveChanged, this, [this](bool active) {
		if (auto *wf = static_cast<DictationWaveformWidget *>(_dictationWaveform)) {
			wf->setListening(active, false);
		}
	});
	connect(_speechService, &AgentSpeechService::roomPassThroughChanged, this, [this](bool) {
		if (_dictationVisible && _dictationStatusHint && !_dictationStatusHint->property("error").toBool()) {
			updateDictationStatusHint();
		}
	});
	connect(_speechService, &AgentSpeechService::speechEventReceived,
		this, &CopilotSidebarWidget::onSpeechEvent);
}

void CopilotSidebarWidget::onSpeechEvent(const SpeechToTextEvent &event) {
	if (!_inputEdit) {
		return;
	}
	auto *wf = static_cast<DictationWaveformWidget *>(_dictationWaveform);
	switch (event.status) {
	case SpeechToTextStatus::Started:
		if (wf) {
			wf->setListening(true, false);
		}
		updateDictationStatusHint();
		break;
	case SpeechToTextStatus::Recognizing:
	case SpeechToTextStatus::Recognized: {
		if (_dictationAutoSendTimer) {
			_dictationAutoSendTimer->stop();
		}
		const DictationTextEditOp op = _dictationTracker.applySpeechEvent(
			event.status,
			event.segmentId,
			event.text,
			_inputEdit->toPlainText());
		if (op.hasEdit) {
			_applyingDictationEdit = true;
			QTextCursor cursor = _inputEdit->textCursor();
			const int currentLen = _inputEdit->toPlainText().size();
			const int start = std::clamp(op.replaceStart, 0, currentLen);
			const int end = std::clamp(op.replaceStart + op.replaceLength, 0, currentLen);
			cursor.setPosition(start);
			cursor.setPosition(end, QTextCursor::KeepAnchor);
			cursor.insertText(op.replacement);
			const int nextLen = _inputEdit->toPlainText().size();
			cursor.setPosition(std::clamp(op.nextCursorPos, 0, nextLen));
			_inputEdit->setTextCursor(cursor);
			_applyingDictationEdit = false;
			_inputEdit->ensureCursorVisible();
		}
		updateInputEditHeight();
		syncActionButton();

		if (event.status == SpeechToTextStatus::Recognizing) {
			if (wf) {
				wf->setListening(true, true);
			}
			updateDictationStatusHint(
				QCoreApplication::translate("MeetingUI", "Transcribing..."),
				false);
		} else {
			if (wf) {
				wf->setListening(true, false);
			}
			updateDictationStatusHint();
			const int timeoutMs = _coordinator ? _coordinator->dictationSpeechTimeoutMs() : 0;
			if (timeoutMs > 0 &&
				_coordinator && !_coordinator->isStreaming() &&
				_speechService && _speechService->isDictationActive() &&
				!_inputEdit->toPlainText().trimmed().isEmpty() &&
				_dictationAutoSendTimer) {
				_dictationAutoSendTimer->start(timeoutMs);
			}
		}
		break;
	}
	case SpeechToTextStatus::Stopped:
		if (_dictationAutoSendTimer) {
			_dictationAutoSendTimer->stop();
		}
		if (wf) {
			wf->setListening(false, false);
		}
		if (!_dictationStatusHint || !_dictationStatusHint->property("error").toBool()) {
			updateDictationStatusHint();
		}
		break;
	case SpeechToTextStatus::Error:
		if (_dictationAutoSendTimer) {
			_dictationAutoSendTimer->stop();
		}
		if (wf) {
			wf->setListening(false, false);
		}
		updateDictationStatusHint(
			event.errorMessage.isEmpty()
				? QCoreApplication::translate("MeetingUI", "Voice dictation error")
				: event.errorMessage,
			true);
		break;
	}
}

void CopilotSidebarWidget::onDictationMicActivated() {
	if (!_dictationMicCombo) {
		return;
	}
	const QString devId = _dictationMicCombo->currentData().toString();
	if (_coordinator) {
		_coordinator->setDictationDeviceId(devId);
	}
	if (_speechService && _speechService->isDictationActive()) {
		_speechService->restartWithDevice(devId);
	}
}

void CopilotSidebarWidget::updateDictationStatusHint(const QString &statusText, bool isError) {
	if (!_dictationStatusHint) {
		return;
	}
	QString resolved = statusText.trimmed();
	bool resolvedError = isError;
	if (resolved.isEmpty()) {
		resolvedError = false;
		if (_dictationInfoShown) {
			resolved = QCoreApplication::translate(
				"MeetingUI",
				"Uses live room captions when unmuted with server transcription; otherwise captures local microphone audio and sends it to your configured speech recognition service (WebSocket streaming ASR or HTTP STT).");
		} else if (_speechService && _speechService->isDictationActive() && _speechService->isUsingRoomPassThrough()) {
			resolved = QCoreApplication::translate(
				"MeetingUI",
				"Using live meeting captions for voice input.");
		}
	}
	_dictationStatusHint->setProperty("error", resolvedError);
	if (auto *st = _dictationStatusHint->style()) {
		st->unpolish(_dictationStatusHint);
		st->polish(_dictationStatusHint);
	}
	if (resolved.isEmpty()) {
		_dictationStatusHint->clear();
		_dictationStatusHint->hide();
	} else {
		_dictationStatusHint->setText(resolved);
		_dictationStatusHint->setVisible(_dictationVisible);
	}
	_dictationStatusHint->updateGeometry();
	if (_dictationCard) {
		_dictationCard->updateGeometry();
	}
	if (_inputPanel) {
		_inputPanel->updateGeometry();
	}
}

void CopilotSidebarWidget::onHistoryViewToggled() {
	setHistoryViewVisible(!_historyVisible);
}

void CopilotSidebarWidget::setHistoryViewVisible(bool visible) {
	if (_historyVisible == visible) {
		return;
	}
	_historyVisible = visible;
	if (_historyVisible) {
		hideCompletionPopup();
		rebuildHistoryList();
	}
	if (_bodyStack) {
		_bodyStack->setCurrentIndex(_historyVisible ? 1 : 0);
	}
	if (_backSessionsBtn) {
		_backSessionsBtn->setProperty("active", _historyVisible);
		if (auto *st = _backSessionsBtn->style()) {
			st->unpolish(_backSessionsBtn);
			st->polish(_backSessionsBtn);
		}
	}
	updateSessionTitleText();
}

void CopilotSidebarWidget::onRenameActiveSession() {
	if (!_coordinator) {
		return;
	}
	const QString activeId = _coordinator->activeSessionId();
	if (activeId.isEmpty()) {
		return;
	}
	QString activeTitle;
	for (const auto &s : _coordinator->sessionSummaries()) {
		if (s.sessionId == activeId) {
			activeTitle = s.title;
			break;
		}
	}
	auto *dlg = new QDialog(window());
	Theme::setTone(*dlg, resolvedCurrentTone(this));
	Theme::setStyleVariant(*dlg, "meeting-chat-sidebar-widget-dlg");
	dlg->setWindowTitle(QCoreApplication::translate("MeetingUI", "Rename Conversation"));
	dlg->resize(340, 130);

	auto *lay = new QVBoxLayout(dlg);
	lay->setContentsMargins(16, 16, 16, 16);
	lay->setSpacing(10);

	auto *edit = new QLineEdit(activeTitle, dlg);
	edit->selectAll();
	lay->addWidget(edit);

	auto *btns = new QHBoxLayout();
	btns->addStretch(1);
	auto *cancelBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Cancel"), dlg);
	cancelBtn->setAutoDefault(false);
	auto *saveBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Save"), dlg);
	saveBtn->setDefault(true);
	btns->addWidget(cancelBtn);
	btns->addWidget(saveBtn);
	lay->addLayout(btns);

	connect(cancelBtn, &QPushButton::clicked, dlg, &QDialog::reject);
	connect(saveBtn, &QPushButton::clicked, dlg, &QDialog::accept);
	Theme::makeDialogAdaptive(*dlg, dlg->size());
	if (dlg->exec() == QDialog::Accepted) {
		_coordinator->renameSession(activeId, edit->text());
	}
	dlg->deleteLater();
}

void CopilotSidebarWidget::onDeleteActiveSession() {
	if (!_coordinator) {
		return;
	}
	const QString activeId = _coordinator->activeSessionId();
	if (activeId.isEmpty()) {
		return;
	}
	cancelEditingMessage();
	_coordinator->deleteSession(activeId);
}

void CopilotSidebarWidget::updateChoiceCombosTheme() {
	const auto tone = resolvedCurrentTone(this);
	if (_dictationMicCombo) {
		Theme::styleChoiceControls(*_dictationMicCombo, tone);
	}
}

void CopilotSidebarWidget::updateInputEditHeight() {
	if (!_inputEdit || !_inputEdit->document()) {
		return;
	}
	const qreal visualLines = std::max<qreal>(1.0, _inputEdit->document()->size().height());
	const int lineH = std::max(14, _inputEdit->fontMetrics().lineSpacing());
	const int targetH = std::clamp(
		static_cast<int>(std::ceil(visualLines * lineH)) + 14,
		kMinInputHeight,
		kMaxInputHeight);
	if (_inputEdit->height() != targetH) {
		_inputEdit->setFixedHeight(targetH);
	}
}

void CopilotSidebarWidget::updateScrollToBottomButton() {
	if (!_scrollArea || !_scrollToBottomBtn) {
		return;
	}
	const QScrollBar *bar = _scrollArea->verticalScrollBar();
	const bool show = bar && (bar->maximum() > 0) && !isNearBottom();
	if (show) {
		const int right = _scrollArea->width() - 14;
		const int bottom = _scrollArea->height() - 12;
		_scrollToBottomBtn->move(
			std::max(8, right - _scrollToBottomBtn->width()),
			std::max(8, bottom - _scrollToBottomBtn->height()));
		_scrollToBottomBtn->show();
		_scrollToBottomBtn->raise();
	} else {
		_scrollToBottomBtn->hide();
	}
}

void CopilotSidebarWidget::onAddContextClicked() {
	if (!_coordinator || !_coordinator->isConfigured() || !_addContextBtn) {
		return;
	}
	QMenu menu(this);
	Theme::styleMenu(menu, resolvedCurrentTone(this));

	for (const auto &desc : kContextChipDescriptors) {
		const bool alreadySelected = std::any_of(
			_pendingContextChips.cbegin(),
			_pendingContextChips.cend(),
			[&desc](const AgentContextChip &chip) { return chip.kind == desc.kind; });
		QAction *act = menu.addAction(
			QStringLiteral("%1 — %2").arg(
				QCoreApplication::translate("MeetingUI", desc.displayLabel),
				QCoreApplication::translate("MeetingUI", desc.description)));
		act->setCheckable(true);
		act->setChecked(alreadySelected);
		const AgentContextChipKind kind = desc.kind;
		connect(act, &QAction::triggered, this, [this, kind, alreadySelected] {
			if (alreadySelected) {
				removePendingContextChip(kind);
			} else {
				addPendingContextChip(kind);
			}
		});
	}
	menu.addSeparator();
	QAction *imgAct = menu.addAction(QCoreApplication::translate("MeetingUI", "Attach image..."));
	imgAct->setEnabled(_pendingImages.size() < kMaxAgentImagesPerMessage);
	connect(imgAct, &QAction::triggered, this, &CopilotSidebarWidget::onAttachImageClicked);

	menu.exec(_addContextBtn->mapToGlobal(QPoint(0, -menu.sizeHint().height())));
}

void CopilotSidebarWidget::addPendingContextChip(AgentContextChipKind kind) {
	for (const auto &chip : _pendingContextChips) {
		if (chip.kind == kind) {
			return;
		}
	}
	_pendingContextChips.push_back(AgentContextChip{ kind, localizedChipLabel(kind) });
	syncHeaderAndState();
	syncActionButton();
}

void CopilotSidebarWidget::removePendingContextChip(AgentContextChipKind kind) {
	const auto prevSize = _pendingContextChips.size();
	_pendingContextChips.erase(
		std::remove_if(_pendingContextChips.begin(), _pendingContextChips.end(),
			[kind](const AgentContextChip &chip) { return chip.kind == kind; }),
		_pendingContextChips.end());
	if (_pendingContextChips.size() != prevSize) {
		syncHeaderAndState();
		syncActionButton();
	}
}

void CopilotSidebarWidget::clearPendingContextChips() {
	if (_pendingContextChips.isEmpty()) {
		return;
	}
	_pendingContextChips.clear();
	rebuildContextChipsStrip();
}

void CopilotSidebarWidget::rebuildContextChipsStrip() {
	if (!_contextChipsStrip || !_contextChipsRowsLayout) {
		return;
	}
	while (QLayoutItem *item = _contextChipsRowsLayout->takeAt(0)) {
		if (QLayout *rowLayout = item->layout()) {
			while (QLayoutItem *child = rowLayout->takeAt(0)) {
				if (QWidget *w = child->widget()) {
					w->deleteLater();
				}
				delete child;
			}
		}
		if (QWidget *w = item->widget()) {
			w->deleteLater();
		}
		delete item;
	}

	if (_pendingContextChips.isEmpty()) {
		_contextChipsStrip->hide();
		return;
	}

	constexpr int kChipsPerRow = 2;
	QHBoxLayout *currentRow = nullptr;
	for (int i = 0; i < _pendingContextChips.size(); ++i) {
		if (i % kChipsPerRow == 0) {
			if (currentRow) {
				currentRow->addStretch(1);
			}
			currentRow = new QHBoxLayout();
			currentRow->setContentsMargins(0, 0, 0, 0);
			currentRow->setSpacing(6);
			_contextChipsRowsLayout->addLayout(currentRow);
		}
		const AgentContextChip chip = _pendingContextChips.at(i);
		auto *btn = new QPushButton(
			QStringLiteral("%1  ✕").arg(chip.label),
			_contextChipsStrip);
		btn->setObjectName(QStringLiteral("CopilotContextChipBtn"));
		btn->setAccessibleName(
			QCoreApplication::translate("MeetingUI", "Remove context chip %1").arg(chip.label));
		btn->setCursor(Qt::PointingHandCursor);
		btn->setAutoDefault(false);
		connect(btn, &QPushButton::clicked, this, [this, kind = chip.kind] {
			removePendingContextChip(kind);
		});
		currentRow->addWidget(btn);
	}
	if (currentRow) {
		currentRow->addStretch(1);
	}
	_contextChipsStrip->show();
}

void CopilotSidebarWidget::updateCompletionPopup() {
	if (!_completionPopup || !_completionList || !_inputEdit || !_inputEdit->hasFocus()) {
		hideCompletionPopup();
		return;
	}
	const QString fullText = _inputEdit->toPlainText();
	const int cursorPos = std::clamp(_inputEdit->textCursor().position(), 0, static_cast<int>(fullText.size()));
	const QString before = fullText.left(cursorPos);

	_completionList->clear();
	if (before.startsWith(QLatin1Char('/')) && !before.contains(QLatin1Char(' ')) &&
		!before.contains(QLatin1Char('\n'))) {
		const QString prefix = before.toLower();
		for (const auto &desc : kSlashCommandDescriptors) {
			const QString cmd = QLatin1String(desc.command);
			if (!cmd.startsWith(prefix)) {
				continue;
			}
			auto *item = new QListWidgetItem(
				QStringLiteral("%1  —  %2").arg(
					cmd,
					QCoreApplication::translate("MeetingUI", desc.description)));
			item->setData(kCompletionKindRole, QStringLiteral("slash"));
			item->setData(kCompletionValueRole, cmd);
			item->setData(kCompletionTokenLenRole, before.size());
			_completionList->addItem(item);
		}
	} else {
		int start = cursorPos - 1;
		while (start >= 0 && !before.at(start).isSpace()) {
			--start;
		}
		++start;
		const QString token = before.mid(start);
		if (token.startsWith(QLatin1Char('#'))) {
			const QString prefix = token.toLower();
			for (const auto &desc : kContextChipDescriptors) {
				const QString tag = QLatin1String(desc.hashTag);
				if (!tag.startsWith(prefix)) {
					continue;
				}
				auto *item = new QListWidgetItem(
					QStringLiteral("%1  —  %2").arg(
						tag,
						QCoreApplication::translate("MeetingUI", desc.description)));
				item->setData(kCompletionKindRole, QStringLiteral("chip"));
				item->setData(kCompletionValueRole, static_cast<int>(desc.kind));
				item->setData(kCompletionTokenLenRole, token.size());
				_completionList->addItem(item);
			}
		}
	}

	const int count = _completionList->count();
	if (count <= 0) {
		hideCompletionPopup();
		return;
	}
	_completionList->setCurrentRow(0);
	const int rowH = std::max(26, _completionList->sizeHintForRow(0));
	const int popupH = std::min(196, rowH * count + 12);
	const int popupW = std::max(200, width() - 20);
	const int px = 10;
	const int composerTopY = _composerCard ? _composerCard->mapTo(this, QPoint(0, 0)).y() : _inputPanel->y();
	const int py = std::max(8, composerTopY - popupH - 4);
	_completionPopup->setGeometry(px, py, popupW, popupH);
	_completionPopup->show();
	_completionPopup->raise();
}

void CopilotSidebarWidget::hideCompletionPopup() {
	if (_completionPopup) {
		_completionPopup->hide();
	}
}

void CopilotSidebarWidget::applyCompletionItem(QListWidgetItem *item) {
	if (!item || !_inputEdit) {
		hideCompletionPopup();
		return;
	}
	const QString kind = item->data(kCompletionKindRole).toString();
	const int tokenLen = item->data(kCompletionTokenLenRole).toInt();

	if (kind == QStringLiteral("slash")) {
		const QString cmd = item->data(kCompletionValueRole).toString();
		hideCompletionPopup();
		for (const auto &desc : kSlashCommandDescriptors) {
			if (cmd != QLatin1String(desc.command)) {
				continue;
			}
			if (cmd == QStringLiteral("/clear")) {
				_inputEdit->clear();
				onClearClicked();
				return;
			}
			if (cmd == QStringLiteral("/ask")) {
				_inputEdit->clear();
				if (_coordinator) {
					_coordinator->setChatMode(AgentChatMode::Ask);
				}
				return;
			}
			if (cmd == QStringLiteral("/agent")) {
				_inputEdit->clear();
				if (_coordinator) {
					_coordinator->setChatMode(AgentChatMode::Agent);
				}
				return;
			}
			if (desc.attachMeetingChat) {
				addPendingContextChip(AgentContextChipKind::MeetingChat);
			}
			if (desc.attachTranscript) {
				addPendingContextChip(AgentContextChipKind::Transcript);
			}
			if (desc.attachTelemetry) {
				addPendingContextChip(AgentContextChipKind::Telemetry);
			}
			if (desc.attachSchedule) {
				addPendingContextChip(AgentContextChipKind::Schedule);
			}
			_inputEdit->setPlainText(QCoreApplication::translate("MeetingUI", desc.promptTemplate));
			QTextCursor c = _inputEdit->textCursor();
			c.movePosition(QTextCursor::End);
			_inputEdit->setTextCursor(c);
			_inputEdit->setFocus();
			return;
		}
		return;
	}

	if (kind == QStringLiteral("chip")) {
		const auto chipKind = static_cast<AgentContextChipKind>(item->data(kCompletionValueRole).toInt());
		QTextCursor c = _inputEdit->textCursor();
		if (tokenLen > 0) {
			c.movePosition(QTextCursor::Left, QTextCursor::KeepAnchor, tokenLen);
			c.removeSelectedText();
			_inputEdit->setTextCursor(c);
		}
		hideCompletionPopup();
		addPendingContextChip(chipKind);
		_inputEdit->setFocus();
	}
}

void CopilotSidebarWidget::handleInternalActionUrl(const QUrl &url) {
	if (!url.isValid() || url.scheme().compare(QStringLiteral("cohavora"), Qt::CaseInsensitive) != 0) {
		return;
	}
	const QString host = url.host().toLower();
	if (host == QStringLiteral("retry")) {
		if (_coordinator && _coordinator->canRetryLastTurn()) {
			clearRequestError();
			_coordinator->retryLastTurn();
		}
		return;
	}
	if (host == QStringLiteral("dictation")) {
		_dictationInfoShown = !_dictationInfoShown;
		updateDictationStatusHint();
		return;
	}
	if (host == QStringLiteral("settings")) {
		const QString path = url.path().toLower();
		int pageIdx = 4; // Copilot
		int subTabIdx = -1;
		if (path == QStringLiteral("/general")) {
			pageIdx = 0;
		} else if (path == QStringLiteral("/video")) {
			pageIdx = 1;
		} else if (path == QStringLiteral("/audio")) {
			pageIdx = 2;
		} else if (path == QStringLiteral("/security")) {
			pageIdx = 3;
		} else if (path == QStringLiteral("/copilot/asr") ||
		           path == QStringLiteral("/copilot/dictation") ||
		           path == QStringLiteral("/copilot/stt")) {
			pageIdx = 4;
			subTabIdx = 1;
		} else if (path == QStringLiteral("/copilot/translation") ||
		           path == QStringLiteral("/copilot/agent")) {
			pageIdx = 4;
			subTabIdx = 2;
		} else if (path == QStringLiteral("/copilot") ||
		           path == QStringLiteral("/copilot/chat")) {
			pageIdx = 4;
			subTabIdx = 0;
		} else if (path == QStringLiteral("/about")) {
			pageIdx = 5;
		}
		emit openSettingsPageRequested(pageIdx, subTabIdx);
	}
}

void CopilotSidebarWidget::onNewChatClicked() {
	if (!_coordinator) {
		return;
	}
	cancelEditingMessage();
	clearRequestError();
	hideCompletionPopup();
	setHistoryViewVisible(false);
	_coordinator->createNewSession();
	focusInput();
}

void CopilotSidebarWidget::onExportConversationClicked() {
	if (!_coordinator || _coordinator->messages().isEmpty()) {
		return;
	}
	const QString markdown = _coordinator->exportSessionMarkdown();
	if (markdown.trimmed().isEmpty()) {
		return;
	}
	const QString savePath = QFileDialog::getSaveFileName(
		this,
		QCoreApplication::translate("MeetingUI", "Export Copilot Conversation"),
		QStringLiteral("copilot-chat.md"),
		QCoreApplication::translate("MeetingUI", "Markdown (*.md);;Text Files (*.txt);;All Files (*.*)"),
		nullptr,
		QFileDialog::DontUseNativeDialog);
	if (savePath.isEmpty()) {
		return;
	}
	QFile outFile(savePath);
	if (!outFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
		showRequestError(QCoreApplication::translate("MeetingUI", "Failed to save exported conversation."));
		return;
	}
	outFile.write(markdown.toUtf8());
}

void CopilotSidebarWidget::beginEditingMessage(
		const QString &messageId,
		const QString &content,
		const QVector<AgentContextChip> &chips) {
	if (!_coordinator || _coordinator->isStreaming() || messageId.isEmpty()) {
		return;
	}
	if (_historyVisible) {
		setHistoryViewVisible(false);
	}
	_editingMessageId = messageId;
	if (_editingBanner) {
		_editingBanner->show();
	}
	clearPendingImages();
	_pendingContextChips = chips;
	rebuildContextChipsStrip();
	if (_inputEdit) {
		_inputEdit->setPlainText(content);
		QTextCursor c = _inputEdit->textCursor();
		c.movePosition(QTextCursor::End);
		_inputEdit->setTextCursor(c);
		_inputEdit->setFocus();
	}
	syncActionButton();
}

void CopilotSidebarWidget::cancelEditingMessage() {
	_editingMessageId.clear();
	if (_editingBanner) {
		_editingBanner->hide();
	}
}

void CopilotSidebarWidget::syncLeftEdgeShadow() {
	QWidget *host = parentWidget();
	if (!_leftEdgeShadowEnabled || !host) {
		if (_leftEdgeShadow) {
			_leftEdgeShadow->hide();
		}
		return;
	}
	if (!_leftEdgeShadow) {
		_leftEdgeShadow = new LeftEdgeShadowOverlay(host);
	} else if (_leftEdgeShadow->parentWidget() != host) {
		_leftEdgeShadow->setParent(host);
	}
	if (!isVisible() || width() <= 0 || height() <= 0) {
		_leftEdgeShadow->hide();
		return;
	}
	const int shadowW = std::min(kLeftEdgeShadowWidth, std::max(0, x()));
	if (shadowW <= 0) {
		_leftEdgeShadow->hide();
		return;
	}
	_leftEdgeShadow->setGeometry(x() - shadowW, y(), shadowW, height());
	_leftEdgeShadow->show();
	_leftEdgeShadow->raise();
	_leftEdgeShadow->update();
}

void CopilotSidebarWidget::changeEvent(QEvent *event) {
	QWidget::changeEvent(event);
	if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
		updateChoiceCombosTheme();
		if (_leftEdgeShadow) {
			_leftEdgeShadow->update();
		}
	} else if (event->type() == QEvent::ParentChange) {
		syncLeftEdgeShadow();
	} else if (event->type() == QEvent::LanguageChange) {
		retranslateUi();
	}
}

void CopilotSidebarWidget::retranslateUi() {
	if (_chatTabLabel) {
		_chatTabLabel->setText(QCoreApplication::translate("MeetingUI", "Chat"));
	}
	if (_moreActionsBtn) {
		_moreActionsBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "More Copilot actions"));
		_moreActionsBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Clear, export, or open settings"));
	}
	if (_newChatBtn) {
		_newChatBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "New chat"));
		_newChatBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Start a new Copilot conversation"));
	}
	if (_settingsBtn) {
		_settingsBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Open Copilot settings"));
		_settingsBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Open Copilot settings"));
	}
	if (_expandBtn) {
		const QString tip = _expanded
			? QCoreApplication::translate("MeetingUI", "Restore default panel width")
			: QCoreApplication::translate("MeetingUI", "Expand panel width");
		_expandBtn->setAccessibleName(tip);
		_expandBtn->setToolTip(tip);
	}
	if (_closeBtn) {
		_closeBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Close Copilot"));
		_closeBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Close Copilot"));
	}
	if (_backSessionsBtn) {
		_backSessionsBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Toggle conversation history"));
		_backSessionsBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Browse saved conversations"));
	}
	if (_sessionsBtn) {
		_sessionsBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Chat history and actions"));
		_sessionsBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Switch saved conversations"));
	}
	if (_e2eeBannerLabel) {
		_e2eeBannerLabel->setText(QCoreApplication::translate("MeetingUI",
			"Encrypted meeting: context is withheld until allowed."));
	}
	if (_e2eeAuthorizeBtn) {
		_e2eeAuthorizeBtn->setText(QCoreApplication::translate("MeetingUI", "Allow"));
	}
	if (_unconfiguredTitle) {
		_unconfiguredTitle->setText(QCoreApplication::translate("MeetingUI", "Connect your Agent API"));
	}
	if (_unconfiguredBody) {
		_unconfiguredBody->setText(QCoreApplication::translate("MeetingUI",
			"Configure an OpenAI-compatible API, custom REST webhook, or local model server to chat with AI Copilot."));
	}
	if (_configureBtn) {
		const QString text = QCoreApplication::translate("MeetingUI", "Configure Copilot");
		_configureBtn->setText(text);
		_configureBtn->setAccessibleName(text);
	}
	if (_emptyLabel) {
		_emptyLabel->setText(QCoreApplication::translate("MeetingUI",
			"Ask AI Copilot to summarize notes, draft agendas, or answer technical questions."));
	}
	if (_scrollToBottomBtn) {
		_scrollToBottomBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Jump to latest message"));
		_scrollToBottomBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Jump to bottom"));
	}
	if (_historyNewBtn) {
		_historyNewBtn->setText(QCoreApplication::translate("MeetingUI", "+ New Chat"));
	}
	if (_historyRenameBtn) {
		_historyRenameBtn->setText(QCoreApplication::translate("MeetingUI", "Rename..."));
	}
	if (_historyDeleteBtn) {
		_historyDeleteBtn->setText(QCoreApplication::translate("MeetingUI", "Delete"));
	}
	if (_historyExportBtn) {
		_historyExportBtn->setText(QCoreApplication::translate("MeetingUI", "Export .md"));
	}
	if (_inputEdit) {
		_inputEdit->setAccessibleName(QCoreApplication::translate("MeetingUI", "Copilot message input"));
		_inputEdit->setPlaceholderText(QCoreApplication::translate("MeetingUI",
			"Chat with Copilot (/ commands, # context)"));
	}
	if (_addContextBtn) {
		_addContextBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Attach context"));
		_addContextBtn->setToolTip(QCoreApplication::translate("MeetingUI",
			"Attach meeting chat, telemetry, participants, schedule, or images (or type #)"));
	}
	if (_modelSwitchBtn) {
		_modelSwitchBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Switch AI model and reasoning"));
	}
	if (_dictationBtn) {
		_dictationBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Toggle voice dictation"));
		_dictationBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Voice dictation (speak into message box)"));
	}
	if (_modePermBtn) {
		_modePermBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Copilot mode and tool permissions"));
	}
	if (_statusIndicatorBtn) {
		_statusIndicatorBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Copilot connection status"));
	}
	syncHeaderAndState();
	syncActionButton();
	rebuildHistoryList();
	rebuildMessages();
	rebuildContextChipsStrip();
}

void CopilotSidebarWidget::showEvent(QShowEvent *event) {
	QWidget::showEvent(event);
	updateChoiceCombosTheme();
	updateSessionTitleText();
	updateModelSwitchLabel();
	updateModePermLabel();
	updateScrollToBottomButton();
	syncLeftEdgeShadow();
}

void CopilotSidebarWidget::hideEvent(QHideEvent *event) {
	QWidget::hideEvent(event);
	if (_dictationVisible) {
		setDictationVisible(false);
	}
	if (_leftEdgeShadow) {
		_leftEdgeShadow->hide();
	}
}

void CopilotSidebarWidget::moveEvent(QMoveEvent *event) {
	QWidget::moveEvent(event);
	syncLeftEdgeShadow();
}

void CopilotSidebarWidget::resizeEvent(QResizeEvent *event) {
	QWidget::resizeEvent(event);
	updateSessionTitleText();
	updateModelSwitchLabel();
	updateModePermLabel();
	updateInputEditHeight();
	updateScrollToBottomButton();
	if (_completionPopup && _completionPopup->isVisible()) {
		updateCompletionPopup();
	}
	syncLeftEdgeShadow();
}

bool CopilotSidebarWidget::eventFilter(QObject *watched, QEvent *event) {
	if (_scrollArea && watched == _scrollArea->viewport() && event->type() == QEvent::Resize) {
		updateScrollToBottomButton();
	}

	if (watched == _inputEdit && event->type() == QEvent::FocusIn) {
		if (_composerCard) {
			_composerCard->setProperty("focused", true);
			if (auto *st = _composerCard->style()) {
				st->unpolish(_composerCard);
				st->polish(_composerCard);
			}
		}
	}

	if (watched == _inputEdit && event->type() == QEvent::FocusOut) {
		if (_composerCard) {
			_composerCard->setProperty("focused", false);
			if (auto *st = _composerCard->style()) {
				st->unpolish(_composerCard);
				st->polish(_composerCard);
			}
		}
		QTimer::singleShot(150, this, [this] {
			if (_inputEdit && !_inputEdit->hasFocus() &&
				_completionPopup && !_completionPopup->isAncestorOf(QApplication::focusWidget())) {
				hideCompletionPopup();
			}
		});
	}

	if (watched == _inputEdit && event->type() == QEvent::KeyPress) {
		const auto *keyEvent = static_cast<QKeyEvent *>(event);
		if (_completionPopup && _completionPopup->isVisible() && _completionList) {
			const int rowCount = _completionList->count();
			if (keyEvent->key() == Qt::Key_Down && rowCount > 0) {
				const int next = std::min(rowCount - 1, _completionList->currentRow() + 1);
				_completionList->setCurrentRow(next);
				return true;
			}
			if (keyEvent->key() == Qt::Key_Up && rowCount > 0) {
				const int prev = std::max(0, _completionList->currentRow() - 1);
				_completionList->setCurrentRow(prev);
				return true;
			}
			if (keyEvent->key() == Qt::Key_Escape) {
				hideCompletionPopup();
				return true;
			}
			if (keyEvent->key() == Qt::Key_Tab ||
				((keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) &&
				 !(keyEvent->modifiers() & Qt::ShiftModifier))) {
				if (QListWidgetItem *cur = _completionList->currentItem()) {
					applyCompletionItem(cur);
					return true;
				}
			}
		}

		if (keyEvent->matches(QKeySequence::Paste)) {
			if (!_coordinator || !_coordinator->isConfigured()) {
				return QWidget::eventFilter(watched, event);
			}
			const QMimeData *mime = QGuiApplication::clipboard()
				? QGuiApplication::clipboard()->mimeData()
				: nullptr;
			if (mime && mime->hasImage()) {
				const QImage clipImg = qvariant_cast<QImage>(mime->imageData());
				if (!clipImg.isNull()) {
					addPendingImageFromQImage(clipImg, QStringLiteral("clipboard.png"));
					syncHeaderAndState();
					return true;
				}
			}
			if (mime && mime->hasUrls() && hasSupportedImageUrls(mime->urls())) {
				for (const QUrl &url : mime->urls()) {
					if (url.isLocalFile() && isSupportedImageFilePath(url.toLocalFile())) {
						addPendingImageFromFile(url.toLocalFile());
					}
				}
				syncHeaderAndState();
				return true;
			}
		}
		if ((keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) &&
			!(keyEvent->modifiers() & Qt::ShiftModifier)) {
			onSendOrStopClicked();
			return true;
		}
		if (keyEvent->key() == Qt::Key_Escape && _dictationVisible) {
			setDictationVisible(false);
			return true;
		}
	}

	if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
		const auto *dragEvent = static_cast<const QDragMoveEvent *>(event);
		const QMimeData *mime = dragEvent->mimeData();
		if (_coordinator && _coordinator->isConfigured() && mime &&
			(mime->hasImage() || (mime->hasUrls() && hasSupportedImageUrls(mime->urls())))) {
			static_cast<QDropEvent *>(event)->acceptProposedAction();
			return true;
		}
	} else if (event->type() == QEvent::Drop) {
		auto *dropEvent = static_cast<QDropEvent *>(event);
		const QMimeData *mime = dropEvent->mimeData();
		if (_coordinator && _coordinator->isConfigured() && mime) {
			bool handled = false;
			if (mime->hasImage()) {
				const QImage img = qvariant_cast<QImage>(mime->imageData());
				if (!img.isNull()) {
					addPendingImageFromQImage(img, QStringLiteral("dropped.png"));
					handled = true;
				}
			}
			if (!handled && mime->hasUrls() && hasSupportedImageUrls(mime->urls())) {
				for (const QUrl &url : mime->urls()) {
					if (url.isLocalFile() && isSupportedImageFilePath(url.toLocalFile())) {
						addPendingImageFromFile(url.toLocalFile());
						handled = true;
					}
				}
			}
			if (handled) {
				dropEvent->acceptProposedAction();
				syncHeaderAndState();
				return true;
			}
		}
	}

	return QWidget::eventFilter(watched, event);
}

} // namespace OpenMeeting
