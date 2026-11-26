#include "src/ui/copilot_settings_page.h"
#include "src/ui/detail/settings_ui_helpers.h"
#include "src/ui/whiteboard/accessible_combo_box.h"
#include "src/ui/app_icons.h"
#include "src/ui/app_theme.h"
#include "src/agent/agent_coordinator.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QPointer>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtCore/QVariant>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QStyle>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <array>
#include <utility>

namespace MeetingUI {
namespace {

QString keyEndpointMismatchWarningText(
		OpenMeeting::AgentKeyEndpointMismatch mismatch,
		bool forSpeech) {
	if (mismatch == OpenMeeting::AgentKeyEndpointMismatch::BailianPlanKeyOnStandardHost) {
		return forSpeech
			? QCoreApplication::translate("MeetingUI",
				"Warning: Bailian Token/Coding Plan key (sk-sp-...) is not valid for standard DashScope speech endpoints; use a standard sk-... key.")
			: QCoreApplication::translate("MeetingUI",
				"Warning: Bailian Token/Coding Plan key (sk-sp-...) requires a plan endpoint (e.g. coding.dashscope.aliyuncs.com or token-plan.*.maas.aliyuncs.com), not a standard DashScope URL.");
	}
	if (mismatch == OpenMeeting::AgentKeyEndpointMismatch::StandardKeyOnBailianPlanHost) {
		return forSpeech
			? QCoreApplication::translate("MeetingUI",
				"Warning: Standard sk-... key does not match a Bailian Token/Coding Plan host, and speech ASR models generally require the standard DashScope endpoint (dashscope.aliyuncs.com).")
			: QCoreApplication::translate("MeetingUI",
				"Warning: Standard sk-... key does not match a Bailian Token/Coding Plan endpoint (use an sk-sp-... plan key or switch to the standard DashScope URL).");
	}
	if (mismatch == OpenMeeting::AgentKeyEndpointMismatch::GroqKeyOnNonGroqHost) {
		return forSpeech
			? QCoreApplication::translate("MeetingUI",
				"Warning: Detected a Groq API key (gsk_...) against a non-Groq STT endpoint. Use a matching provider key (e.g. sk-... for SiliconFlow/OpenAI/DashScope) or set the STT URL to https://api.groq.com/openai/v1.")
			: QCoreApplication::translate("MeetingUI",
				"Warning: Detected a Groq API key (gsk_...) against a non-Groq endpoint. Use a matching provider key or switch the Base URL to https://api.groq.com/openai/v1.");
	}
	return {};
}

QString defaultCopilotModelHint() {
	return QCoreApplication::translate("MeetingUI",
		"Click Refresh Models to fetch available chat models from the endpoint, or enter a model name manually.");
}

} // namespace

CopilotSettingsPage::CopilotSettingsPage(
		OpenMeeting::SessionManager &session,
		QWidget *parent)
	: QWidget(parent)
	, _session(session) {
	buildUi();
}

CopilotSettingsPage::~CopilotSettingsPage() {
	shutdown();
}

void CopilotSettingsPage::clearKeyEditors() {
	clearCopilotKeyEditor();
	wipeAndClearLineEdit(_copilotSttApiKey);
}

void CopilotSettingsPage::flushBeforeClose() {
	if (_translationPromptCommitTimer && _translationPromptCommitTimer->isActive()) {
		_translationPromptCommitTimer->stop();
		emit preferencesChanged();
	}
	commitCopilotProfile(true);
	clearKeyEditors();
}

void CopilotSettingsPage::shutdown() {
	disconnectCopilotCoordinatorSignals();
	clearKeyEditors();
}

void CopilotSettingsPage::refreshThemeVisuals() {
	if (_copilotNewProfileBtn) {
		_copilotNewProfileBtn->setIcon(AppTheme::icon(AppTheme::Icon::Plus, AppTheme::ColorRole::Text));
	}
	if (_copilotProfileMoreBtn) {
		_copilotProfileMoreBtn->setIcon(AppTheme::icon(AppTheme::Icon::MoreHorizontal, AppTheme::ColorRole::Text));
	}
	if (_copilotSttNewProfileBtn) {
		_copilotSttNewProfileBtn->setIcon(AppTheme::icon(AppTheme::Icon::Plus, AppTheme::ColorRole::Text));
	}
	if (_copilotSttProfileMoreBtn) {
		_copilotSttProfileMoreBtn->setIcon(AppTheme::icon(AppTheme::Icon::MoreHorizontal, AppTheme::ColorRole::Text));
	}
	for (auto *sec : {_copilotChatAdvSection, _copilotSttAdvSection, _copilotTrAdvSection}) {
		if (sec) {
			sec->refreshIcons();
		}
	}
}

void CopilotSettingsPage::connectTranslationControls() {
	for (auto *checkBox : {
		_translationEnableLocal,
		_translationPreferRoomAgent,
		_translationEnableOutput,
		_translationIncludeLocalMic }) {
		if (!checkBox) continue;
		connect(checkBox, &QCheckBox::toggled, this, [this, checkBox](bool) {
			if (checkBox == _translationEnableOutput) {
				updateCopilotTranslationLinkedUi();
			}
			if (!_updatingUi) {
				emit preferencesChanged();
			}
		});
	}
	for (auto *combo : {
		_translationRoleModeCombo,
		_captionDisplayModeCombo,
		_translationProfileCombo }) {
		if (!combo) continue;
		connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, combo](int) {
			if (combo == _translationProfileCombo) {
				updateCopilotAdvSummaries();
			}
			if (!_updatingUi) {
				emit preferencesChanged();
			}
		});
	}
	if (_translationTargetLangCombo) {
		connect(_translationTargetLangCombo, QOverload<int>::of(&QComboBox::activated),
			this, [this](int) {
				if (!_updatingUi) {
					emit preferencesChanged();
				}
			});
		if (auto *trLangEdit = _translationTargetLangCombo->lineEdit()) {
			connect(trLangEdit, &QLineEdit::editingFinished, this, [this] {
				if (!_updatingUi) {
					emit preferencesChanged();
				}
			});
		}
	}
	if (_translationModelEdit) {
		connect(_translationModelEdit, &QLineEdit::textChanged, this, [this](const QString &) {
			updateCopilotAdvSummaries();
		});
		connect(_translationModelEdit, &QLineEdit::editingFinished, this, [this] {
			if (!_updatingUi) {
				emit preferencesChanged();
			}
		});
	}
	if (_translationPromptEdit && _translationPromptCommitTimer) {
		connect(_translationPromptEdit, &QPlainTextEdit::textChanged, this, [this] {
			updateCopilotAdvSummaries();
			if (!_updatingUi && _translationPromptCommitTimer) {
				_translationPromptCommitTimer->start();
			}
		});
		connect(_translationPromptCommitTimer, &QTimer::timeout, this, [this] {
			if (!_updatingUi) {
				emit preferencesChanged();
			}
		});
	}
	if (_translationMaxConcurrentStreamsSpin) {
		connect(_translationMaxConcurrentStreamsSpin,
			static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
			this, [this](int) {
				updateCopilotAdvSummaries();
				if (!_updatingUi) {
					emit preferencesChanged();
				}
			});
	}
	if (_translationIdleReleaseSpin) {
		connect(_translationIdleReleaseSpin,
			static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
			this, [this](int) {
				updateCopilotAdvSummaries();
				if (!_updatingUi) {
					emit preferencesChanged();
				}
			});
	}
}

void CopilotSettingsPage::readTranslationPreferences(OpenMeeting::MediaPreferences &value) const {
	value.enableLocalMeetingTranslation = _translationEnableLocal && _translationEnableLocal->isChecked();
	if (_translationRoleModeCombo) {
		value.meetingTranslationRoleMode = static_cast<OpenMeeting::MeetingTranslationRoleMode>(
			_translationRoleModeCombo->currentData().toInt());
	}
	value.preferRoomAgentTranscript = !_translationPreferRoomAgent || _translationPreferRoomAgent->isChecked();
	value.enableTranslationOutput = _translationEnableOutput && _translationEnableOutput->isChecked();
	if (_translationTargetLangCombo) {
		QString lang = _translationTargetLangCombo->currentText().trimmed();
		const int langIdx = _translationTargetLangCombo->currentIndex();
		if (langIdx >= 0 && lang == _translationTargetLangCombo->itemText(langIdx).trimmed()) {
			lang = _translationTargetLangCombo->itemData(langIdx).toString().trimmed();
		}
		lang = lang.left(64);
		value.translationTargetLanguage = lang.isEmpty() ? QStringLiteral("zh") : lang;
	}
	if (_captionDisplayModeCombo) {
		value.liveCaptionDisplayMode = static_cast<OpenMeeting::LiveCaptionDisplayMode>(
			_captionDisplayModeCombo->currentData().toInt());
	}
	if (_translationProfileCombo) {
		value.translationProfileId = _translationProfileCombo->currentData().toString().trimmed();
	}
	if (_translationModelEdit) {
		value.translationModelOverride = _translationModelEdit->text().trimmed().left(256);
	}
	if (_translationPromptEdit) {
		value.translationSystemPrompt = _translationPromptEdit->toPlainText().trimmed().left(4000);
	}
	value.translateLocalMicrophone = !_translationIncludeLocalMic || _translationIncludeLocalMic->isChecked();
	if (_translationMaxConcurrentStreamsSpin) {
		value.maxConcurrentStreamingAsr = std::clamp(_translationMaxConcurrentStreamsSpin->value(), 1, 8);
	}
	if (_translationIdleReleaseSpin) {
		value.streamingIdleReleaseMs = std::clamp(_translationIdleReleaseSpin->value() * 1000, 3000, 120000);
	}
}

void CopilotSettingsPage::applyTranslationPreferences(const OpenMeeting::MediaPreferences &value) {
	const auto oldUpdating = _updatingUi;
	_updatingUi = true;
	if (_translationEnableLocal) {
		_translationEnableLocal->setChecked(value.enableLocalMeetingTranslation);
	}
	if (_translationRoleModeCombo) {
		const int idx = _translationRoleModeCombo->findData(static_cast<int>(value.meetingTranslationRoleMode));
		_translationRoleModeCombo->setCurrentIndex(idx >= 0 ? idx : 0);
	}
	if (_translationPreferRoomAgent) {
		_translationPreferRoomAgent->setChecked(value.preferRoomAgentTranscript);
	}
	if (_translationEnableOutput) {
		_translationEnableOutput->setChecked(value.enableTranslationOutput);
	}
	if (_translationTargetLangCombo) {
		const QSignalBlocker langBlocker(_translationTargetLangCombo);
		const QString lang = value.translationTargetLanguage.trimmed();
		const int idx = _translationTargetLangCombo->findData(lang);
		if (idx >= 0) {
			_translationTargetLangCombo->setCurrentIndex(idx);
		} else {
			_translationTargetLangCombo->setCurrentIndex(-1);
			_translationTargetLangCombo->setEditText(lang.isEmpty() ? QStringLiteral("zh") : lang);
		}
	}
	if (_captionDisplayModeCombo) {
		const int idx = _captionDisplayModeCombo->findData(static_cast<int>(value.liveCaptionDisplayMode));
		_captionDisplayModeCombo->setCurrentIndex(idx >= 0 ? idx : 0);
	}
	if (_translationProfileCombo) {
		const QSignalBlocker profBlocker(_translationProfileCombo);
		const QString pid = value.translationProfileId.trimmed();
		int idx = pid.isEmpty() ? 0 : _translationProfileCombo->findData(pid);
		if (idx < 0 && !pid.isEmpty()) {
			refreshCopilotProfileSelectors();
			idx = _translationProfileCombo->findData(pid);
		}
		_translationProfileCombo->setCurrentIndex(idx >= 0 ? idx : 0);
	}
	if (_translationModelEdit) {
		const QSignalBlocker modelBlocker(_translationModelEdit);
		_translationModelEdit->setText(value.translationModelOverride);
	}
	if (_translationPromptEdit) {
		const QSignalBlocker promptBlocker(_translationPromptEdit);
		_translationPromptEdit->setPlainText(value.translationSystemPrompt);
	}
	if (_translationIncludeLocalMic) {
		_translationIncludeLocalMic->setChecked(value.translateLocalMicrophone);
	}
	if (_translationMaxConcurrentStreamsSpin) {
		_translationMaxConcurrentStreamsSpin->setValue(
			std::clamp(value.maxConcurrentStreamingAsr, 1, 8));
	}
	if (_translationIdleReleaseSpin) {
		const QSignalBlocker idleBlocker(_translationIdleReleaseSpin);
		_translationIdleReleaseSpin->setValue(
			std::clamp(value.streamingIdleReleaseMs, 3000, 120000) / 1000);
	}
	updateCopilotTranslationLinkedUi();
	_updatingUi = oldUpdating;
}

void CopilotSettingsPage::focusSearchMatch(const QString &needle) {
	if (needle.isEmpty() || !_copilotSubPages) {
		return;
	}
	const auto containsNeedle = [&needle](const QString &text) {
		return text.toLower().contains(needle);
	};
	for (int tab = 0; tab < _copilotSubPages->count(); ++tab) {
		QWidget *subScroll = _copilotSubPages->widget(tab);
		if (!subScroll) {
			continue;
		}
		QWidget *matchedWidget = nullptr;
		// Prefer exact field labels or control buttons inside the subpage content rather than card headers
		const auto checkWidgets = [&](auto list) {
			for (auto *w : list) {
				if (!w) {
					continue;
				}
				const QString objName = w->objectName();
				if (objName == QStringLiteral("settingsCardTitle") ||
					objName == QStringLiteral("settingsScopeBadge") ||
					objName == QStringLiteral("settingsCollapsibleSummary")) {
					continue;
				}
				if (containsNeedle(w->text())) {
					matchedWidget = w;
					return true;
				}
			}
			return false;
		};
		if (!checkWidgets(subScroll->findChildren<QLabel *>())) {
			checkWidgets(subScroll->findChildren<QAbstractButton *>());
		}
		if (!matchedWidget) {
			// Fallback to any label/button match inside subpage
			for (auto *lbl : subScroll->findChildren<QLabel *>()) {
				if (containsNeedle(lbl->text())) {
					matchedWidget = lbl;
					break;
				}
			}
		}
		if (!matchedWidget) {
			continue;
		}

		setSubTab(tab);
		for (auto *sec : {_copilotChatAdvSection, _copilotSttAdvSection, _copilotTrAdvSection}) {
			if (sec && sec->bodyWidget() && sec->bodyWidget()->isAncestorOf(matchedWidget)) {
				sec->setExpanded(true);
			}
		}
		QWidget *scrollTarget = matchedWidget;
		const bool hiddenInCustomSpeechSection =
			((_copilotSttProfileToolbar && _copilotSttProfileToolbar->isAncestorOf(matchedWidget)) ||
			 (_copilotSttProfileNameBlock && _copilotSttProfileNameBlock->isAncestorOf(matchedWidget)) ||
			 (_copilotSttEndpointContainer && _copilotSttEndpointContainer->isAncestorOf(matchedWidget))) &&
			(selectedCopilotSpeechSourceMode() != OpenMeeting::AgentSpeechSourceMode::CustomProfile);
		if (hiddenInCustomSpeechSection && _copilotSttModeCustomBtn) {
			scrollTarget = _copilotSttModeCustomBtn;
		}
		if (auto *scrollArea = qobject_cast<QScrollArea *>(subScroll)) {
			const QPointer<QScrollArea> guardScroll(scrollArea);
			const QPointer<QWidget> guardTarget(scrollTarget);
			QTimer::singleShot(0, this, [guardScroll, guardTarget] {
				if (guardScroll && guardTarget && guardTarget->isVisible()) {
					guardScroll->ensureWidgetVisible(guardTarget, 0, 24);
				}
			});
		}
		return;
	}
}

OpenMeeting::AgentCoordinator &CopilotSettingsPage::copilotCoordinator() {
	if (!_agentCoordinator) {
		if (!_ownedAgentCoordinator) {
			_ownedAgentCoordinator = std::make_unique<OpenMeeting::AgentCoordinator>(_session);
		}
		return *_ownedAgentCoordinator;
	}
	return *_agentCoordinator;
}

void CopilotSettingsPage::disconnectCopilotCoordinatorSignals() {
	if (_agentProfileConn) {
		disconnect(_agentProfileConn);
		_agentProfileConn = {};
	}
	if (_agentProfilesConn) {
		disconnect(_agentProfilesConn);
		_agentProfilesConn = {};
	}
	if (_agentSpeechProfilesConn) {
		disconnect(_agentSpeechProfilesConn);
		_agentSpeechProfilesConn = {};
	}
	if (_agentProbeConn) {
		disconnect(_agentProbeConn);
		_agentProbeConn = {};
	}
	if (_agentSpeechProbeConn) {
		disconnect(_agentSpeechProbeConn);
		_agentSpeechProbeConn = {};
	}
	if (_agentModelsConn) {
		disconnect(_agentModelsConn);
		_agentModelsConn = {};
	}
	if (_agentSpeechModelsConn) {
		disconnect(_agentSpeechModelsConn);
		_agentSpeechModelsConn = {};
	}
}

void CopilotSettingsPage::setAgentCoordinator(OpenMeeting::AgentCoordinator *coordinator, bool pageCurrentlyVisible) {
	if (_agentCoordinator == coordinator) {
		return;
	}
	disconnectCopilotCoordinatorSignals();
	_agentCoordinator = coordinator;
	if (_agentCoordinator) {
		_ownedAgentCoordinator.reset();
	}
	_copilotLastModelFetchBaseUrl.clear();
	_copilotLastSttModelFetchUrl.clear();
	_copilotLastSttEndpointKey.clear();
	_copilotSttModelAutoSelected = false;
	if (_copilotModelCombo) {
		const bool prevUpdating = _updatingUi;
		_updatingUi = true;
		const QSignalBlocker comboBlocker(_copilotModelCombo);
		_copilotModelCombo->clear();
		_updatingUi = prevUpdating;
	}
	auto &activeCoord = copilotCoordinator();
	if (activeCoord.isFetchingModels()) {
		setCopilotModelHint(
			QCoreApplication::translate("MeetingUI", "Fetching models..."),
			QStringLiteral("pending"));
	} else {
		resetCopilotModelHint();
	}
	if (_copilotRefreshModelsBtn) {
		_copilotRefreshModelsBtn->setEnabled(!activeCoord.isFetchingModels());
	}
	if (_copilotTestBtn) {
		_copilotTestBtn->setEnabled(!activeCoord.isProbing());
	}
	if (_copilotSttTestBtn) {
		_copilotSttTestBtn->setEnabled(!activeCoord.isSpeechProbing());
	}
	setCopilotTestStatus(
		activeCoord.isProbing() ? QStringLiteral("pending") : QString(),
		activeCoord.isProbing()
			? QCoreApplication::translate("MeetingUI", "Testing connection...")
			: QString());
	setCopilotSttTestStatus(
		activeCoord.isSpeechProbing() ? QStringLiteral("pending") : QString(),
		activeCoord.isSpeechProbing()
			? QCoreApplication::translate("MeetingUI", "Testing connection...")
			: QString());
	syncControls();
	if (pageCurrentlyVisible) {
		maybeAutoRefreshModels();
	}
}

