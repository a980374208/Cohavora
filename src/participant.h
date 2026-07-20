#pragma once

#include <string>
#include <memory>
#include <map>
#include <functional>
#include "track.h"

// Forward declare generated protobuf messages
namespace livekit {
namespace proto {
class SignalRequest;
}
}

namespace livekit {

class Participant {
public:
    Participant(const std::string& sid, const std::string& identity)
        : sid_(sid), identity_(identity) {}
    virtual ~Participant() = default;

    std::string sid() const { return sid_; }
    std::string identity() const { return identity_; }
    std::string metadata() const { return metadata_; }

    void set_metadata(const std::string& metadata) { metadata_ = metadata; }
    void set_sid(const std::string& sid) { sid_ = sid; }

    std::map<std::string, std::shared_ptr<TrackPublication>> tracks() const { return tracks_; }

    void add_publication(std::shared_ptr<TrackPublication> pub) {
        tracks_[pub->sid()] = pub;
    }

    std::shared_ptr<TrackPublication> get_publication(const std::string& sid) {
        auto it = tracks_.find(sid);
        if (it != tracks_.end()) {
            return it->second;
        }
        return nullptr;
    }

protected:
    std::string sid_;
    std::string identity_;
    std::string metadata_;
    std::map<std::string, std::shared_ptr<TrackPublication>> tracks_;
};

class LocalParticipant : public Participant {
public:
    using SendSignalHandler = std::function<void(const proto::SignalRequest&)>;

    LocalParticipant(const std::string& sid, const std::string& identity, SendSignalHandler send_handler)
        : Participant(sid, identity), send_handler_(send_handler) {}

    // 模拟发布本地 Track 逻辑
    void PublishTrack(std::shared_ptr<Track> track);

    // 模拟本地静音控制逻辑
    void SetMuted(const std::string& track_sid, bool muted);

private:
    SendSignalHandler send_handler_;
};

class RemoteParticipant : public Participant {
public:
    RemoteParticipant(const std::string& sid, const std::string& identity)
        : Participant(sid, identity) {}
};

} // namespace livekit
