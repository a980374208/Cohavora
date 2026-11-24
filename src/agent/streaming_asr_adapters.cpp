#include "src/agent/streaming_asr_adapters.h"

#include "src/signal/websocket_client.h"
#include "src/telemetry/log_redaction.h"
#include "src/telemetry/safe_spawn.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtCore/QUuid>

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace OpenMeeting {
namespace {

constexpr size_t kMaxPreReadyChunks = 20; // ~2.0 s of 100 ms chunks
constexpr size_t kMaxDecompressBytes = 4 * 1024 * 1024;

void writeBe32(uint8_t *dst, uint32_t value) {
	dst[0] = static_cast<uint8_t>((value >> 24) & 0xFFu);
	dst[1] = static_cast<uint8_t>((value >> 16) & 0xFFu);
	dst[2] = static_cast<uint8_t>((value >> 8) & 0xFFu);
	dst[3] = static_cast<uint8_t>(value & 0xFFu);
}

uint32_t readBe32(const uint8_t *src) {
	return (static_cast<uint32_t>(src[0]) << 24) |
		(static_cast<uint32_t>(src[1]) << 16) |
		(static_cast<uint32_t>(src[2]) << 8) |
		static_cast<uint32_t>(src[3]);
}

bool isReservedCustomWsHeader(const QString &name) {
	const QString lower = name.trimmed().toLower();
	return lower == QStringLiteral("host") ||
		lower == QStringLiteral("upgrade") ||
		lower == QStringLiteral("connection") ||
		lower == QStringLiteral("user-agent") ||
		lower == QStringLiteral("sec-websocket-key") ||
		lower == QStringLiteral("sec-websocket-version") ||
		lower == QStringLiteral("sec-websocket-accept") ||
		lower == QStringLiteral("authorization") ||
		lower == QStringLiteral("xi-api-key") ||
		lower == QStringLiteral("x-dashscope-workspace") ||
		lower.startsWith(QStringLiteral("x-api-"));
}

QString normalizeModelBasenameLower(const QString &model) {
	QString clean = model.trimmed().toLower();
	const int slash = clean.lastIndexOf(QLatin1Char('/'));
	if (slash >= 0 && slash + 1 < clean.size()) {
		clean = clean.mid(slash + 1);
	}
	return clean;
}

bool openAiRealtimeDisallowsServerVad(const QString &modelId) {
	const QString m = normalizeModelBasenameLower(modelId);
	return m.contains(QStringLiteral("-live-transcribe")) ||
		m.contains(QStringLiteral("-realtime-whisper"));
}

QJsonArray parseAssemblyAiLanguageCodes(const QString &raw) {
	QJsonArray arr;
	QStringList seen;
	QString normalized = raw;
	for (QChar &ch : normalized) {
		if (ch == QLatin1Char(';') || ch.isSpace()) {
			ch = QLatin1Char(',');
		}
	}
	const QStringList parts = normalized.split(QLatin1Char(','), Qt::SkipEmptyParts);
	for (const QString &part : parts) {
		QString token = part.trimmed().toLower();
		const int dashIdx = token.indexOf(QLatin1Char('-'));
		const int underIdx = token.indexOf(QLatin1Char('_'));
		int splitIdx = -1;
		if (dashIdx >= 0 && underIdx >= 0) {
			splitIdx = std::min(dashIdx, underIdx);
		} else {
			splitIdx = std::max(dashIdx, underIdx);
		}
		if (splitIdx >= 0) {
			token = token.left(splitIdx);
		}
		token = token.trimmed();
		if (token.isEmpty() || token == QStringLiteral("auto")) {
			continue;
		}
		if (!seen.contains(token)) {
			seen.push_back(token);
			arr.push_back(token);
		}
	}
	return arr;
}

QStringList parseDeepgramPromptTerms(const QString &raw, bool stripIntensifier) {
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
		if (stripIntensifier) {
			const int colonIdx = term.lastIndexOf(QLatin1Char(':'));
			if (colonIdx > 0 && colonIdx + 1 < term.size()) {
				bool ok = false;
				const double weight = term.mid(colonIdx + 1).trimmed().toDouble(&ok);
				if (ok && std::isfinite(weight)) {
					term = term.left(colonIdx).trimmed();
				}
			}
		}
		if (term.isEmpty()) {
			continue;
		}
		const QString lower = term.toLower();
		if (seenLower.contains(lower)) {
			continue;
		}
		if (out.size() >= 50 || totalChars + term.size() > 400) {
			break;
		}
		seenLower.push_back(lower);
		out.push_back(term);
		totalChars += term.size();
	}
	return out;
}

QString joinDeepgramTranscriptParts(const QStringList &parts, const QString &extra = QString()) {
	QString combined;
	auto appendPiece = [&](const QString &rawPiece) {
		const QString piece = rawPiece.trimmed();
		if (piece.isEmpty()) {
			return;
		}
		if (combined.isEmpty()) {
			combined = piece;
			return;
		}
		const QChar prevChar = combined.back();
		const QChar nextChar = piece.front();
		const bool adjacentCjk = prevChar.unicode() >= 0x2E80 && nextChar.unicode() >= 0x2E80;
		if (!adjacentCjk) {
			combined.append(QLatin1Char(' '));
		}
		combined.append(piece);
	};
	for (const QString &p : parts) {
		appendPiece(p);
	}
	if (!extra.isEmpty()) {
		appendPiece(extra);
	}
	return combined;
}

QString extractDeepgramDetectedLanguage(
		const QJsonObject &altObj,
		const QJsonObject &channelObj,
		const QString &fallbackLang) {
	const QJsonArray langs = altObj.value(QStringLiteral("languages")).toArray();
	if (!langs.isEmpty()) {
		const QString first = langs.first().toString().trimmed();
		if (!first.isEmpty()) {
			return first;
		}
	}
	const QString channelDetected = channelObj.value(QStringLiteral("detected_language")).toString().trimmed();
	if (!channelDetected.isEmpty()) {
		return channelDetected;
	}
	const QJsonArray words = altObj.value(QStringLiteral("words")).toArray();
	if (!words.isEmpty()) {
		const QString wordLang = words.first().toObject().value(QStringLiteral("language")).toString().trimmed();
		if (!wordLang.isEmpty()) {
			return wordLang;
		}
	}
	const QString cleanFallback = fallbackLang.trimmed();
	if (cleanFallback.compare(QStringLiteral("auto"), Qt::CaseInsensitive) == 0) {
		return {};
	}
	return cleanFallback;
}

QString normalizeIso639PrimaryLanguage(const QString &raw) {
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

float computePcm16RmsNormalized(const uint8_t *pcmBytes, size_t byteCount) {
	const size_t sampleCount = byteCount / sizeof(int16_t);
	if (!pcmBytes || sampleCount == 0) {
		return 0.0f;
	}
	double sumSq = 0.0;
	for (size_t i = 0; i < sampleCount; ++i) {
		const uint16_t raw = static_cast<uint16_t>(pcmBytes[i * 2]) |
			(static_cast<uint16_t>(pcmBytes[i * 2 + 1]) << 8);
		const int16_t s = static_cast<int16_t>(raw);
		const double norm = static_cast<double>(s) / 32768.0;
		sumSq += norm * norm;
	}
	return static_cast<float>(std::sqrt(sumSq / static_cast<double>(sampleCount)));
}

bool isNarrowband8kModel(const QString &model) {
	const QString m = normalizeModelBasenameLower(model);
	return m.contains(QStringLiteral("-8k-")) ||
		m.contains(QStringLiteral("_8k_")) ||
		m.endsWith(QStringLiteral("-8k")) ||
		m.endsWith(QStringLiteral("_8k"));
}

bool isLegacyV1Model(const QString &model) {
	const QString m = normalizeModelBasenameLower(model);
	return m.endsWith(QStringLiteral("-v1")) ||
		m.endsWith(QStringLiteral("_v1"));
}

void appendCustomHeaders(
		const StreamingAsrConfig &config,
		std::vector<std::pair<std::string, std::string>> &outHeaders) {
	for (auto it = config.customHeaders.cbegin(); it != config.customHeaders.cend(); ++it) {
		const QString key = it.key().trimmed();
		const QString val = it.value().trimmed();
		if (isValidCustomHeaderName(key) &&
			isValidCustomHeaderValue(val) &&
			!isReservedCustomWsHeader(key)) {
			outHeaders.emplace_back(key.toStdString(), val.toStdString());
		}
	}
}

QString sanitizeStreamingError(const QString &raw, int httpStatus = 0) {
	const QString redacted = redactEmbeddedApiKeys(raw.trimmed().left(512));
	const QByteArray utf8 = redacted.toUtf8();
	const auto sanitized = livekit::secure_log::SanitizeForOutput(
		std::string_view(utf8.constData(), static_cast<size_t>(utf8.size())));
	const QString clean = QString::fromUtf8(sanitized.data(), static_cast<int>(sanitized.size())).trimmed();
	if (httpStatus > 0) {
		QString msg = clean.isEmpty()
			? QStringLiteral("WebSocket upgrade failed (HTTP %1).").arg(httpStatus)
			: QStringLiteral("HTTP %1: %2").arg(httpStatus).arg(clean);
		if (httpStatus == 401 || httpStatus == 403) {
			msg += QLatin1Char('\n') + QCoreApplication::translate(
				"MeetingUI",
				"Hint: Streaming ASR authentication failed. Check the API key, App Key/Resource ID (if Volcengine), and permissions in Copilot Settings.");
		} else if (httpStatus == 404) {
			msg += QLatin1Char('\n') + QCoreApplication::translate(
				"MeetingUI",
				"Hint: The WebSocket streaming ASR endpoint returned HTTP 404. Verify the Speech-to-Text URL and selected STT Protocol in Copilot Settings.");
		} else if (httpStatus == 429) {
			msg += QLatin1Char('\n') + QCoreApplication::translate(
				"MeetingUI",
				"Hint: Streaming ASR quota or rate limit exceeded.");
		}
		return msg;
	}
	return clean.isEmpty()
		? QCoreApplication::translate("MeetingUI", "Streaming speech recognition connection failed.")
		: clean;
}

QString extractJsonErrorMessage(const QJsonObject &root) {
	const QJsonValue errVal = root.value(QStringLiteral("error"));
	if (errVal.isString()) {
		return errVal.toString().trimmed();
	}
	if (errVal.isObject()) {
		const QString msg = errVal.toObject().value(QStringLiteral("message")).toString().trimmed();
		if (!msg.isEmpty()) {
			return msg;
		}
		const QString code = errVal.toObject().value(QStringLiteral("code")).toString().trimmed();
		if (!code.isEmpty()) {
			return code;
		}
	}
	for (const char *k : { "message", "error_message", "detail", "reason" }) {
		const QString v = root.value(QLatin1String(k)).toString().trimmed();
		if (!v.isEmpty()) {
			return v;
		}
	}
	return {};
}

QUrl normalizeWsScheme(QUrl url) {
	const QString s = url.scheme().toLower();
	if (s == QStringLiteral("https")) {
		url.setScheme(QStringLiteral("wss"));
	} else if (s == QStringLiteral("http")) {
		url.setScheme(QStringLiteral("ws"));
	}
	return url;
}

QString makeBase64Pcm(const std::vector<uint8_t> &pcmBytes) {
	return QString::fromLatin1(QByteArray(
		reinterpret_cast<const char *>(pcmBytes.data()),
		static_cast<int>(pcmBytes.size())).toBase64());
}

} // namespace

// =========================================================================
// Volcengine Gzip & SAUC v3 Binary Codec
// =========================================================================

std::vector<uint8_t> gzipCompressBytes(const uint8_t *data, size_t size, bool *ok) {
	if (ok) *ok = false;
	z_stream zs{};
	if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
		return {};
	}
	zs.next_in = const_cast<Bytef *>(reinterpret_cast<const Bytef *>(data));
	zs.avail_in = static_cast<uInt>(size);

	std::vector<uint8_t> out;
	uint8_t buf[8192];
	int ret = Z_OK;
	do {
		zs.next_out = buf;
		zs.avail_out = sizeof(buf);
		ret = deflate(&zs, Z_FINISH);
		if (ret == Z_STREAM_ERROR) {
			deflateEnd(&zs);
			return {};
		}
		const size_t produced = sizeof(buf) - zs.avail_out;
		if (produced > 0) {
			out.insert(out.end(), buf, buf + produced);
		}
	} while (zs.avail_out == 0);

	deflateEnd(&zs);
	if (ret != Z_STREAM_END) {
		return {};
	}
	if (ok) *ok = true;
	return out;
}

std::vector<uint8_t> gzipDecompressBytes(const uint8_t *data, size_t size, bool *ok) {
	if (ok) *ok = false;
	if (!data || size == 0) {
		if (ok) *ok = true;
		return {};
	}
	z_stream zs{};
	if (inflateInit2(&zs, 15 + 32) != Z_OK) {
		return {};
	}
	zs.next_in = const_cast<Bytef *>(reinterpret_cast<const Bytef *>(data));
	zs.avail_in = static_cast<uInt>(size);

	std::vector<uint8_t> out;
	uint8_t buf[8192];
	int ret = Z_OK;
	do {
		zs.next_out = buf;
		zs.avail_out = sizeof(buf);
		ret = inflate(&zs, Z_NO_FLUSH);
		if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR || ret == Z_NEED_DICT) {
			inflateEnd(&zs);
			return {};
		}
		const size_t produced = sizeof(buf) - zs.avail_out;
		if (produced > 0) {
			if (out.size() + produced > kMaxDecompressBytes) {
				inflateEnd(&zs);
				return {};
			}
			out.insert(out.end(), buf, buf + produced);
		}
	} while (ret != Z_STREAM_END && zs.avail_in > 0);

	inflateEnd(&zs);
	if (ret != Z_STREAM_END && out.empty()) {
		return {};
	}
	if (ok) *ok = true;
	return out;
}

std::vector<uint8_t> buildVolcengineFullClientPacket(
		const QByteArray &jsonPayload,
		int32_t sequence,
		bool useGzip) {
	std::vector<uint8_t> body;
	if (useGzip) {
		bool ok = false;
		body = gzipCompressBytes(
			reinterpret_cast<const uint8_t *>(jsonPayload.constData()),
			static_cast<size_t>(jsonPayload.size()),
			&ok);
		if (!ok) {
			useGzip = false;
		}
	}
	if (!useGzip) {
		body.assign(jsonPayload.cbegin(), jsonPayload.cend());
	}

	std::vector<uint8_t> pkt(12 + body.size());
	pkt[0] = 0x11; // version=1, header_size=1 (4 bytes)
	pkt[1] = 0x11; // msgType=1 (FullClientRequest), flags=1 (PosSequence)
	pkt[2] = static_cast<uint8_t>((0x1 << 4) | (useGzip ? 0x1 : 0x0)); // serial=1 JSON, comp=0|1
	pkt[3] = 0x00;
	writeBe32(&pkt[4], static_cast<uint32_t>(sequence));
	writeBe32(&pkt[8], static_cast<uint32_t>(body.size()));
	if (!body.empty()) {
		std::memcpy(&pkt[12], body.data(), body.size());
	}
	return pkt;
}

