#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <asio.hpp>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFont>
#include <QFontMetrics>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMetaObject>
#include <QPainter>
#include <QPaintEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "src/core/room.h"
#include "src/core/local_audio_track.h"
#include "src/core/local_video_track.h"
#include "src/media/linux_camera_capture.h"
#include "src/render/qt_cpu_video_renderer.h"
#include "src/ui/app_theme.h"
#include "api/audio_options.h"
#include "api/media_stream_interface.h"

namespace {

using Frame = livekit::render::OwnedI420Frame::Ptr;

enum class EventKind {
    Connected,
    Disconnected,
    Reconnecting,
    Reconnected,
    ConnectFailed,
    ParticipantUpsert,
    ParticipantRemove,
    RemoteReset,
    VideoAdded,
    VideoRemoved,
    FrameReady,
    AudioOutput,
    Devices,
    MicrophoneState,
    CameraState,
    SpeakerState,
};

struct DeviceChoice {
    QString id;
    QString name;
};

struct UiEvent {
    EventKind kind;
    uint64_t generation = 0;
    QString participant_sid;
    QString participant_name;
    QString track_sid;
    int audio_tracks = 0;
    int video_tracks = 0;
    Frame frame;
    bool audio_available = false;
    bool enabled = false;
    bool success = true;
    QString selected_id;
    QString message;
    QString microphone_selected_id;
    QString speaker_selected_id;
    QString camera_selected_id;
    std::vector<DeviceChoice> microphones;
    std::vector<DeviceChoice> speakers;
    std::vector<DeviceChoice> cameras;
};

struct FrameMailbox {
    bool Submit(Frame frame) {
        std::lock_guard lock(mutex);
        if (!active) return false;
        latest = std::move(frame);
        if (queued) return false;
        queued = true;
        return true;
    }

    Frame Take() {
        std::lock_guard lock(mutex);
        queued = false;
        return std::exchange(latest, {});
    }

    void Close() {
        std::lock_guard lock(mutex);
        active = false;
        latest.reset();
    }

    std::mutex mutex;
    Frame latest;
    bool active = true;
    bool queued = false;
};

class NativeSession;

class SessionListener final : public livekit::RoomListener {
public:
    explicit SessionListener(std::weak_ptr<NativeSession> owner) : owner_(std::move(owner)) {}

    void OnDisconnected(livekit::RoomDisconnectReason, const std::string&) override;
    void OnReconnecting() override;
    void OnReconnected() override;
    void OnParticipantConnected(std::shared_ptr<livekit::RemoteParticipant> participant) override;
    void OnParticipantDisconnected(std::shared_ptr<livekit::RemoteParticipant> participant) override;
    void OnTrackSubscribed(std::shared_ptr<livekit::Track> track,
                           std::shared_ptr<livekit::TrackPublication> publication,
                           std::shared_ptr<livekit::RemoteParticipant> participant) override;
    void OnTrackUnsubscribed(std::shared_ptr<livekit::Track> track,
                             std::shared_ptr<livekit::TrackPublication> publication,
                             std::shared_ptr<livekit::RemoteParticipant> participant) override;
    void OnTrackMuted(std::shared_ptr<livekit::Participant> participant,
                      std::shared_ptr<livekit::TrackPublication> publication, bool muted) override;
    void OnLocalTrackRepublished(const std::string& previous_sid,
                                 std::shared_ptr<livekit::TrackPublication> publication) override;

private:
    std::weak_ptr<NativeSession> owner_;
};

class NativeSession final : public std::enable_shared_from_this<NativeSession> {
public:
    NativeSession(QObject& receiver, std::function<void(UiEvent)> deliver)
        : receiver_(receiver), deliver_(std::move(deliver)),
          guard_(asio::make_work_guard(io_)),
          room_(livekit::Room::Create(io_.get_executor())) {}

    ~NativeSession() { Shutdown(); }

    void Start() {
        listener_ = std::make_shared<SessionListener>(weak_from_this());
        room_->AddListener(listener_);
        runner_ = std::thread([this] { io_.run(); });
    }

    void RefreshDevices() {
        const auto generation = generation_.load();
        auto self = shared_from_this();
        asio::post(io_, [self, generation] {
            if (!self->Current(generation)) return;
            UiEvent event{EventKind::Devices, generation};
            auto& manager = livekit::WebRTCManager::Instance();
            for (const auto& device : manager.ListRecordingDevices()) {
                event.microphones.push_back({QString::fromStdString(device.id),
                                             QString::fromStdString(device.name)});
            }
            for (const auto& device : manager.ListPlayoutDevices()) {
                event.speakers.push_back({QString::fromStdString(device.id),
                                          QString::fromStdString(device.name)});
            }
            for (const auto& device : livekit::media::LinuxCameraCapture::Enumerate()) {
                event.cameras.push_back({QString::fromStdString(device.id),
                                         QString::fromStdString(device.name)});
            }
            if (self->camera_device_id_.empty() && !event.cameras.empty()) {
                self->camera_device_id_ = event.cameras.front().id.toStdString();
            }
            event.microphone_selected_id = QString::fromStdString(self->microphone_device_id_);
            event.speaker_selected_id = QString::fromStdString(self->speaker_device_id_);
            event.camera_selected_id = QString::fromStdString(self->camera_device_id_);
            self->Send(std::move(event));
        });
    }

    void SetMicrophone(bool enabled, std::string device_id) {
        const auto generation = generation_.load();
        auto self = shared_from_this();
        asio::post(io_, [self, generation, enabled, device_id = std::move(device_id)] {
            if (!self->Current(generation)) return;
            if (self->reconnecting_ || self->microphone_busy_ ||
                self->microphone_unpublish_uncertain_) {
                self->SendMediaState(EventKind::MicrophoneState, generation,
                    self->microphone_track_ && livekit::WebRTCManager::Instance().RecordingActive(), false,
                    self->microphone_device_id_, self->microphone_unpublish_uncertain_
                        ? QStringLiteral("麦克风发布状态未确认，请离会后重试")
                        : QStringLiteral("麦克风暂不可切换，请稍后重试"));
                return;
            }
            self->microphone_busy_ = true;
            asio::co_spawn(self->io_,
                self->ChangeMicrophone(generation, enabled, device_id),
                [self](std::exception_ptr) {});
        });
    }

    void SetCamera(bool enabled, std::string device_id) {
        const auto generation = generation_.load();
        auto self = shared_from_this();
        asio::post(io_, [self, generation, enabled, device_id = std::move(device_id)] {
            if (!self->Current(generation)) return;
            if (self->reconnecting_ || self->camera_busy_ ||
                self->camera_unpublish_uncertain_) {
                self->SendMediaState(EventKind::CameraState, generation,
                    self->camera_track_ && self->camera_capture_.Running(), false,
                    self->camera_device_id_, self->camera_unpublish_uncertain_
                        ? QStringLiteral("摄像头发布状态未确认，请离会后重试")
                        : QStringLiteral("摄像头暂不可切换，请稍后重试"));
                return;
            }
            self->camera_busy_ = true;
            asio::co_spawn(self->io_,
                self->ChangeCamera(generation, enabled, device_id),
                [self](std::exception_ptr) {});
        });
    }

