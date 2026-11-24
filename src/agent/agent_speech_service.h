#pragma once

#include "src/agent/agent_types.h"
#include "src/agent/streaming_asr_session.h"
#include "src/core/session_shutdown_service.h"

#include <QtCore/QByteArray>
#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QVector>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace OpenMeeting {

class AgentCoordinator;
class MeetingCoordinator;
struct MeetingTranscriptItem;

enum class SpeechToTextStatus {
	Started = 0,
	Recognizing = 1,
	Recognized = 2,
	Stopped = 3,
	Error = 4,
};

struct SpeechToTextEvent {
	quint64 sessionId = 0;
	SpeechToTextStatus status = SpeechToTextStatus::Stopped;
	QString segmentId;
	QString text;
	QString errorMessage;
	bool fromMeetingTranscript = false;
};

struct DictationTextEditOp {
	bool hasEdit = false;
	int replaceStart = 0;
	int replaceLength = 0;
	QString replacement;
	int nextCursorPos = 0;
};

// Tracks an anchor span inside an editor buffer for VS Code style
// interim replacement and finalized speech commits without overwriting
// unrelated surrounding or manually typed text.
class DictationAnchorTracker final {
public:
	void reset();
	void beginSession(int cursorPos);
	void onManualUserEdit(int newCursorPos);

	[[nodiscard]] DictationTextEditOp applySpeechEvent(
		SpeechToTextStatus status,
		const QString &segmentId,
		const QString &rawText,
		const QString &currentBuffer);

	[[nodiscard]] int anchorPos() const { return _anchorPos; }
	[[nodiscard]] int interimLength() const { return _interimLength; }

private:
	[[nodiscard]] static bool shouldInsertLeadingSpace(QChar prevChar, QChar nextChar);

	int _anchorPos = -1;
	int _interimLength = 0;
	bool _insertedLeadingSpace = false;
	QString _activeSegmentId;
};

class DictationVadSegmenter final {
public:
	struct Config {
		int sampleRate = 16000;
		int channels = 1;
		float thresholdDb = -42.0f;
		int preRollMs = 200;
		int silenceCommitMs = 600;
		int minSpeechMs = 220;
		int maxUtteranceMs = 15000;
	};

	struct FrameOutcome {
		float normalizedLevel = 0.0f;
		bool isSpeaking = false;
		bool speechJustStarted = false;
		std::optional<QByteArray> completedWav;
	};

	DictationVadSegmenter();
	explicit DictationVadSegmenter(const Config &config);

	void reset();
	[[nodiscard]] FrameOutcome pushPcm16(const int16_t *samples, size_t sampleCount);
	[[nodiscard]] std::optional<QByteArray> flushPendingWav();

	[[nodiscard]] static QByteArray encodePcm16ToWav(
		const int16_t *samples,
		size_t sampleCount,
		int sampleRate = 16000,
		int channels = 1);
	[[nodiscard]] static float computeNormalizedLevel(
		const int16_t *samples,
		size_t sampleCount);

private:
	[[nodiscard]] QByteArray buildAndClearUtteranceWav();

	Config _config;
	size_t _preRollCapacitySamples = 3200;
	size_t _minSpeechSamples = 3520;
	size_t _maxUtteranceSamples = 240000;
	size_t _silenceCommitSamples = 9600;
	std::vector<int16_t> _preRoll;
	std::vector<int16_t> _utterance;
	size_t _speechSamples = 0;
	size_t _trailingSilenceSamples = 0;
	bool _inSpeech = false;
};

struct SpeechTranscriptionRequest {
	AgentProfile profile;
	QString apiKey;
	QString customSttBaseUrl;
	AgentSpeechProtocolMode protocolMode = AgentSpeechProtocolMode::Auto;
	QString sttModel;
	AgentModelCapabilities sttModelCapabilities;
	QString language;
	QString prompt;
	bool streamResponse = false;
	QByteArray wavAudioBytes;
};

class AgentSpeechRequestHandle : public QObject {
	Q_OBJECT
public:
	using QObject::QObject;
	~AgentSpeechRequestHandle() override = default;

	virtual void cancel() = 0;

signals:
	void deltaReceived(const QString &accumulatedText);
	void finished(bool success, const QString &text, const QString &safeError);
};

class IAgentSpeechTransport {
public:
	virtual ~IAgentSpeechTransport() = default;

	virtual AgentSpeechRequestHandle *transcribeWav(
		const SpeechTranscriptionRequest &request,
		QObject *parent = nullptr) = 0;
};

class OpenAiCompatibleSpeechTransport final : public IAgentSpeechTransport {
public:
	struct MultipartTranscriptionFields {
		QString model;
		QString modelFieldName = QStringLiteral("model");
		QString responseFormat;
		QString language;
		QString languageFieldName = QStringLiteral("language");
		QString prompt;
		QStringList keyterms;
		bool format = false;
		bool stream = false;
		bool filePartLast = false;
	};

	OpenAiCompatibleSpeechTransport();
	~OpenAiCompatibleSpeechTransport() override;

	AgentSpeechRequestHandle *transcribeWav(
		const SpeechTranscriptionRequest &request,
		QObject *parent = nullptr) override;

	[[nodiscard]] static MultipartTranscriptionFields buildMultipartTranscriptionFields(
		const QString &modelId,
		const QString &endpointHost,
		const QString &language,
		const QString &prompt,
		bool enableStream,
		const QString &endpointPath = QString());

