#pragma once

#include "src/agent/agent_types.h"

#include <QtCore/QByteArray>
#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QVector>

#include <functional>
#include <memory>

namespace OpenMeeting {

struct SseEvent {
	QString event;
	QByteArray data;
	QString id;
};

// Incremental Server-Sent Events parser handling chunk boundaries across
// CRLF/LF/CR newlines, comment lines, multi-line data fields, and [DONE].
class SseEventFramer final {
public:
	static constexpr qsizetype kMaxBufferBytes = 2 * 1024 * 1024;
	static constexpr qsizetype kMaxEventBytes = 1 * 1024 * 1024;

	[[nodiscard]] bool feed(const QByteArray &chunk, QVector<SseEvent> &outEvents, QString *errorOut = nullptr);
	void flush(QVector<SseEvent> &outEvents);
	void reset();

private:
	void processLine(const QByteArray &line, QVector<SseEvent> &outEvents, QString *errorOut, bool &overflow);
	void flushCurrentEvent(QVector<SseEvent> &outEvents);

	QByteArray _buffer;
	QString _currentEvent;
	QByteArray _currentData;
	QString _currentId;
	bool _hasData = false;
	bool _pendingCr = false;
};

struct AgentChatRequest {
	AgentProfile profile;
	QString apiKey;
	QVector<AgentChatMessage> messages;
	std::optional<AgentMeetingContext> meetingContext;
	QString explicitContextPrompt;
	AgentChatMode mode = AgentChatMode::Agent;
	QVector<AgentToolDefinition> tools;
	bool stream = true;
};

struct AgentStreamDelta {
	QString textDelta;
	QString reasoningDelta;
	bool isDone = false;
	QString finishReason;
	QString safeError;
	QVector<AgentToolCall> toolCalls;
	QVector<AgentThinkingBlock> thinkingBlocks;
};

class AgentRequestHandle : public QObject {
	Q_OBJECT
public:
	using QObject::QObject;
	~AgentRequestHandle() override = default;

	virtual void cancel() = 0;

signals:
	void deltaReceived(const OpenMeeting::AgentStreamDelta &delta);
	void finished(bool success, const QString &safeError);
};

class AgentModelListHandle : public QObject {
	Q_OBJECT
public:
	using QObject::QObject;
	~AgentModelListHandle() override = default;

	virtual void cancel() = 0;
	[[nodiscard]] virtual QHash<QString, AgentModelCapabilities> modelCapabilities() const { return {}; }
	[[nodiscard]] virtual QStringList speechModels() const { return {}; }
	[[nodiscard]] virtual int filteredNonChatCount() const { return 0; }

signals:
	void finished(bool success, const QStringList &models, const QString &safeError);
};

class IAgentTransport {
public:
	virtual ~IAgentTransport() = default;

	virtual AgentRequestHandle *sendChat(const AgentChatRequest &request, QObject *parent = nullptr) = 0;

	// Lightweight probe using current profile + key. Runs outside generation lock.
	virtual AgentRequestHandle *probe(
		const AgentProfile &profile,
		const QString &apiKey,
		QObject *parent = nullptr) = 0;

	virtual AgentModelListHandle *listModels(
		const AgentProfile &profile,
		const QString &apiKey,
		QObject *parent = nullptr) = 0;
};

} // namespace OpenMeeting
