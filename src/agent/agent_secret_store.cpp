#include "src/agent/agent_secret_store.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <wincrypt.h>
#else
#include <openssl/crypto.h>
#endif

#ifdef Q_OS_LINUX
#include "src/net/linux_secret_service.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QUuid>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#endif

namespace OpenMeeting {
namespace {

constexpr auto kSecretPrefix = "agent/secrets/";
constexpr auto kLinuxRecordPrefix = "agent/secretRecords/";
constexpr int kMaxPlaintextBytes = 64 * 1024;
constexpr int kMaxCiphertextBytes = 128 * 1024;

void wipeBytes(QByteArray &bytes) {
	if (!bytes.isEmpty()) {
#ifdef Q_OS_WIN
		SecureZeroMemory(bytes.data(), static_cast<SIZE_T>(bytes.size()));
#else
		OPENSSL_cleanse(bytes.data(), static_cast<size_t>(bytes.size()));
#endif
	}
	bytes.clear();
}

void wipeString(QString &text) {
	if (!text.isEmpty()) {
		const int len = text.size();
#ifdef Q_OS_WIN
		SecureZeroMemory(text.data(), static_cast<SIZE_T>(len) * sizeof(QChar));
#else
		OPENSSL_cleanse(text.data(), static_cast<size_t>(len) * sizeof(QChar));
#endif
	}
	text.clear();
}

QString dpapiSettingsKey(const QString &normalizedId) {
	return QLatin1String(kSecretPrefix) + normalizedId;
}

QString linuxRecordSettingsKey(const QString &normalizedId) {
	return QLatin1String(kLinuxRecordPrefix) + normalizedId;
}

#ifdef Q_OS_WIN
bool protectDpapiBytes(const QByteArray &input, QByteArray &output, bool encrypt) {
	DATA_BLOB source{
		static_cast<DWORD>(input.size()),
		reinterpret_cast<BYTE *>(const_cast<char *>(input.constData())),
	};
	DATA_BLOB result{};
	const BOOL ok = encrypt
		? CryptProtectData(
			&source,
			L"Cohavora Agent Key v1",
			nullptr,
			nullptr,
			nullptr,
			CRYPTPROTECT_UI_FORBIDDEN,
			&result)
		: CryptUnprotectData(
			&source,
			nullptr,
			nullptr,
			nullptr,
			nullptr,
			CRYPTPROTECT_UI_FORBIDDEN,
			&result);
	if (!ok) {
		return false;
	}
	const DWORD limit = static_cast<DWORD>(encrypt ? kMaxCiphertextBytes : kMaxPlaintextBytes);
	const bool bounded = result.cbData <= limit;
	if (bounded) {
		output = QByteArray(reinterpret_cast<const char *>(result.pbData), static_cast<int>(result.cbData));
	}
	if (result.pbData) {
		SecureZeroMemory(result.pbData, result.cbData);
		LocalFree(result.pbData);
	}
	return bounded;
}

class DpapiSlotBackend final : public AgentSecretStore::SlotBackend {
public:
	AgentSecretStore::Status saveSecret(
			const QString &normalizedId,
			const QByteArray &plainUtf8,
			QSettings &settings) override {
		QByteArray cipher;
		const bool encrypted = protectDpapiBytes(plainUtf8, cipher, true);
		if (!encrypted) {
			return AgentSecretStore::Status::StorageUnavailable;
		}
		settings.setValue(dpapiSettingsKey(normalizedId), QString::fromLatin1(cipher.toBase64()));
		wipeBytes(cipher);
		settings.sync();
		if (settings.status() != QSettings::NoError) {
			return AgentSecretStore::Status::SaveFailed;
		}
		return AgentSecretStore::Status::Ok;
	}

