#include "src/agent/meeting_tool_registry.h"
#include "src/agent/detail/meeting_tool_helpers.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonParseError>

namespace OpenMeeting {
using namespace tool_helpers;

MeetingToolRegistry::MeetingToolRegistry() {
	registerMeetingControlTools();
	registerHostModerationTools();
	registerCatalogTools();
	registerContactTools();
	registerClientSettingsTools();
}

void MeetingToolRegistry::registerTool(RegisteredTool tool) {
	const QString &name = tool.definition.name;
	if (name.isEmpty()) {
		return;
	}
	if (!_registry.contains(name)) {
		_toolOrder.push_back(name);
	}
	_registry.insert(name, std::move(tool));
}

QVector<AgentToolDefinition> MeetingToolRegistry::toolDefinitions(
		const AgentToolViewContext &ctx) const {
	QVector<AgentToolDefinition> result;
	for (const QString &name : _toolOrder) {
		const auto it = _registry.constFind(name);
		if (it == _registry.constEnd() || !it->exposedInToolDefinitions) {
			continue;
		}
		const auto scope = it->definition.scope;
		bool scopeEnabled = false;
		switch (scope) {
		case AgentToolScope::ClientGlobal:
			scopeEnabled = ctx.canUseClientPrefTools;
			break;
		case AgentToolScope::AccountCatalog:
			scopeEnabled = ctx.canUseCatalogTools;
			break;
		case AgentToolScope::AccountContacts:
			scopeEnabled = ctx.canUseContactTools;
			break;
		case AgentToolScope::ActiveMeeting:
			scopeEnabled = ctx.canShareMeetingData;
			break;
		case AgentToolScope::MeetingHost:
			scopeEnabled = ctx.canShareMeetingData;
			break;
		}
		if (!scopeEnabled) {
			continue;
		}
		if (it->isAvailable && !it->isAvailable(ctx)) {
			continue;
		}
		result.push_back(it->definition);
	}
	return result;
}

QVector<AgentToolDefinition> MeetingToolRegistry::toolDefinitions(
		bool includeMeetingTools,
		bool includeCatalogTools,
		bool includeClientPreferenceTools) const {
	QVector<AgentToolDefinition> result;
	for (const QString &name : _toolOrder) {
		const auto it = _registry.constFind(name);
		if (it == _registry.constEnd() || !it->exposedInToolDefinitions) {
			continue;
		}
		const auto scope = it->definition.scope;
		if ((scope == AgentToolScope::AccountCatalog && includeCatalogTools) ||
			(scope == AgentToolScope::ClientGlobal && includeClientPreferenceTools) ||
			((scope == AgentToolScope::ActiveMeeting || scope == AgentToolScope::MeetingHost) && includeMeetingTools)) {
			result.push_back(it->definition);
		}
	}
	return result;
}

std::optional<AgentToolDefinition> MeetingToolRegistry::findTool(const QString &name) const {
	const auto it = _registry.constFind(name);
	if (it == _registry.constEnd()) {
		return std::nullopt;
	}
	return it->definition;
}

bool MeetingToolRegistry::isToolAvailable(const QString &name, const AgentToolViewContext &ctx) const {
	const auto it = _registry.constFind(name);
	if (it == _registry.constEnd()) {
		return false;
	}
	if (it->isAvailable) {
		return it->isAvailable(ctx);
	}
	return true;
}

bool MeetingToolRegistry::isReadOnlyTool(const QString &name) const {
	const auto def = findTool(name);
	return def.has_value() && def->risk == AgentToolRisk::ReadOnly;
}

bool MeetingToolRegistry::isCatalogTool(const QString &name) {
	return name == QStringLiteral("list_scheduled_meetings") ||
		name == QStringLiteral("get_scheduled_meeting_detail") ||
		name == QStringLiteral("schedule_meeting") ||
		name == QStringLiteral("update_scheduled_meeting") ||
		name == QStringLiteral("cancel_scheduled_meeting");
}

bool MeetingToolRegistry::isContactTool(const QString &name) {
	return name == QStringLiteral("search_contacts") ||
		name == QStringLiteral("list_contact_groups") ||
		name == QStringLiteral("list_org_departments") ||
		name == QStringLiteral("list_contact_requests") ||
		name == QStringLiteral("get_my_profile") ||
		name == QStringLiteral("manage_contact_group") ||
		name == QStringLiteral("update_contact_relation") ||
		name == QStringLiteral("manage_contact_request") ||
		name == QStringLiteral("update_my_profile") ||
		name == QStringLiteral("call_contact");
}

bool MeetingToolRegistry::isClientPreferenceTool(const QString &name) {
	return name == QStringLiteral("get_join_media_preferences") ||
		name == QStringLiteral("set_join_media_preferences") ||
		name == QStringLiteral("get_client_preferences") ||
		name == QStringLiteral("update_client_preferences");
}

bool MeetingToolRegistry::parseToolArguments(
		const QString &argumentsJson,
		QJsonObject &outArgs,
		QString &outError) {
	outArgs = QJsonObject{};
	outError.clear();
	const QString trimmed = argumentsJson.trimmed();
	if (trimmed.isEmpty()) {
		return true;
	}
	QJsonParseError err{};
	const QJsonDocument doc = QJsonDocument::fromJson(trimmed.toUtf8(), &err);
	if (err.error != QJsonParseError::NoError || !doc.isObject()) {
		outError = QStringLiteral("Tool arguments must be a valid JSON object.");
		return false;
	}
	outArgs = doc.object();
	return true;
}

AgentToolResult MeetingToolRegistry::executeReadOnly(
		const QString &name,
		const QJsonObject &args,
		const AgentToolViewContext &ctx) const {
	const auto it = _registry.constFind(name);
	if (it == _registry.constEnd() ||
		it->definition.risk != AgentToolRisk::ReadOnly ||
		!it->executeReadOnly) {
		return makeErrorResult(name, QStringLiteral("Unknown read-only tool."));
	}
	return it->executeReadOnly(args, ctx);
}

AgentToolResult MeetingToolRegistry::executeReadOnly(
		const QString &name,
		const QJsonObject &args,
		const AgentMeetingContext &overview,
		MeetingCoordinator *meeting,
		const QVector<AgentMeetingChatEntry> &recentChat,
		const QVariantMap &telemetrySnapshot,
		MeetingCatalogController *catalog,
		const SessionManager *sessionManager) const {
	AgentToolViewContext ctx;
	ctx.meetingOverview = overview;
	ctx.meeting = meeting;
	ctx.catalog = catalog;
	ctx.sessionManager = sessionManager;
	ctx.recentChat = &recentChat;
	ctx.telemetrySnapshot = &telemetrySnapshot;
	ctx.canShareMeetingData = overview.inMeeting;
	ctx.canUseCatalogTools = (catalog != nullptr);
	ctx.canUseClientPrefTools = (sessionManager != nullptr);
	return executeReadOnly(name, args, ctx);
}

bool MeetingToolRegistry::preparePendingAction(
		const QString &name,
		const QJsonObject &rawArgs,
		const AgentToolViewContext &ctx,
		PreparedPendingAction &outPrepared,
		QString &outError) const {
	outPrepared = PreparedPendingAction{};
	outError.clear();

	const auto it = _registry.constFind(name);
	if (it == _registry.constEnd() ||
		it->definition.risk != AgentToolRisk::RequiresConfirmation ||
		!it->prepareAction) {
		outError = QStringLiteral("Unsupported meeting action tool.");
		return false;
	}
	return it->prepareAction(rawArgs, ctx, outPrepared, outError);
}

bool MeetingToolRegistry::describePendingAction(
		const QString &name,
		const QJsonObject &args,
		QString &outTitle,
		QString &outSummary,
		QString &outError,
		const MeetingCatalogController *catalog,
		const SessionManager *sessionManager,
		MeetingCoordinator *meeting) const {
	AgentToolViewContext ctx;
	ctx.catalog = const_cast<MeetingCatalogController *>(catalog);
	ctx.sessionManager = sessionManager;
	ctx.meeting = meeting;
	PreparedPendingAction prepared;
	if (!preparePendingAction(name, args, ctx, prepared, outError)) {
		outTitle.clear();
		outSummary.clear();
		return false;
	}
	outTitle = prepared.title;
	outSummary = prepared.summary;
	return true;
}

ToolExecutionOutcome MeetingToolRegistry::executeConfirmedActionWithOutcome(
		const QString &name,
		const QJsonObject &normalizedArgs,
		const AgentToolExecuteContext &ctx) const {
	const auto it = _registry.constFind(name);
	if (it == _registry.constEnd() ||
		it->definition.risk != AgentToolRisk::RequiresConfirmation ||
		!it->executeConfirmed) {
		return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Unknown action tool.")));
	}
	return it->executeConfirmed(normalizedArgs, ctx);
}

AgentToolResult MeetingToolRegistry::executeConfirmedAction(
		const QString &name,
		const QJsonObject &args,
		MeetingCoordinator *meeting,
		MeetingCatalogController *catalog,
		SessionManager *sessionManager) const {
	AgentToolExecuteContext ctx;
	ctx.meeting = meeting;
	ctx.catalog = catalog;
	ctx.sessionManager = sessionManager;
	return executeConfirmedActionWithOutcome(name, args, ctx).toolResult;
}

QString MeetingToolRegistry::formatJoinMediaPreferencesCompletionSummary(const QJsonObject &args) {
	QStringList parts;
	if (args.contains(QStringLiteral("enable_microphone_on_join")) &&
		args.value(QStringLiteral("enable_microphone_on_join")).isBool()) {
		const bool en = args.value(QStringLiteral("enable_microphone_on_join")).toBool();
		parts.push_back(en
			? QCoreApplication::translate("MeetingUI", "\"Enable microphone when joining a meeting\" is now turned ON")
			: QCoreApplication::translate("MeetingUI", "\"Enable microphone when joining a meeting\" is now turned OFF (muted by default on join)"));
	}
	if (args.contains(QStringLiteral("enable_camera_on_join")) &&
		args.value(QStringLiteral("enable_camera_on_join")).isBool()) {
		const bool en = args.value(QStringLiteral("enable_camera_on_join")).toBool();
		parts.push_back(en
			? QCoreApplication::translate("MeetingUI", "\"Enable camera when joining a meeting\" is now turned ON")
			: QCoreApplication::translate("MeetingUI", "\"Enable camera when joining a meeting\" is now turned OFF"));
	}
	if (args.contains(QStringLiteral("enable_speaker_on_join")) &&
		args.value(QStringLiteral("enable_speaker_on_join")).isBool()) {
		const bool en = args.value(QStringLiteral("enable_speaker_on_join")).toBool();
		parts.push_back(en
			? QCoreApplication::translate("MeetingUI", "\"Use computer audio when joining a meeting\" is now turned ON")
			: QCoreApplication::translate("MeetingUI", "\"Use computer audio when joining a meeting\" is now turned OFF"));
	}
	return QCoreApplication::translate("MeetingUI", "Join media settings updated: %1.")
		.arg(parts.join(QStringLiteral("; ")));
}

ToolInvocationPresentation MeetingToolRegistry::describeToolInvocation(
	const QString &name,
	const QJsonObject &args,
	bool succeeded) {
	struct StaticEntry {
		const char *toolName;
		const char *invocation;
		const char *pastSucceeded;
		const char *pastFailed;
	};
	static const StaticEntry kStaticEntries[] = {
		{ "get_meeting_overview",
		  QT_TRANSLATE_NOOP("MeetingUI", "Reading meeting overview..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Read meeting overview"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to read meeting overview") },
		{ "list_participants",
		  QT_TRANSLATE_NOOP("MeetingUI", "Checking participant list..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Checked participant list"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to check participants") },
		{ "get_recent_chat",
		  QT_TRANSLATE_NOOP("MeetingUI", "Reading recent meeting chat..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Read recent meeting chat"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to read recent meeting chat") },
		{ "get_recent_transcripts",
		  QT_TRANSLATE_NOOP("MeetingUI", "Reading meeting voice transcripts..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Read meeting voice transcripts"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to read meeting transcripts") },
		{ "get_telemetry_snapshot",
		  QT_TRANSLATE_NOOP("MeetingUI", "Reading audio/video telemetry..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Read audio/video telemetry"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to read telemetry") },
		{ "control_collaboration_state",
		  QT_TRANSLATE_NOOP("MeetingUI", "Updating meeting state..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Updated meeting state"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to update meeting state") },
		{ "leave_meeting",
		  QT_TRANSLATE_NOOP("MeetingUI", "Leaving the meeting..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Left the meeting"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to leave the meeting") },
		{ "list_scheduled_meetings",
		  QT_TRANSLATE_NOOP("MeetingUI", "Listing scheduled meetings..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Listed scheduled meetings"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to list scheduled meetings") },
		{ "get_scheduled_meeting_detail",
		  QT_TRANSLATE_NOOP("MeetingUI", "Loading scheduled meeting details..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Loaded scheduled meeting details"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to load meeting details") },
		{ "get_client_preferences",
		  QT_TRANSLATE_NOOP("MeetingUI", "Reading client preferences..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Read client preferences"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to read client preferences") },
		{ "get_join_media_preferences",
		  QT_TRANSLATE_NOOP("MeetingUI", "Reading join media preferences..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Read join media preferences"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to read join media preferences") },
		{ "list_media_devices",
		  QT_TRANSLATE_NOOP("MeetingUI", "Listing audio/video devices..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Listed audio/video devices"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to list media devices") },
		{ "get_app_update_status",
		  QT_TRANSLATE_NOOP("MeetingUI", "Checking app update status..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Checked app update status"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to check update status") },
		{ "check_for_updates",
		  QT_TRANSLATE_NOOP("MeetingUI", "Checking for app updates..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Checked for app updates"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to check for updates") },
		{ "control_app_update",
		  QT_TRANSLATE_NOOP("MeetingUI", "Applying app update action..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Completed app update action"),
		  QT_TRANSLATE_NOOP("MeetingUI", "App update action failed") },
		{ "update_client_preferences",
		  QT_TRANSLATE_NOOP("MeetingUI", "Updating client preferences..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Updated client preferences"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to update client preferences") },
		{ "set_join_media_preferences",
		  QT_TRANSLATE_NOOP("MeetingUI", "Updating join media preferences..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Updated join media preferences"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to update join media preferences") },
		{ "search_contacts",
		  QT_TRANSLATE_NOOP("MeetingUI", "Searching contacts..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Searched contacts"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to search contacts") },
		{ "list_contact_groups",
		  QT_TRANSLATE_NOOP("MeetingUI", "Listing contact groups..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Listed contact groups"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to list contact groups") },
		{ "list_org_departments",
		  QT_TRANSLATE_NOOP("MeetingUI", "Reading organization directory..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Read organization directory"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to read organization directory") },
		{ "list_contact_requests",
		  QT_TRANSLATE_NOOP("MeetingUI", "Checking contact requests..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Checked contact requests"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to check contact requests") },
		{ "get_my_profile",
		  QT_TRANSLATE_NOOP("MeetingUI", "Reading personal profile..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Read personal profile"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to read personal profile") },
		{ "update_my_profile",
		  QT_TRANSLATE_NOOP("MeetingUI", "Updating personal profile..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Updated personal profile"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to update personal profile") },
	};

	const auto makePres = [succeeded](const QString &inv, const QString &ok, const QString &fail) {
		return ToolInvocationPresentation{ inv, succeeded ? ok : fail };
	};

	if (name == QLatin1String("set_microphone_muted")) {
		const bool muted = args.value(QStringLiteral("muted")).toBool(true);
		return muted
			? makePres(
				QCoreApplication::translate("MeetingUI", "Muting microphone..."),
				QCoreApplication::translate("MeetingUI", "Muted microphone"),
				QCoreApplication::translate("MeetingUI", "Failed to mute microphone"))
			: makePres(
				QCoreApplication::translate("MeetingUI", "Unmuting microphone..."),
				QCoreApplication::translate("MeetingUI", "Unmuted microphone"),
				QCoreApplication::translate("MeetingUI", "Failed to unmute microphone"));
	}
	if (name == QLatin1String("set_camera_enabled")) {
		const bool enabled = args.value(QStringLiteral("enabled")).toBool(false);
		return enabled
			? makePres(
				QCoreApplication::translate("MeetingUI", "Turning on camera..."),
				QCoreApplication::translate("MeetingUI", "Turned on camera"),
				QCoreApplication::translate("MeetingUI", "Failed to turn on camera"))
			: makePres(
				QCoreApplication::translate("MeetingUI", "Turning off camera..."),
				QCoreApplication::translate("MeetingUI", "Turned off camera"),
				QCoreApplication::translate("MeetingUI", "Failed to turn off camera"));
	}
	if (name == QLatin1String("send_meeting_chat")) {
		return makePres(
			QCoreApplication::translate("MeetingUI", "Sending meeting chat message..."),
			QCoreApplication::translate("MeetingUI", "Sent meeting chat message"),
			QCoreApplication::translate("MeetingUI", "Failed to send meeting chat"));
	}
	if (name == QLatin1String("moderate_participants")) {
		return makePres(
			QCoreApplication::translate("MeetingUI", "Moderating participant..."),
			QCoreApplication::translate("MeetingUI", "Moderated participant"),
			QCoreApplication::translate("MeetingUI", "Failed to moderate participant"));
	}
	if (name == QLatin1String("schedule_meeting")) {
		const QString title = args.value(QStringLiteral("title")).toString().trimmed();
		return title.isEmpty()
			? makePres(
				QCoreApplication::translate("MeetingUI", "Scheduling meeting..."),
				QCoreApplication::translate("MeetingUI", "Scheduled meeting"),
				QCoreApplication::translate("MeetingUI", "Failed to schedule meeting"))
			: makePres(
				QCoreApplication::translate("MeetingUI", "Scheduling meeting \"%1\"...").arg(title),
				QCoreApplication::translate("MeetingUI", "Scheduled meeting \"%1\"").arg(title),
				QCoreApplication::translate("MeetingUI", "Failed to schedule \"%1\"").arg(title));
	}
	if (name == QLatin1String("update_scheduled_meeting")) {
		return makePres(
			QCoreApplication::translate("MeetingUI", "Updating scheduled meeting..."),
			QCoreApplication::translate("MeetingUI", "Updated scheduled meeting"),
			QCoreApplication::translate("MeetingUI", "Failed to update scheduled meeting"));
	}
	if (name == QLatin1String("cancel_scheduled_meeting")) {
		return makePres(
			QCoreApplication::translate("MeetingUI", "Canceling scheduled meeting..."),
			QCoreApplication::translate("MeetingUI", "Canceled scheduled meeting"),
			QCoreApplication::translate("MeetingUI", "Failed to cancel scheduled meeting"));
	}
	if (name == QLatin1String("join_or_start_meeting")) {
		return makePres(
			QCoreApplication::translate("MeetingUI", "Joining or starting meeting..."),
			QCoreApplication::translate("MeetingUI", "Joined or started meeting"),
			QCoreApplication::translate("MeetingUI", "Failed to join or start meeting"));
	}
	if (name == QLatin1String("manage_contact_group")) {
		const QString action = args.value(QStringLiteral("action")).toString().trimmed().toLower();
		const QString groupName = args.value(QStringLiteral("name")).toString().trimmed();
		if (action == QStringLiteral("create")) {
			return groupName.isEmpty()
				? makePres(
					QCoreApplication::translate("MeetingUI", "Creating contact group..."),
					QCoreApplication::translate("MeetingUI", "Created contact group"),
					QCoreApplication::translate("MeetingUI", "Failed to create contact group"))
				: makePres(
					QCoreApplication::translate("MeetingUI", "Creating contact group \"%1\"...").arg(groupName),
					QCoreApplication::translate("MeetingUI", "Created contact group \"%1\"").arg(groupName),
					QCoreApplication::translate("MeetingUI", "Failed to create contact group \"%1\"").arg(groupName));
		}
		if (action == QStringLiteral("rename")) {
			return makePres(
				QCoreApplication::translate("MeetingUI", "Renaming contact group..."),
				QCoreApplication::translate("MeetingUI", "Renamed contact group"),
				QCoreApplication::translate("MeetingUI", "Failed to rename contact group"));
		}
		if (action == QStringLiteral("reorder")) {
			return makePres(
				QCoreApplication::translate("MeetingUI", "Reordering contact group..."),
				QCoreApplication::translate("MeetingUI", "Reordered contact group"),
				QCoreApplication::translate("MeetingUI", "Failed to reorder contact group"));
		}
		if (action == QStringLiteral("delete")) {
			return makePres(
				QCoreApplication::translate("MeetingUI", "Deleting contact group..."),
				QCoreApplication::translate("MeetingUI", "Deleted contact group"),
				QCoreApplication::translate("MeetingUI", "Failed to delete contact group"));
		}
		return makePres(
			QCoreApplication::translate("MeetingUI", "Updating contact groups..."),
			QCoreApplication::translate("MeetingUI", "Updated contact groups"),
			QCoreApplication::translate("MeetingUI", "Failed to update contact groups"));
	}
	if (name == QLatin1String("update_contact_relation")) {
		const QString action = args.value(QStringLiteral("action")).toString(QStringLiteral("update")).trimmed().toLower();
		if (action == QStringLiteral("remove")) {
			return makePres(
				QCoreApplication::translate("MeetingUI", "Removing contact..."),
				QCoreApplication::translate("MeetingUI", "Removed contact"),
				QCoreApplication::translate("MeetingUI", "Failed to remove contact"));
		}
		return makePres(
			QCoreApplication::translate("MeetingUI", "Updating contact relation..."),
			QCoreApplication::translate("MeetingUI", "Updated contact relation"),
			QCoreApplication::translate("MeetingUI", "Failed to update contact relation"));
	}
	if (name == QLatin1String("manage_contact_request")) {
		const QString action = args.value(QStringLiteral("action")).toString().trimmed().toLower();
		if (action == QStringLiteral("accept")) {
			return makePres(
				QCoreApplication::translate("MeetingUI", "Accepting contact request..."),
				QCoreApplication::translate("MeetingUI", "Accepted contact request"),
				QCoreApplication::translate("MeetingUI", "Failed to accept contact request"));
		}
		if (action == QStringLiteral("reject")) {
			return makePres(
				QCoreApplication::translate("MeetingUI", "Declining contact request..."),
				QCoreApplication::translate("MeetingUI", "Declined contact request"),
				QCoreApplication::translate("MeetingUI", "Failed to decline contact request"));
		}
		return makePres(
			QCoreApplication::translate("MeetingUI", "Sending contact request..."),
			QCoreApplication::translate("MeetingUI", "Sent contact request"),
			QCoreApplication::translate("MeetingUI", "Failed to send contact request"));
	}
	if (name == QLatin1String("call_contact")) {
		const QString mode = args.value(QStringLiteral("mode")).toString().trimmed().toLower();
		if (mode == QStringLiteral("meeting_invite")) {
			return makePres(
				QCoreApplication::translate("MeetingUI", "Inviting contact to meeting..."),
				QCoreApplication::translate("MeetingUI", "Invited contact to meeting"),
				QCoreApplication::translate("MeetingUI", "Failed to invite contact"));
		}
		return makePres(
			QCoreApplication::translate("MeetingUI", "Calling contact..."),
			QCoreApplication::translate("MeetingUI", "Called contact"),
			QCoreApplication::translate("MeetingUI", "Failed to call contact"));
	}

	for (const StaticEntry &entry : kStaticEntries) {
		if (name == QLatin1String(entry.toolName)) {
			return makePres(
				QCoreApplication::translate("MeetingUI", entry.invocation),
				QCoreApplication::translate("MeetingUI", entry.pastSucceeded),
				QCoreApplication::translate("MeetingUI", entry.pastFailed));
		}
	}

	return makePres(
		QCoreApplication::translate("MeetingUI", "Running %1...").arg(name),
		QCoreApplication::translate("MeetingUI", "Completed %1").arg(name),
		QCoreApplication::translate("MeetingUI", "Tool %1 failed").arg(name));
}

} // namespace OpenMeeting
