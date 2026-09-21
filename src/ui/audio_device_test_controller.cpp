#include <QtCore/QCoreApplication>
#include "audio_device_test_controller.h"

#include "src/media/wasapi_capture.h"
#include "src/media/wasapi_enumerator.h"
#include "src/rtc/audio_frame.h"
#include "src/rtc/audio_source.h"

#include <QtCore/QMetaObject>
#include <QtCore/QThread>

#include <windows.h>
#include <audioclient.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace MeetingUI {
namespace {

using Microsoft::WRL::ComPtr;

constexpr auto kLevelPublishInterval = std::chrono::milliseconds(33);
constexpr double kPi = 3.14159265358979323846;
constexpr double kToneFrequencyHz = 660.0;
constexpr double kToneAmplitude = 0.12;

QString HResultMessage(const QString &operation, HRESULT result) {
	return QCoreApplication::translate("MeetingUI", "%1 failed (error code 0x%2)")
		.arg(operation)
		.arg(static_cast<quint32>(result), 8, 16, QLatin1Char('0'));
}

std::wstring Utf8ToWide(const QString &value) {
	return value.toStdWString();
}

AudioDeviceDescriptor ToDescriptor(const livekit::WasapiDeviceInfo &device) {
	AudioDeviceDescriptor result;
	result.id = QString::fromUtf8(device.id.data(), static_cast<int>(device.id.size()));
	result.name = QString::fromUtf8(device.name.data(), static_cast<int>(device.name.size()));
	result.isDefault = device.is_default;
	result.sampleRate = static_cast<int>(device.default_sample_rate);
	result.channelCount = static_cast<int>(device.default_channels);
	return result;
}

QVector<AudioDeviceDescriptor> ToDescriptors(
		const std::vector<livekit::WasapiDeviceInfo> &devices) {
	QVector<AudioDeviceDescriptor> result;
	result.reserve(static_cast<int>(devices.size()));
	for (const auto &device : devices) {
		result.push_back(ToDescriptor(device));
	}
	return result;
}

float NormalizedLevel(const livekit::AudioFrame &frame) {
	const auto &samples = frame.data();
	if (samples.empty()) {
		return 0.0f;
	}

	long double squaredSum = 0.0;
	for (const auto sample : samples) {
		const auto normalized = static_cast<long double>(sample) / 32768.0L;
		squaredSum += normalized * normalized;
	}
	const auto rms = std::sqrt(squaredSum / static_cast<long double>(samples.size()));
	if (rms <= 0.000001L) {
		return 0.0f;
	}

	// Map -60 dBFS..0 dBFS to the UI's 0..1 range.
	const auto db = 20.0L * std::log10(rms);
	return static_cast<float>(std::clamp((db + 60.0L) / 60.0L, 0.0L, 1.0L));
}

enum class ToneSampleKind {
	Unsupported,
	Float32,
	PcmUnsigned8,
	PcmSigned16,
	PcmSigned24,
	PcmSigned32,
};

struct ToneSampleFormat {
	ToneSampleKind kind = ToneSampleKind::Unsupported;
	int validBits = 0;
};

ToneSampleFormat ParseToneSampleFormat(const WAVEFORMATEX &format) {
	WORD tag = format.wFormatTag;
	GUID subFormat = GUID_NULL;
	int validBits = format.wBitsPerSample;

	if (tag == WAVE_FORMAT_EXTENSIBLE
		&& format.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
		const auto &extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE &>(format);
		subFormat = extended.SubFormat;
		validBits = extended.Samples.wValidBitsPerSample > 0
			? extended.Samples.wValidBitsPerSample
			: format.wBitsPerSample;
		if (IsEqualGUID(subFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
			tag = WAVE_FORMAT_IEEE_FLOAT;
		} else if (IsEqualGUID(subFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
			tag = WAVE_FORMAT_PCM;
		}
	}

	if (tag == WAVE_FORMAT_IEEE_FLOAT && format.wBitsPerSample == 32) {
		return { ToneSampleKind::Float32, 32 };
	}
	if (tag != WAVE_FORMAT_PCM) {
		return {};
	}

	switch (format.wBitsPerSample) {
	case 8: return { ToneSampleKind::PcmUnsigned8, 8 };
	case 16: return { ToneSampleKind::PcmSigned16, std::min(validBits, 16) };
	case 24: return { ToneSampleKind::PcmSigned24, std::min(validBits, 24) };
	case 32: return { ToneSampleKind::PcmSigned32, std::min(validBits, 32) };
	default: return {};
	}
}

void WriteToneSample(
		BYTE *destination,
		const ToneSampleFormat &format,
		double value) {
	value = std::clamp(value, -1.0, 1.0);
	switch (format.kind) {
	case ToneSampleKind::Float32: {
		const auto sample = static_cast<float>(value);
		std::memcpy(destination, &sample, sizeof(sample));
		break;
	}
	case ToneSampleKind::PcmUnsigned8: {
		const auto sample = static_cast<std::uint8_t>(
			std::clamp(std::lround((value + 1.0) * 127.5), 0L, 255L));
		*destination = sample;
		break;
	}
	case ToneSampleKind::PcmSigned16: {
		const auto sample = static_cast<std::int16_t>(
			std::clamp(std::llround(value * 32767.0), -32768LL, 32767LL));
		std::memcpy(destination, &sample, sizeof(sample));
		break;
	}
	case ToneSampleKind::PcmSigned24: {
		const auto sample = static_cast<std::int32_t>(
			std::clamp(std::llround(value * 8388607.0), -8388608LL, 8388607LL));
		destination[0] = static_cast<BYTE>(sample & 0xff);
		destination[1] = static_cast<BYTE>((sample >> 8) & 0xff);
		destination[2] = static_cast<BYTE>((sample >> 16) & 0xff);
		break;
	}
	case ToneSampleKind::PcmSigned32: {
		const auto bits = std::clamp(format.validBits, 1, 32);
		const auto maximum = bits == 32
			? static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())
			: (static_cast<std::int64_t>(1) << (bits - 1)) - 1;
		auto sample = static_cast<std::int64_t>(std::llround(value * maximum));
		if (bits < 32) {
			sample <<= (32 - bits);
		}
		const auto stored = static_cast<std::int32_t>(sample);
		std::memcpy(destination, &stored, sizeof(stored));
		break;
	}
	case ToneSampleKind::Unsupported:
		break;
	}
}

void FillToneBuffer(
		BYTE *buffer,
		UINT32 frameCount,
		const WAVEFORMATEX &format,
		const ToneSampleFormat &sampleFormat,
		std::uint64_t firstFrame,
		std::uint64_t totalFrames) {
	const auto bytesPerSample = format.wBitsPerSample / 8;
	const auto rampFrames = std::max<std::uint64_t>(1, format.nSamplesPerSec / 100);
	for (UINT32 frame = 0; frame != frameCount; ++frame) {
		const auto frameIndex = firstFrame + frame;
		const auto fadeIn = std::min(1.0, static_cast<double>(frameIndex) / rampFrames);
		const auto framesRemaining = totalFrames > frameIndex ? totalFrames - frameIndex : 0;
		const auto fadeOut = std::min(1.0, static_cast<double>(framesRemaining) / rampFrames);
		const auto envelope = std::min(fadeIn, fadeOut);
		const auto phase = 2.0 * kPi * kToneFrequencyHz
			* static_cast<double>(frameIndex) / format.nSamplesPerSec;
		const auto value = std::sin(phase) * kToneAmplitude * envelope;

		for (WORD channel = 0; channel != format.nChannels; ++channel) {
			auto *sample = buffer
				+ static_cast<size_t>(frame) * format.nBlockAlign
				+ static_cast<size_t>(channel) * bytesPerSample;
			WriteToneSample(sample, sampleFormat, value);
		}
	}
}

struct ToneResult {
	bool success = false;
	QString message;
};

class ScopedComInitialization final {
public:
	ScopedComInitialization()
	: _result(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {
	}

	~ScopedComInitialization() {
		if (SUCCEEDED(_result)) {
			CoUninitialize();
		}
	}

	HRESULT result() const noexcept {
		return _result;
	}

private:
	HRESULT _result = E_FAIL;
};

ToneResult PlayTone(
		const QString &deviceId,
		int durationMs,
		const std::atomic<bool> &stopRequested) {
	ScopedComInitialization com;
	const auto comResult = com.result();
	if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
		return { false, HResultMessage(QCoreApplication::translate("MeetingUI", "Initialize audio thread"), comResult) };
	}

	ComPtr<IMMDeviceEnumerator> enumerator;
	auto result = CoCreateInstance(
		__uuidof(MMDeviceEnumerator),
		nullptr,
		CLSCTX_ALL,
		IID_PPV_ARGS(&enumerator));
	if (FAILED(result) || !enumerator) {
		return { false, HResultMessage(QCoreApplication::translate("MeetingUI", "Open audio device service"), result) };
	}

	ComPtr<IMMDevice> device;
	if (deviceId.isEmpty()) {
		result = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
		if (FAILED(result) || !device) {
			result = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
		}
	} else {
		const auto wideId = Utf8ToWide(deviceId);
		result = enumerator->GetDevice(wideId.c_str(), &device);
	}
	if (FAILED(result) || !device) {
		return { false, HResultMessage(QCoreApplication::translate("MeetingUI", "Open selected speaker"), result) };
	}

	ComPtr<IAudioClient> audioClient;
	result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &audioClient);
	if (FAILED(result) || !audioClient) {
		return { false, HResultMessage(QCoreApplication::translate("MeetingUI", "Create speaker test stream"), result) };
	}

	WAVEFORMATEX *mixFormat = nullptr;
	result = audioClient->GetMixFormat(&mixFormat);
	if (FAILED(result) || !mixFormat) {
		return { false, HResultMessage(QCoreApplication::translate("MeetingUI", "Read speaker format"), result) };
	}

	auto releaseFormat = [&] {
		CoTaskMemFree(mixFormat);
		mixFormat = nullptr;
	};
	const auto sampleFormat = ParseToneSampleFormat(*mixFormat);
	if (sampleFormat.kind == ToneSampleKind::Unsupported
		|| mixFormat->nChannels == 0
		|| mixFormat->nSamplesPerSec == 0
		|| mixFormat->nBlockAlign == 0) {
		releaseFormat();
		return { false, QCoreApplication::translate("MeetingUI", "The selected speaker's shared format does not support the test tone") };
	}

	constexpr REFERENCE_TIME kRequestedBufferDuration = 100 * 10000;
	result = audioClient->Initialize(
		AUDCLNT_SHAREMODE_SHARED,
		0,
		kRequestedBufferDuration,
		0,
		mixFormat,
		nullptr);
	if (FAILED(result)) {
		releaseFormat();
		return { false, HResultMessage(QCoreApplication::translate("MeetingUI", "Initialize speaker test stream"), result) };
	}

	UINT32 bufferFrameCount = 0;
	result = audioClient->GetBufferSize(&bufferFrameCount);
	ComPtr<IAudioRenderClient> renderClient;
	if (SUCCEEDED(result)) {
		result = audioClient->GetService(__uuidof(IAudioRenderClient), &renderClient);
	}
	if (FAILED(result) || !renderClient || bufferFrameCount == 0) {
		releaseFormat();
		return { false, HResultMessage(QCoreApplication::translate("MeetingUI", "Prepare speaker test buffer"), result) };
	}

	const auto totalFrames = std::max<std::uint64_t>(
		1,
		static_cast<std::uint64_t>(mixFormat->nSamplesPerSec)
			* static_cast<std::uint64_t>(durationMs) / 1000);
	std::uint64_t submittedFrames = 0;
	bool started = false;

	auto submitAvailableFrames = [&]() -> HRESULT {
		UINT32 padding = 0;
		auto submitResult = audioClient->GetCurrentPadding(&padding);
		if (FAILED(submitResult)) {
			return submitResult;
		}
		const auto available = bufferFrameCount > padding ? bufferFrameCount - padding : 0;
		const auto remaining = totalFrames - submittedFrames;
		const auto frames = static_cast<UINT32>(std::min<std::uint64_t>(available, remaining));
		if (frames == 0) {
			return S_OK;
		}

		BYTE *buffer = nullptr;
		submitResult = renderClient->GetBuffer(frames, &buffer);
		if (FAILED(submitResult)) {
			return submitResult;
		}
		FillToneBuffer(
			buffer,
			frames,
			*mixFormat,
			sampleFormat,
			submittedFrames,
			totalFrames);
		submitResult = renderClient->ReleaseBuffer(frames, 0);
		if (SUCCEEDED(submitResult)) {
			submittedFrames += frames;
		}
		return submitResult;
	};

	result = submitAvailableFrames();
	if (SUCCEEDED(result) && !stopRequested.load(std::memory_order_acquire)) {
		result = audioClient->Start();
		started = SUCCEEDED(result);
	}

	while (SUCCEEDED(result)
		&& !stopRequested.load(std::memory_order_acquire)
		&& submittedFrames < totalFrames) {
		std::this_thread::sleep_for(std::chrono::milliseconds(8));
		result = submitAvailableFrames();
	}

	if (SUCCEEDED(result) && started && !stopRequested.load(std::memory_order_acquire)) {
		const auto drainDeadline = std::chrono::steady_clock::now()
			+ std::chrono::milliseconds(500);
		while (std::chrono::steady_clock::now() < drainDeadline
			&& !stopRequested.load(std::memory_order_acquire)) {
			UINT32 padding = 0;
			result = audioClient->GetCurrentPadding(&padding);
			if (FAILED(result) || padding == 0) {
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(8));
		}
	}

	if (started) {
		audioClient->Stop();
	}
	releaseFormat();

	if (stopRequested.load(std::memory_order_acquire)) {
		return { false, QCoreApplication::translate("MeetingUI", "Speaker test stopped") };
	}
	if (FAILED(result)) {
		return { false, HResultMessage(QCoreApplication::translate("MeetingUI", "Play speaker test tone"), result) };
	}
	return { true, QString() };
}

} // namespace

