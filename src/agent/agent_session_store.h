#pragma once

#include "src/agent/agent_types.h"

#include <QtCore/QSettings>
#include <QtCore/QString>
#include <QtCore/QVector>

namespace OpenMeeting {

class AgentSessionStore final {
public:
	static constexpr int kMaxPersistedSessions = 20;
	static constexpr int kMaxMessagesPerSession = 100;

	struct LoadedSessions {
		QString activeSessionId;
		QVector<AgentChatSessionData> sessions;
	};

	struct SaveTimings {
		qint64 preparationNs = 0;
		qint64 serializationNs = 0;
		qint64 commitNs = 0;
	};

	explicit AgentSessionStore(QString baseDirectory = {});

	[[nodiscard]] static QString resolveBaseDirectory(const QSettings &settings);
	[[nodiscard]] static QString accountScopeKey(bool loggedIn, const QString &userId, const QString &serviceUrl = {});
	[[nodiscard]] static QString deriveSessionTitle(const QVector<AgentChatMessage> &messages);
	[[nodiscard]] static QString exportSessionMarkdown(const AgentChatSessionData &session);

	[[nodiscard]] const QString &baseDirectory() const { return _baseDirectory; }

	[[nodiscard]] LoadedSessions loadAccountSessions(const QString &accountKey) const;
	bool saveAccountSessions(
		const QString &accountKey,
		const QString &activeSessionId,
		const QVector<AgentChatSessionData> &sessions, SaveTimings *timings = nullptr) const;
	bool clearAccountSessions(const QString &accountKey) const;

private:
	[[nodiscard]] QString sessionFilePath(const QString &accountKey) const;

	QString _baseDirectory;
};

} // namespace OpenMeeting
