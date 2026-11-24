#pragma once

#include "src/agent/agent_speech_service.h"
#include "src/agent/agent_transport.h"
#include "src/agent/streaming_asr_session.h"
#include "src/core/meeting_coordinator.h"
#include "src/core/session_shutdown_service.h"
#include "src/rtc/audio_source.h"

#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtCore/QVector>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace OpenMeeting {

class AgentCoordinator;

enum class MeetingTranslatorState {
	Stopped = 0,
	Starting = 1,
	Running = 2,
	Stopping = 3,
	Error = 4,
};

struct MeetingTranslatorConfig {
	bool translateEnabled = false;
	QString targetLanguage = QStringLiteral("zh");
	QString translationProfileId;
	QString translationModelOverride;
	QString translationSystemPrompt;
	bool includeLocalMic = true;
	int maxConcurrentStreamingAsr = 3;
	int streamingIdleReleaseMs = 12000;
};

// Local meeting speech transcription & optional LLM translation pipeline.
// Captures read-only PCM copies from local microphone AudioSource and remote
// microphone tracks, routes either through per-speaker VAD + HTTP STT or a
// bounded lazy WebSocket streaming ASR pool, and invokes optional LLM translation
// without interfering with active media calls.
class MeetingTranslatorService final : public QObject {
	Q_OBJECT
	friend class MeetingTranslatorInspectionAccess;
public:
	explicit MeetingTranslatorService(
		AgentCoordinator *agentCoordinator,
		MeetingCoordinator *meetingCoordinator,
		std::unique_ptr<IAgentSpeechTransport> speechTransport = nullptr,
		std::unique_ptr<IAgentTransport> chatTransport = nullptr,
		QObject *parent = nullptr);
	~MeetingTranslatorService() override;

	void setLocalAudioSource(std::shared_ptr<livekit::AudioSource> source);
	void setStreamingAsrFactory(StreamingAsrFactory factory);

	bool start(const MeetingTranslatorConfig &config);
	void stop();
	void updateConfig(const MeetingTranslatorConfig &config);

	[[nodiscard]] MeetingTranslatorState state() const { return _state; }
	[[nodiscard]] bool isRunning() const { return _state == MeetingTranslatorState::Running; }
	[[nodiscard]] bool isBusy() const;
	[[nodiscard]] const MeetingTranslatorConfig &config() const { return _config; }
	[[nodiscard]] QString lastError() const { return _lastError; }
	[[nodiscard]] int activeStreamingSessionCount() const { return static_cast<int>(_speakerPool.size()); }
	[[nodiscard]] static QString defaultTranslationPromptTemplate();
	[[nodiscard]] static QString buildTranslationSystemPrompt(
		const QString &customTemplate,
		const QString &targetLanguage);

signals:
	void stateChanged(OpenMeeting::MeetingTranslatorState state);
	void activityChanged(bool busy);
	void transcriptProduced(const OpenMeeting::MeetingTranscriptItem &item);
	void errorOccurred(const QString &safeError);

private:
	struct PendingSttJob {
		quint64 generation = 0;
		QString speakerIdentity;
		QString speakerName;
		QString trackSid;
		bool isLocalSpeaker = false;
		QByteArray wavBytes;
	};

	struct PendingTranslateJob {
		quint64 generation = 0;
		MeetingTranscriptItem interimItem;
		QString targetLanguage;
	};

	struct SpeakerVadSlot {
		DictationVadSegmenter vad;
	};

	struct SpeakerAudioStreamSlot {
		std::deque<int16_t> preRoll;
		size_t consecutiveSpeechSamples = 0;
		std::atomic<qint64> lastSpeechAudioMs{0};
		bool wakePending = false;
		std::shared_ptr<IStreamingAsrSession> activeSession;
	};

	struct ActiveSpeakerStream {
		std::string key;
		QString speakerIdentity;
		QString speakerName;
		QString trackSid;
		bool isLocalSpeaker = false;
		std::shared_ptr<IStreamingAsrSession> session;
		qint64 lastSpeechActivityMs = 0;
		qint64 lastInterimEmitMs = 0;
		QHash<QString, QString> segmentIdMap;
	};

	struct DrainingSpeakerStream {
		QString speakerIdentity;
		QString speakerName;
		QString trackSid;
		bool isLocalSpeaker = false;
		std::shared_ptr<IStreamingAsrSession> session;
		QHash<QString, QString> segmentIdMap;
	};

