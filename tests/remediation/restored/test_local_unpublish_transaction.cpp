// Phase2A1 delivered test copy; original input remains immutable and untracked.
// Original: tests/test_local_unpublish_transaction.cpp
// Original SHA256: 44cbc0377af20ce39420fdea906d5c7e8f0879873706a641ad51d670c99fb862
// Provenance and exact adaptations: tests/remediation/restored/PROVENANCE.json
// Adaptation: always-active checks (14 original expressions).
// Lifetime adaptation: coroutine handler owns SID by value before suspension.

#include "tests/support/test_check.h"
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include <asio.hpp>

#include "livekit_rtc.pb.h"
#include "operation.h"
#include "participant.h"
#include "room.h"

namespace {

template <typename Awaitable>
livekit::OperationError ExpectOperationError(asio::io_context& io, Awaitable&& operation) {
    auto result = asio::co_spawn(io, std::forward<Awaitable>(operation), asio::use_future);
    io.run();
    try {
        (void)result.get();
    } catch (const livekit::OperationError& error) {
        return error;
    }
    TEST_CHECK(false && "operation unexpectedly succeeded");
    std::abort();
}

} // namespace

int main() {
    // The media path is intentionally SDP-negotiated. Guard against a future
    // implementation mistaking the remote notification or data-track message
    // for a local media-unpublish request.
    const auto* request = livekit::proto::SignalRequest::descriptor();
    const auto* response = livekit::proto::SignalResponse::descriptor();
    TEST_CHECK(request->FindFieldByName("add_track") != nullptr);
    TEST_CHECK(request->FindFieldByName("unpublish_data_track_request") != nullptr);
    TEST_CHECK(request->FindFieldByName("unpublish_track") == nullptr);
    TEST_CHECK(request->FindFieldByName("remove_track") == nullptr);
    TEST_CHECK(response->FindFieldByName("track_unpublished") != nullptr);

    asio::io_context io;
    auto local = std::make_shared<livekit::LocalParticipant>(
        "PA_LOCAL", "local-user", [](const livekit::proto::SignalRequest&) {});

    // A participant cannot claim local-unpublish success unless its Room owns
    // the sender-removal and negotiation transaction.
    auto error = ExpectOperationError(io, local->UnpublishTrackAsync("TR_LOCAL"));
    TEST_CHECK(error.operation() == livekit::OperationKind::UnpublishTrack);
    TEST_CHECK(error.code() == livekit::OperationErrorCode::InvalidState);

    io.restart();
    bool handler_called = false;
    auto expected = std::make_shared<livekit::TrackPublication>(
        nullptr, "TR_LOCAL", "camera");
    local->SetAsyncUnpublishTrackHandler(
        [&handler_called, expected](std::string sid)
            -> asio::awaitable<std::shared_ptr<livekit::TrackPublication>> {
            TEST_CHECK(sid == "TR_LOCAL");
            handler_called = true;
            co_return expected;
        });
    auto delegated = asio::co_spawn(io,
        local->UnpublishTrackAsync("TR_LOCAL"), asio::use_future);
    io.run();
    TEST_CHECK(delegated.get() == expected);
    TEST_CHECK(handler_called);

    // Even a locally-owned publication must not mutate while the Room is
    // disconnected; validation happens before sender removal and map commit.
    io.restart();
    auto room = livekit::Room::Create(io.get_executor());
    room->SetLocalParticipantForTesting(local);
    auto track = std::make_shared<livekit::Track>(
        "TR_LOCAL", "camera", livekit::TrackKind::Video);
    local->add_publication(std::make_shared<livekit::TrackPublication>(
        track, "TR_LOCAL", "camera"));
    error = ExpectOperationError(io, room->UnpublishLocalTrackAsync("TR_LOCAL"));
    TEST_CHECK(error.operation() == livekit::OperationKind::UnpublishTrack);
    TEST_CHECK(error.code() == livekit::OperationErrorCode::InvalidState);
    TEST_CHECK(local->get_publication("TR_LOCAL") != nullptr);

    std::cout << "[SUCCESS] Local unpublish protocol gate and transaction validation passed."
              << std::endl;
    return 0;
}
