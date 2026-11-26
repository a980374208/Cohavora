#include "src/agent/openai_compatible_transport.h"

#include "src/agent/agent_ui_grounding.h"
#include "src/telemetry/log_redaction.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QTimeZone>
#include <QtCore/QTimer>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QSslConfiguration>
#include <QtNetwork/QSslSocket>

#include <algorithm>
#include <array>

namespace OpenMeeting {
namespace {

constexpr qsizetype kMaxNonStreamingBodyBytes = 4 * 1024 * 1024;

QString appendHttpTroubleshootingHint(
		const QString &baseMessage,
		int httpStatus,
		const QString &safeEndpointPath = {},
		AgentKeyEndpointMismatch keyMismatch = AgentKeyEndpointMismatch::None) {
	if (httpStatus == 401) {
		QString hint = QStringLiteral("%1\n%2").arg(
			baseMessage,
			QCoreApplication::translate(
				"MeetingUI",
				"Hint: Authentication failed. Please check that the API key is filled in, valid, and matches the current Base URL / platform account."));
		if (keyMismatch == AgentKeyEndpointMismatch::BailianPlanKeyOnStandardHost) {
			hint += QLatin1Char('\n') + QCoreApplication::translate(
				"MeetingUI",
				"Detail: Detected a Bailian Token/Coding Plan key (sk-sp-...) against a standard DashScope endpoint. Please switch Base URL to your plan endpoint (e.g. coding.dashscope.aliyuncs.com or token-plan.*.maas.aliyuncs.com) or use a standard DashScope API key (sk-...).");
		} else if (keyMismatch == AgentKeyEndpointMismatch::StandardKeyOnBailianPlanHost) {
			hint += QLatin1Char('\n') + QCoreApplication::translate(
				"MeetingUI",
				"Detail: Detected a standard sk-... key against a Bailian Token/Coding Plan host. Please use the Token/Coding Plan sk-sp-... key for this endpoint, or switch Base URL to the standard DashScope endpoint.");
		}
		return hint;
	}
	if (httpStatus == 403) {
		QString hint = QStringLiteral("%1\n%2").arg(
			baseMessage,
			QCoreApplication::translate(
				"MeetingUI",
				"Hint: Access to this model or endpoint was denied. Please check that the model is activated on the platform, permitted for this API key, and that the Base URL matches your plan."));
		if (keyMismatch == AgentKeyEndpointMismatch::BailianPlanKeyOnStandardHost) {
			hint += QLatin1Char('\n') + QCoreApplication::translate(
				"MeetingUI",
				"Detail: Detected a Bailian Token/Coding Plan key (sk-sp-...) against a standard DashScope endpoint. Please switch Base URL to your plan endpoint (e.g. coding.dashscope.aliyuncs.com or token-plan.*.maas.aliyuncs.com) or use a standard DashScope API key (sk-...).");
		} else if (keyMismatch == AgentKeyEndpointMismatch::StandardKeyOnBailianPlanHost) {
			hint += QLatin1Char('\n') + QCoreApplication::translate(
				"MeetingUI",
				"Detail: Detected a standard sk-... key against a Bailian Token/Coding Plan host. Please use the Token/Coding Plan sk-sp-... key for this endpoint, or switch Base URL to the standard DashScope endpoint.");
		}
		return hint;
	}
	if (httpStatus == 404) {
		const QString hint = QCoreApplication::translate(
			"MeetingUI",
			"Hint: Endpoint not found. Please check that the Base URL, Protocol Dialect, and Model Name match what this provider supports.");
		if (!safeEndpointPath.isEmpty()) {
			return QStringLiteral("%1\n%2\n%3").arg(
				baseMessage,
				hint,
				QCoreApplication::translate("MeetingUI", "Request path: %1").arg(safeEndpointPath));
		}
		return QStringLiteral("%1\n%2").arg(baseMessage, hint);
	}
	if (httpStatus == 400 || httpStatus == 415 || httpStatus == 422) {
		const QString lower = baseMessage.toLower();
		if (lower.contains(QStringLiteral("content.type")) ||
			lower.contains(QStringLiteral("image_url")) ||
			lower.contains(QStringLiteral("input_image")) ||
			lower.contains(QStringLiteral("['text']")) ||
			lower.contains(QStringLiteral("multimodal")) ||
			(lower.contains(QStringLiteral("image")) && lower.contains(QStringLiteral("not support")))) {
			return QStringLiteral("%1\n%2").arg(
				baseMessage,
				QCoreApplication::translate(
					"MeetingUI",
					"Hint: The selected model only accepts text and does not support image input. Please switch to a vision-capable model (e.g. qwen3.8-omni-flash, kimi-k3, *-vl, gpt-4o, gemini)."));
		}
	}
	return baseMessage;
}

QString sanitizeAgentErrorMessage(
		const QString &raw,
		int httpStatus = 0,
		const QString &safeEndpointPath = {},
		AgentKeyEndpointMismatch keyMismatch = AgentKeyEndpointMismatch::None) {
	const QString redactedRaw = redactEmbeddedApiKeys(raw.trimmed().left(512));
	const QByteArray utf8 = redactedRaw.toUtf8();
	const auto sanitized = livekit::secure_log::SanitizeForOutput(
		std::string_view(utf8.constData(), static_cast<size_t>(utf8.size())));
	QString clean = QString::fromUtf8(sanitized.data(), static_cast<int>(sanitized.size())).trimmed();
	QString base;
	if (clean.isEmpty()) {
		base = (httpStatus > 0)
			? QStringLiteral("Agent API request failed (HTTP %1).").arg(httpStatus)
			: QStringLiteral("Agent API request failed.");
	} else if (httpStatus > 0) {
		base = QStringLiteral("HTTP %1: %2").arg(httpStatus).arg(clean);
	} else {
		base = clean;
	}
	return appendHttpTroubleshootingHint(base, httpStatus, safeEndpointPath, keyMismatch);
}

QString extractErrorTextFromJson(const QByteArray &payload) {
	QJsonParseError err{};
	const auto doc = QJsonDocument::fromJson(payload, &err);
	if (err.error != QJsonParseError::NoError || !doc.isObject()) {
		return {};
	}
	const QJsonObject root = doc.object();
	if (root.contains(QStringLiteral("error"))) {
		const auto errorVal = root.value(QStringLiteral("error"));
		if (errorVal.isString()) {
			return errorVal.toString();
		}
		if (errorVal.isObject()) {
			const auto msg = errorVal.toObject().value(QStringLiteral("message")).toString();
			if (!msg.isEmpty()) {
				return msg;
			}
		}
	}
	const auto message = root.value(QStringLiteral("message")).toString();
	if (!message.isEmpty()) {
		return message;
	}
	return root.value(QStringLiteral("detail")).toString();
}

QString formatReplyErrorMessage(
		const QByteArray &body,
		const QNetworkReply *reply,
		int statusCode,
		AgentKeyEndpointMismatch keyMismatch = AgentKeyEndpointMismatch::None) {
	const QString jsonErr = extractErrorTextFromJson(body);
	const QString detail = !jsonErr.isEmpty()
		? jsonErr
		: (!body.trimmed().isEmpty()
			? QString::fromUtf8(body.left(256))
			: (reply ? reply->errorString() : QString()));
	QString safePath;
	if (statusCode == 404 && reply) {
		const QString rawPath = reply->url().path().trimmed().left(160);
		if (!rawPath.isEmpty()) {
			const QByteArray utf8 = rawPath.toUtf8();
			const auto sanitized = livekit::secure_log::SanitizeForOutput(
				std::string_view(utf8.constData(), static_cast<size_t>(utf8.size())));
			safePath = QString::fromUtf8(sanitized.data(), static_cast<int>(sanitized.size())).trimmed();
		}
	}
	return sanitizeAgentErrorMessage(detail, statusCode, safePath, keyMismatch);
}

QString normalizedUrlPath(const QUrl &url) {
	const QString path = url.path();
	return path.isEmpty() ? QStringLiteral("/") : path;
}

void appendPathSuffix(QString &path, const QString &suffix) {
	if (path.endsWith(suffix, Qt::CaseInsensitive)) {
		return;
	}
	if (path == QStringLiteral("/")) {
		path = suffix;
	} else if (path.endsWith(QLatin1Char('/'))) {
		path += suffix.mid(1);
	} else {
		path += suffix;
	}
}

void normalizeGroqOpenAiRootPath(const QUrl &url, QString &path) {
	if (!isGroqAgentHost(url.host())) {
		return;
	}
	if (path == QStringLiteral("/") ||
		path.compare(QStringLiteral("/openai"), Qt::CaseInsensitive) == 0 ||
		path.compare(QStringLiteral("/openai/"), Qt::CaseInsensitive) == 0) {
		path = QStringLiteral("/openai/v1");
	}
}

QUrl resolveOpenAiChatEndpointUrl(const QUrl &baseUrl, const QString &, bool) {
	QUrl url = baseUrl;
	QString path = normalizedUrlPath(url);
	normalizeGroqOpenAiRootPath(url, path);
	appendPathSuffix(path, QStringLiteral("/chat/completions"));
	url.setPath(path);
	return url;
}

QUrl resolveOpenAiResponsesEndpointUrl(const QUrl &baseUrl, const QString &, bool) {
	QUrl url = baseUrl;
	QString path = normalizedUrlPath(url);
	appendPathSuffix(path, QStringLiteral("/responses"));
	url.setPath(path);
	return url;
}

bool endsWithVersionSegment(const QString &path) {
	QString clean = path;
	while (clean.size() > 1 && clean.endsWith(QLatin1Char('/'))) {
		clean.chop(1);
	}
	const int slash = clean.lastIndexOf(QLatin1Char('/'));
	const QString last = (slash >= 0) ? clean.mid(slash + 1).toLower() : QString();
	if (last.size() < 2 || last.at(0) != QLatin1Char('v')) {
		return false;
	}
	int i = 1;
	while (i < last.size() && last.at(i).isDigit()) {
		++i;
	}
	if (i == 1) {
		return false;
	}
	const QString tail = last.mid(i);
	return tail.isEmpty() || tail == QStringLiteral("beta") || tail == QStringLiteral("alpha");
}

QUrl resolveAnthropicMessagesEndpointUrl(const QUrl &baseUrl, const QString &, bool) {
	QUrl url = baseUrl;
	QString path = normalizedUrlPath(url);
	if (!path.endsWith(QStringLiteral("/messages"), Qt::CaseInsensitive)) {
		if (endsWithVersionSegment(path)) {
			appendPathSuffix(path, QStringLiteral("/messages"));
		} else {
			appendPathSuffix(path, QStringLiteral("/v1/messages"));
		}
	}
	url.setPath(path);
	return url;
}

QUrl resolveGeminiEndpointUrl(const QUrl &baseUrl, const QString &model, bool stream) {
	QUrl url = baseUrl;
	QString path = normalizedUrlPath(url);
	QString cleanModel = model.trimmed();
	if (cleanModel.startsWith(QStringLiteral("models/"), Qt::CaseInsensitive)) {
		cleanModel = cleanModel.mid(7);
	}
	const int qIdx = cleanModel.indexOf(QLatin1Char('?'));
	if (qIdx >= 0) {
		cleanModel.truncate(qIdx);
	}
	const int hIdx = cleanModel.indexOf(QLatin1Char('#'));
	if (hIdx >= 0) {
		cleanModel.truncate(hIdx);
	}

	const QString action = stream
		? QStringLiteral(":streamGenerateContent")
		: QStringLiteral(":generateContent");
	if (path.endsWith(QStringLiteral(":streamGenerateContent"), Qt::CaseInsensitive)) {
		path.chop(22);
		path.append(action);
	} else if (path.endsWith(QStringLiteral(":generateContent"), Qt::CaseInsensitive)) {
		path.chop(16);
		path.append(action);
	} else {
		if (path == QStringLiteral("/")) {
			path = QStringLiteral("/v1beta");
		}
		if (path.endsWith(QStringLiteral("/models"), Qt::CaseInsensitive)) {
			path.append(QLatin1Char('/') + cleanModel + action);
		} else {
			appendPathSuffix(path, QStringLiteral("/models/") + cleanModel + action);
		}
	}
	url.setPath(path);
	url.setQuery(stream ? QStringLiteral("alt=sse") : QString());
	return url;
}

QUrl resolveWebhookEndpointUrl(const QUrl &baseUrl, const QString &, bool) {
	return baseUrl;
}

QUrl resolveCommonModelsUrl(const QUrl &baseUrl, bool defaultToV1Root) {
	QUrl url = baseUrl;
	QString path = normalizedUrlPath(url);

	for (const char *suffix : {
		"/chat/completions",
		"/responses",
		"/messages",
		"/audio/transcriptions",
		"/transcriptions",
	}) {
		const QString s = QLatin1String(suffix);
		if (path.endsWith(s, Qt::CaseInsensitive)) {
			path.chop(s.size());
			if (path.isEmpty()) {
				path = QStringLiteral("/");
			}
			break;
		}
	}

	normalizeGroqOpenAiRootPath(url, path);

	if (!path.endsWith(QStringLiteral("/models"), Qt::CaseInsensitive)) {
		if (defaultToV1Root && !endsWithVersionSegment(path)) {
			appendPathSuffix(path, QStringLiteral("/v1/models"));
		} else {
			appendPathSuffix(path, QStringLiteral("/models"));
		}
	}

	url.setPath(path);
	return url;
}

QUrl resolveOpenAiModelsUrl(const QUrl &baseUrl) {
	return resolveCommonModelsUrl(baseUrl, false);
}

QUrl resolveAnthropicModelsUrl(const QUrl &baseUrl) {
	return resolveCommonModelsUrl(baseUrl, true);
}

QUrl resolveGeminiModelsUrl(const QUrl &baseUrl) {
	QUrl url = baseUrl;
	QString path = normalizedUrlPath(url);
	for (const char *action : {":streamGenerateContent", ":generateContent"}) {
		const QString a = QLatin1String(action);
		if (path.endsWith(a, Qt::CaseInsensitive)) {
			const int lastSlash = path.lastIndexOf(QLatin1Char('/'));
			path = (lastSlash > 0) ? path.left(lastSlash) : QStringLiteral("/");
			break;
		}
	}

	if (!path.endsWith(QStringLiteral("/models"), Qt::CaseInsensitive)) {
		if (path == QStringLiteral("/")) {
			path = QStringLiteral("/v1beta/models");
		} else {
			appendPathSuffix(path, QStringLiteral("/models"));
		}
	}
	url.setPath(path);
	return url;
}

QVector<AgentReasoningEffort> parseEffortArray(const QJsonArray &arr) {
	QVector<AgentReasoningEffort> efforts;
	const auto pushUnique = [&efforts](AgentReasoningEffort e) {
		if (e != AgentReasoningEffort::Default && !efforts.contains(e)) {
			efforts.push_back(e);
		}
	};
	for (const auto &v : arr) {
		const QString s = v.toString().trimmed().toLower();
		if (s == QStringLiteral("low")) {
			pushUnique(AgentReasoningEffort::Low);
		} else if (s == QStringLiteral("medium")) {
			pushUnique(AgentReasoningEffort::Medium);
		} else if (s == QStringLiteral("high")) {
			pushUnique(AgentReasoningEffort::High);
		} else if (s == QStringLiteral("xhigh") || s == QStringLiteral("extra") || s == QStringLiteral("max")) {
			pushUnique(AgentReasoningEffort::Extra);
		}
	}
	return efforts;
}

AgentModelCapabilities extractCapabilitiesForModelObject(const QString &modelId, const QJsonObject &obj) {
	AgentModelCapabilities caps = inferModelCapabilitiesById(modelId);
	bool explicitServerDecision = false;
	bool serverSupports = false;
	QVector<AgentReasoningEffort> serverEfforts;
	bool explicitVisionDecision = false;
	bool serverVision = false;

	const auto containsVisionModality = [](const QJsonArray &arr) -> bool {
		for (const auto &el : arr) {
			const QString m = el.toString().trimmed().toLower();
			if (m == QStringLiteral("image") || m == QStringLiteral("vision")) {
				return true;
			}
		}
		return false;
	};

	// 1. Copilot-style capabilities: { capabilities: { supports: { thinking / reasoning / reasoning_effort / vision } } }
	const QJsonObject capObj = obj.value(QStringLiteral("capabilities")).toObject();
	if (!capObj.isEmpty()) {
		const QJsonObject supports = capObj.value(QStringLiteral("supports")).toObject();
		for (const char *key : {"thinking", "reasoning", "reasoning_effort"}) {
			const QLatin1String k(key);
			if (supports.contains(k)) {
				explicitServerDecision = true;
				if (supports.value(k).isBool()) {
					serverSupports = serverSupports || supports.value(k).toBool();
				} else if (supports.value(k).isObject()) {
					serverSupports = true;
				}
			}
		}
		if (supports.value(QStringLiteral("reasoning_efforts")).isArray()) {
			explicitServerDecision = true;
			serverEfforts = parseEffortArray(supports.value(QStringLiteral("reasoning_efforts")).toArray());
			if (!serverEfforts.isEmpty()) {
				serverSupports = true;
			}
		}
		for (const char *vKey : {"vision", "image_input", "multimodal"}) {
			const QLatin1String k(vKey);
			if (supports.contains(k)) {
				if (supports.value(k).isBool()) {
					explicitVisionDecision = true;
					serverVision = serverVision || supports.value(k).toBool();
				} else if (supports.value(k).isObject()) {
					explicitVisionDecision = true;
					serverVision = true;
				}
			}
		}
		const QLatin1String capVisionKey("vision");
		if (capObj.contains(capVisionKey)) {
			if (capObj.value(capVisionKey).isBool()) {
				explicitVisionDecision = true;
				serverVision = serverVision || capObj.value(capVisionKey).toBool();
			} else if (capObj.value(capVisionKey).isObject()) {
				explicitVisionDecision = true;
				serverVision = true;
			}
		}
	}

	// 2. OpenRouter / HuggingFace / vLLM architecture modalities
	const QJsonObject arch = obj.value(QStringLiteral("architecture")).toObject();
	if (!arch.isEmpty()) {
		if (arch.value(QStringLiteral("input_modalities")).isArray()) {
			explicitVisionDecision = true;
			if (containsVisionModality(arch.value(QStringLiteral("input_modalities")).toArray())) {
				serverVision = true;
			}
		} else if (arch.value(QStringLiteral("modality")).isString()) {
			explicitVisionDecision = true;
			const QString mod = arch.value(QStringLiteral("modality")).toString().trimmed().toLower();
			if (mod.contains(QStringLiteral("image")) ||
				mod.contains(QStringLiteral("vision")) ||
				mod.contains(QStringLiteral("multimodal"))) {
				serverVision = true;
			}
		}
	}

	for (const char *arrKey : {"input_modalities", "modalities"}) {
		const QLatin1String k(arrKey);
		if (obj.value(k).isArray()) {
			explicitVisionDecision = true;
			if (containsVisionModality(obj.value(k).toArray())) {
				serverVision = true;
			}
		}
	}

	// 2b. Audio recognition modalities declared by the server take precedence over
	// model-name heuristics when classifying speech-to-text candidates.
	const auto collectModalities = [](const QJsonArray &arr) {
		QStringList values;
		for (const auto &el : arr) {
			const QString m = el.toString().trimmed().toLower();
			if (!m.isEmpty() && !values.contains(m)) {
				values.push_back(m);
			}
		}
		return values;
	};
	QStringList serverInputModalities;
	QStringList serverOutputModalities;
	if (arch.value(QStringLiteral("input_modalities")).isArray()) {
		serverInputModalities = collectModalities(arch.value(QStringLiteral("input_modalities")).toArray());
	}
	if (arch.value(QStringLiteral("output_modalities")).isArray()) {
		serverOutputModalities = collectModalities(arch.value(QStringLiteral("output_modalities")).toArray());
	}
	for (const char *arrKey : {"input_modalities", "modalities"}) {
		const QLatin1String k(arrKey);
		if (serverInputModalities.isEmpty() && obj.value(k).isArray()) {
			serverInputModalities = collectModalities(obj.value(k).toArray());
		}
	}
	if (serverOutputModalities.isEmpty() && obj.value(QStringLiteral("output_modalities")).isArray()) {
		serverOutputModalities = collectModalities(obj.value(QStringLiteral("output_modalities")).toArray());
	}
	const QString subType = obj.value(QStringLiteral("sub_type")).toString().trimmed().toLower();
	if (serverInputModalities.isEmpty() && serverOutputModalities.isEmpty() &&
		(subType == QStringLiteral("speech-to-text") || subType == QStringLiteral("asr"))) {
		serverInputModalities = QStringList{ QStringLiteral("audio") };
		serverOutputModalities = QStringList{ QStringLiteral("text") };
	}
	if (!serverInputModalities.isEmpty() || !serverOutputModalities.isEmpty()) {
		caps.modelId = modelId;
		caps.inputModalities = serverInputModalities;
		caps.outputModalities = serverOutputModalities;
		caps.modalitySource = AgentModelCapabilitySource::ServerMetadata;
	}

	for (const char *vKey : {"vision", "supports_vision", "multimodal"}) {
		const QLatin1String k(vKey);
		if (obj.contains(k)) {
			if (obj.value(k).isBool()) {
				explicitVisionDecision = true;
				serverVision = serverVision || obj.value(k).toBool();
			} else if (obj.value(k).isObject()) {
				explicitVisionDecision = true;
				serverVision = true;
			}
		}
	}

	// 3. OpenRouter / OpenAI-compatible style supported_parameters / supported_reasoning_efforts
	const QJsonArray supportedParams = obj.value(QStringLiteral("supported_parameters")).toArray();
	if (!supportedParams.isEmpty()) {
		explicitServerDecision = true;
		for (const auto &p : supportedParams) {
			const QString param = p.toString().trimmed().toLower();
			if (param == QStringLiteral("reasoning") ||
				param == QStringLiteral("reasoning_effort") ||
				param == QStringLiteral("include_reasoning")) {
				serverSupports = true;
				break;
			}
		}
	}
	for (const char *arrKey : {"supported_reasoning_efforts", "reasoning_efforts"}) {
		const QLatin1String k(arrKey);
		if (obj.value(k).isArray()) {
			explicitServerDecision = true;
			const auto parsed = parseEffortArray(obj.value(k).toArray());
			if (!parsed.isEmpty()) {
				serverEfforts = parsed;
				serverSupports = true;
			}
		}
	}

	// 4. Top-level boolean / object hints (reasoning / thinking / supports_reasoning)
	for (const char *boolKey : {"reasoning", "thinking", "supports_reasoning"}) {
		const QLatin1String k(boolKey);
		if (obj.contains(k)) {
			if (obj.value(k).isBool()) {
				explicitServerDecision = true;
				serverSupports = serverSupports || obj.value(k).toBool();
			} else if (obj.value(k).isObject()) {
				explicitServerDecision = true;
				serverSupports = true;
			}
		}
	}

	if (explicitServerDecision) {
		caps.modelId = modelId;
		caps.source = AgentModelCapabilitySource::ServerMetadata;
		caps.supportsReasoningEffort = serverSupports;
		if (!serverSupports) {
			caps.supportedEfforts.clear();
		} else if (!serverEfforts.isEmpty()) {
			caps.supportedEfforts = serverEfforts;
		} else if (caps.supportedEfforts.isEmpty()) {
			caps.supportedEfforts = {
				AgentReasoningEffort::Low,
				AgentReasoningEffort::Medium,
				AgentReasoningEffort::High,
			};
		}
	}
	if (explicitVisionDecision) {
		caps.modelId = modelId;
		caps.supportsVision = serverVision;
		caps.visionSource = AgentModelCapabilitySource::ServerMetadata;
	}

	auto readPositiveInt = [](const QJsonObject &source, const char *key) -> int {
		const QJsonValue v = source.value(QLatin1String(key));
		if (v.isDouble()) {
			const int iv = qRound(v.toDouble());
			return iv > 0 ? iv : 0;
		}
		if (v.isString()) {
			bool ok = false;
			const int iv = v.toString().trimmed().toInt(&ok);
			return (ok && iv > 0) ? iv : 0;
		}
		return 0;
	};
	int serverContextLength = 0;
	for (const char *key : {
		"context_length",
		"context_window",
		"max_context_length",
		"max_model_len",
		"inputTokenLimit",
		"max_input_tokens",
		"max_prompt_tokens"
	}) {
		serverContextLength = readPositiveInt(obj, key);
		if (serverContextLength > 0) {
			break;
		}
	}
	if (serverContextLength <= 0 && !capObj.isEmpty()) {
		const QJsonObject capLimits = capObj.value(QStringLiteral("limits")).toObject();
		for (const char *key : { "max_context_window_tokens", "max_prompt_tokens", "context_window", "context_length" }) {
			serverContextLength = readPositiveInt(capLimits, key);
			if (serverContextLength > 0) {
				break;
			}
		}
	}
	if (serverContextLength <= 0) {
		const QJsonObject topLimits = obj.value(QStringLiteral("limits")).toObject();
		for (const char *key : { "context_window", "context_length", "max_input_tokens", "max_prompt_tokens" }) {
			serverContextLength = readPositiveInt(topLimits, key);
			if (serverContextLength > 0) {
				break;
			}
		}
	}
	if (serverContextLength <= 0) {
		const QJsonObject topProvider = obj.value(QStringLiteral("top_provider")).toObject();
		serverContextLength = readPositiveInt(topProvider, "context_length");
	}
	if (serverContextLength > 0) {
		caps.modelId = modelId;
		caps.contextLengthTokens = serverContextLength;
		caps.contextLengthSource = AgentModelCapabilitySource::ServerMetadata;
	}
	return caps;
}

bool isNonChatServerModelObject(
		const QString &modelId,
		const QJsonObject &obj,
		const AgentModelCapabilities &caps = {}) {
	if (isNonChatAgentModelId(modelId)) {
		return true;
	}
	if (caps.modalitySource == AgentModelCapabilitySource::ServerMetadata &&
		!caps.inputModalities.isEmpty() &&
		!caps.inputModalities.contains(QStringLiteral("text"))) {
		return true;
	}
	const QString capType = obj.value(QStringLiteral("capabilities"))
		.toObject()
		.value(QStringLiteral("type"))
		.toString()
		.trimmed()
		.toLower();
	if (!capType.isEmpty() && capType != QStringLiteral("chat")) {
		return true;
	}
	const QJsonArray genMethods = obj.value(QStringLiteral("supportedGenerationMethods")).toArray();
	if (!genMethods.isEmpty()) {
		const bool supportsGenerateContent = std::any_of(
			genMethods.cbegin(),
			genMethods.cend(),
			[](const QJsonValue &m) {
				return m.toString().trimmed().compare(QStringLiteral("generateContent"), Qt::CaseInsensitive) == 0;
			});
		if (!supportsGenerateContent) {
			return true;
		}
	}
	return false;
}

QStringList extractModelsFromJson(
		const QByteArray &payload,
		QString *errorOut = nullptr,
		QHash<QString, AgentModelCapabilities> *capsOut = nullptr,
		int *filteredNonChatOut = nullptr,
		QStringList *speechModelsOut = nullptr,
		const QString &endpointHost = QString()) {
	if (capsOut) {
		capsOut->clear();
	}
	if (filteredNonChatOut) {
		*filteredNonChatOut = 0;
	}
	if (speechModelsOut) {
		speechModelsOut->clear();
	}
	QJsonParseError err{};
	const auto doc = QJsonDocument::fromJson(payload, &err);
	if (err.error != QJsonParseError::NoError || !doc.isObject()) {
		if (errorOut) {
			*errorOut = QStringLiteral("Invalid JSON response from models endpoint.");
		}
		return {};
	}
	const QJsonObject root = doc.object();
	if (root.contains(QStringLiteral("error")) && !root.value(QStringLiteral("error")).isNull()) {
		if (errorOut) {
			*errorOut = sanitizeAgentErrorMessage(extractErrorTextFromJson(payload));
		}
		return {};
	}

	QStringList chatModels;
	QStringList nonChatModels;
	QSet<QString> seen;
	const auto appendUnique = [&](const QString &id, const QJsonObject &obj) {
		const QString clean = id.trimmed();
		if (clean.isEmpty() || seen.contains(clean)) {
			return;
		}
		seen.insert(clean);
		const AgentModelCapabilities modelCaps = extractCapabilitiesForModelObject(clean, obj);
		if (capsOut && !capsOut->contains(clean)) {
			capsOut->insert(clean, modelCaps);
		}
		if (speechModelsOut && classifySpeechModel(clean, endpointHost, modelCaps).catalogMember) {
			speechModelsOut->push_back(clean);
		}
		if (isNonChatServerModelObject(clean, obj, modelCaps)) {
			nonChatModels.push_back(clean);
		} else {
			chatModels.push_back(clean);
		}
	};

	const QJsonArray dataArr = root.value(QStringLiteral("data")).toArray();
	for (const auto &val : dataArr) {
		const QJsonObject obj = val.toObject();
		appendUnique(obj.value(QStringLiteral("id")).toString(), obj);
	}
	if (seen.isEmpty()) {
		const QJsonArray modelsArr = root.value(QStringLiteral("models")).toArray();
		for (const auto &val : modelsArr) {
			if (val.isString()) {
				appendUnique(val.toString(), QJsonObject{});
			} else if (val.isObject()) {
				const QJsonObject obj = val.toObject();
				QString name = obj.value(QStringLiteral("id")).toString().trimmed();
				if (name.isEmpty()) {
					name = obj.value(QStringLiteral("name")).toString().trimmed();
					if (name.startsWith(QStringLiteral("models/"), Qt::CaseInsensitive)) {
						name = name.mid(7);
					}
				}
				appendUnique(name, obj);
			}
		}
	}

	if (!chatModels.isEmpty()) {
		if (filteredNonChatOut) {
			*filteredNonChatOut = nonChatModels.size();
		}
		return chatModels;
	}
	return nonChatModels;
}

QString formatMeetingContextPrompt(const AgentMeetingContext &ctx) {
	QStringList lines;
	if (ctx.inMeeting) {
		lines.push_back(QStringLiteral("Active Meeting ID: %1").arg(
			ctx.meetingId.isEmpty() ? QStringLiteral("unknown") : ctx.meetingId));
		if (!ctx.meetingName.trimmed().isEmpty()) {
			lines.push_back(QStringLiteral("Meeting Title: %1").arg(ctx.meetingName.trimmed()));
		}
		lines.push_back(QStringLiteral("Participants: %1").arg(ctx.participantCount));
		lines.push_back(QStringLiteral("Local Media State: mic=%1, camera=%2, screenShare=%3")
			.arg(ctx.micMuted ? QStringLiteral("muted") : QStringLiteral("unmuted"))
			.arg(ctx.cameraEnabled ? QStringLiteral("on") : QStringLiteral("off"))
			.arg(ctx.screenSharing ? QStringLiteral("sharing") : QStringLiteral("idle")));
		if (ctx.encryptedMeeting) {
			lines.push_back(QStringLiteral("E2EE Status: encrypted (explicitly authorized by user)"));
		}
		for (const QString &entry : ctx.recentSafeSummaryLines) {
			const QString clean = entry.trimmed();
			if (!clean.isEmpty()) {
				lines.push_back(clean);
			}
		}
	} else {
		lines.push_back(QStringLiteral("Client State: pre-meeting lobby (no active meeting joined)."));
	}
	return QStringLiteral("[Meeting Context]\n") + lines.join(QLatin1Char('\n'));
}

QString formatSchedulingClockPrompt(
		bool includeSchedulingRules,
		bool includeJoinPreferencesRule,
		bool includeContactsProfileRules) {
	QStringList lines;
	if (includeSchedulingRules) {
		const QDateTime now = QDateTime::currentDateTime();
		QString tzId = QString::fromUtf8(QTimeZone::systemTimeZoneId());
		if (tzId.isEmpty()) {
			tzId = QStringLiteral("Asia/Shanghai");
		}
		static const std::array<const char *, 7> kWeekdays = {
			"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"
		};
		const int dow = qBound(1, now.date().dayOfWeek(), 7);
		const QString weekdayName = QLatin1String(kWeekdays[static_cast<size_t>(dow - 1)]);

		lines.push_back(QStringLiteral("[Local Time & Scheduling Context]"));
		lines.push_back(QStringLiteral("Current Local Time: %1 (%2)")
			.arg(now.toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss")), weekdayName));
		lines.push_back(QStringLiteral("Current Unix Timestamp (seconds): %1").arg(now.toSecsSinceEpoch()));
		lines.push_back(QStringLiteral("Local Timezone ID: %1").arg(tzId));
		lines.push_back(QStringLiteral(
			"Scheduling Rules: Convert relative expressions (e.g. 'tomorrow 3pm', 'next Wednesday 10:00') into exact "
			"'YYYY-MM-DDTHH:mm:ss' in the user's local timezone before calling schedule_meeting or update_scheduled_meeting. "
			"Both standard recurrence (None, Daily, Weekly, WeekDay, Monthly) and custom recurrence "
			"(repeat_type='Custom' with repeat_unit='Day'|'Week'|'Month', repeat_interval>=1, optional repeat_days_of_week [0=Sun..6=Sat], "
			"optional repeat_end_time or repeat_times) are supported. "
			"If the user does not explicitly ask for a password, omit 'password' (default to no password). "
			"If the user does not explicitly specify camera/microphone join restrictions for a new scheduled meeting, omit disable_camera_on_join / disable_microphone_on_join so the meeting defaults to the user's current client media preferences."));
	}
	if (includeJoinPreferencesRule) {
		const QString audioPrefPath = QStringLiteral("%1 -> %2 -> '%3'")
			.arg(QCoreApplication::translate("MeetingUI", "Settings"),
				QCoreApplication::translate("MeetingUI", "Audio"),
				QCoreApplication::translate("MeetingUI", "Enable microphone on joining"));
		lines.push_back(QStringLiteral(
			"Client Join Preferences Rule: When the user says '默认静音入会' (mute on join by default), "
			"'入会时默认开启麦克风', or asks to check/change default microphone, camera, or speaker state when joining meetings "
			"(without explicitly asking to modify a specific scheduled meeting's server-side participant restrictions), "
			"ALWAYS call 'get_client_preferences' or 'update_client_preferences' (pass enable_microphone_on_join=false for '默认静音入会') "
			"to inspect or modify the local client %1 option, NOT 'update_scheduled_meeting'.")
			.arg(audioPrefPath));
	}
	if (includeContactsProfileRules) {
		lines.push_back(QStringLiteral(
			"Contacts, Organization & Personal Profile Rules: "
			"1) Use 'get_my_profile' to inspect the signed-in user's own profile (account, employee number, department, job title, email, phone, bio/signature) and self-only historical employee numbers / organization removal reasons. "
			"2) Use 'update_my_profile' to edit the signed-in user's own nickname, email, phone, or bio/signature (allowed whether in an organization or not; requires user confirmation). "
			"NEVER claim to edit employeeNo (auto-assigned on joining an organization, immutable and never reused) or jobTitle (only editable by the organization founder in the Web Org Admin Console). "
			"3) Treat 'list_org_departments' as the current account's visible tree. When 'orgMemberVisibility' is 'own_dept', only the user's departments and their ancestor paths are available. "
			"An ancestor can be visible while its member list is restricted. Treat 'membersRestricted=true' as a permission result, 'membersLoading=true' as pending, and 'cachedMemberCount' as cached rows rather than a department total. "
			"Do not infer that unseen departments or members do not exist. "
			"4) For a specific person, prefer an exact account with 'search_contacts'. A cross-department match can identify someone without granting access to their department's member list. "
			"'remoteSearchTriggered=true' means a background lookup started. An empty cached result at that point does not establish that the account is missing. "
			"Search results can omit employee number, department and job title. Guide the user to select the result and open its detail card for those fields. "
			"Cross-department detail fields are enabled by default and follow the founder's current setting. Report only details actually returned, and never substitute another user's data or use 'get_my_profile' for someone else. "
			"5) Both the desktop and Web console organization trees start collapsed. In the desktop tree, branch counters show direct child nodes by default or the number of distinct members in permitted descendants if configured. "
			"Leaf groups show online/member counts. Do not infer the active count mode or a total from cached member rows. "
			"6) Organization visibility, cross-department detail display and tree counters are founder-managed in the Web Org Admin Console via the top-bar Settings dialog and the subsequent apply confirmation. "
			"These are organization-wide controls. The desktop preference tools cannot read or change them. "
			"7) Send new contact requests with 'manage_contact_request' and action 'send', including for coworkers in the same department. "
			"Use the exact account, email, employee number, or user ID. If a search only yields fuzzy candidates, ask the user to select the intended recipient before sending. "
			"After sending, report that the request is awaiting the recipient when the tool returns a pending request. Only recipient acceptance creates a mutual contact. "
			"Do not describe a pending or rejected request as an added contact."));
	}
	return lines.join(QLatin1Char('\n'));
}

struct SseParseState {
	QMap<int, AgentToolCall> toolCallAcc;
	QMap<int, AgentThinkingBlock> thinkingAcc;
	QSet<int> toolCallHasDelta;
};

struct ProtocolDialectSpec {
	QUrl (*resolveEndpoint)(const QUrl &baseUrl, const QString &model, bool stream) = nullptr;
	QUrl (*resolveModels)(const QUrl &baseUrl) = nullptr;
	void (*applyAuthHeaders)(
		QNetworkRequest &netReq,
		const AgentProfile &profile,
		const QString &apiKey) = nullptr;
	QByteArray (*buildPayload)(
		const AgentChatRequest &req,
		const QString &combinedSystem) = nullptr;
	bool (*parseSseDelta)(
		const SseEvent &ev,
		const QJsonObject &obj,
		const QByteArray &rawTrimmed,
		AgentStreamDelta &outDelta,
		QString &errorOut,
		SseParseState &state) = nullptr;
};

struct AppliedCustomHeaderFlags {
	bool hasAuthorization = false;
	bool hasXApiKey = false;
	bool hasAnthropicVersion = false;
	bool hasXGoogApiKey = false;
};

AppliedCustomHeaderFlags applyValidatedCustomHeaders(
		QNetworkRequest &netReq,
		const QMap<QString, QString> &customHeaders) {
	AppliedCustomHeaderFlags flags;
	for (auto it = customHeaders.cbegin(); it != customHeaders.cend(); ++it) {
		if (!isValidCustomHeaderName(it.key()) || !isValidCustomHeaderValue(it.value())) {
			continue;
		}
		const QString trimmedName = it.key().trimmed();
		if (trimmedName.compare(QStringLiteral("authorization"), Qt::CaseInsensitive) == 0) {
			flags.hasAuthorization = true;
		} else if (trimmedName.compare(QStringLiteral("x-api-key"), Qt::CaseInsensitive) == 0) {
			flags.hasXApiKey = true;
		} else if (trimmedName.compare(QStringLiteral("anthropic-version"), Qt::CaseInsensitive) == 0) {
			flags.hasAnthropicVersion = true;
		} else if (trimmedName.compare(QStringLiteral("x-goog-api-key"), Qt::CaseInsensitive) == 0) {
			flags.hasXGoogApiKey = true;
		}
		netReq.setRawHeader(trimmedName.toUtf8(), it.value().toUtf8());
	}
	return flags;
}

void applyBearerAuthHeaders(
		QNetworkRequest &netReq,
		const AgentProfile &profile,
		const QString &apiKey) {
	const AppliedCustomHeaderFlags flags = applyValidatedCustomHeaders(netReq, profile.customHeaders);
	if (!flags.hasAuthorization && !apiKey.trimmed().isEmpty()) {
		netReq.setRawHeader("Authorization", QByteArray("Bearer ") + apiKey.trimmed().toUtf8());
	}
}

void applyAnthropicAuthHeaders(
		QNetworkRequest &netReq,
		const AgentProfile &profile,
		const QString &apiKey) {
	const AppliedCustomHeaderFlags flags = applyValidatedCustomHeaders(netReq, profile.customHeaders);
	if (!flags.hasAnthropicVersion) {
		netReq.setRawHeader("anthropic-version", "2023-06-01");
	}
	if (!flags.hasXApiKey && !flags.hasAuthorization && !apiKey.trimmed().isEmpty()) {
		netReq.setRawHeader("x-api-key", apiKey.trimmed().toUtf8());
	}
}

void applyGeminiAuthHeaders(
		QNetworkRequest &netReq,
		const AgentProfile &profile,
		const QString &apiKey) {
	const AppliedCustomHeaderFlags flags = applyValidatedCustomHeaders(netReq, profile.customHeaders);
	if (!flags.hasXGoogApiKey && !flags.hasAuthorization && !apiKey.trimmed().isEmpty()) {
		netReq.setRawHeader("x-goog-api-key", apiKey.trimmed().toUtf8());
	}
}

bool hasValidMessageImages(const AgentChatMessage &msg) {
	return msg.role == AgentChatMessage::Role::User &&
		std::any_of(msg.images.cbegin(), msg.images.cend(), [](const AgentImageAttachment &img) {
			return img.isValid();
		});
}

QString buildImageDataUrl(const AgentImageAttachment &img) {
	return QStringLiteral("data:%1;base64,%2").arg(
		img.mimeType,
		QString::fromLatin1(img.encodedBytes.toBase64()));
}

QString buildCombinedSystemPrompt(const AgentChatRequest &req, bool includeClientUiGuide = true) {
	QString combinedSystem = req.profile.systemPrompt.trimmed();
	const auto appendBlock = [&combinedSystem](const QString &block) {
		if (block.isEmpty()) {
			return;
		}
		if (!combinedSystem.isEmpty()) {
			combinedSystem.append(QStringLiteral("\n\n"));
		}
		combinedSystem.append(block);
	};

	if (req.meetingContext.has_value() &&
		(!req.meetingContext->encryptedMeeting || req.meetingContext->e2eeContextAuthorized)) {
		appendBlock(formatMeetingContextPrompt(*req.meetingContext));
	}
	appendBlock(req.explicitContextPrompt.trimmed());

	if (req.mode == AgentChatMode::Ask && !req.tools.isEmpty()) {
		appendBlock(QStringLiteral(
			"[Interaction Mode: Ask]\n"
			"The user selected read-only Ask mode. Answer questions using only the available read-only inspection tools and provided context. "
			"Do not propose or attempt state-changing or confirmation actions."));
	}

	const bool hasSchedulingTools = std::any_of(
		req.tools.cbegin(),
		req.tools.cend(),
		[](const AgentToolDefinition &t) {
			return t.name == QStringLiteral("schedule_meeting") ||
				t.name == QStringLiteral("update_scheduled_meeting");
		});
	const bool hasJoinPrefTools = std::any_of(
		req.tools.cbegin(),
		req.tools.cend(),
		[](const AgentToolDefinition &t) {
			return t.name == QStringLiteral("update_client_preferences") ||
				t.name == QStringLiteral("set_join_media_preferences");
		});
	const bool hasContactProfileTools = std::any_of(
		req.tools.cbegin(),
		req.tools.cend(),
		[](const AgentToolDefinition &t) {
			return t.scope == AgentToolScope::AccountContacts;
		});
	if (hasSchedulingTools || hasJoinPrefTools || hasContactProfileTools) {
		appendBlock(formatSchedulingClockPrompt(hasSchedulingTools, hasJoinPrefTools, hasContactProfileTools));
	}
	if (includeClientUiGuide) {
		appendBlock(buildClientUiNavigationGuidePrompt());
	}
	return combinedSystem;
}

QByteArray buildOpenAiResponsesPayload(const AgentChatRequest &req, const QString &combinedSystem) {
	QJsonObject body;
	body.insert(QStringLiteral("model"), req.profile.model.trimmed());
	body.insert(QStringLiteral("stream"), req.stream);
	if (!combinedSystem.isEmpty()) {
		body.insert(QStringLiteral("instructions"), combinedSystem);
	}
	const QString effortWire = reasoningEffortToWire(req.profile.reasoningEffort);
	if (!effortWire.isEmpty()) {
		QJsonObject reasoningObj;
		reasoningObj.insert(QStringLiteral("effort"), effortWire);
		body.insert(QStringLiteral("reasoning"), reasoningObj);
	}
	QJsonArray inputArray;
	for (const auto &msg : req.messages) {
		if (msg.role == AgentChatMessage::Role::System) {
			continue;
		}
		if (msg.role == AgentChatMessage::Role::Tool) {
			if (msg.toolCallId.isEmpty()) {
				continue;
			}
			QJsonObject item;
			item.insert(QStringLiteral("type"), QStringLiteral("function_call_output"));
			item.insert(QStringLiteral("call_id"), msg.toolCallId);
			item.insert(QStringLiteral("output"), msg.content);
			inputArray.append(item);
			continue;
		}
		if (msg.role == AgentChatMessage::Role::Assistant && !msg.toolCalls.isEmpty()) {
			if (!msg.content.trimmed().isEmpty()) {
				QJsonObject textItem;
				textItem.insert(QStringLiteral("role"), QStringLiteral("assistant"));
				textItem.insert(QStringLiteral("content"), msg.content);
				inputArray.append(textItem);
			}
			for (const auto &tc : msg.toolCalls) {
				if (tc.id.isEmpty() || tc.name.isEmpty()) {
					continue;
				}
				QJsonObject callItem;
				callItem.insert(QStringLiteral("type"), QStringLiteral("function_call"));
				callItem.insert(QStringLiteral("call_id"), tc.id);
				callItem.insert(QStringLiteral("name"), tc.name);
				callItem.insert(QStringLiteral("arguments"),
					tc.argumentsJson.isEmpty() ? QStringLiteral("{}") : tc.argumentsJson);
				inputArray.append(callItem);
			}
			continue;
		}
		const QString trimmedContent = msg.content.trimmed();
		const bool msgHasImages = hasValidMessageImages(msg);
		if (trimmedContent.isEmpty() && !msgHasImages) {
			continue;
		}
		QJsonObject item;
		item.insert(QStringLiteral("role"),
			msg.role == AgentChatMessage::Role::Assistant ? QStringLiteral("assistant") : QStringLiteral("user"));
		if (msgHasImages) {
			QJsonArray contentParts;
			if (!trimmedContent.isEmpty()) {
				QJsonObject textPart;
				textPart.insert(QStringLiteral("type"), QStringLiteral("input_text"));
				textPart.insert(QStringLiteral("text"), msg.content);
				contentParts.append(textPart);
			}
			for (const auto &img : msg.images) {
				if (!img.isValid()) {
					continue;
				}
				QJsonObject imgPart;
				imgPart.insert(QStringLiteral("type"), QStringLiteral("input_image"));
				imgPart.insert(QStringLiteral("image_url"), buildImageDataUrl(img));
				imgPart.insert(QStringLiteral("detail"), QStringLiteral("auto"));
				contentParts.append(imgPart);
			}
			item.insert(QStringLiteral("content"), contentParts);
		} else {
			item.insert(QStringLiteral("content"), msg.content);
		}
		inputArray.append(item);
	}
	body.insert(QStringLiteral("input"), inputArray);
	if (!req.tools.isEmpty()) {
		QJsonArray toolsArr;
		for (const auto &tool : req.tools) {
			QJsonObject fn;
			fn.insert(QStringLiteral("type"), QStringLiteral("function"));
			fn.insert(QStringLiteral("name"), tool.name);
			fn.insert(QStringLiteral("description"), tool.description);
			fn.insert(QStringLiteral("parameters"), tool.parametersSchema);
			toolsArr.append(fn);
		}
		body.insert(QStringLiteral("tools"), toolsArr);
		body.insert(QStringLiteral("tool_choice"), QStringLiteral("auto"));
	}
	return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

QByteArray buildOpenAiChatOrWebhookPayload(const AgentChatRequest &req, const QString &combinedSystem) {
	const auto roleString = [](AgentChatMessage::Role role) -> QString {
		switch (role) {
		case AgentChatMessage::Role::System:
			return QStringLiteral("system");
		case AgentChatMessage::Role::User:
			return QStringLiteral("user");
		case AgentChatMessage::Role::Assistant:
			return QStringLiteral("assistant");
		case AgentChatMessage::Role::Tool:
			return QStringLiteral("tool");
		}
		return QStringLiteral("user");
	};

	QJsonArray messagesArray;
	if (!combinedSystem.isEmpty()) {
		QJsonObject sysMsg;
		sysMsg.insert(QStringLiteral("role"), QStringLiteral("system"));
		sysMsg.insert(QStringLiteral("content"), combinedSystem);
		messagesArray.append(sysMsg);
	}
	for (const auto &msg : req.messages) {
		if (msg.role == AgentChatMessage::Role::System) {
			continue;
		}
		if (msg.role == AgentChatMessage::Role::Tool) {
			if (msg.toolCallId.isEmpty()) {
				continue;
			}
			QJsonObject item;
			item.insert(QStringLiteral("role"), QStringLiteral("tool"));
			item.insert(QStringLiteral("tool_call_id"), msg.toolCallId);
			item.insert(QStringLiteral("content"), msg.content);
			messagesArray.append(item);
			continue;
		}
		if (msg.role == AgentChatMessage::Role::Assistant && !msg.toolCalls.isEmpty()) {
			QJsonObject item;
			item.insert(QStringLiteral("role"), QStringLiteral("assistant"));
			item.insert(QStringLiteral("content"),
				msg.content.trimmed().isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(msg.content));
			QJsonArray tcArr;
			for (const auto &tc : msg.toolCalls) {
				QJsonObject fn;
				fn.insert(QStringLiteral("name"), tc.name);
				fn.insert(QStringLiteral("arguments"),
					tc.argumentsJson.isEmpty() ? QStringLiteral("{}") : tc.argumentsJson);
				QJsonObject tcObj;
				tcObj.insert(QStringLiteral("id"), tc.id);
				tcObj.insert(QStringLiteral("type"), QStringLiteral("function"));
				tcObj.insert(QStringLiteral("function"), fn);
				tcArr.append(tcObj);
			}
			item.insert(QStringLiteral("tool_calls"), tcArr);
			messagesArray.append(item);
			continue;
		}
		const bool msgHasImages = hasValidMessageImages(msg);
		if (msg.content.trimmed().isEmpty() && !msgHasImages) {
			continue;
		}
		QJsonObject item;
		item.insert(QStringLiteral("role"), roleString(msg.role));
		if (msgHasImages) {
			QJsonArray contentParts;
			if (!msg.content.trimmed().isEmpty()) {
				QJsonObject textPart;
				textPart.insert(QStringLiteral("type"), QStringLiteral("text"));
				textPart.insert(QStringLiteral("text"), msg.content);
				contentParts.append(textPart);
			}
			QJsonArray webhookAttachments;
			for (const auto &img : msg.images) {
				if (!img.isValid()) {
					continue;
				}
				const QString dataUrl = buildImageDataUrl(img);
				QJsonObject imageUrlObj;
				imageUrlObj.insert(QStringLiteral("url"), dataUrl);
				imageUrlObj.insert(QStringLiteral("detail"), QStringLiteral("auto"));
				QJsonObject imgPart;
				imgPart.insert(QStringLiteral("type"), QStringLiteral("image_url"));
				imgPart.insert(QStringLiteral("image_url"), imageUrlObj);
				contentParts.append(imgPart);

				if (req.profile.protocol == AgentProtocolDialect::CustomWebhookJson) {
					QJsonObject attObj;
					attObj.insert(QStringLiteral("fileName"), img.fileName);
					attObj.insert(QStringLiteral("mimeType"), img.mimeType);
					attObj.insert(QStringLiteral("width"), img.width);
					attObj.insert(QStringLiteral("height"), img.height);
					attObj.insert(QStringLiteral("dataUrl"), dataUrl);
					webhookAttachments.append(attObj);
				}
			}
			item.insert(QStringLiteral("content"), contentParts);
			if (!webhookAttachments.isEmpty()) {
				item.insert(QStringLiteral("attachments"), webhookAttachments);
			}
		} else {
			item.insert(QStringLiteral("content"), msg.content);
		}
		messagesArray.append(item);
	}

	QJsonObject body;
	body.insert(QStringLiteral("model"), req.profile.model.trimmed());
	body.insert(QStringLiteral("stream"), req.stream);
	body.insert(QStringLiteral("temperature"), req.profile.temperature);
	if (req.profile.protocol == AgentProtocolDialect::OpenAiChatCompletionsSse) {
		const QString effortWire = reasoningEffortToWire(req.profile.reasoningEffort);
		if (!effortWire.isEmpty()) {
			body.insert(QStringLiteral("reasoning_effort"), effortWire);
		}
	}
	body.insert(QStringLiteral("messages"), messagesArray);

	if (req.profile.protocol == AgentProtocolDialect::OpenAiChatCompletionsSse && !req.tools.isEmpty()) {
		QJsonArray toolsArr;
		for (const auto &tool : req.tools) {
			QJsonObject fn;
			fn.insert(QStringLiteral("name"), tool.name);
			fn.insert(QStringLiteral("description"), tool.description);
			fn.insert(QStringLiteral("parameters"), tool.parametersSchema);
			QJsonObject wrapper;
			wrapper.insert(QStringLiteral("type"), QStringLiteral("function"));
			wrapper.insert(QStringLiteral("function"), fn);
			toolsArr.append(wrapper);
		}
		body.insert(QStringLiteral("tools"), toolsArr);
		body.insert(QStringLiteral("tool_choice"), QStringLiteral("auto"));
	}

	if (req.profile.protocol == AgentProtocolDialect::CustomWebhookJson) {
		body.insert(QStringLiteral("profileId"), req.profile.id);
		if (!combinedSystem.isEmpty()) {
			body.insert(QStringLiteral("systemPrompt"), combinedSystem);
		}
	}

	return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

bool isLegacyClaude3ModelId(const QString &modelId) {
	const QString lower = modelId.trimmed().toLower();
	return lower.startsWith(QStringLiteral("claude-3-")) &&
		!lower.contains(QStringLiteral("3-5")) &&
		!lower.contains(QStringLiteral("3.5")) &&
		!lower.contains(QStringLiteral("3-7")) &&
		!lower.contains(QStringLiteral("3.7"));
}

bool isValidSignedAnthropicThinkingBlock(const AgentThinkingBlock &tb) {
	if (tb.type == QStringLiteral("redacted_thinking")) {
		return !tb.thinking.isEmpty();
	}
	return tb.type == QStringLiteral("thinking") &&
		!tb.thinking.isEmpty() &&
		!tb.signature.isEmpty();
}

QJsonObject parseJsonObjectOrEmpty(const QString &jsonText) {
	if (jsonText.trimmed().isEmpty()) {
		return {};
	}
	QJsonParseError err{};
	const QJsonDocument doc = QJsonDocument::fromJson(jsonText.toUtf8(), &err);
	return (err.error == QJsonParseError::NoError && doc.isObject()) ? doc.object() : QJsonObject{};
}

void appendMergedRoleTurn(
		QJsonArray &messagesArray,
		const QString &partsKey,
		const QString &role,
		const QJsonArray &blocks) {
	if (blocks.isEmpty()) {
		return;
	}
	if (!messagesArray.isEmpty()) {
		QJsonObject last = messagesArray.last().toObject();
		if (last.value(QStringLiteral("role")).toString() == role) {
			QJsonArray merged = last.value(partsKey).toArray();
			for (const auto &b : blocks) {
				merged.append(b);
			}
			last.insert(partsKey, merged);
			messagesArray.replace(messagesArray.size() - 1, last);
			return;
		}
	}
	QJsonObject turn;
	turn.insert(QStringLiteral("role"), role);
	turn.insert(partsKey, blocks);
	messagesArray.append(turn);
}

void appendAnthropicTurn(
		QJsonArray &messagesArray,
		const QString &role,
		const QJsonArray &blocks) {
	appendMergedRoleTurn(messagesArray, QStringLiteral("content"), role, blocks);
}

QByteArray buildAnthropicMessagesPayload(const AgentChatRequest &req, const QString &combinedSystem) {
	QJsonObject body;
	const QString modelId = req.profile.model.trimmed();
	body.insert(QStringLiteral("model"), modelId);
	body.insert(QStringLiteral("stream"), req.stream);
	if (!combinedSystem.isEmpty()) {
		body.insert(QStringLiteral("system"), combinedSystem);
	}

	bool canEnableThinking = (req.profile.reasoningEffort != AgentReasoningEffort::Default);
	if (canEnableThinking) {
		for (const auto &msg : req.messages) {
			if (msg.role != AgentChatMessage::Role::Assistant || msg.toolCalls.isEmpty()) {
				continue;
			}
			const bool hasSignedBlock = std::any_of(
				msg.thinkingBlocks.cbegin(),
				msg.thinkingBlocks.cend(),
				isValidSignedAnthropicThinkingBlock);
			if (!hasSignedBlock) {
				// Anthropic rejects tool_use continuation when thinking is enabled without signed prior thinking blocks.
				canEnableThinking = false;
				break;
			}
		}
	}

	if (canEnableThinking) {
		int budgetTokens = 4096;
		int maxTokens = 12288;
		switch (req.profile.reasoningEffort) {
		case AgentReasoningEffort::Low:
			budgetTokens = 2048;
			maxTokens = 8192;
			break;
		case AgentReasoningEffort::Medium:
			budgetTokens = 4096;
			maxTokens = 12288;
			break;
		case AgentReasoningEffort::High:
			budgetTokens = 8192;
			maxTokens = 16384;
			break;
		case AgentReasoningEffort::Extra:
			budgetTokens = 16384;
			maxTokens = 24576;
			break;
		case AgentReasoningEffort::Default:
			break;
		}
		QJsonObject thinkingObj;
		thinkingObj.insert(QStringLiteral("type"), QStringLiteral("enabled"));
		thinkingObj.insert(QStringLiteral("budget_tokens"), budgetTokens);
		body.insert(QStringLiteral("thinking"), thinkingObj);
		body.insert(QStringLiteral("max_tokens"), maxTokens);
	} else {
		body.insert(QStringLiteral("max_tokens"), isLegacyClaude3ModelId(modelId) ? 4096 : 8192);
		body.insert(QStringLiteral("temperature"), qBound(0.0, req.profile.temperature, 1.0));
	}

	QJsonArray messagesArray;
	for (const auto &msg : req.messages) {
		if (msg.role == AgentChatMessage::Role::System) {
			continue;
		}
		if (msg.role == AgentChatMessage::Role::Tool) {
			if (msg.toolCallId.isEmpty()) {
				continue;
			}
			QJsonObject resBlock;
			resBlock.insert(QStringLiteral("type"), QStringLiteral("tool_result"));
			resBlock.insert(QStringLiteral("tool_use_id"), msg.toolCallId);
			resBlock.insert(QStringLiteral("content"), msg.content);
			QJsonArray blocks;
			blocks.append(resBlock);
			appendAnthropicTurn(messagesArray, QStringLiteral("user"), blocks);
			continue;
		}
		if (msg.role == AgentChatMessage::Role::Assistant) {
			QJsonArray blocks;
			for (const auto &tb : msg.thinkingBlocks) {
				if (!isValidSignedAnthropicThinkingBlock(tb)) {
					continue;
				}
				QJsonObject tbObj;
				tbObj.insert(QStringLiteral("type"), tb.type);
				if (tb.type == QStringLiteral("redacted_thinking")) {
					tbObj.insert(QStringLiteral("data"), tb.thinking);
				} else {
					tbObj.insert(QStringLiteral("thinking"), tb.thinking);
					tbObj.insert(QStringLiteral("signature"), tb.signature);
				}
				blocks.append(tbObj);
			}
			if (!msg.content.trimmed().isEmpty()) {
				QJsonObject textBlock;
				textBlock.insert(QStringLiteral("type"), QStringLiteral("text"));
				textBlock.insert(QStringLiteral("text"), msg.content);
				blocks.append(textBlock);
			}
			for (const auto &tc : msg.toolCalls) {
				if (tc.id.isEmpty() || tc.name.isEmpty()) {
					continue;
				}
				QJsonObject toolBlock;
				toolBlock.insert(QStringLiteral("type"), QStringLiteral("tool_use"));
				toolBlock.insert(QStringLiteral("id"), tc.id);
				toolBlock.insert(QStringLiteral("name"), tc.name);
				toolBlock.insert(QStringLiteral("input"), parseJsonObjectOrEmpty(tc.argumentsJson));
				blocks.append(toolBlock);
			}
			appendAnthropicTurn(messagesArray, QStringLiteral("assistant"), blocks);
			continue;
		}

		QJsonArray blocks;
		if (!msg.content.trimmed().isEmpty()) {
			QJsonObject textBlock;
			textBlock.insert(QStringLiteral("type"), QStringLiteral("text"));
			textBlock.insert(QStringLiteral("text"), msg.content);
			blocks.append(textBlock);
		}
		for (const auto &img : msg.images) {
			if (!img.isValid()) {
				continue;
			}
			QJsonObject srcObj;
			srcObj.insert(QStringLiteral("type"), QStringLiteral("base64"));
			srcObj.insert(QStringLiteral("media_type"), img.mimeType);
			srcObj.insert(QStringLiteral("data"), QString::fromLatin1(img.encodedBytes.toBase64()));
			QJsonObject imgBlock;
			imgBlock.insert(QStringLiteral("type"), QStringLiteral("image"));
			imgBlock.insert(QStringLiteral("source"), srcObj);
			blocks.append(imgBlock);
		}
		appendAnthropicTurn(messagesArray, QStringLiteral("user"), blocks);
	}

	while (!messagesArray.isEmpty() &&
		messagesArray.first().toObject().value(QStringLiteral("role")).toString() != QStringLiteral("user")) {
		messagesArray.removeFirst();
	}
	body.insert(QStringLiteral("messages"), messagesArray);

	if (!req.tools.isEmpty()) {
		QJsonArray toolsArr;
		for (const auto &tool : req.tools) {
			QJsonObject schema = tool.parametersSchema;
			if (schema.isEmpty()) {
				schema.insert(QStringLiteral("type"), QStringLiteral("object"));
				schema.insert(QStringLiteral("properties"), QJsonObject{});
			}
			QJsonObject toolObj;
			toolObj.insert(QStringLiteral("name"), tool.name);
			toolObj.insert(QStringLiteral("description"), tool.description);
			toolObj.insert(QStringLiteral("input_schema"), schema);
			toolsArr.append(toolObj);
		}
		body.insert(QStringLiteral("tools"), toolsArr);
		QJsonObject toolChoice;
		toolChoice.insert(QStringLiteral("type"), QStringLiteral("auto"));
		body.insert(QStringLiteral("tool_choice"), toolChoice);
	}

	return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

QJsonObject sanitizeGeminiFunctionSchema(const QJsonObject &schema, bool isRoot = true) {
	if (schema.isEmpty()) {
		return {};
	}
	QJsonObject out;
	QString upperType;
	for (auto it = schema.constBegin(); it != schema.constEnd(); ++it) {
		const QString k = it.key();
		if (k == QStringLiteral("additionalProperties") ||
			k == QStringLiteral("$schema") ||
			k == QStringLiteral("$id") ||
			k == QStringLiteral("default")) {
			continue;
		}
		if (k == QStringLiteral("type") && it.value().isString()) {
			upperType = it.value().toString().trimmed().toUpper();
			out.insert(k, upperType);
		} else if (k == QStringLiteral("properties") && it.value().isObject()) {
			const QJsonObject rawProps = it.value().toObject();
			QJsonObject cleanProps;
			for (auto pIt = rawProps.constBegin(); pIt != rawProps.constEnd(); ++pIt) {
				if (pIt.value().isObject()) {
					cleanProps.insert(pIt.key(), sanitizeGeminiFunctionSchema(pIt.value().toObject(), false));
				} else {
					cleanProps.insert(pIt.key(), pIt.value());
				}
			}
			out.insert(k, cleanProps);
		} else if (k == QStringLiteral("items") && it.value().isObject()) {
			out.insert(k, sanitizeGeminiFunctionSchema(it.value().toObject(), false));
		} else {
			out.insert(k, it.value());
		}
	}
	if (isRoot && upperType == QStringLiteral("OBJECT") &&
		out.value(QStringLiteral("properties")).toObject().isEmpty()) {
		return {};
	}
	return out;
}

QByteArray buildGeminiPayload(const AgentChatRequest &req, const QString &combinedSystem) {
	QJsonObject body;
	if (!combinedSystem.isEmpty()) {
		QJsonObject part;
		part.insert(QStringLiteral("text"), combinedSystem);
		QJsonArray parts;
		parts.append(part);
		QJsonObject sysInst;
		sysInst.insert(QStringLiteral("parts"), parts);
		body.insert(QStringLiteral("systemInstruction"), sysInst);
	}

	QJsonObject genConfig;
	genConfig.insert(QStringLiteral("temperature"), qBound(0.0, req.profile.temperature, 2.0));
	if (req.profile.reasoningEffort != AgentReasoningEffort::Default) {
		QString normModel = req.profile.model.trimmed().toLower();
		const int slash = normModel.lastIndexOf(QLatin1Char('/'));
		if (slash >= 0 && slash + 1 < normModel.size()) {
			normModel = normModel.mid(slash + 1);
		}
		const bool useThinkingLevel =
			normModel.startsWith(QStringLiteral("gemini-3")) ||
			normModel.startsWith(QStringLiteral("gemini-4"));
		QJsonObject thinkingCfg;
		thinkingCfg.insert(QStringLiteral("includeThoughts"), true);
		if (useThinkingLevel) {
			QString level = QStringLiteral("MEDIUM");
			switch (req.profile.reasoningEffort) {
			case AgentReasoningEffort::Low:
				level = QStringLiteral("LOW");
				break;
			case AgentReasoningEffort::Medium:
				level = QStringLiteral("MEDIUM");
				break;
			case AgentReasoningEffort::High:
			case AgentReasoningEffort::Extra:
				level = QStringLiteral("HIGH");
				break;
			case AgentReasoningEffort::Default:
				break;
			}
			thinkingCfg.insert(QStringLiteral("thinkingLevel"), level);
		} else {
			int budget = 6144;
			switch (req.profile.reasoningEffort) {
			case AgentReasoningEffort::Low:
				budget = 2048;
				break;
			case AgentReasoningEffort::Medium:
				budget = 6144;
				break;
			case AgentReasoningEffort::High:
				budget = 16384;
				break;
			case AgentReasoningEffort::Extra:
				budget = 24576;
				break;
			case AgentReasoningEffort::Default:
				break;
			}
			thinkingCfg.insert(QStringLiteral("thinkingBudget"), budget);
		}
		genConfig.insert(QStringLiteral("thinkingConfig"), thinkingCfg);
	}
	body.insert(QStringLiteral("generationConfig"), genConfig);

	QHash<QString, AgentToolCall> callById;
	for (const auto &msg : req.messages) {
		if (msg.role == AgentChatMessage::Role::Assistant) {
			for (const auto &tc : msg.toolCalls) {
				if (!tc.id.isEmpty()) {
					callById.insert(tc.id, tc);
				}
			}
		}
	}

	QJsonArray contentsArray;
	for (const auto &msg : req.messages) {
		if (msg.role == AgentChatMessage::Role::System) {
			continue;
		}
		if (msg.role == AgentChatMessage::Role::Tool) {
			const AgentToolCall matchedCall = callById.value(msg.toolCallId);
			const QString fnName = !msg.toolName.isEmpty() ? msg.toolName : matchedCall.name;
			if (fnName.isEmpty()) {
				continue;
			}
			QJsonObject respObj;
			QJsonParseError respErr{};
			const QJsonDocument respDoc = QJsonDocument::fromJson(msg.content.toUtf8(), &respErr);
			if (respErr.error == QJsonParseError::NoError && respDoc.isObject()) {
				respObj = respDoc.object();
			} else if (respErr.error == QJsonParseError::NoError && respDoc.isArray()) {
				respObj.insert(QStringLiteral("result"), respDoc.array());
			} else {
				respObj.insert(QStringLiteral("result"), msg.content);
			}
			QJsonObject frObj;
			frObj.insert(QStringLiteral("name"), fnName);
			frObj.insert(QStringLiteral("response"), respObj);
			if (!matchedCall.providerCallId.isEmpty()) {
				frObj.insert(QStringLiteral("id"), matchedCall.providerCallId);
			}
			QJsonObject partObj;
			partObj.insert(QStringLiteral("functionResponse"), frObj);
			QJsonArray parts;
			parts.append(partObj);
			appendMergedRoleTurn(contentsArray, QStringLiteral("parts"), QStringLiteral("user"), parts);
			continue;
		}
		if (msg.role == AgentChatMessage::Role::Assistant) {
			QJsonArray parts;
			QString geminiSig;
			for (const auto &tb : msg.thinkingBlocks) {
				if (tb.type == QStringLiteral("gemini_thought") && !tb.signature.isEmpty()) {
					geminiSig = tb.signature;
				}
			}
			if (!msg.content.trimmed().isEmpty()) {
				QJsonObject textPart;
				textPart.insert(QStringLiteral("text"), msg.content);
				if (msg.toolCalls.isEmpty() && !geminiSig.isEmpty()) {
					textPart.insert(QStringLiteral("thoughtSignature"), geminiSig);
				}
				parts.append(textPart);
			} else if (msg.toolCalls.isEmpty() && !geminiSig.isEmpty()) {
				QJsonObject sigPart;
				sigPart.insert(QStringLiteral("thoughtSignature"), geminiSig);
				parts.append(sigPart);
			}
			for (const auto &tc : msg.toolCalls) {
				if (tc.name.isEmpty()) {
					continue;
				}
				QJsonObject fcObj;
				fcObj.insert(QStringLiteral("name"), tc.name);
				fcObj.insert(QStringLiteral("args"), parseJsonObjectOrEmpty(tc.argumentsJson));
				if (!tc.providerCallId.isEmpty()) {
					fcObj.insert(QStringLiteral("id"), tc.providerCallId);
				}
				QJsonObject partObj;
				partObj.insert(QStringLiteral("functionCall"), fcObj);
				if (!tc.thoughtSignature.isEmpty()) {
					partObj.insert(QStringLiteral("thoughtSignature"), tc.thoughtSignature);
				}
				parts.append(partObj);
			}
			appendMergedRoleTurn(contentsArray, QStringLiteral("parts"), QStringLiteral("model"), parts);
			continue;
		}

		QJsonArray parts;
		if (!msg.content.trimmed().isEmpty()) {
			QJsonObject textPart;
			textPart.insert(QStringLiteral("text"), msg.content);
			parts.append(textPart);
		}
		for (const auto &img : msg.images) {
			if (!img.isValid()) {
				continue;
			}
			QJsonObject inlineObj;
			inlineObj.insert(QStringLiteral("mimeType"), img.mimeType);
			inlineObj.insert(QStringLiteral("data"), QString::fromLatin1(img.encodedBytes.toBase64()));
			QJsonObject imgPart;
			imgPart.insert(QStringLiteral("inlineData"), inlineObj);
			parts.append(imgPart);
		}
		appendMergedRoleTurn(contentsArray, QStringLiteral("parts"), QStringLiteral("user"), parts);
	}

	while (!contentsArray.isEmpty() &&
		contentsArray.first().toObject().value(QStringLiteral("role")).toString() != QStringLiteral("user")) {
		contentsArray.removeFirst();
	}
	body.insert(QStringLiteral("contents"), contentsArray);

	if (!req.tools.isEmpty()) {
		QJsonArray declsArr;
		for (const auto &tool : req.tools) {
			QJsonObject decl;
			decl.insert(QStringLiteral("name"), tool.name);
			decl.insert(QStringLiteral("description"), tool.description);
			const QJsonObject cleanedSchema = sanitizeGeminiFunctionSchema(tool.parametersSchema, true);
			if (!cleanedSchema.isEmpty()) {
				decl.insert(QStringLiteral("parameters"), cleanedSchema);
			}
			declsArr.append(decl);
		}
		QJsonObject toolWrapper;
		toolWrapper.insert(QStringLiteral("functionDeclarations"), declsArr);
		QJsonArray toolsArr;
		toolsArr.append(toolWrapper);
		body.insert(QStringLiteral("tools"), toolsArr);

		QJsonObject fnCallingCfg;
		fnCallingCfg.insert(QStringLiteral("mode"), QStringLiteral("AUTO"));
		QJsonObject toolCfg;
		toolCfg.insert(QStringLiteral("functionCallingConfig"), fnCallingCfg);
		body.insert(QStringLiteral("toolConfig"), toolCfg);
	}

	return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

QString extractTextFromContentValue(const QJsonValue &val) {
	if (val.isString()) {
		return val.toString();
	}
	if (!val.isArray()) {
		return {};
	}
	QString combined;
	const QJsonArray parts = val.toArray();
	for (const auto &part : parts) {
		if (part.isString()) {
			combined.append(part.toString());
			continue;
		}
		if (!part.isObject()) {
			continue;
		}
		const QJsonObject obj = part.toObject();
		const QString text = obj.value(QStringLiteral("text")).toString();
		if (!text.isEmpty()) {
			combined.append(text);
			continue;
		}
		const QString content = obj.value(QStringLiteral("content")).toString();
		if (!content.isEmpty()) {
			combined.append(content);
		}
	}
	return combined;
}

void accumulateToolCalls(const QJsonArray &toolCallsArray, QMap<int, AgentToolCall> &acc) {
	for (int i = 0; i < toolCallsArray.size(); ++i) {
		const QJsonObject tcObj = toolCallsArray.at(i).toObject();
		const int index = tcObj.contains(QStringLiteral("index"))
			? tcObj.value(QStringLiteral("index")).toInt(i)
			: i;
		AgentToolCall &entry = acc[index];
		const QString id = tcObj.value(QStringLiteral("id")).toString();
		if (!id.isEmpty()) {
			entry.id = id;
		}
		const QJsonObject fn = tcObj.value(QStringLiteral("function")).toObject();
		const QString name = fn.value(QStringLiteral("name")).toString();
		if (!name.isEmpty()) {
			entry.name = name;
		}
		const QString args = fn.value(QStringLiteral("arguments")).toString();
		if (!args.isEmpty()) {
			entry.argumentsJson.append(args);
		}
	}
}

bool parseOpenAiResponsesSseDelta(
		const SseEvent &ev,
		const QJsonObject &obj,
		const QByteArray &,
		AgentStreamDelta &outDelta,
		QString &errorOut,
		SseParseState &state) {
	QMap<int, AgentToolCall> *toolCallAcc = &state.toolCallAcc;
	const QString eventType = !ev.event.isEmpty()
		? ev.event
		: obj.value(QStringLiteral("type")).toString();
	const auto resolveOutputIndex = [&obj, toolCallAcc]() -> int {
		if (obj.contains(QStringLiteral("output_index"))) {
			return obj.value(QStringLiteral("output_index")).toInt(0);
		}
		return (!toolCallAcc || toolCallAcc->isEmpty()) ? 0 : toolCallAcc->lastKey();
	};
	const auto applyFunctionCallItem = [toolCallAcc](int index, const QJsonObject &item, bool overwriteArgs) {
		if (!toolCallAcc || item.value(QStringLiteral("type")).toString() != QStringLiteral("function_call")) {
			return;
		}
		AgentToolCall &entry = (*toolCallAcc)[index];
		const QString callId = item.value(QStringLiteral("call_id")).toString();
		const QString itemId = item.value(QStringLiteral("id")).toString();
		if (!callId.isEmpty()) {
			entry.id = callId;
		} else if (entry.id.isEmpty() && !itemId.isEmpty()) {
			entry.id = itemId;
		}
		const QString name = item.value(QStringLiteral("name")).toString();
		if (!name.isEmpty()) {
			entry.name = name;
		}
		const QString args = item.value(QStringLiteral("arguments")).toString();
		if (!args.isEmpty() && (overwriteArgs || entry.argumentsJson.isEmpty())) {
			entry.argumentsJson = args;
		}
	};

	if (eventType == QStringLiteral("response.output_text.delta")) {
		outDelta.textDelta = obj.value(QStringLiteral("delta")).toString();
		return true;
	}
	if (eventType == QStringLiteral("response.reasoning_summary_text.delta") ||
		eventType == QStringLiteral("response.reasoning_text.delta") ||
		eventType == QStringLiteral("response.reasoning.delta")) {
		outDelta.reasoningDelta = obj.value(QStringLiteral("delta")).toString();
		return true;
	}
	if (eventType == QStringLiteral("response.output_item.added")) {
		const int index = obj.contains(QStringLiteral("output_index"))
			? obj.value(QStringLiteral("output_index")).toInt(toolCallAcc->size())
			: toolCallAcc->size();
		applyFunctionCallItem(index, obj.value(QStringLiteral("item")).toObject(), false);
		return true;
	}
	if (eventType == QStringLiteral("response.function_call_arguments.delta")) {
		AgentToolCall &entry = (*toolCallAcc)[resolveOutputIndex()];
		const QString argDelta = obj.value(QStringLiteral("delta")).toString();
		if (!argDelta.isEmpty()) {
			entry.argumentsJson.append(argDelta);
		}
		return true;
	}
	if (eventType == QStringLiteral("response.function_call_arguments.done")) {
		AgentToolCall &entry = (*toolCallAcc)[resolveOutputIndex()];
		const QString fullArgs = obj.value(QStringLiteral("arguments")).toString();
		if (!fullArgs.isEmpty()) {
			entry.argumentsJson = fullArgs;
		}
		const QString name = obj.value(QStringLiteral("name")).toString();
		if (!name.isEmpty()) {
			entry.name = name;
		}
		const QString callId = obj.value(QStringLiteral("call_id")).toString();
		if (!callId.isEmpty()) {
			entry.id = callId;
		}
		return true;
	}
	if (eventType == QStringLiteral("response.output_item.done")) {
		applyFunctionCallItem(resolveOutputIndex(), obj.value(QStringLiteral("item")).toObject(), true);
		return true;
	}
	if (eventType == QStringLiteral("response.completed") ||
		eventType == QStringLiteral("response.done")) {
		if (toolCallAcc->isEmpty()) {
			QJsonArray outputArr = obj.value(QStringLiteral("response")).toObject()
				.value(QStringLiteral("output")).toArray();
			if (outputArr.isEmpty()) {
				outputArr = obj.value(QStringLiteral("output")).toArray();
			}
			for (int i = 0; i < outputArr.size(); ++i) {
				applyFunctionCallItem(i, outputArr.at(i).toObject(), true);
			}
		}
		outDelta.isDone = true;
		return true;
	}
	if (eventType == QStringLiteral("response.failed") || eventType == QStringLiteral("error")) {
		QString errText = obj.value(QStringLiteral("message")).toString();
		if (errText.isEmpty()) {
			errText = obj.value(QStringLiteral("response")).toObject()
				.value(QStringLiteral("error")).toObject()
				.value(QStringLiteral("message")).toString();
		}
		errorOut = sanitizeAgentErrorMessage(errText);
		return false;
	}
	return true;
}

bool parseAnthropicMessagesSseDelta(
		const SseEvent &ev,
		const QJsonObject &obj,
		const QByteArray &rawTrimmed,
		AgentStreamDelta &outDelta,
		QString &errorOut,
		SseParseState &state) {
	const QString eventType = !ev.event.isEmpty()
		? ev.event
		: obj.value(QStringLiteral("type")).toString();

	if (eventType == QStringLiteral("ping") || eventType == QStringLiteral("message_start")) {
		return true;
	}

	if (eventType == QStringLiteral("content_block_start")) {
		const int index = obj.value(QStringLiteral("index")).toInt(0);
		const QJsonObject block = obj.value(QStringLiteral("content_block")).toObject();
		const QString blockType = block.value(QStringLiteral("type")).toString();
		if (blockType == QStringLiteral("text")) {
			const QString initialText = block.value(QStringLiteral("text")).toString();
			if (!initialText.isEmpty()) {
				outDelta.textDelta = initialText;
			}
		} else if (blockType == QStringLiteral("thinking")) {
			AgentThinkingBlock tb;
			tb.type = QStringLiteral("thinking");
			tb.thinking = block.value(QStringLiteral("thinking")).toString();
			tb.signature = block.value(QStringLiteral("signature")).toString();
			state.thinkingAcc[index] = tb;
			if (!tb.thinking.isEmpty()) {
				outDelta.reasoningDelta = tb.thinking;
			}
		} else if (blockType == QStringLiteral("redacted_thinking")) {
			AgentThinkingBlock tb;
			tb.type = QStringLiteral("redacted_thinking");
			tb.thinking = block.value(QStringLiteral("data")).toString();
			state.thinkingAcc[index] = tb;
		} else if (blockType == QStringLiteral("tool_use")) {
			AgentToolCall &entry = state.toolCallAcc[index];
			entry.id = block.value(QStringLiteral("id")).toString();
			entry.name = block.value(QStringLiteral("name")).toString();
			state.toolCallHasDelta.remove(index);
			const QJsonValue inputVal = block.value(QStringLiteral("input"));
			if (inputVal.isObject() && !inputVal.toObject().isEmpty()) {
				entry.argumentsJson = QString::fromUtf8(
					QJsonDocument(inputVal.toObject()).toJson(QJsonDocument::Compact));
			} else {
				entry.argumentsJson.clear();
			}
		}
		return true;
	}

	if (eventType == QStringLiteral("content_block_delta")) {
		const int index = obj.value(QStringLiteral("index")).toInt(0);
		const QJsonObject deltaObj = obj.value(QStringLiteral("delta")).toObject();
		const QString deltaType = deltaObj.value(QStringLiteral("type")).toString();
		if (deltaType == QStringLiteral("text_delta")) {
			outDelta.textDelta = deltaObj.value(QStringLiteral("text")).toString();
		} else if (deltaType == QStringLiteral("thinking_delta")) {
			const QString chunk = deltaObj.value(QStringLiteral("thinking")).toString();
			outDelta.reasoningDelta = chunk;
			AgentThinkingBlock &tb = state.thinkingAcc[index];
			tb.type = QStringLiteral("thinking");
			tb.thinking.append(chunk);
		} else if (deltaType == QStringLiteral("signature_delta")) {
			AgentThinkingBlock &tb = state.thinkingAcc[index];
			tb.type = QStringLiteral("thinking");
			tb.signature.append(deltaObj.value(QStringLiteral("signature")).toString());
		} else if (deltaType == QStringLiteral("input_json_delta")) {
			const QString partial = deltaObj.value(QStringLiteral("partial_json")).toString();
			AgentToolCall &entry = state.toolCallAcc[index];
			if (!state.toolCallHasDelta.contains(index)) {
				entry.argumentsJson.clear();
				state.toolCallHasDelta.insert(index);
			}
			entry.argumentsJson.append(partial);
		}
		return true;
	}

	if (eventType == QStringLiteral("content_block_stop")) {
		const int index = obj.value(QStringLiteral("index")).toInt(0);
		if (state.thinkingAcc.contains(index)) {
			AgentThinkingBlock tb = state.thinkingAcc.take(index);
			if (isValidSignedAnthropicThinkingBlock(tb)) {
				outDelta.thinkingBlocks.push_back(std::move(tb));
			}
		}
		if (state.toolCallAcc.contains(index)) {
			if (state.toolCallAcc[index].argumentsJson.isEmpty()) {
				state.toolCallAcc[index].argumentsJson = QStringLiteral("{}");
			}
			state.toolCallHasDelta.remove(index);
		}
		return true;
	}

	if (eventType == QStringLiteral("message_delta")) {
		const QString stopReason = obj.value(QStringLiteral("delta"))
			.toObject()
			.value(QStringLiteral("stop_reason"))
			.toString();
		if (!stopReason.isEmpty()) {
			outDelta.finishReason = stopReason;
		}
		return true;
	}

	if (eventType == QStringLiteral("message_stop")) {
		outDelta.isDone = true;
		return true;
	}

	if (eventType == QStringLiteral("error")) {
		QString errText = extractErrorTextFromJson(rawTrimmed);
		if (errText.isEmpty()) {
			errText = obj.value(QStringLiteral("message")).toString();
		}
		errorOut = sanitizeAgentErrorMessage(errText);
		return false;
	}

	return true;
}

bool parseOpenAiChatCompletionsSseDelta(
		const SseEvent &,
		const QJsonObject &obj,
		const QByteArray &,
		AgentStreamDelta &outDelta,
		QString &,
		SseParseState &state) {
	const auto choices = obj.value(QStringLiteral("choices")).toArray();
	if (!choices.isEmpty()) {
		const QJsonObject first = choices.first().toObject();
		const QJsonObject deltaObj = first.value(QStringLiteral("delta")).toObject();
		QString reasoning = deltaObj.value(QStringLiteral("reasoning_content")).toString();
		if (reasoning.isEmpty()) {
			reasoning = deltaObj.value(QStringLiteral("reasoning")).toString();
		}
		QString text = extractTextFromContentValue(deltaObj.value(QStringLiteral("content")));
		if (text.isEmpty()) {
			text = deltaObj.value(QStringLiteral("text")).toString();
		}
		if (text.isEmpty()) {
			text = first.value(QStringLiteral("text")).toString();
		}
		if (first.value(QStringLiteral("message")).isObject()) {
			const QJsonObject msgObj = first.value(QStringLiteral("message")).toObject();
			if (reasoning.isEmpty()) {
				reasoning = msgObj.value(QStringLiteral("reasoning_content")).toString();
			}
			if (reasoning.isEmpty()) {
				reasoning = msgObj.value(QStringLiteral("reasoning")).toString();
			}
			if (text.isEmpty()) {
				text = extractTextFromContentValue(msgObj.value(QStringLiteral("content")));
			}
			if (msgObj.value(QStringLiteral("tool_calls")).isArray()) {
				accumulateToolCalls(msgObj.value(QStringLiteral("tool_calls")).toArray(), state.toolCallAcc);
			}
		}
		outDelta.reasoningDelta = reasoning;
		outDelta.textDelta = text;
		if (deltaObj.value(QStringLiteral("tool_calls")).isArray()) {
			accumulateToolCalls(deltaObj.value(QStringLiteral("tool_calls")).toArray(), state.toolCallAcc);
		}
		const auto finishVal = first.value(QStringLiteral("finish_reason"));
		if (finishVal.isString() && !finishVal.toString().isEmpty()) {
			outDelta.finishReason = finishVal.toString();
		}
		return true;
	}

	// Fallback custom SSE keys (delta / content / text / reply)
	for (const char *key : {"delta", "content", "text", "reply"}) {
		const QString val = obj.value(QLatin1String(key)).toString();
		if (!val.isEmpty()) {
			outDelta.textDelta = val;
			return true;
		}
	}
	if (obj.value(QStringLiteral("done")).toBool(false)) {
		outDelta.isDone = true;
	}
	return true;
}

bool parseGeminiResponseObject(
		const QJsonObject &root,
		QString *outText,
		QString *outReasoning,
		QVector<AgentToolCall> *outToolCalls,
		QVector<AgentThinkingBlock> *outThinkingBlocks,
		QString *outFinishReason,
		QString *errorOut) {
	const QJsonArray candidates = root.value(QStringLiteral("candidates")).toArray();
	if (candidates.isEmpty()) {
		const QString blockReason = root.value(QStringLiteral("promptFeedback"))
			.toObject()
			.value(QStringLiteral("blockReason"))
			.toString()
			.trimmed();
		if (!blockReason.isEmpty()) {
			if (errorOut) {
				*errorOut = sanitizeAgentErrorMessage(
					QStringLiteral("Gemini request blocked by policy: %1").arg(blockReason));
			}
			return false;
		}
		return true;
	}

	const QJsonObject first = candidates.first().toObject();
	const QString finishReason = first.value(QStringLiteral("finishReason")).toString().trimmed();
	if (outFinishReason && !finishReason.isEmpty()) {
		*outFinishReason = finishReason;
	}

	bool emittedTextOrTool = false;
	const QJsonArray parts = first.value(QStringLiteral("content"))
		.toObject()
		.value(QStringLiteral("parts"))
		.toArray();
	for (const auto &partVal : parts) {
		if (!partVal.isObject()) {
			continue;
		}
		const QJsonObject part = partVal.toObject();
		QString thoughtSig = part.value(QStringLiteral("thoughtSignature")).toString();
		if (thoughtSig.isEmpty()) {
			thoughtSig = part.value(QStringLiteral("thought_signature")).toString();
		}

		if (part.contains(QStringLiteral("functionCall")) && part.value(QStringLiteral("functionCall")).isObject()) {
			const QJsonObject fc = part.value(QStringLiteral("functionCall")).toObject();
			AgentToolCall tc;
			tc.providerCallId = fc.value(QStringLiteral("id")).toString();
			tc.id = tc.providerCallId;
			tc.name = fc.value(QStringLiteral("name")).toString();
			const QJsonValue argsVal = fc.value(QStringLiteral("args"));
			tc.argumentsJson = argsVal.isObject()
				? QString::fromUtf8(QJsonDocument(argsVal.toObject()).toJson(QJsonDocument::Compact))
				: QStringLiteral("{}");
			tc.thoughtSignature = thoughtSig;
			if (!tc.name.isEmpty()) {
				emittedTextOrTool = true;
				if (outToolCalls) {
					outToolCalls->push_back(std::move(tc));
				}
			}
			continue;
		}

		if (part.contains(QStringLiteral("text"))) {
			const QString text = part.value(QStringLiteral("text")).toString();
			const bool isThought = part.value(QStringLiteral("thought")).toBool(false);
			if (isThought) {
				if (outReasoning && !text.isEmpty()) {
					outReasoning->append(text);
				}
			} else if (!text.isEmpty()) {
				emittedTextOrTool = true;
				if (outText) {
					outText->append(text);
				}
			}
			if (!thoughtSig.isEmpty() && outThinkingBlocks) {
				outThinkingBlocks->push_back(AgentThinkingBlock{
					QStringLiteral("gemini_thought"),
					{},
					thoughtSig,
				});
			}
			continue;
		}

		if (!thoughtSig.isEmpty() && outThinkingBlocks) {
			outThinkingBlocks->push_back(AgentThinkingBlock{
				QStringLiteral("gemini_thought"),
				{},
				thoughtSig,
			});
		}
	}

	if (!emittedTextOrTool &&
		(finishReason == QStringLiteral("SAFETY") ||
		 finishReason == QStringLiteral("PROHIBITED_CONTENT") ||
		 finishReason == QStringLiteral("BLOCKLIST") ||
		 finishReason == QStringLiteral("SPII") ||
		 finishReason == QStringLiteral("MALFORMED_FUNCTION_CALL"))) {
		if (errorOut) {
			*errorOut = sanitizeAgentErrorMessage(
				QStringLiteral("Gemini stopped generation (%1).").arg(finishReason));
		}
		return false;
	}
	return true;
}

bool parseGeminiSseDelta(
		const SseEvent &,
		const QJsonObject &obj,
		const QByteArray &,
		AgentStreamDelta &outDelta,
		QString &errorOut,
		SseParseState &) {
	return parseGeminiResponseObject(
		obj,
		&outDelta.textDelta,
		&outDelta.reasoningDelta,
		&outDelta.toolCalls,
		&outDelta.thinkingBlocks,
		&outDelta.finishReason,
		&errorOut);
}

const ProtocolDialectSpec &dialectSpec(AgentProtocolDialect dialect) {
	static const ProtocolDialectSpec kOpenAiChat{
		&resolveOpenAiChatEndpointUrl,
		&resolveOpenAiModelsUrl,
		&applyBearerAuthHeaders,
		&buildOpenAiChatOrWebhookPayload,
		&parseOpenAiChatCompletionsSseDelta,
	};
	static const ProtocolDialectSpec kOpenAiResponses{
		&resolveOpenAiResponsesEndpointUrl,
		&resolveOpenAiModelsUrl,
		&applyBearerAuthHeaders,
		&buildOpenAiResponsesPayload,
		&parseOpenAiResponsesSseDelta,
	};
	static const ProtocolDialectSpec kWebhook{
		&resolveWebhookEndpointUrl,
		&resolveOpenAiModelsUrl,
		&applyBearerAuthHeaders,
		&buildOpenAiChatOrWebhookPayload,
		&parseOpenAiChatCompletionsSseDelta,
	};
	static const ProtocolDialectSpec kAnthropicMessages{
		&resolveAnthropicMessagesEndpointUrl,
		&resolveAnthropicModelsUrl,
		&applyAnthropicAuthHeaders,
		&buildAnthropicMessagesPayload,
		&parseAnthropicMessagesSseDelta,
	};
	static const ProtocolDialectSpec kGoogleGemini{
		&resolveGeminiEndpointUrl,
		&resolveGeminiModelsUrl,
		&applyGeminiAuthHeaders,
		&buildGeminiPayload,
		&parseGeminiSseDelta,
	};

	switch (dialect) {
	case AgentProtocolDialect::OpenAiResponsesSse:
		return kOpenAiResponses;
	case AgentProtocolDialect::CustomWebhookJson:
		return kWebhook;
	case AgentProtocolDialect::AnthropicMessagesSse:
		return kAnthropicMessages;
	case AgentProtocolDialect::GoogleGeminiSse:
		return kGoogleGemini;
	case AgentProtocolDialect::OpenAiChatCompletionsSse:
	default:
		return kOpenAiChat;
	}
}

QUrl resolveEndpointUrl(
		const QUrl &baseUrl,
		const QString &model,
		AgentProtocolDialect dialect,
		bool stream) {
	return dialectSpec(dialect).resolveEndpoint(baseUrl, model, stream);
}

QUrl resolveModelsUrl(const QUrl &baseUrl, AgentProtocolDialect dialect) {
	return dialectSpec(dialect).resolveModels(baseUrl);
}

QByteArray buildRequestPayload(const AgentChatRequest &req, bool includeClientUiGuide = true) {
	const QString combinedSystem = buildCombinedSystemPrompt(req, includeClientUiGuide);
	return dialectSpec(req.profile.protocol).buildPayload(req, combinedSystem);
}

bool parseSsePayloadDelta(
		AgentProtocolDialect dialect,
		const SseEvent &ev,
		AgentStreamDelta &outDelta,
		QString &errorOut,
		SseParseState &state) {
	const QByteArray trimmed = ev.data.trimmed();
	if (trimmed.isEmpty()) {
		return true;
	}
	if (trimmed == "[DONE]") {
		outDelta.isDone = true;
		return true;
	}

	QJsonParseError parseErr{};
	const QJsonDocument doc = QJsonDocument::fromJson(trimmed, &parseErr);
	if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
		// Ignore non-JSON keepalive payloads unless clearly malformed error text.
		return true;
	}
	const QJsonObject obj = doc.object();

	if (obj.contains(QStringLiteral("error")) && !obj.value(QStringLiteral("error")).isNull()) {
		const auto errMsg = extractErrorTextFromJson(trimmed);
		errorOut = sanitizeAgentErrorMessage(errMsg);
		return false;
	}

	if (dialect != AgentProtocolDialect::OpenAiResponsesSse &&
		(ev.event.startsWith(QStringLiteral("response.")) ||
		 obj.value(QStringLiteral("type")).toString().startsWith(QStringLiteral("response.")))) {
		return parseOpenAiResponsesSseDelta(ev, obj, trimmed, outDelta, errorOut, state);
	}

	return dialectSpec(dialect).parseSseDelta(ev, obj, trimmed, outDelta, errorOut, state);
}

class ThinkTagStreamFilter final {
public:
	void feed(const QString &input, QString &outText, QString &outReasoning) {
		if (input.isEmpty()) {
			return;
		}
		if (_buf.isEmpty() && !_inThink && !input.contains(QLatin1Char('<'))) {
			outText.append(input);
			return;
		}
		_buf.append(input);
		static const QString kOpen = QStringLiteral("<think>");
		static const QString kClose = QStringLiteral("</think>");
		while (!_buf.isEmpty()) {
			const QString &tag = _inThink ? kClose : kOpen;
			const int pos = _buf.indexOf(tag);
			if (pos >= 0) {
				QString &dest = _inThink ? outReasoning : outText;
				dest.append(_buf.left(pos));
				_buf.remove(0, pos + tag.size());
				_inThink = !_inThink;
				continue;
			}
			const int maxPrefix = std::min<int>(tag.size() - 1, _buf.size());
			int keepSuffix = 0;
			for (int len = maxPrefix; len >= 1; --len) {
				if (_buf.endsWith(QStringView(tag).left(len))) {
					keepSuffix = len;
					break;
				}
			}
			const int emitLen = _buf.size() - keepSuffix;
			if (emitLen > 0) {
				QString &dest = _inThink ? outReasoning : outText;
				dest.append(_buf.left(emitLen));
				_buf.remove(0, emitLen);
			}
			break;
		}
	}

	void flush(QString &outText, QString &outReasoning) {
		if (_buf.isEmpty()) {
			return;
		}
		QString &dest = _inThink ? outReasoning : outText;
		dest.append(_buf);
		_buf.clear();
	}

private:
	QString _buf;
	bool _inThink = false;
};

QString extractNonStreamingJsonText(
		const QByteArray &payload,
		QString *errorOut = nullptr,
		QVector<AgentToolCall> *outToolCalls = nullptr,
		QString *outReasoning = nullptr,
		QVector<AgentThinkingBlock> *outThinkingBlocks = nullptr) {
	QJsonParseError err{};
	const auto doc = QJsonDocument::fromJson(payload, &err);
	if (err.error != QJsonParseError::NoError || !doc.isObject()) {
		if (errorOut) {
			*errorOut = QStringLiteral("Invalid JSON response from agent endpoint.");
		}
		return {};
	}
	const QJsonObject root = doc.object();
	if (root.contains(QStringLiteral("error")) && !root.value(QStringLiteral("error")).isNull()) {
		if (errorOut) {
			*errorOut = sanitizeAgentErrorMessage(extractErrorTextFromJson(payload));
		}
		return {};
	}

	const auto choices = root.value(QStringLiteral("choices")).toArray();
	if (!choices.isEmpty()) {
		const QJsonObject first = choices.first().toObject();
		if (first.value(QStringLiteral("message")).isObject()) {
			const QJsonObject msgObj = first.value(QStringLiteral("message")).toObject();
			if (outReasoning) {
				QString r = msgObj.value(QStringLiteral("reasoning_content")).toString();
				if (r.isEmpty()) {
					r = msgObj.value(QStringLiteral("reasoning")).toString();
				}
				*outReasoning = r;
			}
			if (outToolCalls && msgObj.value(QStringLiteral("tool_calls")).isArray()) {
				QMap<int, AgentToolCall> acc;
				accumulateToolCalls(msgObj.value(QStringLiteral("tool_calls")).toArray(), acc);
				for (auto it = acc.cbegin(); it != acc.cend(); ++it) {
					if (!it.value().name.isEmpty()) {
						outToolCalls->push_back(it.value());
					}
				}
			}
			const QString content = extractTextFromContentValue(msgObj.value(QStringLiteral("content")));
			if (!content.isEmpty()) {
				return content;
			}
		}
		const QString text = first.value(QStringLiteral("text")).toString();
		if (!text.isEmpty()) {
			return text;
		}
	}

	if (const QString outputText = root.value(QStringLiteral("output_text")).toString(); !outputText.isEmpty()) {
		return outputText;
	}

	const auto outputArr = root.value(QStringLiteral("output")).toArray();
	if (!outputArr.isEmpty()) {
		QString combinedText;
		for (const auto &outVal : outputArr) {
			const QJsonObject outObj = outVal.toObject();
			const QString outType = outObj.value(QStringLiteral("type")).toString();
			if (outType == QStringLiteral("function_call")) {
				if (outToolCalls) {
					AgentToolCall tc;
					tc.id = outObj.value(QStringLiteral("call_id")).toString();
					if (tc.id.isEmpty()) {
						tc.id = outObj.value(QStringLiteral("id")).toString();
					}
					tc.name = outObj.value(QStringLiteral("name")).toString();
					tc.argumentsJson = outObj.value(QStringLiteral("arguments")).toString();
					if (!tc.name.isEmpty()) {
						outToolCalls->push_back(std::move(tc));
					}
				}
				continue;
			}
			const auto contentArr = outObj.value(QStringLiteral("content")).toArray();
			for (const auto &cVal : contentArr) {
				const QString text = cVal.toObject().value(QStringLiteral("text")).toString();
				if (!text.isEmpty()) {
					combinedText.append(text);
				}
			}
		}
		if (!combinedText.isEmpty() || (outToolCalls && !outToolCalls->isEmpty())) {
			return combinedText;
		}
	}

	const auto anthropicContent = root.value(QStringLiteral("content")).toArray();
	if (!anthropicContent.isEmpty() &&
		(root.value(QStringLiteral("type")).toString() == QStringLiteral("message") ||
		 root.value(QStringLiteral("role")).toString() == QStringLiteral("assistant"))) {
		QString combinedText;
		for (const auto &cVal : anthropicContent) {
			if (!cVal.isObject()) {
				continue;
			}
			const QJsonObject block = cVal.toObject();
			const QString bType = block.value(QStringLiteral("type")).toString();
			if (bType == QStringLiteral("text")) {
				combinedText.append(block.value(QStringLiteral("text")).toString());
			} else if (bType == QStringLiteral("thinking")) {
				const QString t = block.value(QStringLiteral("thinking")).toString();
				const QString sig = block.value(QStringLiteral("signature")).toString();
				if (outReasoning && !t.isEmpty()) {
					if (!outReasoning->isEmpty()) {
						outReasoning->append(QLatin1Char('\n'));
					}
					outReasoning->append(t);
				}
				if (outThinkingBlocks) {
					AgentThinkingBlock tb{QStringLiteral("thinking"), t, sig};
					if (isValidSignedAnthropicThinkingBlock(tb)) {
						outThinkingBlocks->push_back(std::move(tb));
					}
				}
			} else if (bType == QStringLiteral("redacted_thinking")) {
				if (outThinkingBlocks) {
					AgentThinkingBlock tb{
						QStringLiteral("redacted_thinking"),
						block.value(QStringLiteral("data")).toString(),
						{},
					};
					if (isValidSignedAnthropicThinkingBlock(tb)) {
						outThinkingBlocks->push_back(std::move(tb));
					}
				}
			} else if (bType == QStringLiteral("tool_use")) {
				if (outToolCalls) {
					AgentToolCall tc;
					tc.id = block.value(QStringLiteral("id")).toString();
					tc.name = block.value(QStringLiteral("name")).toString();
					const QJsonValue inputVal = block.value(QStringLiteral("input"));
					tc.argumentsJson = inputVal.isObject()
						? QString::fromUtf8(QJsonDocument(inputVal.toObject()).toJson(QJsonDocument::Compact))
						: QStringLiteral("{}");
					if (!tc.name.isEmpty()) {
						outToolCalls->push_back(std::move(tc));
					}
				}
			}
		}
		if (!combinedText.isEmpty() ||
			(outToolCalls && !outToolCalls->isEmpty()) ||
			(outThinkingBlocks && !outThinkingBlocks->isEmpty())) {
			return combinedText;
		}
	}

	if (root.contains(QStringLiteral("candidates")) || root.contains(QStringLiteral("promptFeedback"))) {
		QString combinedText;
		QString finishReason;
		QString gemErr;
		if (!parseGeminiResponseObject(
				root,
				&combinedText,
				outReasoning,
				outToolCalls,
				outThinkingBlocks,
				&finishReason,
				&gemErr)) {
			if (errorOut) {
				*errorOut = gemErr;
			}
			return {};
		}
		if (!combinedText.isEmpty() ||
			(outToolCalls && !outToolCalls->isEmpty()) ||
			(outThinkingBlocks && !outThinkingBlocks->isEmpty())) {
			return combinedText;
		}
	}

	if (root.value(QStringLiteral("message")).isObject()) {
		const QString content = root.value(QStringLiteral("message")).toObject()
			.value(QStringLiteral("content")).toString();
		if (!content.isEmpty()) {
			return content;
		}
	}

	for (const char *key : {"reply", "response", "content", "text"}) {
		const QString val = root.value(QLatin1String(key)).toString();
		if (!val.isEmpty()) {
			return val;
		}
	}
	return {};
}

QNetworkRequest buildAuthorizedNetworkRequest(
		const QUrl &endpoint,
		const AgentProfile &profile,
		const QString &apiKey,
		bool stream) {
	QNetworkRequest netReq(endpoint);
	netReq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
	netReq.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json; charset=utf-8"));
	netReq.setRawHeader(
		"Accept",
		stream ? "text/event-stream, application/json" : "application/json, text/event-stream");
	netReq.setRawHeader("Cache-Control", "no-cache");

	if (endpoint.scheme() == QStringLiteral("https")) {
		QSslConfiguration sslConfig = QSslConfiguration::defaultConfiguration();
		sslConfig.setPeerVerifyMode(QSslSocket::VerifyPeer);
		netReq.setSslConfiguration(sslConfig);
	}

	dialectSpec(profile.protocol).applyAuthHeaders(netReq, profile, apiKey);
	return netReq;
}

class NetworkAgentRequestHandle final : public AgentRequestHandle {
public:
	NetworkAgentRequestHandle(
		QNetworkReply *reply,
		AgentProtocolDialect dialect,
		bool expectStream,
		bool probeOnly,
		int connectTimeoutMs,
		int idleTimeoutMs,
		QObject *parent = nullptr,
		AgentKeyEndpointMismatch keyMismatch = AgentKeyEndpointMismatch::None)
		: AgentRequestHandle(parent)
		, _reply(reply)
		, _dialect(dialect)
		, _expectStream(expectStream)
		, _probeOnly(probeOnly)
		, _keyMismatch(keyMismatch) {
		if (_reply) {
			_reply->setParent(this);
		}

		_connectTimer.setSingleShot(true);
		_connectTimer.setInterval(qBound(2000, connectTimeoutMs, 60000));
		connect(&_connectTimer, &QTimer::timeout, this, &NetworkAgentRequestHandle::onConnectTimeout);

		_idleTimer.setSingleShot(true);
		_idleTimer.setInterval(qBound(3000, idleTimeoutMs, 180000));
		connect(&_idleTimer, &QTimer::timeout, this, &NetworkAgentRequestHandle::onIdleTimeout);

		if (!_reply) {
			QTimer::singleShot(0, this, [this] {
				finishOnce(false, QStringLiteral("Failed to create network request."));
			});
			return;
		}

		connect(_reply, &QNetworkReply::metaDataChanged, this, &NetworkAgentRequestHandle::onMetaDataChanged);
		connect(_reply, &QNetworkReply::readyRead, this, &NetworkAgentRequestHandle::onReadyRead);
		connect(_reply, &QNetworkReply::finished, this, &NetworkAgentRequestHandle::onReplyFinished);

		_connectTimer.start();
		_idleTimer.start();
	}

	~NetworkAgentRequestHandle() override {
		_finished = true;
		_connectTimer.stop();
		_idleTimer.stop();
		if (_reply) {
			_reply->disconnect(this);
			if (_reply->isRunning()) {
				_reply->abort();
			}
		}
	}

	void cancel() override {
		if (_finished) {
			return;
		}
		_cancelled = true;
		abortAndFail(QStringLiteral("Request cancelled."));
	}

private:
	void abortAndFail(const QString &safeError) {
		if (_finished) {
			return;
		}
		_finished = true;
		_connectTimer.stop();
		_idleTimer.stop();
		if (_reply) {
			_reply->disconnect(this);
			if (_reply->isRunning()) {
				_reply->abort();
			}
		}
		emit finished(false, safeError);
	}

	void onConnectTimeout() {
		if (_finished || _headersReceived) {
			return;
		}
		abortAndFail(QStringLiteral("Connection to agent endpoint timed out."));
	}

	void onIdleTimeout() {
		if (_finished) {
			return;
		}
		abortAndFail(QStringLiteral("Agent response stream timed out waiting for data."));
	}

	void onMetaDataChanged() {
		if (_finished || !_reply) {
			return;
		}
		_headersReceived = true;
		_connectTimer.stop();
		_idleTimer.start();

		const QString contentType = _reply->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
		const int statusCode = _reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (statusCode >= 300 && statusCode < 400) {
			// ManualRedirectPolicy leaves redirects unfollowed so auth headers never leak.
			abortAndFail(QStringLiteral("HTTP redirects are blocked for security (HTTP %1).").arg(statusCode));
			return;
		}
		if (contentType.contains(QStringLiteral("text/event-stream"))) {
			_isSseResponse = true;
		} else if (!contentType.isEmpty()) {
			_isSseResponse = false;
		} else {
			_isSseResponse = _expectStream;
		}
	}

	void onReadyRead() {
		if (_finished || !_reply) {
			return;
		}
		_headersReceived = true;
		_connectTimer.stop();
		_idleTimer.start();

		const QByteArray chunk = _reply->readAll();
		if (chunk.isEmpty()) {
			return;
		}

		const int statusCode = _reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (statusCode >= 400) {
			if (_errorOrJsonBuffer.size() < 64 * 1024) {
				_errorOrJsonBuffer.append(chunk);
			}
			return;
		}

		if (!_isSseResponse) {
			if (_errorOrJsonBuffer.size() + chunk.size() > kMaxNonStreamingBodyBytes) {
				abortAndFail(QStringLiteral("Agent response exceeded maximum allowed size."));
				return;
			}
			_errorOrJsonBuffer.append(chunk);
			return;
		}

		QVector<SseEvent> events;
		QString frameError;
		if (!_framer.feed(chunk, events, &frameError)) {
			abortAndFail(frameError);
			return;
		}

		QString parseError;
		if (!processSseEvents(events, parseError)) {
			abortAndFail(parseError);
			return;
		}
	}

	bool processSseEvents(const QVector<SseEvent> &events, QString &errorOut) {
		for (const auto &ev : events) {
			AgentStreamDelta delta;
			if (!parseSsePayloadDelta(_dialect, ev, delta, errorOut, _sseState)) {
				return false;
			}
			if (delta.reasoningDelta.isEmpty() && !delta.textDelta.isEmpty()) {
				QString filteredText;
				QString filteredReasoning;
				_thinkFilter.feed(delta.textDelta, filteredText, filteredReasoning);
				delta.textDelta = std::move(filteredText);
				delta.reasoningDelta = std::move(filteredReasoning);
			}
			if (delta.isDone) {
				QString tailText;
				QString tailReasoning;
				_thinkFilter.flush(tailText, tailReasoning);
				if (!tailText.isEmpty()) {
					delta.textDelta.append(tailText);
				}
				if (!tailReasoning.isEmpty()) {
					delta.reasoningDelta.append(tailReasoning);
				}
			}
			if (delta.isDone ||
				delta.finishReason == QStringLiteral("tool_calls") ||
				delta.finishReason == QStringLiteral("tool_use")) {
				flushAccumulatedToolCalls(delta);
			}
			if (!_probeOnly && (!delta.textDelta.isEmpty() || !delta.reasoningDelta.isEmpty() ||
				!delta.thinkingBlocks.isEmpty() || !delta.toolCalls.isEmpty() ||
				delta.isDone || !delta.finishReason.isEmpty())) {
				emit deltaReceived(delta);
			}
			if (delta.isDone) {
				_sawDoneMarker = true;
			}
		}
		return true;
	}

	void flushAccumulatedToolCalls(AgentStreamDelta &delta) {
		if (_sseState.toolCallAcc.isEmpty()) {
			return;
		}
		for (auto it = _sseState.toolCallAcc.begin(); it != _sseState.toolCallAcc.end(); ++it) {
			if (!it.value().name.isEmpty()) {
				if (it.value().argumentsJson.isEmpty()) {
					it.value().argumentsJson = QStringLiteral("{}");
				}
				delta.toolCalls.push_back(it.value());
			}
		}
		_sseState.toolCallAcc.clear();
		_sseState.toolCallHasDelta.clear();
	}

	void onReplyFinished() {
		if (_finished || !_reply) {
			return;
		}
		_connectTimer.stop();
		_idleTimer.stop();

		if (_cancelled) {
			finishOnce(false, QStringLiteral("Request cancelled."));
			return;
		}

		// Drain any final buffered bytes.
		const QByteArray trailing = _reply->readAll();
		const int statusCode = _reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (!trailing.isEmpty()) {
			if (statusCode >= 400 || !_isSseResponse) {
				if (_errorOrJsonBuffer.size() + trailing.size() <= kMaxNonStreamingBodyBytes) {
					_errorOrJsonBuffer.append(trailing);
				}
			} else {
				QVector<SseEvent> events;
				QString frameError;
				if (_framer.feed(trailing, events, &frameError)) {
					_framer.flush(events);
					QString parseError;
					if (!processSseEvents(events, parseError)) {
						finishOnce(false, parseError);
						return;
					}
				}
			}
		} else if (_isSseResponse && statusCode < 400) {
			QVector<SseEvent> events;
			_framer.flush(events);
			if (!events.isEmpty()) {
				QString parseError;
				if (!processSseEvents(events, parseError)) {
					finishOnce(false, parseError);
					return;
				}
			}
		}

		const auto netError = _reply->error();
		if (netError != QNetworkReply::NoError || statusCode >= 400 || statusCode == 0) {
			finishOnce(false, formatReplyErrorMessage(_errorOrJsonBuffer, _reply, statusCode, _keyMismatch));
			return;
		}

		if (!_isSseResponse) {
			QString parseErr;
			QVector<AgentToolCall> nonStreamTools;
			QString nonStreamReasoning;
			QVector<AgentThinkingBlock> nonStreamThinkingBlocks;
			const QString rawText = extractNonStreamingJsonText(
				_errorOrJsonBuffer,
				&parseErr,
				&nonStreamTools,
				&nonStreamReasoning,
				&nonStreamThinkingBlocks);
			if (!parseErr.isEmpty()) {
				finishOnce(false, parseErr);
				return;
			}
			if (!_probeOnly) {
				AgentStreamDelta delta;
				if (nonStreamReasoning.isEmpty() && !rawText.isEmpty()) {
					QString filteredText;
					QString filteredReasoning;
					_thinkFilter.feed(rawText, filteredText, filteredReasoning);
					_thinkFilter.flush(filteredText, filteredReasoning);
					delta.textDelta = std::move(filteredText);
					delta.reasoningDelta = std::move(filteredReasoning);
				} else {
					delta.textDelta = rawText;
					delta.reasoningDelta = std::move(nonStreamReasoning);
				}
				delta.toolCalls = std::move(nonStreamTools);
				delta.thinkingBlocks = std::move(nonStreamThinkingBlocks);
				delta.isDone = true;
				emit deltaReceived(delta);
			}
		} else if (!_sawDoneMarker && !_probeOnly) {
			AgentStreamDelta doneDelta;
			_thinkFilter.flush(doneDelta.textDelta, doneDelta.reasoningDelta);
			flushAccumulatedToolCalls(doneDelta);
			doneDelta.isDone = true;
			emit deltaReceived(doneDelta);
		}

		finishOnce(true, {});
	}

	void finishOnce(bool success, const QString &safeError) {
		if (_finished) {
			return;
		}
		_finished = true;
		_connectTimer.stop();
		_idleTimer.stop();
		emit finished(success, safeError);
	}

	QPointer<QNetworkReply> _reply;
	AgentProtocolDialect _dialect = AgentProtocolDialect::OpenAiChatCompletionsSse;
	AgentKeyEndpointMismatch _keyMismatch = AgentKeyEndpointMismatch::None;
	bool _expectStream = true;
	bool _probeOnly = false;
	bool _headersReceived = false;
	bool _isSseResponse = true;
	bool _sawDoneMarker = false;
	bool _cancelled = false;
	bool _finished = false;
	QTimer _connectTimer;
	QTimer _idleTimer;
	SseEventFramer _framer;
	ThinkTagStreamFilter _thinkFilter;
	QByteArray _errorOrJsonBuffer;
	SseParseState _sseState;
};

class NetworkModelListHandle final : public AgentModelListHandle {
public:
	NetworkModelListHandle(
		QNetworkReply *reply,
		int connectTimeoutMs,
		const QString &endpointHost = QString(),
		QObject *parent = nullptr,
		AgentKeyEndpointMismatch keyMismatch = AgentKeyEndpointMismatch::None)
		: AgentModelListHandle(parent)
		, _reply(reply)
		, _endpointHost(endpointHost)
		, _keyMismatch(keyMismatch) {
		_timeoutTimer.setSingleShot(true);
		_timeoutTimer.setInterval(qBound(2000, connectTimeoutMs, 60000));
		connect(&_timeoutTimer, &QTimer::timeout, this, &NetworkModelListHandle::onTimeout);

		if (_reply) {
			connect(_reply, &QNetworkReply::readyRead, this, &NetworkModelListHandle::onReadyRead);
			connect(_reply, &QNetworkReply::finished, this, &NetworkModelListHandle::onReplyFinished);
			_timeoutTimer.start();
		}
	}

	~NetworkModelListHandle() override {
		cancel();
	}

	[[nodiscard]] QHash<QString, AgentModelCapabilities> modelCapabilities() const override {
		return _capabilities;
	}

	[[nodiscard]] QStringList speechModels() const override {
		return _speechModels;
	}

	[[nodiscard]] int filteredNonChatCount() const override {
		return _filteredNonChatCount;
	}

	void cancel() override {
		if (_finished) {
			return;
		}
		_cancelled = true;
		abortAndFail(QStringLiteral("Request cancelled."));
	}

private:
	void abortAndFail(const QString &safeError) {
		if (_finished) {
			return;
		}
		_finished = true;
		_timeoutTimer.stop();
		if (_reply) {
			_reply->disconnect(this);
			if (_reply->isRunning()) {
				_reply->abort();
			}
		}
		emit finished(false, {}, safeError);
	}

	void onTimeout() {
		if (_finished) {
			return;
		}
		abortAndFail(QStringLiteral("Timed out fetching models."));
	}

	void onReadyRead() {
		if (_finished || !_reply) {
			return;
		}
		const QByteArray chunk = _reply->readAll();
		if (!chunk.isEmpty() && _body.size() + chunk.size() <= kMaxNonStreamingBodyBytes) {
			_body.append(chunk);
		}
	}

	void onReplyFinished() {
		if (_finished || !_reply) {
			return;
		}
		_timeoutTimer.stop();
		if (_cancelled) {
			finishOnce(false, {}, QStringLiteral("Request cancelled."));
			return;
		}

		const QByteArray trailing = _reply->readAll();
		if (!trailing.isEmpty() && _body.size() + trailing.size() <= kMaxNonStreamingBodyBytes) {
			_body.append(trailing);
		}

		const int statusCode = _reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (statusCode >= 300 && statusCode < 400) {
			finishOnce(false, {}, QStringLiteral("HTTP redirects are blocked for security (HTTP %1).").arg(statusCode));
			return;
		}
		if (_reply->error() != QNetworkReply::NoError || statusCode >= 400 || statusCode == 0) {
			finishOnce(false, {}, formatReplyErrorMessage(_body, _reply, statusCode, _keyMismatch));
			return;
		}

		QString parseErr;
		_capabilities.clear();
		_speechModels.clear();
		_filteredNonChatCount = 0;
		const QStringList models = extractModelsFromJson(
			_body, &parseErr, &_capabilities, &_filteredNonChatCount, &_speechModels, _endpointHost);
		if (!parseErr.isEmpty()) {
			_capabilities.clear();
			_speechModels.clear();
			_filteredNonChatCount = 0;
			finishOnce(false, {}, parseErr);
			return;
		}
		finishOnce(true, models, {});
	}

	void finishOnce(bool success, const QStringList &models, const QString &safeError) {
		if (_finished) {
			return;
		}
		_finished = true;
		_timeoutTimer.stop();
		emit finished(success, models, safeError);
	}

	QPointer<QNetworkReply> _reply;
	QString _endpointHost;
	AgentKeyEndpointMismatch _keyMismatch = AgentKeyEndpointMismatch::None;
	QTimer _timeoutTimer;
	QByteArray _body;
	QHash<QString, AgentModelCapabilities> _capabilities;
	QStringList _speechModels;
	int _filteredNonChatCount = 0;
	bool _cancelled = false;
	bool _finished = false;
};

} // namespace

bool SseEventFramer::feed(const QByteArray &chunk, QVector<SseEvent> &outEvents, QString *errorOut) {
	if (chunk.isEmpty()) {
		return true;
	}

	int startOffset = 0;
	if (_pendingCr) {
		_pendingCr = false;
		if (chunk.at(0) == '\n') {
			startOffset = 1;
		}
	}

	if (startOffset < chunk.size()) {
		if (_buffer.size() + (chunk.size() - startOffset) > kMaxBufferBytes) {
			if (errorOut) {
				*errorOut = QStringLiteral("SSE frame buffer exceeded maximum allowed size.");
			}
			return false;
		}
		_buffer.append(chunk.constData() + startOffset, chunk.size() - startOffset);
	}

	// Strip UTF-8 BOM if present at beginning of stream.
	if (_buffer.startsWith("\xEF\xBB\xBF") && !_hasData && _currentEvent.isEmpty() && _currentId.isEmpty()) {
		_buffer.remove(0, 3);
	}

	bool overflow = false;
	int lineStart = 0;
	const int total = _buffer.size();
	int i = 0;
	while (i < total) {
		const char ch = _buffer.at(i);
		if (ch == '\r' || ch == '\n') {
			const QByteArray line = _buffer.mid(lineStart, i - lineStart);
			processLine(line, outEvents, errorOut, overflow);
			if (overflow) {
				return false;
			}
			if (ch == '\r') {
				if (i + 1 < total) {
					if (_buffer.at(i + 1) == '\n') {
						++i;
					}
				} else {
					_pendingCr = true;
				}
			}
			lineStart = i + 1;
		}
		++i;
	}

	if (lineStart > 0) {
		_buffer.remove(0, lineStart);
	}
	return true;
}

void SseEventFramer::flush(QVector<SseEvent> &outEvents) {
	if (!_buffer.isEmpty()) {
		bool overflow = false;
		processLine(_buffer, outEvents, nullptr, overflow);
		_buffer.clear();
		_pendingCr = false;
	}
	flushCurrentEvent(outEvents);
}

void SseEventFramer::reset() {
	_buffer.clear();
	_currentEvent.clear();
	_currentData.clear();
	_currentId.clear();
	_hasData = false;
	_pendingCr = false;
}

void SseEventFramer::processLine(
		const QByteArray &line,
		QVector<SseEvent> &outEvents,
		QString *errorOut,
		bool &overflow) {
	if (line.isEmpty()) {
		flushCurrentEvent(outEvents);
		return;
	}
	if (line.at(0) == ':') {
		return;
	}

	const int colon = line.indexOf(':');
	const QByteArray field = colon >= 0 ? line.left(colon) : line;
	QByteArray value = colon >= 0 ? line.mid(colon + 1) : QByteArray();
	if (!value.isEmpty() && value.at(0) == ' ') {
		value.remove(0, 1);
	}

	if (field == "data") {
		const qsizetype extra = (_hasData ? 1 : 0) + value.size();
		if (_currentData.size() + extra > kMaxEventBytes) {
			overflow = true;
			if (errorOut) {
				*errorOut = QStringLiteral("SSE event payload exceeded maximum allowed size.");
			}
			return;
		}
		if (_hasData) {
			_currentData.append('\n');
		}
		_currentData.append(value);
		_hasData = true;
	} else if (field == "event") {
		_currentEvent = QString::fromUtf8(value);
	} else if (field == "id") {
		if (!value.contains('\0')) {
			_currentId = QString::fromUtf8(value);
		}
	}
}

void SseEventFramer::flushCurrentEvent(QVector<SseEvent> &outEvents) {
	if (!_hasData) {
		_currentEvent.clear();
		_currentData.clear();
		return;
	}
	SseEvent ev;
	ev.event = _currentEvent;
	ev.data = _currentData;
	ev.id = _currentId;
	outEvents.push_back(std::move(ev));
	_currentEvent.clear();
	_currentData.clear();
	_hasData = false;
}

OpenAiCompatibleTransport::OpenAiCompatibleTransport() {
	_remoteNam = std::make_unique<QNetworkAccessManager>();
	_loopbackNam = std::make_unique<QNetworkAccessManager>();
	_loopbackNam->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
}

QNetworkAccessManager &OpenAiCompatibleTransport::networkManagerFor(bool loopback) {
	return loopback ? *_loopbackNam : *_remoteNam;
}

AgentRequestHandle *OpenAiCompatibleTransport::sendChat(const AgentChatRequest &request, QObject *parent) {
	const AgentEndpointDecision decision = evaluateAgentEndpoint(request.profile.baseUrl);
	if (!decision.allowed) {
		qWarning() << "[Agent] Blocked request endpoint:" << decision.reason;
		auto *handle = new NetworkAgentRequestHandle(
			nullptr, request.profile.protocol, request.stream, false,
			request.profile.connectTimeoutMs, request.profile.idleTimeoutMs, parent);
		QTimer::singleShot(0, handle, [handle, reason = decision.reason] {
			emit handle->finished(false, reason);
		});
		return handle;
	}

	if (request.profile.model.trimmed().isEmpty()) {
		auto *handle = new NetworkAgentRequestHandle(
			nullptr, request.profile.protocol, request.stream, false,
			request.profile.connectTimeoutMs, request.profile.idleTimeoutMs, parent);
		QTimer::singleShot(0, handle, [handle] {
			emit handle->finished(false, QStringLiteral("Model name is required."));
		});
		return handle;
	}

	const QUrl endpoint = resolveEndpointUrl(
		decision.normalizedUrl, request.profile.model, request.profile.protocol, request.stream);
	const QNetworkRequest netReq = buildAuthorizedNetworkRequest(
		endpoint, request.profile, request.apiKey, request.stream);

	const auto endpointBytes = endpoint.toString(QUrl::FullyEncoded).toUtf8();
	const auto safeEndpoint = livekit::secure_log::EndpointSummary(
		std::string_view(endpointBytes.constData(), static_cast<size_t>(endpointBytes.size())));
	int imageCount = 0;
	qint64 totalImageBytes = 0;
	for (const auto &msg : request.messages) {
		for (const auto &img : msg.images) {
			if (img.isValid()) {
				++imageCount;
				totalImageBytes += img.encodedBytes.size();
			}
		}
	}
	qInfo() << "[Agent] Sending chat request endpoint="
		<< QString::fromStdString(safeEndpoint)
		<< "protocol=" << static_cast<int>(request.profile.protocol)
		<< "stream=" << request.stream
		<< "imageCount=" << imageCount
		<< "totalImageBytes=" << totalImageBytes;

	const QByteArray payload = buildRequestPayload(request);
	const bool loopback = isAgentLoopbackHost(endpoint.host());
	const AgentKeyEndpointMismatch keyMismatch = evaluateKeyEndpointCompatibility(request.apiKey, endpoint.host());
	QNetworkReply *reply = networkManagerFor(loopback).post(netReq, payload);

	return new NetworkAgentRequestHandle(
		reply,
		request.profile.protocol,
		request.stream,
		false,
		request.profile.connectTimeoutMs,
		request.profile.idleTimeoutMs,
		parent,
		keyMismatch);
}

AgentRequestHandle *OpenAiCompatibleTransport::probe(
		const AgentProfile &profile,
		const QString &apiKey,
		QObject *parent) {
	const AgentEndpointDecision decision = evaluateAgentEndpoint(profile.baseUrl);
	if (!decision.allowed) {
		auto *handle = new NetworkAgentRequestHandle(
			nullptr, profile.protocol, false, true,
			profile.connectTimeoutMs, profile.idleTimeoutMs, parent);
		QTimer::singleShot(0, handle, [handle, reason = decision.reason] {
			emit handle->finished(false, reason);
		});
		return handle;
	}
	if (profile.model.trimmed().isEmpty()) {
		auto *handle = new NetworkAgentRequestHandle(
			nullptr, profile.protocol, false, true,
			profile.connectTimeoutMs, profile.idleTimeoutMs, parent);
		QTimer::singleShot(0, handle, [handle] {
			emit handle->finished(false, QStringLiteral("Model name is required."));
		});
		return handle;
	}

	AgentChatRequest req;
	req.profile = profile;
	req.profile.systemPrompt.clear();
	req.apiKey = apiKey;
	req.stream = false;
	AgentChatMessage msg;
	msg.id = QStringLiteral("probe");
	msg.role = AgentChatMessage::Role::User;
	msg.content = QStringLiteral("Reply with OK.");
	req.messages.push_back(std::move(msg));

	const QUrl endpoint = resolveEndpointUrl(
		decision.normalizedUrl, profile.model, profile.protocol, false);
	const QNetworkRequest netReq = buildAuthorizedNetworkRequest(
		endpoint, profile, apiKey, false);

	const bool loopback = isAgentLoopbackHost(endpoint.host());
	const AgentKeyEndpointMismatch keyMismatch = evaluateKeyEndpointCompatibility(apiKey, endpoint.host());
	QNetworkReply *reply = networkManagerFor(loopback).post(netReq, buildRequestPayload(req, false));
	return new NetworkAgentRequestHandle(
		reply,
		profile.protocol,
		false,
		true,
		profile.connectTimeoutMs,
		profile.idleTimeoutMs,
		parent,
		keyMismatch);
}

AgentModelListHandle *OpenAiCompatibleTransport::listModels(
		const AgentProfile &profile,
		const QString &apiKey,
		QObject *parent) {
	const AgentEndpointDecision decision = evaluateAgentEndpoint(profile.baseUrl);
	if (!decision.allowed) {
		auto *handle = new NetworkModelListHandle(nullptr, profile.connectTimeoutMs, QString(), parent);
		QTimer::singleShot(0, handle, [handle, reason = decision.reason] {
			emit handle->finished(false, {}, reason);
		});
		return handle;
	}

	const QUrl endpoint = resolveModelsUrl(decision.normalizedUrl, profile.protocol);
	QNetworkRequest netReq = buildAuthorizedNetworkRequest(endpoint, profile, apiKey, false);
	netReq.setRawHeader("Accept", "application/json");
	const bool loopback = isAgentLoopbackHost(endpoint.host());
	const AgentKeyEndpointMismatch keyMismatch = evaluateKeyEndpointCompatibility(apiKey, endpoint.host());
	QNetworkReply *reply = networkManagerFor(loopback).get(netReq);
	return new NetworkModelListHandle(reply, profile.connectTimeoutMs, endpoint.host(), parent, keyMismatch);
}

} // namespace OpenMeeting
