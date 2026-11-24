#pragma once

#include "src/agent/agent_types.h"

#include <QtCore/QByteArray>
#include <QtCore/QHash>
#include <QtCore/QSettings>
#include <QtCore/QString>

#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace OpenMeeting {

// Independent secret store for custom Agent API keys.
// Kept intentionally isolated from StoredSession / auth logout tombstones so
// account switching or expired user sessions do not unexpectedly erase the
// user's configured Agent credentials unless explicitly cleared.
class AgentSecretStore final {
public:
	enum class Status {
		Ok,
		Empty,
		SaveFailed,
		StorageUnavailable,
		Corrupted,
	};

	struct LoadResult {
		Status status = Status::Empty;
		QString apiKey;
	};

	struct SlotBackend {
		virtual ~SlotBackend() = default;
		virtual Status saveSecret(const QString &normalizedId, const QByteArray &plainUtf8, QSettings &settings) = 0;
		virtual LoadResult loadSecret(const QString &normalizedId, QSettings &settings) = 0;
		virtual bool hasPersistedRecord(const QString &normalizedId, const QSettings &settings) const = 0;
		virtual Status clearSecret(const QString &normalizedId, QSettings &settings) = 0;
	};

	explicit AgentSecretStore(QSettings &settings, std::unique_ptr<SlotBackend> backend = nullptr);
	~AgentSecretStore();

	AgentSecretStore(const AgentSecretStore &) = delete;
	AgentSecretStore &operator=(const AgentSecretStore &) = delete;

	Status saveApiKey(const QString &profileId, const QString &apiKey, KeyStoragePolicy policy);
	[[nodiscard]] LoadResult loadApiKey(const QString &profileId, KeyStoragePolicy policy);
	[[nodiscard]] bool hasApiKey(const QString &profileId, KeyStoragePolicy policy) const;
	[[nodiscard]] bool isPersistedOnDisk(const QString &profileId, KeyStoragePolicy policy) const;
	Status clearApiKey(const QString &profileId);
	void clearMemorySecrets();

private:
	[[nodiscard]] static QString normalizeProfileId(const QString &profileId);
	[[nodiscard]] static QString settingsKeyFor(const QString &profileId);

	QSettings &_settings;
	std::unique_ptr<SlotBackend> _backend;
	std::unordered_map<QString, QByteArray> _memorySecrets;
	std::unordered_set<QString> _persistedIds;
	std::unordered_set<QString> _sessionOnlyIds;
};

} // namespace OpenMeeting