	AgentSecretStore::LoadResult loadSecret(
			const QString &normalizedId,
			QSettings &settings) override {
		const QString key = dpapiSettingsKey(normalizedId);
		if (!settings.contains(key)) {
			return { AgentSecretStore::Status::Empty, {} };
		}
		const QString encoded = settings.value(key).toString();
		if (encoded.isEmpty()) {
			return { AgentSecretStore::Status::Empty, {} };
		}
		const QByteArray cipher = QByteArray::fromBase64(encoded.toLatin1());
		if (cipher.isEmpty() || cipher.size() > kMaxCiphertextBytes) {
			return { AgentSecretStore::Status::Corrupted, {} };
		}
		QByteArray plain;
		if (!protectDpapiBytes(cipher, plain, false)) {
			return { AgentSecretStore::Status::Corrupted, {} };
		}
		QString result = QString::fromUtf8(plain);
		wipeBytes(plain);
		if (result.trimmed().isEmpty()) {
			wipeString(result);
			return { AgentSecretStore::Status::Corrupted, {} };
		}
		return { AgentSecretStore::Status::Ok, std::move(result) };
	}

	bool hasPersistedRecord(const QString &normalizedId, const QSettings &settings) const override {
		return !settings.value(dpapiSettingsKey(normalizedId)).toString().isEmpty();
	}

	AgentSecretStore::Status clearSecret(const QString &normalizedId, QSettings &settings) override {
		bool dirty = false;
		for (const QString &key : { dpapiSettingsKey(normalizedId), linuxRecordSettingsKey(normalizedId) }) {
			if (settings.contains(key)) {
				settings.remove(key);
				dirty = true;
			}
		}
		if (dirty) {
			settings.sync();
			if (settings.status() != QSettings::NoError) {
				return AgentSecretStore::Status::SaveFailed;
			}
		}
		return AgentSecretStore::Status::Ok;
	}
};
#elif defined(Q_OS_LINUX)
bool isValidUuidWithoutBraces(const QString &value) {
	const QUuid parsed(value);
	return !value.isEmpty() && !parsed.isNull() && parsed.toString(QUuid::WithoutBraces) == value;
}

class LinuxSecretServiceSlotBackend final : public AgentSecretStore::SlotBackend {
public:
	~LinuxSecretServiceSlotBackend() override = default;

	AgentSecretStore::Status saveSecret(
			const QString &normalizedId,
			const QByteArray &plainUtf8,
			QSettings &settings) override {
		QJsonObject payloadObj;
		payloadObj.insert(QStringLiteral("version"), 1);
		payloadObj.insert(QStringLiteral("id"), normalizedId);
		payloadObj.insert(QStringLiteral("apiKey"), QString::fromUtf8(plainUtf8));
		QByteArray payloadBytes = QJsonDocument(payloadObj).toJson(QJsonDocument::Compact);
		if (payloadBytes.isEmpty() || payloadBytes.size() > kMaxPlaintextBytes) {
			wipeBytes(payloadBytes);
			return AgentSecretStore::Status::SaveFailed;
		}
		auto secret = std::make_shared<SecretBytes>(std::move(payloadBytes));
		wipeBytes(payloadBytes);

		const QString pointerKey = linuxRecordSettingsKey(normalizedId);
		const QString oldUuid = settings.value(pointerKey).toString().trimmed();
		const QString nextUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);

		const SecretServiceResult saveRes = runBlocking(
			15000,
			[&](SecretServiceTransport &transport, SecretServiceTransport::Completion done) {
				return transport.save(nextUuid, secret, CredentialStore::Interaction::UserInitiated, std::move(done));
			});
		if (saveRes.status != CredentialStatus::Ready) {
			return (saveRes.status == CredentialStatus::SaveFailed)
				? AgentSecretStore::Status::SaveFailed
				: AgentSecretStore::Status::StorageUnavailable;
		}

		settings.setValue(pointerKey, nextUuid);
		settings.sync();
		if (settings.status() != QSettings::NoError) {
			if (auto *transport = ensureTransport()) {
				transport->erase({ nextUuid }, [](SecretServiceResult) {});
			}
			return AgentSecretStore::Status::SaveFailed;
		}

