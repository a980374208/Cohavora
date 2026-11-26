#pragma once

#include "src/agent/agent_secret_store.h"
#include "src/agent/agent_session_store.h"
#include "src/agent/agent_speech_service.h"
#include "src/agent/agent_transport.h"
#include "src/agent/agent_types.h"
#include "src/agent/meeting_tool_registry.h"
#include "src/agent/streaming_asr_session.h"

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtCore/QVariantMap>
#include <QtCore/QVector>

#include <memory>
#include <future>
#include <optional>

namespace OpenMeeting {

class CallInviteController;
class ContactCatalogController;
class MeetingCatalogController;
class MeetingCoordinator;
class PresenceWatchController;
class SessionManager;

struct AgentSpeechProbeConfig {
	AgentProfile chatProfile;
	std::optional<AgentSpeechSourceMode> sourceModeOverride;
	QString speechProfileId;
	QString sttBaseUrl;
	AgentSpeechProtocolMode protocolMode = AgentSpeechProtocolMode::Auto;
	QString sttModel;
	QString language;
	QString prompt;
	QString draftSttApiKey;
	QString draftChatApiKey;
	QString draftAppKey;
	QString resourceId;
	QString workspaceId;
};

class AgentCoordinator final : public QObject {
	Q_OBJECT
public:
	explicit AgentCoordinator(
		SessionManager &sessionManager,
		std::unique_ptr<QSettings> settings = nullptr,
		std::unique_ptr<IAgentTransport> transport = nullptr,
		QObject *parent = nullptr);
	~AgentCoordinator() override;

	[[nodiscard]] const AgentProfile &profile() const { return _profile; }
	void setProfile(const AgentProfile &profile);
	[[nodiscard]] const QVector<AgentProfile> &profiles() const { return _profiles; }
	[[nodiscard]] QString activeProfileId() const { return _profile.id; }
	[[nodiscard]] std::optional<AgentProfile> findProfile(const QString &profileId) const;
	QString createProfile(const QString &displayName = QString(), const AgentProfile *templateProfile = nullptr);
	bool switchProfile(const QString &profileId);
	bool deleteProfile(const QString &profileId);
	[[nodiscard]] bool isTranslationTargetConfigured(
		const QString &profileIdOverride,
		const QString &modelOverride) const;
	[[nodiscard]] bool resolveTranslationChatTarget(
		const QString &profileIdOverride,
		const QString &modelOverride,
		AgentProfile *outProfile,
		QString *outApiKey,
		QString *outError = nullptr);

	[[nodiscard]] bool isConfigured() const;
	[[nodiscard]] bool hasApiKey() const;
	[[nodiscard]] bool isApiKeyPersisted() const;
	[[nodiscard]] AgentSecretStore::Status lastSecretStatus() const { return _lastSecretStatus; }
	AgentSecretStore::Status saveApiKey(const QString &apiKey);
	AgentSecretStore::Status clearApiKey();

	void attachMeeting(MeetingCoordinator *coordinator);
	[[nodiscard]] MeetingCoordinator *attachedMeeting() const;
	void attachCatalog(MeetingCatalogController *catalog);
	[[nodiscard]] MeetingCatalogController *attachedCatalog() const;
	void attachContacts(ContactCatalogController *contacts,
	                    PresenceWatchController *presence = nullptr,
	                    CallInviteController *callInvite = nullptr);
	void setHostSettingsBridge(AgentHostSettingsBridge bridge);
	[[nodiscard]] bool isInMeeting() const;
	[[nodiscard]] bool isEncryptedMeeting() const;
	[[nodiscard]] bool isE2eeContextAuthorized() const;
	void authorizeE2eeContextForCurrentMeeting(bool authorized);