void CopilotSettingsPage::buildUi() {
	auto *page = this;
	page->setObjectName(QStringLiteral("settingsCopilotRoot"));
	auto *rootLayout = new QVBoxLayout(page);
	rootLayout->setContentsMargins(0, 0, 0, 0);
	rootLayout->setSpacing(0);

	// 顶部固定 3 段式子页签切换栏
	auto *tabBarWrap = new QWidget(page);
	tabBarWrap->setObjectName(QStringLiteral("settingsCopilotTabBarWrap"));
	auto *tabBarWrapLayout = new QVBoxLayout(tabBarWrap);
	tabBarWrapLayout->setContentsMargins(20, 10, 20, 6);
	tabBarWrapLayout->setSpacing(0);

	auto *segmentedFrame = new QFrame(tabBarWrap);
	segmentedFrame->setObjectName(QStringLiteral("settingsSegmentedControl"));
	auto *segLayout = new QHBoxLayout(segmentedFrame);
	segLayout->setContentsMargins(3, 3, 3, 3);
	segLayout->setSpacing(4);

	auto *tabGroup = new QButtonGroup(segmentedFrame);
	tabGroup->setExclusive(true);

	const std::array<std::pair<const char *, QString>, 3> tabDefs = {
		std::pair{"settingsCopilotTabChat", QCoreApplication::translate("MeetingUI", "Models & Chat")},
		std::pair{"settingsCopilotTabStt", QCoreApplication::translate("MeetingUI", "Speech & Dictation (ASR)")},
		std::pair{"settingsCopilotTabTranslation", QCoreApplication::translate("MeetingUI", "Live Translation & Agent")}
	};
	for (size_t i = 0; i < tabDefs.size(); ++i) {
		auto *btn = new QPushButton(tabDefs[i].second, segmentedFrame);
		btn->setObjectName(QString::fromLatin1(tabDefs[i].first));
		btn->setProperty("segmentedTab", true);
		btn->setCheckable(true);
		btn->setAutoDefault(false);
		btn->setCursor(Qt::PointingHandCursor);
		btn->setChecked(i == 0);
		tabGroup->addButton(btn, static_cast<int>(i));
		segLayout->addWidget(btn, 1);
		_copilotSubTabButtons[i] = btn;
	}
	tabBarWrapLayout->addWidget(segmentedFrame);
	rootLayout->addWidget(tabBarWrap);

	_copilotSubPages = new QStackedWidget(page);
	_copilotSubPages->setObjectName(QStringLiteral("settingsCopilotSubPages"));
	rootLayout->addWidget(_copilotSubPages, 1);

	const auto makeSubScrollPage = [this] {
		auto *scroll = makeScrollablePage(_copilotSubPages);
		scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		auto *content = scroll->widget();
		auto *outer = static_cast<QVBoxLayout *>(content->layout());
		outer->setContentsMargins(20, 8, 20, 14);
		outer->setSpacing(10);
		_copilotSubPages->addWidget(scroll);
		return std::pair{content, outer};
	};

	const auto makeScopeAccessory = [](QToolButton *hintIcon, const QString &badgeText, const QString &tone, QWidget *parent, QLabel **outBadge = nullptr) {
		auto *wrap = new QWidget(parent);
		auto *h = new QHBoxLayout(wrap);
		h->setContentsMargins(0, 0, 0, 0);
		h->setSpacing(6);
		if (hintIcon) {
			hintIcon->setParent(wrap);
			h->addWidget(hintIcon, 0, Qt::AlignVCenter);
		}
		auto *badge = new QLabel(badgeText, wrap);
		badge->setObjectName(QStringLiteral("settingsScopeBadge"));
		badge->setProperty("badgeTone", tone);
		badge->setWordWrap(false);
		badge->setProperty("keepSingleLine", true);
		h->addWidget(badge, 0, Qt::AlignVCenter);
		if (outBadge) {
			*outBadge = badge;
		}
		return wrap;
	};

	const auto makeFieldBlock = [](QWidget *labelOrRowWidget, QWidget *field, QWidget *parent) {
		auto *block = new QWidget(parent);
		auto *vbox = new QVBoxLayout(block);
		vbox->setContentsMargins(0, 0, 0, 0);
		vbox->setSpacing(4);
		vbox->addWidget(labelOrRowWidget);
		vbox->addWidget(field);
		return block;
	};

	// =========================================================================
	// Sub-Page 0: Models & Chat (Profile-scoped)
	// =========================================================================
	const auto [chatContent, chatOuter] = makeSubScrollPage();

	auto *profileToolbarRow = new QHBoxLayout();
	profileToolbarRow->setSpacing(8);
	auto *profileSelectorLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Active Profile"), chatContent);
	profileSelectorLabel->setWordWrap(false);
	profileSelectorLabel->setProperty("keepSingleLine", true);
	profileToolbarRow->addWidget(profileSelectorLabel);

	_copilotProfileSelector = createElidedAccessibleComboBox(chatContent);
	_copilotProfileSelector->setObjectName(QStringLiteral("settingsCopilotProfileSelector"));
	_copilotProfileSelector->setAccessibleName(profileSelectorLabel->text());
	AppTheme::styleChoiceControls(*_copilotProfileSelector, AppTheme::Tone::Light);
	profileSelectorLabel->setBuddy(_copilotProfileSelector);
	profileToolbarRow->addWidget(_copilotProfileSelector, 1);

	_copilotNewProfileBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "New"), chatContent);
	_copilotNewProfileBtn->setObjectName(QStringLiteral("settingsCopilotNewProfile"));
	_copilotNewProfileBtn->setAutoDefault(false);
	_copilotNewProfileBtn->setIconSize(QSize(13, 13));
	_copilotNewProfileBtn->setIcon(AppTheme::icon(AppTheme::Icon::Plus, AppTheme::ColorRole::Text));
	profileToolbarRow->addWidget(_copilotNewProfileBtn);

	_copilotProfileMoreBtn = new QToolButton(chatContent);
	_copilotProfileMoreBtn->setObjectName(QStringLiteral("settingsCopilotProfileMore"));
	_copilotProfileMoreBtn->setAutoRaise(false);
	_copilotProfileMoreBtn->setCursor(Qt::PointingHandCursor);
	_copilotProfileMoreBtn->setIconSize(QSize(15, 15));
	_copilotProfileMoreBtn->setIcon(AppTheme::icon(AppTheme::Icon::MoreHorizontal, AppTheme::ColorRole::Text));
	_copilotProfileMoreBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Duplicate or delete profile"));
	_copilotProfileMoreBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Profile actions"));
	profileToolbarRow->addWidget(_copilotProfileMoreBtn);
	chatOuter->addLayout(profileToolbarRow);

	auto *introHintIcon = makeHintIcon(
		QCoreApplication::translate("MeetingUI", "AI Copilot"),
		QCoreApplication::translate("MeetingUI",
			"Configure a custom OpenAI-compatible API, Responses API, Anthropic Messages API, Google Gemini API, or custom REST webhook for AI Copilot."),
		chatContent,
		15);
	introHintIcon->setObjectName(QStringLiteral("settingsCopilotIntroHintIcon"));

	_copilotTestBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Test Connection"), chatContent);
	_copilotTestBtn->setObjectName(QStringLiteral("settingsCopilotTestBtn"));
	_copilotTestBtn->setProperty("headerAction", true);
	_copilotTestBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Test chat model connection"));
	_copilotTestBtn->setAccessibleName(_copilotTestBtn->toolTip());
	_copilotTestBtn->setAutoDefault(false);

	QVBoxLayout *chatLayout = nullptr;
	auto *chatCard = makeSettingsCard(
		QCoreApplication::translate("MeetingUI", "Connection & Model"),
		chatContent,
		&chatLayout,
		_copilotTestBtn,
		makeScopeAccessory(
			introHintIcon,
			QCoreApplication::translate("MeetingUI", "Per Profile"),
			QStringLiteral("accent"),
			chatContent));
	chatLayout->setContentsMargins(16, 12, 16, 12);
	chatLayout->setSpacing(7);
	chatOuter->addWidget(chatCard);

	_copilotTestStatus = new QLabel(chatCard);
	_copilotTestStatus->setObjectName(QStringLiteral("settingsCopilotStatus"));
	_copilotTestStatus->setTextFormat(Qt::PlainText);
	_copilotTestStatus->setWordWrap(true);
	_copilotTestStatus->hide();
	chatLayout->addWidget(_copilotTestStatus);

	// 2 列行: Display Name (5) + Protocol Dialect (7)
	auto *nameProtoRow = new QHBoxLayout();
	nameProtoRow->setSpacing(10);

	auto *nameLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Display Name"), chatCard);
	nameLabel->setWordWrap(false);
	nameLabel->setProperty("keepSingleLine", true);
	_copilotProfileName = new QLineEdit(chatCard);
	_copilotProfileName->setObjectName(QStringLiteral("settingsCopilotProfileName"));
	_copilotProfileName->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Custom Agent"));
	nameLabel->setBuddy(_copilotProfileName);
	nameProtoRow->addWidget(makeFieldBlock(nameLabel, _copilotProfileName, chatCard), 5);

	auto *protocolLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Protocol Dialect"), chatCard);
	protocolLabel->setWordWrap(false);
	protocolLabel->setProperty("keepSingleLine", true);
	_copilotProtocolCombo = createElidedAccessibleComboBox(chatCard);
	_copilotProtocolCombo->setObjectName(QStringLiteral("settingsCopilotProtocol"));
	_copilotProtocolCombo->setAccessibleName(protocolLabel->text());
	_copilotProtocolCombo->addItem(
		QCoreApplication::translate("MeetingUI", "OpenAI ChatCompletions (SSE)"),
		static_cast<int>(OpenMeeting::AgentProtocolDialect::OpenAiChatCompletionsSse));
	_copilotProtocolCombo->addItem(
		QCoreApplication::translate("MeetingUI", "OpenAI Responses (SSE)"),
		static_cast<int>(OpenMeeting::AgentProtocolDialect::OpenAiResponsesSse));
	_copilotProtocolCombo->addItem(
		QCoreApplication::translate("MeetingUI", "Anthropic Messages (SSE)"),
		static_cast<int>(OpenMeeting::AgentProtocolDialect::AnthropicMessagesSse));
	_copilotProtocolCombo->addItem(
		QCoreApplication::translate("MeetingUI", "Google Gemini (SSE)"),
		static_cast<int>(OpenMeeting::AgentProtocolDialect::GoogleGeminiSse));
	_copilotProtocolCombo->addItem(
		QCoreApplication::translate("MeetingUI", "Custom Webhook (JSON / SSE)"),
		static_cast<int>(OpenMeeting::AgentProtocolDialect::CustomWebhookJson));
	AppTheme::styleChoiceControls(*_copilotProtocolCombo, AppTheme::Tone::Light);
	protocolLabel->setBuddy(_copilotProtocolCombo);
	nameProtoRow->addWidget(makeFieldBlock(protocolLabel, _copilotProtocolCombo, chatCard), 7);
	chatLayout->addLayout(nameProtoRow);

	auto *urlLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Base URL / Endpoint"), chatCard);
	auto *urlHintIcon = makeHintIcon(
		urlLabel->text(),
		QCoreApplication::translate("MeetingUI",
			"HTTPS is required for remote endpoints. HTTP is permitted only for localhost / 127.0.0.1 (e.g. Ollama or vLLM)."),
		chatCard);
	urlHintIcon->setObjectName(QStringLiteral("settingsCopilotUrlHintIcon"));
	_copilotBaseUrl = new QLineEdit(chatCard);
	_copilotBaseUrl->setObjectName(QStringLiteral("settingsCopilotBaseUrl"));
	_copilotBaseUrl->setPlaceholderText(QStringLiteral("https://api.openai.com/v1 or http://127.0.0.1:11434/v1"));
	urlLabel->setBuddy(_copilotBaseUrl);
	auto *urlBlockLayout = new QVBoxLayout();
	urlBlockLayout->setSpacing(4);
	urlBlockLayout->addLayout(makeTitleWithHintRow(urlLabel, urlHintIcon));
	urlBlockLayout->addWidget(_copilotBaseUrl);
	chatLayout->addLayout(urlBlockLayout);

	auto *keyLabel = new QLabel(QCoreApplication::translate("MeetingUI", "API Key / Bearer Token"), chatCard);
	_copilotKeyHintIcon = makeHintIcon(keyLabel->text(), QString(), chatCard);
	_copilotKeyHintIcon->setObjectName(QStringLiteral("settingsCopilotKeyHintIcon"));
	auto *keyRow = new QHBoxLayout();
	keyRow->setSpacing(8);
	_copilotApiKey = new QLineEdit(chatCard);
	_copilotApiKey->setObjectName(QStringLiteral("settingsCopilotApiKey"));
	_copilotApiKey->setEchoMode(QLineEdit::Password);
	_copilotApiKey->setPlaceholderText(
		QCoreApplication::translate("MeetingUI", "Enter API key (leave blank to keep saved key)"));
	keyLabel->setBuddy(_copilotApiKey);
	keyRow->addWidget(_copilotApiKey, 1);

	_copilotSaveKeyBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Save Key"), chatCard);
	_copilotSaveKeyBtn->setObjectName(QStringLiteral("settingsCopilotSaveKey"));
	_copilotSaveKeyBtn->setAutoDefault(false);
	keyRow->addWidget(_copilotSaveKeyBtn);

	_copilotClearKeyBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Clear Key"), chatCard);
	_copilotClearKeyBtn->setObjectName(QStringLiteral("settingsCopilotClearKey"));
	_copilotClearKeyBtn->setAutoDefault(false);
	keyRow->addWidget(_copilotClearKeyBtn);

	auto *keyBlockLayout = new QVBoxLayout();
	keyBlockLayout->setSpacing(4);
	keyBlockLayout->addLayout(makeTitleWithHintRow(keyLabel, _copilotKeyHintIcon));
	keyBlockLayout->addLayout(keyRow);
	chatLayout->addLayout(keyBlockLayout);

	auto *modelLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Model Name"), chatCard);
	_copilotModelHintIcon = makeHintIcon(modelLabel->text(), defaultCopilotModelHint(), chatCard);
	_copilotModelHintIcon->setObjectName(QStringLiteral("settingsCopilotModelHintIcon"));
	auto *modelRow = new QHBoxLayout();
	modelRow->setSpacing(8);
	_copilotModelCombo = createElidedAccessibleComboBox(chatCard);
	_copilotModelCombo->setObjectName(QStringLiteral("settingsCopilotModel"));
	_copilotModelCombo->setAccessibleName(modelLabel->text());
	_copilotModelCombo->setEditable(true);
	_copilotModelCombo->setInsertPolicy(QComboBox::NoInsert);
	AppTheme::styleChoiceControls(*_copilotModelCombo, AppTheme::Tone::Light);
	if (auto *edit = _copilotModelCombo->lineEdit()) {
		edit->setPlaceholderText(QStringLiteral("gpt-4o-mini / qwen-plus"));
	}
	modelLabel->setBuddy(_copilotModelCombo);
	modelRow->addWidget(_copilotModelCombo, 1);

	_copilotRefreshModelsBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Refresh Models"), chatCard);
	_copilotRefreshModelsBtn->setObjectName(QStringLiteral("settingsCopilotRefreshModels"));
	_copilotRefreshModelsBtn->setAutoDefault(false);
	modelRow->addWidget(_copilotRefreshModelsBtn);

	auto *modelBlockLayout = new QVBoxLayout();
	modelBlockLayout->setSpacing(4);
	modelBlockLayout->addLayout(makeTitleWithHintRow(modelLabel, _copilotModelHintIcon));
	modelBlockLayout->addLayout(modelRow);
	chatLayout->addLayout(modelBlockLayout);

	auto *reasoningRow = new QHBoxLayout();
	reasoningRow->setSpacing(10);
	auto *reasoningLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Reasoning Effort"), chatCard);
	_copilotReasoningHintIcon = makeHintIcon(reasoningLabel->text(), QString(), chatCard);
	_copilotReasoningHintIcon->setObjectName(QStringLiteral("settingsCopilotReasoningHintIcon"));
	_copilotReasoningCombo = createAccessibleComboBox(chatCard);
	_copilotReasoningCombo->setObjectName(QStringLiteral("settingsCopilotReasoningEffort"));
	_copilotReasoningCombo->setAccessibleName(reasoningLabel->text());
	_copilotReasoningCombo->addItem(
		QCoreApplication::translate("MeetingUI", "Default (Provider / Model Default)"),
		static_cast<int>(OpenMeeting::AgentReasoningEffort::Default));
	_copilotReasoningCombo->addItem(
		QCoreApplication::translate("MeetingUI", "Low"),
		static_cast<int>(OpenMeeting::AgentReasoningEffort::Low));
	_copilotReasoningCombo->addItem(
		QCoreApplication::translate("MeetingUI", "Medium"),
		static_cast<int>(OpenMeeting::AgentReasoningEffort::Medium));
	_copilotReasoningCombo->addItem(
		QCoreApplication::translate("MeetingUI", "High"),
		static_cast<int>(OpenMeeting::AgentReasoningEffort::High));
	_copilotReasoningCombo->addItem(
		QCoreApplication::translate("MeetingUI", "Extra (xhigh)"),
		static_cast<int>(OpenMeeting::AgentReasoningEffort::Extra));
	AppTheme::styleChoiceControls(*_copilotReasoningCombo, AppTheme::Tone::Light);
	reasoningLabel->setBuddy(_copilotReasoningCombo);
	reasoningRow->addLayout(makeTitleWithHintRow(reasoningLabel, _copilotReasoningHintIcon, false));
	reasoningRow->addWidget(_copilotReasoningCombo, 1);
	chatLayout->addLayout(reasoningRow);

	auto *promptLabel = new QLabel(QCoreApplication::translate("MeetingUI", "System Prompt"), chatCard);
	_copilotSystemPrompt = new QPlainTextEdit(chatCard);
	_copilotSystemPrompt->setObjectName(QStringLiteral("settingsCopilotSystemPrompt"));
	_copilotSystemPrompt->setFixedHeight(56);
	promptLabel->setBuddy(_copilotSystemPrompt);
	chatLayout->addWidget(makeFieldBlock(promptLabel, _copilotSystemPrompt, chatCard));

	// 唯一高级折叠区：Advanced Runtime & Privacy
	_copilotChatAdvSection = new SettingsCollapsibleSection(
		QCoreApplication::translate("MeetingUI", "Advanced Runtime & Privacy"),
		QStringLiteral("settingsCopilotChatAdvToggle"),
		chatCard);
	chatLayout->addWidget(_copilotChatAdvSection);
	auto *chatAdvLayout = _copilotChatAdvSection->bodyLayout();

	auto *headersLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Custom Headers"), _copilotChatAdvSection->bodyWidget());
	auto *headersHintIcon = makeHintIcon(
		headersLabel->text(),
		QCoreApplication::translate("MeetingUI", "Optional, one per line: Header-Name: value"),
		_copilotChatAdvSection->bodyWidget());
	headersHintIcon->setObjectName(QStringLiteral("settingsCopilotHeadersHintIcon"));
	_copilotCustomHeaders = new QPlainTextEdit(_copilotChatAdvSection->bodyWidget());
	_copilotCustomHeaders->setObjectName(QStringLiteral("settingsCopilotHeaders"));
	_copilotCustomHeaders->setFixedHeight(56);
	_copilotCustomHeaders->setPlaceholderText(QStringLiteral("X-Custom-Header: value"));
	headersLabel->setBuddy(_copilotCustomHeaders);
	auto *headersBlockLayout = new QVBoxLayout();
	headersBlockLayout->setSpacing(4);
	headersBlockLayout->addLayout(makeTitleWithHintRow(headersLabel, headersHintIcon));
	headersBlockLayout->addWidget(_copilotCustomHeaders);
	chatAdvLayout->addLayout(headersBlockLayout);

	auto *chatAdvGrid = new QGridLayout();
	chatAdvGrid->setHorizontalSpacing(10);
	chatAdvGrid->setVerticalSpacing(8);

	auto *historyLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Max Context Messages"), _copilotChatAdvSection->bodyWidget());
	_copilotMaxHistorySpin = new QSpinBox(_copilotChatAdvSection->bodyWidget());
	_copilotMaxHistorySpin->setObjectName(QStringLiteral("settingsCopilotMaxHistory"));
	_copilotMaxHistorySpin->setRange(2, 64);
	historyLabel->setBuddy(_copilotMaxHistorySpin);
	chatAdvGrid->addWidget(makeFieldBlock(historyLabel, _copilotMaxHistorySpin, _copilotChatAdvSection->bodyWidget()), 0, 0);

	auto *tempLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Temperature"), _copilotChatAdvSection->bodyWidget());
	_copilotTemperatureSpin = new QDoubleSpinBox(_copilotChatAdvSection->bodyWidget());
	_copilotTemperatureSpin->setObjectName(QStringLiteral("settingsCopilotTemperature"));
	_copilotTemperatureSpin->setRange(0.0, 2.0);
	_copilotTemperatureSpin->setSingleStep(0.1);
	_copilotTemperatureSpin->setDecimals(2);
	tempLabel->setBuddy(_copilotTemperatureSpin);
	chatAdvGrid->addWidget(makeFieldBlock(tempLabel, _copilotTemperatureSpin, _copilotChatAdvSection->bodyWidget()), 0, 1);

	auto *connectTimeoutLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Connect Timeout"), _copilotChatAdvSection->bodyWidget());
	_copilotConnectTimeoutSpin = new QSpinBox(_copilotChatAdvSection->bodyWidget());
	_copilotConnectTimeoutSpin->setObjectName(QStringLiteral("settingsCopilotConnectTimeout"));
	_copilotConnectTimeoutSpin->setAccessibleName(connectTimeoutLabel->text());
	_copilotConnectTimeoutSpin->setRange(2000, 60000);
	_copilotConnectTimeoutSpin->setSingleStep(1000);
	_copilotConnectTimeoutSpin->setSuffix(QStringLiteral(" ms"));
	_copilotConnectTimeoutSpin->setValue(10000);
	connectTimeoutLabel->setBuddy(_copilotConnectTimeoutSpin);
	chatAdvGrid->addWidget(makeFieldBlock(connectTimeoutLabel, _copilotConnectTimeoutSpin, _copilotChatAdvSection->bodyWidget()), 1, 0);

	auto *idleTimeoutLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Idle Timeout"), _copilotChatAdvSection->bodyWidget());
	_copilotIdleTimeoutSpin = new QSpinBox(_copilotChatAdvSection->bodyWidget());
	_copilotIdleTimeoutSpin->setObjectName(QStringLiteral("settingsCopilotIdleTimeout"));
	_copilotIdleTimeoutSpin->setAccessibleName(idleTimeoutLabel->text());
	_copilotIdleTimeoutSpin->setRange(3000, 180000);
	_copilotIdleTimeoutSpin->setSingleStep(1000);
	_copilotIdleTimeoutSpin->setSuffix(QStringLiteral(" ms"));
	_copilotIdleTimeoutSpin->setValue(30000);
	idleTimeoutLabel->setBuddy(_copilotIdleTimeoutSpin);
	chatAdvGrid->addWidget(makeFieldBlock(idleTimeoutLabel, _copilotIdleTimeoutSpin, _copilotChatAdvSection->bodyWidget()), 1, 1);
	chatAdvLayout->addLayout(chatAdvGrid);

	auto *privacyDivider = new QFrame(_copilotChatAdvSection->bodyWidget());
	privacyDivider->setObjectName(QStringLiteral("settingsRowDivider"));
	privacyDivider->setFixedHeight(1);
	chatAdvLayout->addWidget(privacyDivider);

	auto *privacyGroupTitle = new QLabel(
		QCoreApplication::translate("MeetingUI", "Privacy & Local Storage"),
		_copilotChatAdvSection->bodyWidget());
	privacyGroupTitle->setObjectName(QStringLiteral("settingsSubGroupTitle"));
	chatAdvLayout->addWidget(privacyGroupTitle);

	_copilotEncryptKeyCheck = new QCheckBox(
		QCoreApplication::translate("MeetingUI", "Persist encrypted API key on this device (DPAPI / Keyring)"),
		_copilotChatAdvSection->bodyWidget());
	_copilotEncryptKeyCheck->setObjectName(QStringLiteral("settingsCopilotEncryptKey"));
	_copilotEncryptKeyCheck->setChecked(true);
	auto *encryptHintIcon = makeHintIcon(
		_copilotEncryptKeyCheck->text(),
		QCoreApplication::translate("MeetingUI",
			"Recommended. When unchecked, the API key is kept only in process memory for the current session."),
		_copilotChatAdvSection->bodyWidget());
	encryptHintIcon->setObjectName(QStringLiteral("settingsCopilotEncryptHintIcon"));
	chatAdvLayout->addLayout(makeTitleWithHintRow(_copilotEncryptKeyCheck, encryptHintIcon));

	auto *historyRow = new QHBoxLayout();
	historyRow->setSpacing(10);
	_copilotPersistHistoryCheck = new QCheckBox(
		QCoreApplication::translate("MeetingUI", "Save Copilot chat history locally per account"),
		_copilotChatAdvSection->bodyWidget());
	_copilotPersistHistoryCheck->setObjectName(QStringLiteral("settingsCopilotPersistHistory"));
	historyRow->addWidget(_copilotPersistHistoryCheck, 1);

	_copilotClearHistoryBtn = new QPushButton(
		QCoreApplication::translate("MeetingUI", "Clear Saved History"),
		_copilotChatAdvSection->bodyWidget());
	_copilotClearHistoryBtn->setObjectName(QStringLiteral("settingsCopilotClearHistory"));
	_copilotClearHistoryBtn->setAutoDefault(false);
	historyRow->addWidget(_copilotClearHistoryBtn);
	chatAdvLayout->addLayout(historyRow);

	chatOuter->addStretch(1);

	// =========================================================================
	// Sub-Page 1: Speech & Dictation (Follow Chat or Custom Speech Profiles)
	// =========================================================================
	const auto [sttContent, sttOuter] = makeSubScrollPage();

	auto *speechModeRow = new QHBoxLayout();
	speechModeRow->setSpacing(10);
	auto *speechModeLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Speech Source"), sttContent);
	speechModeLabel->setWordWrap(false);
	speechModeLabel->setProperty("keepSingleLine", true);
	speechModeRow->addWidget(speechModeLabel);

	auto *speechModeFrame = new QFrame(sttContent);
	speechModeFrame->setObjectName(QStringLiteral("settingsSpeechModeControl"));
	auto *speechModeLayout = new QHBoxLayout(speechModeFrame);
	speechModeLayout->setContentsMargins(2, 2, 2, 2);
	speechModeLayout->setSpacing(3);
	auto *speechModeGroup = new QButtonGroup(speechModeFrame);
	speechModeGroup->setExclusive(true);

	_copilotSttModeFollowBtn = new QPushButton(
		QCoreApplication::translate("MeetingUI", "Reuse \"Models & Chat\""),
		speechModeFrame);
	_copilotSttModeFollowBtn->setObjectName(QStringLiteral("settingsCopilotSttModeFollow"));
	_copilotSttModeFollowBtn->setProperty("segmentedSubTab", true);
	_copilotSttModeFollowBtn->setCheckable(true);
	_copilotSttModeFollowBtn->setAutoDefault(false);
	_copilotSttModeFollowBtn->setCursor(Qt::PointingHandCursor);
	_copilotSttModeFollowBtn->setChecked(true);
	speechModeGroup->addButton(_copilotSttModeFollowBtn, static_cast<int>(OpenMeeting::AgentSpeechSourceMode::FollowChat));
	speechModeLayout->addWidget(_copilotSttModeFollowBtn, 1);

	_copilotSttModeCustomBtn = new QPushButton(
		QCoreApplication::translate("MeetingUI", "Custom Speech Profiles"),
		speechModeFrame);
	_copilotSttModeCustomBtn->setObjectName(QStringLiteral("settingsCopilotSttModeCustom"));
	_copilotSttModeCustomBtn->setProperty("segmentedSubTab", true);
	_copilotSttModeCustomBtn->setCheckable(true);
	_copilotSttModeCustomBtn->setAutoDefault(false);
	_copilotSttModeCustomBtn->setCursor(Qt::PointingHandCursor);
	speechModeGroup->addButton(_copilotSttModeCustomBtn, static_cast<int>(OpenMeeting::AgentSpeechSourceMode::CustomProfile));
	speechModeLayout->addWidget(_copilotSttModeCustomBtn, 1);

	speechModeRow->addWidget(speechModeFrame, 1);
	sttOuter->addLayout(speechModeRow);

	_copilotSttProfileToolbar = new QWidget(sttContent);
	auto *sttToolbarLayout = new QHBoxLayout(_copilotSttProfileToolbar);
	sttToolbarLayout->setContentsMargins(0, 0, 0, 0);
	sttToolbarLayout->setSpacing(8);

	auto *sttProfileSelectorLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Speech Profile"), _copilotSttProfileToolbar);
	sttProfileSelectorLabel->setWordWrap(false);
	sttProfileSelectorLabel->setProperty("keepSingleLine", true);
	sttToolbarLayout->addWidget(sttProfileSelectorLabel);

	_copilotSttProfileSelector = createElidedAccessibleComboBox(_copilotSttProfileToolbar);
	_copilotSttProfileSelector->setObjectName(QStringLiteral("settingsCopilotSttProfileSelector"));
	_copilotSttProfileSelector->setAccessibleName(sttProfileSelectorLabel->text());
	AppTheme::styleChoiceControls(*_copilotSttProfileSelector, AppTheme::Tone::Light);
	sttProfileSelectorLabel->setBuddy(_copilotSttProfileSelector);
	sttToolbarLayout->addWidget(_copilotSttProfileSelector, 1);

	_copilotSttNewProfileBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "New"), _copilotSttProfileToolbar);
	_copilotSttNewProfileBtn->setObjectName(QStringLiteral("settingsCopilotSttNewProfile"));
	_copilotSttNewProfileBtn->setAutoDefault(false);
	_copilotSttNewProfileBtn->setIconSize(QSize(13, 13));
	_copilotSttNewProfileBtn->setIcon(AppTheme::icon(AppTheme::Icon::Plus, AppTheme::ColorRole::Text));
	sttToolbarLayout->addWidget(_copilotSttNewProfileBtn);

	_copilotSttProfileMoreBtn = new QToolButton(_copilotSttProfileToolbar);
	_copilotSttProfileMoreBtn->setObjectName(QStringLiteral("settingsCopilotSttProfileMore"));
	_copilotSttProfileMoreBtn->setAutoRaise(false);
	_copilotSttProfileMoreBtn->setCursor(Qt::PointingHandCursor);
	_copilotSttProfileMoreBtn->setIconSize(QSize(15, 15));
	_copilotSttProfileMoreBtn->setIcon(AppTheme::icon(AppTheme::Icon::MoreHorizontal, AppTheme::ColorRole::Text));
	_copilotSttProfileMoreBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Duplicate or delete speech profile"));
	_copilotSttProfileMoreBtn->setAccessibleName(QCoreApplication::translate("MeetingUI", "Speech profile actions"));
	sttToolbarLayout->addWidget(_copilotSttProfileMoreBtn);
	_copilotSttProfileToolbar->hide();
	sttOuter->addWidget(_copilotSttProfileToolbar);

	const QString sttCardTitle = QCoreApplication::translate(
		"MeetingUI", "Speech Recognition & Dictation (ASR / STT)");
	auto *sttHintIcon = makeHintIcon(
		sttCardTitle,
		QCoreApplication::translate(
			"MeetingUI",
			"Shared by Copilot voice dictation and live meeting transcription. Reuse your active \"Models & Chat\" connection, or switch to Custom Speech Profiles to manage multiple dedicated ASR endpoints."),
		sttContent,
		15);
	sttHintIcon->setObjectName(QStringLiteral("settingsCopilotSttHintIcon"));

	_copilotSttTestBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Test Connection"), sttContent);
	_copilotSttTestBtn->setObjectName(QStringLiteral("settingsCopilotSttTestBtn"));
	_copilotSttTestBtn->setProperty("headerAction", true);
	_copilotSttTestBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Test speech-to-text model connection"));
	_copilotSttTestBtn->setAccessibleName(_copilotSttTestBtn->toolTip());
	_copilotSttTestBtn->setAutoDefault(false);

	QVBoxLayout *sttLayout = nullptr;
	auto *sttCard = makeSettingsCard(
		sttCardTitle,
		sttContent,
		&sttLayout,
		_copilotSttTestBtn,
		makeScopeAccessory(
			sttHintIcon,
			QCoreApplication::translate("MeetingUI", "Follows Chat"),
			QStringLiteral("muted"),
			sttContent,
			&_copilotSttScopeBadge));
	sttLayout->setContentsMargins(16, 12, 16, 12);
	sttLayout->setSpacing(8);
	sttOuter->addWidget(sttCard);

	_copilotSttTestStatus = new QLabel(sttCard);
	_copilotSttTestStatus->setObjectName(QStringLiteral("settingsCopilotSttStatus"));
	_copilotSttTestStatus->setTextFormat(Qt::PlainText);
	_copilotSttTestStatus->setWordWrap(true);
	_copilotSttTestStatus->hide();
	sttLayout->addWidget(_copilotSttTestStatus);

	// 复用模式下显示当前对话配置摘要
	_copilotSttFollowBanner = new QFrame(sttCard);
	_copilotSttFollowBanner->setObjectName(QStringLiteral("settingsInlineBanner"));
	auto *followBannerLayout = new QHBoxLayout(_copilotSttFollowBanner);
	followBannerLayout->setContentsMargins(10, 7, 10, 7);
	followBannerLayout->setSpacing(8);
	_copilotSttFollowLabel = new QLabel(_copilotSttFollowBanner);
	_copilotSttFollowLabel->setObjectName(QStringLiteral("hintLabel"));
	_copilotSttFollowLabel->setWordWrap(true);
	followBannerLayout->addWidget(_copilotSttFollowLabel, 1);
	sttLayout->addWidget(_copilotSttFollowBanner);

	_copilotSttKeyWarningBanner = new QLabel(sttCard);
	_copilotSttKeyWarningBanner->setObjectName(QStringLiteral("settingsInlineWarning"));
	_copilotSttKeyWarningBanner->setWordWrap(true);
	_copilotSttKeyWarningBanner->setTextFormat(Qt::PlainText);
	_copilotSttKeyWarningBanner->hide();
	sttLayout->addWidget(_copilotSttKeyWarningBanner);

	// 2 列行: 配置名称 (5, 仅自定义模式显示) + 识别协议 (7, 复用模式下占满整行)
	auto *sttNameProtoRow = new QHBoxLayout();
	sttNameProtoRow->setSpacing(10);

	auto *sttNameLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Profile Name"), sttCard);
	sttNameLabel->setWordWrap(false);
	sttNameLabel->setProperty("keepSingleLine", true);
	_copilotSttProfileName = new QLineEdit(sttCard);
	_copilotSttProfileName->setObjectName(QStringLiteral("settingsCopilotSttProfileName"));
	_copilotSttProfileName->setPlaceholderText(QCoreApplication::translate("MeetingUI", "e.g. ElevenLabs / Deepgram / Groq"));
	sttNameLabel->setBuddy(_copilotSttProfileName);
	_copilotSttProfileNameBlock = makeFieldBlock(sttNameLabel, _copilotSttProfileName, sttCard);
	_copilotSttProfileNameBlock->hide();
	sttNameProtoRow->addWidget(_copilotSttProfileNameBlock, 5);

	auto *sttProtoBlock = new QWidget(sttCard);
	auto *sttProtoBlockLayout = new QVBoxLayout(sttProtoBlock);
	sttProtoBlockLayout->setContentsMargins(0, 0, 0, 0);
	sttProtoBlockLayout->setSpacing(4);
	auto *sttProtoLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Recognition Protocol"), sttProtoBlock);
	sttProtoLabel->setWordWrap(false);
	sttProtoLabel->setProperty("keepSingleLine", true);
	auto *sttProtoHintIcon = makeHintIcon(
		sttProtoLabel->text(),
		QCoreApplication::translate(
			"MeetingUI",
			"Auto mode routes wss:// URLs and streaming models (e.g. gpt-4o-transcribe, paraformer-realtime-v2, qwen3-asr-flash-realtime) to WebSocket streaming ASR, while keeping https:// + whisper-1 / qwen3-asr-flash on HTTP STT. Select a specific streaming protocol in Custom Speech Profiles to use its official wss:// endpoint with a dedicated key."),
		sttProtoBlock);
	sttProtoHintIcon->setObjectName(QStringLiteral("settingsCopilotSttProtoHintIcon"));
	_copilotSttProtocolCombo = createElidedAccessibleComboBox(sttProtoBlock);
	_copilotSttProtocolCombo->setObjectName(QStringLiteral("settingsCopilotSttProtocol"));
	_copilotSttProtocolCombo->setAccessibleName(sttProtoLabel->text());
	_copilotSttProtocolCombo->addItem(
		QCoreApplication::translate("MeetingUI", "Auto (Recommended)"),
		static_cast<int>(OpenMeeting::AgentSpeechProtocolMode::Auto));
	_copilotSttProtocolCombo->addItem(
		QCoreApplication::translate("MeetingUI", "OpenAI Transcriptions (/audio/transcriptions)"),
		static_cast<int>(OpenMeeting::AgentSpeechProtocolMode::OpenAiTranscriptions));
	_copilotSttProtocolCombo->addItem(
		QCoreApplication::translate("MeetingUI", "Chat Audio (/chat/completions)"),
		static_cast<int>(OpenMeeting::AgentSpeechProtocolMode::ChatInputAudio));
	_copilotSttProtocolCombo->addItem(
		QCoreApplication::translate("MeetingUI", "DashScope Multimodal (multimodal-generation)"),
		static_cast<int>(OpenMeeting::AgentSpeechProtocolMode::DashScopeMultimodal));
	_copilotSttProtocolCombo->addItem(
		QCoreApplication::translate("MeetingUI", "OpenAI Realtime (WebSocket)"),
		static_cast<int>(OpenMeeting::AgentSpeechProtocolMode::OpenAiRealtimeWs));
	_copilotSttProtocolCombo->addItem(
		QCoreApplication::translate("MeetingUI", "DashScope Inference (WebSocket · Paraformer/Fun-ASR)"),
		static_cast<int>(OpenMeeting::AgentSpeechProtocolMode::DashScopeInferenceWs));
	_copilotSttProtocolCombo->addItem(
		QCoreApplication::translate("MeetingUI", "DashScope Qwen3 Realtime (WebSocket)"),
		static_cast<int>(OpenMeeting::AgentSpeechProtocolMode::DashScopeQwenRealtimeWs));
	_copilotSttProtocolCombo->addItem(
		QCoreApplication::translate("MeetingUI", "Deepgram Streaming (WebSocket)"),
		static_cast<int>(OpenMeeting::AgentSpeechProtocolMode::DeepgramWs));
	_copilotSttProtocolCombo->addItem(
		QCoreApplication::translate("MeetingUI", "AssemblyAI v3 (WebSocket)"),
		static_cast<int>(OpenMeeting::AgentSpeechProtocolMode::AssemblyAiV3Ws));
	_copilotSttProtocolCombo->addItem(
		QCoreApplication::translate("MeetingUI", "Volcengine Doubao SAUC v3 (WebSocket)"),
		static_cast<int>(OpenMeeting::AgentSpeechProtocolMode::VolcengineBigModelWs));
	_copilotSttProtocolCombo->addItem(
		QCoreApplication::translate("MeetingUI", "ElevenLabs Scribe Realtime (WebSocket)"),
		static_cast<int>(OpenMeeting::AgentSpeechProtocolMode::ElevenLabsScribeWs));
	_copilotSttProtocolCombo->addItem(
		QCoreApplication::translate("MeetingUI", "xAI Grok STT (WebSocket)"),
		static_cast<int>(OpenMeeting::AgentSpeechProtocolMode::XaiSttWs));
	AppTheme::styleChoiceControls(*_copilotSttProtocolCombo, AppTheme::Tone::Light);
	sttProtoLabel->setBuddy(_copilotSttProtocolCombo);
	sttProtoBlockLayout->addLayout(makeTitleWithHintRow(sttProtoLabel, sttProtoHintIcon));
	sttProtoBlockLayout->addWidget(_copilotSttProtocolCombo);
	sttNameProtoRow->addWidget(sttProtoBlock, 7);
	sttLayout->addLayout(sttNameProtoRow);

	// 自定义语音端点与密钥容器（仅自定义语音配置模式显示）
	_copilotSttEndpointContainer = new QWidget(sttCard);
	auto *sttEpLayout = new QVBoxLayout(_copilotSttEndpointContainer);
	sttEpLayout->setContentsMargins(0, 0, 0, 0);
	sttEpLayout->setSpacing(8);

	auto *sttUrlLabel = new QLabel(
		QCoreApplication::translate("MeetingUI", "Speech Endpoint URL (HTTP / WSS)"),
		_copilotSttEndpointContainer);
	sttUrlLabel->setWordWrap(false);
	sttUrlLabel->setProperty("keepSingleLine", true);
	_copilotSttBaseUrl = new QLineEdit(_copilotSttEndpointContainer);
	_copilotSttBaseUrl->setObjectName(QStringLiteral("settingsCopilotSttBaseUrl"));
	_copilotSttBaseUrl->setAccessibleName(sttUrlLabel->text());
	_copilotSttBaseUrl->setClearButtonEnabled(true);
	_copilotSttBaseUrl->setPlaceholderText(QCoreApplication::translate(
		"MeetingUI",
		"Optional — https:// or wss:// (leave blank for protocol default)"));
	sttUrlLabel->setBuddy(_copilotSttBaseUrl);
	sttEpLayout->addWidget(makeFieldBlock(sttUrlLabel, _copilotSttBaseUrl, _copilotSttEndpointContainer));

	_copilotSttKeyLabel = new QLabel(
		QCoreApplication::translate("MeetingUI", "Speech API Key"),
		_copilotSttEndpointContainer);
	_copilotSttKeyLabel->setWordWrap(false);
	_copilotSttKeyLabel->setProperty("keepSingleLine", true);
	_copilotSttKeyHintIcon = makeHintIcon(_copilotSttKeyLabel->text(), QString(), _copilotSttEndpointContainer);
	_copilotSttKeyHintIcon->setObjectName(QStringLiteral("settingsCopilotSttKeyHintIcon"));
	auto *sttKeyRow = new QHBoxLayout();
	sttKeyRow->setSpacing(8);
	_copilotSttApiKey = new QLineEdit(_copilotSttEndpointContainer);
	_copilotSttApiKey->setObjectName(QStringLiteral("settingsCopilotSttApiKey"));
	_copilotSttApiKey->setAccessibleName(_copilotSttKeyLabel->text());
	_copilotSttApiKey->setEchoMode(QLineEdit::Password);
	_copilotSttApiKey->setPlaceholderText(QCoreApplication::translate(
		"MeetingUI",
		"Enter speech API key (leave blank to keep saved key)"));
	_copilotSttKeyLabel->setBuddy(_copilotSttApiKey);
	sttKeyRow->addWidget(_copilotSttApiKey, 1);

	_copilotSaveSttKeyBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Save Key"), _copilotSttEndpointContainer);
	_copilotSaveSttKeyBtn->setObjectName(QStringLiteral("settingsCopilotSaveSttKey"));
	_copilotSaveSttKeyBtn->setAutoDefault(false);
	sttKeyRow->addWidget(_copilotSaveSttKeyBtn);

	_copilotClearSttKeyBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Clear Key"), _copilotSttEndpointContainer);
	_copilotClearSttKeyBtn->setObjectName(QStringLiteral("settingsCopilotClearSttKey"));
	_copilotClearSttKeyBtn->setAutoDefault(false);
	sttKeyRow->addWidget(_copilotClearSttKeyBtn);

	auto *sttKeyBlockLayout = new QVBoxLayout();
	sttKeyBlockLayout->setSpacing(4);
	sttKeyBlockLayout->addLayout(makeTitleWithHintRow(_copilotSttKeyLabel, _copilotSttKeyHintIcon));
	sttKeyBlockLayout->addLayout(sttKeyRow);
	sttEpLayout->addLayout(sttKeyBlockLayout);

	// Volcengine SAUC v3 auxiliary container (App Key + Resource ID)
	_copilotSttVolcContainer = new QWidget(_copilotSttEndpointContainer);
	_copilotSttVolcContainer->setObjectName(QStringLiteral("settingsCopilotSttVolcContainer"));
	auto *volcLayout = new QVBoxLayout(_copilotSttVolcContainer);
	volcLayout->setContentsMargins(0, 0, 0, 0);
	volcLayout->setSpacing(8);

	auto *volcAppKeyLabel = new QLabel(
		QCoreApplication::translate("MeetingUI", "Volcengine App Key (Optional · Legacy X-Api-App-Key)"),
		_copilotSttVolcContainer);
	_copilotSttAppKeyHintIcon = makeHintIcon(volcAppKeyLabel->text(), QString(), _copilotSttVolcContainer);
	_copilotSttAppKeyHintIcon->setObjectName(QStringLiteral("settingsCopilotSttAppKeyHintIcon"));
	auto *volcAppKeyRow = new QHBoxLayout();
	volcAppKeyRow->setSpacing(8);
	_copilotSttAppKey = new QLineEdit(_copilotSttVolcContainer);
	_copilotSttAppKey->setObjectName(QStringLiteral("settingsCopilotSttAppKey"));
	_copilotSttAppKey->setAccessibleName(volcAppKeyLabel->text());
	_copilotSttAppKey->setEchoMode(QLineEdit::Password);
	_copilotSttAppKey->setPlaceholderText(QCoreApplication::translate(
		"MeetingUI", "Optional — leave blank for single X-Api-Key auth"));
	volcAppKeyLabel->setBuddy(_copilotSttAppKey);
	volcAppKeyRow->addWidget(_copilotSttAppKey, 1);

	_copilotSaveSttAppKeyBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Save"), _copilotSttVolcContainer);
	_copilotSaveSttAppKeyBtn->setObjectName(QStringLiteral("settingsCopilotSaveSttAppKey"));
	_copilotSaveSttAppKeyBtn->setAutoDefault(false);
	volcAppKeyRow->addWidget(_copilotSaveSttAppKeyBtn);

	_copilotClearSttAppKeyBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Clear"), _copilotSttVolcContainer);
	_copilotClearSttAppKeyBtn->setObjectName(QStringLiteral("settingsCopilotClearSttAppKey"));
	_copilotClearSttAppKeyBtn->setAutoDefault(false);
	volcAppKeyRow->addWidget(_copilotClearSttAppKeyBtn);

	auto *volcKeyBlock = new QVBoxLayout();
	volcKeyBlock->setSpacing(4);
	volcKeyBlock->addLayout(makeTitleWithHintRow(volcAppKeyLabel, _copilotSttAppKeyHintIcon));
	volcKeyBlock->addLayout(volcAppKeyRow);
	volcLayout->addLayout(volcKeyBlock);

	auto *volcResLabel = new QLabel(
		QCoreApplication::translate("MeetingUI", "Volcengine Resource ID"),
		_copilotSttVolcContainer);
	_copilotSttResourceIdCombo = createAccessibleComboBox(_copilotSttVolcContainer);
	_copilotSttResourceIdCombo->setObjectName(QStringLiteral("settingsCopilotSttResourceId"));
	_copilotSttResourceIdCombo->setAccessibleName(volcResLabel->text());
	_copilotSttResourceIdCombo->setEditable(true);
	_copilotSttResourceIdCombo->setInsertPolicy(QComboBox::NoInsert);
	_copilotSttResourceIdCombo->addItem(QStringLiteral("volc.seedasr.sauc.duration"));
	_copilotSttResourceIdCombo->addItem(QStringLiteral("volc.seedasr.sauc.concurrent"));
	_copilotSttResourceIdCombo->addItem(QStringLiteral("volc.bigasr.sauc.duration"));
	_copilotSttResourceIdCombo->addItem(QStringLiteral("volc.bigasr.sauc.concurrent"));
	AppTheme::styleChoiceControls(*_copilotSttResourceIdCombo, AppTheme::Tone::Light);
	volcResLabel->setBuddy(_copilotSttResourceIdCombo);
	volcLayout->addWidget(makeFieldBlock(volcResLabel, _copilotSttResourceIdCombo, _copilotSttVolcContainer));
	_copilotSttVolcContainer->hide();
	sttEpLayout->addWidget(_copilotSttVolcContainer);

	// DashScope Workspace ID auxiliary container
	_copilotSttWorkspaceContainer = new QWidget(_copilotSttEndpointContainer);
	_copilotSttWorkspaceContainer->setObjectName(QStringLiteral("settingsCopilotSttWorkspaceContainer"));
	auto *wsLabel = new QLabel(
		QCoreApplication::translate("MeetingUI", "DashScope Workspace ID"),
		_copilotSttWorkspaceContainer);
	_copilotSttWorkspaceEdit = new QLineEdit(_copilotSttWorkspaceContainer);
	_copilotSttWorkspaceEdit->setObjectName(QStringLiteral("settingsCopilotSttWorkspace"));
	_copilotSttWorkspaceEdit->setAccessibleName(wsLabel->text());
	_copilotSttWorkspaceEdit->setClearButtonEnabled(true);
	_copilotSttWorkspaceEdit->setPlaceholderText(QCoreApplication::translate(
		"MeetingUI", "Optional — X-DashScope-WorkSpace"));
	wsLabel->setBuddy(_copilotSttWorkspaceEdit);
	auto *wsLayout = new QVBoxLayout(_copilotSttWorkspaceContainer);
	wsLayout->setContentsMargins(0, 0, 0, 0);
	wsLayout->setSpacing(4);
	wsLayout->addWidget(wsLabel);
	wsLayout->addWidget(_copilotSttWorkspaceEdit);
	_copilotSttWorkspaceContainer->hide();
	sttEpLayout->addWidget(_copilotSttWorkspaceContainer);

	_copilotSttEndpointContainer->hide();
	sttLayout->addWidget(_copilotSttEndpointContainer);

	// 识别模型整行
	auto *sttModelLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Speech Model"), sttCard);
	sttModelLabel->setWordWrap(false);
	sttModelLabel->setProperty("keepSingleLine", true);
	_copilotSttModelCombo = createElidedAccessibleComboBox(sttCard);
	_copilotSttModelCombo->setObjectName(QStringLiteral("settingsCopilotSttModel"));
	_copilotSttModelCombo->setAccessibleName(sttModelLabel->text());
	_copilotSttModelCombo->setEditable(true);
	_copilotSttModelCombo->setInsertPolicy(QComboBox::NoInsert);
	AppTheme::styleChoiceControls(*_copilotSttModelCombo, AppTheme::Tone::Light);
	if (auto *sttEdit = _copilotSttModelCombo->lineEdit()) {
		sttEdit->setPlaceholderText(QCoreApplication::translate(
			"MeetingUI",
			"Auto (e.g. gpt-4o-transcribe / paraformer-realtime-v2 / qwen3-asr-flash / whisper-1)"));
	}
	sttModelLabel->setBuddy(_copilotSttModelCombo);
	sttLayout->addWidget(makeFieldBlock(sttModelLabel, _copilotSttModelCombo, sttCard));

	// 2 列行: Recognition Language + Auto-Send Pause
	auto *dictGrid = new QGridLayout();
	dictGrid->setHorizontalSpacing(10);
	dictGrid->setVerticalSpacing(6);

	auto *dictLangLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Recognition Language"), sttCard);
	dictLangLabel->setWordWrap(false);
	dictLangLabel->setProperty("keepSingleLine", true);
	_copilotDictationLangCombo = createAccessibleComboBox(sttCard);
	_copilotDictationLangCombo->setObjectName(QStringLiteral("settingsCopilotDictationLang"));
	_copilotDictationLangCombo->setAccessibleName(dictLangLabel->text());
	_copilotDictationLangCombo->setEditable(true);
	_copilotDictationLangCombo->setInsertPolicy(QComboBox::NoInsert);
	_copilotDictationLangCombo->addItem(QCoreApplication::translate("MeetingUI", "Auto-detect"), QString());
	_copilotDictationLangCombo->addItem(QCoreApplication::translate("MeetingUI", "Chinese (zh)"), QStringLiteral("zh"));
	_copilotDictationLangCombo->addItem(QCoreApplication::translate("MeetingUI", "English (en)"), QStringLiteral("en"));
	_copilotDictationLangCombo->addItem(QCoreApplication::translate("MeetingUI", "Japanese (ja)"), QStringLiteral("ja"));
	_copilotDictationLangCombo->addItem(QCoreApplication::translate("MeetingUI", "Korean (ko)"), QStringLiteral("ko"));
	AppTheme::styleChoiceControls(*_copilotDictationLangCombo, AppTheme::Tone::Light);
	if (auto *langEdit = _copilotDictationLangCombo->lineEdit()) {
		langEdit->setPlaceholderText(QStringLiteral("zh / en / auto"));
	}
	dictLangLabel->setBuddy(_copilotDictationLangCombo);
	dictGrid->addWidget(makeFieldBlock(dictLangLabel, _copilotDictationLangCombo, sttCard), 0, 0);

	auto *speechTimeoutLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Auto-Send Pause"), sttCard);
	speechTimeoutLabel->setWordWrap(false);
	speechTimeoutLabel->setProperty("keepSingleLine", true);
	_copilotSpeechTimeoutSpin = new QSpinBox(sttCard);
	_copilotSpeechTimeoutSpin->setObjectName(QStringLiteral("settingsCopilotSpeechTimeout"));
	_copilotSpeechTimeoutSpin->setAccessibleName(speechTimeoutLabel->text());
	_copilotSpeechTimeoutSpin->setRange(0, 10000);
	_copilotSpeechTimeoutSpin->setSingleStep(250);
	_copilotSpeechTimeoutSpin->setSuffix(QStringLiteral(" ms"));
	_copilotSpeechTimeoutSpin->setSpecialValueText(QCoreApplication::translate("MeetingUI", "Manual (0 ms)"));
	speechTimeoutLabel->setBuddy(_copilotSpeechTimeoutSpin);
	dictGrid->addWidget(makeFieldBlock(speechTimeoutLabel, _copilotSpeechTimeoutSpin, sttCard), 0, 1);
	dictGrid->setColumnStretch(0, 1);
	dictGrid->setColumnStretch(1, 1);
	sttLayout->addLayout(dictGrid);

	// 唯一高级折叠区: Advanced Acoustic & Recognition
	_copilotSttAdvSection = new SettingsCollapsibleSection(
		QCoreApplication::translate("MeetingUI", "Advanced Acoustic & Recognition"),
		QStringLiteral("settingsCopilotSttAdvToggle"),
		sttCard);
	sttLayout->addWidget(_copilotSttAdvSection);
	auto *sttAdvLayout = _copilotSttAdvSection->bodyLayout();

	auto *sttPromptLabel = new QLabel(
		QCoreApplication::translate("MeetingUI", "Recognition Prompt"),
		_copilotSttAdvSection->bodyWidget());
	_copilotSttPromptEdit = new QLineEdit(_copilotSttAdvSection->bodyWidget());
	_copilotSttPromptEdit->setObjectName(QStringLiteral("settingsCopilotSttPrompt"));
	_copilotSttPromptEdit->setAccessibleName(sttPromptLabel->text());
	_copilotSttPromptEdit->setClearButtonEnabled(true);
	_copilotSttPromptEdit->setMaxLength(1000);
	_copilotSttPromptEdit->setPlaceholderText(QCoreApplication::translate(
		"MeetingUI", "Optional — names, acronyms, or domain terms to improve accuracy"));
	sttPromptLabel->setBuddy(_copilotSttPromptEdit);
	sttAdvLayout->addWidget(makeFieldBlock(sttPromptLabel, _copilotSttPromptEdit, _copilotSttAdvSection->bodyWidget()));

	auto *vadGrid = new QGridLayout();
	vadGrid->setHorizontalSpacing(10);
	vadGrid->setVerticalSpacing(6);

	auto *vadThresholdLabel = new QLabel(
		QCoreApplication::translate("MeetingUI", "VAD Threshold"),
		_copilotSttAdvSection->bodyWidget());
	auto *vadHintIcon = makeHintIcon(
		QCoreApplication::translate("MeetingUI", "Voice Activity Detection (VAD)"),
		QCoreApplication::translate(
			"MeetingUI",
			"Threshold controls microphone/speaker wake sensitivity (-60 dBFS more sensitive, -15 dBFS stricter). Silence Split sets pause duration to commit a segment in HTTP STT mode."),
		_copilotSttAdvSection->bodyWidget());
	vadHintIcon->setObjectName(QStringLiteral("settingsCopilotVadHintIcon"));
	_copilotVadThresholdSpin = new QSpinBox(_copilotSttAdvSection->bodyWidget());
	_copilotVadThresholdSpin->setObjectName(QStringLiteral("settingsCopilotVadThreshold"));
	_copilotVadThresholdSpin->setAccessibleName(vadThresholdLabel->text());
	_copilotVadThresholdSpin->setRange(-60, -15);
	_copilotVadThresholdSpin->setSingleStep(1);
	_copilotVadThresholdSpin->setSuffix(QStringLiteral(" dBFS"));
	_copilotVadThresholdSpin->setValue(-42);
	vadThresholdLabel->setBuddy(_copilotVadThresholdSpin);
	auto *vadLeftCol = new QVBoxLayout();
	vadLeftCol->setSpacing(4);
	vadLeftCol->addLayout(makeTitleWithHintRow(vadThresholdLabel, vadHintIcon));
	vadLeftCol->addWidget(_copilotVadThresholdSpin);
	vadGrid->addLayout(vadLeftCol, 0, 0);

	auto *vadSilenceLabel = new QLabel(
		QCoreApplication::translate("MeetingUI", "VAD Silence Split"),
		_copilotSttAdvSection->bodyWidget());
	_copilotVadSilenceCommitSpin = new QSpinBox(_copilotSttAdvSection->bodyWidget());
	_copilotVadSilenceCommitSpin->setObjectName(QStringLiteral("settingsCopilotVadSilenceCommit"));
	_copilotVadSilenceCommitSpin->setAccessibleName(vadSilenceLabel->text());
	_copilotVadSilenceCommitSpin->setRange(200, 3000);
	_copilotVadSilenceCommitSpin->setSingleStep(50);
	_copilotVadSilenceCommitSpin->setSuffix(QStringLiteral(" ms"));
	_copilotVadSilenceCommitSpin->setValue(600);
	vadSilenceLabel->setBuddy(_copilotVadSilenceCommitSpin);
	vadGrid->addWidget(makeFieldBlock(vadSilenceLabel, _copilotVadSilenceCommitSpin, _copilotSttAdvSection->bodyWidget()), 0, 1);
	vadGrid->setColumnStretch(0, 1);
	vadGrid->setColumnStretch(1, 1);
	sttAdvLayout->addLayout(vadGrid);

	sttOuter->addStretch(1);

	// =========================================================================
	// Sub-Page 2: Live Translation & Room Agent (Global)
	// =========================================================================
	const auto [trContent, trOuter] = makeSubScrollPage();

	const QString trCardTitle = QCoreApplication::translate("MeetingUI", "Live Translation & Room Agent");
	auto *trHintIcon = makeHintIcon(
		trCardTitle,
		QCoreApplication::translate(
			"MeetingUI",
			"Speech recognition reuses your \"Speech & Dictation (ASR)\" API settings; caption translation reuses \"Models & Chat\" when enabled. You can start/stop translation or claim/release the room Agent role anytime from the top Agent badge in a meeting."),
		trContent,
		15);
	trHintIcon->setObjectName(QStringLiteral("settingsTranslationHintIcon"));
	QVBoxLayout *trLayout = nullptr;
	auto *trCard = makeSettingsCard(
		trCardTitle,
		trContent,
		&trLayout,
		nullptr,
		makeScopeAccessory(
			trHintIcon,
			QCoreApplication::translate("MeetingUI", "Global"),
			QStringLiteral("muted"),
			trContent));
	trLayout->setContentsMargins(16, 12, 16, 12);
	trLayout->setSpacing(8);
	trOuter->addWidget(trCard);

	_translationEnableLocal = new QCheckBox(
		QCoreApplication::translate("MeetingUI", "Enable local meeting transcription/translation on join"), trCard);
	_translationEnableLocal->setObjectName(QStringLiteral("settingsTranslationEnableLocal"));
	trLayout->addWidget(_translationEnableLocal);

	auto *trRoleLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Working Mode"), trCard);
	_translationRoleModeCombo = createAccessibleComboBox(trCard);
	_translationRoleModeCombo->setObjectName(QStringLiteral("settingsTranslationRoleMode"));
	_translationRoleModeCombo->setAccessibleName(trRoleLabel->text());
	_translationRoleModeCombo->addItem(
		QCoreApplication::translate("MeetingUI", "Local translation only"),
		static_cast<int>(OpenMeeting::MeetingTranslationRoleMode::LocalOnly));
	_translationRoleModeCombo->addItem(
		QCoreApplication::translate("MeetingUI", "Also act as room Agent when available"),
		static_cast<int>(OpenMeeting::MeetingTranslationRoleMode::RoomAgentIfAvailable));
	AppTheme::styleChoiceControls(*_translationRoleModeCombo, AppTheme::Tone::Light);
	trRoleLabel->setBuddy(_translationRoleModeCombo);
	trLayout->addWidget(makeFieldBlock(trRoleLabel, _translationRoleModeCombo, trCard));

	// 字幕样式常驻外露（不随翻译开关收起）
	auto *captionHeaderRow = new QHBoxLayout();
	captionHeaderRow->setContentsMargins(0, 0, 0, 0);
	captionHeaderRow->setSpacing(8);
	auto *captionDispLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Caption Style"), trCard);
	captionDispLabel->setWordWrap(false);
	captionDispLabel->setProperty("keepSingleLine", true);
	captionHeaderRow->addWidget(captionDispLabel);
	captionHeaderRow->addStretch(1);
	_captionStyleReuseHint = new QLabel(
		QCoreApplication::translate("MeetingUI", "Applies to reused room captions as well"),
		trCard);
	_captionStyleReuseHint->setObjectName(QStringLiteral("hintLabel"));
	_captionStyleReuseHint->setWordWrap(false);
	_captionStyleReuseHint->setProperty("keepSingleLine", true);
	captionHeaderRow->addWidget(_captionStyleReuseHint);

	_captionDisplayModeCombo = createAccessibleComboBox(trCard);
	_captionDisplayModeCombo->setObjectName(QStringLiteral("settingsCaptionDisplayMode"));
	_captionDisplayModeCombo->setAccessibleName(captionDispLabel->text());
	_captionDisplayModeCombo->addItem(
		QCoreApplication::translate("MeetingUI", "Bilingual (Original + Translation)"),
		static_cast<int>(OpenMeeting::LiveCaptionDisplayMode::Bilingual));
	_captionDisplayModeCombo->addItem(
		QCoreApplication::translate("MeetingUI", "Translation only"),
		static_cast<int>(OpenMeeting::LiveCaptionDisplayMode::TranslatedOnly));
	_captionDisplayModeCombo->addItem(
		QCoreApplication::translate("MeetingUI", "Original only"),
		static_cast<int>(OpenMeeting::LiveCaptionDisplayMode::OriginalOnly));
	AppTheme::styleChoiceControls(*_captionDisplayModeCombo, AppTheme::Tone::Light);
	captionDispLabel->setBuddy(_captionDisplayModeCombo);
	auto *captionBlockLayout = new QVBoxLayout();
	captionBlockLayout->setSpacing(4);
	captionBlockLayout->addLayout(captionHeaderRow);
	captionBlockLayout->addWidget(_captionDisplayModeCombo);
	trLayout->addLayout(captionBlockLayout);

	_translationPreferRoomAgent = new QCheckBox(
		QCoreApplication::translate("MeetingUI", "Reuse existing room Agent captions instead of running local API"), trCard);
	_translationPreferRoomAgent->setObjectName(QStringLiteral("settingsTranslationPreferRoomAgent"));
	trLayout->addWidget(_translationPreferRoomAgent);

	_translationIncludeLocalMic = new QCheckBox(
		QCoreApplication::translate("MeetingUI", "Include my own microphone speech in local translation"), trCard);
	_translationIncludeLocalMic->setObjectName(QStringLiteral("settingsTranslationIncludeLocalMic"));
	trLayout->addWidget(_translationIncludeLocalMic);

	// 翻译开关动态联动容器
	auto *translatePanel = new QFrame(trCard);
	translatePanel->setObjectName(QStringLiteral("settingsSubPanel"));
	auto *translatePanelLayout = new QVBoxLayout(translatePanel);
	translatePanelLayout->setContentsMargins(12, 8, 12, 8);
	translatePanelLayout->setSpacing(8);

	_translationEnableOutput = new QCheckBox(
		QCoreApplication::translate("MeetingUI", "Translate captions to target language (uses Copilot model)"),
		translatePanel);
	_translationEnableOutput->setObjectName(QStringLiteral("settingsTranslationEnableOutput"));
	translatePanelLayout->addWidget(_translationEnableOutput);

	_translationTargetContainer = new QWidget(translatePanel);
	auto *trTargetLayout = new QVBoxLayout(_translationTargetContainer);
	trTargetLayout->setContentsMargins(0, 4, 0, 0);
	trTargetLayout->setSpacing(4);
	auto *trLangLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Target Language"), _translationTargetContainer);
	_translationTargetLangCombo = createAccessibleComboBox(_translationTargetContainer);
	_translationTargetLangCombo->setObjectName(QStringLiteral("settingsTranslationTargetLang"));
	_translationTargetLangCombo->setAccessibleName(trLangLabel->text());
	_translationTargetLangCombo->setEditable(true);
	_translationTargetLangCombo->setInsertPolicy(QComboBox::NoInsert);
	_translationTargetLangCombo->addItem(QCoreApplication::translate("MeetingUI", "Chinese (zh)"), QStringLiteral("zh"));
	_translationTargetLangCombo->addItem(QCoreApplication::translate("MeetingUI", "English (en)"), QStringLiteral("en"));
	_translationTargetLangCombo->addItem(QCoreApplication::translate("MeetingUI", "Japanese (ja)"), QStringLiteral("ja"));
	_translationTargetLangCombo->addItem(QCoreApplication::translate("MeetingUI", "Korean (ko)"), QStringLiteral("ko"));
	_translationTargetLangCombo->addItem(QCoreApplication::translate("MeetingUI", "French (fr)"), QStringLiteral("fr"));
	_translationTargetLangCombo->addItem(QCoreApplication::translate("MeetingUI", "German (de)"), QStringLiteral("de"));
	_translationTargetLangCombo->addItem(QCoreApplication::translate("MeetingUI", "Spanish (es)"), QStringLiteral("es"));
	AppTheme::styleChoiceControls(*_translationTargetLangCombo, AppTheme::Tone::Light);
	if (auto *trLangEdit = _translationTargetLangCombo->lineEdit()) {
		trLangEdit->setPlaceholderText(QStringLiteral("zh / en / ja / pt-BR"));
	}
	trLangLabel->setBuddy(_translationTargetLangCombo);
	trTargetLayout->addWidget(trLangLabel);
	trTargetLayout->addWidget(_translationTargetLangCombo);
	_translationTargetContainer->hide();
	translatePanelLayout->addWidget(_translationTargetContainer);
	trLayout->addWidget(translatePanel);

	// 唯一平级高级折叠区: Advanced Translation & Streaming
	_copilotTrAdvSection = new SettingsCollapsibleSection(
		QCoreApplication::translate("MeetingUI", "Advanced Translation & Streaming"),
		QStringLiteral("settingsTranslationAdvToggle"),
		trCard);
	trLayout->addWidget(_copilotTrAdvSection);
	auto *trAdvLayout = _copilotTrAdvSection->bodyLayout();

	// 组 1: Dedicated Translation Override
	auto *overrideHeaderRow = new QHBoxLayout();
	overrideHeaderRow->setContentsMargins(0, 0, 0, 0);
	overrideHeaderRow->setSpacing(8);
	auto *overrideGroupTitle = new QLabel(
		QCoreApplication::translate("MeetingUI", "Dedicated Translation Override"),
		_copilotTrAdvSection->bodyWidget());
	overrideGroupTitle->setObjectName(QStringLiteral("settingsSubGroupTitle"));
	overrideGroupTitle->setWordWrap(false);
	overrideGroupTitle->setProperty("keepSingleLine", true);
	overrideHeaderRow->addWidget(overrideGroupTitle);
	overrideHeaderRow->addStretch(1);
	_translationOverrideInactiveHint = new QLabel(
		QCoreApplication::translate("MeetingUI", "Enabled when caption translation is on"),
		_copilotTrAdvSection->bodyWidget());
	_translationOverrideInactiveHint->setObjectName(QStringLiteral("hintLabel"));
	_translationOverrideInactiveHint->setWordWrap(false);
	_translationOverrideInactiveHint->setProperty("keepSingleLine", true);
	overrideHeaderRow->addWidget(_translationOverrideInactiveHint);
	trAdvLayout->addLayout(overrideHeaderRow);

	_translationOverrideGroup = new QWidget(_copilotTrAdvSection->bodyWidget());
	auto *overrideGroupLayout = new QVBoxLayout(_translationOverrideGroup);
	overrideGroupLayout->setContentsMargins(0, 0, 0, 0);
	overrideGroupLayout->setSpacing(8);

	auto *trProfileLabel = new QLabel(
		QCoreApplication::translate("MeetingUI", "Translation Profile"),
		_translationOverrideGroup);
	_translationProfileCombo = createElidedAccessibleComboBox(_translationOverrideGroup);
	_translationProfileCombo->setObjectName(QStringLiteral("settingsTranslationProfile"));
	_translationProfileCombo->setAccessibleName(trProfileLabel->text());
	_translationProfileCombo->addItem(
		QCoreApplication::translate("MeetingUI", "Follow Active Copilot Profile"), QString());
	AppTheme::styleChoiceControls(*_translationProfileCombo, AppTheme::Tone::Light);
	trProfileLabel->setBuddy(_translationProfileCombo);
	overrideGroupLayout->addWidget(makeFieldBlock(trProfileLabel, _translationProfileCombo, _translationOverrideGroup));

	auto *trModelLabel = new QLabel(
		QCoreApplication::translate("MeetingUI", "Translation Model Override"),
		_translationOverrideGroup);
	_translationModelEdit = new QLineEdit(_translationOverrideGroup);
	_translationModelEdit->setObjectName(QStringLiteral("settingsTranslationModelOverride"));
	_translationModelEdit->setAccessibleName(trModelLabel->text());
	_translationModelEdit->setClearButtonEnabled(true);
	_translationModelEdit->setMaxLength(256);
	_translationModelEdit->setPlaceholderText(QCoreApplication::translate(
		"MeetingUI", "Optional — e.g. gpt-4o-mini / qwen-turbo (leave blank to use profile model)"));
	trModelLabel->setBuddy(_translationModelEdit);
	overrideGroupLayout->addWidget(makeFieldBlock(trModelLabel, _translationModelEdit, _translationOverrideGroup));

	auto *trPromptLabel = new QLabel(
		QCoreApplication::translate("MeetingUI", "Translation Prompt"),
		_translationOverrideGroup);
	_translationPromptEdit = new QPlainTextEdit(_translationOverrideGroup);
	_translationPromptEdit->setObjectName(QStringLiteral("settingsTranslationPrompt"));
	_translationPromptEdit->setAccessibleName(trPromptLabel->text());
	_translationPromptEdit->setFixedHeight(58);
	_translationPromptEdit->setPlaceholderText(QCoreApplication::translate(
		"MeetingUI", "Optional — leave blank for default prompt; supports {target_language} placeholder"));
	trPromptLabel->setBuddy(_translationPromptEdit);
	overrideGroupLayout->addWidget(makeFieldBlock(trPromptLabel, _translationPromptEdit, _translationOverrideGroup));

	_translationPromptCommitTimer = new QTimer(this);
	_translationPromptCommitTimer->setSingleShot(true);
	_translationPromptCommitTimer->setInterval(300);

	trAdvLayout->addWidget(_translationOverrideGroup);

	auto *trDivider = new QFrame(_copilotTrAdvSection->bodyWidget());
	trDivider->setObjectName(QStringLiteral("settingsRowDivider"));
	trDivider->setFixedHeight(1);
	trAdvLayout->addWidget(trDivider);

	// 组 2: Streaming Concurrency & Idle Release
	auto *streamGroupTitle = new QLabel(
		QCoreApplication::translate("MeetingUI", "Streaming Concurrency & Idle Release"),
		_copilotTrAdvSection->bodyWidget());
	streamGroupTitle->setObjectName(QStringLiteral("settingsSubGroupTitle"));
	trAdvLayout->addWidget(streamGroupTitle);

	auto *trConcurrencyGrid = new QGridLayout();
	trConcurrencyGrid->setHorizontalSpacing(10);
	trConcurrencyGrid->setVerticalSpacing(6);

	auto *trConcurrencyLabel = new QLabel(
		QCoreApplication::translate("MeetingUI", "Max Concurrent Streaming Connections"),
		_copilotTrAdvSection->bodyWidget());
	auto *trConcurrencyHint = makeHintIcon(
		trConcurrencyLabel->text(),
		QCoreApplication::translate(
			"MeetingUI",
			"Limits simultaneous per-speaker WebSocket ASR connections during a meeting. Inactive speakers auto-release after the configured idle timeout; speech wakes them with a 250 ms pre-roll buffer."),
		_copilotTrAdvSection->bodyWidget());
	trConcurrencyHint->setObjectName(QStringLiteral("settingsTranslationMaxConcurrentHint"));
	_translationMaxConcurrentStreamsSpin = new QSpinBox(_copilotTrAdvSection->bodyWidget());
	_translationMaxConcurrentStreamsSpin->setObjectName(QStringLiteral("settingsTranslationMaxConcurrentStreams"));
	_translationMaxConcurrentStreamsSpin->setAccessibleName(trConcurrencyLabel->text());
	_translationMaxConcurrentStreamsSpin->setRange(1, 8);
	_translationMaxConcurrentStreamsSpin->setValue(3);
	trConcurrencyLabel->setBuddy(_translationMaxConcurrentStreamsSpin);
	auto *trConcLeftCol = new QVBoxLayout();
	trConcLeftCol->setSpacing(4);
	trConcLeftCol->addLayout(makeTitleWithHintRow(trConcurrencyLabel, trConcurrencyHint));
	trConcLeftCol->addWidget(_translationMaxConcurrentStreamsSpin);
	trConcurrencyGrid->addLayout(trConcLeftCol, 0, 0);

	auto *trIdleLabel = new QLabel(
		QCoreApplication::translate("MeetingUI", "Idle Stream Release"),
		_copilotTrAdvSection->bodyWidget());
	_translationIdleReleaseSpin = new QSpinBox(_copilotTrAdvSection->bodyWidget());
	_translationIdleReleaseSpin->setObjectName(QStringLiteral("settingsTranslationIdleRelease"));
	_translationIdleReleaseSpin->setAccessibleName(trIdleLabel->text());
	_translationIdleReleaseSpin->setRange(3, 120);
	_translationIdleReleaseSpin->setSingleStep(1);
	_translationIdleReleaseSpin->setSuffix(QStringLiteral(" s"));
	_translationIdleReleaseSpin->setValue(12);
	trIdleLabel->setBuddy(_translationIdleReleaseSpin);
	trConcurrencyGrid->addWidget(makeFieldBlock(trIdleLabel, _translationIdleReleaseSpin, _copilotTrAdvSection->bodyWidget()), 0, 1);
	trConcurrencyGrid->setColumnStretch(0, 1);
	trConcurrencyGrid->setColumnStretch(1, 1);
	trAdvLayout->addLayout(trConcurrencyGrid);

	trOuter->addStretch(1);

	// =========================================================================
	// Signal Connections
	// =========================================================================
	connect(tabGroup, &QButtonGroup::idClicked, this, &CopilotSettingsPage::setSubTab);

	_copilotChatAdvSection->setResetHandler([this] { resetCopilotChatAdvancedDefaults(); });
	_copilotSttAdvSection->setResetHandler([this] { resetCopilotSttAdvancedDefaults(); });
	_copilotTrAdvSection->setResetHandler([this] { resetCopilotTranslationAdvancedDefaults(); });

	const auto commitLive = [this] {
		if (!_updatingUi) {
			commitCopilotProfile(false);
		}
	};
	const auto onConnectionFieldEdited = [this] {
		if (!_updatingUi) {
			resetCopilotTestStatus();
		}
	};
	const auto onSttFieldEdited = [this] {
		if (!_updatingUi) {
			resetCopilotSttTestStatus();
		}
	};
	connect(_copilotProfileSelector, static_cast<void (QComboBox::*)(int)>(&QComboBox::activated),
		this, &CopilotSettingsPage::onCopilotProfileSelected);
	connect(_copilotNewProfileBtn, &QPushButton::clicked, this, [this] {
		createNewCopilotProfile(false);
	});
	connect(_copilotProfileMoreBtn, &QToolButton::clicked, this, &CopilotSettingsPage::showCopilotProfileMenu);
	connect(_copilotProfileName, &QLineEdit::editingFinished, this, [this, commitLive] {
		commitLive();
		updateCopilotSttModeUi();
	});
	connect(_copilotBaseUrl, &QLineEdit::textEdited, this, [this, onConnectionFieldEdited](const QString &) {
		onConnectionFieldEdited();
		if (!_updatingUi && copilotSttReusesChatHost()) {
			resetCopilotSttTestStatus();
		}
		updateCopilotKeyStatus();
		updateCopilotSttKeyStatus();
		updateCopilotSttModeUi();
	});
	connect(_copilotBaseUrl, &QLineEdit::editingFinished, this, [this, commitLive] {
		updateCopilotSttProtocolUi();
		commitLive();
		updateCopilotSttKeyStatus();
		updateCopilotSttModeUi();
		if (_updatingUi || !_copilotBaseUrl) {
			return;
		}
		const QString currentBase = _copilotBaseUrl->text().trimmed();
		if (currentBase != _copilotLastModelFetchBaseUrl) {
			refreshCopilotModels(true);
		}
	});
	if (auto *modelEdit = _copilotModelCombo->lineEdit()) {
		connect(modelEdit, &QLineEdit::textEdited, this, [this, onConnectionFieldEdited](const QString &) {
			onConnectionFieldEdited();
			updateCopilotReasoningCapabilityHint();
		});
		connect(modelEdit, &QLineEdit::editingFinished, this, commitLive);
	}
	connect(_copilotModelCombo, static_cast<void (QComboBox::*)(int)>(&QComboBox::activated),
		this, [this, commitLive](int) {
			if (!_updatingUi && _copilotModelCombo &&
				_copilotModelCombo->currentText().trimmed() != copilotCoordinator().profile().model) {
				resetCopilotTestStatus();
			}
			updateCopilotReasoningCapabilityHint();
			commitLive();
		});
	connect(_copilotReasoningCombo, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
		this, [commitLive](int) { commitLive(); });
	connect(_copilotApiKey, &QLineEdit::textEdited, this, [this, onConnectionFieldEdited](const QString &) {
		onConnectionFieldEdited();
		if (!_updatingUi && !copilotCoordinator().hasDictationApiKey() && copilotSttReusesChatHost()) {
			resetCopilotSttTestStatus();
		}
		updateCopilotKeyStatus();
		updateCopilotSttKeyStatus();
		updateCopilotSttModeUi();
	});
	connect(_copilotCustomHeaders, &QPlainTextEdit::textChanged, this, [this, onConnectionFieldEdited] {
		onConnectionFieldEdited();
		if (!_updatingUi && copilotSttReusesChatHost()) {
			resetCopilotSttTestStatus();
		}
		updateCopilotAdvSummaries();
	});
	connect(_copilotRefreshModelsBtn, &QPushButton::clicked, this, [this] {
		refreshCopilotModels(false);
	});
	connect(_copilotProtocolCombo, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
		this, [this, commitLive](int) {
			updateCopilotBaseUrlPlaceholder();
			updateCopilotReasoningCapabilityHint();
			commitLive();
		});
	connect(_copilotEncryptKeyCheck, &QCheckBox::toggled, this, [this, commitLive](bool) {
		commitLive();
		updateCopilotAdvSummaries();
	});
	connect(_copilotPersistHistoryCheck, &QCheckBox::toggled, this, [this, commitLive](bool) {
		commitLive();
		updateCopilotAdvSummaries();
	});
	connect(_copilotClearHistoryBtn, &QPushButton::clicked, this, [this] {
		const bool removed = copilotCoordinator().clearAllSavedSessions();
		setCopilotTestStatus(removed ? QStringLiteral("ok") : QStringLiteral("error"), removed
			? QCoreApplication::translate("MeetingUI", "Saved Copilot chat history cleared for the current account.")
			: QCoreApplication::translate("MeetingUI", "Saved history could not be removed. Retry"));
	});
	connect(_copilotMaxHistorySpin, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
		this, [this, commitLive](int) {
			commitLive();
			updateCopilotAdvSummaries();
		});
	connect(_copilotTemperatureSpin, static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged),
		this, [this, commitLive](double) {
			commitLive();
			updateCopilotAdvSummaries();
		});
	connect(_copilotConnectTimeoutSpin, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
		this, [this, commitLive](int) {
			commitLive();
			updateCopilotAdvSummaries();
		});
	connect(_copilotIdleTimeoutSpin, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
		this, [this, commitLive](int) {
			commitLive();
			updateCopilotAdvSummaries();
		});
	connect(_copilotSttModeFollowBtn, &QPushButton::clicked, this, [this] {
		onCopilotSpeechSourceModeChanged(OpenMeeting::AgentSpeechSourceMode::FollowChat);
	});
	connect(_copilotSttModeCustomBtn, &QPushButton::clicked, this, [this] {
		onCopilotSpeechSourceModeChanged(OpenMeeting::AgentSpeechSourceMode::CustomProfile);
	});
	connect(_copilotSttProfileSelector, static_cast<void (QComboBox::*)(int)>(&QComboBox::activated),
		this, &CopilotSettingsPage::onCopilotSpeechProfileSelected);
	connect(_copilotSttNewProfileBtn, &QPushButton::clicked, this, [this] {
		createNewCopilotSpeechProfile(false);
	});
	connect(_copilotSttProfileMoreBtn, &QToolButton::clicked, this, &CopilotSettingsPage::showCopilotSpeechProfileMenu);
	connect(_copilotSttProfileName, &QLineEdit::editingFinished, this, commitLive);
	connect(_copilotSttBaseUrl, &QLineEdit::textChanged, this, [this, onSttFieldEdited](const QString &) {
		if (!_updatingUi) {
			onSttFieldEdited();
			updateCopilotSttProtocolUi();
			updateCopilotSttKeyStatus();
		}
	});
	connect(_copilotSttBaseUrl, &QLineEdit::editingFinished, this, [this, commitLive] {
		updateCopilotSttProtocolUi();
		commitLive();
		updateCopilotSttKeyStatus();
		refreshCopilotSpeechModels(true);
	});
	connect(_copilotSaveSttKeyBtn, &QPushButton::clicked, this, &CopilotSettingsPage::saveCopilotSttKey);
	connect(_copilotClearSttKeyBtn, &QPushButton::clicked, this, &CopilotSettingsPage::clearCopilotSttKey);
	connect(_copilotSaveSttAppKeyBtn, &QPushButton::clicked, this, &CopilotSettingsPage::saveCopilotSttAppKey);
	connect(_copilotClearSttAppKeyBtn, &QPushButton::clicked, this, &CopilotSettingsPage::clearCopilotSttAppKey);
	connect(_copilotSttTestBtn, &QPushButton::clicked, this, &CopilotSettingsPage::testCopilotSttConnection);
	connect(_copilotSttApiKey, &QLineEdit::textEdited, this, [this, onSttFieldEdited](const QString &) {
		if (!_updatingUi) {
			onSttFieldEdited();
			updateCopilotSttKeyStatus();
		}
	});
	connect(_copilotSttAppKey, &QLineEdit::textEdited, this, [this, onSttFieldEdited](const QString &) {
		if (!_updatingUi) {
			onSttFieldEdited();
			updateCopilotSttAppKeyStatus();
		}
	});
	connect(_copilotSttProtocolCombo, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
		this, [this, onSttFieldEdited, commitLive](int) {
			if (!_updatingUi &&
				selectedCopilotSpeechSourceMode() == OpenMeeting::AgentSpeechSourceMode::CustomProfile &&
				_copilotSttProtocolCombo) {
				const auto selectedProto = OpenMeeting::agentSpeechProtocolModeFromInt(
					_copilotSttProtocolCombo->currentData().toInt());
				if (selectedProto != OpenMeeting::AgentSpeechProtocolMode::Auto) {
					applyCopilotSpeechProtocolPreset(selectedProto, /*forceOverwrite=*/true);
				}
			}
			onSttFieldEdited();
			updateCopilotSttProtocolUi();
			updateCopilotSttKeyStatus();
			commitLive();
		});
	if (auto *resEdit = _copilotSttResourceIdCombo->lineEdit()) {
		connect(resEdit, &QLineEdit::textEdited, this, [onSttFieldEdited](const QString &) {
			onSttFieldEdited();
		});
		connect(resEdit, &QLineEdit::editingFinished, this, commitLive);
	}
	connect(_copilotSttResourceIdCombo, static_cast<void (QComboBox::*)(int)>(&QComboBox::activated),
		this, [onSttFieldEdited, commitLive](int) {
			onSttFieldEdited();
			commitLive();
		});
	connect(_copilotSttWorkspaceEdit, &QLineEdit::textEdited, this, [onSttFieldEdited](const QString &) {
		onSttFieldEdited();
	});
	connect(_copilotSttWorkspaceEdit, &QLineEdit::editingFinished, this, commitLive);
	if (auto *sttModelEdit = _copilotSttModelCombo->lineEdit()) {
		connect(sttModelEdit, &QLineEdit::textEdited, this, [this, onSttFieldEdited](const QString &) {
			if (!_updatingUi) {
				_copilotSttModelAutoSelected = false;
				onSttFieldEdited();
				updateCopilotSttProtocolUi(false);
				updateCopilotSttKeyStatus();
			}
		});
		connect(sttModelEdit, &QLineEdit::editingFinished, this, commitLive);
	}
	connect(_copilotSttModelCombo, static_cast<void (QComboBox::*)(int)>(&QComboBox::activated),
		this, [this, onSttFieldEdited, commitLive](int) {
			_copilotSttModelAutoSelected = false;
			onSttFieldEdited();
			updateCopilotSttProtocolUi(false);
			updateCopilotSttKeyStatus();
			commitLive();
		});
	if (auto *langEdit = _copilotDictationLangCombo->lineEdit()) {
		connect(langEdit, &QLineEdit::textEdited, this, [onSttFieldEdited](const QString &) {
			onSttFieldEdited();
		});
		connect(langEdit, &QLineEdit::editingFinished, this, commitLive);
	}
	connect(_copilotDictationLangCombo, static_cast<void (QComboBox::*)(int)>(&QComboBox::activated),
		this, [onSttFieldEdited, commitLive](int) {
			onSttFieldEdited();
			commitLive();
		});
	connect(_copilotSpeechTimeoutSpin, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
		this, [commitLive](int) { commitLive(); });
	connect(_copilotVadThresholdSpin, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
		this, [this, commitLive](int) {
			commitLive();
			updateCopilotAdvSummaries();
		});
	connect(_copilotVadSilenceCommitSpin, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
		this, [this, commitLive](int) {
			commitLive();
			updateCopilotAdvSummaries();
		});
	connect(_copilotSttPromptEdit, &QLineEdit::textEdited, this, [this, onSttFieldEdited](const QString &) {
		onSttFieldEdited();
		updateCopilotAdvSummaries();
	});
	connect(_copilotSttPromptEdit, &QLineEdit::editingFinished, this, commitLive);
	connect(_copilotSaveKeyBtn, &QPushButton::clicked, this, &CopilotSettingsPage::saveCopilotKey);
	connect(_copilotClearKeyBtn, &QPushButton::clicked, this, &CopilotSettingsPage::clearCopilotKey);
	connect(_copilotTestBtn, &QPushButton::clicked, this, &CopilotSettingsPage::testCopilotConnection);

	updateCopilotSttModeUi();
	updateCopilotTranslationLinkedUi();
	updateCopilotAdvSummaries();
}

