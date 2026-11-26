#include "src/agent/meeting_tool_registry.h"
#include "src/agent/detail/meeting_tool_helpers.h"

#include "src/agent/agent_coordinator.h"
#include "src/agent/agent_ui_grounding.h"
#include "src/core/meeting_coordinator.h"
#include "src/media/audio_device_enumerator.h"
#include "src/media/camera_enumerator.h"
#include "src/net/session_manager.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QSet>

#include <algorithm>
#include <cmath>

namespace OpenMeeting {
using namespace tool_helpers;

namespace {

QString mirrorModeToString(VideoMirrorMode mode) {
	switch (mode) {
	case VideoMirrorMode::LocalOnly:
		return QStringLiteral("local_only");
	case VideoMirrorMode::LocalAndRemote:
		return QStringLiteral("local_and_remote");
	case VideoMirrorMode::Off:
	default:
		return QStringLiteral("off");
	}
}

std::optional<VideoMirrorMode> parseMirrorMode(const QString &raw) {
	const QString norm = raw.trimmed().toLower();
	if (norm == QStringLiteral("off") || norm == QStringLiteral("none")) {
		return VideoMirrorMode::Off;
	}
	if (norm == QStringLiteral("local_only") || norm == QStringLiteral("local")) {
		return VideoMirrorMode::LocalOnly;
	}
	if (norm == QStringLiteral("local_and_remote") || norm == QStringLiteral("all") || norm == QStringLiteral("both")) {
		return VideoMirrorMode::LocalAndRemote;
	}
	return std::nullopt;
}

QString participantNameDisplayToString(ParticipantNameDisplayMode mode) {
	switch (mode) {
	case ParticipantNameDisplayMode::Always:
		return QStringLiteral("always");
	case ParticipantNameDisplayMode::OnHover:
		return QStringLiteral("on_hover");
	case ParticipantNameDisplayMode::Auto:
	default:
		return QStringLiteral("auto");
	}
}

std::optional<ParticipantNameDisplayMode> parseParticipantNameDisplay(const QString &raw) {
	const QString norm = raw.trimmed().toLower();
	if (norm == QStringLiteral("auto")) {
		return ParticipantNameDisplayMode::Auto;
	}
	if (norm == QStringLiteral("always")) {
		return ParticipantNameDisplayMode::Always;
	}
	if (norm == QStringLiteral("on_hover") || norm == QStringLiteral("hover")) {
		return ParticipantNameDisplayMode::OnHover;
	}
	return std::nullopt;
}

QString boolDisplayLabel(bool enabled) {
	return localizedBooleanSettingLabel(enabled);
}

VideoEncoderPreferencePlatform currentEncoderPlatform() {
#if defined(Q_OS_LINUX)
	return VideoEncoderPreferencePlatform::Linux;
#else
	return VideoEncoderPreferencePlatform::Windows;
#endif
}

std::optional<QString> parseVideoCodecPreferenceInput(const QString &raw) {
	const QString norm = raw.trimmed().toLower();
	if (norm == QStringLiteral("auto")) return QStringLiteral("auto");
	if (norm == QStringLiteral("vp8")) return QStringLiteral("vp8");
	if (norm == QStringLiteral("vp9")) return QStringLiteral("vp9");
	if (norm == QStringLiteral("av1")) return QStringLiteral("av1");
	if (norm == QStringLiteral("h264") || norm == QStringLiteral("h.264") || norm == QStringLiteral("avc")) {
		return QStringLiteral("h264");
	}
	if (norm == QStringLiteral("h265") || norm == QStringLiteral("h.265") || norm == QStringLiteral("hevc")) {
		return QStringLiteral("h265");
	}
	return std::nullopt;
}

std::optional<QString> parseThemeInput(const QString &raw) {
	const QString norm = raw.trimmed().toLower();
	if (norm == QStringLiteral("system") || norm == QStringLiteral("auto") || norm == QStringLiteral("follow_system")) {
		return QStringLiteral("system");
	}
	if (norm == QStringLiteral("light")) return QStringLiteral("light");
	if (norm == QStringLiteral("dark")) return QStringLiteral("dark");
	if (norm == QStringLiteral("teal")) return QStringLiteral("teal");
	return std::nullopt;
}

std::optional<QString> parseLanguageInput(const QString &raw) {
	const QString norm = raw.trimmed().toLower();
	if (norm == QStringLiteral("system") || norm == QStringLiteral("auto") || norm == QStringLiteral("follow_system")) {
		return QStringLiteral("system");
	}
	if (norm == QStringLiteral("zh_cn") || norm == QStringLiteral("zh-cn") ||
		norm == QStringLiteral("zh") || norm == QStringLiteral("chinese")) {
		return QStringLiteral("zh_CN");
	}
	if (norm == QStringLiteral("en_us") || norm == QStringLiteral("en-us") ||
		norm == QStringLiteral("en") || norm == QStringLiteral("english")) {
		return QStringLiteral("en_US");
	}
	return std::nullopt;
}

QJsonObject serializeClientPreferences(
		const SessionManager &sessionManager,
		const AgentCoordinator *agentCoordinator = nullptr,
		const AgentHostSettingsBridge *hostSettings = nullptr) {
	const auto &prefs = sessionManager.mediaPreferences();
	const auto &sec = sessionManager.meetingSecurityPreferences();
	QJsonObject payload;
	payload.insert(QStringLiteral("enableMicrophoneOnJoin"), prefs.enableMicrophone);
	payload.insert(QStringLiteral("defaultMutedOnJoin"), !prefs.enableMicrophone);
	payload.insert(QStringLiteral("enableCameraOnJoin"), prefs.enableVideo);
	payload.insert(QStringLiteral("enableSpeakerOnJoin"), prefs.enableSpeaker);
	payload.insert(QStringLiteral("pushToTalkWhenMuted"), prefs.pushToTalkWhenMuted);
	payload.insert(QStringLiteral("echoCancellation"), prefs.echoCancellation);
	payload.insert(QStringLiteral("noiseSuppression"), prefs.noiseSuppression);
	payload.insert(QStringLiteral("autoGainControl"), prefs.autoGainControl);
	payload.insert(QStringLiteral("cameraDeviceId"), prefs.cameraDeviceId);
	payload.insert(QStringLiteral("microphoneDeviceId"), prefs.microphoneDeviceId);
	payload.insert(QStringLiteral("speakerDeviceId"), prefs.speakerDeviceId);
	payload.insert(QStringLiteral("mirrorMode"), mirrorModeToString(prefs.mirrorMode));
	payload.insert(QStringLiteral("receiveResolutionMatch"),
		QString::fromLatin1(livekit::VideoResolutionMatchName(prefs.receiveResolutionMatch)));
	payload.insert(QStringLiteral("videoEncoderPreference"), prefs.videoEncoderPreference);
	payload.insert(QStringLiteral("cameraVideoCodec"), prefs.cameraVideoCodec);
	payload.insert(QStringLiteral("screenShareVideoCodec"), prefs.screenShareVideoCodec);
	payload.insert(QStringLiteral("videoCaptureWidth"), prefs.videoCaptureWidth);
	payload.insert(QStringLiteral("videoCaptureHeight"), prefs.videoCaptureHeight);
	payload.insert(QStringLiteral("videoCaptureFps"), prefs.videoCaptureFps);
	payload.insert(QStringLiteral("screenShareResolution"),
		screenShareResolutionToString(prefs.screenShareResolution));
	payload.insert(QStringLiteral("screenShareFps"), prefs.screenShareFps);
	payload.insert(QStringLiteral("showActiveSpeaker"), prefs.showActiveSpeaker);
	payload.insert(QStringLiteral("participantNameDisplay"),
		participantNameDisplayToString(prefs.participantNameDisplay));
	payload.insert(QStringLiteral("stayInMeetingWhenLocked"), prefs.stayInMeetingWhenLocked);
	payload.insert(QStringLiteral("quitOnMainWindowClose"), prefs.quitOnMainWindowClose);
	payload.insert(QStringLiteral("quickMeetingsE2ee"), sec.quickMeetingsE2ee);
	payload.insert(QStringLiteral("screenShareE2ee"), sec.screenShareE2ee);
	payload.insert(QStringLiteral("meetingDetailsE2ee"), sec.meetingDetailsE2ee);
	payload.insert(QStringLiteral("allMeetingsE2ee"), sec.allMeetingsE2ee);
	payload.insert(QStringLiteral("hasMeetingEncryptionKey"), sessionManager.hasMeetingEncryptionKey());
	if (hostSettings) {
		if (hostSettings->getTheme) {
			payload.insert(QStringLiteral("theme"), hostSettings->getTheme());
		}
		if (hostSettings->getLanguage) {
			payload.insert(QStringLiteral("language"), hostSettings->getLanguage());
		}
	}
	if (agentCoordinator) {
		const auto &ap = agentCoordinator->profile();
		payload.insert(QStringLiteral("copilotProfileName"), ap.name);
		payload.insert(QStringLiteral("copilotTemperature"), ap.temperature);
		payload.insert(QStringLiteral("copilotMaxContextMessages"), ap.maxContextMessages);
		payload.insert(QStringLiteral("copilotSendMeetingContextByDefault"), ap.sendMeetingContextByDefault);
	}
	payload.insert(QStringLiteral("uiDetails"), buildClientPreferencesUiDetails(payload));
	return payload;
}

} // namespace

void MeetingToolRegistry::registerClientSettingsTools() {
	// get_client_preferences (new comprehensive read tool)
	{
		RegisteredTool t;
		t.definition = {
			QStringLiteral("get_client_preferences"),
			QStringLiteral("Read all local client settings (theme, language, audio/video devices, join defaults, 3A audio processing, video encoder backend, camera/screen-share codecs such as H.265/H.264/AV1, mirror mode, resolution/fps preferences, UI display flags, E2EE default switches, and non-secret Copilot settings). Also returns localized 'uiDetails' with exact UI menu paths, current UI labels, and valid localized options."),
			makeObjectSchema({}),
			AgentToolRisk::ReadOnly,
			AgentToolScope::ClientGlobal,
		};
		t.executeReadOnly = [](const QJsonObject &, const AgentToolViewContext &ctx) {
			const QString name = QStringLiteral("get_client_preferences");
			if (!ctx.sessionManager) {
				return makeErrorResult(name, QStringLiteral("Session preferences are unavailable."));
			}
			return makeOkResult(name, serializeClientPreferences(
				*ctx.sessionManager, ctx.agentCoordinator, ctx.hostSettings));
		};
		registerTool(std::move(t));
	}

	// list_media_devices
	{
		RegisteredTool t;
		t.definition = {
			QStringLiteral("list_media_devices"),
			QStringLiteral("List available camera, microphone, and speaker devices along with the currently selected device IDs."),
			makeObjectSchema({}),
			AgentToolRisk::ReadOnly,
			AgentToolScope::ClientGlobal,
		};
		t.executeReadOnly = [](const QJsonObject &, const AgentToolViewContext &ctx) {
			const QString name = QStringLiteral("list_media_devices");
			QJsonArray cameras;
			for (const auto &cam : livekit::CameraEnumerator::EnumerateVideoDevices()) {
				QString id = QString::fromStdString(cam.path);
				if (id.isEmpty()) {
					id = QString::fromStdString(cam.name);
				}
				QJsonObject obj;
				obj.insert(QStringLiteral("id"), id);
				obj.insert(QStringLiteral("name"), QString::fromStdString(cam.name));
				obj.insert(QStringLiteral("isDefault"), cam.is_default);
				cameras.append(obj);
			}
			QJsonArray microphones;
			for (const auto &mic : livekit::AudioDeviceEnumerator::EnumerateInputDevices()) {
				QJsonObject obj;
				obj.insert(QStringLiteral("id"), QString::fromStdString(mic.id));
				obj.insert(QStringLiteral("name"), QString::fromStdString(mic.name));
				obj.insert(QStringLiteral("isDefault"), mic.is_default);
				microphones.append(obj);
			}
			QJsonArray speakers;
			for (const auto &spk : livekit::AudioDeviceEnumerator::EnumerateOutputDevices()) {
				QJsonObject obj;
				obj.insert(QStringLiteral("id"), QString::fromStdString(spk.id));
				obj.insert(QStringLiteral("name"), QString::fromStdString(spk.name));
				obj.insert(QStringLiteral("isDefault"), spk.is_default);
				speakers.append(obj);
			}
			QJsonObject payload;
			payload.insert(QStringLiteral("cameras"), cameras);
			payload.insert(QStringLiteral("microphones"), microphones);
			payload.insert(QStringLiteral("speakers"), speakers);
			if (ctx.sessionManager) {
				const auto &prefs = ctx.sessionManager->mediaPreferences();
				payload.insert(QStringLiteral("selectedCameraDeviceId"), prefs.cameraDeviceId);
				payload.insert(QStringLiteral("selectedMicrophoneDeviceId"), prefs.microphoneDeviceId);
				payload.insert(QStringLiteral("selectedSpeakerDeviceId"), prefs.speakerDeviceId);
			}
			return makeOkResult(name, payload);
		};
		registerTool(std::move(t));
	}

	// get_app_update_status
	{
		RegisteredTool t;
		t.definition = {
			QStringLiteral("get_app_update_status"),
			QStringLiteral("Read the application name, current version, and current software update check/download status (from Settings -> About)."),
			makeObjectSchema({}),
			AgentToolRisk::ReadOnly,
			AgentToolScope::ClientGlobal,
		};
		t.executeReadOnly = [](const QJsonObject &, const AgentToolViewContext &ctx) {
			const QString name = QStringLiteral("get_app_update_status");
			if (!ctx.hostSettings || !ctx.hostSettings->getAppUpdateStatus) {
				return makeErrorResult(name, QStringLiteral("Application update service is unavailable."));
			}
			return makeOkResult(name, ctx.hostSettings->getAppUpdateStatus());
		};
		registerTool(std::move(t));
	}

	// check_for_updates
	{
		RegisteredTool t;
		t.definition = {
			QStringLiteral("check_for_updates"),
			QStringLiteral("Trigger a software update check (equivalent to clicking 'Check for Updates' in Settings -> About) and return the immediate status."),
			makeObjectSchema({}),
			AgentToolRisk::ReadOnly,
			AgentToolScope::ClientGlobal,
		};
		t.executeReadOnly = [](const QJsonObject &, const AgentToolViewContext &ctx) {
			const QString name = QStringLiteral("check_for_updates");
			if (!ctx.hostSettings || !ctx.hostSettings->performAppUpdateAction) {
				return makeErrorResult(name, QStringLiteral("Application update service is unavailable."));
			}
			const QJsonObject res = ctx.hostSettings->performAppUpdateAction(QStringLiteral("check"));
			if (res.contains(QStringLiteral("error")) && !res.value(QStringLiteral("ok")).toBool(true)) {
				return makeErrorResult(name, res.value(QStringLiteral("error")).toString());
			}
			return makeOkResult(name, res);
		};
		registerTool(std::move(t));
	}

	// control_app_update
	{
		RegisteredTool t;
		QJsonObject props;
		props.insert(QStringLiteral("action"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Update action: 'download' (download available update), 'install' (launch verified installer), or 'cancel' (cancel active update check/download).")));
		t.definition = {
			QStringLiteral("control_app_update"),
			QStringLiteral("Download, install, or cancel a client software update (requires user confirmation)."),
			makeObjectSchema(props, {QStringLiteral("action")}),
			AgentToolRisk::RequiresConfirmation,
			AgentToolScope::ClientGlobal,
		};
		t.prepareAction = [](const QJsonObject &args, const AgentToolViewContext &,
				PreparedPendingAction &out, QString &outError) {
			const QString action = args.value(QStringLiteral("action")).toString().trimmed().toLower();
			if (action == QStringLiteral("download")) {
				out.title = QCoreApplication::translate("MeetingUI", "Download Update");
				out.summary = QCoreApplication::translate("MeetingUI", "Download the available Cohavora update package.");
			} else if (action == QStringLiteral("install")) {
				out.title = QCoreApplication::translate("MeetingUI", "Start Installer");
				out.summary = QCoreApplication::translate("MeetingUI", "Launch the verified installer to update Cohavora.");
			} else if (action == QStringLiteral("cancel")) {
				out.title = QCoreApplication::translate("MeetingUI", "Cancel Update");
				out.summary = QCoreApplication::translate("MeetingUI", "Cancel the ongoing update check or download.");
			} else {
				outError = QStringLiteral("Invalid update action '%1'. Use download, install, or cancel.").arg(action);
				return false;
			}
			out.normalizedArgs.insert(QStringLiteral("action"), action);
			return true;
		};
		t.executeConfirmed = [](const QJsonObject &args, const AgentToolExecuteContext &ctx) {
			const QString name = QStringLiteral("control_app_update");
			if (!ctx.hostSettings || !ctx.hostSettings->performAppUpdateAction) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Application update service is unavailable.")));
			}
			const QString action = args.value(QStringLiteral("action")).toString();
			const QJsonObject res = ctx.hostSettings->performAppUpdateAction(action);
			if (!res.value(QStringLiteral("ok")).toBool(true)) {
				return makeSimpleOutcome(makeErrorResult(name, res.value(QStringLiteral("error")).toString(QStringLiteral("Update action failed."))));
			}
			return makeSimpleOutcome(
				makeOkResult(name, res),
				QCoreApplication::translate("MeetingUI", "Executed."));
		};
		registerTool(std::move(t));
	}

	// get_join_media_preferences (hidden legacy alias)
	{
		RegisteredTool t;
		t.definition = {
			QStringLiteral("get_join_media_preferences"),
			QStringLiteral("Read the user's local client Settings -> Audio / Video join preferences."),
			makeObjectSchema({}),
			AgentToolRisk::ReadOnly,
			AgentToolScope::ClientGlobal,
		};
		t.exposedInToolDefinitions = false;
		t.executeReadOnly = [](const QJsonObject &, const AgentToolViewContext &ctx) {
			const QString name = QStringLiteral("get_join_media_preferences");
			if (!ctx.sessionManager) {
				return makeErrorResult(name, QStringLiteral("Session preferences are unavailable."));
			}
			const auto &prefs = ctx.sessionManager->mediaPreferences();
			QJsonObject payload;
			payload.insert(QStringLiteral("enableMicrophoneOnJoin"), prefs.enableMicrophone);
			payload.insert(QStringLiteral("defaultMutedOnJoin"), !prefs.enableMicrophone);
			payload.insert(QStringLiteral("enableCameraOnJoin"), prefs.enableVideo);
			payload.insert(QStringLiteral("enableSpeakerOnJoin"), prefs.enableSpeaker);
			payload.insert(QStringLiteral("pushToTalkWhenMuted"), prefs.pushToTalkWhenMuted);
			return makeOkResult(name, payload);
		};
		registerTool(std::move(t));
	}

	// update_client_preferences (comprehensive settings update tool)
	{
		RegisteredTool t;
		QJsonObject props;
		props.insert(QStringLiteral("enable_microphone_on_join"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Set 'Enable microphone when joining a meeting' (false = muted by default on join).")));
		props.insert(QStringLiteral("enable_camera_on_join"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Set 'Enable camera when joining a meeting'.")));
		props.insert(QStringLiteral("enable_speaker_on_join"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Set 'Use computer audio when joining a meeting'.")));
		props.insert(QStringLiteral("echo_cancellation"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Enable or disable acoustic echo cancellation (AEC).")));
		props.insert(QStringLiteral("noise_suppression"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Enable or disable audio noise suppression (ANS).")));
		props.insert(QStringLiteral("auto_gain_control"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Enable or disable automatic gain control (AGC).")));
		props.insert(QStringLiteral("push_to_talk_when_muted"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Enable or disable push-to-talk (hold Space to unmute temporarily while muted).")));
		props.insert(QStringLiteral("mirror_mode"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Video mirror mode: 'off', 'local_only', or 'local_and_remote'.")));
		props.insert(QStringLiteral("receive_resolution_match"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Remote video resolution matching: 'auto', 'up', 'down', or 'nearest'.")));
		props.insert(QStringLiteral("video_encoder_preference"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Video encoder backend preference (shared by camera and screen share): 'auto' (prefer hardware), 'software', 'nvenc', 'qsv', 'amf' (Windows), or 'vaapi' (Linux).")));
		props.insert(QStringLiteral("camera_video_codec"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Camera video codec: 'auto', 'vp8', 'h264', 'vp9', 'av1', or 'h265' (HEVC).")));
		props.insert(QStringLiteral("screen_share_video_codec"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Screen share video codec: 'auto', 'vp8', 'h264', 'vp9', 'av1', or 'h265' (HEVC).")));
		props.insert(QStringLiteral("video_capture_width"), makeProp(
			QStringLiteral("integer"),
			QStringLiteral("Camera capture width in pixels (0 for automatic ~1080p, or explicit width such as 1280/1920).")));
		props.insert(QStringLiteral("video_capture_height"), makeProp(
			QStringLiteral("integer"),
			QStringLiteral("Camera capture height in pixels (0 for automatic ~1080p, or explicit height such as 720/1080).")));
		props.insert(QStringLiteral("video_capture_fps"), makeProp(
			QStringLiteral("integer"),
			QStringLiteral("Camera capture frame rate (15, 24, 30, or 60).")));
		props.insert(QStringLiteral("screen_share_resolution"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Screen share resolution preset: 'auto', '720p', '1080p', '1440p', 'native', or '2160p'.")));
		props.insert(QStringLiteral("screen_share_fps"), makeProp(
			QStringLiteral("integer"),
			QStringLiteral("Screen share frame rate (15, 20, or 30).")));
		props.insert(QStringLiteral("show_active_speaker"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Highlight the active speaker in meeting layouts.")));
		props.insert(QStringLiteral("participant_name_display"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Participant name overlay display mode: 'auto', 'always', or 'on_hover'.")));
		props.insert(QStringLiteral("stay_in_meeting_when_locked"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Stay in the meeting when the screen is locked.")));
		props.insert(QStringLiteral("quit_on_main_window_close"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Quit application when closing the main window (false minimizes to tray).")));
		props.insert(QStringLiteral("quick_meetings_e2ee"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Enable end-to-end encryption by default for quick meetings.")));
		props.insert(QStringLiteral("screen_share_e2ee"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Enable E2EE by default for screen share.")));
		props.insert(QStringLiteral("meeting_details_e2ee"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Enable E2EE by default for scheduled meeting details.")));
		props.insert(QStringLiteral("all_meetings_e2ee"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Require E2EE for all meetings.")));
		props.insert(QStringLiteral("clear_e2ee_key"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Pass true to clear the in-memory E2EE encryption key.")));
		props.insert(QStringLiteral("theme"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Application UI theme (Settings > General > Theme): pass 'system' (System), 'light' (Light), 'dark' (Dark), or 'teal' (Teal).")));
		props.insert(QStringLiteral("language"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Application UI language (Settings > General > Language, takes effect on next launch): pass 'system' (Follow system), 'zh_CN' (Simplified Chinese), or 'en_US' (English).")));
		props.insert(QStringLiteral("camera_device_id"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Preferred camera device ID (from list_media_devices, or empty string for default).")));
		props.insert(QStringLiteral("microphone_device_id"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Preferred microphone device ID (from list_media_devices, or empty string for default).")));
		props.insert(QStringLiteral("speaker_device_id"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Preferred speaker device ID (from list_media_devices, or empty string for default).")));
		props.insert(QStringLiteral("copilot_profile_name"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Display name of the Copilot profile.")));
		props.insert(QStringLiteral("copilot_temperature"), makeProp(
			QStringLiteral("number"),
			QStringLiteral("Copilot sampling temperature (0.0 to 2.0).")));
		props.insert(QStringLiteral("copilot_max_context_messages"), makeProp(
			QStringLiteral("integer"),
			QStringLiteral("Maximum history messages sent per Copilot request (2 to 64).")));
		props.insert(QStringLiteral("copilot_send_context_by_default"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Automatically attach meeting context when joining a meeting.")));

		t.definition = {
			QStringLiteral("update_client_preferences"),
			QStringLiteral("Update local client settings (theme, language, audio/video devices, join defaults, 3A audio processing, video encoder backend like nvenc/qsv/amf/software, camera/screen-share codecs like h265/h264/av1, mirror mode, capture/share resolution & fps, UI behavior, E2EE switches, and non-secret Copilot settings). Always refer to user-facing Settings labels (or 'uiDetails' in the tool result) instead of raw enum IDs when replying to the user. Requires user confirmation."),
			makeObjectSchema(props),
			AgentToolRisk::RequiresConfirmation,
			AgentToolScope::ClientGlobal,
		};
		t.prepareAction = [](const QJsonObject &args, const AgentToolViewContext &ctx,
				PreparedPendingAction &out, QString &outError) {
			QStringList lines;
			const MediaPreferences *curMedia = ctx.sessionManager ? &ctx.sessionManager->mediaPreferences() : nullptr;
			const MeetingSecurityPreferences *curSec = ctx.sessionManager ? &ctx.sessionManager->meetingSecurityPreferences() : nullptr;

			const auto addBool = [&](const QString &key, const QString &label, std::optional<bool> currentVal) {
				if (!args.contains(key)) {
					return true;
				}
				if (!args.value(key).isBool()) {
					outError = QStringLiteral("Parameter '%1' must be a boolean.").arg(key);
					return false;
				}
				const bool target = args.value(key).toBool();
				out.normalizedArgs.insert(key, target);
				if (currentVal.has_value()) {
					lines.push_back(QStringLiteral("%1: %2 -> %3")
						.arg(label, boolDisplayLabel(*currentVal), boolDisplayLabel(target)));
				} else {
					lines.push_back(QStringLiteral("%1 -> %2").arg(label, boolDisplayLabel(target)));
				}
				return true;
			};

			if (!addBool(QStringLiteral("enable_microphone_on_join"),
					QCoreApplication::translate("MeetingUI", "Enable microphone on joining"),
					curMedia ? std::optional<bool>(curMedia->enableMicrophone) : std::nullopt)) return false;
			if (!addBool(QStringLiteral("enable_camera_on_join"),
					QCoreApplication::translate("MeetingUI", "Enable camera on joining"),
					curMedia ? std::optional<bool>(curMedia->enableVideo) : std::nullopt)) return false;
			if (!addBool(QStringLiteral("enable_speaker_on_join"),
					QCoreApplication::translate("MeetingUI", "Use computer audio when joining"),
					curMedia ? std::optional<bool>(curMedia->enableSpeaker) : std::nullopt)) return false;
			if (!addBool(QStringLiteral("echo_cancellation"),
					QCoreApplication::translate("MeetingUI", "Echo cancellation (AEC)"),
					curMedia ? std::optional<bool>(curMedia->echoCancellation) : std::nullopt)) return false;
			if (!addBool(QStringLiteral("noise_suppression"),
					QCoreApplication::translate("MeetingUI", "Noise suppression (ANS)"),
					curMedia ? std::optional<bool>(curMedia->noiseSuppression) : std::nullopt)) return false;
			if (!addBool(QStringLiteral("auto_gain_control"),
					QCoreApplication::translate("MeetingUI", "Automatic gain control (AGC)"),
					curMedia ? std::optional<bool>(curMedia->autoGainControl) : std::nullopt)) return false;
			if (!addBool(QStringLiteral("push_to_talk_when_muted"),
					QCoreApplication::translate("MeetingUI", "Hold Space to temporarily unmute"),
					curMedia ? std::optional<bool>(curMedia->pushToTalkWhenMuted) : std::nullopt)) return false;
			if (!addBool(QStringLiteral("show_active_speaker"),
					QCoreApplication::translate("MeetingUI", "Show the active speaker"),
					curMedia ? std::optional<bool>(curMedia->showActiveSpeaker) : std::nullopt)) return false;
			if (!addBool(QStringLiteral("stay_in_meeting_when_locked"),
					QCoreApplication::translate("MeetingUI", "Stay in the meeting when the screen is locked"),
					curMedia ? std::optional<bool>(curMedia->stayInMeetingWhenLocked) : std::nullopt)) return false;
			if (!addBool(QStringLiteral("quit_on_main_window_close"),
					QCoreApplication::translate("MeetingUI", "Quit when the main window is closed"),
					curMedia ? std::optional<bool>(curMedia->quitOnMainWindowClose) : std::nullopt)) return false;
			if (!addBool(QStringLiteral("quick_meetings_e2ee"),
					QCoreApplication::translate("MeetingUI", "Encrypt quick meetings"),
					curSec ? std::optional<bool>(curSec->quickMeetingsE2ee) : std::nullopt)) return false;
			if (!addBool(QStringLiteral("screen_share_e2ee"),
					QCoreApplication::translate("MeetingUI", "Encrypt screen sharing"),
					curSec ? std::optional<bool>(curSec->screenShareE2ee) : std::nullopt)) return false;
			if (!addBool(QStringLiteral("meeting_details_e2ee"),
					QCoreApplication::translate("MeetingUI", "Encrypt meetings joined from meeting details"),
					curSec ? std::optional<bool>(curSec->meetingDetailsE2ee) : std::nullopt)) return false;
			if (!addBool(QStringLiteral("all_meetings_e2ee"),
					QCoreApplication::translate("MeetingUI", "Encrypt all meetings"),
					curSec ? std::optional<bool>(curSec->allMeetingsE2ee) : std::nullopt)) return false;

			if (args.contains(QStringLiteral("mirror_mode"))) {
				const QString raw = args.value(QStringLiteral("mirror_mode")).toString();
				const auto modeOpt = parseMirrorMode(raw);
				if (!modeOpt.has_value()) {
					outError = QStringLiteral("Invalid mirror_mode '%1'. Use off, local_only, or local_and_remote.").arg(raw);
					return false;
				}
				const QString norm = mirrorModeToString(*modeOpt);
				out.normalizedArgs.insert(QStringLiteral("mirror_mode"), norm);
				lines.push_back(QStringLiteral("%1 -> %2")
					.arg(QCoreApplication::translate("MeetingUI", "Mirror Video"),
						formatPreferenceDisplayWithParam(localizedMirrorModeLabel(norm), norm)));
			}

			if (args.contains(QStringLiteral("receive_resolution_match"))) {
				const QString raw = args.value(QStringLiteral("receive_resolution_match")).toString().trimmed().toLower();
				if (raw != QStringLiteral("auto") && raw != QStringLiteral("up") &&
					raw != QStringLiteral("down") && raw != QStringLiteral("nearest")) {
					outError = QStringLiteral("Invalid receive_resolution_match '%1'. Use auto, up, down, or nearest.").arg(raw);
					return false;
				}
				out.normalizedArgs.insert(QStringLiteral("receive_resolution_match"), raw);
				lines.push_back(QStringLiteral("%1 -> %2")
					.arg(QCoreApplication::translate("MeetingUI", "Simulcast resolution matching"),
						formatPreferenceDisplayWithParam(localizedReceiveResolutionMatchLabel(raw), raw)));
			}

			if (args.contains(QStringLiteral("video_encoder_preference"))) {
				const QString raw = args.value(QStringLiteral("video_encoder_preference")).toString().trimmed();
				const auto sel = NormalizeVideoEncoderPreference(raw.toStdString(), currentEncoderPlatform());
				if (sel.reason == VideoEncoderPreferenceReason::UnsupportedOnPlatform) {
					outError = QStringLiteral("Video encoder '%1' is not supported on this platform.").arg(raw);
					return false;
				}
				if (sel.reason == VideoEncoderPreferenceReason::InvalidValue) {
					outError = QStringLiteral("Invalid video_encoder_preference '%1'. Use auto, software, nvenc, qsv, amf (Windows), or vaapi (Linux).").arg(raw);
					return false;
				}
				const QString norm = QString::fromStdString(sel.value);
				out.normalizedArgs.insert(QStringLiteral("video_encoder_preference"), norm);
				lines.push_back(QStringLiteral("%1 -> %2")
					.arg(QCoreApplication::translate("MeetingUI", "Video encoder"),
						formatPreferenceDisplayWithParam(localizedVideoEncoderLabel(norm), norm)));
			}

			if (args.contains(QStringLiteral("camera_video_codec"))) {
				const QString raw = args.value(QStringLiteral("camera_video_codec")).toString();
				const auto codecOpt = parseVideoCodecPreferenceInput(raw);
				if (!codecOpt.has_value()) {
					outError = QStringLiteral("Invalid camera_video_codec '%1'. Use auto, vp8, h264, vp9, av1, or h265.").arg(raw);
					return false;
				}
				out.normalizedArgs.insert(QStringLiteral("camera_video_codec"), *codecOpt);
				lines.push_back(QStringLiteral("%1 -> %2")
					.arg(QCoreApplication::translate("MeetingUI", "Camera codec"),
						formatPreferenceDisplayWithParam(localizedVideoCodecLabel(*codecOpt), codecOpt->toUpper())));
			}

			if (args.contains(QStringLiteral("screen_share_video_codec"))) {
				const QString raw = args.value(QStringLiteral("screen_share_video_codec")).toString();
				const auto codecOpt = parseVideoCodecPreferenceInput(raw);
				if (!codecOpt.has_value()) {
					outError = QStringLiteral("Invalid screen_share_video_codec '%1'. Use auto, vp8, h264, vp9, av1, or h265.").arg(raw);
					return false;
				}
				out.normalizedArgs.insert(QStringLiteral("screen_share_video_codec"), *codecOpt);
				lines.push_back(QStringLiteral("%1 -> %2")
					.arg(QCoreApplication::translate("MeetingUI", "Screen share codec"),
						formatPreferenceDisplayWithParam(localizedVideoCodecLabel(*codecOpt), codecOpt->toUpper())));
			}

			if (args.contains(QStringLiteral("video_capture_width")) ||
				args.contains(QStringLiteral("video_capture_height"))) {
				const int w = args.contains(QStringLiteral("video_capture_width"))
					? args.value(QStringLiteral("video_capture_width")).toInt(-1)
					: (curMedia ? curMedia->videoCaptureWidth : 0);
				const int h = args.contains(QStringLiteral("video_capture_height"))
					? args.value(QStringLiteral("video_capture_height")).toInt(-1)
					: (curMedia ? curMedia->videoCaptureHeight : 0);
				if (w < 0 || w > 7680 || h < 0 || h > 4320 || ((w == 0) != (h == 0))) {
					outError = QStringLiteral("video_capture_width and video_capture_height must both be 0 (auto) or valid positive dimensions.");
					return false;
				}
				out.normalizedArgs.insert(QStringLiteral("video_capture_width"), w);
				out.normalizedArgs.insert(QStringLiteral("video_capture_height"), h);
				lines.push_back(QStringLiteral("%1 -> %2")
					.arg(QCoreApplication::translate("MeetingUI", "HD Camera Quality"),
						(w == 0 && h == 0)
							? QCoreApplication::translate("MeetingUI", "Auto")
							: QStringLiteral("%1x%2").arg(w).arg(h)));
			}

			if (args.contains(QStringLiteral("video_capture_fps"))) {
				const int fps = args.value(QStringLiteral("video_capture_fps")).toInt(0);
				if (fps != 15 && fps != 24 && fps != 30 && fps != 60) {
					outError = QStringLiteral("video_capture_fps must be 15, 24, 30, or 60.");
					return false;
				}
				out.normalizedArgs.insert(QStringLiteral("video_capture_fps"), fps);
				lines.push_back(QStringLiteral("%1 -> %2 fps")
					.arg(QCoreApplication::translate("MeetingUI", "Video capture FPS"))
					.arg(fps));
			}

			if (args.contains(QStringLiteral("screen_share_resolution"))) {
				const QString raw = args.value(QStringLiteral("screen_share_resolution")).toString();
				const auto resOpt = parseScreenShareResolution(raw);
				if (!resOpt.has_value()) {
					outError = QStringLiteral("Invalid screen_share_resolution '%1'.").arg(raw);
					return false;
				}
				const QString norm = screenShareResolutionToString(static_cast<int>(*resOpt));
				out.normalizedArgs.insert(QStringLiteral("screen_share_resolution"), norm);
				lines.push_back(QStringLiteral("%1 -> %2")
					.arg(QCoreApplication::translate("MeetingUI", "Screen share resolution"),
						formatPreferenceDisplayWithParam(localizedScreenShareResolutionLabel(norm), norm)));
			}

			if (args.contains(QStringLiteral("screen_share_fps"))) {
				const int fps = args.value(QStringLiteral("screen_share_fps")).toInt(0);
				if (fps != 15 && fps != 20 && fps != 30) {
					outError = QStringLiteral("screen_share_fps must be 15, 20, or 30.");
					return false;
				}
				out.normalizedArgs.insert(QStringLiteral("screen_share_fps"), fps);
				lines.push_back(QStringLiteral("%1 -> %2 FPS")
					.arg(QCoreApplication::translate("MeetingUI", "Screen share frame rate"))
					.arg(fps));
			}

			if (args.contains(QStringLiteral("participant_name_display"))) {
				const QString raw = args.value(QStringLiteral("participant_name_display")).toString();
				const auto modeOpt = parseParticipantNameDisplay(raw);
				if (!modeOpt.has_value()) {
					outError = QStringLiteral("Invalid participant_name_display '%1'. Use auto, always, or on_hover.").arg(raw);
					return false;
				}
				const QString norm = participantNameDisplayToString(*modeOpt);
				out.normalizedArgs.insert(QStringLiteral("participant_name_display"), norm);
				lines.push_back(QStringLiteral("%1 -> %2")
					.arg(QCoreApplication::translate("MeetingUI", "Participant names in meetings"),
						formatPreferenceDisplayWithParam(localizedParticipantNameDisplayLabel(norm), norm)));
			}

			if (args.contains(QStringLiteral("theme"))) {
				const QString raw = args.value(QStringLiteral("theme")).toString();
				const auto themeOpt = parseThemeInput(raw);
				if (!themeOpt.has_value()) {
					outError = QStringLiteral("Invalid theme '%1'. Use system, light, dark, or teal.").arg(raw);
					return false;
				}
				out.normalizedArgs.insert(QStringLiteral("theme"), *themeOpt);
				lines.push_back(QStringLiteral("%1 -> %2")
					.arg(QCoreApplication::translate("MeetingUI", "Theme"),
						formatPreferenceDisplayWithParam(localizedThemeLabel(*themeOpt), *themeOpt)));
			}

			if (args.contains(QStringLiteral("language"))) {
				const QString raw = args.value(QStringLiteral("language")).toString();
				const auto langOpt = parseLanguageInput(raw);
				if (!langOpt.has_value()) {
					outError = QStringLiteral("Invalid language '%1'. Use system, zh_CN, or en_US.").arg(raw);
					return false;
				}
				out.normalizedArgs.insert(QStringLiteral("language"), *langOpt);
				lines.push_back(QStringLiteral("%1 -> %2")
					.arg(QCoreApplication::translate("MeetingUI", "Language"),
						formatPreferenceDisplayWithParam(localizedLanguageLabel(*langOpt), *langOpt)));
			}

			const auto addDeviceId = [&](const QString &key, const QString &label) {
				if (!args.contains(key)) return true;
				if (!args.value(key).isString()) {
					outError = QStringLiteral("Parameter '%1' must be a string.").arg(key);
					return false;
				}
				const QString id = args.value(key).toString().trimmed();
				out.normalizedArgs.insert(key, id);
				lines.push_back(QStringLiteral("%1 -> %2")
					.arg(label, id.isEmpty() ? QCoreApplication::translate("MeetingUI", "System Default") : id));
				return true;
			};
			if (!addDeviceId(QStringLiteral("camera_device_id"),
					QCoreApplication::translate("MeetingUI", "Camera"))) return false;
			if (!addDeviceId(QStringLiteral("microphone_device_id"),
					QCoreApplication::translate("MeetingUI", "Microphone"))) return false;
			if (!addDeviceId(QStringLiteral("speaker_device_id"),
					QCoreApplication::translate("MeetingUI", "Speaker"))) return false;

			if (args.contains(QStringLiteral("clear_e2ee_key"))) {
				if (!args.value(QStringLiteral("clear_e2ee_key")).isBool()) {
					outError = QStringLiteral("Parameter 'clear_e2ee_key' must be a boolean.");
					return false;
				}
				if (args.value(QStringLiteral("clear_e2ee_key")).toBool()) {
					out.normalizedArgs.insert(QStringLiteral("clear_e2ee_key"), true);
					lines.push_back(QCoreApplication::translate("MeetingUI", "Clear key"));
				}
			}

			if (args.contains(QStringLiteral("copilot_profile_name"))) {
				const QString nameVal = args.value(QStringLiteral("copilot_profile_name")).toString().trimmed();
				if (nameVal.isEmpty()) {
					outError = QStringLiteral("copilot_profile_name must not be empty.");
					return false;
				}
				out.normalizedArgs.insert(QStringLiteral("copilot_profile_name"), nameVal.left(64));
				lines.push_back(QStringLiteral("%1 -> %2")
					.arg(QCoreApplication::translate("MeetingUI", "Display Name"), nameVal.left(64)));
			}

			if (args.contains(QStringLiteral("copilot_temperature"))) {
				const double temp = args.value(QStringLiteral("copilot_temperature")).toDouble(-1.0);
				if (!std::isfinite(temp) || temp < 0.0 || temp > 2.0) {
					outError = QStringLiteral("copilot_temperature must be between 0.0 and 2.0.");
					return false;
				}
				out.normalizedArgs.insert(QStringLiteral("copilot_temperature"), temp);
				lines.push_back(QStringLiteral("%1 -> %2")
					.arg(QCoreApplication::translate("MeetingUI", "Temperature"))
					.arg(temp, 0, 'f', 2));
			}

			if (args.contains(QStringLiteral("copilot_max_context_messages"))) {
				const int maxCtx = args.value(QStringLiteral("copilot_max_context_messages")).toInt(0);
				if (maxCtx < 2 || maxCtx > 64) {
					outError = QStringLiteral("copilot_max_context_messages must be between 2 and 64.");
					return false;
				}
				out.normalizedArgs.insert(QStringLiteral("copilot_max_context_messages"), maxCtx);
				lines.push_back(QStringLiteral("%1 -> %2")
					.arg(QCoreApplication::translate("MeetingUI", "Max Context Messages"))
					.arg(maxCtx));
			}

			if (!addBool(QStringLiteral("copilot_send_context_by_default"),
					QCoreApplication::translate("MeetingUI", "Send meeting context by default"),
					ctx.agentCoordinator ? std::optional<bool>(ctx.agentCoordinator->profile().sendMeetingContextByDefault) : std::nullopt)) return false;

			if (lines.isEmpty()) {
				outError = QStringLiteral("At least one valid client preference field must be specified.");
				return false;
			}
			out.title = QCoreApplication::translate("MeetingUI", "Update Client Settings");
			out.summary = lines.join(QStringLiteral("\n"));
			return true;
		};
		t.executeConfirmed = [](const QJsonObject &args, const AgentToolExecuteContext &ctx) {
			const QString name = QStringLiteral("update_client_preferences");
			if (!ctx.sessionManager) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Session preferences are unavailable.")));
			}

			MediaPreferences prefs = ctx.sessionManager->mediaPreferences();
			bool mediaChanged = false;
			const auto applyBool = [&](const QString &key, bool &field) {
				if (args.contains(key) && args.value(key).isBool()) {
					field = args.value(key).toBool();
					mediaChanged = true;
				}
			};
			applyBool(QStringLiteral("enable_microphone_on_join"), prefs.enableMicrophone);
			applyBool(QStringLiteral("enable_camera_on_join"), prefs.enableVideo);
			applyBool(QStringLiteral("enable_speaker_on_join"), prefs.enableSpeaker);
			applyBool(QStringLiteral("echo_cancellation"), prefs.echoCancellation);
			applyBool(QStringLiteral("noise_suppression"), prefs.noiseSuppression);
			applyBool(QStringLiteral("auto_gain_control"), prefs.autoGainControl);
			applyBool(QStringLiteral("push_to_talk_when_muted"), prefs.pushToTalkWhenMuted);
			applyBool(QStringLiteral("show_active_speaker"), prefs.showActiveSpeaker);
			applyBool(QStringLiteral("stay_in_meeting_when_locked"), prefs.stayInMeetingWhenLocked);
			applyBool(QStringLiteral("quit_on_main_window_close"), prefs.quitOnMainWindowClose);

			if (args.contains(QStringLiteral("mirror_mode"))) {
				if (const auto m = parseMirrorMode(args.value(QStringLiteral("mirror_mode")).toString())) {
					prefs.mirrorMode = *m;
					mediaChanged = true;
				}
			}
			if (args.contains(QStringLiteral("receive_resolution_match"))) {
				const QByteArray val = args.value(QStringLiteral("receive_resolution_match")).toString().toLatin1();
				prefs.receiveResolutionMatch = livekit::ParseVideoResolutionMatch(val.constData());
				mediaChanged = true;
			}
			if (args.contains(QStringLiteral("video_encoder_preference"))) {
				prefs.videoEncoderPreference = normalizeVideoEncoderPreference(
					args.value(QStringLiteral("video_encoder_preference")).toString());
				mediaChanged = true;
			}
			if (args.contains(QStringLiteral("camera_video_codec"))) {
				prefs.cameraVideoCodec = normalizeVideoCodecPreference(
					args.value(QStringLiteral("camera_video_codec")).toString());
				mediaChanged = true;
			}
			if (args.contains(QStringLiteral("screen_share_video_codec"))) {
				prefs.screenShareVideoCodec = normalizeVideoCodecPreference(
					args.value(QStringLiteral("screen_share_video_codec")).toString());
				mediaChanged = true;
			}
			if (args.contains(QStringLiteral("video_capture_width")) &&
				args.contains(QStringLiteral("video_capture_height"))) {
				prefs.videoCaptureWidth = args.value(QStringLiteral("video_capture_width")).toInt(prefs.videoCaptureWidth);
				prefs.videoCaptureHeight = args.value(QStringLiteral("video_capture_height")).toInt(prefs.videoCaptureHeight);
				mediaChanged = true;
			}
			if (args.contains(QStringLiteral("video_capture_fps"))) {
				prefs.videoCaptureFps = args.value(QStringLiteral("video_capture_fps")).toInt(prefs.videoCaptureFps);
				mediaChanged = true;
			}
			if (args.contains(QStringLiteral("screen_share_resolution"))) {
				if (const auto r = parseScreenShareResolution(args.value(QStringLiteral("screen_share_resolution")).toString())) {
					prefs.screenShareResolution = static_cast<int>(*r);
					mediaChanged = true;
				}
			}
			if (args.contains(QStringLiteral("screen_share_fps"))) {
				prefs.screenShareFps = args.value(QStringLiteral("screen_share_fps")).toInt(prefs.screenShareFps);
				mediaChanged = true;
			}
			if (args.contains(QStringLiteral("participant_name_display"))) {
				if (const auto d = parseParticipantNameDisplay(args.value(QStringLiteral("participant_name_display")).toString())) {
					prefs.participantNameDisplay = *d;
					mediaChanged = true;
				}
			}
			if (args.contains(QStringLiteral("camera_device_id"))) {
				prefs.cameraDeviceId = args.value(QStringLiteral("camera_device_id")).toString();
				mediaChanged = true;
			}
			if (args.contains(QStringLiteral("microphone_device_id"))) {
				prefs.microphoneDeviceId = args.value(QStringLiteral("microphone_device_id")).toString();
				mediaChanged = true;
			}
			if (args.contains(QStringLiteral("speaker_device_id"))) {
				prefs.speakerDeviceId = args.value(QStringLiteral("speaker_device_id")).toString();
				mediaChanged = true;
			}
			if (mediaChanged) {
				ctx.sessionManager->setMediaPreferences(prefs);
			}

			MeetingSecurityPreferences sec = ctx.sessionManager->meetingSecurityPreferences();
			bool secChanged = false;
			const auto applySecBool = [&](const QString &key, bool &field) {
				if (args.contains(key) && args.value(key).isBool()) {
					field = args.value(key).toBool();
					secChanged = true;
				}
			};
			applySecBool(QStringLiteral("quick_meetings_e2ee"), sec.quickMeetingsE2ee);
			applySecBool(QStringLiteral("screen_share_e2ee"), sec.screenShareE2ee);
			applySecBool(QStringLiteral("meeting_details_e2ee"), sec.meetingDetailsE2ee);
			applySecBool(QStringLiteral("all_meetings_e2ee"), sec.allMeetingsE2ee);
			if (secChanged) {
				ctx.sessionManager->setMeetingSecurityPreferences(sec);
			}
			bool otherChanged = false;
			if (args.value(QStringLiteral("clear_e2ee_key")).toBool(false)) {
				ctx.sessionManager->clearMeetingEncryptionKey();
				otherChanged = true;
			}
			if (args.contains(QStringLiteral("theme")) && ctx.hostSettings && ctx.hostSettings->setTheme) {
				if (ctx.hostSettings->setTheme(args.value(QStringLiteral("theme")).toString())) {
					otherChanged = true;
				}
			}
			if (args.contains(QStringLiteral("language")) && ctx.hostSettings && ctx.hostSettings->setLanguage) {
				if (ctx.hostSettings->setLanguage(args.value(QStringLiteral("language")).toString())) {
					otherChanged = true;
				}
			}
			if (ctx.agentCoordinator && (
					args.contains(QStringLiteral("copilot_profile_name")) ||
					args.contains(QStringLiteral("copilot_temperature")) ||
					args.contains(QStringLiteral("copilot_max_context_messages")) ||
					args.contains(QStringLiteral("copilot_send_context_by_default")))) {
				AgentProfile ap = ctx.agentCoordinator->profile();
				if (args.contains(QStringLiteral("copilot_profile_name"))) {
					ap.name = args.value(QStringLiteral("copilot_profile_name")).toString();
				}
				if (args.contains(QStringLiteral("copilot_temperature"))) {
					ap.temperature = args.value(QStringLiteral("copilot_temperature")).toDouble(ap.temperature);
				}
				if (args.contains(QStringLiteral("copilot_max_context_messages"))) {
					ap.maxContextMessages = args.value(QStringLiteral("copilot_max_context_messages")).toInt(ap.maxContextMessages);
				}
				if (args.contains(QStringLiteral("copilot_send_context_by_default"))) {
					ap.sendMeetingContextByDefault = args.value(QStringLiteral("copilot_send_context_by_default")).toBool(ap.sendMeetingContextByDefault);
				}
				ctx.agentCoordinator->setProfile(ap);
				otherChanged = true;
			}

			if (!mediaChanged && !secChanged && !otherChanged) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("No valid client preference specified.")));
			}

			return makeSimpleOutcome(
				makeOkResult(name, serializeClientPreferences(
					*ctx.sessionManager, ctx.agentCoordinator, ctx.hostSettings)),
				QCoreApplication::translate("MeetingUI", "Settings updated."),
				QCoreApplication::translate("MeetingUI", "Client settings have been updated."));
		};
		registerTool(std::move(t));
	}

	// set_join_media_preferences (hidden legacy alias)
	{
		RegisteredTool t;
		QJsonObject joinPrefProps;
		joinPrefProps.insert(QStringLiteral("enable_microphone_on_join"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Set local client setting 'Enable microphone when joining a meeting'.")));
		joinPrefProps.insert(QStringLiteral("enable_camera_on_join"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Optional: set local client setting 'Enable camera when joining a meeting'.")));
		joinPrefProps.insert(QStringLiteral("enable_speaker_on_join"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Optional: set local client setting 'Use computer audio when joining a meeting'.")));
		t.definition = {
			QStringLiteral("set_join_media_preferences"),
			QStringLiteral("Update the user's local client Settings -> Audio / Video join preferences."),
			makeObjectSchema(joinPrefProps),
			AgentToolRisk::RequiresConfirmation,
			AgentToolScope::ClientGlobal,
		};
		t.exposedInToolDefinitions = false;
		t.prepareAction = [](const QJsonObject &args, const AgentToolViewContext &ctx,
				PreparedPendingAction &out, QString &outError) {
			const bool hasMic = args.contains(QStringLiteral("enable_microphone_on_join")) &&
				args.value(QStringLiteral("enable_microphone_on_join")).isBool();
			const bool hasCam = args.contains(QStringLiteral("enable_camera_on_join")) &&
				args.value(QStringLiteral("enable_camera_on_join")).isBool();
			const bool hasSpk = args.contains(QStringLiteral("enable_speaker_on_join")) &&
				args.value(QStringLiteral("enable_speaker_on_join")).isBool();
			if (!hasMic && !hasCam && !hasSpk) {
				outError = QStringLiteral(
					"At least one boolean preference ('enable_microphone_on_join', 'enable_camera_on_join', "
					"or 'enable_speaker_on_join') must be specified.");
				return false;
			}

			const auto stateLabel = [](bool enabled) {
				return enabled
					? QCoreApplication::translate("MeetingUI", "Enabled")
					: QCoreApplication::translate("MeetingUI", "Disabled (Muted on Join)");
			};

			QStringList lines;
			if (hasMic) {
				const bool targetMic = args.value(QStringLiteral("enable_microphone_on_join")).toBool();
				if (ctx.sessionManager) {
					const bool curMic = ctx.sessionManager->mediaPreferences().enableMicrophone;
					lines.push_back(QCoreApplication::translate(
						"MeetingUI",
						"Enable microphone when joining a meeting: %1 -> %2")
						.arg(stateLabel(curMic), stateLabel(targetMic)));
				} else {
					lines.push_back(QCoreApplication::translate(
						"MeetingUI",
						"Enable microphone when joining a meeting -> %1")
						.arg(stateLabel(targetMic)));
				}
			}
			if (hasCam) {
				const bool targetCam = args.value(QStringLiteral("enable_camera_on_join")).toBool();
				if (ctx.sessionManager) {
					const bool curCam = ctx.sessionManager->mediaPreferences().enableVideo;
					lines.push_back(QCoreApplication::translate(
						"MeetingUI",
						"Enable camera when joining a meeting: %1 -> %2")
						.arg(boolDisplayLabel(curCam), boolDisplayLabel(targetCam)));
				} else {
					lines.push_back(QCoreApplication::translate(
						"MeetingUI",
						"Enable camera when joining a meeting -> %1")
						.arg(boolDisplayLabel(targetCam)));
				}
			}
			if (hasSpk) {
				const bool targetSpk = args.value(QStringLiteral("enable_speaker_on_join")).toBool();
				if (ctx.sessionManager) {
					const bool curSpk = ctx.sessionManager->mediaPreferences().enableSpeaker;
					lines.push_back(QCoreApplication::translate(
						"MeetingUI",
						"Use computer audio when joining a meeting: %1 -> %2")
						.arg(boolDisplayLabel(curSpk), boolDisplayLabel(targetSpk)));
				} else {
					lines.push_back(QCoreApplication::translate(
						"MeetingUI",
						"Use computer audio when joining a meeting -> %1")
						.arg(boolDisplayLabel(targetSpk)));
				}
			}

			out.title = QCoreApplication::translate("MeetingUI", "Update Join Media Settings");
			out.summary = lines.join(QStringLiteral("\n"));
			out.normalizedArgs = args;
			return true;
		};
		t.executeConfirmed = [](const QJsonObject &args, const AgentToolExecuteContext &ctx) {
			const QString name = QStringLiteral("set_join_media_preferences");
			if (!ctx.sessionManager) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Session preferences are unavailable.")));
			}
			const bool hasMic = args.contains(QStringLiteral("enable_microphone_on_join")) &&
				args.value(QStringLiteral("enable_microphone_on_join")).isBool();
			const bool hasCam = args.contains(QStringLiteral("enable_camera_on_join")) &&
				args.value(QStringLiteral("enable_camera_on_join")).isBool();
			const bool hasSpk = args.contains(QStringLiteral("enable_speaker_on_join")) &&
				args.value(QStringLiteral("enable_speaker_on_join")).isBool();
			if (!hasMic && !hasCam && !hasSpk) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("No valid join media preference specified.")));
			}
			if (hasMic) {
				ctx.sessionManager->setEnableMicrophone(args.value(QStringLiteral("enable_microphone_on_join")).toBool());
			}
			if (hasCam) {
				ctx.sessionManager->setEnableVideo(args.value(QStringLiteral("enable_camera_on_join")).toBool());
			}
			if (hasSpk) {
				ctx.sessionManager->setEnableSpeaker(args.value(QStringLiteral("enable_speaker_on_join")).toBool());
			}
			const auto &prefs = ctx.sessionManager->mediaPreferences();
			QJsonObject payload;
			payload.insert(QStringLiteral("enableMicrophoneOnJoin"), prefs.enableMicrophone);
			payload.insert(QStringLiteral("defaultMutedOnJoin"), !prefs.enableMicrophone);
			payload.insert(QStringLiteral("enableCameraOnJoin"), prefs.enableVideo);
			payload.insert(QStringLiteral("enableSpeakerOnJoin"), prefs.enableSpeaker);
			return makeSimpleOutcome(
				makeOkResult(name, payload),
				QCoreApplication::translate("MeetingUI", "Settings updated."),
				MeetingToolRegistry::formatJoinMediaPreferencesCompletionSummary(args));
		};
		registerTool(std::move(t));
	}
}

} // namespace OpenMeeting
