#include "remote_control.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace livekit::remote_control {
uint64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool Input::valid() const {
    switch (kind) {
    case InputKind::Move: return code == 0 && !down && !extended;
    case InputKind::Button: return code >= 1 && code <= 3 && !extended;
    case InputKind::Wheel: return code != 0 && code >= -1200 && code <= 1200 && !down && !extended;
    case InputKind::Key: return code > 0 && code <= 0x7f;
    }
    return false;
}
std::string Encode(const Message& m) {
    nlohmann::json j{{"v",2},{"k",int(m.kind)},{"r",m.request},{"g",m.grant},
        {"t",m.track},{"s",m.share},{"e",m.epoch},{"q",m.sequence},
        {"c",m.challenge},{"w",m.width},{"h",m.height},{"a",m.inputEpoch},{"p",m.paused}};
    if (m.kind == Kind::Input) j["i"] = {int(m.input.kind),m.input.x,m.input.y,
        m.input.code,m.input.down,m.input.extended};
    return j.dump();
}
std::optional<Message> Decode(std::string_view wire) {
    if (wire.empty() || wire.size() > MaxWireBytes) return {};
    try {
        const auto j = nlohmann::json::parse(wire);
        if (!j.is_object() || j.size() > 14 || j.at("v") != 2) return {};
        const auto integer = [](const nlohmann::json& v, int low, int high) {
            if (!v.is_number_integer()) throw 0;
            const auto n = v.get<int64_t>();
            if (n < low || n > high) throw 0;
            return int(n);
        };
        const auto u64 = [](const nlohmann::json& v) {
            if (!v.is_number_unsigned()) throw 0;
            return v.get<uint64_t>();
        };
        const auto str = [](const nlohmann::json& v) {
            auto s = v.get<std::string>();
            if (s.size() > 128 || s.find('\0') != std::string::npos) throw 0;
            return s;
        };
        Message m;
        m.kind = Kind(integer(j.at("k"), 0, int(Kind::InputState)));
        m.request = str(j.at("r")); m.grant = str(j.at("g"));
        m.track = str(j.at("t")); m.share = str(j.at("s"));
        m.epoch = u64(j.at("e")); m.sequence = u64(j.at("q"));
        m.challenge = u64(j.at("c"));
        m.inputEpoch = u64(j.at("a")); m.paused = j.at("p").get<bool>();
        m.width = integer(j.at("w"),0,32768); m.height = integer(j.at("h"),0,32768);
        if (m.request.empty() || m.track.empty()) return {};
        if (m.kind == Kind::Input) {
            const auto& i = j.at("i");
            if (!i.is_array() || i.size() != 6) return {};
            m.input = {InputKind(integer(i[0],0,3)), uint16_t(integer(i[1],0,65535)),
                uint16_t(integer(i[2],0,65535)), integer(i[3],-1200,1200),
                i[4].get<bool>(), i[5].get<bool>()};
            if (!m.input.valid()) return {};
        }
        return m;
    } catch (...) { return {}; }
}
std::optional<AbsolutePoint> NormalizePointer(double x, double y,
        double left, double top, double width, double height) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(left) || !std::isfinite(top) ||
        !std::isfinite(width) || !std::isfinite(height) || width <= 1 || height <= 1 ||
        x < left || y < top || x >= left+width || y >= top+height) return {};
    return AbsolutePoint{int(std::lround(std::clamp((x-left)/(width-1),0.0,1.0)*65535)),
        int(std::lround(std::clamp((y-top)/(height-1),0.0,1.0)*65535))};
}
std::optional<AbsolutePoint> MapAbsolute(uint16_t x, uint16_t y,
        const ScreenBinding& b, int vx, int vy, int vw, int vh) {
    if (vw <= 1 || vh <= 1 || b.physical_width <= 1 || b.physical_height <= 1) return {};
    const int64_t right = int64_t(b.physical_x) + b.physical_width;
    const int64_t bottom = int64_t(b.physical_y) + b.physical_height;
    if (b.physical_x < vx || b.physical_y < vy || right > int64_t(vx)+vw || bottom > int64_t(vy)+vh) return {};
    const auto axis = [](uint16_t u, int origin, int extent, int vo, int ve) {
        const auto pixel = int64_t(origin) + std::llround(double(u)*(extent-1)/65535.0);
        return int(std::llround(double(pixel-vo)*65535.0/(ve-1)));
    };
    return AbsolutePoint{axis(x,b.physical_x,b.physical_width,vx,vw),
        axis(y,b.physical_y,b.physical_height,vy,vh)};
}

