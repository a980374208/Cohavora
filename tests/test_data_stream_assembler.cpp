#include "tests/support/test_check.h"
#include <chrono>
#include <cstdint>
#include <future>
#include <iostream>
#include <latch>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

#include "data_stream.h"
#include "data_stream_assembler.h"
#include "room.h"

namespace livekit {

class RoomStreamDeliveryTestAccess final {
public:
    static void InstallSession(
        Room& room,
        uint64_t generation,
        DataStreamReaderBudget::Limits reader_limits,
        IncomingDataStreamAssembler::Limits assembler_limits = {}) {
        std::lock_guard lock(room.room_mutex_);
        room.session_generation_.store(generation, std::memory_order_release);
        room.installed_session_generation_ = generation;
        room.incoming_reader_budget_ =
            std::make_shared<DataStreamReaderBudget>(reader_limits);
        room.incoming_data_streams_ =
            std::make_unique<IncomingDataStreamAssembler>(assembler_limits);
    }

    static void AdvanceSession(
        Room& room,
        uint64_t generation,
        DataStreamReaderBudget::Limits reader_limits,
        IncomingDataStreamAssembler::Limits assembler_limits = {}) {
        std::lock_guard lock(room.room_mutex_);
        (void)room.TakePendingOperationsLocked();
        room.session_generation_.store(generation, std::memory_order_release);
        room.installed_session_generation_ = generation;
        room.incoming_reader_budget_ =
            std::make_shared<DataStreamReaderBudget>(reader_limits);
        room.incoming_data_streams_ =
            std::make_unique<IncomingDataStreamAssembler>(assembler_limits);
    }

    static void Dispatch(Room& room,
                         const proto::DataPacket& packet,
                         uint64_t generation) {
        std::string encoded;
        TEST_CHECK(packet.SerializeToString(&encoded));
        room.OnIncomingDataPacket(
            std::vector<uint8_t>(encoded.begin(), encoded.end()),
            "PA_READER", "", generation);
    }

    static std::shared_ptr<DataStreamReaderBudget> Budget(const Room& room) {
        std::lock_guard lock(room.room_mutex_);
        return room.incoming_reader_budget_;
    }

    static bool AssemblerContains(const Room& room,
                                  const std::string& stream_id) {
        std::lock_guard lock(room.room_mutex_);
        return room.incoming_data_streams_->Contains(stream_id);
    }

    static IncomingDataStreamAssembler::TimePoint Deadline(
        const Room& room,
        const std::string& stream_id) {
        std::lock_guard lock(room.room_mutex_);
        return room.incoming_stream_deadlines_.at(stream_id);
    }

    static bool HasCleanupTimer(const Room& room) {
        std::lock_guard lock(room.room_mutex_);
        return room.incoming_stream_cleanup_timer_ != nullptr;
    }

    static size_t PurgeAt(
        Room& room,
        IncomingDataStreamAssembler::TimePoint now,
        uint64_t generation) {
        std::lock_guard lock(room.room_mutex_);
        const auto removed =
            room.PurgeIncomingStreamsLocked(now, generation);
        room.ScheduleIncomingStreamCleanupLocked(generation);
        return removed;
    }
};

} // namespace livekit

namespace {

std::span<const uint8_t> Bytes(std::string_view value) {
    return {
        reinterpret_cast<const uint8_t*>(value.data()),
        value.size()
    };
}

template <typename Predicate>
bool WaitUntil(Predicate&& predicate) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

livekit::proto::DataPacket TextHeader(
    const std::string& stream_id,
    std::optional<uint64_t> total_length = std::nullopt) {
    livekit::proto::DataPacket packet;
    auto* header = packet.mutable_stream_header();
    header->set_stream_id(stream_id);
    header->set_topic("reader-budget");
    header->mutable_text_header();
    if (total_length.has_value()) header->set_total_length(*total_length);
    return packet;
}

livekit::proto::DataPacket StreamChunk(const std::string& stream_id,
                                       uint64_t index,
                                       std::string content) {
    livekit::proto::DataPacket packet;
    auto* chunk = packet.mutable_stream_chunk();
    chunk->set_stream_id(stream_id);
    chunk->set_chunk_index(index);
    chunk->set_content(std::move(content));
    return packet;
}

livekit::proto::DataPacket StreamTrailer(
    const std::string& stream_id,
    std::string reason = {}) {
    livekit::proto::DataPacket packet;
    auto* trailer = packet.mutable_stream_trailer();
    trailer->set_stream_id(stream_id);
    trailer->set_reason(std::move(reason));
    return packet;
}

class ReaderTrace final : public livekit::RoomListener {
public:
    void OnTextStreamOpened(
        std::shared_ptr<livekit::TextStreamReader> reader,
        std::shared_ptr<livekit::Participant>) override {
        text_readers.push_back(std::move(reader));
    }