    void SelectMicrophone(std::string device_id) {
        const auto generation = generation_.load();
        auto self = shared_from_this();
        asio::post(io_, [self, generation, device_id = std::move(device_id)] {
            if (!self->Current(generation)) return;
            if (self->reconnecting_ || self->microphone_busy_ ||
                self->microphone_unpublish_uncertain_) {
                self->SendMediaState(EventKind::MicrophoneState, generation,
                    self->microphone_track_ && livekit::WebRTCManager::Instance().RecordingActive(), false,
                    self->microphone_device_id_, self->microphone_unpublish_uncertain_
                        ? QStringLiteral("麦克风发布状态未确认，请离会后重试")
                        : QStringLiteral("麦克风暂不可切换，请稍后重试"));
                return;
            }
            self->microphone_busy_ = true;
            const bool active = static_cast<bool>(self->microphone_track_);
            auto& manager = livekit::WebRTCManager::Instance();
            bool selected = !active || manager.SetRecordingDeviceById(device_id);
            if (active && selected && !manager.RecordingActive()) {
                selected = manager.EnsureRecording();
            }
            if (selected) self->microphone_device_id_ = device_id;
            if (active && !manager.RecordingActive()) {
                asio::co_spawn(self->io_,
                    self->ChangeMicrophone(generation, false, {},
                        QStringLiteral("麦克风设备已停止录音，轨道已取消发布")),
                    [self](std::exception_ptr) {});
                return;
            }
            self->SendMediaState(EventKind::MicrophoneState, generation, active, selected,
                self->microphone_device_id_, selected ? QString{} : QStringLiteral("麦克风切换失败"));
            self->microphone_busy_ = false;
        });
    }

    void SelectCamera(std::string device_id) {
        const auto generation = generation_.load();
        auto self = shared_from_this();
        asio::post(io_, [self, generation, device_id = std::move(device_id)] {
            if (!self->Current(generation)) return;
            if (self->reconnecting_ || self->camera_busy_ ||
                self->camera_unpublish_uncertain_) {
                self->SendMediaState(EventKind::CameraState, generation,
                    self->camera_track_ && self->camera_capture_.Running(), false,
                    self->camera_device_id_, self->camera_unpublish_uncertain_
                        ? QStringLiteral("摄像头发布状态未确认，请离会后重试")
                        : QStringLiteral("摄像头暂不可切换，请稍后重试"));
                return;
            }
            self->camera_busy_ = true;
            const bool active = static_cast<bool>(self->camera_track_);
            const bool selected = !active || self->camera_capture_.Start(
                device_id, self->camera_source_);
            if (selected) self->camera_device_id_ = device_id;
            if (active && !selected && !self->camera_capture_.Running()) {
                asio::co_spawn(self->io_,
                    self->ChangeCamera(generation, false, {},
                        QStringLiteral("摄像头已停止采集，轨道已取消发布")),
                    [self](std::exception_ptr) {});
                return;
            }
            self->SendMediaState(EventKind::CameraState, generation, active, selected,
                self->camera_device_id_, selected ? QString{} : QStringLiteral("摄像头切换失败"));
            self->camera_busy_ = false;
        });
    }

    void SelectSpeaker(std::string device_id) {
        const auto generation = generation_.load();
        auto self = shared_from_this();
        asio::post(io_, [self, generation, device_id = std::move(device_id)] {
            if (!self->Current(generation)) return;
            if (self->reconnecting_) {
                self->SendMediaState(EventKind::SpeakerState, generation,
                    !self->speaker_muted_, false, self->speaker_device_id_,
                    QStringLiteral("重连期间无法切换扬声器"));
                return;
            }
            const auto result = livekit::WebRTCManager::Instance().SwitchPlayoutDeviceById(device_id);
            if (result.selection == livekit::PlayoutSelection::Requested) {
                self->speaker_device_id_ = device_id;
            } else if (result.selection == livekit::PlayoutSelection::Unknown) {
                self->speaker_device_id_.clear();
            }
            if (self->audio_output_.has_value() || !result.applied) {
                self->audio_output_ = result.playing;
                UiEvent audio{EventKind::AudioOutput, generation};
                audio.audio_available = result.playing;
                self->Send(std::move(audio));
            }
            QString failure_message;
            if (!result.applied) {
                switch (result.failure_stage) {
                case livekit::PlayoutFailureStage::Resolve:
                    failure_message = QStringLiteral("所选扬声器已不可用");
                    break;
                case livekit::PlayoutFailureStage::Stop:
                    failure_message = QStringLiteral("当前扬声器无法停止");
                    break;
                case livekit::PlayoutFailureStage::Select:
                    failure_message = QStringLiteral("所选扬声器无法打开");
                    break;
                case livekit::PlayoutFailureStage::Init:
                case livekit::PlayoutFailureStage::Start:
                case livekit::PlayoutFailureStage::Verify:
                    failure_message = QStringLiteral("所选扬声器播放流无法启动");
                    break;
                case livekit::PlayoutFailureStage::Rollback:
                    failure_message = QStringLiteral("扬声器切换失败，播放输出未能恢复");
                    break;
                case livekit::PlayoutFailureStage::None:
                    failure_message = QStringLiteral("扬声器切换失败，请检查播放设备");
                    break;
                }
            }
            self->SendMediaState(EventKind::SpeakerState, generation, !self->speaker_muted_,
                result.applied, self->speaker_device_id_,
                std::move(failure_message));
        });
    }

    void SetSpeakerEnabled(bool enabled) {
        const auto generation = generation_.load();
        auto self = shared_from_this();
        asio::post(io_, [self, generation, enabled] {
            if (!self->Current(generation)) return;
            if (self->reconnecting_) {
                self->SendMediaState(EventKind::SpeakerState, generation,
                    !self->speaker_muted_, false, self->speaker_device_id_,
                    QStringLiteral("重连期间无法切换扬声器"));
                return;
            }
            self->room_->SetAudioOutputMuted(!enabled);
            self->speaker_muted_ = !enabled;
            self->SendMediaState(EventKind::SpeakerState, generation, enabled, true,
                                 self->speaker_device_id_, {});
        });
    }

    uint64_t Connect(std::string url, std::string token, bool allow_insecure) {
        const auto generation = ++generation_;
        auto self = shared_from_this();
        asio::post(io_, [self, generation, url = std::move(url),
                         token = std::move(token), allow_insecure]() mutable {
            if (!self->Current(generation)) return;
            asio::co_spawn(self->io_,
                self->ConnectOnIo(generation, std::move(url), std::move(token),
                                  allow_insecure),
                [self](std::exception_ptr) {});
        });
        return generation;
    }

    uint64_t Leave() {
        const auto generation = ++generation_;
        auto self = shared_from_this();
        asio::post(io_, [self, generation] {
            self->ClearVideo();
            self->StopLocalMedia();
            self->audio_output_.reset();
            asio::co_spawn(self->io_, self->DisconnectOnIo(generation),
                           [self](std::exception_ptr) {});
        });
        return generation;
    }