Runtime::Runtime(Hooks hooks) : hooks_(std::move(hooks)) {}
Runtime::~Runtime() { if (lease_) lease_->valid = false; if (hooks_.input) hooks_.input->stop(); }
Message Runtime::message(Kind k) const { auto m = scope_; m.kind = k; return m; }
void Runtime::project(Reason reason) {
    if (hooks_.project) hooks_.project({state_,reason,peer_.identity,scope_.request,
        scope_.grant,scope_.track,scope_.width,scope_.height,
        inputStatus_.epoch,inputStatus_.paused || controllerPaused_,controllerPaused_});
}
bool Runtime::send(Message m) {
    try {
    if (m.kind != Kind::Request && m.kind != Kind::Grant && m.kind != Kind::Deny) m.sequence = ++tx_;
    const auto wire = Encode(m);
    return hooks_.send && wire.size() <= MaxWireBytes && hooks_.send(peer_.identity,wire);
    } catch (...) { return false; }
}
void Runtime::finish(Reason reason, bool notify) {
    if (state_ == State::Idle) return;
    if (lease_) lease_->valid = false;
    if (hooks_.input) hooks_.input->stop();
    if (notify) (void)send(message(Kind::End));
    state_ = State::Idle;
    project(reason);
    peer_ = {}; peerCurrent_ = {}; surface_.reset(); lease_.reset(); scope_ = {};
    tx_ = rx_ = 0; pingOutstanding_ = false; host_ = false;
    inputStatus_ = {}; focusRevision_ = 0; controllerPaused_ = false;
}
void Runtime::stop(Reason reason) { finish(reason,true); }
bool Runtime::current(uint64_t now) {
    if (state_ == State::Idle) return false;
    if (!peerCurrent_ || !peerCurrent_()) { finish(Reason::PeerChanged,true); return false; }
    if (now >= deadline_) { finish(Reason::Timeout,true); return false; }
    if (host_ && (!hooks_.surface || hooks_.surface() != surface_)) {
        finish(Reason::Unavailable,true); return false;
    }
    if (lease_ && !lease_->permits(now)) { finish(Reason::InputStopped,true); return false; }
    return true;
}
void Runtime::request(Peer peer, Current check, std::string track, uint64_t now) {
    if (state_ != State::Idle || peer.identity.empty() || !check || !check() || track.empty()) return;
    peer_ = std::move(peer); peerCurrent_ = std::move(check);
    scope_ = {}; scope_.request = hooks_.newId(); scope_.track = std::move(track);
    state_ = State::Requesting; host_ = false; deadline_ = now + RequestMs;
    tx_ = rx_ = 0;
    if (!send(message(Kind::Request))) { finish(Reason::Transport,false); return; }
    project();
}
void Runtime::consent(std::string_view id, bool allow, uint64_t now) {
    if (state_ != State::AwaitingConsent || id != scope_.request || !current(now)) return;
    if (!allow) { finish(Reason::Declined,true); return; }
    scope_.grant = hooks_.newId();
    state_ = State::AwaitingReady; deadline_ = now + RequestMs;
    if (!send(message(Kind::Grant))) { finish(Reason::Transport,false); return; }
    project();
}
void Runtime::activate(std::string_view grant, uint64_t now) {
    if (state_ != State::AwaitingActivation || grant != scope_.grant || !current(now)) return;
    state_ = State::Controlling; deadline_ = now + LeaseMs;
    nextPing_ = now; pingOutstanding_ = false;
    if (!send(message(Kind::Ready))) { finish(Reason::Transport,false); return; }
    project();
}
void Runtime::pause(bool paused, std::string_view grant, uint64_t now) {
    if (state_ != State::Controlling || grant != scope_.grant || !current(now) || controllerPaused_ == paused) return;
    controllerPaused_ = paused;
    inputStatus_.paused = true; // Resume waits for the host's new admission token.
    auto m = message(paused ? Kind::Pause : Kind::Resume);
    m.challenge = ++focusRevision_;
    if (!send(m)) { finish(Reason::Transport,false); return; }
    project();
}
void Runtime::syncInputStatus(bool force) {
    if (state_ != State::Controlled) return;
    const auto status = hooks_.input->status();
    if (!force && status == inputStatus_) return;
    inputStatus_ = status;
    auto m = message(Kind::InputState);
    m.inputEpoch = status.epoch; m.paused = status.paused; m.challenge = focusRevision_;
    if (!send(m)) { finish(Reason::Transport,false); return; }
    project();
}
bool Runtime::matches(const Message& m, const Peer& peer) const {
    return peer == peer_ && m.request == scope_.request && m.track == scope_.track;
}
bool Runtime::rate(uint64_t now) {
    if (now - rateStart_ >= 1000) { rateStart_ = now; rateCount_ = 0; }
    return ++rateCount_ <= 180;
}
void Runtime::receive(const Message& m, Peer peer, Current check, uint64_t now) {
    if (peer.identity.empty() || peer.generation == 0 || peer.incarnation == 0 || !check || !check()) return;
    if (m.kind == Kind::Request) {
        if (state_ != State::Idle || (lastRequest_ && now - lastRequest_ < 2000) ||
            m.request.empty() || !m.grant.empty() || m.sequence != 0) return;
        const auto surface = hooks_.surface ? hooks_.surface() : std::nullopt;
        if (!surface || surface->track != m.track || surface->binding.share_session_id.empty() ||
            !surface->binding.source_epoch) return;
        lastRequest_ = now;
        surface_ = surface; host_ = true; peer_ = std::move(peer); peerCurrent_ = std::move(check);
        scope_ = {}; scope_.request = m.request; scope_.track = m.track;
        scope_.share = surface->binding.share_session_id; scope_.epoch = surface->binding.source_epoch;
        scope_.width = surface->binding.physical_width; scope_.height = surface->binding.physical_height;
        state_ = State::AwaitingConsent; deadline_ = now + RequestMs; tx_ = rx_ = 0;
        project(); return;
    }
    if (!matches(m,peer) || !current(now)) return;
    if (m.kind == Kind::End || m.kind == Kind::Deny) {
        // Cancelling a pending request may precede receipt of the host's Grant.
        if (m.grant == scope_.grant || (host_ && state_ != State::Controlled && m.grant.empty()))
            finish(Reason::Cancelled,false);
        return;
    }
    if (state_ == State::Requesting && m.kind == Kind::Grant) {
        if (m.grant.empty() || m.share.empty() || !m.epoch || m.sequence || m.width < 2 || m.height < 2) return;
        // Consent may arrive while the controller is using a separate remote
        // desktop app to approve it. Native input starts only after the UI
        // explicitly activates the visible, focused shared video.
        scope_ = m; state_ = State::AwaitingActivation; deadline_ = now + RequestMs;
        rateStart_ = now; rateCount_ = 0;
        nextPing_ = now; pingOutstanding_ = false; tx_ = rx_ = 0;
        project(); return;
    }
    if (m.grant.empty() || m.grant != scope_.grant || m.share != scope_.share || m.epoch != scope_.epoch) return;
    if (m.sequence <= rx_) return;
    if (m.sequence != rx_ + 1) { finish(Reason::Protocol,true); return; }
    rx_ = m.sequence;
    if (state_ == State::AwaitingReady && m.kind == Kind::Ready) {
        lease_ = std::make_shared<Lease>(); lease_->current = peerCurrent_;
        lease_->deadline = deadline_ = now + LeaseMs;
        if (!hooks_.input || !hooks_.input->start(surface_->binding,lease_)) {
            finish(Reason::InputStopped,true); return;
        }
        state_ = State::Controlled; nextPing_ = now; pingOutstanding_ = false;
        rateStart_ = now; rateCount_ = 0;
        syncInputStatus(true); return;
    }
    if (state_ != State::Controlled && state_ != State::Controlling) return;
    if (!rate(now)) { finish(Reason::Protocol,true); return; }
    if (m.kind == Kind::Ping) {
        auto reply = message(Kind::Pong); reply.challenge = m.challenge;
        if (!send(reply)) finish(Reason::Transport,false);
    } else if (m.kind == Kind::Pong) {
        if (pingOutstanding_ && m.challenge == challenge_ && now - pingSent_ < LeaseMs/2) {
            pingOutstanding_ = false; deadline_ = now + LeaseMs;
            if (lease_) lease_->deadline = deadline_;
        }
    } else if ((m.kind == Kind::Pause || m.kind == Kind::Resume) && state_ == State::Controlled) {
        if (m.challenge != focusRevision_ + 1) { finish(Reason::Protocol,true); return; }
        focusRevision_ = m.challenge; controllerPaused_ = m.kind == Kind::Pause;
        hooks_.input->pause(controllerPaused_);
        syncInputStatus(true);
    } else if (m.kind == Kind::InputState && state_ == State::Controlling) {
        if (m.challenge != focusRevision_) return; // Old focus acknowledgement.
        if (m.inputEpoch < inputStatus_.epoch) { finish(Reason::Protocol,true); return; }
        inputStatus_ = {m.inputEpoch,m.paused}; project();
    } else if (m.kind == Kind::Input && state_ == State::Controlled) {
        if (!m.input.valid() || !hooks_.input->submit(m.input,m.inputEpoch)) finish(Reason::InputStopped,true);
    } else { finish(Reason::Protocol,true); }
}
void Runtime::input(const Input& input, std::string_view grant, uint64_t now, uint64_t inputEpoch) {
    if (state_ != State::Controlling || grant != scope_.grant || !current(now)) return;
    if (controllerPaused_ || inputStatus_.paused || !inputEpoch || inputEpoch != inputStatus_.epoch) return;
    if (!input.valid() || !rate(now)) { finish(Reason::Protocol,true); return; }
    auto m = message(Kind::Input); m.input = input; m.inputEpoch = inputEpoch;
    if (!send(m)) finish(Reason::Transport,false);
}
void Runtime::tick(uint64_t now) {
    if (!current(now) || (state_ != State::Controlled && state_ != State::Controlling)) return;
    if (state_ == State::Controlled) syncInputStatus();
    if (state_ == State::Idle) return;
    if (now >= nextPing_ && !pingOutstanding_) {
        auto m = message(Kind::Ping); m.challenge = ++challenge_;
        pingOutstanding_ = true; pingSent_ = now; nextPing_ = now + HeartbeatMs;
        if (!send(m)) finish(Reason::Transport,false);
    }
}
} // namespace livekit::remote_control