std::vector<uint8_t> buildVolcengineAudioPacket(
		const uint8_t *pcmBytes,
		size_t size,
		int32_t sequence,
		bool isLast,
		bool useGzip) {
	std::vector<uint8_t> body;
	if (useGzip) {
		bool ok = false;
		body = gzipCompressBytes(pcmBytes, size, &ok);
		if (!ok) {
			useGzip = false;
		}
	}
	if (!useGzip && pcmBytes && size > 0) {
		body.assign(pcmBytes, pcmBytes + size);
	}

	const uint8_t flags = isLast ? 0x03 : 0x01;
	const int32_t encodedSeq = isLast ? -std::abs(sequence) : std::abs(sequence);

	std::vector<uint8_t> pkt(12 + body.size());
	pkt[0] = 0x11;
	pkt[1] = static_cast<uint8_t>((0x2 << 4) | flags); // msgType=2 (AudioOnlyRequest)
	pkt[2] = static_cast<uint8_t>(useGzip ? 0x01 : 0x00); // serial=0 Raw, comp=0|1
	pkt[3] = 0x00;
	writeBe32(&pkt[4], static_cast<uint32_t>(encodedSeq));
	writeBe32(&pkt[8], static_cast<uint32_t>(body.size()));
	if (!body.empty()) {
		std::memcpy(&pkt[12], body.data(), body.size());
	}
	return pkt;
}

VolcengineSaucServerPacket parseVolcengineServerPacket(const std::vector<uint8_t> &packet) {
	VolcengineSaucServerPacket out;
	if (packet.size() < 8) {
		return out;
	}
	const size_t headerBytes = static_cast<size_t>(packet[0] & 0x0F) * 4;
	if (headerBytes < 4 || packet.size() < headerBytes + 4) {
		return out;
	}
	out.messageType = (packet[1] >> 4) & 0x0F;
	out.flags = packet[1] & 0x0F;
	out.serialization = (packet[2] >> 4) & 0x0F;
	out.compression = packet[2] & 0x0F;
	out.isFinalPacket = (out.flags & 0x02) != 0;

	size_t offset = headerBytes;
	if (out.messageType == 0x0F) {
		out.isError = true;
		if (offset + 8 > packet.size()) {
			return out;
		}
		out.errorCode = readBe32(&packet[offset]);
		offset += 4;
		const uint32_t payloadLen = readBe32(&packet[offset]);
		offset += 4;
		if (offset + payloadLen > packet.size()) {
			return out;
		}
		if (out.compression == 0x01) {
			bool ok = false;
			const auto raw = gzipDecompressBytes(&packet[offset], payloadLen, &ok);
			if (!ok) return out;
			out.payload = QByteArray(reinterpret_cast<const char *>(raw.data()), static_cast<int>(raw.size()));
		} else {
			out.payload = QByteArray(reinterpret_cast<const char *>(&packet[offset]), static_cast<int>(payloadLen));
		}
		out.valid = true;
		return out;
	}

	if (out.flags == 0x01 || out.flags == 0x03) {
		if (offset + 4 > packet.size()) {
			return out;
		}
		out.sequence = static_cast<int32_t>(readBe32(&packet[offset]));
		offset += 4;
	}
	if (offset + 4 > packet.size()) {
		return out;
	}
	const uint32_t payloadLen = readBe32(&packet[offset]);
	offset += 4;
	if (offset + payloadLen > packet.size()) {
		return out;
	}
	if (out.compression == 0x01) {
		bool ok = false;
		const auto raw = gzipDecompressBytes(&packet[offset], payloadLen, &ok);
		if (!ok) {
			return out;
		}
		out.payload = QByteArray(reinterpret_cast<const char *>(raw.data()), static_cast<int>(raw.size()));
	} else {
		out.payload = QByteArray(reinterpret_cast<const char *>(&packet[offset]), static_cast<int>(payloadLen));
	}
	out.valid = true;
	return out;
}

// =========================================================================
// URL Resolver Shared Helper
// =========================================================================

QUrl resolveStreamingSpeechEndpointUrl(
		AgentSpeechProtocolMode protocol,
		const QString &configuredUrl,
		const QString &model,
		const QString &language,
		QString *outError,
		const QString &prompt) {
	if (outError) {
		outError->clear();
	}
	const QString rawInput = configuredUrl.trimmed().isEmpty()
		? defaultStreamingSpeechEndpoint(protocol)
		: configuredUrl.trimmed();
	const AgentEndpointDecision ep = evaluateAgentSpeechEndpoint(rawInput);
	if (!ep.allowed) {
		if (outError) {
			*outError = ep.reason;
		}
		return {};
	}

	QUrl url = normalizeWsScheme(ep.normalizedUrl);
	QString path = url.path();
	if (path.isEmpty()) {
		path = QStringLiteral("/");
	}
	QUrlQuery query(url);

	switch (protocol) {
	case AgentSpeechProtocolMode::OpenAiRealtimeWs: {
		if (!path.contains(QStringLiteral("/realtime"), Qt::CaseInsensitive)) {
			if (path == QStringLiteral("/") ||
				path == QStringLiteral("/v1") ||
				path == QStringLiteral("/v1/") ||
				path.endsWith(QStringLiteral("/audio/transcriptions"), Qt::CaseInsensitive) ||
				path.endsWith(QStringLiteral("/chat/completions"), Qt::CaseInsensitive)) {
				path = QStringLiteral("/v1/realtime");
			} else if (path.endsWith(QLatin1Char('/'))) {
				path += QStringLiteral("realtime");
			} else {
				path += QStringLiteral("/realtime");
			}
			url.setPath(path);
		}
		if (!query.hasQueryItem(QStringLiteral("intent"))) {
			query.addQueryItem(QStringLiteral("intent"), QStringLiteral("transcription"));
		}
		url.setQuery(query);
		break;
	}
	case AgentSpeechProtocolMode::DashScopeQwenRealtimeWs: {
		if (!path.contains(QStringLiteral("/realtime"), Qt::CaseInsensitive)) {
			url.setPath(QStringLiteral("/api-ws/v1/realtime"));
		}
		const QString effectiveModel = model.trimmed().isEmpty()
			? QStringLiteral("qwen3-asr-flash-realtime")
			: model.trimmed();
		if (!query.hasQueryItem(QStringLiteral("model"))) {
			query.addQueryItem(QStringLiteral("model"), effectiveModel);
		}
		url.setQuery(query);
		break;
	}
	case AgentSpeechProtocolMode::DashScopeInferenceWs: {
		if (!path.contains(QStringLiteral("/api-ws/"), Qt::CaseInsensitive) &&
			!path.contains(QStringLiteral("/inference"), Qt::CaseInsensitive)) {
			url.setPath(QStringLiteral("/api-ws/v1/inference"));
		}
		break;
	}
	case AgentSpeechProtocolMode::DeepgramWs: {
		if (!path.contains(QStringLiteral("/v1/listen"), Qt::CaseInsensitive) &&
			(path == QStringLiteral("/") ||
			 path == QStringLiteral("/v1") ||
			 path == QStringLiteral("/v1/"))) {
			url.setPath(QStringLiteral("/v1/listen"));
		}
		const QString effectiveModel = model.trimmed().isEmpty()
			? QStringLiteral("nova-3")
			: model.trimmed();
		if (!query.hasQueryItem(QStringLiteral("model"))) {
			query.addQueryItem(QStringLiteral("model"), effectiveModel);
		}
		if (!query.hasQueryItem(QStringLiteral("encoding"))) {
			query.addQueryItem(QStringLiteral("encoding"), QStringLiteral("linear16"));
		}
		if (!query.hasQueryItem(QStringLiteral("sample_rate"))) {
			query.addQueryItem(QStringLiteral("sample_rate"), QStringLiteral("16000"));
		}
		if (!query.hasQueryItem(QStringLiteral("channels"))) {
			query.addQueryItem(QStringLiteral("channels"), QStringLiteral("1"));
		}
		if (!query.hasQueryItem(QStringLiteral("interim_results"))) {
			query.addQueryItem(QStringLiteral("interim_results"), QStringLiteral("true"));
		}
		if (!query.hasQueryItem(QStringLiteral("smart_format"))) {
			query.addQueryItem(QStringLiteral("smart_format"), QStringLiteral("true"));
		}
		if (!query.hasQueryItem(QStringLiteral("endpointing"))) {
			query.addQueryItem(QStringLiteral("endpointing"), QStringLiteral("300"));
		}
		const QString interimVal = query.queryItemValue(QStringLiteral("interim_results")).trimmed().toLower();
		const QString endpointingVal = query.queryItemValue(QStringLiteral("endpointing")).trimmed().toLower();
		const bool interimEnabled = (interimVal != QStringLiteral("false") && interimVal != QStringLiteral("0"));
		const bool endpointingEnabled = (endpointingVal != QStringLiteral("false") && endpointingVal != QStringLiteral("0"));
		if (interimEnabled && endpointingEnabled && !query.hasQueryItem(QStringLiteral("utterance_end_ms"))) {
			query.addQueryItem(QStringLiteral("utterance_end_ms"), QStringLiteral("1000"));
		}
		QString effectiveModelLower = query.queryItemValue(QStringLiteral("model")).trimmed().toLower();
		if (effectiveModelLower.isEmpty()) {
			effectiveModelLower = effectiveModel.toLower();
		}
		if (!query.hasQueryItem(QStringLiteral("language"))) {
			const QString cleanLang = language.trimmed();
			if (!cleanLang.isEmpty() && cleanLang.compare(QStringLiteral("auto"), Qt::CaseInsensitive) != 0) {
				query.addQueryItem(QStringLiteral("language"), cleanLang);
			} else if ((effectiveModelLower.startsWith(QStringLiteral("nova-3")) ||
						effectiveModelLower.startsWith(QStringLiteral("nova-2"))) &&
					   !effectiveModelLower.contains(QStringLiteral("-medical")) &&
					   !effectiveModelLower.endsWith(QStringLiteral("-en"))) {
				query.addQueryItem(QStringLiteral("language"), QStringLiteral("multi"));
			}
		}
		if (!prompt.trimmed().isEmpty() &&
			!query.hasQueryItem(QStringLiteral("keyterm")) &&
			!query.hasQueryItem(QStringLiteral("keywords"))) {
			const bool useKeyterm = effectiveModelLower.startsWith(QStringLiteral("nova-3"));
			const QStringList terms = parseDeepgramPromptTerms(prompt, useKeyterm);
			const QString paramName = useKeyterm ? QStringLiteral("keyterm") : QStringLiteral("keywords");
			for (const QString &term : terms) {
				query.addQueryItem(paramName, term);
			}
		}
		url.setQuery(query);
		break;
	}
	case AgentSpeechProtocolMode::AssemblyAiV3Ws: {
		if (!path.contains(QStringLiteral("/v3/ws"), Qt::CaseInsensitive) &&
			(path == QStringLiteral("/") ||
			 path == QStringLiteral("/v1") ||
			 path == QStringLiteral("/v1/") ||
			 path == QStringLiteral("/v3") ||
			 path == QStringLiteral("/v3/"))) {
			url.setPath(QStringLiteral("/v3/ws"));
		}
		if (!query.hasQueryItem(QStringLiteral("sample_rate"))) {
			query.addQueryItem(QStringLiteral("sample_rate"), QStringLiteral("16000"));
		}
		if (!query.hasQueryItem(QStringLiteral("encoding"))) {
			query.addQueryItem(QStringLiteral("encoding"), QStringLiteral("pcm_s16le"));
		}
		if (!query.hasQueryItem(QStringLiteral("format_turns"))) {
			query.addQueryItem(QStringLiteral("format_turns"), QStringLiteral("true"));
		}
		QString cleanModel = model.trimmed();
		if (cleanModel.compare(QStringLiteral("universal-streaming"), Qt::CaseInsensitive) == 0) {
			cleanModel = QStringLiteral("assemblyai-stt-3-7-realtime");
		}
		if (!cleanModel.isEmpty() &&
			cleanModel.compare(QStringLiteral("default"), Qt::CaseInsensitive) != 0 &&
			!query.hasQueryItem(QStringLiteral("speech_model"))) {
			query.addQueryItem(QStringLiteral("speech_model"), cleanModel);
		}
		const QString effectiveModelLower =
			query.queryItemValue(QStringLiteral("speech_model")).trimmed().toLower();
		if (effectiveModelLower != QStringLiteral("universal-streaming-english") &&
			!query.hasQueryItem(QStringLiteral("language_detection"))) {
			query.addQueryItem(QStringLiteral("language_detection"), QStringLiteral("true"));
		}
		url.setQuery(query);
		break;
	}
	case AgentSpeechProtocolMode::VolcengineBigModelWs: {
		if (path == QStringLiteral("/") || path == QStringLiteral("/api") ||
			path == QStringLiteral("/api/v3") || path == QStringLiteral("/api/v3/")) {
			url.setPath(QStringLiteral("/api/v3/sauc/bigmodel_async"));
		}
		break;
	}
	case AgentSpeechProtocolMode::ElevenLabsScribeWs: {
		if (!path.contains(QStringLiteral("/speech-to-text/realtime"), Qt::CaseInsensitive) &&
			(path == QStringLiteral("/") ||
			 path.compare(QStringLiteral("/v1"), Qt::CaseInsensitive) == 0 ||
			 path.compare(QStringLiteral("/v1/"), Qt::CaseInsensitive) == 0 ||
			 path.compare(QStringLiteral("/v1/speech-to-text"), Qt::CaseInsensitive) == 0 ||
			 path.compare(QStringLiteral("/v1/speech-to-text/"), Qt::CaseInsensitive) == 0)) {
			url.setPath(QStringLiteral("/v1/speech-to-text/realtime"));
		}
		const QString effectiveModel = model.trimmed().isEmpty()
			? QStringLiteral("scribe_v3_live")
			: model.trimmed();
		if (!query.hasQueryItem(QStringLiteral("model_id"))) {
			query.addQueryItem(QStringLiteral("model_id"), effectiveModel);
		}
		const QString resolvedModelLower =
			query.queryItemValue(QStringLiteral("model_id")).trimmed().toLower();
		const bool isScribeV3Live =
			resolvedModelLower.startsWith(QStringLiteral("scribe_v3")) ||
			resolvedModelLower.startsWith(QStringLiteral("scribe-v3"));
		if (!query.hasQueryItem(QStringLiteral("audio_format"))) {
			query.addQueryItem(QStringLiteral("audio_format"), QStringLiteral("pcm_16000"));
		}
		if (!query.hasQueryItem(QStringLiteral("commit_strategy"))) {
			query.addQueryItem(QStringLiteral("commit_strategy"), QStringLiteral("vad"));
		}
		QString effectiveStrategy =
			query.queryItemValue(QStringLiteral("commit_strategy")).trimmed().toLower();
		if (!isScribeV3Live) {
			if (effectiveStrategy == QStringLiteral("turn_prediction")) {
				query.removeAllQueryItems(QStringLiteral("commit_strategy"));
				query.addQueryItem(QStringLiteral("commit_strategy"), QStringLiteral("vad"));
				effectiveStrategy = QStringLiteral("vad");
			}
			query.removeAllQueryItems(QStringLiteral("multilang_mode"));
			query.removeAllQueryItems(QStringLiteral("diarize"));
		}
		if ((effectiveStrategy == QStringLiteral("vad") ||
			 effectiveStrategy == QStringLiteral("turn_prediction")) &&
			!query.hasQueryItem(QStringLiteral("vad_silence_threshold_secs"))) {
			query.addQueryItem(QStringLiteral("vad_silence_threshold_secs"), QStringLiteral("1.0"));
		}
		if (!query.hasQueryItem(QStringLiteral("language_code"))) {
			const QString normLang = normalizeIso639PrimaryLanguage(language);
			if (!normLang.isEmpty()) {
				query.addQueryItem(QStringLiteral("language_code"), normLang);
			}
		}
		const bool hasExplicitLanguage =
			!query.queryItemValue(QStringLiteral("language_code")).trimmed().isEmpty();
		if (!hasExplicitLanguage) {
			if (!query.hasQueryItem(QStringLiteral("include_timestamps"))) {
				query.addQueryItem(QStringLiteral("include_timestamps"), QStringLiteral("true"));
			}
			if (!query.hasQueryItem(QStringLiteral("include_language_detection"))) {
				query.addQueryItem(QStringLiteral("include_language_detection"), QStringLiteral("true"));
			}
			if (isScribeV3Live && !query.hasQueryItem(QStringLiteral("multilang_mode"))) {
				query.addQueryItem(QStringLiteral("multilang_mode"), QStringLiteral("true"));
			}
		}
		url.setQuery(query);
		break;
	}
	case AgentSpeechProtocolMode::XaiSttWs: {
		if (!path.contains(QStringLiteral("/stt"), Qt::CaseInsensitive) &&
			(path == QStringLiteral("/") ||
			 path.compare(QStringLiteral("/v1"), Qt::CaseInsensitive) == 0 ||
			 path.compare(QStringLiteral("/v1/"), Qt::CaseInsensitive) == 0 ||
			 path.endsWith(QStringLiteral("/chat/completions"), Qt::CaseInsensitive) ||
			 path.endsWith(QStringLiteral("/audio/transcriptions"), Qt::CaseInsensitive))) {
			url.setPath(QStringLiteral("/v1/stt"));
		}
		const QString effectiveModel = model.trimmed().isEmpty()
			? QStringLiteral("grok-voice-transcribe-2.0")
			: model.trimmed();
		if (!query.hasQueryItem(QStringLiteral("model"))) {
			query.addQueryItem(QStringLiteral("model"), effectiveModel);
		}
		if (!query.hasQueryItem(QStringLiteral("encoding"))) {
			query.addQueryItem(QStringLiteral("encoding"), QStringLiteral("pcm"));
		}
		if (!query.hasQueryItem(QStringLiteral("sample_rate"))) {
			query.addQueryItem(QStringLiteral("sample_rate"), QStringLiteral("16000"));
		}
		if (!query.hasQueryItem(QStringLiteral("interim_results"))) {
			query.addQueryItem(QStringLiteral("interim_results"), QStringLiteral("true"));
		}
		if (!query.hasQueryItem(QStringLiteral("endpointing"))) {
			query.addQueryItem(QStringLiteral("endpointing"), QStringLiteral("400"));
		}
		if (!query.hasQueryItem(QStringLiteral("language"))) {
			const QString normLang = normalizeIso639PrimaryLanguage(language);
			if (!normLang.isEmpty()) {
				query.addQueryItem(QStringLiteral("language"), normLang);
			}
		}
		const QString effectiveLangLower = normalizeIso639PrimaryLanguage(
			query.queryItemValue(QStringLiteral("language")));
		if (!query.hasQueryItem(QStringLiteral("format")) &&
			isXaiSttFormatSupportedLanguage(effectiveLangLower)) {
			query.addQueryItem(QStringLiteral("format"), QStringLiteral("true"));
		}
		if (!prompt.trimmed().isEmpty() && !query.hasQueryItem(QStringLiteral("keyterm"))) {
			const QStringList terms = parseXaiSttKeyterms(prompt, 2000);
			for (const QString &term : terms) {
				query.addQueryItem(QStringLiteral("keyterm"), term);
			}
		}
		url.setQuery(query);
		break;
	}
	default:
		break;
	}

	return url;
}