    std::vector<std::shared_ptr<livekit::TextStreamReader>> text_readers;
};

void TestReaderBudgets() {
    using livekit::ByteStreamInfo;
    using livekit::ByteStreamReader;
    using livekit::DataStreamReaderBudget;
    using livekit::TextStreamInfo;
    using livekit::TextStreamReader;

    DataStreamReaderBudget::Limits limits;
    limits.max_active_readers = 4;
    limits.max_buffered_bytes_per_reader = 4;
    limits.max_buffered_bytes = 8;
    limits.max_queued_chunks_per_reader = 2;
    limits.max_read_all_bytes = 5;
    auto budget = std::make_shared<DataStreamReaderBudget>(limits);

    TextStreamInfo streaming_info;
    streaming_info.stream_id = "streaming-text";
    TextStreamReader streaming(std::move(streaming_info), budget);
    TEST_CHECK(streaming.TryOnChunkUpdate("abcd"));
    TEST_CHECK(streaming.buffered_bytes() == 4);
    TEST_CHECK(budget->buffered_bytes() == 4);
    std::string text;
    TEST_CHECK(streaming.ReadNext(text));
    TEST_CHECK(text == "abcd");
    TEST_CHECK(streaming.buffered_bytes() == 0);
    TEST_CHECK(budget->buffered_bytes() == 0);
    TEST_CHECK(streaming.TryOnChunkUpdate("efgh"));
    streaming.OnStreamClose("", {});
    TEST_CHECK(!streaming.is_failed());
    TEST_CHECK(streaming.ReadNext(text));
    TEST_CHECK(text == "efgh");
    TEST_CHECK(!streaming.ReadNext(text));

    ByteStreamInfo byte_info;
    byte_info.stream_id = "bounded-byte";
    ByteStreamReader bytes(std::move(byte_info), budget);
    const std::vector<uint8_t> boundary{1, 2, 3, 4};
    TEST_CHECK(bytes.TryOnChunkUpdate(boundary.data(), boundary.size()));
    std::vector<uint8_t> byte_chunk;
    TEST_CHECK(bytes.ReadNext(byte_chunk));
    TEST_CHECK(byte_chunk == boundary);
    const std::vector<uint8_t> oversized{1, 2, 3, 4, 5};
    TEST_CHECK(!bytes.TryOnChunkUpdate(oversized.data(), oversized.size()));
    TEST_CHECK(bytes.is_failed());
    TEST_CHECK(bytes.close_reason() == livekit::kDataStreamBufferLimitExceeded);
    bytes.OnStreamClose("later-close", {});
    TEST_CHECK(bytes.close_reason() == livekit::kDataStreamBufferLimitExceeded);

    DataStreamReaderBudget::Limits chunk_limits = limits;
    chunk_limits.max_active_readers = 1;
    chunk_limits.max_buffered_bytes_per_reader = 8;
    chunk_limits.max_buffered_bytes = 8;
    chunk_limits.max_queued_chunks_per_reader = 2;
    auto chunk_budget =
        std::make_shared<DataStreamReaderBudget>(chunk_limits);
    TextStreamInfo chunk_info;
    chunk_info.stream_id = "chunk-limited";
    TextStreamReader chunk_limited(std::move(chunk_info), chunk_budget);
    TEST_CHECK(chunk_limited.TryOnChunkUpdate("a"));
    TEST_CHECK(chunk_limited.TryOnChunkUpdate("b"));
    TEST_CHECK(!chunk_limited.TryOnChunkUpdate("c"));
    TEST_CHECK(chunk_limited.close_reason() ==
               livekit::kDataStreamChunkLimitExceeded);
    TEST_CHECK(chunk_limited.ReadNext(text));
    TEST_CHECK(chunk_limited.ReadNext(text));
    TEST_CHECK(!chunk_limited.ReadNext(text));
    TEST_CHECK(chunk_budget->buffered_bytes() == 0);

    DataStreamReaderBudget::Limits active_limits = limits;
    active_limits.max_active_readers = 2;
    active_limits.max_buffered_bytes = 6;
    auto shared = std::make_shared<DataStreamReaderBudget>(active_limits);
    TextStreamInfo first_info;
    first_info.stream_id = "first";
    auto first = std::make_shared<TextStreamReader>(first_info, shared);
    TextStreamInfo second_info;
    second_info.stream_id = "second";
    auto second = std::make_shared<TextStreamReader>(second_info, shared);
    TextStreamInfo rejected_info;
    rejected_info.stream_id = "active-rejected";
    auto rejected = std::make_shared<TextStreamReader>(rejected_info, shared);
    TEST_CHECK(rejected->is_failed());
    TEST_CHECK(rejected->close_reason() ==
               livekit::kDataStreamActiveReaderLimitExceeded);
    TEST_CHECK(shared->active_readers() == 2);

    TEST_CHECK(first->TryOnChunkUpdate("1234"));
    first->OnStreamClose("", {});
    TEST_CHECK(shared->active_readers() == 1);
    TEST_CHECK(shared->buffered_bytes() == 4);
    TextStreamInfo replacement_info;
    replacement_info.stream_id = "replacement";
    auto replacement =
        std::make_shared<TextStreamReader>(replacement_info, shared);
    TEST_CHECK(replacement->admitted());
    TEST_CHECK(!replacement->TryOnChunkUpdate("abc"));
    TEST_CHECK(replacement->close_reason() ==
               livekit::kDataStreamBufferLimitExceeded);
    TEST_CHECK(shared->buffered_bytes() == 4);
    TEST_CHECK(first->ReadNext(text));
    TEST_CHECK(shared->buffered_bytes() == 0);
    TextStreamInfo reused_info;
    reused_info.stream_id = "reused";
    auto reused = std::make_shared<TextStreamReader>(reused_info, shared);
    TEST_CHECK(reused->TryOnChunkUpdate("abc"));
    reused->OnStreamClose("", {});
    second->OnStreamClose("", {});
    TEST_CHECK(shared->active_readers() == 0);
    TEST_CHECK(reused->ReadNext(text));
    TEST_CHECK(shared->buffered_bytes() == 0);

    DataStreamReaderBudget::Limits read_all_limits = limits;
    read_all_limits.max_active_readers = 1;
    read_all_limits.max_buffered_bytes = 8;
    read_all_limits.max_read_all_bytes = 5;
    auto read_all_budget =
        std::make_shared<DataStreamReaderBudget>(read_all_limits);
    TextStreamInfo read_all_info;
    read_all_info.stream_id = "read-all";
    TextStreamReader read_all_reader(std::move(read_all_info), read_all_budget);
    std::latch read_all_started(1);
    auto read_all_result = std::async(std::launch::async, [&] {
        read_all_started.count_down();
        try {
            (void)read_all_reader.ReadAll();
        } catch (const std::length_error& error) {
            return std::string(error.what());
        }
        return std::string("no error");
    });
    read_all_started.wait();
    TEST_CHECK(read_all_reader.TryOnChunkUpdate("abc"));
    TEST_CHECK(WaitUntil([&] { return read_all_reader.buffered_bytes() == 0; }));
    TEST_CHECK(read_all_reader.TryOnChunkUpdate("def"));
    TEST_CHECK(WaitUntil([&] { return read_all_reader.is_closed(); }));
    TEST_CHECK(read_all_result.get() == livekit::kDataStreamReadAllLimitExceeded);
    TEST_CHECK(read_all_reader.is_failed());
    TEST_CHECK(read_all_reader.close_reason() ==
               livekit::kDataStreamReadAllLimitExceeded);
    TEST_CHECK(!read_all_reader.TryOnChunkUpdate("late"));
    TEST_CHECK(read_all_budget->active_readers() == 0);
    TEST_CHECK(read_all_budget->buffered_bytes() == 0);

    DataStreamReaderBudget::Limits aggregate_limits = limits;
    aggregate_limits.max_active_readers = 2;
    aggregate_limits.max_buffered_bytes_per_reader = 5;
    aggregate_limits.max_buffered_bytes = 5;
    aggregate_limits.max_read_all_bytes = 10;
    auto aggregate_budget =
        std::make_shared<DataStreamReaderBudget>(aggregate_limits);
    TextStreamInfo accumulator_info;
    accumulator_info.stream_id = "accumulator";
    TextStreamReader accumulator(std::move(accumulator_info), aggregate_budget);
    TextStreamInfo competitor_info;
    competitor_info.stream_id = "competitor";
    TextStreamReader competitor(std::move(competitor_info), aggregate_budget);
    std::latch accumulator_started(1);
    auto accumulated = std::async(std::launch::async, [&] {
        accumulator_started.count_down();
        return accumulator.ReadAll();
    });
    accumulator_started.wait();
    TEST_CHECK(accumulator.TryOnChunkUpdate("1234"));
    TEST_CHECK(WaitUntil([&] { return accumulator.buffered_bytes() == 0; }));
    TEST_CHECK(aggregate_budget->buffered_bytes() == 4);
    TEST_CHECK(!competitor.TryOnChunkUpdate("ab"));
    TEST_CHECK(competitor.close_reason() ==
               livekit::kDataStreamBufferLimitExceeded);
    accumulator.OnStreamClose("", {});
    TEST_CHECK(accumulated.get() == "1234");
    TEST_CHECK(aggregate_budget->buffered_bytes() == 0);

    DataStreamReaderBudget::Limits huge_limits = limits;
    huge_limits.max_active_readers = 1;
    huge_limits.max_buffered_bytes_per_reader = 8;
    huge_limits.max_buffered_bytes = 8;
    huge_limits.max_read_all_bytes = 8;
    auto huge_budget = std::make_shared<DataStreamReaderBudget>(huge_limits);
    ByteStreamInfo huge_info;
    huge_info.stream_id = "huge-declaration";
    huge_info.total_length = std::numeric_limits<size_t>::max();
    ByteStreamReader huge(std::move(huge_info), huge_budget);
    const uint8_t one = 42;
    TEST_CHECK(huge.TryOnChunkUpdate(&one, 1));
    huge.OnStreamClose("cancelled", {});
    const auto small_result = huge.ReadAll();
    TEST_CHECK(small_result == std::vector<uint8_t>{42});
    TEST_CHECK(small_result.capacity() <= huge_limits.max_read_all_bytes);

    DataStreamReaderBudget::Limits byte_all_limits = limits;
    byte_all_limits.max_active_readers = 1;
    byte_all_limits.max_buffered_bytes_per_reader = 6;
    byte_all_limits.max_buffered_bytes = 6;
    byte_all_limits.max_read_all_bytes = 5;
    auto byte_all_budget =
        std::make_shared<DataStreamReaderBudget>(byte_all_limits);
    ByteStreamInfo byte_all_info;
    byte_all_info.stream_id = "byte-read-all";
    ByteStreamReader byte_all(std::move(byte_all_info), byte_all_budget);
    const std::vector<uint8_t> three{1, 2, 3};
    TEST_CHECK(byte_all.TryOnChunkUpdate(three.data(), three.size()));
    TEST_CHECK(byte_all.TryOnChunkUpdate(three.data(), three.size()));
    byte_all.OnStreamClose("", {});
    bool byte_all_threw = false;
    try {
        (void)byte_all.ReadAll();
    } catch (const std::length_error& error) {
        byte_all_threw = std::string(error.what()) ==
                         livekit::kDataStreamReadAllLimitExceeded;
    }
    TEST_CHECK(byte_all_threw);
    TEST_CHECK(byte_all.close_reason() ==
               livekit::kDataStreamReadAllLimitExceeded);
    TEST_CHECK(byte_all_budget->buffered_bytes() == 0);

    TextStreamInfo truncated_info;
    truncated_info.stream_id = "truncated";
    truncated_info.total_length = 4;
    TextStreamReader truncated(std::move(truncated_info));
    TEST_CHECK(truncated.TryOnChunkUpdate("abc"));
    truncated.OnStreamClose("", {});
    TEST_CHECK(truncated.is_failed());
    TEST_CHECK(truncated.close_reason() == livekit::kDataStreamLengthMismatch);
    bool truncated_threw = false;
    try {
        (void)truncated.ReadAll();
    } catch (const std::runtime_error& error) {
        truncated_threw =
            std::string(error.what()) == livekit::kDataStreamLengthMismatch;
    }
    TEST_CHECK(truncated_threw);

    TextStreamInfo empty_info;
    empty_info.stream_id = "empty-unknown";
    TextStreamReader empty(std::move(empty_info));
    empty.OnStreamClose("", {});
    TEST_CHECK(empty.ReadAll().empty());
}

void TestRoomReaderLifecycle() {
    livekit::DataStreamReaderBudget::Limits reader_limits;
    reader_limits.max_active_readers = 4;
    reader_limits.max_buffered_bytes_per_reader = 4;
    reader_limits.max_buffered_bytes = 8;
    reader_limits.max_queued_chunks_per_reader = 4;
    reader_limits.max_read_all_bytes = 6;
    reader_limits.stream_ttl = std::chrono::seconds(2);

    livekit::IncomingDataStreamAssembler::Limits assembler_limits;
    assembler_limits.max_streams = 4;
    assembler_limits.max_stream_size = 4;
    assembler_limits.max_buffered_bytes = 8;
    assembler_limits.max_chunks_per_stream = 4;
    assembler_limits.stream_ttl = std::chrono::seconds(2);
    assembler_limits.cleanup_interval = std::chrono::seconds(1);

    asio::io_context io;
    auto room = livekit::Room::Create(io.get_executor());
    auto trace = std::make_shared<ReaderTrace>();
    room->AddListener(trace);
    livekit::RoomStreamDeliveryTestAccess::InstallSession(
        *room, 1, reader_limits, assembler_limits);
    auto budget = livekit::RoomStreamDeliveryTestAccess::Budget(*room);

    // The legacy assembler rejects this declared size, but the modern Reader
    // remains valid and is governed by current-buffer and declared-length rules.
    livekit::RoomStreamDeliveryTestAccess::Dispatch(
        *room, TextHeader("modern-only", 5), 1);
    TEST_CHECK(trace->text_readers.size() == 1);
    auto modern_only = trace->text_readers.back();
    TEST_CHECK(!livekit::RoomStreamDeliveryTestAccess::AssemblerContains(
        *room, "modern-only"));
    TEST_CHECK(!modern_only->is_closed());
    livekit::RoomStreamDeliveryTestAccess::Dispatch(
        *room, StreamChunk("modern-only", 0, "abcd"), 1);
    TEST_CHECK(modern_only->buffered_bytes() == 4);
    livekit::RoomStreamDeliveryTestAccess::Dispatch(
        *room, StreamChunk("modern-only", 1, "ef"), 1);
    TEST_CHECK(modern_only->is_failed());
    TEST_CHECK(modern_only->close_reason() ==
               livekit::kDataStreamDeclaredLengthExceeded);
    TEST_CHECK(budget->active_readers() == 0);
    std::string text;
    TEST_CHECK(modern_only->ReadNext(text));
    TEST_CHECK(text == "abcd");
    TEST_CHECK(!modern_only->ReadNext(text));
    TEST_CHECK(budget->buffered_bytes() == 0);

    // Once admitted, an assembler rejection is terminal for the paired Reader
    // and wakes a consumer without exposing a truncated normal stream.
    livekit::RoomStreamDeliveryTestAccess::Dispatch(
        *room, TextHeader("assembler-reject", 4), 1);
    auto assembler_reject = trace->text_readers.back();
    TEST_CHECK(livekit::RoomStreamDeliveryTestAccess::AssemblerContains(
        *room, "assembler-reject"));
    std::latch blocked_started(1);
    auto blocked_read = std::async(std::launch::async, [&] {
        blocked_started.count_down();
        std::string value;
        return assembler_reject->ReadNext(value);
    });
    blocked_started.wait();
    livekit::RoomStreamDeliveryTestAccess::Dispatch(
        *room, StreamChunk("assembler-reject", 0, "abcde"), 1);
    TEST_CHECK(!blocked_read.get());
    TEST_CHECK(assembler_reject->is_failed());
    TEST_CHECK(assembler_reject->close_reason() ==
               livekit::kDataStreamAssemblyRejected);
    TEST_CHECK(budget->buffered_bytes() == 0);

    // Production schedules a timer at admission; the same cleanup path is
    // invoked here with controlled time, so no packet or sleep is required.
    livekit::RoomStreamDeliveryTestAccess::Dispatch(
        *room, TextHeader("expires-without-packet"), 1);
    auto expiring = trace->text_readers.back();
    TEST_CHECK(livekit::RoomStreamDeliveryTestAccess::HasCleanupTimer(*room));
    const auto deadline = livekit::RoomStreamDeliveryTestAccess::Deadline(
        *room, "expires-without-packet");
    TEST_CHECK(livekit::RoomStreamDeliveryTestAccess::PurgeAt(
                   *room, deadline, 1) == 1);
    TEST_CHECK(expiring->is_failed());
    TEST_CHECK(expiring->close_reason() == livekit::kDataStreamExpired);

    livekit::RoomStreamDeliveryTestAccess::Dispatch(
        *room, TextHeader("replace"), 1);
    auto replaced = trace->text_readers.back();
    livekit::RoomStreamDeliveryTestAccess::Dispatch(
        *room, StreamChunk("replace", 0, "old"), 1);
    livekit::RoomStreamDeliveryTestAccess::Dispatch(
        *room, TextHeader("replace"), 1);
    auto replacement = trace->text_readers.back();
    TEST_CHECK(replacement != replaced);
    TEST_CHECK(replaced->close_reason() == livekit::kDataStreamReplaced);
    livekit::RoomStreamDeliveryTestAccess::Dispatch(
        *room, StreamTrailer("replace"), 1);
    TEST_CHECK(replacement->is_closed());
    TEST_CHECK(!replacement->is_failed());

    livekit::RoomStreamDeliveryTestAccess::Dispatch(
        *room, TextHeader("old-session"), 1);
    auto old_session_reader = trace->text_readers.back();
    livekit::RoomStreamDeliveryTestAccess::Dispatch(
        *room, StreamChunk("old-session", 0, "old"), 1);
    auto old_budget = budget;
    livekit::RoomStreamDeliveryTestAccess::AdvanceSession(
        *room, 2, reader_limits, assembler_limits);
    auto new_budget = livekit::RoomStreamDeliveryTestAccess::Budget(*room);
    TEST_CHECK(old_session_reader->is_closed());
    TEST_CHECK(old_session_reader->close_reason() == "session closed");
    TEST_CHECK(old_budget->active_readers() == 0);
    TEST_CHECK(old_budget->buffered_bytes() == 6);
    TEST_CHECK(new_budget->buffered_bytes() == 0);

    livekit::RoomStreamDeliveryTestAccess::Dispatch(
        *room, StreamChunk("old-session", 1, "late"), 1);
    TEST_CHECK(new_budget->buffered_bytes() == 0);
    livekit::RoomStreamDeliveryTestAccess::Dispatch(
        *room, TextHeader("new-session"), 2);
    auto new_session_reader = trace->text_readers.back();
    TEST_CHECK(new_session_reader->admitted());
    TEST_CHECK(new_budget->active_readers() == 1);
    livekit::RoomStreamDeliveryTestAccess::Dispatch(
        *room, StreamTrailer("new-session"), 2);
    TEST_CHECK(new_budget->active_readers() == 0);

    TEST_CHECK(replaced->ReadNext(text));
    TEST_CHECK(text == "old");
    TEST_CHECK(old_session_reader->ReadNext(text));
    TEST_CHECK(text == "old");
    TEST_CHECK(old_budget->buffered_bytes() == 0);
    livekit::RoomStreamDeliveryTestAccess::AdvanceSession(
        *room, 3, reader_limits, assembler_limits);
}

} // namespace

int main() {
    using livekit::IncomingDataStreamAssembler;

    const auto start = IncomingDataStreamAssembler::Clock::now();

    livekit::TextStreamInfo bounded_info;
    bounded_info.stream_id = "declared-length-overrun";
    bounded_info.total_length = 4;
    livekit::TextStreamReader bounded_reader(std::move(bounded_info));
    bounded_reader.OnChunkUpdate("abcde");
    TEST_CHECK(bounded_reader.is_closed());
    TEST_CHECK(bounded_reader.close_reason() ==
               "data stream declared length exceeded");

    TestReaderBudgets();
    TestRoomReaderLifecycle();

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