    void Shutdown() {
        if (stopping_.exchange(true)) return;
        ++generation_;
        if (runner_.joinable()) {
            auto drained = std::make_shared<std::promise<void>>();
            auto ready = drained->get_future();
            asio::post(io_, [this, drained] {
                ClearVideo();
                StopLocalMedia();
                drained->set_value();
            });
            ready.wait();
            // Retire waits for callbacks; the I/O runner must remain active.
            room_->Retire();
            guard_.reset();
            runner_.join();
        }
        listener_.reset();
        room_.reset();
    }

    void Disconnected() {
        const auto generation = generation_.load();
        auto self = shared_from_this();
        asio::post(io_, [self, generation] {
            if (!self->Current(generation)) return;
            self->ClearVideo();
            self->StopLocalMedia();
            self->audio_output_.reset();
            self->Send({EventKind::Disconnected, generation});
        });
    }

    void Reconnecting(bool reconnected) {
        const auto generation = generation_.load();
        auto self = shared_from_this();
        asio::post(io_, [self, generation, reconnected] {
            if (!self->Current(generation)) return;
            self->reconnecting_ = !reconnected;
            self->audio_output_.reset();
            if (reconnected) {
                self->ClearVideo();
                self->Send({EventKind::RemoteReset, generation});
                for (const auto& [sid, participant] : self->room_->remote_participants()) {
                    self->SendParticipant(participant, generation);
                    for (const auto& [track_sid, publication] : participant->tracks()) {
                        const auto track = publication ? publication->track() : nullptr;
                        if (track && track->kind() == livekit::TrackKind::Video) {
                            self->AddVideoSubscription(track, publication, participant, generation);
                        }
                    }
                }
                if (self->microphone_track_ && !self->microphone_busy_ &&
                    !self->microphone_unpublish_uncertain_) {
                    auto& manager = livekit::WebRTCManager::Instance();
                    if (!manager.RecordingActive() && !manager.EnsureRecording()) {
                        self->microphone_busy_ = true;
                        asio::co_spawn(self->io_,
                            self->ChangeMicrophone(generation, false, {},
                                QStringLiteral("重连后麦克风录音未恢复，轨道已取消发布")),
                            [self](std::exception_ptr) {});
                    }
                }
                if (self->camera_track_ && !self->camera_busy_ &&
                    !self->camera_unpublish_uncertain_ &&
                    !self->camera_capture_.Running()) {
                    self->camera_busy_ = true;
                    asio::co_spawn(self->io_,
                        self->ChangeCamera(generation, false, {},
                            QStringLiteral("重连后摄像头采集未恢复，轨道已取消发布")),
                        [self](std::exception_ptr) {});
                }
                self->CheckAudioOutput(generation);
            }
            self->Send({reconnected ? EventKind::Reconnected : EventKind::Reconnecting,
                        generation});
        });
    }

    void TrackRepublished(const std::string& previous_sid,
                          std::shared_ptr<livekit::TrackPublication> publication) {
        if (!publication) return;
        const auto generation = generation_.load();
        auto self = shared_from_this();
        asio::post(io_, [self, generation, previous_sid, publication = std::move(publication)] {
            if (!self->Current(generation)) return;
            if (self->microphone_sid_ == previous_sid && self->microphone_track_ == publication->track()) {
                self->microphone_sid_ = publication->sid();
            }
            if (self->camera_sid_ == previous_sid && self->camera_track_ == publication->track()) {
                self->camera_sid_ = publication->sid();
            }
        });
    }

    void ParticipantChanged(std::shared_ptr<livekit::RemoteParticipant> participant,
                            bool removed) {
        if (!participant) return;
        const auto generation = generation_.load();
        auto self = shared_from_this();
        asio::post(io_, [self, participant = std::move(participant), generation, removed] {
            if (!self->Current(generation)) return;
            if (removed) {
                const auto sid = QString::fromStdString(participant->sid());
                for (auto it = self->videos_.begin(); it != self->videos_.end();) {
                    if (it->second.participant_sid == sid) {
                        it->second.mailbox->Close();
                        self->Send({EventKind::VideoRemoved, generation, {}, {},
                                    QString::fromStdString(it->first)});
                        it = self->videos_.erase(it);
                    } else {
                        ++it;
                    }
                }
                self->Send({EventKind::ParticipantRemove, generation, sid});
            } else {
                self->SendParticipant(participant, generation);
            }
        });
    }

    void TrackChanged(std::shared_ptr<livekit::Track> track,
                      std::shared_ptr<livekit::TrackPublication> publication,
                      std::shared_ptr<livekit::RemoteParticipant> participant,
                      bool subscribed) {
        if (!publication || !participant) return;
        const auto generation = generation_.load();
        auto self = shared_from_this();
        asio::post(io_, [self, track = std::move(track), publication = std::move(publication),
                         participant = std::move(participant), generation, subscribed] {
            if (!self->Current(generation)) return;
            const auto key = publication->sid();
            if (key.empty()) return;
            if (subscribed && track && track->kind() == livekit::TrackKind::Video) {
                self->AddVideoSubscription(track, publication, participant, generation);
            } else if (!subscribed) {
                if (auto it = self->videos_.find(key); it != self->videos_.end()) {
                    it->second.mailbox->Close();
                    self->videos_.erase(it);
                    self->Send({EventKind::VideoRemoved, generation, {}, {},
                                QString::fromStdString(key)});
                }
            }
            if (subscribed && track && track->kind() == livekit::TrackKind::Audio) {
                self->CheckAudioOutput(generation);
            }
            self->SendParticipant(participant, generation);
        });
    }

    void RefreshParticipant(std::shared_ptr<livekit::Participant> participant) {
        const auto remote = std::dynamic_pointer_cast<livekit::RemoteParticipant>(participant);
        if (remote) ParticipantChanged(remote, false);
    }

private:
    struct VideoSubscription {
        QString participant_sid;
        std::shared_ptr<FrameMailbox> mailbox;
        livekit::Track::I420VideoFrameSubscription subscription;
    };

    void AddVideoSubscription(const std::shared_ptr<livekit::Track>& track,
                              const std::shared_ptr<livekit::TrackPublication>& publication,
                              const std::shared_ptr<livekit::RemoteParticipant>& participant,
                              uint64_t generation) {
        const auto key = publication->sid();
        if (key.empty()) return;
        if (auto old = videos_.find(key); old != videos_.end()) {
            old->second.mailbox->Close();
            videos_.erase(old);
        }
        auto mailbox = std::make_shared<FrameMailbox>();
        auto weak = weak_from_this();
        auto subscription = track->subscribeI420VideoFrames(
            [weak, mailbox, key, generation](Frame frame) {
                if (!frame || !mailbox->Submit(std::move(frame))) return;
                if (auto owner = weak.lock()) owner->SendFrame(mailbox, key, generation);
            });
        videos_.emplace(key, VideoSubscription{
            QString::fromStdString(participant->sid()),
            std::move(mailbox), std::move(subscription)});
        Send({EventKind::VideoAdded, generation,
              QString::fromStdString(participant->sid()),
              QString::fromStdString(participant->identity()),
              QString::fromStdString(key)});
    }

