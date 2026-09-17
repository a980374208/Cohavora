// NEW-CPP_QT-002: actual LocalParticipant -> Room transaction lifetime coverage.
// New remediation input; protected test_local_unpublish_transaction.cpp is intact.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <asio.hpp>

#include "livekit_rtc.pb.h"
#include "operation.h"
#include "participant.h"
#include "room.h"
#include "tests/support/test_check.h"

namespace unpublish_test {

struct Gate {
    explicit Gate(asio::any_io_executor executor) : timer(executor) {
        timer.expires_at((asio::steady_timer::time_point::max)());
    }
    void release(std::exception_ptr error = {}) {
        TEST_CHECK(started == 1 && waiting && !released);
        failure = std::move(error);
        released = true;
        std::error_code ec;
        TEST_CHECK(timer.cancel(ec) == 1);
        TEST_CHECK(!ec);
    }
    asio::steady_timer timer;
    int started = 0;
    bool waiting = false;
    bool released = false;
    std::exception_ptr failure;
};

struct Boundaries {
    explicit Boundaries(asio::any_io_executor executor)
        : remove(std::make_shared<Gate>(executor)), negotiate(std::make_shared<Gate>(executor)) {}
    std::shared_ptr<Gate> remove;
    std::shared_ptr<Gate> negotiate;
    std::shared_ptr<livekit::Track> removed_track;
    uint64_t remove_generation = 0;
    uint64_t negotiate_generation = 0;
    std::chrono::milliseconds negotiate_timeout{0};
};

// Every coroutine below owns its state/arguments. Installed callbacks are
// ordinary lambdas that return these awaitables, not capturing coroutine lambdas.
asio::awaitable<void> Wait(std::shared_ptr<Gate> gate) {
    ++gate->started;
    gate->waiting = true;
    std::error_code ec;
    co_await gate->timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
    TEST_CHECK(gate->released);
    TEST_CHECK(ec == asio::error::operation_aborted);
    gate->waiting = false;
    if (gate->failure) std::rethrow_exception(gate->failure);
}

asio::awaitable<void> Remove(std::shared_ptr<Boundaries> state,
                             std::shared_ptr<livekit::Track> track, uint64_t generation) {
    state->removed_track = std::move(track);
    state->remove_generation = generation;
    co_await Wait(state->remove);
}

asio::awaitable<void> Negotiate(std::shared_ptr<Boundaries> state,
                                std::chrono::milliseconds timeout, uint64_t generation) {
    state->negotiate_timeout = timeout;
    state->negotiate_generation = generation;
    co_await Wait(state->negotiate);
}

} // namespace unpublish_test

namespace livekit {

// Sole definition of the private friend. It arranges fixtures and reads state;
// it never performs the unpublish commit or removes a publication itself.
class RoomUnpublishTestAccess {
public:
    struct State {
        std::size_t pending_count = 0;
        bool has_pending = false;
        bool sender_removed = false;
        uint64_t pending_generation = 0;
        std::shared_ptr<TrackPublication> pending_publication;
        std::vector<std::string> recovery_sids;
    };

    static void Install(Room& room, std::shared_ptr<LocalParticipant> local,
                        std::shared_ptr<RemoteParticipant> remote,
                        std::shared_ptr<TrackPublication> target,
                        std::shared_ptr<TrackPublication> sentinel,
                        std::shared_ptr<unpublish_test::Boundaries> boundaries,
                        bool inject_boundaries) {
        std::lock_guard lock(room.room_mutex_);
        room.local_participant_ = std::move(local);
        const auto remote_sid = remote->sid();
        room.remote_participants_.emplace(remote_sid, std::move(remote));
        room.connection_state_ = ConnectionState::Connected;
        room.session_generation_.store(1);
        room.published_track_records_.push_back({target->track(), target->sid()});
        room.published_track_records_.push_back({sentinel->track(), sentinel->sid()});
        if (inject_boundaries) {
            auto hooks = std::make_shared<Room::LocalUnpublishTestHooks>();
            hooks->remove_sender = [boundaries](std::shared_ptr<Track> track, uint64_t generation) {
                return unpublish_test::Remove(boundaries, std::move(track), generation);
            };
            hooks->negotiate = [boundaries](std::chrono::milliseconds timeout, uint64_t generation) {
                return unpublish_test::Negotiate(boundaries, timeout, generation);
            };
            room.local_unpublish_test_hooks_ = std::move(hooks);
        }
        // Connect() and this fixture use the same production binding helper.
        room.BindLocalUnpublishHandler();
    }

