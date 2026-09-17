#include "tests/support/test_check.h"
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string_view>

#include "data_stream_assembler.h"

namespace {

std::span<const uint8_t> Bytes(std::string_view value) {
    return {
        reinterpret_cast<const uint8_t*>(value.data()),
        value.size()
    };
}

} // namespace

int main() {
    using livekit::IncomingDataStreamAssembler;

    const auto start = IncomingDataStreamAssembler::Clock::now();

    // Out-of-order chunks are accepted but only emitted once all contiguous
    // chunk indexes and the declared byte count are present.
    IncomingDataStreamAssembler assembler;
    TEST_CHECK(assembler.Begin("stream-1", "topic", 6, "alice", "PA_1", start));
    TEST_CHECK(!assembler.AddChunk("stream-1", 1, Bytes("def"), start));
    auto completed = assembler.AddChunk("stream-1", 0, Bytes("abc"), start);
    TEST_CHECK(completed);
    TEST_CHECK(std::string(completed->payload.begin(), completed->payload.end()) == "abcdef");
    TEST_CHECK(completed->topic == "topic");
    TEST_CHECK(completed->sender_identity == "alice");
    TEST_CHECK(completed->sender_sid == "PA_1");
    TEST_CHECK(assembler.active_streams() == 0);
    TEST_CHECK(assembler.buffered_bytes() == 0);

    // Duplicate chunks must not inflate the received byte count.
    TEST_CHECK(assembler.Begin("stream-2", "topic", 6, {}, {}, start));
    TEST_CHECK(!assembler.AddChunk("stream-2", 0, Bytes("abc"), start));
    TEST_CHECK(!assembler.AddChunk("stream-2", 0, Bytes("abc"), start));
    completed = assembler.AddChunk("stream-2", 1, Bytes("def"), start);
    TEST_CHECK(completed);

    // Matching byte counts are not enough: gaps in chunk indexes are rejected.
    TEST_CHECK(assembler.Begin("stream-3", "topic", 3, {}, {}, start));
    TEST_CHECK(!assembler.AddChunk("stream-3", 1, Bytes("abc"), start));
    TEST_CHECK(!assembler.Finish("stream-3", start));
    TEST_CHECK(assembler.active_streams() == 0);

    IncomingDataStreamAssembler::Limits limits;
    limits.max_stream_size = 4;
    limits.max_buffered_bytes = 4;
    limits.stream_ttl = std::chrono::seconds(2);
    limits.cleanup_interval = std::chrono::seconds(1);
    IncomingDataStreamAssembler limited(limits);

    TEST_CHECK(!limited.Begin("oversized", "topic", 5, {}, {}, start));
    TEST_CHECK(limited.Begin("expiring", "topic", 4, {}, {}, start));
    TEST_CHECK(!limited.AddChunk("expiring", 0, Bytes("ab"), start));
    TEST_CHECK(limited.buffered_bytes() == 2);
    TEST_CHECK(limited.PurgeExpired(start + std::chrono::seconds(2)) == 1);
    TEST_CHECK(limited.buffered_bytes() == 0);

    std::cout << "[PASS] IncomingDataStreamAssembler tests passed\n";
    return 0;
}
