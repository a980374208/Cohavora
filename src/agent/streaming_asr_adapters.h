#pragma once

#include "src/agent/streaming_asr_session.h"

#include <QtCore/QByteArray>
#include <QtCore/QHash>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace OpenMeeting {

struct VolcengineSaucServerPacket {
	bool valid = false;
	bool isError = false;
	bool isFinalPacket = false;
	uint8_t messageType = 0;
	uint8_t flags = 0;
	uint8_t serialization = 0;
	uint8_t compression = 0;
	uint32_t errorCode = 0;
	int32_t sequence = 0;
	QByteArray payload;
};

[[nodiscard]] std::vector<uint8_t> gzipCompressBytes(const uint8_t *data, size_t size, bool *ok = nullptr);
[[nodiscard]] std::vector<uint8_t> gzipDecompressBytes(const uint8_t *data, size_t size, bool *ok = nullptr);

[[nodiscard]] std::vector<uint8_t> buildVolcengineFullClientPacket(
	const QByteArray &jsonPayload,
	int32_t sequence = 1,
	bool useGzip = true);

[[nodiscard]] std::vector<uint8_t> buildVolcengineAudioPacket(
	const uint8_t *pcmBytes,
	size_t size,
	int32_t sequence,
	bool isLast,
	bool useGzip = true);

[[nodiscard]] VolcengineSaucServerPacket parseVolcengineServerPacket(const std::vector<uint8_t> &packet);

[[nodiscard]] QUrl resolveStreamingSpeechEndpointUrl(
	AgentSpeechProtocolMode protocol,
	const QString &configuredUrl,
	const QString &model,
	const QString &language,
	QString *outError = nullptr,
	const QString &prompt = QString());

class BaseWsStreamingAsrSession : public std::enable_shared_from_this<BaseWsStreamingAsrSession> {
public:
	virtual ~BaseWsStreamingAsrSession();

	[[nodiscard]] virtual int requiredSampleRate() const = 0;
	void start(StreamingAsrCallbacks callbacks);
	void pushPcm16(const int16_t *samples, size_t sampleCount, int inputSampleRate);
	void finish();
	void abort();
	[[nodiscard]] StreamingAsrState state() const;

protected:
	struct ResolvedConnection {
		bool valid = false;
		bool allowPlaintextLoopback = false;
		QString url;
		QString bearerToken;
		std::vector<std::pair<std::string, std::string>> extraHeaders;
		QString errorMessage;
	};

	struct Impl;

	explicit BaseWsStreamingAsrSession(const StreamingAsrConfig &config);

	[[nodiscard]] const StreamingAsrConfig &config() const { return _config; }
	[[nodiscard]] std::shared_ptr<Impl> impl() const { return _impl; }

	[[nodiscard]] virtual ResolvedConnection buildConnection() const = 0;
	virtual void onConnectedOnStrand() = 0;
	virtual void onTextMessageOnStrand(const std::string &payload) = 0;
	virtual void onBinaryMessageOnStrand(const std::vector<uint8_t> &payload) = 0;
	virtual void sendAudioChunkOnStrand(const std::vector<uint8_t> &pcmBytes) = 0;
	virtual void sendFinishOnStrand() = 0;
	virtual void onBeforeFinishOnStrand() {}
	virtual int finishTimeoutMs() const { return 2500; }

	// Helpers callable only while running inside _impl->strand_ callbacks.
	void markReadyOnStrand();
	void sendTextOnStrand(std::string text);
	void sendBinaryOnStrand(std::vector<uint8_t> data);
	void emitResultFromStrand(const StreamingAsrResult &result);
	void finishCleanlyFromStrand();
	void failFromStrand(const QString &safeError);
	[[nodiscard]] bool isFinishingOnStrand() const;
	[[nodiscard]] std::chrono::steady_clock::time_point lastAudioSentOnStrand() const;
	[[nodiscard]] asio::any_io_executor strandExecutor() const;

	StreamingAsrConfig _config;
	std::shared_ptr<Impl> _impl;
};

