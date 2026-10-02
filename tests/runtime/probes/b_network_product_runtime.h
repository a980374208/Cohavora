#pragma once
#include "src/core/meeting_coordinator.h"
#include "src/ui/meeting_room_window.h"
#include "src/render/owned_i420_frame.h"
#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QTimer>
#include <atomic>
#include <map>

// Read production UI state on its Qt owner. No business state is injected.
class ParticipantWindowTestAccess final {
public:
    static QJsonObject networkState(const MeetingUI::MeetingRoomWindow &window) {
        QJsonObject state{{"visible",window.isVisible()}, {"preparing_media",window._preparingMedia}};
        if (const auto *banner=window._recoveryBanner) {
            state.insert("banner_visible",banner->isVisible());
            state.insert("reconnecting_banner",banner->text()==QCoreApplication::translate(
                "MeetingUI","⚠️ Connection interrupted. Reconnecting to the meeting..."));
        }
        if (window._remoteRenderSession) {
            const auto statistics=window._remoteRenderSession->statistics();
            state.insert("render_delivered",static_cast<qint64>(statistics.delivered_to_gpu+statistics.delivered_to_qt_cpu));
            state.insert("render_attached",static_cast<int>(statistics.attached_track_count));
        }
        state.insert("render_backend",livekit::render::RenderBackendName(window.renderDiagnostics().actual_backend));
        return state;
    }
};

