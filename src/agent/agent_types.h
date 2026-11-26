#pragma once

#include "src/net/service_endpoint_policy.h"

#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QJsonObject>
#include <QtCore/QMap>
#include <QtCore/QMetaType>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtCore/QVector>

#include <cstdint>
#include <string>

namespace OpenMeeting {

enum class AgentProtocolDialect {
	OpenAiChatCompletionsSse = 0,
	OpenAiResponsesSse = 1,
	CustomWebhookJson = 2,
	AnthropicMessagesSse = 3,
	GoogleGeminiSse = 4,
};

enum class AgentSpeechProtocolMode {
	Auto = 0,
	OpenAiTranscriptions = 1,
	ChatInputAudio = 2,
	DashScopeMultimodal = 3,
	OpenAiRealtimeWs = 4,
	DashScopeInferenceWs = 5,
	DashScopeQwenRealtimeWs = 6,
	DeepgramWs = 7,
	AssemblyAiV3Ws = 8,
	VolcengineBigModelWs = 9,
	ElevenLabsScribeWs = 10,
	XaiSttWs = 11,
};

enum class AgentSpeechSourceMode {
	FollowChat = 0,
	CustomProfile = 1,
};

enum class KeyStoragePolicy {
	MemoryOnly = 0,
	EncryptedStore = 1,
};

enum class AgentReasoningEffort {
	Default = 0,
	Low = 1,
	Medium = 2,
	High = 3,
	Extra = 4,
};

enum class AgentChatMode {
	Agent = 0,
	Ask = 1,
};

enum class AgentPermissionMode {
	Manual = 0,
	ConfirmAll = 1,
};

enum class AgentContextChipKind {
	MeetingChat = 0,
	Telemetry = 1,
	Participants = 2,
	Schedule = 3,
	Transcript = 4,
};

struct AgentContextChip {
	AgentContextChipKind kind = AgentContextChipKind::MeetingChat;
	QString label;

	bool operator==(const AgentContextChip &other) const = default;
};

struct AgentFollowupSuggestion {
	QString label;
	QString prompt;

	bool operator==(const AgentFollowupSuggestion &other) const = default;
};

struct AgentResponsePart {
	enum class Type {
		Thinking = 0,
		Markdown = 1,
		ToolInvocation = 2,
	};

	enum class ToolState {
		Running = 0,
		WaitingForConfirmation = 1,
		Completed = 2,
		Failed = 3,
		Rejected = 4,
	};

	Type type = Type::Markdown;
	QString text;
	bool active = false;
	qint64 durationMs = 0;

	QString toolCallId;
	QString toolName;
	QString invocationMessage;
	QString pastTenseMessage;
	QString argumentsJson;
	QString resultSummary;
	QString actionId;
	bool isReadOnly = true;
	ToolState toolState = ToolState::Running;

	bool operator==(const AgentResponsePart &other) const = default;
};

enum class AgentModelCapabilitySource {
	None = 0,
	RuleInferred = 1,
	ServerMetadata = 2,
};

struct AgentModelCapabilities {
	QString modelId;
	bool supportsReasoningEffort = false;
	QVector<AgentReasoningEffort> supportedEfforts;
	AgentModelCapabilitySource source = AgentModelCapabilitySource::None;
	bool supportsVision = false;
	AgentModelCapabilitySource visionSource = AgentModelCapabilitySource::None;
	int contextLengthTokens = 0;
	AgentModelCapabilitySource contextLengthSource = AgentModelCapabilitySource::None;
	QStringList inputModalities;
	QStringList outputModalities;
	AgentModelCapabilitySource modalitySource = AgentModelCapabilitySource::None;

	bool operator==(const AgentModelCapabilities &other) const = default;
};

struct AgentEndpointDecision {
	bool allowed = false;
	bool loopbackPlaintext = false;
	QUrl normalizedUrl;
	QString reason;
};

struct AgentProfile {
	QString id = QStringLiteral("default");
	QString name = QStringLiteral("Custom Agent");
	AgentProtocolDialect protocol = AgentProtocolDialect::OpenAiChatCompletionsSse;
	QString baseUrl;
	QString model;
	QString systemPrompt;
	QMap<QString, QString> customHeaders;
	double temperature = 0.7;
	int maxContextMessages = 16;
	int connectTimeoutMs = 10000;
	int idleTimeoutMs = 30000;
	KeyStoragePolicy keyStorage = KeyStoragePolicy::EncryptedStore;
	bool sendMeetingContextByDefault = false;
	bool persistChatHistory = true;
	AgentReasoningEffort reasoningEffort = AgentReasoningEffort::Default;