class OpenAiRealtimeAsrSession final : public BaseWsStreamingAsrSession {
public:
	explicit OpenAiRealtimeAsrSession(const StreamingAsrConfig &config);

	[[nodiscard]] int requiredSampleRate() const override;

protected:
	[[nodiscard]] ResolvedConnection buildConnection() const override;
	void onConnectedOnStrand() override;
	void onTextMessageOnStrand(const std::string &payload) override;
	void onBinaryMessageOnStrand(const std::vector<uint8_t> &) override {}
	void sendAudioChunkOnStrand(const std::vector<uint8_t> &pcmBytes) override;
	void sendFinishOnStrand() override;

private:
	void sendSessionUpdateOnStrand();
	void sendCommitOnStrand();

	bool _isDashScopeQwen = false;
	bool _effectiveServerVad = true;
	bool _sessionUpdateSent = false;
	bool _hasUncommittedAudio = false;
	bool _speechDetectedSinceCommit = false;
	int _uncommittedAudioMs = 0;
	int _speechAudioMs = 0;
	int _trailingSilenceMs = 0;
	int _pendingCommits = 0;
	QHash<QString, QString> _partialByItemId;
};

class DashScopeInferenceAsrSession final : public BaseWsStreamingAsrSession {
public:
	explicit DashScopeInferenceAsrSession(const StreamingAsrConfig &config);

	[[nodiscard]] int requiredSampleRate() const override;

protected:
	[[nodiscard]] ResolvedConnection buildConnection() const override;
	void onConnectedOnStrand() override;
	void onTextMessageOnStrand(const std::string &payload) override;
	void onBinaryMessageOnStrand(const std::vector<uint8_t> &) override {}
	void sendAudioChunkOnStrand(const std::vector<uint8_t> &pcmBytes) override;
	void sendFinishOnStrand() override;

private:
	std::string _taskId;
	quint64 _segmentSeq = 1;
};

class DeepgramAsrSession final : public BaseWsStreamingAsrSession {
public:
	explicit DeepgramAsrSession(const StreamingAsrConfig &config);
	~DeepgramAsrSession() override;

	[[nodiscard]] int requiredSampleRate() const override { return 16000; }

protected:
	[[nodiscard]] ResolvedConnection buildConnection() const override;
	void onConnectedOnStrand() override;
	void onTextMessageOnStrand(const std::string &payload) override;
	void onBinaryMessageOnStrand(const std::vector<uint8_t> &) override {}
	void sendAudioChunkOnStrand(const std::vector<uint8_t> &pcmBytes) override;
	void sendFinishOnStrand() override;
	void onBeforeFinishOnStrand() override;

private:
	void armKeepAliveTimerOnStrand();
	void cancelKeepAliveTimerOnStrand();
	void flushFinalizedUtteranceOnStrand();

	std::shared_ptr<asio::steady_timer> _keepAliveTimer;
	bool _waitForSpeechFinal = true;
	QStringList _finalizedParts;
	QString _utteranceLanguage;
	quint64 _segmentSeq = 1;
};

class AssemblyAiV3AsrSession final : public BaseWsStreamingAsrSession {
public:
	explicit AssemblyAiV3AsrSession(const StreamingAsrConfig &config);

	[[nodiscard]] int requiredSampleRate() const override { return 16000; }

protected:
	[[nodiscard]] ResolvedConnection buildConnection() const override;
	void onConnectedOnStrand() override;
	void onTextMessageOnStrand(const std::string &payload) override;
	void onBinaryMessageOnStrand(const std::vector<uint8_t> &) override {}
	void sendAudioChunkOnStrand(const std::vector<uint8_t> &pcmBytes) override;
	void sendFinishOnStrand() override;

private:
	void flushPendingFinalTurnOnStrand();

	bool _expectFormattedTurns = true;
	qint64 _lastFinalTurnOrder = -1;
	qint64 _pendingFinalTurnOrder = -1;
	QString _pendingFinalText;
	QString _pendingFinalLanguage;
};