    void SendMediaState(EventKind kind, uint64_t generation, bool enabled,
                        bool success, const std::string& selected_id, QString message) {
        UiEvent event{kind, generation};
        event.enabled = enabled;
        event.success = success;
        event.selected_id = QString::fromStdString(selected_id);
        event.message = std::move(message);
        Send(std::move(event));
    }

    asio::awaitable<void> ChangeMicrophone(uint64_t generation, bool enabled,
                                           std::string device_id, QString stopped_reason = {}) {
        const bool device_lost = !stopped_reason.isEmpty();
        bool success = !device_lost;
        bool local_capture_stopped = true;
        QString message = std::move(stopped_reason);
        auto& manager = livekit::WebRTCManager::Instance();
        try {
            if (enabled && !microphone_track_) {
                const auto local = room_->local_participant();
                const auto factory = manager.factory();
                const auto* signaling = manager.signaling_thread();
                if (!local || !factory || !signaling ||
                    !manager.SetRecordingDeviceById(device_id)) {
                    throw std::runtime_error("microphone unavailable");
                }
                auto track = livekit::LocalAudioTrack::createLocalAudioTrack("microphone", {});
                manager.signaling_thread()->BlockingCall([&] {
                    const auto source = factory->CreateAudioSource(webrtc::AudioOptions{});
                    if (source) track->set_rtc_track(
                        factory->CreateAudioTrack("microphone", source.get()));
                });
                if (!track->rtc_track()) throw std::runtime_error("audio track unavailable");
                const auto publication = co_await local->PublishTrackAsync(track);
                if (!publication) throw std::runtime_error("audio publish failed");
                if (!Current(generation)) {
                    try { co_await local->UnpublishTrackAsync(publication->sid()); }
                    catch (...) {}
                    co_return;
                }
                if (!manager.EnsureRecording()) {
                    try { co_await local->UnpublishTrackAsync(publication->sid()); }
                    catch (...) {}
                    if (Current(generation)) manager.StopRecording();
                    throw std::runtime_error("microphone start failed");
                }
                microphone_track_ = std::move(track);
                microphone_sid_ = publication->sid();
                microphone_device_id_ = device_id;
            } else if (enabled && !manager.RecordingActive()) {
                if (!manager.SetRecordingDeviceById(device_id) || !manager.EnsureRecording()) {
                    throw std::runtime_error("microphone restart failed");
                }
                microphone_device_id_ = device_id;
            } else if (!enabled && microphone_track_) {
                microphone_unpublish_uncertain_ = true;
                microphone_track_->set_muted(true);
                local_capture_stopped = manager.StopRecording();
                const auto local = room_->local_participant();
                if (!local || microphone_sid_.empty())
                    throw std::runtime_error("microphone publication unavailable");
                co_await local->UnpublishTrackAsync(microphone_sid_);
                if (!Current(generation)) {
                    co_return;
                }
                microphone_track_.reset();
                microphone_sid_.clear();
                microphone_unpublish_uncertain_ = false;
                if (!local_capture_stopped) {
                    success = false;
                    message = QStringLiteral("麦克风已取消发布，但录音设备停止失败");
                }
            }
        } catch (...) {
            success = false;
            message = enabled ? QStringLiteral("麦克风启动或发布失败")
                : (local_capture_stopped
                    ? QStringLiteral("麦克风本地录音已停止，取消发布未确认；请离会后重试")
                    : QStringLiteral("麦克风录音停止失败，取消发布未确认；请离会后重试"));
        }
        if (Current(generation)) {
            SendMediaState(EventKind::MicrophoneState, generation,
                microphone_track_ && manager.RecordingActive(), success, microphone_device_id_,
                std::move(message));
        }
        if (Current(generation)) microphone_busy_ = false;
    }

    void StopCameraCapture() {
        camera_capture_.Stop();
        if (preview_mailbox_) preview_mailbox_->Close();
        preview_subscription_.reset();
        preview_mailbox_.reset();
        camera_source_.reset();
    }

    asio::awaitable<void> ChangeCamera(uint64_t generation, bool enabled,
                                       std::string device_id, QString stopped_reason = {}) {
        const bool device_lost = !stopped_reason.isEmpty();
        bool success = !device_lost;
        QString message = std::move(stopped_reason);
        try {
            if (enabled && !camera_track_) {
                const auto local = room_->local_participant();
                if (!local || device_id.empty()) throw std::runtime_error("camera unavailable");
                auto source = std::make_shared<livekit::VideoSource>(640, 480);
                auto mailbox = std::make_shared<FrameMailbox>();
                auto weak = weak_from_this();
                auto subscription = source->subscribe(
                    [weak, mailbox, generation](const livekit::VideoFrame& frame,
                                                const livekit::VideoCaptureOptions& options) {
                        if (frame.type() != livekit::VideoBufferType::I420) return;
                        const auto planes = frame.planeInfos();
                        if (planes.size() != 3) return;
                        auto preview = livekit::render::OwnedI420Frame::CopyFromPlanes(
                            frame.width(), frame.height(),
                            reinterpret_cast<const uint8_t*>(planes[0].data_ptr), planes[0].stride,
                            reinterpret_cast<const uint8_t*>(planes[1].data_ptr), planes[1].stride,
                            reinterpret_cast<const uint8_t*>(planes[2].data_ptr), planes[2].stride,
                            options.timestamp_us, options.rotation);
                        if (!preview || !mailbox->Submit(std::move(preview))) return;
                        if (auto owner = weak.lock()) {
                            owner->SendFrame(mailbox, "local-camera", generation);
                        }
                    });
                if (!camera_capture_.Start(device_id, source)) {
                    mailbox->Close();
                    throw std::runtime_error("camera start failed");
                }
                livekit::VideoPublishOptions options;
                options.simulcast = false;
                auto track = livekit::LocalVideoTrack::createLocalVideoTrack(
                    "camera", source, livekit::TrackSource::Camera, options);
                const auto publication = co_await local->PublishTrackAsync(track);
                if (!publication) throw std::runtime_error("camera publish failed");
                if (!Current(generation)) {
                    camera_capture_.Stop();
                    mailbox->Close();
                    try { co_await local->UnpublishTrackAsync(publication->sid()); }
                    catch (...) {}
                    co_return;
                }
                camera_source_ = std::move(source);
                preview_mailbox_ = std::move(mailbox);
                preview_subscription_ = std::move(subscription);
                camera_track_ = std::move(track);
                camera_sid_ = publication->sid();
                camera_device_id_ = device_id;
                Send({EventKind::VideoAdded, generation, {}, QStringLiteral("本地摄像头"),
                      QStringLiteral("local-camera")});
            } else if (enabled && !camera_capture_.Running()) {
                if (!camera_capture_.Start(device_id, camera_source_)) {
                    throw std::runtime_error("camera restart failed");
                }
                camera_device_id_ = device_id;
            } else if (!enabled && camera_track_) {
                camera_unpublish_uncertain_ = true;
                camera_track_->set_muted(true);
                camera_capture_.Stop();
                if (preview_mailbox_) preview_mailbox_->Close();
                preview_subscription_.reset();
                preview_mailbox_.reset();
                Send({EventKind::VideoRemoved, generation, {}, {},
                      QStringLiteral("local-camera")});
                const auto local = room_->local_participant();
                if (!local || camera_sid_.empty())
                    throw std::runtime_error("camera publication unavailable");
                co_await local->UnpublishTrackAsync(camera_sid_);
                if (!Current(generation)) {
                    co_return;
                }
                StopCameraCapture();
                camera_track_.reset();
                camera_sid_.clear();
                camera_unpublish_uncertain_ = false;
            }
        } catch (...) {
            success = false;
            if (Current(generation) && !camera_track_) StopCameraCapture();
            message = enabled ? QStringLiteral("摄像头启动或发布失败")
                : QStringLiteral("摄像头本地采集已停止，取消发布未确认；请离会后重试");
        }
        if (Current(generation)) {
            SendMediaState(EventKind::CameraState, generation,
                camera_track_ && camera_capture_.Running(), success, camera_device_id_,
                std::move(message));
        }
        if (Current(generation)) camera_busy_ = false;
    }

