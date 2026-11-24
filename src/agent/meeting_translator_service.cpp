#include "src/agent/meeting_translator_service.h"

#include "src/agent/agent_coordinator.h"
#include "src/agent/openai_compatible_transport.h"
#include "src/media/audio_vad.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QThread>

#include <algorithm>
#include <cmath>
#include <limits>

namespace OpenMeeting {
namespace {

constexpr int kTargetSampleRate = 16000;
constexpr int kMaxInFlightStt = 2;
constexpr int kMaxInFlightTranslate = 2;
constexpr size_t kMaxQueuedSttJobs = 6;
constexpr size_t kMaxQueuedTranslateJobs = 8;
constexpr int kMaxConsecutiveAuthFailures = 3;

constexpr size_t kStreamPreRollSamples = 4000;        // 250 ms @ 16 kHz
constexpr size_t kStreamWakePendingMaxSamples = 16000; // 1.0 s guard while wake hops thread
constexpr size_t kStreamWakeSpeechSamples = 960;       // 60 ms continuous speech
constexpr qint64 kStreamInterimThrottleMs = 250;       // 250 ms UI/broadcast throttle
constexpr int kStreamIdleCheckIntervalMs = 1000;
constexpr int kStreamDrainTimeoutMs = 1500;

DictationVadSegmenter::Config makeMeetingVadConfig(float thresholdDb, int silenceCommitMs) {
	DictationVadSegmenter::Config cfg;
	cfg.sampleRate = kTargetSampleRate;
	cfg.channels = 1;
	cfg.thresholdDb = std::clamp(thresholdDb, -60.0f, -15.0f);
	cfg.preRollMs = 200;
	cfg.silenceCommitMs = std::clamp(silenceCommitMs, 200, 3000);
	cfg.minSpeechMs = 240;
	cfg.maxUtteranceMs = 8000;
	return cfg;
}

bool isLikelyAuthError(const QString &err) {
	return err.contains(QStringLiteral("HTTP 401")) ||
		err.contains(QStringLiteral("HTTP 403")) ||
		err.contains(QStringLiteral("API key"), Qt::CaseInsensitive) ||
		err.contains(QStringLiteral("Unauthorized"), Qt::CaseInsensitive);
}

} // namespace

MeetingTranslatorService::MeetingTranslatorService(
		AgentCoordinator *agentCoordinator,
		MeetingCoordinator *meetingCoordinator,
		std::unique_ptr<IAgentSpeechTransport> speechTransport,
		std::unique_ptr<IAgentTransport> chatTransport,
		QObject *parent)
	: QObject(parent)
	, _agentCoordinator(agentCoordinator)
	, _meetingCoordinator(meetingCoordinator)
	, _speechTransport(speechTransport
		? std::move(speechTransport)
		: std::make_unique<OpenAiCompatibleSpeechTransport>())
	, _chatTransport(chatTransport
		? std::move(chatTransport)
		: std::make_unique<OpenAiCompatibleTransport>())
	, _callbackGate(QtCallbackGate<MeetingTranslatorService>::Create(this)) {
	_idleReapTimer.setInterval(kStreamIdleCheckIntervalMs);
	connect(&_idleReapTimer, &QTimer::timeout, this, &MeetingTranslatorService::reapIdleStreamingSessions);
	if (_agentCoordinator) {
		_agentProfileConn = connect(
			_agentCoordinator,
			&AgentCoordinator::profileChanged,
			this,
			&MeetingTranslatorService::handleAgentConfigChanged);
	}
}

MeetingTranslatorService::~MeetingTranslatorService() {
	if (_agentProfileConn) {
		disconnect(_agentProfileConn);
		_agentProfileConn = {};
	}
	_idleReapTimer.stop();
	stop();
	if (_callbackGate) {
		_callbackGate->Revoke();
	}
}

QString MeetingTranslatorService::buildSpeechConfigSignature() const {
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

void MeetingTranslatorService::syncVadConfigFromCoordinator(bool resetSpeakerVads) {
	const float prevDb = _vadThresholdDb.load(std::memory_order_acquire);
	const int prevSilence = _vadSilenceCommitMs.load(std::memory_order_acquire);
	const float nextDb = _agentCoordinator
		? static_cast<float>(std::clamp(_agentCoordinator->dictationVadThresholdDb(), -60, -15))
		: -42.0f;
	const int nextSilence = _agentCoordinator
		? std::clamp(_agentCoordinator->dictationVadSilenceCommitMs(), 200, 3000)
		: 600;
	_vadThresholdDb.store(nextDb, std::memory_order_release);
	_vadSilenceCommitMs.store(nextSilence, std::memory_order_release);
	if (resetSpeakerVads && (prevDb != nextDb || prevSilence != nextSilence)) {
		std::lock_guard<std::mutex> lock(_vadMutex);
		_speakerVads.clear();
	}
}

void MeetingTranslatorService::handleAgentConfigChanged() {
	if (_state != MeetingTranslatorState::Running || !_agentCoordinator) {
		return;
	}
	syncVadConfigFromCoordinator(true);
	const QString nextSig = buildSpeechConfigSignature();
	if (nextSig == _activeSpeechConfigSignature) {
		return;
	}
	const bool wantStreaming = isStreamingSpeechProtocol(_agentCoordinator->effectiveDictationSpeechProtocol());
	if (wantStreaming != _streamingMode) {
		start(_config);
	} else if (_streamingMode) {
		_activeSpeechConfigSignature = nextSig;
		stopAllStreamingSessions();
	} else {
		_activeSpeechConfigSignature = nextSig;
	}
}

void MeetingTranslatorService::setStreamingAsrFactory(StreamingAsrFactory factory) {
	_streamingAsrFactory = std::move(factory);
}

bool MeetingTranslatorService::isBusy() const {
	return !_inFlightStt.isEmpty() ||
		!_inFlightTranslate.isEmpty() ||
		!_speakerPool.empty() ||
		!_drainingSessions.isEmpty();
}

void MeetingTranslatorService::updateActivityState() {
	const bool busyNow = isBusy();
	if (_lastBusyEmitted != busyNow) {
		_lastBusyEmitted = busyNow;
		emit activityChanged(busyNow);
	}
}

void MeetingTranslatorService::setLocalAudioSource(std::shared_ptr<livekit::AudioSource> source) {
	if (_localAudioSource == source) {
		return;
	}
	if (_localMicSubscription) {
		_localMicSubscription->disconnect();
		_localMicSubscription.reset();
	}
	_localAudioSource = std::move(source);
	if (_state == MeetingTranslatorState::Running) {
		bindLocalMicSubscription();
	}
}

bool MeetingTranslatorService::start(const MeetingTranslatorConfig &config) {
	_config = config;
	_config.maxConcurrentStreamingAsr = std::clamp(_config.maxConcurrentStreamingAsr, 1, 8);
	_config.streamingIdleReleaseMs = std::clamp(_config.streamingIdleReleaseMs, 3000, 120000);
	_lastError.clear();
	_consecutiveFailures = 0;

	if (!_meetingCoordinator || _meetingCoordinator->state() != MeetingState::InMeeting) {
		_lastError = QCoreApplication::translate(
			"MeetingUI", "Join a meeting before starting live translation.");
		setState(MeetingTranslatorState::Error);
		emit errorOccurred(_lastError);
		return false;
	}
	if (!_agentCoordinator) {
		_lastError = QCoreApplication::translate(
			"MeetingUI", "Copilot coordinator is unavailable.");
		setState(MeetingTranslatorState::Error);
		emit errorOccurred(_lastError);
		return false;
	}

	const AgentSpeechProtocolMode effectiveProto = _agentCoordinator->effectiveDictationSpeechProtocol();
	const bool wantStreaming = isStreamingSpeechProtocol(effectiveProto);

	if (wantStreaming) {
		QString cfgErr;
		(void)_agentCoordinator->buildStreamingAsrConfig(&cfgErr);
		if (!cfgErr.isEmpty()) {
			_lastError = cfgErr;
			setState(MeetingTranslatorState::Error);
			emit errorOccurred(_lastError);
			return false;
		}
	} else {
		QString sttKeyErr;
		(void)_agentCoordinator->loadApiKeyForSpeech(&sttKeyErr);
		if (!sttKeyErr.isEmpty()) {
			_lastError = sttKeyErr;
			setState(MeetingTranslatorState::Error);
			emit errorOccurred(_lastError);
			return false;
		}
		const QString sttUrl = _agentCoordinator->dictationSttBaseUrl().trimmed().isEmpty()
			? _agentCoordinator->profile().baseUrl.trimmed()
			: _agentCoordinator->dictationSttBaseUrl().trimmed();
		if (sttUrl.isEmpty()) {
			_lastError = QCoreApplication::translate(
				"MeetingUI", "Configure a Speech-to-Text (STT) or Copilot endpoint in Settings before starting live translation.");
			setState(MeetingTranslatorState::Error);
			emit errorOccurred(_lastError);
			return false;
		}
	}

	if (_config.translateEnabled && !_config.targetLanguage.trimmed().isEmpty()) {
		if (!_agentCoordinator->isTranslationTargetConfigured(
				_config.translationProfileId, _config.translationModelOverride)) {
			_lastError = QCoreApplication::translate(
				"MeetingUI", "Configure Copilot Base URL and Model in Settings to enable translation.");
			setState(MeetingTranslatorState::Error);
			emit errorOccurred(_lastError);
			return false;
		}
	}

	syncVadConfigFromCoordinator(true);
	if (_state == MeetingTranslatorState::Running && _streamingMode == wantStreaming) {
		_activeSpeechConfigSignature = buildSpeechConfigSignature();
		if (_streamingMode) {
			evictOldestSpeakerStreamsIfNeeded(_config.maxConcurrentStreamingAsr);
		}
		bindLocalMicSubscription();
		return true;
	}
	if (_state == MeetingTranslatorState::Running) {
		stop();
	}

	_streamingMode = wantStreaming;
	setState(MeetingTranslatorState::Starting);
	const quint64 gen = _generation.fetch_add(1, std::memory_order_acq_rel) + 1;
	{
		std::lock_guard<std::mutex> lock(_vadMutex);
		_speakerVads.clear();
	}
	stopAllStreamingSessions();
	_sttQueue.clear();
	_translateQueue.clear();

	const auto gate = _callbackGate;
	_meetingCoordinator->setRemoteAudioFrameCallback(
		[this, gate, gen](
				const QString &speakerIdentity,
				const QString &speakerName,
				const QString &trackSid,
				const livekit::AudioFrame &frame) {
			if (!gate || !gate->active() || _generation.load(std::memory_order_acquire) != gen) {
				return;
			}
			handleAudioFrame(gen, speakerIdentity, speakerName, trackSid, false, frame);
		});

	bindLocalMicSubscription();
	if (_streamingMode) {
		_idleReapTimer.start();
	} else {
		_idleReapTimer.stop();
	}
	_activeSpeechConfigSignature = buildSpeechConfigSignature();
	setState(MeetingTranslatorState::Running);
	return true;
}

void MeetingTranslatorService::stop() {
	if (_state == MeetingTranslatorState::Stopped &&
		!_localMicSubscription &&
		_inFlightStt.isEmpty() &&
		_inFlightTranslate.isEmpty() &&
		_speakerPool.empty() &&
		_drainingSessions.isEmpty()) {
		return;
	}
	setState(MeetingTranslatorState::Stopping);
	_idleReapTimer.stop();
	_generation.fetch_add(1, std::memory_order_acq_rel);
	unbindAudioSources();
	cancelInFlightRequests();
	stopAllStreamingSessions();
	_sttQueue.clear();
	_translateQueue.clear();
	{
		std::lock_guard<std::mutex> lock(_vadMutex);
		_speakerVads.clear();
	}
	_streamingMode = false;
	_activeSpeechConfigSignature.clear();
	updateActivityState();
	setState(MeetingTranslatorState::Stopped);
}

void MeetingTranslatorService::updateConfig(const MeetingTranslatorConfig &config) {
	const bool micChanged = (_config.includeLocalMic != config.includeLocalMic);
	_config = config;
	_config.maxConcurrentStreamingAsr = std::clamp(_config.maxConcurrentStreamingAsr, 1, 8);
	_config.streamingIdleReleaseMs = std::clamp(_config.streamingIdleReleaseMs, 3000, 120000);
	if (_state == MeetingTranslatorState::Running) {
		if (micChanged) {
			bindLocalMicSubscription();
			if (!_config.includeLocalMic && _streamingMode && _meetingCoordinator) {
				const std::string localKey = std::string("local:") +
					SessionManager::instance().userId().toStdString();
				retireSpeakerStream(localKey);
			}
		}
		if (_streamingMode) {
			evictOldestSpeakerStreamsIfNeeded(_config.maxConcurrentStreamingAsr);
		}
	}
}

void MeetingTranslatorService::setState(MeetingTranslatorState next) {
	if (_state == next) {
		return;
	}
	_state = next;
	emit stateChanged(_state);
}

void MeetingTranslatorService::bindLocalMicSubscription() {
	if (_localMicSubscription) {
		_localMicSubscription->disconnect();
		_localMicSubscription.reset();
	}
	if (!_config.includeLocalMic || !_localAudioSource || !_meetingCoordinator) {
		return;
	}
	const quint64 gen = _generation.load(std::memory_order_acquire);
	const auto gate = _callbackGate;
	const QPointer<MeetingCoordinator> coordinator = _meetingCoordinator;
	const QString localId = SessionManager::instance().userId();
	const QString localName = SessionManager::instance().nickname().trimmed().isEmpty()
		? localId
		: SessionManager::instance().nickname().trimmed();

	_localMicSubscription = _localAudioSource->subscribe(
		[this, gate, gen, coordinator, localId, localName](const livekit::AudioFrame &frame) {
			if (!gate || !gate->active() || _generation.load(std::memory_order_acquire) != gen) {
				return;
			}
			if (!coordinator || coordinator->isLocalAudioMuted()) {
				return;
			}
			handleAudioFrame(gen, localId, localName, QString(), true, frame);
		});
}

void MeetingTranslatorService::unbindAudioSources() {
	if (_localMicSubscription) {
		_localMicSubscription->disconnect();
		_localMicSubscription.reset();
	}
	if (_meetingCoordinator) {
		_meetingCoordinator->setRemoteAudioFrameCallback(nullptr);
	}
}

std::vector<int16_t> MeetingTranslatorService::resampleTo16kMono(
		const int16_t *samples,
		int sampleRate,
		int channels,
		int framesPerChannel) {
	if (!samples || sampleRate <= 0 || channels <= 0 || framesPerChannel <= 0) {
		return {};
	}
	std::vector<int16_t> mono(static_cast<size_t>(framesPerChannel));
	if (channels == 1) {
		std::copy(samples, samples + framesPerChannel, mono.begin());
	} else {
		for (int i = 0; i < framesPerChannel; ++i) {
			int32_t sum = 0;
			for (int ch = 0; ch < channels; ++ch) {
				sum += samples[static_cast<size_t>(i) * channels + ch];
			}
			mono[static_cast<size_t>(i)] = static_cast<int16_t>(sum / channels);
		}
	}
	if (sampleRate == kTargetSampleRate) {
		return mono;
	}
	const double ratio = static_cast<double>(sampleRate) / static_cast<double>(kTargetSampleRate);
	const size_t outFrames = std::max<size_t>(
		1, static_cast<size_t>(std::floor(static_cast<double>(framesPerChannel) / ratio)));
	std::vector<int16_t> out(outFrames);
	for (size_t i = 0; i < outFrames; ++i) {
		const double srcPos = static_cast<double>(i) * ratio;
		const size_t idx0 = std::min<size_t>(static_cast<size_t>(srcPos), mono.size() - 1);
		const size_t idx1 = std::min<size_t>(idx0 + 1, mono.size() - 1);
		const double frac = srcPos - static_cast<double>(idx0);
		const double val = static_cast<double>(mono[idx0]) * (1.0 - frac) +
			static_cast<double>(mono[idx1]) * frac;
		out[i] = static_cast<int16_t>(std::clamp(static_cast<int>(std::lround(val)), -32768, 32767));
	}
	return out;
}

void MeetingTranslatorService::handleAudioFrame(
		quint64 generation,
		const QString &speakerIdentity,
		const QString &speakerName,
		const QString &trackSid,
		bool isLocalSpeaker,
		const livekit::AudioFrame &frame) {
	if (frame.data().empty() || frame.sampleRate() <= 0 || frame.numChannels() <= 0 ||
		frame.samplesPerChannel() <= 0) {
		return;
	}
	const auto pcm16k = resampleTo16kMono(
		frame.data().data(),
		frame.sampleRate(),
		frame.numChannels(),
		frame.samplesPerChannel());
	if (pcm16k.empty()) {
		return;
	}

	const std::string key = (isLocalSpeaker ? std::string("local:") : std::string("remote:")) +
		speakerIdentity.toStdString();

	if (_streamingMode) {
		handleStreamingAudioFrame(
			generation, key, speakerIdentity, speakerName, trackSid, isLocalSpeaker, pcm16k);
		return;
	}

	std::optional<QByteArray> completedWav;
	{
		const float vadDb = _vadThresholdDb.load(std::memory_order_acquire);
		const int vadSilenceMs = _vadSilenceCommitMs.load(std::memory_order_acquire);
		std::lock_guard<std::mutex> lock(_vadMutex);
		if (_generation.load(std::memory_order_acquire) != generation) {
			return;
		}
		auto &slot = _speakerVads[key];
		if (!slot) {
			slot = std::make_unique<SpeakerVadSlot>(
				SpeakerVadSlot{ DictationVadSegmenter(makeMeetingVadConfig(vadDb, vadSilenceMs)) });
		}
		auto outcome = slot->vad.pushPcm16(pcm16k.data(), pcm16k.size());
		if (outcome.completedWav.has_value() && !outcome.completedWav->isEmpty()) {
			completedWav = std::move(outcome.completedWav);
		}
	}

	if (!completedWav.has_value()) {
		return;
	}

	PendingSttJob job;
	job.generation = generation;
	job.speakerIdentity = speakerIdentity;
	job.speakerName = speakerName;
	job.trackSid = trackSid;
	job.isLocalSpeaker = isLocalSpeaker;
	job.wavBytes = std::move(*completedWav);

	_callbackGate->Post([job = std::move(job)](MeetingTranslatorService *self) mutable {
		if (self->_state != MeetingTranslatorState::Running ||
			self->_generation.load(std::memory_order_acquire) != job.generation) {
			return;
		}
		if (job.isLocalSpeaker && self->_meetingCoordinator) {
			job.trackSid = self->_meetingCoordinator->localAudioTrackSid();
		}
		self->enqueueSttJob(std::move(job));
	});
}

void MeetingTranslatorService::handleStreamingAudioFrame(
		quint64 generation,
		const std::string &key,
		const QString &speakerIdentity,
		const QString &speakerName,
		const QString &trackSid,
		bool isLocalSpeaker,
		const std::vector<int16_t> &pcm16k) {
	const float rms = livekit::AudioVad::CalculateRmsPcm16(pcm16k.data(), pcm16k.size());
	const float wakeDb = _vadThresholdDb.load(std::memory_order_acquire);
	const bool speaking = livekit::AudioVad(wakeDb).IsSpeaking(rms);
	const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

	std::shared_ptr<IStreamingAsrSession> activeSession;
	bool shouldWake = false;
	{
		std::lock_guard<std::mutex> lock(_streamAudioMutex);
		if (_generation.load(std::memory_order_acquire) != generation) {
			return;
		}
		auto &slot = _speakerAudioSlots[key];
		if (!slot) {
			slot = std::make_shared<SpeakerAudioStreamSlot>();
		}
		if (slot->activeSession) {
			if (speaking) {
				slot->lastSpeechAudioMs.store(nowMs, std::memory_order_release);
			}
			activeSession = slot->activeSession;
		} else {
			slot->preRoll.insert(slot->preRoll.end(), pcm16k.begin(), pcm16k.end());
			const size_t maxSamples = slot->wakePending ? kStreamWakePendingMaxSamples : kStreamPreRollSamples;
			while (slot->preRoll.size() > maxSamples) {
				slot->preRoll.pop_front();
			}
			if (speaking) {
				slot->consecutiveSpeechSamples += pcm16k.size();
			} else {
				slot->consecutiveSpeechSamples = 0;
			}
			if (!slot->wakePending && slot->consecutiveSpeechSamples >= kStreamWakeSpeechSamples) {
				slot->wakePending = true;
				slot->lastSpeechAudioMs.store(nowMs, std::memory_order_release);
				shouldWake = true;
			}
		}
	}

	if (activeSession) {
		activeSession->pushPcm16(pcm16k.data(), pcm16k.size(), kTargetSampleRate);
		return;
	}
	if (shouldWake && _callbackGate && _callbackGate->active()) {
		_callbackGate->Post([generation, key, speakerIdentity, speakerName, trackSid, isLocalSpeaker](
				MeetingTranslatorService *self) {
			self->wakeSpeakerStream(
				generation, key, speakerIdentity, speakerName, trackSid, isLocalSpeaker);
		});
	}
}

void MeetingTranslatorService::wakeSpeakerStream(
		quint64 generation,
		const std::string &key,
		const QString &speakerIdentity,
		const QString &speakerName,
		const QString &trackSid,
		bool isLocalSpeaker) {
	const auto clearWakePending = [&] {
		std::lock_guard<std::mutex> lock(_streamAudioMutex);
		const auto it = _speakerAudioSlots.find(key);
		if (it != _speakerAudioSlots.end() && it->second) {
			it->second->wakePending = false;
			it->second->consecutiveSpeechSamples = 0;
		}
	};

	if (_state != MeetingTranslatorState::Running || !_streamingMode ||
		_generation.load(std::memory_order_acquire) != generation || !_agentCoordinator) {
		clearWakePending();
		return;
	}
	if (_speakerPool.find(key) != _speakerPool.end()) {
		clearWakePending();
		return;
	}

	const int maxAllowed = std::clamp(_config.maxConcurrentStreamingAsr, 1, 8);
	evictOldestSpeakerStreamsIfNeeded(std::max(0, maxAllowed - 1), key);

	QString cfgErr;
	const StreamingAsrConfig streamCfg = _agentCoordinator->buildStreamingAsrConfig(&cfgErr);
	if (!cfgErr.isEmpty()) {
		clearWakePending();
		noteAuthOrFatalFailure(cfgErr);
		return;
	}

	std::shared_ptr<IStreamingAsrSession> session = _streamingAsrFactory
		? _streamingAsrFactory(streamCfg)
		: createStreamingAsrSession(streamCfg);
	if (!session) {
		clearWakePending();
		emit errorOccurred(QCoreApplication::translate(
			"MeetingUI", "Failed to create speaker streaming ASR session."));
		return;
	}

	const QString effectiveTrackSid = (isLocalSpeaker && _meetingCoordinator)
		? _meetingCoordinator->localAudioTrackSid()
		: trackSid;
	const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

	ActiveSpeakerStream entry;
	entry.key = key;
	entry.speakerIdentity = speakerIdentity;
	entry.speakerName = speakerName;
	entry.trackSid = effectiveTrackSid;
	entry.isLocalSpeaker = isLocalSpeaker;
	entry.session = session;
	entry.lastSpeechActivityMs = nowMs;
	_speakerPool.emplace(key, std::move(entry));

	const auto gate = _callbackGate;
	std::weak_ptr<IStreamingAsrSession> weakSess = session;
	StreamingAsrCallbacks cbs;
	cbs.onResult = [gate, generation, key, weakSess](const StreamingAsrResult &res) {
		if (!gate || !gate->active()) {
			return;
		}
		gate->Post([generation, key, weakSess, res](MeetingTranslatorService *self) {
			if (auto s = weakSess.lock()) {
				self->handleStreamingResult(generation, key, s, res);
			}
		});
	};
	cbs.onError = [gate, generation, key, weakSess](const QString &safeError) {
		if (!gate || !gate->active()) {
			return;
		}
		gate->Post([generation, key, weakSess, safeError](MeetingTranslatorService *self) {
			self->handleStreamingTerminal(generation, key, 0, weakSess.lock(), safeError);
		});
	};
	cbs.onFinished = [gate, generation, key, weakSess]() {
		if (!gate || !gate->active()) {
			return;
		}
		gate->Post([generation, key, weakSess](MeetingTranslatorService *self) {
			self->handleStreamingTerminal(generation, key, 0, weakSess.lock(), QString());
		});
	};

	session->start(std::move(cbs));

	std::vector<int16_t> preRollVec;
	{
		std::lock_guard<std::mutex> lock(_streamAudioMutex);
		if (_generation.load(std::memory_order_acquire) == generation) {
			auto &slot = _speakerAudioSlots[key];
			if (!slot) {
				slot = std::make_shared<SpeakerAudioStreamSlot>();
			}
			slot->activeSession = session;
			slot->wakePending = false;
			slot->consecutiveSpeechSamples = 0;
			slot->lastSpeechAudioMs.store(nowMs, std::memory_order_release);
			preRollVec.assign(slot->preRoll.begin(), slot->preRoll.end());
			slot->preRoll.clear();
		}
	}
	if (!preRollVec.empty()) {
		session->pushPcm16(preRollVec.data(), preRollVec.size(), kTargetSampleRate);
	}
	updateActivityState();
}

void MeetingTranslatorService::retireSpeakerStream(const std::string &key) {
	const auto it = _speakerPool.find(key);
	if (it == _speakerPool.end()) {
		return;
	}
	ActiveSpeakerStream active = std::move(it->second);
	_speakerPool.erase(it);

	{
		std::lock_guard<std::mutex> lock(_streamAudioMutex);
		const auto audioIt = _speakerAudioSlots.find(key);
		if (audioIt != _speakerAudioSlots.end() && audioIt->second) {
			audioIt->second->activeSession.reset();
			audioIt->second->wakePending = false;
			audioIt->second->consecutiveSpeechSamples = 0;
		}
	}

	if (!active.session) {
		updateActivityState();
		return;
	}

	const quint64 drainId = ++_drainSeq;
	const quint64 gen = _generation.load(std::memory_order_acquire);
	DrainingSpeakerStream draining;
	draining.speakerIdentity = std::move(active.speakerIdentity);
	draining.speakerName = std::move(active.speakerName);
	draining.trackSid = std::move(active.trackSid);
	draining.isLocalSpeaker = active.isLocalSpeaker;
	draining.session = active.session;
	draining.segmentIdMap = std::move(active.segmentIdMap);
	_drainingSessions.insert(drainId, std::move(draining));

	active.session->finish();
	QPointer<MeetingTranslatorService> selfPtr(this);
	QTimer::singleShot(kStreamDrainTimeoutMs, this, [selfPtr, gen, drainId] {
		if (!selfPtr || selfPtr->_generation.load(std::memory_order_acquire) != gen) {
			return;
		}
		auto drainIt = selfPtr->_drainingSessions.find(drainId);
		if (drainIt != selfPtr->_drainingSessions.end()) {
			if (drainIt->session) {
				drainIt->session->abort();
			}
			selfPtr->_drainingSessions.erase(drainIt);
			selfPtr->updateActivityState();
		}
	});
	updateActivityState();
}

void MeetingTranslatorService::evictOldestSpeakerStreamsIfNeeded(
		int maxAllowed,
		const std::string &excludeKey) {
	const int bound = std::max(0, maxAllowed);
	while (static_cast<int>(_speakerPool.size()) > bound) {
		std::string victimKey;
		qint64 oldestMs = (std::numeric_limits<qint64>::max)();
		for (auto &pair : _speakerPool) {
			if (!excludeKey.empty() && pair.first == excludeKey) {
				continue;
			}
			qint64 lastMs = pair.second.lastSpeechActivityMs;
			{
				std::lock_guard<std::mutex> lock(_streamAudioMutex);
				const auto aIt = _speakerAudioSlots.find(pair.first);
				if (aIt != _speakerAudioSlots.end() && aIt->second) {
					lastMs = std::max(lastMs, aIt->second->lastSpeechAudioMs.load(std::memory_order_acquire));
				}
			}
			if (lastMs < oldestMs) {
				oldestMs = lastMs;
				victimKey = pair.first;
			}
		}
		if (victimKey.empty()) {
			break;
		}
		retireSpeakerStream(victimKey);
	}
}

void MeetingTranslatorService::reapIdleStreamingSessions() {
	if (_state != MeetingTranslatorState::Running || !_streamingMode) {
		return;
	}
	const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
	const qint64 idleLimitMs = std::clamp(_config.streamingIdleReleaseMs, 3000, 120000);
	std::vector<std::string> expiredKeys;
	for (auto &pair : _speakerPool) {
		qint64 lastMs = pair.second.lastSpeechActivityMs;
		{
			std::lock_guard<std::mutex> lock(_streamAudioMutex);
			const auto aIt = _speakerAudioSlots.find(pair.first);
			if (aIt != _speakerAudioSlots.end() && aIt->second) {
				lastMs = std::max(lastMs, aIt->second->lastSpeechAudioMs.load(std::memory_order_acquire));
			}
		}
		if ((nowMs - lastMs) >= idleLimitMs) {
			expiredKeys.push_back(pair.first);
		}
	}
	for (const auto &k : expiredKeys) {
		retireSpeakerStream(k);
	}
}

void MeetingTranslatorService::handleStreamingResult(
		quint64 generation,
		const std::string &speakerKey,
		const std::shared_ptr<IStreamingAsrSession> &sourceSession,
		const StreamingAsrResult &res) {
	if (_state != MeetingTranslatorState::Running ||
		_generation.load(std::memory_order_acquire) != generation) {
		return;
	}
	const QString clean = res.text.trimmed();
	if (clean.isEmpty()) {
		return;
	}

	QString speakerIdentity;
	QString speakerName;
	QString trackSid;
	bool isLocalSpeaker = false;
	QHash<QString, QString> *segMap = nullptr;
	qint64 *lastInterimPtr = nullptr;

	auto activeIt = _speakerPool.find(speakerKey);
	if (activeIt != _speakerPool.end() && activeIt->second.session == sourceSession) {
		speakerIdentity = activeIt->second.speakerIdentity;
		speakerName = activeIt->second.speakerName;
		trackSid = activeIt->second.trackSid;
		isLocalSpeaker = activeIt->second.isLocalSpeaker;
		segMap = &activeIt->second.segmentIdMap;
		lastInterimPtr = &activeIt->second.lastInterimEmitMs;
		activeIt->second.lastSpeechActivityMs = QDateTime::currentMSecsSinceEpoch();
	} else {
		for (auto it = _drainingSessions.begin(); it != _drainingSessions.end(); ++it) {
			if (it.value().session == sourceSession) {
				speakerIdentity = it.value().speakerIdentity;
				speakerName = it.value().speakerName;
				trackSid = it.value().trackSid;
				isLocalSpeaker = it.value().isLocalSpeaker;
				segMap = &it.value().segmentIdMap;
				break;
			}
		}
	}
	if (!segMap) {
		return;
	}

	const QString providerSegKey = res.segmentId.trimmed().isEmpty()
		? QStringLiteral("default")
		: res.segmentId.trimmed();
	QString segId = segMap->value(providerSegKey);
	const bool isNewSegment = segId.isEmpty();
	if (isNewSegment) {
		segId = QStringLiteral("ltr-%1-%2").arg(generation).arg(++_segmentSeq);
		segMap->insert(providerSegKey, segId);
	}

	if (!res.isFinal) {
		const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
		if (!isNewSegment && lastInterimPtr && (nowMs - *lastInterimPtr) < kStreamInterimThrottleMs) {
			return;
		}
		if (lastInterimPtr) {
			*lastInterimPtr = nowMs;
		}
	} else {
		segMap->remove(providerSegKey);
	}

	_consecutiveFailures = 0;
	const bool shouldTranslate = res.isFinal &&
		_config.translateEnabled &&
		!_config.targetLanguage.trimmed().isEmpty() &&
		_agentCoordinator && _agentCoordinator->isTranslationTargetConfigured(
			_config.translationProfileId, _config.translationModelOverride);

	MeetingTranscriptItem item;
	item.segmentId = segId;
	item.speakerIdentity = speakerIdentity;
	item.speakerName = speakerName.isEmpty() ? speakerIdentity : speakerName;
	item.trackSid = trackSid;
	item.text = clean;
	item.sourceText = clean;
	item.sourceLanguage = !res.language.trimmed().isEmpty()
		? res.language.trimmed()
		: (_agentCoordinator ? _agentCoordinator->dictationLanguage() : QString());
	item.language = item.sourceLanguage;
	item.timestampMs = QDateTime::currentMSecsSinceEpoch();
	item.isFinal = res.isFinal && !shouldTranslate;
	item.isLocalSpeaker = isLocalSpeaker;
	item.isAgentGenerated = false;
	item.isTranslated = false;
	item.isLocalTranslator = true;

	emitTranscript(item);
	if (shouldTranslate) {
		PendingTranslateJob tJob;
		tJob.generation = generation;
		tJob.interimItem = item;
		tJob.targetLanguage = _config.targetLanguage.trimmed();
		enqueueTranslateJob(std::move(tJob));
	}
}

void MeetingTranslatorService::handleStreamingTerminal(
		quint64 generation,
		const std::string &speakerKey,
		quint64,
		const std::shared_ptr<IStreamingAsrSession> &sourceSession,
		const QString &safeError) {
	if (_generation.load(std::memory_order_acquire) != generation) {
		return;
	}
	bool removedFromActive = false;
	const auto activeIt = _speakerPool.find(speakerKey);
	if (activeIt != _speakerPool.end() && activeIt->second.session == sourceSession) {
		_speakerPool.erase(activeIt);
		removedFromActive = true;
		std::lock_guard<std::mutex> lock(_streamAudioMutex);
		const auto audioIt = _speakerAudioSlots.find(speakerKey);
		if (audioIt != _speakerAudioSlots.end() && audioIt->second) {
			audioIt->second->activeSession.reset();
			audioIt->second->wakePending = false;
			audioIt->second->consecutiveSpeechSamples = 0;
		}
	} else {
		for (auto it = _drainingSessions.begin(); it != _drainingSessions.end(); ++it) {
			if (it.value().session == sourceSession) {
				_drainingSessions.erase(it);
				break;
			}
		}
	}

	if (!safeError.isEmpty() && removedFromActive && _state == MeetingTranslatorState::Running) {
		if (isLikelyAuthError(safeError)) {
			noteAuthOrFatalFailure(safeError);
			return;
		}
		emit errorOccurred(safeError);
	}
	updateActivityState();
}

void MeetingTranslatorService::stopAllStreamingSessions() {
	std::unordered_map<std::string, ActiveSpeakerStream> pool;
	pool.swap(_speakerPool);
	QHash<quint64, DrainingSpeakerStream> draining;
	draining.swap(_drainingSessions);

	{
		std::lock_guard<std::mutex> lock(_streamAudioMutex);
		_speakerAudioSlots.clear();
	}

	for (auto &pair : pool) {
		if (pair.second.session) {
			pair.second.session->abort();
		}
	}
	for (auto it = draining.begin(); it != draining.end(); ++it) {
		if (it.value().session) {
			it.value().session->abort();
		}
	}
}

void MeetingTranslatorService::enqueueSttJob(PendingSttJob job) {
	while (_sttQueue.size() >= kMaxQueuedSttJobs) {
		_sttQueue.pop_front();
	}
	_sttQueue.push_back(std::move(job));
	pumpSttQueue();
}

void MeetingTranslatorService::pumpSttQueue() {
	if (_state != MeetingTranslatorState::Running || !_agentCoordinator || !_speechTransport) {
		return;
	}
	while (_inFlightStt.size() < kMaxInFlightStt && !_sttQueue.empty()) {
		PendingSttJob job = std::move(_sttQueue.front());
		_sttQueue.pop_front();
		if (job.generation != _generation.load(std::memory_order_acquire)) {
			continue;
		}

		QString keyErr;
		const QString apiKey = _agentCoordinator->loadApiKeyForSpeech(&keyErr);
		if (!keyErr.isEmpty()) {
			noteAuthOrFatalFailure(keyErr);
			return;
		}

		SpeechTranscriptionRequest req;
		req.profile = _agentCoordinator->profile();
		req.apiKey = apiKey;
		req.customSttBaseUrl = _agentCoordinator->dictationSttBaseUrl();
		req.protocolMode = _agentCoordinator->dictationSpeechProtocol();
		req.sttModel = _agentCoordinator->resolvedSpeechModel();
		req.sttModelCapabilities = _agentCoordinator->resolveSpeechModelCapabilities(req.sttModel);
		req.language = _agentCoordinator->dictationLanguage();
		req.prompt = _agentCoordinator->dictationPrompt();
		req.wavAudioBytes = job.wavBytes;

		QPointer<AgentSpeechRequestHandle> handle = _speechTransport->transcribeWav(req, this);
		if (!handle) {
			continue;
		}
		_inFlightStt.push_back(handle);
		if (!_lastBusyEmitted) {
			_lastBusyEmitted = true;
			emit activityChanged(true);
		}

		connect(handle, &AgentSpeechRequestHandle::finished, this,
			[this, job, handle](bool success, const QString &text, const QString &safeError) {
				_inFlightStt.removeAll(handle);
				if (handle) {
					handle->deleteLater();
				}
				const bool stale = (_state != MeetingTranslatorState::Running) ||
					(job.generation != _generation.load(std::memory_order_acquire));
				if (stale) {
					return;
				}
				if (!success) {
					if (isLikelyAuthError(safeError)) {
						noteAuthOrFatalFailure(safeError);
						return;
					}
					emit errorOccurred(safeError);
				} else {
					_consecutiveFailures = 0;
					const QString clean = text.trimmed();
					if (!clean.isEmpty()) {
						const QString segId = QStringLiteral("ltr-%1-%2")
							.arg(job.generation)
							.arg(++_segmentSeq);
						const bool shouldTranslate = _config.translateEnabled &&
							!_config.targetLanguage.trimmed().isEmpty() &&
							_agentCoordinator && _agentCoordinator->isTranslationTargetConfigured(
								_config.translationProfileId, _config.translationModelOverride);

						MeetingTranscriptItem item;
						item.segmentId = segId;
						item.speakerIdentity = job.speakerIdentity;
						item.speakerName = job.speakerName.isEmpty() ? job.speakerIdentity : job.speakerName;
						item.trackSid = job.trackSid;
						item.text = clean;
						item.sourceText = clean;
						item.sourceLanguage = _agentCoordinator ? _agentCoordinator->dictationLanguage() : QString();
						item.language = item.sourceLanguage;
						item.timestampMs = QDateTime::currentMSecsSinceEpoch();
						item.isFinal = !shouldTranslate;
						item.isLocalSpeaker = job.isLocalSpeaker;
						item.isAgentGenerated = false;
						item.isTranslated = false;
						item.isLocalTranslator = true;

						emitTranscript(item);
						if (shouldTranslate) {
							PendingTranslateJob tJob;
							tJob.generation = job.generation;
							tJob.interimItem = item;
							tJob.targetLanguage = _config.targetLanguage.trimmed();
							enqueueTranslateJob(std::move(tJob));
						}
					}
				}
				pumpSttQueue();
				updateActivityState();
			});
	}
}

void MeetingTranslatorService::enqueueTranslateJob(PendingTranslateJob job) {
	while (_translateQueue.size() >= kMaxQueuedTranslateJobs) {
		_translateQueue.pop_front();
	}
	_translateQueue.push_back(std::move(job));
	pumpTranslateQueue();
}

QString MeetingTranslatorService::languageDisplayName(const QString &langCode) {
	const QString lower = langCode.trimmed().toLower();
	if (lower == QStringLiteral("zh") || lower == QStringLiteral("zh-cn") || lower == QStringLiteral("zh-hans")) {
		return QStringLiteral("Simplified Chinese (zh-CN)");
	}
	if (lower == QStringLiteral("en")) return QStringLiteral("English");
	if (lower == QStringLiteral("ja")) return QStringLiteral("Japanese");
	if (lower == QStringLiteral("ko")) return QStringLiteral("Korean");
	if (lower == QStringLiteral("fr")) return QStringLiteral("French");
	if (lower == QStringLiteral("de")) return QStringLiteral("German");
	if (lower == QStringLiteral("es")) return QStringLiteral("Spanish");
	return langCode.trimmed();
}

QString MeetingTranslatorService::defaultTranslationPromptTemplate() {
	return QStringLiteral(
		"You are a real-time meeting caption translator. Translate the user's spoken transcript "
		"accurately and concisely into {target_language}. Output ONLY the translated text without quotes, notes, "
		"speaker prefixes, or explanations. If the input is already in {target_language}, return it cleanly as-is.");
}

QString MeetingTranslatorService::buildTranslationSystemPrompt(
		const QString &customTemplate,
		const QString &targetLanguage) {
	const QString langName = languageDisplayName(targetLanguage);
	const QString trimmed = customTemplate.trimmed().left(4000);
	if (trimmed.isEmpty()) {
		QString prompt = defaultTranslationPromptTemplate();
		prompt.replace(QStringLiteral("{target_language}"), langName);
		return prompt;
	}
	const bool hasPlaceholder =
		trimmed.contains(QStringLiteral("{target_language}"), Qt::CaseInsensitive) ||
		trimmed.contains(QStringLiteral("{language}"), Qt::CaseInsensitive);
	QString prompt = trimmed;
	prompt.replace(QStringLiteral("{target_language}"), langName, Qt::CaseInsensitive);
	prompt.replace(QStringLiteral("{language}"), langName, Qt::CaseInsensitive);
	if (!hasPlaceholder && !langName.isEmpty()) {
		prompt.append(QStringLiteral("\nTarget language: %1").arg(langName));
	}
	return prompt;
}

void MeetingTranslatorService::pumpTranslateQueue() {
	if (_state != MeetingTranslatorState::Running || !_agentCoordinator || !_chatTransport) {
		return;
	}
	while (_inFlightTranslate.size() < kMaxInFlightTranslate && !_translateQueue.empty()) {
		PendingTranslateJob job = std::move(_translateQueue.front());
		_translateQueue.pop_front();
		if (job.generation != _generation.load(std::memory_order_acquire)) {
			continue;
		}

		AgentProfile resolvedProfile;
		QString apiKey;
		QString keyErr;
		if (!_agentCoordinator->resolveTranslationChatTarget(
				_config.translationProfileId,
				_config.translationModelOverride,
				&resolvedProfile,
				&apiKey,
				&keyErr)) {
			if (!keyErr.isEmpty()) {
				emit errorOccurred(keyErr);
			}
			MeetingTranscriptItem fallback = job.interimItem;
			fallback.isFinal = true;
			emitTranscript(fallback);
			continue;
		}

		AgentChatRequest req;
		req.profile = std::move(resolvedProfile);
		req.profile.temperature = 0.1;
		req.profile.reasoningEffort = AgentReasoningEffort::Low;
		req.profile.systemPrompt = buildTranslationSystemPrompt(
			_config.translationSystemPrompt, job.targetLanguage);
		req.apiKey = apiKey;
		req.mode = AgentChatMode::Ask;
		req.stream = true;

		AgentChatMessage userMsg;
		userMsg.id = job.interimItem.segmentId;
		userMsg.role = AgentChatMessage::Role::User;
		userMsg.content = job.interimItem.sourceText;
		req.messages.push_back(std::move(userMsg));

		QPointer<AgentRequestHandle> handle = _chatTransport->sendChat(req, this);
		if (!handle) {
			MeetingTranscriptItem fallback = job.interimItem;
			fallback.isFinal = true;
			emitTranscript(fallback);
			continue;
		}
		_inFlightTranslate.push_back(handle);
		if (!_lastBusyEmitted) {
			_lastBusyEmitted = true;
			emit activityChanged(true);
		}

		auto accumulated = std::make_shared<QString>();
		connect(handle, &AgentRequestHandle::deltaReceived, this,
			[accumulated](const AgentStreamDelta &delta) {
				if (!delta.textDelta.isEmpty()) {
					accumulated->append(delta.textDelta);
				}
			});
		connect(handle, &AgentRequestHandle::finished, this,
			[this, job, handle, accumulated](bool success, const QString &safeError) {
				_inFlightTranslate.removeAll(handle);
				if (handle) {
					handle->deleteLater();
				}
				const bool stale = (_state != MeetingTranslatorState::Running) ||
					(job.generation != _generation.load(std::memory_order_acquire));
				if (stale) {
					return;
				}
				MeetingTranscriptItem finalItem = job.interimItem;
				finalItem.isFinal = true;
				finalItem.timestampMs = QDateTime::currentMSecsSinceEpoch();
				const QString translated = accumulated->trimmed();
				if (success && !translated.isEmpty()) {
					finalItem.text = translated;
					finalItem.language = job.targetLanguage;
					finalItem.isTranslated = (translated != finalItem.sourceText);
				} else if (!safeError.isEmpty()) {
					emit errorOccurred(safeError);
				}
				emitTranscript(finalItem);
				pumpTranslateQueue();
				updateActivityState();
			});
	}
}

void MeetingTranslatorService::cancelInFlightRequests() {
	for (const auto &h : _inFlightStt) {
		if (h) {
			h->disconnect(this);
			h->cancel();
			h->deleteLater();
		}
	}
	_inFlightStt.clear();
	for (const auto &h : _inFlightTranslate) {
		if (h) {
			h->disconnect(this);
			h->cancel();
			h->deleteLater();
		}
	}
	_inFlightTranslate.clear();
}

void MeetingTranslatorService::noteAuthOrFatalFailure(const QString &safeError) {
	_lastError = safeError;
	++_consecutiveFailures;
	emit errorOccurred(safeError);
	if (_consecutiveFailures >= kMaxConsecutiveAuthFailures) {
		_idleReapTimer.stop();
		_generation.fetch_add(1, std::memory_order_acq_rel);
		unbindAudioSources();
		cancelInFlightRequests();
		stopAllStreamingSessions();
		_sttQueue.clear();
		_translateQueue.clear();
		updateActivityState();
		setState(MeetingTranslatorState::Error);
	}
}

void MeetingTranslatorService::emitTranscript(const MeetingTranscriptItem &item) {
	if (_meetingCoordinator) {
		_meetingCoordinator->ingestLocalTranscriptItem(item);
	}
	emit transcriptProduced(item);
}

} // namespace OpenMeeting