void CopilotSettingsPage::setCopilotModelHint(const QString &text, const QString &tone) {
	if (!_copilotModelHintIcon) {
		return;
	}
	const QString trimmed = text.trimmed();
	setHintIcon(
		_copilotModelHintIcon,
		trimmed.isEmpty() ? defaultCopilotModelHint() : trimmed,
		trimmed.isEmpty() ? QString() : tone);
}

void CopilotSettingsPage::resetCopilotModelHint() {
	setCopilotModelHint(defaultCopilotModelHint());
}

void CopilotSettingsPage::setCopilotTestStatus(const QString &tone, const QString &text) {
	if (!_copilotTestStatus) {
		return;
	}
	_copilotTestStatus->setProperty("statusTone", tone);
	if (auto *st = _copilotTestStatus->style()) {
		st->unpolish(_copilotTestStatus);
		st->polish(_copilotTestStatus);
	}
	_copilotTestStatus->setText(text);
	_copilotTestStatus->setVisible(!text.trimmed().isEmpty());
}

void CopilotSettingsPage::resetCopilotTestStatus() {
	if (_agentCoordinator || _ownedAgentCoordinator) {
		copilotCoordinator().cancelProbe();
	}
	if (_copilotTestBtn) {
		_copilotTestBtn->setEnabled(true);
	}
	setCopilotTestStatus(QString(), QString());
}