		if (isValidUuidWithoutBraces(oldUuid) && oldUuid != nextUuid) {
			if (auto *transport = ensureTransport()) {
				transport->erase({ oldUuid }, [](SecretServiceResult) {});
			}
		}
		return AgentSecretStore::Status::Ok;
	}

	AgentSecretStore::LoadResult loadSecret(
			const QString &normalizedId,
			QSettings &settings) override {
		const QString pointerKey = linuxRecordSettingsKey(normalizedId);
		const QString uuid = settings.value(pointerKey).toString().trimmed();
		if (uuid.isEmpty()) {
			return { AgentSecretStore::Status::Empty, {} };
		}
		if (!isValidUuidWithoutBraces(uuid)) {
			settings.remove(pointerKey);
			settings.sync();
			return { AgentSecretStore::Status::Corrupted, {} };
		}

		const SecretServiceResult res = runBlocking(
			2500,
			[&](SecretServiceTransport &transport, SecretServiceTransport::Completion done) {
				return transport.load(uuid, CredentialStore::Interaction::Background, std::move(done));
			});
		switch (res.status) {
		case CredentialStatus::Empty:
			settings.remove(pointerKey);
			settings.sync();
			return { AgentSecretStore::Status::Empty, {} };
		case CredentialStatus::InvalidRecord:
			if (auto *transport = ensureTransport()) {
				transport->erase({ uuid }, [](SecretServiceResult) {});
			}
			settings.remove(pointerKey);
			settings.sync();
			return { AgentSecretStore::Status::Corrupted, {} };
		case CredentialStatus::Ready:
			break;
		case CredentialStatus::Locked:
		case CredentialStatus::Denied:
		case CredentialStatus::TimedOut:
		case CredentialStatus::Busy:
		case CredentialStatus::Cancelled:
		case CredentialStatus::Unavailable:
		default:
			return { AgentSecretStore::Status::StorageUnavailable, {} };
		}

		if (!res.secret || res.secret->bytes().isEmpty()) {
			if (auto *transport = ensureTransport()) {
				transport->erase({ uuid }, [](SecretServiceResult) {});
			}
			settings.remove(pointerKey);
			settings.sync();
			return { AgentSecretStore::Status::Corrupted, {} };
		}

		QJsonParseError err{};
		const QJsonDocument doc = QJsonDocument::fromJson(res.secret->bytes(), &err);
		if (err.error != QJsonParseError::NoError || !doc.isObject()) {
			if (auto *transport = ensureTransport()) {
				transport->erase({ uuid }, [](SecretServiceResult) {});
			}
			settings.remove(pointerKey);
			settings.sync();
			return { AgentSecretStore::Status::Corrupted, {} };
		}
		const QJsonObject obj = doc.object();
		QString apiKey = obj.value(QStringLiteral("apiKey")).toString().trimmed();
		if (obj.value(QStringLiteral("version")).toInt() != 1 ||
			obj.value(QStringLiteral("id")).toString() != normalizedId ||
			apiKey.isEmpty()) {
			wipeString(apiKey);
			if (auto *transport = ensureTransport()) {
				transport->erase({ uuid }, [](SecretServiceResult) {});
			}
			settings.remove(pointerKey);
			settings.sync();
			return { AgentSecretStore::Status::Corrupted, {} };
		}
		return { AgentSecretStore::Status::Ok, std::move(apiKey) };
	}

	bool hasPersistedRecord(const QString &normalizedId, const QSettings &settings) const override {
		return isValidUuidWithoutBraces(settings.value(linuxRecordSettingsKey(normalizedId)).toString().trimmed());
	}

