#include "src/agent/meeting_tool_registry.h"
#include "src/agent/detail/meeting_tool_helpers.h"

#include "src/agent/agent_coordinator.h"
#include "src/core/meeting_coordinator.h"
#include "src/net/session_manager.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QSet>

#include <algorithm>

namespace OpenMeeting {
using namespace tool_helpers;

namespace {

constexpr int kMaxListedParticipants = 50;
constexpr int kDefaultChatLimit = 20;
constexpr int kMaxChatLimit = 50;
constexpr int kMaxChatSummaryTextLength = 500;
constexpr int kMaxOutboundChatLength = 1000;

QString connectionQualityLabel(livekit::ConnectionQuality q) {
	switch (q) {
	case livekit::ConnectionQuality::Poor:
		return QStringLiteral("poor");
	case livekit::ConnectionQuality::Good:
		return QStringLiteral("good");
	case livekit::ConnectionQuality::Excellent:
		return QStringLiteral("excellent");
	case livekit::ConnectionQuality::Lost:
		return QStringLiteral("lost");
	case livekit::ConnectionQuality::Unknown:
	default:
		return QStringLiteral("unknown");
	}
}

QString resolveParticipantDisplayName(const MeetingCoordinator *meeting, const QString &userId) {
	if (!meeting || userId.isEmpty()) {
		return userId;
	}
	for (const auto &p : meeting->participants()) {
		if (p.identity == userId) {
			return p.name.trimmed().isEmpty() ? userId : p.name.trimmed();
		}
	}
	return userId;
}

} // namespace

void MeetingToolRegistry::registerMeetingControlTools() {
	// get_meeting_overview
	{
		RegisteredTool t;
		t.definition = {
			QStringLiteral("get_meeting_overview"),
			QStringLiteral("Get a safe summary of the active meeting (ID, title, encryption, participant count, and local media state)."),
			makeObjectSchema({}),
			AgentToolRisk::ReadOnly,
			AgentToolScope::ActiveMeeting,
		};
		t.executeReadOnly = [](const QJsonObject &, const AgentToolViewContext &ctx) {
			if (!ctx.meeting || ctx.meeting->state() == MeetingState::Idle || !ctx.meetingOverview.inMeeting) {
				return makeErrorResult(QStringLiteral("get_meeting_overview"), QStringLiteral("No active meeting session."));
			}
			QJsonObject payload;
			payload.insert(QStringLiteral("meetingId"), ctx.meetingOverview.meetingId);
			payload.insert(QStringLiteral("meetingName"), ctx.meetingOverview.meetingName);
			payload.insert(QStringLiteral("encryptedMeeting"), ctx.meetingOverview.encryptedMeeting);
			payload.insert(QStringLiteral("participantCount"), ctx.meetingOverview.participantCount);
			payload.insert(QStringLiteral("micMuted"), ctx.meetingOverview.micMuted);
			payload.insert(QStringLiteral("cameraEnabled"), ctx.meetingOverview.cameraEnabled);
			payload.insert(QStringLiteral("screenSharing"), ctx.meetingOverview.screenSharing);
			payload.insert(QStringLiteral("isHost"), ctx.meeting->isHost());
			return makeOkResult(QStringLiteral("get_meeting_overview"), payload);
		};
		registerTool(std::move(t));
	}

	// list_participants
	{
		RegisteredTool t;
		t.definition = {
			QStringLiteral("list_participants"),
			QStringLiteral("List current meeting participants with userId, display name, host/local role, mic/camera state, and connection quality."),
			makeObjectSchema({}),
			AgentToolRisk::ReadOnly,
			AgentToolScope::ActiveMeeting,
		};
		t.executeReadOnly = [](const QJsonObject &, const AgentToolViewContext &ctx) {
			if (!ctx.meeting || ctx.meeting->state() == MeetingState::Idle || !ctx.meetingOverview.inMeeting) {
				return makeErrorResult(QStringLiteral("list_participants"), QStringLiteral("No active meeting session."));
			}
			const auto rawList = ctx.meeting->participants();
			std::vector<ParticipantInfo> list;
			list.reserve(rawList.size());
			for (const auto &p : rawList) {
				if (!p.isAgent) {
					list.push_back(p);
				}
			}
			QJsonArray items;
			const int limit = std::min<int>(static_cast<int>(list.size()), kMaxListedParticipants);
			for (int i = 0; i < limit; ++i) {
				const auto &p = list[i];
				QJsonObject row;
				row.insert(QStringLiteral("userId"), p.identity);
				row.insert(QStringLiteral("name"), p.name.trimmed().isEmpty() ? p.identity : p.name.trimmed());
				row.insert(QStringLiteral("isLocal"), p.isLocal);
				row.insert(QStringLiteral("isHost"), p.isHost);
				row.insert(QStringLiteral("micMuted"), p.isAudioMuted);
				row.insert(QStringLiteral("cameraEnabled"), p.isVideoEnabled);
				row.insert(QStringLiteral("speaking"), p.isSpeaking);
				row.insert(QStringLiteral("connectionQuality"), connectionQualityLabel(p.connectionQuality));
				items.append(row);
			}
			QJsonObject payload;
			payload.insert(QStringLiteral("totalCount"), static_cast<int>(list.size()));
			payload.insert(QStringLiteral("truncated"), static_cast<int>(list.size()) > limit);
			payload.insert(QStringLiteral("participants"), items);
			return makeOkResult(QStringLiteral("list_participants"), payload);
		};
		registerTool(std::move(t));
	}

	// get_recent_chat
	{
		RegisteredTool t;
		QJsonObject chatProps;
		chatProps.insert(QStringLiteral("limit"), makeProp(
			QStringLiteral("integer"),
			QStringLiteral("Maximum number of recent chat messages to return (1-50, default 20).")));
		t.definition = {
			QStringLiteral("get_recent_chat"),
			QStringLiteral("Read recent in-meeting text chat messages."),
			makeObjectSchema(chatProps),
			AgentToolRisk::ReadOnly,
			AgentToolScope::ActiveMeeting,
		};
		t.executeReadOnly = [](const QJsonObject &args, const AgentToolViewContext &ctx) {
			if (!ctx.meeting || ctx.meeting->state() == MeetingState::Idle || !ctx.meetingOverview.inMeeting) {
				return makeErrorResult(QStringLiteral("get_recent_chat"), QStringLiteral("No active meeting session."));
			}
			const QVector<AgentMeetingChatEntry> emptyChat;
			const QVector<AgentMeetingChatEntry> &recentChat = ctx.recentChat ? *ctx.recentChat : emptyChat;
			const int rawLimit = args.value(QStringLiteral("limit")).toInt(kDefaultChatLimit);
			const int limit = qBound(1, rawLimit, kMaxChatLimit);
			const int total = recentChat.size();
			const int start = std::max(0, total - limit);
			QJsonArray items;
			for (int i = start; i < total; ++i) {
				const auto &entry = recentChat.at(i);
				QJsonObject row;
				row.insert(QStringLiteral("sender"), entry.senderName);
				row.insert(QStringLiteral("isLocal"), entry.isLocal);
				row.insert(QStringLiteral("text"), entry.text.left(kMaxChatSummaryTextLength));
				if (entry.timestamp.isValid()) {
					row.insert(QStringLiteral("time"), entry.timestamp.toString(QStringLiteral("HH:mm:ss")));
				}
				items.append(row);
			}
			QJsonObject payload;
			payload.insert(QStringLiteral("totalCount"), total);
			payload.insert(QStringLiteral("messages"), items);
			return makeOkResult(QStringLiteral("get_recent_chat"), payload);
		};
		registerTool(std::move(t));
	}

	// get_recent_transcripts
	{
		RegisteredTool t;
		QJsonObject transcriptProps;
		transcriptProps.insert(QStringLiteral("limit"), makeProp(
			QStringLiteral("integer"),
			QStringLiteral("Maximum number of recent voice transcript segments to return (1-50, default 30).")));
		t.definition = {
			QStringLiteral("get_recent_transcripts"),
			QStringLiteral("Read recent live or finalized spoken voice transcripts from participants and AI voice agents in the active meeting."),
			makeObjectSchema(transcriptProps),
			AgentToolRisk::ReadOnly,
			AgentToolScope::ActiveMeeting,
		};
		t.executeReadOnly = [](const QJsonObject &args, const AgentToolViewContext &ctx) {
			if (!ctx.meeting || ctx.meeting->state() == MeetingState::Idle || !ctx.meetingOverview.inMeeting) {
				return makeErrorResult(QStringLiteral("get_recent_transcripts"), QStringLiteral("No active meeting session."));
			}
			const QVector<AgentMeetingTranscriptEntry> emptyTranscripts;
			const QVector<AgentMeetingTranscriptEntry> &recentTranscripts =
				ctx.recentTranscripts ? *ctx.recentTranscripts : emptyTranscripts;
			const int rawLimit = args.value(QStringLiteral("limit")).toInt(30);
			const int limit = qBound(1, rawLimit, kMaxChatLimit);
			const int total = recentTranscripts.size();
			const int start = std::max(0, total - limit);
			QJsonArray items;
			for (int i = start; i < total; ++i) {
				const auto &entry = recentTranscripts.at(i);
				QJsonObject row;
				row.insert(QStringLiteral("speaker"),
					entry.speakerName.trimmed().isEmpty() ? entry.speakerIdentity : entry.speakerName);
				row.insert(QStringLiteral("speakerId"), entry.speakerIdentity);
				row.insert(QStringLiteral("isLocal"), entry.isLocalSpeaker);
				row.insert(QStringLiteral("isAgentGenerated"), entry.isAgentGenerated);
				row.insert(QStringLiteral("isFinal"), entry.isFinal);
				if (!entry.language.isEmpty()) {
					row.insert(QStringLiteral("language"), entry.language);
				}
				row.insert(QStringLiteral("text"), entry.text.left(kMaxChatSummaryTextLength));
				if (entry.timestampMs > 0) {
					row.insert(
						QStringLiteral("time"),
						QDateTime::fromMSecsSinceEpoch(entry.timestampMs).toString(QStringLiteral("HH:mm:ss")));
				}
				items.append(row);
			}
			QJsonObject payload;
			payload.insert(QStringLiteral("totalCount"), total);
			payload.insert(QStringLiteral("transcripts"), items);
			return makeOkResult(QStringLiteral("get_recent_transcripts"), payload);
		};
		registerTool(std::move(t));
	}

	// get_telemetry_snapshot
	{
		RegisteredTool t;
		t.definition = {
			QStringLiteral("get_telemetry_snapshot"),
			QStringLiteral("Read the latest safe projected meeting telemetry snapshot (network quality, RTT, packet loss, bitrate, reconnect stats)."),
			makeObjectSchema({}),
			AgentToolRisk::ReadOnly,
			AgentToolScope::ActiveMeeting,
		};
		t.executeReadOnly = [](const QJsonObject &, const AgentToolViewContext &ctx) {
			if (!ctx.meeting || ctx.meeting->state() == MeetingState::Idle || !ctx.meetingOverview.inMeeting) {
				return makeErrorResult(QStringLiteral("get_telemetry_snapshot"), QStringLiteral("No active meeting session."));
			}
			QJsonObject payload;
			if (!ctx.telemetrySnapshot || ctx.telemetrySnapshot->isEmpty()) {
				payload.insert(QStringLiteral("available"), false);
			} else {
				payload.insert(QStringLiteral("available"), true);
				payload.insert(QStringLiteral("snapshot"), QJsonObject::fromVariantMap(*ctx.telemetrySnapshot));
			}
			return makeOkResult(QStringLiteral("get_telemetry_snapshot"), payload);
		};
		registerTool(std::move(t));
	}

	// set_microphone_muted
	{
		RegisteredTool t;
		QJsonObject micProps;
		micProps.insert(QStringLiteral("muted"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("True to mute the local microphone, false to unmute.")));
		t.definition = {
			QStringLiteral("set_microphone_muted"),
			QStringLiteral("Mute or unmute the local microphone (requires user confirmation)."),
			makeObjectSchema(micProps, {QStringLiteral("muted")}),
			AgentToolRisk::RequiresConfirmation,
			AgentToolScope::ActiveMeeting,
		};
		t.prepareAction = [](const QJsonObject &args, const AgentToolViewContext &,
				PreparedPendingAction &out, QString &outError) {
			const auto val = args.value(QStringLiteral("muted"));
			if (!val.isBool()) {
				outError = QStringLiteral("Missing boolean parameter 'muted'.");
				return false;
			}
			const bool muted = val.toBool();
			out.title = muted
				? QCoreApplication::translate("MeetingUI", "Mute Microphone")
				: QCoreApplication::translate("MeetingUI", "Unmute Microphone");
			out.summary = muted
				? QCoreApplication::translate("MeetingUI", "Turn off your microphone in the active meeting.")
				: QCoreApplication::translate("MeetingUI", "Turn on your microphone in the active meeting.");
			out.normalizedArgs = args;
			return true;
		};
		t.executeConfirmed = [](const QJsonObject &args, const AgentToolExecuteContext &ctx) {
			const QString name = QStringLiteral("set_microphone_muted");
			if (!ctx.meeting || ctx.meeting->state() == MeetingState::Idle) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Meeting session is no longer active.")));
			}
			const auto val = args.value(QStringLiteral("muted"));
			if (!val.isBool()) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Missing boolean parameter 'muted'.")));
			}
			const bool muted = val.toBool();
			ctx.meeting->setLocalAudioMuted(muted);
			QJsonObject payload;
			payload.insert(QStringLiteral("micMuted"), ctx.meeting->isLocalAudioMuted());
			return makeSimpleOutcome(
				makeOkResult(name, payload),
				QCoreApplication::translate("MeetingUI", "Executed."));
		};
		registerTool(std::move(t));
	}

	// set_camera_enabled
	{
		RegisteredTool t;
		QJsonObject camProps;
		camProps.insert(QStringLiteral("enabled"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("True to turn on the local camera, false to turn it off.")));
		t.definition = {
			QStringLiteral("set_camera_enabled"),
			QStringLiteral("Enable or disable the local camera (requires user confirmation)."),
			makeObjectSchema(camProps, {QStringLiteral("enabled")}),
			AgentToolRisk::RequiresConfirmation,
			AgentToolScope::ActiveMeeting,
		};
		t.prepareAction = [](const QJsonObject &args, const AgentToolViewContext &,
				PreparedPendingAction &out, QString &outError) {
			const auto val = args.value(QStringLiteral("enabled"));
			if (!val.isBool()) {
				outError = QStringLiteral("Missing boolean parameter 'enabled'.");
				return false;
			}
			const bool enabled = val.toBool();
			out.title = enabled
				? QCoreApplication::translate("MeetingUI", "Turn On Camera")
				: QCoreApplication::translate("MeetingUI", "Turn Off Camera");
			out.summary = enabled
				? QCoreApplication::translate("MeetingUI", "Start sending local camera video to the meeting.")
				: QCoreApplication::translate("MeetingUI", "Stop sending local camera video to the meeting.");
			out.normalizedArgs = args;
			return true;
		};
		t.executeConfirmed = [](const QJsonObject &args, const AgentToolExecuteContext &ctx) {
			const QString name = QStringLiteral("set_camera_enabled");
			if (!ctx.meeting || ctx.meeting->state() == MeetingState::Idle) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Meeting session is no longer active.")));
			}
			const auto val = args.value(QStringLiteral("enabled"));
			if (!val.isBool()) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Missing boolean parameter 'enabled'.")));
			}
			ctx.meeting->setLocalVideoEnabled(val.toBool());
			QJsonObject payload;
			payload.insert(QStringLiteral("cameraEnabled"), ctx.meeting->isLocalVideoEnabled());
			return makeSimpleOutcome(
				makeOkResult(name, payload),
				QCoreApplication::translate("MeetingUI", "Executed."));
		};
		registerTool(std::move(t));
	}

	// send_meeting_chat
	{
		RegisteredTool t;
		QJsonObject sendChatProps;
		sendChatProps.insert(QStringLiteral("message"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Text message to post to the public meeting chat (1-1000 characters).")));
		t.definition = {
			QStringLiteral("send_meeting_chat"),
			QStringLiteral("Send a public text message to the meeting chat (requires user confirmation)."),
			makeObjectSchema(sendChatProps, {QStringLiteral("message")}),
			AgentToolRisk::RequiresConfirmation,
			AgentToolScope::ActiveMeeting,
		};
		t.prepareAction = [](const QJsonObject &args, const AgentToolViewContext &,
				PreparedPendingAction &out, QString &outError) {
			const QString msg = args.value(QStringLiteral("message")).toString().trimmed();
			if (msg.isEmpty()) {
				outError = QStringLiteral("Message content must not be empty.");
				return false;
			}
			if (msg.size() > kMaxOutboundChatLength) {
				outError = QStringLiteral("Message exceeds maximum length (1000 characters).");
				return false;
			}
			out.title = QCoreApplication::translate("MeetingUI", "Send Meeting Chat");
			out.summary = QCoreApplication::translate("MeetingUI", "Post to public chat: \"%1\"")
				.arg(msg.left(160));
			out.normalizedArgs = args;
			out.normalizedArgs.insert(QStringLiteral("message"), msg);
			return true;
		};
		t.executeConfirmed = [](const QJsonObject &args, const AgentToolExecuteContext &ctx) {
			const QString name = QStringLiteral("send_meeting_chat");
			if (!ctx.meeting || ctx.meeting->state() == MeetingState::Idle) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Meeting session is no longer active.")));
			}
			const QString msg = args.value(QStringLiteral("message")).toString().trimmed();
			if (msg.isEmpty() || msg.size() > kMaxOutboundChatLength) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Invalid chat message.")));
			}
			ctx.meeting->sendChatMessage(msg);
			QJsonObject payload;
			payload.insert(QStringLiteral("sent"), true);
			payload.insert(QStringLiteral("length"), msg.size());
			return makeSimpleOutcome(
				makeOkResult(name, payload),
				QCoreApplication::translate("MeetingUI", "Executed."));
		};
		registerTool(std::move(t));
	}

	// control_collaboration_state
	{
		RegisteredTool t;
		QJsonObject props;
		props.insert(QStringLiteral("action"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Collaboration action: 'stop_screen_share', 'set_screen_share_quality', or 'activate_whiteboard'.")));
		props.insert(QStringLiteral("resolution"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("For 'set_screen_share_quality': 'auto', '720p', '1080p', '1440p', 'native', or '2160p'.")));
		props.insert(QStringLiteral("fps"), makeProp(
			QStringLiteral("integer"),
			QStringLiteral("For 'set_screen_share_quality': frame rate 15, 20, or 30.")));
		t.definition = {
			QStringLiteral("control_collaboration_state"),
			QStringLiteral("Control in-meeting screen sharing or whiteboard collaboration (stop screen share, adjust screen share quality, or open whiteboard). Requires user confirmation."),
			makeObjectSchema(props, {QStringLiteral("action")}),
			AgentToolRisk::RequiresConfirmation,
			AgentToolScope::ActiveMeeting,
		};
		t.prepareAction = [](const QJsonObject &args, const AgentToolViewContext &,
				PreparedPendingAction &out, QString &outError) {
			const QString action = args.value(QStringLiteral("action")).toString().trimmed().toLower();
			if (action == QStringLiteral("stop_screen_share")) {
				out.title = QCoreApplication::translate("MeetingUI", "Stop Screen Share");
				out.summary = QCoreApplication::translate("MeetingUI", "Stop sharing your screen with the meeting.");
				out.normalizedArgs.insert(QStringLiteral("action"), action);
				return true;
			}
			if (action == QStringLiteral("activate_whiteboard")) {
				out.title = QCoreApplication::translate("MeetingUI", "Open Whiteboard");
				out.summary = QCoreApplication::translate("MeetingUI", "Activate the shared meeting whiteboard.");
				out.normalizedArgs.insert(QStringLiteral("action"), action);
				return true;
			}
			if (action == QStringLiteral("set_screen_share_quality")) {
				const QString resStr = args.value(QStringLiteral("resolution")).toString(QStringLiteral("auto"));
				const auto resOpt = parseScreenShareResolution(resStr);
				if (!resOpt.has_value()) {
					outError = QStringLiteral("Invalid resolution '%1'. Use auto, 720p, 1080p, 1440p, native, or 2160p.")
						.arg(resStr);
					return false;
				}
				const int fps = args.contains(QStringLiteral("fps")) ? args.value(QStringLiteral("fps")).toInt(20) : 20;
				if (fps != 15 && fps != 20 && fps != 30) {
					outError = QStringLiteral("fps must be 15, 20, or 30.");
					return false;
				}
				out.title = QCoreApplication::translate("MeetingUI", "Adjust Screen Share Quality");
				out.summary = QCoreApplication::translate("MeetingUI", "Set screen share quality to %1 at %2 fps.")
					.arg(screenShareResolutionToString(static_cast<int>(*resOpt)))
					.arg(fps);
				out.normalizedArgs.insert(QStringLiteral("action"), action);
				out.normalizedArgs.insert(QStringLiteral("resolution"),
					screenShareResolutionToString(static_cast<int>(*resOpt)));
				out.normalizedArgs.insert(QStringLiteral("fps"), fps);
				return true;
			}
			outError = QStringLiteral("Unsupported collaboration action '%1'.").arg(action);
			return false;
		};
		t.executeConfirmed = [](const QJsonObject &args, const AgentToolExecuteContext &ctx) {
			const QString name = QStringLiteral("control_collaboration_state");
			if (!ctx.meeting || ctx.meeting->state() == MeetingState::Idle) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Meeting session is no longer active.")));
			}
			const QString action = args.value(QStringLiteral("action")).toString();
			QJsonObject payload;
			payload.insert(QStringLiteral("action"), action);
			if (action == QStringLiteral("stop_screen_share")) {
				ctx.meeting->stopScreenShare();
			} else if (action == QStringLiteral("activate_whiteboard")) {
				ctx.meeting->activateWhiteboard();
			} else if (action == QStringLiteral("set_screen_share_quality")) {
				const auto resOpt = parseScreenShareResolution(args.value(QStringLiteral("resolution")).toString());
				if (!resOpt.has_value()) {
					return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Invalid screen share resolution.")));
				}
				const int fps = args.value(QStringLiteral("fps")).toInt(20);
				livekit::ScreenShareQuality q;
				q.resolution = *resOpt;
				q.fps = fps;
				ctx.meeting->setScreenShareQuality(q);
				payload.insert(QStringLiteral("resolution"), screenShareResolutionToString(static_cast<int>(*resOpt)));
				payload.insert(QStringLiteral("fps"), fps);
			} else {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Unsupported collaboration action.")));
			}
			return makeSimpleOutcome(
				makeOkResult(name, payload),
				QCoreApplication::translate("MeetingUI", "Executed."));
		};
		registerTool(std::move(t));
	}

	// leave_meeting
	{
		RegisteredTool t;
		QJsonObject leaveProps;
		leaveProps.insert(QStringLiteral("end_for_all"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Optional. True to end the meeting for everyone (host only), false to leave only.")));
		t.definition = {
			QStringLiteral("leave_meeting"),
			QStringLiteral("Leave or end the current meeting (requires user confirmation)."),
			makeObjectSchema(leaveProps),
			AgentToolRisk::RequiresConfirmation,
			AgentToolScope::ActiveMeeting,
			AgentConcurrencyGroup::MeetingTransition,
		};
		t.prepareAction = [](const QJsonObject &args, const AgentToolViewContext &,
				PreparedPendingAction &out, QString &) {
			const bool endForAll = args.value(QStringLiteral("end_for_all")).toBool(false);
			out.title = endForAll
				? QCoreApplication::translate("MeetingUI", "End Meeting for Everyone")
				: QCoreApplication::translate("MeetingUI", "Leave Meeting");
			out.summary = endForAll
				? QCoreApplication::translate("MeetingUI", "End the active meeting for all participants.")
				: QCoreApplication::translate("MeetingUI", "Disconnect and leave the active meeting.");
			out.normalizedArgs = args;
			return true;
		};
		t.executeConfirmed = [](const QJsonObject &args, const AgentToolExecuteContext &ctx) {
			const QString name = QStringLiteral("leave_meeting");
			if (!ctx.meeting || ctx.meeting->state() == MeetingState::Idle) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Meeting session is no longer active.")));
			}
			const bool endForAll = args.value(QStringLiteral("end_for_all")).toBool(false);
			ctx.meeting->leaveMeetingAsync(endForAll);
			QJsonObject payload;
			payload.insert(QStringLiteral("leaving"), true);
			payload.insert(QStringLiteral("endForAll"), endForAll);
			return makeSimpleOutcome(
				makeOkResult(name, payload),
				QCoreApplication::translate("MeetingUI", "Executed."));
		};
		registerTool(std::move(t));
	}
}

void MeetingToolRegistry::registerHostModerationTools() {
	RegisteredTool t;
	QJsonObject props;
	props.insert(QStringLiteral("action"), makeProp(
		QStringLiteral("string"),
		QStringLiteral("Host moderation action: 'mute_all', 'set_participant_mic', 'set_participant_camera', 'transfer_host', 'kick_participant', or 'whiteboard_access'.")));
	props.insert(QStringLiteral("target_user_id"), makeProp(
		QStringLiteral("string"),
		QStringLiteral("Target participant userId (from list_participants) for participant-targeted actions.")));
	props.insert(QStringLiteral("enabled"), makeProp(
		QStringLiteral("boolean"),
		QStringLiteral("For 'set_participant_mic'/'set_participant_camera': true to request unmute/enable, false to mute/disable.")));
	props.insert(QStringLiteral("muted"), makeProp(
		QStringLiteral("boolean"),
		QStringLiteral("For 'mute_all': true to mute all participants (default true), false to request unmute.")));
	props.insert(QStringLiteral("allow_self_unmute"), makeProp(
		QStringLiteral("boolean"),
		QStringLiteral("For 'mute_all': whether participants may unmute themselves (default true).")));
	props.insert(QStringLiteral("reason"), makeProp(
		QStringLiteral("string"),
		QStringLiteral("Optional reason for 'kick_participant'.")));
	props.insert(QStringLiteral("whiteboard_locked"), makeProp(
		QStringLiteral("boolean"),
		QStringLiteral("For 'whiteboard_access': true to lock whiteboard editing, false to unlock.")));
	props.insert(QStringLiteral("whiteboard_writers_open"), makeProp(
		QStringLiteral("boolean"),
		QStringLiteral("For 'whiteboard_access': true to allow participants to draw, false for host-only.")));

	t.definition = {
		QStringLiteral("moderate_participants"),
		QStringLiteral("Host-only meeting moderation: mute all, request participant mic/camera state, transfer host, remove a participant, or manage whiteboard permissions (requires user confirmation)."),
		makeObjectSchema(props, {QStringLiteral("action")}),
		AgentToolRisk::RequiresConfirmation,
		AgentToolScope::MeetingHost,
	};
	t.isAvailable = [](const AgentToolViewContext &ctx) {
		return ctx.meeting && ctx.meeting->state() == MeetingState::InMeeting && ctx.meeting->isHost();
	};
	t.prepareAction = [](const QJsonObject &args, const AgentToolViewContext &ctx,
			PreparedPendingAction &out, QString &outError) {
		if (!ctx.meeting || ctx.meeting->state() != MeetingState::InMeeting || !ctx.meeting->isHost()) {
			outError = QStringLiteral("Host permissions in an active meeting are required.");
			return false;
		}
		const QString action = args.value(QStringLiteral("action")).toString().trimmed().toLower();
		out.normalizedArgs.insert(QStringLiteral("action"), action);

		if (action == QStringLiteral("mute_all")) {
			const bool muted = args.contains(QStringLiteral("muted"))
				? args.value(QStringLiteral("muted")).toBool(true)
				: true;
			const bool allowSelf = args.value(QStringLiteral("allow_self_unmute")).toBool(true);
			out.title = muted
				? QCoreApplication::translate("MeetingUI", "Mute All Participants")
				: QCoreApplication::translate("MeetingUI", "Unmute All Participants");
			out.summary = muted
				? QCoreApplication::translate("MeetingUI", "Mute all remote participants in the meeting.")
				: QCoreApplication::translate("MeetingUI", "Request all remote participants to unmute.");
			out.normalizedArgs.insert(QStringLiteral("muted"), muted);
			out.normalizedArgs.insert(QStringLiteral("allow_self_unmute"), allowSelf);
			return true;
		}

		if (action == QStringLiteral("whiteboard_access")) {
			const bool hasLocked = args.contains(QStringLiteral("whiteboard_locked")) &&
				args.value(QStringLiteral("whiteboard_locked")).isBool();
			const bool hasOpen = args.contains(QStringLiteral("whiteboard_writers_open")) &&
				args.value(QStringLiteral("whiteboard_writers_open")).isBool();
			if (!hasLocked && !hasOpen) {
				outError = QStringLiteral("Specify 'whiteboard_locked' or 'whiteboard_writers_open'.");
				return false;
			}
			QStringList parts;
			if (hasLocked) {
				const bool locked = args.value(QStringLiteral("whiteboard_locked")).toBool();
				out.normalizedArgs.insert(QStringLiteral("whiteboard_locked"), locked);
				parts.push_back(locked
					? QCoreApplication::translate("MeetingUI", "Lock whiteboard")
					: QCoreApplication::translate("MeetingUI", "Unlock whiteboard"));
			}
			if (hasOpen) {
				const bool open = args.value(QStringLiteral("whiteboard_writers_open")).toBool();
				out.normalizedArgs.insert(QStringLiteral("whiteboard_writers_open"), open);
				parts.push_back(open
					? QCoreApplication::translate("MeetingUI", "Allow participants to write")
					: QCoreApplication::translate("MeetingUI", "Restrict whiteboard to host"));
			}
			out.title = QCoreApplication::translate("MeetingUI", "Update Whiteboard Permissions");
			out.summary = parts.join(QStringLiteral("\n"));
			return true;
		}

		const QString targetId = args.value(QStringLiteral("target_user_id")).toString().trimmed();
		if (targetId.isEmpty()) {
			outError = QStringLiteral("Missing required parameter 'target_user_id'.");
			return false;
		}
		const QString displayName = resolveParticipantDisplayName(ctx.meeting, targetId);
		out.normalizedArgs.insert(QStringLiteral("target_user_id"), targetId);

		if (action == QStringLiteral("set_participant_mic")) {
			if (!args.value(QStringLiteral("enabled")).isBool()) {
				outError = QStringLiteral("Missing boolean parameter 'enabled'.");
				return false;
			}
			const bool en = args.value(QStringLiteral("enabled")).toBool();
			out.normalizedArgs.insert(QStringLiteral("enabled"), en);
			out.title = en
				? QCoreApplication::translate("MeetingUI", "Request Participant Unmute")
				: QCoreApplication::translate("MeetingUI", "Mute Participant");
			out.summary = en
				? QCoreApplication::translate("MeetingUI", "Ask \"%1\" to turn on their microphone.").arg(displayName)
				: QCoreApplication::translate("MeetingUI", "Mute \"%1\"'s microphone.").arg(displayName);
			return true;
		}

		if (action == QStringLiteral("set_participant_camera")) {
			if (!args.value(QStringLiteral("enabled")).isBool()) {
				outError = QStringLiteral("Missing boolean parameter 'enabled'.");
				return false;
			}
			const bool en = args.value(QStringLiteral("enabled")).toBool();
			out.normalizedArgs.insert(QStringLiteral("enabled"), en);
			out.title = en
				? QCoreApplication::translate("MeetingUI", "Request Participant Camera")
				: QCoreApplication::translate("MeetingUI", "Turn Off Participant Camera");
			out.summary = en
				? QCoreApplication::translate("MeetingUI", "Ask \"%1\" to turn on their camera.").arg(displayName)
				: QCoreApplication::translate("MeetingUI", "Turn off \"%1\"'s camera.").arg(displayName);
			return true;
		}

		if (action == QStringLiteral("transfer_host")) {
			if (targetId == ctx.meeting->meetingDetail().effectiveHostUserId()) {
				outError = QStringLiteral("Selected user is already the host.");
				return false;
			}
			out.title = QCoreApplication::translate("MeetingUI", "Transfer Host Role");
			out.summary = QCoreApplication::translate("MeetingUI", "Transfer meeting host permissions to \"%1\".").arg(displayName);
			return true;
		}

		if (action == QStringLiteral("kick_participant")) {
			if (targetId == ctx.meeting->meetingDetail().effectiveHostUserId()) {
				outError = QStringLiteral("Cannot remove the meeting host.");
				return false;
			}
			const QString reason = args.value(QStringLiteral("reason")).toString().trimmed();
			if (!reason.isEmpty()) {
				out.normalizedArgs.insert(QStringLiteral("reason"), reason.left(128));
			}
			out.title = QCoreApplication::translate("MeetingUI", "Remove Participant");
			out.summary = QCoreApplication::translate("MeetingUI", "Remove \"%1\" from the current meeting.").arg(displayName);
			return true;
		}

		outError = QStringLiteral("Unsupported moderation action '%1'.").arg(action);
		return false;
	};
	t.executeConfirmed = [](const QJsonObject &args, const AgentToolExecuteContext &ctx) {
		const QString name = QStringLiteral("moderate_participants");
		if (!ctx.meeting || ctx.meeting->state() != MeetingState::InMeeting || !ctx.meeting->isHost()) {
			return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Host permissions in an active meeting are required.")));
		}
		const QString action = args.value(QStringLiteral("action")).toString();
		const QString targetId = args.value(QStringLiteral("target_user_id")).toString();
		QJsonObject payload;
		payload.insert(QStringLiteral("action"), action);

		if (action == QStringLiteral("mute_all")) {
			const bool muted = args.value(QStringLiteral("muted")).toBool(true);
			const bool allowSelf = args.value(QStringLiteral("allow_self_unmute")).toBool(true);
			ctx.meeting->muteAllParticipants(muted, allowSelf);
			payload.insert(QStringLiteral("muted"), muted);
		} else if (action == QStringLiteral("whiteboard_access")) {
			if (args.contains(QStringLiteral("whiteboard_locked"))) {
				ctx.meeting->setWhiteboardLocked(args.value(QStringLiteral("whiteboard_locked")).toBool());
			}
			if (args.contains(QStringLiteral("whiteboard_writers_open"))) {
				ctx.meeting->setWhiteboardWritersOpen(args.value(QStringLiteral("whiteboard_writers_open")).toBool());
			}
		} else if (action == QStringLiteral("set_participant_mic")) {
			const bool en = args.value(QStringLiteral("enabled")).toBool();
			ctx.meeting->requestParticipantMicrophone(targetId, en);
			payload.insert(QStringLiteral("targetUserId"), targetId);
			payload.insert(QStringLiteral("enabled"), en);
		} else if (action == QStringLiteral("set_participant_camera")) {
			const bool en = args.value(QStringLiteral("enabled")).toBool();
			ctx.meeting->requestParticipantCamera(targetId, en);
			payload.insert(QStringLiteral("targetUserId"), targetId);
			payload.insert(QStringLiteral("enabled"), en);
		} else if (action == QStringLiteral("transfer_host")) {
			ctx.meeting->transferHost(targetId);
			payload.insert(QStringLiteral("newHostUserId"), targetId);
		} else if (action == QStringLiteral("kick_participant")) {
			const QString reason = args.value(QStringLiteral("reason")).toString();
			ctx.meeting->kickParticipant(targetId, reason);
			payload.insert(QStringLiteral("removedUserId"), targetId);
		} else {
			return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Unsupported moderation action.")));
		}

		return makeSimpleOutcome(
			makeOkResult(name, payload),
			QCoreApplication::translate("MeetingUI", "Executed."));
	};
	registerTool(std::move(t));
}

} // namespace OpenMeeting