// =========================================================================
// BaseWsStreamingAsrSession
// =========================================================================

struct BaseWsStreamingAsrSession::Impl : public std::enable_shared_from_this<Impl> {
	Impl()
		: strand(asio::make_strand(StreamingAsrIoRuntime::instance().ioContext())) {}

	std::weak_ptr<BaseWsStreamingAsrSession> ownerWeak;
	asio::strand<asio::io_context::executor_type> strand;
	std::shared_ptr<livekit::WebSocketClient> ws;
	std::shared_ptr<asio::steady_timer> finishTimer;

	std::mutex callbackMutex;
	StreamingAsrCallbacks callbacks;

	std::mutex pcmMutex;
	std::vector<int16_t> pendingPcm;

	std::deque<std::vector<uint8_t>> preReadyQueue;
	std::chrono::steady_clock::time_point lastAudioSentTime{};
	std::atomic<StreamingAsrState> state{ StreamingAsrState::Idle };
	std::atomic<bool> aborted{ false };
	std::atomic<bool> finishRequested{ false };
	std::atomic<bool> terminalNotified{ false };
	bool connectedOnce = false;
	bool finishSent = false;

	void setState(StreamingAsrState next) {
		const StreamingAsrState prev = state.exchange(next, std::memory_order_acq_rel);
		if (prev == next || aborted.load(std::memory_order_acquire)) {
			return;
		}
		std::function<void(StreamingAsrState)> cb;
		{
			std::lock_guard<std::mutex> lock(callbackMutex);
			cb = callbacks.onStateChanged;
		}
		if (cb) {
			cb(next);
		}
	}

	void notifyResult(const StreamingAsrResult &result) {
		if (aborted.load(std::memory_order_acquire)) {
			return;
		}
		std::function<void(const StreamingAsrResult &)> cb;
		{
			std::lock_guard<std::mutex> lock(callbackMutex);
			cb = callbacks.onResult;
		}
		if (cb) {
			cb(result);
		}
	}

	void notifyError(const QString &safeError) {
		if (aborted.load(std::memory_order_acquire) || terminalNotified.exchange(true, std::memory_order_acq_rel)) {
			return;
		}
		setState(StreamingAsrState::Error);
		cancelFinishTimer();
		if (ws) {
			ws->Abort();
		}
		std::function<void(const QString &)> cb;
		{
			std::lock_guard<std::mutex> lock(callbackMutex);
			cb = callbacks.onError;
		}
		if (cb) {
			cb(safeError);
		}
	}

	void notifyFinished() {
		if (aborted.load(std::memory_order_acquire) || terminalNotified.exchange(true, std::memory_order_acq_rel)) {
			return;
		}
		if (auto owner = ownerWeak.lock()) {
			owner->onBeforeFinishOnStrand();
		}
		setState(StreamingAsrState::Closed);
		cancelFinishTimer();
		std::function<void()> cb;
		{
			std::lock_guard<std::mutex> lock(callbackMutex);
			cb = callbacks.onFinished;
		}
		if (cb) {
			cb();
		}
	}

	void cancelFinishTimer() {
		if (finishTimer) {
			std::error_code ec;
			finishTimer->cancel(ec);
			finishTimer.reset();
		}
	}

	void armFinishTimer(int timeoutMs) {
		cancelFinishTimer();
		auto self = shared_from_this();
		finishTimer = std::make_shared<asio::steady_timer>(strand);
		finishTimer->expires_after(std::chrono::milliseconds(std::max(300, timeoutMs)));
		finishTimer->async_wait([self](const std::error_code &ec) {
			if (ec || self->aborted.load(std::memory_order_acquire) ||
				self->terminalNotified.load(std::memory_order_acquire)) {
				return;
			}
			auto client = self->ws;
			self->notifyFinished();
			if (client && client->IsConnected()) {
				livekit::safe_co_spawn(self->strand, [client]() -> asio::awaitable<void> {
					co_await client->Close(1000, "finish timeout");
				}, [](std::exception_ptr) {});
			}
		});
	}

	void flushPreReadyQueue() {
		auto owner = ownerWeak.lock();
		if (aborted.load(std::memory_order_acquire) || !owner) {
			return;
		}
		while (!preReadyQueue.empty()) {
			auto chunk = std::move(preReadyQueue.front());
			preReadyQueue.pop_front();
			dispatchAudioChunkOnStrand(chunk);
		}
		if (finishRequested.load(std::memory_order_acquire) && !finishSent) {
			beginFinishOnStrand();
		}
	}

	void dispatchAudioChunkOnStrand(const std::vector<uint8_t> &pcmBytes) {
		auto owner = ownerWeak.lock();
		if (aborted.load(std::memory_order_acquire) || !owner || pcmBytes.empty()) {
			return;
		}
		const size_t maxQueuedBytes = static_cast<size_t>(owner->requiredSampleRate()) * 2 * 3;
		if (ws && ws->QueuedBytes() > maxQueuedBytes) {
			return;
		}
		lastAudioSentTime = std::chrono::steady_clock::now();
		owner->sendAudioChunkOnStrand(pcmBytes);
	}

	void beginFinishOnStrand() {
		auto owner = ownerWeak.lock();
		if (aborted.load(std::memory_order_acquire) || !owner || finishSent ||
			terminalNotified.load(std::memory_order_acquire)) {
			return;
		}
		finishSent = true;
		setState(StreamingAsrState::Finishing);
		armFinishTimer(owner->finishTimeoutMs());
		owner->sendFinishOnStrand();
	}
};

BaseWsStreamingAsrSession::BaseWsStreamingAsrSession(const StreamingAsrConfig &config)
	: _config(config)
	, _impl(std::make_shared<Impl>()) {
}

BaseWsStreamingAsrSession::~BaseWsStreamingAsrSession() {
	abort();
}

StreamingAsrState BaseWsStreamingAsrSession::state() const {
	return _impl->state.load(std::memory_order_acquire);
}

void BaseWsStreamingAsrSession::start(StreamingAsrCallbacks callbacks) {
	StreamingAsrIoRuntime::instance().ensureStarted();
	_impl->ownerWeak = weak_from_this();
	{
		std::lock_guard<std::mutex> lock(_impl->callbackMutex);
		_impl->callbacks = std::move(callbacks);
	}
	if (_impl->state.load(std::memory_order_acquire) != StreamingAsrState::Idle ||
		_impl->aborted.load(std::memory_order_acquire)) {
		return;
	}
	const ResolvedConnection conn = buildConnection();
	if (!conn.valid) {
		_impl->notifyError(conn.errorMessage.isEmpty()
			? QCoreApplication::translate("MeetingUI", "Invalid streaming speech recognition configuration.")
			: conn.errorMessage);
		return;
	}

	_impl->setState(StreamingAsrState::Connecting);
	std::weak_ptr<Impl> weakImpl = _impl;
	livekit::safe_co_spawn(_impl->strand, [weakImpl, conn]() -> asio::awaitable<void> {
		auto self = weakImpl.lock();
		auto owner = self ? self->ownerWeak.lock() : nullptr;
		if (!self || !owner || self->aborted.load(std::memory_order_acquire)) {
			co_return;
		}
		auto &runtime = StreamingAsrIoRuntime::instance();
		auto client = std::make_shared<livekit::WebSocketClient>(runtime.ioContext(), runtime.sslContext());
		self->ws = client;

		client->SetOnTextMessage([weakImpl](const std::string &payload) {
			if (auto locked = weakImpl.lock(); locked && !locked->aborted.load(std::memory_order_acquire)) {
				if (auto lockedOwner = locked->ownerWeak.lock()) {
					lockedOwner->onTextMessageOnStrand(payload);
				}
			}
		});
		client->SetOnMessage([weakImpl](const std::vector<uint8_t> &payload) {
			if (auto locked = weakImpl.lock(); locked && !locked->aborted.load(std::memory_order_acquire)) {
				if (auto lockedOwner = locked->ownerWeak.lock()) {
					lockedOwner->onBinaryMessageOnStrand(payload);
				}
			}
		});
		client->SetOnClose([weakImpl](uint16_t code, const std::string &reason) {
			auto locked = weakImpl.lock();
			if (!locked || locked->aborted.load(std::memory_order_acquire) ||
				locked->terminalNotified.load(std::memory_order_acquire)) {
				return;
			}
			const StreamingAsrState cur = locked->state.load(std::memory_order_acquire);
			if (cur == StreamingAsrState::Finishing || code == 1000 || code == 1001) {
				locked->notifyFinished();
			} else {
				const QString reasonStr = QString::fromUtf8(reason.data(), static_cast<int>(reason.size()));
				locked->notifyError(sanitizeStreamingError(
					QStringLiteral("WebSocket closed (%1): %2").arg(code).arg(reasonStr)));
			}
		});
		client->SetOnError([weakImpl](const std::error_code &ec) {
			auto locked = weakImpl.lock();
			if (!locked || locked->aborted.load(std::memory_order_acquire) ||
				locked->terminalNotified.load(std::memory_order_acquire)) {
				return;
			}
			if (locked->state.load(std::memory_order_acquire) == StreamingAsrState::Finishing &&
				(ec == asio::error::eof || ec == asio::ssl::error::stream_truncated)) {
				locked->notifyFinished();
				return;
			}
			const auto status = livekit::WebSocketHttpStatus(ec);
			locked->notifyError(sanitizeStreamingError(
				QString::fromStdString(ec.message()),
				status.has_value() ? static_cast<int>(*status) : 0));
		});

		livekit::CredentialUrlPolicy policy;
		policy.allow_insecure = conn.allowPlaintextLoopback;
		policy.require_secure = !conn.allowPlaintextLoopback;

		const int timeoutMs = std::clamp(owner->config().connectTimeoutMs, 2000, 30000);
		const std::error_code ec = co_await client->Connect(
			conn.url.toStdString(),
			conn.bearerToken.toStdString(),
			std::chrono::milliseconds(timeoutMs),
			policy,
			conn.extraHeaders);

		if (self->aborted.load(std::memory_order_acquire) ||
			self->terminalNotified.load(std::memory_order_acquire)) {
			if (client->IsConnected()) {
				client->Abort();
			}
			co_return;
		}
		if (ec) {
			const auto status = livekit::WebSocketHttpStatus(ec);
			const std::string detail = client->LastHandshakeErrorDetail();
			const QString rawMsg = !detail.empty()
				? QString::fromUtf8(detail.data(), static_cast<int>(detail.size()))
				: QString::fromStdString(ec.message());
			self->notifyError(sanitizeStreamingError(
				rawMsg,
				status.has_value() ? static_cast<int>(*status) : 0));
			co_return;
		}

		self->connectedOnce = true;
		client->StartRead();
		if (auto lockedOwner = self->ownerWeak.lock()) {
			lockedOwner->onConnectedOnStrand();
		}
	}, [weakImpl](std::exception_ptr ep) {
		auto self = weakImpl.lock();
		if (!self || self->aborted.load(std::memory_order_acquire)) {
			return;
		}
		try {
			if (ep) std::rethrow_exception(ep);
		} catch (const std::exception &ex) {
			self->notifyError(sanitizeStreamingError(QString::fromUtf8(ex.what())));
		} catch (...) {
			self->notifyError(sanitizeStreamingError(QStringLiteral("Streaming ASR coroutine failed.")));
		}
	});
}