	AgentSecretStore::Status clearSecret(const QString &normalizedId, QSettings &settings) override {
		const QString pointerKey = linuxRecordSettingsKey(normalizedId);
		const QString uuid = settings.value(pointerKey).toString().trimmed();
		bool dirty = false;
		for (const QString &key : { pointerKey, dpapiSettingsKey(normalizedId) }) {
			if (settings.contains(key)) {
				settings.remove(key);
				dirty = true;
			}
		}
		if (dirty) {
			settings.sync();
		}
		if (isValidUuidWithoutBraces(uuid)) {
			runBlocking(
				2500,
				[&](SecretServiceTransport &transport, SecretServiceTransport::Completion done) {
					return transport.erase({ uuid }, std::move(done));
				});
		}
		if (dirty && settings.status() != QSettings::NoError) {
			return AgentSecretStore::Status::SaveFailed;
		}
		return AgentSecretStore::Status::Ok;
	}

private:
	SecretServiceTransport *ensureTransport() const {
		if (!_transport) {
			_transport = makeSecretServiceTransport(SecretServiceNamespace::Product);
		}
		return _transport.get();
	}

	template <typename SubmitFn>
	SecretServiceResult runBlocking(int timeoutMs, SubmitFn &&submit) const {
		struct WaitState {
			std::mutex mutex;
			std::condition_variable cv;
			bool done = false;
			SecretServiceResult result;
		};
		auto *transport = ensureTransport();
		if (!transport) {
			return {};
		}
		auto state = std::make_shared<WaitState>();
		const auto reqId = submit(*transport, [state](SecretServiceResult res) {
			{
				std::lock_guard lock(state->mutex);
				state->result = std::move(res);
				state->done = true;
			}
			state->cv.notify_one();
		});
		if (reqId == 0) {
			return {};
		}
		std::unique_lock lock(state->mutex);
		if (!state->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return state->done; })) {
			lock.unlock();
			transport->cancel(reqId);
			SecretServiceResult timedOut;
			timedOut.status = CredentialStatus::TimedOut;
			return timedOut;
		}
		return std::move(state->result);
	}

	mutable std::unique_ptr<SecretServiceTransport> _transport;
};
#else
class UnavailableSlotBackend final : public AgentSecretStore::SlotBackend {
public:
	AgentSecretStore::Status saveSecret(const QString &, const QByteArray &, QSettings &) override {
		return AgentSecretStore::Status::StorageUnavailable;
	}
	AgentSecretStore::LoadResult loadSecret(const QString &, QSettings &) override {
		return { AgentSecretStore::Status::Empty, {} };
	}
	bool hasPersistedRecord(const QString &, const QSettings &) const override {
		return false;
	}
	AgentSecretStore::Status clearSecret(const QString &, QSettings &) override {
		return AgentSecretStore::Status::Ok;
	}
};
#endif

std::unique_ptr<AgentSecretStore::SlotBackend> makeDefaultSlotBackend() {
#ifdef Q_OS_WIN
	return std::make_unique<DpapiSlotBackend>();
#elif defined(Q_OS_LINUX)
	return std::make_unique<LinuxSecretServiceSlotBackend>();
#else
	return std::make_unique<UnavailableSlotBackend>();
#endif
}

} // namespace

AgentSecretStore::AgentSecretStore(QSettings &settings, std::unique_ptr<SlotBackend> backend)
	: _settings(settings)
	, _backend(backend ? std::move(backend) : makeDefaultSlotBackend()) {
}

AgentSecretStore::~AgentSecretStore() {
	clearMemorySecrets();
}

QString AgentSecretStore::normalizeProfileId(const QString &profileId) {
	QString clean;
	clean.reserve(profileId.size());
	for (QChar ch : profileId) {
		if (ch.isLetterOrNumber() || ch == QLatin1Char('_') || ch == QLatin1Char('-')) {
			clean.append(ch);
		}
	}
	return clean.isEmpty() ? QStringLiteral("default") : clean;
}

QString AgentSecretStore::settingsKeyFor(const QString &profileId) {
	return dpapiSettingsKey(normalizeProfileId(profileId));
}

void AgentSecretStore::clearMemorySecrets() {
	for (auto &[id, secret] : _memorySecrets) {
		Q_UNUSED(id);
		wipeBytes(secret);
	}
	_memorySecrets.clear();
	_persistedIds.clear();
	_sessionOnlyIds.clear();
}

