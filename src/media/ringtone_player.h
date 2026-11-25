#pragma once

#include <QtCore/QString>

#include <memory>

namespace OpenMeeting {

// Lightweight zero-QtMultimedia ringtone synthesizer.
// Windows uses WASAPI Shared Mode; Linux uses PulseAudio when available (no-op fallback otherwise).
// Always call stop() synchronously BEFORE handing audio hardware to MeetingCoordinator.
class RingtonePlayer final {
public:
    RingtonePlayer();
    ~RingtonePlayer();

    void start(const QString &speakerDeviceId = {}, bool speakerEnabled = true);
    void stop();
    bool isPlaying() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;

    RingtonePlayer(const RingtonePlayer &) = delete;
    RingtonePlayer &operator=(const RingtonePlayer &) = delete;
};

} // namespace OpenMeeting