	[[nodiscard]] static QUrl resolveTranscriptionEndpointUrl(
		const QUrl &baseUrl,
		bool isCustomSttUrl);
	[[nodiscard]] static QUrl resolveChatCompletionsEndpointUrl(
		const QUrl &baseUrl);
	[[nodiscard]] static QUrl resolveDashScopeMultimodalGenerationEndpointUrl(
		const QUrl &baseUrl);
	[[nodiscard]] static bool usesChatCompletionsProtocol(
		const QString &modelId,
		const QUrl &endpointBaseUrl,
		const AgentModelCapabilities &caps = {});
	[[nodiscard]] static bool supportsMultipartTranscriptionSse(
		const QString &modelId,
		const QString &endpointHost = QString());
	[[nodiscard]] static bool supportsChatInputAudioSse(
		const QString &modelId,
		const QString &endpointHost = QString());
	[[nodiscard]] static QString parseTranscriptionResponseJson(
		const QByteArray &payload,
		const QString &contentType,
		QString *errorOut = nullptr);

private:
	struct Impl;
	std::unique_ptr<Impl> _impl;
};

class IDictationAudioSource {
public:
	using FrameCallback = std::function<void(const int16_t *samples, size_t count)>;
	using StateCallback = std::function<void(bool available, const QString &safeError)>;

	virtual ~IDictationAudioSource() = default;
	virtual bool start(
		const QString &deviceId,
		FrameCallback onFrame,
		StateCallback onStateChanged) = 0;
	virtual void stop() = 0;
	[[nodiscard]] virtual bool isPlatformCaptureSupported() const = 0;
};

class AgentSpeechService final : public QObject {
	Q_OBJECT
public:
	using AudioSourceFactory = std::function<std::unique_ptr<IDictationAudioSource>()>;

	explicit AgentSpeechService(
		AgentCoordinator *agentCoordinator,
		MeetingCoordinator *meetingCoordinator = nullptr,
		std::unique_ptr<IAgentSpeechTransport> transport = nullptr,
		AudioSourceFactory audioSourceFactory = {},
		QObject *parent = nullptr);
	~AgentSpeechService() override;

	void setAgentCoordinator(AgentCoordinator *coordinator);
	void setMeetingCoordinator(MeetingCoordinator *meeting);
	void setStreamingAsrFactory(StreamingAsrFactory factory);

	bool startDictation(const QString &deviceId = QString());
	void stopDictation();
	bool restartWithDevice(const QString &deviceId);

	[[nodiscard]] bool isDictationActive() const { return _active; }
	[[nodiscard]] bool isUsingRoomPassThrough() const { return _roomPassThroughActive; }
	[[nodiscard]] quint64 currentSessionId() const { return _sessionId; }

signals:
	void dictationActiveChanged(bool active);
	void audioLevelChanged(float normalizedLevel);
	void roomPassThroughChanged(bool usingRoomTranscript);
	void speechEventReceived(const OpenMeeting::SpeechToTextEvent &event);

private:
	struct UtteranceSlot {
		quint64 sequence = 0;
		bool completed = false;
		bool success = false;
		QString text;
		QString errorMessage;
	};

	void bindAgentCoordinator();
	void handleAgentConfigChanged();
	[[nodiscard]] QString buildSpeechConfigSignature() const;
	void bindMeetingCoordinator();
	void submitUtteranceWav(quint64 sessionId, const QString &segmentId, const QByteArray &wavBytes);
	void flushOrderedUtterances();
	void handleRoomTranscript(const OpenMeeting::MeetingTranscriptItem &item);
	void updatePassThroughMode();
	void cleanupActiveSession(bool emitStopped);
	void finalizeDrainingStream(quint64 expectedSessionId, bool abortSession);

	QPointer<AgentCoordinator> _agentCoordinator;
	QMetaObject::Connection _agentProfileConn;
	QString _activeSpeechConfigSignature;
	int _activeVadThresholdDb = -42;
	int _activeVadSilenceCommitMs = 600;
	QPointer<MeetingCoordinator> _meetingCoordinator;
	QVector<QMetaObject::Connection> _meetingConnections;

	std::unique_ptr<IAgentSpeechTransport> _transport;
	StreamingAsrFactory _streamingAsrFactory;
	AudioSourceFactory _audioSourceFactory;
	std::unique_ptr<IDictationAudioSource> _audioSource;
	std::shared_ptr<QtCallbackGate<AgentSpeechService>> _callbackGate;

	DictationVadSegmenter _vadSegmenter;
	std::shared_ptr<IStreamingAsrSession> _activeStreamSession;
	std::shared_ptr<IStreamingAsrSession> _drainingStreamSession;
	quint64 _drainingStreamSessionId = 0;
	QTimer _streamDrainTimer;
	QString _currentDeviceId;
	bool _active = false;
	std::atomic<bool> _roomPassThroughActive{false};
	bool _roomSawLocalTranscriptThisSession = false;
	quint64 _sessionId = 0;
	quint64 _nextUtteranceSeq = 1;
	quint64 _nextCommitSeq = 1;
	qint64 _lastLevelEmitMs = 0;
	QString _activeCaptureSegmentId;
	QHash<quint64, UtteranceSlot> _pendingUtterances;
	QVector<QPointer<AgentSpeechRequestHandle>> _inFlightHandles;
};

} // namespace OpenMeeting

Q_DECLARE_METATYPE(OpenMeeting::SpeechToTextStatus)
Q_DECLARE_METATYPE(OpenMeeting::SpeechToTextEvent)
