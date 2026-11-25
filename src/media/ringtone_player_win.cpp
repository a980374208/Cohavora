#include "src/media/ringtone_player.h"

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
#include <mutex>
#include <thread>

namespace OpenMeeting {
namespace {

using Microsoft::WRL::ComPtr;

constexpr double kPi = 3.14159265358979323846;
constexpr double kFreqA = 440.0;    // A4
constexpr double kFreqB = 554.365;  // C#5 (pleasant major third chime)
constexpr double kAmplitude = 0.09;
constexpr double kRingOnSeconds = 1.2;
constexpr double kCadenceSeconds = 3.0;
constexpr double kFadeSeconds = 0.04;

enum class SampleKind { Unsupported, Float32, Pcm16, Pcm32 };

SampleKind resolveSampleKind(const WAVEFORMATEX &fmt) {
    WORD tag = fmt.wFormatTag;
    GUID subFormat = GUID_NULL;
    if (tag == WAVE_FORMAT_EXTENSIBLE && fmt.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto &ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE &>(fmt);
        subFormat = ext.SubFormat;
    }
    if (tag == WAVE_FORMAT_IEEE_FLOAT || (tag == WAVE_FORMAT_EXTENSIBLE && IsEqualGUID(subFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))) {
        return fmt.wBitsPerSample == 32 ? SampleKind::Float32 : SampleKind::Unsupported;
    }
    if (tag == WAVE_FORMAT_PCM || (tag == WAVE_FORMAT_EXTENSIBLE && IsEqualGUID(subFormat, KSDATAFORMAT_SUBTYPE_PCM))) {
        if (fmt.wBitsPerSample == 16) return SampleKind::Pcm16;
        if (fmt.wBitsPerSample == 32) return SampleKind::Pcm32;
    }
    return SampleKind::Unsupported;
}

void writeSample(BYTE *dst, SampleKind kind, double value) {
    const double clamped = std::clamp(value, -1.0, 1.0);
    switch (kind) {
    case SampleKind::Float32: {
        const float f = static_cast<float>(clamped);
        std::memcpy(dst, &f, sizeof(f));
        break;
    }
    case SampleKind::Pcm16: {
        const auto s = static_cast<int16_t>(std::lrint(clamped * 32767.0));
        std::memcpy(dst, &s, sizeof(s));
        break;
    }
    case SampleKind::Pcm32: {
        const auto s = static_cast<int32_t>(std::llrint(clamped * 2147483647.0));
        std::memcpy(dst, &s, sizeof(s));
        break;
    }
    default:
        break;
    }
}

void runWasapiRingLoop(const QString &deviceId, std::atomic<bool> &stopFlag) {
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comOwned = SUCCEEDED(comHr);
    auto comCleanup = [&]() {
        if (comOwned) CoUninitialize();
    };

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
        comCleanup();
        return;
    }
    ComPtr<IMMDevice> device;
    if (!deviceId.trimmed().isEmpty()) {
        const std::wstring wideId = deviceId.toStdWString();
        enumerator->GetDevice(wideId.c_str(), &device);
    }
    if (!device) {
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eCommunications, &device))) {
            if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
                comCleanup();
                return;
            }
        }
    }

    ComPtr<IAudioClient> audioClient;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(audioClient.GetAddressOf())))) {
        comCleanup();
        return;
    }

    WAVEFORMATEX *mixFormat = nullptr;
    if (FAILED(audioClient->GetMixFormat(&mixFormat)) || !mixFormat) {
        comCleanup();
        return;
    }

    const SampleKind sampleKind = resolveSampleKind(*mixFormat);
    const uint32_t sampleRate = mixFormat->nSamplesPerSec;
    const uint16_t channels = mixFormat->nChannels;
    const uint32_t bytesPerSample = mixFormat->wBitsPerSample / 8;

    const REFERENCE_TIME bufferDuration = 1000000; // 100ms
    const HRESULT initHr = audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED, 0, bufferDuration, 0, mixFormat, nullptr);
    CoTaskMemFree(mixFormat);
    if (FAILED(initHr) || sampleKind == SampleKind::Unsupported || sampleRate == 0 || channels == 0) {
        comCleanup();
        return;
    }

    UINT32 bufferFrames = 0;
    ComPtr<IAudioRenderClient> renderClient;
    if (FAILED(audioClient->GetBufferSize(&bufferFrames)) ||
        FAILED(audioClient->GetService(IID_PPV_ARGS(&renderClient))) ||
        FAILED(audioClient->Start())) {
        comCleanup();
        return;
    }

    uint64_t frameIndex = 0;
    const auto startedAt = std::chrono::steady_clock::now();
    // Bounded to 50s safety ceiling so an abandoned instance can never ring forever.
    while (!stopFlag.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() - startedAt < std::chrono::seconds(50)) {
        UINT32 padding = 0;
        if (FAILED(audioClient->GetCurrentPadding(&padding))) break;
        const UINT32 available = bufferFrames > padding ? (bufferFrames - padding) : 0;
        if (available > 0) {
            BYTE *data = nullptr;
            if (SUCCEEDED(renderClient->GetBuffer(available, &data)) && data) {
                for (UINT32 i = 0; i < available; ++i, ++frameIndex) {
                    const double t = static_cast<double>(frameIndex) / static_cast<double>(sampleRate);
                    const double phaseT = std::fmod(t, kCadenceSeconds);
                    double sample = 0.0;
                    if (phaseT < kRingOnSeconds) {
                        double env = 1.0;
                        if (phaseT < kFadeSeconds) env = phaseT / kFadeSeconds;
                        else if (phaseT > kRingOnSeconds - kFadeSeconds) env = (kRingOnSeconds - phaseT) / kFadeSeconds;
                        const double wave = 0.55 * std::sin(2.0 * kPi * kFreqA * t) +
                                            0.45 * std::sin(2.0 * kPi * kFreqB * t);
                        sample = kAmplitude * env * wave;
                    }
                    BYTE *framePtr = data + static_cast<size_t>(i) * channels * bytesPerSample;
                    for (uint16_t ch = 0; ch < channels; ++ch) {
                        writeSample(framePtr + static_cast<size_t>(ch) * bytesPerSample, sampleKind, sample);
                    }
                }
                renderClient->ReleaseBuffer(available, 0);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }

    audioClient->Stop();
    comCleanup();
}

} // namespace

struct RingtonePlayer::Impl {
    std::mutex mu;
    std::thread thread;
    std::atomic<bool> stopFlag{true};
    std::atomic<bool> playing{false};
};

RingtonePlayer::RingtonePlayer() : _impl(std::make_unique<Impl>()) {}

RingtonePlayer::~RingtonePlayer() {
    stop();
}

void RingtonePlayer::start(const QString &speakerDeviceId, bool speakerEnabled) {
    stop();
    if (!speakerEnabled) return;
    std::lock_guard<std::mutex> lock(_impl->mu);
    _impl->stopFlag.store(false, std::memory_order_release);
    _impl->playing.store(true, std::memory_order_release);
    _impl->thread = std::thread([this, speakerDeviceId]() {
        runWasapiRingLoop(speakerDeviceId, _impl->stopFlag);
        _impl->playing.store(false, std::memory_order_release);
    });
}

void RingtonePlayer::stop() {
    _impl->stopFlag.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lock(_impl->mu);
    if (_impl->thread.joinable()) {
        _impl->thread.join();
    }
    _impl->playing.store(false, std::memory_order_release);
}

bool RingtonePlayer::isPlaying() const noexcept {
    return _impl->playing.load(std::memory_order_acquire);
}

} // namespace OpenMeeting
