#include "src/agent/agent_speech_service.h"

#include "src/agent/agent_coordinator.h"
#include "src/core/meeting_coordinator.h"
#include "src/media/audio_vad.h"
#include "src/telemetry/log_redaction.h"

#ifdef _WIN32
#include "src/media/wasapi_capture.h"
#include "src/rtc/audio_frame.h"
#include "src/rtc/audio_source.h"
#endif

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QTimer>
#include <QtCore/QUrlQuery>
#include <QtCore/QUuid>
#include <QtNetwork/QHttpMultiPart>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QSslConfiguration>
#include <QtNetwork/QSslSocket>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>

namespace OpenMeeting {
namespace {

constexpr qsizetype kMaxSpeechResponseBytes = 1024 * 1024;
constexpr qsizetype kMaxSpeechAudioUploadBytes = 25 * 1024 * 1024;
constexpr qint64 kWaveformLevelEmitIntervalMs = 33;

bool isModelNotActivatedError(const QString &detail) {
	const QString lower = detail.trimmed().toLower();
	return lower.contains(QStringLiteral("has not activated the model")) ||
		lower.contains(QStringLiteral("not activated")) ||
		lower.contains(QStringLiteral("activate the model service")) ||
		lower.contains(QStringLiteral("modelnotopen")) ||
		lower.contains(QStringLiteral("model_not_open"));
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

QString normalizeMultipartIso639Language(const QString &raw) {
	QString token = raw.trimmed().toLower();
	if (token.isEmpty() || token == QStringLiteral("auto")) {
		return {};
	}
	const int dashIdx = token.indexOf(QLatin1Char('-'));
	const int underIdx = token.indexOf(QLatin1Char('_'));
	int splitIdx = -1;
	if (dashIdx >= 0 && underIdx >= 0) {
		splitIdx = std::min(dashIdx, underIdx);
	} else {
		splitIdx = std::max(dashIdx, underIdx);
	}
	if (splitIdx >= 0) {
		token = token.left(splitIdx).trimmed();
	}
	if (token.isEmpty() || token == QStringLiteral("auto")) {
		return {};
	}
	return token;
}

bool isXaiSttModelId(const QString &modelId) {
	QString normalized = modelId.trimmed().toLower();
	const int slash = normalized.lastIndexOf(QLatin1Char('/'));
	if (slash >= 0 && slash + 1 < normalized.size()) {
		normalized = normalized.mid(slash + 1);
	}
	return normalized == QStringLiteral("grok-stt") ||
		normalized.startsWith(QStringLiteral("grok-stt-")) ||
		normalized.startsWith(QStringLiteral("grok-voice-transcribe"));
}

bool isXaiSttMultipartTarget(
		const QString &endpointHost,
		const QString &endpointPath,
		const QString &modelId) {
	if (isXaiAgentHost(endpointHost) || isXaiSttModelId(modelId)) {
		return true;
	}
	QString cleanPath = endpointPath.trimmed().toLower();
	while (cleanPath.size() > 1 && cleanPath.endsWith(QLatin1Char('/'))) {
		cleanPath.chop(1);
	}
	return cleanPath.endsWith(QStringLiteral("/v1/stt")) ||
		cleanPath == QStringLiteral("/stt");
}

bool isElevenLabsSttMultipartTarget(
		const QString &endpointHost,
		const QString &endpointPath,
		const QString &modelId) {
	if (isElevenLabsAgentHost(endpointHost) || isElevenLabsScribeBatchModelId(modelId)) {
		return true;
	}
	QString cleanPath = endpointPath.trimmed().toLower();
	while (cleanPath.size() > 1 && cleanPath.endsWith(QLatin1Char('/'))) {
		cleanPath.chop(1);
	}
	return cleanPath.endsWith(QStringLiteral("/speech-to-text"));
}

QString stripSenseVoiceControlTags(const QString &text) {
	if (!text.contains(QStringLiteral("<|"))) {
		return text.trimmed();
	}
	QString out;
	out.reserve(text.size());
	int i = 0;
	const int n = text.size();
	while (i < n) {
		if (i + 3 < n && text.at(i) == QLatin1Char('<') && text.at(i + 1) == QLatin1Char('|')) {
			const int closeIdx = text.indexOf(QStringLiteral("|>"), i + 2);
			const int tokenLen = (closeIdx >= 0) ? (closeIdx - (i + 2)) : -1;
			if (tokenLen >= 1 && tokenLen <= 24) {
				bool validTag = true;
				for (int k = i + 2; k < closeIdx; ++k) {
					const ushort uc = text.at(k).unicode();
					const bool isAsciiAlnum = (uc >= 'a' && uc <= 'z') ||
						(uc >= 'A' && uc <= 'Z') ||
						(uc >= '0' && uc <= '9');
					if (!isAsciiAlnum && uc != '_' && uc != '-' && uc != '/') {
						validTag = false;
						break;
					}
				}
				if (validTag) {
					i = closeIdx + 2;
					continue;
				}
			}
		}
		out.append(text.at(i));
		++i;
	}
	return out.trimmed();
}

QString appendSpeechErrorHint(
		const QString &baseMessage,
		int httpStatus,
		AgentKeyEndpointMismatch keyMismatch = AgentKeyEndpointMismatch::None,
		bool usedChatInputAudio = false) {
	if (httpStatus == 401 || httpStatus == 403) {
		QString hint = QStringLiteral("%1\n%2").arg(
			baseMessage,
			QCoreApplication::translate(
				"MeetingUI",
				"Hint: Speech recognition authentication failed. Check the API key and permissions in Copilot Settings."));
		if (keyMismatch == AgentKeyEndpointMismatch::BailianPlanKeyOnStandardHost) {
			hint += QLatin1Char('\n') + QCoreApplication::translate(
				"MeetingUI",
				"Detail: Detected a Bailian Token/Coding Plan key (sk-sp-...) against a standard DashScope endpoint. Speech recognition requires a standard DashScope API key (sk-...) or a matching plan endpoint.");
		} else if (keyMismatch == AgentKeyEndpointMismatch::StandardKeyOnBailianPlanHost) {
			hint += QLatin1Char('\n') + QCoreApplication::translate(
				"MeetingUI",
				"Detail: Detected a standard sk-... key against a Bailian Token/Coding Plan host. Plan endpoints require an sk-sp-... key, and speech ASR models generally require the standard DashScope endpoint (dashscope.aliyuncs.com).");
		} else if (keyMismatch == AgentKeyEndpointMismatch::GroqKeyOnNonGroqHost) {
			hint += QLatin1Char('\n') + QCoreApplication::translate(
				"MeetingUI",
				"Detail: Detected a Groq API key (gsk_...) against a non-Groq speech endpoint. Use a matching provider API key (e.g. sk-... for SiliconFlow/OpenAI/DashScope) or switch the STT endpoint to https://api.groq.com/openai/v1.");
		}
		return hint;
	}
	if (httpStatus == 404 || httpStatus == 400 || httpStatus == 405) {
		if (isModelNotActivatedError(baseMessage)) {
			return QStringLiteral("%1\n%2").arg(
				baseMessage,
				QCoreApplication::translate(
					"MeetingUI",
					"Hint: This model is not activated for your account in this region yet. Please activate it in your provider console (e.g. Volcengine Ark Open Management) or select an activated model/endpoint ID."));
		}
		return QStringLiteral("%1\n%2").arg(
			baseMessage,
			usedChatInputAudio
				? QCoreApplication::translate(
					"MeetingUI",
					"Hint: This endpoint rejected the speech recognition request or does not support the selected ASR model on this route. Try another STT model or configure a dedicated Speech-to-Text Endpoint in Copilot Settings.")
				: QCoreApplication::translate(
					"MeetingUI",
					"Hint: This endpoint may not expose /audio/transcriptions or the selected STT model is unsupported. You can configure a dedicated Speech-to-Text Endpoint/Model in Copilot Settings."));
	}
	return baseMessage;
}

QString sanitizeSpeechError(
		const QString &raw,
		int httpStatus = 0,
		AgentKeyEndpointMismatch keyMismatch = AgentKeyEndpointMismatch::None,
		bool usedChatInputAudio = false) {
	const QString redactedRaw = redactEmbeddedApiKeys(raw.trimmed().left(512));
	const QByteArray utf8 = redactedRaw.toUtf8();
	const auto sanitized = livekit::secure_log::SanitizeForOutput(
		std::string_view(utf8.constData(), static_cast<size_t>(utf8.size())));
	QString clean = QString::fromUtf8(sanitized.data(), static_cast<int>(sanitized.size())).trimmed();
	QString base;
	if (clean.isEmpty()) {
		base = (httpStatus > 0)
			? QStringLiteral("Speech transcription failed (HTTP %1).").arg(httpStatus)
			: QStringLiteral("Speech transcription failed.");
	} else if (httpStatus > 0) {
		base = QStringLiteral("HTTP %1: %2").arg(httpStatus).arg(clean);
	} else {
		base = clean;
	}
	return appendSpeechErrorHint(base, httpStatus, keyMismatch, usedChatInputAudio);
}

QString extractJsonErrorDetail(const QByteArray &payload) {
	QJsonParseError err{};
	const auto doc = QJsonDocument::fromJson(payload, &err);
	if (err.error != QJsonParseError::NoError || !doc.isObject()) {
		return {};
	}
	const QJsonObject root = doc.object();
	const auto errorVal = root.value(QStringLiteral("error"));
	if (errorVal.isString()) {
		return errorVal.toString();
	}
	if (errorVal.isObject()) {
		const QString msg = errorVal.toObject().value(QStringLiteral("message")).toString();
		if (!msg.isEmpty()) {
			return msg;
		}
	}
	const QString msg = root.value(QStringLiteral("message")).toString();
	if (!msg.isEmpty()) {
		return msg;
	}
	const QString detail = root.value(QStringLiteral("detail")).toString();
	if (!detail.isEmpty()) {
		return detail;
	}
	return root.value(QStringLiteral("code")).toString();
}

QString extractChatContentText(const QJsonValue &contentVal) {
	if (contentVal.isString()) {
		return contentVal.toString();
	}
	if (contentVal.isArray()) {
		QStringList parts;
		for (const QJsonValue &partVal : contentVal.toArray()) {
			if (partVal.isString()) {
				parts.push_back(partVal.toString());
			} else if (partVal.isObject()) {
				const QJsonObject partObj = partVal.toObject();
				const QString partType = partObj.value(QStringLiteral("type")).toString();
				if (partType.isEmpty() ||
					partType == QStringLiteral("text") ||
					partType == QStringLiteral("output_text")) {
					const QString t = partObj.value(QStringLiteral("text")).toString();
					if (!t.isEmpty()) {
						parts.push_back(t);
					}
				}
			}
		}
		return parts.join(QString());
	}
	return {};
}

QString extractResponsesOutputArrayText(const QJsonArray &output) {
	QStringList parts;
	for (const QJsonValue &itemVal : output) {
		const QJsonObject itemObj = itemVal.toObject();
		const QString itemType = itemObj.value(QStringLiteral("type")).toString();
		const QJsonValue contentVal = itemObj.value(QStringLiteral("content"));
		if (!itemType.isEmpty() && itemType != QStringLiteral("message") && !contentVal.isArray()) {
			continue;
		}
		const QString piece = extractChatContentText(contentVal);
		if (!piece.isEmpty()) {
			parts.push_back(piece);
		}
	}
	return parts.join(QString()).trimmed();
}

bool isReservedMultipartHeader(const QString &name) {
	const QString lower = name.trimmed().toLower();
	return lower == QStringLiteral("content-type") ||
		lower == QStringLiteral("content-length") ||
		lower == QStringLiteral("authorization") ||
		lower == QStringLiteral("xi-api-key") ||
		lower == QStringLiteral("host") ||
		lower == QStringLiteral("connection") ||
		lower == QStringLiteral("transfer-encoding");
}

void writeLe16(char *dst, uint16_t v) {
	dst[0] = static_cast<char>(v & 0xFFu);
	dst[1] = static_cast<char>((v >> 8) & 0xFFu);
}

void writeLe32(char *dst, uint32_t v) {
	dst[0] = static_cast<char>(v & 0xFFu);
	dst[1] = static_cast<char>((v >> 8) & 0xFFu);
	dst[2] = static_cast<char>((v >> 16) & 0xFFu);
	dst[3] = static_cast<char>((v >> 24) & 0xFFu);
}

class NetworkSpeechRequestHandle final : public AgentSpeechRequestHandle {
public:
	using FallbackFactory = std::function<QNetworkReply *()>;

	explicit NetworkSpeechRequestHandle(
		const QString &immediateError,
		QObject *parent = nullptr)
		: AgentSpeechRequestHandle(parent) {
		const QString msg = immediateError.trimmed().isEmpty()
			? QStringLiteral("Failed to create speech transcription request.")
			: immediateError;
		QTimer::singleShot(0, this, [this, msg] {
			finishOnce(false, {}, msg);
		});
	}

	NetworkSpeechRequestHandle(
		QNetworkReply *reply,
		int timeoutMs,
		QObject *parent = nullptr,
		AgentKeyEndpointMismatch keyMismatch = AgentKeyEndpointMismatch::None,
		bool usedChatInputAudio = false,
		FallbackFactory fallbackFactory = {})
		: AgentSpeechRequestHandle(parent)
		, _reply(reply)
		, _keyMismatch(keyMismatch)
		, _usedChatInputAudio(usedChatInputAudio)
		, _fallbackFactory(std::move(fallbackFactory)) {
		_timer.setSingleShot(true);
		_timer.setInterval(qBound(5000, timeoutMs, 60000));
		connect(&_timer, &QTimer::timeout, this, &NetworkSpeechRequestHandle::onTimeout);

		if (!_reply) {
			QTimer::singleShot(0, this, [this] {
				finishOnce(false, {}, QStringLiteral("Failed to create speech transcription request."));
			});
			return;
		}
		attachReply(_reply);
	}

	~NetworkSpeechRequestHandle() override {
		_finished = true;
		_timer.stop();
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
		_fallbackFactory = {};
		_timer.stop();
		if (_reply) {
			_reply->disconnect(this);
			if (_reply->isRunning()) {
				_reply->abort();
			}
		}
		finishOnce(false, {}, QStringLiteral("Request cancelled."));
	}

private:
	void resetReplyCapture() {
		_body.clear();
		_isSseResponse = false;
		_sseSawDelta = false;
		_sseLineBuffer.clear();
		_sseAccumulatedText.clear();
		_sseFinalText.clear();
		_sseErrorText.clear();
	}

	void handleSseLine(QByteArray line) {
		if (line.endsWith('\r')) {
			line.chop(1);
		}
		if (!line.startsWith("data:")) {
			return;
		}
		const QByteArray payload = line.mid(5).trimmed();
		if (payload.isEmpty() || payload == "[DONE]") {
			return;
		}
		QJsonParseError err{};
		const QJsonDocument doc = QJsonDocument::fromJson(payload, &err);
		if (err.error != QJsonParseError::NoError || !doc.isObject()) {
			return;
		}
		const QJsonObject obj = doc.object();
		const QString type = obj.value(QStringLiteral("type")).toString();
		if (obj.contains(QStringLiteral("error")) && !obj.value(QStringLiteral("error")).isNull()) {
			const QString detail = extractJsonErrorDetail(payload);
			_sseErrorText = detail.isEmpty()
				? QStringLiteral("Speech transcription streaming error.")
				: detail;
			return;
		}
		if (type == QStringLiteral("transcript.text.delta") ||
			type == QStringLiteral("response.output_text.delta")) {
			const QString delta = obj.value(QStringLiteral("delta")).toString();
			if (!delta.isEmpty()) {
				_sseSawDelta = true;
				_sseAccumulatedText += delta;
				emit deltaReceived(_sseAccumulatedText.trimmed());
			}
			return;
		}
		if (type == QStringLiteral("transcript.text.done") ||
			type == QStringLiteral("response.output_text.done")) {
			const QString doneText = obj.value(QStringLiteral("text")).toString().trimmed();
			if (!doneText.isEmpty()) {
				_sseFinalText = doneText;
			}
			return;
		}
		if (type == QStringLiteral("response.completed")) {
			if (_sseFinalText.isEmpty()) {
				const QJsonObject respObj = obj.value(QStringLiteral("response")).toObject();
				const QString outText = respObj.value(QStringLiteral("output_text")).toString().trimmed();
				if (!outText.isEmpty()) {
					_sseFinalText = outText;
				} else if (respObj.value(QStringLiteral("output")).isArray()) {
					const QString arrText = extractResponsesOutputArrayText(
						respObj.value(QStringLiteral("output")).toArray());
					if (!arrText.isEmpty()) {
						_sseFinalText = arrText;
					}
				}
			}
			return;
		}
		if (type == QStringLiteral("response.failed") ||
			type == QStringLiteral("error")) {
			const QJsonObject respErr = obj.value(QStringLiteral("response")).toObject()
				.value(QStringLiteral("error")).toObject();
			QString detail = respErr.value(QStringLiteral("message")).toString().trimmed();
			if (detail.isEmpty()) {
				detail = extractJsonErrorDetail(payload);
			}
			_sseErrorText = detail.isEmpty()
				? QStringLiteral("Speech transcription streaming error.")
				: detail;
			return;
		}
		const QJsonArray choices = obj.value(QStringLiteral("choices")).toArray();
		if (!choices.isEmpty()) {
			const QJsonObject deltaObj = choices.first().toObject().value(QStringLiteral("delta")).toObject();
			const QString chunk = extractChatContentText(deltaObj.value(QStringLiteral("content")));
			if (!chunk.isEmpty()) {
				_sseSawDelta = true;
				_sseAccumulatedText += chunk;
				emit deltaReceived(_sseAccumulatedText.trimmed());
			}
		}
	}

	void processSseChunk(const QByteArray &chunk, bool flushTail) {
		if (!chunk.isEmpty()) {
			if (_sseLineBuffer.size() < kMaxSpeechResponseBytes) {
				const int keep = std::min<int>(
					chunk.size(),
					static_cast<int>(kMaxSpeechResponseBytes) - _sseLineBuffer.size());
				_sseLineBuffer.append(chunk.constData(), keep);
			}
			int nl = -1;
			while ((nl = _sseLineBuffer.indexOf('\n')) >= 0) {
				const QByteArray line = _sseLineBuffer.left(nl);
				_sseLineBuffer.remove(0, nl + 1);
				handleSseLine(line);
			}
		}
		if (flushTail && !_sseLineBuffer.isEmpty()) {
			const QByteArray tail = _sseLineBuffer;
			_sseLineBuffer.clear();
			handleSseLine(tail);
		}
	}

	void attachReply(QNetworkReply *reply) {
		resetReplyCapture();
		_reply = reply;
		_reply->setParent(this);
		connect(_reply, &QNetworkReply::metaDataChanged, this, &NetworkSpeechRequestHandle::onMetaDataChanged);
		connect(_reply, &QNetworkReply::readyRead, this, &NetworkSpeechRequestHandle::onReadyRead);
		connect(_reply, &QNetworkReply::finished, this, &NetworkSpeechRequestHandle::onReplyFinished);
		_timer.start();
	}

	void onTimeout() {
		if (_finished) {
			return;
		}
		_fallbackFactory = {};
		if (_reply) {
			_reply->disconnect(this);
			if (_reply->isRunning()) {
				_reply->abort();
			}
		}
		finishOnce(false, {}, QStringLiteral("Speech transcription request timed out."));
	}

	void onMetaDataChanged() {
		if (_finished || !_reply) {
			return;
		}
		const int statusCode = _reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (statusCode >= 300 && statusCode < 400) {
			_fallbackFactory = {};
			_reply->disconnect(this);
			_reply->abort();
			finishOnce(false, {}, QStringLiteral("HTTP redirects are blocked for security (HTTP %1).").arg(statusCode));
			return;
		}
		const QString contentType = _reply->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
		if ((statusCode == 0 || statusCode < 400) &&
			contentType.contains(QStringLiteral("text/event-stream"))) {
			_isSseResponse = true;
		}
	}

	void onReadyRead() {
		if (_finished || !_reply) {
			return;
		}
		const QByteArray chunk = _reply->readAll();
		if (chunk.isEmpty()) {
			return;
		}
		if (!_isSseResponse && _body.isEmpty()) {
			const int statusCode = _reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
			const QString contentType = _reply->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
			if ((statusCode == 0 || statusCode < 400) &&
				contentType.contains(QStringLiteral("text/event-stream"))) {
				_isSseResponse = true;
			}
		}
		if (_isSseResponse) {
			processSseChunk(chunk, false);
			return;
		}
		if (_body.size() < kMaxSpeechResponseBytes) {
			const int keep = std::min<int>(chunk.size(), static_cast<int>(kMaxSpeechResponseBytes) - _body.size());
			_body.append(chunk.constData(), keep);
		}
	}

	void onReplyFinished() {
		if (_finished || !_reply) {
			return;
		}
		_timer.stop();
		if (_cancelled) {
			finishOnce(false, {}, QStringLiteral("Request cancelled."));
			return;
		}
		const QByteArray trailing = _reply->readAll();
		if (_isSseResponse) {
			processSseChunk(trailing, true);
		} else if (!trailing.isEmpty() && _body.size() < kMaxSpeechResponseBytes) {
			const int keep = std::min<int>(trailing.size(), static_cast<int>(kMaxSpeechResponseBytes) - _body.size());
			_body.append(trailing.constData(), keep);
		}
		const int statusCode = _reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (statusCode >= 300 && statusCode < 400) {
			finishOnce(false, {}, QStringLiteral("HTTP redirects are blocked for security (HTTP %1).").arg(statusCode));
			return;
		}
		if (_reply->error() != QNetworkReply::NoError || statusCode >= 400 || statusCode == 0) {
			const auto extractDetail = [](const QByteArray &body, const QString &replyErr) -> QString {
				const QString jsonDetail = extractJsonErrorDetail(body);
				if (!jsonDetail.isEmpty()) {
					return jsonDetail;
				}
				if (!body.trimmed().isEmpty()) {
					return QString::fromUtf8(body.left(256));
				}
				return replyErr;
			};
			const auto isEmptyErrorBody = [](const QString &detail) -> bool {
				const QString t = detail.trimmed();
				return t.isEmpty() || t == QStringLiteral("{}");
			};
			const QString currentDetail = extractDetail(_body, _reply->errorString());
			if ((statusCode == 400 || statusCode == 404 || statusCode == 405 || statusCode == 422) &&
				!_sseSawDelta && _fallbackFactory && !isModelNotActivatedError(currentDetail)) {
				_primaryStatusCode = statusCode;
				_primaryErrorDetail = currentDetail;
				FallbackFactory nextAttempt = std::move(_fallbackFactory);
				_fallbackFactory = {};
				if (QNetworkReply *fallbackReply = nextAttempt()) {
					QNetworkReply *oldReply = _reply;
					_reply.clear();
					if (oldReply) {
						oldReply->disconnect(this);
						oldReply->deleteLater();
					}
					attachReply(fallbackReply);
					return;
				}
			}
			const bool preferPrimary = isEmptyErrorBody(currentDetail) && !isEmptyErrorBody(_primaryErrorDetail);
			const int reportStatus = preferPrimary ? _primaryStatusCode : statusCode;
			const QString reportDetail = preferPrimary ? _primaryErrorDetail : currentDetail;
			finishOnce(false, {}, sanitizeSpeechError(reportDetail, reportStatus, _keyMismatch, _usedChatInputAudio));
			return;
		}

		if (_isSseResponse) {
			if (!_sseErrorText.isEmpty()) {
				finishOnce(false, {}, sanitizeSpeechError(_sseErrorText));
				return;
			}
			const QString resolved = !_sseFinalText.isEmpty()
				? _sseFinalText
				: _sseAccumulatedText.trimmed();
			finishOnce(true, resolved, {});
			return;
		}

		const QString contentType = _reply->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
		QString parseErr;
		const QString text = OpenAiCompatibleSpeechTransport::parseTranscriptionResponseJson(
			_body, contentType, &parseErr);
		if (!parseErr.isEmpty()) {
			finishOnce(false, {}, sanitizeSpeechError(parseErr));
			return;
		}
		finishOnce(true, text, {});
	}

	void finishOnce(bool success, const QString &text, const QString &safeError) {
		if (_finished) {
			return;
		}
		_finished = true;
		_fallbackFactory = {};
		_timer.stop();
		emit finished(success, text, safeError);
	}

	QPointer<QNetworkReply> _reply;
	AgentKeyEndpointMismatch _keyMismatch = AgentKeyEndpointMismatch::None;
	bool _usedChatInputAudio = false;
	FallbackFactory _fallbackFactory;
	int _primaryStatusCode = 0;
	QString _primaryErrorDetail;
	QTimer _timer;
	QByteArray _body;
	bool _isSseResponse = false;
	bool _sseSawDelta = false;
	QByteArray _sseLineBuffer;
	QString _sseAccumulatedText;
	QString _sseFinalText;
	QString _sseErrorText;
	bool _cancelled = false;
	bool _finished = false;
};

#ifdef _WIN32
class WasapiDictationAudioSource final : public IDictationAudioSource {
public:
	WasapiDictationAudioSource() = default;
	~WasapiDictationAudioSource() override {
		stop();
	}

	bool isPlatformCaptureSupported() const override { return true; }

	bool start(
			const QString &deviceId,
			FrameCallback onFrame,
			StateCallback onStateChanged) override {
		stop();

		auto alive = std::make_shared<std::atomic<bool>>(true);
		auto source = std::make_shared<livekit::AudioSource>(16000, 1);
		auto capture = livekit::WasapiAudioCapture::Create();
		if (!source || !capture) {
			return false;
		}

		auto sub = source->subscribe(
			[alive, cb = std::move(onFrame)](const livekit::AudioFrame &frame) {
				if (!alive->load(std::memory_order_acquire) || !cb) {
					return;
				}
				const auto &pcm = frame.data();
				if (!pcm.empty()) {
					cb(pcm.data(), pcm.size());
				}
			});

		capture->SetCaptureStateCallback(
			[alive, stateCb = std::move(onStateChanged)](bool available) {
				if (!alive->load(std::memory_order_acquire) || !stateCb) {
					return;
				}
				stateCb(
					available,
					available
						? QString()
						: QCoreApplication::translate(
							"MeetingUI",
							"Microphone became unavailable. Check system audio permissions."));
			});

		livekit::WasapiCaptureConfig config;
		config.device_id = deviceId.trimmed().toUtf8().toStdString();
		config.type = livekit::WasapiCaptureType::Microphone;
		config.target_sample_rate = 16000;
		config.target_channels = 1;
		config.buffer_duration_ms = 20;
		config.auto_reconnect = true;

		if (!capture->Init(config, source)) {
			alive->store(false, std::memory_order_release);
			capture->SetCaptureStateCallback(nullptr);
			if (sub) sub->disconnect();
			return false;
		}
		// Never create/attach a render-reference APM instance outside or alongside the meeting ADM.
		capture->DisableApm();
		if (!capture->Start()) {
			alive->store(false, std::memory_order_release);
			capture->SetCaptureStateCallback(nullptr);
			capture->Stop();
			if (sub) sub->disconnect();
			return false;
		}

		std::lock_guard<std::mutex> lock(_mutex);
		_alive = std::move(alive);
		_source = std::move(source);
		_subscription = std::move(sub);
		_capture = std::move(capture);
		return true;
	}

	void stop() override {
		std::shared_ptr<std::atomic<bool>> alive;
		std::shared_ptr<livekit::AudioSource::Subscription> sub;
		std::shared_ptr<livekit::WasapiAudioCapture> capture;
		std::shared_ptr<livekit::AudioSource> source;
		{
			std::lock_guard<std::mutex> lock(_mutex);
			alive = std::move(_alive);
			sub = std::move(_subscription);
			capture = std::move(_capture);
			source = std::move(_source);
		}
		if (alive) {
			alive->store(false, std::memory_order_release);
		}
		if (sub) {
			sub->disconnect();
		}
		if (capture) {
			capture->SetCaptureStateCallback(nullptr);
			capture->Stop();
		}
		source.reset();
	}

private:
	std::mutex _mutex;
	std::shared_ptr<std::atomic<bool>> _alive;
	std::shared_ptr<livekit::AudioSource> _source;
	std::shared_ptr<livekit::AudioSource::Subscription> _subscription;
	std::shared_ptr<livekit::WasapiAudioCapture> _capture;
};
#else
class UnsupportedDictationAudioSource final : public IDictationAudioSource {
public:
	bool isPlatformCaptureSupported() const override { return false; }
	bool start(const QString &, FrameCallback, StateCallback) override { return false; }
	void stop() override {}
};
#endif

std::unique_ptr<IDictationAudioSource> createDefaultDictationAudioSource() {
#ifdef _WIN32
	return std::make_unique<WasapiDictationAudioSource>();
#else
	return std::make_unique<UnsupportedDictationAudioSource>();
#endif
}

} // namespace

// =========================================================================
// DictationAnchorTracker
// =========================================================================

void DictationAnchorTracker::reset() {
	_anchorPos = -1;
	_interimLength = 0;
	_insertedLeadingSpace = false;
	_activeSegmentId.clear();
}

void DictationAnchorTracker::beginSession(int cursorPos) {
	_anchorPos = std::max(0, cursorPos);
	_interimLength = 0;
	_insertedLeadingSpace = false;
	_activeSegmentId.clear();
}

void DictationAnchorTracker::onManualUserEdit(int newCursorPos) {
	_anchorPos = std::max(0, newCursorPos);
	_interimLength = 0;
	_insertedLeadingSpace = false;
	_activeSegmentId.clear();
}

bool DictationAnchorTracker::shouldInsertLeadingSpace(QChar prevChar, QChar nextChar) {
	if (prevChar.isNull() || prevChar.isSpace() || nextChar.isNull() || nextChar.isSpace()) {
		return false;
	}
	// Avoid inserting spaces before punctuation or between CJK ideographs.
	if (nextChar.isPunct()) {
		return false;
	}
	const bool prevLatin = prevChar.unicode() < 0x2E80 && (prevChar.isLetterOrNumber() || prevChar == QLatin1Char(',') || prevChar == QLatin1Char('.') || prevChar == QLatin1Char(':') || prevChar == QLatin1Char(';'));
	const bool nextLatin = nextChar.unicode() < 0x2E80 && nextChar.isLetterOrNumber();
	return prevLatin && nextLatin;
}

DictationTextEditOp DictationAnchorTracker::applySpeechEvent(
		SpeechToTextStatus status,
		const QString &segmentId,
		const QString &rawText,
		const QString &currentBuffer) {
	DictationTextEditOp op;
	if (status != SpeechToTextStatus::Recognizing && status != SpeechToTextStatus::Recognized) {
		return op;
	}
	const QString clean = rawText.trimmed();
	if (clean.isEmpty()) {
		return op;
	}

	const int bufLen = currentBuffer.size();
	if (_anchorPos < 0 || _anchorPos > bufLen) {
		_anchorPos = bufLen;
		_interimLength = 0;
		_insertedLeadingSpace = false;
	}
	if (_anchorPos + _interimLength > bufLen) {
		_interimLength = std::max(0, bufLen - _anchorPos);
	}

	// If segmentId changed while previous clause wasn't finalized cleanly,
	// promote the previous interim clause and start a new anchor after it.
	if (!_activeSegmentId.isEmpty() && !segmentId.isEmpty() &&
		_activeSegmentId != segmentId && _interimLength > 0) {
		_anchorPos += _interimLength;
		_interimLength = 0;
		_insertedLeadingSpace = false;
	}
	if (!segmentId.isEmpty()) {
		_activeSegmentId = segmentId;
	}

	if (_interimLength == 0) {
		_insertedLeadingSpace = (_anchorPos > 0) &&
			shouldInsertLeadingSpace(currentBuffer.at(_anchorPos - 1), clean.at(0));
	}

	const QString formatted = _insertedLeadingSpace
		? (QLatin1Char(' ') + clean)
		: clean;

	op.hasEdit = true;
	op.replaceStart = _anchorPos;
	op.replaceLength = _interimLength;
	op.replacement = formatted;
	op.nextCursorPos = _anchorPos + formatted.size();

	if (status == SpeechToTextStatus::Recognizing) {
		_interimLength = formatted.size();
	} else {
		_anchorPos = op.nextCursorPos;
		_interimLength = 0;
		_insertedLeadingSpace = false;
		_activeSegmentId.clear();
	}
	return op;
}

// =========================================================================
// DictationVadSegmenter
// =========================================================================

DictationVadSegmenter::DictationVadSegmenter()
	: DictationVadSegmenter(Config{}) {
}

DictationVadSegmenter::DictationVadSegmenter(const Config &config)
	: _config(config) {
	_config.sampleRate = std::max(8000, _config.sampleRate);
	_config.channels = std::max(1, _config.channels);
	const size_t samplesPerMs = static_cast<size_t>(_config.sampleRate * _config.channels) / 1000;
	_preRollCapacitySamples = std::max<size_t>(1, samplesPerMs * static_cast<size_t>(std::max(0, _config.preRollMs)));
	_minSpeechSamples = std::max<size_t>(1, samplesPerMs * static_cast<size_t>(std::max(40, _config.minSpeechMs)));
	_silenceCommitSamples = std::max<size_t>(samplesPerMs * 100, samplesPerMs * static_cast<size_t>(std::max(150, _config.silenceCommitMs)));
	_maxUtteranceSamples = std::max<size_t>(_minSpeechSamples * 2, samplesPerMs * static_cast<size_t>(std::max(2000, _config.maxUtteranceMs)));
}

void DictationVadSegmenter::reset() {
	_preRoll.clear();
	_utterance.clear();
	_speechSamples = 0;
	_trailingSilenceSamples = 0;
	_inSpeech = false;
}

DictationVadSegmenter::FrameOutcome DictationVadSegmenter::pushPcm16(
		const int16_t *samples,
		size_t sampleCount) {
	FrameOutcome out;
	if (!samples || sampleCount == 0) {
		return out;
	}

	const float rms = livekit::AudioVad::CalculateRmsPcm16(samples, sampleCount);
	out.normalizedLevel = livekit::AudioVad::RmsToAudioLevel(rms);
	livekit::AudioVad vad(_config.thresholdDb);
	const bool frameSpeaking = vad.IsSpeaking(rms);
	out.isSpeaking = frameSpeaking || _inSpeech;

	if (!_inSpeech) {
		if (frameSpeaking) {
			_inSpeech = true;
			out.speechJustStarted = true;
			out.isSpeaking = true;
			_utterance.swap(_preRoll);
			_preRoll.clear();
			_utterance.insert(_utterance.end(), samples, samples + sampleCount);
			_speechSamples = sampleCount;
			_trailingSilenceSamples = 0;
		} else {
			_preRoll.insert(_preRoll.end(), samples, samples + sampleCount);
			if (_preRoll.size() > _preRollCapacitySamples) {
				const size_t overflow = _preRoll.size() - _preRollCapacitySamples;
				_preRoll.erase(_preRoll.begin(), _preRoll.begin() + static_cast<ptrdiff_t>(overflow));
			}
			return out;
		}
	} else {
		_utterance.insert(_utterance.end(), samples, samples + sampleCount);
		if (frameSpeaking) {
			_speechSamples += sampleCount;
			_trailingSilenceSamples = 0;
		} else {
			_trailingSilenceSamples += sampleCount;
		}
	}

	const bool silenceElapsed = !_preRollCapacitySamples
		? (_trailingSilenceSamples >= _silenceCommitSamples)
		: (_trailingSilenceSamples >= _silenceCommitSamples);
	const bool maxDurationHit = _utterance.size() >= _maxUtteranceSamples;

	if (silenceElapsed || maxDurationHit) {
		if (_speechSamples >= _minSpeechSamples) {
			out.completedWav = buildAndClearUtteranceWav();
		} else {
			reset();
		}
	}
	return out;
}

std::optional<QByteArray> DictationVadSegmenter::flushPendingWav() {
	if (!_inSpeech || _speechSamples < _minSpeechSamples || _utterance.empty()) {
		reset();
		return std::nullopt;
	}
	return buildAndClearUtteranceWav();
}

QByteArray DictationVadSegmenter::buildAndClearUtteranceWav() {
	QByteArray wav = encodePcm16ToWav(
		_utterance.data(),
		_utterance.size(),
		_config.sampleRate,
		_config.channels);
	reset();
	return wav;
}

QByteArray DictationVadSegmenter::encodePcm16ToWav(
		const int16_t *samples,
		size_t sampleCount,
		int sampleRate,
		int channels) {
	if (!samples || sampleCount == 0 || sampleRate <= 0 || channels <= 0) {
		return {};
	}
	const uint32_t dataBytes = static_cast<uint32_t>(sampleCount * sizeof(int16_t));
	const uint32_t riffSize = 36u + dataBytes;
	const uint16_t ch = static_cast<uint16_t>(channels);
	const uint32_t sr = static_cast<uint32_t>(sampleRate);
	const uint16_t bitsPerSample = 16;
	const uint16_t blockAlign = static_cast<uint16_t>(ch * (bitsPerSample / 8));
	const uint32_t byteRate = sr * blockAlign;

	QByteArray out;
	out.resize(44 + static_cast<int>(dataBytes));
	char *hdr = out.data();
	std::memcpy(hdr + 0, "RIFF", 4);
	writeLe32(hdr + 4, riffSize);
	std::memcpy(hdr + 8, "WAVE", 4);
	std::memcpy(hdr + 12, "fmt ", 4);
	writeLe32(hdr + 16, 16u);
	writeLe16(hdr + 20, 1u); // PCM
	writeLe16(hdr + 22, ch);
	writeLe32(hdr + 24, sr);
	writeLe32(hdr + 28, byteRate);
	writeLe16(hdr + 32, blockAlign);
	writeLe16(hdr + 34, bitsPerSample);
	std::memcpy(hdr + 36, "data", 4);
	writeLe32(hdr + 40, dataBytes);
	std::memcpy(hdr + 44, samples, dataBytes);
	return out;
}

float DictationVadSegmenter::computeNormalizedLevel(const int16_t *samples, size_t sampleCount) {
	if (!samples || sampleCount == 0) {
		return 0.0f;
	}
	const float rms = livekit::AudioVad::CalculateRmsPcm16(samples, sampleCount);
	return livekit::AudioVad::RmsToAudioLevel(rms);
}

// =========================================================================
// OpenAiCompatibleSpeechTransport
// =========================================================================

struct OpenAiCompatibleSpeechTransport::Impl {
	std::unique_ptr<QNetworkAccessManager> remoteNam;
	std::unique_ptr<QNetworkAccessManager> loopbackNam;
};

OpenAiCompatibleSpeechTransport::OpenAiCompatibleSpeechTransport()
	: _impl(std::make_unique<Impl>()) {
	_impl->remoteNam = std::make_unique<QNetworkAccessManager>();
	_impl->loopbackNam = std::make_unique<QNetworkAccessManager>();
	_impl->loopbackNam->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
}

OpenAiCompatibleSpeechTransport::~OpenAiCompatibleSpeechTransport() = default;

QUrl OpenAiCompatibleSpeechTransport::resolveTranscriptionEndpointUrl(
		const QUrl &baseUrl,
		bool isCustomSttUrl) {
	QUrl url = baseUrl;
	QString path = url.path();
	if (path.isEmpty()) {
		path = QStringLiteral("/");
	}
	if (path.endsWith(QStringLiteral("/v1/stt/"), Qt::CaseInsensitive) ||
		path.compare(QStringLiteral("/stt/"), Qt::CaseInsensitive) == 0 ||
		(isCustomSttUrl && path.endsWith(QStringLiteral("/stt/"), Qt::CaseInsensitive))) {
		path.chop(1);
	}
	if (path.endsWith(QStringLiteral("/v1/stt"), Qt::CaseInsensitive) ||
		(isCustomSttUrl && path.endsWith(QStringLiteral("/stt"), Qt::CaseInsensitive))) {
		url.setPath(path);
		return url;
	}
	if (path.endsWith(QStringLiteral("/v1/speech-to-text/"), Qt::CaseInsensitive) ||
		path.compare(QStringLiteral("/speech-to-text/"), Qt::CaseInsensitive) == 0 ||
		(isCustomSttUrl && path.endsWith(QStringLiteral("/speech-to-text/"), Qt::CaseInsensitive))) {
		path.chop(1);
	}
	if (path.endsWith(QStringLiteral("/v1/speech-to-text"), Qt::CaseInsensitive) ||
		(isCustomSttUrl && path.endsWith(QStringLiteral("/speech-to-text"), Qt::CaseInsensitive))) {
		url.setPath(path);
		return url;
	}
	for (const char *chatSuffix : { "/chat/completions", "/responses", "/messages" }) {
		const QString s = QLatin1String(chatSuffix);
		if (path.endsWith(s, Qt::CaseInsensitive)) {
			path.chop(s.size());
			if (path.isEmpty()) {
				path = QStringLiteral("/");
			}
			break;
		}
	}
	if (isElevenLabsAgentHost(url)) {
		for (const char *txSuffix : { "/speech-to-text/realtime", "/audio/transcriptions", "/transcriptions" }) {
			const QString s = QLatin1String(txSuffix);
			if (path.endsWith(s, Qt::CaseInsensitive)) {
				path.chop(s.size());
				if (path.isEmpty()) {
					path = QStringLiteral("/");
				}
				break;
			}
		}
		if (path == QStringLiteral("/") ||
			path.compare(QStringLiteral("/v1"), Qt::CaseInsensitive) == 0 ||
			path.compare(QStringLiteral("/v1/"), Qt::CaseInsensitive) == 0) {
			path = QStringLiteral("/v1/speech-to-text");
		} else if (!path.endsWith(QStringLiteral("/speech-to-text"), Qt::CaseInsensitive)) {
			if (path.endsWith(QLatin1Char('/'))) {
				path += QStringLiteral("speech-to-text");
			} else {
				path += QStringLiteral("/speech-to-text");
			}
		}
		url.setPath(path);
		return url;
	}
	if (isXaiAgentHost(url)) {
		for (const char *txSuffix : { "/audio/transcriptions", "/transcriptions" }) {
			const QString s = QLatin1String(txSuffix);
			if (path.endsWith(s, Qt::CaseInsensitive)) {
				path.chop(s.size());
				if (path.isEmpty()) {
					path = QStringLiteral("/");
				}
				break;
			}
		}
		if (path == QStringLiteral("/") ||
			path.compare(QStringLiteral("/v1"), Qt::CaseInsensitive) == 0 ||
			path.compare(QStringLiteral("/v1/"), Qt::CaseInsensitive) == 0) {
			path = QStringLiteral("/v1/stt");
		} else if (path.endsWith(QLatin1Char('/'))) {
			path += QStringLiteral("stt");
		} else {
			path += QStringLiteral("/stt");
		}
		url.setPath(path);
		return url;
	}
	if (path.endsWith(QStringLiteral("/audio/transcriptions"), Qt::CaseInsensitive) ||
		path.endsWith(QStringLiteral("/transcriptions"), Qt::CaseInsensitive) ||
		(isCustomSttUrl && path.endsWith(QStringLiteral("/inference"), Qt::CaseInsensitive))) {
		url.setPath(path);
		return url;
	}
	normalizeGroqOpenAiRootPath(url, path);
	if (path == QStringLiteral("/")) {
		path = QStringLiteral("/v1/audio/transcriptions");
	} else if (path.endsWith(QLatin1Char('/'))) {
		path += QStringLiteral("audio/transcriptions");
	} else {
		path += QStringLiteral("/audio/transcriptions");
	}
	url.setPath(path);
	return url;
}

QUrl OpenAiCompatibleSpeechTransport::resolveChatCompletionsEndpointUrl(const QUrl &baseUrl) {
	QUrl url = baseUrl;
	QString path = url.path();
	if (path.isEmpty()) {
		path = QStringLiteral("/");
	}
	for (const char *suffix : { "/audio/transcriptions", "/transcriptions", "/responses", "/messages" }) {
		const QString s = QLatin1String(suffix);
		if (path.endsWith(s, Qt::CaseInsensitive)) {
			path.chop(s.size());
			if (path.isEmpty()) {
				path = QStringLiteral("/");
			}
			break;
		}
	}
	if (path.endsWith(QStringLiteral("/chat/completions"), Qt::CaseInsensitive)) {
		url.setPath(path);
		return url;
	}
	normalizeGroqOpenAiRootPath(url, path);
	if (path == QStringLiteral("/")) {
		path = QStringLiteral("/v1/chat/completions");
	} else if (path.endsWith(QLatin1Char('/'))) {
		path += QStringLiteral("chat/completions");
	} else {
		path += QStringLiteral("/chat/completions");
	}
	url.setPath(path);
	return url;
}

QUrl OpenAiCompatibleSpeechTransport::resolveDashScopeMultimodalGenerationEndpointUrl(const QUrl &baseUrl) {
	QUrl url = baseUrl;
	url.setPath(QStringLiteral("/api/v1/services/aigc/multimodal-generation/generation"));
	url.setQuery(QString());
	url.setFragment(QString());
	return url;
}

bool OpenAiCompatibleSpeechTransport::usesChatCompletionsProtocol(
		const QString &modelId,
		const QUrl &endpointBaseUrl,
		const AgentModelCapabilities &caps) {
	return classifySpeechModel(
		modelId,
		endpointBaseUrl.host(),
		caps).kind == AgentSpeechModelKind::SyncChatInputAudio;
}

bool OpenAiCompatibleSpeechTransport::supportsMultipartTranscriptionSse(
		const QString &modelId,
		const QString &endpointHost) {
	QString normalized = modelId.trimmed().toLower();
	const int slash = normalized.lastIndexOf(QLatin1Char('/'));
	if (slash >= 0 && slash + 1 < normalized.size()) {
		normalized = normalized.mid(slash + 1);
	}
	if (normalized.isEmpty()) {
		return false;
	}
	if (isGroqAgentHost(endpointHost) ||
		isSiliconFlowAgentHost(endpointHost) ||
		isDashScopeAgentHost(endpointHost) ||
		isXaiAgentHost(endpointHost) ||
		isXaiSttModelId(modelId) ||
		isElevenLabsAgentHost(endpointHost) ||
		isElevenLabsScribeModelId(modelId)) {
		return false;
	}
	return normalized.contains(QStringLiteral("-transcribe")) &&
		!normalized.contains(QStringLiteral("-diarize")) &&
		!normalized.contains(QStringLiteral("-live-transcribe"));
}

bool OpenAiCompatibleSpeechTransport::supportsChatInputAudioSse(
		const QString &modelId,
		const QString &endpointHost) {
	if (isDashScopeAgentHost(endpointHost)) {
		return false;
	}
	QString normalized = modelId.trimmed().toLower();
	const int slash = normalized.lastIndexOf(QLatin1Char('/'));
	if (slash >= 0 && slash + 1 < normalized.size()) {
		normalized = normalized.mid(slash + 1);
	}
	if (normalized.isEmpty()) {
		return false;
	}
	return !(normalized.contains(QStringLiteral("qwen")) &&
		normalized.contains(QStringLiteral("asr")));
}

OpenAiCompatibleSpeechTransport::MultipartTranscriptionFields
OpenAiCompatibleSpeechTransport::buildMultipartTranscriptionFields(
		const QString &modelId,
		const QString &endpointHost,
		const QString &language,
		const QString &prompt,
		bool enableStream,
		const QString &endpointPath) {
	MultipartTranscriptionFields fields;
	if (isXaiSttMultipartTarget(endpointHost, endpointPath, modelId)) {
		fields.model = modelId.trimmed().isEmpty()
			? QStringLiteral("grok-voice-transcribe-2.0")
			: modelId.trimmed();
		fields.responseFormat.clear();
		fields.language = normalizeMultipartIso639Language(language);
		fields.format = isXaiSttFormatSupportedLanguage(fields.language);
		fields.prompt.clear();
		fields.keyterms = parseXaiSttKeyterms(prompt);
		fields.stream = false;
		fields.filePartLast = true;
		return fields;
	}
	if (isElevenLabsSttMultipartTarget(endpointHost, endpointPath, modelId)) {
		fields.model = modelId.trimmed().isEmpty()
			? QStringLiteral("scribe_v3")
			: modelId.trimmed();
		fields.modelFieldName = QStringLiteral("model_id");
		fields.languageFieldName = QStringLiteral("language_code");
		fields.language = normalizeMultipartIso639Language(language);
		fields.stream = false;
		return fields;
	}
	fields.model = modelId.trimmed();
	if (isSiliconFlowAgentHost(endpointHost)) {
		return fields;
	}
	fields.responseFormat = QStringLiteral("json");
	fields.language = normalizeMultipartIso639Language(language);
	QString cleanPrompt = prompt.trimmed();
	if (isGroqAgentHost(endpointHost) && cleanPrompt.size() > 600) {
		cleanPrompt = cleanPrompt.left(600);
	}
	fields.prompt = cleanPrompt;
	fields.stream = enableStream && !isGroqAgentHost(endpointHost);
	return fields;
}

QString OpenAiCompatibleSpeechTransport::parseTranscriptionResponseJson(
		const QByteArray &payload,
		const QString &contentType,
		QString *errorOut) {
	if (errorOut) {
		errorOut->clear();
	}
	const QByteArray trimmed = payload.trimmed();
	if (trimmed.isEmpty()) {
		return {};
	}
	QJsonParseError err{};
	const QJsonDocument doc = QJsonDocument::fromJson(trimmed, &err);
	if (err.error == QJsonParseError::NoError && doc.isObject()) {
		const QJsonObject root = doc.object();
		if (root.contains(QStringLiteral("error")) && !root.value(QStringLiteral("error")).isNull()) {
			if (errorOut) {
				*errorOut = extractJsonErrorDetail(trimmed);
				if (errorOut->isEmpty()) {
					*errorOut = QStringLiteral("Speech transcription server reported an error.");
				}
			}
			return {};
		}
		for (const char *key : { "text", "transcript", "transcription", "output_text" }) {
			if (root.contains(QLatin1String(key))) {
				const QString val = root.value(QLatin1String(key)).toString().trimmed();
				if (!val.isEmpty()) {
					return stripSenseVoiceControlTags(val);
				}
			}
		}
		const auto extractChoicesText = [](const QJsonArray &choices) -> QString {
			if (choices.isEmpty()) {
				return {};
			}
			const QJsonObject msgObj = choices.first().toObject().value(QStringLiteral("message")).toObject();
			return extractChatContentText(msgObj.value(QStringLiteral("content"))).trimmed();
		};
		if (root.contains(QStringLiteral("choices")) && root.value(QStringLiteral("choices")).isArray()) {
			const QString t = extractChoicesText(root.value(QStringLiteral("choices")).toArray());
			if (!t.isEmpty()) {
				return stripSenseVoiceControlTags(t);
			}
		}
		if (root.contains(QStringLiteral("output")) && root.value(QStringLiteral("output")).isArray()) {
			const QString t = extractResponsesOutputArrayText(root.value(QStringLiteral("output")).toArray());
			if (!t.isEmpty()) {
				return stripSenseVoiceControlTags(t);
			}
		}
		if (root.contains(QStringLiteral("output")) && root.value(QStringLiteral("output")).isObject()) {
			const QJsonObject outObj = root.value(QStringLiteral("output")).toObject();
			if (outObj.contains(QStringLiteral("choices")) && outObj.value(QStringLiteral("choices")).isArray()) {
				const QString t = extractChoicesText(outObj.value(QStringLiteral("choices")).toArray());
				if (!t.isEmpty()) {
					return stripSenseVoiceControlTags(t);
				}
			}
			const QString t = outObj.value(QStringLiteral("text")).toString().trimmed();
			if (!t.isEmpty()) {
				return stripSenseVoiceControlTags(t);
			}
		}
		return {};
	}
	if (contentType.contains(QStringLiteral("text/plain"))) {
		return stripSenseVoiceControlTags(QString::fromUtf8(trimmed));
	}
	if (errorOut) {
		*errorOut = QStringLiteral("Invalid JSON response from speech transcription endpoint.");
	}
	return {};
}

AgentSpeechRequestHandle *OpenAiCompatibleSpeechTransport::transcribeWav(
		const SpeechTranscriptionRequest &request,
		QObject *parent) {
	const QString effectiveBase = request.customSttBaseUrl.trimmed().isEmpty()
		? request.profile.baseUrl.trimmed()
		: request.customSttBaseUrl.trimmed();
	const AgentEndpointDecision decision = evaluateAgentEndpoint(effectiveBase);
	if (!decision.allowed) {
		return new NetworkSpeechRequestHandle(decision.reason, parent);
	}
	if (request.wavAudioBytes.isEmpty()) {
		return new NetworkSpeechRequestHandle(QStringLiteral("Audio buffer is empty."), parent);
	}
	if (request.wavAudioBytes.size() > kMaxSpeechAudioUploadBytes) {
		return new NetworkSpeechRequestHandle(
			QCoreApplication::translate(
				"MeetingUI",
				"Audio payload exceeds the 25 MB transcription upload limit."),
			parent);
	}

	const QString hostLower = decision.normalizedUrl.host().toLower();
	const QString hostDefaultModel = isDashScopeAgentHost(hostLower)
		? QStringLiteral("qwen3-asr-flash")
		: (isGroqAgentHost(hostLower)
			? QStringLiteral("whisper-large-v3-turbo")
			: (isSiliconFlowAgentHost(hostLower)
				? QStringLiteral("FunAudioLLM/SenseVoiceSmall")
				: (isVolcengineArkHost(hostLower)
					? QStringLiteral("doubao-seed-2-1-lite-260915")
					: (isXaiAgentHost(hostLower)
						? QStringLiteral("grok-voice-transcribe-2.0")
						: (isElevenLabsAgentHost(hostLower)
							? QStringLiteral("scribe_v3")
							: (isOpenAiAgentHost(hostLower)
								? QStringLiteral("gpt-transcribe")
								: QStringLiteral("whisper-1")))))));
	const QString effectiveModel = request.sttModel.trimmed().isEmpty()
		? hostDefaultModel
		: request.sttModel.trimmed();
	const AgentSpeechModelInfo modelInfo = classifySpeechModel(
		effectiveModel, decision.normalizedUrl.host(), request.sttModelCapabilities);

	enum class ResolvedSpeechWireProtocol {
		MultipartTranscriptions,
		ChatCompletionsInputAudio,
		DashScopeMultimodalGeneration,
	};
	ResolvedSpeechWireProtocol wireProtocol = ResolvedSpeechWireProtocol::MultipartTranscriptions;
	switch (request.protocolMode) {
	case AgentSpeechProtocolMode::OpenAiTranscriptions:
		wireProtocol = ResolvedSpeechWireProtocol::MultipartTranscriptions;
		break;
	case AgentSpeechProtocolMode::ChatInputAudio:
		wireProtocol = ResolvedSpeechWireProtocol::ChatCompletionsInputAudio;
		break;
	case AgentSpeechProtocolMode::DashScopeMultimodal:
		wireProtocol = ResolvedSpeechWireProtocol::DashScopeMultimodalGeneration;
		break;
	case AgentSpeechProtocolMode::Auto:
	default:
		if (!modelInfo.usableForDictation) {
			const QString syncExample = hostDefaultModel;
			QString baseMsg;
			switch (modelInfo.kind) {
			case AgentSpeechModelKind::TtsOnly:
				baseMsg = QCoreApplication::translate(
					"MeetingUI",
					"Model '%1' is a text-to-speech synthesis model and cannot be used for microphone dictation. Please select a synchronous speech recognition model such as %2.")
					.arg(effectiveModel, syncExample);
				break;
			case AgentSpeechModelKind::AudioUnderstanding:
				baseMsg = QCoreApplication::translate(
					"MeetingUI",
					"Model '%1' is a general audio understanding model rather than dedicated dictation ASR (unless declared with audio-to-text capability). Please select a synchronous speech recognition model such as %2.")
					.arg(effectiveModel, syncExample);
				break;
			case AgentSpeechModelKind::RealtimeStream:
				baseMsg = QCoreApplication::translate(
					"MeetingUI",
					"Model '%1' requires WebSocket streaming ASR and cannot be used with the selected HTTP protocol. Switch Recognition Protocol to Auto (Recommended) or the matching WebSocket streaming protocol, or choose a synchronous HTTP model such as %2.")
					.arg(effectiveModel, syncExample);
				break;
			case AgentSpeechModelKind::NotSpeech:
			case AgentSpeechModelKind::AsyncFiletrans:
			default:
				baseMsg = QCoreApplication::translate(
					"MeetingUI",
					"Model '%1' is an async file-URL transcription model and cannot be used for live microphone recognition. Please select a streaming or synchronous model such as %2.")
					.arg(effectiveModel, syncExample);
				break;
			}
			const QString unsupportedMsg = baseMsg
				+ QStringLiteral(" [") + agentSpeechModelKindToken(modelInfo.kind) + QStringLiteral("]");
			return new NetworkSpeechRequestHandle(unsupportedMsg, parent);
		}
		wireProtocol = (modelInfo.kind == AgentSpeechModelKind::SyncChatInputAudio)
			? ResolvedSpeechWireProtocol::ChatCompletionsInputAudio
			: ResolvedSpeechWireProtocol::MultipartTranscriptions;
		break;
	}

	const bool isCustom = !request.customSttBaseUrl.trimmed().isEmpty();
	const QUrl endpoint = (wireProtocol == ResolvedSpeechWireProtocol::ChatCompletionsInputAudio)
		? resolveChatCompletionsEndpointUrl(decision.normalizedUrl)
		: ((wireProtocol == ResolvedSpeechWireProtocol::DashScopeMultimodalGeneration)
			? resolveDashScopeMultimodalGenerationEndpointUrl(decision.normalizedUrl)
			: resolveTranscriptionEndpointUrl(decision.normalizedUrl, isCustom));

	QNetworkRequest netReq(endpoint);
	netReq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
	netReq.setRawHeader("Accept", "application/json, text/plain");
	netReq.setRawHeader("Cache-Control", "no-cache");

	if (endpoint.scheme() == QStringLiteral("https")) {
		QSslConfiguration sslConfig = QSslConfiguration::defaultConfiguration();
		sslConfig.setPeerVerifyMode(QSslSocket::VerifyPeer);
		netReq.setSslConfiguration(sslConfig);
	}
	const bool elevenLabsMultipart =
		(wireProtocol == ResolvedSpeechWireProtocol::MultipartTranscriptions) &&
		isElevenLabsSttMultipartTarget(endpoint.host(), endpoint.path(), effectiveModel);
	const QString cleanApiKey = request.apiKey.trimmed();
	if (!cleanApiKey.isEmpty()) {
		if (elevenLabsMultipart && isElevenLabsSingleUseToken(cleanApiKey)) {
			QUrl authEndpoint = endpoint;
			QUrlQuery authQuery(authEndpoint);
			authQuery.removeAllQueryItems(QStringLiteral("token"));
			authQuery.addQueryItem(QStringLiteral("token"), cleanApiKey);
			authEndpoint.setQuery(authQuery);
			netReq.setUrl(authEndpoint);
		} else if (elevenLabsMultipart) {
			netReq.setRawHeader("xi-api-key", cleanApiKey.toUtf8());
		} else {
			netReq.setRawHeader("Authorization", QByteArray("Bearer ") + cleanApiKey.toUtf8());
		}
	}
	const bool forwardProfileCustomHeaders = !isCustom ||
		AgentCoordinator::isSameAgentOrigin(request.customSttBaseUrl, request.profile.baseUrl);
	if (forwardProfileCustomHeaders) {
		for (auto it = request.profile.customHeaders.cbegin(); it != request.profile.customHeaders.cend(); ++it) {
			if (isValidCustomHeaderName(it.key()) &&
				isValidCustomHeaderValue(it.value()) &&
				!isReservedMultipartHeader(it.key())) {
				netReq.setRawHeader(it.key().trimmed().toUtf8(), it.value().trimmed().toUtf8());
			}
		}
	}

	const auto endpointBytes = endpoint.toString(QUrl::FullyEncoded).toUtf8();
	const auto safeEndpoint = livekit::secure_log::EndpointSummary(
		std::string_view(endpointBytes.constData(), static_cast<size_t>(endpointBytes.size())));
	const bool loopback = isAgentLoopbackHost(endpoint.host());
	const AgentKeyEndpointMismatch keyMismatch = evaluateKeyEndpointCompatibility(request.apiKey, endpoint.host());
	QNetworkAccessManager &nam = loopback ? *_impl->loopbackNam : *_impl->remoteNam;

	const auto buildDashScopeNativePayload = [&](const QString &wavDataUri) -> QByteArray {
		QJsonObject nativeAudioObj;
		nativeAudioObj.insert(QStringLiteral("data"), wavDataUri);
		QJsonObject nativeAudioPart;
		nativeAudioPart.insert(QStringLiteral("type"), QStringLiteral("input_audio"));
		nativeAudioPart.insert(QStringLiteral("input_audio"), nativeAudioObj);

		QJsonArray nativeUserContent;
		nativeUserContent.append(nativeAudioPart);
		QJsonObject nativeUserMsg;
		nativeUserMsg.insert(QStringLiteral("role"), QStringLiteral("user"));
		nativeUserMsg.insert(QStringLiteral("content"), nativeUserContent);

		QJsonArray nativeMessages;
		nativeMessages.append(nativeUserMsg);

		QJsonObject nativeInput;
		nativeInput.insert(QStringLiteral("messages"), nativeMessages);

		QJsonObject nativeParams;
		nativeParams.insert(QStringLiteral("format"), QStringLiteral("wav"));
		nativeParams.insert(QStringLiteral("sample_rate"), QStringLiteral("16000"));
		if (!request.language.trimmed().isEmpty()) {
			nativeParams.insert(QStringLiteral("language"), request.language.trimmed());
		}

		QJsonObject nativePayload;
		nativePayload.insert(QStringLiteral("model"), effectiveModel);
		nativePayload.insert(QStringLiteral("input"), nativeInput);
		nativePayload.insert(QStringLiteral("parameters"), nativeParams);
		return QJsonDocument(nativePayload).toJson(QJsonDocument::Compact);
	};

	if (wireProtocol == ResolvedSpeechWireProtocol::DashScopeMultimodalGeneration) {
		netReq.setRawHeader("Content-Type", "application/json");
		netReq.setRawHeader("Accept", "application/json");
		netReq.setRawHeader("X-DashScope-SSE", "disable");
		const QString wavDataUri = QStringLiteral("data:audio/wav;base64,") +
			QString::fromLatin1(request.wavAudioBytes.toBase64());
		const QByteArray nativeJsonBytes = buildDashScopeNativePayload(wavDataUri);
		qInfo() << "[AgentSpeech] Posting DashScope multimodal-generation utterance wavBytes=" << request.wavAudioBytes.size()
			<< "model=" << effectiveModel
			<< "endpoint=" << QString::fromStdString(safeEndpoint);
		QNetworkReply *reply = nam.post(netReq, nativeJsonBytes);
		return new NetworkSpeechRequestHandle(
			reply,
			request.profile.connectTimeoutMs + 25000,
			parent,
			keyMismatch,
			true);
	}

	if (wireProtocol == ResolvedSpeechWireProtocol::ChatCompletionsInputAudio) {
		netReq.setRawHeader("Content-Type", "application/json");
		const QString modelLower = effectiveModel.trimmed().toLower();
		const bool isQwenAsrModel = modelLower.contains(QStringLiteral("qwen")) &&
			modelLower.contains(QStringLiteral("asr"));
		const bool dashScopeCompatible = isDashScopeAgentHost(endpoint.host()) ||
			endpoint.path().contains(QStringLiteral("/compatible-mode/"), Qt::CaseInsensitive);
		const bool useDashScopeDataUri = dashScopeCompatible || isQwenAsrModel;
		const QString rawBase64 = QString::fromLatin1(request.wavAudioBytes.toBase64());
		const QString wavDataUri = QStringLiteral("data:audio/wav;base64,") + rawBase64;
		const bool dashScopeDedicatedAsr = modelInfo.dedicatedAsr && isDashScopeAgentHost(endpoint.host());

		QJsonObject audioObj;
		audioObj.insert(QStringLiteral("data"), useDashScopeDataUri ? wavDataUri : rawBase64);
		if (!dashScopeDedicatedAsr) {
			audioObj.insert(QStringLiteral("format"), QStringLiteral("wav"));
		}

		QJsonObject audioPart;
		audioPart.insert(QStringLiteral("type"), QStringLiteral("input_audio"));
		audioPart.insert(QStringLiteral("input_audio"), audioObj);

		QJsonArray messagesArr;
		if (!modelInfo.dedicatedAsr) {
			QJsonObject sysMsg;
			sysMsg.insert(QStringLiteral("role"), QStringLiteral("system"));
			sysMsg.insert(
				QStringLiteral("content"),
				QStringLiteral(
					"You are a strict speech-to-text transcriber. Transcribe the user's audio verbatim "
					"and output ONLY the recognized speech text. Do not reply conversationally, do not add "
					"explanations, prefixes, or commentary. If the audio contains no intelligible speech, "
					"output an empty string."));
			messagesArr.append(sysMsg);
		}

		QJsonArray contentArr;
		contentArr.append(audioPart);
		if (!modelInfo.dedicatedAsr) {
			QJsonObject textPart;
			textPart.insert(QStringLiteral("type"), QStringLiteral("text"));
			QString instruction = QStringLiteral("Transcribe this audio verbatim.");
			const QString cleanLang = request.language.trimmed();
			if (!cleanLang.isEmpty()) {
				instruction += QStringLiteral(" Primary language: %1.").arg(cleanLang);
			}
			const QString cleanPrompt = request.prompt.trimmed();
			if (!cleanPrompt.isEmpty()) {
				instruction += QStringLiteral(" Context / vocabulary hint: ") + cleanPrompt;
			}
			textPart.insert(QStringLiteral("text"), instruction);
			contentArr.append(textPart);
		}

		QJsonObject userMsg;
		userMsg.insert(QStringLiteral("role"), QStringLiteral("user"));
		userMsg.insert(QStringLiteral("content"), contentArr);
		messagesArr.append(userMsg);

		const bool isArkOrDoubaoSeed = isVolcengineArkHost(endpoint.host()) ||
			modelLower.startsWith(QStringLiteral("doubao-seed-"));
		const bool useChatSse = request.streamResponse && !useDashScopeDataUri &&
			supportsChatInputAudioSse(effectiveModel, endpoint.host());

		QJsonObject payload;
		payload.insert(QStringLiteral("model"), effectiveModel);
		payload.insert(QStringLiteral("messages"), messagesArr);
		payload.insert(QStringLiteral("stream"), useChatSse);

		if (isArkOrDoubaoSeed) {
			QJsonObject thinkingObj;
			thinkingObj.insert(QStringLiteral("type"), QStringLiteral("disabled"));
			payload.insert(QStringLiteral("thinking"), thinkingObj);
			payload.insert(QStringLiteral("temperature"), 0);
		}

		if (useDashScopeDataUri && modelInfo.dedicatedAsr) {
			QJsonObject asrOptions;
			asrOptions.insert(QStringLiteral("enable_itn"), true);
			if (!request.language.trimmed().isEmpty()) {
				asrOptions.insert(QStringLiteral("language"), request.language.trimmed());
			}
			payload.insert(QStringLiteral("asr_options"), asrOptions);
		}

		NetworkSpeechRequestHandle::FallbackFactory fallbackFactory;
		const bool allowDashScopeNativeFallback =
			(request.protocolMode == AgentSpeechProtocolMode::Auto) &&
			modelInfo.dedicatedAsr &&
			(isDashScopeAgentHost(endpoint.host()) ||
				endpoint.path().contains(QStringLiteral("/compatible-mode/"), Qt::CaseInsensitive));
		if (allowDashScopeNativeFallback) {
			const QUrl nativeEndpoint = resolveDashScopeMultimodalGenerationEndpointUrl(endpoint);
			QNetworkRequest nativeReq = netReq;
			nativeReq.setUrl(nativeEndpoint);
			nativeReq.setRawHeader("Accept", "application/json");
			nativeReq.setRawHeader("X-DashScope-SSE", "disable");

			const QByteArray nativeJsonBytes = buildDashScopeNativePayload(wavDataUri);
			const auto nativeBytes = nativeEndpoint.toString(QUrl::FullyEncoded).toUtf8();
			const QString safeNativeEndpoint = QString::fromStdString(livekit::secure_log::EndpointSummary(
				std::string_view(nativeBytes.constData(), static_cast<size_t>(nativeBytes.size()))));
			QPointer<QNetworkAccessManager> namPtr(&nam);
			fallbackFactory = [namPtr, nativeReq, nativeJsonBytes, effectiveModel, safeNativeEndpoint]() -> QNetworkReply * {
				if (!namPtr) {
					return nullptr;
				}
				qInfo() << "[AgentSpeech] Falling back to DashScope multimodal-generation model="
					<< effectiveModel << "endpoint=" << safeNativeEndpoint;
				return namPtr->post(nativeReq, nativeJsonBytes);
			};
		} else if (useChatSse) {
			QNetworkRequest fallbackReq = netReq;
			fallbackReq.setRawHeader("Accept", "application/json, text/plain");
			netReq.setRawHeader("Accept", "text/event-stream, application/json, text/plain");
			QJsonObject nonStreamPayload = payload;
			nonStreamPayload.insert(QStringLiteral("stream"), false);
			const QByteArray fallbackJsonBytes = QJsonDocument(nonStreamPayload).toJson(QJsonDocument::Compact);
			QPointer<QNetworkAccessManager> namPtr(&nam);
			fallbackFactory = [namPtr, fallbackReq, fallbackJsonBytes, effectiveModel, safeEndpoint]() -> QNetworkReply * {
				if (!namPtr) {
					return nullptr;
				}
				qInfo() << "[AgentSpeech] Falling back to non-streaming /chat/completions model="
					<< effectiveModel << "endpoint=" << QString::fromStdString(safeEndpoint);
				return namPtr->post(fallbackReq, fallbackJsonBytes);
			};
		}

		const QByteArray jsonBytes = QJsonDocument(payload).toJson(QJsonDocument::Compact);
		qInfo() << "[AgentSpeech] Posting chat input_audio utterance wavBytes=" << request.wavAudioBytes.size()
			<< "model=" << effectiveModel
			<< "stream=" << useChatSse
			<< "endpoint=" << QString::fromStdString(safeEndpoint);

		QNetworkReply *reply = nam.post(netReq, jsonBytes);
		return new NetworkSpeechRequestHandle(
			reply,
			request.profile.connectTimeoutMs + 25000,
			parent,
			keyMismatch,
			true,
			std::move(fallbackFactory));
	}

	const QString cleanLang = request.language.trimmed();
	const QString cleanPrompt = request.prompt.trimmed();
	const QString endpointHost = endpoint.host();
	const QString endpointPath = endpoint.path();
	const auto buildMultipartBody = [audioBytes = request.wavAudioBytes, effectiveModel, endpointHost, endpointPath, cleanLang, cleanPrompt](
			bool enableStream) -> QHttpMultiPart * {
		const MultipartTranscriptionFields fields = buildMultipartTranscriptionFields(
			effectiveModel,
			endpointHost,
			cleanLang,
			cleanPrompt,
			enableStream,
			endpointPath);
		auto *multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
		const auto appendAudioPart = [&]() {
			QHttpPart audioPart;
			audioPart.setHeader(
				QNetworkRequest::ContentDispositionHeader,
				QVariant(QStringLiteral("form-data; name=\"file\"; filename=\"dictation.wav\"")));
			audioPart.setHeader(QNetworkRequest::ContentTypeHeader, QVariant(QStringLiteral("audio/wav")));
			audioPart.setBody(audioBytes);
			multiPart->append(audioPart);
		};
		const auto appendTextPart = [&](const QString &name, const QByteArray &value) {
			QHttpPart part;
			part.setHeader(
				QNetworkRequest::ContentDispositionHeader,
				QVariant(QStringLiteral("form-data; name=\"%1\"").arg(name)));
			part.setBody(value);
			multiPart->append(part);
		};

		if (!fields.filePartLast) {
			appendAudioPart();
		}

		appendTextPart(fields.modelFieldName, fields.model.toUtf8());

		if (!fields.responseFormat.isEmpty()) {
			appendTextPart(QStringLiteral("response_format"), fields.responseFormat.toUtf8());
		}
		if (!fields.language.isEmpty()) {
			appendTextPart(fields.languageFieldName, fields.language.toUtf8());
		}
		if (fields.format) {
			appendTextPart(QStringLiteral("format"), QByteArray("true"));
		}
		if (!fields.prompt.isEmpty()) {
			appendTextPart(QStringLiteral("prompt"), fields.prompt.toUtf8());
		}
		for (const QString &term : fields.keyterms) {
			appendTextPart(QStringLiteral("keyterm"), term.toUtf8());
		}
		if (fields.stream) {
			appendTextPart(QStringLiteral("stream"), QByteArray("true"));
		}

		if (fields.filePartLast) {
			appendAudioPart();
		}
		return multiPart;
	};

	const bool useSse = request.streamResponse &&
		supportsMultipartTranscriptionSse(effectiveModel, endpointHost) &&
		!isXaiSttMultipartTarget(endpointHost, endpointPath, effectiveModel) &&
		!isElevenLabsSttMultipartTarget(endpointHost, endpointPath, effectiveModel);
	NetworkSpeechRequestHandle::FallbackFactory fallbackFactory;
	if (useSse) {
		QNetworkRequest fallbackReq = netReq;
		fallbackReq.setRawHeader("Accept", "application/json, text/plain");
		netReq.setRawHeader("Accept", "text/event-stream, application/json, text/plain");
		QPointer<QNetworkAccessManager> namPtr(&nam);
		fallbackFactory = [namPtr, fallbackReq, buildMultipartBody, effectiveModel, safeEndpoint]() -> QNetworkReply * {
			if (!namPtr) {
				return nullptr;
			}
			qInfo() << "[AgentSpeech] Falling back to non-streaming /audio/transcriptions model="
				<< effectiveModel << "endpoint=" << QString::fromStdString(safeEndpoint);
			QHttpMultiPart *retryPart = buildMultipartBody(false);
			QNetworkReply *retryReply = namPtr->post(fallbackReq, retryPart);
			if (retryReply) {
				retryPart->setParent(retryReply);
			} else {
				delete retryPart;
			}
			return retryReply;
		};
	}

	QHttpMultiPart *multiPart = buildMultipartBody(useSse);
	qInfo() << "[AgentSpeech] Uploading utterance wavBytes=" << request.wavAudioBytes.size()
		<< "model=" << effectiveModel
		<< "stream=" << useSse
		<< "endpoint=" << QString::fromStdString(safeEndpoint);

	QNetworkReply *reply = nam.post(netReq, multiPart);
	if (reply) {
		multiPart->setParent(reply);
	} else {
		delete multiPart;
	}
	return new NetworkSpeechRequestHandle(
		reply,
		request.profile.connectTimeoutMs + 25000,
		parent,
		keyMismatch,
		false,
		std::move(fallbackFactory));
}

// =========================================================================
// AgentSpeechService
// =========================================================================

AgentSpeechService::AgentSpeechService(
		AgentCoordinator *agentCoordinator,
		MeetingCoordinator *meetingCoordinator,
		std::unique_ptr<IAgentSpeechTransport> transport,
		AudioSourceFactory audioSourceFactory,
		QObject *parent)
	: QObject(parent)
	, _agentCoordinator(agentCoordinator)
	, _meetingCoordinator(meetingCoordinator)
	, _transport(transport ? std::move(transport) : std::make_unique<OpenAiCompatibleSpeechTransport>())
	, _audioSourceFactory(audioSourceFactory ? std::move(audioSourceFactory) : createDefaultDictationAudioSource)
	, _callbackGate(QtCallbackGate<AgentSpeechService>::Create(this)) {
	qRegisterMetaType<OpenMeeting::SpeechToTextStatus>("OpenMeeting::SpeechToTextStatus");
	qRegisterMetaType<OpenMeeting::SpeechToTextEvent>("OpenMeeting::SpeechToTextEvent");
	_streamDrainTimer.setSingleShot(true);
	_streamDrainTimer.setInterval(800);
	connect(&_streamDrainTimer, &QTimer::timeout, this, [this] {
		if (_drainingStreamSessionId != 0) {
			finalizeDrainingStream(_drainingStreamSessionId, true);
		}
	});
	bindAgentCoordinator();
	bindMeetingCoordinator();
}

AgentSpeechService::~AgentSpeechService() {
	_callbackGate->Revoke();
	_streamDrainTimer.stop();
	if (_drainingStreamSession) {
		_drainingStreamSession->abort();
		_drainingStreamSession.reset();
	}
	_drainingStreamSessionId = 0;
	cleanupActiveSession(false);
}

void AgentSpeechService::setAgentCoordinator(AgentCoordinator *coordinator) {
	if (_agentCoordinator == coordinator) {
		return;
	}
	if (_active) {
		stopDictation();
	}
	_agentCoordinator = coordinator;
	bindAgentCoordinator();
}

void AgentSpeechService::bindAgentCoordinator() {
	if (_agentProfileConn) {
		disconnect(_agentProfileConn);
		_agentProfileConn = {};
	}
	if (!_agentCoordinator) {
		return;
	}
	_agentProfileConn = connect(
		_agentCoordinator,
		&AgentCoordinator::profileChanged,
		this,
		&AgentSpeechService::handleAgentConfigChanged);
}

QString AgentSpeechService::buildSpeechConfigSignature() const {
	if (!_agentCoordinator) {
		return {};
	}
	return QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9|%10|%11|%12|%13|%14")
		.arg(static_cast<int>(_agentCoordinator->speechSourceMode()))
		.arg(_agentCoordinator->activeSpeechProfileId().trimmed())
		.arg(static_cast<int>(_agentCoordinator->effectiveDictationSpeechProtocol()))
		.arg(_agentCoordinator->dictationSttBaseUrl().trimmed())
		.arg(_agentCoordinator->profile().baseUrl.trimmed())
		.arg(_agentCoordinator->resolvedSpeechModel().trimmed())
		.arg(_agentCoordinator->dictationLanguage().trimmed())
		.arg(_agentCoordinator->dictationPrompt().trimmed())
		.arg(_agentCoordinator->dictationSttResourceId().trimmed())
		.arg(_agentCoordinator->dictationSttWorkspace().trimmed())
		.arg(_agentCoordinator->hasDictationApiKey() ? 1 : 0)
		.arg(_agentCoordinator->hasDictationAppKey() ? 1 : 0)
		.arg(_agentCoordinator->dictationVadThresholdDb())
		.arg(_agentCoordinator->dictationVadSilenceCommitMs());
}

void AgentSpeechService::handleAgentConfigChanged() {
	if (!_active || !_agentCoordinator) {
		return;
	}
	const QString nextSig = buildSpeechConfigSignature();
	if (nextSig == _activeSpeechConfigSignature) {
		return;
	}
	const bool vadChanged =
		(_activeVadThresholdDb != _agentCoordinator->dictationVadThresholdDb()) ||
		(_activeVadSilenceCommitMs != _agentCoordinator->dictationVadSilenceCommitMs());
	const bool wantStreaming =
		isStreamingSpeechProtocol(_agentCoordinator->effectiveDictationSpeechProtocol()) &&
		!_roomPassThroughActive.load(std::memory_order_acquire);
	const bool currentlyStreaming = (_activeStreamSession != nullptr);
	if (wantStreaming != currentlyStreaming || currentlyStreaming || (!wantStreaming && vadChanged)) {
		restartWithDevice(_currentDeviceId);
	} else {
		_activeSpeechConfigSignature = nextSig;
	}
}

void AgentSpeechService::setMeetingCoordinator(MeetingCoordinator *meeting) {
	if (_meetingCoordinator == meeting) {
		return;
	}
	_meetingCoordinator = meeting;
	bindMeetingCoordinator();
	updatePassThroughMode();
}

void AgentSpeechService::setStreamingAsrFactory(StreamingAsrFactory factory) {
	_streamingAsrFactory = std::move(factory);
}

void AgentSpeechService::bindMeetingCoordinator() {
	for (const auto &conn : _meetingConnections) {
		disconnect(conn);
	}
	_meetingConnections.clear();

	if (!_meetingCoordinator) {
		return;
	}
	_meetingConnections.push_back(connect(
		_meetingCoordinator, &MeetingCoordinator::transcriptionReceived,
		this, &AgentSpeechService::handleRoomTranscript));
	_meetingConnections.push_back(connect(
		_meetingCoordinator, &MeetingCoordinator::localAudioLevelChanged,
		this, [this](float level) {
			if (_active && _roomPassThroughActive) {
				emit audioLevelChanged(level);
			}
		}));
	_meetingConnections.push_back(connect(
		_meetingCoordinator, &MeetingCoordinator::localAudioMuteChanged,
		this, [this](bool) {
			updatePassThroughMode();
		}));
	_meetingConnections.push_back(connect(
		_meetingCoordinator, &MeetingCoordinator::stateChanged,
		this, [this] {
			updatePassThroughMode();
		}));
	_meetingConnections.push_back(connect(
		_meetingCoordinator, &MeetingCoordinator::meetingLeft,
		this, [this] {
			updatePassThroughMode();
		}));
	_meetingConnections.push_back(connect(
		_meetingCoordinator, &QObject::destroyed,
		this, [this] {
			_meetingCoordinator.clear();
			updatePassThroughMode();
		}));
}

bool AgentSpeechService::startDictation(const QString &deviceId) {
	if (_active) {
		if (_currentDeviceId != deviceId.trimmed()) {
			return restartWithDevice(deviceId);
		}
		return true;
	}

	if (_drainingStreamSession) {
		_streamDrainTimer.stop();
		_drainingStreamSession->abort();
		_drainingStreamSession.reset();
		_drainingStreamSessionId = 0;
	}

	++_sessionId;
	const quint64 sid = _sessionId;
	_currentDeviceId = deviceId.trimmed();
	{
		DictationVadSegmenter::Config vadCfg;
		if (_agentCoordinator) {
			_activeVadThresholdDb = _agentCoordinator->dictationVadThresholdDb();
			_activeVadSilenceCommitMs = _agentCoordinator->dictationVadSilenceCommitMs();
			vadCfg.thresholdDb = static_cast<float>(_activeVadThresholdDb);
			vadCfg.silenceCommitMs = _activeVadSilenceCommitMs;
		} else {
			_activeVadThresholdDb = -42;
			_activeVadSilenceCommitMs = 600;
		}
		_vadSegmenter = DictationVadSegmenter(vadCfg);
	}
	_pendingUtterances.clear();
	_nextUtteranceSeq = 1;
	_nextCommitSeq = 1;
	_lastLevelEmitMs = 0;
	_roomSawLocalTranscriptThisSession = false;
	_activeCaptureSegmentId = QStringLiteral("mic-%1-0").arg(sid);

	const bool canPassThroughNow = _meetingCoordinator &&
		_meetingCoordinator->state() == MeetingState::InMeeting &&
		!_meetingCoordinator->isLocalAudioMuted() &&
		_meetingCoordinator->hasRoomLocalTranscription();

	const bool useStreamingAsr = _agentCoordinator &&
		isStreamingSpeechProtocol(_agentCoordinator->effectiveDictationSpeechProtocol()) &&
		!canPassThroughNow;

	std::shared_ptr<IStreamingAsrSession> createdStreamSession;
	if (useStreamingAsr) {
		QString cfgErr;
		const StreamingAsrConfig streamCfg = _agentCoordinator->buildStreamingAsrConfig(&cfgErr);
		if (!cfgErr.isEmpty()) {
			SpeechToTextEvent ev;
			ev.sessionId = sid;
			ev.status = SpeechToTextStatus::Error;
			ev.errorMessage = cfgErr;
			emit speechEventReceived(ev);
			return false;
		}
		createdStreamSession = _streamingAsrFactory
			? _streamingAsrFactory(streamCfg)
			: createStreamingAsrSession(streamCfg);
		if (!createdStreamSession) {
			SpeechToTextEvent ev;
			ev.sessionId = sid;
			ev.status = SpeechToTextStatus::Error;
			ev.errorMessage = QCoreApplication::translate(
				"MeetingUI", "Failed to create streaming speech recognition session.");
			emit speechEventReceived(ev);
			return false;
		}
	}

	_audioSource = _audioSourceFactory();
	const bool hardwareSupported = _audioSource && _audioSource->isPlatformCaptureSupported();
	bool captureStarted = false;
	if (hardwareSupported) {
		const auto gate = _callbackGate;
		if (createdStreamSession) {
			const auto streamSession = createdStreamSession;
			captureStarted = _audioSource->start(
				_currentDeviceId,
				[this, gate, sid, streamSession](const int16_t *samples, size_t count) {
					if (!gate || !gate->active() || !samples || count == 0) {
						return;
					}
					const float level = DictationVadSegmenter::computeNormalizedLevel(samples, count);
					const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
					const bool shouldEmitLevel = (_lastLevelEmitMs == 0) ||
						((nowMs - _lastLevelEmitMs) >= kWaveformLevelEmitIntervalMs);
					if (shouldEmitLevel) {
						_lastLevelEmitMs = nowMs;
						gate->Post([sid, level](AgentSpeechService *self) {
							if (!self->_active || self->_sessionId != sid) {
								return;
							}
							if (!self->_roomPassThroughActive) {
								emit self->audioLevelChanged(level);
							}
						});
					}
					if (!_roomPassThroughActive) {
						streamSession->pushPcm16(samples, count, 16000);
					}
				},
				[gate, sid](bool available, const QString &safeError) {
					if (!gate || !gate->active() || available) {
						return;
					}
					gate->Post([sid, safeError](AgentSpeechService *self) {
						if (!self->_active || self->_sessionId != sid) {
							return;
						}
						if (!self->_roomPassThroughActive) {
							SpeechToTextEvent ev;
							ev.sessionId = sid;
							ev.status = SpeechToTextStatus::Error;
							ev.errorMessage = safeError;
							emit self->speechEventReceived(ev);
						}
					});
				});
		} else {
			captureStarted = _audioSource->start(
				_currentDeviceId,
				[this, gate, sid](const int16_t *samples, size_t count) {
					if (!gate || !gate->active() || !samples || count == 0) {
						return;
					}
					const auto outcome = _vadSegmenter.pushPcm16(samples, count);
					const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
					const bool shouldEmitLevel = (_lastLevelEmitMs == 0) ||
						((nowMs - _lastLevelEmitMs) >= kWaveformLevelEmitIntervalMs);
					if (shouldEmitLevel) {
						_lastLevelEmitMs = nowMs;
						const float level = outcome.normalizedLevel;
						gate->Post([sid, level](AgentSpeechService *self) {
							if (!self->_active || self->_sessionId != sid) {
								return;
							}
							if (!self->_roomPassThroughActive) {
								emit self->audioLevelChanged(level);
							}
						});
					}
					if (outcome.speechJustStarted) {
						gate->Post([sid](AgentSpeechService *self) {
							if (!self->_active || self->_sessionId != sid || self->_roomPassThroughActive) {
								return;
							}
							SpeechToTextEvent ev;
							ev.sessionId = sid;
							ev.status = SpeechToTextStatus::Recognizing;
							ev.segmentId = self->_activeCaptureSegmentId;
							emit self->speechEventReceived(ev);
						});
					}
					if (outcome.completedWav.has_value() && !outcome.completedWav->isEmpty()) {
						const QByteArray wav = *outcome.completedWav;
						gate->Post([sid, wav](AgentSpeechService *self) {
							if (!self->_active || self->_sessionId != sid || self->_roomPassThroughActive) {
								return;
							}
							const QString segId = self->_activeCaptureSegmentId;
							self->_activeCaptureSegmentId = QStringLiteral("mic-%1-%2")
								.arg(sid)
								.arg(self->_nextUtteranceSeq);
							self->submitUtteranceWav(sid, segId, wav);
						});
					}
				},
				[gate, sid](bool available, const QString &safeError) {
					if (!gate || !gate->active() || available) {
						return;
					}
					gate->Post([sid, safeError](AgentSpeechService *self) {
						if (!self->_active || self->_sessionId != sid) {
							return;
						}
						if (!self->_roomPassThroughActive) {
							SpeechToTextEvent ev;
							ev.sessionId = sid;
							ev.status = SpeechToTextStatus::Error;
							ev.errorMessage = safeError;
							emit self->speechEventReceived(ev);
						}
					});
				});
		}
	}

	if (!captureStarted && !canPassThroughNow) {
		_audioSource.reset();
		if (createdStreamSession) {
			createdStreamSession->abort();
			createdStreamSession.reset();
		}
		SpeechToTextEvent ev;
		ev.sessionId = sid;
		ev.status = SpeechToTextStatus::Error;
		ev.errorMessage = hardwareSupported
			? QCoreApplication::translate(
				"MeetingUI",
				"Unable to access the selected microphone. Check permissions or select another device.")
			: QCoreApplication::translate(
				"MeetingUI",
				"Local microphone dictation capture requires Windows WASAPI or an unmuted meeting with live transcription.");
		emit speechEventReceived(ev);
		return false;
	}

	_active = true;
	_activeSpeechConfigSignature = buildSpeechConfigSignature();
	if (createdStreamSession) {
		_activeStreamSession = std::move(createdStreamSession);
		const auto gate = _callbackGate;
		StreamingAsrCallbacks cbs;
		cbs.onResult = [gate, sid](const StreamingAsrResult &res) {
			if (!gate || !gate->active()) {
				return;
			}
			gate->Post([sid, res](AgentSpeechService *self) {
				const bool isCurrent = self->_active && self->_sessionId == sid;
				const bool isDraining = self->_drainingStreamSession && self->_drainingStreamSessionId == sid;
				if ((!isCurrent && !isDraining) || self->_roomPassThroughActive) {
					return;
				}
				const QString clean = res.text.trimmed();
				if (clean.isEmpty()) {
					return;
				}
				SpeechToTextEvent ev;
				ev.sessionId = sid;
				ev.status = res.isFinal ? SpeechToTextStatus::Recognized : SpeechToTextStatus::Recognizing;
				ev.segmentId = res.segmentId.trimmed().isEmpty()
					? QStringLiteral("ws-%1").arg(sid)
					: QStringLiteral("ws-%1-%2").arg(sid).arg(res.segmentId);
				ev.text = clean;
				emit self->speechEventReceived(ev);
			});
		};
		cbs.onError = [gate, sid](const QString &safeError) {
			if (!gate || !gate->active()) {
				return;
			}
			gate->Post([sid, safeError](AgentSpeechService *self) {
				if (self->_drainingStreamSession && self->_drainingStreamSessionId == sid) {
					self->finalizeDrainingStream(sid, false);
					return;
				}
				if (!self->_active || self->_sessionId != sid ||
					self->_roomPassThroughActive.load(std::memory_order_acquire)) {
					return;
				}
				SpeechToTextEvent ev;
				ev.sessionId = sid;
				ev.status = SpeechToTextStatus::Error;
				ev.errorMessage = safeError;
				emit self->speechEventReceived(ev);
				self->cleanupActiveSession(true);
			});
		};
		cbs.onFinished = [gate, sid]() {
			if (!gate || !gate->active()) {
				return;
			}
			gate->Post([sid](AgentSpeechService *self) {
				if (self->_drainingStreamSession && self->_drainingStreamSessionId == sid) {
					self->finalizeDrainingStream(sid, false);
					return;
				}
				if (self->_active && self->_sessionId == sid &&
					!self->_roomPassThroughActive.load(std::memory_order_acquire)) {
					self->cleanupActiveSession(true);
				}
			});
		};
		_activeStreamSession->start(std::move(cbs));
	}

	updatePassThroughMode();
	emit dictationActiveChanged(true);

	SpeechToTextEvent startedEv;
	startedEv.sessionId = sid;
	startedEv.status = SpeechToTextStatus::Started;
	startedEv.fromMeetingTranscript = _roomPassThroughActive;
	emit speechEventReceived(startedEv);
	return true;
}

void AgentSpeechService::stopDictation() {
	if (!_active) {
		return;
	}
	if (_activeStreamSession && !_roomPassThroughActive) {
		const quint64 endedSessionId = _sessionId;
		++_sessionId;
		_active = false;
		_activeSpeechConfigSignature.clear();
		if (_audioSource) {
			_audioSource->stop();
			_audioSource.reset();
		}
		_vadSegmenter.reset();
		emit audioLevelChanged(0.0f);
		emit dictationActiveChanged(false);

		_drainingStreamSession = std::move(_activeStreamSession);
		_drainingStreamSessionId = endedSessionId;
		_streamDrainTimer.start();
		_drainingStreamSession->finish();
		return;
	}
	cleanupActiveSession(true);
}

void AgentSpeechService::finalizeDrainingStream(quint64 expectedSessionId, bool abortSession) {
	if (_drainingStreamSessionId != expectedSessionId) {
		return;
	}
	_streamDrainTimer.stop();
	auto draining = std::move(_drainingStreamSession);
	_drainingStreamSessionId = 0;
	if (draining) {
		if (abortSession) {
			draining->abort();
		}
		draining.reset();
	}
	SpeechToTextEvent ev;
	ev.sessionId = expectedSessionId;
	ev.status = SpeechToTextStatus::Stopped;
	emit speechEventReceived(ev);
}

bool AgentSpeechService::restartWithDevice(const QString &deviceId) {
	const QString clean = deviceId.trimmed();
	if (!_active) {
		_currentDeviceId = clean;
		return false;
	}
	cleanupActiveSession(false);
	return startDictation(clean);
}

void AgentSpeechService::cleanupActiveSession(bool emitStopped) {
	const quint64 endedSessionId = _sessionId;
	++_sessionId;
	const bool wasActive = _active;
	_active = false;
	_activeSpeechConfigSignature.clear();

	// Stop audio capture (joins worker thread) before touching _vadSegmenter.
	if (_audioSource) {
		_audioSource->stop();
		_audioSource.reset();
	}
	_vadSegmenter.reset();
	if (_activeStreamSession) {
		_activeStreamSession->abort();
		_activeStreamSession.reset();
	}
	if (_drainingStreamSession) {
		_streamDrainTimer.stop();
		_drainingStreamSession->abort();
		_drainingStreamSession.reset();
		_drainingStreamSessionId = 0;
	}
	for (const auto &h : _inFlightHandles) {
		if (h) {
			h->disconnect(this);
			h->cancel();
			h->deleteLater();
		}
	}
	_inFlightHandles.clear();
	_pendingUtterances.clear();

	if (_roomPassThroughActive.load(std::memory_order_acquire)) {
		_roomPassThroughActive.store(false, std::memory_order_release);
		emit roomPassThroughChanged(false);
	}
	emit audioLevelChanged(0.0f);
	if (wasActive) {
		emit dictationActiveChanged(false);
	}
	if (emitStopped && wasActive) {
		SpeechToTextEvent ev;
		ev.sessionId = endedSessionId;
		ev.status = SpeechToTextStatus::Stopped;
		emit speechEventReceived(ev);
	}
}

void AgentSpeechService::submitUtteranceWav(
		quint64 sessionId,
		const QString &segmentId,
		const QByteArray &wavBytes) {
	if (!_active || _sessionId != sessionId || !_agentCoordinator || !_transport) {
		return;
	}
	if (isStreamingSpeechProtocol(_agentCoordinator->effectiveDictationSpeechProtocol())) {
		if (!_activeStreamSession && !_roomPassThroughActive.load(std::memory_order_acquire)) {
			restartWithDevice(_currentDeviceId);
		}
		return;
	}
	QString keyErr;
	const QString apiKey = _agentCoordinator->loadApiKeyForSpeech(&keyErr);
	if (!keyErr.isEmpty()) {
		SpeechToTextEvent ev;
		ev.sessionId = sessionId;
		ev.status = SpeechToTextStatus::Error;
		ev.errorMessage = keyErr;
		emit speechEventReceived(ev);
		return;
	}

	const quint64 seq = _nextUtteranceSeq++;
	_pendingUtterances.insert(seq, UtteranceSlot{ seq, false, false, {}, {} });

	SpeechTranscriptionRequest req;
	req.profile = _agentCoordinator->profile();
	req.apiKey = apiKey;
	req.customSttBaseUrl = _agentCoordinator->dictationSttBaseUrl();
	req.protocolMode = _agentCoordinator->dictationSpeechProtocol();
	req.sttModel = _agentCoordinator->resolvedSpeechModel();
	req.sttModelCapabilities = _agentCoordinator->resolveSpeechModelCapabilities(req.sttModel);
	req.language = _agentCoordinator->dictationLanguage();
	req.prompt = _agentCoordinator->dictationPrompt();
	const QString effectiveBase = req.customSttBaseUrl.trimmed().isEmpty()
		? req.profile.baseUrl.trimmed()
		: req.customSttBaseUrl.trimmed();
	const QString endpointHost = QUrl(effectiveBase, QUrl::StrictMode).host().toLower();
	const AgentSpeechModelInfo modelInfo = classifySpeechModel(
		req.sttModel, endpointHost, req.sttModelCapabilities);
	const bool wantsChatInputAudio =
		(req.protocolMode == AgentSpeechProtocolMode::ChatInputAudio) ||
		(req.protocolMode == AgentSpeechProtocolMode::Auto &&
		 modelInfo.kind == AgentSpeechModelKind::SyncChatInputAudio);
	const bool wantsMultipart =
		(req.protocolMode == AgentSpeechProtocolMode::OpenAiTranscriptions) ||
		(req.protocolMode == AgentSpeechProtocolMode::Auto &&
		 modelInfo.kind == AgentSpeechModelKind::SyncMultipart);
	req.streamResponse =
		(wantsMultipart &&
		 OpenAiCompatibleSpeechTransport::supportsMultipartTranscriptionSse(req.sttModel, endpointHost)) ||
		(wantsChatInputAudio &&
		 OpenAiCompatibleSpeechTransport::supportsChatInputAudioSse(req.sttModel, endpointHost));
	req.wavAudioBytes = wavBytes;

	QPointer<AgentSpeechRequestHandle> handle = _transport->transcribeWav(req, this);
	if (!handle) {
		_pendingUtterances[seq] = UtteranceSlot{ seq, true, false, {}, QStringLiteral("Failed to start speech request.") };
		flushOrderedUtterances();
		return;
	}
	_inFlightHandles.push_back(handle);
	connect(handle, &AgentSpeechRequestHandle::deltaReceived, this,
		[this, sessionId, seq](const QString &accumulatedText) {
			if (!_active || _sessionId != sessionId || _roomPassThroughActive.load(std::memory_order_acquire)) {
				return;
			}
			if (seq != _nextCommitSeq || !_pendingUtterances.contains(seq)) {
				return;
			}
			const QString clean = accumulatedText.trimmed();
			if (clean.isEmpty()) {
				return;
			}
			SpeechToTextEvent ev;
			ev.sessionId = sessionId;
			ev.status = SpeechToTextStatus::Recognizing;
			ev.segmentId = QStringLiteral("stt-%1-%2").arg(sessionId).arg(seq);
			ev.text = clean;
			emit speechEventReceived(ev);
		});
	connect(handle, &AgentSpeechRequestHandle::finished, this,
		[this, sessionId, seq, segmentId, handle](bool success, const QString &text, const QString &safeError) {
			_inFlightHandles.removeAll(handle);
			if (handle) {
				handle->deleteLater();
			}
			if (!_active || _sessionId != sessionId || !_pendingUtterances.contains(seq)) {
				return;
			}
			UtteranceSlot &slot = _pendingUtterances[seq];
			slot.completed = true;
			slot.success = success;
			slot.text = text;
			slot.errorMessage = safeError;
			Q_UNUSED(segmentId);
			flushOrderedUtterances();
		});
}

void AgentSpeechService::flushOrderedUtterances() {
	while (_pendingUtterances.contains(_nextCommitSeq)) {
		const UtteranceSlot slot = _pendingUtterances.value(_nextCommitSeq);
		if (!slot.completed) {
			break;
		}
		_pendingUtterances.remove(_nextCommitSeq);
		const QString segId = QStringLiteral("stt-%1-%2").arg(_sessionId).arg(_nextCommitSeq);
		++_nextCommitSeq;

		SpeechToTextEvent ev;
		ev.sessionId = _sessionId;
		ev.segmentId = segId;
		if (slot.success) {
			if (slot.text.trimmed().isEmpty()) {
				continue;
			}
			ev.status = SpeechToTextStatus::Recognized;
			ev.text = slot.text.trimmed();
		} else {
			ev.status = SpeechToTextStatus::Error;
			ev.errorMessage = slot.errorMessage;
		}
		emit speechEventReceived(ev);
	}
}

void AgentSpeechService::handleRoomTranscript(const OpenMeeting::MeetingTranscriptItem &item) {
	if (!_active || !item.isLocalSpeaker) {
		return;
	}
	// Privacy guard: NEVER reuse room transcription if user intentionally muted meeting mic.
	if (!_meetingCoordinator || _meetingCoordinator->state() != MeetingState::InMeeting ||
		_meetingCoordinator->isLocalAudioMuted()) {
		return;
	}
	_roomSawLocalTranscriptThisSession = true;
	updatePassThroughMode();

	const QString clean = item.text.trimmed();
	if (clean.isEmpty()) {
		return;
	}

	SpeechToTextEvent ev;
	ev.sessionId = _sessionId;
	ev.status = item.isFinal ? SpeechToTextStatus::Recognized : SpeechToTextStatus::Recognizing;
	ev.segmentId = item.segmentId;
	ev.text = clean;
	ev.fromMeetingTranscript = true;
	emit speechEventReceived(ev);
}

void AgentSpeechService::updatePassThroughMode() {
	const bool shouldPassThrough = _active && _meetingCoordinator &&
		_meetingCoordinator->state() == MeetingState::InMeeting &&
		!_meetingCoordinator->isLocalAudioMuted() &&
		(_meetingCoordinator->hasRoomLocalTranscription() || _roomSawLocalTranscriptThisSession);

	const bool wasPassThrough = _roomPassThroughActive.load(std::memory_order_acquire);
	if (wasPassThrough == shouldPassThrough) {
		return;
	}
	_roomPassThroughActive.store(shouldPassThrough, std::memory_order_release);
	emit roomPassThroughChanged(shouldPassThrough);
	if (!wasPassThrough && shouldPassThrough && _activeStreamSession) {
		_activeStreamSession->abort();
		_activeStreamSession.reset();
	}
	if (wasPassThrough && !shouldPassThrough && _active && _agentCoordinator &&
		isStreamingSpeechProtocol(_agentCoordinator->effectiveDictationSpeechProtocol()) &&
		!_activeStreamSession) {
		restartWithDevice(_currentDeviceId);
	}
}

} // namespace OpenMeeting