void CopilotSettingsPage::setCopilotSttTestStatus(const QString &tone, const QString &text) {
	if (!_copilotSttTestStatus) {
		return;
	}
	_copilotSttTestStatus->setProperty("statusTone", tone);
	if (auto *st = _copilotSttTestStatus->style()) {
		st->unpolish(_copilotSttTestStatus);
		st->polish(_copilotSttTestStatus);
	}
	_copilotSttTestStatus->setText(text);
	_copilotSttTestStatus->setVisible(!text.trimmed().isEmpty());
}

void CopilotSettingsPage::resetCopilotSttTestStatus() {
	if (_agentCoordinator || _ownedAgentCoordinator) {
		copilotCoordinator().cancelSpeechProbe();
	}
	if (_copilotSttTestBtn) {
		_copilotSttTestBtn->setEnabled(true);
	}
	setCopilotSttTestStatus(QString(), QString());
}

OpenMeeting::AgentSpeechSourceMode CopilotSettingsPage::selectedCopilotSpeechSourceMode() const {
	if (_copilotSttModeCustomBtn && _copilotSttModeCustomBtn->isChecked()) {
		return OpenMeeting::AgentSpeechSourceMode::CustomProfile;
	}
	if (_copilotSttModeFollowBtn && _copilotSttModeFollowBtn->isChecked()) {
		return OpenMeeting::AgentSpeechSourceMode::FollowChat;
	}
	return const_cast<CopilotSettingsPage *>(this)->copilotCoordinator().speechSourceMode();
}