struct AudioDeviceTestController::Impl {
	std::atomic<bool> shuttingDown = false;

	std::atomic<bool> enumerating = false;
	std::atomic<std::uint64_t> enumerationGeneration = 0;
	std::thread enumerationThread;
	std::mutex enumerationMutex;

	std::atomic<bool> microphoneActive = false;
	std::atomic<std::uint64_t> microphoneGeneration = 0;
	std::atomic<std::int64_t> lastLevelPublishNs = 0;
	std::shared_ptr<livekit::WasapiAudioCapture> microphoneCapture;
	std::shared_ptr<livekit::AudioSource> microphoneSource;
	std::mutex microphoneMutex;

	std::atomic<bool> speakerActive = false;
	std::atomic<bool> speakerStopRequested = false;
	std::atomic<std::uint64_t> speakerGeneration = 0;
	std::thread speakerThread;
	std::mutex speakerMutex;
};

AudioDeviceTestController::AudioDeviceTestController(QObject *parent)
: QObject(parent)
, _impl(std::make_unique<Impl>()) {
	qRegisterMetaType<MeetingUI::AudioDeviceDescriptor>(
		"MeetingUI::AudioDeviceDescriptor");
	qRegisterMetaType<QVector<MeetingUI::AudioDeviceDescriptor>>(
		"QVector<MeetingUI::AudioDeviceDescriptor>");
}