	[[nodiscard]] bool hasSameConnectionConfig(const AgentProfile &other) const {
		return id == other.id &&
			protocol == other.protocol &&
			baseUrl == other.baseUrl &&
			model == other.model &&
			customHeaders == other.customHeaders &&
			keyStorage == other.keyStorage;
	}

	bool operator==(const AgentProfile &other) const = default;
};

struct AgentSpeechProfile {
	QString id = QStringLiteral("default_stt");
	QString name = QStringLiteral("Default Speech");
	AgentSpeechProtocolMode protocol = AgentSpeechProtocolMode::Auto;
	QString baseUrl;
	QString model;
	QString resourceId = QStringLiteral("volc.seedasr.sauc.duration");
	QString workspaceId;

	[[nodiscard]] bool hasSameConnectionConfig(const AgentSpeechProfile &other) const {
		return id == other.id &&
			protocol == other.protocol &&
			baseUrl == other.baseUrl &&
			model == other.model &&
			resourceId == other.resourceId &&
			workspaceId == other.workspaceId;
	}

	bool operator==(const AgentSpeechProfile &other) const = default;
};

struct AgentMeetingContext {
	QString meetingId;
	QString meetingName;
	bool inMeeting = false;
	bool encryptedMeeting = false;
	bool e2eeContextAuthorized = false;
	int participantCount = 0;
	bool micMuted = false;
	bool cameraEnabled = false;
	bool screenSharing = false;
	QStringList recentSafeSummaryLines;
};

struct SessionGenerationToken {
	quint64 authGeneration = 0;
	uint64_t coordinatorSession = 0;
	uint64_t nativeRoomGeneration = 0;

	bool operator==(const SessionGenerationToken &other) const = default;
};

enum class AgentToolRisk {
	ReadOnly = 0,
	RequiresConfirmation = 1,
};

enum class AgentToolScope {
	ClientGlobal = 0,
	AccountCatalog = 1,
	ActiveMeeting = 2,
	MeetingHost = 3,
	AccountContacts = 4,
};

enum class AgentConcurrencyGroup {
	None = 0,
	CatalogWrite = 1,
	MeetingTransition = 2,
	ContactWrite = 3,
};

struct AgentToolDefinition {
	QString name;
	QString description;
	QJsonObject parametersSchema;
	AgentToolRisk risk = AgentToolRisk::ReadOnly;
	AgentToolScope scope = AgentToolScope::ActiveMeeting;
	AgentConcurrencyGroup concurrencyGroup = AgentConcurrencyGroup::None;
};

struct AgentToolCall {
	QString id;
	QString name;
	QString argumentsJson;
	QString providerCallId;
	QString thoughtSignature;

	bool operator==(const AgentToolCall &other) const = default;
};

struct AgentThinkingBlock {
	QString type = QStringLiteral("thinking"); // "thinking", "redacted_thinking", or "gemini_thought"
	QString thinking;
	QString signature;

	bool operator==(const AgentThinkingBlock &other) const = default;
};

struct AgentToolResult {
	QString callId;
	QString toolName;
	bool success = false;
	QString content;
	QString errorMessage;
};

enum class AgentPendingActionStatus {
	Pending = 0,
	Approved = 1,
	Rejected = 2,
	Expired = 3,
	Executed = 4,
	Failed = 5,
};

struct AgentPendingToolAction {
	QString actionId;
	QString toolCallId;
	QString toolName;
	QString title;
	QString summary;
	QJsonObject arguments;
	SessionGenerationToken generationToken;
	AgentToolScope scope = AgentToolScope::ActiveMeeting;
	AgentConcurrencyGroup concurrencyGroup = AgentConcurrencyGroup::None;
	AgentPendingActionStatus status = AgentPendingActionStatus::Pending;
	QString resultMessage;
	QDateTime createdAt;
};

struct AgentMeetingChatEntry {
	QString senderName;
	QString text;
	bool isLocal = false;
	QDateTime timestamp;
};

struct AgentMeetingTranscriptEntry {
	QString segmentId;
	QString speakerIdentity;
	QString speakerName;
	QString text;
	QString language;
	qint64 timestampMs = 0;
	bool isFinal = false;
	bool isLocalSpeaker = false;
	bool isAgentGenerated = false;
};

inline constexpr int kMaxAgentImagesPerMessage = 4;
inline constexpr qint64 kMaxRawAgentImageBytes = 10LL * 1024 * 1024;     // 10 MiB
inline constexpr qint64 kMaxEncodedAgentImageBytes = 4LL * 1024 * 1024;  // 4 MiB
inline constexpr int kMaxAgentImageDimension = 2048;
inline constexpr int kMaxAgentImageSourceDimension = 8192;
inline constexpr qint64 kMaxAgentImageSourcePixels = 36000000LL;         // 36 MP

struct AgentImageAttachment {
	QString id;
	QString fileName;
	QString mimeType;          // "image/png", "image/jpeg", "image/webp", "image/gif"
	QByteArray encodedBytes;   // Normalized & re-encoded image bytes (EXIF stripped)
	int width = 0;
	int height = 0;
	qint64 originalSize = 0;