bool CopilotSettingsPage::copilotSttReusesChatHost() const {
	if (selectedCopilotSpeechSourceMode() == OpenMeeting::AgentSpeechSourceMode::FollowChat) {
		return true;
	}
	const auto &coord = const_cast<CopilotSettingsPage *>(this)->copilotCoordinator();
	const QString sttUrl = _copilotSttBaseUrl
		? _copilotSttBaseUrl->text().trimmed()
		: coord.dictationSttBaseUrl();
	const auto sttProto = _copilotSttProtocolCombo
		? OpenMeeting::agentSpeechProtocolModeFromInt(_copilotSttProtocolCombo->currentData().toInt())
		: coord.dictationSpeechProtocol();
	const QString modelText = _copilotSttModelCombo
		? _copilotSttModelCombo->currentText().trimmed()
		: coord.dictationSttModel();
	const QString chatUrl = _copilotBaseUrl
		? _copilotBaseUrl->text().trimmed()
		: coord.profile().baseUrl;
	const auto effectiveProto = OpenMeeting::resolveEffectiveSpeechProtocol(
		sttProto, sttUrl, chatUrl, modelText);
	if (sttUrl.isEmpty()) {
		if (effectiveProto == OpenMeeting::AgentSpeechProtocolMode::DeepgramWs ||
			effectiveProto == OpenMeeting::AgentSpeechProtocolMode::AssemblyAiV3Ws ||
			effectiveProto == OpenMeeting::AgentSpeechProtocolMode::VolcengineBigModelWs ||
			effectiveProto == OpenMeeting::AgentSpeechProtocolMode::ElevenLabsScribeWs) {
			return OpenMeeting::AgentCoordinator::isSameSpeechOrigin(
				OpenMeeting::defaultStreamingSpeechEndpoint(effectiveProto), chatUrl);
		}
		if (effectiveProto == OpenMeeting::AgentSpeechProtocolMode::DashScopeInferenceWs ||
			effectiveProto == OpenMeeting::AgentSpeechProtocolMode::DashScopeQwenRealtimeWs) {
			return OpenMeeting::isDashScopeAgentHost(QUrl(chatUrl));
		}
		if (effectiveProto == OpenMeeting::AgentSpeechProtocolMode::XaiSttWs) {
			return OpenMeeting::isXaiAgentHost(QUrl(chatUrl));
		}
		return true;
	}
	return OpenMeeting::AgentCoordinator::isSameSpeechOrigin(sttUrl, chatUrl) ||
		OpenMeeting::AgentCoordinator::isSameSpeechOrigin(sttUrl, coord.profile().baseUrl);
}

void CopilotSettingsPage::maybeAutoRefreshModels() {
	refreshCopilotSpeechModels(true);
	if (!_copilotModelCombo || !_copilotBaseUrl || _copilotModelCombo->count() > 0) {
		return;
	}
	const QString currentBase = _copilotBaseUrl->text().trimmed();
	if (currentBase.isEmpty() || _copilotLastModelFetchBaseUrl == currentBase) {
		return;
	}
	if (!OpenMeeting::evaluateAgentEndpoint(currentBase).allowed) {
		return;
	}
	refreshCopilotModels(true);
}

void CopilotSettingsPage::syncControls() {
	if (!_copilotProfileName) {
		return;
	}
	auto &coord = copilotCoordinator();
	if (!_agentProfileConn) {
		_agentProfileConn = connect(&coord, &OpenMeeting::AgentCoordinator::profileChanged,
			this, [this] {
				if (!_updatingUi) {
					syncControls();
				}
			});
	}
	if (!_agentProfilesConn) {
		_agentProfilesConn = connect(&coord, &OpenMeeting::AgentCoordinator::profilesChanged,
			this, [this] {
				refreshCopilotProfileSelectors();
			});
	}
	if (!_agentSpeechProfilesConn) {
		_agentSpeechProfilesConn = connect(&coord, &OpenMeeting::AgentCoordinator::speechProfilesChanged,
			this, [this] {
				refreshCopilotSpeechProfileSelectors();
			});
	}
	if (!_agentProbeConn) {
		_agentProbeConn = connect(&coord, &OpenMeeting::AgentCoordinator::probeFinished,
			this, [this](bool success, const QString &message) {
				if (_copilotTestBtn) {
					_copilotTestBtn->setEnabled(true);
				}
				const QString modelName = _copilotModelCombo
					? _copilotModelCombo->currentText().trimmed()
					: QString();
				const QString okText = modelName.isEmpty()
					? QCoreApplication::translate("MeetingUI", "Connection verified.")
					: QCoreApplication::translate("MeetingUI", "Connection verified (%1).").arg(modelName);
				setCopilotTestStatus(
					success ? QStringLiteral("ok") : QStringLiteral("error"),
					success ? okText : message);
			});
	}
	if (!_agentSpeechProbeConn) {
		_agentSpeechProbeConn = connect(&coord, &OpenMeeting::AgentCoordinator::speechProbeFinished,
			this, [this](bool success, const QString &message) {
				if (_copilotSttTestBtn) {
					_copilotSttTestBtn->setEnabled(true);
				}
				setCopilotSttTestStatus(
					success ? QStringLiteral("ok") : QStringLiteral("error"),
					(success && message.trimmed().isEmpty())
						? QCoreApplication::translate("MeetingUI", "Connection verified.")
						: message);
			});
	}

	if (!_agentSpeechModelsConn) {
		_agentSpeechModelsConn = connect(&coord, &OpenMeeting::AgentCoordinator::speechModelsFetched,
			this, [this](bool success, const QStringList &, const QString &) {
				const QString before = _copilotSttModelCombo
					? _copilotSttModelCombo->currentText().trimmed()
					: QString();
				updateCopilotSttModelChoices(success);
				if (success && !_updatingUi && _copilotSttModelCombo &&
					_copilotSttModelCombo->currentText().trimmed() != before) {
					commitCopilotProfile(false);
				}
			});
	}

	if (!_agentModelsConn) {
		_agentModelsConn = connect(&coord, &OpenMeeting::AgentCoordinator::modelsFetched,
			this, [this](bool success, const QStringList &models, const QString &safeError) {
				if (_copilotRefreshModelsBtn) {
					_copilotRefreshModelsBtn->setEnabled(true);
				}
				if (!_copilotModelCombo) {
					return;
				}
				const bool prevUpdating = _updatingUi;
				_updatingUi = true;
				const QSignalBlocker comboBlocker(_copilotModelCombo);
				const QString currentText = _copilotModelCombo->currentText().trimmed();
				_copilotModelCombo->clear();
				for (const QString &m : models) {
					_copilotModelCombo->addItem(m);
				}
				bool switchedAwayFromMissingModel = false;
				if (!models.isEmpty()) {
					const int idx = currentText.isEmpty() ? -1 : _copilotModelCombo->findText(currentText);
					if (idx >= 0) {
						_copilotModelCombo->setCurrentIndex(idx);
					} else {
						// Previous endpoint's model does not exist on this endpoint -> switch to first available model
						_copilotModelCombo->setCurrentIndex(0);
						switchedAwayFromMissingModel = true;
					}
				} else if (!currentText.isEmpty()) {
					_copilotModelCombo->setEditText(currentText);
				}
				_updatingUi = prevUpdating;

				if (!models.isEmpty() && (currentText.isEmpty() || switchedAwayFromMissingModel)) {
					if (switchedAwayFromMissingModel) {
						resetCopilotTestStatus();
					}
					commitCopilotProfile(false);
				}
				updateCopilotSttModelChoices();
				updateCopilotReasoningCapabilityHint();
				if (success) {
					const int filtered = copilotCoordinator().lastFilteredNonChatModelCount();
					if (models.isEmpty()) {
						setCopilotModelHint(QCoreApplication::translate("MeetingUI",
							"No models returned; enter model name manually."));
					} else if (filtered > 0) {
						setCopilotModelHint(QCoreApplication::translate("MeetingUI",
							"Loaded %1 chat model(s) (%2 non-chat hidden).").arg(models.size()).arg(filtered));
					} else {
						setCopilotModelHint(QCoreApplication::translate("MeetingUI",
							"Loaded %1 model(s).").arg(models.size()));
					}
				} else {
					setCopilotModelHint(safeError, QStringLiteral("error"));
				}
			});
	}

	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	refreshCopilotProfileSelectors();
	const auto &p = coord.profile();
	_copilotProfileName->setText(p.name);
	const int protoIdx = _copilotProtocolCombo->findData(static_cast<int>(p.protocol));
	_copilotProtocolCombo->setCurrentIndex(protoIdx >= 0 ? protoIdx : 0);
	_copilotBaseUrl->setText(p.baseUrl);
	{
		const QSignalBlocker comboBlocker(_copilotModelCombo);
		const int modelIdx = _copilotModelCombo->findText(p.model);
		if (modelIdx >= 0) {
			_copilotModelCombo->setCurrentIndex(modelIdx);
		} else {
			_copilotModelCombo->setEditText(p.model);
		}
	}
	if (_copilotReasoningCombo) {
		const QSignalBlocker reasoningBlocker(_copilotReasoningCombo);
		const int rIdx = _copilotReasoningCombo->findData(static_cast<int>(p.reasoningEffort));
		_copilotReasoningCombo->setCurrentIndex(rIdx >= 0 ? rIdx : 0);
	}
	_copilotEncryptKeyCheck->setChecked(p.keyStorage == OpenMeeting::KeyStoragePolicy::EncryptedStore);
	if (_copilotPersistHistoryCheck) {
		_copilotPersistHistoryCheck->setChecked(p.persistChatHistory);
	}
	_copilotSystemPrompt->setPlainText(p.systemPrompt);

	QStringList headerLines;
	for (auto it = p.customHeaders.cbegin(); it != p.customHeaders.cend(); ++it) {
		headerLines.push_back(QStringLiteral("%1: %2").arg(it.key(), it.value()));
	}
	_copilotCustomHeaders->setPlainText(headerLines.join(QLatin1Char('\n')));
	_copilotMaxHistorySpin->setValue(p.maxContextMessages);
	_copilotTemperatureSpin->setValue(p.temperature);
	if (_copilotConnectTimeoutSpin) {
		const QSignalBlocker cToBlocker(_copilotConnectTimeoutSpin);
		_copilotConnectTimeoutSpin->setValue(p.connectTimeoutMs);
	}
	if (_copilotIdleTimeoutSpin) {
		const QSignalBlocker iToBlocker(_copilotIdleTimeoutSpin);
		_copilotIdleTimeoutSpin->setValue(p.idleTimeoutMs);
	}
	if (_copilotRefreshModelsBtn) {
		_copilotRefreshModelsBtn->setEnabled(!coord.isFetchingModels());
	}
	const bool isCustomSpeech = (coord.speechSourceMode() == OpenMeeting::AgentSpeechSourceMode::CustomProfile);
	if (_copilotSttModeFollowBtn && _copilotSttModeCustomBtn) {
		const QSignalBlocker followBlocker(_copilotSttModeFollowBtn);
		const QSignalBlocker customBlocker(_copilotSttModeCustomBtn);
		_copilotSttModeFollowBtn->setChecked(!isCustomSpeech);
		_copilotSttModeCustomBtn->setChecked(isCustomSpeech);
	}
	refreshCopilotSpeechProfileSelectors();
	const auto &sp = coord.speechProfile();
	if (_copilotSttProfileName) {
		_copilotSttProfileName->setText(sp.name);
	}
	if (_copilotSttBaseUrl) {
		_copilotSttBaseUrl->setText(sp.baseUrl);
	}
	if (_copilotSttProtocolCombo) {
		const QSignalBlocker sttProtoBlocker(_copilotSttProtocolCombo);
		const int pIdx = _copilotSttProtocolCombo->findData(static_cast<int>(coord.dictationSpeechProtocol()));
		_copilotSttProtocolCombo->setCurrentIndex(pIdx >= 0 ? pIdx : 0);
	}
	const QString syncedEffectiveSttUrl = coord.dictationSttBaseUrl().trimmed().isEmpty()
		? p.baseUrl
		: coord.dictationSttBaseUrl().trimmed();
	_copilotLastSttEndpointKey = OpenMeeting::AgentCoordinator::canonicalSpeechEndpointKey(syncedEffectiveSttUrl);
	_copilotSttModelAutoSelected = false;
	if (_copilotSttModelCombo) {
		const QSignalBlocker sttBlocker(_copilotSttModelCombo);
		_copilotSttModelCombo->setEditText(coord.dictationSttModel());
	}
	updateCopilotSttModelChoices();
	if (_copilotDictationLangCombo) {
		const QSignalBlocker langBlocker(_copilotDictationLangCombo);
		const QString lang = coord.dictationLanguage().trimmed();
		const int langIdx = _copilotDictationLangCombo->findData(lang);
		if (langIdx >= 0) {
			_copilotDictationLangCombo->setCurrentIndex(langIdx);
		} else {
			_copilotDictationLangCombo->setEditText(lang);
		}
	}
	if (_copilotSpeechTimeoutSpin) {
		const QSignalBlocker timeoutBlocker(_copilotSpeechTimeoutSpin);
		_copilotSpeechTimeoutSpin->setValue(qBound(0, coord.dictationSpeechTimeoutMs(), 10000));
	}
	if (_copilotVadThresholdSpin) {
		const QSignalBlocker vadDbBlocker(_copilotVadThresholdSpin);
		_copilotVadThresholdSpin->setValue(qBound(-60, coord.dictationVadThresholdDb(), -15));
	}
	if (_copilotVadSilenceCommitSpin) {
		const QSignalBlocker vadSilBlocker(_copilotVadSilenceCommitSpin);
		_copilotVadSilenceCommitSpin->setValue(qBound(200, coord.dictationVadSilenceCommitMs(), 3000));
	}
	if (_copilotSttResourceIdCombo) {
		const QSignalBlocker resBlocker(_copilotSttResourceIdCombo);
		const QString resId = sp.resourceId.trimmed().isEmpty()
			? QStringLiteral("volc.seedasr.sauc.duration")
			: sp.resourceId.trimmed();
		const int rIdx = _copilotSttResourceIdCombo->findText(resId);
		if (rIdx >= 0) {
			_copilotSttResourceIdCombo->setCurrentIndex(rIdx);
		} else {
			_copilotSttResourceIdCombo->setEditText(resId);
		}
	}
	if (_copilotSttWorkspaceEdit) {
		_copilotSttWorkspaceEdit->setText(sp.workspaceId);
	}
	if (_copilotSttPromptEdit) {
		_copilotSttPromptEdit->setText(coord.dictationPrompt());
	}
	updateCopilotSttProtocolUi();
	updateCopilotSttModeUi();
	updateCopilotBaseUrlPlaceholder();
	updateCopilotKeyStatus();
	updateCopilotSttKeyStatus();
	updateCopilotSttAppKeyStatus();
	updateCopilotReasoningCapabilityHint();
	updateCopilotAdvSummaries();
	if (_copilotTestBtn) {
		_copilotTestBtn->setEnabled(!coord.isProbing());
	}
	if (_copilotSttTestBtn) {
		_copilotSttTestBtn->setEnabled(!coord.isSpeechProbing());
	}
	_updatingUi = oldUpdating;
}

void CopilotSettingsPage::updateCopilotBaseUrlPlaceholder() {
	if (!_copilotBaseUrl) {
		return;
	}
	const auto protocol = OpenMeeting::agentProtocolDialectFromInt(
		_copilotProtocolCombo ? _copilotProtocolCombo->currentData().toInt() : 0);
	if (protocol == OpenMeeting::AgentProtocolDialect::AnthropicMessagesSse) {
		_copilotBaseUrl->setPlaceholderText(QStringLiteral("https://api.anthropic.com/v1"));
	} else if (protocol == OpenMeeting::AgentProtocolDialect::GoogleGeminiSse) {
		_copilotBaseUrl->setPlaceholderText(QStringLiteral("https://generativelanguage.googleapis.com/v1beta"));
	} else if (protocol == OpenMeeting::AgentProtocolDialect::CustomWebhookJson) {
		_copilotBaseUrl->setPlaceholderText(QStringLiteral("https://your-agent-webhook.example.com/chat"));
	} else {
		_copilotBaseUrl->setPlaceholderText(QStringLiteral("https://api.openai.com/v1 or http://127.0.0.1:11434/v1"));
	}
}

void CopilotSettingsPage::updateCopilotReasoningCapabilityHint() {
	if (!_copilotReasoningHintIcon || !_copilotModelCombo) {
		return;
	}
	const auto protocol = OpenMeeting::agentProtocolDialectFromInt(
		_copilotProtocolCombo ? _copilotProtocolCombo->currentData().toInt() : 0);
	if (protocol == OpenMeeting::AgentProtocolDialect::CustomWebhookJson) {
		setHintIcon(_copilotReasoningHintIcon, QCoreApplication::translate("MeetingUI",
			"Reasoning effort is not sent for the Custom Webhook protocol."));
		return;
	}
	const QString modelId = _copilotModelCombo->currentText().trimmed();
	if (modelId.isEmpty()) {
		if (protocol == OpenMeeting::AgentProtocolDialect::AnthropicMessagesSse) {
			setHintIcon(_copilotReasoningHintIcon, QCoreApplication::translate("MeetingUI",
				"Controls extended thinking budget sent to capable models. Keep Default unless overriding."));
		} else if (protocol == OpenMeeting::AgentProtocolDialect::GoogleGeminiSse) {
			setHintIcon(_copilotReasoningHintIcon, QCoreApplication::translate("MeetingUI",
				"Controls thinking budget/level sent to capable Gemini models. Keep Default unless overriding."));
		} else {
			setHintIcon(_copilotReasoningHintIcon, QCoreApplication::translate("MeetingUI",
				"Controls reasoning_effort sent to reasoning-capable models. Keep Default unless overriding."));
		}
		return;
	}
	const auto caps = copilotCoordinator().resolveModelCapabilities(modelId);
	QString hintText;
	if (caps.supportsReasoningEffort) {
		QStringList effortNames;
		for (const auto e : caps.supportedEfforts) {
			const QString label = OpenMeeting::reasoningEffortShortLabel(e);
			if (!label.isEmpty()) {
				effortNames.push_back(label);
			}
		}
		const QString levels = effortNames.join(QStringLiteral(" / "));
		if (caps.source == OpenMeeting::AgentModelCapabilitySource::ServerMetadata) {
			hintText = QCoreApplication::translate("MeetingUI",
				"Server /models metadata reports reasoning support (%1).").arg(levels);
		} else {
			hintText = QCoreApplication::translate("MeetingUI",
				"Detected as reasoning-capable by model name rule (%1). You can override if needed.").arg(levels);
		}
	} else if (caps.source == OpenMeeting::AgentModelCapabilitySource::ServerMetadata) {
		hintText = QCoreApplication::translate("MeetingUI",
			"Server /models metadata did not list reasoning parameters for this model; Default is recommended (manual override allowed).");
	} else {
		hintText = QCoreApplication::translate("MeetingUI",
			"No server capability metadata or known reasoning prefix detected; Default is recommended (manual override allowed).");
	}
	if (caps.supportsVision) {
		hintText += QLatin1Char(' ') + QCoreApplication::translate("MeetingUI", "Image input: supported.");
	} else if (caps.visionSource == OpenMeeting::AgentModelCapabilitySource::ServerMetadata) {
		hintText += QLatin1Char(' ') + QCoreApplication::translate("MeetingUI", "Image input: not listed by server metadata.");
	}
	setHintIcon(_copilotReasoningHintIcon, hintText);
}

