#pragma once

#include "src/core/meeting_coordinator.h"
#include "src/ui/meeting_log_console.h"
#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>
#include <QtCore/QDateTime>
#include <QtCore/QJsonDocument>
#include <atomic>
#include <map>
#include <array>
#include <chrono>

// Runtime-only controls use the same Coordinator APIs as the product UI.
// Audio callbacks retain counters only; no PCM is saved or logged.
class E2eeProductLifecycle final {
public:
    using Emit = std::function<void(const char*, QJsonObject)>;
    E2eeProductLifecycle(OpenMeeting::MeetingCoordinator& owner, Emit callback)
        : owner_(owner), emit_(std::move(callback)) {}
    void setSource(const livekit::DesktopSource& source) { source_ = source; }
    bool partialAssetReceived() const { return partialAsset_->load(); }
    QJsonObject retirementEvidence() const {
        const auto alive = [](const auto& entries) {
            int count = 0;
            for (const auto& [_, weak] : entries) if (!weak.expired()) ++count;
            return count;
        };
        return {{"rooms_observed", static_cast<int>(rooms_.size())}, {"rooms_alive", alive(rooms_)},
            {"managers_observed", static_cast<int>(managers_.size())}, {"managers_alive", alive(managers_)},
            {"media_observations_observed", static_cast<int>(observations_.size())},
            {"media_observations_alive", alive(observations_)}};
    }
    void tick(qint64 now) {
        const auto room = owner_.room();
        if (!room) return;
        // Weak evidence must not extend the lifetime being measured.
        rooms_[room.get()] = room;
        if (auto manager = room->e2ee_manager()) {
            managers_[manager.get()] = manager;
            for (const auto& observation : manager->media_observations())
                observations_[observation.get()] = observation;
        }
        if (qEnvironmentVariable("E2EE_PRODUCT_BOARD_PARTIAL") == "1" && boardRoom_.lock() != room) {
            boardRoom_ = room;
            room->AddListener(std::make_shared<BoardReceipt>(partialAsset_, oldTail_));
        }
        if (partialAssetReceived() && !partialReported_) {
            partialReported_ = true;
            emit_("product_board_partial_received", {});
        }
        if (oldTail_->load() && !tailReported_) {
            tailReported_ = true;
            emit_("product_board_old_tail_received", {});
        }
        if (qEnvironmentVariable("E2EE_PRODUCT_LIFECYCLE") == "1" && loggedRoom_.lock() != room) {
            loggedRoom_ = room;
            room->SetLogHandler([queue = logs_](const std::string& cat, const std::string& tag, const std::string& message) {
                const auto category = cat == "ERROR" ? MeetingUI::LogCategory::Error :
                    cat == "SIGNAL" ? MeetingUI::LogCategory::Signal : cat == "WEBRTC" ? MeetingUI::LogCategory::WebRTC :
                    cat == "TRACK" ? MeetingUI::LogCategory::Track : cat == "MEDIA" ? MeetingUI::LogCategory::Media : MeetingUI::LogCategory::General;
                MeetingUI::LogToConsole(category, QString::fromStdString(tag), QString::fromStdString(message));
                if (tag.empty() || tag.size() > 64 || !std::all_of(tag.begin(), tag.end(),
                    [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; })) return;
                std::lock_guard lock(queue->mutex);
                if (queue->entries.size() < 2048) queue->entries.append(QJsonObject{
                    {"tag", QString::fromStdString(tag)}, {"time_ms", QDateTime::currentMSecsSinceEpoch()}});
            });
        }
        if (owner_.state() != OpenMeeting::MeetingState::InMeeting) return;
        for (const auto& [identity, participant] : room->remote_participants()) {
            for (const auto& [sid, publication] : participant->tracks()) {
                const auto track = publication->SnapshotState().track;
                if (!track || track->kind() != livekit::TrackKind::Audio) continue;
                auto& record = audio_[sid];
                if (record.track.lock() != track) {
                    record.track = track; record.counters = std::make_shared<AudioCounters>();
                    std::weak_ptr<AudioCounters> weak = record.counters;
                    track->addAudioSink([weak](const livekit::AudioFrame& frame) {
                        if (auto count = weak.lock()) {
                            count->samples.fetch_add(frame.totalSamples(), std::memory_order_relaxed);
                            count->frames.fetch_add(1, std::memory_order_relaxed);
                            if (frame.sampleRate() <= 0 || frame.numChannels() <= 0 || frame.samplesPerChannel() <= 0 ||
                                frame.totalSamples() != static_cast<size_t>(frame.samplesPerChannel()) * frame.numChannels())
                                count->invalidFrames.fetch_add(1, std::memory_order_relaxed);
                            const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count();
                            const auto previous = count->lastMicros.exchange(micros, std::memory_order_relaxed);
                            if (previous > 0) {
                                if (micros < previous) count->clockOrderErrors.fetch_add(1, std::memory_order_relaxed);
                                else {
                                    const auto gap = micros - previous;
                                    size_t bin = 0;
                                    while (bin < kGapBounds.size() && gap > kGapBounds[bin]) ++bin;
                                    count->gaps[bin].fetch_add(1, std::memory_order_relaxed);
                                }
                            }
                        }
                    });
                }
            }
        }
        if (now >= nextSample_) {
            nextSample_ = now + 1000;
            if (qEnvironmentVariable("E2EE_PRODUCT_LIFECYCLE") == "1") {
                QJsonArray tags;
                { std::lock_guard lock(logs_->mutex); std::swap(tags, logs_->entries); }
                if (!tags.isEmpty()) emit_("product_log_tags", {{"entries", tags}});
            }
            for (auto it = audio_.begin(); it != audio_.end();) {
                if (it->second.track.expired()) { it = audio_.erase(it); continue; }
                const auto counters = it->second.counters;
                QJsonArray gaps;
                for (const auto& value : counters->gaps) gaps.append(static_cast<qint64>(value.load(std::memory_order_relaxed)));
                emit_("product_pcm", {{"track", QString::fromStdString(it->first)},
                    {"samples", static_cast<qint64>(counters->samples.load(std::memory_order_relaxed))},
                    {"frames", static_cast<qint64>(counters->frames.load(std::memory_order_relaxed))},
                    {"invalid_frames", static_cast<qint64>(counters->invalidFrames.load(std::memory_order_relaxed))},
                    {"clock_order_errors", static_cast<qint64>(counters->clockOrderErrors.load(std::memory_order_relaxed))},
                    {"gap_bins_us", gaps}});
                ++it;
            }
        }
        if (qEnvironmentVariable("E2EE_PRODUCT_LIFECYCLE") != "1") return;
        const auto elapsed = now - qEnvironmentVariable("E2EE_LIFECYCLE_START_MS").toLongLong();
        if (!muted_ && elapsed >= 10000) {
            muted_ = true; owner_.setLocalAudioMuted(true);
            emit_("product_audio_control", {{"muted", true}});
        }
        if (muted_ && !unmuted_ && elapsed >= 15000) {
            unmuted_ = true; owner_.setLocalAudioMuted(false);
            emit_("product_audio_control", {{"muted", false}});
        }
        if (!source_ || cycles_ >= 3) return;
        const auto snapshot = owner_.screenShareSnapshot();
        if (!stopping_ && snapshot.state == livekit::ScreenShareState::Active && elapsed >= 25000 + cycles_ * 20000) {
            stopping_ = true;
            emit_("product_share_stop", {{"cycle", cycles_ + 1}});
            owner_.stopScreenShare();
        }
        if (stopping_ && snapshot.state == livekit::ScreenShareState::Idle) {
            const auto manager = room->e2ee_manager();
            if (!manager) return;
            const auto status = manager->ReadMediaStatus(0);
            if (std::any_of(status.tracks.begin(), status.tracks.end(),
                [](const auto& t) { return !t.receiving && t.video; })) return;
            ++cycles_; stopping_ = false;
            emit_("product_share_retired", {{"cycle", cycles_}, {"sender_video_bindings", 0}});
            owner_.startScreenShare(*source_, 15);
            emit_("product_share_restart", {{"cycle", cycles_}});
        }
    }
private:
    struct BoardReceipt final : livekit::RoomListener {
        BoardReceipt(std::shared_ptr<std::atomic_bool> received, std::shared_ptr<std::atomic_bool> tail)
            : received_(std::move(received)), tail_(std::move(tail)) {}
        void OnDataReceived(const std::vector<uint8_t>& bytes, std::shared_ptr<livekit::RemoteParticipant> participant,
            const std::string& topic) override {
            if (!participant || participant->identity() != "receiver" || topic != "whiteboard.v1.asset" || bytes.size() > 12288) return;
            const auto object = QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()))).object();
            if (object.value("kind").toString() != "asset_chunk" || object.value("chunkCount").toInt() != 2) return;
            if (object.value("chunkIndex").toInt() == 0) received_->store(true);
            if (object.value("chunkIndex").toInt() == 1) tail_->store(true);
        }
        std::shared_ptr<std::atomic_bool> received_;
        std::shared_ptr<std::atomic_bool> tail_;
    };
    std::shared_ptr<std::atomic_bool> partialAsset_ = std::make_shared<std::atomic_bool>(false);
    std::shared_ptr<std::atomic_bool> oldTail_ = std::make_shared<std::atomic_bool>(false);
    std::weak_ptr<livekit::Room> boardRoom_;
    bool partialReported_ = false, tailReported_ = false;
    inline static constexpr std::array<int64_t, 9> kGapBounds{5000, 10000, 15000, 20000, 30000, 50000, 100000, 250000, 1000000};
    struct AudioCounters {
        std::atomic<uint64_t> samples{0}, frames{0}, invalidFrames{0}, clockOrderErrors{0};
        std::atomic<int64_t> lastMicros{0};
        std::array<std::atomic<uint64_t>, kGapBounds.size() + 1> gaps{};
    };
    struct AudioRecord {
        std::weak_ptr<livekit::Track> track;
        std::shared_ptr<AudioCounters> counters;
    };
    std::map<const void*, std::weak_ptr<livekit::Room>> rooms_;
    std::map<const void*, std::weak_ptr<livekit::E2eeManager>> managers_;
    std::map<const void*, std::weak_ptr<const livekit::E2eeManager::MediaObservation>> observations_;
    OpenMeeting::MeetingCoordinator& owner_;
    Emit emit_;
    std::map<std::string, AudioRecord> audio_;
    std::optional<livekit::DesktopSource> source_;
    qint64 nextSample_ = 0;
    struct LogQueue { std::mutex mutex; QJsonArray entries; };
    std::shared_ptr<LogQueue> logs_ = std::make_shared<LogQueue>();
    std::weak_ptr<livekit::Room> loggedRoom_;
    bool muted_ = false, unmuted_ = false, stopping_ = false;
    int cycles_ = 0;
};
