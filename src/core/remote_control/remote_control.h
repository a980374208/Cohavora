#pragma once

#include "src/media/screen_binding.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace livekit::remote_control {

inline constexpr std::string_view Topic = "cohavora.remote-control.v2";
inline constexpr std::size_t MaxWireBytes = 2048;
inline constexpr uint64_t LeaseMs = 3000, HeartbeatMs = 500, RequestMs = 15000;
uint64_t NowMs(); // steady clock, never wall clock

struct Peer {
    std::string identity;
    uint64_t generation = 0, incarnation = 0;
    bool operator==(const Peer&) const = default;
};
struct Surface {
    std::string track;
    ScreenBinding binding;
    bool operator==(const Surface&) const = default;
};
enum class State { Idle, Requesting, AwaitingConsent, AwaitingReady, Controlling, Controlled, AwaitingActivation };
enum class Reason { None, Declined, Cancelled, Timeout, Unavailable, PeerChanged,
                    InputStopped, Transport, Protocol, Busy };
enum class InputKind { Move, Button, Wheel, Key };
struct Input {
    InputKind kind = InputKind::Move;
    uint16_t x = 0, y = 0;
    int code = 0; // button 1..3; signed wheel delta; physical scan code
    bool down = false, extended = false;
    bool valid() const;
    bool operator==(const Input&) const = default;
};
enum class Kind { Request, Grant, Ready, Input, Ping, Pong, End, Deny, Pause, Resume, InputState };
struct Message {
    Kind kind = Kind::Request;
    std::string request, grant, track, share;
    uint64_t epoch = 0, sequence = 0, challenge = 0;
    int width = 0, height = 0;
    Input input;
    uint64_t inputEpoch = 0;
    bool paused = true;
};
std::string Encode(const Message&);
std::optional<Message> Decode(std::string_view);

struct Projection {
    State state = State::Idle;
    Reason reason = Reason::None;
    std::string peer, request, grant, track;
    int width = 0, height = 0;
    uint64_t inputEpoch = 0;
    bool inputPaused = true, controllerPaused = false;
};
struct InputStatus {
    uint64_t epoch = 0;
    bool paused = true;
    bool operator==(const InputStatus&) const = default;
};
// Revocation and watchdog admission are independently observable by the input
// owner. No mutable Runtime state is accessed from that owner.
struct Lease {
    std::atomic<bool> valid{true};
    std::atomic<uint64_t> deadline{0};
    std::function<bool()> current;
    bool permits(uint64_t now) const {
        try { return valid.load() && now < deadline.load() && current && current(); }
        catch (...) { return false; }
    }
};
class InputBackend {
public:
    virtual ~InputBackend() = default;
    virtual bool start(const ScreenBinding&, std::shared_ptr<Lease>) = 0;
    virtual bool submit(const Input&, uint64_t inputEpoch) = 0;
    virtual void pause(bool) = 0;
    virtual InputStatus status() = 0;
    virtual void stop() = 0;
};

// Strand-only protocol authority. Injected callbacks make the complete peer
// handshake, fail-closed transitions and timeouts testable without OS input.
class Runtime final {
public:
    using Current = std::function<bool()>;
    struct Hooks {
        std::function<bool(std::string_view, std::string_view)> send; // identity, wire
        std::function<std::optional<Surface>()> surface;
        std::function<std::string()> newId;
        std::function<void(Projection)> project;
        std::shared_ptr<InputBackend> input;
    };
    explicit Runtime(Hooks);
    ~Runtime();
    State state() const { return state_; }
    void request(Peer, Current, std::string track, uint64_t now);
    void consent(std::string_view request, bool allow, uint64_t now);
    void activate(std::string_view grant, uint64_t now);
    void receive(const Message&, Peer, Current, uint64_t now);
    void input(const Input&, std::string_view grant, uint64_t now, uint64_t inputEpoch);
    void pause(bool paused, std::string_view grant, uint64_t now);
    void tick(uint64_t now);
    void stop(Reason reason = Reason::Cancelled);
private:
    bool current(uint64_t now);
    bool matches(const Message&, const Peer&) const;
    Message message(Kind) const;
    bool send(Message);
    void project(Reason = Reason::None);
    void finish(Reason, bool notify);
    bool rate(uint64_t now);
    void syncInputStatus(bool force = false);
    Hooks hooks_;
    State state_ = State::Idle;
    Peer peer_;
    Current peerCurrent_;
    std::optional<Surface> surface_;
    std::shared_ptr<Lease> lease_;
    Message scope_;
    uint64_t deadline_ = 0, nextPing_ = 0, challenge_ = 0, pingSent_ = 0;
    uint64_t tx_ = 0, rx_ = 0, rateStart_ = 0, lastRequest_ = 0;
    unsigned rateCount_ = 0;
    bool pingOutstanding_ = false, host_ = false;
    InputStatus inputStatus_;
    uint64_t focusRevision_ = 0;
    bool controllerPaused_ = false;
};

// All arguments use physical pixels. Empty rectangles are rejected. A shared
// monitor may start at a negative virtual-desktop coordinate.
struct AbsolutePoint { int x = 0, y = 0; bool operator==(const AbsolutePoint&) const = default; };
std::optional<AbsolutePoint> NormalizePointer(double x, double y,
    double left, double top, double width, double height);
std::optional<AbsolutePoint> MapAbsolute(uint16_t x, uint16_t y,
    const ScreenBinding&, int virtualX, int virtualY, int virtualWidth, int virtualHeight);
} // namespace livekit::remote_control