    static State Inspect(Room& room, const std::string& sid) {
        std::lock_guard lock(room.room_mutex_);
        State result;
        result.pending_count = room.pending_local_unpublishes_.size();
        const auto pending = room.pending_local_unpublishes_.find(sid);
        if (pending != room.pending_local_unpublishes_.end()) {
            result.has_pending = true;
            result.sender_removed = pending->second.sender_removed;
            result.pending_generation = pending->second.generation;
            result.pending_publication = pending->second.publication;
        }
        for (const auto& record : room.published_track_records_) {
            result.recovery_sids.push_back(record.previous_sid);
        }
        return result;
    }

    static void DisconnectState(Room& room) {
        std::lock_guard lock(room.room_mutex_);
        room.connection_state_ = ConnectionState::Disconnected;
    }

    static void ChangeGeneration(Room& room) {
        room.session_generation_.fetch_add(1);
    }

    static void TearDown(Room& room) {
        std::lock_guard lock(room.room_mutex_);
        // Break the pre-existing participant-handler/Room capture cycle only
        // after all futures were consumed. Do not run unrelated native teardown.
        room.local_participant_->SetAsyncUnpublishTrackHandler({});
        room.local_unpublish_test_hooks_.reset();
        room.connection_state_ = ConnectionState::Disconnected;
    }
};

} // namespace livekit

namespace {

using Publication = std::shared_ptr<livekit::TrackPublication>;
using PublicationFuture = std::future<Publication>;
using Access = livekit::RoomUnpublishTestAccess;
using ErrorCode = livekit::OperationErrorCode;
enum class Entry { Participant, Room };
const std::string kSentinelSid = "TR_SENTINEL";
const std::string kRemoteSid = "TR_REMOTE";

struct Listener : livekit::RoomListener {
    std::shared_ptr<livekit::LocalParticipant> local;
    std::weak_ptr<livekit::Room> room;
    std::string expected_sid;
    Publication notified;
    int calls = 0;
    void OnLocalTrackUnpublished(Publication publication) override {
        ++calls;
        notified = std::move(publication);
        TEST_CHECK(local->get_publication(expected_sid) == nullptr);
        const auto owner = room.lock();
        TEST_CHECK(owner != nullptr);
        TEST_CHECK(Access::Inspect(*owner, expected_sid).pending_count == 0);
    }
};

struct Fixture {
    explicit Fixture(std::string target = "TR_LOCAL", bool inject_boundaries = true)
        : sid(std::move(target)),
          room(livekit::Room::Create(io.get_executor())),
          local(std::make_shared<livekit::LocalParticipant>(
              "PA_LOCAL", "local", [](const livekit::proto::SignalRequest&) {})),
          remote(std::make_shared<livekit::RemoteParticipant>("PA_REMOTE", "remote")),
          track(std::make_shared<livekit::Track>(sid, "camera", livekit::TrackKind::Video)),
          publication(std::make_shared<livekit::TrackPublication>(track, sid, "camera")),
          sentinel(std::make_shared<livekit::TrackPublication>(
              std::make_shared<livekit::Track>(kSentinelSid, "other", livekit::TrackKind::Audio),
              kSentinelSid, "other")),
          remote_publication(std::make_shared<livekit::TrackPublication>(
              std::make_shared<livekit::Track>(kRemoteSid, "remote", livekit::TrackKind::Video),
              kRemoteSid, "remote")),
          boundaries(std::make_shared<unpublish_test::Boundaries>(io.get_executor())),
          listener(std::make_shared<Listener>()) {
        local->add_publication(publication);
        local->add_publication(sentinel);
        remote->add_publication(remote_publication);
        Access::Install(*room, local, remote, publication, sentinel, boundaries, inject_boundaries);
        listener->local = local;
        listener->room = room;
        listener->expected_sid = sid;
        room->AddListener(listener);
    }
    ~Fixture() { Access::TearDown(*room); }