OpenMeeting::AgentProfile CopilotSettingsPage::collectCopilotProfileFromUi() const {
	auto &coord = const_cast<CopilotSettingsPage *>(this)->copilotCoordinator();
	OpenMeeting::AgentProfile p = coord.profile();
	p.name = _copilotProfileName->text().trimmed();
	if (p.name.isEmpty()) {
		p.name = QStringLiteral("Custom Agent");
	}
	p.protocol = static_cast<OpenMeeting::AgentProtocolDialect>(
		_copilotProtocolCombo->currentData().toInt());
	p.baseUrl = _copilotBaseUrl->text().trimmed();
	p.model = _copilotModelCombo ? _copilotModelCombo->currentText().trimmed() : QString();
	if (_copilotReasoningCombo) {
		p.reasoningEffort = OpenMeeting::reasoningEffortFromInt(
			_copilotReasoningCombo->currentData().toInt());
	}
	p.keyStorage = _copilotEncryptKeyCheck->isChecked()
		? OpenMeeting::KeyStoragePolicy::EncryptedStore
		: OpenMeeting::KeyStoragePolicy::MemoryOnly;
	if (_copilotPersistHistoryCheck) {
		p.persistChatHistory = _copilotPersistHistoryCheck->isChecked();
	}
	p.systemPrompt = _copilotSystemPrompt->toPlainText();
	p.maxContextMessages = _copilotMaxHistorySpin->value();
	p.temperature = _copilotTemperatureSpin->value();
	if (_copilotConnectTimeoutSpin) {
		p.connectTimeoutMs = _copilotConnectTimeoutSpin->value();
	}
	if (_copilotIdleTimeoutSpin) {
		p.idleTimeoutMs = _copilotIdleTimeoutSpin->value();
	}

	QMap<QString, QString> headers;
	const auto lines = _copilotCustomHeaders->toPlainText().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
	for (const QString &rawLine : lines) {
		const int colon = rawLine.indexOf(QLatin1Char(':'));
		if (colon <= 0) {
			continue;
		}
		const QString key = rawLine.left(colon).trimmed();
		const QString val = rawLine.mid(colon + 1).trimmed();
		if (OpenMeeting::isValidCustomHeaderName(key) && OpenMeeting::isValidCustomHeaderValue(val)) {
			headers.insert(key, val);
		}
	}
	p.customHeaders = std::move(headers);
	return p;
}

OpenMeeting::AgentSpeechProfile CopilotSettingsPage::collectCopilotSpeechProfileFromUi() const {
	const auto &coord = const_cast<CopilotSettingsPage *>(this)->copilotCoordinator();
	OpenMeeting::AgentSpeechProfile sp = coord.speechProfile();
	if (_copilotSttProfileName) {
		const QString trimmedName = _copilotSttProfileName->text().trimmed();
		sp.name = trimmedName.isEmpty()
			? QCoreApplication::translate("MeetingUI", "Default Speech")
			: trimmedName;
	}
	if (_copilotSttProtocolCombo) {
		sp.protocol = OpenMeeting::agentSpeechProtocolModeFromInt(
			_copilotSttProtocolCombo->currentData().toInt());
	}
	if (_copilotSttBaseUrl) {
		sp.baseUrl = _copilotSttBaseUrl->text().trimmed();
	}
	if (_copilotSttModelCombo) {
		sp.model = _copilotSttModelCombo->currentText().trimmed();
	}
	if (_copilotSttResourceIdCombo) {
		const QString res = _copilotSttResourceIdCombo->currentText().trimmed();
		sp.resourceId = res.isEmpty() ? QStringLiteral("volc.seedasr.sauc.duration") : res;
	}
	if (_copilotSttWorkspaceEdit) {
		sp.workspaceId = _copilotSttWorkspaceEdit->text().trimmed();
	}
	return sp;
}

void CopilotSettingsPage::commitCopilotProfile(bool includeTypedApiKey) {
	if (!_copilotProfileName || _updatingUi) {
		return;
	}
	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	auto &coord = copilotCoordinator();
	const auto previous = coord.profile();
	const auto next = collectCopilotProfileFromUi();
	const bool connectionChanged = !previous.hasSameConnectionConfig(next);
	const bool isFollowChat = (coord.speechSourceMode() == OpenMeeting::AgentSpeechSourceMode::FollowChat);
	const auto nextSttProtocol = _copilotSttProtocolCombo
		? OpenMeeting::agentSpeechProtocolModeFromInt(_copilotSttProtocolCombo->currentData().toInt())
		: coord.dictationSpeechProtocol();
	const QString nextSttModel = _copilotSttModelCombo
		? _copilotSttModelCombo->currentText().trimmed()
		: coord.dictationSttModel();
	QString nextLang = coord.dictationLanguage();
	if (_copilotDictationLangCombo) {
		nextLang = _copilotDictationLangCombo->currentText().trimmed();
		const int langIdx = _copilotDictationLangCombo->currentIndex();
		if (langIdx >= 0 && nextLang == _copilotDictationLangCombo->itemText(langIdx).trimmed()) {
			nextLang = _copilotDictationLangCombo->itemData(langIdx).toString().trimmed();
		}
	}
	const int nextSpeechTimeoutMs = _copilotSpeechTimeoutSpin
		? _copilotSpeechTimeoutSpin->value()
		: coord.dictationSpeechTimeoutMs();
	const int nextVadThresholdDb = _copilotVadThresholdSpin
		? _copilotVadThresholdSpin->value()
		: coord.dictationVadThresholdDb();
	const int nextVadSilenceCommitMs = _copilotVadSilenceCommitSpin
		? _copilotVadSilenceCommitSpin->value()
		: coord.dictationVadSilenceCommitMs();
	const QString nextSttPrompt = _copilotSttPromptEdit
		? _copilotSttPromptEdit->text().trimmed()
		: coord.dictationPrompt();

	bool sttConnectionChanged =
		(coord.dictationSpeechProtocol() != nextSttProtocol) ||
		(coord.dictationSttModel() != nextSttModel) ||
		(coord.dictationLanguage() != nextLang) ||
		(coord.dictationPrompt() != nextSttPrompt);

	QString nextSttBaseUrl;
	if (!isFollowChat) {
		const auto nextSpeechProf = collectCopilotSpeechProfileFromUi();
		nextSttBaseUrl = nextSpeechProf.baseUrl;
		sttConnectionChanged = sttConnectionChanged || !coord.speechProfile().hasSameConnectionConfig(nextSpeechProf);
	}
	const bool sharedHostChanged =
		(previous.baseUrl != next.baseUrl || previous.customHeaders != next.customHeaders) &&
		(isFollowChat ||
			nextSttBaseUrl.isEmpty() ||
			OpenMeeting::AgentCoordinator::isSameSpeechOrigin(nextSttBaseUrl, previous.baseUrl) ||
			OpenMeeting::AgentCoordinator::isSameSpeechOrigin(nextSttBaseUrl, next.baseUrl));
	QString typedKey = (includeTypedApiKey && _copilotApiKey)
		? _copilotApiKey->text().trimmed()
		: QString();
	const bool keyChanged = !typedKey.isEmpty();

	coord.setProfile(next);
	if (isFollowChat) {
		if (_copilotSttProtocolCombo) {
			coord.setDictationSpeechProtocol(nextSttProtocol);
		}
		if (_copilotSttModelCombo) {
			coord.setDictationSttModel(nextSttModel);
		}
	} else {
		coord.setSpeechProfile(collectCopilotSpeechProfileFromUi());
		refreshCopilotSpeechProfileSelectors();
	}
	if (_copilotDictationLangCombo) {
		coord.setDictationLanguage(nextLang);
	}
	if (_copilotSpeechTimeoutSpin) {
		coord.setDictationSpeechTimeoutMs(nextSpeechTimeoutMs);
	}
	if (_copilotVadThresholdSpin) {
		coord.setDictationVadThresholdDb(nextVadThresholdDb);
	}
	if (_copilotVadSilenceCommitSpin) {
		coord.setDictationVadSilenceCommitMs(nextVadSilenceCommitMs);
	}
	if (_copilotSttPromptEdit) {
		coord.setDictationPrompt(nextSttPrompt);
	}
	if (keyChanged) {
		coord.saveApiKey(typedKey);
		wipeQString(typedKey);
		clearCopilotKeyEditor();
	}
	bool sttKeyChanged = false;
	bool sttAppKeyChanged = false;
	if (!isFollowChat) {
		QString typedSttKey = (includeTypedApiKey && _copilotSttApiKey)
			? _copilotSttApiKey->text().trimmed()
			: QString();
		sttKeyChanged = !typedSttKey.isEmpty();
		if (sttKeyChanged) {
			coord.saveDictationApiKey(typedSttKey);
			wipeQString(typedSttKey);
			wipeAndClearLineEdit(_copilotSttApiKey);
		}
		QString typedAppKey = (includeTypedApiKey && _copilotSttAppKey)
			? _copilotSttAppKey->text().trimmed()
			: QString();
		sttAppKeyChanged = !typedAppKey.isEmpty();
		if (sttAppKeyChanged) {
			coord.saveDictationAppKey(typedAppKey);
			wipeQString(typedAppKey);
			wipeAndClearLineEdit(_copilotSttAppKey);
		}
	}
	if (connectionChanged || keyChanged) {
		resetCopilotTestStatus();
	}
	const bool sharedKeyChanged = keyChanged &&
		(isFollowChat || (!coord.hasDictationApiKey() &&
			(nextSttBaseUrl.isEmpty() || OpenMeeting::AgentCoordinator::isSameSpeechOrigin(nextSttBaseUrl, next.baseUrl))));
	if (sttConnectionChanged || sttKeyChanged || sttAppKeyChanged || sharedHostChanged || sharedKeyChanged) {
		resetCopilotSttTestStatus();
	}
	updateCopilotKeyStatus();
	updateCopilotSttKeyStatus();
	updateCopilotSttAppKeyStatus();
	updateCopilotSttModeUi();
	_updatingUi = oldUpdating;
}

void CopilotSettingsPage::refreshCopilotProfileSelectors() {
	const auto &coord = const_cast<CopilotSettingsPage *>(this)->copilotCoordinator();
	const auto &profiles = coord.profiles();
	if (_copilotProfileSelector) {
		const QSignalBlocker blocker(_copilotProfileSelector);
		_copilotProfileSelector->clear();
		int activeIdx = 0;
		for (int i = 0; i < profiles.size(); ++i) {
			const auto &prof = profiles[i];
			const QString label = prof.name.trimmed().isEmpty() ? prof.id : prof.name.trimmed();
			_copilotProfileSelector->addItem(label, prof.id);
			if (prof.id == coord.activeProfileId()) {
				activeIdx = i;
			}
		}
		_copilotProfileSelector->setCurrentIndex(activeIdx);
	}
	if (_copilotProfileMoreBtn) {
		_copilotProfileMoreBtn->setEnabled(!profiles.isEmpty());
	}
	if (_translationProfileCombo) {
		const QSignalBlocker blocker(_translationProfileCombo);
		const QString desiredId = _translationProfileCombo->count() > 0
			? _translationProfileCombo->currentData().toString().trimmed()
			: _session.mediaPreferences().translationProfileId.trimmed();
		_translationProfileCombo->clear();
		_translationProfileCombo->addItem(
			QCoreApplication::translate("MeetingUI", "Follow Active Copilot Profile"),
			QString());
		for (const auto &prof : profiles) {
			const QString name = prof.name.trimmed().isEmpty() ? prof.id : prof.name.trimmed();
			const QString label = prof.model.trimmed().isEmpty()
				? name
				: QStringLiteral("%1 (%2)").arg(name, prof.model.trimmed());
			_translationProfileCombo->addItem(label, prof.id);
		}
		int idx = desiredId.isEmpty() ? 0 : _translationProfileCombo->findData(desiredId);
		if (idx < 0) {
			idx = 0;
		}
		_translationProfileCombo->setCurrentIndex(idx);
	}
}

void CopilotSettingsPage::onCopilotProfileSelected(int) {
	if (_updatingUi || !_copilotProfileSelector) {
		return;
	}
	auto &coord = copilotCoordinator();
	const QString targetId = _copilotProfileSelector->currentData().toString().trimmed();
	if (targetId.isEmpty() || targetId == coord.activeProfileId()) {
		return;
	}
	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	commitCopilotProfile(false);
	clearCopilotKeyEditor();
	const bool switched = coord.switchProfile(targetId);
	_updatingUi = oldUpdating;
	if (switched) {
		_copilotLastModelFetchBaseUrl.clear();
		if (_copilotModelCombo) {
			const bool prev = _updatingUi;
			_updatingUi = true;
			const QSignalBlocker b(_copilotModelCombo);
			_copilotModelCombo->clear();
			_updatingUi = prev;
		}
		resetCopilotTestStatus();
		resetCopilotModelHint();
		syncControls();
		maybeAutoRefreshModels();
	}
}

void CopilotSettingsPage::createNewCopilotProfile(bool duplicateCurrent) {
	if (_updatingUi) {
		return;
	}
	auto &coord = copilotCoordinator();
	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	commitCopilotProfile(false);
	clearCopilotKeyEditor();
	const OpenMeeting::AgentProfile currentProfile = coord.profile();
	const QString newName = duplicateCurrent
		? QCoreApplication::translate("MeetingUI", "%1 (Copy)").arg(
			currentProfile.name.trimmed().isEmpty() ? currentProfile.id : currentProfile.name.trimmed())
		: QString();
	coord.createProfile(newName, duplicateCurrent ? &currentProfile : nullptr);
	_updatingUi = oldUpdating;

	_copilotLastModelFetchBaseUrl.clear();
	if (_copilotModelCombo) {
		const bool prev = _updatingUi;
		_updatingUi = true;
		const QSignalBlocker b(_copilotModelCombo);
		_copilotModelCombo->clear();
		_updatingUi = prev;
	}
	resetCopilotTestStatus();
	resetCopilotModelHint();
	syncControls();
	if (_copilotProfileName) {
		_copilotProfileName->setFocus();
		_copilotProfileName->selectAll();
	}
}

void CopilotSettingsPage::deleteCurrentCopilotProfile() {
	if (_updatingUi) {
		return;
	}
	auto &coord = copilotCoordinator();
	if (coord.profiles().size() <= 1) {
		return;
	}
	const QString currentName = coord.profile().name.trimmed().isEmpty()
		? coord.activeProfileId()
		: coord.profile().name.trimmed();
	QMessageBox confirmBox(
		QMessageBox::Question,
		QCoreApplication::translate("MeetingUI", "Delete Connection Profile"),
		QCoreApplication::translate(
			"MeetingUI",
			"Delete profile \"%1\" and remove its saved API key?").arg(currentName),
		QMessageBox::Yes | QMessageBox::No,
		this);
	const auto confirmTone = AppTheme::effectiveTheme() == AppTheme::Theme::Dark
		? AppTheme::Tone::Dark
		: AppTheme::Tone::Light;
	AppTheme::setTone(confirmBox, confirmTone);
	if (confirmBox.exec() != QMessageBox::Yes) {
		return;
	}

	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	clearCopilotKeyEditor();
	const bool deleted = coord.deleteProfile(coord.activeProfileId());
	_updatingUi = oldUpdating;
	if (deleted) {
		_copilotLastModelFetchBaseUrl.clear();
		if (_copilotModelCombo) {
			const bool prev = _updatingUi;
			_updatingUi = true;
			const QSignalBlocker b(_copilotModelCombo);
			_copilotModelCombo->clear();
			_updatingUi = prev;
		}
		resetCopilotTestStatus();
		resetCopilotModelHint();
		syncControls();
		maybeAutoRefreshModels();
	}
}

void CopilotSettingsPage::showCopilotProfileMenu() {
	if (!_copilotProfileMoreBtn || _updatingUi) {
		return;
	}
	QMenu menu(this);
	menu.setObjectName(QStringLiteral("settingsCopilotProfileMenu"));
	const auto tone = AppTheme::effectiveTheme() == AppTheme::Theme::Dark
		? AppTheme::Tone::Dark
		: AppTheme::Tone::Light;
	AppTheme::styleMenu(menu, tone);

	auto *dupAct = menu.addAction(
		AppTheme::icon(AppTheme::Icon::Copy, AppTheme::ColorRole::Text),
		QCoreApplication::translate("MeetingUI", "Duplicate Current Profile"));
	dupAct->setObjectName(QStringLiteral("settingsCopilotDuplicateProfile"));
	connect(dupAct, &QAction::triggered, this, [this] {
		createNewCopilotProfile(true);
	});

	const bool canDelete = copilotCoordinator().profiles().size() > 1;
	auto *delAct = menu.addAction(
		AppTheme::icon(AppTheme::Icon::Trash, AppTheme::ColorRole::Danger),
		QCoreApplication::translate("MeetingUI", "Delete Current Profile"));
	delAct->setObjectName(QStringLiteral("settingsCopilotDeleteProfile"));
	delAct->setEnabled(canDelete);
	connect(delAct, &QAction::triggered, this, [this] {
		deleteCurrentCopilotProfile();
	});

	menu.exec(_copilotProfileMoreBtn->mapToGlobal(QPoint(0, _copilotProfileMoreBtn->height() + 4)));
}

void CopilotSettingsPage::refreshCopilotSpeechProfileSelectors() {
	if (!_copilotSttProfileSelector) {
		return;
	}
	const auto &coord = const_cast<CopilotSettingsPage *>(this)->copilotCoordinator();
	const auto &profiles = coord.speechProfiles();
	const QSignalBlocker blocker(_copilotSttProfileSelector);
	_copilotSttProfileSelector->clear();
	int activeIdx = 0;
	for (int i = 0; i < profiles.size(); ++i) {
		const auto &sp = profiles[i];
		const QString label = sp.name.trimmed().isEmpty() ? sp.id : sp.name.trimmed();
		_copilotSttProfileSelector->addItem(label, sp.id);
		if (sp.id == coord.activeSpeechProfileId()) {
			activeIdx = i;
		}
	}
	_copilotSttProfileSelector->setCurrentIndex(activeIdx);
	if (_copilotSttProfileMoreBtn) {
		_copilotSttProfileMoreBtn->setEnabled(!profiles.isEmpty());
	}
}

void CopilotSettingsPage::onCopilotSpeechSourceModeChanged(OpenMeeting::AgentSpeechSourceMode mode) {
	if (_updatingUi) {
		return;
	}
	auto &coord = copilotCoordinator();
	if (coord.speechSourceMode() == mode) {
		updateCopilotSttModeUi();
		return;
	}
	const auto previousFollowProto =
		(coord.speechSourceMode() == OpenMeeting::AgentSpeechSourceMode::FollowChat && _copilotSttProtocolCombo)
			? OpenMeeting::agentSpeechProtocolModeFromInt(_copilotSttProtocolCombo->currentData().toInt())
			: OpenMeeting::AgentSpeechProtocolMode::Auto;
	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	// Commit any pending edits in the previous mode before switching cleanly.
	if (coord.speechSourceMode() == OpenMeeting::AgentSpeechSourceMode::FollowChat) {
		if (_copilotSttProtocolCombo) {
			coord.setDictationSpeechProtocol(
				OpenMeeting::agentSpeechProtocolModeFromInt(_copilotSttProtocolCombo->currentData().toInt()));
		}
		if (_copilotSttModelCombo) {
			coord.setDictationSttModel(_copilotSttModelCombo->currentText().trimmed());
		}
	} else {
		coord.setSpeechProfile(collectCopilotSpeechProfileFromUi());
		wipeAndClearLineEdit(_copilotSttApiKey);
		wipeAndClearLineEdit(_copilotSttAppKey);
	}
	coord.setSpeechSourceMode(mode);
	_copilotLastSttEndpointKey.clear();
	_copilotLastSttModelFetchUrl.clear();
	_copilotSttModelAutoSelected = false;
	_updatingUi = oldUpdating;

	resetCopilotSttTestStatus();
	syncControls();
	if (mode == OpenMeeting::AgentSpeechSourceMode::CustomProfile) {
		const auto &sp = coord.speechProfile();
		const bool untouchedCustomProfile =
			(sp.protocol == OpenMeeting::AgentSpeechProtocolMode::Auto) &&
			sp.baseUrl.trimmed().isEmpty() &&
			!coord.hasDictationApiKey();
		if (untouchedCustomProfile && previousFollowProto != OpenMeeting::AgentSpeechProtocolMode::Auto) {
			if (_copilotSttProtocolCombo) {
				const QSignalBlocker protoBlocker(_copilotSttProtocolCombo);
				const int pIdx = _copilotSttProtocolCombo->findData(static_cast<int>(previousFollowProto));
				if (pIdx >= 0) {
					_copilotSttProtocolCombo->setCurrentIndex(pIdx);
				}
			}
			applyCopilotSpeechProtocolPreset(previousFollowProto, /*forceOverwrite=*/false);
			updateCopilotSttProtocolUi();
			updateCopilotSttKeyStatus();
			commitCopilotProfile(false);
		} else if (sp.protocol != OpenMeeting::AgentSpeechProtocolMode::Auto &&
			_copilotSttBaseUrl &&
			_copilotSttBaseUrl->text().trimmed().isEmpty()) {
			applyCopilotSpeechProtocolPreset(sp.protocol, /*forceOverwrite=*/false);
			updateCopilotSttProtocolUi();
			updateCopilotSttKeyStatus();
			commitCopilotProfile(false);
		}
		refreshCopilotSpeechModels(true);
	}
}

void CopilotSettingsPage::onCopilotSpeechProfileSelected(int) {
	if (_updatingUi || !_copilotSttProfileSelector) {
		return;
	}
	auto &coord = copilotCoordinator();
	const QString targetId = _copilotSttProfileSelector->currentData().toString().trimmed();
	if (targetId.isEmpty() || targetId == coord.activeSpeechProfileId()) {
		return;
	}
	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	if (coord.speechSourceMode() == OpenMeeting::AgentSpeechSourceMode::CustomProfile) {
		coord.setSpeechProfile(collectCopilotSpeechProfileFromUi());
	}
	wipeAndClearLineEdit(_copilotSttApiKey);
	wipeAndClearLineEdit(_copilotSttAppKey);
	const bool switched = coord.switchSpeechProfile(targetId);
	_updatingUi = oldUpdating;
	if (switched) {
		_copilotLastSttModelFetchUrl.clear();
		_copilotLastSttEndpointKey.clear();
		_copilotSttModelAutoSelected = false;
		resetCopilotSttTestStatus();
		syncControls();
		refreshCopilotSpeechModels(true);
	}
}

void CopilotSettingsPage::createNewCopilotSpeechProfile(bool duplicateCurrent) {
	if (_updatingUi) {
		return;
	}
	auto &coord = copilotCoordinator();
	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	if (coord.speechSourceMode() == OpenMeeting::AgentSpeechSourceMode::CustomProfile) {
		coord.setSpeechProfile(collectCopilotSpeechProfileFromUi());
	}
	wipeAndClearLineEdit(_copilotSttApiKey);
	wipeAndClearLineEdit(_copilotSttAppKey);
	const OpenMeeting::AgentSpeechProfile currentSpeechProfile = coord.speechProfile();
	const QString newName = duplicateCurrent
		? QCoreApplication::translate("MeetingUI", "%1 (Copy)").arg(
			currentSpeechProfile.name.trimmed().isEmpty()
				? currentSpeechProfile.id
				: currentSpeechProfile.name.trimmed())
		: QString();
	coord.createSpeechProfile(newName, duplicateCurrent ? &currentSpeechProfile : nullptr);
	_updatingUi = oldUpdating;

	_copilotLastSttModelFetchUrl.clear();
	_copilotLastSttEndpointKey.clear();
	_copilotSttModelAutoSelected = false;
	resetCopilotSttTestStatus();
	syncControls();
	if (duplicateCurrent) {
		refreshCopilotSpeechModels(true);
	}
	if (_copilotSttProfileName) {
		_copilotSttProfileName->setFocus();
		_copilotSttProfileName->selectAll();
	}
}