AgentSecretStore::Status AgentSecretStore::saveApiKey(
		const QString &profileId,
		const QString &apiKey,
		KeyStoragePolicy policy) {
	const QString normalizedId = normalizeProfileId(profileId);
	const QString trimmed = apiKey.trimmed();
	if (trimmed.isEmpty()) {
		return clearApiKey(normalizedId);
	}

	QByteArray utf8 = trimmed.toUtf8();
	if (utf8.size() > kMaxPlaintextBytes) {
		wipeBytes(utf8);
		return Status::SaveFailed;
	}

	if (auto it = _memorySecrets.find(normalizedId); it != _memorySecrets.end()) {
		wipeBytes(it->second);
	}
	_memorySecrets[normalizedId] = utf8;

	if (policy == KeyStoragePolicy::MemoryOnly) {
		_backend->clearSecret(normalizedId, _settings);
		_persistedIds.erase(normalizedId);
		_sessionOnlyIds.insert(normalizedId);
		wipeBytes(utf8);
		return Status::Ok;
	}

	const Status st = _backend->saveSecret(normalizedId, utf8, _settings);
	wipeBytes(utf8);
	if (st == Status::Ok) {
		_persistedIds.insert(normalizedId);
		_sessionOnlyIds.erase(normalizedId);
	} else {
		_persistedIds.erase(normalizedId);
		_sessionOnlyIds.insert(normalizedId);
	}
	return st;
}

AgentSecretStore::LoadResult AgentSecretStore::loadApiKey(
		const QString &profileId,
		KeyStoragePolicy policy) {
	const QString normalizedId = normalizeProfileId(profileId);
	if (const auto it = _memorySecrets.find(normalizedId); it != _memorySecrets.end() && !it->second.isEmpty()) {
		return { Status::Ok, QString::fromUtf8(it->second) };
	}

	if (policy == KeyStoragePolicy::MemoryOnly) {
		return { Status::Empty, {} };
	}

	LoadResult res = _backend->loadSecret(normalizedId, _settings);
	if (res.status == Status::Ok) {
		QByteArray utf8 = res.apiKey.toUtf8();
		_memorySecrets[normalizedId] = utf8;
		wipeBytes(utf8);
		_persistedIds.insert(normalizedId);
		_sessionOnlyIds.erase(normalizedId);
	} else if (res.status == Status::StorageUnavailable || res.status == Status::Corrupted || res.status == Status::Empty) {
		_persistedIds.erase(normalizedId);
	}
	return res;
}

bool AgentSecretStore::hasApiKey(const QString &profileId, KeyStoragePolicy policy) const {
	const QString normalizedId = normalizeProfileId(profileId);
	if (const auto it = _memorySecrets.find(normalizedId); it != _memorySecrets.end() && !it->second.isEmpty()) {
		return true;
	}
	if (policy == KeyStoragePolicy::MemoryOnly) {
		return false;
	}
	return _backend->hasPersistedRecord(normalizedId, _settings);
}

bool AgentSecretStore::isPersistedOnDisk(const QString &profileId, KeyStoragePolicy policy) const {
	if (policy == KeyStoragePolicy::MemoryOnly) {
		return false;
	}
	const QString normalizedId = normalizeProfileId(profileId);
	if (_sessionOnlyIds.find(normalizedId) != _sessionOnlyIds.end()) {
		return false;
	}
	if (_persistedIds.find(normalizedId) != _persistedIds.end()) {
		return true;
	}
	return _backend->hasPersistedRecord(normalizedId, _settings);
}

AgentSecretStore::Status AgentSecretStore::clearApiKey(const QString &profileId) {
	const QString normalizedId = normalizeProfileId(profileId);
	if (const auto it = _memorySecrets.find(normalizedId); it != _memorySecrets.end()) {
		wipeBytes(it->second);
		_memorySecrets.erase(it);
	}
	_persistedIds.erase(normalizedId);
	_sessionOnlyIds.erase(normalizedId);
	return _backend->clearSecret(normalizedId, _settings);
}

} // namespace OpenMeeting
