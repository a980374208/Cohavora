#pragma once

#include "src/net/service_endpoint_policy.h"

#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QJsonObject>
#include <QtCore/QMap>
#include <QtCore/QMetaType>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtCore/QVector>
#include <QtNetwork/QHostAddress>

#ifdef QT_GUI_LIB
#include <QtCore/QBuffer>
#include <QtCore/QIODevice>
#include <QtCore/QUuid>
#include <QtGui/QColor>
#include <QtGui/QImage>
#include <QtGui/QImageReader>
#include <QtGui/QImageWriter>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
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

[[nodiscard]] inline AgentSpeechSourceMode agentSpeechSourceModeFromInt(int raw) {
	return raw == static_cast<int>(AgentSpeechSourceMode::CustomProfile)
		? AgentSpeechSourceMode::CustomProfile
		: AgentSpeechSourceMode::FollowChat;
}

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

[[nodiscard]] inline bool isAgentLoopbackHost(const QString &host) {
	QString normalized = host.trimmed().toLower();
	if (normalized.startsWith(QLatin1Char('[')) && normalized.endsWith(QLatin1Char(']'))) {
		normalized = normalized.mid(1, normalized.size() - 2);
	}
	if (normalized == QStringLiteral("localhost") ||
		normalized.endsWith(QStringLiteral(".localhost"))) {
		return true;
	}
	QHostAddress address;
	if (!address.setAddress(normalized)) {
		return false;
	}
	if (address.isLoopback()) {
		return true;
	}
	// Handle IPv4-mapped IPv6 addresses like ::ffff:127.0.0.1
	bool ok = false;
	const quint32 ipv4 = address.toIPv4Address(&ok);
	if (ok && ((ipv4 & 0xff000000u) == 0x7f000000u)) {
		return true;
	}
	return false;
}

[[nodiscard]] inline bool isValidCustomHeaderName(const QString &name) {
	const QString trimmed = name.trimmed();
	if (trimmed.isEmpty() || trimmed.size() > 128) {
		return false;
	}
	for (QChar ch : trimmed) {
		const ushort code = ch.unicode();
		if (code <= 0x20 || code >= 0x7f) {
			return false;
		}
		if (ch == QLatin1Char(':') || ch == QLatin1Char('\r') || ch == QLatin1Char('\n')) {
			return false;
		}
	}
	const QString lower = trimmed.toLower();
	if (lower == QStringLiteral("host") ||
		lower == QStringLiteral("content-length") ||
		lower == QStringLiteral("transfer-encoding") ||
		lower == QStringLiteral("connection")) {
		return false;
	}
	return true;
}