void BaseWsStreamingAsrSession::pushPcm16(
		const int16_t *samples,
		size_t sampleCount,
		int inputSampleRate) {
	if (!samples || sampleCount == 0 ||
		_impl->aborted.load(std::memory_order_acquire) ||
		_impl->finishRequested.load(std::memory_order_acquire)) {
		return;
	}
	const StreamingAsrState curState = _impl->state.load(std::memory_order_acquire);
	if (curState != StreamingAsrState::Connecting && curState != StreamingAsrState::Ready) {
		return;
	}

	const int targetRate = requiredSampleRate();
	const auto resampled = resamplePcm16Mono(samples, sampleCount, inputSampleRate, targetRate);
	if (resampled.empty()) {
		return;
	}

	const size_t chunkSamples = static_cast<size_t>(std::max(160, targetRate / 10)); // 100 ms
	std::vector<std::vector<uint8_t>> readyChunks;
	{
		std::lock_guard<std::mutex> lock(_impl->pcmMutex);
		_impl->pendingPcm.insert(_impl->pendingPcm.end(), resampled.begin(), resampled.end());
		while (_impl->pendingPcm.size() >= chunkSamples) {
			std::vector<uint8_t> bytes(chunkSamples * sizeof(int16_t));
			std::memcpy(bytes.data(), _impl->pendingPcm.data(), bytes.size());
			_impl->pendingPcm.erase(
				_impl->pendingPcm.begin(),
				_impl->pendingPcm.begin() + static_cast<std::ptrdiff_t>(chunkSamples));
			readyChunks.push_back(std::move(bytes));
		}
	}

	if (readyChunks.empty()) {
		return;
	}
	std::weak_ptr<Impl> weakImpl = _impl;
	asio::post(_impl->strand, [weakImpl, chunks = std::move(readyChunks)]() mutable {
		auto self = weakImpl.lock();
		if (!self || self->aborted.load(std::memory_order_acquire) ||
			self->terminalNotified.load(std::memory_order_acquire)) {
			return;
		}
		const StreamingAsrState st = self->state.load(std::memory_order_acquire);
		if (st == StreamingAsrState::Connecting) {
			for (auto &chunk : chunks) {
				while (self->preReadyQueue.size() >= kMaxPreReadyChunks) {
					self->preReadyQueue.pop_front();
				}
				self->preReadyQueue.push_back(std::move(chunk));
			}
		} else if (st == StreamingAsrState::Ready) {
			for (const auto &chunk : chunks) {
				self->dispatchAudioChunkOnStrand(chunk);
			}
		}
	});
}

void BaseWsStreamingAsrSession::finish() {
	if (_impl->aborted.load(std::memory_order_acquire) ||
		_impl->terminalNotified.load(std::memory_order_acquire)) {
		return;
	}
	_impl->finishRequested.store(true, std::memory_order_release);

	std::vector<uint8_t> tailBytes;
	{
		std::lock_guard<std::mutex> lock(_impl->pcmMutex);
		if (!_impl->pendingPcm.empty()) {
			tailBytes.resize(_impl->pendingPcm.size() * sizeof(int16_t));
			std::memcpy(tailBytes.data(), _impl->pendingPcm.data(), tailBytes.size());
			_impl->pendingPcm.clear();
		}
	}

	std::weak_ptr<Impl> weakImpl = _impl;
	asio::post(_impl->strand, [weakImpl, tailBytes = std::move(tailBytes)]() mutable {
		auto self = weakImpl.lock();
		if (!self || self->aborted.load(std::memory_order_acquire) ||
			self->terminalNotified.load(std::memory_order_acquire)) {
			return;
		}
		const StreamingAsrState st = self->state.load(std::memory_order_acquire);
		if (st == StreamingAsrState::Connecting) {
			if (!tailBytes.empty()) {
				while (self->preReadyQueue.size() >= kMaxPreReadyChunks) {
					self->preReadyQueue.pop_front();
				}
				self->preReadyQueue.push_back(std::move(tailBytes));
			}
			if (!self->finishTimer) {
				auto lockedOwner = self->ownerWeak.lock();
				// Guard in case handshake takes too long while finish() was already requested.
				self->armFinishTimer(std::max(1500, lockedOwner ? lockedOwner->finishTimeoutMs() : 2500));
			}
		} else if (st == StreamingAsrState::Ready) {
			if (!tailBytes.empty()) {
				self->dispatchAudioChunkOnStrand(tailBytes);
			}
			self->beginFinishOnStrand();
		}
	});
}

void BaseWsStreamingAsrSession::abort() {
	if (_impl->aborted.exchange(true, std::memory_order_acq_rel)) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(_impl->callbackMutex);
		_impl->callbacks = StreamingAsrCallbacks{};
	}
	_impl->state.store(StreamingAsrState::Closed, std::memory_order_release);
	std::shared_ptr<Impl> self = _impl;
	asio::post(_impl->strand, [self]() {
		self->cancelFinishTimer();
		self->preReadyQueue.clear();
		self->ownerWeak.reset();
		if (self->ws) {
			self->ws->Abort();
			self->ws.reset();
		}
	});
}

void BaseWsStreamingAsrSession::markReadyOnStrand() {
	if (_impl->aborted.load(std::memory_order_acquire) ||
		_impl->terminalNotified.load(std::memory_order_acquire)) {
		return;
	}
	if (_impl->state.load(std::memory_order_acquire) == StreamingAsrState::Connecting) {
		_impl->setState(StreamingAsrState::Ready);
		_impl->flushPreReadyQueue();
	}
}

void BaseWsStreamingAsrSession::sendTextOnStrand(std::string text) {
	if (_impl->aborted.load(std::memory_order_acquire) || !_impl->ws || !_impl->ws->IsConnected()) {
		return;
	}
	auto client = _impl->ws;
	auto payload = std::make_shared<std::string>(std::move(text));
	std::weak_ptr<Impl> weakImpl = _impl;
	livekit::safe_co_spawn(_impl->strand, [client, payload]() -> asio::awaitable<void> {
		co_await client->SendText(std::move(*payload));
	}, [weakImpl](std::exception_ptr) {
		if (auto locked = weakImpl.lock(); locked && !locked->aborted.load(std::memory_order_acquire)) {
			locked->notifyError(sanitizeStreamingError(QStringLiteral("Failed to send WebSocket text message.")));
		}
	});
}

void BaseWsStreamingAsrSession::sendBinaryOnStrand(std::vector<uint8_t> data) {
	if (_impl->aborted.load(std::memory_order_acquire) || !_impl->ws || !_impl->ws->IsConnected()) {
		return;
	}
	auto client = _impl->ws;
	auto payload = std::make_shared<std::vector<uint8_t>>(std::move(data));
	std::weak_ptr<Impl> weakImpl = _impl;
	livekit::safe_co_spawn(_impl->strand, [client, payload]() -> asio::awaitable<void> {
		co_await client->SendBinary(std::move(*payload));
	}, [weakImpl](std::exception_ptr) {
		if (auto locked = weakImpl.lock(); locked && !locked->aborted.load(std::memory_order_acquire)) {
			locked->notifyError(sanitizeStreamingError(QStringLiteral("Failed to send WebSocket binary audio frame.")));
		}
	});
}

void BaseWsStreamingAsrSession::emitResultFromStrand(const StreamingAsrResult &result) {
	_impl->notifyResult(result);
}

void BaseWsStreamingAsrSession::finishCleanlyFromStrand() {
	auto client = _impl->ws;
	_impl->notifyFinished();
	if (client && client->IsConnected()) {
		livekit::safe_co_spawn(_impl->strand, [client]() -> asio::awaitable<void> {
			co_await client->Close(1000, "done");
		}, [](std::exception_ptr) {});
	}
}

void BaseWsStreamingAsrSession::failFromStrand(const QString &safeError) {
	_impl->notifyError(sanitizeStreamingError(safeError));
}

bool BaseWsStreamingAsrSession::isFinishingOnStrand() const {
	return _impl->state.load(std::memory_order_acquire) == StreamingAsrState::Finishing ||
		_impl->finishRequested.load(std::memory_order_acquire);
}

std::chrono::steady_clock::time_point BaseWsStreamingAsrSession::lastAudioSentOnStrand() const {
	return _impl->lastAudioSentTime;
}

asio::any_io_executor BaseWsStreamingAsrSession::strandExecutor() const {
	return _impl->strand;
}

// =========================================================================
// 1. OpenAiRealtimeAsrSession (OpenAI Realtime + DashScope Qwen3 Realtime)
// =========================================================================

OpenAiRealtimeAsrSession::OpenAiRealtimeAsrSession(const StreamingAsrConfig &config)
	: BaseWsStreamingAsrSession(config)
	, _isDashScopeQwen(config.protocol == AgentSpeechProtocolMode::DashScopeQwenRealtimeWs) {
	const QString effectiveModel = _config.model.trimmed().isEmpty()
		? (_isDashScopeQwen ? QStringLiteral("qwen3-asr-flash-realtime") : QStringLiteral("gpt-live-transcribe"))
		: _config.model.trimmed();
	_effectiveServerVad = _isDashScopeQwen
		? _config.enableServerVad
		: (_config.enableServerVad && !openAiRealtimeDisallowsServerVad(effectiveModel));
}

int OpenAiRealtimeAsrSession::requiredSampleRate() const {
	return _isDashScopeQwen ? 16000 : 24000;
}

BaseWsStreamingAsrSession::ResolvedConnection OpenAiRealtimeAsrSession::buildConnection() const {
	ResolvedConnection res;
	QString err;
	const AgentSpeechProtocolMode proto = _isDashScopeQwen
		? AgentSpeechProtocolMode::DashScopeQwenRealtimeWs
		: AgentSpeechProtocolMode::OpenAiRealtimeWs;
	const QUrl url = resolveStreamingSpeechEndpointUrl(
		proto, _config.endpointUrl, _config.model, _config.language, &err);
	if (!url.isValid()) {
		res.errorMessage = err;
		return res;
	}
	res.allowPlaintextLoopback = (url.scheme() == QStringLiteral("ws"));
	if (_config.apiKey.trimmed().isEmpty() && !res.allowPlaintextLoopback) {
		res.errorMessage = QCoreApplication::translate(
			"MeetingUI", "An STT API key is required for WebSocket streaming ASR.");
		return res;
	}
	res.url = url.toString(QUrl::FullyEncoded);
	res.bearerToken = _config.apiKey.trimmed();
	if (_isDashScopeQwen && !_config.workspaceId.trimmed().isEmpty()) {
		res.extraHeaders.emplace_back("X-DashScope-WorkSpace", _config.workspaceId.trimmed().toStdString());
	}
	appendCustomHeaders(_config, res.extraHeaders);
	res.valid = true;
	return res;
}

void OpenAiRealtimeAsrSession::onConnectedOnStrand() {
	// Wait for the server's session.created greeting before sending session.update.
}

void OpenAiRealtimeAsrSession::sendSessionUpdateOnStrand() {
	if (_sessionUpdateSent) {
		return;
	}
	_sessionUpdateSent = true;
	if (_isDashScopeQwen) {
		QJsonObject trObj;
		if (!_config.language.trimmed().isEmpty()) {
			trObj.insert(QStringLiteral("language"), _config.language.trimmed());
		}
		QJsonObject sessionObj;
		sessionObj.insert(QStringLiteral("modalities"), QJsonArray{ QStringLiteral("text") });
		sessionObj.insert(QStringLiteral("input_audio_format"), QStringLiteral("pcm"));
		sessionObj.insert(QStringLiteral("sample_rate"), 16000);
		sessionObj.insert(QStringLiteral("input_audio_transcription"), trObj);
		if (_config.enableServerVad) {
			QJsonObject vad;
			vad.insert(QStringLiteral("type"), QStringLiteral("server_vad"));
			vad.insert(QStringLiteral("threshold"), 0.2);
			vad.insert(QStringLiteral("silence_duration_ms"), 500);
			sessionObj.insert(QStringLiteral("turn_detection"), vad);
		} else {
			sessionObj.insert(QStringLiteral("turn_detection"), QJsonValue::Null);
		}
		QJsonObject root;
		root.insert(QStringLiteral("type"), QStringLiteral("session.update"));
		root.insert(QStringLiteral("session"), sessionObj);
		sendTextOnStrand(QJsonDocument(root).toJson(QJsonDocument::Compact).toStdString());
		return;
	}

	const QString effectiveModel = _config.model.trimmed().isEmpty()
		? QStringLiteral("gpt-live-transcribe")
		: _config.model.trimmed();
	_effectiveServerVad = _config.enableServerVad && !openAiRealtimeDisallowsServerVad(effectiveModel);

	QJsonObject trObj;
	trObj.insert(QStringLiteral("model"), effectiveModel);
	if (!_config.language.trimmed().isEmpty()) {
		trObj.insert(QStringLiteral("language"), _config.language.trimmed());
	}
	if (!_config.prompt.trimmed().isEmpty()) {
		trObj.insert(QStringLiteral("prompt"), _config.prompt.trimmed());
	}

	QJsonObject formatObj;
	formatObj.insert(QStringLiteral("type"), QStringLiteral("audio/pcm"));
	formatObj.insert(QStringLiteral("rate"), requiredSampleRate());

	QJsonObject noiseObj;
	noiseObj.insert(QStringLiteral("type"), QStringLiteral("near_field"));

	QJsonObject inputObj;
	inputObj.insert(QStringLiteral("format"), formatObj);
	inputObj.insert(QStringLiteral("noise_reduction"), noiseObj);
	inputObj.insert(QStringLiteral("transcription"), trObj);
	if (_effectiveServerVad) {
		QJsonObject vad;
		vad.insert(QStringLiteral("type"), QStringLiteral("server_vad"));
		vad.insert(QStringLiteral("threshold"), 0.5);
		vad.insert(QStringLiteral("prefix_padding_ms"), 300);
		vad.insert(QStringLiteral("silence_duration_ms"), 500);
		inputObj.insert(QStringLiteral("turn_detection"), vad);
	} else {
		inputObj.insert(QStringLiteral("turn_detection"), QJsonValue::Null);
	}

	QJsonObject audioObj;
	audioObj.insert(QStringLiteral("input"), inputObj);

	QJsonObject sessionObj;
	sessionObj.insert(QStringLiteral("type"), QStringLiteral("transcription"));
	sessionObj.insert(QStringLiteral("audio"), audioObj);

	QJsonObject root;
	root.insert(QStringLiteral("type"), QStringLiteral("session.update"));
	root.insert(QStringLiteral("session"), sessionObj);
	sendTextOnStrand(QJsonDocument(root).toJson(QJsonDocument::Compact).toStdString());
}

void OpenAiRealtimeAsrSession::sendCommitOnStrand() {
	QJsonObject commitMsg;
	commitMsg.insert(QStringLiteral("type"), QStringLiteral("input_audio_buffer.commit"));
	sendTextOnStrand(QJsonDocument(commitMsg).toJson(QJsonDocument::Compact).toStdString());
	_hasUncommittedAudio = false;
	_speechDetectedSinceCommit = false;
	_uncommittedAudioMs = 0;
	_speechAudioMs = 0;
	_trailingSilenceMs = 0;
	++_pendingCommits;
}

