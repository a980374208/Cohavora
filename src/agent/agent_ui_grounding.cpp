#include "src/agent/agent_ui_grounding.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QStringList>

#include <array>

namespace OpenMeeting {
namespace {

QString uiTr(const char *source) {
	return QCoreApplication::translate("MeetingUI", source);
}

QString roomTr(const char *source) {
	return QCoreApplication::translate("MeetingRoomWindow", source);
}

QString tileTr(const char *source) {
	return QCoreApplication::translate("MeetingUI::MeetingRoomWindow", source);
}

QString wbTr(const char *source) {
	return QCoreApplication::translate("MeetingUI::WhiteboardPanel", source);
}

struct ChoiceEntry {
	const char *paramValue;
	const char *labelSource;
	bool fixedLiteral = false;
};

constexpr std::array<ChoiceEntry, 4> kThemeChoices{{
	{"system", "System", false},
	{"light", "Light", false},
	{"dark", "Dark", false},
	{"teal", "Teal", false},
}};

constexpr std::array<ChoiceEntry, 3> kLanguageChoices{{
	{"system", "Follow system", false},
	{"zh_CN", "Simplified Chinese", false},
	{"en_US", "English", false},
}};

constexpr std::array<ChoiceEntry, 3> kNameDisplayChoices{{
	{"auto", "Auto (default)", false},
	{"always", "Always show", false},
	{"on_hover", "Show on hover", false},
}};

constexpr std::array<ChoiceEntry, 3> kMirrorChoices{{
	{"off", "Disabled", false},
	{"local_only", "Mirror only my preview", false},
	{"local_and_remote", "Mirror for all participants", false},
}};

constexpr std::array<ChoiceEntry, 4> kReceiveMatchChoices{{
	{"auto", "Auto (follow layout)", false},
	{"up", "Match up (prefer clarity)", false},
	{"down", "Match down (save bandwidth)", false},
	{"nearest", "Nearest resolution", false},
}};

constexpr std::array<ChoiceEntry, 6> kVideoEncoderChoices{{
	{"auto", "Auto (prefer hardware)", false},
	{"software", "Software", false},
	{"nvenc", "NVIDIA NVENC", true},
	{"qsv", "Intel QSV", true},
	{"amf", "AMD AMF", true},
	{"vaapi", "VAAPI", true},
}};

constexpr std::array<ChoiceEntry, 6> kVideoCodecChoices{{
	{"auto", "Auto (prefer hardware H.264)", false},
	{"vp8", "VP8", true},
	{"h264", "H.264", true},
	{"vp9", "VP9", true},
	{"av1", "AV1", true},
	{"h265", "H.265", true},
}};

constexpr std::array<ChoiceEntry, 6> kScreenShareResChoices{{
	{"auto", "Auto (up to 2K)", false},
	{"720p", "720p", false},
	{"1080p", "1080p", false},
	{"1440p", "1440p (2K)", false},
	{"2160p", "2160p (4K)", false},
	{"native", "Native (up to 4K)", false},
}};

QString entryLabel(const ChoiceEntry &entry) {
	return entry.fixedLiteral
		? QString::fromUtf8(entry.labelSource)
		: uiTr(entry.labelSource);
}

template <std::size_t N>
QString lookupChoiceLabel(const std::array<ChoiceEntry, N> &entries, const QString &rawParam) {
	const QString norm = rawParam.trimmed();
	for (const auto &entry : entries) {
		if (norm.compare(QLatin1String(entry.paramValue), Qt::CaseInsensitive) == 0) {
			return entryLabel(entry);
		}
	}
	return norm;
}

template <std::size_t N>
QJsonArray buildChoiceOptionsArray(const std::array<ChoiceEntry, N> &entries) {
	QJsonArray options;
	for (const auto &entry : entries) {
		QJsonObject item;
		item.insert(QStringLiteral("label"), entryLabel(entry));
		item.insert(QStringLiteral("paramValue"), QString::fromLatin1(entry.paramValue));
		options.append(item);
	}
	return options;
}

template <std::size_t N>
QString joinChoiceLabels(const std::array<ChoiceEntry, N> &entries) {
	QStringList labels;
	labels.reserve(static_cast<int>(N));
	for (const auto &entry : entries) {
		labels.push_back(entryLabel(entry));
	}
	return labels.join(QStringLiteral(" / "));
}

QString makeUiPath(const QStringList &segments) {
	QStringList nonEmpty;
	for (const QString &seg : segments) {
		const QString trimmed = seg.trimmed();
		if (!trimmed.isEmpty()) {
			nonEmpty.push_back(trimmed);
		}
	}
	return nonEmpty.join(QStringLiteral(" > "));
}

QJsonObject makeDetailEntry(
		const QString &uiPath,
		const QString &currentLabel,
		const QString &paramName,
		const QJsonArray &options = {}) {
	QJsonObject obj;
	if (!uiPath.isEmpty()) {
		obj.insert(QStringLiteral("uiPath"), uiPath);
	}
	obj.insert(QStringLiteral("currentLabel"), currentLabel);
	obj.insert(QStringLiteral("paramName"), paramName);
	if (!options.isEmpty()) {
		obj.insert(QStringLiteral("options"), options);
	}
	return obj;
}

QString bracketedList(const QStringList &items, const QString &sep = QStringLiteral(", ")) {
	QStringList wrapped;
	wrapped.reserve(items.size());
	for (const QString &item : items) {
		wrapped.push_back(QStringLiteral("[%1]").arg(item));
	}
	return wrapped.join(sep);
}

} // namespace

QString localizedBooleanSettingLabel(bool enabled) {
	return uiTr(enabled ? "Enabled" : "Disabled");
}

QString localizedThemeLabel(const QString &themeId) {
	return lookupChoiceLabel(kThemeChoices, themeId);
}

QString localizedLanguageLabel(const QString &langId) {
	return lookupChoiceLabel(kLanguageChoices, langId);
}

QString localizedParticipantNameDisplayLabel(const QString &mode) {
	return lookupChoiceLabel(kNameDisplayChoices, mode);
}

QString localizedMirrorModeLabel(const QString &mode) {
	return lookupChoiceLabel(kMirrorChoices, mode);
}

QString localizedReceiveResolutionMatchLabel(const QString &mode) {
	return lookupChoiceLabel(kReceiveMatchChoices, mode);
}

QString localizedVideoEncoderLabel(const QString &encoderPref) {
	return lookupChoiceLabel(kVideoEncoderChoices, encoderPref);
}

QString localizedVideoCodecLabel(const QString &codec) {
	return lookupChoiceLabel(kVideoCodecChoices, codec);
}

QString localizedScreenShareResolutionLabel(const QString &resPreset) {
	return lookupChoiceLabel(kScreenShareResChoices, resPreset);
}

QString formatPreferenceDisplayWithParam(
		const QString &displayLabel,
		const QString &rawParam) {
	const QString cleanLabel = displayLabel.trimmed();
	const QString cleanParam = rawParam.trimmed();
	if (cleanLabel.isEmpty()) {
		return cleanParam;
	}
	if (cleanParam.isEmpty() || cleanLabel.compare(cleanParam, Qt::CaseInsensitive) == 0) {
		return cleanLabel;
	}
	return QStringLiteral("%1 (%2)").arg(cleanLabel, cleanParam);
}

QJsonObject buildClientPreferencesUiDetails(const QJsonObject &rawPrefsPayload) {
	const QString kSettings = uiTr("Settings");
	const QString kGeneral = uiTr("General");
	const QString kVideo = uiTr("Video");
	const QString kAudio = uiTr("Audio");
	const QString kSecurity = uiTr("Security");
	const QString kCopilot = uiTr("Copilot");
	const QString kPublishingCodec = uiTr("Publishing codec");
	const QString kReceiveQualityMirror = uiTr("Receive quality & Mirror");
	const QString kAudioEnhancement = uiTr("Noise Reduction and Audio Enhancement");
	const QString kE2ee = uiTr("End-to-end encryption (E2EE)");

	QJsonObject details;
	const auto insertToggle = [&](const QString &jsonKey, const QString &path, const QString &paramName) {
		if (!rawPrefsPayload.contains(jsonKey)) {
			return;
		}
		const bool enabled = rawPrefsPayload.value(jsonKey).toBool();
		details.insert(jsonKey, makeDetailEntry(
			path,
			localizedBooleanSettingLabel(enabled),
			paramName,
			QJsonArray{
				QJsonObject{{QStringLiteral("label"), uiTr("Enabled")}, {QStringLiteral("paramValue"), true}},
				QJsonObject{{QStringLiteral("label"), uiTr("Disabled")}, {QStringLiteral("paramValue"), false}},
			}));
	};

	if (rawPrefsPayload.contains(QStringLiteral("theme"))) {
		const QString raw = rawPrefsPayload.value(QStringLiteral("theme")).toString();
		details.insert(QStringLiteral("theme"), makeDetailEntry(
			makeUiPath({kSettings, kGeneral, uiTr("Theme")}),
			localizedThemeLabel(raw),
			QStringLiteral("theme"),
			buildChoiceOptionsArray(kThemeChoices)));
	}
	if (rawPrefsPayload.contains(QStringLiteral("language"))) {
		const QString raw = rawPrefsPayload.value(QStringLiteral("language")).toString();
		details.insert(QStringLiteral("language"), makeDetailEntry(
			makeUiPath({kSettings, kGeneral, uiTr("Language")}),
			localizedLanguageLabel(raw),
			QStringLiteral("language"),
			buildChoiceOptionsArray(kLanguageChoices)));
	}
	if (rawPrefsPayload.contains(QStringLiteral("participantNameDisplay"))) {
		const QString raw = rawPrefsPayload.value(QStringLiteral("participantNameDisplay")).toString();
		details.insert(QStringLiteral("participantNameDisplay"), makeDetailEntry(
			makeUiPath({kSettings, kGeneral, uiTr("Participant names in meetings")}),
			localizedParticipantNameDisplayLabel(raw),
			QStringLiteral("participant_name_display"),
			buildChoiceOptionsArray(kNameDisplayChoices)));
	}

	insertToggle(QStringLiteral("enableCameraOnJoin"),
		makeUiPath({kSettings, kGeneral, uiTr("Enable camera on joining")}),
		QStringLiteral("enable_camera_on_join"));
	insertToggle(QStringLiteral("enableMicrophoneOnJoin"),
		makeUiPath({kSettings, kGeneral, uiTr("Enable microphone on joining")}),
		QStringLiteral("enable_microphone_on_join"));
	insertToggle(QStringLiteral("enableSpeakerOnJoin"),
		makeUiPath({kSettings, kGeneral, uiTr("Use computer audio when joining")}),
		QStringLiteral("enable_speaker_on_join"));
	insertToggle(QStringLiteral("quitOnMainWindowClose"),
		makeUiPath({kSettings, kGeneral, uiTr("Quit when the main window is closed")}),
		QStringLiteral("quit_on_main_window_close"));
	insertToggle(QStringLiteral("showActiveSpeaker"),
		makeUiPath({kSettings, kGeneral, uiTr("Show the active speaker")}),
		QStringLiteral("show_active_speaker"));
	insertToggle(QStringLiteral("stayInMeetingWhenLocked"),
		makeUiPath({kSettings, kGeneral, uiTr("Stay in the meeting when the screen is locked")}),
		QStringLiteral("stay_in_meeting_when_locked"));

	if (rawPrefsPayload.contains(QStringLiteral("videoEncoderPreference"))) {
		const QString raw = rawPrefsPayload.value(QStringLiteral("videoEncoderPreference")).toString();
		details.insert(QStringLiteral("videoEncoderPreference"), makeDetailEntry(
			makeUiPath({kSettings, kVideo, kPublishingCodec, uiTr("Video encoder")}),
			localizedVideoEncoderLabel(raw),
			QStringLiteral("video_encoder_preference"),
			buildChoiceOptionsArray(kVideoEncoderChoices)));
	}
	if (rawPrefsPayload.contains(QStringLiteral("cameraVideoCodec"))) {
		const QString raw = rawPrefsPayload.value(QStringLiteral("cameraVideoCodec")).toString();
		details.insert(QStringLiteral("cameraVideoCodec"), makeDetailEntry(
			makeUiPath({kSettings, kVideo, kPublishingCodec, uiTr("Camera codec")}),
			localizedVideoCodecLabel(raw),
			QStringLiteral("camera_video_codec"),
			buildChoiceOptionsArray(kVideoCodecChoices)));
	}
	if (rawPrefsPayload.contains(QStringLiteral("screenShareVideoCodec"))) {
		const QString raw = rawPrefsPayload.value(QStringLiteral("screenShareVideoCodec")).toString();
		details.insert(QStringLiteral("screenShareVideoCodec"), makeDetailEntry(
			makeUiPath({kSettings, kVideo, kPublishingCodec, uiTr("Screen share codec")}),
			localizedVideoCodecLabel(raw),
			QStringLiteral("screen_share_video_codec"),
			buildChoiceOptionsArray(kVideoCodecChoices)));
	}
	if (rawPrefsPayload.contains(QStringLiteral("screenShareResolution"))) {
		const QString raw = rawPrefsPayload.value(QStringLiteral("screenShareResolution")).toString();
		details.insert(QStringLiteral("screenShareResolution"), makeDetailEntry(
			makeUiPath({kSettings, kVideo, kPublishingCodec, uiTr("Screen share resolution")}),
			localizedScreenShareResolutionLabel(raw),
			QStringLiteral("screen_share_resolution"),
			buildChoiceOptionsArray(kScreenShareResChoices)));
	}
	if (rawPrefsPayload.contains(QStringLiteral("screenShareFps"))) {
		const int fps = rawPrefsPayload.value(QStringLiteral("screenShareFps")).toInt(20);
		details.insert(QStringLiteral("screenShareFps"), makeDetailEntry(
			makeUiPath({kSettings, kVideo, kPublishingCodec, uiTr("Screen share frame rate")}),
			QStringLiteral("%1 FPS").arg(fps),
			QStringLiteral("screen_share_fps")));
	}
	if (rawPrefsPayload.contains(QStringLiteral("receiveResolutionMatch"))) {
		const QString raw = rawPrefsPayload.value(QStringLiteral("receiveResolutionMatch")).toString();
		details.insert(QStringLiteral("receiveResolutionMatch"), makeDetailEntry(
			makeUiPath({kSettings, kVideo, kReceiveQualityMirror, uiTr("Simulcast resolution matching")}),
			localizedReceiveResolutionMatchLabel(raw),
			QStringLiteral("receive_resolution_match"),
			buildChoiceOptionsArray(kReceiveMatchChoices)));
	}
	if (rawPrefsPayload.contains(QStringLiteral("mirrorMode"))) {
		const QString raw = rawPrefsPayload.value(QStringLiteral("mirrorMode")).toString();
		details.insert(QStringLiteral("mirrorMode"), makeDetailEntry(
			makeUiPath({kSettings, kVideo, kReceiveQualityMirror, uiTr("Mirror Video")}),
			localizedMirrorModeLabel(raw),
			QStringLiteral("mirror_mode"),
			buildChoiceOptionsArray(kMirrorChoices)));
	}
	if (rawPrefsPayload.contains(QStringLiteral("videoCaptureWidth")) &&
		rawPrefsPayload.contains(QStringLiteral("videoCaptureHeight"))) {
		const int w = rawPrefsPayload.value(QStringLiteral("videoCaptureWidth")).toInt();
		const int h = rawPrefsPayload.value(QStringLiteral("videoCaptureHeight")).toInt();
		const int fps = rawPrefsPayload.value(QStringLiteral("videoCaptureFps")).toInt(30);
		const QString resLabel = (w <= 0 || h <= 0)
			? uiTr("Auto")
			: QStringLiteral("%1 x %2 @ %3 fps").arg(w).arg(h).arg(fps);
		details.insert(QStringLiteral("videoCapture"), makeDetailEntry(
			makeUiPath({kSettings, kVideo, uiTr("Camera"), uiTr("HD Camera Quality")}),
			resLabel,
			QStringLiteral("video_capture_width / video_capture_height / video_capture_fps")));
	}

	insertToggle(QStringLiteral("pushToTalkWhenMuted"),
		makeUiPath({kSettings, kAudio, uiTr("Hold Space to temporarily unmute")}),
		QStringLiteral("push_to_talk_when_muted"));
	insertToggle(QStringLiteral("echoCancellation"),
		makeUiPath({kSettings, kAudio, kAudioEnhancement, uiTr("Echo cancellation (AEC)")}),
		QStringLiteral("echo_cancellation"));
	insertToggle(QStringLiteral("noiseSuppression"),
		makeUiPath({kSettings, kAudio, kAudioEnhancement, uiTr("Noise suppression (ANS)")}),
		QStringLiteral("noise_suppression"));
	insertToggle(QStringLiteral("autoGainControl"),
		makeUiPath({kSettings, kAudio, kAudioEnhancement, uiTr("Automatic gain control (AGC)")}),
		QStringLiteral("auto_gain_control"));

	insertToggle(QStringLiteral("quickMeetingsE2ee"),
		makeUiPath({kSettings, kSecurity, kE2ee, uiTr("Encrypt quick meetings")}),
		QStringLiteral("quick_meetings_e2ee"));
	insertToggle(QStringLiteral("screenShareE2ee"),
		makeUiPath({kSettings, kSecurity, kE2ee, uiTr("Encrypt screen sharing")}),
		QStringLiteral("screen_share_e2ee"));
	insertToggle(QStringLiteral("meetingDetailsE2ee"),
		makeUiPath({kSettings, kSecurity, kE2ee, uiTr("Encrypt meetings joined from meeting details")}),
		QStringLiteral("meeting_details_e2ee"));
	insertToggle(QStringLiteral("allMeetingsE2ee"),
		makeUiPath({kSettings, kSecurity, kE2ee, uiTr("Encrypt all meetings")}),
		QStringLiteral("all_meetings_e2ee"));

	if (rawPrefsPayload.contains(QStringLiteral("copilotProfileName"))) {
		details.insert(QStringLiteral("copilotProfileName"), makeDetailEntry(
			makeUiPath({kSettings, kCopilot, uiTr("Display Name")}),
			rawPrefsPayload.value(QStringLiteral("copilotProfileName")).toString(),
			QStringLiteral("copilot_profile_name")));
	}
	if (rawPrefsPayload.contains(QStringLiteral("copilotTemperature"))) {
		details.insert(QStringLiteral("copilotTemperature"), makeDetailEntry(
			makeUiPath({kSettings, kCopilot, uiTr("Temperature")}),
			QString::number(rawPrefsPayload.value(QStringLiteral("copilotTemperature")).toDouble(), 'f', 2),
			QStringLiteral("copilot_temperature")));
	}
	if (rawPrefsPayload.contains(QStringLiteral("copilotMaxContextMessages"))) {
		details.insert(QStringLiteral("copilotMaxContextMessages"), makeDetailEntry(
			makeUiPath({kSettings, kCopilot, uiTr("Max Context Messages")}),
			QString::number(rawPrefsPayload.value(QStringLiteral("copilotMaxContextMessages")).toInt()),
			QStringLiteral("copilot_max_context_messages")));
	}
	if (rawPrefsPayload.contains(QStringLiteral("copilotSendMeetingContextByDefault"))) {
		const bool enabled = rawPrefsPayload.value(QStringLiteral("copilotSendMeetingContextByDefault")).toBool();
		details.insert(QStringLiteral("copilotSendMeetingContextByDefault"), makeDetailEntry(
			{},
			localizedBooleanSettingLabel(enabled),
			QStringLiteral("copilot_send_context_by_default")));
	}
	return details;
}

QString buildClientUiNavigationGuidePrompt() {
	const QString kSettings = uiTr("Settings");
	const QString kGeneral = uiTr("General");
	const QString kVideo = uiTr("Video");
	const QString kAudio = uiTr("Audio");
	const QString kSecurity = uiTr("Security");
	const QString kCopilot = uiTr("Copilot");
	const QString kAbout = uiTr("About");

	QStringList lines;
	lines.push_back(QStringLiteral("[Client UI Navigation & Operations Guide]"));
	lines.push_back(QStringLiteral(
		"Grounding Rule: When explaining how to perform an operation in Cohavora or reporting current client settings, "
		"ALWAYS quote the exact localized menu paths, tab names, button/control labels, and option names below (or in tool 'uiDetails'). "
		"NEVER invent non-existent or removed menu names (e.g., 'Contacts', 'User Center', 'Account / Billing'). "
		"NEVER show raw internal enum IDs (such as 'system', 'light', 'dark', 'teal', 'zh_CN', 'en_US', 'on_hover', 'local_only', 'local_and_remote', 'nvenc', 'qsv', 'amf', 'vaapi') "
		"as user-facing UI option labels unless the user explicitly asks for internal parameter values."));

	// 1. Main Window & Copilot Drawer
	const QString accountMenuItems = bracketedList({
		uiTr("Switch Account / Sign In"),
		uiTr("Sign Out"),
	});
	const QString secondaryActions = bracketedList({
		uiTr("Schedule Meeting"),
		uiTr("Share Screen"),
		uiTr("All Meetings"),
	}, QStringLiteral(" / "));
	lines.push_back(
		QStringLiteral("- Main Window: Top bar ([%1], [%2], [%3]: %4); command bar ([%5] + 9-digit meeting ID input + [%6]); ")
			.arg(uiTr("AI Copilot"), kSettings, uiTr("Account menu"), accountMenuItems,
				uiTr("Start Instant Meeting"), uiTr("Join"))
		+ QStringLiteral("secondary row (%1); schedule card ([%2] with [%3]); bottom [%4] strip (Mic / Camera / Encryption quick toggles, [%5]).")
			.arg(secondaryActions, uiTr("Upcoming Meetings"), uiTr("Refresh"),
				uiTr("Join Presets"), uiTr("More Settings")));

	lines.push_back(QStringLiteral("- [%1] Dialog: %2 / %3, [%4] / [%5], expandable [%6] & [%7].")
		.arg(uiTr("Join Meeting"), uiTr("Meeting ID"), uiTr("Display Name"),
			uiTr("Enable microphone on joining"), uiTr("Enable camera on joining"),
			uiTr("Encryption / Security Settings"), uiTr("Advanced LiveKit Connection")));

	const QString copilotHeaderMenu = bracketedList({
		uiTr("Model Name"),
		uiTr("Reasoning Effort"),
		uiTr("Refresh Available Models"),
		uiTr("Configure Endpoint & API Key..."),
	});
	const QString copilotMoreMenu = bracketedList({
		uiTr("New Chat"),
		uiTr("Clear Conversation"),
		uiTr("Export Conversation (.md)"),
		uiTr("Copilot Settings..."),
	});
	const QString copilotHistoryActions = bracketedList({
		uiTr("+ New Chat"),
		uiTr("Rename..."),
		uiTr("Delete"),
		uiTr("Export .md"),
	});
	lines.push_back(
		QStringLiteral(
			"- [%1] Drawer: header quick actions ([%2], [%3], [%4]/[%5], more menu: %6); [%7] view (%8); "
			"model menu (%9); ")
			.arg(uiTr("AI Copilot"), uiTr("New chat"), uiTr("Open Copilot settings"),
				uiTr("Expand panel width"), uiTr("Restore default panel width"),
				copilotMoreMenu, uiTr("Conversation History"), copilotHistoryActions,
				copilotHeaderMenu)
		+ QStringLiteral(
			"composer ('+' context chips #meeting-chat/#transcript/#telemetry/#participants/#schedule & [%1], '/' slash commands, [%2] voice input card); "
			"footer mode/permission menu ([%3] vs [%4]; [%5], [%6]).")
			.arg(uiTr("Attach image..."), uiTr("Dictation"),
				uiTr("Interactive (Agent tools)"), uiTr("Ask (Read-only Q&A)"),
				uiTr("Manual permissions (confirm actions)"), uiTr("Confirm all tools (strict)")));

	// 2. Settings Dialog (6 exact pages in 2 nav groups + search bar)
	const QString settingsTabs = QStringList{kGeneral, kVideo, kAudio, kSecurity, kCopilot, kAbout}
		.join(QStringLiteral(" | "));
	lines.push_back(QStringLiteral("- [%1] Navigation Tabs (exact 6 pages, with [%2] search bar; groups [%3]: %4/%5/%6; [%7]: %8/%9/%10): %11")
		.arg(kSettings, uiTr("Search settings..."), uiTr("Preferences"), kGeneral, kVideo, kAudio,
			uiTr("Advanced & Integrations"), kSecurity, kCopilot)
		.arg(kAbout, settingsTabs));

	const QString joinDefaultChecks = bracketedList({
		uiTr("Enable camera on joining"),
		uiTr("Enable microphone on joining"),
		uiTr("Use computer audio when joining"),
	});
	const QString displayWindowChecks = bracketedList({
		uiTr("Show the active speaker"),
		uiTr("Stay in the meeting when the screen is locked"),
		uiTr("Quit when the main window is closed"),
	});
	lines.push_back(
		QStringLiteral("  * [%1 > %2]: [%3] ([%4]: %5; [%6]: %7); [%8] (%9); ")
			.arg(kSettings, kGeneral, uiTr("Appearance & Language"),
				uiTr("Theme"), joinChoiceLabels(kThemeChoices),
				uiTr("Language"), joinChoiceLabels(kLanguageChoices),
				uiTr("Default Join Preferences"), joinDefaultChecks)
		+ QStringLiteral("[%1] ([%2]: %3; %4).")
			.arg(uiTr("Meeting Display & Window"), uiTr("Participant names in meetings"),
				joinChoiceLabels(kNameDisplayChoices), displayWindowChecks));

	const QString codecChoices = joinChoiceLabels(kVideoCodecChoices);
	const QString videoPart1 = QStringLiteral(
		"  * [%1 > %2]: [%3] ([%4] device selector, [%5], [%6] + resolution); "
		"[%7] -> [%8] (%9), [%10] (")
		.arg(kSettings, kVideo, uiTr("Camera & Preview"), uiTr("Camera"),
			uiTr("Enable camera on joining"), uiTr("HD Camera Quality"),
			uiTr("Publishing codec"), uiTr("Video encoder"),
			joinChoiceLabels(kVideoEncoderChoices), uiTr("Camera codec"));
	const QString videoPart2 = QStringLiteral(
		"%1), [%2] (%1), [%3] (%4), [%5] (15/20/30 FPS); [%6] -> [%7] (%8), [%9] ")
		.arg(codecChoices, uiTr("Screen share codec"),
			uiTr("Screen share resolution"), joinChoiceLabels(kScreenShareResChoices),
			uiTr("Screen share frame rate"), uiTr("Receive quality & Mirror"),
			uiTr("Simulcast resolution matching"), joinChoiceLabels(kReceiveMatchChoices),
			uiTr("Mirror Video"));
	const QString videoPart3 = QStringLiteral("(%1 / %2).")
		.arg(uiTr("Mirror only my preview"), uiTr("Mirror for all participants"));
	lines.push_back(videoPart1 + videoPart2 + videoPart3);

	const QString audioEnhancements = bracketedList({
		uiTr("Echo cancellation (AEC)"),
		uiTr("Noise suppression (ANS)"),
		uiTr("Automatic gain control (AGC)"),
	});
	lines.push_back(
		QStringLiteral("  * [%1 > %2]: [%3] ([%4] + [%5], [%6] + [%7]); ")
			.arg(kSettings, kAudio, uiTr("Input & Output Devices"),
				uiTr("Speaker"), uiTr("Test Speaker"),
				uiTr("Microphone"), uiTr("Test Microphone"))
		+ QStringLiteral("[%1] ([%2], [%3], [%4]); [%5] (%6).")
			.arg(uiTr("Audio Behavior"),
				uiTr("Enable microphone on joining"),
				uiTr("Use computer audio when joining"),
				uiTr("Hold Space to temporarily unmute"),
				uiTr("Noise Reduction and Audio Enhancement"),
				audioEnhancements));

	const QString e2eeChecks = bracketedList({
		uiTr("Encrypt quick meetings"),
		uiTr("Encrypt screen sharing"),
		uiTr("Encrypt meetings joined from meeting details"),
		uiTr("Encrypt all meetings"),
	});
	lines.push_back(QStringLiteral(
		"  * [%1 > %2]: [%3] (%4); [%5] (session memory only; [%6], [%7]; E2EE supports VP8 / H.264).")
		.arg(kSettings, kSecurity, uiTr("End-to-end encryption (E2EE)"), e2eeChecks,
			uiTr("Encryption key"), uiTr("Save key"), uiTr("Clear key")));

	const QString copilotConnFields = bracketedList({
		uiTr("Active Profile"),
		uiTr("Display Name"),
		uiTr("Protocol Dialect"),
		uiTr("Base URL / Endpoint"),
		uiTr("API Key / Bearer Token"),
		uiTr("Save Key"),
		uiTr("Clear Key"),
		uiTr("Test Connection"),
		uiTr("Persist encrypted API key on this device (DPAPI / Keyring)"),
	});
	const QString copilotModelFields = bracketedList({
		uiTr("Model Name"),
		uiTr("Refresh Models"),
		uiTr("Reasoning Effort"),
		uiTr("Save Copilot chat history locally per account"),
		uiTr("Clear Saved History"),
	});
	const QString copilotPromptFields = bracketedList({
		uiTr("System Prompt"),
		uiTr("Max Context Messages"),
		uiTr("Temperature"),
		uiTr("Connect Timeout"),
		uiTr("Idle Timeout"),
	});
	const QString copilotSttFields = bracketedList({
		uiTr("Speech Endpoint URL (HTTP / WSS)"),
		uiTr("Speech API Key"),
		uiTr("Save Key"),
		uiTr("Clear Key"),
		uiTr("Test Connection"),
		uiTr("Recognition Protocol"),
		uiTr("Speech Model"),
		uiTr("Language"),
		uiTr("Auto-Send Pause"),
		uiTr("VAD Threshold"),
		uiTr("VAD Silence Split"),
	});
	const QString copilotTranslationFields = bracketedList({
		uiTr("Target Language"),
		uiTr("Translation Profile"),
		uiTr("Translation Model Override"),
		uiTr("Translation Prompt"),
		uiTr("Max Concurrent Streaming Connections"),
		uiTr("Idle Stream Release"),
	});
	lines.push_back(
		QStringLiteral("  * [%1 > %2]: [%3] (%4); [%5] (%6); ")
			.arg(kSettings, kCopilot, uiTr("Connection Profile"), copilotConnFields,
				uiTr("Model & Reasoning"), copilotModelFields)
		+ QStringLiteral("[%1] (%2); [%3] (%4); [%5] (%6).")
			.arg(uiTr("Prompt & Runtime Options"), copilotPromptFields,
				uiTr("Speech Recognition & Dictation (ASR / STT)"), copilotSttFields,
				uiTr("Live Translation & Room Agent"), copilotTranslationFields));

	lines.push_back(QStringLiteral("  * [%1 > %2]: version info, [%3].")
		.arg(kSettings, kAbout, uiTr("Check for Updates")));

	// 3. In-Meeting Window Operations
	lines.push_back(QStringLiteral("- In-Meeting Operations:"));

	const QString speakerLayouts = QStringList{
		roomTr("Right member list"),
		roomTr("Top member list"),
		roomTr("Upper L"),
		roomTr("Lower L"),
		QStringLiteral("%1 (%2)").arg(roomTr("Floating list"), roomTr("Open in separate window")),
	}.join(QStringLiteral(" / "));
	lines.push_back(
		QStringLiteral("  * Top Bar: [%1] + duration timer; network signal icon ([%2] with [%3]); Meeting ID badge (click to copy); active speaker pill; ")
			.arg(uiTr("Meetings"), uiTr("Meeting telemetry"), uiTr("Reports"))
		+ QStringLiteral("[%1] popup ([%2] + [%3], [%4]: %5, [%6]); [%7] / [%8] ([%9]) chips.")
			.arg(roomTr("Layout"), roomTr("Gallery"),
				roomTr("%1 per screen").arg(QStringLiteral("9/25/49")), roomTr("Speaker layouts"),
				speakerLayouts, roomTr("Show only speaking people in the member list"),
				uiTr("Console"), uiTr("Simulate"), uiTr("Simulate Scenario")));

	const QString mediaToggles = QStringLiteral("[%1]/[%2], [%3]/[%4], [%5]/[%6]")
		.arg(uiTr("Mute"), uiTr("Unmute"), uiTr("Speaker"), uiTr("Enable Speaker"),
			uiTr("Start Video"), uiTr("Stop Video"));
	const QString shareControls =
		QStringLiteral("[%1]/[%2] (%3: [%4]/[%5], [%6], [%7]; ")
			.arg(uiTr("Share Screen"), uiTr("Stop Sharing"), uiTr("Select a Source to Share"),
				uiTr("Screen"), uiTr("Window"), uiTr("Screen share resolution"),
				uiTr("Screen share frame rate"))
		+ QStringLiteral("active share banner: [%1], [%2], [%3])")
			.arg(uiTr("Share quality"), uiTr("Annotate"), uiTr("Shared audio"));
	lines.push_back(
		QStringLiteral("  * Bottom Toolbar: %1 (click icon to toggle; dropdown arrow or right-click to pick device; hold Space to temporarily unmute); %2; [%3] (copy invite); [%4]; [%5] (text/image/file); [%6]; [%7]; ")
			.arg(mediaToggles, shareControls, uiTr("Invite"), uiTr("Participants"), uiTr("Chat"),
				uiTr("Whiteboard"), uiTr("Copilot"))
		+ QStringLiteral("[%1] (host: [%2] / [%3]; participant: [%4]).")
			.arg(uiTr("End Meeting"), uiTr("Leave Only"), uiTr("End for Everyone"), uiTr("Leave Meeting")));

	const QString hostActions = bracketedList({
		uiTr("Mute Participant"),
		uiTr("Request Unmute"),
		uiTr("Turn Off Participant Video"),
		uiTr("Request Camera On"),
		uiTr("Transfer Host"),
		uiTr("Remove from Meeting"),
	});
	lines.push_back(QStringLiteral(
		"  * [%1] Sidebar & Moderation: search bar; self '...' ([%2]/[%3], [%4]/[%5]); host buttons ([%6], [%7]) & participant '...' (%8).")
		.arg(uiTr("Participants"), uiTr("Mute Myself"), uiTr("Enable Microphone"),
			uiTr("Turn Off Camera"), uiTr("Turn On Camera"),
			uiTr("Mute All"), uiTr("Unmute"), hostActions));

	const QString wbTools = bracketedList({
		wbTr("Pen"),
		wbTr("Highlighter"),
		wbTr("Line"),
		wbTr("Text"),
		wbTr("Eraser"),
		wbTr("Laser"),
		wbTr("Pan"),
	});
	const QString wbPageActions = bracketedList({
		wbTr("Add page"),
		wbTr("Import image"),
		wbTr("Fit page"),
		wbTr("Export PNG"),
		wbTr("Clear page"),
		wbTr("Lock board"),
		wbTr("Participants can draw"),
	});
	lines.push_back(QStringLiteral("  * [%1] Panel: [%2] / [%3]; tools (%4); pages & host controls (%5).")
		.arg(uiTr("Whiteboard"), wbTr("Local whiteboard"), wbTr("Collaborative whiteboard"),
			wbTools, wbPageActions));

	const QString tileMenuItems = QStringLiteral("[%1]/[%2], [%3]/[%4], [%5], [%6]/[%7]")
		.arg(tileTr("Lock this view"), tileTr("Unlock this view"),
			tileTr("Stop receiving all other video"), tileTr("Resume receiving other video"),
			tileTr("Open in separate window"), tileTr("Remote control"), tileTr("End remote control"));
	lines.push_back(QStringLiteral(
		"  * Video Stage & Screen View: tile menu [%1] (%2); multi-page [%3] ([%4], [%5], [%6]).")
		.arg(tileTr("For my layout"), tileMenuItems, roomTr("Video pages"),
			roomTr("Previous video page"), roomTr("Next video page"), roomTr("Jump to page")));

	return lines.join(QLatin1Char('\n'));
}

} // namespace OpenMeeting
