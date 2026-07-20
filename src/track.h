#pragma once

#include <string>
#include <memory>

namespace livekit {

enum class TrackKind {
    Audio,
    Video,
    Unknown
};

class Track {
public:
    Track(const std::string& sid, const std::string& name, TrackKind kind)
        : sid_(sid), name_(name), kind_(kind), muted_(false) {}
    virtual ~Track() = default;

    std::string sid() const { return sid_; }
    std::string name() const { return name_; }
    TrackKind kind() const { return kind_; }
    bool muted() const { return muted_; }

    void set_muted(bool muted) { muted_ = muted; }
    void set_sid(const std::string& sid) { sid_ = sid; }

private:
    std::string sid_;
    std::string name_;
    TrackKind kind_;
    bool muted_;
};

class TrackPublication {
public:
    TrackPublication(std::shared_ptr<Track> track, const std::string& sid, const std::string& name)
        : track_(track), sid_(sid), name_(name) {}

    std::string sid() const { return sid_; }
    std::string name() const { return name_; }
    std::shared_ptr<Track> track() const { return track_; }
    bool muted() const { return track_ ? track_->muted() : false; }

private:
    std::shared_ptr<Track> track_;
    std::string sid_;
    std::string name_;
};

} // namespace livekit