void CopilotSettingsPage::deleteCurrentCopilotSpeechProfile() {
	if (_updatingUi) {
		return;
	}
	auto &coord = copilotCoordinator();
	if (coord.speechProfiles().size() <= 1) {
		return;
	}
	const QString currentName = coord.speechProfile().name.trimmed().isEmpty()
		? coord.activeSpeechProfileId()
		: coord.speechProfile().name.trimmed();
	QMessageBox confirmBox(
		QMessageBox::Question,
		QCoreApplication::translate("MeetingUI", "Delete Speech Profile"),
		QCoreApplication::translate(
			"MeetingUI",
			"Delete speech profile \"%1\" and remove its saved API keys?").arg(currentName),
		QMessageBox::Yes | QMessageBox::No,
		this);
	const auto tone = AppTheme::effectiveTheme() == AppTheme::Theme::Dark
		? AppTheme::Tone::Dark
		: AppTheme::Tone::Light;
	AppTheme::setTone(confirmBox, tone);
	if (confirmBox.exec() != QMessageBox::Yes) {
		return;
	}

	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	wipeAndClearLineEdit(_copilotSttApiKey);
	wipeAndClearLineEdit(_copilotSttAppKey);
	const bool deleted = coord.deleteSpeechProfile(coord.activeSpeechProfileId());
	_updatingUi = oldUpdating;
	if (deleted) {
		_copilotLastSttModelFetchUrl.clear();
		_copilotLastSttEndpointKey.clear();
		_copilotSttModelAutoSelected = false;
		resetCopilotSttTestStatus();
		syncControls();
		refreshCopilotSpeechModels(true);
	}
}

void CopilotSettingsPage::showCopilotSpeechProfileMenu() {
	if (!_copilotSttProfileMoreBtn || _updatingUi) {
		return;
	}
	QMenu menu(this);
	menu.setObjectName(QStringLiteral("settingsCopilotSttProfileMenu"));
	const auto tone = AppTheme::effectiveTheme() == AppTheme::Theme::Dark
		? AppTheme::Tone::Dark
		: AppTheme::Tone::Light;
	AppTheme::styleMenu(menu, tone);

	auto *dupAct = menu.addAction(
		AppTheme::icon(AppTheme::Icon::Copy, AppTheme::ColorRole::Text),
		QCoreApplication::translate("MeetingUI", "Duplicate Speech Profile"));
	dupAct->setObjectName(QStringLiteral("settingsCopilotSttDuplicateProfile"));
	connect(dupAct, &QAction::triggered, this, [this] {
		createNewCopilotSpeechProfile(true);
	});

	const bool canDelete = copilotCoordinator().speechProfiles().size() > 1;
	auto *delAct = menu.addAction(
		AppTheme::icon(AppTheme::Icon::Trash, AppTheme::ColorRole::Danger),
		QCoreApplication::translate("MeetingUI", "Delete Speech Profile"));
	delAct->setObjectName(QStringLiteral("settingsCopilotSttDeleteProfile"));
	delAct->setEnabled(canDelete);
	connect(delAct, &QAction::triggered, this, [this] {
		deleteCurrentCopilotSpeechProfile();
	});

	menu.exec(_copilotSttProfileMoreBtn->mapToGlobal(QPoint(0, _copilotSttProfileMoreBtn->height() + 4)));
}

void CopilotSettingsPage::setSubTab(int index) {
	if (!_copilotSubPages || index < 0 || index >= _copilotSubPages->count()) {
		return;
	}
	if (_copilotSubPages->currentIndex() != index) {
		_copilotSubPages->setCurrentIndex(index);
	}
	if (index >= 0 && index < static_cast<int>(_copilotSubTabButtons.size()) && _copilotSubTabButtons[index]) {
		const QSignalBlocker blocker(_copilotSubTabButtons[index]);
		_copilotSubTabButtons[index]->setChecked(true);
	}
}

int CopilotSettingsPage::currentSubTab() const {
	return _copilotSubPages ? _copilotSubPages->currentIndex() : 0;
}

void CopilotSettingsPage::updateCopilotAdvSummaries() {
	if (_copilotChatAdvSection) {
		QStringList custom;
		if (_copilotCustomHeaders && !_copilotCustomHeaders->toPlainText().trimmed().isEmpty()) {
			custom.push_back(QCoreApplication::translate("MeetingUI", "Custom headers"));
		}
		if (_copilotMaxHistorySpin && _copilotMaxHistorySpin->value() != 16) {
			custom.push_back(QCoreApplication::translate("MeetingUI", "%1 turns").arg(_copilotMaxHistorySpin->value()));
		}
		if (_copilotTemperatureSpin && std::abs(_copilotTemperatureSpin->value() - 0.7) > 0.001) {
			custom.push_back(QCoreApplication::translate("MeetingUI", "Temp %1").arg(_copilotTemperatureSpin->value(), 0, 'f', 2));
		}
		if (_copilotConnectTimeoutSpin && _copilotConnectTimeoutSpin->value() != 10000) {
			custom.push_back(QCoreApplication::translate("MeetingUI", "Connect %1 ms").arg(_copilotConnectTimeoutSpin->value()));
		}
		if (_copilotIdleTimeoutSpin && _copilotIdleTimeoutSpin->value() != 30000) {
			custom.push_back(QCoreApplication::translate("MeetingUI", "Idle %1 ms").arg(_copilotIdleTimeoutSpin->value()));
		}
		if (_copilotEncryptKeyCheck && !_copilotEncryptKeyCheck->isChecked()) {
			custom.push_back(QCoreApplication::translate("MeetingUI", "Session-only key"));
		}
		if (_copilotPersistHistoryCheck && !_copilotPersistHistoryCheck->isChecked()) {
			custom.push_back(QCoreApplication::translate("MeetingUI", "History not persisted"));
		}
		_copilotChatAdvSection->setSummary(
			QCoreApplication::translate("MeetingUI", "Default · 16 turns · 0.70 temp · 10s / 30s"),
			custom);
	}

	if (_copilotSttAdvSection) {
		QStringList custom;
		if (_copilotSttPromptEdit && !_copilotSttPromptEdit->text().trimmed().isEmpty()) {
			custom.push_back(QCoreApplication::translate("MeetingUI", "Recognition prompt"));
		}
		if (_copilotVadThresholdSpin && _copilotVadThresholdSpin->value() != -42) {
			custom.push_back(QCoreApplication::translate("MeetingUI", "VAD %1 dBFS").arg(_copilotVadThresholdSpin->value()));
		}
		if (_copilotVadSilenceCommitSpin && _copilotVadSilenceCommitSpin->value() != 600) {
			custom.push_back(QCoreApplication::translate("MeetingUI", "Silence %1 ms").arg(_copilotVadSilenceCommitSpin->value()));
		}
		_copilotSttAdvSection->setSummary(
			QCoreApplication::translate("MeetingUI", "Default · -42 dBFS · 600 ms"),
			custom);
	}

	if (_copilotTrAdvSection) {
		QStringList custom;
		if (_translationProfileCombo && !_translationProfileCombo->currentData().toString().trimmed().isEmpty()) {
			custom.push_back(QCoreApplication::translate("MeetingUI", "Dedicated translation profile"));
		}
		if (_translationModelEdit && !_translationModelEdit->text().trimmed().isEmpty()) {
			custom.push_back(QCoreApplication::translate("MeetingUI", "Model: %1").arg(_translationModelEdit->text().trimmed()));
		}
		if (_translationPromptEdit && !_translationPromptEdit->toPlainText().trimmed().isEmpty()) {
			custom.push_back(QCoreApplication::translate("MeetingUI", "Custom translation prompt"));
		}
		if (_translationMaxConcurrentStreamsSpin && _translationMaxConcurrentStreamsSpin->value() != 3) {
			custom.push_back(QCoreApplication::translate("MeetingUI", "%1 streams").arg(_translationMaxConcurrentStreamsSpin->value()));
		}
		if (_translationIdleReleaseSpin && _translationIdleReleaseSpin->value() != 12) {
			custom.push_back(QCoreApplication::translate("MeetingUI", "Idle release %1 s").arg(_translationIdleReleaseSpin->value()));
		}
		_copilotTrAdvSection->setSummary(
			QCoreApplication::translate("MeetingUI", "Default · Follow active profile · 3 streams · 12s"),
			custom);
	}
}

void CopilotSettingsPage::updateCopilotSttModeUi() {
	const auto &coord = const_cast<CopilotSettingsPage *>(this)->copilotCoordinator();
	const bool isCustomMode =
		(selectedCopilotSpeechSourceMode() == OpenMeeting::AgentSpeechSourceMode::CustomProfile);

	if (_copilotSttProfileToolbar) {
		_copilotSttProfileToolbar->setVisible(isCustomMode);
	}
	if (_copilotSttFollowBanner) {
		_copilotSttFollowBanner->setVisible(!isCustomMode);
	}
	if (_copilotSttProfileNameBlock) {
		_copilotSttProfileNameBlock->setVisible(isCustomMode);
	}
	if (_copilotSttEndpointContainer) {
		_copilotSttEndpointContainer->setVisible(isCustomMode);
	}
	if (_copilotSttScopeBadge) {
		_copilotSttScopeBadge->setText(isCustomMode
			? QCoreApplication::translate("MeetingUI", "Per Speech Profile")
			: QCoreApplication::translate("MeetingUI", "Follows Chat"));
		_copilotSttScopeBadge->setProperty("badgeTone",
			isCustomMode ? QStringLiteral("accent") : QStringLiteral("muted"));
		if (auto *st = _copilotSttScopeBadge->style()) {
			st->unpolish(_copilotSttScopeBadge);
			st->polish(_copilotSttScopeBadge);
		}
	}
	if (_copilotSttFollowLabel && !isCustomMode) {
		QString chatName = _copilotProfileName
			? _copilotProfileName->text().trimmed()
			: coord.profile().name.trimmed();
		if (chatName.isEmpty()) {
			chatName = QCoreApplication::translate("MeetingUI", "Active Chat profile");
		}
		const QString chatUrl = _copilotBaseUrl
			? _copilotBaseUrl->text().trimmed()
			: coord.profile().baseUrl.trimmed();
		const QUrl chatParsed = QUrl::fromUserInput(chatUrl);
		const QString hostDisplay = (chatParsed.isValid() && !chatParsed.host().isEmpty())
			? chatParsed.host()
			: QString();
		const bool hasChatKey = coord.hasApiKey() ||
			(_copilotApiKey && !_copilotApiKey->text().trimmed().isEmpty());
		if (hostDisplay.isEmpty()) {
			_copilotSttFollowLabel->setText(
				QCoreApplication::translate(
					"MeetingUI",
					"Reusing \"%1\" — configure a Base URL in \"Models & Chat\" first")
					.arg(chatName));
		} else if (hasChatKey) {
			_copilotSttFollowLabel->setText(
				QCoreApplication::translate(
					"MeetingUI",
					"Reusing \"%1\" (%2) — endpoint URL & API key synced automatically")
					.arg(chatName, hostDisplay));
		} else {
			_copilotSttFollowLabel->setText(
				QCoreApplication::translate(
					"MeetingUI",
					"Reusing \"%1\" (%2) — endpoint synced (no Chat API key saved)")
					.arg(chatName, hostDisplay));
		}
	}
}

void CopilotSettingsPage::updateCopilotTranslationLinkedUi() {
	const bool enabled = _translationEnableOutput && _translationEnableOutput->isChecked();
	if (_translationTargetContainer) {
		_translationTargetContainer->setVisible(enabled);
	}
	if (_captionStyleReuseHint) {
		_captionStyleReuseHint->setVisible(!enabled);
	}
	if (_translationOverrideGroup) {
		_translationOverrideGroup->setEnabled(enabled);
	}
	if (_translationOverrideInactiveHint) {
		_translationOverrideInactiveHint->setVisible(!enabled);
	}
	updateCopilotAdvSummaries();
}

void CopilotSettingsPage::resetCopilotChatAdvancedDefaults() {
	if (_updatingUi) {
		return;
	}
	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	if (_copilotCustomHeaders) {
		const QSignalBlocker b(_copilotCustomHeaders);
		_copilotCustomHeaders->clear();
	}
	if (_copilotMaxHistorySpin) {
		const QSignalBlocker b(_copilotMaxHistorySpin);
		_copilotMaxHistorySpin->setValue(16);
	}
	if (_copilotTemperatureSpin) {
		const QSignalBlocker b(_copilotTemperatureSpin);
		_copilotTemperatureSpin->setValue(0.70);
	}
	if (_copilotConnectTimeoutSpin) {
		const QSignalBlocker b(_copilotConnectTimeoutSpin);
		_copilotConnectTimeoutSpin->setValue(10000);
	}
	if (_copilotIdleTimeoutSpin) {
		const QSignalBlocker b(_copilotIdleTimeoutSpin);
		_copilotIdleTimeoutSpin->setValue(30000);
	}
	if (_copilotEncryptKeyCheck) {
		const QSignalBlocker b(_copilotEncryptKeyCheck);
		_copilotEncryptKeyCheck->setChecked(true);
	}
	if (_copilotPersistHistoryCheck) {
		const QSignalBlocker b(_copilotPersistHistoryCheck);
		_copilotPersistHistoryCheck->setChecked(true);
	}
	_updatingUi = oldUpdating;
	resetCopilotTestStatus();
	if (copilotSttReusesChatHost()) {
		resetCopilotSttTestStatus();
	}
	commitCopilotProfile(false);
	updateCopilotAdvSummaries();
}

void CopilotSettingsPage::resetCopilotSttAdvancedDefaults() {
	if (_updatingUi) {
		return;
	}
	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	if (_copilotSttPromptEdit) {
		const QSignalBlocker b(_copilotSttPromptEdit);
		_copilotSttPromptEdit->clear();
	}
	if (_copilotVadThresholdSpin) {
		const QSignalBlocker b(_copilotVadThresholdSpin);
		_copilotVadThresholdSpin->setValue(-42);
	}
	if (_copilotVadSilenceCommitSpin) {
		const QSignalBlocker b(_copilotVadSilenceCommitSpin);
		_copilotVadSilenceCommitSpin->setValue(600);
	}
	_updatingUi = oldUpdating;
	resetCopilotSttTestStatus();
	commitCopilotProfile(false);
	updateCopilotAdvSummaries();
}

void CopilotSettingsPage::resetCopilotTranslationAdvancedDefaults() {
	if (_updatingUi) {
		return;
	}
	if (_translationPromptCommitTimer && _translationPromptCommitTimer->isActive()) {
		_translationPromptCommitTimer->stop();
	}
	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	if (_translationProfileCombo) {
		const QSignalBlocker b(_translationProfileCombo);
		_translationProfileCombo->setCurrentIndex(0);
	}
	if (_translationModelEdit) {
		const QSignalBlocker b(_translationModelEdit);
		_translationModelEdit->clear();
	}
	if (_translationPromptEdit) {
		const QSignalBlocker b(_translationPromptEdit);
		_translationPromptEdit->clear();
	}
	if (_translationMaxConcurrentStreamsSpin) {
		const QSignalBlocker b(_translationMaxConcurrentStreamsSpin);
		_translationMaxConcurrentStreamsSpin->setValue(3);
	}
	if (_translationIdleReleaseSpin) {
		const QSignalBlocker b(_translationIdleReleaseSpin);
		_translationIdleReleaseSpin->setValue(12);
	}
	_updatingUi = oldUpdating;
	emit preferencesChanged();
	updateCopilotAdvSummaries();
}

void CopilotSettingsPage::saveCopilotKey() {
	if (!_copilotApiKey) {
		return;
	}
	QString key = _copilotApiKey->text().trimmed();
	if (key.isEmpty()) {
		updateCopilotKeyStatus();
		return;
	}
	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	auto &coord = copilotCoordinator();
	coord.setProfile(collectCopilotProfileFromUi());
	const auto st = coord.saveApiKey(key);
	wipeQString(key);
	clearCopilotKeyEditor();
	resetCopilotTestStatus();
	if (!coord.hasDictationApiKey() && copilotSttReusesChatHost()) {
		resetCopilotSttTestStatus();
	}
	_updatingUi = oldUpdating;
	updateCopilotKeyStatus();
	updateCopilotSttKeyStatus();
	updateCopilotSttModeUi();
	if ((st == OpenMeeting::AgentSecretStore::Status::StorageUnavailable ||
		 st == OpenMeeting::AgentSecretStore::Status::SaveFailed) && _copilotKeyHintIcon) {
		const QString storageNotice = (st == OpenMeeting::AgentSecretStore::Status::StorageUnavailable)
			? QCoreApplication::translate("MeetingUI",
				"OS encrypted storage is unavailable on this platform; key kept in session memory only.")
			: QCoreApplication::translate("MeetingUI",
				"Unable to write encrypted API key to settings; kept in session memory only.");
		const QString currentUrl = _copilotBaseUrl ? _copilotBaseUrl->text().trimmed() : coord.profile().baseUrl;
		const QString warn = keyEndpointMismatchWarningText(coord.chatKeyEndpointCompatibility(currentUrl), false);
		setHintIcon(
			_copilotKeyHintIcon,
			warn.isEmpty() ? storageNotice : (storageNotice + QLatin1Char('\n') + warn),
			QStringLiteral("warning"));
	}
	refreshCopilotModels(true);
}

void CopilotSettingsPage::clearCopilotKey() {
	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	clearCopilotKeyEditor();
	auto &coord = copilotCoordinator();
	coord.clearApiKey();
	resetCopilotTestStatus();
	if (!coord.hasDictationApiKey() && copilotSttReusesChatHost()) {
		resetCopilotSttTestStatus();
	}
	_updatingUi = oldUpdating;
	updateCopilotKeyStatus();
	updateCopilotSttKeyStatus();
	updateCopilotSttModeUi();
}

void CopilotSettingsPage::clearCopilotKeyEditor() {
	wipeAndClearLineEdit(_copilotApiKey);
}

void CopilotSettingsPage::updateCopilotKeyStatus() {
	if (!_copilotKeyHintIcon) {
		return;
	}
	auto &coord = copilotCoordinator();
	QString statusText;
	if (!coord.hasApiKey()) {
		statusText = QCoreApplication::translate("MeetingUI",
			"No API key saved (optional for local keyless servers).");
	} else if (coord.isApiKeyPersisted()) {
		statusText = QCoreApplication::translate("MeetingUI",
			"API key saved (encrypted on this device).");
	} else {
		statusText = QCoreApplication::translate("MeetingUI",
			"API key loaded (session memory only).");
	}
	const QString chatUrl = _copilotBaseUrl ? _copilotBaseUrl->text().trimmed() : coord.profile().baseUrl;
	QString draftKey = _copilotApiKey ? _copilotApiKey->text().trimmed() : QString();
	const QString warn = keyEndpointMismatchWarningText(
		coord.chatKeyEndpointCompatibility(chatUrl, draftKey), false);
	wipeQString(draftKey);
	if (!warn.isEmpty()) {
		statusText += QLatin1Char('\n') + warn;
	}
	setHintIcon(_copilotKeyHintIcon, statusText, warn.isEmpty() ? QString() : QStringLiteral("warning"));
}

void CopilotSettingsPage::saveCopilotSttKey() {
	if (!_copilotSttApiKey) {
		return;
	}
	QString key = _copilotSttApiKey->text().trimmed();
	if (key.isEmpty()) {
		updateCopilotSttKeyStatus();
		return;
	}
	updateCopilotSttModelChoices();
	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	auto &coord = copilotCoordinator();
	coord.setSpeechSourceMode(OpenMeeting::AgentSpeechSourceMode::CustomProfile);
	coord.setSpeechProfile(collectCopilotSpeechProfileFromUi());
	const auto st = coord.saveDictationApiKey(key);
	wipeQString(key);
	wipeAndClearLineEdit(_copilotSttApiKey);
	resetCopilotSttTestStatus();
	_updatingUi = oldUpdating;
	refreshCopilotSpeechProfileSelectors();
	updateCopilotSttKeyStatus();
	if ((st == OpenMeeting::AgentSecretStore::Status::StorageUnavailable ||
		 st == OpenMeeting::AgentSecretStore::Status::SaveFailed) && _copilotSttKeyHintIcon) {
		const QString storageNotice = (st == OpenMeeting::AgentSecretStore::Status::StorageUnavailable)
			? QCoreApplication::translate("MeetingUI",
				"OS encrypted storage is unavailable on this platform; key kept in session memory only.")
			: QCoreApplication::translate("MeetingUI",
				"Unable to write encrypted API key to settings; kept in session memory only.");
		const QString sttUrl = _copilotSttBaseUrl ? _copilotSttBaseUrl->text().trimmed() : coord.dictationSttBaseUrl();
		const QString chatUrl = _copilotBaseUrl ? _copilotBaseUrl->text().trimmed() : coord.profile().baseUrl;
		QStringList lines{ storageNotice };
		if (copilotSttReusesChatHost()) {
			lines.push_back(QCoreApplication::translate("MeetingUI",
				"Warning: STT Base URL is blank or shares the Chat host, so this separate STT key overrides your Chat API key for dictation. Click Clear Key to reuse the Chat key."));
		}
		const QString warn = keyEndpointMismatchWarningText(
			coord.speechKeyEndpointCompatibility(sttUrl, chatUrl), true);
		if (!warn.isEmpty()) {
			lines.push_back(warn);
		}
		setHintIcon(_copilotSttKeyHintIcon, lines.join(QLatin1Char('\n')), QStringLiteral("warning"));
	}
	_copilotLastSttModelFetchUrl.clear();
	refreshCopilotSpeechModels(true);
}

void CopilotSettingsPage::clearCopilotSttKey() {
	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	wipeAndClearLineEdit(_copilotSttApiKey);
	copilotCoordinator().clearDictationApiKey();
	resetCopilotSttTestStatus();
	_updatingUi = oldUpdating;
	updateCopilotSttKeyStatus();
}

void CopilotSettingsPage::saveCopilotSttAppKey() {
	if (!_copilotSttAppKey) {
		return;
	}
	QString appKey = _copilotSttAppKey->text().trimmed();
	if (appKey.isEmpty()) {
		updateCopilotSttAppKeyStatus();
		return;
	}
	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	auto &coord = copilotCoordinator();
	coord.setSpeechSourceMode(OpenMeeting::AgentSpeechSourceMode::CustomProfile);
	coord.setSpeechProfile(collectCopilotSpeechProfileFromUi());
	const auto st = coord.saveDictationAppKey(appKey);
	wipeQString(appKey);
	wipeAndClearLineEdit(_copilotSttAppKey);
	resetCopilotSttTestStatus();
	_updatingUi = oldUpdating;
	refreshCopilotSpeechProfileSelectors();
	updateCopilotSttAppKeyStatus();
	if ((st == OpenMeeting::AgentSecretStore::Status::StorageUnavailable ||
		 st == OpenMeeting::AgentSecretStore::Status::SaveFailed) && _copilotSttAppKeyHintIcon) {
		setHintIcon(
			_copilotSttAppKeyHintIcon,
			QCoreApplication::translate("MeetingUI",
				"OS encrypted storage is unavailable; Volcengine App Key is kept in session memory only."),
			QStringLiteral("warning"));
	}
}

void CopilotSettingsPage::clearCopilotSttAppKey() {
	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	wipeAndClearLineEdit(_copilotSttAppKey);
	copilotCoordinator().clearDictationAppKey();
	resetCopilotSttTestStatus();
	_updatingUi = oldUpdating;
	updateCopilotSttAppKeyStatus();
}

void CopilotSettingsPage::updateCopilotSttAppKeyStatus() {
	if (!_copilotSttAppKeyHintIcon) {
		return;
	}
	const auto &coord = copilotCoordinator();
	QString text;
	if (!coord.hasDictationAppKey()) {
		text = QCoreApplication::translate("MeetingUI",
			"No Volcengine App Key saved (optional — leave blank to use single X-Api-Key auth; fill only for legacy X-Api-App-Key + X-Api-Access-Key).");
	} else if (coord.isDictationAppKeyPersisted()) {
		text = QCoreApplication::translate("MeetingUI",
			"Volcengine App Key saved (encrypted on this device).");
	} else {
		text = QCoreApplication::translate("MeetingUI",
			"Volcengine App Key loaded (session memory only).");
	}
	setHintIcon(_copilotSttAppKeyHintIcon, text);
}

void CopilotSettingsPage::applyCopilotSpeechProtocolPreset(
		OpenMeeting::AgentSpeechProtocolMode protocol,
		bool forceOverwrite) {
	if (selectedCopilotSpeechSourceMode() != OpenMeeting::AgentSpeechSourceMode::CustomProfile ||
		protocol == OpenMeeting::AgentSpeechProtocolMode::Auto) {
		return;
	}
	const QString defaultUrl = OpenMeeting::defaultSpeechEndpoint(protocol);
	const QString defaultName = OpenMeeting::defaultSpeechProfileName(protocol);

	if (_copilotSttBaseUrl && !defaultUrl.isEmpty() &&
		(forceOverwrite || _copilotSttBaseUrl->text().trimmed().isEmpty())) {
		const QSignalBlocker blocker(_copilotSttBaseUrl);
		_copilotSttBaseUrl->setText(defaultUrl);
	}

	if (_copilotSttProfileName && !defaultName.isEmpty()) {
		const QString currentName = _copilotSttProfileName->text().trimmed();
		const auto isIndexedDefaultName = [&currentName](const QString &pattern) {
			const int placeholderIdx = pattern.indexOf(QStringLiteral("%1"));
			if (placeholderIdx < 0) {
				return false;
			}
			const QString prefix = pattern.left(placeholderIdx);
			const QString suffix = pattern.mid(placeholderIdx + 2);
			if (!currentName.startsWith(prefix) || !currentName.endsWith(suffix)) {
				return false;
			}
			const QString numStr = currentName.mid(
				prefix.size(),
				currentName.size() - prefix.size() - suffix.size()).trimmed();
			bool ok = false;
			const int num = numStr.toInt(&ok);
			return ok && num > 0;
		};
		const bool isGenericDefaultName =
			currentName.isEmpty() ||
			currentName == QStringLiteral("Default Speech") ||
			currentName == QCoreApplication::translate("MeetingUI", "Default Speech") ||
			isIndexedDefaultName(QStringLiteral("Speech Profile %1")) ||
			isIndexedDefaultName(QCoreApplication::translate("MeetingUI", "Speech Profile %1"));
		if (forceOverwrite || isGenericDefaultName) {
			const QSignalBlocker blocker(_copilotSttProfileName);
			_copilotSttProfileName->setText(defaultName);
		}
	}
}

