#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QUrl>

namespace OpenMeeting {

struct AgentEndpointDecision;
struct AgentModelCapabilities;
struct AgentSpeechModelInfo;
enum class AgentKeyEndpointMismatch;
enum class AgentProtocolDialect;
enum class AgentReasoningEffort;
enum class AgentSpeechModelKind;
enum class AgentSpeechProtocolMode;
enum class AgentSpeechSourceMode;

[[nodiscard]] AgentSpeechSourceMode agentSpeechSourceModeFromInt(int raw);
[[nodiscard]] bool isAgentLoopbackHost(const QString &host);
[[nodiscard]] bool isValidCustomHeaderName(const QString &name);
[[nodiscard]] bool isValidCustomHeaderValue(const QString &value);
[[nodiscard]] AgentEndpointDecision evaluateAgentEndpoint(const QString &urlText);
[[nodiscard]] AgentProtocolDialect agentProtocolDialectFromInt(int raw);
[[nodiscard]] AgentSpeechProtocolMode agentSpeechProtocolModeFromInt(int raw);
[[nodiscard]] bool isStreamingSpeechProtocol(AgentSpeechProtocolMode mode);
[[nodiscard]] QString defaultStreamingSpeechEndpoint(AgentSpeechProtocolMode mode);
[[nodiscard]] QString defaultSpeechEndpoint(AgentSpeechProtocolMode mode);
[[nodiscard]] QString defaultSpeechProfileName(AgentSpeechProtocolMode mode);
[[nodiscard]] AgentReasoningEffort reasoningEffortFromInt(int raw);
[[nodiscard]] QString reasoningEffortToWire(AgentReasoningEffort effort);
[[nodiscard]] QString reasoningEffortShortLabel(AgentReasoningEffort effort);

[[nodiscard]] bool isDashScopeAgentHost(const QString &host);
[[nodiscard]] bool isDashScopeAgentHost(const QUrl &url);
[[nodiscard]] bool isBailianPlanAgentHost(const QString &host);
[[nodiscard]] bool isBailianPlanAgentHost(const QUrl &url);
[[nodiscard]] bool isBailianPlanApiKey(const QString &apiKey);
[[nodiscard]] bool isGroqAgentHost(const QString &host);
[[nodiscard]] bool isGroqAgentHost(const QUrl &url);
[[nodiscard]] bool isSiliconFlowAgentHost(const QString &host);
[[nodiscard]] bool isSiliconFlowAgentHost(const QUrl &url);
[[nodiscard]] bool isOpenAiAgentHost(const QString &host);
[[nodiscard]] bool isOpenAiAgentHost(const QUrl &url);
[[nodiscard]] bool isDeepgramAgentHost(const QString &host);
[[nodiscard]] bool isDeepgramAgentHost(const QUrl &url);
[[nodiscard]] bool isAssemblyAiAgentHost(const QString &host);
[[nodiscard]] bool isAssemblyAiAgentHost(const QUrl &url);
[[nodiscard]] bool isAssemblyAiStreamingHost(const QString &host);
[[nodiscard]] bool isAssemblyAiStreamingHost(const QUrl &url);
[[nodiscard]] bool isAssemblyAiLlmGatewayHost(const QString &host);
[[nodiscard]] bool isAssemblyAiLlmGatewayHost(const QUrl &url);
[[nodiscard]] bool isVolcengineArkHost(const QString &host);
[[nodiscard]] bool isVolcengineArkHost(const QUrl &url);
[[nodiscard]] bool isVolcengineSpeechHost(const QString &host);
[[nodiscard]] bool isVolcengineSpeechHost(const QUrl &url);
[[nodiscard]] bool isElevenLabsAgentHost(const QString &host);
[[nodiscard]] bool isElevenLabsAgentHost(const QUrl &url);
[[nodiscard]] bool isElevenLabsSingleUseToken(const QString &credential);
[[nodiscard]] bool isElevenLabsScribeModelId(const QString &modelId);
[[nodiscard]] bool isElevenLabsScribeRealtimeModelId(const QString &modelId);
[[nodiscard]] bool isElevenLabsScribeBatchModelId(const QString &modelId);
[[nodiscard]] bool isXaiAgentHost(const QString &host);
[[nodiscard]] bool isXaiAgentHost(const QUrl &url);
[[nodiscard]] bool isXaiSttFormatSupportedLanguage(const QString &primaryLangLower);
[[nodiscard]] QStringList parseXaiSttKeyterms(const QString &raw, int maxTotalChars = 0);
[[nodiscard]] bool isSensitiveSpeechQueryParam(const QString &key);
[[nodiscard]] AgentEndpointDecision evaluateAgentSpeechEndpoint(const QString &urlText);
[[nodiscard]] AgentSpeechProtocolMode resolveEffectiveSpeechProtocol(
	AgentSpeechProtocolMode configuredMode,
	const QString &sttBaseUrl,
	const QString &chatBaseUrl,
	const QString &modelId);
[[nodiscard]] bool isGroqApiKey(const QString &apiKey);
[[nodiscard]] AgentKeyEndpointMismatch evaluateKeyEndpointCompatibility(
	const QString &apiKey,
	const QString &endpointUrlOrHost);
[[nodiscard]] QString redactEmbeddedApiKeys(const QString &text);
[[nodiscard]] QString agentSpeechModelKindToken(AgentSpeechModelKind kind);
[[nodiscard]] bool hasDeclaredAudioInputModality(const AgentModelCapabilities &caps);
[[nodiscard]] AgentSpeechModelInfo classifySpeechModel(
	const QString &modelId,
	const QString &endpointHost = QString(),
	const AgentModelCapabilities &caps = {});
[[nodiscard]] bool isSpeechSttAgentModelId(
	const QString &modelId,
	const QString &endpointHost = QString());
[[nodiscard]] bool isNonChatAgentModelId(const QString &modelId);
[[nodiscard]] QString detectSupportedAgentImageMimeByMagic(const QByteArray &bytes);
[[nodiscard]] bool inferVisionSupportByNormalizedId(const QString &normalized);
[[nodiscard]] int inferContextLengthByNormalizedId(const QString &normalized);
[[nodiscard]] QString formatAgentContextWindowLabel(int tokens);
[[nodiscard]] AgentModelCapabilities inferModelCapabilitiesById(const QString &modelId);

} // namespace OpenMeeting