void OpenAiRealtimeAsrSession::sendAudioChunkOnStrand(const std::vector<uint8_t> &pcmBytes) {
	_hasUncommittedAudio = true;
	const int sampleRate = std::max(8000, requiredSampleRate());
	const int chunkSamples = static_cast<int>(pcmBytes.size() / sizeof(int16_t));
	const int chunkMs = std::max(1, (chunkSamples * 1000 + sampleRate / 2) / sampleRate);
	_uncommittedAudioMs += chunkMs;

	QJsonObject msg;
	msg.insert(QStringLiteral("type"), QStringLiteral("input_audio_buffer.append"));
	msg.insert(QStringLiteral("audio"), makeBase64Pcm(pcmBytes));
	sendTextOnStrand(QJsonDocument(msg).toJson(QJsonDocument::Compact).toStdString());

	if (!_isDashScopeQwen && !_effectiveServerVad) {
		constexpr float kClientVadSpeechRmsThreshold = 0.007943f; // -42 dBFS
		const float rms = computePcm16RmsNormalized(pcmBytes.data(), pcmBytes.size());
		if (rms >= kClientVadSpeechRmsThreshold) {
			_speechDetectedSinceCommit = true;
			_speechAudioMs += chunkMs;
			_trailingSilenceMs = 0;
		} else if (_speechDetectedSinceCommit) {
			_trailingSilenceMs += chunkMs;
		}

		const int silenceThresholdMs = std::clamp(_config.clientCommitSilenceMs, 200, 5000);
		const int maxUtteranceMs = std::clamp(_config.clientCommitMaxUtteranceMs, 3000, 60000);
		if (_speechDetectedSinceCommit && _speechAudioMs >= 200 && _uncommittedAudioMs >= 150 &&
			_trailingSilenceMs >= silenceThresholdMs) {
			sendCommitOnStrand();
		} else if (_speechDetectedSinceCommit && _uncommittedAudioMs >= maxUtteranceMs) {
			sendCommitOnStrand();
		} else if (!_speechDetectedSinceCommit && _uncommittedAudioMs >= 15000) {
			QJsonObject clearMsg;
			clearMsg.insert(QStringLiteral("type"), QStringLiteral("input_audio_buffer.clear"));
			sendTextOnStrand(QJsonDocument(clearMsg).toJson(QJsonDocument::Compact).toStdString());
			_hasUncommittedAudio = false;
			_uncommittedAudioMs = 0;
			_speechAudioMs = 0;
			_trailingSilenceMs = 0;
		}
	}
}

void OpenAiRealtimeAsrSession::sendFinishOnStrand() {
	if (_isDashScopeQwen) {
		if (_hasUncommittedAudio || !_config.enableServerVad) {
			QJsonObject commitMsg;
			commitMsg.insert(QStringLiteral("type"), QStringLiteral("input_audio_buffer.commit"));
			sendTextOnStrand(QJsonDocument(commitMsg).toJson(QJsonDocument::Compact).toStdString());
			_hasUncommittedAudio = false;
		}
		QJsonObject finishMsg;
		finishMsg.insert(QStringLiteral("type"), QStringLiteral("session.finish"));
		sendTextOnStrand(QJsonDocument(finishMsg).toJson(QJsonDocument::Compact).toStdString());
		return;
	}
	if (_hasUncommittedAudio && _uncommittedAudioMs >= 100) {
		sendCommitOnStrand();
	} else if (_pendingCommits == 0) {
		finishCleanlyFromStrand();
	}
}

void OpenAiRealtimeAsrSession::onTextMessageOnStrand(const std::string &payload) {
	QJsonParseError err{};
	const QJsonDocument doc = QJsonDocument::fromJson(
		QByteArray(payload.data(), static_cast<int>(payload.size())), &err);
	if (err.error != QJsonParseError::NoError || !doc.isObject()) {
		return;
	}
	const QJsonObject root = doc.object();
	const QString type = root.value(QStringLiteral("type")).toString();

	if (type == QStringLiteral("session.created") ||
		type == QStringLiteral("transcription_session.created")) {
		sendSessionUpdateOnStrand();
		if (_isDashScopeQwen) {
			markReadyOnStrand();
		}
		return;
	}
	if (type == QStringLiteral("session.updated") ||
		type == QStringLiteral("transcription_session.updated")) {
		markReadyOnStrand();
		return;
	}
	if (type == QStringLiteral("input_audio_buffer.committed")) {
		if (_effectiveServerVad) {
			_hasUncommittedAudio = false;
			_uncommittedAudioMs = 0;
		}
		return;
	}
	if (type == QStringLiteral("conversation.item.input_audio_transcription.delta")) {
		const QString itemId = root.value(QStringLiteral("item_id")).toString();
		const QString key = itemId.isEmpty() ? QStringLiteral("oai-cur") : itemId;
		const QString delta = root.value(QStringLiteral("delta")).toString();
		QString &acc = _partialByItemId[key];
		acc += delta;
		const QString trimmed = acc.trimmed();
		if (!trimmed.isEmpty()) {
			emitResultFromStrand(StreamingAsrResult{ key, trimmed, false, _config.language });
		}
		return;
	}
	if (type == QStringLiteral("conversation.item.input_audio_transcription.text")) {
		const QString itemId = root.value(QStringLiteral("item_id")).toString();
		const QString key = itemId.isEmpty() ? QStringLiteral("qwen-cur") : itemId;
		const QString text = root.value(QStringLiteral("text")).toString();
		const QString stash = root.value(QStringLiteral("stash")).toString();
		const QString combined = (text + stash).trimmed();
		if (!combined.isEmpty()) {
			emitResultFromStrand(StreamingAsrResult{ key, combined, false,
				root.value(QStringLiteral("language")).toString(_config.language) });
		}
		return;
	}
	if (type == QStringLiteral("conversation.item.input_audio_transcription.completed")) {
		if (_pendingCommits > 0) {
			--_pendingCommits;
		}
		if (_effectiveServerVad) {
			_hasUncommittedAudio = false;
			_uncommittedAudioMs = 0;
		}
		const QString itemId = root.value(QStringLiteral("item_id")).toString();
		const QString key = itemId.isEmpty() ? QStringLiteral("oai-cur") : itemId;
		const QString accumulated = _partialByItemId.take(key);
		QString transcript = root.value(QStringLiteral("transcript")).toString().trimmed();
		if (transcript.isEmpty()) {
			transcript = accumulated.trimmed();
		}
		if (!transcript.isEmpty()) {
			emitResultFromStrand(StreamingAsrResult{ key, transcript, true, _config.language });
		}
		if (isFinishingOnStrand() && !_isDashScopeQwen && _pendingCommits == 0) {
			finishCleanlyFromStrand();
		}
		return;
	}
	if (type == QStringLiteral("conversation.item.input_audio_transcription.failed")) {
		if (_pendingCommits > 0) {
			--_pendingCommits;
		}
		const QJsonObject errObj = root.value(QStringLiteral("error")).toObject();
		const QString code = errObj.value(QStringLiteral("code")).toString().trimmed();
		const QString detail = extractJsonErrorMessage(root);
		if (isFinishingOnStrand() && !_isDashScopeQwen &&
			(code == QStringLiteral("input_audio_buffer_commit_empty") ||
			 (detail.contains(QStringLiteral("commit"), Qt::CaseInsensitive) &&
			  detail.contains(QStringLiteral("empty"), Qt::CaseInsensitive)) ||
			 detail.contains(QStringLiteral("buffer too small"), Qt::CaseInsensitive))) {
			if (_pendingCommits == 0) {
				finishCleanlyFromStrand();
			}
			return;
		}
		failFromStrand(detail.isEmpty() ? QStringLiteral("Realtime transcription failed.") : detail);
		return;
	}
	if (type == QStringLiteral("session.finished")) {
		finishCleanlyFromStrand();
		return;
	}
	if (type == QStringLiteral("error")) {
		const QJsonObject errObj = root.value(QStringLiteral("error")).toObject();
		const QString code = errObj.value(QStringLiteral("code")).toString().trimmed();
		const QString detail = extractJsonErrorMessage(root);
		if (isFinishingOnStrand() &&
			(code == QStringLiteral("input_audio_buffer_commit_empty") ||
			 (detail.contains(QStringLiteral("commit"), Qt::CaseInsensitive) &&
			  detail.contains(QStringLiteral("empty"), Qt::CaseInsensitive)) ||
			 detail.contains(QStringLiteral("buffer too small"), Qt::CaseInsensitive))) {
			if (_pendingCommits > 0) {
				--_pendingCommits;
			}
			if (_isDashScopeQwen || _pendingCommits == 0) {
				finishCleanlyFromStrand();
			}
			return;
		}
		failFromStrand(detail.isEmpty() ? QStringLiteral("Realtime ASR error.") : detail);
	}
}

// =========================================================================
// 2. DashScopeInferenceAsrSession (Paraformer / Fun-ASR Realtime)
// =========================================================================

DashScopeInferenceAsrSession::DashScopeInferenceAsrSession(const StreamingAsrConfig &config)
	: BaseWsStreamingAsrSession(config)
	, _taskId(QUuid::createUuid().toString(QUuid::Id128).toStdString()) {
}

int DashScopeInferenceAsrSession::requiredSampleRate() const {
	return isNarrowband8kModel(_config.model) ? 8000 : 16000;
}

BaseWsStreamingAsrSession::ResolvedConnection DashScopeInferenceAsrSession::buildConnection() const {
	ResolvedConnection res;
	QString err;
	const QUrl url = resolveStreamingSpeechEndpointUrl(
		AgentSpeechProtocolMode::DashScopeInferenceWs,
		_config.endpointUrl,
		_config.model,
		_config.language,
		&err);
	if (!url.isValid()) {
		res.errorMessage = err;
		return res;
	}
	res.allowPlaintextLoopback = (url.scheme() == QStringLiteral("ws"));
	if (_config.apiKey.trimmed().isEmpty() && !res.allowPlaintextLoopback) {
		res.errorMessage = QCoreApplication::translate(
			"MeetingUI", "An STT API key is required for DashScope Inference WebSocket ASR.");
		return res;
	}
	res.url = url.toString(QUrl::FullyEncoded);
	res.bearerToken = _config.apiKey.trimmed();
	if (!_config.workspaceId.trimmed().isEmpty()) {
		res.extraHeaders.emplace_back("X-DashScope-WorkSpace", _config.workspaceId.trimmed().toStdString());
	}
	appendCustomHeaders(_config, res.extraHeaders);
	res.valid = true;
	return res;
}

void DashScopeInferenceAsrSession::onConnectedOnStrand() {
	const QString effectiveModel = _config.model.trimmed().isEmpty()
		? QStringLiteral("paraformer-realtime-v2")
		: _config.model.trimmed();
	const int sampleRate = requiredSampleRate();

	QJsonObject header;
	header.insert(QStringLiteral("action"), QStringLiteral("run-task"));
	header.insert(QStringLiteral("task_id"), QString::fromStdString(_taskId));
	header.insert(QStringLiteral("streaming"), QStringLiteral("duplex"));

	QJsonObject params;
	params.insert(QStringLiteral("format"), QStringLiteral("pcm"));
	params.insert(QStringLiteral("sample_rate"), sampleRate);
	if (!isLegacyV1Model(effectiveModel)) {
		params.insert(QStringLiteral("heartbeat"), true);
		if (!_config.language.trimmed().isEmpty() && !isNarrowband8kModel(effectiveModel)) {
			params.insert(QStringLiteral("language_hints"), QJsonArray{ _config.language.trimmed() });
		}
	}

	QJsonObject payload;
	payload.insert(QStringLiteral("task_group"), QStringLiteral("audio"));
	payload.insert(QStringLiteral("task"), QStringLiteral("asr"));
	payload.insert(QStringLiteral("function"), QStringLiteral("recognition"));
	payload.insert(QStringLiteral("model"), effectiveModel);
	payload.insert(QStringLiteral("parameters"), params);
	payload.insert(QStringLiteral("input"), QJsonObject{});

	QJsonObject root;
	root.insert(QStringLiteral("header"), header);
	root.insert(QStringLiteral("payload"), payload);
	sendTextOnStrand(QJsonDocument(root).toJson(QJsonDocument::Compact).toStdString());
}

void DashScopeInferenceAsrSession::sendAudioChunkOnStrand(const std::vector<uint8_t> &pcmBytes) {
	sendBinaryOnStrand(pcmBytes);
}

void DashScopeInferenceAsrSession::sendFinishOnStrand() {
	QJsonObject header;
	header.insert(QStringLiteral("action"), QStringLiteral("finish-task"));
	header.insert(QStringLiteral("task_id"), QString::fromStdString(_taskId));
	header.insert(QStringLiteral("streaming"), QStringLiteral("duplex"));

	QJsonObject payload;
	payload.insert(QStringLiteral("input"), QJsonObject{});

	QJsonObject root;
	root.insert(QStringLiteral("header"), header);
	root.insert(QStringLiteral("payload"), payload);
	sendTextOnStrand(QJsonDocument(root).toJson(QJsonDocument::Compact).toStdString());
}

void DashScopeInferenceAsrSession::onTextMessageOnStrand(const std::string &payload) {
	QJsonParseError err{};
	const QJsonDocument doc = QJsonDocument::fromJson(
		QByteArray(payload.data(), static_cast<int>(payload.size())), &err);
	if (err.error != QJsonParseError::NoError || !doc.isObject()) {
		return;
	}
	const QJsonObject root = doc.object();
	const QJsonObject header = root.value(QStringLiteral("header")).toObject();
	const QString event = header.value(QStringLiteral("event")).toString();

	if (event == QStringLiteral("task-started")) {
		markReadyOnStrand();
		return;
	}
	if (event == QStringLiteral("result-generated")) {
		const QJsonObject sentence = root.value(QStringLiteral("payload")).toObject()
			.value(QStringLiteral("output")).toObject()
			.value(QStringLiteral("sentence")).toObject();
		if (sentence.value(QStringLiteral("heartbeat")).toBool(false)) {
			return;
		}
		const QString text = sentence.value(QStringLiteral("text")).toString().trimmed();
		const bool hasSentenceEndKey = sentence.contains(QStringLiteral("sentence_end"));
		const bool hasEndTime = sentence.contains(QStringLiteral("end_time")) &&
			!sentence.value(QStringLiteral("end_time")).isNull();
		const bool sentenceEnd = hasSentenceEndKey
			? sentence.value(QStringLiteral("sentence_end")).toBool(false)
			: hasEndTime;
		QString segId;
		const QJsonValue serverSentenceId = sentence.value(QStringLiteral("sentence_id"));
		if (sentence.contains(QStringLiteral("sentence_id")) &&
			!serverSentenceId.isNull() &&
			!serverSentenceId.isUndefined()) {
			const QString sidStr = serverSentenceId.isString()
				? serverSentenceId.toString().trimmed()
				: QString::number(static_cast<qint64>(serverSentenceId.toDouble(0.0)));
			if (!sidStr.isEmpty()) {
				segId = QStringLiteral("ds-%1").arg(sidStr);
			}
		}
		if (segId.isEmpty()) {
			segId = QStringLiteral("ds-%1").arg(_segmentSeq);
		}
		if (!text.isEmpty()) {
			emitResultFromStrand(StreamingAsrResult{ segId, text, sentenceEnd, _config.language });
		}
		if (sentenceEnd) {
			++_segmentSeq;
		}
		return;
	}
	if (event == QStringLiteral("task-finished")) {
		finishCleanlyFromStrand();
		return;
	}
	if (event == QStringLiteral("task-failed")) {
		const QString msg = header.value(QStringLiteral("error_message")).toString().trimmed();
		const QString code = header.value(QStringLiteral("error_code")).toString().trimmed();
		if (code.compare(QStringLiteral("AllocationQuota.FreeTierOnly"), Qt::CaseInsensitive) == 0 ||
			msg.contains(QStringLiteral("free quota has been exhausted"), Qt::CaseInsensitive) ||
			msg.contains(QStringLiteral("use free tier only"), Qt::CaseInsensitive)) {
			failFromStrand(QCoreApplication::translate(
				"MeetingUI",
				"Free quota exhausted or 'use free tier only' mode is enabled for this model/region. Verify your DashScope region/workspace or disable free-tier-only in the console."));
			return;
		}
		failFromStrand(!msg.isEmpty() ? msg : (!code.isEmpty() ? code : QStringLiteral("DashScope ASR task failed.")));
	}
}

