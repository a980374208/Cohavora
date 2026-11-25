#include "src/media/ringtone_player.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__linux__) && defined(COHAVORA_HAS_PULSE_SIMPLE)
#include <pulse/simple.h>
#endif

namespace OpenMeeting {

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
    Q_UNUSED(speakerDeviceId);
#if defined(__linux__) && defined(COHAVORA_HAS_PULSE_SIMPLE)
    std::lock_guard<std::mutex> lock(_impl->mu);
    _impl->stopFlag.store(false, std::memory_order_release);
    _impl->playing.store(true, std::memory_order_release);
    _impl->thread = std::thread([this]() {
        pa_sample_spec spec{};
        spec.format = PA_SAMPLE_S16LE;
        spec.rate = 48000;
        spec.channels = 1;
        int error = 0;
        pa_simple *pa = pa_simple_new(nullptr, "Cohavora", PA_STREAM_PLAYBACK, nullptr,
                                      "ringtone", &spec, nullptr, nullptr, &error);
        if (pa) {
            constexpr double kPi = 3.14159265358979323846;
            std::vector<int16_t> buf(960);
            uint64_t frameIndex = 0;
            const auto started = std::chrono::steady_clock::now();
            while (!_impl->stopFlag.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() - started < std::chrono::seconds(50)) {
                for (size_t i = 0; i < buf.size(); ++i, ++frameIndex) {
                    const double t = static_cast<double>(frameIndex) / 48000.0;
                    const double phase = std::fmod(t, 3.0);
                    double s = 0.0;
                    if (phase < 1.2) {
                        const double env = std::min({1.0, phase / 0.04, (1.2 - phase) / 0.04});
                        s = 0.09 * env * (0.55 * std::sin(2.0 * kPi * 440.0 * t) + 0.45 * std::sin(2.0 * kPi * 554.365 * t));
                    }
                    buf[i] = static_cast<int16_t>(std::lrint(s * 32767.0));
                }
                if (pa_simple_write(pa, buf.data(), buf.size() * sizeof(int16_t), &error) < 0) break;
            }
            pa_simple_free(pa);
        }
        _impl->playing.store(false, std::memory_order_release);
    });
#endif
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