AudioDeviceTestController::~AudioDeviceTestController() {
	_impl->shuttingDown.store(true, std::memory_order_release);
	stopAll();
	std::lock_guard<std::mutex> lock(_impl->enumerationMutex);
	if (_impl->enumerationThread.joinable()) {
		_impl->enumerationThread.join();
	}
}

bool AudioDeviceTestController::isEnumerating() const noexcept {
	return _impl->enumerating.load(std::memory_order_acquire);
}

bool AudioDeviceTestController::isMicrophoneTestActive() const noexcept {
	return _impl->microphoneActive.load(std::memory_order_acquire);
}

bool AudioDeviceTestController::isSpeakerTestActive() const noexcept {
	return _impl->speakerActive.load(std::memory_order_acquire);
}

void AudioDeviceTestController::refreshDevices() {
	Q_ASSERT(thread() == QThread::currentThread());
	std::uint64_t generation = 0;
	{
		std::lock_guard<std::mutex> lock(_impl->enumerationMutex);
		if (_impl->enumerating.exchange(true, std::memory_order_acq_rel)) {
			return;
		}
		if (_impl->enumerationThread.joinable()) {
			_impl->enumerationThread.join();
		}

		generation = _impl->enumerationGeneration.fetch_add(
			1,
			std::memory_order_acq_rel) + 1;
		_impl->enumerationThread = std::thread([this, generation] {
			QVector<AudioDeviceDescriptor> microphones;
			QVector<AudioDeviceDescriptor> speakers;
			QString error;
			try {
				microphones = ToDescriptors(livekit::WasapiEnumerator::EnumerateInputDevices());
				speakers = ToDescriptors(livekit::WasapiEnumerator::EnumerateOutputDevices());
			} catch (const std::exception &exception) {
				error = QCoreApplication::translate("MeetingUI", "Failed to list audio devices: %1")
					.arg(QString::fromLocal8Bit(exception.what()));
			} catch (...) {
				error = QCoreApplication::translate("MeetingUI", "Failed to list audio devices");
			}

			_impl->enumerating.store(false, std::memory_order_release);
			QMetaObject::invokeMethod(
				this,
				[this,
				 generation,
				 microphones = std::move(microphones),
				 speakers = std::move(speakers),
				 error = std::move(error)]() mutable {
					if (_impl->shuttingDown.load(std::memory_order_acquire)
						|| _impl->enumerationGeneration.load(std::memory_order_acquire) != generation) {
						return;
					}
					emit deviceEnumerationStateChanged(false);
					if (!error.isEmpty()) {
						emit deviceEnumerationFailed(error);
						return;
					}
					emit devicesChanged(microphones, speakers);
				},
				Qt::QueuedConnection);
		});
	}
	emit deviceEnumerationStateChanged(true);
}