// =========================================================================
// 3. DeepgramAsrSession (Deepgram Nova-2 / Nova-3 Streaming)
// =========================================================================

DeepgramAsrSession::DeepgramAsrSession(const StreamingAsrConfig &config)
	: BaseWsStreamingAsrSession(config) {
	const QUrl epUrl(_config.endpointUrl.trimmed(), QUrl::StrictMode);
	if (epUrl.isValid() && epUrl.hasQuery()) {
		const QUrlQuery query(epUrl);
		const QString epVal = query.queryItemValue(QStringLiteral("endpointing")).trimmed().toLower();
		const int uttEndMs = query.queryItemValue(QStringLiteral("utterance_end_ms")).trimmed().toInt();
		if ((epVal == QStringLiteral("false") || epVal == QStringLiteral("0")) && uttEndMs <= 0) {
			_waitForSpeechFinal = false;
		}
	}
}

DeepgramAsrSession::~DeepgramAsrSession() {
	if (_keepAliveTimer) {
		std::error_code ec;
		_keepAliveTimer->cancel(ec);
	}
}

BaseWsStreamingAsrSession::ResolvedConnection DeepgramAsrSession::buildConnection() const {
	ResolvedConnection res;
	QString err;
	const QUrl url = resolveStreamingSpeechEndpointUrl(
		AgentSpeechProtocolMode::DeepgramWs,
		_config.endpointUrl,
		_config.model,
		_config.language,
		&err,
		_config.prompt);
	if (!url.isValid()) {
		res.errorMessage = err;
		return res;
	}
	res.allowPlaintextLoopback = (url.scheme() == QStringLiteral("ws"));
	if (_config.apiKey.trimmed().isEmpty() && !res.allowPlaintextLoopback) {
		res.errorMessage = QCoreApplication::translate(
			"MeetingUI", "A Deepgram API key is required for Deepgram WebSocket ASR.");
		return res;
	}
	res.url = url.toString(QUrl::FullyEncoded);
	if (!_config.apiKey.trimmed().isEmpty()) {
		res.extraHeaders.emplace_back(
			"Authorization",
			std::string("Token ") + _config.apiKey.trimmed().toStdString());
	}
	appendCustomHeaders(_config, res.extraHeaders);
	res.valid = true;
	return res;
}

void DeepgramAsrSession::onConnectedOnStrand() {
	markReadyOnStrand();
	armKeepAliveTimerOnStrand();
}

void DeepgramAsrSession::armKeepAliveTimerOnStrand() {
	cancelKeepAliveTimerOnStrand();
	if (isFinishingOnStrand()) {
		return;
	}
	_keepAliveTimer = std::make_shared<asio::steady_timer>(strandExecutor());
	_keepAliveTimer->expires_after(std::chrono::seconds(4));
	std::weak_ptr<Impl> weakImpl = impl();
	std::weak_ptr<BaseWsStreamingAsrSession> weakSelf = weak_from_this();
	_keepAliveTimer->async_wait([weakSelf, weakImpl](const std::error_code &ec) {
		if (ec) {
			return;
		}
		auto locked = weakImpl.lock();
		auto baseSelf = weakSelf.lock();
		auto *self = baseSelf ? static_cast<DeepgramAsrSession *>(baseSelf.get()) : nullptr;
		if (!locked || !self || locked->aborted.load(std::memory_order_acquire) ||
			locked->terminalNotified.load(std::memory_order_acquire) ||
			self->isFinishingOnStrand()) {
			return;
		}
		const auto now = std::chrono::steady_clock::now();
		const auto last = self->lastAudioSentOnStrand();
		if (last.time_since_epoch().count() == 0 ||
			std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count() >= 3500) {
			self->sendTextOnStrand(std::string(R"({"type":"KeepAlive"})"));
		}
		self->armKeepAliveTimerOnStrand();
	});
}

void DeepgramAsrSession::cancelKeepAliveTimerOnStrand() {
	if (_keepAliveTimer) {
		std::error_code ec;
		_keepAliveTimer->cancel(ec);
		_keepAliveTimer.reset();
	}
}

void DeepgramAsrSession::sendAudioChunkOnStrand(const std::vector<uint8_t> &pcmBytes) {
	sendBinaryOnStrand(pcmBytes);
}

void DeepgramAsrSession::sendFinishOnStrand() {
	cancelKeepAliveTimerOnStrand();
	sendTextOnStrand(std::string(R"({"type":"CloseStream"})"));
}

void DeepgramAsrSession::onBeforeFinishOnStrand() {
	cancelKeepAliveTimerOnStrand();
	flushFinalizedUtteranceOnStrand();
}

void DeepgramAsrSession::flushFinalizedUtteranceOnStrand() {
	if (_finalizedParts.isEmpty()) {
		return;
	}
	const QString fullText = joinDeepgramTranscriptParts(_finalizedParts);
	QString lang = _utteranceLanguage.trimmed();
	if (lang.isEmpty()) {
		const QString fallback = _config.language.trimmed();
		if (fallback.compare(QStringLiteral("auto"), Qt::CaseInsensitive) != 0) {
			lang = fallback;
		}
	}
	const QString segId = QStringLiteral("dg-%1").arg(_segmentSeq);
	_finalizedParts.clear();
	_utteranceLanguage.clear();
	++_segmentSeq;
	if (!fullText.isEmpty()) {
		emitResultFromStrand(StreamingAsrResult{ segId, fullText, true, lang });
	}
}

void DeepgramAsrSession::onTextMessageOnStrand(const std::string &payload) {
	QJsonParseError err{};
	const QJsonDocument doc = QJsonDocument::fromJson(
		QByteArray(payload.data(), static_cast<int>(payload.size())), &err);
	if (err.error != QJsonParseError::NoError || !doc.isObject()) {
		return;
	}
	const QJsonObject root = doc.object();
	const QString type = root.value(QStringLiteral("type")).toString();

	if (type == QStringLiteral("Results")) {
		const bool isFinal = root.value(QStringLiteral("is_final")).toBool(false);
		const bool speechFinal = root.value(QStringLiteral("speech_final")).toBool(false);
		const bool fromFinalize = root.value(QStringLiteral("from_finalize")).toBool(false);
		const QJsonObject channel = root.value(QStringLiteral("channel")).toObject();
		const QJsonArray alts = channel.value(QStringLiteral("alternatives")).toArray();
		QString transcript;
		QString detectedLang;
		if (!alts.isEmpty()) {
			const QJsonObject altObj = alts.first().toObject();
			transcript = altObj.value(QStringLiteral("transcript")).toString().trimmed();
			detectedLang = extractDeepgramDetectedLanguage(altObj, channel, _config.language);
		}
		if (isFinal) {
			if (!transcript.isEmpty()) {
				_finalizedParts.push_back(transcript);
				if (!detectedLang.isEmpty()) {
					_utteranceLanguage = detectedLang;
				}
			}
			if (!_waitForSpeechFinal || speechFinal || fromFinalize) {
				flushFinalizedUtteranceOnStrand();
			} else if (!_finalizedParts.isEmpty()) {
				const QString segId = QStringLiteral("dg-%1").arg(_segmentSeq);
				const QString interimText = joinDeepgramTranscriptParts(_finalizedParts);
				const QString interimLang = !_utteranceLanguage.isEmpty() ? _utteranceLanguage : detectedLang;
				emitResultFromStrand(StreamingAsrResult{ segId, interimText, false, interimLang });
			}
		} else {
			if (!transcript.isEmpty()) {
				const QString segId = QStringLiteral("dg-%1").arg(_segmentSeq);
				const QString interimText = joinDeepgramTranscriptParts(_finalizedParts, transcript);
				const QString interimLang = !detectedLang.isEmpty() ? detectedLang : _utteranceLanguage;
				emitResultFromStrand(StreamingAsrResult{ segId, interimText, false, interimLang });
			}
			if (speechFinal || fromFinalize) {
				flushFinalizedUtteranceOnStrand();
			}
		}
		return;
	}
	if (type == QStringLiteral("UtteranceEnd")) {
		flushFinalizedUtteranceOnStrand();
		return;
	}
	if (type == QStringLiteral("Metadata")) {
		flushFinalizedUtteranceOnStrand();
		if (isFinishingOnStrand()) {
			finishCleanlyFromStrand();
		}
		return;
	}
	if (type == QStringLiteral("Error")) {
		QString detail = root.value(QStringLiteral("description")).toString().trimmed();
		if (detail.isEmpty()) {
			detail = extractJsonErrorMessage(root);
		}
		failFromStrand(detail.isEmpty() ? QStringLiteral("Deepgram streaming error.") : detail);
	}
}

// =========================================================================
// 4. AssemblyAiV3AsrSession (AssemblyAI Universal-Streaming v3)
// =========================================================================

AssemblyAiV3AsrSession::AssemblyAiV3AsrSession(const StreamingAsrConfig &config)
	: BaseWsStreamingAsrSession(config) {
	const QUrl epUrl(_config.endpointUrl.trimmed(), QUrl::StrictMode);
	if (epUrl.isValid() && epUrl.hasQuery()) {
		const QUrlQuery query(epUrl);
		const QString fmt = query.queryItemValue(QStringLiteral("format_turns")).trimmed().toLower();
		if (fmt == QStringLiteral("false") || fmt == QStringLiteral("0")) {
			_expectFormattedTurns = false;
		}
	}
}

void AssemblyAiV3AsrSession::flushPendingFinalTurnOnStrand() {
	if (_pendingFinalTurnOrder < 0) {
		return;
	}
	const qint64 turnOrder = _pendingFinalTurnOrder;
	const QString text = _pendingFinalText;
	const QString lang = _pendingFinalLanguage;
	_pendingFinalTurnOrder = -1;
	_pendingFinalText.clear();
	_pendingFinalLanguage.clear();
	if (turnOrder > _lastFinalTurnOrder) {
		_lastFinalTurnOrder = turnOrder;
		if (!text.isEmpty()) {
			emitResultFromStrand(StreamingAsrResult{
				QStringLiteral("aai-%1").arg(turnOrder),
				text,
				true,
				lang });
		}
	}
}

BaseWsStreamingAsrSession::ResolvedConnection AssemblyAiV3AsrSession::buildConnection() const {
	ResolvedConnection res;
	QString err;
	const QUrl url = resolveStreamingSpeechEndpointUrl(
		AgentSpeechProtocolMode::AssemblyAiV3Ws,
		_config.endpointUrl,
		_config.model,
		_config.language,
		&err);
	if (!url.isValid()) {
		res.errorMessage = err;
		return res;
	}
	res.allowPlaintextLoopback = (url.scheme() == QStringLiteral("ws"));
	if (_config.apiKey.trimmed().isEmpty() && !res.allowPlaintextLoopback) {
		res.errorMessage = QCoreApplication::translate(
			"MeetingUI", "An AssemblyAI API key is required for AssemblyAI v3 WebSocket ASR.");
		return res;
	}
	res.url = url.toString(QUrl::FullyEncoded);
	if (!_config.apiKey.trimmed().isEmpty()) {
		// AssemblyAI v3 expects the bare API key in Authorization without a Bearer prefix.
		res.extraHeaders.emplace_back("Authorization", _config.apiKey.trimmed().toStdString());
	}
	appendCustomHeaders(_config, res.extraHeaders);
	res.valid = true;
	return res;
}

void AssemblyAiV3AsrSession::onConnectedOnStrand() {
	// Wait for AssemblyAI v3 Begin event before streaming queued PCM.
}

void AssemblyAiV3AsrSession::sendAudioChunkOnStrand(const std::vector<uint8_t> &pcmBytes) {
	sendBinaryOnStrand(pcmBytes);
}

void AssemblyAiV3AsrSession::sendFinishOnStrand() {
	sendTextOnStrand(std::string(R"({"type":"Terminate"})"));
}

