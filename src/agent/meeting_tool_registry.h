#pragma once

#include "src/agent/agent_types.h"

#include <QtCore/QHash>
#include <QtCore/QJsonObject>
#include <QtCore/QStringList>
#include <QtCore/QVariantMap>
#include <QtCore/QVector>

#include <functional>
#include <optional>

namespace OpenMeeting {

class AgentCoordinator;
class MeetingCatalogController;
class MeetingCoordinator;
class SessionManager;

struct AgentHostSettingsBridge {
	std::function<QString()> getTheme;
	std::function<bool(const QString &themeId)> setTheme;
	std::function<QString()> getLanguage;
	std::function<bool(const QString &langId)> setLanguage;
	std::function<QJsonObject()> getAppUpdateStatus;
	std::function<QJsonObject(const QString &action)> performAppUpdateAction;
	std::function<QJsonObject(const QString &meetingId, const QString &password, const QString &displayName)> joinMeetingFromLobby;
	std::function<QJsonObject(const QString &title, int durationMinutes)> startQuickMeetingFromLobby;
};

struct AgentToolViewContext {
	AgentMeetingContext meetingOverview;
	MeetingCoordinator *meeting = nullptr;
	MeetingCatalogController *catalog = nullptr;
	const SessionManager *sessionManager = nullptr;
	const AgentCoordinator *agentCoordinator = nullptr;
	const AgentHostSettingsBridge *hostSettings = nullptr;
	const QVector<AgentMeetingChatEntry> *recentChat = nullptr;
	const QVector<AgentMeetingTranscriptEntry> *recentTranscripts = nullptr;
	const QVariantMap *telemetrySnapshot = nullptr;
	bool canShareMeetingData = false;
	bool canUseCatalogTools = false;
	bool canUseClientPrefTools = false;
};

struct AgentToolExecuteContext {
	MeetingCoordinator *meeting = nullptr;
	MeetingCatalogController *catalog = nullptr;
	SessionManager *sessionManager = nullptr;
	AgentCoordinator *agentCoordinator = nullptr;
	const AgentHostSettingsBridge *hostSettings = nullptr;
};

struct PreparedPendingAction {
	QString title;
	QString summary;
	QJsonObject normalizedArgs;
};

struct ToolExecutionOutcome {
	AgentToolResult toolResult;
	bool asyncInFlight = false;
	QString uiStatusMessage;
	QString assistantFollowupMessage;
};

struct ToolInvocationPresentation {
	QString invocationMessage;
	QString pastTenseMessage;
};

struct RegisteredTool {
	AgentToolDefinition definition;
	bool exposedInToolDefinitions = true;
	std::function<bool(const AgentToolViewContext &ctx)> isAvailable;
	std::function<AgentToolResult(const QJsonObject &args, const AgentToolViewContext &ctx)> executeReadOnly;
	std::function<bool(
		const QJsonObject &rawArgs,
		const AgentToolViewContext &ctx,
		PreparedPendingAction &outPrepared,
		QString &outError)> prepareAction;
	std::function<ToolExecutionOutcome(
		const QJsonObject &normalizedArgs,
		const AgentToolExecuteContext &ctx)> executeConfirmed;
};

// Controlled table-driven registry of read-only inspection tools, catalog/scheduling tools,
// host moderation tools, client preference tools, and confirmation-gated action tools.
// Never holds raw livekit::Room pointers.
class MeetingToolRegistry final {
public:
	MeetingToolRegistry();

	void registerTool(RegisteredTool tool);

	[[nodiscard]] QVector<AgentToolDefinition> toolDefinitions(
		const AgentToolViewContext &ctx) const;

	// Legacy scope-filter overload retained for existing callers/tests.
	[[nodiscard]] QVector<AgentToolDefinition> toolDefinitions(
		bool includeMeetingTools,
		bool includeCatalogTools = false,
		bool includeClientPreferenceTools = false) const;

	[[nodiscard]] std::optional<AgentToolDefinition> findTool(const QString &name) const;
	[[nodiscard]] bool isToolAvailable(const QString &name, const AgentToolViewContext &ctx) const;
	[[nodiscard]] bool isReadOnlyTool(const QString &name) const;
	[[nodiscard]] static bool isCatalogTool(const QString &name);
	[[nodiscard]] static bool isClientPreferenceTool(const QString &name);

	[[nodiscard]] static bool parseToolArguments(
		const QString &argumentsJson,
		QJsonObject &outArgs,
		QString &outError);

	[[nodiscard]] AgentToolResult executeReadOnly(
		const QString &name,
		const QJsonObject &args,
		const AgentToolViewContext &ctx) const;

	[[nodiscard]] AgentToolResult executeReadOnly(
		const QString &name,
		const QJsonObject &args,
		const AgentMeetingContext &overview,
		MeetingCoordinator *meeting,
		const QVector<AgentMeetingChatEntry> &recentChat,
		const QVariantMap &telemetrySnapshot,
		MeetingCatalogController *catalog = nullptr,
		const SessionManager *sessionManager = nullptr) const;

	[[nodiscard]] bool preparePendingAction(
		const QString &name,
		const QJsonObject &rawArgs,
		const AgentToolViewContext &ctx,
		PreparedPendingAction &outPrepared,
		QString &outError) const;

	[[nodiscard]] bool describePendingAction(
		const QString &name,
		const QJsonObject &args,
		QString &outTitle,
		QString &outSummary,
		QString &outError,
		const MeetingCatalogController *catalog = nullptr,
		const SessionManager *sessionManager = nullptr,
		MeetingCoordinator *meeting = nullptr) const;

	[[nodiscard]] ToolExecutionOutcome executeConfirmedActionWithOutcome(
		const QString &name,
		const QJsonObject &normalizedArgs,
		const AgentToolExecuteContext &ctx) const;

	[[nodiscard]] AgentToolResult executeConfirmedAction(
		const QString &name,
		const QJsonObject &args,
		MeetingCoordinator *meeting,
		MeetingCatalogController *catalog = nullptr,
		SessionManager *sessionManager = nullptr) const;

	[[nodiscard]] static QString formatScheduledMeetingCompletionSummary(
		const QJsonObject &args,
		const QString &meetingId);
	[[nodiscard]] static QString formatJoinMediaPreferencesCompletionSummary(
		const QJsonObject &args);
	[[nodiscard]] static ToolInvocationPresentation describeToolInvocation(
		const QString &name,
		const QJsonObject &args = {},
		bool succeeded = true);

private:
	void registerCatalogTools();
	void registerMeetingControlTools();
	void registerHostModerationTools();
	void registerClientSettingsTools();

	QHash<QString, RegisteredTool> _registry;
	QStringList _toolOrder;
};

} // namespace OpenMeeting