    void pump() {
        io.restart();
        io.poll(); // Only ready handlers run. No sleep or wall-clock timeout.
    }

    void check_untouched() const {
        TEST_CHECK(local->get_publication(sid) == publication);
        TEST_CHECK(publication->track() == track);
        TEST_CHECK(track->sid() == sid);
        TEST_CHECK(local->get_publication(kSentinelSid) == sentinel);
        TEST_CHECK(sentinel->track()->sid() == kSentinelSid);
        TEST_CHECK(remote->get_publication(kRemoteSid) == remote_publication);
        TEST_CHECK(remote_publication->track()->sid() == kRemoteSid);
        TEST_CHECK(room->remote_participants().at("PA_REMOTE") == remote);
        TEST_CHECK((Access::Inspect(*room, sid).recovery_sids ==
                    std::vector<std::string>{sid, kSentinelSid}));
        TEST_CHECK(listener->calls == 0);
    }

    std::string sid;
    asio::io_context io;
    std::shared_ptr<livekit::Room> room;
    std::shared_ptr<livekit::LocalParticipant> local;
    std::shared_ptr<livekit::RemoteParticipant> remote;
    std::shared_ptr<livekit::Track> track;
    Publication publication;
    Publication sentinel;
    Publication remote_publication;
    std::shared_ptr<unpublish_test::Boundaries> boundaries;
    std::shared_ptr<Listener> listener;
};

// Ordinary forwarding function, deliberately no early copy: tests-first must
// expose the real public coroutine signatures rather than fix them in a wrapper.
asio::awaitable<Publication> Begin(Entry entry, Fixture& fixture, const std::string& sid) {
    if (entry == Entry::Participant) return fixture.local->UnpublishTrackAsync(sid);
    return fixture.room->UnpublishLocalTrackAsync(sid);
}

PublicationFuture Spawn(Fixture& fixture, asio::awaitable<Publication> operation) {
    return asio::co_spawn(fixture.io, std::move(operation), asio::use_future);
}

bool Ready(PublicationFuture& result) {
    return result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
}

void RequirePending(PublicationFuture& result) {
    if (!Ready(result)) return;
    // Consume even an unexpectedly completed future. The live caller-mutation
    // red case reaches here as a defined InvalidState, not use-after-lifetime.
    try {
        (void)result.get();
        std::fputs("UNPUBLISH_LIFETIME_EARLY_COMPLETION\n", stderr);
    } catch (const livekit::OperationError& error) {
        std::fprintf(stderr, "UNPUBLISH_LIFETIME_EARLY_ERROR: stage=%s message=%s\n",
                     error.stage().c_str(), error.what());
    }
    TEST_CHECK(false && "expected the creation-time SID to reach the controlled boundary");
}

livekit::OperationError GetError(Fixture& fixture, PublicationFuture& result) {
    fixture.pump();
    TEST_CHECK(Ready(result));
    try {
        (void)result.get();
    } catch (const livekit::OperationError& error) {
        return error;
    }
    TEST_CHECK(false && "operation unexpectedly succeeded");
    std::abort();
}

void CheckError(const livekit::OperationError& error, ErrorCode code,
                const std::string& stage, bool retryable = false) {
    TEST_CHECK(error.operation() == livekit::OperationKind::UnpublishTrack);
    TEST_CHECK(error.code() == code);
    TEST_CHECK(error.stage() == stage);
    TEST_CHECK(error.retryable() == retryable);
}

void CheckPending(Fixture& fixture, PublicationFuture& result, bool sender_removed) {
    RequirePending(result);
    fixture.check_untouched();
    const auto state = Access::Inspect(*fixture.room, fixture.sid);
    TEST_CHECK(state.pending_count == 1);
    TEST_CHECK(state.has_pending && state.sender_removed == sender_removed);
    TEST_CHECK(state.pending_publication == fixture.publication);
    TEST_CHECK(state.pending_generation == 1);
    TEST_CHECK(state.recovery_sids.size() == 2);
    TEST_CHECK(fixture.boundaries->remove->started == 1);
    TEST_CHECK(fixture.boundaries->removed_track == fixture.track);
    TEST_CHECK(fixture.boundaries->remove_generation == 1);
    TEST_CHECK(fixture.boundaries->negotiate->started == (sender_removed ? 1 : 0));
}

void FinishSuccess(Fixture& fixture, PublicationFuture& result, std::string* mutate = nullptr) {
    fixture.pump();
    CheckPending(fixture, result, false);
    TEST_CHECK(fixture.boundaries->remove->waiting);
    if (mutate) mutate->assign(160, 'X');
    fixture.boundaries->remove->release();
    fixture.pump();
    CheckPending(fixture, result, true);
    TEST_CHECK(!fixture.boundaries->remove->waiting);
    TEST_CHECK(fixture.boundaries->negotiate->waiting);
    TEST_CHECK(fixture.boundaries->negotiate_generation == 1);
    TEST_CHECK(fixture.boundaries->negotiate_timeout.count() > 0);
    if (mutate) mutate->clear();
    fixture.boundaries->negotiate->release();
    fixture.pump();
    TEST_CHECK(Ready(result));
    TEST_CHECK(result.get() == fixture.publication);
    TEST_CHECK(fixture.local->get_publication(fixture.sid) == nullptr);
    TEST_CHECK(fixture.local->get_publication(kSentinelSid) == fixture.sentinel);
    TEST_CHECK(fixture.sentinel->track()->sid() == kSentinelSid);
    TEST_CHECK(fixture.remote->get_publication(kRemoteSid) == fixture.remote_publication);
    TEST_CHECK(fixture.remote_publication->track()->sid() == kRemoteSid);
    TEST_CHECK(fixture.room->remote_participants().at("PA_REMOTE") == fixture.remote);
    TEST_CHECK(fixture.listener->calls == 1 && fixture.listener->notified == fixture.publication);
    TEST_CHECK(fixture.publication->track() == nullptr);
    TEST_CHECK(fixture.track->sid().empty());
    const auto state = Access::Inspect(*fixture.room, fixture.sid);
    TEST_CHECK(state.pending_count == 0 && !state.has_pending);
    TEST_CHECK(state.recovery_sids == std::vector<std::string>{kSentinelSid});
    fixture.pump();
    TEST_CHECK(fixture.listener->calls == 1);
}

void TestLiveMutation(Entry entry) {
    for (const auto& sid : {std::string("TR_LOCAL"), "TR_" + std::string(160, 'L')}) {
        Fixture fixture(sid);
        std::string caller_sid = sid;
        auto operation = Begin(entry, fixture, caller_sid);
        caller_sid.assign("TR_CHANGED_BEFORE_INITIAL_RESUME");
        auto result = Spawn(fixture, std::move(operation));
        FinishSuccess(fixture, result);
        TEST_CHECK(caller_sid == "TR_CHANGED_BEFORE_INITIAL_RESUME");
    }
}

void TestTemporaryInputs() {
    for (const auto entry : {Entry::Participant, Entry::Room}) {
        {
            Fixture fixture;
            auto operation = Begin(entry, fixture, "TR_LOCAL");
            auto result = Spawn(fixture, std::move(operation));
            FinishSuccess(fixture, result);
        }
        {
            Fixture fixture;
            auto operation = Begin(entry, fixture, std::string("TR_") + "LOCAL");
            auto result = Spawn(fixture, std::move(operation));
            FinishSuccess(fixture, result);
        }
        {
            Fixture fixture("TR_" + std::string(160, 'L'));
            auto operation = [&fixture, entry] {
                const std::string scoped_sid = fixture.sid;
                return Begin(entry, fixture, scoped_sid);
            }(); // Ordinary lambda returns before the operation is ever resumed.
            auto result = Spawn(fixture, std::move(operation));
            FinishSuccess(fixture, result);
        }
    }
}

void TestMutationWhileSuspended() {
    for (const auto entry : {Entry::Participant, Entry::Room}) {
        Fixture fixture("TR_" + std::string(160, 'S'));
        std::string caller_sid = fixture.sid;
        auto result = Spawn(fixture, Begin(entry, fixture, caller_sid));
        FinishSuccess(fixture, result, &caller_sid);
        TEST_CHECK(caller_sid.empty());
    }
}

void TestInvalidInputs() {
    for (const auto entry : {Entry::Participant, Entry::Room}) {
        for (const auto& sid : {std::string{}, std::string("TR_UNKNOWN"), kRemoteSid}) {
            Fixture fixture;
            auto result = Spawn(fixture, Begin(entry, fixture, sid));
            const auto error = GetError(fixture, result);
            CheckError(error, ErrorCode::InvalidState,
                       sid.empty() && entry == Entry::Participant ? "validate" : "unpublish_validate");
            fixture.check_untouched();
            TEST_CHECK(Access::Inspect(*fixture.room, fixture.sid).pending_count == 0);
            TEST_CHECK(fixture.boundaries->remove->started == 0);
            TEST_CHECK(fixture.boundaries->negotiate->started == 0);
        }
        Fixture fixture;
        Access::DisconnectState(*fixture.room);
        auto result = Spawn(fixture, Begin(entry, fixture, fixture.sid));
        CheckError(GetError(fixture, result), ErrorCode::InvalidState, "unpublish_validate");
        fixture.check_untouched();
        TEST_CHECK(Access::Inspect(*fixture.room, fixture.sid).pending_count == 0);
        TEST_CHECK(fixture.boundaries->remove->started == 0);
        TEST_CHECK(fixture.boundaries->negotiate->started == 0);
    }
}

void TestDuplicateRequest() {
    for (const auto entry : {Entry::Participant, Entry::Room}) {
        Fixture fixture;
        auto first = Spawn(fixture, Begin(entry, fixture, fixture.sid));
        fixture.pump();
        CheckPending(fixture, first, false);
        auto duplicate = Spawn(fixture, Begin(entry, fixture, fixture.sid));
        CheckError(GetError(fixture, duplicate), ErrorCode::InvalidState, "unpublish_validate");
        CheckPending(fixture, first, false);
        FinishSuccess(fixture, first);
    }
}

std::exception_ptr RemoveFailure(ErrorCode code = ErrorCode::InvalidState) {
    return std::make_exception_ptr(livekit::OperationError(
        livekit::OperationKind::UnpublishTrack, code, "injected_remove", "sender not removed"));
}

void TestBoundaryFailures() {
    for (const auto entry : {Entry::Participant, Entry::Room}) {
        for (const bool cancelled : {false, true}) {
            Fixture fixture;
            auto result = Spawn(fixture, Begin(entry, fixture, fixture.sid));
            fixture.pump();
            CheckPending(fixture, result, false);
            if (cancelled) Access::ChangeGeneration(*fixture.room);
            const auto code = cancelled ? ErrorCode::Cancelled : ErrorCode::InvalidState;
            fixture.boundaries->remove->release(RemoveFailure(code));
            CheckError(GetError(fixture, result), code, "injected_remove");
            fixture.check_untouched();
            TEST_CHECK(Access::Inspect(*fixture.room, fixture.sid).pending_count == 0);
            TEST_CHECK(fixture.boundaries->negotiate->started == 0);
        }
        Fixture fixture;
        auto result = Spawn(fixture, Begin(entry, fixture, fixture.sid));
        fixture.pump();
        CheckPending(fixture, result, false);
        fixture.boundaries->remove->release();
        fixture.pump();
        CheckPending(fixture, result, true);
        fixture.boundaries->negotiate->release(std::make_exception_ptr(livekit::OperationError(
            livekit::OperationKind::Negotiate, ErrorCode::NegotiationFailed,
            "injected_negotiate", "answer unavailable")));
        CheckError(GetError(fixture, result), ErrorCode::StateUncertain, "unpublish_negotiate", true);
        fixture.check_untouched();
        const auto state = Access::Inspect(*fixture.room, fixture.sid);
        TEST_CHECK(state.pending_count == 1 && state.has_pending && state.sender_removed);
        TEST_CHECK(state.pending_publication == fixture.publication);
    }
}

void TestGenerationChanges() {
    for (const auto entry : {Entry::Participant, Entry::Room}) {
        for (const bool after_remove : {false, true}) {
            Fixture fixture;
            auto result = Spawn(fixture, Begin(entry, fixture, fixture.sid));
            fixture.pump();
            CheckPending(fixture, result, false);
            if (after_remove) {
                fixture.boundaries->remove->release();
                fixture.pump();
                CheckPending(fixture, result, true);
            }
            Access::ChangeGeneration(*fixture.room);
            if (after_remove) fixture.boundaries->negotiate->release();
            else fixture.boundaries->remove->release();
            CheckError(GetError(fixture, result), ErrorCode::StateUncertain, "unpublish_negotiate", true);
            fixture.check_untouched();
            const auto state = Access::Inspect(*fixture.room, fixture.sid);
            TEST_CHECK(state.pending_count == 1 && state.has_pending);
            TEST_CHECK(state.sender_removed == after_remove);
            TEST_CHECK(state.pending_publication == fixture.publication);
            TEST_CHECK(fixture.boundaries->negotiate->started == (after_remove ? 1 : 0));
        }
    }
}

void TestDefaultNativeBoundary() {
    for (const auto entry : {Entry::Participant, Entry::Room}) {
        Fixture fixture("TR_LOCAL", false);
        auto result = Spawn(fixture, Begin(entry, fixture, fixture.sid));
        // No native rtc_track was installed. The unchanged real removal method
        // must reject it before any WebRTC thread, device or network is needed.
        CheckError(GetError(fixture, result), ErrorCode::InvalidState, "remove_sender_validate");
        fixture.check_untouched();
        TEST_CHECK(Access::Inspect(*fixture.room, fixture.sid).pending_count == 0);
        TEST_CHECK(fixture.boundaries->remove->started == 0);
        TEST_CHECK(fixture.boundaries->negotiate->started == 0);
    }
}

} // namespace

