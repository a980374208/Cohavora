#include "src/agent/agent_coordinator.h"

#include "src/agent/agent_speech_probe_audio.h"
#include "src/agent/openai_compatible_transport.h"
#include "src/core/call_invite_controller.h"
#include "src/core/contact_catalog_controller.h"
#include "src/core/meeting_catalog_controller.h"
#include "src/core/meeting_coordinator.h"
#include "src/core/presence_watch_controller.h"
#include "src/net/session_manager.h"

#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QUuid>

#include <algorithm>
#include <vector>

namespace OpenMeeting {
namespace {

constexpr auto kProfilePrefix = "agent/profile/";
constexpr auto kProfilesRootPrefix = "agent/profiles/";
constexpr auto kProfilesIdsKey = "agent/profiles/ids";
constexpr auto kProfilesActiveIdKey = "agent/profiles/activeId";
constexpr auto kSpeechSourceModeKey = "agent/speech/sourceMode";
constexpr auto kSpeechFollowProtocolKey = "agent/speech/followChatProtocol";
constexpr auto kSpeechFollowModelKey = "agent/speech/followChatModel";
constexpr auto kSpeechProfilesRootPrefix = "agent/speech/profiles/";
constexpr auto kSpeechProfilesIdsKey = "agent/speech/profiles/ids";
constexpr auto kSpeechProfilesActiveIdKey = "agent/speech/profiles/activeId";
constexpr auto kDefaultSpeechResourceId = "volc.seedasr.sauc.duration";
constexpr int kDeltaCoalesceIntervalMs = 40;
constexpr int kMaxStoredMessages = 100;
constexpr int kMaxStoredChatEntries = 50;
constexpr int kMaxStoredTranscriptEntries = 100;
constexpr int kMaxToolRounds = 4;

std::unique_ptr<QSettings> makeDefaultAgentSettings() {
	auto settings = std::make_unique<QSettings>();
	settings->setFallbacksEnabled(false);
	return settings;
}

KeyStoragePolicy policyFromInt(int raw) {
	return raw == static_cast<int>(KeyStoragePolicy::MemoryOnly)
		? KeyStoragePolicy::MemoryOnly
		: KeyStoragePolicy::EncryptedStore;
}

AgentProtocolDialect dialectFromInt(int raw) {
	return agentProtocolDialectFromInt(raw);
}

QString defaultSystemPrompt() {
	return QStringLiteral(
		"You are Cohavora Copilot, a concise and helpful meeting assistant. "
		"Answer accurately, stay structured, and do not invent meeting details you were not given.");
}

QString profileSettingsPrefix(const QString &profileId) {
	return QLatin1String(kProfilesRootPrefix) + profileId + QLatin1Char('/');
}

AgentProfile normalizeProfileValues(AgentProfile prof, const QString &fallbackId) {
	prof.id = prof.id.trimmed();
	if (prof.id.isEmpty()) {
		prof.id = fallbackId.trimmed().isEmpty() ? QStringLiteral("default") : fallbackId.trimmed();
	}
	prof.baseUrl = prof.baseUrl.trimmed();
	prof.model = prof.model.trimmed();
	prof.temperature = qBound(0.0, prof.temperature, 2.0);
	prof.maxContextMessages = qBound(2, prof.maxContextMessages, 64);
	prof.connectTimeoutMs = qBound(2000, prof.connectTimeoutMs, 60000);
	prof.idleTimeoutMs = qBound(3000, prof.idleTimeoutMs, 180000);
	return prof;
}

AgentProfile loadSingleProfileFromPrefix(
		const QSettings &settings,
		const QString &prefix,
		const QString &fallbackId) {
	AgentProfile prof;
	prof.id = settings.value(prefix + QStringLiteral("id"), fallbackId).toString().trimmed();
	if (prof.id.isEmpty()) {
		prof.id = fallbackId;
	}
	prof.name = settings.value(prefix + QStringLiteral("name"),
		QStringLiteral("Custom Agent")).toString();
	prof.protocol = dialectFromInt(
		settings.value(prefix + QStringLiteral("protocol"),
			static_cast<int>(AgentProtocolDialect::OpenAiChatCompletionsSse)).toInt());
	prof.baseUrl = settings.value(prefix + QStringLiteral("baseUrl")).toString().trimmed();
	prof.model = settings.value(prefix + QStringLiteral("model")).toString().trimmed();
	prof.systemPrompt = settings.value(prefix + QStringLiteral("systemPrompt"),
		defaultSystemPrompt()).toString();
	prof.temperature = qBound(0.0,
		settings.value(prefix + QStringLiteral("temperature"), 0.7).toDouble(),
		2.0);
	prof.maxContextMessages = qBound(2,
		settings.value(prefix + QStringLiteral("maxContextMessages"), 16).toInt(),
		64);
	prof.connectTimeoutMs = qBound(2000,
		settings.value(prefix + QStringLiteral("connectTimeoutMs"), 10000).toInt(),
		60000);
	prof.idleTimeoutMs = qBound(3000,
		settings.value(prefix + QStringLiteral("idleTimeoutMs"), 30000).toInt(),
		180000);
	// Default strictly to EncryptedStore per security/UX policy.
	prof.keyStorage = policyFromInt(
		settings.value(prefix + QStringLiteral("keyStorage"),
			static_cast<int>(KeyStoragePolicy::EncryptedStore)).toInt());
	prof.sendMeetingContextByDefault = settings.value(
		prefix + QStringLiteral("sendMeetingContextByDefault"), false).toBool();
	prof.persistChatHistory = settings.value(
		prefix + QStringLiteral("persistChatHistory"), true).toBool();
	prof.reasoningEffort = reasoningEffortFromInt(
		settings.value(prefix + QStringLiteral("reasoningEffort"),
			static_cast<int>(AgentReasoningEffort::Default)).toInt());

	const QByteArray headersRaw = settings.value(
		prefix + QStringLiteral("customHeadersJson")).toByteArray();
	if (!headersRaw.isEmpty()) {
		const auto doc = QJsonDocument::fromJson(headersRaw);
		if (doc.isObject()) {
			const auto obj = doc.object();
			for (auto it = obj.begin(); it != obj.end(); ++it) {
				if (isValidCustomHeaderName(it.key()) && isValidCustomHeaderValue(it.value().toString())) {
					prof.customHeaders.insert(it.key().trimmed(), it.value().toString());
				}
			}
		}
	}
	return prof;
}

void saveSingleProfileToPrefix(QSettings &settings, const QString &prefix, const AgentProfile &prof) {
	settings.setValue(prefix + QStringLiteral("id"), prof.id);
	settings.setValue(prefix + QStringLiteral("name"), prof.name);
	settings.setValue(prefix + QStringLiteral("protocol"),
		static_cast<int>(prof.protocol));
	settings.setValue(prefix + QStringLiteral("baseUrl"), prof.baseUrl.trimmed());
	settings.setValue(prefix + QStringLiteral("model"), prof.model.trimmed());
	settings.setValue(prefix + QStringLiteral("systemPrompt"), prof.systemPrompt);
	settings.setValue(prefix + QStringLiteral("temperature"), prof.temperature);
	settings.setValue(prefix + QStringLiteral("maxContextMessages"), prof.maxContextMessages);
	settings.setValue(prefix + QStringLiteral("connectTimeoutMs"), prof.connectTimeoutMs);
	settings.setValue(prefix + QStringLiteral("idleTimeoutMs"), prof.idleTimeoutMs);
	settings.setValue(prefix + QStringLiteral("keyStorage"),
		static_cast<int>(prof.keyStorage));
	settings.setValue(prefix + QStringLiteral("sendMeetingContextByDefault"),
		prof.sendMeetingContextByDefault);
	settings.setValue(prefix + QStringLiteral("persistChatHistory"),
		prof.persistChatHistory);
	settings.setValue(prefix + QStringLiteral("reasoningEffort"),
		static_cast<int>(prof.reasoningEffort));

	QJsonObject headersObj;
	for (auto it = prof.customHeaders.cbegin(); it != prof.customHeaders.cend(); ++it) {
		if (isValidCustomHeaderName(it.key()) && isValidCustomHeaderValue(it.value())) {
			headersObj.insert(it.key().trimmed(), it.value());
		}
	}
	if (headersObj.isEmpty()) {
		settings.remove(prefix + QStringLiteral("customHeadersJson"));
	} else {
		settings.setValue(prefix + QStringLiteral("customHeadersJson"),
			QString::fromUtf8(QJsonDocument(headersObj).toJson(QJsonDocument::Compact)));
	}
}

QString speechProfileSettingsPrefix(const QString &profileId) {
	return QLatin1String(kSpeechProfilesRootPrefix) + profileId + QLatin1Char('/');
}

AgentSpeechProfile normalizeSpeechProfileValues(AgentSpeechProfile prof, const QString &fallbackId) {
	prof.id = prof.id.trimmed();
	if (prof.id.isEmpty()) {
		prof.id = fallbackId.trimmed().isEmpty() ? QStringLiteral("default_stt") : fallbackId.trimmed();
	}
	prof.name = prof.name.trimmed();
	if (prof.name.isEmpty()) {
		prof.name = QCoreApplication::translate("MeetingUI", "Default Speech");
	}
	prof.baseUrl = prof.baseUrl.trimmed();
	prof.model = prof.model.trimmed();
	prof.resourceId = prof.resourceId.trimmed();
	if (prof.resourceId.isEmpty()) {
		prof.resourceId = QLatin1String(kDefaultSpeechResourceId);
	}
	prof.workspaceId = prof.workspaceId.trimmed();
	return prof;
}

AgentSpeechProfile loadSingleSpeechProfileFromPrefix(
		const QSettings &settings,
		const QString &prefix,
		const QString &fallbackId) {
	AgentSpeechProfile prof;
	prof.id = settings.value(prefix + QStringLiteral("id"), fallbackId).toString().trimmed();
	if (prof.id.isEmpty()) {
		prof.id = fallbackId;
	}
	prof.name = settings.value(prefix + QStringLiteral("name"), QStringLiteral("Default Speech")).toString().trimmed();
	prof.protocol = agentSpeechProtocolModeFromInt(
		settings.value(prefix + QStringLiteral("protocol"),
			static_cast<int>(AgentSpeechProtocolMode::Auto)).toInt());
	prof.baseUrl = settings.value(prefix + QStringLiteral("baseUrl")).toString().trimmed();
	prof.model = settings.value(prefix + QStringLiteral("model")).toString().trimmed();
	prof.resourceId = settings.value(
		prefix + QStringLiteral("resourceId"),
		QLatin1String(kDefaultSpeechResourceId)).toString().trimmed();
	if (prof.resourceId.isEmpty()) {
		prof.resourceId = QLatin1String(kDefaultSpeechResourceId);
	}
	prof.workspaceId = settings.value(prefix + QStringLiteral("workspaceId")).toString().trimmed();
	return normalizeSpeechProfileValues(prof, fallbackId);
}

void saveSingleSpeechProfileToPrefix(QSettings &settings, const QString &prefix, const AgentSpeechProfile &prof) {
	settings.setValue(prefix + QStringLiteral("id"), prof.id);
	settings.setValue(prefix + QStringLiteral("name"), prof.name);
	settings.setValue(prefix + QStringLiteral("protocol"), static_cast<int>(prof.protocol));
	settings.setValue(prefix + QStringLiteral("baseUrl"), prof.baseUrl.trimmed());
	settings.setValue(prefix + QStringLiteral("model"), prof.model.trimmed());
	settings.setValue(prefix + QStringLiteral("resourceId"),
		prof.resourceId.trimmed().isEmpty() ? QLatin1String(kDefaultSpeechResourceId) : prof.resourceId.trimmed());
	settings.setValue(prefix + QStringLiteral("workspaceId"), prof.workspaceId.trimmed());
}

QString inferLegacySpeechProfileName(const QString &url, AgentSpeechProtocolMode proto) {
	const QString cleanUrl = url.trimmed();
	const QUrl parsed = QUrl::fromUserInput(cleanUrl);
	const QString host = parsed.host().trimmed().toLower();
	if (isElevenLabsAgentHost(parsed) || proto == AgentSpeechProtocolMode::ElevenLabsScribeWs) {
		return QStringLiteral("ElevenLabs");
	}
	if (host.contains(QStringLiteral("deepgram.com")) || proto == AgentSpeechProtocolMode::DeepgramWs) {
		return QStringLiteral("Deepgram");
	}
	if (host.contains(QStringLiteral("assemblyai.com")) || proto == AgentSpeechProtocolMode::AssemblyAiV3Ws) {
		return QStringLiteral("AssemblyAI");
	}
	if (isGroqAgentHost(host)) {
		return QStringLiteral("Groq");
	}
	if (host.contains(QStringLiteral("bytedance.com")) ||
		host.contains(QStringLiteral("volces.com")) ||
		proto == AgentSpeechProtocolMode::VolcengineBigModelWs) {
		return QCoreApplication::translate("MeetingUI", "Volcengine SAUC");
	}
	if (isDashScopeAgentHost(parsed) ||
		proto == AgentSpeechProtocolMode::DashScopeInferenceWs ||
		proto == AgentSpeechProtocolMode::DashScopeQwenRealtimeWs ||
		proto == AgentSpeechProtocolMode::DashScopeMultimodal) {
		return QStringLiteral("DashScope");
	}
	if (host.contains(QStringLiteral("openai.com")) || proto == AgentSpeechProtocolMode::OpenAiRealtimeWs) {
		return QStringLiteral("OpenAI");
	}
	if (isXaiAgentHost(parsed) || proto == AgentSpeechProtocolMode::XaiSttWs) {
		return QStringLiteral("xAI");
	}
	if (!host.isEmpty()) {
		return host;
	}
	return QCoreApplication::translate("MeetingUI", "Default Speech");
}

QString extractToolResultSummary(const QString &jsonText, int maxLen = 120) {
	const QString trimmed = jsonText.trimmed();
	if (trimmed.isEmpty()) {
		return {};
	}
	const QJsonDocument doc = QJsonDocument::fromJson(trimmed.toUtf8());
	if (doc.isObject()) {
		QJsonObject obj = doc.object();
		for (const char *key : { "summary", "message", "error" }) {
			const QString val = obj.value(QLatin1String(key)).toString().trimmed();
			if (!val.isEmpty()) {
				return (val.size() > maxLen) ? (val.left(maxLen - 3) + QStringLiteral("...")) : val;
			}
		}
		if (obj.contains(QStringLiteral("count"))) {
			return QStringLiteral("count: %1").arg(obj.value(QStringLiteral("count")).toInt());
		}
		obj.remove(QStringLiteral("ok"));
		if (!obj.isEmpty()) {
			const QString compact = QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
			return (compact.size() > maxLen) ? (compact.left(maxLen - 3) + QStringLiteral("...")) : compact;
		}
	}
	return (trimmed.size() > maxLen) ? (trimmed.left(maxLen - 3) + QStringLiteral("...")) : trimmed;
}

QString endpointHostLower(const QString &baseUrl) {
	const AgentEndpointDecision ep = evaluateAgentSpeechEndpoint(baseUrl);
	return ep.allowed ? ep.normalizedUrl.host().toLower() : QString();
}

bool isStandardDashScopeHost(const QUrl &url) {
	return isDashScopeAgentHost(url) && !isBailianPlanAgentHost(url);
}

QString defaultModelForProtocol(AgentSpeechProtocolMode proto) {
	switch (proto) {
	case AgentSpeechProtocolMode::OpenAiRealtimeWs:
		return QStringLiteral("gpt-live-transcribe");
	case AgentSpeechProtocolMode::DashScopeInferenceWs:
		return QStringLiteral("paraformer-realtime-v2");
	case AgentSpeechProtocolMode::DashScopeQwenRealtimeWs:
		return QStringLiteral("qwen3-asr-flash-realtime");
	case AgentSpeechProtocolMode::DeepgramWs:
		return QStringLiteral("nova-3");
	case AgentSpeechProtocolMode::AssemblyAiV3Ws:
		return QStringLiteral("assemblyai-stt-3-7-realtime");
	case AgentSpeechProtocolMode::VolcengineBigModelWs:
		return QStringLiteral("bigmodel");
	case AgentSpeechProtocolMode::ElevenLabsScribeWs:
		return QStringLiteral("scribe_v3_live");
	case AgentSpeechProtocolMode::XaiSttWs:
		return QStringLiteral("grok-voice-transcribe-2.0");
	default:
		return {};
	}
}

QStringList suggestedModelsForStreamingProtocol(AgentSpeechProtocolMode proto) {
	switch (proto) {
	case AgentSpeechProtocolMode::OpenAiRealtimeWs:
		return {
			QStringLiteral("gpt-live-transcribe"),
			QStringLiteral("gpt-realtime-whisper"),
			QStringLiteral("gpt-4o-transcribe"),
			QStringLiteral("gpt-4o-mini-transcribe"),
			QStringLiteral("whisper-1"),
		};
	case AgentSpeechProtocolMode::DashScopeInferenceWs:
		return {
			QStringLiteral("paraformer-realtime-v2"),
			QStringLiteral("fun-asr-realtime"),
			QStringLiteral("paraformer-realtime-8k-v2"),
		};
	case AgentSpeechProtocolMode::DashScopeQwenRealtimeWs:
		return {
			QStringLiteral("qwen3-asr-flash-realtime"),
		};
	case AgentSpeechProtocolMode::DeepgramWs:
		return {
			QStringLiteral("nova-3"),
			QStringLiteral("nova-2"),
		};
	case AgentSpeechProtocolMode::AssemblyAiV3Ws:
		return {
			QStringLiteral("assemblyai-stt-3-7-realtime"),
			QStringLiteral("assemblyai-stt-3-7-realtime-agent"),
			QStringLiteral("universal-3-6-pro"),
			QStringLiteral("u3-rt-pro"),
			QStringLiteral("universal-streaming-multilingual"),
		};
	case AgentSpeechProtocolMode::VolcengineBigModelWs:
		return {
			QStringLiteral("bigmodel"),
		};
	case AgentSpeechProtocolMode::ElevenLabsScribeWs:
		return {
			QStringLiteral("scribe_v3_live"),
			QStringLiteral("scribe_v2_realtime"),
		};
	case AgentSpeechProtocolMode::XaiSttWs:
		return {
			QStringLiteral("grok-voice-transcribe-2.0"),
		};
	default:
		return {};
	}
}

bool modelMatchesStreamingProtocol(const QString &modelId, AgentSpeechProtocolMode proto) {
	QString model = modelId.trimmed().toLower();
	if (model.isEmpty()) {
		return false;
	}
	const int slash = model.lastIndexOf(QLatin1Char('/'));
	if (slash >= 0 && slash + 1 < model.size()) {
		model = model.mid(slash + 1);
	}
	const bool dashInf = model.startsWith(QStringLiteral("paraformer-realtime")) ||
		model.startsWith(QStringLiteral("fun-asr-realtime")) ||
		model.startsWith(QStringLiteral("gummy-realtime")) ||
		(model.contains(QStringLiteral("qwen")) &&
		 model.contains(QStringLiteral("asr")) &&
		 (model.contains(QStringLiteral("stream")) ||
		  model.endsWith(QStringLiteral("-message"))));
	const bool dashQwen = model.contains(QStringLiteral("qwen")) &&
		model.contains(QStringLiteral("asr")) &&
		model.contains(QStringLiteral("realtime"));
	switch (proto) {
	case AgentSpeechProtocolMode::DashScopeInferenceWs:
		return dashInf;
	case AgentSpeechProtocolMode::DashScopeQwenRealtimeWs:
		return dashQwen;
	case AgentSpeechProtocolMode::OpenAiRealtimeWs: {
		if (!model.contains(QStringLiteral("-diarize")) &&
			(model == QStringLiteral("gpt-4o-transcribe") ||
			 model.startsWith(QStringLiteral("gpt-4o-transcribe-")) ||
			 model == QStringLiteral("gpt-4o-mini-transcribe") ||
			 model.startsWith(QStringLiteral("gpt-4o-mini-transcribe-")) ||
			 model.contains(QStringLiteral("-live-transcribe")) ||
			 model.contains(QStringLiteral("-realtime-whisper")) ||
			 model == QStringLiteral("whisper-1"))) {
			return true;
		}
		if (!dashInf && !dashQwen) {
			const AgentSpeechModelInfo info = classifySpeechModel(modelId);
			return info.kind == AgentSpeechModelKind::RealtimeStream && info.dedicatedAsr;
		}
		return false;
	}
	case AgentSpeechProtocolMode::DeepgramWs:
		return model.startsWith(QStringLiteral("nova-")) ||
			model == QStringLiteral("nova") ||
			model == QStringLiteral("enhanced") ||
			model.startsWith(QStringLiteral("enhanced-")) ||
			model == QStringLiteral("base") ||
			model.startsWith(QStringLiteral("base-"));
	case AgentSpeechProtocolMode::AssemblyAiV3Ws:
		return model.startsWith(QStringLiteral("assemblyai-stt-")) ||
			model.startsWith(QStringLiteral("universal-")) ||
			model.startsWith(QStringLiteral("u3-")) ||
			model.startsWith(QStringLiteral("whisper-rt"));
	case AgentSpeechProtocolMode::VolcengineBigModelWs:
		return model.contains(QStringLiteral("bigmodel")) ||
			model.contains(QStringLiteral("seedasr"));
	case AgentSpeechProtocolMode::ElevenLabsScribeWs:
		return model.startsWith(QStringLiteral("scribe")) &&
			(model.contains(QStringLiteral("realtime")) ||
			 model.contains(QStringLiteral("live")));
	case AgentSpeechProtocolMode::XaiSttWs:
		return model == QStringLiteral("grok-stt") ||
			model.startsWith(QStringLiteral("grok-stt-")) ||
			model.startsWith(QStringLiteral("grok-voice-transcribe"));
	default:
		return false;
	}
}

QString fallbackDefaultSpeechModelForHost(const QString &hostLower) {
	if (isDeepgramAgentHost(hostLower)) {
		return QStringLiteral("nova-3");
	}
	if (isAssemblyAiStreamingHost(hostLower)) {
		return QStringLiteral("assemblyai-stt-3-7-realtime");
	}
	if (isVolcengineSpeechHost(hostLower)) {
		return QStringLiteral("bigmodel");
	}
	if (isVolcengineArkHost(hostLower)) {
		return QStringLiteral("doubao-seed-2-1-lite-260915");
	}
	if (isElevenLabsAgentHost(hostLower)) {
		return QStringLiteral("scribe_v3");
	}
	if (isXaiAgentHost(hostLower)) {
		return QStringLiteral("grok-voice-transcribe-2.0");
	}
	if (isGroqAgentHost(hostLower)) {
		return QStringLiteral("whisper-large-v3-turbo");
	}
	if (isOpenAiAgentHost(hostLower)) {
		return QStringLiteral("gpt-transcribe");
	}
	if (isSiliconFlowAgentHost(hostLower)) {
		return QStringLiteral("FunAudioLLM/SenseVoiceSmall");
	}
	if (isDashScopeAgentHost(hostLower)) {
		return QStringLiteral("qwen3-asr-flash");
	}
	return QStringLiteral("whisper-1");
}

QString pickDefaultSpeechModelFromCatalog(
		const QStringList &catalog,
		const QString &hostLower,
		const QHash<QString, AgentModelCapabilities> &caps) {
	const QStringList preferred = [&]() -> QStringList {
		if (isGroqAgentHost(hostLower)) {
			return {
				QStringLiteral("whisper-large-v3-turbo"),
				QStringLiteral("whisper-large-v3"),
			};
		}
		if (isSiliconFlowAgentHost(hostLower)) {
			return {
				QStringLiteral("FunAudioLLM/SenseVoiceSmall"),
				QStringLiteral("TeleAI/TeleSpeechASR"),
			};
		}
		if (isDashScopeAgentHost(hostLower)) {
			return {
				QStringLiteral("qwen3-asr-flash"),
			};
		}
		if (isVolcengineArkHost(hostLower)) {
			return {
				QStringLiteral("doubao-seed-2-1-lite-260915"),
				QStringLiteral("doubao-seed-2-0-lite-260428"),
				QStringLiteral("doubao-seed-2-0-mini-260428"),
			};
		}
		if (isXaiAgentHost(hostLower)) {
			return {
				QStringLiteral("grok-voice-transcribe-2.0"),
				QStringLiteral("grok-stt-2.0"),
				QStringLiteral("grok-stt"),
			};
		}
		return {
			QStringLiteral("gpt-transcribe"),
			QStringLiteral("gpt-4o-mini-transcribe"),
			QStringLiteral("gpt-4o-transcribe"),
			QStringLiteral("whisper-1"),
			QStringLiteral("whisper-large-v3-turbo"),
		};
	}();
	for (const QString &p : preferred) {
		for (const QString &m : catalog) {
			if (m.compare(p, Qt::CaseInsensitive) == 0 &&
				classifySpeechModel(m, hostLower, caps.value(m)).usableForDictation) {
				return m;
			}
		}
	}
	// 专用同步识别模型优先；实时流/异步文件转写/纯音频理解家族由 classifySpeechModel 统一排除。
	for (const QString &m : catalog) {
		const AgentSpeechModelInfo info = classifySpeechModel(m, hostLower, caps.value(m));
		if (info.dedicatedAsr && info.usableForDictation) {
			return m;
		}
	}
	for (const QString &m : catalog) {
		const AgentSpeechModelInfo info = classifySpeechModel(m, hostLower, caps.value(m));
		if (info.catalogMember && info.usableForDictation) {
			return m;
		}
	}
	// 目录中没有可用的同步听写模型时，绝不盲取首项，改为按端点回退安全默认值。
	return fallbackDefaultSpeechModelForHost(hostLower);
}

QByteArray buildSpeechProbeWav() {
	// Decode the embedded ~1.08 s spoken speech clip ("你好测试") to 16 kHz mono 16-bit PCM WAV.
	// Real spoken speech satisfies server-side neural VAD and ASR validators (e.g. Qwen-ASR /
	// DashScope) that reject digital silence or synthetic tones with HTTP 400.
	const std::vector<int16_t> pcm16k = detail::decodeSpeechProbePcm16k();
	return DictationVadSegmenter::encodePcm16ToWav(pcm16k.data(), pcm16k.size(), 16000, 1);
}

} // namespace

AgentCoordinator::AgentCoordinator(
		SessionManager &sessionManager,
		std::unique_ptr<QSettings> settings,
		std::unique_ptr<IAgentTransport> transport,
		QObject *parent)
	: QObject(parent)
	, _sessionManager(sessionManager)
	, _settings(settings ? std::move(settings) : makeDefaultAgentSettings())
	, _secretStore(*_settings)
	, _sessionStore(AgentSessionStore::resolveBaseDirectory(*_settings))
	, _transport(transport ? std::move(transport) : std::make_unique<OpenAiCompatibleTransport>()) {
	_settings->setFallbacksEnabled(false);

	_deltaFlushTimer.setSingleShot(true);
	_deltaFlushTimer.setInterval(kDeltaCoalesceIntervalMs);
	connect(&_deltaFlushTimer, &QTimer::timeout, this, &AgentCoordinator::flushPendingDeltas);

	_persistTimer.setSingleShot(true);
	_persistTimer.setInterval(250);
	connect(&_persistTimer, &QTimer::timeout, this, &AgentCoordinator::flushPersistSessions);

	_speechProbeTimer.setSingleShot(true);
	_speechProbeTimer.setInterval(10000);
	connect(&_speechProbeTimer, &QTimer::timeout, this, [this] {
		if (!_speechProbeStreamSession && !_speechProbePending) {
			return;
		}
		cancelSpeechProbe();
		emit speechProbeFinished(
			false,
			QCoreApplication::translate("MeetingUI", "WebSocket speech connection test timed out."));
	});

	loadProfileFromSettings();
	_currentAccountKey = AgentSessionStore::accountScopeKey(
		_sessionManager.isLoggedIn(),
		_sessionManager.userId());
	loadAccountSessionsFromDisk();

	connect(&_sessionManager, &SessionManager::authenticationReset,
		this, &AgentCoordinator::onAuthSessionChanged);
	connect(&_sessionManager, &SessionManager::loggedIn,
		this, [this](const UserInfo &) { onAuthSessionChanged(); });
	connect(&_sessionManager, &SessionManager::sessionInvalidated,
		this, &AgentCoordinator::onAuthSessionChanged);
}

AgentCoordinator::~AgentCoordinator() {
	_deltaFlushTimer.stop();
	_persistTimer.stop();
	_activeToolRound.reset();
	if (_activeHandle) {
		_activeHandle->disconnect(this);
		_activeHandle->cancel();
	}
	flushPersistSessions();
	cancelProbe();
	cancelSpeechProbe();
	if (_modelsHandle) {
		_modelsHandle->disconnect(this);
		_modelsHandle->cancel();
	}
	if (_sttModelsHandle) {
		_sttModelsHandle->disconnect(this);
		_sttModelsHandle->cancel();
	}
}

void AgentCoordinator::loadProfileFromSettings() {
	_profiles.clear();
	const QStringList storedIds = _settings->value(QLatin1String(kProfilesIdsKey)).toStringList();
	QSet<QString> seenIds;
	for (const QString &rawId : storedIds) {
		const QString cleanId = rawId.trimmed();
		if (cleanId.isEmpty() || seenIds.contains(cleanId)) {
			continue;
		}
		seenIds.insert(cleanId);
		_profiles.push_back(loadSingleProfileFromPrefix(*_settings, profileSettingsPrefix(cleanId), cleanId));
	}
	if (_profiles.isEmpty()) {
		_profiles.push_back(loadSingleProfileFromPrefix(
			*_settings, QLatin1String(kProfilePrefix), QStringLiteral("default")));
	}
	const QString activeId = _settings->value(QLatin1String(kProfilesActiveIdKey)).toString().trimmed();
	_profile = _profiles.front();
	for (const auto &p : _profiles) {
		if (!activeId.isEmpty() && p.id == activeId) {
			_profile = p;
			break;
		}
	}

	_sendMeetingContext = _profile.sendMeetingContextByDefault;
	const int rawMode = _settings->value(QStringLiteral("agent/chatMode"),
		static_cast<int>(AgentChatMode::Agent)).toInt();
	_chatMode = (rawMode == static_cast<int>(AgentChatMode::Ask))
		? AgentChatMode::Ask
		: AgentChatMode::Agent;
	const int rawPerm = _settings->value(QStringLiteral("agent/permissionMode"),
		static_cast<int>(AgentPermissionMode::Manual)).toInt();
	_permissionMode = (rawPerm == static_cast<int>(AgentPermissionMode::ConfirmAll))
		? AgentPermissionMode::ConfirmAll
		: AgentPermissionMode::Manual;

	_speechProfiles.clear();
	const QStringList storedSpeechIds = _settings->value(QLatin1String(kSpeechProfilesIdsKey)).toStringList();
	QSet<QString> seenSpeechIds;
	for (const QString &rawId : storedSpeechIds) {
		const QString cleanId = rawId.trimmed();
		if (cleanId.isEmpty() || seenSpeechIds.contains(cleanId)) {
			continue;
		}
		seenSpeechIds.insert(cleanId);
		_speechProfiles.push_back(loadSingleSpeechProfileFromPrefix(
			*_settings, speechProfileSettingsPrefix(cleanId), cleanId));
	}
	if (_speechProfiles.isEmpty()) {
		const QString legacyUrl = _settings->value(QStringLiteral("agent/dictationSttBaseUrl")).toString().trimmed();
		const AgentSpeechProtocolMode legacyProto = agentSpeechProtocolModeFromInt(
			_settings->value(
				QStringLiteral("agent/dictationSpeechProtocol"),
				static_cast<int>(AgentSpeechProtocolMode::Auto)).toInt());
		const QString legacyModel = _settings->value(QStringLiteral("agent/dictationSttModel")).toString().trimmed();
		QString legacyResourceId = _settings->value(
			QStringLiteral("agent/dictationSttResourceId"),
			QLatin1String(kDefaultSpeechResourceId)).toString().trimmed();
		if (legacyResourceId.isEmpty()) {
			legacyResourceId = QLatin1String(kDefaultSpeechResourceId);
		}
		const QString legacyWorkspace = _settings->value(QStringLiteral("agent/dictationSttWorkspace")).toString().trimmed();

		const bool hasCustomUrl = !legacyUrl.isEmpty();
		const bool hasLegacySttKey = _secretStore.hasApiKey(
			QStringLiteral("default_stt"), KeyStoragePolicy::EncryptedStore);
		const bool hasLegacyAppKey = _secretStore.hasApiKey(
			QStringLiteral("default_stt_app"), KeyStoragePolicy::EncryptedStore);
		const bool isDedicatedThirdParty =
			legacyProto == AgentSpeechProtocolMode::DeepgramWs ||
			legacyProto == AgentSpeechProtocolMode::AssemblyAiV3Ws ||
			legacyProto == AgentSpeechProtocolMode::VolcengineBigModelWs ||
			legacyProto == AgentSpeechProtocolMode::ElevenLabsScribeWs;

		AgentSpeechProfile initial;
		initial.id = QStringLiteral("default_stt");
		initial.resourceId = legacyResourceId;
		initial.workspaceId = legacyWorkspace;
		if (hasCustomUrl || hasLegacySttKey || hasLegacyAppKey || isDedicatedThirdParty) {
			_speechSourceMode = AgentSpeechSourceMode::CustomProfile;
			initial.name = inferLegacySpeechProfileName(legacyUrl, legacyProto);
			initial.protocol = legacyProto;
			initial.baseUrl = legacyUrl;
			initial.model = legacyModel;
			_followChatSpeechProtocol = AgentSpeechProtocolMode::Auto;
			_followChatSpeechModel.clear();
		} else {
			_speechSourceMode = AgentSpeechSourceMode::FollowChat;
			initial.name = QCoreApplication::translate("MeetingUI", "Default Speech");
			initial.protocol = AgentSpeechProtocolMode::Auto;
			initial.baseUrl.clear();
			initial.model.clear();
			_followChatSpeechProtocol = legacyProto;
			_followChatSpeechModel = legacyModel;
		}
		initial = normalizeSpeechProfileValues(initial, QStringLiteral("default_stt"));
		_speechProfiles.push_back(initial);
		_speechProfile = initial;
	} else {
		_speechSourceMode = agentSpeechSourceModeFromInt(
			_settings->value(
				QLatin1String(kSpeechSourceModeKey),
				static_cast<int>(AgentSpeechSourceMode::FollowChat)).toInt());
		_followChatSpeechProtocol = agentSpeechProtocolModeFromInt(
			_settings->value(
				QLatin1String(kSpeechFollowProtocolKey),
				static_cast<int>(AgentSpeechProtocolMode::Auto)).toInt());
		_followChatSpeechModel = _settings->value(QLatin1String(kSpeechFollowModelKey)).toString().trimmed();
		const QString activeSpeechId = _settings->value(QLatin1String(kSpeechProfilesActiveIdKey)).toString().trimmed();
		_speechProfile = _speechProfiles.front();
		for (const auto &sp : _speechProfiles) {
			if (!activeSpeechId.isEmpty() && sp.id == activeSpeechId) {
				_speechProfile = sp;
				break;
			}
		}
	}

	_dictationLanguage = _settings->value(QStringLiteral("agent/dictationLanguage")).toString().trimmed();
	_dictationPrompt = _settings->value(QStringLiteral("agent/dictationPrompt")).toString().trimmed().left(1000);
	_dictationSpeechTimeoutMs = qBound(0, _settings->value(QStringLiteral("agent/dictationSpeechTimeoutMs"), 0).toInt(), 10000);
	_dictationVadThresholdDb = qBound(-60, _settings->value(QStringLiteral("agent/dictationVadThresholdDb"), -42).toInt(), -15);
	_dictationVadSilenceCommitMs = qBound(200, _settings->value(QStringLiteral("agent/dictationVadSilenceCommitMs"), 600).toInt(), 3000);
	_dictationDeviceId = _settings->value(QStringLiteral("agent/dictationDeviceId")).toString().trimmed();
	const auto loaded = _secretStore.loadApiKey(_profile.id, _profile.keyStorage);
	_lastSecretStatus = loaded.status;
}

void AgentCoordinator::saveProfileToSettings() const {
	QStringList ids;
	ids.reserve(_profiles.size());
	for (const auto &p : _profiles) {
		if (!p.id.trimmed().isEmpty()) {
			ids.push_back(p.id.trimmed());
			saveSingleProfileToPrefix(*_settings, profileSettingsPrefix(p.id.trimmed()), p);
		}
	}
	_settings->setValue(QLatin1String(kProfilesIdsKey), ids);
	_settings->setValue(QLatin1String(kProfilesActiveIdKey), _profile.id);
	saveSingleProfileToPrefix(*_settings, QLatin1String(kProfilePrefix), _profile);
	_settings->sync();
}

void AgentCoordinator::saveSpeechProfilesToSettings() const {
	QStringList ids;
	ids.reserve(_speechProfiles.size());
	for (const auto &sp : _speechProfiles) {
		const QString cleanId = sp.id.trimmed();
		if (!cleanId.isEmpty()) {
			ids.push_back(cleanId);
			saveSingleSpeechProfileToPrefix(*_settings, speechProfileSettingsPrefix(cleanId), sp);
		}
	}
	_settings->setValue(QLatin1String(kSpeechProfilesIdsKey), ids);
	_settings->setValue(QLatin1String(kSpeechProfilesActiveIdKey), _speechProfile.id);
	_settings->setValue(QLatin1String(kSpeechSourceModeKey), static_cast<int>(_speechSourceMode));
	_settings->setValue(QLatin1String(kSpeechFollowProtocolKey), static_cast<int>(_followChatSpeechProtocol));
	_settings->setValue(QLatin1String(kSpeechFollowModelKey), _followChatSpeechModel.trimmed());
	_settings->sync();
}

void AgentCoordinator::setProfile(const AgentProfile &profile) {
	const KeyStoragePolicy previousPolicy = _profile.keyStorage;
	const bool previousPersist = _profile.persistChatHistory;
	const QString previousId = _profile.id;
	const QString previousName = _profile.name;
	const int previousCount = _profiles.size();

	const AgentProfile next = normalizeProfileValues(profile, _profile.id);

	bool foundExisting = false;
	bool replacedSingleProfile = false;
	for (int i = 0; i < _profiles.size(); ++i) {
		if (_profiles[i].id == next.id) {
			_profiles[i] = next;
			foundExisting = true;
			break;
		}
	}
	if (!foundExisting) {
		if (_profiles.size() <= 1) {
			if (_profiles.isEmpty()) {
				_profiles.push_back(next);
			} else {
				_profiles[0] = next;
			}
			replacedSingleProfile = true;
		} else {
			_profiles.push_back(next);
		}
	}

	const QString previousBaseUrl = _profile.baseUrl;
	const bool changed = (_profile != next);
	const bool connectionChanged = !_profile.hasSameConnectionConfig(next);
	const bool endpointOrProtocolChanged =
		_profile.id != next.id ||
		_profile.protocol != next.protocol ||
		_profile.baseUrl != next.baseUrl ||
		_profile.customHeaders != next.customHeaders ||
		_profile.keyStorage != next.keyStorage;
	const bool speechSharedConfigChanged =
		_profile.id != next.id ||
		_profile.baseUrl != next.baseUrl ||
		_profile.customHeaders != next.customHeaders;
	const QString effectiveSttUrl = dictationSttBaseUrl();
	const bool speechUsesChatHost =
		effectiveSttUrl.isEmpty() ||
		isSameSpeechOrigin(effectiveSttUrl, previousBaseUrl) ||
		isSameSpeechOrigin(effectiveSttUrl, next.baseUrl);
	const bool listMetaChanged =
		(previousId != next.id) ||
		(previousName != next.name) ||
		(previousCount != _profiles.size());
	_profile = next;
	_sendMeetingContext = _profile.sendMeetingContextByDefault;
	if (connectionChanged) {
		cancelProbe();
	}
	if (speechSharedConfigChanged && speechUsesChatHost) {
		cancelSpeechProbe();
	}
	if (endpointOrProtocolChanged) {
		if (_modelsHandle) {
			_modelsHandle->disconnect(this);
			_modelsHandle->cancel();
			_modelsHandle->deleteLater();
			_modelsHandle.clear();
		}
		_cachedModels.clear();
		_cachedSpeechModels.clear();
		_modelCapabilities.clear();
		_lastFilteredNonChatModelCount = 0;
	}

	// If switching from EncryptedStore to MemoryOnly while a key exists, purge disk blob
	// while keeping the in-session memory secret (Chat key for the edited profile, and speech profile keys
	// when editing storage policy of the same profile or legacy single-profile rename).
	if (previousId == _profile.id || replacedSingleProfile) {
		const auto migratedStatus = migrateSecretStoragePolicy(previousId, _profile.id, previousPolicy, _profile.keyStorage);
		if (previousPolicy != _profile.keyStorage || replacedSingleProfile) {
			_lastSecretStatus = migratedStatus;
		}
		if (previousPolicy != _profile.keyStorage) {
			for (const auto &sp : _speechProfiles) {
				const QString spId = sp.id.trimmed();
				if (!spId.isEmpty()) {
					migrateSecretStoragePolicy(spId, spId, previousPolicy, _profile.keyStorage);
					const QString appId = spId + QStringLiteral("_app");
					migrateSecretStoragePolicy(appId, appId, previousPolicy, _profile.keyStorage);
				}
			}
		}
	} else {
		_lastSecretStatus = _secretStore.loadApiKey(_profile.id, _profile.keyStorage).status;
	}

	if (replacedSingleProfile && previousId != next.id && !previousId.trimmed().isEmpty()) {
		_settings->remove(QStringLiteral("agent/profiles/") + previousId.trimmed());
	}
	saveProfileToSettings();
	if (previousPersist && !_profile.persistChatHistory) {
		_persistTimer.stop();
		_sessionStore.clearAccountSessions(_currentAccountKey);
	} else if (!previousPersist && _profile.persistChatHistory) {
		flushPersistSessions();
	}
	if (listMetaChanged) {
		emit profilesChanged();
	}
	if (changed) {
		emit profileChanged();
		emit contextStateChanged();
	}
}

std::optional<AgentProfile> AgentCoordinator::findProfile(const QString &profileId) const {
	const QString cleanId = profileId.trimmed();
	if (cleanId.isEmpty()) {
		return std::nullopt;
	}
	for (const auto &p : _profiles) {
		if (p.id == cleanId) {
			return p;
		}
	}
	return std::nullopt;
}

QString AgentCoordinator::createProfile(const QString &displayName, const AgentProfile *templateProfile) {
	const QString previousBaseUrl = _profile.baseUrl;
	QString newId;
	do {
		newId = QStringLiteral("profile_%1")
			.arg(QUuid::createUuid().toString(QUuid::Id128).left(8));
	} while (findProfile(newId).has_value());

	AgentProfile created = templateProfile ? *templateProfile : AgentProfile{};
	created.id = newId;
	if (!templateProfile) {
		created.systemPrompt = defaultSystemPrompt();
	}
	const QString trimmedName = displayName.trimmed();
	if (!trimmedName.isEmpty()) {
		created.name = trimmedName;
	} else {
		QSet<QString> existingNames;
		for (const auto &p : _profiles) {
			existingNames.insert(p.name.trimmed());
		}
		int n = 2;
		QString candidate;
		do {
			candidate = QCoreApplication::translate("MeetingUI", "Profile %1").arg(n++);
		} while (existingNames.contains(candidate));
		created.name = candidate;
	}
	created = normalizeProfileValues(created, newId);

	_profiles.push_back(created);
	_profile = created;
	_sendMeetingContext = _profile.sendMeetingContextByDefault;

	cancelProbe();
	const QString effectiveSttUrl = dictationSttBaseUrl();
	if (effectiveSttUrl.isEmpty() ||
		isSameSpeechOrigin(effectiveSttUrl, previousBaseUrl) ||
		isSameSpeechOrigin(effectiveSttUrl, _profile.baseUrl)) {
		cancelSpeechProbe();
	}
	if (_modelsHandle) {
		_modelsHandle->disconnect(this);
		_modelsHandle->cancel();
		_modelsHandle->deleteLater();
		_modelsHandle.clear();
	}
	_cachedModels.clear();
	_cachedSpeechModels.clear();
	_modelCapabilities.clear();
	_lastFilteredNonChatModelCount = 0;

	_lastSecretStatus = _secretStore.loadApiKey(_profile.id, _profile.keyStorage).status;
	saveProfileToSettings();
	emit profilesChanged();
	emit profileChanged();
	emit contextStateChanged();
	return newId;
}

bool AgentCoordinator::switchProfile(const QString &profileId) {
	const auto targetOpt = findProfile(profileId);
	if (!targetOpt.has_value()) {
		return false;
	}
	if (_profile.id == targetOpt->id) {
		return true;
	}

	const QString previousBaseUrl = _profile.baseUrl;
	const AgentProfile next = normalizeProfileValues(*targetOpt, targetOpt->id);
	_profile = next;
	_sendMeetingContext = _profile.sendMeetingContextByDefault;

	cancelProbe();
	const QString effectiveSttUrl = dictationSttBaseUrl();
	if (effectiveSttUrl.isEmpty() ||
		isSameSpeechOrigin(effectiveSttUrl, previousBaseUrl) ||
		isSameSpeechOrigin(effectiveSttUrl, _profile.baseUrl)) {
		cancelSpeechProbe();
	}
	if (_modelsHandle) {
		_modelsHandle->disconnect(this);
		_modelsHandle->cancel();
		_modelsHandle->deleteLater();
		_modelsHandle.clear();
	}
	_cachedModels.clear();
	_cachedSpeechModels.clear();
	_modelCapabilities.clear();
	_lastFilteredNonChatModelCount = 0;

	_lastSecretStatus = _secretStore.loadApiKey(_profile.id, _profile.keyStorage).status;
	saveProfileToSettings();
	emit profileChanged();
	emit contextStateChanged();
	return true;
}

bool AgentCoordinator::deleteProfile(const QString &profileId) {
	if (_profiles.size() <= 1) {
		return false;
	}
	const QString cleanId = profileId.trimmed();
	int idx = -1;
	for (int i = 0; i < _profiles.size(); ++i) {
		if (_profiles[i].id == cleanId) {
			idx = i;
			break;
		}
	}
	if (idx < 0) {
		return false;
	}

	const bool deletingActive = (_profile.id == cleanId);
	const QString previousBaseUrl = _profile.baseUrl;
	_secretStore.clearApiKey(cleanId);
	_settings->remove(QStringLiteral("agent/profiles/") + cleanId);
	_profiles.removeAt(idx);

	if (deletingActive) {
		_profile = normalizeProfileValues(_profiles.front(), _profiles.front().id);
		_sendMeetingContext = _profile.sendMeetingContextByDefault;
		cancelProbe();
		const QString effectiveSttUrl = dictationSttBaseUrl();
		if (effectiveSttUrl.isEmpty() ||
			isSameSpeechOrigin(effectiveSttUrl, previousBaseUrl) ||
			isSameSpeechOrigin(effectiveSttUrl, _profile.baseUrl)) {
			cancelSpeechProbe();
		}
		if (_modelsHandle) {
			_modelsHandle->disconnect(this);
			_modelsHandle->cancel();
			_modelsHandle->deleteLater();
			_modelsHandle.clear();
		}
		_cachedModels.clear();
		_cachedSpeechModels.clear();
		_modelCapabilities.clear();
		_lastFilteredNonChatModelCount = 0;
		_lastSecretStatus = _secretStore.loadApiKey(_profile.id, _profile.keyStorage).status;
	}

	saveProfileToSettings();
	emit profilesChanged();
	if (deletingActive) {
		emit profileChanged();
		emit contextStateChanged();
	}
	return true;
}

bool AgentCoordinator::isTranslationTargetConfigured(
		const QString &profileIdOverride,
		const QString &modelOverride) const {
	AgentProfile target = _profile;
	if (!profileIdOverride.trimmed().isEmpty()) {
		if (const auto found = findProfile(profileIdOverride)) {
			target = *found;
		}
	}
	const QString effectiveModel = modelOverride.trimmed().isEmpty()
		? target.model.trimmed()
		: modelOverride.trimmed();
	return !effectiveModel.isEmpty() && evaluateAgentEndpoint(target.baseUrl).allowed;
}

bool AgentCoordinator::resolveTranslationChatTarget(
		const QString &profileIdOverride,
		const QString &modelOverride,
		AgentProfile *outProfile,
		QString *outApiKey,
		QString *outError) {
	if (outError) {
		outError->clear();
	}
	AgentProfile target = _profile;
	if (!profileIdOverride.trimmed().isEmpty()) {
		if (const auto found = findProfile(profileIdOverride)) {
			target = *found;
		}
	}
	if (!modelOverride.trimmed().isEmpty()) {
		target.model = modelOverride.trimmed();
	}
	if (target.model.trimmed().isEmpty() || !evaluateAgentEndpoint(target.baseUrl).allowed) {
		if (outError) {
			*outError = QCoreApplication::translate(
				"MeetingUI", "Configure Copilot Base URL and Model in Settings to enable translation.");
		}
		return false;
	}
	const auto loaded = _secretStore.loadApiKey(target.id, target.keyStorage);
	if (loaded.status == AgentSecretStore::Status::Corrupted) {
		if (outError) {
			*outError = QCoreApplication::translate(
				"MeetingUI",
				"Saved API key could not be decrypted. Please re-enter it in Settings > Copilot.");
		}
		return false;
	}
	if (loaded.status == AgentSecretStore::Status::StorageUnavailable) {
		if (outError) {
			*outError = QCoreApplication::translate(
				"MeetingUI",
				"Saved API key could not be accessed (OS encrypted storage is locked or unavailable). Please unlock your keyring or re-enter it in Settings > Copilot.");
		}
		return false;
	}
	if (outProfile) {
		*outProfile = std::move(target);
	}
	if (outApiKey) {
		*outApiKey = loaded.apiKey.trimmed();
	}
	return true;
}

bool AgentCoordinator::isConfigured() const {
	return !_profile.model.trimmed().isEmpty() &&
		evaluateAgentEndpoint(_profile.baseUrl).allowed;
}

bool AgentCoordinator::hasApiKey() const {
	return _secretStore.hasApiKey(_profile.id, _profile.keyStorage);
}

bool AgentCoordinator::isApiKeyPersisted() const {
	return _secretStore.isPersistedOnDisk(_profile.id, _profile.keyStorage);
}

AgentSecretStore::Status AgentCoordinator::saveApiKey(const QString &apiKey) {
	cancelProbe();
	const QString effectiveSttUrl = dictationSttBaseUrl();
	if (!hasDictationApiKey() &&
		(effectiveSttUrl.isEmpty() || isSameAgentOrigin(effectiveSttUrl, _profile.baseUrl))) {
		cancelSpeechProbe();
	}
	_lastSecretStatus = _secretStore.saveApiKey(_profile.id, apiKey, _profile.keyStorage);
	emit profileChanged();
	return _lastSecretStatus;
}

AgentSecretStore::Status AgentCoordinator::clearApiKey() {
	cancelProbe();
	const QString effectiveSttUrl = dictationSttBaseUrl();
	if (!hasDictationApiKey() &&
		(effectiveSttUrl.isEmpty() || isSameAgentOrigin(effectiveSttUrl, _profile.baseUrl))) {
		cancelSpeechProbe();
	}
	_lastSecretStatus = _secretStore.clearApiKey(_profile.id);
	emit profileChanged();
	return _lastSecretStatus;
}

MeetingCoordinator *AgentCoordinator::attachedMeeting() const {
	return _meeting;
}

MeetingCatalogController *AgentCoordinator::attachedCatalog() const {
	return _catalog;
}

void AgentCoordinator::setHostSettingsBridge(AgentHostSettingsBridge bridge) {
	_hostSettings = std::move(bridge);
}

void AgentCoordinator::attachCatalog(MeetingCatalogController *catalog) {
	if (_catalog == catalog) {
		return;
	}
	for (const auto &conn : _catalogConnections) {
		disconnect(conn);
	}
	_catalogConnections.clear();
	_inFlightActionByGroup.remove(static_cast<int>(AgentConcurrencyGroup::CatalogWrite));
	_catalog = catalog;

	if (_catalog) {
		_catalogConnections.push_back(connect(_catalog, &MeetingCatalogController::writeStateChanged,
			this, &AgentCoordinator::onCatalogWriteStateChanged));
		_catalogConnections.push_back(connect(_catalog, &QObject::destroyed,
			this, [this] {
				_catalog.clear();
				_inFlightActionByGroup.remove(static_cast<int>(AgentConcurrencyGroup::CatalogWrite));
			}));
	}
}

void AgentCoordinator::attachContacts(
		ContactCatalogController *contacts,
		PresenceWatchController *presence,
		CallInviteController *callInvite) {
	if (_contacts == contacts && _presence == presence && _callInvite == callInvite) {
		return;
	}
	for (const auto &conn : _contactConnections) {
		disconnect(conn);
	}
	_contactConnections.clear();
	_inFlightActionByGroup.remove(static_cast<int>(AgentConcurrencyGroup::ContactWrite));
	_contacts = contacts;
	_presence = presence;
	_callInvite = callInvite;

	if (_contacts) {
		_contactConnections.push_back(connect(_contacts, &QObject::destroyed,
			this, [this] {
				_contacts.clear();
				_inFlightActionByGroup.remove(static_cast<int>(AgentConcurrencyGroup::ContactWrite));
			}));
	}
	if (_presence) {
		_contactConnections.push_back(connect(_presence, &QObject::destroyed,
			this, [this] {
				_presence.clear();
			}));
	}
	if (_callInvite) {
		_contactConnections.push_back(connect(_callInvite, &QObject::destroyed,
			this, [this] {
				_callInvite.clear();
				_inFlightActionByGroup.remove(static_cast<int>(AgentConcurrencyGroup::ContactWrite));
			}));
	}
}

void AgentCoordinator::attachMeeting(MeetingCoordinator *coordinator) {
	if (_meeting == coordinator) {
		return;
	}

	for (const auto &conn : _meetingConnections) {
		disconnect(conn);
	}
	_meetingConnections.clear();

	if (_activeHandle && _activeRequestToken.coordinatorSession != 0) {
		cancelActiveRequestWithReason(AgentChatMessage::Status::Cancelled,
			QStringLiteral("Meeting context changed."));
	}
	expireAllPendingActions(QCoreApplication::translate("MeetingUI", "Meeting session changed."), true);

	++_meetingAttachmentGeneration;
	_meeting = coordinator;
	_e2eeAuthorized = false;
	_e2eeAuthorizedMeetingId.clear();
	_e2eeAuthorizedSession = 0;
	_recentMeetingChat.clear();
	_recentTranscripts.clear();
	_latestTelemetrySnapshot.clear();

	if (_meeting) {
		for (const auto &t : _meeting->recentTranscripts()) {
			AgentMeetingTranscriptEntry entry;
			entry.segmentId = t.segmentId;
			entry.speakerIdentity = t.speakerIdentity;
			entry.speakerName = t.speakerName;
			entry.text = t.text;
			entry.language = t.language;
			entry.timestampMs = t.timestampMs;
			entry.isFinal = t.isFinal;
			entry.isLocalSpeaker = t.isLocalSpeaker;
			entry.isAgentGenerated = t.isAgentGenerated;
			recordMeetingTranscript(entry);
		}
		_meetingConnections.push_back(connect(_meeting, &MeetingCoordinator::sessionStopping,
			this, &AgentCoordinator::onMeetingEnded));
		_meetingConnections.push_back(connect(_meeting, &MeetingCoordinator::meetingLeft,
			this, &AgentCoordinator::onMeetingEnded));
		_meetingConnections.push_back(connect(_meeting, &MeetingCoordinator::stateChanged,
			this, [this] { emit contextStateChanged(); }));
		_meetingConnections.push_back(connect(_meeting, &MeetingCoordinator::chatMessageReceived,
			this, [this](const QString &, const QString &senderName, const QString &message, int64_t) {
				recordMeetingChatMessage(senderName, message, false);
			}));
		_meetingConnections.push_back(connect(_meeting, &MeetingCoordinator::transcriptionReceived,
			this, [this](const MeetingTranscriptItem &t) {
				AgentMeetingTranscriptEntry entry;
				entry.segmentId = t.segmentId;
				entry.speakerIdentity = t.speakerIdentity;
				entry.speakerName = t.speakerName;
				entry.text = t.text;
				entry.language = t.language;
				entry.timestampMs = t.timestampMs;
				entry.isFinal = t.isFinal;
				entry.isLocalSpeaker = t.isLocalSpeaker;
				entry.isAgentGenerated = t.isAgentGenerated;
				recordMeetingTranscript(entry);
			}));
		_meetingConnections.push_back(connect(_meeting, &MeetingCoordinator::telemetrySnapshotChanged,
			this, &AgentCoordinator::updateTelemetrySnapshot));
		_meetingConnections.push_back(connect(_meeting, &QObject::destroyed,
			this, &AgentCoordinator::onMeetingEnded));
	}
	emit contextStateChanged();
}

bool AgentCoordinator::isInMeeting() const {
	return _meeting && _meeting->state() != MeetingState::Idle;
}

bool AgentCoordinator::isEncryptedMeeting() const {
	return isInMeeting() && _meeting->requiresEncryption();
}

bool AgentCoordinator::isE2eeContextAuthorized() const {
	if (!isEncryptedMeeting()) {
		return true;
	}
	return _e2eeAuthorized &&
		_e2eeAuthorizedSession == _meetingAttachmentGeneration &&
		_e2eeAuthorizedMeetingId == _meeting->currentMeetingId();
}

void AgentCoordinator::authorizeE2eeContextForCurrentMeeting(bool authorized) {
	if (!_meeting) {
		_e2eeAuthorized = false;
		_e2eeAuthorizedMeetingId.clear();
		_e2eeAuthorizedSession = 0;
		emit contextStateChanged();
		return;
	}
	_e2eeAuthorized = authorized;
	_e2eeAuthorizedSession = authorized ? _meetingAttachmentGeneration : 0;
	_e2eeAuthorizedMeetingId = authorized ? _meeting->currentMeetingId() : QString();
	emit contextStateChanged();
}

void AgentCoordinator::setContextSendingEnabled(bool enabled) {
	if (_sendMeetingContext == enabled) {
		return;
	}
	_sendMeetingContext = enabled;
	emit contextStateChanged();
}

void AgentCoordinator::setChatMode(AgentChatMode mode) {
	if (_chatMode == mode) {
		return;
	}
	_chatMode = mode;
	_settings->setValue(QStringLiteral("agent/chatMode"), static_cast<int>(_chatMode));
	const int activeIdx = indexOfSession(_activeSessionId);
	if (activeIdx >= 0) {
		_sessions[activeIdx].summary.chatMode = _chatMode;
		schedulePersistSessions();
	}
	emit chatModeChanged(_chatMode);
}

void AgentCoordinator::setPermissionMode(AgentPermissionMode mode) {
	if (_permissionMode == mode) {
		return;
	}
	_permissionMode = mode;
	_settings->setValue(QStringLiteral("agent/permissionMode"), static_cast<int>(_permissionMode));
	emit permissionModeChanged(_permissionMode);
}

bool AgentCoordinator::switchModel(const QString &modelId) {
	const QString clean = modelId.trimmed();
	if (clean.isEmpty() || _profile.model == clean) {
		return false;
	}
	AgentProfile next = _profile;
	next.model = clean;
	setProfile(next);
	return true;
}

bool AgentCoordinator::isSameAgentOrigin(const QString &baseUrlA, const QString &baseUrlB) {
	const AgentEndpointDecision epA = evaluateAgentEndpoint(baseUrlA);
	const AgentEndpointDecision epB = evaluateAgentEndpoint(baseUrlB);
	if (!epA.allowed || !epB.allowed) {
		return false;
	}
	return epA.normalizedUrl.scheme() == epB.normalizedUrl.scheme() &&
		epA.normalizedUrl.host().compare(epB.normalizedUrl.host(), Qt::CaseInsensitive) == 0 &&
		epA.normalizedUrl.port() == epB.normalizedUrl.port();
}

bool AgentCoordinator::isSameSpeechOrigin(const QString &urlA, const QString &urlB) {
	const AgentEndpointDecision epA = evaluateAgentSpeechEndpoint(urlA);
	const AgentEndpointDecision epB = evaluateAgentSpeechEndpoint(urlB);
	if (!epA.allowed || !epB.allowed) {
		return false;
	}
	auto schemeSecure = [](const QUrl &u) {
		const QString s = u.scheme().toLower();
		return s == QStringLiteral("https") || s == QStringLiteral("wss");
	};
	auto effectivePort = [&](const QUrl &u) {
		const int p = u.port(-1);
		return (p > 0) ? p : (schemeSecure(u) ? 443 : 80);
	};
	return schemeSecure(epA.normalizedUrl) == schemeSecure(epB.normalizedUrl) &&
		epA.normalizedUrl.host().compare(epB.normalizedUrl.host(), Qt::CaseInsensitive) == 0 &&
		effectivePort(epA.normalizedUrl) == effectivePort(epB.normalizedUrl);
}

QString AgentCoordinator::canonicalSpeechEndpointKey(const QString &baseUrl) {
	const AgentEndpointDecision ep = evaluateAgentSpeechEndpoint(baseUrl);
	if (!ep.allowed) {
		return {};
	}
	QUrl url = ep.normalizedUrl;
	url.setQuery(QString());
	QString path = url.path();
	while (path.endsWith(QLatin1Char('/')) && path.size() > 1) {
		path.chop(1);
	}
	for (const QLatin1String suffix : {
			 QLatin1String("/audio/transcriptions"),
			 QLatin1String("/transcriptions"),
		 }) {
		if (path.endsWith(suffix, Qt::CaseInsensitive)) {
			path.chop(suffix.size());
			break;
		}
	}
	if (path == QStringLiteral("/")) {
		path.clear();
	}
	url.setPath(path);
	QString key = url.toString(QUrl::FullyEncoded);
	while (key.endsWith(QLatin1Char('/'))) {
		key.chop(1);
	}
	return key;
}

QStringList AgentCoordinator::cachedExternalSpeechModelsForUrl(const QString &sttBaseUrl) const {
	const QString key = canonicalSpeechEndpointKey(sttBaseUrl);
	if (key.isEmpty() || key != _cachedExternalSpeechKey) {
		return {};
	}
	return _cachedExternalSpeechModels;
}

QHash<QString, AgentModelCapabilities> AgentCoordinator::cachedExternalSpeechCapabilitiesForUrl(
		const QString &sttBaseUrl) const {
	const QString key = canonicalSpeechEndpointKey(sttBaseUrl);
	if (key.isEmpty() || key != _cachedExternalSpeechKey) {
		return {};
	}
	return _externalSttModelCapabilities;
}

QStringList AgentCoordinator::suggestedSpeechModelsForEndpoint(
		const QString &sttBaseUrl,
		const QString &chatBaseUrl,
		const QStringList &chatCachedSpeechModels,
		const QStringList &externalCachedSpeechModels,
		AgentSpeechProtocolMode protocolMode) {
	const QString cleanStt = sttBaseUrl.trimmed();
	const QString cleanChat = chatBaseUrl.trimmed();
	const AgentSpeechProtocolMode urlAutoProto = resolveEffectiveSpeechProtocol(
		AgentSpeechProtocolMode::Auto, cleanStt, cleanChat, QString());
	const AgentSpeechProtocolMode effectiveStreamingProto = isStreamingSpeechProtocol(protocolMode)
		? protocolMode
		: ((protocolMode == AgentSpeechProtocolMode::Auto && isStreamingSpeechProtocol(urlAutoProto))
			? urlAutoProto
			: AgentSpeechProtocolMode::Auto);

	QString effectiveUrl = cleanStt.isEmpty() ? cleanChat : cleanStt;
	if (cleanStt.isEmpty() && isStreamingSpeechProtocol(effectiveStreamingProto)) {
		if (effectiveStreamingProto == AgentSpeechProtocolMode::DashScopeInferenceWs ||
			effectiveStreamingProto == AgentSpeechProtocolMode::DashScopeQwenRealtimeWs) {
			effectiveUrl = isStandardDashScopeHost(QUrl(cleanChat))
				? cleanChat
				: defaultStreamingSpeechEndpoint(effectiveStreamingProto);
		} else if (effectiveStreamingProto == AgentSpeechProtocolMode::XaiSttWs) {
			effectiveUrl = isXaiAgentHost(QUrl(cleanChat))
				? cleanChat
				: defaultStreamingSpeechEndpoint(effectiveStreamingProto);
		} else if (effectiveStreamingProto == AgentSpeechProtocolMode::OpenAiRealtimeWs) {
			effectiveUrl = !cleanChat.isEmpty()
				? cleanChat
				: defaultStreamingSpeechEndpoint(effectiveStreamingProto);
		} else {
			effectiveUrl = defaultStreamingSpeechEndpoint(effectiveStreamingProto);
		}
	}

	const bool externalEndpoint = !cleanStt.isEmpty()
		? !isSameSpeechOrigin(cleanStt, cleanChat)
		: (isStreamingSpeechProtocol(effectiveStreamingProto) &&
			(effectiveUrl.isEmpty() || !isSameSpeechOrigin(effectiveUrl, cleanChat)));
	const QStringList &discovered = externalEndpoint
		? externalCachedSpeechModels
		: chatCachedSpeechModels;
	const QString host = endpointHostLower(effectiveUrl);

	if (isStreamingSpeechProtocol(effectiveStreamingProto)) {
		QStringList filtered;
		for (const QString &m : discovered) {
			if (modelMatchesStreamingProtocol(m, effectiveStreamingProto)) {
				filtered.push_back(m);
			}
		}
		if (!filtered.isEmpty()) {
			return filtered;
		}
		const QStringList protoModels = suggestedModelsForStreamingProtocol(effectiveStreamingProto);
		if (!protoModels.isEmpty()) {
			return protoModels;
		}
	} else if (protocolMode != AgentSpeechProtocolMode::Auto) {
		QStringList filtered;
		for (const QString &m : discovered) {
			if (classifySpeechModel(m, host).usableForDictation) {
				filtered.push_back(m);
			}
		}
		if (!discovered.isEmpty()) {
			return filtered;
		}
		if (!externalEndpoint && host.isEmpty()) {
			return {};
		}
	} else {
		if (!discovered.isEmpty()) {
			return discovered;
		}
		if (!externalEndpoint && host.isEmpty()) {
			return {};
		}
	}

	if (isGroqAgentHost(host)) {
		return {
			QStringLiteral("whisper-large-v3-turbo"),
			QStringLiteral("whisper-large-v3"),
		};
	}
	if (isOpenAiAgentHost(host)) {
		return {
			QStringLiteral("gpt-transcribe"),
			QStringLiteral("gpt-live-transcribe"),
			QStringLiteral("gpt-4o-mini-transcribe"),
			QStringLiteral("gpt-4o-transcribe"),
			QStringLiteral("whisper-1"),
		};
	}
	if (isSiliconFlowAgentHost(host)) {
		return {
			QStringLiteral("FunAudioLLM/SenseVoiceSmall"),
			QStringLiteral("TeleAI/TeleSpeechASR"),
		};
	}
	if (isDashScopeAgentHost(host)) {
		return {
			QStringLiteral("qwen3-asr-flash"),
		};
	}
	if (isVolcengineArkHost(host)) {
		return {
			QStringLiteral("doubao-seed-2-1-lite-260915"),
			QStringLiteral("doubao-seed-2-0-lite-260428"),
			QStringLiteral("doubao-seed-2-0-mini-260428"),
		};
	}
	if (isXaiAgentHost(host)) {
		return {
			QStringLiteral("grok-voice-transcribe-2.0"),
		};
	}
	if (isElevenLabsAgentHost(host)) {
		return {
			QStringLiteral("scribe_v3"),
			QStringLiteral("scribe_v2"),
		};
	}
	return {
		QStringLiteral("whisper-1"),
		QStringLiteral("gpt-transcribe"),
		QStringLiteral("whisper-large-v3-turbo"),
		QStringLiteral("gpt-4o-mini-transcribe"),
		QStringLiteral("gpt-4o-transcribe"),
	};
}

QString AgentCoordinator::resolveDefaultSpeechModelForEndpoint(
		const QString &sttBaseUrl,
		const QString &chatBaseUrl,
		const QStringList &chatCachedSpeechModels,
		const QHash<QString, AgentModelCapabilities> &modelCaps,
		const QStringList &externalCachedSpeechModels,
		const QHash<QString, AgentModelCapabilities> &externalModelCaps,
		AgentSpeechProtocolMode protocolMode) {
	if (isStreamingSpeechProtocol(protocolMode)) {
		const QString protoDefault = defaultModelForProtocol(protocolMode);
		if (!protoDefault.isEmpty()) {
			return protoDefault;
		}
	}
	const QString cleanStt = sttBaseUrl.trimmed();
	if (protocolMode == AgentSpeechProtocolMode::Auto) {
		const AgentSpeechProtocolMode urlAutoProto = resolveEffectiveSpeechProtocol(
			AgentSpeechProtocolMode::Auto, cleanStt, chatBaseUrl, QString());
		if (isStreamingSpeechProtocol(urlAutoProto)) {
			const QString protoDefault = defaultModelForProtocol(urlAutoProto);
			if (!protoDefault.isEmpty()) {
				return protoDefault;
			}
		}
	}
	if (!cleanStt.isEmpty() && !isSameSpeechOrigin(cleanStt, chatBaseUrl)) {
		const QString sttHost = endpointHostLower(cleanStt);
		if (!externalCachedSpeechModels.isEmpty()) {
			return pickDefaultSpeechModelFromCatalog(
				externalCachedSpeechModels, sttHost, externalModelCaps);
		}
		return fallbackDefaultSpeechModelForHost(sttHost);
	}
	return pickDefaultSpeechModelFromCatalog(
		chatCachedSpeechModels, endpointHostLower(chatBaseUrl), modelCaps);
}

QString AgentCoordinator::dictationSttBaseUrl() const {
	return (_speechSourceMode == AgentSpeechSourceMode::FollowChat)
		? QString()
		: _speechProfile.baseUrl;
}

AgentSpeechProtocolMode AgentCoordinator::dictationSpeechProtocol() const {
	return (_speechSourceMode == AgentSpeechSourceMode::FollowChat)
		? _followChatSpeechProtocol
		: _speechProfile.protocol;
}

QString AgentCoordinator::dictationSttModel() const {
	return (_speechSourceMode == AgentSpeechSourceMode::FollowChat)
		? _followChatSpeechModel
		: _speechProfile.model;
}

QString AgentCoordinator::dictationSttResourceId() const {
	if (_speechSourceMode == AgentSpeechSourceMode::FollowChat) {
		return QLatin1String(kDefaultSpeechResourceId);
	}
	return _speechProfile.resourceId.trimmed().isEmpty()
		? QLatin1String(kDefaultSpeechResourceId)
		: _speechProfile.resourceId.trimmed();
}

QString AgentCoordinator::dictationSttWorkspace() const {
	return (_speechSourceMode == AgentSpeechSourceMode::FollowChat)
		? QString()
		: _speechProfile.workspaceId;
}

void AgentCoordinator::resetExternalSpeechModelCacheIfNeeded(const QString &nextSttBaseUrl) {
	const QString nextKey = canonicalSpeechEndpointKey(nextSttBaseUrl.trimmed());
	if (_cachedExternalSpeechKey != nextKey) {
		_cachedExternalSpeechKey.clear();
		_cachedExternalSpeechModels.clear();
		_externalSttModelCapabilities.clear();
		if (_sttModelsHandle) {
			_sttModelsHandle->disconnect(this);
			_sttModelsHandle->cancel();
			_sttModelsHandle->deleteLater();
			_sttModelsHandle.clear();
		}
	}
}

void AgentCoordinator::promoteFollowChatToCustomIfNeeded() {
	if (_speechSourceMode == AgentSpeechSourceMode::CustomProfile) {
		return;
	}
	_speechSourceMode = AgentSpeechSourceMode::CustomProfile;
	const bool untouchedProfile = _speechProfile.baseUrl.trimmed().isEmpty() &&
		!_secretStore.hasApiKey(dictationSecretId(), KeyStoragePolicy::EncryptedStore);
	if (untouchedProfile) {
		if (_speechProfile.protocol == AgentSpeechProtocolMode::Auto) {
			_speechProfile.protocol = _followChatSpeechProtocol;
		}
		if (_speechProfile.model.trimmed().isEmpty()) {
			_speechProfile.model = _followChatSpeechModel.trimmed();
		}
		for (int i = 0; i < _speechProfiles.size(); ++i) {
			if (_speechProfiles[i].id == _speechProfile.id) {
				_speechProfiles[i] = _speechProfile;
				break;
			}
		}
	}
}

void AgentCoordinator::setSpeechSourceMode(AgentSpeechSourceMode mode) {
	if (_speechSourceMode == mode) {
		return;
	}
	cancelSpeechProbe();
	_speechSourceMode = mode;
	resetExternalSpeechModelCacheIfNeeded(dictationSttBaseUrl());
	saveSpeechProfilesToSettings();
	emit profileChanged();
}

std::optional<AgentSpeechProfile> AgentCoordinator::findSpeechProfile(const QString &profileId) const {
	const QString cleanId = profileId.trimmed();
	if (cleanId.isEmpty()) {
		return std::nullopt;
	}
	for (const auto &sp : _speechProfiles) {
		if (sp.id == cleanId) {
			return sp;
		}
	}
	return std::nullopt;
}

void AgentCoordinator::setSpeechProfile(const AgentSpeechProfile &profile) {
	const QString previousId = _speechProfile.id;
	const QString previousName = _speechProfile.name;
	const int previousCount = _speechProfiles.size();
	const AgentSpeechProfile next = normalizeSpeechProfileValues(profile, _speechProfile.id);

	bool foundExisting = false;
	for (int i = 0; i < _speechProfiles.size(); ++i) {
		if (_speechProfiles[i].id == next.id) {
			_speechProfiles[i] = next;
			foundExisting = true;
			break;
		}
	}
	if (!foundExisting) {
		if (_speechProfiles.size() <= 1) {
			if (_speechProfiles.isEmpty()) {
				_speechProfiles.push_back(next);
			} else {
				_speechProfiles[0] = next;
			}
		} else {
			_speechProfiles.push_back(next);
		}
	}

	const bool changed = (_speechProfile != next);
	const bool connectionChanged = !_speechProfile.hasSameConnectionConfig(next);
	const bool listMetaChanged =
		(previousId != next.id) ||
		(previousName != next.name) ||
		(previousCount != _speechProfiles.size());
	_speechProfile = next;
	if (connectionChanged) {
		cancelSpeechProbe();
	}
	resetExternalSpeechModelCacheIfNeeded(dictationSttBaseUrl());
	saveSpeechProfilesToSettings();
	if (listMetaChanged) {
		emit speechProfilesChanged();
	}
	if (changed) {
		emit profileChanged();
	}
}

QString AgentCoordinator::createSpeechProfile(
		const QString &displayName,
		const AgentSpeechProfile *templateProfile) {
	QString newId;
	do {
		newId = QStringLiteral("stt_%1")
			.arg(QUuid::createUuid().toString(QUuid::Id128).left(8));
	} while (findSpeechProfile(newId).has_value());

	AgentSpeechProfile created = templateProfile ? *templateProfile : AgentSpeechProfile{};
	created.id = newId;
	const QString trimmedName = displayName.trimmed();
	if (!trimmedName.isEmpty()) {
		created.name = trimmedName;
	} else {
		QSet<QString> existingNames;
		for (const auto &sp : _speechProfiles) {
			existingNames.insert(sp.name.trimmed());
		}
		int n = 2;
		QString candidate;
		do {
			candidate = QCoreApplication::translate("MeetingUI", "Speech Profile %1").arg(n++);
		} while (existingNames.contains(candidate));
		created.name = candidate;
	}
	created = normalizeSpeechProfileValues(created, newId);

	_speechProfiles.push_back(created);
	_speechProfile = created;
	_speechSourceMode = AgentSpeechSourceMode::CustomProfile;
	cancelSpeechProbe();
	resetExternalSpeechModelCacheIfNeeded(dictationSttBaseUrl());
	saveSpeechProfilesToSettings();
	emit speechProfilesChanged();
	emit profileChanged();
	return newId;
}

bool AgentCoordinator::switchSpeechProfile(const QString &profileId) {
	const auto targetOpt = findSpeechProfile(profileId);
	if (!targetOpt.has_value()) {
		return false;
	}
	if (_speechProfile.id == targetOpt->id) {
		return true;
	}
	_speechProfile = normalizeSpeechProfileValues(*targetOpt, targetOpt->id);
	cancelSpeechProbe();
	resetExternalSpeechModelCacheIfNeeded(dictationSttBaseUrl());
	saveSpeechProfilesToSettings();
	emit profileChanged();
	return true;
}

bool AgentCoordinator::deleteSpeechProfile(const QString &profileId) {
	if (_speechProfiles.size() <= 1) {
		return false;
	}
	const QString cleanId = profileId.trimmed();
	int idx = -1;
	for (int i = 0; i < _speechProfiles.size(); ++i) {
		if (_speechProfiles[i].id == cleanId) {
			idx = i;
			break;
		}
	}
	if (idx < 0) {
		return false;
	}

	const bool deletingActive = (_speechProfile.id == cleanId);
	_secretStore.clearApiKey(cleanId);
	_secretStore.clearApiKey(cleanId + QStringLiteral("_app"));
	_settings->remove(QStringLiteral("agent/speech/profiles/") + cleanId);
	_speechProfiles.removeAt(idx);

	if (deletingActive) {
		_speechProfile = normalizeSpeechProfileValues(_speechProfiles.front(), _speechProfiles.front().id);
		cancelSpeechProbe();
		resetExternalSpeechModelCacheIfNeeded(dictationSttBaseUrl());
	}

	saveSpeechProfilesToSettings();
	emit speechProfilesChanged();
	if (deletingActive) {
		emit profileChanged();
	}
	return true;
}

QString AgentCoordinator::resolvedSpeechModel() const {
	const QString explicitModel = dictationSttModel().trimmed();
	if (!explicitModel.isEmpty()) {
		return explicitModel;
	}
	const QString sttUrl = dictationSttBaseUrl();
	return resolveDefaultSpeechModelForEndpoint(
		sttUrl,
		_profile.baseUrl,
		_cachedSpeechModels,
		_modelCapabilities,
		cachedExternalSpeechModelsForUrl(sttUrl),
		cachedExternalSpeechCapabilitiesForUrl(sttUrl),
		dictationSpeechProtocol());
}

AgentSpeechProtocolMode AgentCoordinator::effectiveDictationSpeechProtocol() const {
	return resolveEffectiveSpeechProtocol(
		dictationSpeechProtocol(),
		dictationSttBaseUrl(),
		_profile.baseUrl,
		resolvedSpeechModel());
}

void AgentCoordinator::setDictationSttBaseUrl(const QString &baseUrl) {
	const QString clean = baseUrl.trimmed();
	const bool needPromote = !clean.isEmpty() && (_speechSourceMode == AgentSpeechSourceMode::FollowChat);
	if (!needPromote && _speechProfile.baseUrl == clean) {
		return;
	}
	cancelSpeechProbe();
	if (needPromote) {
		promoteFollowChatToCustomIfNeeded();
	}
	_speechProfile.baseUrl = clean;
	for (int i = 0; i < _speechProfiles.size(); ++i) {
		if (_speechProfiles[i].id == _speechProfile.id) {
			_speechProfiles[i] = _speechProfile;
			break;
		}
	}
	resetExternalSpeechModelCacheIfNeeded(dictationSttBaseUrl());
	saveSpeechProfilesToSettings();
	emit profileChanged();
}

void AgentCoordinator::setDictationSpeechProtocol(AgentSpeechProtocolMode mode) {
	if (_speechSourceMode == AgentSpeechSourceMode::FollowChat) {
		if (_followChatSpeechProtocol == mode) {
			return;
		}
		cancelSpeechProbe();
		_followChatSpeechProtocol = mode;
	} else {
		if (_speechProfile.protocol == mode) {
			return;
		}
		cancelSpeechProbe();
		_speechProfile.protocol = mode;
		for (int i = 0; i < _speechProfiles.size(); ++i) {
			if (_speechProfiles[i].id == _speechProfile.id) {
				_speechProfiles[i] = _speechProfile;
				break;
			}
		}
	}
	saveSpeechProfilesToSettings();
	emit profileChanged();
}

void AgentCoordinator::setDictationSttModel(const QString &model) {
	const QString clean = model.trimmed();
	if (_speechSourceMode == AgentSpeechSourceMode::FollowChat) {
		if (_followChatSpeechModel == clean) {
			return;
		}
		cancelSpeechProbe();
		_followChatSpeechModel = clean;
	} else {
		if (_speechProfile.model == clean) {
			return;
		}
		cancelSpeechProbe();
		_speechProfile.model = clean;
		for (int i = 0; i < _speechProfiles.size(); ++i) {
			if (_speechProfiles[i].id == _speechProfile.id) {
				_speechProfiles[i] = _speechProfile;
				break;
			}
		}
	}
	saveSpeechProfilesToSettings();
	emit profileChanged();
}

void AgentCoordinator::setDictationLanguage(const QString &language) {
	const QString clean = language.trimmed();
	if (_dictationLanguage == clean) {
		return;
	}
	cancelSpeechProbe();
	_dictationLanguage = clean;
	_settings->setValue(QStringLiteral("agent/dictationLanguage"), _dictationLanguage);
	_settings->sync();
	emit profileChanged();
}

void AgentCoordinator::setDictationPrompt(const QString &prompt) {
	const QString clean = prompt.trimmed().left(1000);
	if (_dictationPrompt == clean) {
		return;
	}
	cancelSpeechProbe();
	_dictationPrompt = clean;
	_settings->setValue(QStringLiteral("agent/dictationPrompt"), _dictationPrompt);
	_settings->sync();
	emit profileChanged();
}

void AgentCoordinator::setDictationSpeechTimeoutMs(int timeoutMs) {
	const int bounded = qBound(0, timeoutMs, 10000);
	if (_dictationSpeechTimeoutMs == bounded) {
		return;
	}
	_dictationSpeechTimeoutMs = bounded;
	_settings->setValue(QStringLiteral("agent/dictationSpeechTimeoutMs"), _dictationSpeechTimeoutMs);
	_settings->sync();
	emit profileChanged();
}

void AgentCoordinator::setDictationVadThresholdDb(int thresholdDb) {
	const int bounded = qBound(-60, thresholdDb, -15);
	if (_dictationVadThresholdDb == bounded) {
		return;
	}
	_dictationVadThresholdDb = bounded;
	_settings->setValue(QStringLiteral("agent/dictationVadThresholdDb"), _dictationVadThresholdDb);
	_settings->sync();
	emit profileChanged();
}

void AgentCoordinator::setDictationVadSilenceCommitMs(int silenceMs) {
	const int bounded = qBound(200, silenceMs, 3000);
	if (_dictationVadSilenceCommitMs == bounded) {
		return;
	}
	_dictationVadSilenceCommitMs = bounded;
	_settings->setValue(QStringLiteral("agent/dictationVadSilenceCommitMs"), _dictationVadSilenceCommitMs);
	_settings->sync();
	emit profileChanged();
}

void AgentCoordinator::setDictationDeviceId(const QString &deviceId) {
	const QString clean = deviceId.trimmed();
	if (_dictationDeviceId == clean) {
		return;
	}
	_dictationDeviceId = clean;
	_settings->setValue(QStringLiteral("agent/dictationDeviceId"), _dictationDeviceId);
}

void AgentCoordinator::setDictationSttResourceId(const QString &resourceId) {
	const QString clean = resourceId.trimmed().isEmpty()
		? QLatin1String(kDefaultSpeechResourceId)
		: resourceId.trimmed();
	if (_speechProfile.resourceId == clean) {
		return;
	}
	cancelSpeechProbe();
	_speechProfile.resourceId = clean;
	for (int i = 0; i < _speechProfiles.size(); ++i) {
		if (_speechProfiles[i].id == _speechProfile.id) {
			_speechProfiles[i] = _speechProfile;
			break;
		}
	}
	saveSpeechProfilesToSettings();
	emit profileChanged();
}

void AgentCoordinator::setDictationSttWorkspace(const QString &workspace) {
	const QString clean = workspace.trimmed();
	if (_speechProfile.workspaceId == clean) {
		return;
	}
	cancelSpeechProbe();
	_speechProfile.workspaceId = clean;
	for (int i = 0; i < _speechProfiles.size(); ++i) {
		if (_speechProfiles[i].id == _speechProfile.id) {
			_speechProfiles[i] = _speechProfile;
			break;
		}
	}
	saveSpeechProfilesToSettings();
	emit profileChanged();
}

QString AgentCoordinator::dictationSecretId() const {
	return _speechProfile.id.trimmed().isEmpty()
		? QStringLiteral("default_stt")
		: _speechProfile.id.trimmed();
}

QString AgentCoordinator::dictationAppKeySecretId() const {
	return dictationSecretId() + QStringLiteral("_app");
}

AgentSecretStore::Status AgentCoordinator::migrateSecretStoragePolicy(
		const QString &previousId,
		const QString &nextId,
		KeyStoragePolicy previousPolicy,
		KeyStoragePolicy nextPolicy) {
	if (previousPolicy == KeyStoragePolicy::EncryptedStore &&
		nextPolicy == KeyStoragePolicy::MemoryOnly) {
		const auto existing = _secretStore.loadApiKey(previousId, previousPolicy);
		if (existing.status == AgentSecretStore::Status::Ok) {
			return _secretStore.saveApiKey(nextId, existing.apiKey, KeyStoragePolicy::MemoryOnly);
		}
		return _secretStore.clearApiKey(previousId);
	}
	if (previousPolicy == KeyStoragePolicy::MemoryOnly &&
		nextPolicy == KeyStoragePolicy::EncryptedStore) {
		const auto existing = _secretStore.loadApiKey(previousId, KeyStoragePolicy::MemoryOnly);
		if (existing.status == AgentSecretStore::Status::Ok) {
			return _secretStore.saveApiKey(nextId, existing.apiKey, KeyStoragePolicy::EncryptedStore);
		}
	}
	return _lastSecretStatus;
}

bool AgentCoordinator::hasDictationApiKey() const {
	if (_speechSourceMode == AgentSpeechSourceMode::FollowChat) {
		return false;
	}
	return _secretStore.hasApiKey(dictationSecretId(), KeyStoragePolicy::EncryptedStore);
}

bool AgentCoordinator::isDictationApiKeyPersisted() const {
	if (_speechSourceMode == AgentSpeechSourceMode::FollowChat) {
		return false;
	}
	return _secretStore.isPersistedOnDisk(dictationSecretId(), KeyStoragePolicy::EncryptedStore);
}

AgentKeyEndpointMismatch AgentCoordinator::chatKeyEndpointCompatibility(
		const QString &baseUrlOverride,
		const QString &draftApiKey) {
	const QString effectiveUrl = baseUrlOverride.trimmed().isEmpty()
		? _profile.baseUrl
		: baseUrlOverride.trimmed();
	QString effectiveKey = draftApiKey.trimmed();
	if (effectiveKey.isEmpty()) {
		const auto loaded = _secretStore.loadApiKey(_profile.id, _profile.keyStorage);
		if (loaded.status == AgentSecretStore::Status::Ok) {
			effectiveKey = loaded.apiKey.trimmed();
		}
	}
	return evaluateKeyEndpointCompatibility(effectiveKey, effectiveUrl);
}

AgentKeyEndpointMismatch AgentCoordinator::speechKeyEndpointCompatibility(
		const QString &sttBaseUrlOverride,
		const QString &chatBaseUrlOverride,
		const QString &draftSttApiKey) {
	const QString cleanChat = chatBaseUrlOverride.trimmed().isEmpty()
		? _profile.baseUrl
		: chatBaseUrlOverride.trimmed();
	if (_speechSourceMode == AgentSpeechSourceMode::FollowChat) {
		QString effectiveKey = draftSttApiKey.trimmed();
		if (effectiveKey.isEmpty()) {
			const auto chatLoaded = _secretStore.loadApiKey(_profile.id, _profile.keyStorage);
			if (chatLoaded.status == AgentSecretStore::Status::Ok) {
				effectiveKey = chatLoaded.apiKey.trimmed();
			}
		}
		return evaluateKeyEndpointCompatibility(effectiveKey, cleanChat);
	}
	const QString cleanStt = sttBaseUrlOverride.trimmed();
	const QString effectiveUrl = cleanStt.isEmpty() ? cleanChat : cleanStt;

	QString effectiveKey = draftSttApiKey.trimmed();
	if (effectiveKey.isEmpty()) {
		const auto sttLoaded = _secretStore.loadApiKey(dictationSecretId(), KeyStoragePolicy::EncryptedStore);
		if (sttLoaded.status == AgentSecretStore::Status::Ok && !sttLoaded.apiKey.trimmed().isEmpty()) {
			effectiveKey = sttLoaded.apiKey.trimmed();
		} else if (cleanStt.isEmpty() || isSameSpeechOrigin(cleanStt, cleanChat)) {
			const auto chatLoaded = _secretStore.loadApiKey(_profile.id, _profile.keyStorage);
			if (chatLoaded.status == AgentSecretStore::Status::Ok) {
				effectiveKey = chatLoaded.apiKey.trimmed();
			}
		}
	}
	return evaluateKeyEndpointCompatibility(effectiveKey, effectiveUrl);
}

AgentSecretStore::Status AgentCoordinator::saveDictationApiKey(const QString &apiKey) {
	cancelSpeechProbe();
	const bool needPromote = !apiKey.trimmed().isEmpty() && (_speechSourceMode == AgentSpeechSourceMode::FollowChat);
	if (needPromote) {
		promoteFollowChatToCustomIfNeeded();
		saveSpeechProfilesToSettings();
	}
	const auto st = _secretStore.saveApiKey(dictationSecretId(), apiKey, _profile.keyStorage);
	emit profileChanged();
	return st;
}

AgentSecretStore::Status AgentCoordinator::clearDictationApiKey() {
	cancelSpeechProbe();
	const auto st = _secretStore.clearApiKey(dictationSecretId());
	emit profileChanged();
	return st;
}

bool AgentCoordinator::hasDictationAppKey() const {
	return _secretStore.hasApiKey(dictationAppKeySecretId(), KeyStoragePolicy::EncryptedStore);
}

bool AgentCoordinator::isDictationAppKeyPersisted() const {
	return _secretStore.isPersistedOnDisk(dictationAppKeySecretId(), KeyStoragePolicy::EncryptedStore);
}

AgentSecretStore::Status AgentCoordinator::saveDictationAppKey(const QString &appKey) {
	cancelSpeechProbe();
	const auto st = _secretStore.saveApiKey(dictationAppKeySecretId(), appKey, _profile.keyStorage);
	emit profileChanged();
	return st;
}

AgentSecretStore::Status AgentCoordinator::clearDictationAppKey() {
	cancelSpeechProbe();
	const auto st = _secretStore.clearApiKey(dictationAppKeySecretId());
	emit profileChanged();
	return st;
}

QString AgentCoordinator::loadAppKeyForSpeech(QString *outError) {
	if (outError) {
		outError->clear();
	}
	const auto loaded = _secretStore.loadApiKey(dictationAppKeySecretId(), KeyStoragePolicy::EncryptedStore);
	if (loaded.status == AgentSecretStore::Status::Corrupted) {
		if (outError) {
			*outError = QCoreApplication::translate(
				"MeetingUI",
				"Saved Volcengine App Key could not be decrypted. Please re-enter it in Settings > Copilot.");
		}
		return {};
	}
	if (loaded.status == AgentSecretStore::Status::StorageUnavailable) {
		if (outError) {
			*outError = QCoreApplication::translate(
				"MeetingUI",
				"Saved Volcengine App Key could not be accessed (OS encrypted storage is locked or unavailable).");
		}
		return {};
	}
	return loaded.apiKey.trimmed();
}

QString AgentCoordinator::loadApiKeyForSpeech(QString *outError) {
	if (outError) {
		outError->clear();
	}
	if (_speechSourceMode == AgentSpeechSourceMode::CustomProfile) {
		const auto sttLoaded = _secretStore.loadApiKey(dictationSecretId(), KeyStoragePolicy::EncryptedStore);
		if (sttLoaded.status == AgentSecretStore::Status::Corrupted) {
			if (outError) {
				*outError = QCoreApplication::translate(
					"MeetingUI",
					"Saved STT API key could not be decrypted. Please re-enter it in Settings > Copilot.");
			}
			return {};
		}
		if (sttLoaded.status == AgentSecretStore::Status::StorageUnavailable) {
			if (outError) {
				*outError = QCoreApplication::translate(
					"MeetingUI",
					"Saved STT API key could not be accessed (OS encrypted storage is locked or unavailable). Please unlock your keyring or re-enter it in Settings > Copilot.");
			}
			return {};
		}
		if (sttLoaded.status == AgentSecretStore::Status::Ok && !sttLoaded.apiKey.trimmed().isEmpty()) {
			return sttLoaded.apiKey.trimmed();
		}
	}

	// No separate STT key configured: reuse Chat API key only when using the Chat endpoint
	// or a same-origin STT path. Remote third-party STT providers require their own key so
	// Chat credentials are never forwarded cross-origin. Loopback STT servers may be keyless.
	const QString customStt = dictationSttBaseUrl().trimmed();
	const AgentSpeechProtocolMode effectiveProto = effectiveDictationSpeechProtocol();
	QString effectiveSttUrl = customStt;
	if (effectiveSttUrl.isEmpty() && isStreamingSpeechProtocol(effectiveProto)) {
		if (effectiveProto == AgentSpeechProtocolMode::DashScopeInferenceWs ||
			effectiveProto == AgentSpeechProtocolMode::DashScopeQwenRealtimeWs) {
			effectiveSttUrl = isStandardDashScopeHost(QUrl(_profile.baseUrl))
				? _profile.baseUrl.trimmed()
				: defaultStreamingSpeechEndpoint(effectiveProto);
		} else if (effectiveProto == AgentSpeechProtocolMode::XaiSttWs) {
			effectiveSttUrl = isXaiAgentHost(QUrl(_profile.baseUrl))
				? _profile.baseUrl.trimmed()
				: defaultStreamingSpeechEndpoint(effectiveProto);
		} else if (effectiveProto == AgentSpeechProtocolMode::OpenAiRealtimeWs) {
			effectiveSttUrl = _profile.baseUrl.trimmed().isEmpty()
				? defaultStreamingSpeechEndpoint(effectiveProto)
				: _profile.baseUrl.trimmed();
		} else {
			effectiveSttUrl = defaultStreamingSpeechEndpoint(effectiveProto);
		}
	}

	if (!effectiveSttUrl.isEmpty()) {
		const AgentEndpointDecision sttEp = evaluateAgentSpeechEndpoint(effectiveSttUrl);
		const bool sameOrigin = isSameSpeechOrigin(effectiveSttUrl, _profile.baseUrl);
		const bool loopbackStt = sttEp.allowed && isAgentLoopbackHost(sttEp.normalizedUrl.host());
		if (loopbackStt && !sameOrigin) {
			return {};
		}
		if (!sameOrigin) {
			if (outError) {
				*outError = QCoreApplication::translate(
					"MeetingUI",
					"A separate STT API key is required for the custom Speech-to-Text endpoint. Please enter it in Settings > Copilot.");
			}
			return {};
		}
	}

	const auto loaded = _secretStore.loadApiKey(_profile.id, _profile.keyStorage);
	if (loaded.status == AgentSecretStore::Status::Corrupted) {
		if (outError) {
			*outError = QCoreApplication::translate(
				"MeetingUI",
				"Saved API key could not be decrypted. Please re-enter it in Settings > Copilot.");
		}
		return {};
	}
	if (loaded.status == AgentSecretStore::Status::StorageUnavailable) {
		if (outError) {
			*outError = QCoreApplication::translate(
				"MeetingUI",
				"Saved API key could not be accessed (OS encrypted storage is locked or unavailable). Please unlock your keyring or re-enter it in Settings > Copilot.");
		}
		return {};
	}
	return loaded.apiKey;
}

StreamingAsrConfig AgentCoordinator::buildStreamingAsrConfig(QString *outError) {
	if (outError) {
		outError->clear();
	}
	StreamingAsrConfig cfg;
	const AgentSpeechProtocolMode proto = effectiveDictationSpeechProtocol();
	cfg.protocol = proto;
	cfg.model = resolvedSpeechModel();
	cfg.language = _dictationLanguage.trimmed();
	cfg.prompt = _dictationPrompt.trimmed();
	cfg.resourceId = dictationSttResourceId().trimmed();
	cfg.workspaceId = dictationSttWorkspace().trimmed();
	cfg.connectTimeoutMs = _profile.connectTimeoutMs;
	cfg.enableServerVad = true;

	QString effectiveUrl = dictationSttBaseUrl().trimmed();
	if (effectiveUrl.isEmpty()) {
		if (proto == AgentSpeechProtocolMode::OpenAiRealtimeWs && !_profile.baseUrl.trimmed().isEmpty()) {
			effectiveUrl = _profile.baseUrl.trimmed();
		} else if ((proto == AgentSpeechProtocolMode::DashScopeInferenceWs ||
					proto == AgentSpeechProtocolMode::DashScopeQwenRealtimeWs) &&
				   isStandardDashScopeHost(QUrl(_profile.baseUrl))) {
			effectiveUrl = _profile.baseUrl.trimmed();
		} else if (proto == AgentSpeechProtocolMode::XaiSttWs &&
				   isXaiAgentHost(QUrl(_profile.baseUrl))) {
			effectiveUrl = _profile.baseUrl.trimmed();
		} else {
			effectiveUrl = defaultStreamingSpeechEndpoint(proto);
		}
	}
	cfg.endpointUrl = effectiveUrl;

	QString keyErr;
	cfg.apiKey = loadApiKeyForSpeech(&keyErr);
	if (!keyErr.isEmpty()) {
		if (outError) *outError = keyErr;
		return {};
	}
	if (proto == AgentSpeechProtocolMode::VolcengineBigModelWs) {
		if (_speechSourceMode == AgentSpeechSourceMode::CustomProfile) {
			QString appErr;
			cfg.appKey = loadAppKeyForSpeech(&appErr);
			if (!appErr.isEmpty()) {
				if (outError) *outError = appErr;
				return {};
			}
		}
		const AgentEndpointDecision ep = evaluateAgentSpeechEndpoint(cfg.endpointUrl);
		const bool loopback = ep.allowed && isAgentLoopbackHost(ep.normalizedUrl.host());
		if (!loopback && cfg.apiKey.trimmed().isEmpty()) {
			if (outError) {
				*outError = QCoreApplication::translate(
					"MeetingUI", "Volcengine streaming ASR requires an API Key (or legacy App Key + Access Key).");
			}
			return {};
		}
	} else {
		const AgentEndpointDecision ep = evaluateAgentSpeechEndpoint(cfg.endpointUrl);
		const bool loopback = ep.allowed && isAgentLoopbackHost(ep.normalizedUrl.host());
		if (!loopback && cfg.apiKey.trimmed().isEmpty()) {
			if (outError) {
				*outError = QCoreApplication::translate(
					"MeetingUI", "An STT API key is required for WebSocket streaming ASR.");
			}
			return {};
		}
	}

	if (isSameSpeechOrigin(cfg.endpointUrl, _profile.baseUrl)) {
		cfg.customHeaders = _profile.customHeaders;
	}
	return cfg;
}

QString AgentCoordinator::loadApiKeyForChat(QString *outError) {
	if (outError) {
		outError->clear();
	}
	const auto loaded = _secretStore.loadApiKey(_profile.id, _profile.keyStorage);
	if (loaded.status == AgentSecretStore::Status::Corrupted) {
		if (outError) {
			*outError = QCoreApplication::translate(
				"MeetingUI",
				"Saved API key could not be decrypted. Please re-enter it in Settings > Copilot.");
		}
		return {};
	}
	if (loaded.status == AgentSecretStore::Status::StorageUnavailable) {
		if (outError) {
			*outError = QCoreApplication::translate(
				"MeetingUI",
				"Saved API key could not be accessed (OS encrypted storage is locked or unavailable). Please unlock your keyring or re-enter it in Settings > Copilot.");
		}
		return {};
	}
	return loaded.apiKey.trimmed();
}

QString AgentCoordinator::buildExplicitContextBlock(const QVector<AgentContextChip> &chips) const {
	if (chips.isEmpty()) {
		return {};
	}
	const AgentToolViewContext viewCtx = buildToolViewContext();
	QStringList sections;
	QSet<int> seenKinds;
	for (const AgentContextChip &chip : chips) {
		const int kindInt = static_cast<int>(chip.kind);
		if (seenKinds.contains(kindInt)) {
			continue;
		}
		seenKinds.insert(kindInt);

		QString toolName;
		QString fallbackLabel;
		QJsonObject args;
		switch (chip.kind) {
		case AgentContextChipKind::MeetingChat:
			toolName = QStringLiteral("get_recent_chat");
			fallbackLabel = QStringLiteral("Recent Meeting Chat");
			args.insert(QStringLiteral("limit"), 20);
			break;
		case AgentContextChipKind::Telemetry:
			toolName = QStringLiteral("get_telemetry_snapshot");
			fallbackLabel = QStringLiteral("Audio/Video Telemetry");
			break;
		case AgentContextChipKind::Participants:
			toolName = QStringLiteral("list_participants");
			fallbackLabel = QStringLiteral("Meeting Participants");
			break;
		case AgentContextChipKind::Schedule:
			toolName = QStringLiteral("list_scheduled_meetings");
			fallbackLabel = QStringLiteral("Scheduled Meetings");
			break;
		case AgentContextChipKind::Transcript:
			toolName = QStringLiteral("get_recent_transcripts");
			fallbackLabel = QStringLiteral("Recent Meeting Transcripts");
			args.insert(QStringLiteral("limit"), 30);
			break;
		}
		const QString label = chip.label.trimmed().isEmpty() ? fallbackLabel : chip.label.trimmed();
		const AgentToolResult res = _toolRegistry.executeReadOnly(toolName, args, viewCtx);
		if (res.success && !res.content.trimmed().isEmpty()) {
			sections.push_back(QStringLiteral("### %1\n%2").arg(label, res.content.trimmed()));
		} else {
			const QString reason = res.errorMessage.trimmed().isEmpty()
				? QStringLiteral("Unavailable")
				: res.errorMessage.trimmed();
			sections.push_back(QStringLiteral("### %1\n{\"available\":false,\"reason\":\"%2\"}")
				.arg(label, QString(reason).replace(QLatin1Char('"'), QLatin1Char('\''))));
		}
	}
	if (sections.isEmpty()) {
		return {};
	}
	return QStringLiteral("## Explicit User-Attached Context\n") + sections.join(QStringLiteral("\n\n"));
}

AgentMeetingContext AgentCoordinator::buildCurrentMeetingContext() const {
	AgentMeetingContext ctx;
	if (!_meeting || _meeting->state() == MeetingState::Idle) {
		ctx.inMeeting = false;
		return ctx;
	}
	ctx.inMeeting = true;
	ctx.meetingId = _meeting->currentMeetingId();
	ctx.meetingName = _meeting->meetingDetail().meetingName;
	ctx.encryptedMeeting = _meeting->requiresEncryption();
	ctx.e2eeContextAuthorized = isE2eeContextAuthorized();
	int visibleParticipants = 0;
	for (const auto &p : _meeting->participants()) {
		if (!p.isAgent) {
			++visibleParticipants;
		}
	}
	ctx.participantCount = visibleParticipants;
	ctx.micMuted = _meeting->isLocalAudioMuted();
	ctx.cameraEnabled = _meeting->isLocalVideoEnabled();
	ctx.screenSharing = false;
	return ctx;
}

SessionGenerationToken AgentCoordinator::currentGenerationToken() const {
	SessionGenerationToken token;
	token.authGeneration = _sessionManager.authGeneration();
	if (_meeting && _meeting->state() != MeetingState::Idle) {
		token.coordinatorSession = _meetingAttachmentGeneration;
		const auto list = _meeting->participants();
		if (!list.empty()) {
			token.nativeRoomGeneration = list.front().participantKey.native_room_generation;
		}
	}
	return token;
}

bool AgentCoordinator::isGenerationCurrent(const SessionGenerationToken &token) const {
	if (token.authGeneration != _sessionManager.authGeneration()) {
		return false;
	}
	if (token.coordinatorSession != 0) {
		if (!_meeting || _meeting->state() == MeetingState::Idle) {
			return false;
		}
		if (token.coordinatorSession != _meetingAttachmentGeneration) {
			return false;
		}
		if (token.nativeRoomGeneration != 0) {
			const auto list = _meeting->participants();
			if (!list.empty() && list.front().participantKey.native_room_generation != 0 &&
				list.front().participantKey.native_room_generation != token.nativeRoomGeneration) {
				return false;
			}
		}
	}
	return true;
}

int AgentCoordinator::indexOfMessage(const QString &id) const {
	for (int i = 0; i < _messages.size(); ++i) {
		if (_messages[i].id == id) {
			return i;
		}
	}
	return -1;
}

int AgentCoordinator::indexOfPendingAction(const QString &actionId) const {
	for (int i = 0; i < _pendingActions.size(); ++i) {
		if (_pendingActions[i].actionId == actionId) {
			return i;
		}
	}
	return -1;
}

void AgentCoordinator::recordMeetingChatMessage(const QString &senderName, const QString &message, bool isLocal) {
	if (message.trimmed().isEmpty()) {
		return;
	}
	AgentMeetingChatEntry entry;
	entry.senderName = senderName.trimmed().isEmpty() ? QStringLiteral("Participant") : senderName.trimmed();
	entry.text = message;
	entry.isLocal = isLocal;
	entry.timestamp = QDateTime::currentDateTime();
	_recentMeetingChat.push_back(std::move(entry));
	while (_recentMeetingChat.size() > kMaxStoredChatEntries) {
		_recentMeetingChat.removeFirst();
	}
}

void AgentCoordinator::recordMeetingTranscript(const AgentMeetingTranscriptEntry &entry) {
	if (entry.text.trimmed().isEmpty()) {
		return;
	}
	AgentMeetingTranscriptEntry stored = entry;
	if (stored.speakerName.trimmed().isEmpty()) {
		stored.speakerName = stored.speakerIdentity.trimmed().isEmpty()
			? QStringLiteral("Participant")
			: stored.speakerIdentity.trimmed();
	}
	if (stored.timestampMs <= 0) {
		stored.timestampMs = QDateTime::currentMSecsSinceEpoch();
	}
	int existingIdx = -1;
	if (!stored.segmentId.isEmpty()) {
		for (int i = _recentTranscripts.size() - 1; i >= 0; --i) {
			if (_recentTranscripts[i].segmentId == stored.segmentId &&
				_recentTranscripts[i].speakerIdentity == stored.speakerIdentity) {
				existingIdx = i;
				break;
			}
		}
	}
	if (existingIdx >= 0) {
		_recentTranscripts[existingIdx] = std::move(stored);
	} else {
		_recentTranscripts.push_back(std::move(stored));
		while (_recentTranscripts.size() > kMaxStoredTranscriptEntries) {
			_recentTranscripts.removeFirst();
		}
	}
}

void AgentCoordinator::updateTelemetrySnapshot(const QVariantMap &snapshot) {
	_latestTelemetrySnapshot = snapshot;
}

bool AgentCoordinator::validateE2eeForAttachments(
		const QVector<AgentImageAttachment> &images,
		const QVector<AgentContextChip> &chips) {
	if (!isInMeeting() || !isEncryptedMeeting() || isE2eeContextAuthorized()) {
		return true;
	}
	if (!images.isEmpty()) {
		emit requestFailed(QCoreApplication::translate(
			"MeetingUI",
			"End-to-end encrypted meeting: please authorize AI sharing in the banner before sending images."));
		return false;
	}
	const bool hasMeetingChip = std::any_of(chips.cbegin(), chips.cend(),
		[](const AgentContextChip &chip) {
			return chip.kind == AgentContextChipKind::MeetingChat ||
				chip.kind == AgentContextChipKind::Telemetry ||
				chip.kind == AgentContextChipKind::Participants ||
				chip.kind == AgentContextChipKind::Transcript;
		});
	if (hasMeetingChip) {
		emit requestFailed(QCoreApplication::translate(
			"MeetingUI",
			"End-to-end encrypted meeting: please authorize AI sharing in the banner before attaching meeting context."));
		return false;
	}
	return true;
}

void AgentCoordinator::sendUserMessage(
		const QString &text,
		const QVector<AgentImageAttachment> &images,
		const QVector<AgentContextChip> &contextChips) {
	const QString trimmed = text.trimmed();
	QVector<AgentImageAttachment> validImages;
	validImages.reserve(std::min(static_cast<int>(images.size()), kMaxAgentImagesPerMessage));
	for (const auto &img : images) {
		if (!img.isValid()) {
			continue;
		}
		validImages.push_back(img);
		if (validImages.size() >= kMaxAgentImagesPerMessage) {
			break;
		}
	}

	QVector<AgentContextChip> validChips;
	QSet<int> seenChipKinds;
	for (const auto &chip : contextChips) {
		const int kindKey = static_cast<int>(chip.kind);
		if (seenChipKinds.contains(kindKey)) {
			continue;
		}
		seenChipKinds.insert(kindKey);
		validChips.push_back(chip);
	}

	if (trimmed.isEmpty() && validImages.isEmpty() && validChips.isEmpty()) {
		return;
	}

	if (!isConfigured()) {
		const AgentEndpointDecision decision = evaluateAgentEndpoint(_profile.baseUrl);
		const QString err = !decision.allowed
			? decision.reason
			: QStringLiteral("Model name is required.");
		emit requestFailed(err);
		return;
	}

	if (!validateE2eeForAttachments(validImages, validChips)) {
		return;
	}

	if (_activeHandle) {
		cancelActiveRequestWithReason(AgentChatMessage::Status::Cancelled,
			QStringLiteral("Replaced by a newer request."));
	}
	++_conversationNonce;
	_activeToolRound.reset();

	const auto secret = _secretStore.loadApiKey(_profile.id, _profile.keyStorage);
	if (secret.status == AgentSecretStore::Status::Corrupted) {
		const QString err = QStringLiteral("Saved API key could not be decrypted. Please re-enter it in Settings > Copilot.");
		emit requestFailed(err);
		return;
	}

	AgentChatMessage userMsg;
	userMsg.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
	userMsg.role = AgentChatMessage::Role::User;
	userMsg.content = trimmed;
	userMsg.images = validImages;
	userMsg.contextChips = validChips;
	userMsg.status = AgentChatMessage::Status::Complete;
	userMsg.timestamp = QDateTime::currentDateTime();
	_messages.push_back(userMsg);
	trimStoredMessagesToLimit();

	constexpr int kMaxRetainedFullImageMessages = 10;
	int retainedImageMsgCount = 0;
	for (int i = _messages.size() - 1; i >= 0; --i) {
		if (_messages[i].images.isEmpty()) {
			continue;
		}
		++retainedImageMsgCount;
		if (retainedImageMsgCount > kMaxRetainedFullImageMessages) {
			for (auto &att : _messages[i].images) {
				att.encodedBytes.clear();
			}
		}
	}

	syncActiveSessionSnapshot(true);
	schedulePersistSessions();
	emit messageAdded(_messages.last());

	const SessionGenerationToken token = currentGenerationToken();
	dispatchChatRequest(token, secret.apiKey, 0);
}

bool AgentCoordinator::canRetryLastTurn() const {
	if (_activeHandle || !isConfigured() || _messages.isEmpty()) {
		return false;
	}
	int lastUserIdx = -1;
	for (int i = _messages.size() - 1; i >= 0; --i) {
		if (_messages[i].role == AgentChatMessage::Role::User) {
			lastUserIdx = i;
			break;
		}
	}
	if (lastUserIdx < 0) {
		return false;
	}
	if (lastUserIdx == _messages.size() - 1) {
		return true;
	}
	const auto tailStatus = _messages.last().status;
	return tailStatus == AgentChatMessage::Status::Failed ||
		tailStatus == AgentChatMessage::Status::Cancelled;
}

bool AgentCoordinator::restartFromUserIndex(
		int userIndex,
		const QString *editedText,
		const QVector<AgentContextChip> *editedChips) {
	if (_activeHandle || !isConfigured() ||
		userIndex < 0 || userIndex >= _messages.size() ||
		_messages[userIndex].role != AgentChatMessage::Role::User) {
		return false;
	}

	QVector<AgentContextChip> nextChips = editedChips
		? *editedChips
		: _messages[userIndex].contextChips;
	QVector<AgentContextChip> dedupedChips;
	QSet<int> seenChipKinds;
	for (const auto &chip : nextChips) {
		const int key = static_cast<int>(chip.kind);
		if (!seenChipKinds.contains(key)) {
			seenChipKinds.insert(key);
			dedupedChips.push_back(chip);
		}
	}

	const QString nextContent = editedText ? editedText->trimmed() : _messages[userIndex].content;
	if (nextContent.isEmpty() && _messages[userIndex].images.isEmpty() && dedupedChips.isEmpty()) {
		return false;
	}

	if (!validateE2eeForAttachments(_messages[userIndex].images, dedupedChips)) {
		return false;
	}

	const auto secret = _secretStore.loadApiKey(_profile.id, _profile.keyStorage);
	if (secret.status == AgentSecretStore::Status::Corrupted) {
		emit requestFailed(QStringLiteral("Saved API key could not be decrypted. Please re-enter it in Settings > Copilot."));
		return false;
	}

	++_conversationNonce;
	_activeToolRound.reset();
	expireAllPendingActions(QCoreApplication::translate("MeetingUI", "Turn retried."));

	if (editedText || editedChips) {
		const QString prevDerived = AgentSessionStore::deriveSessionTitle(_messages);
		_messages[userIndex].content = nextContent;
		_messages[userIndex].contextChips = dedupedChips;
		_messages[userIndex].timestamp = QDateTime::currentDateTime();
		const int activeIdx = indexOfSession(_activeSessionId);
		if (activeIdx >= 0 && _sessions[activeIdx].summary.title == prevDerived) {
			_sessions[activeIdx].summary.title = AgentSessionStore::deriveSessionTitle(_messages);
		}
		emit messageUpdated(_messages[userIndex]);
	}

	while (_messages.size() - 1 > userIndex) {
		const QString removedId = _messages.last().id;
		_messages.removeLast();
		emit messageRemoved(removedId);
	}

	_streamingMessageId.clear();
	resetStreamingBuffers();

	syncActiveSessionSnapshot(true);
	schedulePersistSessions();
	dispatchChatRequest(currentGenerationToken(), secret.apiKey, 0);
	return true;
}

bool AgentCoordinator::retryLastTurn() {
	if (!canRetryLastTurn()) {
		return false;
	}
	int lastUserIdx = -1;
	for (int i = _messages.size() - 1; i >= 0; --i) {
		if (_messages[i].role == AgentChatMessage::Role::User) {
			lastUserIdx = i;
			break;
		}
	}
	return restartFromUserIndex(lastUserIdx, nullptr, nullptr);
}

bool AgentCoordinator::regenerateFromAssistantMessage(const QString &assistantMessageId) {
	if (_activeHandle || !isConfigured()) {
		return false;
	}
	const int assistantIdx = indexOfMessage(assistantMessageId);
	if (assistantIdx < 0) {
		return false;
	}
	int targetUserIdx = -1;
	for (int i = assistantIdx; i >= 0; --i) {
		if (_messages[i].role == AgentChatMessage::Role::User) {
			targetUserIdx = i;
			break;
		}
	}
	if (targetUserIdx < 0) {
		return false;
	}
	return restartFromUserIndex(targetUserIdx, nullptr, nullptr);
}

bool AgentCoordinator::editAndResendUserMessage(
		const QString &userMessageId,
		const QString &newText,
		const QVector<AgentContextChip> *overrideChips) {
	const int userIdx = indexOfMessage(userMessageId);
	if (userIdx < 0 || _messages[userIdx].role != AgentChatMessage::Role::User) {
		return false;
	}
	return restartFromUserIndex(userIdx, &newText, overrideChips);
}

void AgentCoordinator::trimStoredMessagesToLimit() {
	while (_messages.size() > kMaxStoredMessages) {
		_messages.removeFirst();
	}
	// Never leave an orphaned Role::Tool message at the front after trimming.
	while (!_messages.isEmpty() && _messages.front().role == AgentChatMessage::Role::Tool) {
		_messages.removeFirst();
	}
}

QVector<AgentChatMessage> AgentCoordinator::buildContextMessages(
		int maxContextMessages,
		int toolRound,
		QVector<bool> *outFollowedByFailedTurn) const {
	if (outFollowedByFailedTurn) {
		outFollowedByFailedTurn->clear();
	}

	QVector<AgentChatMessage> validated;
	QVector<bool> validatedFailed;
	validated.reserve(_messages.size());
	validatedFailed.reserve(_messages.size());

	for (int i = 0; i < _messages.size(); ++i) {
		const auto &msg = _messages.at(i);
		if (msg.status != AgentChatMessage::Status::Complete) {
			continue;
		}
		if (msg.role == AgentChatMessage::Role::Tool) {
			// Skip orphaned Role::Tool messages that lack a preceding validated Assistant(toolCalls).
			continue;
		}
		const bool followedByFailed = (i + 1 < _messages.size() &&
			_messages[i + 1].role == AgentChatMessage::Role::Assistant &&
			_messages[i + 1].status != AgentChatMessage::Status::Complete);
		if (msg.role == AgentChatMessage::Role::User &&
			!msg.images.isEmpty() &&
			followedByFailed &&
			msg.content.trimmed().isEmpty()) {
			continue;
		}
		if (msg.role == AgentChatMessage::Role::Assistant && !msg.toolCalls.isEmpty()) {
			QSet<QString> expectedIds;
			for (const auto &tc : msg.toolCalls) {
				if (!tc.id.trimmed().isEmpty()) {
					expectedIds.insert(tc.id);
				}
			}
			if (expectedIds.isEmpty()) {
				if (!msg.content.trimmed().isEmpty()) {
					AgentChatMessage plain = msg;
					plain.toolCalls.clear();
					validated.push_back(std::move(plain));
					validatedFailed.push_back(followedByFailed);
				}
				continue;
			}
			QVector<AgentChatMessage> matchedTools;
			QSet<QString> seenIds;
			int j = i + 1;
			for (; j < _messages.size(); ++j) {
				const auto &next = _messages.at(j);
				if (next.status != AgentChatMessage::Status::Complete) {
					continue;
				}
				if (next.role != AgentChatMessage::Role::Tool) {
					break;
				}
				if (expectedIds.contains(next.toolCallId) && !seenIds.contains(next.toolCallId)) {
					seenIds.insert(next.toolCallId);
					matchedTools.push_back(next);
				}
			}
			if (seenIds.size() == expectedIds.size()) {
				validated.push_back(msg);
				validatedFailed.push_back(false);
				for (auto &t : matchedTools) {
					validated.push_back(std::move(t));
					validatedFailed.push_back(false);
				}
			} else if (!msg.content.trimmed().isEmpty()) {
				AgentChatMessage plain = msg;
				plain.toolCalls.clear();
				validated.push_back(std::move(plain));
				validatedFailed.push_back(followedByFailed);
			}
			i = j - 1;
			continue;
		}
		validated.push_back(msg);
		validatedFailed.push_back(followedByFailed);
	}

	if (validated.isEmpty()) {
		return {};
	}

	struct AtomicUnit {
		int start = 0;
		int count = 0;
		bool isUser = false;
	};
	QVector<AtomicUnit> units;
	units.reserve(validated.size());
	for (int i = 0; i < validated.size();) {
		const auto &m = validated.at(i);
		if (m.role == AgentChatMessage::Role::Assistant && !m.toolCalls.isEmpty()) {
			int end = i + 1;
			while (end < validated.size() && validated.at(end).role == AgentChatMessage::Role::Tool) {
				++end;
			}
			units.push_back({i, end - i, false});
			i = end;
		} else {
			units.push_back({i, 1, m.role == AgentChatMessage::Role::User});
			++i;
		}
	}

	int lastUserUnitIdx = -1;
	for (int u = units.size() - 1; u >= 0; --u) {
		if (units.at(u).isUser) {
			lastUserUnitIdx = u;
			break;
		}
	}

	int firstUnitIdx = units.size() - 1;
	int keptCount = units.back().count;
	for (int u = units.size() - 2; u >= 0; --u) {
		if (keptCount + units.at(u).count > maxContextMessages) {
			break;
		}
		keptCount += units.at(u).count;
		firstUnitIdx = u;
	}

	// Preserve the entire active user turn while inside a multi-round tool loop.
	if (toolRound > 0 && lastUserUnitIdx >= 0 && firstUnitIdx > lastUserUnitIdx) {
		firstUnitIdx = lastUserUnitIdx;
	}

	const int startMsgIdx = units.at(firstUnitIdx).start;
	QVector<AgentChatMessage> out;
	out.reserve(validated.size() - startMsgIdx);
	for (int i = startMsgIdx; i < validated.size(); ++i) {
		out.push_back(validated.at(i));
		if (outFollowedByFailedTurn) {
			outFollowedByFailedTurn->push_back(validatedFailed.at(i));
		}
	}
	return out;
}

AgentToolViewContext AgentCoordinator::buildToolViewContext() const {
	AgentToolViewContext ctx;
	ctx.meetingOverview = buildCurrentMeetingContext();
	ctx.meeting = _meeting.data();
	ctx.catalog = _catalog.data();
	ctx.contacts = _contacts.data();
	ctx.presence = _presence.data();
	ctx.callInvite = _callInvite.data();
	ctx.sessionManager = &_sessionManager;
	ctx.agentCoordinator = this;
	ctx.hostSettings = &_hostSettings;
	ctx.recentChat = &_recentMeetingChat;
	ctx.recentTranscripts = &_recentTranscripts;
	ctx.telemetrySnapshot = &_latestTelemetrySnapshot;
	ctx.canShareMeetingData = isInMeeting() && (!isEncryptedMeeting() || isE2eeContextAuthorized());
	ctx.canUseCatalogTools = _sessionManager.isLoggedIn() && (_catalog != nullptr);
	ctx.canUseContactTools = _sessionManager.isLoggedIn() && (_contacts != nullptr || _callInvite != nullptr);
	ctx.canUseClientPrefTools = true;
	return ctx;
}

void AgentCoordinator::dispatchChatRequest(
		const SessionGenerationToken &token,
		const QString &apiKey,
		int toolRound) {
	if (!isGenerationCurrent(token)) {
		return;
	}

	AgentChatRequest req;
	req.profile = _profile;
	req.apiKey = apiKey;
	req.mode = _chatMode;
	req.stream = (_profile.protocol != AgentProtocolDialect::CustomWebhookJson);

	const int maxMsgs = qBound(2, _profile.maxContextMessages, 64);
	QVector<bool> msgFollowedByFailedTurn;
	req.messages = buildContextMessages(maxMsgs, toolRound, &msgFollowedByFailedTurn);

	const AgentModelCapabilities currentCaps = resolveModelCapabilities(_profile.model);
	int lastUserIndex = -1;
	for (int k = req.messages.size() - 1; k >= 0; --k) {
		if (req.messages[k].role == AgentChatMessage::Role::User) {
			lastUserIndex = k;
			break;
		}
	}
	if (lastUserIndex >= 0 && !req.messages[lastUserIndex].contextChips.isEmpty()) {
		req.explicitContextPrompt = buildExplicitContextBlock(req.messages[lastUserIndex].contextChips);
	}

	constexpr int kMaxImageTurnsInRequest = 3;
	constexpr int kMaxTotalImagesInRequest = 6;
	constexpr qint64 kMaxTotalImageBytesInRequest = 8LL * 1024 * 1024;
	int keptImageTurns = 0;
	int keptImageCount = 0;
	qint64 keptImageBytes = 0;
	for (int i = req.messages.size() - 1; i >= 0; --i) {
		auto &msg = req.messages[i];
		if (msg.role != AgentChatMessage::Role::User || msg.images.isEmpty()) {
			continue;
		}
		const bool isHistoricalUserMsg = (i < lastUserIndex);
		const bool failedTurn = (i < msgFollowedByFailedTurn.size() && msgFollowedByFailedTurn[i]);
		const bool blockedByNonVisionHistory = (isHistoricalUserMsg && !currentCaps.supportsVision);
		const bool turnAllowed = !failedTurn && !blockedByNonVisionHistory &&
			(keptImageTurns < kMaxImageTurnsInRequest);
		QVector<AgentImageAttachment> keptForMsg;
		QStringList omittedNotes;
		for (const auto &att : msg.images) {
			const qint64 attBytes = att.encodedBytes.size();
			if (turnAllowed &&
				att.isValid() &&
				keptImageCount < kMaxTotalImagesInRequest &&
				(keptImageBytes + attBytes <= kMaxTotalImageBytesInRequest)) {
				keptForMsg.push_back(att);
				++keptImageCount;
				keptImageBytes += attBytes;
			} else {
				const QString displayName = att.fileName.isEmpty()
					? QStringLiteral("image")
					: att.fileName;
				omittedNotes.push_back(
					QStringLiteral("[Historical image attachment omitted: %1 (%2x%3)]")
						.arg(displayName)
						.arg(att.width)
						.arg(att.height));
			}
		}
		if (!keptForMsg.isEmpty()) {
			++keptImageTurns;
		}
		msg.images = std::move(keptForMsg);
		if (!omittedNotes.isEmpty()) {
			if (!msg.content.trimmed().isEmpty()) {
				msg.content.append(QLatin1Char('\n'));
			}
			msg.content.append(omittedNotes.join(QLatin1Char('\n')));
		}
	}

	const AgentToolViewContext viewCtx = buildToolViewContext();
	if (_sendMeetingContext && viewCtx.canShareMeetingData) {
		req.meetingContext = viewCtx.meetingOverview;
	}
	if ((viewCtx.canShareMeetingData || viewCtx.canUseCatalogTools ||
			viewCtx.canUseContactTools || viewCtx.canUseClientPrefTools) &&
		(_profile.protocol == AgentProtocolDialect::OpenAiChatCompletionsSse ||
		 _profile.protocol == AgentProtocolDialect::OpenAiResponsesSse ||
		 _profile.protocol == AgentProtocolDialect::AnthropicMessagesSse ||
		 _profile.protocol == AgentProtocolDialect::GoogleGeminiSse)) {
		QVector<AgentToolDefinition> defs = _toolRegistry.toolDefinitions(viewCtx);
		if (_chatMode == AgentChatMode::Ask) {
			defs.erase(std::remove_if(defs.begin(), defs.end(), [](const AgentToolDefinition &def) {
				return def.risk != AgentToolRisk::ReadOnly;
			}), defs.end());
		}
		req.tools = std::move(defs);
	}

	AgentChatMessage assistantMsg;
	assistantMsg.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
	assistantMsg.role = AgentChatMessage::Role::Assistant;
	assistantMsg.content.clear();
	assistantMsg.status = AgentChatMessage::Status::Streaming;
	assistantMsg.timestamp = QDateTime::currentDateTime();
	_messages.push_back(assistantMsg);
	_streamingMessageId = assistantMsg.id;
	resetStreamingBuffers();
	emit messageAdded(_messages.last());

	const uint64_t nonce = ++_requestNonce;
	_activeRequestToken = token;

	_activeHandle = _transport->sendChat(req, this);
	emit streamingStateChanged(true);

	connect(_activeHandle, &AgentRequestHandle::deltaReceived, this,
		[this, nonce, token](const AgentStreamDelta &delta) {
			if (nonce != _requestNonce || !isGenerationCurrent(token)) {
				return;
			}
			if (!delta.reasoningDelta.isEmpty()) {
				if (_thinkingStartMs == 0) {
					_thinkingStartMs = QDateTime::currentMSecsSinceEpoch();
				}
				_pendingReasoningBuffer.append(delta.reasoningDelta);
				if (!_deltaFlushTimer.isActive()) {
					_deltaFlushTimer.start();
				}
			}
			if (!delta.textDelta.isEmpty()) {
				_pendingDeltaBuffer.append(delta.textDelta);
				if (!_deltaFlushTimer.isActive()) {
					_deltaFlushTimer.start();
				}
			}
			if (!delta.toolCalls.isEmpty()) {
				for (const auto &tc : delta.toolCalls) {
					_pendingToolCalls.push_back(tc);
				}
			}
			if (!delta.thinkingBlocks.isEmpty()) {
				for (const auto &tb : delta.thinkingBlocks) {
					_pendingThinkingBlocks.push_back(tb);
				}
			}
		});

	connect(_activeHandle, &AgentRequestHandle::finished, this,
		[this, nonce, token, apiKey, toolRound](bool success, const QString &safeError) {
			const auto handle = _activeHandle;
			_activeHandle.clear();
			if (handle) {
				handle->deleteLater();
			}

			if (nonce != _requestNonce) {
				return;
			}
			flushPendingDeltas();

			const int idx = indexOfMessage(_streamingMessageId);
			if (!isGenerationCurrent(token)) {
				if (idx >= 0) {
					finalizeThinkingForMessage(_messages[idx]);
					if (_messages[idx].status == AgentChatMessage::Status::Streaming) {
						_messages[idx].status = AgentChatMessage::Status::Cancelled;
						_messages[idx].errorMessage = QStringLiteral("Session changed before completion.");
					}
					emit messageUpdated(_messages[idx]);
				}
				_streamingMessageId.clear();
				resetStreamingBuffers();
				emit streamingStateChanged(false);
				return;
			}

			QVector<AgentToolCall> completedCalls = _pendingToolCalls;
			QVector<AgentThinkingBlock> completedThinkingBlocks = _pendingThinkingBlocks;
			resetStreamingBuffers();
			for (auto &tc : completedCalls) {
				if (tc.id.trimmed().isEmpty()) {
					tc.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
				}
			}

			if (idx >= 0) {
				finalizeThinkingForMessage(_messages[idx]);
				if (success && completedCalls.isEmpty() && _messages[idx].content.trimmed().isEmpty()) {
					_messages[idx].status = AgentChatMessage::Status::Failed;
					_messages[idx].errorMessage = QCoreApplication::translate(
						"MeetingUI", "The model returned an empty response.");
				} else if (success) {
					_messages[idx].status = AgentChatMessage::Status::Complete;
					if (!completedThinkingBlocks.isEmpty()) {
						_messages[idx].thinkingBlocks = std::move(completedThinkingBlocks);
					}
					if (!completedCalls.isEmpty()) {
						_messages[idx].toolCalls = completedCalls;
					} else {
						_messages[idx].followups = generateFollowupSuggestions(idx);
					}
				} else {
					_messages[idx].status = AgentChatMessage::Status::Failed;
					_messages[idx].errorMessage = safeError;
				}
				emit messageUpdated(_messages[idx]);
			}
			_thinkingStartMs = 0;
			_streamingMessageId.clear();
			syncActiveSessionSnapshot(true);
			schedulePersistSessions();

			if (success && !completedCalls.isEmpty()) {
				handleCompletedToolCalls(token, apiKey, toolRound, completedCalls);
				return;
			}

			emit streamingStateChanged(false);
			if (!success && !safeError.isEmpty()) {
				emit requestFailed(safeError);
			}
		});
}

void AgentCoordinator::handleCompletedToolCalls(
		const SessionGenerationToken &token,
		const QString &apiKey,
		int toolRound,
		const QVector<AgentToolCall> &toolCalls) {
	if (!isGenerationCurrent(token)) {
		emit streamingStateChanged(false);
		return;
	}

	if (_catalog && !_catalog->upcomingState().hasSnapshot) {
		const bool needsUpcomingSnapshot = std::any_of(
			toolCalls.cbegin(),
			toolCalls.cend(),
			[](const AgentToolCall &tc) {
				return tc.name == QStringLiteral("list_scheduled_meetings");
			});
		if (needsUpcomingSnapshot) {
			_catalog->refreshUpcoming();
			if (_catalog->upcomingState().refreshing) {
				const uint64_t waitNonce = _requestNonce;
				auto conn = std::make_shared<QMetaObject::Connection>();
				*conn = connect(_catalog, &MeetingCatalogController::upcomingChanged, this,
					[this, conn, waitNonce, token, apiKey, toolRound, toolCalls]() {
						if (_catalog && _catalog->upcomingState().refreshing) {
							return;
						}
						disconnect(*conn);
						if (waitNonce != _requestNonce || !isGenerationCurrent(token)) {
							emit streamingStateChanged(false);
							return;
						}
						handleCompletedToolCalls(token, apiKey, toolRound, toolCalls);
					});
				return;
			}
		}
	}

	bool executedReadOnly = false;
	bool queuedConfirmation = false;
	QSet<QString> roundActionIds;
	const AgentToolViewContext viewCtx = buildToolViewContext();

	QString hostAssistantId;
	for (int i = _messages.size() - 1; i >= 0; --i) {
		if (_messages[i].role == AgentChatMessage::Role::Assistant) {
			hostAssistantId = _messages[i].id;
			break;
		}
		if (_messages[i].role == AgentChatMessage::Role::User) {
			break;
		}
	}
	QVector<AgentResponsePart> toolParts;
	toolParts.reserve(toolCalls.size());

	for (const auto &rawCall : toolCalls) {
		if (!isGenerationCurrent(token)) {
			emit streamingStateChanged(false);
			return;
		}
		const QString callId = rawCall.id.trimmed().isEmpty()
			? QUuid::createUuid().toString(QUuid::WithoutBraces)
			: rawCall.id;

		AgentChatMessage toolMsg;
		toolMsg.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
		toolMsg.role = AgentChatMessage::Role::Tool;
		toolMsg.toolCallId = callId;
		toolMsg.toolName = rawCall.name;
		toolMsg.status = AgentChatMessage::Status::Complete;
		toolMsg.timestamp = QDateTime::currentDateTime();

		const auto def = _toolRegistry.findTool(rawCall.name);
		if (!def.has_value()) {
			toolMsg.content = QStringLiteral("{\"ok\":false,\"error\":\"Unknown meeting tool.\"}");
			_messages.push_back(toolMsg);
			const auto pres = MeetingToolRegistry::describeToolInvocation(rawCall.name, {}, false);
			AgentResponsePart part;
			part.type = AgentResponsePart::Type::ToolInvocation;
			part.toolCallId = callId;
			part.toolName = rawCall.name;
			part.invocationMessage = pres.invocationMessage;
			part.pastTenseMessage = pres.pastTenseMessage;
			part.argumentsJson = rawCall.argumentsJson;
			part.resultSummary = QStringLiteral("Unknown meeting tool.");
			part.isReadOnly = false;
			part.toolState = AgentResponsePart::ToolState::Failed;
			toolParts.push_back(std::move(part));
			executedReadOnly = true;
			continue;
		}

		QJsonObject args;
		QString argErr;
		if (!MeetingToolRegistry::parseToolArguments(rawCall.argumentsJson, args, argErr)) {
			QJsonObject errObj;
			errObj.insert(QStringLiteral("ok"), false);
			errObj.insert(QStringLiteral("error"), argErr);
			toolMsg.content = QString::fromUtf8(QJsonDocument(errObj).toJson(QJsonDocument::Compact));
			_messages.push_back(toolMsg);
			const auto pres = MeetingToolRegistry::describeToolInvocation(rawCall.name, {}, false);
			AgentResponsePart part;
			part.type = AgentResponsePart::Type::ToolInvocation;
			part.toolCallId = callId;
			part.toolName = rawCall.name;
			part.invocationMessage = pres.invocationMessage;
			part.pastTenseMessage = pres.pastTenseMessage;
			part.argumentsJson = rawCall.argumentsJson;
			part.resultSummary = argErr;
			part.isReadOnly = (def->risk == AgentToolRisk::ReadOnly);
			part.toolState = AgentResponsePart::ToolState::Failed;
			toolParts.push_back(std::move(part));
			executedReadOnly = true;
			continue;
		}

		if (def->risk == AgentToolRisk::ReadOnly && _permissionMode != AgentPermissionMode::ConfirmAll) {
			const AgentToolResult res = _toolRegistry.executeReadOnly(rawCall.name, args, viewCtx);
			toolMsg.content = res.content;
			_messages.push_back(toolMsg);
			const auto pres = MeetingToolRegistry::describeToolInvocation(rawCall.name, args, res.success);
			AgentResponsePart part;
			part.type = AgentResponsePart::Type::ToolInvocation;
			part.toolCallId = callId;
			part.toolName = rawCall.name;
			part.invocationMessage = pres.invocationMessage;
			part.pastTenseMessage = pres.pastTenseMessage;
			part.argumentsJson = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
			part.resultSummary = res.success
				? extractToolResultSummary(res.content)
				: (res.errorMessage.isEmpty() ? extractToolResultSummary(res.content) : res.errorMessage);
			part.isReadOnly = true;
			part.toolState = res.success
				? AgentResponsePart::ToolState::Completed
				: AgentResponsePart::ToolState::Failed;
			toolParts.push_back(std::move(part));
			executedReadOnly = true;
			continue;
		}

		PreparedPendingAction prepared;
		QString descErr;
		if (def->risk == AgentToolRisk::ReadOnly) {
			const auto pres = MeetingToolRegistry::describeToolInvocation(rawCall.name, args, true);
			prepared.title = pres.invocationMessage.isEmpty() ? rawCall.name : pres.invocationMessage;
			const QString argsCompact = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
			prepared.summary = (argsCompact.isEmpty() || argsCompact == QStringLiteral("{}"))
				? QCoreApplication::translate("MeetingUI", "Read-only query requiring confirmation under current permission mode.")
				: argsCompact;
			prepared.normalizedArgs = args;
		} else if (!_toolRegistry.preparePendingAction(rawCall.name, args, viewCtx, prepared, descErr)) {
			QJsonObject errObj;
			errObj.insert(QStringLiteral("ok"), false);
			errObj.insert(QStringLiteral("error"), descErr);
			toolMsg.content = QString::fromUtf8(QJsonDocument(errObj).toJson(QJsonDocument::Compact));
			_messages.push_back(toolMsg);
			const auto pres = MeetingToolRegistry::describeToolInvocation(rawCall.name, args, false);
			AgentResponsePart part;
			part.type = AgentResponsePart::Type::ToolInvocation;
			part.toolCallId = callId;
			part.toolName = rawCall.name;
			part.invocationMessage = pres.invocationMessage;
			part.pastTenseMessage = pres.pastTenseMessage;
			part.argumentsJson = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
			part.resultSummary = descErr;
			part.isReadOnly = false;
			part.toolState = AgentResponsePart::ToolState::Failed;
			toolParts.push_back(std::move(part));
			executedReadOnly = true;
			continue;
		}

		AgentPendingToolAction action;
		action.actionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
		action.toolCallId = callId;
		action.toolName = rawCall.name;
		action.title = prepared.title;
		action.summary = prepared.summary;
		action.arguments = prepared.normalizedArgs;
		action.scope = def->scope;
		action.concurrencyGroup = def->concurrencyGroup;
		action.generationToken = token;
		if (def->scope == AgentToolScope::ClientGlobal ||
			def->scope == AgentToolScope::AccountCatalog ||
			def->scope == AgentToolScope::AccountContacts) {
			action.generationToken.coordinatorSession = 0;
			action.generationToken.nativeRoomGeneration = 0;
		}
		action.status = AgentPendingActionStatus::Pending;
		action.createdAt = QDateTime::currentDateTime();

		toolMsg.pendingActionId = action.actionId;
		toolMsg.content = QStringLiteral("{\"ok\":true,\"status\":\"awaiting_user_confirmation\",\"actionId\":\"%1\"}")
			.arg(action.actionId);
		_messages.push_back(toolMsg);

		const auto pres = MeetingToolRegistry::describeToolInvocation(rawCall.name, prepared.normalizedArgs, true);
		AgentResponsePart part;
		part.type = AgentResponsePart::Type::ToolInvocation;
		part.toolCallId = callId;
		part.toolName = rawCall.name;
		part.invocationMessage = pres.invocationMessage;
		part.pastTenseMessage = prepared.title;
		part.argumentsJson = QString::fromUtf8(QJsonDocument(prepared.normalizedArgs).toJson(QJsonDocument::Compact));
		part.resultSummary = prepared.summary;
		part.actionId = action.actionId;
		part.isReadOnly = (def->risk == AgentToolRisk::ReadOnly);
		part.toolState = AgentResponsePart::ToolState::WaitingForConfirmation;
		toolParts.push_back(std::move(part));

		_pendingActions.push_back(action);
		roundActionIds.insert(action.actionId);
		emit pendingActionAdded(_pendingActions.last());
		queuedConfirmation = true;
	}

	trimStoredMessagesToLimit();

	if (!hostAssistantId.isEmpty() && !toolParts.isEmpty()) {
		const int hostIdx = indexOfMessage(hostAssistantId);
		if (hostIdx >= 0) {
			for (const auto &tp : toolParts) {
				_messages[hostIdx].parts.push_back(tp);
			}
			emit messageUpdated(_messages[hostIdx]);
		}
	}
	syncActiveSessionSnapshot(true);
	schedulePersistSessions();

	if (queuedConfirmation) {
		_activeToolRound.reset();
		_activeToolRound.turnNonce = _conversationNonce;
		_activeToolRound.token = token;
		_activeToolRound.apiKey = apiKey;
		_activeToolRound.toolRound = toolRound;
		_activeToolRound.hasReadOnlyResults = executedReadOnly;
		_activeToolRound.pendingActionIds = std::move(roundActionIds);
		emit streamingStateChanged(false);
		return;
	}

	if (executedReadOnly && toolRound < kMaxToolRounds && isGenerationCurrent(token)) {
		dispatchChatRequest(token, apiKey, toolRound + 1);
		return;
	}

	emit streamingStateChanged(false);
}

void AgentCoordinator::updateToolMessageForAction(const QString &actionId, const QString &statusJson) {
	if (actionId.isEmpty() || statusJson.isEmpty()) {
		return;
	}
	for (int i = _messages.size() - 1; i >= 0; --i) {
		if (_messages[i].role == AgentChatMessage::Role::Tool &&
			_messages[i].pendingActionId == actionId) {
			_messages[i].content = statusJson;
			emit messageUpdated(_messages[i]);
			return;
		}
	}
}

void AgentCoordinator::resolvePendingActionInRound(
		const QString &actionId,
		bool approved,
		bool succeeded,
		bool isMeetingTransitionSuccess,
		const QString &localSummaryMessage) {
	const auto appendLocalSummaries = [this](const QStringList &summaries) {
		for (const QString &summary : summaries) {
			if (summary.isEmpty()) {
				continue;
			}
			AgentChatMessage doneMsg;
			doneMsg.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
			doneMsg.role = AgentChatMessage::Role::Assistant;
			doneMsg.content = summary;
			AgentResponsePart mdPart;
			mdPart.type = AgentResponsePart::Type::Markdown;
			mdPart.text = summary;
			doneMsg.parts.push_back(std::move(mdPart));
			doneMsg.status = AgentChatMessage::Status::Complete;
			doneMsg.timestamp = QDateTime::currentDateTime();
			_messages.push_back(doneMsg);
			trimStoredMessagesToLimit();
			const int insertedIdx = _messages.size() - 1;
			_messages[insertedIdx].followups = generateFollowupSuggestions(insertedIdx);
			syncActiveSessionSnapshot(true);
			schedulePersistSessions();
			emit messageAdded(_messages.last());
		}
	};

	if (_activeToolRound.turnNonce != _conversationNonce ||
		!_activeToolRound.pendingActionIds.remove(actionId)) {
		if (approved && succeeded && !localSummaryMessage.isEmpty()) {
			appendLocalSummaries({localSummaryMessage});
		}
		return;
	}

	if (approved) {
		_activeToolRound.hadApprovedAction = true;
		if (!succeeded) {
			_activeToolRound.hadFailureOrRejection = true;
			_activeToolRound.allApprovedHaveLocalSummary = false;
		} else {
			if (localSummaryMessage.isEmpty()) {
				_activeToolRound.allApprovedHaveLocalSummary = false;
			} else {
				_activeToolRound.localSummaries.push_back(localSummaryMessage);
			}
			if (isMeetingTransitionSuccess) {
				_activeToolRound.hasMeetingTransitionSuccess = true;
			}
		}
	} else {
		_activeToolRound.hadFailureOrRejection = true;
	}

	if (!_activeToolRound.pendingActionIds.isEmpty()) {
		return;
	}

	const bool shouldContinueLlm =
		_activeToolRound.hadApprovedAction &&
		!_activeToolRound.hasMeetingTransitionSuccess &&
		_activeToolRound.toolRound < kMaxToolRounds &&
		isGenerationCurrent(_activeToolRound.token) &&
		(_activeToolRound.hasReadOnlyResults ||
		 _activeToolRound.hadFailureOrRejection ||
		 !_activeToolRound.allApprovedHaveLocalSummary);

	if (shouldContinueLlm) {
		const SessionGenerationToken token = _activeToolRound.token;
		const QString apiKey = _activeToolRound.apiKey;
		const int nextRound = _activeToolRound.toolRound + 1;
		_activeToolRound.reset();
		dispatchChatRequest(token, apiKey, nextRound);
		return;
	}

	if (!_activeToolRound.hadFailureOrRejection && _activeToolRound.hadApprovedAction) {
		appendLocalSummaries(_activeToolRound.localSummaries);
	}
	_activeToolRound.reset();
}

bool AgentCoordinator::approvePendingAction(const QString &actionId) {
	const int idx = indexOfPendingAction(actionId);
	if (idx < 0) {
		return false;
	}
	AgentPendingToolAction &action = _pendingActions[idx];
	if (action.status != AgentPendingActionStatus::Pending) {
		return false;
	}

	if (!isGenerationCurrent(action.generationToken)) {
		action.status = AgentPendingActionStatus::Expired;
		action.resultMessage = QCoreApplication::translate(
			"MeetingUI", "Action expired because the meeting or account session changed.");
		QJsonObject statusObj;
		statusObj.insert(QStringLiteral("ok"), false);
		statusObj.insert(QStringLiteral("status"), QStringLiteral("expired"));
		statusObj.insert(QStringLiteral("error"), action.resultMessage);
		updateToolMessageForAction(action.actionId,
			QString::fromUtf8(QJsonDocument(statusObj).toJson(QJsonDocument::Compact)));
		const auto pres = MeetingToolRegistry::describeToolInvocation(action.toolName, action.arguments, false);
		updateToolInvocationPartForAction(
			action.actionId,
			AgentResponsePart::ToolState::Failed,
			pres.pastTenseMessage,
			action.resultMessage);
		emit pendingActionUpdated(action);
		resolvePendingActionInRound(action.actionId, false, false, false, {});
		return false;
	}

	if (!_toolRegistry.isToolAvailable(action.toolName, buildToolViewContext())) {
		action.status = AgentPendingActionStatus::Failed;
		action.resultMessage = QCoreApplication::translate(
			"MeetingUI", "Action is no longer available in the current meeting or permission state.");
		QJsonObject statusObj;
		statusObj.insert(QStringLiteral("ok"), false);
		statusObj.insert(QStringLiteral("status"), QStringLiteral("failed"));
		statusObj.insert(QStringLiteral("error"), action.resultMessage);
		updateToolMessageForAction(action.actionId,
			QString::fromUtf8(QJsonDocument(statusObj).toJson(QJsonDocument::Compact)));
		const auto pres = MeetingToolRegistry::describeToolInvocation(action.toolName, action.arguments, false);
		updateToolInvocationPartForAction(
			action.actionId,
			AgentResponsePart::ToolState::Failed,
			pres.pastTenseMessage,
			action.resultMessage);
		emit pendingActionUpdated(action);
		resolvePendingActionInRound(action.actionId, false, false, false, {});
		return false;
	}

	const int groupKey = static_cast<int>(action.concurrencyGroup);
	if (action.concurrencyGroup != AgentConcurrencyGroup::None &&
		_inFlightActionByGroup.contains(groupKey)) {
		return false;
	}

	action.status = AgentPendingActionStatus::Approved;
	const auto toolDef = _toolRegistry.findTool(action.toolName);
	struct AsyncCompletionBridge {
		bool returned = false;
		bool completedSync = false;
		ToolExecutionOutcome syncOutcome;
	};
	auto asyncBridge = std::make_shared<AsyncCompletionBridge>();
	const QString actionIdCopy = action.actionId;
	const SessionGenerationToken expectedToken = action.generationToken;
	const uint64_t expectedConversationNonce = _conversationNonce;

	ToolExecutionOutcome outcome;
	if (toolDef.has_value() && toolDef->risk == AgentToolRisk::ReadOnly) {
		outcome.toolResult = _toolRegistry.executeReadOnly(
			action.toolName,
			action.arguments,
			buildToolViewContext());
		outcome.uiStatusMessage = outcome.toolResult.success
			? extractToolResultSummary(outcome.toolResult.content)
			: outcome.toolResult.errorMessage;
	} else {
		AgentToolExecuteContext execCtx;
		execCtx.meeting = _meeting.data();
		execCtx.catalog = _catalog.data();
		execCtx.contacts = _contacts.data();
		execCtx.callInvite = _callInvite.data();
		execCtx.sessionManager = &_sessionManager;
		execCtx.agentCoordinator = this;
		execCtx.hostSettings = &_hostSettings;
		const QPointer<AgentCoordinator> self(this);
		execCtx.completeAsync = [self, asyncBridge, actionIdCopy, groupKey, expectedToken, expectedConversationNonce](
				const ToolExecutionOutcome &finalOutcome) {
			if (!asyncBridge->returned) {
				asyncBridge->syncOutcome = finalOutcome;
				asyncBridge->syncOutcome.asyncInFlight = false;
				asyncBridge->completedSync = true;
				return;
			}
			if (!self) {
				return;
			}
			if (groupKey != static_cast<int>(AgentConcurrencyGroup::None)) {
				if (self->_inFlightActionByGroup.value(groupKey) != actionIdCopy) {
					return;
				}
				self->_inFlightActionByGroup.remove(groupKey);
			}
			if (self->_conversationNonce != expectedConversationNonce ||
				!self->isGenerationCurrent(expectedToken)) {
				return;
			}
			self->finalizeActionOutcome(actionIdCopy, finalOutcome);
		};
		outcome = _toolRegistry.executeConfirmedActionWithOutcome(
			action.toolName,
			action.arguments,
			execCtx);
	}
	asyncBridge->returned = true;
	if (asyncBridge->completedSync) {
		outcome = std::move(asyncBridge->syncOutcome);
		outcome.asyncInFlight = false;
	}
	const AgentToolResult &res = outcome.toolResult;

	if (action.concurrencyGroup == AgentConcurrencyGroup::CatalogWrite) {
		if (!res.success) {
			action.status = AgentPendingActionStatus::Failed;
			action.resultMessage = res.errorMessage;
			updateToolMessageForAction(action.actionId, res.content);
			const auto pres = MeetingToolRegistry::describeToolInvocation(action.toolName, action.arguments, false);
			updateToolInvocationPartForAction(
				action.actionId,
				AgentResponsePart::ToolState::Failed,
				pres.pastTenseMessage,
				action.resultMessage);
			emit pendingActionUpdated(action);
			resolvePendingActionInRound(action.actionId, true, false, false, {});
			return false;
		}
		if (outcome.asyncInFlight && _catalog && _catalog->lastWriteResult().inFlight) {
			_inFlightActionByGroup.insert(groupKey, action.actionId);
			action.resultMessage = QCoreApplication::translate("MeetingUI", "Submitting request...");
			const auto pres = MeetingToolRegistry::describeToolInvocation(action.toolName, action.arguments, true);
			updateToolInvocationPartForAction(
				action.actionId,
				AgentResponsePart::ToolState::Running,
				pres.invocationMessage,
				action.resultMessage);
			emit pendingActionUpdated(action);
			return true;
		}
		finalizeCatalogAction(action);
		return action.status == AgentPendingActionStatus::Executed;
	}

	if (outcome.asyncInFlight && res.success) {
		if (action.concurrencyGroup != AgentConcurrencyGroup::None) {
			_inFlightActionByGroup.insert(groupKey, action.actionId);
		}
		action.resultMessage = outcome.uiStatusMessage.isEmpty()
			? QCoreApplication::translate("MeetingUI", "Submitting request...")
			: outcome.uiStatusMessage;
		const auto pres = MeetingToolRegistry::describeToolInvocation(action.toolName, action.arguments, true);
		updateToolInvocationPartForAction(
			action.actionId,
			AgentResponsePart::ToolState::Running,
			pres.invocationMessage,
			action.resultMessage);
		emit pendingActionUpdated(action);
		return true;
	}

	return finalizeActionOutcome(actionIdCopy, outcome);
}

bool AgentCoordinator::finalizeActionOutcome(const QString &actionId, const ToolExecutionOutcome &outcome) {
	const int idx = indexOfPendingAction(actionId);
	if (idx < 0) {
		return false;
	}
	AgentPendingToolAction &action = _pendingActions[idx];
	const AgentToolResult &res = outcome.toolResult;
	if (res.success) {
		action.status = AgentPendingActionStatus::Executed;
		action.resultMessage = outcome.uiStatusMessage.isEmpty()
			? QCoreApplication::translate("MeetingUI", "Executed.")
			: outcome.uiStatusMessage;
	} else {
		action.status = AgentPendingActionStatus::Failed;
		action.resultMessage = res.errorMessage.isEmpty()
			? QCoreApplication::translate("MeetingUI", "Operation failed.")
			: res.errorMessage;
	}
	const bool isMeetingTransition =
		(action.concurrencyGroup == AgentConcurrencyGroup::MeetingTransition) && res.success;
	const QString executedActionId = action.actionId;
	updateToolMessageForAction(executedActionId, res.content);
	{
		const auto pres = MeetingToolRegistry::describeToolInvocation(action.toolName, action.arguments, res.success);
		updateToolInvocationPartForAction(
			executedActionId,
			res.success ? AgentResponsePart::ToolState::Completed : AgentResponsePart::ToolState::Failed,
			pres.pastTenseMessage,
			action.resultMessage);
	}
	emit pendingActionUpdated(action);

	resolvePendingActionInRound(
		executedActionId,
		true,
		res.success,
		isMeetingTransition,
		res.success ? outcome.assistantFollowupMessage : QString());
	return res.success;
}

void AgentCoordinator::onCatalogWriteStateChanged() {
	const int groupKey = static_cast<int>(AgentConcurrencyGroup::CatalogWrite);
	const QString actionId = _inFlightActionByGroup.value(groupKey);
	if (!_catalog || _catalog->lastWriteResult().inFlight || actionId.isEmpty()) {
		return;
	}
	_inFlightActionByGroup.remove(groupKey);

	const int idx = indexOfPendingAction(actionId);
	if (idx < 0) {
		return;
	}
	finalizeCatalogAction(_pendingActions[idx]);
}

void AgentCoordinator::finalizeCatalogAction(AgentPendingToolAction &action) {
	if (!_catalog) {
		action.status = AgentPendingActionStatus::Failed;
		action.resultMessage = QCoreApplication::translate("MeetingUI", "Meeting catalog is unavailable.");
		QJsonObject statusObj;
		statusObj.insert(QStringLiteral("ok"), false);
		statusObj.insert(QStringLiteral("status"), QStringLiteral("failed"));
		statusObj.insert(QStringLiteral("error"), action.resultMessage);
		const QString actionIdCopy = action.actionId;
		updateToolMessageForAction(actionIdCopy,
			QString::fromUtf8(QJsonDocument(statusObj).toJson(QJsonDocument::Compact)));
		const auto pres = MeetingToolRegistry::describeToolInvocation(action.toolName, action.arguments, false);
		updateToolInvocationPartForAction(
			actionIdCopy,
			AgentResponsePart::ToolState::Failed,
			pres.pastTenseMessage,
			action.resultMessage);
		emit pendingActionUpdated(action);
		resolvePendingActionInRound(actionIdCopy, true, false, false, {});
		return;
	}

	const auto &writeRes = _catalog->lastWriteResult();
	if (!writeRes.success) {
		action.status = AgentPendingActionStatus::Failed;
		action.resultMessage = writeRes.error.message.isEmpty()
			? QCoreApplication::translate("MeetingUI", "Operation failed.")
			: writeRes.error.message;
		QJsonObject statusObj;
		statusObj.insert(QStringLiteral("ok"), false);
		statusObj.insert(QStringLiteral("status"), QStringLiteral("failed"));
		statusObj.insert(QStringLiteral("error"), action.resultMessage);
		const QString actionIdCopy = action.actionId;
		updateToolMessageForAction(actionIdCopy,
			QString::fromUtf8(QJsonDocument(statusObj).toJson(QJsonDocument::Compact)));
		const auto pres = MeetingToolRegistry::describeToolInvocation(action.toolName, action.arguments, false);
		updateToolInvocationPartForAction(
			actionIdCopy,
			AgentResponsePart::ToolState::Failed,
			pres.pastTenseMessage,
			action.resultMessage);
		emit pendingActionUpdated(action);
		resolvePendingActionInRound(actionIdCopy, true, false, false, {});
		return;
	}

	action.status = AgentPendingActionStatus::Executed;
	QString assistantSummary;
	if (action.toolName == QStringLiteral("schedule_meeting")) {
		action.resultMessage = QCoreApplication::translate("MeetingUI", "Scheduled (ID: %1).").arg(writeRes.meetingId);
		assistantSummary = MeetingToolRegistry::formatScheduledMeetingCompletionSummary(
			action.arguments,
			writeRes.meetingId);
	} else if (action.toolName == QStringLiteral("update_scheduled_meeting")) {
		action.resultMessage = QCoreApplication::translate("MeetingUI", "Meeting updated and schedule list refreshed.");
		assistantSummary = QCoreApplication::translate(
			"MeetingUI",
			"Scheduled meeting %1 has been updated, and the schedule list has been refreshed.")
			.arg(writeRes.meetingId);
	} else if (action.toolName == QStringLiteral("cancel_scheduled_meeting")) {
		action.resultMessage = QCoreApplication::translate("MeetingUI", "Meeting cancelled and schedule list refreshed.");
		assistantSummary = QCoreApplication::translate(
			"MeetingUI",
			"Scheduled meeting %1 has been cancelled, and the schedule list has been refreshed.")
			.arg(writeRes.meetingId);
	} else {
		action.resultMessage = QCoreApplication::translate("MeetingUI", "Executed.");
	}

	QJsonObject okObj;
	okObj.insert(QStringLiteral("ok"), true);
	okObj.insert(QStringLiteral("status"), QStringLiteral("executed"));
	if (!writeRes.meetingId.isEmpty()) {
		okObj.insert(QStringLiteral("meetingId"), writeRes.meetingId);
	}
	okObj.insert(QStringLiteral("summary"), action.resultMessage);
	const QString actionIdCopy = action.actionId;
	updateToolMessageForAction(actionIdCopy,
		QString::fromUtf8(QJsonDocument(okObj).toJson(QJsonDocument::Compact)));
	const auto pres = MeetingToolRegistry::describeToolInvocation(action.toolName, action.arguments, true);
	updateToolInvocationPartForAction(
		actionIdCopy,
		AgentResponsePart::ToolState::Completed,
		pres.pastTenseMessage,
		action.resultMessage);
	emit pendingActionUpdated(action);

	resolvePendingActionInRound(actionIdCopy, true, true, false, assistantSummary);
}

bool AgentCoordinator::rejectPendingAction(const QString &actionId) {
	const int idx = indexOfPendingAction(actionId);
	if (idx < 0) {
		return false;
	}
	AgentPendingToolAction &action = _pendingActions[idx];
	if (action.status != AgentPendingActionStatus::Pending) {
		return false;
	}
	action.status = AgentPendingActionStatus::Rejected;
	action.resultMessage = QCoreApplication::translate("MeetingUI", "Cancelled by user.");
	const QString actionIdCopy = action.actionId;
	updateToolMessageForAction(actionIdCopy,
		QStringLiteral("{\"ok\":false,\"status\":\"rejected_by_user\"}"));
	updateToolInvocationPartForAction(
		actionIdCopy,
		AgentResponsePart::ToolState::Rejected,
		QCoreApplication::translate("MeetingUI", "Cancelled: %1").arg(action.title),
		action.resultMessage);
	emit pendingActionUpdated(action);
	resolvePendingActionInRound(actionIdCopy, false, false, false, {});
	return true;
}

void AgentCoordinator::expireAllPendingActions(const QString &reason, bool meetingScopedOnly) {
	for (auto &action : _pendingActions) {
		if (action.status == AgentPendingActionStatus::Pending) {
			if (meetingScopedOnly &&
				action.scope != AgentToolScope::ActiveMeeting &&
				action.scope != AgentToolScope::MeetingHost) {
				continue;
			}
			action.status = AgentPendingActionStatus::Expired;
			action.resultMessage = reason;
			QJsonObject expObj;
			expObj.insert(QStringLiteral("ok"), false);
			expObj.insert(QStringLiteral("status"), QStringLiteral("expired"));
			expObj.insert(QStringLiteral("reason"), reason);
			updateToolMessageForAction(action.actionId,
				QString::fromUtf8(QJsonDocument(expObj).toJson(QJsonDocument::Compact)));
			const auto pres = MeetingToolRegistry::describeToolInvocation(action.toolName, action.arguments, false);
			updateToolInvocationPartForAction(
				action.actionId,
				AgentResponsePart::ToolState::Failed,
				pres.pastTenseMessage,
				reason);
			emit pendingActionUpdated(action);
			_activeToolRound.pendingActionIds.remove(action.actionId);
		}
	}
	if (_activeToolRound.pendingActionIds.isEmpty()) {
		_activeToolRound.reset();
	}
}

void AgentCoordinator::finalizeThinkingForMessage(AgentChatMessage &message) {
	message.isThinkingActive = false;
	if (!message.reasoningContent.isEmpty() && message.thinkingDurationMs <= 0 && _thinkingStartMs > 0) {
		message.thinkingDurationMs = qMax<qint64>(1, QDateTime::currentMSecsSinceEpoch() - _thinkingStartMs);
	}
	for (auto &part : message.parts) {
		if (part.type == AgentResponsePart::Type::Thinking) {
			part.active = false;
			part.durationMs = message.thinkingDurationMs;
		}
	}
}

void AgentCoordinator::updateToolInvocationPartForAction(
		const QString &actionId,
		AgentResponsePart::ToolState state,
		const QString &pastTenseMessage,
		const QString &resultSummary) {
	if (actionId.isEmpty()) {
		return;
	}
	for (int i = _messages.size() - 1; i >= 0; --i) {
		if (_messages[i].role != AgentChatMessage::Role::Assistant) {
			continue;
		}
		for (auto &part : _messages[i].parts) {
			if (part.type == AgentResponsePart::Type::ToolInvocation && part.actionId == actionId) {
				part.toolState = state;
				if (!pastTenseMessage.isEmpty()) {
					part.pastTenseMessage = pastTenseMessage;
				}
				if (!resultSummary.isEmpty()) {
					part.resultSummary = resultSummary;
				}
				emit messageUpdated(_messages[i]);
				return;
			}
		}
	}
}

QVector<AgentFollowupSuggestion> AgentCoordinator::generateFollowupSuggestions(
		int assistantMessageIndex) const {
	if (assistantMessageIndex < 0 || assistantMessageIndex >= _messages.size()) {
		return {};
	}
	const AgentChatMessage &msg = _messages[assistantMessageIndex];
	if (msg.role != AgentChatMessage::Role::Assistant ||
		msg.status != AgentChatMessage::Status::Complete ||
		msg.content.trimmed().isEmpty()) {
		return {};
	}

	QString lastUserPrompt;
	QSet<QString> ranTools;
	for (int i = assistantMessageIndex; i >= 0; --i) {
		if (_messages[i].role == AgentChatMessage::Role::Tool && !_messages[i].toolName.isEmpty()) {
			ranTools.insert(_messages[i].toolName);
		} else if (_messages[i].role == AgentChatMessage::Role::User) {
			lastUserPrompt = _messages[i].content.trimmed();
			break;
		}
	}

	QVector<AgentFollowupSuggestion> candidates;
	if (ranTools.contains(QStringLiteral("get_telemetry_snapshot"))) {
		candidates.push_back({
			QCoreApplication::translate("MeetingUI", "Check participants"),
			QCoreApplication::translate("MeetingUI", "List the current meeting participants and their media status.")
		});
		if (_chatMode == AgentChatMode::Agent) {
			candidates.push_back({
				QCoreApplication::translate("MeetingUI", "Adjust join media settings"),
				QCoreApplication::translate("MeetingUI", "Show my join media preferences.")
			});
		}
	} else if (ranTools.contains(QStringLiteral("list_scheduled_meetings")) ||
		ranTools.contains(QStringLiteral("schedule_meeting"))) {
		candidates.push_back({
			QCoreApplication::translate("MeetingUI", "Join media defaults"),
			QCoreApplication::translate("MeetingUI", "Show my default microphone, camera, and speaker settings for joining meetings.")
		});
		candidates.push_back({
			QCoreApplication::translate("MeetingUI", "Check app preferences"),
			QCoreApplication::translate("MeetingUI", "Show my current client preferences.")
		});
	} else if (ranTools.contains(QStringLiteral("get_recent_chat"))) {
		candidates.push_back({
			QCoreApplication::translate("MeetingUI", "Extract action items"),
			QCoreApplication::translate("MeetingUI", "List the key decisions and action items from the recent meeting chat.")
		});
	} else if (isInMeeting() && (!isEncryptedMeeting() || isE2eeContextAuthorized())) {
		if (!ranTools.contains(QStringLiteral("get_telemetry_snapshot"))) {
			candidates.push_back({
				QCoreApplication::translate("MeetingUI", "Diagnose A/V quality"),
				QCoreApplication::translate("MeetingUI", "Check current audio/video telemetry and network quality.")
			});
		}
		if (!ranTools.contains(QStringLiteral("get_recent_chat"))) {
			candidates.push_back({
				QCoreApplication::translate("MeetingUI", "Summarize chat"),
				QCoreApplication::translate("MeetingUI", "Summarize the recent meeting chat messages.")
			});
		}
	} else if (_sessionManager.isLoggedIn() && _catalog) {
		candidates.push_back({
			QCoreApplication::translate("MeetingUI", "Upcoming schedule"),
			QCoreApplication::translate("MeetingUI", "List my upcoming scheduled meetings.")
		});
		candidates.push_back({
			QCoreApplication::translate("MeetingUI", "Check client preferences"),
			QCoreApplication::translate("MeetingUI", "Show my current client preferences.")
		});
	} else {
		candidates.push_back({
			QCoreApplication::translate("MeetingUI", "Check client preferences"),
			QCoreApplication::translate("MeetingUI", "Show my current client preferences.")
		});
	}

	QVector<AgentFollowupSuggestion> out;
	for (const auto &c : candidates) {
		if (!lastUserPrompt.isEmpty() &&
			QString::compare(c.prompt.trimmed(), lastUserPrompt, Qt::CaseInsensitive) == 0) {
			continue;
		}
		out.push_back(c);
		if (out.size() >= 2) {
			break;
		}
	}
	return out;
}

void AgentCoordinator::flushPendingDeltas() {
	if ((_pendingDeltaBuffer.isEmpty() && _pendingReasoningBuffer.isEmpty()) ||
		_streamingMessageId.isEmpty()) {
		_pendingDeltaBuffer.clear();
		_pendingReasoningBuffer.clear();
		return;
	}
	const int idx = indexOfMessage(_streamingMessageId);
	if (idx >= 0) {
		AgentChatMessage &msg = _messages[idx];
		if (!_pendingReasoningBuffer.isEmpty()) {
			msg.reasoningContent.append(_pendingReasoningBuffer);
			msg.isThinkingActive = true;
			int thinkingIdx = -1;
			for (int i = msg.parts.size() - 1; i >= 0; --i) {
				if (msg.parts[i].type == AgentResponsePart::Type::Thinking) {
					thinkingIdx = i;
					break;
				}
			}
			if (thinkingIdx < 0) {
				AgentResponsePart part;
				part.type = AgentResponsePart::Type::Thinking;
				part.active = true;
				part.text = _pendingReasoningBuffer;
				msg.parts.push_back(std::move(part));
			} else {
				msg.parts[thinkingIdx].text.append(_pendingReasoningBuffer);
				msg.parts[thinkingIdx].active = true;
			}
			_pendingReasoningBuffer.clear();
		}
		if (!_pendingDeltaBuffer.isEmpty()) {
			if (msg.isThinkingActive) {
				finalizeThinkingForMessage(msg);
			}
			msg.content.append(_pendingDeltaBuffer);
			if (!msg.parts.isEmpty() && msg.parts.last().type == AgentResponsePart::Type::Markdown) {
				msg.parts.last().text.append(_pendingDeltaBuffer);
			} else {
				AgentResponsePart part;
				part.type = AgentResponsePart::Type::Markdown;
				part.text = _pendingDeltaBuffer;
				msg.parts.push_back(std::move(part));
			}
			_pendingDeltaBuffer.clear();
		}
		emit messageUpdated(_messages[idx]);
	} else {
		_pendingDeltaBuffer.clear();
		_pendingReasoningBuffer.clear();
	}
}

void AgentCoordinator::resetStreamingBuffers() {
	_pendingDeltaBuffer.clear();
	_pendingReasoningBuffer.clear();
	_thinkingStartMs = 0;
	_pendingToolCalls.clear();
	_pendingThinkingBlocks.clear();
}

void AgentCoordinator::cancelActiveRequestWithReason(
		AgentChatMessage::Status terminalStatus,
		const QString &reason) {
	_deltaFlushTimer.stop();
	flushPendingDeltas();

	if (!_activeHandle && _streamingMessageId.isEmpty()) {
		_thinkingStartMs = 0;
		return;
	}

	++_requestNonce;
	const auto handle = _activeHandle;
	_activeHandle.clear();
	if (handle) {
		handle->disconnect(this);
		handle->cancel();
		handle->deleteLater();
	}

	const int idx = indexOfMessage(_streamingMessageId);
	if (idx >= 0) {
		finalizeThinkingForMessage(_messages[idx]);
		if (_messages[idx].status == AgentChatMessage::Status::Streaming) {
			_messages[idx].status = terminalStatus;
			_messages[idx].errorMessage = reason;
		}
		emit messageUpdated(_messages[idx]);
	}
	_streamingMessageId.clear();
	resetStreamingBuffers();
	_activeRequestToken = SessionGenerationToken{};
	syncActiveSessionSnapshot(true);
	schedulePersistSessions();
	emit streamingStateChanged(false);
}

void AgentCoordinator::stopActiveRequest() {
	++_conversationNonce;
	_activeToolRound.reset();
	cancelActiveRequestWithReason(AgentChatMessage::Status::Cancelled,
		QStringLiteral("Stopped by user."));
}

void AgentCoordinator::clearHistory() {
	stopActiveRequest();
	expireAllPendingActions(QCoreApplication::translate("MeetingUI", "Conversation cleared."));
	_activeToolRound.reset();
	_inFlightActionByGroup.clear();
	_pendingActions.clear();
	_messages.clear();
	resetStreamingBuffers();

	const int activeIdx = indexOfSession(_activeSessionId);
	if (activeIdx >= 0) {
		_sessions[activeIdx].messages.clear();
		_sessions[activeIdx].summary.title = QCoreApplication::translate("MeetingUI", "New Chat");
		_sessions[activeIdx].summary.messageCount = 0;
		_sessions[activeIdx].summary.updatedAt = QDateTime::currentDateTime();
	}
	flushPersistSessions();
	emit historyCleared();
	emit sessionsChanged();
}

void AgentCoordinator::onMeetingEnded() {
	if (_activeHandle && _activeRequestToken.coordinatorSession != 0) {
		cancelActiveRequestWithReason(AgentChatMessage::Status::Cancelled,
			QStringLiteral("Meeting ended before response completed."));
	}
	expireAllPendingActions(QCoreApplication::translate("MeetingUI", "Meeting ended."), true);

	for (const auto &conn : _meetingConnections) {
		disconnect(conn);
	}
	_meetingConnections.clear();

	++_meetingAttachmentGeneration;
	_meeting.clear();
	_e2eeAuthorized = false;
	_e2eeAuthorizedMeetingId.clear();
	_e2eeAuthorizedSession = 0;
	_recentMeetingChat.clear();
	_recentTranscripts.clear();
	_latestTelemetrySnapshot.clear();
	emit contextStateChanged();
}

void AgentCoordinator::onAuthSessionChanged() {
	cancelActiveRequestWithReason(AgentChatMessage::Status::Cancelled,
		QStringLiteral("Account session changed."));
	expireAllPendingActions(QCoreApplication::translate("MeetingUI", "Account session changed."));
	_persistTimer.stop();
	flushPersistSessions();

	_activeToolRound.reset();
	_inFlightActionByGroup.clear();
	_pendingActions.clear();
	_messages.clear();
	resetStreamingBuffers();

	_cachedModels.clear();
	_cachedSpeechModels.clear();
	_modelCapabilities.clear();
	_lastFilteredNonChatModelCount = 0;
	_cachedExternalSpeechKey.clear();
	_cachedExternalSpeechModels.clear();
	_externalSttModelCapabilities.clear();
	cancelProbe();
	cancelSpeechProbe();
	if (_modelsHandle) {
		_modelsHandle->disconnect(this);
		_modelsHandle->cancel();
		_modelsHandle->deleteLater();
		_modelsHandle.clear();
	}
	if (_sttModelsHandle) {
		_sttModelsHandle->disconnect(this);
		_sttModelsHandle->cancel();
		_sttModelsHandle->deleteLater();
		_sttModelsHandle.clear();
	}

	_currentAccountKey = AgentSessionStore::accountScopeKey(
		_sessionManager.isLoggedIn(),
		_sessionManager.userId());
	loadAccountSessionsFromDisk();
	emit historyCleared();
	emit sessionsChanged();
}

int AgentCoordinator::indexOfSession(const QString &sessionId) const {
	for (int i = 0; i < _sessions.size(); ++i) {
		if (_sessions[i].summary.sessionId == sessionId) {
			return i;
		}
	}
	return -1;
}

void AgentCoordinator::loadAccountSessionsFromDisk() {
	_sessions.clear();
	_activeSessionId.clear();
	_messages.clear();

	if (_profile.persistChatHistory) {
		auto loaded = _sessionStore.loadAccountSessions(_currentAccountKey);
		_sessions = std::move(loaded.sessions);
		_activeSessionId = loaded.activeSessionId;
	}

	int activeIdx = indexOfSession(_activeSessionId);
	if (_sessions.isEmpty()) {
		AgentChatSessionData fresh;
		fresh.summary.sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
		fresh.summary.title = QCoreApplication::translate("MeetingUI", "New Chat");
		fresh.summary.createdAt = QDateTime::currentDateTime();
		fresh.summary.updatedAt = fresh.summary.createdAt;
		fresh.summary.chatMode = _chatMode;
		fresh.summary.messageCount = 0;
		_sessions.push_back(std::move(fresh));
		activeIdx = 0;
	} else if (activeIdx < 0) {
		activeIdx = 0;
	}

	_activeSessionId = _sessions[activeIdx].summary.sessionId;
	_messages = _sessions[activeIdx].messages;
	trimStoredMessagesToLimit();
	if (!_messages.isEmpty()) {
		const auto savedMode = _sessions[activeIdx].summary.chatMode;
		if (savedMode != _chatMode) {
			_chatMode = savedMode;
			emit chatModeChanged(_chatMode);
		}
	}
}

void AgentCoordinator::syncActiveSessionSnapshot(bool updateTimestamp) const {
	int activeIdx = indexOfSession(_activeSessionId);
	if (activeIdx < 0) {
		AgentChatSessionData fresh;
		fresh.summary.sessionId = _activeSessionId.isEmpty()
			? QUuid::createUuid().toString(QUuid::WithoutBraces)
			: _activeSessionId;
		fresh.summary.title = QCoreApplication::translate("MeetingUI", "New Chat");
		fresh.summary.createdAt = QDateTime::currentDateTime();
		fresh.summary.updatedAt = fresh.summary.createdAt;
		fresh.summary.chatMode = _chatMode;
		_sessions.push_front(std::move(fresh));
		activeIdx = 0;
	}

	auto &session = _sessions[activeIdx];
	session.messages = _messages;
	session.summary.chatMode = _chatMode;
	session.summary.messageCount = _messages.size();
	if (updateTimestamp) {
		session.summary.updatedAt = QDateTime::currentDateTime();
	}
	const QString defaultTitle = QCoreApplication::translate("MeetingUI", "New Chat");
	if (!_messages.isEmpty() &&
		(session.summary.title.trimmed().isEmpty() || session.summary.title == defaultTitle)) {
		session.summary.title = AgentSessionStore::deriveSessionTitle(_messages);
	}
}

void AgentCoordinator::schedulePersistSessions() {
	if (!_profile.persistChatHistory) {
		return;
	}
	if (!_persistTimer.isActive()) {
		_persistTimer.start();
	}
}

void AgentCoordinator::flushPersistSessions() {
	_persistTimer.stop();
	if (!_profile.persistChatHistory) {
		return;
	}
	syncActiveSessionSnapshot(false);
	_sessionStore.saveAccountSessions(_currentAccountKey, _activeSessionId, _sessions);
}

QVector<AgentChatSessionSummary> AgentCoordinator::sessionSummaries() const {
	syncActiveSessionSnapshot(false);
	QVector<AgentChatSessionSummary> out;
	out.reserve(_sessions.size());
	for (const auto &s : _sessions) {
		out.push_back(s.summary);
	}
	std::stable_sort(out.begin(), out.end(), [](const AgentChatSessionSummary &a, const AgentChatSessionSummary &b) {
		return a.updatedAt > b.updatedAt;
	});
	return out;
}

QString AgentCoordinator::createNewSession() {
	if (_messages.isEmpty() && indexOfSession(_activeSessionId) >= 0) {
		return _activeSessionId;
	}
	stopActiveRequest();
	expireAllPendingActions(QCoreApplication::translate("MeetingUI", "Switched conversation."));
	_activeToolRound.reset();
	_inFlightActionByGroup.clear();
	_pendingActions.clear();

	syncActiveSessionSnapshot(true);

	AgentChatSessionData fresh;
	fresh.summary.sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	fresh.summary.title = QCoreApplication::translate("MeetingUI", "New Chat");
	fresh.summary.createdAt = QDateTime::currentDateTime();
	fresh.summary.updatedAt = fresh.summary.createdAt;
	fresh.summary.chatMode = _chatMode;
	fresh.summary.messageCount = 0;

	_sessions.push_front(std::move(fresh));
	_activeSessionId = _sessions.front().summary.sessionId;
	_messages.clear();
	resetStreamingBuffers();

	flushPersistSessions();
	emit historyCleared();
	emit sessionsChanged();
	return _activeSessionId;
}

bool AgentCoordinator::switchSession(const QString &sessionId) {
	if (sessionId.isEmpty() || indexOfSession(sessionId) < 0) {
		return false;
	}
	if (sessionId == _activeSessionId) {
		return true;
	}

	stopActiveRequest();
	expireAllPendingActions(QCoreApplication::translate("MeetingUI", "Switched conversation."));
	_activeToolRound.reset();
	_inFlightActionByGroup.clear();
	_pendingActions.clear();

	syncActiveSessionSnapshot(false);

	// Prune empty non-active session when switching away cleanly.
	for (int i = _sessions.size() - 1; i >= 0; --i) {
		if (_sessions[i].summary.sessionId != sessionId && _sessions[i].messages.isEmpty()) {
			_sessions.removeAt(i);
		}
	}

	const int nextIdx = indexOfSession(sessionId);
	if (nextIdx < 0) {
		return false;
	}
	_activeSessionId = sessionId;
	_messages = _sessions[nextIdx].messages;
	trimStoredMessagesToLimit();
	resetStreamingBuffers();

	if (_sessions[nextIdx].summary.chatMode != _chatMode) {
		_chatMode = _sessions[nextIdx].summary.chatMode;
		emit chatModeChanged(_chatMode);
	}

	flushPersistSessions();
	emit historyCleared();
	emit sessionsChanged();
	return true;
}

bool AgentCoordinator::deleteSession(const QString &sessionId) {
	const int idx = indexOfSession(sessionId);
	if (idx < 0) {
		return false;
	}
	const bool deletingActive = (sessionId == _activeSessionId);
	if (deletingActive) {
		stopActiveRequest();
		expireAllPendingActions(QCoreApplication::translate("MeetingUI", "Conversation deleted."));
		_activeToolRound.reset();
		_inFlightActionByGroup.clear();
		_pendingActions.clear();
	}

	_sessions.removeAt(idx);

	if (_sessions.isEmpty()) {
		AgentChatSessionData fresh;
		fresh.summary.sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
		fresh.summary.title = QCoreApplication::translate("MeetingUI", "New Chat");
		fresh.summary.createdAt = QDateTime::currentDateTime();
		fresh.summary.updatedAt = fresh.summary.createdAt;
		fresh.summary.chatMode = _chatMode;
		_sessions.push_back(std::move(fresh));
	}

	if (deletingActive) {
		std::stable_sort(_sessions.begin(), _sessions.end(), [](const AgentChatSessionData &a, const AgentChatSessionData &b) {
			return a.summary.updatedAt > b.summary.updatedAt;
		});
		_activeSessionId = _sessions.front().summary.sessionId;
		_messages = _sessions.front().messages;
		trimStoredMessagesToLimit();
		_pendingDeltaBuffer.clear();
		_pendingReasoningBuffer.clear();
		_thinkingStartMs = 0;
		_pendingToolCalls.clear();
		if (_sessions.front().summary.chatMode != _chatMode) {
			_chatMode = _sessions.front().summary.chatMode;
			emit chatModeChanged(_chatMode);
		}
		emit historyCleared();
	}

	flushPersistSessions();
	emit sessionsChanged();
	return true;
}

bool AgentCoordinator::renameSession(const QString &sessionId, const QString &newTitle) {
	const int idx = indexOfSession(sessionId);
	const QString clean = newTitle.trimmed();
	if (idx < 0 || clean.isEmpty()) {
		return false;
	}
	_sessions[idx].summary.title = (clean.size() > 60) ? clean.left(60) : clean;
	flushPersistSessions();
	emit sessionsChanged();
	return true;
}

bool AgentCoordinator::clearAllSavedSessions() {
	stopActiveRequest();
	expireAllPendingActions(QCoreApplication::translate("MeetingUI", "Conversation cleared."));
	_activeToolRound.reset();
	_inFlightActionByGroup.clear();
	_pendingActions.clear();
	_messages.clear();
	resetStreamingBuffers();
	_sessions.clear();
	_sessionStore.clearAccountSessions(_currentAccountKey);

	AgentChatSessionData fresh;
	fresh.summary.sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	fresh.summary.title = QCoreApplication::translate("MeetingUI", "New Chat");
	fresh.summary.createdAt = QDateTime::currentDateTime();
	fresh.summary.updatedAt = fresh.summary.createdAt;
	fresh.summary.chatMode = _chatMode;
	_sessions.push_back(std::move(fresh));
	_activeSessionId = fresh.summary.sessionId;

	emit historyCleared();
	emit sessionsChanged();
	return true;
}

QString AgentCoordinator::exportSessionMarkdown(const QString &sessionId) const {
	syncActiveSessionSnapshot(false);
	const QString targetId = sessionId.isEmpty() ? _activeSessionId : sessionId;
	const int idx = indexOfSession(targetId);
	if (idx < 0) {
		return {};
	}
	return AgentSessionStore::exportSessionMarkdown(_sessions[idx]);
}

void AgentCoordinator::setSpeechTransport(std::unique_ptr<IAgentSpeechTransport> transport) {
	cancelSpeechProbe();
	_speechTransport = std::move(transport);
}

void AgentCoordinator::setSpeechStreamFactory(StreamingAsrFactory factory) {
	cancelSpeechProbe();
	_speechStreamFactory = std::move(factory);
}

void AgentCoordinator::cancelProbe() {
	if (!_probeHandle) {
		return;
	}
	_probeHandle->disconnect(this);
	_probeHandle->cancel();
	_probeHandle->deleteLater();
	_probeHandle.clear();
}

void AgentCoordinator::testConnection(const AgentProfile &draftProfile, const QString &draftApiKey) {
	cancelProbe();

	QString effectiveKey = draftApiKey.trimmed();
	if (effectiveKey.isEmpty()) {
		const auto loaded = _secretStore.loadApiKey(
			draftProfile.id.trimmed().isEmpty() ? _profile.id : draftProfile.id.trimmed(),
			draftProfile.keyStorage);
		if (loaded.status == AgentSecretStore::Status::Ok) {
			effectiveKey = loaded.apiKey;
		}
	}

	const auto generation = _sessionManager.authGeneration();
	_probeHandle = _transport->probe(draftProfile, effectiveKey, this);
	connect(_probeHandle, &AgentRequestHandle::finished, this,
		[this, generation](bool success, const QString &safeError) {
			const auto handle = _probeHandle;
			_probeHandle.clear();
			if (handle) {
				handle->deleteLater();
			}
			if (generation != _sessionManager.authGeneration()) {
				return;
			}
			emit probeFinished(success,
				success ? QStringLiteral("Connection verified.") : safeError);
		});
}

void AgentCoordinator::cancelSpeechProbe() {
	++_speechProbeNonce;
	_speechProbePending = false;
	_speechProbeTimer.stop();
	if (_speechProbeStreamSession) {
		_speechProbeStreamSession->abort();
		_speechProbeStreamSession.reset();
	}
	if (!_speechProbeHandle) {
		return;
	}
	_speechProbeHandle->disconnect(this);
	_speechProbeHandle->cancel();
	_speechProbeHandle->deleteLater();
	_speechProbeHandle.clear();
}

void AgentCoordinator::testSpeechConnection(const AgentSpeechProbeConfig &config) {
	cancelSpeechProbe();

	const auto generation = _sessionManager.authGeneration();
	const uint64_t nonce = _speechProbeNonce;
	_speechProbePending = true;

	const auto emitDeferredError = [this, generation, nonce](const QString &message) {
		QTimer::singleShot(0, this, [this, generation, nonce, message] {
			if (nonce != _speechProbeNonce) {
				return;
			}
			_speechProbePending = false;
			if (generation != _sessionManager.authGeneration()) {
				return;
			}
			emit speechProbeFinished(false, message);
		});
	};

	const AgentSpeechSourceMode effectiveSourceMode = config.sourceModeOverride.value_or(_speechSourceMode);
	const QString cleanSttUrl = (effectiveSourceMode == AgentSpeechSourceMode::FollowChat)
		? QString()
		: config.sttBaseUrl.trimmed();
	const QString cleanChatUrl = config.chatProfile.baseUrl.trimmed().isEmpty()
		? _profile.baseUrl.trimmed()
		: config.chatProfile.baseUrl.trimmed();
	const QString effectiveModel = config.sttModel.trimmed().isEmpty()
		? resolveDefaultSpeechModelForEndpoint(
			cleanSttUrl,
			cleanChatUrl,
			_cachedSpeechModels,
			_modelCapabilities,
			cachedExternalSpeechModelsForUrl(cleanSttUrl),
			cachedExternalSpeechCapabilitiesForUrl(cleanSttUrl),
			config.protocolMode)
		: config.sttModel.trimmed();
	const AgentSpeechProtocolMode effectiveProto = resolveEffectiveSpeechProtocol(
		config.protocolMode, cleanSttUrl, cleanChatUrl, effectiveModel);
	const bool streamingProto = isStreamingSpeechProtocol(effectiveProto);

	QString effectiveBase = cleanSttUrl.isEmpty() ? cleanChatUrl : cleanSttUrl;
	if (cleanSttUrl.isEmpty() && streamingProto) {
		if (effectiveProto == AgentSpeechProtocolMode::DashScopeInferenceWs ||
			effectiveProto == AgentSpeechProtocolMode::DashScopeQwenRealtimeWs) {
			effectiveBase = isStandardDashScopeHost(QUrl(cleanChatUrl))
				? cleanChatUrl
				: defaultStreamingSpeechEndpoint(effectiveProto);
		} else if (effectiveProto == AgentSpeechProtocolMode::XaiSttWs) {
			effectiveBase = isXaiAgentHost(QUrl(cleanChatUrl))
				? cleanChatUrl
				: defaultStreamingSpeechEndpoint(effectiveProto);
		} else if (effectiveProto == AgentSpeechProtocolMode::OpenAiRealtimeWs) {
			effectiveBase = cleanChatUrl.isEmpty()
				? defaultStreamingSpeechEndpoint(effectiveProto)
				: cleanChatUrl;
		} else {
			effectiveBase = defaultStreamingSpeechEndpoint(effectiveProto);
		}
	}

	const AgentEndpointDecision decision = streamingProto
		? evaluateAgentSpeechEndpoint(effectiveBase)
		: evaluateAgentEndpoint(effectiveBase);
	if (!decision.allowed) {
		emitDeferredError(decision.reason);
		return;
	}

	const bool hasCustomSttUrl = !cleanSttUrl.isEmpty();
	const bool sameOrigin = isSameSpeechOrigin(effectiveBase, cleanChatUrl);
	const bool loopbackStt = isAgentLoopbackHost(decision.normalizedUrl.host());
	const QString chatProfileId = config.chatProfile.id.trimmed().isEmpty()
		? _profile.id
		: config.chatProfile.id.trimmed();
	const QString sttSecretId = config.speechProfileId.trimmed().isEmpty()
		? dictationSecretId()
		: config.speechProfileId.trimmed();
	const QString appSecretId = sttSecretId + QStringLiteral("_app");
	const KeyStoragePolicy keyStorage = config.chatProfile.keyStorage;

	QString effectiveKey = (effectiveSourceMode == AgentSpeechSourceMode::CustomProfile)
		? config.draftSttApiKey.trimmed()
		: QString();
	if (effectiveSourceMode == AgentSpeechSourceMode::CustomProfile && effectiveKey.isEmpty()) {
		const auto sttLoaded = _secretStore.loadApiKey(sttSecretId, KeyStoragePolicy::EncryptedStore);
		if (sttLoaded.status == AgentSecretStore::Status::Corrupted) {
			emitDeferredError(QCoreApplication::translate(
				"MeetingUI",
				"Saved STT API key could not be decrypted. Please re-enter it in Settings > Copilot."));
			return;
		}
		if (sttLoaded.status == AgentSecretStore::Status::StorageUnavailable) {
			emitDeferredError(QCoreApplication::translate(
				"MeetingUI",
				"Saved STT API key could not be accessed (OS encrypted storage is locked or unavailable). Please unlock your keyring or re-enter it in Settings > Copilot."));
			return;
		}
		if (sttLoaded.status == AgentSecretStore::Status::Ok) {
			effectiveKey = sttLoaded.apiKey.trimmed();
		}
	}
	if (effectiveKey.isEmpty()) {
		if (!sameOrigin) {
			if (!loopbackStt) {
				emitDeferredError(QCoreApplication::translate(
					"MeetingUI",
					"A separate STT API key is required for the custom Speech-to-Text endpoint. Please enter it in Settings > Copilot."));
				return;
			}
		} else {
			if (!config.draftChatApiKey.trimmed().isEmpty()) {
				effectiveKey = config.draftChatApiKey.trimmed();
			} else {
				const auto chatLoaded = _secretStore.loadApiKey(chatProfileId, keyStorage);
				if (chatLoaded.status == AgentSecretStore::Status::Corrupted) {
					emitDeferredError(QCoreApplication::translate(
						"MeetingUI",
						"Saved API key could not be decrypted. Please re-enter it in Settings > Copilot."));
					return;
				}
				if (chatLoaded.status == AgentSecretStore::Status::StorageUnavailable) {
					emitDeferredError(QCoreApplication::translate(
						"MeetingUI",
						"Saved API key could not be accessed (OS encrypted storage is locked or unavailable). Please unlock your keyring or re-enter it in Settings > Copilot."));
					return;
				}
				if (chatLoaded.status == AgentSecretStore::Status::Ok) {
					effectiveKey = chatLoaded.apiKey.trimmed();
				}
			}
		}
	}

	if (streamingProto) {
		QString effectiveAppKey = (effectiveSourceMode == AgentSpeechSourceMode::CustomProfile)
			? config.draftAppKey.trimmed()
			: QString();
		if (effectiveSourceMode == AgentSpeechSourceMode::CustomProfile &&
			effectiveProto == AgentSpeechProtocolMode::VolcengineBigModelWs &&
			effectiveAppKey.isEmpty()) {
			const auto appLoaded = _secretStore.loadApiKey(appSecretId, KeyStoragePolicy::EncryptedStore);
			if (appLoaded.status == AgentSecretStore::Status::Corrupted) {
				emitDeferredError(QCoreApplication::translate(
					"MeetingUI",
					"Saved Volcengine App Key could not be decrypted. Please re-enter it in Settings > Copilot."));
				return;
			}
			if (appLoaded.status == AgentSecretStore::Status::StorageUnavailable) {
				emitDeferredError(QCoreApplication::translate(
					"MeetingUI",
					"Saved Volcengine App Key could not be accessed (OS encrypted storage is locked or unavailable)."));
				return;
			}
			if (appLoaded.status == AgentSecretStore::Status::Ok) {
				effectiveAppKey = appLoaded.apiKey.trimmed();
			}
		}
		if (!loopbackStt && effectiveProto == AgentSpeechProtocolMode::VolcengineBigModelWs &&
			effectiveKey.isEmpty()) {
			emitDeferredError(QCoreApplication::translate(
				"MeetingUI",
				"Volcengine streaming ASR requires an API Key (or legacy App Key + Access Key)."));
			return;
		}
		if (!loopbackStt && effectiveKey.isEmpty()) {
			emitDeferredError(QCoreApplication::translate(
				"MeetingUI",
				"An STT API key is required for WebSocket streaming ASR."));
			return;
		}

		QString fallbackResourceId = QLatin1String(kDefaultSpeechResourceId);
		QString fallbackWorkspaceId;
		if (effectiveSourceMode == AgentSpeechSourceMode::CustomProfile) {
			if (const auto targetSp = findSpeechProfile(sttSecretId)) {
				fallbackResourceId = targetSp->resourceId;
				fallbackWorkspaceId = targetSp->workspaceId;
			} else {
				fallbackResourceId = dictationSttResourceId();
				fallbackWorkspaceId = dictationSttWorkspace();
			}
		}

		StreamingAsrConfig streamCfg;
		streamCfg.protocol = effectiveProto;
		streamCfg.endpointUrl = effectiveBase;
		streamCfg.apiKey = effectiveKey;
		streamCfg.appKey = effectiveAppKey;
		streamCfg.resourceId = config.resourceId.trimmed().isEmpty()
			? fallbackResourceId
			: config.resourceId.trimmed();
		streamCfg.workspaceId = config.workspaceId.trimmed().isEmpty()
			? fallbackWorkspaceId
			: config.workspaceId.trimmed();
		streamCfg.model = effectiveModel;
		streamCfg.language = config.language.trimmed();
		streamCfg.prompt = config.prompt.trimmed().isEmpty()
			? _dictationPrompt
			: config.prompt.trimmed();
		if (sameOrigin) {
			streamCfg.customHeaders = config.chatProfile.customHeaders;
		}
		streamCfg.connectTimeoutMs = config.chatProfile.connectTimeoutMs;
		streamCfg.enableServerVad = true;

		std::unique_ptr<IStreamingAsrSession> session = _speechStreamFactory
			? _speechStreamFactory(streamCfg)
			: createStreamingAsrSession(streamCfg);
		if (!session) {
			emitDeferredError(QCoreApplication::translate(
				"MeetingUI", "Failed to create streaming speech recognition session."));
			return;
		}

		struct WsProbeTracker {
			bool sawReady = false;
			bool completed = false;
			QString recognizedText;
		};
		auto tracker = std::make_shared<WsProbeTracker>();
		QPointer<AgentCoordinator> selfPtr(this);

		StreamingAsrCallbacks cbs;
		cbs.onStateChanged = [selfPtr, generation, nonce, tracker](StreamingAsrState st) {
			if (st == StreamingAsrState::Ready) {
				QMetaObject::invokeMethod(selfPtr, [selfPtr, generation, nonce, tracker] {
					if (!selfPtr || nonce != selfPtr->_speechProbeNonce ||
						generation != selfPtr->_sessionManager.authGeneration()) {
						return;
					}
					tracker->sawReady = true;
				}, Qt::QueuedConnection);
			}
		};
		cbs.onResult = [selfPtr, generation, nonce, tracker](const StreamingAsrResult &res) {
			QMetaObject::invokeMethod(selfPtr, [selfPtr, generation, nonce, tracker, res] {
				if (!selfPtr || nonce != selfPtr->_speechProbeNonce ||
					generation != selfPtr->_sessionManager.authGeneration()) {
					return;
				}
				tracker->sawReady = true;
				const QString clean = res.text.trimmed();
				if (!clean.isEmpty()) {
					tracker->recognizedText = clean;
				}
			}, Qt::QueuedConnection);
		};
		cbs.onError = [selfPtr, generation, nonce, tracker](const QString &safeError) {
			QMetaObject::invokeMethod(selfPtr, [selfPtr, generation, nonce, tracker, safeError] {
				if (!selfPtr || tracker->completed || nonce != selfPtr->_speechProbeNonce) {
					return;
				}
				tracker->completed = true;
				selfPtr->_speechProbePending = false;
				selfPtr->_speechProbeTimer.stop();
				selfPtr->_speechProbeStreamSession.reset();
				if (generation != selfPtr->_sessionManager.authGeneration()) {
					return;
				}
				emit selfPtr->speechProbeFinished(false, safeError);
			}, Qt::QueuedConnection);
		};
		cbs.onFinished = [selfPtr, generation, nonce, tracker, effectiveModel]() {
			QMetaObject::invokeMethod(selfPtr, [selfPtr, generation, nonce, tracker, effectiveModel] {
				if (!selfPtr || tracker->completed || nonce != selfPtr->_speechProbeNonce) {
					return;
				}
				tracker->completed = true;
				selfPtr->_speechProbePending = false;
				selfPtr->_speechProbeTimer.stop();
				selfPtr->_speechProbeStreamSession.reset();
				if (generation != selfPtr->_sessionManager.authGeneration()) {
					return;
				}
				if (!tracker->sawReady) {
					emit selfPtr->speechProbeFinished(
						false,
						QCoreApplication::translate(
							"MeetingUI", "WebSocket closed before speech session became ready."));
					return;
				}
				const QString okMsg = tracker->recognizedText.isEmpty()
					? QCoreApplication::translate("MeetingUI", "WebSocket connection verified (%1).").arg(effectiveModel)
					: QCoreApplication::translate("MeetingUI", "WebSocket verified (%1: %2).")
						.arg(effectiveModel, tracker->recognizedText.left(32));
				emit selfPtr->speechProbeFinished(true, okMsg);
			}, Qt::QueuedConnection);
		};

		_speechProbeStreamSession = std::move(session);
		_speechProbeTimer.start(std::clamp(config.chatProfile.connectTimeoutMs + 4000, 5000, 64000));
		_speechProbeStreamSession->start(std::move(cbs));

		const std::vector<int16_t> probePcm = detail::decodeSpeechProbePcm16k();
		for (size_t off = 0; off < probePcm.size(); off += 1600) {
			const size_t n = std::min<size_t>(1600, probePcm.size() - off);
			_speechProbeStreamSession->pushPcm16(probePcm.data() + off, n, 16000);
		}
		_speechProbeStreamSession->finish();
		return;
	}

	AgentModelCapabilities effectiveCaps;
	if (hasCustomSttUrl && !sameOrigin) {
		effectiveCaps = cachedExternalSpeechCapabilitiesForUrl(cleanSttUrl).value(effectiveModel);
	} else {
		effectiveCaps = _modelCapabilities.value(effectiveModel);
	}

	SpeechTranscriptionRequest req;
	req.profile = config.chatProfile;
	if (req.profile.baseUrl.trimmed().isEmpty()) {
		req.profile.baseUrl = cleanChatUrl;
	}
	req.apiKey = effectiveKey;
	req.customSttBaseUrl = cleanSttUrl;
	req.protocolMode = config.protocolMode;
	req.sttModel = effectiveModel;
	req.sttModelCapabilities = effectiveCaps;
	req.language = config.language.trimmed();
	req.prompt = config.prompt.trimmed().isEmpty()
		? _dictationPrompt
		: config.prompt.trimmed();
	req.wavAudioBytes = buildSpeechProbeWav();

	if (!_speechTransport) {
		_speechTransport = std::make_unique<OpenAiCompatibleSpeechTransport>();
	}
	QPointer<AgentSpeechRequestHandle> boundHandle = _speechTransport->transcribeWav(req, this);
	if (!boundHandle) {
		emitDeferredError(QCoreApplication::translate(
			"MeetingUI",
			"Failed to start speech connection test."));
		return;
	}
	_speechProbeHandle = boundHandle;
	connect(boundHandle, &AgentSpeechRequestHandle::finished, this,
		[this, generation, nonce, effectiveModel, boundHandle](bool success, const QString &, const QString &safeError) {
			if (boundHandle) {
				boundHandle->deleteLater();
			}
			if (_speechProbeHandle == boundHandle) {
				_speechProbeHandle.clear();
			}
			if (nonce != _speechProbeNonce) {
				return;
			}
			_speechProbePending = false;
			if (generation != _sessionManager.authGeneration()) {
				return;
			}
			emit speechProbeFinished(
				success,
				success
					? QCoreApplication::translate("MeetingUI", "Connection verified (%1).").arg(effectiveModel)
					: safeError);
		});
}

void AgentCoordinator::fetchModels(const AgentProfile &draftProfile, const QString &draftApiKey) {
	if (_modelsHandle) {
		_modelsHandle->disconnect(this);
		_modelsHandle->cancel();
		_modelsHandle->deleteLater();
		_modelsHandle.clear();
	}

	QString effectiveKey = draftApiKey.trimmed();
	if (effectiveKey.isEmpty()) {
		const auto loaded = _secretStore.loadApiKey(
			draftProfile.id.trimmed().isEmpty() ? _profile.id : draftProfile.id.trimmed(),
			draftProfile.keyStorage);
		if (loaded.status == AgentSecretStore::Status::Ok) {
			effectiveKey = loaded.apiKey;
		}
	}

	const auto generation = _sessionManager.authGeneration();
	_modelsHandle = _transport->listModels(draftProfile, effectiveKey, this);
	connect(_modelsHandle, &AgentModelListHandle::finished, this,
		[this, generation](bool success, const QStringList &models, const QString &safeError) {
			const auto handle = _modelsHandle;
			const auto fetchedCaps = handle ? handle->modelCapabilities() : QHash<QString, AgentModelCapabilities>{};
			const auto fetchedSpeech = handle ? handle->speechModels() : QStringList{};
			const int filteredCount = handle ? handle->filteredNonChatCount() : 0;
			_modelsHandle.clear();
			if (handle) {
				handle->deleteLater();
			}
			if (generation != _sessionManager.authGeneration()) {
				return;
			}
			if (success) {
				_cachedModels = models;
				_cachedSpeechModels = fetchedSpeech;
				_modelCapabilities = fetchedCaps;
				_lastFilteredNonChatModelCount = filteredCount;
			}
			emit modelsFetched(success, models, safeError);
		});
}

void AgentCoordinator::fetchSpeechModels(
		const QString &sttBaseUrl,
		const QString &draftSttApiKey,
		const QString &chatBaseUrl) {
	if (_sttModelsHandle) {
		_sttModelsHandle->disconnect(this);
		_sttModelsHandle->cancel();
		_sttModelsHandle->deleteLater();
		_sttModelsHandle.clear();
	}

	const QString cleanStt = sttBaseUrl.trimmed();
	if (cleanStt.isEmpty()) {
		_cachedExternalSpeechKey.clear();
		_cachedExternalSpeechModels.clear();
		_externalSttModelCapabilities.clear();
		return;
	}

	const AgentEndpointDecision sttDecision = evaluateAgentEndpoint(cleanStt);
	const auto generation = _sessionManager.authGeneration();
	if (!sttDecision.allowed) {
		_cachedExternalSpeechKey.clear();
		_cachedExternalSpeechModels.clear();
		_externalSttModelCapabilities.clear();
		const QString reason = sttDecision.reason;
		QTimer::singleShot(0, this, [this, generation, reason] {
			if (generation != _sessionManager.authGeneration()) {
				return;
			}
			emit speechModelsFetched(false, {}, reason);
		});
		return;
	}
	if (isElevenLabsAgentHost(sttDecision.normalizedUrl)) {
		_cachedExternalSpeechKey.clear();
		_cachedExternalSpeechModels.clear();
		_externalSttModelCapabilities.clear();
		QTimer::singleShot(0, this, [this, generation] {
			if (generation != _sessionManager.authGeneration()) {
				return;
			}
			emit speechModelsFetched(true, {}, {});
		});
		return;
	}

	const QString effectiveChatUrl = chatBaseUrl.trimmed().isEmpty() ? _profile.baseUrl : chatBaseUrl.trimmed();
	const bool sameOrigin = isSameAgentOrigin(cleanStt, effectiveChatUrl);
	const bool loopbackStt = isAgentLoopbackHost(sttDecision.normalizedUrl.host());

	QString effectiveKey = draftSttApiKey.trimmed();
	if (effectiveKey.isEmpty() && _speechSourceMode == AgentSpeechSourceMode::CustomProfile) {
		const auto sttLoaded = _secretStore.loadApiKey(dictationSecretId(), KeyStoragePolicy::EncryptedStore);
		if (sttLoaded.status == AgentSecretStore::Status::Corrupted) {
			const QString err = QCoreApplication::translate(
				"MeetingUI",
				"Saved STT API key could not be decrypted. Please re-enter it in Settings > Copilot.");
			QTimer::singleShot(0, this, [this, generation, err] {
				if (generation != _sessionManager.authGeneration()) {
					return;
				}
				emit speechModelsFetched(false, {}, err);
			});
			return;
		}
		if (sttLoaded.status == AgentSecretStore::Status::StorageUnavailable) {
			const QString err = QCoreApplication::translate(
				"MeetingUI",
				"Saved STT API key could not be accessed (OS encrypted storage is locked or unavailable). Please unlock your keyring or re-enter it in Settings > Copilot.");
			QTimer::singleShot(0, this, [this, generation, err] {
				if (generation != _sessionManager.authGeneration()) {
					return;
				}
				emit speechModelsFetched(false, {}, err);
			});
			return;
		}
		if (sttLoaded.status == AgentSecretStore::Status::Ok) {
			effectiveKey = sttLoaded.apiKey.trimmed();
		}
	}
	if (effectiveKey.isEmpty()) {
		if (!sameOrigin && !loopbackStt) {
			_cachedExternalSpeechKey.clear();
			_cachedExternalSpeechModels.clear();
			_externalSttModelCapabilities.clear();
			const QString err = QCoreApplication::translate(
				"MeetingUI",
				"A separate STT API key is required for the custom Speech-to-Text endpoint. Please enter it in Settings > Copilot.");
			QTimer::singleShot(0, this, [this, generation, err] {
				if (generation != _sessionManager.authGeneration()) {
					return;
				}
				emit speechModelsFetched(false, {}, err);
			});
			return;
		}
		if (sameOrigin) {
			const auto chatLoaded = _secretStore.loadApiKey(_profile.id, _profile.keyStorage);
			if (chatLoaded.status == AgentSecretStore::Status::Ok) {
				effectiveKey = chatLoaded.apiKey.trimmed();
			}
		}
	}

	const QString endpointKey = canonicalSpeechEndpointKey(cleanStt);
	AgentProfile speechProfile;
	speechProfile.id = dictationSecretId();
	speechProfile.protocol = AgentProtocolDialect::OpenAiChatCompletionsSse;
	speechProfile.baseUrl = sttDecision.normalizedUrl.toString(QUrl::FullyEncoded);
	speechProfile.keyStorage = _profile.keyStorage;
	speechProfile.connectTimeoutMs = _profile.connectTimeoutMs;
	speechProfile.idleTimeoutMs = _profile.idleTimeoutMs;

	_sttModelsHandle = _transport->listModels(speechProfile, effectiveKey, this);
	connect(_sttModelsHandle, &AgentModelListHandle::finished, this,
		[this, generation, endpointKey](bool success, const QStringList &models, const QString &safeError) {
			Q_UNUSED(models);
			const auto handle = _sttModelsHandle;
			const auto fetchedCaps = handle ? handle->modelCapabilities() : QHash<QString, AgentModelCapabilities>{};
			const auto fetchedSpeech = handle ? handle->speechModels() : QStringList{};
			_sttModelsHandle.clear();
			if (handle) {
				handle->deleteLater();
			}
			if (generation != _sessionManager.authGeneration()) {
				return;
			}
			if (success) {
				_cachedExternalSpeechKey = endpointKey;
				_cachedExternalSpeechModels = fetchedSpeech;
				_externalSttModelCapabilities = fetchedCaps;
			} else {
				_cachedExternalSpeechKey.clear();
				_cachedExternalSpeechModels.clear();
				_externalSttModelCapabilities.clear();
			}
			emit speechModelsFetched(success, fetchedSpeech, safeError);
		});
}

AgentModelCapabilities AgentCoordinator::resolveModelCapabilities(const QString &modelId) const {
	const QString clean = modelId.trimmed();
	if (clean.isEmpty()) {
		return {};
	}
	const auto it = _modelCapabilities.find(clean);
	if (it != _modelCapabilities.end()) {
		return it.value();
	}
	return inferModelCapabilitiesById(clean);
}

AgentModelCapabilities AgentCoordinator::resolveSpeechModelCapabilities(const QString &modelId) const {
	const QString clean = modelId.trimmed();
	if (clean.isEmpty()) {
		return {};
	}
	const QString cleanStt = dictationSttBaseUrl().trimmed();
	if (!cleanStt.isEmpty() && !isSameAgentOrigin(cleanStt, _profile.baseUrl)) {
		const auto externalCaps = cachedExternalSpeechCapabilitiesForUrl(cleanStt);
		const auto it = externalCaps.find(clean);
		if (it != externalCaps.end()) {
			return it.value();
		}
		return {};
	}
	const auto it = _modelCapabilities.find(clean);
	if (it != _modelCapabilities.end()) {
		return it.value();
	}
	return {};
}

} // namespace OpenMeeting