class BNetworkProductObserver final {
public:
    using Emit=std::function<void(const char *,QJsonObject)>;
    BNetworkProductObserver(std::shared_ptr<OpenMeeting::MeetingCoordinator> owner,
                            bool generatedVideo, bool showUi, Emit record)
        : owner_(std::move(owner)), record_(std::move(record)), generatedVideo_(generatedVideo) {
        if (showUi) openUi();
        QObject::connect(owner_.get(),&OpenMeeting::MeetingCoordinator::remoteVideoTrackAvailable,
            owner_.get(),[this](const QString &,std::shared_ptr<livekit::Track> track) {
                const auto id=track->sid();
                // A signal resume can replace the Track object while keeping
                // its publication SID. Subscribe to the current object and
                // retain the SID's cumulative counter across the rebind.
                subscriptions_.erase(id);
                auto &counter=counters_[id];
                if (!counter) counter=std::make_shared<Counter>();
                subscriptions_.emplace(id,track->subscribeI420VideoFrames([counter](auto frame) {
                    if (!frame) return;
                    counter->width.store(frame->width(),std::memory_order_relaxed);
                    counter->height.store(frame->height(),std::memory_order_relaxed);
                    counter->frames.fetch_add(1,std::memory_order_relaxed);
                }));
            });
        QObject::connect(owner_.get(),&OpenMeeting::MeetingCoordinator::remoteVideoTrackUnavailable,
            owner_.get(),[this](const QString &,const QString &id) { subscriptions_.erase(id.toStdString()); });
        QObject::connect(owner_.get(),&OpenMeeting::MeetingCoordinator::telemetrySnapshotChanged,
            owner_.get(),[this](const QVariantMap &values) { telemetry_=values; });
        QObject::connect(owner_.get(),&OpenMeeting::MeetingCoordinator::stateChanged,
            owner_.get(),[this](OpenMeeting::MeetingState state,const QString &) {
                if (window_) QTimer::singleShot(0,owner_.get(),[this,state] {
                    auto value=ParticipantWindowTestAccess::networkState(*window_);
                    value.insert("state",static_cast<int>(state));
                    record_("network_ui_state",value);
                });
            });
        // Match production's prepare-media-before-admission ordering. Without
        // this real frame, publication resolves the source's initial 1280x720
        // presets although the fixture subsequently delivers 640x360 frames.
        if (generatedVideo_) {
            auto initial=livekit::VideoFrame::create(640,360,livekit::VideoBufferType::I420);
            std::fill(initial.data(),initial.data()+initial.dataSize(),128);
            owner_->localVideoSource()->captureFrame(initial);
        }
        producer_.setInterval(50);
        QObject::connect(&producer_,&QTimer::timeout,owner_.get(),[this] {
            if (!generatedVideo_ || owner_->state()!=OpenMeeting::MeetingState::InMeeting) return;
            auto source=owner_->localVideoSource();
            if (!source) return;
            auto frame=livekit::VideoFrame::create(640,360,livekit::VideoBufferType::I420);
            std::fill(frame.data(),frame.data()+frame.dataSize(),128);
            ++produced_;
            for (int y=0;y<360;++y) for (int x=0;x<640;++x)
                frame.data()[y*640+x]=static_cast<std::uint8_t>(32+((x*11+y*7+produced_*13)%190));
            source->captureFrame(frame);
        });
        if (generatedVideo_) producer_.start();
    }
    ~BNetworkProductObserver() {
        producer_.stop();
        subscriptions_.clear();
        window_.reset();
    }
    void prepareJoin(std::function<void()> admission) {
        if (window_ && !prepared_) {
            prepared_=true;
            window_->prepareMediaAndJoin(std::move(admission));
        } else admission();
    }
    bool handle(const QJsonObject &command) {
        const auto action=command.value("action").toString();
        if (action=="network_reopen_ui") {
            window_.reset();prepared_=false;openUi();
            return true;
        }
        if (action=="network_open_ui") {
            if (window_) record_("unknown_command",{});
            else openUi();
            return true;
        }
        if (action=="network_path_switch") {
            if (auto room=owner_->room()) room->SimulateScenario(livekit::SimulateScenarioType::SwitchCandidate);
            else record_("unknown_command",{});
            return true;
        }
        if (action=="network_reconnect" || action=="network_full_reconnect") {
            if (auto room=owner_->room()) room->SimulateScenario(action=="network_full_reconnect"
                ? livekit::SimulateScenarioType::FullReconnect : livekit::SimulateScenarioType::SignalReconnect);
            else record_("unknown_command",{});
            return true;
        }
        if (action!="network_observe") return false;
        QJsonArray tracks;
        std::uint64_t frames=0;
        for (const auto &[id,counter]:counters_) {
            const auto count=counter->frames.load(std::memory_order_relaxed);
            frames+=count;
            tracks.append(QJsonObject{{"sid",QString::fromStdString(id)}, {"frames",static_cast<qint64>(count)},
                {"width",counter->width.load(std::memory_order_relaxed)},
                {"height",counter->height.load(std::memory_order_relaxed)}, {"attached",subscriptions_.contains(id)}});
        }
        QJsonObject value{{"sequence",command.value("sequence")}, {"state",static_cast<int>(owner_->state())},
            {"decoded_frames",static_cast<qint64>(frames)}, {"produced_frames",static_cast<qint64>(produced_)}, {"tracks",tracks}};
        const QStringList keys={"inboundRtpTrafficAvailability","inboundRtpBitrateBps","outboundRtpTrafficAvailability",
            "outboundRtpBitrateBps","windowInboundRtpBytes","windowOutboundRtpBytes","inboundPacketsLost",
            "inboundPacketLossRatio","inboundJitterAvailability","inboundJitterMaxMs","selectedCandidatePairCount",
            "selectedMediaTransports","mediaProtocols","relayProtocols","mediaPathSwitches",
            "localCandidateTypes","remoteCandidateTypes","inboundVideoFramesDecoded",
            "outboundVideoFramesEncoded","inboundVideoCodecs","inboundVideoFps","outboundVideoFps"};
        QJsonObject metrics;
        for (const auto &key:keys) if (telemetry_.contains(key)) metrics.insert(key,QJsonValue::fromVariant(telemetry_[key]));
        value.insert("telemetry",metrics);
        if (window_) value.insert("ui",ParticipantWindowTestAccess::networkState(*window_));
        record_("network_observation",value);
        return true;
    }
private:
    void openUi() {
        MeetingUI::MeetingRoomWindow::Config config;
        config.meetingId=qEnvironmentVariable("B_FILE_ROOM");
        config.displayName="B10 recovery receiver";
        config.audioMuted=true;
        config.videoEnabled=false;
        window_=std::make_unique<MeetingUI::MeetingRoomWindow>(config,owner_);
        window_->show();
    }
    struct Counter { std::atomic<std::uint64_t> frames{0}; std::atomic<int> width{0},height{0}; };
    std::shared_ptr<OpenMeeting::MeetingCoordinator> owner_;
    Emit record_;
    bool generatedVideo_=false,prepared_=false;
    QTimer producer_;
    std::uint64_t produced_=0;
    std::unique_ptr<MeetingUI::MeetingRoomWindow> window_;
    std::map<std::string,std::shared_ptr<Counter>> counters_;
    std::map<std::string,livekit::Track::I420VideoFrameSubscription> subscriptions_;
    QVariantMap telemetry_;
};
