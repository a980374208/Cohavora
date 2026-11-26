#pragma once

#include "src/agent/meeting_tool_registry.h"
#include "src/media/screen_share_quality.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <optional>

namespace OpenMeeting::tool_helpers {

inline QJsonObject makeObjectSchema(const QJsonObject &properties, const QStringList &required = {}) {
	QJsonObject schema;
	schema.insert(QStringLiteral("type"), QStringLiteral("object"));
	schema.insert(QStringLiteral("properties"), properties);
	if (!required.isEmpty()) {
		QJsonArray req;
		for (const auto &field : required) {
			req.append(field);
		}
		schema.insert(QStringLiteral("required"), req);
	}
	schema.insert(QStringLiteral("additionalProperties"), false);
	return schema;
}

inline QJsonObject makeProp(const QString &type, const QString &description) {
	QJsonObject prop;
	prop.insert(QStringLiteral("type"), type);
	prop.insert(QStringLiteral("description"), description);
	return prop;
}

inline QJsonObject makeArrayProp(const QString &itemType, const QString &description) {
	QJsonObject items;
	items.insert(QStringLiteral("type"), itemType);
	QJsonObject prop;
	prop.insert(QStringLiteral("type"), QStringLiteral("array"));
	prop.insert(QStringLiteral("items"), items);
	prop.insert(QStringLiteral("description"), description);
	return prop;
}

inline QString jsonString(const QJsonObject &obj) {
	return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

inline AgentToolResult makeErrorResult(const QString &toolName, const QString &message) {
	AgentToolResult res;
	res.toolName = toolName;
	res.success = false;
	res.errorMessage = message;
	QJsonObject errObj;
	errObj.insert(QStringLiteral("ok"), false);
	errObj.insert(QStringLiteral("error"), message);
	res.content = jsonString(errObj);
	return res;
}

inline AgentToolResult makeOkResult(const QString &toolName, const QJsonObject &payload) {
	AgentToolResult res;
	res.toolName = toolName;
	res.success = true;
	QJsonObject obj = payload;
	obj.insert(QStringLiteral("ok"), true);
	res.content = jsonString(obj);
	return res;
}

inline ToolExecutionOutcome makeSimpleOutcome(
		const AgentToolResult &res,
		const QString &uiStatus = {},
		const QString &assistantFollowup = {}) {
	ToolExecutionOutcome out;
	out.toolResult = res;
	out.asyncInFlight = false;
	out.uiStatusMessage = uiStatus;
	out.assistantFollowupMessage = assistantFollowup;
	return out;
}

inline QString screenShareResolutionToString(int res) {
	switch (static_cast<livekit::ScreenShareResolution>(res)) {
	case livekit::ScreenShareResolution::P720:
		return QStringLiteral("720p");
	case livekit::ScreenShareResolution::P1080:
		return QStringLiteral("1080p");
	case livekit::ScreenShareResolution::P1440:
		return QStringLiteral("1440p");
	case livekit::ScreenShareResolution::Native:
		return QStringLiteral("native");
	case livekit::ScreenShareResolution::P2160:
		return QStringLiteral("2160p");
	case livekit::ScreenShareResolution::Auto:
	default:
		return QStringLiteral("auto");
	}
}

inline std::optional<livekit::ScreenShareResolution> parseScreenShareResolution(const QString &raw) {
	const QString norm = raw.trimmed().toLower();
	if (norm == QStringLiteral("auto")) return livekit::ScreenShareResolution::Auto;
	if (norm == QStringLiteral("720p") || norm == QStringLiteral("p720")) return livekit::ScreenShareResolution::P720;
	if (norm == QStringLiteral("1080p") || norm == QStringLiteral("p1080")) return livekit::ScreenShareResolution::P1080;
	if (norm == QStringLiteral("1440p") || norm == QStringLiteral("p1440")) return livekit::ScreenShareResolution::P1440;
	if (norm == QStringLiteral("native")) return livekit::ScreenShareResolution::Native;
	if (norm == QStringLiteral("2160p") || norm == QStringLiteral("p2160") || norm == QStringLiteral("4k")) {
		return livekit::ScreenShareResolution::P2160;
	}
	return std::nullopt;
}

} // namespace OpenMeeting::tool_helpers