    void StopLocalMedia() {
        StopCameraCapture();
        camera_track_.reset();
        camera_sid_.clear();
        microphone_track_.reset();
        microphone_sid_.clear();
        livekit::WebRTCManager::Instance().StopRecording();
        room_->SetAudioOutputMuted(false);
        speaker_muted_ = false;
        camera_busy_ = false;
        microphone_busy_ = false;
        camera_unpublish_uncertain_ = false;
        microphone_unpublish_uncertain_ = false;
        reconnecting_ = false;
    }

    asio::awaitable<void> ConnectOnIo(uint64_t generation, std::string url,
                                       std::string token, bool allow_insecure) {
        livekit::SignalOptions options;
        options.auto_subscribe = true;
        options.single_peer_connection = true;
        options.allow_insecure_transport = allow_insecure;
        options.connect_timeout = std::chrono::seconds(15);
        // Full restart may need a second publisher SDP round after Join.
        options.timeouts.negotiation = std::chrono::seconds(25);
        options.timeouts.reconnect_attempt = std::chrono::seconds(35);
        options.timeouts.reconnect_total = std::chrono::seconds(90);
        try {
            co_await room_->ConnectAsync(url, token, options);
            if (!Current(generation)) co_return;
            Send({EventKind::Connected, generation});
            CheckAudioOutput(generation);
            for (const auto& [sid, participant] : room_->remote_participants()) {
                SendParticipant(participant, generation);
            }
        } catch (...) {
            if (Current(generation)) Send({EventKind::ConnectFailed, generation});
        }
    }

    asio::awaitable<void> DisconnectOnIo(uint64_t generation) {
        try {
            co_await room_->DisconnectAsync();
        } catch (...) {
            // The room can already be disconnected after a failed join.
        }
        if (Current(generation)) Send({EventKind::Disconnected, generation});
    }

    bool Current(uint64_t generation) const {
        return !stopping_.load() && generation_.load() == generation;
    }

    void Send(UiEvent event) {
        if (!Current(event.generation)) return;
        QMetaObject::invokeMethod(&receiver_,
            [deliver = deliver_, event = std::move(event)]() mutable {
                deliver(std::move(event));
            }, Qt::QueuedConnection);
    }

    void SendFrame(std::shared_ptr<FrameMailbox> mailbox, const std::string& key,
                   uint64_t generation) {
        if (!Current(generation)) return;
        auto weak = weak_from_this();
        QMetaObject::invokeMethod(&receiver_,
            [weak, mailbox = std::move(mailbox), key = QString::fromStdString(key),
             generation]() mutable {
                const auto frame = mailbox->Take();
                if (auto owner = weak.lock(); owner && frame && owner->Current(generation)) {
                    owner->deliver_({EventKind::FrameReady, generation, {}, {},
                                     std::move(key), 0, 0, std::move(frame)});
                }
            }, Qt::QueuedConnection);
    }

    void SendParticipant(const std::shared_ptr<livekit::RemoteParticipant>& participant,
                         uint64_t generation) {
        if (!participant) return;
        const auto snapshot = participant->SnapshotState();
        UiEvent event{EventKind::ParticipantUpsert, generation};
        event.participant_sid = QString::fromStdString(snapshot.sid);
        event.participant_name = QString::fromStdString(
            snapshot.name.empty() ? snapshot.identity : snapshot.name);
        for (const auto& publication : snapshot.publications) {
            if (!publication.track) continue;
            if (publication.kind == livekit::TrackKind::Audio) ++event.audio_tracks;
            if (publication.kind == livekit::TrackKind::Video) ++event.video_tracks;
        }
        Send(std::move(event));
    }

    void ClearVideo() {
        for (auto& [key, video] : videos_) video.mailbox->Close();
        videos_.clear();
    }

    void CheckAudioOutput(uint64_t generation) {
        if (!Current(generation) || audio_output_ == true) return;
        const bool available = livekit::WebRTCManager::Instance().EnsurePlayout();
        if (audio_output_ == available) return;
        audio_output_ = available;
        UiEvent event{EventKind::AudioOutput, generation};
        event.audio_available = available;
        Send(std::move(event));
    }

    QObject& receiver_;
    std::function<void(UiEvent)> deliver_;
    asio::io_context io_;
    asio::executor_work_guard<asio::io_context::executor_type> guard_;
    std::shared_ptr<livekit::Room> room_;
    std::shared_ptr<SessionListener> listener_;
    std::thread runner_;
    std::atomic<uint64_t> generation_{0};
    std::atomic<bool> stopping_{false};
    std::optional<bool> audio_output_;
    std::unordered_map<std::string, VideoSubscription> videos_;
    livekit::media::LinuxCameraCapture camera_capture_;
    std::shared_ptr<livekit::VideoSource> camera_source_;
    std::shared_ptr<livekit::VideoSource::Subscription> preview_subscription_;
    std::shared_ptr<FrameMailbox> preview_mailbox_;
    std::shared_ptr<livekit::LocalAudioTrack> microphone_track_;
    std::shared_ptr<livekit::LocalVideoTrack> camera_track_;
    std::string microphone_sid_;
    std::string camera_sid_;
    std::string microphone_device_id_;
    std::string speaker_device_id_;
    std::string camera_device_id_;
    bool microphone_busy_ = false;
    bool camera_busy_ = false;
    bool microphone_unpublish_uncertain_ = false;
    bool camera_unpublish_uncertain_ = false;
    bool speaker_muted_ = false;
    bool reconnecting_ = false;
};

void SessionListener::OnDisconnected(livekit::RoomDisconnectReason, const std::string&) {
    if (auto owner = owner_.lock()) owner->Disconnected();
}
void SessionListener::OnReconnecting() {
    if (auto owner = owner_.lock()) owner->Reconnecting(false);
}
void SessionListener::OnReconnected() {
    if (auto owner = owner_.lock()) owner->Reconnecting(true);
}
void SessionListener::OnParticipantConnected(
        std::shared_ptr<livekit::RemoteParticipant> participant) {
    if (auto owner = owner_.lock()) owner->ParticipantChanged(std::move(participant), false);
}
void SessionListener::OnParticipantDisconnected(
        std::shared_ptr<livekit::RemoteParticipant> participant) {
    if (auto owner = owner_.lock()) owner->ParticipantChanged(std::move(participant), true);
}
void SessionListener::OnTrackSubscribed(
        std::shared_ptr<livekit::Track> track,
        std::shared_ptr<livekit::TrackPublication> publication,
        std::shared_ptr<livekit::RemoteParticipant> participant) {
    if (auto owner = owner_.lock()) {
        owner->TrackChanged(std::move(track), std::move(publication),
                            std::move(participant), true);
    }
}
void SessionListener::OnTrackUnsubscribed(
        std::shared_ptr<livekit::Track> track,
        std::shared_ptr<livekit::TrackPublication> publication,
        std::shared_ptr<livekit::RemoteParticipant> participant) {
    if (auto owner = owner_.lock()) {
        owner->TrackChanged(std::move(track), std::move(publication),
                            std::move(participant), false);
    }
}
void SessionListener::OnTrackMuted(std::shared_ptr<livekit::Participant> participant,
                                    std::shared_ptr<livekit::TrackPublication>, bool) {
    if (auto owner = owner_.lock()) owner->RefreshParticipant(std::move(participant));
}
void SessionListener::OnLocalTrackRepublished(
        const std::string& previous_sid,
        std::shared_ptr<livekit::TrackPublication> publication) {
    if (auto owner = owner_.lock()) {
        owner->TrackRepublished(previous_sid, std::move(publication));
    }
}

class VideoTile final : public QWidget {
public:
    explicit VideoTile(QString name, QWidget* parent = nullptr)
        : QWidget(parent), name_(std::move(name)) {
        setMinimumSize(280, 158);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    }

    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return width * 9 / 16; }
    QSize sizeHint() const override { return {480, 270}; }