class VolcengineSaucAsrSession final : public BaseWsStreamingAsrSession {
public:
	explicit VolcengineSaucAsrSession(const StreamingAsrConfig &config);

	[[nodiscard]] int requiredSampleRate() const override { return 16000; }

protected:
	[[nodiscard]] ResolvedConnection buildConnection() const override;
	void onConnectedOnStrand() override;
	void onTextMessageOnStrand(const std::string &payload) override;
	void onBinaryMessageOnStrand(const std::vector<uint8_t> &payload) override;
	void sendAudioChunkOnStrand(const std::vector<uint8_t> &pcmBytes) override;
	void sendFinishOnStrand() override;

private:
	int32_t _nextSequence = 2;
	int _committedUtterances = 0;
};

class ElevenLabsScribeAsrSession final : public BaseWsStreamingAsrSession {
public:
	explicit ElevenLabsScribeAsrSession(const StreamingAsrConfig &config);

	[[nodiscard]] int requiredSampleRate() const override { return 16000; }

protected:
	[[nodiscard]] ResolvedConnection buildConnection() const override;
	void onConnectedOnStrand() override;
	void onTextMessageOnStrand(const std::string &payload) override;
	void onBinaryMessageOnStrand(const std::vector<uint8_t> &) override {}
	void sendAudioChunkOnStrand(const std::vector<uint8_t> &pcmBytes) override;
	void sendFinishOnStrand() override;
	void onBeforeFinishOnStrand() override;
	int finishTimeoutMs() const override { return 1500; }

private:
	void flushPendingCommitOnStrand();

	quint64 _segmentSeq = 1;
	bool _hasAudioSent = false;
	bool _expectTimestamps = false;
	bool _hasPendingCommit = false;
	QString _pendingCommitText;
	QString _pendingCommitLanguage;
	QString _lastPartialLanguage;
};

class XaiSttAsrSession final : public BaseWsStreamingAsrSession {
public:
	explicit XaiSttAsrSession(const StreamingAsrConfig &config);

	[[nodiscard]] int requiredSampleRate() const override { return 16000; }

protected:
	[[nodiscard]] ResolvedConnection buildConnection() const override;
	void onConnectedOnStrand() override;
	void onTextMessageOnStrand(const std::string &payload) override;
	void onBinaryMessageOnStrand(const std::vector<uint8_t> &) override {}
	void sendAudioChunkOnStrand(const std::vector<uint8_t> &pcmBytes) override;
	void sendFinishOnStrand() override;
	void onBeforeFinishOnStrand() override;

private:
	void flushFinalizedUtteranceOnStrand();

	QStringList _finalizedParts;
	QString _utteranceLanguage;
	QString _lastInterimText;
	QString _lastInterimLanguage;
	quint64 _segmentSeq = 1;
	int _emittedFinalCount = 0;
	bool _readyMarked = false;
};

class StreamingAsrSessionHandle final : public IStreamingAsrSession {
public:
	explicit StreamingAsrSessionHandle(std::shared_ptr<BaseWsStreamingAsrSession> inner)
		: _inner(std::move(inner)) {}
	~StreamingAsrSessionHandle() override {
		if (_inner) {
			_inner->abort();
		}
	}

	[[nodiscard]] int requiredSampleRate() const override {
		return _inner ? _inner->requiredSampleRate() : 16000;
	}
	void start(StreamingAsrCallbacks callbacks) override {
		if (_inner) _inner->start(std::move(callbacks));
	}
	void pushPcm16(const int16_t *samples, size_t sampleCount, int inputSampleRate) override {
		if (_inner) _inner->pushPcm16(samples, sampleCount, inputSampleRate);
	}
	void finish() override {
		if (_inner) _inner->finish();
	}
	void abort() override {
		if (_inner) _inner->abort();
	}
	[[nodiscard]] StreamingAsrState state() const override {
		return _inner ? _inner->state() : StreamingAsrState::Closed;
	}

private:
	std::shared_ptr<BaseWsStreamingAsrSession> _inner;
};

} // namespace OpenMeeting