	[[nodiscard]] bool isContextSendingEnabled() const { return _sendMeetingContext; }
	void setContextSendingEnabled(bool enabled);
	[[nodiscard]] AgentChatMode chatMode() const { return _chatMode; }
	void setChatMode(AgentChatMode mode);
	[[nodiscard]] AgentPermissionMode permissionMode() const { return _permissionMode; }
	void setPermissionMode(AgentPermissionMode mode);
	[[nodiscard]] const QStringList &cachedModels() const { return _cachedModels; }
	[[nodiscard]] const QStringList &cachedSpeechModels() const { return _cachedSpeechModels; }
	[[nodiscard]] const QHash<QString, AgentModelCapabilities> &cachedModelCapabilities() const {
		return _modelCapabilities;
	}
	[[nodiscard]] QStringList cachedExternalSpeechModelsForUrl(const QString &sttBaseUrl) const;
	[[nodiscard]] QHash<QString, AgentModelCapabilities> cachedExternalSpeechCapabilitiesForUrl(
		const QString &sttBaseUrl) const;
	[[nodiscard]] QString resolvedSpeechModel() const;
	[[nodiscard]] static bool isSameAgentOrigin(const QString &baseUrlA, const QString &baseUrlB);
	[[nodiscard]] static bool isSameSpeechOrigin(const QString &urlA, const QString &urlB);
	[[nodiscard]] static QString canonicalSpeechEndpointKey(const QString &baseUrl);
	[[nodiscard]] static QStringList suggestedSpeechModelsForEndpoint(
		const QString &sttBaseUrl,
		const QString &chatBaseUrl,
		const QStringList &chatCachedSpeechModels,
		const QStringList &externalCachedSpeechModels = {},
		AgentSpeechProtocolMode protocolMode = AgentSpeechProtocolMode::Auto);
	[[nodiscard]] static QString resolveDefaultSpeechModelForEndpoint(
		const QString &sttBaseUrl,
		const QString &chatBaseUrl,
		const QStringList &chatCachedSpeechModels,
		const QHash<QString, AgentModelCapabilities> &modelCaps = {},
		const QStringList &externalCachedSpeechModels = {},
		const QHash<QString, AgentModelCapabilities> &externalModelCaps = {},
		AgentSpeechProtocolMode protocolMode = AgentSpeechProtocolMode::Auto);
	bool switchModel(const QString &modelId);

	[[nodiscard]] AgentSpeechSourceMode speechSourceMode() const { return _speechSourceMode; }
	void setSpeechSourceMode(AgentSpeechSourceMode mode);
	[[nodiscard]] const AgentSpeechProfile &speechProfile() const { return _speechProfile; }
	void setSpeechProfile(const AgentSpeechProfile &profile);
	[[nodiscard]] const QVector<AgentSpeechProfile> &speechProfiles() const { return _speechProfiles; }
	[[nodiscard]] QString activeSpeechProfileId() const { return _speechProfile.id; }
	[[nodiscard]] std::optional<AgentSpeechProfile> findSpeechProfile(const QString &profileId) const;
	QString createSpeechProfile(
		const QString &displayName = QString(),
		const AgentSpeechProfile *templateProfile = nullptr);
	bool switchSpeechProfile(const QString &profileId);
	bool deleteSpeechProfile(const QString &profileId);
	[[nodiscard]] AgentSpeechProtocolMode followChatSpeechProtocol() const { return _followChatSpeechProtocol; }
	[[nodiscard]] QString followChatSpeechModel() const { return _followChatSpeechModel; }

