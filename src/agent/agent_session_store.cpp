#include "src/agent/agent_session_store.h"
#include <QtCore/QElapsedTimer>

#include "src/core/service_account_scope.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QStandardPaths>
#include <QtCore/QUuid>

#include <algorithm>

namespace OpenMeeting {
namespace {

constexpr int kStorageSchemaVersion = 1;
constexpr qint64 kMaxSessionFileBytes = 8LL * 1024 * 1024;

qint64 readInt64(const QJsonObject &obj, const QString &key, qint64 fallback = 0) {
	const QJsonValue v = obj.value(key);
	if (v.isUndefined() || v.isNull()) {
		return fallback;
	}
	bool ok = false;
	const qint64 res = v.toVariant().toLongLong(&ok);
	return ok ? res : static_cast<qint64>(v.toDouble(static_cast<double>(fallback)));
}

QString safeScopeKey(const QString &raw) {
	QString out;
	out.reserve(raw.size());
	for (const QChar ch : raw) {
		if (ch.isLetterOrNumber() || ch == QLatin1Char('_') || ch == QLatin1Char('-')) {
			out.append(ch);
		}
		if (out.size() >= 48) {
			break;
		}
	}
	return out.isEmpty() ? QStringLiteral("anonymous") : out;
}

QJsonObject serializeImageMeta(const AgentImageAttachment &att) {
	QJsonObject obj;
	if (!att.id.isEmpty()) {
		obj.insert(QStringLiteral("id"), att.id);
	}
	obj.insert(QStringLiteral("fileName"), att.fileName);
	obj.insert(QStringLiteral("mimeType"), att.mimeType);
	obj.insert(QStringLiteral("width"), att.width);
	obj.insert(QStringLiteral("height"), att.height);
	obj.insert(QStringLiteral("originalSize"), att.originalSize);
	return obj;
}

AgentImageAttachment deserializeImageMeta(const QJsonObject &obj) {
	AgentImageAttachment att;
	att.id = obj.value(QStringLiteral("id")).toString();
	att.fileName = obj.value(QStringLiteral("fileName")).toString();
	att.mimeType = obj.value(QStringLiteral("mimeType")).toString();
	att.width = obj.value(QStringLiteral("width")).toInt();
	att.height = obj.value(QStringLiteral("height")).toInt();
	att.originalSize = readInt64(obj, QStringLiteral("originalSize"));
	return att;
}

QJsonObject serializeToolCall(const AgentToolCall &tc) {
	QJsonObject obj;
	obj.insert(QStringLiteral("id"), tc.id);
	obj.insert(QStringLiteral("name"), tc.name);
	obj.insert(QStringLiteral("argumentsJson"), tc.argumentsJson);
	if (!tc.providerCallId.isEmpty()) {
		obj.insert(QStringLiteral("providerCallId"), tc.providerCallId);
	}
	if (!tc.thoughtSignature.isEmpty()) {
		obj.insert(QStringLiteral("thoughtSignature"), tc.thoughtSignature);
	}
	return obj;
}

AgentToolCall deserializeToolCall(const QJsonObject &obj) {
	AgentToolCall tc;
	tc.id = obj.value(QStringLiteral("id")).toString();
	tc.name = obj.value(QStringLiteral("name")).toString();
	tc.argumentsJson = obj.value(QStringLiteral("argumentsJson")).toString();
	tc.providerCallId = obj.value(QStringLiteral("providerCallId")).toString();
	tc.thoughtSignature = obj.value(QStringLiteral("thoughtSignature")).toString();
	return tc;
}

QJsonObject serializeThinkingBlock(const AgentThinkingBlock &tb) {
	QJsonObject obj;
	obj.insert(QStringLiteral("type"), tb.type);
	if (!tb.thinking.isEmpty()) {
		obj.insert(QStringLiteral("thinking"), tb.thinking);
	}
	if (!tb.signature.isEmpty()) {
		obj.insert(QStringLiteral("signature"), tb.signature);
	}
	return obj;
}

AgentThinkingBlock deserializeThinkingBlock(const QJsonObject &obj) {
	AgentThinkingBlock tb;
	tb.type = obj.value(QStringLiteral("type")).toString(QStringLiteral("thinking"));
	if (tb.type != QStringLiteral("thinking") &&
		tb.type != QStringLiteral("redacted_thinking") &&
		tb.type != QStringLiteral("gemini_thought")) {
		tb.type = QStringLiteral("thinking");
	}
	tb.thinking = obj.value(QStringLiteral("thinking")).toString();
	tb.signature = obj.value(QStringLiteral("signature")).toString();
	return tb;
}

QJsonObject serializeResponsePart(const AgentResponsePart &part) {
	QJsonObject obj;
	obj.insert(QStringLiteral("type"), static_cast<int>(part.type));
	obj.insert(QStringLiteral("text"), part.text);
	obj.insert(QStringLiteral("durationMs"), part.durationMs);
	if (part.type == AgentResponsePart::Type::ToolInvocation) {
		obj.insert(QStringLiteral("toolCallId"), part.toolCallId);
		obj.insert(QStringLiteral("toolName"), part.toolName);
		obj.insert(QStringLiteral("invocationMessage"), part.invocationMessage);
		obj.insert(QStringLiteral("pastTenseMessage"), part.pastTenseMessage);
		obj.insert(QStringLiteral("argumentsJson"), part.argumentsJson);
		obj.insert(QStringLiteral("resultSummary"), part.resultSummary);
		obj.insert(QStringLiteral("isReadOnly"), part.isReadOnly);
		auto st = part.toolState;
		if (st == AgentResponsePart::ToolState::Running ||
			st == AgentResponsePart::ToolState::WaitingForConfirmation) {
			st = AgentResponsePart::ToolState::Failed;
		}
		obj.insert(QStringLiteral("toolState"), static_cast<int>(st));
	}
	return obj;
}

AgentResponsePart deserializeResponsePart(const QJsonObject &obj) {
	AgentResponsePart part;
	const int rawType = obj.value(QStringLiteral("type")).toInt(static_cast<int>(AgentResponsePart::Type::Markdown));
	switch (rawType) {
	case static_cast<int>(AgentResponsePart::Type::Thinking):
		part.type = AgentResponsePart::Type::Thinking;
		break;
	case static_cast<int>(AgentResponsePart::Type::ToolInvocation):
		part.type = AgentResponsePart::Type::ToolInvocation;
		break;
	default:
		part.type = AgentResponsePart::Type::Markdown;
		break;
	}
	part.text = obj.value(QStringLiteral("text")).toString();
	part.active = false;
	part.durationMs = readInt64(obj, QStringLiteral("durationMs"));
	if (part.type == AgentResponsePart::Type::ToolInvocation) {
		part.toolCallId = obj.value(QStringLiteral("toolCallId")).toString();
		part.toolName = obj.value(QStringLiteral("toolName")).toString();
		part.invocationMessage = obj.value(QStringLiteral("invocationMessage")).toString();
		part.pastTenseMessage = obj.value(QStringLiteral("pastTenseMessage")).toString();
		part.argumentsJson = obj.value(QStringLiteral("argumentsJson")).toString();
		part.resultSummary = obj.value(QStringLiteral("resultSummary")).toString();
		part.isReadOnly = obj.value(QStringLiteral("isReadOnly")).toBool(true);
		const int rawState = obj.value(QStringLiteral("toolState")).toInt(static_cast<int>(AgentResponsePart::ToolState::Completed));
		if (rawState >= static_cast<int>(AgentResponsePart::ToolState::Running) &&
			rawState <= static_cast<int>(AgentResponsePart::ToolState::Rejected)) {
			part.toolState = static_cast<AgentResponsePart::ToolState>(rawState);
		}
		if (part.toolState == AgentResponsePart::ToolState::Running ||
			part.toolState == AgentResponsePart::ToolState::WaitingForConfirmation) {
			part.toolState = AgentResponsePart::ToolState::Failed;
		}
	}
	return part;
}

QJsonObject serializeMessage(const AgentChatMessage &msg) {
	QJsonObject obj;
	obj.insert(QStringLiteral("id"), msg.id);
	obj.insert(QStringLiteral("role"), static_cast<int>(msg.role));
	obj.insert(QStringLiteral("content"), msg.content);
	if (!msg.reasoningContent.isEmpty()) {
		obj.insert(QStringLiteral("reasoningContent"), msg.reasoningContent);
		obj.insert(QStringLiteral("thinkingDurationMs"), msg.thinkingDurationMs);
	}
	auto st = msg.status;
	QString errMsg = msg.errorMessage;
	if (st == AgentChatMessage::Status::Streaming) {
		st = AgentChatMessage::Status::Cancelled;
		if (errMsg.isEmpty()) {
			errMsg = QStringLiteral("Interrupted before completion.");
		}
	}
	obj.insert(QStringLiteral("status"), static_cast<int>(st));
	if (!errMsg.isEmpty()) {
		obj.insert(QStringLiteral("errorMessage"), errMsg);
	}
	if (msg.timestamp.isValid()) {
		obj.insert(QStringLiteral("timestampMs"), msg.timestamp.toMSecsSinceEpoch());
	}
	if (!msg.toolCalls.isEmpty()) {
		QJsonArray calls;
		for (const auto &tc : msg.toolCalls) {
			calls.push_back(serializeToolCall(tc));
		}
		obj.insert(QStringLiteral("toolCalls"), calls);
	}
	if (!msg.thinkingBlocks.isEmpty()) {
		QJsonArray tbArr;
		for (const auto &tb : msg.thinkingBlocks) {
			tbArr.push_back(serializeThinkingBlock(tb));
		}
		obj.insert(QStringLiteral("thinkingBlocks"), tbArr);
	}
	if (!msg.toolCallId.isEmpty()) {
		obj.insert(QStringLiteral("toolCallId"), msg.toolCallId);
	}
	if (!msg.toolName.isEmpty()) {
		obj.insert(QStringLiteral("toolName"), msg.toolName);
	}
	if (msg.role == AgentChatMessage::Role::Tool &&
		msg.content.contains(QStringLiteral("awaiting_user_confirmation"))) {
		obj.insert(QStringLiteral("content"),
			QStringLiteral("{\"ok\":false,\"status\":\"expired\",\"reason\":\"Session reloaded before confirmation.\"}"));
	}
	if (!msg.images.isEmpty()) {
		QJsonArray imgs;
		for (const auto &img : msg.images) {
			imgs.push_back(serializeImageMeta(img));
		}
		obj.insert(QStringLiteral("images"), imgs);
	}
	if (!msg.contextChips.isEmpty()) {
		QJsonArray chips;
		for (const auto &chip : msg.contextChips) {
			QJsonObject c;
			c.insert(QStringLiteral("kind"), static_cast<int>(chip.kind));
			c.insert(QStringLiteral("label"), chip.label);
			chips.push_back(c);
		}
		obj.insert(QStringLiteral("contextChips"), chips);
	}
	if (!msg.parts.isEmpty()) {
		QJsonArray partsArr;
		for (const auto &part : msg.parts) {
			partsArr.push_back(serializeResponsePart(part));
		}
		obj.insert(QStringLiteral("parts"), partsArr);
	}
	if (!msg.followups.isEmpty()) {
		QJsonArray fuArr;
		for (const auto &fu : msg.followups) {
			QJsonObject f;
			f.insert(QStringLiteral("label"), fu.label);
			f.insert(QStringLiteral("prompt"), fu.prompt);
			fuArr.push_back(f);
		}
		obj.insert(QStringLiteral("followups"), fuArr);
	}
	return obj;
}

AgentChatMessage deserializeMessage(const QJsonObject &obj) {
	AgentChatMessage msg;
	msg.id = obj.value(QStringLiteral("id")).toString();
	if (msg.id.isEmpty()) {
		msg.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
	}
	const int rawRole = obj.value(QStringLiteral("role")).toInt(static_cast<int>(AgentChatMessage::Role::User));
	if (rawRole >= static_cast<int>(AgentChatMessage::Role::System) &&
		rawRole <= static_cast<int>(AgentChatMessage::Role::Tool)) {
		msg.role = static_cast<AgentChatMessage::Role>(rawRole);
	}
	msg.content = obj.value(QStringLiteral("content")).toString();
	msg.reasoningContent = obj.value(QStringLiteral("reasoningContent")).toString();
	msg.thinkingDurationMs = readInt64(obj, QStringLiteral("thinkingDurationMs"));
	msg.isThinkingActive = false;

	const int rawStatus = obj.value(QStringLiteral("status")).toInt(static_cast<int>(AgentChatMessage::Status::Complete));
	if (rawStatus >= static_cast<int>(AgentChatMessage::Status::Complete) &&
		rawStatus <= static_cast<int>(AgentChatMessage::Status::Failed)) {
		msg.status = static_cast<AgentChatMessage::Status>(rawStatus);
	}
	if (msg.status == AgentChatMessage::Status::Streaming) {
		msg.status = AgentChatMessage::Status::Cancelled;
	}
	msg.errorMessage = obj.value(QStringLiteral("errorMessage")).toString();
	const qint64 tsMs = readInt64(obj, QStringLiteral("timestampMs"));
	if (tsMs > 0) {
		msg.timestamp = QDateTime::fromMSecsSinceEpoch(tsMs);
	}
	for (const auto &val : obj.value(QStringLiteral("toolCalls")).toArray()) {
		if (val.isObject()) {
			msg.toolCalls.push_back(deserializeToolCall(val.toObject()));
		}
	}
	for (const auto &val : obj.value(QStringLiteral("thinkingBlocks")).toArray()) {
		if (val.isObject()) {
			msg.thinkingBlocks.push_back(deserializeThinkingBlock(val.toObject()));
		}
	}
	msg.toolCallId = obj.value(QStringLiteral("toolCallId")).toString();
	msg.toolName = obj.value(QStringLiteral("toolName")).toString();
	if (msg.role == AgentChatMessage::Role::Tool &&
		msg.content.contains(QStringLiteral("awaiting_user_confirmation"))) {
		msg.content = QStringLiteral("{\"ok\":false,\"status\":\"expired\",\"reason\":\"Session reloaded before confirmation.\"}");
	}
	for (const auto &val : obj.value(QStringLiteral("images")).toArray()) {
		if (val.isObject()) {
			msg.images.push_back(deserializeImageMeta(val.toObject()));
		}
	}
	for (const auto &val : obj.value(QStringLiteral("contextChips")).toArray()) {
		if (val.isObject()) {
			const auto co = val.toObject();
			const int rawKind = co.value(QStringLiteral("kind")).toInt(0);
			if (rawKind >= static_cast<int>(AgentContextChipKind::MeetingChat) &&
				rawKind <= static_cast<int>(AgentContextChipKind::Schedule)) {
				msg.contextChips.push_back({
					static_cast<AgentContextChipKind>(rawKind),
					co.value(QStringLiteral("label")).toString(),
				});
			}
		}
	}
	for (const auto &val : obj.value(QStringLiteral("parts")).toArray()) {
		if (val.isObject()) {
			msg.parts.push_back(deserializeResponsePart(val.toObject()));
		}
	}
	for (const auto &val : obj.value(QStringLiteral("followups")).toArray()) {
		if (val.isObject()) {
			const auto fo = val.toObject();
			AgentFollowupSuggestion sug{
				fo.value(QStringLiteral("label")).toString(),
				fo.value(QStringLiteral("prompt")).toString(),
			};
			if (!sug.label.isEmpty() && !sug.prompt.isEmpty()) {
				msg.followups.push_back(std::move(sug));
			}
		}
	}
	return msg;
}

} // namespace

AgentSessionStore::AgentSessionStore(QString baseDirectory)
	: _baseDirectory(std::move(baseDirectory)) {
	if (_baseDirectory.trimmed().isEmpty()) {
		const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
		_baseDirectory = QDir(appData).filePath(QStringLiteral("copilot_sessions"));
	}
}

QString AgentSessionStore::resolveBaseDirectory(const QSettings &settings) {
	if (settings.format() == QSettings::IniFormat && !settings.fileName().trimmed().isEmpty()) {
		const QFileInfo fi(settings.fileName());
		return QDir(fi.absolutePath()).filePath(QStringLiteral("copilot_sessions"));
	}
	const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
	return QDir(appData).filePath(QStringLiteral("copilot_sessions"));
}

QString AgentSessionStore::accountScopeKey(bool loggedIn, const QString &userId, const QString &serviceUrl) {
	return localServiceAccountScopeKey(loggedIn, userId, serviceUrl);
}

QString AgentSessionStore::deriveSessionTitle(const QVector<AgentChatMessage> &messages) {
	for (const auto &msg : messages) {
		if (msg.role != AgentChatMessage::Role::User) {
			continue;
		}
		QString line = msg.content.trimmed();
		if (line.isEmpty() && !msg.contextChips.isEmpty()) {
			line = msg.contextChips.first().label;
		}
		if (line.isEmpty() && !msg.images.isEmpty()) {
			line = QCoreApplication::translate("MeetingUI", "Image prompt");
		}
		line.replace(QLatin1Char('\n'), QLatin1Char(' '));
		line = line.simplified();
		if (!line.isEmpty()) {
			return (line.size() > 38) ? (line.left(35) + QStringLiteral("...")) : line;
		}
	}
	return QCoreApplication::translate("MeetingUI", "New Chat");
}

QString AgentSessionStore::exportSessionMarkdown(const AgentChatSessionData &session) {
	QStringList out;
	const QString title = session.summary.title.trimmed().isEmpty()
		? deriveSessionTitle(session.messages)
		: session.summary.title.trimmed();
	out.push_back(QStringLiteral("# %1").arg(title));
	if (session.summary.updatedAt.isValid()) {
		out.push_back(QStringLiteral("_Updated: %1_").arg(
			session.summary.updatedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm"))));
	}
	out.push_back(QString());

	for (const auto &msg : session.messages) {
		if (msg.role == AgentChatMessage::Role::System || msg.role == AgentChatMessage::Role::Tool) {
			continue;
		}
		const QString who = (msg.role == AgentChatMessage::Role::User)
			? QStringLiteral("## You")
			: QStringLiteral("## Copilot");
		const QString ts = msg.timestamp.isValid()
			? QStringLiteral(" (%1)").arg(msg.timestamp.toString(QStringLiteral("HH:mm:ss")))
			: QString();
		out.push_back(who + ts);

		if (!msg.contextChips.isEmpty()) {
			QStringList chipLabels;
			for (const auto &c : msg.contextChips) {
				chipLabels.push_back(c.label);
			}
			out.push_back(QStringLiteral("> Context: %1").arg(chipLabels.join(QStringLiteral(", "))));
			out.push_back(QString());
		}

		if (!msg.reasoningContent.trimmed().isEmpty()) {
			out.push_back(QStringLiteral("<details><summary>Thinking process</summary>\n\n%1\n\n</details>")
				.arg(msg.reasoningContent.trimmed()));
			out.push_back(QString());
		}

		for (const auto &part : msg.parts) {
			if (part.type == AgentResponsePart::Type::ToolInvocation) {
				const QString label = part.pastTenseMessage.isEmpty()
					? part.invocationMessage
					: part.pastTenseMessage;
				if (!label.isEmpty()) {
					out.push_back(QStringLiteral("- [Tool `%1`] %2").arg(part.toolName, label));
				}
			}
		}

		if (!msg.content.trimmed().isEmpty()) {
			out.push_back(msg.content.trimmed());
		}
		if (!msg.images.isEmpty()) {
			QStringList names;
			for (const auto &img : msg.images) {
				names.push_back(img.fileName.isEmpty() ? QStringLiteral("image") : img.fileName);
			}
			out.push_back(QStringLiteral("_Attached images: %1_").arg(names.join(QStringLiteral(", "))));
		}
		if (!msg.errorMessage.trimmed().isEmpty() && msg.status != AgentChatMessage::Status::Complete) {
			out.push_back(QStringLiteral("> Status: %1").arg(msg.errorMessage.trimmed()));
		}
		out.push_back(QString());
	}
	return out.join(QLatin1Char('\n')).trimmed() + QLatin1Char('\n');
}

QString AgentSessionStore::sessionFilePath(const QString &accountKey) const {
	const QString scope = safeScopeKey(accountKey);
	return QDir(QDir(_baseDirectory).filePath(scope)).filePath(QStringLiteral("sessions.json"));
}

AgentSessionStore::LoadedSessions AgentSessionStore::loadAccountSessions(const QString &accountKey) const {
	LoadedSessions loaded;
	QFile file(sessionFilePath(accountKey));
	if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
		return loaded;
	}
	if (file.size() <= 0 || file.size() > kMaxSessionFileBytes) {
		return loaded;
	}
	const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
	if (!doc.isObject()) {
		return loaded;
	}
	const QJsonObject root = doc.object();
	if (root.value(QStringLiteral("version")).toInt(0) != kStorageSchemaVersion) {
		return loaded;
	}
	loaded.activeSessionId = root.value(QStringLiteral("activeSessionId")).toString();

	const QJsonArray arr = root.value(QStringLiteral("sessions")).toArray();
	for (const auto &val : arr) {
		if (!val.isObject()) {
			continue;
		}
		const QJsonObject so = val.toObject();
		AgentChatSessionData s;
		s.summary.sessionId = so.value(QStringLiteral("sessionId")).toString();
		if (s.summary.sessionId.trimmed().isEmpty()) {
			continue;
		}
		s.summary.title = so.value(QStringLiteral("title")).toString();
		const qint64 createdMs = readInt64(so, QStringLiteral("createdAtMs"));
		const qint64 updatedMs = readInt64(so, QStringLiteral("updatedAtMs"));
		s.summary.createdAt = createdMs > 0 ? QDateTime::fromMSecsSinceEpoch(createdMs) : QDateTime::currentDateTime();
		s.summary.updatedAt = updatedMs > 0 ? QDateTime::fromMSecsSinceEpoch(updatedMs) : s.summary.createdAt;
		const int rawMode = so.value(QStringLiteral("chatMode")).toInt(0);
		s.summary.chatMode = (rawMode == static_cast<int>(AgentChatMode::Ask))
			? AgentChatMode::Ask
			: AgentChatMode::Agent;

		const QJsonArray msgsArr = so.value(QStringLiteral("messages")).toArray();
		for (const auto &mv : msgsArr) {
			if (mv.isObject()) {
				s.messages.push_back(deserializeMessage(mv.toObject()));
				if (s.messages.size() >= kMaxMessagesPerSession) {
					break;
				}
			}
		}
		if (s.summary.title.trimmed().isEmpty()) {
			s.summary.title = deriveSessionTitle(s.messages);
		}
		s.summary.messageCount = s.messages.size();
		loaded.sessions.push_back(std::move(s));
		if (loaded.sessions.size() >= kMaxPersistedSessions) {
			break;
		}
	}
	return loaded;
}

bool AgentSessionStore::saveAccountSessions(
		const QString &accountKey,
		const QString &activeSessionId,
		const QVector<AgentChatSessionData> &sessions, SaveTimings *timings) const {
	QElapsedTimer timer;
	if (timings) timer.start();
	const QString path = sessionFilePath(accountKey);
	const QFileInfo fi(path);
	if (!QDir().mkpath(fi.absolutePath())) {
		return false;
	}

	QVector<AgentChatSessionData> filtered;
	filtered.reserve(sessions.size());
	for (const auto &s : sessions) {
		if (s.summary.sessionId.trimmed().isEmpty()) {
			continue;
		}
		if (s.messages.isEmpty() && s.summary.sessionId != activeSessionId) {
			continue;
		}
		AgentChatSessionData copy = s;
		if (copy.messages.size() > kMaxMessagesPerSession) {
			copy.messages = copy.messages.mid(copy.messages.size() - kMaxMessagesPerSession);
		}
		copy.summary.messageCount = copy.messages.size();
		if (copy.summary.title.trimmed().isEmpty()) {
			copy.summary.title = deriveSessionTitle(copy.messages);
		}
		filtered.push_back(std::move(copy));
	}

	std::stable_sort(filtered.begin(), filtered.end(), [](const AgentChatSessionData &a, const AgentChatSessionData &b) {
		return a.summary.updatedAt > b.summary.updatedAt;
	});
	if (filtered.size() > kMaxPersistedSessions) {
		filtered.resize(kMaxPersistedSessions);
	}

	if (timings) timings->preparationNs = timer.nsecsElapsed();
	QJsonArray arr;
	for (const auto &s : filtered) {
		QJsonObject so;
		so.insert(QStringLiteral("sessionId"), s.summary.sessionId);
		so.insert(QStringLiteral("title"), s.summary.title);
		so.insert(QStringLiteral("createdAtMs"), s.summary.createdAt.isValid()
			? s.summary.createdAt.toMSecsSinceEpoch()
			: QDateTime::currentMSecsSinceEpoch());
		so.insert(QStringLiteral("updatedAtMs"), s.summary.updatedAt.isValid()
			? s.summary.updatedAt.toMSecsSinceEpoch()
			: QDateTime::currentMSecsSinceEpoch());
		so.insert(QStringLiteral("chatMode"), static_cast<int>(s.summary.chatMode));
		QJsonArray msgsArr;
		for (const auto &m : s.messages) {
			msgsArr.push_back(serializeMessage(m));
		}
		so.insert(QStringLiteral("messages"), msgsArr);
		arr.push_back(so);
	}

	QJsonObject root;
	root.insert(QStringLiteral("version"), kStorageSchemaVersion);
	root.insert(QStringLiteral("activeSessionId"), activeSessionId);
	root.insert(QStringLiteral("sessions"), arr);

	const auto bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
	if (timings) timings->serializationNs = timer.nsecsElapsed() - timings->preparationNs;
	if (bytes.size() > kMaxSessionFileBytes) return false;
	QSaveFile save(path);
	if (!save.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		return false;
	}
	if (save.write(bytes) != bytes.size()) return false;
	const bool committed = save.commit();
	if (timings) timings->commitNs = timer.nsecsElapsed() - timings->preparationNs - timings->serializationNs;
	return committed;
}

bool AgentSessionStore::clearAccountSessions(const QString &accountKey) const {
	const QString path = sessionFilePath(accountKey);
	if (!QFile::exists(path)) {
		return true;
	}
	return QFile::remove(path);
}

} // namespace OpenMeeting