    void SetFrame(const Frame& frame) {
        image_ = renderer_.Convert(*frame);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), MeetingUI::AppTheme::color(
            MeetingUI::AppTheme::ColorRole::Stage));
        if (!image_.isNull()) {
            const auto fitted = image_.size().scaled(size(), Qt::KeepAspectRatio);
            const QRect target((width() - fitted.width()) / 2,
                               (height() - fitted.height()) / 2,
                               fitted.width(), fitted.height());
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.drawImage(target, image_);
        }
        painter.fillRect(QRect(0, height() - 30, width(), 30), QColor(0, 0, 0, 155));
        painter.setPen(Qt::white);
        painter.drawText(QRect(10, height() - 30, width() - 20, 30),
                         Qt::AlignVCenter | Qt::AlignLeft, name_);
    }

private:
    QString name_;
    QImage image_;
    livekit::render::QtCpuVideoRenderer renderer_;
};

class LinuxWindow final : public QMainWindow {
public:
    LinuxWindow() {
        setWindowTitle("Cohavora - Linux 会议");
        resize(1100, 720);

        auto* root = new QWidget(this);
        auto* layout = new QVBoxLayout(root);
        layout->setContentsMargins(18, 16, 18, 16);
        layout->setSpacing(14);
        auto* header = new QHBoxLayout;
        auto* title = new QLabel("Cohavora", root);
        QFont title_font = title->font();
        title_font.setPointSize(17);
        title_font.setBold(true);
        title->setFont(title_font);
        header->addWidget(title);
        header->addStretch();
        status_ = new QLabel("未连接", root);
        header->addWidget(status_);
        leave_ = new QPushButton("离开", root);
        leave_->setVisible(false);
        header->addWidget(leave_);
        layout->addLayout(header);

        pages_ = new QStackedWidget(root);
        auto* login = new QWidget(pages_);
        auto* login_layout = new QVBoxLayout(login);
        login_layout->addStretch();
        auto* form_title = new QLabel("加入 LiveKit 房间", login);
        QFont heading = form_title->font();
        heading.setPointSize(15);
        heading.setBold(true);
        form_title->setFont(heading);
        login_layout->addWidget(form_title);
        auto* form = new QFormLayout;
        url_ = new QLineEdit(login);
        url_->setPlaceholderText("wss://your-livekit-server");
        url_->setText(qEnvironmentVariable("LIVEKIT_URL"));
        form->addRow("服务器地址", url_);
        token_ = new QLineEdit(login);
        token_->setEchoMode(QLineEdit::Password);
        token_->setText(qEnvironmentVariable("LIVEKIT_TOKEN"));
        form->addRow("访问令牌", token_);
        login_layout->addLayout(form);
        insecure_ = new QCheckBox("允许非加密 ws 连接", login);
        login_layout->addWidget(insecure_);
        join_ = new QPushButton("加入房间", login);
        login_layout->addWidget(join_, 0, Qt::AlignRight);
        login_layout->addStretch();
        pages_->addWidget(login);

        auto* meeting = new QWidget(pages_);
        auto* meeting_layout = new QHBoxLayout(meeting);
        meeting_layout->setContentsMargins(0, 0, 0, 0);
        meeting_layout->setSpacing(16);
        auto* scroll = new QScrollArea(meeting);
        scroll->setWidgetResizable(true);
        video_container_ = new QWidget(scroll);
        video_grid_ = new QGridLayout(video_container_);
        video_grid_->setContentsMargins(0, 0, 0, 0);
        video_grid_->setSpacing(10);
        video_grid_->setColumnStretch(0, 1);
        video_grid_->setColumnStretch(1, 1);
        placeholder_ = new QLabel("等待远端视频", video_container_);
        placeholder_->setAlignment(Qt::AlignCenter);
        video_grid_->addWidget(placeholder_, 0, 0, 1, 2);
        scroll->setWidget(video_container_);
        meeting_layout->addWidget(scroll, 1);
        auto* sidebar = new QWidget(meeting);
        sidebar->setFixedWidth(270);
        auto* side_layout = new QVBoxLayout(sidebar);
        side_layout->setContentsMargins(0, 0, 0, 0);
        side_layout->addWidget(new QLabel("参与者", sidebar));
        participants_ = new QListWidget(sidebar);
        side_layout->addWidget(participants_);
        audio_status_ = new QLabel("音频输出：待检测", sidebar);
        audio_status_->setWordWrap(true);
        side_layout->addWidget(audio_status_);
        auto* devices_title = new QLabel("设备", sidebar);
        QFont device_font = devices_title->font();
        device_font.setBold(true);
        devices_title->setFont(device_font);
        side_layout->addWidget(devices_title);
        microphone_toggle_ = new QCheckBox("麦克风", sidebar);
        side_layout->addWidget(microphone_toggle_);
        microphone_devices_ = new QComboBox(sidebar);
        microphone_devices_->addItem("正在检测设备");
        side_layout->addWidget(microphone_devices_);
        camera_toggle_ = new QCheckBox("摄像头", sidebar);
        side_layout->addWidget(camera_toggle_);
        camera_devices_ = new QComboBox(sidebar);
        camera_devices_->addItem("正在检测设备");
        side_layout->addWidget(camera_devices_);
        speaker_toggle_ = new QCheckBox("播放远端音频", sidebar);
        speaker_toggle_->setChecked(true);
        side_layout->addWidget(speaker_toggle_);
        speaker_devices_ = new QComboBox(sidebar);
        speaker_devices_->addItem("正在检测设备");
        side_layout->addWidget(speaker_devices_);
        refresh_devices_ = new QPushButton("刷新设备", sidebar);
        side_layout->addWidget(refresh_devices_, 0, Qt::AlignRight);
        media_status_ = new QLabel(sidebar);
        media_status_->setWordWrap(true);
        side_layout->addWidget(media_status_);
        UpdateControlAvailability();
        meeting_layout->addWidget(sidebar);
        pages_->addWidget(meeting);
        layout->addWidget(pages_, 1);
        setCentralWidget(root);

        connect(join_, &QPushButton::clicked, this, [this] { Join(); });
        connect(leave_, &QPushButton::clicked, this, [this] { Leave(); });
        connect(token_, &QLineEdit::returnPressed, this, [this] { Join(); });
        connect(microphone_toggle_, &QCheckBox::clicked, this, [this](bool enabled) {
            if (!session_) return;
            microphone_pending_ = true;
            media_status_->setText(enabled ? "正在开启麦克风" : "正在关闭麦克风");
            UpdateControlAvailability();
            session_->SetMicrophone(enabled, microphone_devices_->currentData().toString().toStdString());
        });
        connect(camera_toggle_, &QCheckBox::clicked, this, [this](bool enabled) {
            if (!session_) return;
            camera_pending_ = true;
            media_status_->setText(enabled ? "正在开启摄像头" : "正在关闭摄像头");
            UpdateControlAvailability();
            session_->SetCamera(enabled, camera_devices_->currentData().toString().toStdString());
        });
        connect(speaker_toggle_, &QCheckBox::clicked, this, [this](bool enabled) {
            if (!session_) return;
            speaker_pending_ = true;
            UpdateControlAvailability();
            session_->SetSpeakerEnabled(enabled);
        });
        connect(microphone_devices_, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int index) {
            if (!session_ || index < 0 || !devices_ready_) return;
            microphone_pending_ = true;
            UpdateControlAvailability();
            session_->SelectMicrophone(microphone_devices_->itemData(index).toString().toStdString());
        });
        connect(camera_devices_, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int index) {
            if (!session_ || index < 0 || !devices_ready_) return;
            camera_pending_ = true;
            UpdateControlAvailability();
            session_->SelectCamera(camera_devices_->itemData(index).toString().toStdString());
        });
        connect(speaker_devices_, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int index) {
            if (!session_ || index < 0 || !devices_ready_) return;
            speaker_pending_ = true;
            UpdateControlAvailability();
            session_->SelectSpeaker(speaker_devices_->itemData(index).toString().toStdString());
        });
        connect(refresh_devices_, &QPushButton::clicked, this, [this] {
            if (session_) session_->RefreshDevices();
        });
    }

    void SetSession(std::shared_ptr<NativeSession> session) { session_ = std::move(session); }

    void Handle(UiEvent event) {
        if (event.generation != generation_) return;
        switch (event.kind) {
        case EventKind::Connected:
            status_->setText("已连接");
            pages_->setCurrentIndex(1);
            leave_->setVisible(true);
            leave_->setEnabled(true);
            session_->RefreshDevices();
            break;
        case EventKind::Disconnected:
            ResetRoom();
            status_->setText("已断开");
            pages_->setCurrentIndex(0);
            join_->setEnabled(true);
            leave_->setVisible(false);
            break;
        case EventKind::Reconnecting:
            status_->setText("正在重连");
            reconnecting_ = true;
            break;
        case EventKind::Reconnected:
            status_->setText("已连接");
            reconnecting_ = false;
            microphone_pending_ = false;
            camera_pending_ = false;
            speaker_pending_ = false;
            session_->RefreshDevices();
            break;
        case EventKind::ConnectFailed:
            ResetRoom();
            status_->setText("入会失败，请检查地址、令牌和网络");
            join_->setEnabled(true);
            break;
        case EventKind::ParticipantUpsert:
            participants_by_sid_[event.participant_sid] =
                {event.participant_name, event.audio_tracks, event.video_tracks};
            UpdateParticipants();
            break;
        case EventKind::ParticipantRemove:
            participants_by_sid_.erase(event.participant_sid);
            UpdateParticipants();
            break;
        case EventKind::RemoteReset:
            participants_by_sid_.clear();
            participants_->clear();
            for (auto it = tiles_.begin(); it != tiles_.end();) {
                if (it->first == QStringLiteral("local-camera")) {
                    ++it;
                    continue;
                }
                video_grid_->removeWidget(it->second);
                it->second->deleteLater();
                it = tiles_.erase(it);
            }
            RebuildVideoGrid();
            break;
        case EventKind::VideoAdded:
            if (!tiles_.contains(event.track_sid)) {
                auto* tile = new VideoTile(event.participant_name, video_container_);
                tiles_.emplace(event.track_sid, tile);
                RebuildVideoGrid();
            }
            break;
        case EventKind::VideoRemoved:
            if (auto it = tiles_.find(event.track_sid); it != tiles_.end()) {
                auto* tile = it->second;
                tiles_.erase(it);
                video_grid_->removeWidget(tile);
                tile->deleteLater();
                RebuildVideoGrid();
            }
            break;
        case EventKind::FrameReady:
            if (auto it = tiles_.find(event.track_sid); it != tiles_.end() && event.frame) {
                it->second->SetFrame(event.frame);
            }
            break;
        case EventKind::AudioOutput:
            audio_status_->setText(event.audio_available
                ? "音频输出：已就绪" : "音频输出不可用，请检查播放设备");
            break;
        case EventKind::Devices:
            has_microphones_ = !event.microphones.empty();
            has_cameras_ = !event.cameras.empty();
            has_speakers_ = !event.speakers.empty();
            PopulateDevices(microphone_devices_, event.microphones,
                            event.microphone_selected_id, "无可用麦克风");
            PopulateDevices(camera_devices_, event.cameras,
                            event.camera_selected_id, "无可用摄像头");
            PopulateDevices(speaker_devices_, event.speakers,
                            event.speaker_selected_id, "无可用扬声器");
            devices_ready_ = true;
            if (!has_cameras_) media_status_->setText("未检测到摄像头");
            break;
        case EventKind::MicrophoneState: {
            microphone_pending_ = false;
            const QSignalBlocker blocked(microphone_toggle_);
            microphone_toggle_->setChecked(event.enabled);
            SelectDevice(microphone_devices_, event.selected_id);
            media_status_->setText(event.message.isEmpty()
                ? (event.enabled ? "麦克风已开启" : "麦克风已关闭") : event.message);
            break;
        }
        case EventKind::CameraState: {
            camera_pending_ = false;
            const QSignalBlocker blocked(camera_toggle_);
            camera_toggle_->setChecked(event.enabled);
            SelectDevice(camera_devices_, event.selected_id);
            media_status_->setText(event.message.isEmpty()
                ? (event.enabled ? "摄像头已开启" : "摄像头已关闭") : event.message);
            break;
        }
        case EventKind::SpeakerState: {
            speaker_pending_ = false;
            const QSignalBlocker blocked(speaker_toggle_);
            speaker_toggle_->setChecked(event.enabled);
            SelectDevice(speaker_devices_, event.selected_id);
            if (!event.message.isEmpty()) media_status_->setText(event.message);
            break;
        }
        }
        UpdateControlAvailability();
    }

