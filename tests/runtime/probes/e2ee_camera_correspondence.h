#pragma once
#include "e2ee_frame_signature.h"
#include "src/core/meeting_coordinator.h"
#include "src/render/owned_i420_frame.h"
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <chrono>
#include <deque>
#include <mutex>

// All camera summaries remain in memory and encrypted test data packets.
// Evidence contains match outcomes only; no image or summary is written.
class E2eeCameraCorrespondence final {
    using Signature = e2ee_frame_test::Signature;
    static int64_t now() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    struct Sample { int64_t time; Signature value; };
    struct Receipt { int epoch; int64_t time; Signature value; };
    struct State {
        std::mutex mutex;
        std::deque<Sample> frames;
        std::deque<Receipt> receipts;
        int errors = 0;
        int64_t last = 0;
        void add(const std::optional<Signature>& value) {
            std::lock_guard lock(mutex);
            if (!value) { ++errors; return; }
            const auto time = now();
            if (time - last < 100) return;
            last = time;
            if (frames.size() == 64) frames.pop_front();
            frames.push_back({time, *value});
        }
    };
    struct Listener final : livekit::RoomListener {
        explicit Listener(std::weak_ptr<State> value) : state(std::move(value)) {}
        void OnDataReceived(const std::vector<uint8_t>& bytes, std::shared_ptr<livekit::RemoteParticipant>,
            const std::string& topic) override {
            if (topic != "e2ee.camera.correspondence") return;
            auto owner = state.lock(); if (!owner) return;
            std::lock_guard lock(owner->mutex);
            if (bytes.size() > 2048 || owner->receipts.size() >= 16) { ++owner->errors; return; }
            const auto object = QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char*>(bytes.data()),
                static_cast<int>(bytes.size()))).object();
            const auto cells = object.value("cells").toArray();
            const int epoch = object.value("epoch").toInt();
            if (cells.size() != 64 || (epoch != 4 && epoch != 6)) { ++owner->errors; return; }
            Signature value{};
            for (int i = 0; i < 64; ++i) {
                if (!cells[i].isDouble() || cells[i].toInt(-1) < 0 || cells[i].toInt(-1) > 255 || cells[i].toDouble() != cells[i].toInt(-1)) {
                    ++owner->errors; return;
                }
                value[i] = cells[i].toInt();
            }
            owner->receipts.push_back({epoch, now(), value});
        }
        std::weak_ptr<State> state;
    };
public:
    using Emit = std::function<void(const char*, QJsonObject)>;
    E2eeCameraCorrespondence(OpenMeeting::MeetingCoordinator& owner, Emit callback)
        : owner_(owner), emit_(std::move(callback)) {}
    void tick() {
        if (qEnvironmentVariable("E2EE_CAMERA_CONTENT") != "1") return;
        auto room = owner_.room(); if (!room) return;
        if (room_.lock() != room) {
            room_ = room;
            room->AddListener(std::make_shared<Listener>(state_));
        }
        if (qEnvironmentVariable("E2EE_PRODUCT_CAMERA") == "1") {
            auto source = owner_.localVideoSource();
            if (source && source_.lock() != source) {
                source_ = source;
                subscription_ = source->subscribe([weak = std::weak_ptr<State>(state_)](
                    const livekit::VideoFrame& frame, const livekit::VideoCaptureOptions&) {
                    if (auto state = weak.lock()) {
                        if (frame.type() == livekit::VideoBufferType::I420 || frame.type() == livekit::VideoBufferType::NV12)
                            state->add(e2ee_frame_test::SampleLuma(frame.data(), frame.width(), frame.width(), frame.height()));
                        else if (frame.type() == livekit::VideoBufferType::BGRA || frame.type() == livekit::VideoBufferType::RGBA)
                            state->add(e2ee_frame_test::Sample(frame.width(), frame.height(), [&](int x, int y) {
                                const auto* p = frame.data() + (y * frame.width() + x) * 4;
                                const int r = p[frame.type() == livekit::VideoBufferType::RGBA ? 0 : 2];
                                const int b = p[frame.type() == livekit::VideoBufferType::RGBA ? 2 : 0];
                                return ((66 * r + 129 * p[1] + 25 * b + 128) >> 8) + 16;
                            }));
                        else { std::lock_guard lock(state->mutex); ++state->errors; }
                    }
                });
            }
        } else {
            for (const auto& [_, participant] : room->remote_participants()) for (const auto& [sid, publication] : participant->tracks()) {
                const auto snapshot = publication->SnapshotState();
                if (!snapshot.track || snapshot.source != livekit::TrackSource::Camera ||
                    snapshot.track->kind() != livekit::TrackKind::Video || track_.lock() == snapshot.track) continue;
                track_ = snapshot.track;
                videoSubscription_ = snapshot.track->subscribeI420VideoFrames([weak = std::weak_ptr<State>(state_)](auto frame) {
                    if (auto state = weak.lock()) state->add(e2ee_frame_test::SampleLuma(
                        frame->data_y(), frame->stride_y(), frame->width(), frame->height()));
                });
            }
        }
        std::vector<QJsonObject> outcomes;
        {
            std::lock_guard lock(state_->mutex);
            const auto time = now();
            for (auto it = state_->receipts.begin(); it != state_->receipts.end();) {
                double best = 256;
                for (const auto& sample : state_->frames) {
                    if (std::abs(sample.time - it->time) <= 5000 && e2ee_frame_test::HasContrast(sample.value) &&
                        e2ee_frame_test::HasContrast(it->value))
                        best = std::min(best, e2ee_frame_test::MeanError(sample.value, it->value));
                }
                if (best <= 8.0 || time - it->time >= 5000) {
                    outcomes.push_back({{"epoch", it->epoch}, {"valid", best <= 8.0}, {"mean_luma_error", best}});
                    it = state_->receipts.erase(it);
                } else ++it;
            }
        }
        for (const auto& outcome : outcomes) emit_("product_camera_correspondence", outcome);
    }
    QJsonObject finish() {
        subscription_.reset(); videoSubscription_.reset();
        std::lock_guard lock(state_->mutex);
        const QJsonObject result{{"errors", state_->errors}, {"pending", static_cast<int>(state_->receipts.size())}};
        state_->frames.clear(); state_->receipts.clear();
        return result;
    }
private:
    OpenMeeting::MeetingCoordinator& owner_;
    Emit emit_;
    std::shared_ptr<State> state_ = std::make_shared<State>();
    std::weak_ptr<livekit::Room> room_;
    std::weak_ptr<livekit::VideoSource> source_;
    std::weak_ptr<livekit::Track> track_;
    std::shared_ptr<livekit::VideoSource::Subscription> subscription_;
    livekit::Track::I420VideoFrameSubscription videoSubscription_;
};