int main(int argc, char** argv) {
    TEST_CHECK(argc == 1 || (argc == 3 && std::string(argv[1]) == "--case"));
    const std::string selected = argc == 3 ? argv[2] : "all";
    struct TestCase { const char* name; std::function<void()> run; };
    const std::vector<TestCase> cases{
        {"live-participant", [] { TestLiveMutation(Entry::Participant); }},
        {"live-room", [] { TestLiveMutation(Entry::Room); }},
        {"temporaries", TestTemporaryInputs},
        {"suspended-mutation", TestMutationWhileSuspended},
        {"invalid", TestInvalidInputs},
        {"duplicate", TestDuplicateRequest},
        {"boundary-failures", TestBoundaryFailures},
        {"generation", TestGenerationChanges},
        {"default-native-boundary", TestDefaultNativeBoundary},
    };
    int executed = 0;
    for (const auto& test : cases) {
        if (selected != "all" && selected != test.name) continue;
        std::printf("[RUN] unpublish lifetime: %s\n", test.name);
        std::fflush(stdout);
        test.run();
        ++executed;
        std::printf("[PASS] unpublish lifetime: %s\n", test.name);
    }
    TEST_CHECK(executed > 0);
    TEST_CHECK(selected != "all" || executed == 9);
    std::printf("[SUCCESS] unpublish lifetime groups executed=%d\n", executed);
    return 0;
}