	void setState(MeetingTranslatorState next);
	void updateActivityState();
	void bindLocalMicSubscription();
	void unbindAudioSources();
	void handleAudioFrame(
		quint64 generation,
		const QString &speakerIdentity,
		const QString &speakerName,
		const QString &trackSid,
		bool isLocalSpeaker,
		const livekit::AudioFrame &frame);
	void handleStreamingAudioFrame(
		quint64 generation,
		const std::string &key,
		const QString &speakerIdentity,
		const QString &speakerName,
		const QString &trackSid,
		bool isLocalSpeaker,
		const std::vector<int16_t> &pcm16k);
	void wakeSpeakerStream(
		quint64 generation,
		const std::string &key,
		const QString &speakerIdentity,
		const QString &speakerName,
		const QString &trackSid,
		bool isLocalSpeaker);
	void retireSpeakerStream(const std::string &key);
	void evictOldestSpeakerStreamsIfNeeded(int maxAllowed, const std::string &excludeKey = {});
	void reapIdleStreamingSessions();
	void handleStreamingResult(
		quint64 generation,
		const std::string &speakerKey,
		const std::shared_ptr<IStreamingAsrSession> &sourceSession,
		const StreamingAsrResult &res);
	void handleStreamingTerminal(
		quint64 generation,
		const std::string &speakerKey,
		quint64 drainId,
		const std::shared_ptr<IStreamingAsrSession> &sourceSession,
		const QString &safeError);
	void stopAllStreamingSessions();
	void enqueueSttJob(PendingSttJob job);
	void pumpSttQueue();
	void enqueueTranslateJob(PendingTranslateJob job);
	void pumpTranslateQueue();
	void cancelInFlightRequests();
	void noteAuthOrFatalFailure(const QString &safeError);
	void emitTranscript(const MeetingTranscriptItem &item);
	void handleAgentConfigChanged();
	void syncVadConfigFromCoordinator(bool resetSpeakerVads);
	[[nodiscard]] QString buildSpeechConfigSignature() const;

	static std::vector<int16_t> resampleTo16kMono(
		const int16_t *samples,
		int sampleRate,
		int channels,
		int framesPerChannel);
	static QString languageDisplayName(const QString &langCode);

	QPointer<AgentCoordinator> _agentCoordinator;
	QMetaObject::Connection _agentProfileConn;
	QString _activeSpeechConfigSignature;
	QPointer<MeetingCoordinator> _meetingCoordinator;
	std::unique_ptr<IAgentSpeechTransport> _speechTransport;
	std::unique_ptr<IAgentTransport> _chatTransport;
	StreamingAsrFactory _streamingAsrFactory;

	std::shared_ptr<livekit::AudioSource> _localAudioSource;
	std::shared_ptr<livekit::AudioSource::Subscription> _localMicSubscription;
	std::shared_ptr<QtCallbackGate<MeetingTranslatorService>> _callbackGate;

	MeetingTranslatorState _state = MeetingTranslatorState::Stopped;
	MeetingTranslatorConfig _config;
	QString _lastError;
	std::atomic<quint64> _generation{0};
	std::atomic<float> _vadThresholdDb{-42.0f};
	std::atomic<int> _vadSilenceCommitMs{600};
	quint64 _segmentSeq = 0;
	quint64 _drainSeq = 0;
	int _consecutiveFailures = 0;
	bool _lastBusyEmitted = false;
	bool _streamingMode = false;

	std::mutex _vadMutex;
	std::unordered_map<std::string, std::unique_ptr<SpeakerVadSlot>> _speakerVads;

	std::mutex _streamAudioMutex;
	std::unordered_map<std::string, std::shared_ptr<SpeakerAudioStreamSlot>> _speakerAudioSlots;
	std::unordered_map<std::string, ActiveSpeakerStream> _speakerPool;
	QHash<quint64, DrainingSpeakerStream> _drainingSessions;
	QTimer _idleReapTimer;

	std::deque<PendingSttJob> _sttQueue;
	std::deque<PendingTranslateJob> _translateQueue;
	QVector<QPointer<AgentSpeechRequestHandle>> _inFlightStt;
	QVector<QPointer<AgentRequestHandle>> _inFlightTranslate;
};

} // namespace OpenMeeting

Q_DECLARE_METATYPE(OpenMeeting::MeetingTranslatorState)
