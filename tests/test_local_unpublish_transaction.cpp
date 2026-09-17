#include <cassert>
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
    assert(false && "operation unexpectedly succeeded");
    std::abort();
}

} // namespace

int main() {
    // The media path is intentionally SDP-negotiated. Guard against a future
    // implementation mistaking the remote notification or data-track message
    // for a local media-unpublish request.
    const auto* request = livekit::proto::SignalRequest::descriptor();
    const auto* response = livekit::proto::SignalResponse::descriptor();
    assert(request->FindFieldByName("add_track") != nullptr);
    assert(request->FindFieldByName("unpublish_data_track_request") != nullptr);
    assert(request->FindFieldByName("unpublish_track") == nullptr);
    assert(request->FindFieldByName("remove_track") == nullptr);
    assert(response->FindFieldByName("track_unpublished") != nullptr);

    asio::io_context io;
    auto local = std::make_shared<livekit::LocalParticipant>(
        "PA_LOCAL", "local-user", [](const livekit::proto::SignalRequest&) {});

    // A participant cannot claim local-unpublish success unless its Room owns
    // the sender-removal and negotiation transaction.
    auto error = ExpectOperationError(io, local->UnpublishTrackAsync("TR_LOCAL"));
    assert(error.operation() == livekit::OperationKind::UnpublishTrack);
    assert(error.code() == livekit::OperationErrorCode::InvalidState);

    io.restart();
    bool handler_called = false;
    auto expected = std::make_shared<livekit::TrackPublication>(
        nullptr, "TR_LOCAL", "camera");
    local->SetAsyncUnpublishTrackHandler(
        [&handler_called, expected](const std::string& sid)
            -> asio::awaitable<std::shared_ptr<livekit::TrackPublication>> {
            assert(sid == "TR_LOCAL");
            handler_called = true;
            co_return expected;
        });
    auto delegated = asio::co_spawn(io,
        local->UnpublishTrackAsync("TR_LOCAL"), asio::use_future);
    io.run();
    assert(delegated.get() == expected);
    assert(handler_called);

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
    assert(error.operation() == livekit::OperationKind::UnpublishTrack);
    assert(error.code() == livekit::OperationErrorCode::InvalidState);
    assert(local->get_publication("TR_LOCAL") != nullptr);

    std::cout << "[SUCCESS] Local unpublish protocol gate and transaction validation passed."
              << std::endl;
    return 0;
}