void AssemblyAiV3AsrSession::onTextMessageOnStrand(const std::string &payload) {
	QJsonParseError err{};
	const QJsonDocument doc = QJsonDocument::fromJson(
		QByteArray(payload.data(), static_cast<int>(payload.size())), &err);
	if (err.error != QJsonParseError::NoError || !doc.isObject()) {
		return;
	}
	const QJsonObject root = doc.object();
	const QString type = root.value(QStringLiteral("type")).toString();

	if (type == QStringLiteral("Begin")) {
		QString effectiveModel = _config.model.trimmed();
		const QUrl epUrl(_config.endpointUrl.trimmed(), QUrl::StrictMode);
		if (epUrl.isValid() && epUrl.hasQuery()) {
			const QString queryModel =
				QUrlQuery(epUrl).queryItemValue(QStringLiteral("speech_model")).trimmed();
			if (!queryModel.isEmpty()) {
				effectiveModel = queryModel;
			}
		}
		QString effectiveModelLower = effectiveModel.toLower();
		if (effectiveModelLower == QStringLiteral("universal-streaming")) {
			effectiveModelLower = QStringLiteral("assemblyai-stt-3-7-realtime");
		}
		if (!effectiveModelLower.startsWith(QStringLiteral("universal-streaming"))) {
			QJsonObject updateCfg;
			const QString trimmedPrompt = _config.prompt.trimmed();
			if (!trimmedPrompt.isEmpty()) {
				updateCfg.insert(QStringLiteral("prompt"), trimmedPrompt.left(1750));
			}
			const QJsonArray langCodes = parseAssemblyAiLanguageCodes(_config.language);
			if (!langCodes.isEmpty()) {
				updateCfg.insert(QStringLiteral("language_codes"), langCodes);
			}
			if (!updateCfg.isEmpty()) {
				updateCfg.insert(QStringLiteral("type"), QStringLiteral("UpdateConfiguration"));
				sendTextOnStrand(QJsonDocument(updateCfg).toJson(QJsonDocument::Compact).toStdString());
			}
		}
		markReadyOnStrand();
		return;
	}
	if (type == QStringLiteral("Turn")) {
		const qint64 turnOrder = static_cast<qint64>(root.value(QStringLiteral("turn_order")).toDouble(0.0));
		const QString transcript = root.value(QStringLiteral("transcript")).toString().trimmed();
		const bool endOfTurn = root.value(QStringLiteral("end_of_turn")).toBool(false);
		const bool hasFormattedFlag = root.contains(QStringLiteral("turn_is_formatted")) &&
			root.value(QStringLiteral("turn_is_formatted")).isBool();
		const bool turnIsFormatted = root.value(QStringLiteral("turn_is_formatted")).toBool(false);
		const QString detectedLang = root.value(QStringLiteral("language_code")).toString().trimmed();
		const QString effectiveLang = !detectedLang.isEmpty() ? detectedLang : _config.language;

		if (_pendingFinalTurnOrder >= 0 && turnOrder > _pendingFinalTurnOrder) {
			flushPendingFinalTurnOnStrand();
		}

		const QString segId = QStringLiteral("aai-%1").arg(turnOrder);
		if (endOfTurn) {
			if (turnOrder <= _lastFinalTurnOrder) {
				return;
			}
			if (_expectFormattedTurns && hasFormattedFlag && !turnIsFormatted) {
				_pendingFinalTurnOrder = turnOrder;
				_pendingFinalText = transcript;
				_pendingFinalLanguage = effectiveLang;
				if (!transcript.isEmpty()) {
					emitResultFromStrand(StreamingAsrResult{ segId, transcript, false, effectiveLang });
				}
			} else {
				QString fallbackText;
				QString fallbackLang;
				if (_pendingFinalTurnOrder == turnOrder) {
					fallbackText = _pendingFinalText;
					fallbackLang = _pendingFinalLanguage;
					_pendingFinalTurnOrder = -1;
					_pendingFinalText.clear();
					_pendingFinalLanguage.clear();
				}
				_lastFinalTurnOrder = turnOrder;
				const QString finalText = !transcript.isEmpty() ? transcript : fallbackText;
				const QString finalLang = !detectedLang.isEmpty()
					? detectedLang
					: (!fallbackLang.isEmpty() ? fallbackLang : _config.language);
				if (!finalText.isEmpty()) {
					emitResultFromStrand(StreamingAsrResult{ segId, finalText, true, finalLang });
				}
			}
		} else if (turnOrder > _lastFinalTurnOrder && !transcript.isEmpty()) {
			emitResultFromStrand(StreamingAsrResult{ segId, transcript, false, effectiveLang });
		}
		return;
	}
	if (type == QStringLiteral("Termination")) {
		flushPendingFinalTurnOnStrand();
		finishCleanlyFromStrand();
		return;
	}
	if (type == QStringLiteral("Error") || root.contains(QStringLiteral("error"))) {
		const QString detail = extractJsonErrorMessage(root);
		failFromStrand(detail.isEmpty() ? QStringLiteral("AssemblyAI streaming error.") : detail);
	}
}

// =========================================================================
// 5. VolcengineSaucAsrSession (Volcengine Doubao SAUC v3 Binary Protocol)
// =========================================================================

VolcengineSaucAsrSession::VolcengineSaucAsrSession(const StreamingAsrConfig &config)
	: BaseWsStreamingAsrSession(config) {
}

BaseWsStreamingAsrSession::ResolvedConnection VolcengineSaucAsrSession::buildConnection() const {
	ResolvedConnection res;
	QString err;
	const QUrl url = resolveStreamingSpeechEndpointUrl(
		AgentSpeechProtocolMode::VolcengineBigModelWs,
		_config.endpointUrl,
		_config.model,
		_config.language,
		&err);
	if (!url.isValid()) {
		res.errorMessage = err;
		return res;
	}
	res.allowPlaintextLoopback = (url.scheme() == QStringLiteral("ws"));
	const QString apiKey = _config.apiKey.trimmed();
	const QString appKey = _config.appKey.trimmed();
	if (!res.allowPlaintextLoopback && apiKey.isEmpty()) {
		res.errorMessage = QCoreApplication::translate(
			"MeetingUI", "Volcengine streaming ASR requires an API Key (or legacy App Key + Access Key).");
		return res;
	}
	res.url = url.toString(QUrl::FullyEncoded);
	const QString resourceId = _config.resourceId.trimmed().isEmpty()
		? (appKey.isEmpty()
			? QStringLiteral("volc.seedasr.sauc.duration")
			: QStringLiteral("volc.bigasr.sauc.duration"))
		: _config.resourceId.trimmed();
	const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	const QString connectId = QUuid::createUuid().toString(QUuid::WithoutBraces);

	if (!appKey.isEmpty()) {
		res.extraHeaders.emplace_back("X-Api-App-Key", appKey.toStdString());
		if (!apiKey.isEmpty()) {
			res.extraHeaders.emplace_back("X-Api-Access-Key", apiKey.toStdString());
		}
	} else if (!apiKey.isEmpty()) {
		res.extraHeaders.emplace_back("X-Api-Key", apiKey.toStdString());
	}
	res.extraHeaders.emplace_back("X-Api-Resource-Id", resourceId.toStdString());
	res.extraHeaders.emplace_back("X-Api-Request-Id", requestId.toStdString());
	res.extraHeaders.emplace_back("X-Api-Connect-Id", connectId.toStdString());
	appendCustomHeaders(_config, res.extraHeaders);
	res.valid = true;
	return res;
}

void VolcengineSaucAsrSession::onConnectedOnStrand() {
	QJsonObject userObj;
	userObj.insert(QStringLiteral("uid"), QStringLiteral("cohavora"));

	QJsonObject audioObj;
	audioObj.insert(QStringLiteral("format"), QStringLiteral("pcm"));
	audioObj.insert(QStringLiteral("rate"), 16000);
	audioObj.insert(QStringLiteral("bits"), 16);
	audioObj.insert(QStringLiteral("channel"), 1);
	if (!_config.language.trimmed().isEmpty()) {
		audioObj.insert(QStringLiteral("language"), _config.language.trimmed());
	}

	QJsonObject reqObj;
	reqObj.insert(QStringLiteral("model_name"), QStringLiteral("bigmodel"));
	reqObj.insert(QStringLiteral("enable_itn"), true);
	reqObj.insert(QStringLiteral("enable_punc"), true);
	reqObj.insert(QStringLiteral("show_utterances"), true);

	QJsonObject root;
	root.insert(QStringLiteral("user"), userObj);
	root.insert(QStringLiteral("audio"), audioObj);
	root.insert(QStringLiteral("request"), reqObj);

	sendBinaryOnStrand(buildVolcengineFullClientPacket(
		QJsonDocument(root).toJson(QJsonDocument::Compact), 1, true));
	markReadyOnStrand();
}

void VolcengineSaucAsrSession::sendAudioChunkOnStrand(const std::vector<uint8_t> &pcmBytes) {
	sendBinaryOnStrand(buildVolcengineAudioPacket(
		pcmBytes.data(), pcmBytes.size(), _nextSequence++, false, true));
}

void VolcengineSaucAsrSession::sendFinishOnStrand() {
	sendBinaryOnStrand(buildVolcengineAudioPacket(
		nullptr, 0, _nextSequence++, true, true));
}

void VolcengineSaucAsrSession::onTextMessageOnStrand(const std::string &payload) {
	QJsonParseError err{};
	const QJsonDocument doc = QJsonDocument::fromJson(
		QByteArray(payload.data(), static_cast<int>(payload.size())), &err);
	if (err.error == QJsonParseError::NoError && doc.isObject()) {
		const QString msg = extractJsonErrorMessage(doc.object());
		if (!msg.isEmpty()) {
			failFromStrand(msg);
		}
	}
}

void VolcengineSaucAsrSession::onBinaryMessageOnStrand(const std::vector<uint8_t> &payload) {
	const VolcengineSaucServerPacket pkt = parseVolcengineServerPacket(payload);
	if (!pkt.valid) {
		return;
	}
	if (pkt.isError) {
		QString detail = QString::fromUtf8(pkt.payload).trimmed();
		const QJsonDocument doc = QJsonDocument::fromJson(pkt.payload);
		if (doc.isObject()) {
			const QString jsonMsg = extractJsonErrorMessage(doc.object());
			if (!jsonMsg.isEmpty()) {
				detail = jsonMsg;
			}
		}
		failFromStrand(QStringLiteral("Volcengine ASR error %1: %2")
			.arg(pkt.errorCode)
			.arg(detail));
		return;
	}
	if (pkt.messageType != 0x09) {
		return;
	}

	QJsonParseError err{};
	const QJsonDocument doc = QJsonDocument::fromJson(pkt.payload, &err);
	if (err.error == QJsonParseError::NoError && doc.isObject()) {
		const QJsonObject resultObj = doc.object().value(QStringLiteral("result")).toObject();
		const QJsonArray utterances = resultObj.value(QStringLiteral("utterances")).toArray();
		if (!utterances.isEmpty()) {
			for (int i = _committedUtterances; i < utterances.size(); ++i) {
				const QJsonObject u = utterances.at(i).toObject();
				const QString text = u.value(QStringLiteral("text")).toString().trimmed();
				const bool definite = u.value(QStringLiteral("definite")).toBool(false) || pkt.isFinalPacket;
				const QString segId = QStringLiteral("volc-%1").arg(i + 1);
				if (!text.isEmpty()) {
					emitResultFromStrand(StreamingAsrResult{ segId, text, definite, _config.language });
				}
				if (definite) {
					_committedUtterances = i + 1;
				} else {
					break;
				}
			}
		} else {
			const QString text = resultObj.value(QStringLiteral("text")).toString().trimmed();
			if (!text.isEmpty()) {
				const QString segId = QStringLiteral("volc-%1").arg(_committedUtterances + 1);
				emitResultFromStrand(StreamingAsrResult{ segId, text, pkt.isFinalPacket, _config.language });
				if (pkt.isFinalPacket) {
					++_committedUtterances;
				}
			}
		}
	}

	if (pkt.isFinalPacket) {
		finishCleanlyFromStrand();
	}
}

// =========================================================================
// 6. ElevenLabsScribeAsrSession (ElevenLabs Scribe Realtime)
// =========================================================================

ElevenLabsScribeAsrSession::ElevenLabsScribeAsrSession(const StreamingAsrConfig &config)
	: BaseWsStreamingAsrSession(config) {
	const QUrl resolvedUrl = resolveStreamingSpeechEndpointUrl(
		AgentSpeechProtocolMode::ElevenLabsScribeWs,
		_config.endpointUrl,
		_config.model,
		_config.language);
	if (resolvedUrl.isValid() && resolvedUrl.hasQuery()) {
		const QUrlQuery query(resolvedUrl);
		const QString incTs = query.queryItemValue(QStringLiteral("include_timestamps")).trimmed().toLower();
		_expectTimestamps = (incTs == QStringLiteral("true") || incTs == QStringLiteral("1"));
	}
}

BaseWsStreamingAsrSession::ResolvedConnection ElevenLabsScribeAsrSession::buildConnection() const {
	ResolvedConnection res;
	QString err;
	QUrl url = resolveStreamingSpeechEndpointUrl(
		AgentSpeechProtocolMode::ElevenLabsScribeWs,
		_config.endpointUrl,
		_config.model,
		_config.language,
		&err);
	if (!url.isValid()) {
		res.errorMessage = err;
		return res;
	}
	if (!_config.enableServerVad) {
		const QUrl rawEp(_config.endpointUrl.trimmed(), QUrl::StrictMode);
		const bool userProvidedStrategy = rawEp.isValid() && rawEp.hasQuery() &&
			QUrlQuery(rawEp).hasQueryItem(QStringLiteral("commit_strategy"));
		if (!userProvidedStrategy) {
			QUrlQuery q(url);
			q.removeAllQueryItems(QStringLiteral("commit_strategy"));
			q.addQueryItem(QStringLiteral("commit_strategy"), QStringLiteral("manual"));
			q.removeAllQueryItems(QStringLiteral("vad_silence_threshold_secs"));
			url.setQuery(q);
		}
	}
	res.allowPlaintextLoopback = (url.scheme() == QStringLiteral("ws"));
	const QString trimmedKey = _config.apiKey.trimmed();
	if (trimmedKey.isEmpty() && !res.allowPlaintextLoopback) {
		res.errorMessage = QCoreApplication::translate(
			"MeetingUI", "An ElevenLabs API key is required for ElevenLabs Scribe WebSocket ASR.");
		return res;
	}
	if (isElevenLabsSingleUseToken(trimmedKey)) {
		QUrlQuery q(url);
		q.removeAllQueryItems(QStringLiteral("token"));
		q.addQueryItem(QStringLiteral("token"), trimmedKey);
		url.setQuery(q);
	} else if (!trimmedKey.isEmpty()) {
		res.extraHeaders.emplace_back("xi-api-key", trimmedKey.toStdString());
	}
	res.url = url.toString(QUrl::FullyEncoded);
	appendCustomHeaders(_config, res.extraHeaders);
	res.valid = true;
	return res;
}

void ElevenLabsScribeAsrSession::onConnectedOnStrand() {
	// Wait for ElevenLabs session_started event.
}

void ElevenLabsScribeAsrSession::sendAudioChunkOnStrand(const std::vector<uint8_t> &pcmBytes) {
	const bool isFirstChunk = !_hasAudioSent;
	_hasAudioSent = true;
	QJsonObject msg;
	msg.insert(QStringLiteral("message_type"), QStringLiteral("input_audio_chunk"));
	msg.insert(QStringLiteral("audio_base_64"), makeBase64Pcm(pcmBytes));
	msg.insert(QStringLiteral("commit"), false);
	msg.insert(QStringLiteral("sample_rate"), 16000);
	if (isFirstChunk) {
		const QString trimmedPrompt = _config.prompt.trimmed();
		if (!trimmedPrompt.isEmpty()) {
			msg.insert(QStringLiteral("previous_text"), trimmedPrompt.left(50));
		}
	}
	sendTextOnStrand(QJsonDocument(msg).toJson(QJsonDocument::Compact).toStdString());
}

void ElevenLabsScribeAsrSession::sendFinishOnStrand() {
	if (!_hasAudioSent) {
		finishCleanlyFromStrand();
		return;
	}
	QJsonObject msg;
	msg.insert(QStringLiteral("message_type"), QStringLiteral("input_audio_chunk"));
	msg.insert(QStringLiteral("audio_base_64"), QStringLiteral(""));
	msg.insert(QStringLiteral("commit"), true);
	msg.insert(QStringLiteral("sample_rate"), 16000);
	sendTextOnStrand(QJsonDocument(msg).toJson(QJsonDocument::Compact).toStdString());
}

void ElevenLabsScribeAsrSession::onBeforeFinishOnStrand() {
	flushPendingCommitOnStrand();
}

void ElevenLabsScribeAsrSession::flushPendingCommitOnStrand() {
	if (!_hasPendingCommit) {
		return;
	}
	const QString text = _pendingCommitText;
	QString lang = _pendingCommitLanguage.trimmed();
	if (lang.isEmpty()) {
		lang = _lastPartialLanguage.trimmed();
	}
	if (lang.isEmpty()) {
		lang = normalizeIso639PrimaryLanguage(_config.language);
	}
	_hasPendingCommit = false;
	_pendingCommitText.clear();
	_pendingCommitLanguage.clear();
	_lastPartialLanguage.clear();
	if (!text.isEmpty()) {
		const QString segId = QStringLiteral("el-%1").arg(_segmentSeq);
		emitResultFromStrand(StreamingAsrResult{ segId, text, true, lang });
		++_segmentSeq;
	}
}

