#include "core/room.h"
#include "tests/support/test_check.h"
#include <asio.hpp>
#include <iostream>
#include <thread>

#if defined(COHAVORA_E2EE_MEDIA_GUARD)
#error This gate must compile Room without the backend availability definition.
#endif

int main() {
    asio::io_context io;
    auto work = asio::make_work_guard(io);
    std::thread worker([&] { io.run(); });
    for (const auto type : {livekit::EncryptionType::GCM, livekit::EncryptionType::CUSTOM}) {
        auto room = livekit::Room::Create(io.get_executor());
        bool rejected = false;
        try { room->EnableE2ee({type, {}}); }
        catch (const livekit::OperationError& error) {
            rejected = error.code() == livekit::OperationErrorCode::EncryptionFailed &&
                error.stage() == "e2ee_backend_unavailable";
        }
        TEST_CHECK(rejected);
        TEST_CHECK(!room->e2ee_manager());
        room->Retire();
    }
    work.reset(); worker.join();
    std::cout << "E2EE_BACKEND_UNAVAILABLE PASS: rejected before manager creation or connect\n";
}