void AudioDeviceTestController::startMicrophoneTest(const QString &deviceId) {
	Q_ASSERT(thread() == QThread::currentThread());
	stopMicrophoneTest();

	const auto availableDevices = livekit::WasapiEnumerator::EnumerateInputDevices();
	const auto requestedId = deviceId.toUtf8().toStdString();
	const auto found = std::find_if(
		availableDevices.begin(),
		availableDevices.end(),
		[&](const livekit::WasapiDeviceInfo &device) {
			return requestedId.empty() || device.id == requestedId;
		});
	if (found == availableDevices.end()) {
		emit microphoneTestFailed(QCoreApplication::translate("MeetingUI", "No microphone available"));
		return;
	}

	const auto generation = _impl->microphoneGeneration.fetch_add(
		1,
		std::memory_order_acq_rel) + 1;
	_impl->lastLevelPublishNs.store(0, std::memory_order_release);
	const auto source = std::make_shared<livekit::AudioSource>(48000, 1);
	const auto capture = livekit::WasapiAudioCapture::Create();

	source->addSink([this, generation](const livekit::AudioFrame &frame) {
		if (_impl->shuttingDown.load(std::memory_order_acquire)
			|| !_impl->microphoneActive.load(std::memory_order_acquire)
			|| _impl->microphoneGeneration.load(std::memory_order_acquire) != generation) {
			return;
		}

		const auto nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
		auto previousNs = _impl->lastLevelPublishNs.load(std::memory_order_relaxed);
		if (previousNs != 0
			&& nowNs - previousNs < std::chrono::duration_cast<std::chrono::nanoseconds>(
				kLevelPublishInterval).count()) {
			return;
		}
		if (!_impl->lastLevelPublishNs.compare_exchange_strong(
				previousNs,
				nowNs,
				std::memory_order_acq_rel,
				std::memory_order_relaxed)) {
			return;
		}

		const auto level = NormalizedLevel(frame);
		QMetaObject::invokeMethod(
			this,
			[this, generation, level] {
				if (!_impl->shuttingDown.load(std::memory_order_acquire)
					&& _impl->microphoneActive.load(std::memory_order_acquire)
					&& _impl->microphoneGeneration.load(std::memory_order_acquire) == generation) {
					emit microphoneLevelChanged(level);
				}
			},
			Qt::QueuedConnection);
	});

	livekit::WasapiCaptureConfig config;
	config.device_id = requestedId;
	config.type = livekit::WasapiCaptureType::Microphone;
	config.target_sample_rate = 48000;
	config.target_channels = 1;
	config.buffer_duration_ms = 20;
	config.auto_reconnect = true;

	if (!capture->Init(config, source)) {
		emit microphoneTestFailed(QCoreApplication::translate("MeetingUI", "Unable to initialize microphone test"));
		return;
	}
	// WasapiAudioCapture creates a private APM by default. The level meter needs
	// raw samples only and must not attach to the meeting's render reference.
	capture->DisableApm();
	if (!capture->Start()) {
		emit microphoneTestFailed(QCoreApplication::translate("MeetingUI", "Unable to start microphone test"));
		return;
	}

	{
		std::lock_guard<std::mutex> lock(_impl->microphoneMutex);
		_impl->microphoneSource = source;
		_impl->microphoneCapture = capture;
	}
	_impl->microphoneActive.store(true, std::memory_order_release);
	emit microphoneTestStateChanged(true);
}