void ElevenLabsScribeAsrSession::onTextMessageOnStrand(const std::string &payload) {
	QJsonParseError err{};
	const QJsonDocument doc = QJsonDocument::fromJson(
		QByteArray(payload.data(), static_cast<int>(payload.size())), &err);
	if (err.error != QJsonParseError::NoError || !doc.isObject()) {
		return;
	}
	const QJsonObject root = doc.object();
	const QString type = root.value(QStringLiteral("message_type")).toString();

	if (type == QStringLiteral("session_started")) {
		markReadyOnStrand();
		return;
	}
	if (type == QStringLiteral("partial_transcript")) {
		if (_hasPendingCommit) {
			flushPendingCommitOnStrand();
		}
		const QString detectedLang = root.value(QStringLiteral("language_code")).toString().trimmed();
		if (!detectedLang.isEmpty()) {
			_lastPartialLanguage = detectedLang;
		}
		QString effectiveLang = detectedLang;
		if (effectiveLang.isEmpty()) {
			effectiveLang = _lastPartialLanguage.trimmed();
		}
		if (effectiveLang.isEmpty()) {
			effectiveLang = normalizeIso639PrimaryLanguage(_config.language);
		}
		const QString text = root.value(QStringLiteral("text")).toString().trimmed();
		if (!text.isEmpty()) {
			const QString segId = QStringLiteral("el-%1").arg(_segmentSeq);
			emitResultFromStrand(StreamingAsrResult{ segId, text, false, effectiveLang });
		}
		return;
	}
	if (type == QStringLiteral("committed_transcript")) {
		if (_hasPendingCommit) {
			flushPendingCommitOnStrand();
		}
		const QString text = root.value(QStringLiteral("text")).toString().trimmed();
		const QString detectedLang = root.value(QStringLiteral("language_code")).toString().trimmed();
		QString commitLang = detectedLang;
		if (commitLang.isEmpty()) {
			commitLang = _lastPartialLanguage.trimmed();
		}
		if (commitLang.isEmpty()) {
			commitLang = normalizeIso639PrimaryLanguage(_config.language);
		}
		if (_expectTimestamps) {
			if (!text.isEmpty()) {
				_hasPendingCommit = true;
				_pendingCommitText = text;
				_pendingCommitLanguage = commitLang;
				const QString segId = QStringLiteral("el-%1").arg(_segmentSeq);
				emitResultFromStrand(StreamingAsrResult{ segId, text, false, commitLang });
			} else if (isFinishingOnStrand()) {
				finishCleanlyFromStrand();
			}
		} else {
			_lastPartialLanguage.clear();
			if (!text.isEmpty()) {
				const QString segId = QStringLiteral("el-%1").arg(_segmentSeq);
				emitResultFromStrand(StreamingAsrResult{ segId, text, true, commitLang });
				++_segmentSeq;
			}
			if (isFinishingOnStrand()) {
				finishCleanlyFromStrand();
			}
		}
		return;
	}
	if (type == QStringLiteral("committed_transcript_with_timestamps")) {
		const QString text = root.value(QStringLiteral("text")).toString().trimmed();
		const QString detectedLang = root.value(QStringLiteral("language_code")).toString().trimmed();
		if (_hasPendingCommit) {
			const QString finalText = !text.isEmpty() ? text : _pendingCommitText;
			QString finalLang = !detectedLang.isEmpty() ? detectedLang : _pendingCommitLanguage;
			if (finalLang.isEmpty()) {
				finalLang = _lastPartialLanguage.trimmed();
			}
			if (finalLang.isEmpty()) {
				finalLang = normalizeIso639PrimaryLanguage(_config.language);
			}
			_hasPendingCommit = false;
			_pendingCommitText.clear();
			_pendingCommitLanguage.clear();
			_lastPartialLanguage.clear();
			if (!finalText.isEmpty()) {
				const QString segId = QStringLiteral("el-%1").arg(_segmentSeq);
				emitResultFromStrand(StreamingAsrResult{ segId, finalText, true, finalLang });
				++_segmentSeq;
			}
		} else if (_expectTimestamps && !text.isEmpty()) {
			QString finalLang = detectedLang;
			if (finalLang.isEmpty()) {
				finalLang = _lastPartialLanguage.trimmed();
			}
			if (finalLang.isEmpty()) {
				finalLang = normalizeIso639PrimaryLanguage(_config.language);
			}
			_lastPartialLanguage.clear();
			const QString segId = QStringLiteral("el-%1").arg(_segmentSeq);
			emitResultFromStrand(StreamingAsrResult{ segId, text, true, finalLang });
			++_segmentSeq;
		}
		if (isFinishingOnStrand()) {
			finishCleanlyFromStrand();
		}
		return;
	}
	if (type == QStringLiteral("commit_throttled")) {
		if (isFinishingOnStrand() && !_hasPendingCommit) {
			finishCleanlyFromStrand();
		}
		return;
	}
	if (type == QStringLiteral("error") ||
		type.endsWith(QStringLiteral("_error")) ||
		type == QStringLiteral("invalid_request") ||
		type == QStringLiteral("insufficient_audio_activity") ||
		type == QStringLiteral("quota_exceeded") ||
		type == QStringLiteral("rate_limited") ||
		type == QStringLiteral("chunk_size_exceeded") ||
		type == QStringLiteral("session_time_limit_exceeded") ||
		type == QStringLiteral("queue_overflow") ||
		type == QStringLiteral("resource_exhausted") ||
		type == QStringLiteral("unaccepted_terms") ||
		root.contains(QStringLiteral("error"))) {
		const QString detail = extractJsonErrorMessage(root);
		failFromStrand(detail.isEmpty() ? type : detail);
	}
}

// =========================================================================
// 7. XaiSttAsrSession (xAI Grok Streaming STT WebSocket /v1/stt)
// =========================================================================

XaiSttAsrSession::XaiSttAsrSession(const StreamingAsrConfig &config)
	: BaseWsStreamingAsrSession(config) {
}

BaseWsStreamingAsrSession::ResolvedConnection XaiSttAsrSession::buildConnection() const {
	ResolvedConnection res;
	QString err;
	const QUrl url = resolveStreamingSpeechEndpointUrl(
		AgentSpeechProtocolMode::XaiSttWs,
		_config.endpointUrl,
		_config.model,
		_config.language,
		&err,
		_config.prompt);
	if (!url.isValid()) {
		res.errorMessage = err;
		return res;
	}
	res.allowPlaintextLoopback = (url.scheme() == QStringLiteral("ws"));
	if (_config.apiKey.trimmed().isEmpty() && !res.allowPlaintextLoopback) {
		res.errorMessage = QCoreApplication::translate(
			"MeetingUI", "An xAI API key is required for xAI Grok STT WebSocket ASR.");
		return res;
	}
	res.url = url.toString(QUrl::FullyEncoded);
	res.bearerToken = _config.apiKey.trimmed();
	appendCustomHeaders(_config, res.extraHeaders);
	res.valid = true;
	return res;
}

void XaiSttAsrSession::onConnectedOnStrand() {
	// Wait for xAI transcript.created event before streaming queued PCM.
}

void XaiSttAsrSession::sendAudioChunkOnStrand(const std::vector<uint8_t> &pcmBytes) {
	sendBinaryOnStrand(pcmBytes);
}

void XaiSttAsrSession::sendFinishOnStrand() {
	sendTextOnStrand(std::string(R"({"type":"audio.done"})"));
}

void XaiSttAsrSession::onBeforeFinishOnStrand() {
	if (!_finalizedParts.isEmpty()) {
		flushFinalizedUtteranceOnStrand();
		return;
	}
	const QString trailingInterim = _lastInterimText.trimmed();
	if (!trailingInterim.isEmpty()) {
		QString lang = _lastInterimLanguage.trimmed();
		if (lang.isEmpty()) {
			lang = normalizeIso639PrimaryLanguage(_config.language);
		}
		const QString segId = QStringLiteral("xai-%1").arg(_segmentSeq);
		++_segmentSeq;
		++_emittedFinalCount;
		_lastInterimText.clear();
		_lastInterimLanguage.clear();
		emitResultFromStrand(StreamingAsrResult{ segId, trailingInterim, true, lang });
	}
}

void XaiSttAsrSession::flushFinalizedUtteranceOnStrand() {
	if (_finalizedParts.isEmpty()) {
		return;
	}
	const QString fullText = joinDeepgramTranscriptParts(_finalizedParts);
	QString lang = _utteranceLanguage.trimmed();
	if (lang.isEmpty()) {
		lang = normalizeIso639PrimaryLanguage(_config.language);
	}
	_finalizedParts.clear();
	_utteranceLanguage.clear();
	_lastInterimText.clear();
	_lastInterimLanguage.clear();
	if (!fullText.isEmpty()) {
		const QString segId = QStringLiteral("xai-%1").arg(_segmentSeq);
		++_segmentSeq;
		++_emittedFinalCount;
		emitResultFromStrand(StreamingAsrResult{ segId, fullText, true, lang });
	}
}

void XaiSttAsrSession::onTextMessageOnStrand(const std::string &payload) {
	QJsonParseError err{};
	const QJsonDocument doc = QJsonDocument::fromJson(
		QByteArray(payload.data(), static_cast<int>(payload.size())), &err);
	if (err.error != QJsonParseError::NoError || !doc.isObject()) {
		return;
	}
	const QJsonObject root = doc.object();
	const QString type = root.value(QStringLiteral("type")).toString();

	if (type == QStringLiteral("transcript.created")) {
		_readyMarked = true;
		markReadyOnStrand();
		return;
	}
	if (type == QStringLiteral("error") ||
		(root.contains(QStringLiteral("error")) && !root.value(QStringLiteral("error")).isNull())) {
		QString detail = extractJsonErrorMessage(root);
		if (detail.isEmpty()) {
			detail = root.value(QStringLiteral("errorCode")).toString().trimmed();
		}
		failFromStrand(detail.isEmpty() ? QStringLiteral("xAI STT streaming error.") : detail);
		return;
	}

	const bool isTranscriptEvent =
		type == QStringLiteral("transcript.partial") ||
		type == QStringLiteral("transcript.utterance") ||
		(type.isEmpty() &&
		 (root.contains(QStringLiteral("text")) || root.contains(QStringLiteral("transcript"))) &&
		 (root.contains(QStringLiteral("is_final")) || root.contains(QStringLiteral("speech_final"))));
	if (isTranscriptEvent) {
		if (!_readyMarked) {
			_readyMarked = true;
			markReadyOnStrand();
		}
		QString text = root.value(QStringLiteral("text")).toString().trimmed();
		if (text.isEmpty()) {
			text = root.value(QStringLiteral("transcript")).toString().trimmed();
		}
		const bool defaultUtteranceFinal = (type == QStringLiteral("transcript.utterance"));
		const bool isFinal = root.value(QStringLiteral("is_final")).toBool(defaultUtteranceFinal);
		const bool speechFinal = root.value(QStringLiteral("speech_final")).toBool(defaultUtteranceFinal);
		QString detectedLang = root.value(QStringLiteral("language")).toString().trimmed();
		if (detectedLang.isEmpty()) {
			detectedLang = root.value(QStringLiteral("language_code")).toString().trimmed();
		}

		if (isFinal) {
			if (!text.isEmpty()) {
				_finalizedParts.push_back(text);
				if (!detectedLang.isEmpty()) {
					_utteranceLanguage = detectedLang;
				}
			}
			_lastInterimText.clear();
			_lastInterimLanguage.clear();
			if (speechFinal) {
				flushFinalizedUtteranceOnStrand();
			} else if (!_finalizedParts.isEmpty()) {
				const QString segId = QStringLiteral("xai-%1").arg(_segmentSeq);
				const QString interimText = joinDeepgramTranscriptParts(_finalizedParts);
				QString interimLang = !_utteranceLanguage.isEmpty() ? _utteranceLanguage : detectedLang;
				if (interimLang.isEmpty()) {
					interimLang = normalizeIso639PrimaryLanguage(_config.language);
				}
				emitResultFromStrand(StreamingAsrResult{ segId, interimText, false, interimLang });
			}
		} else {
			QString interimLang = !detectedLang.isEmpty() ? detectedLang : _utteranceLanguage;
			if (interimLang.isEmpty()) {
				interimLang = normalizeIso639PrimaryLanguage(_config.language);
			}
			if (!text.isEmpty()) {
				const QString segId = QStringLiteral("xai-%1").arg(_segmentSeq);
				const QString interimText = joinDeepgramTranscriptParts(_finalizedParts, text);
				_lastInterimText = interimText;
				_lastInterimLanguage = interimLang;
				emitResultFromStrand(StreamingAsrResult{ segId, interimText, false, interimLang });
			}
			if (speechFinal) {
				if (!_finalizedParts.isEmpty()) {
					flushFinalizedUtteranceOnStrand();
				} else if (!_lastInterimText.isEmpty()) {
					const QString segId = QStringLiteral("xai-%1").arg(_segmentSeq);
					const QString promoted = _lastInterimText;
					const QString promotedLang = !_lastInterimLanguage.isEmpty()
						? _lastInterimLanguage
						: interimLang;
					++_segmentSeq;
					++_emittedFinalCount;
					_lastInterimText.clear();
					_lastInterimLanguage.clear();
					emitResultFromStrand(StreamingAsrResult{ segId, promoted, true, promotedLang });
				}
			}
		}
		return;
	}

	if (type == QStringLiteral("transcript.done")) {
		if (!_readyMarked) {
			_readyMarked = true;
			markReadyOnStrand();
		}
		const QString doneText = root.value(QStringLiteral("text")).toString().trimmed();
		QString doneLang = root.value(QStringLiteral("language")).toString().trimmed();
		if (doneLang.isEmpty()) {
			doneLang = root.value(QStringLiteral("language_code")).toString().trimmed();
		}
		if (!_finalizedParts.isEmpty()) {
			if (_utteranceLanguage.isEmpty() && !doneLang.isEmpty()) {
				_utteranceLanguage = doneLang;
			}
			flushFinalizedUtteranceOnStrand();
		} else if (_emittedFinalCount == 0 && !doneText.isEmpty()) {
			QString lang = doneLang;
			if (lang.isEmpty()) {
				lang = normalizeIso639PrimaryLanguage(_config.language);
			}
			const QString segId = QStringLiteral("xai-%1").arg(_segmentSeq);
			++_segmentSeq;
			++_emittedFinalCount;
			emitResultFromStrand(StreamingAsrResult{ segId, doneText, true, lang });
		} else if (!_lastInterimText.isEmpty()) {
			QString lang = _lastInterimLanguage.trimmed();
			if (lang.isEmpty()) {
				lang = !doneLang.isEmpty()
					? doneLang
					: normalizeIso639PrimaryLanguage(_config.language);
			}
			const QString segId = QStringLiteral("xai-%1").arg(_segmentSeq);
			const QString finalText = _lastInterimText;
			++_segmentSeq;
			++_emittedFinalCount;
			emitResultFromStrand(StreamingAsrResult{ segId, finalText, true, lang });
		}
		_lastInterimText.clear();
		_lastInterimLanguage.clear();
		finishCleanlyFromStrand();
	}
}

} // namespace OpenMeeting
