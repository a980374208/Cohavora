#include "participant.h"
#include "livekit_rtc.pb.h"
#include <iostream>

namespace livekit {

void LocalParticipant::PublishTrack(std::shared_ptr<Track> track) {
    if (!track) return;

    proto::SignalRequest req;
    auto* add_track = req.mutable_add_track();
    add_track->set_cid(track->name()); 
    add_track->set_name(track->name());
    
    if (track->kind() == TrackKind::Audio) {
        add_track->set_type(proto::TrackType::AUDIO);
    } else if (track->kind() == TrackKind::Video) {
        add_track->set_type(proto::TrackType::VIDEO);
    }

    auto pub = std::make_shared<TrackPublication>(track, track->name(), track->name());
    add_publication(pub);

    if (send_handler_) {
        send_handler_(req);
    }
}

void LocalParticipant::SetMuted(const std::string& track_sid, bool muted) {
    auto pub = get_publication(track_sid);
    if (pub && pub->track()) {
        pub->track()->set_muted(muted);
    }

    proto::SignalRequest req;
    auto* mute_req = req.mutable_mute();
    mute_req->set_sid(track_sid);
    mute_req->set_muted(muted);

    if (send_handler_) {
        send_handler_(req);
    }
}

} // namespace livekit