	[[nodiscard]] QString dictationSttBaseUrl() const;
	void setDictationSttBaseUrl(const QString &baseUrl);
	[[nodiscard]] AgentSpeechProtocolMode dictationSpeechProtocol() const;
	void setDictationSpeechProtocol(AgentSpeechProtocolMode mode);
	[[nodiscard]] AgentSpeechProtocolMode effectiveDictationSpeechProtocol() const;
	[[nodiscard]] QString dictationSttModel() const;
	void setDictationSttModel(const QString &model);
	[[nodiscard]] QString dictationLanguage() const { return _dictationLanguage; }
	void setDictationLanguage(const QString &language);
	[[nodiscard]] QString dictationPrompt() const { return _dictationPrompt; }
	void setDictationPrompt(const QString &prompt);
	[[nodiscard]] int dictationSpeechTimeoutMs() const { return _dictationSpeechTimeoutMs; }
	void setDictationSpeechTimeoutMs(int timeoutMs);
	[[nodiscard]] int dictationVadThresholdDb() const { return _dictationVadThresholdDb; }
	void setDictationVadThresholdDb(int thresholdDb);
	[[nodiscard]] int dictationVadSilenceCommitMs() const { return _dictationVadSilenceCommitMs; }
	void setDictationVadSilenceCommitMs(int silenceMs);
	[[nodiscard]] QString dictationDeviceId() const { return _dictationDeviceId; }
	void setDictationDeviceId(const QString &deviceId);
	[[nodiscard]] QString dictationSttResourceId() const;
	void setDictationSttResourceId(const QString &resourceId);
	[[nodiscard]] QString dictationSttWorkspace() const;
	void setDictationSttWorkspace(const QString &workspace);
	[[nodiscard]] bool hasDictationApiKey() const;
	[[nodiscard]] bool isDictationApiKeyPersisted() const;
	[[nodiscard]] bool hasDictationAppKey() const;
	[[nodiscard]] bool isDictationAppKeyPersisted() const;
	[[nodiscard]] AgentKeyEndpointMismatch chatKeyEndpointCompatibility(
		const QString &baseUrlOverride = QString(),
		const QString &draftApiKey = QString());
	[[nodiscard]] AgentKeyEndpointMismatch speechKeyEndpointCompatibility(
		const QString &sttBaseUrlOverride = QString(),
		const QString &chatBaseUrlOverride = QString(),
		const QString &draftSttApiKey = QString());
	AgentSecretStore::Status saveDictationApiKey(const QString &apiKey);
	AgentSecretStore::Status clearDictationApiKey();
	AgentSecretStore::Status saveDictationAppKey(const QString &appKey);
	AgentSecretStore::Status clearDictationAppKey();
	[[nodiscard]] QString loadApiKeyForSpeech(QString *outError = nullptr);
	[[nodiscard]] QString loadAppKeyForSpeech(QString *outError = nullptr);
	[[nodiscard]] StreamingAsrConfig buildStreamingAsrConfig(QString *outError = nullptr);
	[[nodiscard]] QString loadApiKeyForChat(QString *outError = nullptr);
	[[nodiscard]] AgentMeetingContext buildCurrentMeetingContext() const;
	[[nodiscard]] QString buildExplicitContextBlock(const QVector<AgentContextChip> &chips) const;

	[[nodiscard]] const QVector<AgentChatMessage> &messages() const { return _messages; }
	[[nodiscard]] QString activeSessionId() const { return _activeSessionId; }
	[[nodiscard]] QVector<AgentChatSessionSummary> sessionSummaries() const;
	QString createNewSession();
	bool switchSession(const QString &sessionId);
	bool deleteSession(const QString &sessionId);
	bool renameSession(const QString &sessionId, const QString &newTitle);
	bool clearAllSavedSessions();
	enum class HistoryError { None, Save, Remove };
	HistoryError historyError() const { return _historyError; }
	void retryHistoryStorage();
	[[nodiscard]] QString exportSessionMarkdown(const QString &sessionId = QString()) const;

	[[nodiscard]] const QVector<AgentPendingToolAction> &pendingActions() const { return _pendingActions; }
	[[nodiscard]] const MeetingToolRegistry &toolRegistry() const { return _toolRegistry; }
	[[nodiscard]] bool isStreaming() const { return _activeHandle != nullptr; }
	[[nodiscard]] bool isProbing() const { return _probeHandle != nullptr; }
	[[nodiscard]] bool isSpeechProbing() const {
		return _speechProbePending || _speechProbeHandle != nullptr || _speechProbeStreamSession != nullptr;
	}
	[[nodiscard]] bool isFetchingModels() const { return _modelsHandle != nullptr; }
	[[nodiscard]] bool isFetchingSpeechModels() const { return _sttModelsHandle != nullptr; }
	[[nodiscard]] bool canRetryLastTurn() const;