void CopilotSettingsPage::updateCopilotSttProtocolUi(bool refreshModelChoices) {
	const auto selectedProto = _copilotSttProtocolCombo
		? OpenMeeting::agentSpeechProtocolModeFromInt(_copilotSttProtocolCombo->currentData().toInt())
		: OpenMeeting::AgentSpeechProtocolMode::Auto;
	const bool followChat =
		(selectedCopilotSpeechSourceMode() == OpenMeeting::AgentSpeechSourceMode::FollowChat);
	const QString sttUrl = (!followChat && _copilotSttBaseUrl)
		? _copilotSttBaseUrl->text().trimmed()
		: QString();
	const QString chatUrl = _copilotBaseUrl ? _copilotBaseUrl->text().trimmed() : QString();
	const QString modelText = _copilotSttModelCombo ? _copilotSttModelCombo->currentText().trimmed() : QString();
	const auto effectiveProto = OpenMeeting::resolveEffectiveSpeechProtocol(
		selectedProto, sttUrl, chatUrl, modelText);

	if (_copilotSttVolcContainer) {
		_copilotSttVolcContainer->setVisible(
			effectiveProto == OpenMeeting::AgentSpeechProtocolMode::VolcengineBigModelWs);
	}
	if (_copilotSttWorkspaceContainer) {
		_copilotSttWorkspaceContainer->setVisible(
			effectiveProto == OpenMeeting::AgentSpeechProtocolMode::DashScopeInferenceWs ||
			effectiveProto == OpenMeeting::AgentSpeechProtocolMode::DashScopeQwenRealtimeWs);
	}
	if (_copilotSttBaseUrl) {
		const QString defaultWss = OpenMeeting::defaultStreamingSpeechEndpoint(effectiveProto);
		_copilotSttBaseUrl->setPlaceholderText(defaultWss.isEmpty()
			? QCoreApplication::translate("MeetingUI",
				"Optional — https:// or wss:// (leave blank for protocol default)")
			: QCoreApplication::translate("MeetingUI",
				"Optional — leave blank to use %1").arg(defaultWss));
	}
	if (_copilotSttKeyLabel && _copilotSttApiKey) {
		const QUrl effectiveEndpointUrl(sttUrl.isEmpty() ? chatUrl : sttUrl, QUrl::StrictMode);
		const bool isElevenLabsEndpoint = effectiveEndpointUrl.isValid() &&
			OpenMeeting::isElevenLabsAgentHost(effectiveEndpointUrl);
		switch (effectiveProto) {
		case OpenMeeting::AgentSpeechProtocolMode::VolcengineBigModelWs:
			_copilotSttKeyLabel->setText(QCoreApplication::translate("MeetingUI", "Volcengine API Key / Access Key (X-Api-Key)"));
			_copilotSttApiKey->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Enter Volcengine API Key (or legacy Access Key)"));
			break;
		case OpenMeeting::AgentSpeechProtocolMode::DeepgramWs:
			_copilotSttKeyLabel->setText(QCoreApplication::translate("MeetingUI", "Speech API Key (Deepgram Token)"));
			_copilotSttApiKey->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Enter Deepgram API Key"));
			break;
		case OpenMeeting::AgentSpeechProtocolMode::AssemblyAiV3Ws:
			_copilotSttKeyLabel->setText(QCoreApplication::translate("MeetingUI", "Speech API Key (Authorization Key)"));
			_copilotSttApiKey->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Enter AssemblyAI API Key"));
			break;
		case OpenMeeting::AgentSpeechProtocolMode::ElevenLabsScribeWs:
			_copilotSttKeyLabel->setText(QCoreApplication::translate("MeetingUI", "Speech API Key (xi-api-key)"));
			_copilotSttApiKey->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Enter ElevenLabs API Key"));
			break;
		case OpenMeeting::AgentSpeechProtocolMode::OpenAiRealtimeWs:
		case OpenMeeting::AgentSpeechProtocolMode::DashScopeInferenceWs:
		case OpenMeeting::AgentSpeechProtocolMode::DashScopeQwenRealtimeWs:
		case OpenMeeting::AgentSpeechProtocolMode::XaiSttWs:
		case OpenMeeting::AgentSpeechProtocolMode::OpenAiTranscriptions:
		case OpenMeeting::AgentSpeechProtocolMode::ChatInputAudio:
		case OpenMeeting::AgentSpeechProtocolMode::DashScopeMultimodal:
			if (effectiveProto == OpenMeeting::AgentSpeechProtocolMode::OpenAiTranscriptions &&
				isElevenLabsEndpoint) {
				_copilotSttKeyLabel->setText(QCoreApplication::translate("MeetingUI", "Speech API Key (xi-api-key)"));
				_copilotSttApiKey->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Enter ElevenLabs API Key"));
			} else {
				_copilotSttKeyLabel->setText(QCoreApplication::translate("MeetingUI", "Speech API Key (Bearer Token)"));
				_copilotSttApiKey->setPlaceholderText(QCoreApplication::translate(
					"MeetingUI",
					"Enter separate speech API key (optional on same host)"));
			}
			break;
		default:
			if (isElevenLabsEndpoint) {
				_copilotSttKeyLabel->setText(QCoreApplication::translate("MeetingUI", "Speech API Key (xi-api-key)"));
				_copilotSttApiKey->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Enter ElevenLabs API Key"));
			} else {
				_copilotSttKeyLabel->setText(QCoreApplication::translate("MeetingUI", "Speech API Key"));
				_copilotSttApiKey->setPlaceholderText(QCoreApplication::translate(
					"MeetingUI",
					"Enter separate speech API key (optional on same host)"));
			}
			break;
		}
		_copilotSttApiKey->setAccessibleName(_copilotSttKeyLabel->text());
	}
	if (refreshModelChoices) {
		updateCopilotSttModelChoices();
	}
	updateCopilotSttModeUi();
}

void CopilotSettingsPage::updateCopilotSttModelChoices(bool fromServerFetch) {
	if (!_copilotSttModelCombo) {
		return;
	}
	const auto &coord = copilotCoordinator();
	const bool followChat =
		(selectedCopilotSpeechSourceMode() == OpenMeeting::AgentSpeechSourceMode::FollowChat);
	const QString sttUrl = followChat
		? QString()
		: (_copilotSttBaseUrl ? _copilotSttBaseUrl->text().trimmed() : coord.dictationSttBaseUrl());
	const QString chatUrl = _copilotBaseUrl
		? _copilotBaseUrl->text().trimmed()
		: coord.profile().baseUrl;
	const auto sttProtocol = _copilotSttProtocolCombo
		? OpenMeeting::agentSpeechProtocolModeFromInt(_copilotSttProtocolCombo->currentData().toInt())
		: coord.dictationSpeechProtocol();

	const QStringList externalSpeechModels = coord.cachedExternalSpeechModelsForUrl(sttUrl);
	const auto externalSpeechCaps = coord.cachedExternalSpeechCapabilitiesForUrl(sttUrl);
	const QStringList choices = OpenMeeting::AgentCoordinator::suggestedSpeechModelsForEndpoint(
		sttUrl,
		chatUrl,
		coord.cachedSpeechModels(),
		externalSpeechModels,
		sttProtocol);

	QString effectiveChatOrSttUrl = sttUrl.isEmpty() ? chatUrl : sttUrl;
	if (sttUrl.isEmpty() && OpenMeeting::isStreamingSpeechProtocol(sttProtocol)) {
		const QString defUrl = OpenMeeting::defaultStreamingSpeechEndpoint(sttProtocol);
		if (!defUrl.isEmpty()) {
			effectiveChatOrSttUrl = defUrl;
		}
	}
	const QString currentEndpointKey = OpenMeeting::isStreamingSpeechProtocol(sttProtocol)
		? QStringLiteral("proto:%1:%2").arg(static_cast<int>(sttProtocol)).arg(
			OpenMeeting::AgentCoordinator::canonicalSpeechEndpointKey(effectiveChatOrSttUrl))
		: OpenMeeting::AgentCoordinator::canonicalSpeechEndpointKey(effectiveChatOrSttUrl);
	const bool endpointChanged = !currentEndpointKey.isEmpty() &&
		!_copilotLastSttEndpointKey.isEmpty() &&
		currentEndpointKey != _copilotLastSttEndpointKey;
	const bool firstValidEndpoint = !currentEndpointKey.isEmpty() && _copilotLastSttEndpointKey.isEmpty();
	if (!currentEndpointKey.isEmpty()) {
		_copilotLastSttEndpointKey = currentEndpointKey;
	}

	const QSignalBlocker sttBlocker(_copilotSttModelCombo);
	const QString currentModelText = _copilotSttModelCombo->currentText().trimmed();
	_copilotSttModelCombo->clear();
	for (const QString &m : choices) {
		_copilotSttModelCombo->addItem(m);
	}
	const OpenMeeting::AgentEndpointDecision ep = OpenMeeting::evaluateAgentSpeechEndpoint(effectiveChatOrSttUrl);
	const QString epHost = ep.allowed ? ep.normalizedUrl.host().toLower() : QString();
	const bool isExternalGroq = !sttUrl.isEmpty() &&
		!OpenMeeting::AgentCoordinator::isSameSpeechOrigin(sttUrl, chatUrl) &&
		OpenMeeting::isGroqAgentHost(epHost);

	const QString autoDefaultLabel = OpenMeeting::AgentCoordinator::resolveDefaultSpeechModelForEndpoint(
		sttUrl,
		chatUrl,
		coord.cachedSpeechModels(),
		coord.cachedModelCapabilities(),
		externalSpeechModels,
		externalSpeechCaps,
		sttProtocol);
	if (auto *sttEdit = _copilotSttModelCombo->lineEdit()) {
		sttEdit->setPlaceholderText(isExternalGroq
			? QCoreApplication::translate("MeetingUI", "Auto (Groq default: whisper-large-v3-turbo)")
			: QCoreApplication::translate("MeetingUI", "Auto (default: %1)").arg(autoDefaultLabel));
	}
	const int sttIdx = currentModelText.isEmpty() ? -1 : _copilotSttModelCombo->findText(currentModelText);
	const bool shouldSwitchToEndpointDefault =
		!_updatingUi &&
		!currentModelText.isEmpty() &&
		sttIdx < 0 &&
		(endpointChanged || firstValidEndpoint ||
			(fromServerFetch && !choices.isEmpty() && _copilotSttModelAutoSelected));
	if (sttIdx >= 0) {
		_copilotSttModelCombo->setCurrentIndex(sttIdx);
	} else if (shouldSwitchToEndpointDefault) {
		int defaultIdx = autoDefaultLabel.isEmpty() ? -1 : _copilotSttModelCombo->findText(autoDefaultLabel);
		if (defaultIdx < 0 && !choices.isEmpty()) {
			defaultIdx = 0;
		}
		if (defaultIdx >= 0) {
			_copilotSttModelCombo->setCurrentIndex(defaultIdx);
			_copilotSttModelAutoSelected = true;
		} else {
			_copilotSttModelCombo->setCurrentIndex(-1);
			_copilotSttModelCombo->setEditText(QString());
			_copilotSttModelAutoSelected = false;
		}
	} else if (currentModelText.isEmpty()) {
		_copilotSttModelCombo->setCurrentIndex(-1);
		_copilotSttModelCombo->setEditText(QString());
		_copilotSttModelAutoSelected = false;
	} else {
		_copilotSttModelCombo->setCurrentIndex(-1);
		_copilotSttModelCombo->setEditText(currentModelText);
	}
}

void CopilotSettingsPage::updateCopilotSttKeyStatus() {
	auto &coord = copilotCoordinator();
	const bool followChat =
		(selectedCopilotSpeechSourceMode() == OpenMeeting::AgentSpeechSourceMode::FollowChat);
	const QString chatUrl = _copilotBaseUrl ? _copilotBaseUrl->text().trimmed() : coord.profile().baseUrl;

	if (followChat) {
		const auto selectedProto = _copilotSttProtocolCombo
			? OpenMeeting::agentSpeechProtocolModeFromInt(_copilotSttProtocolCombo->currentData().toInt())
			: coord.dictationSpeechProtocol();
		const QString modelText = _copilotSttModelCombo
			? _copilotSttModelCombo->currentText().trimmed()
			: coord.dictationSttModel();
		const auto effectiveProto = OpenMeeting::resolveEffectiveSpeechProtocol(
			selectedProto, QString(), chatUrl, modelText);
		bool followsChatOrigin = true;
		if (effectiveProto == OpenMeeting::AgentSpeechProtocolMode::DeepgramWs ||
			effectiveProto == OpenMeeting::AgentSpeechProtocolMode::AssemblyAiV3Ws ||
			effectiveProto == OpenMeeting::AgentSpeechProtocolMode::VolcengineBigModelWs ||
			effectiveProto == OpenMeeting::AgentSpeechProtocolMode::ElevenLabsScribeWs) {
			followsChatOrigin = OpenMeeting::AgentCoordinator::isSameSpeechOrigin(
				OpenMeeting::defaultStreamingSpeechEndpoint(effectiveProto), chatUrl);
		} else if (effectiveProto == OpenMeeting::AgentSpeechProtocolMode::DashScopeInferenceWs ||
				   effectiveProto == OpenMeeting::AgentSpeechProtocolMode::DashScopeQwenRealtimeWs) {
			followsChatOrigin = OpenMeeting::isDashScopeAgentHost(QUrl(chatUrl)) &&
				!OpenMeeting::isBailianPlanAgentHost(QUrl(chatUrl));
		} else if (effectiveProto == OpenMeeting::AgentSpeechProtocolMode::XaiSttWs) {
			followsChatOrigin = OpenMeeting::isXaiAgentHost(QUrl(chatUrl));
		}

		QStringList bannerLines;
		if (!followsChatOrigin) {
			QString chatName = _copilotProfileName
				? _copilotProfileName->text().trimmed()
				: coord.profile().name.trimmed();
			if (chatName.isEmpty()) {
				chatName = QCoreApplication::translate("MeetingUI", "Active Chat profile");
			}
			bannerLines.push_back(QCoreApplication::translate(
				"MeetingUI",
				"The selected protocol or model targets an external streaming endpoint different from \"%1\". Switch to \"Custom Speech Profiles\" above to configure a dedicated endpoint and API key.")
				.arg(chatName));
		}
		const QString mismatchWarn = keyEndpointMismatchWarningText(
			coord.speechKeyEndpointCompatibility(QString(), chatUrl, QString()), true);
		if (!mismatchWarn.isEmpty()) {
			bannerLines.push_back(mismatchWarn);
		}
		if (_copilotSttKeyWarningBanner) {
			const QString bannerText = bannerLines.join(QLatin1Char('\n')).trimmed();
			_copilotSttKeyWarningBanner->setText(bannerText);
			_copilotSttKeyWarningBanner->setVisible(!bannerText.isEmpty());
		}
		return;
	}

	QStringList lines;
	if (!coord.hasDictationApiKey()) {
		lines.push_back(QCoreApplication::translate("MeetingUI",
			"No separate speech API key saved (reuses Chat key on the same host; required for third-party HTTP or WebSocket streaming providers such as OpenAI, DashScope, Deepgram, AssemblyAI, Volcengine, ElevenLabs, xAI, or Groq)."));
	} else if (coord.isDictationApiKeyPersisted()) {
		lines.push_back(QCoreApplication::translate("MeetingUI",
			"Dedicated speech API key saved (encrypted on this device)."));
	} else {
		lines.push_back(QCoreApplication::translate("MeetingUI",
			"Dedicated speech API key loaded (session memory only)."));
	}

	const QString sttUrl = _copilotSttBaseUrl ? _copilotSttBaseUrl->text().trimmed() : coord.dictationSttBaseUrl();
	QString draftSttKey = _copilotSttApiKey ? _copilotSttApiKey->text().trimmed() : QString();
	bool hasWarning = false;
	QStringList bannerLines;
	if (coord.hasDictationApiKey() && copilotSttReusesChatHost()) {
		hasWarning = true;
		const QString overrideWarn = QCoreApplication::translate("MeetingUI",
			"Warning: Speech Endpoint URL is blank or shares the Chat host, so this separate speech key overrides your Chat API key for speech recognition. Click Clear Key to reuse the Chat key.");
		lines.push_back(overrideWarn);
		bannerLines.push_back(overrideWarn);
	}
	const QString mismatchWarn = keyEndpointMismatchWarningText(
		coord.speechKeyEndpointCompatibility(sttUrl, chatUrl, draftSttKey), true);
	wipeQString(draftSttKey);
	if (!mismatchWarn.isEmpty()) {
		hasWarning = true;
		lines.push_back(mismatchWarn);
		bannerLines.push_back(mismatchWarn);
	}
	if (_copilotSttKeyHintIcon) {
		setHintIcon(
			_copilotSttKeyHintIcon,
			lines.join(QLatin1Char('\n')),
			hasWarning ? QStringLiteral("warning") : QString());
	}
	if (_copilotSttKeyWarningBanner) {
		const QString bannerText = bannerLines.join(QLatin1Char('\n')).trimmed();
		_copilotSttKeyWarningBanner->setText(bannerText);
		_copilotSttKeyWarningBanner->setVisible(!bannerText.isEmpty());
	}
}

void CopilotSettingsPage::testCopilotConnection() {
	if (!_copilotTestBtn || !_copilotTestStatus) {
		return;
	}
	const auto draft = collectCopilotProfileFromUi();
	QString typedKey = _copilotApiKey ? _copilotApiKey->text().trimmed() : QString();

	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	auto &coord = copilotCoordinator();
	const bool sttProbeWasActive = coord.isSpeechProbing();
	coord.setProfile(draft);
	if (!typedKey.isEmpty()) {
		coord.saveApiKey(typedKey);
		clearCopilotKeyEditor();
	}
	if (sttProbeWasActive && !coord.isSpeechProbing()) {
		resetCopilotSttTestStatus();
	}
	_updatingUi = oldUpdating;
	updateCopilotKeyStatus();
	updateCopilotSttKeyStatus();

	_copilotTestBtn->setEnabled(false);
	setCopilotTestStatus(
		QStringLiteral("pending"),
		QCoreApplication::translate("MeetingUI", "Testing connection..."));
	coord.testConnection(draft, typedKey);
	wipeQString(typedKey);
}

void CopilotSettingsPage::testCopilotSttConnection() {
	if (!_copilotSttTestBtn || !_copilotSttTestStatus) {
		return;
	}
	const auto draftProfile = collectCopilotProfileFromUi();
	const auto mode = selectedCopilotSpeechSourceMode();
	const bool isFollowChat = (mode == OpenMeeting::AgentSpeechSourceMode::FollowChat);
	const auto sttProtocol = _copilotSttProtocolCombo
		? OpenMeeting::agentSpeechProtocolModeFromInt(_copilotSttProtocolCombo->currentData().toInt())
		: OpenMeeting::AgentSpeechProtocolMode::Auto;
	const QString sttModel = _copilotSttModelCombo
		? _copilotSttModelCombo->currentText().trimmed()
		: QString();
	QString sttLang;
	if (_copilotDictationLangCombo) {
		sttLang = _copilotDictationLangCombo->currentText().trimmed();
		const int langIdx = _copilotDictationLangCombo->currentIndex();
		if (langIdx >= 0 && sttLang == _copilotDictationLangCombo->itemText(langIdx).trimmed()) {
			sttLang = _copilotDictationLangCombo->itemData(langIdx).toString().trimmed();
		}
	}
	QString typedSttKey = (!isFollowChat && _copilotSttApiKey) ? _copilotSttApiKey->text().trimmed() : QString();
	QString typedChatKey = _copilotApiKey ? _copilotApiKey->text().trimmed() : QString();
	QString typedAppKey = (!isFollowChat && _copilotSttAppKey) ? _copilotSttAppKey->text().trimmed() : QString();
	const QString sttPrompt = _copilotSttPromptEdit
		? _copilotSttPromptEdit->text().trimmed()
		: QString();

	const bool oldUpdating = _updatingUi;
	_updatingUi = true;
	auto &coord = copilotCoordinator();
	coord.setSpeechSourceMode(mode);
	OpenMeeting::AgentSpeechProfile draftSpeechProf = coord.speechProfile();
	if (isFollowChat) {
		if (_copilotSttProtocolCombo) {
			coord.setDictationSpeechProtocol(sttProtocol);
		}
		if (_copilotSttModelCombo) {
			coord.setDictationSttModel(sttModel);
		}
	} else {
		draftSpeechProf = collectCopilotSpeechProfileFromUi();
		coord.setSpeechProfile(draftSpeechProf);
		refreshCopilotSpeechProfileSelectors();
		if (!typedSttKey.isEmpty()) {
			coord.saveDictationApiKey(typedSttKey);
			wipeAndClearLineEdit(_copilotSttApiKey);
		}
		if (!typedAppKey.isEmpty()) {
			coord.saveDictationAppKey(typedAppKey);
			wipeAndClearLineEdit(_copilotSttAppKey);
		}
	}
	if (_copilotDictationLangCombo) {
		coord.setDictationLanguage(sttLang);
	}
	if (_copilotSttPromptEdit) {
		coord.setDictationPrompt(sttPrompt);
	}
	_updatingUi = oldUpdating;
	updateCopilotSttKeyStatus();
	updateCopilotSttAppKeyStatus();

	OpenMeeting::AgentSpeechProbeConfig probeCfg;
	probeCfg.chatProfile = draftProfile;
	probeCfg.sourceModeOverride = mode;
	probeCfg.speechProfileId = isFollowChat ? QString() : coord.activeSpeechProfileId();
	probeCfg.sttBaseUrl = isFollowChat ? QString() : draftSpeechProf.baseUrl;
	probeCfg.protocolMode = isFollowChat ? sttProtocol : draftSpeechProf.protocol;
	probeCfg.sttModel = isFollowChat ? sttModel : draftSpeechProf.model;
	probeCfg.language = sttLang;
	probeCfg.prompt = sttPrompt;
	probeCfg.draftSttApiKey = typedSttKey;
	probeCfg.draftChatApiKey = typedChatKey;
	probeCfg.draftAppKey = typedAppKey;
	probeCfg.resourceId = isFollowChat ? QString() : draftSpeechProf.resourceId;
	probeCfg.workspaceId = isFollowChat ? QString() : draftSpeechProf.workspaceId;

	_copilotSttTestBtn->setEnabled(false);
	setCopilotSttTestStatus(
		QStringLiteral("pending"),
		QCoreApplication::translate("MeetingUI", "Testing connection..."));
	coord.testSpeechConnection(probeCfg);
	wipeQString(typedSttKey);
	wipeQString(typedChatKey);
	wipeQString(typedAppKey);
	wipeQString(probeCfg.draftSttApiKey);
	wipeQString(probeCfg.draftChatApiKey);
	wipeQString(probeCfg.draftAppKey);
}

void CopilotSettingsPage::refreshCopilotModels(bool autoTriggered) {
	if (!_copilotBaseUrl || !_copilotRefreshModelsBtn) {
		return;
	}
	const auto draft = collectCopilotProfileFromUi();
	const auto decision = OpenMeeting::evaluateAgentEndpoint(draft.baseUrl);
	if (!decision.allowed) {
		_copilotLastModelFetchBaseUrl.clear();
		if (!autoTriggered) {
			setCopilotModelHint(decision.reason, QStringLiteral("error"));
		}
		return;
	}
	_copilotLastModelFetchBaseUrl = draft.baseUrl.trimmed();
	QString typedKey = _copilotApiKey ? _copilotApiKey->text().trimmed() : QString();
	_copilotRefreshModelsBtn->setEnabled(false);
	setCopilotModelHint(
		QCoreApplication::translate("MeetingUI", "Fetching models..."),
		QStringLiteral("pending"));
	copilotCoordinator().fetchModels(draft, typedKey);
	wipeQString(typedKey);
	refreshCopilotSpeechModels(autoTriggered);
}

void CopilotSettingsPage::refreshCopilotSpeechModels(bool autoTriggered) {
	if (!_copilotSttBaseUrl ||
		selectedCopilotSpeechSourceMode() != OpenMeeting::AgentSpeechSourceMode::CustomProfile) {
		_copilotLastSttModelFetchUrl.clear();
		return;
	}
	const QString sttUrl = _copilotSttBaseUrl->text().trimmed();
	const QString chatUrl = _copilotBaseUrl
		? _copilotBaseUrl->text().trimmed()
		: copilotCoordinator().profile().baseUrl;
	if (sttUrl.isEmpty() || OpenMeeting::AgentCoordinator::isSameAgentOrigin(sttUrl, chatUrl)) {
		_copilotLastSttModelFetchUrl.clear();
		return;
	}
	const auto sttDecision = OpenMeeting::evaluateAgentEndpoint(sttUrl);
	if (!sttDecision.allowed || OpenMeeting::isElevenLabsAgentHost(sttDecision.normalizedUrl)) {
		_copilotLastSttModelFetchUrl.clear();
		return;
	}
	const QString canonicalKey = OpenMeeting::AgentCoordinator::canonicalSpeechEndpointKey(sttUrl);
	if (autoTriggered && !canonicalKey.isEmpty() && _copilotLastSttModelFetchUrl == canonicalKey) {
		return;
	}
	auto &coord = copilotCoordinator();
	QString typedSttKey = _copilotSttApiKey ? _copilotSttApiKey->text().trimmed() : QString();
	const bool loopbackStt = OpenMeeting::isAgentLoopbackHost(sttDecision.normalizedUrl.host());
	if (typedSttKey.isEmpty() && !coord.hasDictationApiKey() && !loopbackStt) {
		// 尚未填写或保存独立 STT 密钥时不预先发起无鉴权 /models 请求；保存密钥后再拉取。
		return;
	}
	_copilotLastSttModelFetchUrl = canonicalKey;
	coord.fetchSpeechModels(sttUrl, typedSttKey, chatUrl);
	wipeQString(typedSttKey);
}


} // namespace MeetingUI
