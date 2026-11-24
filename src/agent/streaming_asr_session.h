#pragma once

#include "src/agent/agent_types.h"

#include <QtCore/QMap>
#include <QtCore/QString>

#include <asio.hpp>
#include <asio/ssl.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace OpenMeeting {

enum class StreamingAsrState {
	Idle = 0,
	Connecting = 1,
	Ready = 2,
	Finishing = 3,
	Closed = 4,
	Error = 5,
};

struct StreamingAsrConfig {
	AgentSpeechProtocolMode protocol = AgentSpeechProtocolMode::OpenAiRealtimeWs;
	QString endpointUrl;
	QString apiKey;
	QString appKey;
	QString resourceId;
	QString workspaceId;
	QString model;
	QString language;
	QString prompt;
	QMap<QString, QString> customHeaders;
	int connectTimeoutMs = 10000;
	int clientCommitSilenceMs = 600;
	int clientCommitMaxUtteranceMs = 20000;
	bool enableServerVad = true;
};

struct StreamingAsrResult {
	QString segmentId;
	QString text;
	bool isFinal = false;
	QString language;
};

struct StreamingAsrCallbacks {
	std::function<void(StreamingAsrState)> onStateChanged;
	std::function<void(const StreamingAsrResult &)> onResult;
	std::function<void(const QString &safeError)> onError;
	std::function<void()> onFinished;
};

class IStreamingAsrSession {
public:
	virtual ~IStreamingAsrSession() = default;

	[[nodiscard]] virtual int requiredSampleRate() const = 0;
	virtual void start(StreamingAsrCallbacks callbacks) = 0;
	virtual void pushPcm16(const int16_t *samples, size_t sampleCount, int inputSampleRate) = 0;
	virtual void finish() = 0;
	virtual void abort() = 0;
	[[nodiscard]] virtual StreamingAsrState state() const = 0;
};

using StreamingAsrFactory = std::function<std::unique_ptr<IStreamingAsrSession>(const StreamingAsrConfig &)>;

[[nodiscard]] std::vector<int16_t> resamplePcm16Mono(
	const int16_t *samples,
	size_t sampleCount,
	int inputSampleRate,
	int targetSampleRate);

class StreamingAsrIoRuntime final {
public:
	static StreamingAsrIoRuntime &instance();

	~StreamingAsrIoRuntime();
	StreamingAsrIoRuntime(const StreamingAsrIoRuntime &) = delete;
	StreamingAsrIoRuntime &operator=(const StreamingAsrIoRuntime &) = delete;

	void ensureStarted();
	asio::io_context &ioContext() { return _ioCtx; }
	asio::ssl::context &sslContext() { return _sslCtx; }

private:
	StreamingAsrIoRuntime();

	asio::io_context _ioCtx;
	asio::ssl::context _sslCtx;
	std::optional<asio::executor_work_guard<asio::io_context::executor_type>> _workGuard;
	std::once_flag _startOnce;
	std::thread _worker;
};

[[nodiscard]] std::unique_ptr<IStreamingAsrSession> createStreamingAsrSession(const StreamingAsrConfig &config);

} // namespace OpenMeeting