	void sendUserMessage(
		const QString &text,
		const QVector<AgentImageAttachment> &images = {},
		const QVector<AgentContextChip> &contextChips = {});
	bool retryLastTurn();
	bool regenerateFromAssistantMessage(const QString &assistantMessageId);
	bool editAndResendUserMessage(
		const QString &userMessageId,
		const QString &newText,
		const QVector<AgentContextChip> *overrideChips = nullptr);
	void stopActiveRequest();
	void clearHistory();

	bool approvePendingAction(const QString &actionId);
	bool rejectPendingAction(const QString &actionId);
	void recordMeetingChatMessage(const QString &senderName, const QString &message, bool isLocal = false);
	void recordMeetingTranscript(const AgentMeetingTranscriptEntry &entry);
	[[nodiscard]] const QVector<AgentMeetingTranscriptEntry> &recentTranscripts() const { return _recentTranscripts; }
	void updateTelemetrySnapshot(const QVariantMap &snapshot);

	void setSpeechTransport(std::unique_ptr<IAgentSpeechTransport> transport);
	void setSpeechStreamFactory(StreamingAsrFactory factory);
	void testConnection(const AgentProfile &draftProfile, const QString &draftApiKey = QString());
	void cancelProbe();
	void testSpeechConnection(const AgentSpeechProbeConfig &config);
	void cancelSpeechProbe();
	void fetchModels(const AgentProfile &draftProfile, const QString &draftApiKey = QString());
	void fetchSpeechModels(
		const QString &sttBaseUrl,
		const QString &draftSttApiKey = QString(),
		const QString &chatBaseUrl = QString());
	[[nodiscard]] AgentModelCapabilities resolveModelCapabilities(const QString &modelId) const;
	[[nodiscard]] AgentModelCapabilities resolveSpeechModelCapabilities(const QString &modelId) const;
	[[nodiscard]] int lastFilteredNonChatModelCount() const { return _lastFilteredNonChatModelCount; }

	[[nodiscard]] SessionGenerationToken currentGenerationToken() const;
	[[nodiscard]] bool isGenerationCurrent(const SessionGenerationToken &token) const;

signals:
	void profileChanged();
	void profilesChanged();
	void speechProfilesChanged();
	void messageAdded(const OpenMeeting::AgentChatMessage &message);
	void messageUpdated(const OpenMeeting::AgentChatMessage &message);
	void messageRemoved(const QString &messageId);
	void historyCleared();
	void streamingStateChanged(bool streaming);
	void requestFailed(const QString &safeError);
	void probeFinished(bool success, const QString &message);
	void speechProbeFinished(bool success, const QString &message);
	void modelsFetched(bool success, const QStringList &models, const QString &safeError);
	void speechModelsFetched(bool success, const QStringList &speechModels, const QString &safeError);
	void contextStateChanged();
	void chatModeChanged(OpenMeeting::AgentChatMode mode);
	void permissionModeChanged(OpenMeeting::AgentPermissionMode mode);
	void sessionsChanged();
	void historyStorageChanged();
	void pendingActionAdded(const OpenMeeting::AgentPendingToolAction &action);
	void pendingActionUpdated(const OpenMeeting::AgentPendingToolAction &action);

private:
	struct PendingToolRoundState {
		uint64_t turnNonce = 0;
		SessionGenerationToken token;
		QString apiKey;
		int toolRound = 0;
		bool hasReadOnlyResults = false;
		bool hadFailureOrRejection = false;
		bool hadApprovedAction = false;
		bool allApprovedHaveLocalSummary = true;
		bool hasMeetingTransitionSuccess = false;
		QSet<QString> pendingActionIds;
		QStringList localSummaries;

		void reset() {
			turnNonce = 0;
			token = SessionGenerationToken{};
			apiKey.clear();
			toolRound = 0;
			hasReadOnlyResults = false;
			hadFailureOrRejection = false;
			hadApprovedAction = false;
			allApprovedHaveLocalSummary = true;
			hasMeetingTransitionSuccess = false;
			pendingActionIds.clear();
			localSummaries.clear();
		}
	};

