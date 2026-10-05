#include "src/core/meeting_coordinator.h"
#include "src/platform/win/remote_input.h"
#include <QtCore/QUuid>

namespace OpenMeeting {
namespace rc = livekit::remote_control;
void MeetingCoordinator::configureRemoteControlRuntimeOnUiThread() {
    auto session = _sessionRuntime;
    if (!session || !_room) return;
    auto input = rc::CreateWindowsInputBackend();
    _remoteInput = input;
    session->post([session, gate = _sessionUiGate, weakRoom = std::weak_ptr<livekit::Room>(_room), input] {
        if (!session->acceptsDataOnStrand()) return;
        rc::Runtime::Hooks hooks;
        hooks.input = input;
        hooks.newId = [] { return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(); };
        hooks.surface = [weak = std::weak_ptr<MeetingSessionRuntime>(session), weakRoom]() -> std::optional<rc::Surface> {
            const auto owner = weak.lock(); const auto room = weakRoom.lock();
            if (!owner || !room || room->connection_state() != livekit::ConnectionState::Connected) return {};
            const auto share = owner->screenShareOnStrand();
            if (!share) return {};
            const auto s = share->snapshot();
            if (s.state != livekit::ScreenShareState::Active || !s.annotation_binding || s.track_sid.empty()) return {};
            // This is also checked immediately before consent and input, not
            // only by the screen-share session's periodic geometry check.
            if (!livekit::ValidateScreenBinding(*s.annotation_binding)) return {};
            return rc::Surface{s.track_sid,*s.annotation_binding};
        };
        hooks.send = [weakRoom](std::string_view identity, std::string_view wire) {
            const auto room = weakRoom.lock();
            if (!room || identity.empty() || room->GetDataChannelBufferedAmount(true) > 16*1024) return false;
            livekit::proto::DataPacket packet;
            auto* user = packet.mutable_user();
            user->set_topic(rc::Topic.data(),rc::Topic.size());
            user->set_payload(wire.data(),wire.size());
            user->add_destination_identities(identity.data(),identity.size());
            packet.add_destination_identities(identity.data(),identity.size());
            return room->PublishDataPacket(packet,true);
        };
        hooks.project = [gate,generation = session->generation()](rc::Projection p) {
            gate->Post([generation,p = std::move(p)](MeetingCoordinator* self) {
                if (self->isCurrentSessionGenerationOnUiThread(generation)) emit self->remoteControlChanged(p);
            });
        };
        session->remoteControlOnStrand() = std::make_shared<rc::Runtime>(std::move(hooks));
        tickRemoteControl(session);
    });
}
void MeetingCoordinator::tickRemoteControl(const std::shared_ptr<MeetingSessionRuntime>& session) {
    if (!session->acceptsDataOnStrand() || !session->remoteControlOnStrand()) return;
    if (session->remoteControlOverflowed()) {
        session->remoteControlOnStrand()->stop(rc::Reason::Transport);
        session->clearRemoteControlOverflowOnStrand();
    }
    session->remoteControlOnStrand()->tick(rc::NowMs());
    auto& timer = session->remoteControlTimerOnStrand();
    timer.expires_after(std::chrono::milliseconds(50));
    timer.async_wait([weak = std::weak_ptr<MeetingSessionRuntime>(session)](const std::error_code& error) {
        if (!error) if (auto current = weak.lock()) tickRemoteControl(current);
    });
}
void MeetingCoordinator::enqueueRemoteControlData(const std::shared_ptr<MeetingSessionRuntime>& session,
        const std::vector<uint8_t>& data, const livekit::SenderContext& sender) {
    if (!session || data.size() > rc::MaxWireBytes || sender.origin != livekit::SenderOrigin::Remote) return;
    const auto epoch = session->remoteControlEpoch();
    session->postRemoteControl([session,data,sender,epoch] {
        if (!session->acceptsDataOnStrand()) return;
        auto check = [sender,epoch,weak = std::weak_ptr<MeetingSessionRuntime>(session)] {
            const auto current = weak.lock();
            return current && current->remoteControlEpoch() == epoch &&
                !current->remoteControlOverflowed() && sender.encryptionCurrent() &&
                livekit::IsParticipantTicketActive(sender.ticket,sender.key);
        };
        if (!check()) return;
        const auto message = rc::Decode(std::string_view(reinterpret_cast<const char*>(data.data()),data.size()));
        if (message) if (auto runtime = session->remoteControlOnStrand())
            runtime->receive(*message,{sender.key.identity,sender.key.native_room_generation,sender.key.incarnation},
                check,rc::NowMs());
    });
}
void MeetingCoordinator::requestRemoteControl(const QString& identity, const QString& trackSid,
        std::shared_ptr<rc::Lease> frontendLease) {
    if (_state != MeetingState::InMeeting || !_sessionRuntime || !frontendLease ||
        !frontendLease->permits(rc::NowMs())) return;
    for (const auto& presentation : participantPresentations()) {
        if (presentation.participant.identity != identity || presentation.participant.isLocal) continue;
        for (const auto& track : presentation.videoTracks) {
            if (QString::fromStdString(track.key.publication_sid) != trackSid ||
                !isParticipantPresentationCurrent(presentation,&track) ||
                !track.track || track.track->source() != livekit::TrackSource::ScreenShareVideo) continue;
            const auto key = track.key;
            auto session = _sessionRuntime;
            auto check = [ticket = presentation.participant.participantTicket,key,trackTicket = track.ticket,frontendLease,
                          weak = std::weak_ptr<MeetingSessionRuntime>(session),epoch = session->remoteControlEpoch(),
                          weakRoom = std::weak_ptr<livekit::Room>(_room)] {
                const auto room = weakRoom.lock();
                const auto owner = weak.lock();
                return frontendLease->permits(rc::NowMs()) && owner &&
                    owner->remoteControlEpoch() == epoch && !owner->remoteControlOverflowed() &&
                    room && room->connection_state() == livekit::ConnectionState::Connected &&
                    livekit::IsParticipantTicketActive(ticket,key.participant) && livekit::IsTrackTicketActive(trackTicket,key);
            };
            session->post([session,key,check] {
                if (!session->acceptsDataOnStrand()) return;
                if (auto runtime = session->remoteControlOnStrand()) runtime->request(
                    {key.participant.identity,key.participant.native_room_generation,key.participant.incarnation},
                    check,key.publication_sid,rc::NowMs());
            });
            return;
        }
    }
}
void MeetingCoordinator::respondRemoteControl(const QString& requestId, bool allow) {
    auto session = _sessionRuntime;
    if (!session || _state != MeetingState::InMeeting) return;
    session->post([session,id = requestId.toStdString(),allow] {
        if (auto runtime = session->remoteControlOnStrand()) runtime->consent(id,allow,rc::NowMs());
    });
}
void MeetingCoordinator::activateRemoteControl(std::string grant) {
    auto session = _sessionRuntime;
    if (!session || _state != MeetingState::InMeeting) return;
    session->post([session,grant = std::move(grant)] {
        if (auto runtime = session->remoteControlOnStrand()) runtime->activate(grant,rc::NowMs());
    });
}
void MeetingCoordinator::pauseRemoteControl(bool paused, std::string grant) {
    auto session = _sessionRuntime;
    if (!session || _state != MeetingState::InMeeting) return;
    session->invalidateRemoteInput(); // Discard UI input already queued before focus changed.
    session->post([session,paused,grant = std::move(grant)] {
        if (auto runtime = session->remoteControlOnStrand()) runtime->pause(paused,grant,rc::NowMs());
    });
}
void MeetingCoordinator::sendRemoteControlInput(rc::Input input, std::string grant, uint64_t inputEpoch) {
    auto session = _sessionRuntime;
    if (!session || _state != MeetingState::InMeeting) return;
    session->postRemoteControl([session,input,inputEpoch,epoch = session->remoteInputEpoch(),grant = std::move(grant)] {
        if (session->remoteInputEpoch() != epoch) return;
        if (auto runtime = session->remoteControlOnStrand()) runtime->input(input,grant,rc::NowMs(),inputEpoch);
    });
}
void MeetingCoordinator::stopRemoteControl() {
    // The input owner closes immediately even if the session executor is busy.
    // Invalidate pending Request/Grant/Ready work before the strand can start
    // a successor native lease behind this synchronous stop.
    auto session = _sessionRuntime;
    if (session) session->revokeRemoteControl();
    if (auto input = _remoteInput.lock()) input->stop();
    if (session) session->post([session] {
        if (auto runtime = session->remoteControlOnStrand()) runtime->stop();
    });
}
} // namespace OpenMeeting