void AudioDeviceTestController::stopMicrophoneTest() {
	Q_ASSERT(thread() == QThread::currentThread());
	const auto wasActive = _impl->microphoneActive.exchange(false, std::memory_order_acq_rel);
	_impl->microphoneGeneration.fetch_add(1, std::memory_order_acq_rel);

	std::shared_ptr<livekit::WasapiAudioCapture> capture;
	{
		std::lock_guard<std::mutex> lock(_impl->microphoneMutex);
		capture = std::move(_impl->microphoneCapture);
	}
	if (capture) {
		capture->Stop();
	}
	{
		std::lock_guard<std::mutex> lock(_impl->microphoneMutex);
		_impl->microphoneSource.reset();
	}

	if (wasActive && !_impl->shuttingDown.load(std::memory_order_acquire)) {
		emit microphoneLevelChanged(0.0f);
		emit microphoneTestStateChanged(false);
	}
}

void AudioDeviceTestController::playSpeakerTestTone(
		const QString &deviceId,
		int durationMs) {
	Q_ASSERT(thread() == QThread::currentThread());
	stopSpeakerTestTone();
	durationMs = std::clamp(durationMs, 250, 5000);

	const auto generation = _impl->speakerGeneration.fetch_add(
		1,
		std::memory_order_acq_rel) + 1;
	_impl->speakerStopRequested.store(false, std::memory_order_release);
	_impl->speakerActive.store(true, std::memory_order_release);
	{
		std::lock_guard<std::mutex> lock(_impl->speakerMutex);
		_impl->speakerThread = std::thread([this, deviceId, durationMs, generation] {
			auto result = PlayTone(deviceId, durationMs, _impl->speakerStopRequested);
			QMetaObject::invokeMethod(
				this,
				[this, generation, result = std::move(result)]() mutable {
					if (_impl->shuttingDown.load(std::memory_order_acquire)
						|| _impl->speakerGeneration.load(std::memory_order_acquire) != generation) {
						return;
					}
					_impl->speakerActive.store(false, std::memory_order_release);
					emit speakerTestStateChanged(false);
					emit speakerTestFinished(result.success, result.message);
				},
				Qt::QueuedConnection);
		});
	}
	emit speakerTestStateChanged(true);
}

void AudioDeviceTestController::stopSpeakerTestTone() {
	Q_ASSERT(thread() == QThread::currentThread());
	const auto wasActive = _impl->speakerActive.exchange(false, std::memory_order_acq_rel);
	_impl->speakerGeneration.fetch_add(1, std::memory_order_acq_rel);
	_impl->speakerStopRequested.store(true, std::memory_order_release);

	{
		std::lock_guard<std::mutex> lock(_impl->speakerMutex);
		if (_impl->speakerThread.joinable()) {
			_impl->speakerThread.join();
		}
	}

	if (wasActive && !_impl->shuttingDown.load(std::memory_order_acquire)) {
		emit speakerTestStateChanged(false);
	}
}

void AudioDeviceTestController::stopAll() {
	Q_ASSERT(thread() == QThread::currentThread());
	stopMicrophoneTest();
	stopSpeakerTestTone();
}

} // namespace MeetingUI