	void loadProfileFromSettings();
	void saveProfileToSettings() const;
	void saveSpeechProfilesToSettings() const;
	void resetExternalSpeechModelCacheIfNeeded(const QString &nextSttBaseUrl);
	void promoteFollowChatToCustomIfNeeded();
	[[nodiscard]] QString dictationSecretId() const;
	[[nodiscard]] QString dictationAppKeySecretId() const;
	AgentSecretStore::Status migrateSecretStoragePolicy(
		const QString &previousId,
		const QString &nextId,
		KeyStoragePolicy previousPolicy,
		KeyStoragePolicy nextPolicy);
	[[nodiscard]] AgentToolViewContext buildToolViewContext() const;
	void trimStoredMessagesToLimit();
	[[nodiscard]] QVector<AgentChatMessage> buildContextMessages(
		int maxContextMessages,
		int toolRound,
		QVector<bool> *outFollowedByFailedTurn = nullptr) const;
	void dispatchChatRequest(const SessionGenerationToken &token, const QString &apiKey, int toolRound);
	void handleCompletedToolCalls(const SessionGenerationToken &token, const QString &apiKey, int toolRound, const QVector<AgentToolCall> &toolCalls);
	void resolvePendingActionInRound(
		const QString &actionId,
		bool approved,
		bool succeeded,
		bool isMeetingTransitionSuccess,
		const QString &localSummaryMessage);
	void expireAllPendingActions(const QString &reason, bool meetingScopedOnly = false);
	void onCatalogWriteStateChanged();
	void finalizeCatalogAction(AgentPendingToolAction &action);
	bool finalizeActionOutcome(const QString &actionId, const ToolExecutionOutcome &outcome);
	void updateToolMessageForAction(const QString &actionId, const QString &statusJson);
	void finalizeThinkingForMessage(AgentChatMessage &message);
	void updateToolInvocationPartForAction(
		const QString &actionId,
		AgentResponsePart::ToolState state,
		const QString &pastTenseMessage,
		const QString &resultSummary);
	[[nodiscard]] QVector<AgentFollowupSuggestion> generateFollowupSuggestions(
		int assistantMessageIndex) const;
	void flushPendingDeltas();
	void resetStreamingBuffers();
	void cancelActiveRequestWithReason(AgentChatMessage::Status terminalStatus, const QString &reason);
	void onMeetingEnded();
	void onAuthSessionChanged();
	void loadAccountSessionsFromDisk();
	void syncActiveSessionSnapshot(bool updateTimestamp = true) const;
	void schedulePersistSessions();
	void flushPersistSessions();
	void finishHistoryWrite(quint64 revision);
	void drainHistoryWrites();
	bool removeSavedHistory();
	void historyPolicyChanged(bool previousPersist);
	void setHistoryError(HistoryError error);
	int indexOfSession(const QString &sessionId) const;
	[[nodiscard]] bool validateE2eeForAttachments(
		const QVector<AgentImageAttachment> &images,
		const QVector<AgentContextChip> &chips);
	bool restartFromUserIndex(
		int userIndex,
		const QString *editedText,
		const QVector<AgentContextChip> *editedChips);
	int indexOfMessage(const QString &id) const;
	int indexOfPendingAction(const QString &actionId) const;

	SessionManager &_sessionManager;
	std::unique_ptr<QSettings> _settings;
	AgentSecretStore _secretStore;
	AgentSessionStore _sessionStore;
	std::unique_ptr<IAgentTransport> _transport;
	std::unique_ptr<IAgentSpeechTransport> _speechTransport;