	[[nodiscard]] bool isValid() const {
		return !mimeType.isEmpty() && !encodedBytes.isEmpty() && width > 0 && height > 0;
	}

	bool operator==(const AgentImageAttachment &other) const = default;
};

struct AgentChatMessage {
	enum class Role {
		System,
		User,
		Assistant,
		Tool,
	};

	enum class Status {
		Complete,
		Streaming,
		Cancelled,
		Failed,
	};

	QString id;
	Role role = Role::User;
	QString content;
	QString reasoningContent;
	qint64 thinkingDurationMs = 0;
	bool isThinkingActive = false;
	QString errorMessage;
	Status status = Status::Complete;
	QDateTime timestamp;
	QVector<AgentToolCall> toolCalls;
	QVector<AgentThinkingBlock> thinkingBlocks;
	QString toolCallId;
	QString toolName;
	QString pendingActionId;
	QVector<AgentImageAttachment> images;
	QVector<AgentContextChip> contextChips;
	QVector<AgentResponsePart> parts;
	QVector<AgentFollowupSuggestion> followups;
};

struct AgentChatSessionSummary {
	QString sessionId;
	QString title;
	QDateTime createdAt;
	QDateTime updatedAt;
	AgentChatMode chatMode = AgentChatMode::Agent;
	int messageCount = 0;

	bool operator==(const AgentChatSessionSummary &other) const = default;
};

struct AgentChatSessionData {
	AgentChatSessionSummary summary;
	QVector<AgentChatMessage> messages;
};

enum class AgentKeyEndpointMismatch {
	None = 0,
	BailianPlanKeyOnStandardHost = 1,
	StandardKeyOnBailianPlanHost = 2,
	GroqKeyOnNonGroqHost = 3,
};

enum class AgentSpeechModelKind {
	NotSpeech = 0,
	SyncMultipart = 1,
	SyncChatInputAudio = 2,
	AsyncFiletrans = 3,
	RealtimeStream = 4,
	TtsOnly = 5,
	AudioUnderstanding = 6,
};

struct AgentSpeechModelInfo {
	AgentSpeechModelKind kind = AgentSpeechModelKind::NotSpeech;
	bool dedicatedAsr = false;
	bool catalogMember = false;
	bool usableForDictation = false;
};

} // namespace OpenMeeting

#include "src/agent/agent_model_capabilities.h"
#include "src/agent/agent_image_attachment.h"

Q_DECLARE_METATYPE(OpenMeeting::AgentProtocolDialect)
Q_DECLARE_METATYPE(OpenMeeting::AgentSpeechProtocolMode)
Q_DECLARE_METATYPE(OpenMeeting::AgentSpeechSourceMode)
Q_DECLARE_METATYPE(OpenMeeting::KeyStoragePolicy)
Q_DECLARE_METATYPE(OpenMeeting::AgentReasoningEffort)
Q_DECLARE_METATYPE(OpenMeeting::AgentChatMode)
Q_DECLARE_METATYPE(OpenMeeting::AgentPermissionMode)
Q_DECLARE_METATYPE(OpenMeeting::AgentContextChipKind)
Q_DECLARE_METATYPE(OpenMeeting::AgentContextChip)
Q_DECLARE_METATYPE(OpenMeeting::AgentFollowupSuggestion)
Q_DECLARE_METATYPE(OpenMeeting::AgentResponsePart)
Q_DECLARE_METATYPE(OpenMeeting::AgentModelCapabilitySource)
Q_DECLARE_METATYPE(OpenMeeting::AgentModelCapabilities)
Q_DECLARE_METATYPE(OpenMeeting::AgentProfile)
Q_DECLARE_METATYPE(OpenMeeting::AgentSpeechProfile)
Q_DECLARE_METATYPE(OpenMeeting::AgentImageAttachment)
Q_DECLARE_METATYPE(OpenMeeting::AgentChatMessage)
Q_DECLARE_METATYPE(OpenMeeting::AgentChatSessionSummary)
Q_DECLARE_METATYPE(OpenMeeting::AgentChatSessionData)
Q_DECLARE_METATYPE(OpenMeeting::SessionGenerationToken)
Q_DECLARE_METATYPE(OpenMeeting::AgentToolCall)
Q_DECLARE_METATYPE(OpenMeeting::AgentToolResult)
Q_DECLARE_METATYPE(OpenMeeting::AgentPendingActionStatus)
Q_DECLARE_METATYPE(OpenMeeting::AgentPendingToolAction)