private:
    struct ParticipantRow {
        QString name;
        int audio_tracks = 0;
        int video_tracks = 0;
    };

    static void SelectDevice(QComboBox* box, const QString& id) {
        const QSignalBlocker blocked(box);
        const int index = box->findData(id);
        if (index >= 0) {
            box->setCurrentIndex(index);
        } else if (!id.isEmpty()) {
            box->insertItem(0, "所选设备已断开", id);
            box->setCurrentIndex(0);
        }
    }

    static void PopulateDevices(QComboBox* box,
                                const std::vector<DeviceChoice>& devices,
                                const QString& selected_id,
                                const QString& empty_text) {
        const QSignalBlocker blocked(box);
        box->clear();
        if (devices.empty()) {
            box->addItem(selected_id.isEmpty() ? empty_text : "所选设备已断开",
                         selected_id);
            return;
        }
        const int label_width = std::max(80, box->width() - 40);
        for (const auto& device : devices) {
            box->addItem(box->fontMetrics().elidedText(
                device.name, Qt::ElideRight, label_width), device.id);
            box->setItemData(box->count() - 1, device.name, Qt::ToolTipRole);
        }
        const int selected = box->findData(selected_id);
        if (selected >= 0) {
            box->setCurrentIndex(selected);
        } else if (!selected_id.isEmpty()) {
            box->insertItem(0, "所选设备已断开", selected_id);
            box->setCurrentIndex(0);
        }
    }

    void UpdateControlAvailability() {
        const bool ready = pages_->currentIndex() == 1 && leave_->isEnabled() &&
                           devices_ready_ && !reconnecting_;
        microphone_toggle_->setEnabled(ready &&
            (has_microphones_ || microphone_toggle_->isChecked()) && !microphone_pending_);
        microphone_devices_->setEnabled(ready && has_microphones_ && !microphone_pending_);
        camera_toggle_->setEnabled(ready &&
            (has_cameras_ || camera_toggle_->isChecked()) && !camera_pending_);
        camera_devices_->setEnabled(ready && has_cameras_ && !camera_pending_);
        speaker_toggle_->setEnabled(ready &&
            (has_speakers_ || speaker_toggle_->isChecked()) && !speaker_pending_);
        speaker_devices_->setEnabled(ready && has_speakers_ && !speaker_pending_);
        refresh_devices_->setEnabled(ready && !microphone_pending_ &&
                                     !camera_pending_ && !speaker_pending_);
    }

    void Join() {
        if (!session_ || !join_->isEnabled()) return;
        const auto url = QUrl(url_->text().trimmed());
        const bool secure = url.scheme() == "wss";
        const bool explicitly_insecure = url.scheme() == "ws" && insecure_->isChecked();
        if ((!secure && !explicitly_insecure) || url.host().isEmpty() ||
            token_->text().trimmed().isEmpty()) {
            status_->setText("请输入有效的 ws/wss 地址和访问令牌");
            return;
        }
        ResetRoom();
        join_->setEnabled(false);
        status_->setText("正在连接");
        generation_ = session_->Connect(url.toString(QUrl::FullyEncoded).toStdString(),
                                        token_->text().toStdString(), explicitly_insecure);
        token_->clear();
    }

    void Leave() {
        if (!session_ || !leave_->isEnabled()) return;
        leave_->setEnabled(false);
        status_->setText("正在离开");
        UpdateControlAvailability();
        generation_ = session_->Leave();
    }

    void ResetRoom() {
        for (auto& [sid, tile] : tiles_) {
            video_grid_->removeWidget(tile);
            tile->deleteLater();
        }
        tiles_.clear();
        participants_by_sid_.clear();
        participants_->clear();
        audio_status_->setText("音频输出：待检测");
        media_status_->clear();
        devices_ready_ = false;
        has_microphones_ = false;
        has_cameras_ = false;
        has_speakers_ = false;
        reconnecting_ = false;
        microphone_pending_ = false;
        camera_pending_ = false;
        speaker_pending_ = false;
        {
            const QSignalBlocker mic_block(microphone_toggle_);
            const QSignalBlocker camera_block(camera_toggle_);
            const QSignalBlocker speaker_block(speaker_toggle_);
            microphone_toggle_->setChecked(false);
            camera_toggle_->setChecked(false);
            speaker_toggle_->setChecked(true);
        }
        PopulateDevices(microphone_devices_, {}, {}, "正在检测设备");
        PopulateDevices(camera_devices_, {}, {}, "正在检测设备");
        PopulateDevices(speaker_devices_, {}, {}, "正在检测设备");
        RebuildVideoGrid();
        UpdateControlAvailability();
    }

    void UpdateParticipants() {
        participants_->clear();
        for (const auto& [sid, row] : participants_by_sid_) {
            auto label = row.name.isEmpty() ? sid : row.name;
            if (row.audio_tracks) label += "  音频";
            if (row.video_tracks) label += "  视频";
            participants_->addItem(label);
        }
    }

    void RebuildVideoGrid() {
        placeholder_->setVisible(tiles_.empty());
        int index = 0;
        for (const auto& [sid, tile] : tiles_) {
            video_grid_->addWidget(tile, index / 2, index % 2);
            ++index;
        }
    }

    std::shared_ptr<NativeSession> session_;
    uint64_t generation_ = 0;
    QLabel* status_ = nullptr;
    QPushButton* join_ = nullptr;
    QPushButton* leave_ = nullptr;
    QLineEdit* url_ = nullptr;
    QLineEdit* token_ = nullptr;
    QCheckBox* insecure_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    QWidget* video_container_ = nullptr;
    QGridLayout* video_grid_ = nullptr;
    QLabel* placeholder_ = nullptr;
    QListWidget* participants_ = nullptr;
    QLabel* audio_status_ = nullptr;
    QLabel* media_status_ = nullptr;
    QCheckBox* microphone_toggle_ = nullptr;
    QCheckBox* camera_toggle_ = nullptr;
    QCheckBox* speaker_toggle_ = nullptr;
    QComboBox* microphone_devices_ = nullptr;
    QComboBox* camera_devices_ = nullptr;
    QComboBox* speaker_devices_ = nullptr;
    QPushButton* refresh_devices_ = nullptr;
    bool devices_ready_ = false;
    bool has_microphones_ = false;
    bool has_cameras_ = false;
    bool has_speakers_ = false;
    bool reconnecting_ = false;
    bool microphone_pending_ = false;
    bool camera_pending_ = false;
    bool speaker_pending_ = false;
    std::map<QString, ParticipantRow> participants_by_sid_;
    std::map<QString, VideoTile*> tiles_;
};

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    MeetingUI::AppTheme::install(app);
    QObject event_receiver;
    LinuxWindow window;
    auto session = std::make_shared<NativeSession>(event_receiver,
        [&window](UiEvent event) { window.Handle(std::move(event)); });
    window.SetSession(session);
    session->Start();
    window.show();
    const int result = app.exec();
    session->Shutdown();
    return result;
}