	AgentProfile _profile;
	QVector<AgentProfile> _profiles;
	AgentSecretStore::Status _lastSecretStatus = AgentSecretStore::Status::Empty;
	QString _currentAccountKey;
	QString _activeSessionId;
	mutable QVector<AgentChatSessionData> _sessions;
	QTimer _persistTimer;
	std::future<bool> _historyWrite;
	quint64 _historyRevision = 0;
	bool _historyWriteQueued = false;
	HistoryError _historyError = HistoryError::None;
	// Retain a failed account's latest intent until it is selected and can retry.
	struct UnsavedHistory {
		AgentSessionStore::LoadedSessions snapshot;
		HistoryError error = HistoryError::None;
	};
	QHash<QString, UnsavedHistory> _unsavedHistory;
	QPointer<MeetingCoordinator> _meeting;
	QVector<QMetaObject::Connection> _meetingConnections;
	QPointer<MeetingCatalogController> _catalog;
	QVector<QMetaObject::Connection> _catalogConnections;
	QPointer<ContactCatalogController> _contacts;
	QPointer<PresenceWatchController> _presence;
	QPointer<CallInviteController> _callInvite;
	QVector<QMetaObject::Connection> _contactConnections;
	AgentHostSettingsBridge _hostSettings;
	QHash<int, QString> _inFlightActionByGroup;

	bool _sendMeetingContext = false;
	AgentChatMode _chatMode = AgentChatMode::Agent;
	AgentPermissionMode _permissionMode = AgentPermissionMode::Manual;
	bool _e2eeAuthorized = false;
	QString _e2eeAuthorizedMeetingId;
	uint64_t _e2eeAuthorizedSession = 0;

	QVector<AgentChatMessage> _messages;
	QVector<AgentPendingToolAction> _pendingActions;
	QVector<AgentMeetingChatEntry> _recentMeetingChat;
	QVector<AgentMeetingTranscriptEntry> _recentTranscripts;
	QVariantMap _latestTelemetrySnapshot;
	MeetingToolRegistry _toolRegistry;
	QPointer<AgentRequestHandle> _activeHandle;
	QPointer<AgentRequestHandle> _probeHandle;
	QPointer<AgentSpeechRequestHandle> _speechProbeHandle;
	std::unique_ptr<IStreamingAsrSession> _speechProbeStreamSession;
	QTimer _speechProbeTimer;
	StreamingAsrFactory _speechStreamFactory;
	uint64_t _speechProbeNonce = 0;
	bool _speechProbePending = false;
	QPointer<AgentModelListHandle> _modelsHandle;
	QPointer<AgentModelListHandle> _sttModelsHandle;
	QStringList _cachedModels;
	QStringList _cachedSpeechModels;
	QHash<QString, AgentModelCapabilities> _modelCapabilities;
	QString _cachedExternalSpeechKey;
	QStringList _cachedExternalSpeechModels;
	QHash<QString, AgentModelCapabilities> _externalSttModelCapabilities;
	int _lastFilteredNonChatModelCount = 0;
	AgentSpeechSourceMode _speechSourceMode = AgentSpeechSourceMode::FollowChat;
	AgentSpeechProtocolMode _followChatSpeechProtocol = AgentSpeechProtocolMode::Auto;
	QString _followChatSpeechModel;
	QVector<AgentSpeechProfile> _speechProfiles;
	AgentSpeechProfile _speechProfile;
	QString _dictationLanguage;
	QString _dictationPrompt;
	int _dictationSpeechTimeoutMs = 0;
	int _dictationVadThresholdDb = -42;
	int _dictationVadSilenceCommitMs = 600;
	QString _dictationDeviceId;
	QString _streamingMessageId;
	QString _pendingDeltaBuffer;
	QString _pendingReasoningBuffer;
	qint64 _thinkingStartMs = 0;
	QVector<AgentToolCall> _pendingToolCalls;
	QVector<AgentThinkingBlock> _pendingThinkingBlocks;
	QTimer _deltaFlushTimer;

	uint64_t _meetingAttachmentGeneration = 0;
	uint64_t _requestNonce = 0;
	uint64_t _conversationNonce = 0;
	SessionGenerationToken _activeRequestToken;
	PendingToolRoundState _activeToolRound;
};

} // namespace OpenMeeting