[[nodiscard]] inline bool isValidCustomHeaderValue(const QString &value) {
	if (value.size() > 4096) {
		return false;
	}
	for (QChar ch : value) {
		const ushort code = ch.unicode();
		if (code == 0 || ch == QLatin1Char('\r') || ch == QLatin1Char('\n') || (code < 0x20 && code != '\t')) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] inline AgentEndpointDecision evaluateAgentEndpoint(const QString &urlText) {
	AgentEndpointDecision decision;
	const QString trimmed = urlText.trimmed();
	if (trimmed.isEmpty()) {
		decision.reason = QStringLiteral("Endpoint URL is required.");
		return decision;
	}

	const QUrl url(trimmed, QUrl::StrictMode);
	if (!url.isValid() || url.isRelative()) {
		decision.reason = QStringLiteral("Invalid endpoint URL syntax.");
		return decision;
	}

	const QString scheme = url.scheme().toLower();
	if (scheme != QStringLiteral("https") && scheme != QStringLiteral("http")) {
		decision.reason = QStringLiteral("Only http or https endpoints are supported.");
		return decision;
	}

	if (!url.userInfo().isEmpty()) {
		decision.reason = QStringLiteral("Credentials must not be embedded in the endpoint URL.");
		return decision;
	}

	if (url.hasQuery() || url.hasFragment()) {
		decision.reason = QStringLiteral("Endpoint URL must not include a query string or fragment.");
		return decision;
	}

	const QString host = url.host();
	if (host.isEmpty()) {
		decision.reason = QStringLiteral("Endpoint URL must specify a host.");
		return decision;
	}

	if (!hasSafeServicePath(url)) {
		decision.reason = QStringLiteral("Endpoint URL contains an unsafe path.");
		return decision;
	}

	const bool loopback = isAgentLoopbackHost(host);
	if (scheme == QStringLiteral("http") && !loopback && !isDebugHttpTransportEnabled()) {
		decision.reason = QStringLiteral("HTTPS is required for non-local agent endpoints.");
		return decision;
	}

	QUrl normalized = url;
	normalized.setScheme(scheme);
	normalized.setHost(host.toLower());
	if ((scheme == QStringLiteral("https") && normalized.port() == 443) ||
		(scheme == QStringLiteral("http") && normalized.port() == 80)) {
		normalized.setPort(-1);
	}
	QString clean = normalized.toString(QUrl::FullyEncoded);
	while (clean.endsWith(QLatin1Char('/')) && normalized.path() != QStringLiteral("/")) {
		clean.chop(1);
	}
	decision.allowed = true;
	decision.loopbackPlaintext = (scheme == QStringLiteral("http") && loopback);
	decision.normalizedUrl = QUrl(clean, QUrl::StrictMode);
	return decision;
}

[[nodiscard]] inline AgentProtocolDialect agentProtocolDialectFromInt(int raw) {
	switch (raw) {
	case static_cast<int>(AgentProtocolDialect::OpenAiResponsesSse):
		return AgentProtocolDialect::OpenAiResponsesSse;
	case static_cast<int>(AgentProtocolDialect::CustomWebhookJson):
		return AgentProtocolDialect::CustomWebhookJson;
	case static_cast<int>(AgentProtocolDialect::AnthropicMessagesSse):
		return AgentProtocolDialect::AnthropicMessagesSse;
	case static_cast<int>(AgentProtocolDialect::GoogleGeminiSse):
		return AgentProtocolDialect::GoogleGeminiSse;
	case static_cast<int>(AgentProtocolDialect::OpenAiChatCompletionsSse):
	default:
		return AgentProtocolDialect::OpenAiChatCompletionsSse;
	}
}

[[nodiscard]] inline AgentSpeechProtocolMode agentSpeechProtocolModeFromInt(int raw) {
	switch (raw) {
	case static_cast<int>(AgentSpeechProtocolMode::OpenAiTranscriptions):
		return AgentSpeechProtocolMode::OpenAiTranscriptions;
	case static_cast<int>(AgentSpeechProtocolMode::ChatInputAudio):
		return AgentSpeechProtocolMode::ChatInputAudio;
	case static_cast<int>(AgentSpeechProtocolMode::DashScopeMultimodal):
		return AgentSpeechProtocolMode::DashScopeMultimodal;
	case static_cast<int>(AgentSpeechProtocolMode::OpenAiRealtimeWs):
		return AgentSpeechProtocolMode::OpenAiRealtimeWs;
	case static_cast<int>(AgentSpeechProtocolMode::DashScopeInferenceWs):
		return AgentSpeechProtocolMode::DashScopeInferenceWs;
	case static_cast<int>(AgentSpeechProtocolMode::DashScopeQwenRealtimeWs):
		return AgentSpeechProtocolMode::DashScopeQwenRealtimeWs;
	case static_cast<int>(AgentSpeechProtocolMode::DeepgramWs):
		return AgentSpeechProtocolMode::DeepgramWs;
	case static_cast<int>(AgentSpeechProtocolMode::AssemblyAiV3Ws):
		return AgentSpeechProtocolMode::AssemblyAiV3Ws;
	case static_cast<int>(AgentSpeechProtocolMode::VolcengineBigModelWs):
		return AgentSpeechProtocolMode::VolcengineBigModelWs;
	case static_cast<int>(AgentSpeechProtocolMode::ElevenLabsScribeWs):
		return AgentSpeechProtocolMode::ElevenLabsScribeWs;
	case static_cast<int>(AgentSpeechProtocolMode::XaiSttWs):
		return AgentSpeechProtocolMode::XaiSttWs;
	case static_cast<int>(AgentSpeechProtocolMode::Auto):
	default:
		return AgentSpeechProtocolMode::Auto;
	}
}

[[nodiscard]] inline bool isStreamingSpeechProtocol(AgentSpeechProtocolMode mode) {
	switch (mode) {
	case AgentSpeechProtocolMode::OpenAiRealtimeWs:
	case AgentSpeechProtocolMode::DashScopeInferenceWs:
	case AgentSpeechProtocolMode::DashScopeQwenRealtimeWs:
	case AgentSpeechProtocolMode::DeepgramWs:
	case AgentSpeechProtocolMode::AssemblyAiV3Ws:
	case AgentSpeechProtocolMode::VolcengineBigModelWs:
	case AgentSpeechProtocolMode::ElevenLabsScribeWs:
	case AgentSpeechProtocolMode::XaiSttWs:
		return true;
	case AgentSpeechProtocolMode::Auto:
	case AgentSpeechProtocolMode::OpenAiTranscriptions:
	case AgentSpeechProtocolMode::ChatInputAudio:
	case AgentSpeechProtocolMode::DashScopeMultimodal:
	default:
		return false;
	}
}

[[nodiscard]] inline QString defaultStreamingSpeechEndpoint(AgentSpeechProtocolMode mode) {
	switch (mode) {
	case AgentSpeechProtocolMode::OpenAiRealtimeWs:
		return QStringLiteral("wss://api.openai.com/v1/realtime?intent=transcription");
	case AgentSpeechProtocolMode::DashScopeInferenceWs:
		return QStringLiteral("wss://dashscope.aliyuncs.com/api-ws/v1/inference");
	case AgentSpeechProtocolMode::DashScopeQwenRealtimeWs:
		return QStringLiteral("wss://dashscope.aliyuncs.com/api-ws/v1/realtime?model=qwen3-asr-flash-realtime");
	case AgentSpeechProtocolMode::DeepgramWs:
		return QStringLiteral("wss://api.deepgram.com/v1/listen");
	case AgentSpeechProtocolMode::AssemblyAiV3Ws:
		return QStringLiteral("wss://streaming.assemblyai.com/v3/ws");
	case AgentSpeechProtocolMode::VolcengineBigModelWs:
		return QStringLiteral("wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async");
	case AgentSpeechProtocolMode::ElevenLabsScribeWs:
		return QStringLiteral("wss://api.elevenlabs.io/v1/speech-to-text/realtime");
	case AgentSpeechProtocolMode::XaiSttWs:
		return QStringLiteral("wss://api.x.ai/v1/stt");
	default:
		return {};
	}
}

[[nodiscard]] inline QString defaultSpeechEndpoint(AgentSpeechProtocolMode mode) {
	switch (mode) {
	case AgentSpeechProtocolMode::OpenAiTranscriptions:
		return QStringLiteral("https://api.openai.com/v1/audio/transcriptions");
	case AgentSpeechProtocolMode::ChatInputAudio:
		return QStringLiteral("https://api.openai.com/v1/chat/completions");
	case AgentSpeechProtocolMode::DashScopeMultimodal:
		return QStringLiteral("https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation");
	case AgentSpeechProtocolMode::OpenAiRealtimeWs:
	case AgentSpeechProtocolMode::DashScopeInferenceWs:
	case AgentSpeechProtocolMode::DashScopeQwenRealtimeWs:
	case AgentSpeechProtocolMode::DeepgramWs:
	case AgentSpeechProtocolMode::AssemblyAiV3Ws:
	case AgentSpeechProtocolMode::VolcengineBigModelWs:
	case AgentSpeechProtocolMode::ElevenLabsScribeWs:
	case AgentSpeechProtocolMode::XaiSttWs:
		return defaultStreamingSpeechEndpoint(mode);
	case AgentSpeechProtocolMode::Auto:
	default:
		return {};
	}
}

[[nodiscard]] inline QString defaultSpeechProfileName(AgentSpeechProtocolMode mode) {
	switch (mode) {
	case AgentSpeechProtocolMode::OpenAiTranscriptions:
		return QStringLiteral("OpenAI Transcriptions");
	case AgentSpeechProtocolMode::ChatInputAudio:
		return QStringLiteral("OpenAI Chat Audio");
	case AgentSpeechProtocolMode::DashScopeMultimodal:
		return QCoreApplication::translate("MeetingUI", "DashScope Multimodal");
	case AgentSpeechProtocolMode::OpenAiRealtimeWs:
		return QStringLiteral("OpenAI Realtime");
	case AgentSpeechProtocolMode::DashScopeInferenceWs:
		return QCoreApplication::translate("MeetingUI", "DashScope Inference");
	case AgentSpeechProtocolMode::DashScopeQwenRealtimeWs:
		return QCoreApplication::translate("MeetingUI", "DashScope Qwen3 Realtime");
	case AgentSpeechProtocolMode::DeepgramWs:
		return QStringLiteral("Deepgram");
	case AgentSpeechProtocolMode::AssemblyAiV3Ws:
		return QStringLiteral("AssemblyAI");
	case AgentSpeechProtocolMode::VolcengineBigModelWs:
		return QCoreApplication::translate("MeetingUI", "Volcengine SAUC");
	case AgentSpeechProtocolMode::ElevenLabsScribeWs:
		return QStringLiteral("ElevenLabs");
	case AgentSpeechProtocolMode::XaiSttWs:
		return QStringLiteral("xAI Grok STT");
	case AgentSpeechProtocolMode::Auto:
	default:
		return {};
	}
}

[[nodiscard]] inline AgentReasoningEffort reasoningEffortFromInt(int raw) {
	switch (raw) {
	case static_cast<int>(AgentReasoningEffort::Low):
		return AgentReasoningEffort::Low;
	case static_cast<int>(AgentReasoningEffort::Medium):
		return AgentReasoningEffort::Medium;
	case static_cast<int>(AgentReasoningEffort::High):
		return AgentReasoningEffort::High;
	case static_cast<int>(AgentReasoningEffort::Extra):
		return AgentReasoningEffort::Extra;
	case static_cast<int>(AgentReasoningEffort::Default):
	default:
		return AgentReasoningEffort::Default;
	}
}

[[nodiscard]] inline QString reasoningEffortToWire(AgentReasoningEffort effort) {
	switch (effort) {
	case AgentReasoningEffort::Low:
		return QStringLiteral("low");
	case AgentReasoningEffort::Medium:
		return QStringLiteral("medium");
	case AgentReasoningEffort::High:
		return QStringLiteral("high");
	case AgentReasoningEffort::Extra:
		return QStringLiteral("xhigh");
	case AgentReasoningEffort::Default:
	default:
		return {};
	}
}

[[nodiscard]] inline QString reasoningEffortShortLabel(AgentReasoningEffort effort) {
	switch (effort) {
	case AgentReasoningEffort::Low:
		return QStringLiteral("Low");
	case AgentReasoningEffort::Medium:
		return QStringLiteral("Medium");
	case AgentReasoningEffort::High:
		return QStringLiteral("High");
	case AgentReasoningEffort::Extra:
		return QStringLiteral("Extra");
	case AgentReasoningEffort::Default:
	default:
		return {};
	}
}

// 阿里云百炼/DashScope 系域名（含 {WorkspaceId}.{region}.maas.aliyuncs.com 专属域名）。
[[nodiscard]] inline bool isDashScopeAgentHost(const QString &host) {
	const QString clean = host.trimmed().toLower();
	return clean == QStringLiteral("dashscope.aliyuncs.com") ||
		clean == QStringLiteral("dashscope-intl.aliyuncs.com") ||
		clean == QStringLiteral("qianwenaiapi.com") ||
		clean.endsWith(QStringLiteral(".dashscope.aliyuncs.com")) ||
		clean.endsWith(QStringLiteral(".dashscope-intl.aliyuncs.com")) ||
		clean.endsWith(QStringLiteral(".maas.aliyuncs.com")) ||
		clean.endsWith(QStringLiteral(".qianwenaiapi.com"));
}

[[nodiscard]] inline bool isDashScopeAgentHost(const QUrl &url) {
	return isDashScopeAgentHost(url.host());
}

[[nodiscard]] inline bool isBailianPlanAgentHost(const QString &host) {
	const QString clean = host.trimmed().toLower();
	return isDashScopeAgentHost(clean) &&
		(clean.startsWith(QStringLiteral("token-plan.")) ||
		 clean.contains(QStringLiteral(".token-plan.")) ||
		 clean.startsWith(QStringLiteral("coding.")) ||
		 clean.startsWith(QStringLiteral("coding-intl.")) ||
		 clean.contains(QStringLiteral(".coding.")));
}

[[nodiscard]] inline bool isBailianPlanAgentHost(const QUrl &url) {
	return isBailianPlanAgentHost(url.host());
}

[[nodiscard]] inline bool isBailianPlanApiKey(const QString &apiKey) {
	return apiKey.trimmed().startsWith(QStringLiteral("sk-sp-"), Qt::CaseInsensitive);
}

[[nodiscard]] inline bool isGroqAgentHost(const QString &host) {
	const QString clean = host.trimmed().toLower();
	return clean == QStringLiteral("groq.com") ||
		clean.endsWith(QStringLiteral(".groq.com"));
}

[[nodiscard]] inline bool isGroqAgentHost(const QUrl &url) {
	return isGroqAgentHost(url.host());
}

[[nodiscard]] inline bool isSiliconFlowAgentHost(const QString &host) {
	const QString clean = host.trimmed().toLower();
	return clean == QStringLiteral("siliconflow.cn") ||
		clean.endsWith(QStringLiteral(".siliconflow.cn")) ||
		clean == QStringLiteral("siliconflow.com") ||
		clean.endsWith(QStringLiteral(".siliconflow.com"));
}

[[nodiscard]] inline bool isSiliconFlowAgentHost(const QUrl &url) {
	return isSiliconFlowAgentHost(url.host());
}

[[nodiscard]] inline bool isOpenAiAgentHost(const QString &host) {
	const QString clean = host.trimmed().toLower();
	return clean == QStringLiteral("openai.com") ||
		clean.endsWith(QStringLiteral(".openai.com"));
}

[[nodiscard]] inline bool isOpenAiAgentHost(const QUrl &url) {
	return isOpenAiAgentHost(url.host());
}

[[nodiscard]] inline bool isDeepgramAgentHost(const QString &host) {
	const QString clean = host.trimmed().toLower();
	if (clean == QStringLiteral("agent.deepgram.com") ||
		clean.startsWith(QStringLiteral("agent.")) ||
		clean == QStringLiteral("manage.deepgram.com") ||
		clean.startsWith(QStringLiteral("manage."))) {
		return false;
	}
	return clean == QStringLiteral("deepgram.com") ||
		clean.endsWith(QStringLiteral(".deepgram.com"));
}

[[nodiscard]] inline bool isDeepgramAgentHost(const QUrl &url) {
	return isDeepgramAgentHost(url.host());
}

[[nodiscard]] inline bool isAssemblyAiAgentHost(const QString &host) {
	const QString clean = host.trimmed().toLower();
	return clean == QStringLiteral("assemblyai.com") ||
		clean.endsWith(QStringLiteral(".assemblyai.com"));
}

[[nodiscard]] inline bool isAssemblyAiAgentHost(const QUrl &url) {
	return isAssemblyAiAgentHost(url.host());
}

[[nodiscard]] inline bool isAssemblyAiStreamingHost(const QString &host) {
	const QString clean = host.trimmed().toLower();
	return clean == QStringLiteral("streaming.assemblyai.com") ||
		(clean.endsWith(QStringLiteral(".assemblyai.com")) &&
		 clean.startsWith(QStringLiteral("streaming.")));
}

[[nodiscard]] inline bool isAssemblyAiStreamingHost(const QUrl &url) {
	return isAssemblyAiStreamingHost(url.host());
}

[[nodiscard]] inline bool isAssemblyAiLlmGatewayHost(const QString &host) {
	const QString clean = host.trimmed().toLower();
	return clean == QStringLiteral("llm-gateway.assemblyai.com") ||
		(clean.endsWith(QStringLiteral(".assemblyai.com")) &&
		 clean.startsWith(QStringLiteral("llm-gateway.")));
}

[[nodiscard]] inline bool isAssemblyAiLlmGatewayHost(const QUrl &url) {
	return isAssemblyAiLlmGatewayHost(url.host());
}

[[nodiscard]] inline bool isVolcengineArkHost(const QString &host) {
	const QString clean = host.trimmed().toLower();
	return clean == QStringLiteral("ark.volces.com") ||
		(clean.endsWith(QStringLiteral(".volces.com")) &&
		 (clean.startsWith(QStringLiteral("ark.")) ||
		  clean.startsWith(QStringLiteral("ark-")) ||
		  clean.contains(QStringLiteral(".ark."))));
}

[[nodiscard]] inline bool isVolcengineArkHost(const QUrl &url) {
	return isVolcengineArkHost(url.host());
}

[[nodiscard]] inline bool isVolcengineSpeechHost(const QString &host) {
	const QString clean = host.trimmed().toLower();
	if (isVolcengineArkHost(clean)) {
		return false;
	}
	return clean == QStringLiteral("openspeech.bytedance.com") ||
		clean.endsWith(QStringLiteral(".openspeech.bytedance.com")) ||
		clean == QStringLiteral("openspeech.volces.com") ||
		clean.endsWith(QStringLiteral(".openspeech.volces.com"));
}

[[nodiscard]] inline bool isVolcengineSpeechHost(const QUrl &url) {
	return isVolcengineSpeechHost(url.host());
}

[[nodiscard]] inline bool isElevenLabsAgentHost(const QString &host) {
	const QString clean = host.trimmed().toLower();
	return clean == QStringLiteral("elevenlabs.io") ||
		clean.endsWith(QStringLiteral(".elevenlabs.io"));
}

[[nodiscard]] inline bool isElevenLabsAgentHost(const QUrl &url) {
	return isElevenLabsAgentHost(url.host());
}

[[nodiscard]] inline bool isElevenLabsSingleUseToken(const QString &credential) {
	return credential.trimmed().startsWith(QStringLiteral("sutkn_"), Qt::CaseInsensitive);
}

[[nodiscard]] inline bool isElevenLabsScribeModelId(const QString &modelId) {
	QString normalized = modelId.trimmed().toLower();
	const int slash = normalized.lastIndexOf(QLatin1Char('/'));
	if (slash >= 0 && slash + 1 < normalized.size()) {
		normalized = normalized.mid(slash + 1);
	}
	return normalized == QStringLiteral("scribe") ||
		normalized.startsWith(QStringLiteral("scribe_")) ||
		normalized.startsWith(QStringLiteral("scribe-"));
}

[[nodiscard]] inline bool isElevenLabsScribeRealtimeModelId(const QString &modelId) {
	if (!isElevenLabsScribeModelId(modelId)) {
		return false;
	}
	const QString lower = modelId.trimmed().toLower();
	return lower.contains(QStringLiteral("realtime")) ||
		lower.contains(QStringLiteral("live"));
}

[[nodiscard]] inline bool isElevenLabsScribeBatchModelId(const QString &modelId) {
	return isElevenLabsScribeModelId(modelId) &&
		!isElevenLabsScribeRealtimeModelId(modelId);
}

[[nodiscard]] inline bool isXaiAgentHost(const QString &host) {
	const QString clean = host.trimmed().toLower();
	return clean == QStringLiteral("x.ai") ||
		clean.endsWith(QStringLiteral(".x.ai"));
}

[[nodiscard]] inline bool isXaiAgentHost(const QUrl &url) {
	return isXaiAgentHost(url.host());
}

[[nodiscard]] inline bool isXaiSttFormatSupportedLanguage(const QString &primaryLangLower) {
	const QString clean = primaryLangLower.trimmed().toLower();
	return clean == QStringLiteral("ar") ||
		clean == QStringLiteral("de") ||
		clean == QStringLiteral("en") ||
		clean == QStringLiteral("es") ||
		clean == QStringLiteral("fr") ||
		clean == QStringLiteral("ja") ||
		clean == QStringLiteral("pt") ||
		clean == QStringLiteral("ru") ||
		clean == QStringLiteral("sv") ||
		clean == QStringLiteral("vi") ||
		clean == QStringLiteral("zh");
}

[[nodiscard]] inline QStringList parseXaiSttKeyterms(const QString &raw, int maxTotalChars = 0) {
	QString normalized = raw;
	for (QChar &ch : normalized) {
		if (ch == QChar(0xFF0C) || ch == QLatin1Char(';') ||
			ch == QChar(0xFF1B) || ch == QLatin1Char('\r') || ch == QLatin1Char('\n')) {
			ch = QLatin1Char(',');
		}
	}
	QStringList out;
	QStringList seenLower;
	int totalChars = 0;
	const QStringList parts = normalized.split(QLatin1Char(','), Qt::SkipEmptyParts);
	for (const QString &part : parts) {
		QString term = part.trimmed();
		if (term.size() > 50) {
			term = term.left(50).trimmed();
		}
		if (term.isEmpty()) {
			continue;
		}
		const QString lower = term.toLower();
		if (seenLower.contains(lower)) {
			continue;
		}
		if (out.size() >= 100 || (maxTotalChars > 0 && totalChars + term.size() > maxTotalChars)) {
			break;
		}
		seenLower.push_back(lower);
		out.push_back(term);
		totalChars += term.size();
	}
	return out;
}

[[nodiscard]] inline bool isSensitiveSpeechQueryParam(const QString &key) {
	const QString lower = key.trimmed().toLower();
	return lower == QStringLiteral("api_key") ||
		lower == QStringLiteral("apikey") ||
		lower == QStringLiteral("token") ||
		lower == QStringLiteral("access_token") ||
		lower == QStringLiteral("key") ||
		lower == QStringLiteral("secret") ||
		lower == QStringLiteral("authorization") ||
		lower == QStringLiteral("xi-api-key") ||
		lower == QStringLiteral("x-api-key") ||
		lower == QStringLiteral("x-api-access-key") ||
		lower == QStringLiteral("x-api-app-key");
}

[[nodiscard]] inline AgentEndpointDecision evaluateAgentSpeechEndpoint(const QString &urlText) {
	AgentEndpointDecision decision;
	const QString trimmed = urlText.trimmed();
	if (trimmed.isEmpty()) {
		decision.reason = QStringLiteral("Endpoint URL is required.");
		return decision;
	}

	const QUrl url(trimmed, QUrl::StrictMode);
	if (!url.isValid() || url.isRelative()) {
		decision.reason = QStringLiteral("Invalid endpoint URL syntax.");
		return decision;
	}

	const QString scheme = url.scheme().toLower();
	const bool isHttp = (scheme == QStringLiteral("http") || scheme == QStringLiteral("https"));
	const bool isWs = (scheme == QStringLiteral("ws") || scheme == QStringLiteral("wss"));
	if (!isHttp && !isWs) {
		decision.reason = QStringLiteral("Only https, http (loopback), wss, or ws (loopback) endpoints are supported.");
		return decision;
	}

	if (!url.userInfo().isEmpty()) {
		decision.reason = QStringLiteral("Credentials must not be embedded in the endpoint URL.");
		return decision;
	}

	if (url.hasFragment()) {
		decision.reason = QStringLiteral("Endpoint URL must not include a fragment.");
		return decision;
	}

	if (url.hasQuery()) {
		const QUrlQuery query(url);
		const auto items = query.queryItems(QUrl::FullyDecoded);
		for (const auto &item : items) {
			if (isSensitiveSpeechQueryParam(item.first)) {
				decision.reason = QStringLiteral("Credentials must not be passed in URL query parameters.");
				return decision;
			}
		}
	}

	const QString host = url.host();
	if (host.isEmpty()) {
		decision.reason = QStringLiteral("Endpoint URL must specify a host.");
		return decision;
	}

	if (!hasSafeServicePath(url)) {
		decision.reason = QStringLiteral("Endpoint URL contains an unsafe path.");
		return decision;
	}

	const bool loopback = isAgentLoopbackHost(host);
	const bool plaintextScheme = (scheme == QStringLiteral("http") || scheme == QStringLiteral("ws"));
	if (plaintextScheme && !loopback && !isDebugHttpTransportEnabled()) {
		decision.reason = QStringLiteral("TLS (https/wss) is required for non-local speech endpoints.");
		return decision;
	}

	QUrl normalized = url;
	normalized.setScheme(scheme);
	normalized.setHost(host.toLower());
	if (((scheme == QStringLiteral("https") || scheme == QStringLiteral("wss")) && normalized.port() == 443) ||
		((scheme == QStringLiteral("http") || scheme == QStringLiteral("ws")) && normalized.port() == 80)) {
		normalized.setPort(-1);
	}
	QString clean = normalized.toString(QUrl::FullyEncoded);
	if (!normalized.hasQuery()) {
		while (clean.endsWith(QLatin1Char('/')) && normalized.path() != QStringLiteral("/")) {
			clean.chop(1);
		}
	}
	decision.allowed = true;
	decision.loopbackPlaintext = (plaintextScheme && loopback);
	decision.normalizedUrl = QUrl(clean, QUrl::StrictMode);
	return decision;
}

[[nodiscard]] inline AgentSpeechProtocolMode resolveEffectiveSpeechProtocol(
		AgentSpeechProtocolMode configuredMode,
		const QString &sttBaseUrl,
		const QString &chatBaseUrl,
		const QString &modelId) {
	if (configuredMode != AgentSpeechProtocolMode::Auto) {
		return configuredMode;
	}

	const QString effectiveUrl = sttBaseUrl.trimmed().isEmpty()
		? chatBaseUrl.trimmed()
		: sttBaseUrl.trimmed();
	const QUrl url(effectiveUrl, QUrl::StrictMode);
	const QString scheme = url.isValid() ? url.scheme().toLower() : QString();
	const QString host = url.isValid() ? url.host().toLower() : QString();
	const QString path = url.isValid() ? url.path().toLower() : QString();
	const bool wsScheme = (scheme == QStringLiteral("wss") || scheme == QStringLiteral("ws"));

	QString model = modelId.trimmed().toLower();
	const int slash = model.lastIndexOf(QLatin1Char('/'));
	if (slash >= 0 && slash + 1 < model.size()) {
		model = model.mid(slash + 1);
	}

	if (isDeepgramAgentHost(host)) {
		return AgentSpeechProtocolMode::DeepgramWs;
	}
	if (isAssemblyAiStreamingHost(host) ||
		(wsScheme && isAssemblyAiAgentHost(host) && path.contains(QStringLiteral("/v3/ws")))) {
		return AgentSpeechProtocolMode::AssemblyAiV3Ws;
	}
	if (isVolcengineSpeechHost(host) || path.contains(QStringLiteral("/sauc/"))) {
		return AgentSpeechProtocolMode::VolcengineBigModelWs;
	}
	if (isElevenLabsScribeRealtimeModelId(model)) {
		return AgentSpeechProtocolMode::ElevenLabsScribeWs;
	}
	if (isElevenLabsAgentHost(host) || path.contains(QStringLiteral("/speech-to-text/realtime"))) {
		if (!wsScheme && !path.contains(QStringLiteral("/realtime")) && isElevenLabsScribeBatchModelId(model)) {
			return AgentSpeechProtocolMode::Auto;
		}
		return AgentSpeechProtocolMode::ElevenLabsScribeWs;
	}
	const bool isXaiSttPath = path == QStringLiteral("/v1/stt") ||
		path == QStringLiteral("/v1/stt/") ||
		path == QStringLiteral("/stt") ||
		path == QStringLiteral("/stt/");
	if (wsScheme && (isXaiSttPath ||
		(isXaiAgentHost(host) && !path.contains(QStringLiteral("/realtime"))))) {
		return AgentSpeechProtocolMode::XaiSttWs;
	}
	if (isDashScopeAgentHost(host)) {
		if (path.contains(QStringLiteral("/api-ws/v1/inference")) ||
			model.startsWith(QStringLiteral("paraformer-realtime")) ||
			model.startsWith(QStringLiteral("fun-asr-realtime")) ||
			model.startsWith(QStringLiteral("gummy-realtime")) ||
			(model.contains(QStringLiteral("qwen")) &&
			 model.contains(QStringLiteral("asr")) &&
			 (model.contains(QStringLiteral("stream")) ||
			  model.endsWith(QStringLiteral("-message"))))) {
			return AgentSpeechProtocolMode::DashScopeInferenceWs;
		}
		if (path.contains(QStringLiteral("/api-ws/v1/realtime")) ||
			(model.contains(QStringLiteral("qwen")) &&
			 model.contains(QStringLiteral("asr")) &&
			 model.contains(QStringLiteral("realtime")))) {
			return AgentSpeechProtocolMode::DashScopeQwenRealtimeWs;
		}
		return AgentSpeechProtocolMode::Auto;
	}

	if (wsScheme && path.contains(QStringLiteral("/realtime"))) {
		return AgentSpeechProtocolMode::OpenAiRealtimeWs;
	}
	if (!model.contains(QStringLiteral("-diarize")) &&
		(model == QStringLiteral("gpt-4o-transcribe") ||
		 model == QStringLiteral("gpt-4o-mini-transcribe") ||
		 model.startsWith(QStringLiteral("gpt-4o-transcribe-")) ||
		 model.startsWith(QStringLiteral("gpt-4o-mini-transcribe-")) ||
		 model.contains(QStringLiteral("-live-transcribe")) ||
		 model.contains(QStringLiteral("-realtime-whisper")))) {
		return AgentSpeechProtocolMode::OpenAiRealtimeWs;
	}
	return AgentSpeechProtocolMode::Auto;
}

[[nodiscard]] inline bool isGroqApiKey(const QString &apiKey) {
	return apiKey.trimmed().startsWith(QStringLiteral("gsk_"), Qt::CaseInsensitive);
}

enum class AgentKeyEndpointMismatch {
	None = 0,
	BailianPlanKeyOnStandardHost = 1,
	StandardKeyOnBailianPlanHost = 2,
	GroqKeyOnNonGroqHost = 3,
};

[[nodiscard]] inline AgentKeyEndpointMismatch evaluateKeyEndpointCompatibility(
		const QString &apiKey,
		const QString &endpointUrlOrHost) {
	const QString cleanKey = apiKey.trimmed();
	const QString rawTarget = endpointUrlOrHost.trimmed();
	if (cleanKey.isEmpty() || rawTarget.isEmpty()) {
		return AgentKeyEndpointMismatch::None;
	}
	QString host = rawTarget;
	if (rawTarget.contains(QStringLiteral("://"))) {
		const QUrl url(rawTarget);
		if (!url.isValid()) {
			return AgentKeyEndpointMismatch::None;
		}
		host = url.host();
	}
	host = host.trimmed().toLower();
	if (isGroqApiKey(cleanKey) && !isGroqAgentHost(host) && !isAgentLoopbackHost(host)) {
		return AgentKeyEndpointMismatch::GroqKeyOnNonGroqHost;
	}
	if (!isDashScopeAgentHost(host)) {
		return AgentKeyEndpointMismatch::None;
	}
	const bool planKey = isBailianPlanApiKey(cleanKey);
	const bool planHost = isBailianPlanAgentHost(host);
	if (planKey && !planHost) {
		return AgentKeyEndpointMismatch::BailianPlanKeyOnStandardHost;
	}
	if (!planKey && planHost && cleanKey.startsWith(QStringLiteral("sk-"), Qt::CaseInsensitive)) {
		return AgentKeyEndpointMismatch::StandardKeyOnBailianPlanHost;
	}
	return AgentKeyEndpointMismatch::None;
}

[[nodiscard]] inline QString redactEmbeddedApiKeys(const QString &text) {
	if (text.isEmpty()) {
		return text;
	}
	auto isTokenChar = [](QChar ch) {
		return ch.isLetterOrNumber() || ch == QLatin1Char('_') || ch == QLatin1Char('-');
	};
	QString out;
	out.reserve(text.size());
	int i = 0;
	const int n = text.size();
	while (i < n) {
		const bool atBoundary = (i == 0) || !isTokenChar(text.at(i - 1));
		if (atBoundary) {
			const QStringView tail = QStringView(text).mid(i);
			int prefixLen = 0;
			if (tail.startsWith(QLatin1String("sk-"), Qt::CaseInsensitive) ||
				tail.startsWith(QLatin1String("sk_"), Qt::CaseInsensitive)) {
				prefixLen = 3;
			} else if (tail.startsWith(QLatin1String("gsk_"), Qt::CaseInsensitive) ||
					   tail.startsWith(QLatin1String("xai-"), Qt::CaseInsensitive)) {
				prefixLen = 4;
			} else if (tail.startsWith(QLatin1String("sutkn_"), Qt::CaseInsensitive)) {
				prefixLen = 6;
			}
			if (prefixLen > 0) {
				int end = i + prefixLen;
				while (end < n && isTokenChar(text.at(end))) {
					++end;
				}
				if (end - (i + prefixLen) >= 12) {
					out.append(QStringLiteral("[redacted-key]"));
					i = end;
					continue;
				}
			}
		}
		out.append(text.at(i));
		++i;
	}
	return out;
}

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

[[nodiscard]] inline QString agentSpeechModelKindToken(AgentSpeechModelKind kind) {
	switch (kind) {
	case AgentSpeechModelKind::SyncMultipart:
		return QStringLiteral("sync-multipart");
	case AgentSpeechModelKind::SyncChatInputAudio:
		return QStringLiteral("sync-chat-input-audio");
	case AgentSpeechModelKind::AsyncFiletrans:
		return QStringLiteral("async-filetrans");
	case AgentSpeechModelKind::RealtimeStream:
		return QStringLiteral("realtime-stream");
	case AgentSpeechModelKind::TtsOnly:
		return QStringLiteral("tts-only");
	case AgentSpeechModelKind::AudioUnderstanding:
		return QStringLiteral("audio-understanding");
	case AgentSpeechModelKind::NotSpeech:
	default:
		return QStringLiteral("not-speech");
	}
}

[[nodiscard]] inline bool hasDeclaredAudioInputModality(const AgentModelCapabilities &caps) {
	if (caps.modalitySource != AgentModelCapabilitySource::ServerMetadata) {
		return false;
	}
	const bool audioIn = std::any_of(
		caps.inputModalities.cbegin(),
		caps.inputModalities.cend(),
		[](const QString &m) {
			return m == QStringLiteral("audio") || m == QStringLiteral("speech");
		});
	if (!audioIn) {
		return false;
	}
	if (caps.outputModalities.isEmpty()) {
		return true;
	}
	bool hasTextOut = false;
	bool hasAudioOut = false;
	for (const QString &m : caps.outputModalities) {
		if (m == QStringLiteral("text")) {
			hasTextOut = true;
		} else if (m == QStringLiteral("audio") || m == QStringLiteral("speech")) {
			hasAudioOut = true;
		}
	}
	return hasTextOut && !hasAudioOut;
}

// 语音识别模型族系的单一判定入口：传输形态、目录可见性、本地麦克风听写可用性。
// 目录过滤、默认模型选择、请求前门禁均从此处取值，不再各自维护名字黑名单。
// endpointHost 只影响协议形态选择；未知名字保持按端点回退，自定义/自托管 STT 不会被本地拒绝。
[[nodiscard]] inline AgentSpeechModelInfo classifySpeechModel(
		const QString &modelId,
		const QString &endpointHost = QString(),
		const AgentModelCapabilities &caps = {}) {
	AgentSpeechModelInfo info;
	QString normalized = modelId.trimmed().toLower();
	if (normalized.isEmpty()) {
		return info;
	}
	const int slash = normalized.lastIndexOf(QLatin1Char('/'));
	if (slash >= 0 && slash + 1 < normalized.size()) {
		normalized = normalized.mid(slash + 1);
	}
	const bool dashscope = isDashScopeAgentHost(endpointHost);
	const bool volcengineArk = isVolcengineArkHost(endpointHost);
	const bool serverAudioToText = hasDeclaredAudioInputModality(caps);

	if ((normalized.contains(QStringLiteral("tts")) ||
		normalized.startsWith(QStringLiteral("cosyvoice")) ||
		normalized.startsWith(QStringLiteral("speech-")) ||
		normalized.contains(QStringLiteral("-speech")) ||
		normalized.startsWith(QStringLiteral("eleven_"))) &&
		!serverAudioToText) {
		info.kind = AgentSpeechModelKind::TtsOnly;
		return info;
	}

	const bool openAiTranscribe = normalized.startsWith(QStringLiteral("whisper")) ||
		normalized.contains(QStringLiteral("-whisper")) ||
		normalized.contains(QStringLiteral("-transcribe"));
	const bool qwenAsr = normalized.startsWith(QStringLiteral("qwen3-asr")) ||
		(normalized.contains(QStringLiteral("qwen")) && normalized.contains(QStringLiteral("asr")));
	const bool genericAsr = normalized.endsWith(QStringLiteral("-asr")) ||
		normalized.contains(QStringLiteral("-asr-")) ||
		normalized.startsWith(QStringLiteral("asr-"));
	// fun-asr / paraformer / sensevoice 仅在 DashScope 域名下是异步文件 URL 任务；
	// 在 SiliconFlow、FunASR、Xinference 等非 DashScope 兼容服务端上均为同步 multipart ASR。
	const bool sharedAsrFamily = normalized.startsWith(QStringLiteral("fun-asr")) ||
		normalized.startsWith(QStringLiteral("paraformer")) ||
		normalized.startsWith(QStringLiteral("sensevoice"));
	const bool teleSpeechAsr = normalized == QStringLiteral("telespeechasr") ||
		normalized.startsWith(QStringLiteral("telespeechasr-")) ||
		normalized.startsWith(QStringLiteral("telespeech-asr"));
	const bool xaiStt = normalized == QStringLiteral("grok-stt") ||
		normalized.startsWith(QStringLiteral("grok-stt-")) ||
		normalized.startsWith(QStringLiteral("grok-voice-transcribe"));
	const bool elevenLabsScribe = isElevenLabsScribeModelId(normalized);
	const bool elevenLabsScribeRealtime = isElevenLabsScribeRealtimeModelId(normalized);
	const bool dashScopeAsyncOnly = dashscope && sharedAsrFamily;
	const bool dedicatedAsr = openAiTranscribe || qwenAsr || genericAsr ||
		sharedAsrFamily || teleSpeechAsr || xaiStt || elevenLabsScribe;

	// 专用实时流式 ASR 模型可进入语音模型目录供 WebSocket 流式模式选择（catalogMember = dedicatedAsr），
	// 但 usableForDictation 保持 false，以阻止显式 HTTP 协议误发短音频同步请求；
	// 非专用 ASR 实时模型（如 -livetranslate / -s2s- / omni-realtime）继续排除在目录外。
	if (normalized.contains(QStringLiteral("-realtime")) ||
		normalized.contains(QStringLiteral("_realtime")) ||
		normalized.contains(QStringLiteral("-live-transcribe")) ||
		normalized.contains(QStringLiteral("-stream")) ||
		normalized.startsWith(QStringLiteral("stream-")) ||
		elevenLabsScribeRealtime ||
		(qwenAsr && normalized.endsWith(QStringLiteral("-message"))) ||
		normalized.contains(QStringLiteral("-s2s-")) ||
		normalized.contains(QStringLiteral("-livetranslate"))) {
		info.kind = AgentSpeechModelKind::RealtimeStream;
		info.dedicatedAsr = dedicatedAsr;
		info.catalogMember = dedicatedAsr;
		return info;
	}
	if (normalized.contains(QStringLiteral("-filetrans")) ||
		normalized.contains(QStringLiteral("-async")) ||
		dashScopeAsyncOnly) {
		info.kind = AgentSpeechModelKind::AsyncFiletrans;
		info.dedicatedAsr = true;
		return info;
	}

	const bool qwenAudioOrOmni = !qwenAsr &&
		(normalized == QStringLiteral("qwen-audio") ||
			normalized.startsWith(QStringLiteral("qwen-audio-")) ||
			normalized == QStringLiteral("qwen2-audio") ||
			normalized.startsWith(QStringLiteral("qwen2-audio-")) ||
			normalized.contains(QStringLiteral("-omni")) ||
			normalized.startsWith(QStringLiteral("omni-")));
	if (qwenAudioOrOmni && !serverAudioToText) {
		info.kind = AgentSpeechModelKind::AudioUnderstanding;
		return info;
	}

	const bool doubaoSeedAudio =
		normalized == QStringLiteral("doubao-seed-2-1-lite") ||
		normalized.startsWith(QStringLiteral("doubao-seed-2-1-lite-")) ||
		normalized == QStringLiteral("doubao-seed-2-0-lite") ||
		normalized.startsWith(QStringLiteral("doubao-seed-2-0-lite-")) ||
		normalized == QStringLiteral("doubao-seed-2-0-mini") ||
		normalized.startsWith(QStringLiteral("doubao-seed-2-0-mini-"));

	info.usableForDictation = true;
	info.dedicatedAsr = dedicatedAsr;
	info.catalogMember = info.dedicatedAsr || serverAudioToText || doubaoSeedAudio;

	if (openAiTranscribe || sharedAsrFamily || teleSpeechAsr || xaiStt || elevenLabsScribe) {
		info.kind = AgentSpeechModelKind::SyncMultipart;
	} else if (qwenAsr || qwenAudioOrOmni || doubaoSeedAudio ||
		((dashscope || volcengineArk) && (genericAsr || serverAudioToText))) {
		info.kind = AgentSpeechModelKind::SyncChatInputAudio;
	} else if (genericAsr || serverAudioToText) {
		info.kind = AgentSpeechModelKind::SyncMultipart;
	} else {
		info.kind = (dashscope || volcengineArk)
			? AgentSpeechModelKind::SyncChatInputAudio
			: AgentSpeechModelKind::SyncMultipart;
	}
	return info;
}

[[nodiscard]] inline bool isSpeechSttAgentModelId(
		const QString &modelId,
		const QString &endpointHost = QString()) {
	return classifySpeechModel(modelId, endpointHost).catalogMember;
}

[[nodiscard]] inline bool isNonChatAgentModelId(const QString &modelId) {
	QString normalized = modelId.trimmed().toLower();
	if (normalized.isEmpty()) {
		return false;
	}
	const int slash = normalized.lastIndexOf(QLatin1Char('/'));
	if (slash >= 0 && slash + 1 < normalized.size()) {
		normalized = normalized.mid(slash + 1);
	}

	// Embeddings & rerankers
	if (normalized.contains(QStringLiteral("embedding")) ||
		normalized.contains(QStringLiteral("rerank")) ||
		normalized.startsWith(QStringLiteral("bge-")) ||
		normalized.contains(QStringLiteral("-embed-")) ||
		normalized.endsWith(QStringLiteral("-embed"))) {
		return true;
	}

	// Image / video generation
	if (normalized.endsWith(QStringLiteral("-image")) ||
		normalized.contains(QStringLiteral("-image-")) ||
		normalized.startsWith(QStringLiteral("wanx")) ||
		normalized.startsWith(QStringLiteral("wan2.")) ||
		normalized.startsWith(QStringLiteral("z-image")) ||
		normalized.startsWith(QStringLiteral("dall-e")) ||
		normalized.startsWith(QStringLiteral("cogview")) ||
		normalized.startsWith(QStringLiteral("seedream")) ||
		normalized.startsWith(QStringLiteral("imagen"))) {
		return true;
	}

	// Speech / ASR / TTS / Realtime audio-to-audio
	if (normalized.endsWith(QStringLiteral("-tts")) ||
		normalized.contains(QStringLiteral("-tts-")) ||
		normalized.startsWith(QStringLiteral("tts-")) ||
		normalized.endsWith(QStringLiteral("-asr")) ||
		normalized.contains(QStringLiteral("-asr-")) ||
		normalized.startsWith(QStringLiteral("asr-")) ||
		normalized.startsWith(QStringLiteral("fun-asr")) ||
		normalized.startsWith(QStringLiteral("cosyvoice")) ||
		normalized.startsWith(QStringLiteral("paraformer")) ||
		normalized.startsWith(QStringLiteral("sensevoice")) ||
		normalized.startsWith(QStringLiteral("scribe_")) ||
		normalized.startsWith(QStringLiteral("scribe-")) ||
		normalized.startsWith(QStringLiteral("eleven_")) ||
		normalized == QStringLiteral("telespeechasr") ||
		normalized.startsWith(QStringLiteral("telespeechasr-")) ||
		normalized.startsWith(QStringLiteral("telespeech-asr")) ||
		normalized.startsWith(QStringLiteral("speech-")) ||
		normalized.contains(QStringLiteral("-speech")) ||
		normalized.startsWith(QStringLiteral("whisper")) ||
		normalized.contains(QStringLiteral("-whisper")) ||
		normalized.contains(QStringLiteral("-transcribe")) ||
		normalized.startsWith(QStringLiteral("qwen3-asr")) ||
		normalized.contains(QStringLiteral("-realtime")) ||
		normalized.contains(QStringLiteral("-s2s-")) ||
		normalized == QStringLiteral("qwen-audio") ||
		normalized.startsWith(QStringLiteral("qwen-audio-")) ||
		normalized == QStringLiteral("qwen2-audio") ||
		normalized.startsWith(QStringLiteral("qwen2-audio-"))) {
		return true;
	}

	// Specialized OCR / MT / LiveTranslate
	if (normalized.endsWith(QStringLiteral("-ocr")) ||
		normalized.contains(QStringLiteral("-ocr-")) ||
		normalized.contains(QStringLiteral("-livetranslate")) ||
		normalized.startsWith(QStringLiteral("qwen-mt-"))) {
		return true;
	}

	return false;
}

[[nodiscard]] inline QString detectSupportedAgentImageMimeByMagic(const QByteArray &bytes) {
	if (bytes.size() >= 8 && std::memcmp(bytes.constData(), "\x89PNG\r\n\x1a\n", 8) == 0) {
		return QStringLiteral("image/png");
	}
	if (bytes.size() >= 3 &&
		static_cast<unsigned char>(bytes[0]) == 0xFF &&
		static_cast<unsigned char>(bytes[1]) == 0xD8 &&
		static_cast<unsigned char>(bytes[2]) == 0xFF) {
		return QStringLiteral("image/jpeg");
	}
	if (bytes.size() >= 12 &&
		std::memcmp(bytes.constData(), "RIFF", 4) == 0 &&
		std::memcmp(bytes.constData() + 8, "WEBP", 4) == 0) {
		return QStringLiteral("image/webp");
	}
	if (bytes.size() >= 6 &&
		(std::memcmp(bytes.constData(), "GIF87a", 6) == 0 ||
		 std::memcmp(bytes.constData(), "GIF89a", 6) == 0)) {
		return QStringLiteral("image/gif");
	}
	return {};
}

#ifdef QT_GUI_LIB
namespace detail {

[[nodiscard]] inline QImage flattenAlphaOntoWhite(const QImage &src) {
	if (!src.hasAlphaChannel()) {
		return src.convertToFormat(QImage::Format_RGB888);
	}
	QImage rgba = src.convertToFormat(QImage::Format_ARGB32);
	const int w = rgba.width();
	const int h = rgba.height();
	for (int y = 0; y < h; ++y) {
		auto *line = reinterpret_cast<QRgb *>(rgba.scanLine(y));
		for (int x = 0; x < w; ++x) {
			const QRgb px = line[x];
			const int a = qAlpha(px);
			if (a < 255) {
				const int inv = 255 - a;
				line[x] = qRgb(
					(qRed(px) * a + 255 * inv) / 255,
					(qGreen(px) * a + 255 * inv) / 255,
					(qBlue(px) * a + 255 * inv) / 255);
			}
		}
	}
	return rgba.convertToFormat(QImage::Format_RGB888);
}

[[nodiscard]] inline QByteArray encodeAgentQImage(const QImage &img, const char *format, int quality) {
	QByteArray out;
	QBuffer buf(&out);
	if (!buf.open(QIODevice::WriteOnly)) {
		return {};
	}
	QImageWriter writer(&buf, format);
	if (quality >= 0) {
		writer.setQuality(quality);
	}
	if (!writer.write(img)) {
		return {};
	}
	return out;
}

} // namespace detail

[[nodiscard]] inline AgentImageAttachment prepareAgentImageFromQImage(
		const QImage &srcImg,
		const QString &fileName,
		qint64 originalSize = 0,
		QString *outError = nullptr) {
	if (srcImg.isNull() || srcImg.width() < 1 || srcImg.height() < 1) {
		if (outError) {
			*outError = QCoreApplication::translate(
				"MeetingUI", "Unsupported or corrupted image file (only PNG, JPEG, WebP, and GIF are allowed).");
		}
		return {};
	}
	if (srcImg.width() > kMaxAgentImageSourceDimension ||
		srcImg.height() > kMaxAgentImageSourceDimension ||
		qint64(srcImg.width()) * srcImg.height() > kMaxAgentImageSourcePixels) {
		if (outError) {
			*outError = QCoreApplication::translate(
				"MeetingUI", "Image resolution exceeds the supported limit.");
		}
		return {};
	}

	QImage working = srcImg;
	if (working.width() > kMaxAgentImageDimension || working.height() > kMaxAgentImageDimension) {
		working = working.scaled(
			kMaxAgentImageDimension,
			kMaxAgentImageDimension,
			Qt::KeepAspectRatio,
			Qt::SmoothTransformation);
	}

	QString mime;
	QByteArray encoded;
	constexpr qint64 kMaxPngPreferredBytes = 1536LL * 1024;
	if (working.hasAlphaChannel()) {
		const QByteArray pngBytes = detail::encodeAgentQImage(working, "png", -1);
		if (!pngBytes.isEmpty() && pngBytes.size() <= kMaxPngPreferredBytes) {
			mime = QStringLiteral("image/png");
			encoded = pngBytes;
		}
	}
	if (encoded.isEmpty()) {
		const QImage flat = detail::flattenAlphaOntoWhite(working);
		const QByteArray jpgBytes = detail::encodeAgentQImage(flat, "jpg", 85);
		if (!jpgBytes.isEmpty()) {
			mime = QStringLiteral("image/jpeg");
			encoded = jpgBytes;
		} else {
			// Fallback to PNG (built into QtGui) if the JPEG plugin is unavailable.
			const QByteArray pngBytes = detail::encodeAgentQImage(working, "png", -1);
			if (!pngBytes.isEmpty()) {
				mime = QStringLiteral("image/png");
				encoded = pngBytes;
			}
		}
	}
	if (encoded.isEmpty() || encoded.size() > kMaxEncodedAgentImageBytes) {
		if (working.width() > 1280 || working.height() > 1280) {
			working = working.scaled(1280, 1280, Qt::KeepAspectRatio, Qt::SmoothTransformation);
		}
		const QImage flat = detail::flattenAlphaOntoWhite(working);
		QByteArray fallbackBytes = detail::encodeAgentQImage(flat, "jpg", 75);
		QString fallbackMime = QStringLiteral("image/jpeg");
		if (fallbackBytes.isEmpty()) {
			fallbackBytes = detail::encodeAgentQImage(working, "png", -1);
			fallbackMime = QStringLiteral("image/png");
		}
		if (!fallbackBytes.isEmpty() && fallbackBytes.size() <= kMaxEncodedAgentImageBytes) {
			mime = fallbackMime;
			encoded = fallbackBytes;
		} else {
			if (outError) {
				*outError = QCoreApplication::translate("MeetingUI", "Image file exceeds the 10 MB limit.");
			}
			return {};
		}
	}

	AgentImageAttachment att;
	att.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
	att.fileName = fileName.trimmed().isEmpty() ? QStringLiteral("image.png") : fileName.trimmed();
	att.mimeType = mime;
	att.encodedBytes = encoded;
	att.width = working.width();
	att.height = working.height();
	att.originalSize = originalSize > 0 ? originalSize : encoded.size();
	return att;
}

[[nodiscard]] inline AgentImageAttachment prepareAgentImageFromRawBytes(
		const QByteArray &rawBytes,
		const QString &fileName,
		QString *outError = nullptr) {
	if (rawBytes.isEmpty()) {
		if (outError) {
			*outError = QCoreApplication::translate(
				"MeetingUI", "Unsupported or corrupted image file (only PNG, JPEG, WebP, and GIF are allowed).");
		}
		return {};
	}
	if (rawBytes.size() > kMaxRawAgentImageBytes) {
		if (outError) {
			*outError = QCoreApplication::translate("MeetingUI", "Image file exceeds the 10 MB limit.");
		}
		return {};
	}
	const QString detectedMime = detectSupportedAgentImageMimeByMagic(rawBytes);
	if (detectedMime.isEmpty()) {
		if (outError) {
			*outError = QCoreApplication::translate(
				"MeetingUI", "Unsupported or corrupted image file (only PNG, JPEG, WebP, and GIF are allowed).");
		}
		return {};
	}

	const char *readerFmt = nullptr;
	if (detectedMime == QStringLiteral("image/png")) {
		readerFmt = "png";
	} else if (detectedMime == QStringLiteral("image/jpeg")) {
		readerFmt = "jpg";
	} else if (detectedMime == QStringLiteral("image/webp")) {
		readerFmt = "webp";
	} else if (detectedMime == QStringLiteral("image/gif")) {
		readerFmt = "gif";
	}

	QBuffer buf;
	buf.setData(rawBytes);
	if (!buf.open(QIODevice::ReadOnly)) {
		return {};
	}
	QImageReader reader(&buf, readerFmt ? QByteArray(readerFmt) : QByteArray());
	reader.setAutoTransform(true);
	const QSize declaredSize = reader.size();
	if (declaredSize.isValid() &&
		(declaredSize.width() < 1 || declaredSize.height() < 1 ||
		 declaredSize.width() > kMaxAgentImageSourceDimension ||
		 declaredSize.height() > kMaxAgentImageSourceDimension ||
		 qint64(declaredSize.width()) * declaredSize.height() > kMaxAgentImageSourcePixels)) {
		if (outError) {
			*outError = QCoreApplication::translate(
				"MeetingUI", "Image resolution exceeds the supported limit.");
		}
		return {};
	}
	const QImage decoded = reader.read();
	if (decoded.isNull()) {
		if (outError) {
			*outError = QCoreApplication::translate(
				"MeetingUI", "Unsupported or corrupted image file (only PNG, JPEG, WebP, and GIF are allowed).");
		}
		return {};
	}
	return prepareAgentImageFromQImage(decoded, fileName, rawBytes.size(), outError);
}
#endif // QT_GUI_LIB

[[nodiscard]] inline bool inferVisionSupportByNormalizedId(const QString &normalized) {
	if (normalized.contains(QStringLiteral("-vl")) ||
		normalized.startsWith(QStringLiteral("vl-")) ||
		normalized.contains(QStringLiteral("_vl")) ||
		normalized.contains(QStringLiteral("vision")) ||
		normalized.contains(QStringLiteral("omni")) ||
		normalized.contains(QStringLiteral("multimodal")) ||
		normalized.contains(QStringLiteral("pixtral")) ||
		normalized.contains(QStringLiteral("llava")) ||
		normalized.contains(QStringLiteral("minicpm-v")) ||
		normalized.contains(QStringLiteral("internvl")) ||
		normalized.contains(QStringLiteral("molmo")) ||
		normalized.startsWith(QStringLiteral("qvq"))) {
		return true;
	}

	if (normalized.startsWith(QStringLiteral("gpt-4o")) ||
		normalized.startsWith(QStringLiteral("chatgpt-4o")) ||
		normalized.startsWith(QStringLiteral("gpt-4-turbo")) ||
		normalized.startsWith(QStringLiteral("gpt-4.1")) ||
		normalized.startsWith(QStringLiteral("gpt-4.5")) ||
		normalized.startsWith(QStringLiteral("gpt-5")) ||
		normalized == QStringLiteral("o1") ||
		(normalized.startsWith(QStringLiteral("o1-")) &&
			!normalized.startsWith(QStringLiteral("o1-mini")) &&
			!normalized.startsWith(QStringLiteral("o1-preview"))) ||
		normalized == QStringLiteral("o3") ||
		(normalized.startsWith(QStringLiteral("o3-")) &&
			!normalized.startsWith(QStringLiteral("o3-mini"))) ||
		normalized == QStringLiteral("o4") ||
		normalized.startsWith(QStringLiteral("o4-"))) {
		return true;
	}

	if (normalized.startsWith(QStringLiteral("gemini-1.5")) ||
		normalized.startsWith(QStringLiteral("gemini-2")) ||
		normalized.startsWith(QStringLiteral("gemini-3")) ||
		normalized.startsWith(QStringLiteral("gemini-4")) ||
		normalized.startsWith(QStringLiteral("gemini-pro-vision")) ||
		normalized.startsWith(QStringLiteral("gemma-3"))) {
		return true;
	}

	if (normalized.startsWith(QStringLiteral("claude-3")) ||
		normalized.startsWith(QStringLiteral("claude-4")) ||
		normalized.startsWith(QStringLiteral("claude-5")) ||
		normalized.startsWith(QStringLiteral("claude-sonnet-4")) ||
		normalized.startsWith(QStringLiteral("claude-opus-4")) ||
		normalized.startsWith(QStringLiteral("claude-haiku-4"))) {
		return true;
	}

	if (normalized.startsWith(QStringLiteral("glm-4v")) ||
		normalized.startsWith(QStringLiteral("glm-4.1v")) ||
		normalized.startsWith(QStringLiteral("glm-4.5v")) ||
		normalized.startsWith(QStringLiteral("glm-4.6v")) ||
		normalized.startsWith(QStringLiteral("glm-5v")) ||
		normalized.startsWith(QStringLiteral("qwen-vl")) ||
		normalized.startsWith(QStringLiteral("qwen2-vl")) ||
		normalized.startsWith(QStringLiteral("qwen2.5-vl")) ||
		normalized.startsWith(QStringLiteral("qwen3-vl")) ||
		normalized.startsWith(QStringLiteral("kimi-latest")) ||
		normalized.startsWith(QStringLiteral("kimi-k2.5")) ||
		normalized.startsWith(QStringLiteral("kimi-k3")) ||
		normalized.startsWith(QStringLiteral("step-1v")) ||
		normalized.startsWith(QStringLiteral("step-1o")) ||
		normalized.startsWith(QStringLiteral("step-3")) ||
		normalized.startsWith(QStringLiteral("doubao-seed-")) ||
		normalized.startsWith(QStringLiteral("grok-4"))) {
		return true;
	}

	return false;
}

[[nodiscard]] inline int inferContextLengthByNormalizedId(const QString &normalized) {
	if (normalized.isEmpty()) {
		return 0;
	}
	if (normalized.startsWith(QStringLiteral("gemini-1.5")) ||
		normalized.startsWith(QStringLiteral("gemini-2")) ||
		normalized.startsWith(QStringLiteral("gemini-3")) ||
		normalized.startsWith(QStringLiteral("gemini-4"))) {
		return 1000000;
	}
	if (normalized.startsWith(QStringLiteral("gpt-4.1"))) {
		return 1047576;
	}
	if (normalized.startsWith(QStringLiteral("gpt-5"))) {
		return 400000;
	}
	if (normalized == QStringLiteral("o1") || normalized.startsWith(QStringLiteral("o1-")) ||
		normalized == QStringLiteral("o3") || normalized.startsWith(QStringLiteral("o3-")) ||
		normalized == QStringLiteral("o4") || normalized.startsWith(QStringLiteral("o4-"))) {
		return 200000;
	}
	if (normalized.startsWith(QStringLiteral("gpt-4o")) ||
		normalized.startsWith(QStringLiteral("chatgpt-4o")) ||
		normalized.startsWith(QStringLiteral("gpt-4-turbo")) ||
		normalized.startsWith(QStringLiteral("codex-"))) {
		return 128000;
	}
	if (normalized.startsWith(QStringLiteral("claude-3")) ||
		normalized.startsWith(QStringLiteral("claude-4")) ||
		normalized.startsWith(QStringLiteral("claude-5"))) {
		return 200000;
	}
	if (normalized.startsWith(QStringLiteral("doubao-seed-2-1"))) {
		return 1048576;
	}
	if (normalized.startsWith(QStringLiteral("doubao-seed-2-0")) ||
		normalized.startsWith(QStringLiteral("doubao-seed-1-6")) ||
		normalized.startsWith(QStringLiteral("doubao-seed-1-8"))) {
		return 262144;
	}
	if (normalized.startsWith(QStringLiteral("qwen3")) ||
		normalized.startsWith(QStringLiteral("qwen2.5")) ||
		normalized.startsWith(QStringLiteral("qwen-plus")) ||
		normalized.startsWith(QStringLiteral("qwen-max")) ||
		normalized.startsWith(QStringLiteral("qwen-turbo")) ||
		normalized.startsWith(QStringLiteral("qwq")) ||
		normalized.startsWith(QStringLiteral("glm-4")) ||
		normalized.startsWith(QStringLiteral("glm-5")) ||
		normalized.startsWith(QStringLiteral("kimi-"))) {
		return 131072;
	}
	if (normalized.startsWith(QStringLiteral("deepseek-"))) {
		return (normalized.contains(QStringLiteral("v3")) ||
			normalized.contains(QStringLiteral("v4")) ||
			normalized.contains(QStringLiteral("r1")) ||
			normalized.contains(QStringLiteral("reasoner")))
			? 131072
			: 65536;
	}
	if (normalized.startsWith(QStringLiteral("grok-3")) ||
		normalized.startsWith(QStringLiteral("grok-4"))) {
		return 131072;
	}
	return 0;
}

[[nodiscard]] inline QString formatAgentContextWindowLabel(int tokens) {
	if (tokens <= 0) {
		return {};
	}
	if (tokens >= 1000000) {
		if (tokens % 1048576 == 0) {
			return QStringLiteral("%1M").arg(tokens / 1048576);
		}
		if (tokens % 1000000 == 0) {
			return QStringLiteral("%1M").arg(tokens / 1000000);
		}
		if (tokens >= 1047000 && tokens <= 1050000) {
			return QStringLiteral("1M");
		}
		return QStringLiteral("%1M").arg(QString::number(tokens / 1000000.0, 'f', 1));
	}
	if (tokens >= 1024) {
		if (tokens % 1024 == 0) {
			return QStringLiteral("%1K").arg(tokens / 1024);
		}
		return QStringLiteral("%1K").arg(qMax(1, qRound(tokens / 1000.0)));
	}
	return QString::number(tokens);
}

[[nodiscard]] inline AgentModelCapabilities inferModelCapabilitiesById(const QString &modelId) {
	AgentModelCapabilities caps;
	caps.modelId = modelId.trimmed();
	if (caps.modelId.isEmpty() || isNonChatAgentModelId(caps.modelId)) {
		return caps;
	}

	QString normalized = caps.modelId.toLower();
	const int slash = normalized.lastIndexOf(QLatin1Char('/'));
	if (slash >= 0 && slash + 1 < normalized.size()) {
		normalized = normalized.mid(slash + 1);
	}
	if (normalized.startsWith(QStringLiteral("cmaas-"))) {
		normalized = normalized.mid(6);
	}

	const auto setSupported = [&](bool extra) {
		caps.supportsReasoningEffort = true;
		caps.source = AgentModelCapabilitySource::RuleInferred;
		caps.supportedEfforts = {
			AgentReasoningEffort::Low,
			AgentReasoningEffort::Medium,
			AgentReasoningEffort::High,
		};
		if (extra) {
			caps.supportedEfforts.push_back(AgentReasoningEffort::Extra);
		}
	};

	// OpenAI reasoning series: o1, o3, o4, gpt-5+, codex-mini
	if (normalized == QStringLiteral("o1") || normalized.startsWith(QStringLiteral("o1-")) ||
		normalized == QStringLiteral("o3") || normalized.startsWith(QStringLiteral("o3-")) ||
		normalized == QStringLiteral("o4") || normalized.startsWith(QStringLiteral("o4-")) ||
		normalized.startsWith(QStringLiteral("codex-"))) {
		setSupported(false);
	} else if (normalized.startsWith(QStringLiteral("gpt-5"))) {
		setSupported(true);
	} else if (normalized.startsWith(QStringLiteral("gemini-")) &&
		(normalized.contains(QStringLiteral("gemini-2.5")) ||
		 normalized.contains(QStringLiteral("gemini-3")) ||
		 normalized.contains(QStringLiteral("gemini-4")) ||
		 normalized.contains(QStringLiteral("thinking")))) {
		// Gemini 2.5+ / 3+ / 4+ / thinking variants
		setSupported(normalized.contains(QStringLiteral("gemini-3")) ||
			normalized.contains(QStringLiteral("gemini-4")));
	} else if (normalized.startsWith(QStringLiteral("claude-")) &&
		(normalized.contains(QStringLiteral("thinking")) ||
		 normalized.contains(QStringLiteral("3-7")) ||
		 normalized.contains(QStringLiteral("3.7")) ||
		 normalized.contains(QStringLiteral("-4")) ||
		 normalized.contains(QStringLiteral("4-")) ||
		 normalized.contains(QStringLiteral("4.")) ||
		 normalized.contains(QStringLiteral("-5")) ||
		 normalized.contains(QStringLiteral("5.")))) {
		// Claude 3.7 / 4+ / thinking variants
		setSupported(false);
	} else if (normalized.contains(QStringLiteral("deepseek-r1")) ||
		normalized.contains(QStringLiteral("deepseek-reasoner")) ||
		normalized.contains(QStringLiteral("deepseek-v3.2-think")) ||
		normalized.startsWith(QStringLiteral("deepseek-v4")) ||
		normalized.startsWith(QStringLiteral("qwq")) ||
		normalized.startsWith(QStringLiteral("qwen3")) ||
		normalized.contains(QStringLiteral("glm-z1")) ||
		normalized.startsWith(QStringLiteral("glm-4.5")) ||
		normalized.startsWith(QStringLiteral("glm-4.6")) ||
		normalized.startsWith(QStringLiteral("glm-4.7")) ||
		normalized.startsWith(QStringLiteral("glm-5")) ||
		normalized.startsWith(QStringLiteral("kimi-k2.5")) ||
		normalized.startsWith(QStringLiteral("kimi-k2.6")) ||
		normalized.startsWith(QStringLiteral("kimi-k2.7")) ||
		normalized.startsWith(QStringLiteral("kimi-k2.8")) ||
		normalized.startsWith(QStringLiteral("kimi-k3")) ||
		normalized.startsWith(QStringLiteral("minimax-m2.")) ||
		normalized.startsWith(QStringLiteral("minimax-m3")) ||
		normalized.startsWith(QStringLiteral("step-3.")) ||
		normalized.startsWith(QStringLiteral("step-5")) ||
		normalized.startsWith(QStringLiteral("mimo-v2.")) ||
		normalized.startsWith(QStringLiteral("ling-3.")) ||
		normalized.startsWith(QStringLiteral("doubao-seed-")) ||
		normalized.startsWith(QStringLiteral("grok-3-mini")) ||
		normalized.startsWith(QStringLiteral("grok-4")) ||
		normalized.contains(QStringLiteral("thinking"))) {
		// DeepSeek R1 / Reasoner / V3.2-think / V4, QwQ / Qwen3+, GLM Z1 / 4.5+ / 5+, Kimi K2.5+ / K3,
		// MiniMax M2.5+ / M3+, Step 3.7+ / 5+, MiMo V2.6+, Grok 3-mini / 4
		setSupported(false);
	}

	if (inferVisionSupportByNormalizedId(normalized)) {
		caps.supportsVision = true;
		caps.visionSource = AgentModelCapabilitySource::RuleInferred;
	}
	if (const int inferredCtx = inferContextLengthByNormalizedId(normalized); inferredCtx > 0) {
		caps.contextLengthTokens = inferredCtx;
		caps.contextLengthSource = AgentModelCapabilitySource::RuleInferred;
	}

	return caps;
}

} // namespace OpenMeeting

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
