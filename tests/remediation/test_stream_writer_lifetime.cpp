// NEW-CPP_QT-001 regression, added by remediation-20260916T203449Z.
// Exercises production writers and the existing Room factories. The publisher
// is the existing packet boundary; no writer/finalization logic is duplicated.
// This is a new test, not a replacement or edit of the protected original tests.
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <io.h>

#include <asio.hpp>

#include "data_stream.h"
#include "room.h"
#include "tests/support/test_check.h"

namespace {

enum class WriterKind { Text, Byte };
enum class PacketKind { Header, Chunk, Trailer };

const std::string kStreamId = "ST_writer_lifetime";
const std::string kTopic = "lifetime.topic";
const std::string kReplyTo = "ST_reply_" + std::string(96, 'r');
const std::string kFileName = std::string(96, 'n') + ".bin";
const std::string kSender = "sender-lifetime";
const std::map<std::string, std::string> kAttributes{
    {"purpose", "lifetime"}, {"detail", std::string(96, 'm')}};
constexpr int kPublisherSentinel = 409;

[[noreturn]] void FailPurecall() {
    std::fputs("STREAM_WRITER_PURECALL: exit 86\n", stdout);
    std::fflush(stdout);
    std::_Exit(86);
}

[[noreturn]] void FailTerminate() noexcept {
    std::fputs("STREAM_WRITER_TERMINATE: exit 87\n", stdout);
    std::fflush(stdout);
    std::_Exit(87);
}

struct PublisherFailure : std::runtime_error {
    PublisherFailure() : std::runtime_error("publisher sentinel") {}
};

struct OuterFailure {};

PacketKind KindOf(const livekit::proto::DataPacket& packet) {
    if (packet.has_stream_header()) return PacketKind::Header;
    if (packet.has_stream_chunk()) return PacketKind::Chunk;
    TEST_CHECK(packet.has_stream_trailer());
    return PacketKind::Trailer;
}

struct PacketAttempt {
    livekit::proto::DataPacket packet;
    bool reliable;
};

struct PublisherTrace {
    // These are attempts, not acknowledgements of remote delivery. A throwing
    // attempt is deliberately retained so retries/duplicate closure are visible.
    std::vector<PacketAttempt> attempts;
    std::optional<PacketKind> fail_on;
    bool throw_nonstandard = false;
    int remaining_failures = 0;
    int thrown = 0;

    livekit::StreamPacketPublisher publisher() {
        return [this](const livekit::proto::DataPacket& packet, bool reliable) {
            attempts.push_back({packet, reliable});
            if (fail_on == KindOf(packet) && remaining_failures > 0) {
                --remaining_failures;
                ++thrown;
                if (throw_nonstandard) throw kPublisherSentinel;
                throw PublisherFailure();
            }
            return true;
        };
    }

    std::size_t count(PacketKind kind) const {
        std::size_t result = 0;
        for (const auto& attempt : attempts) {
            if (KindOf(attempt.packet) == kind) ++result;
        }
        return result;
    }
};

// Capture only one bounded destructor diagnostic at a time. Restoring stderr
// closes its pipe writer before reading, so no background thread or wait is used.
class StderrCapture {
public:
    StderrCapture() {
        std::fflush(stderr);
        TEST_CHECK(_pipe(pipe_, 4096, _O_BINARY) == 0);
        saved_ = _dup(_fileno(stderr));
        TEST_CHECK(saved_ >= 0);
        TEST_CHECK(_dup2(pipe_[1], _fileno(stderr)) == 0);
    }

    ~StderrCapture() {
        if (saved_ >= 0) restore();
        if (pipe_[0] >= 0) _close(pipe_[0]);
    }

    std::string finish() {
        restore();
        std::string result;
        char buffer[512];
        for (;;) {
            const int read = _read(pipe_[0], buffer, sizeof(buffer));
            TEST_CHECK(read >= 0);
            if (read == 0) break;
            result.append(buffer, static_cast<std::size_t>(read));
        }
        TEST_CHECK(_close(pipe_[0]) == 0);
        pipe_[0] = -1;
        return result;
    }

private:
    void restore() {
        std::fflush(stderr);
        TEST_CHECK(_dup2(saved_, _fileno(stderr)) == 0);
        TEST_CHECK(_close(saved_) == 0);
        saved_ = -1;
        TEST_CHECK(_close(pipe_[1]) == 0);
        pipe_[1] = -1;
    }

    int pipe_[2]{-1, -1};
    int saved_ = -1;
};

std::unique_ptr<livekit::BaseStreamWriter> MakeWriter(
    WriterKind kind, PublisherTrace& trace,
    std::optional<std::size_t> total_size = std::nullopt) {
    if (kind == WriterKind::Text) {
        return std::make_unique<livekit::TextStreamWriter>(
            trace.publisher(), kTopic, kAttributes, kStreamId, total_size,
            kReplyTo, std::vector<std::string>{"destination"}, kSender);
    }
    return std::make_unique<livekit::ByteStreamWriter>(
        trace.publisher(), kFileName, kTopic, kAttributes, kStreamId, total_size,
        "application/x-lifetime", std::vector<std::string>{"destination"}, kSender);
}

void Write(WriterKind kind, livekit::BaseStreamWriter& writer, const std::string& bytes) {
    if (kind == WriterKind::Text) {
        static_cast<livekit::TextStreamWriter&>(writer).Write(bytes);
    } else {
        static_cast<livekit::ByteStreamWriter&>(writer).Write(
            std::vector<uint8_t>(bytes.begin(), bytes.end()));
    }
}

void CheckInfo(WriterKind kind, const livekit::BaseStreamWriter& writer) {
    TEST_CHECK(writer.stream_id() == kStreamId);
    TEST_CHECK(writer.topic() == kTopic);
    TEST_CHECK(writer.timestamp_ms() > 0);
    TEST_CHECK(!writer.is_closed());
    if (kind == WriterKind::Text) {
        const auto& info = static_cast<const livekit::TextStreamWriter&>(writer).info();
        TEST_CHECK(info.reply_to_stream_id == kReplyTo);
        TEST_CHECK(info.sender_identity == kSender);
        TEST_CHECK(info.attributes == kAttributes);
    } else {
        const auto& info = static_cast<const livekit::ByteStreamWriter&>(writer).info();
        TEST_CHECK(info.name == kFileName);
        TEST_CHECK(info.sender_identity == kSender);
        TEST_CHECK(info.attributes == kAttributes);
    }
}

void CheckHeader(const livekit::proto::DataPacket& packet, WriterKind kind,
                 int64_t timestamp, std::optional<std::size_t> total_size) {
    TEST_CHECK(packet.has_stream_header());
    const auto& header = packet.stream_header();
    TEST_CHECK(header.stream_id() == kStreamId);
    TEST_CHECK(header.topic() == kTopic);
    TEST_CHECK(header.timestamp() == timestamp);
    TEST_CHECK(header.has_total_length() == total_size.has_value());
    if (total_size) TEST_CHECK(header.total_length() == *total_size);
    TEST_CHECK(header.attributes_size() == static_cast<int>(kAttributes.size()));
    for (const auto& [key, value] : kAttributes) {
        TEST_CHECK(header.attributes().at(key) == value);
    }
    if (kind == WriterKind::Text) {
        TEST_CHECK(header.mime_type() == "text/plain");
        TEST_CHECK(header.has_text_header());
        TEST_CHECK(!header.has_byte_header());
        TEST_CHECK(header.text_header().operation_type() ==
                   livekit::proto::DataStream::OperationType::DataStream_OperationType_CREATE);
        TEST_CHECK(header.text_header().reply_to_stream_id() == kReplyTo);
    } else {
        TEST_CHECK(header.mime_type() == "application/x-lifetime");
        TEST_CHECK(header.has_byte_header());
        TEST_CHECK(!header.has_text_header());
        TEST_CHECK(header.byte_header().name() == kFileName);
    }
}

void CheckSequence(const PublisherTrace& trace, WriterKind kind, int64_t timestamp,
                   const std::string& payload = {}, const std::string& reason = {},
                   const std::map<std::string, std::string>& trailer_attributes = {},
                   std::optional<std::size_t> total_size = std::nullopt) {
    const std::size_t chunks =
        (payload.size() + livekit::kStreamChunkSize - 1) / livekit::kStreamChunkSize;
    TEST_CHECK(trace.attempts.size() == chunks + 2);
    TEST_CHECK(trace.thrown == 0);
    for (const auto& attempt : trace.attempts) TEST_CHECK(attempt.reliable);
    CheckHeader(trace.attempts.front().packet, kind, timestamp, total_size);
    std::string reconstructed;
    for (std::size_t index = 0; index < chunks; ++index) {
        const auto& packet = trace.attempts[index + 1].packet;
        TEST_CHECK(packet.has_stream_chunk());
        const auto& chunk = packet.stream_chunk();
        TEST_CHECK(chunk.stream_id() == kStreamId);
        TEST_CHECK(chunk.chunk_index() == index);
        TEST_CHECK(!chunk.content().empty());
        TEST_CHECK(chunk.content().size() <= livekit::kStreamChunkSize);
        reconstructed += chunk.content();
    }
    TEST_CHECK(reconstructed == payload);
    const auto& packet = trace.attempts.back().packet;
    TEST_CHECK(packet.has_stream_trailer());
    TEST_CHECK(packet.stream_trailer().stream_id() == kStreamId);
    TEST_CHECK(packet.stream_trailer().reason() == reason);
    TEST_CHECK(packet.stream_trailer().attributes_size() ==
               static_cast<int>(trailer_attributes.size()));
    for (const auto& [key, value] : trailer_attributes) {
        TEST_CHECK(packet.stream_trailer().attributes().at(key) == value);
    }
}

void TestEmpty(WriterKind kind) {
    // First iteration is deliberately unwritten: unchanged production code must
    // reach the test-only purecall hook before any artificial workaround exists.
    for (const bool zero_write : {false, true}) {
        for (const auto total : {std::optional<std::size_t>{},
                                 std::optional<std::size_t>{0}}) {
            PublisherTrace trace;
            auto writer = MakeWriter(kind, trace, total);
            CheckInfo(kind, *writer);
            const auto timestamp = writer->timestamp_ms();
            TEST_CHECK(trace.attempts.empty());
            if (zero_write) {
                Write(kind, *writer, "");
                if (kind == WriterKind::Byte) {
                    static_cast<livekit::ByteStreamWriter&>(*writer).Write(nullptr, 0);
                }
            }
            TEST_CHECK(trace.attempts.empty());
            writer.reset();
            CheckSequence(trace, kind, timestamp, "", "", {}, total);
        }
    }
}

void TestExplicitCloseCancel() {
    for (const auto kind : {WriterKind::Text, WriterKind::Byte}) {
        for (int mode = 0; mode < 4; ++mode) {
            PublisherTrace trace;
            auto writer = MakeWriter(kind, trace);
            const auto timestamp = writer->timestamp_ms();
            std::string reason;
            std::map<std::string, std::string> trailing;
            if (mode == 0) {
                reason = "completed";
                trailing = {{"result", "complete"}, {"count", "0"}};
                writer->Close(reason, trailing);
            } else if (mode == 1) {
                reason = "cancelled";
                writer->Cancel();
            } else if (mode == 2) {
                reason = "cancelled";
                writer->Cancel("");
            } else {
                reason = "user-cancelled";
                writer->Cancel(reason);
            }
            TEST_CHECK(writer->is_closed());
            CheckSequence(trace, kind, timestamp, "", reason, trailing);
            writer->Close("must-not-replace", {{"unexpected", "value"}});
            writer->Cancel("must-not-replace");
            Write(kind, *writer, "must-not-be-written");
            writer.reset();
            CheckSequence(trace, kind, timestamp, "", reason, trailing);
        }
    }
}

void TestChunks() {
    for (const auto kind : {WriterKind::Text, WriterKind::Byte}) {
        for (const bool explicit_close : {false, true}) {
            PublisherTrace trace;
            std::string payload(livekit::kStreamChunkSize * 2 + 17, 'x');
            for (std::size_t i = 0; i < payload.size(); ++i) {
                payload[i] = static_cast<char>(kind == WriterKind::Text ? 'a' + i % 26 : i % 256);
            }
            auto writer = MakeWriter(kind, trace, payload.size());
            const auto timestamp = writer->timestamp_ms();
            TEST_CHECK(trace.attempts.empty());
            Write(kind, *writer, payload);
            TEST_CHECK(trace.count(PacketKind::Header) == 1);
            TEST_CHECK(trace.count(PacketKind::Chunk) == 3);
            TEST_CHECK(trace.count(PacketKind::Trailer) == 0);
            if (explicit_close) {
                writer->Close();
                writer->Close();
                TEST_CHECK(writer->is_closed());
            }
            writer.reset();
            CheckSequence(trace, kind, timestamp, payload, "", {}, payload.size());
        }
    }
}

void TestOuterUnwind() {
    for (const auto kind : {WriterKind::Text, WriterKind::Byte}) {
        for (const bool with_payload : {false, true}) {
            PublisherTrace trace;
            int64_t timestamp = 0;
            bool outer_caught = false;
            try {
                auto writer = MakeWriter(kind, trace);
                timestamp = writer->timestamp_ms();
                if (with_payload) Write(kind, *writer, "before-unwind");
                throw OuterFailure{};
            } catch (const OuterFailure&) {
                outer_caught = true;
            }
            TEST_CHECK(outer_caught);
            CheckSequence(trace, kind, timestamp, with_payload ? "before-unwind" : "");
        }
    }
}

template <typename Action>
void ExpectPublisherFailure(bool nonstandard, Action&& action) {
    bool caught = false;
    try {
        action();
    } catch (const PublisherFailure& error) {
        TEST_CHECK(!nonstandard);
        TEST_CHECK(std::string(error.what()) == "publisher sentinel");
        caught = true;
    } catch (int error) {
        TEST_CHECK(nonstandard);
        TEST_CHECK(error == kPublisherSentinel);
        caught = true;
    }
    TEST_CHECK(caught);
}

void TestExplicitPublisherFailures() {
    for (const auto kind : {WriterKind::Text, WriterKind::Byte}) {
        for (const bool nonstandard : {false, true}) {
            for (const auto point : {PacketKind::Header, PacketKind::Trailer}) {
                for (const bool cancel : {false, true}) {
                    PublisherTrace trace;
                    trace.fail_on = point;
                    trace.throw_nonstandard = nonstandard;
                    trace.remaining_failures = 1;
                    auto writer = MakeWriter(kind, trace);
                    ExpectPublisherFailure(nonstandard, [&] {
                        if (cancel) writer->Cancel();
                        else writer->Close();
                    });
                    TEST_CHECK(trace.thrown == 1);
                    TEST_CHECK(writer->is_closed());
                    const auto attempts = trace.attempts.size();
                    writer->Close();
                    writer->Cancel();
                    writer.reset();
                    TEST_CHECK(trace.attempts.size() == attempts);
                    TEST_CHECK(trace.count(PacketKind::Header) == 1);
                    TEST_CHECK(trace.count(PacketKind::Trailer) ==
                               (point == PacketKind::Trailer ? 1 : 0));
                }
            }
            for (const auto point : {PacketKind::Header, PacketKind::Chunk}) {
                PublisherTrace trace;
                trace.fail_on = point;
                trace.throw_nonstandard = nonstandard;
                trace.remaining_failures = 1;
                bool operation_caught = false;
                try {
                    auto writer = MakeWriter(kind, trace);
                    // Destruction occurs during the publisher exception unwind.
                    Write(kind, *writer, "write-failure");
                } catch (const PublisherFailure&) {
                    TEST_CHECK(!nonstandard);
                    operation_caught = true;
                } catch (int error) {
                    TEST_CHECK(nonstandard && error == kPublisherSentinel);
                    operation_caught = true;
                }
                TEST_CHECK(operation_caught);
                TEST_CHECK(trace.thrown == 1);
                TEST_CHECK(trace.count(PacketKind::Trailer) == 1);
                TEST_CHECK(trace.count(PacketKind::Header) ==
                           (point == PacketKind::Header ? 2 : 1));
                TEST_CHECK(trace.count(PacketKind::Chunk) ==
                           (point == PacketKind::Chunk ? 1 : 0));
            }
        }
    }
}

void TestDestructorPublisherFailures() {
    for (const auto kind : {WriterKind::Text, WriterKind::Byte}) {
        for (const bool nonstandard : {false, true}) {
            for (const auto point : {PacketKind::Header, PacketKind::Trailer}) {
                for (const bool outer_unwind : {false, true}) {
                    for (const bool with_payload : {false, true}) {
                        // A header failure here must be emitted by destruction,
                        // not by the optional earlier Write.
                        if (point == PacketKind::Header && with_payload) continue;
                        PublisherTrace trace;
                        trace.fail_on = point;
                        trace.throw_nonstandard = nonstandard;
                        trace.remaining_failures = 1;
                        bool outer_caught = false;
                        StderrCapture diagnostic;
                        try {
                            auto writer = MakeWriter(kind, trace);
                            if (with_payload) Write(kind, *writer, "before-close");
                            if (outer_unwind) throw OuterFailure{};
                        } catch (const OuterFailure&) {
                            outer_caught = true;
                        }
                        const auto message = diagnostic.finish();
                        TEST_CHECK(outer_caught == outer_unwind);
                        TEST_CHECK(trace.thrown == 1);
                        TEST_CHECK(!message.empty());
                        TEST_CHECK(trace.count(PacketKind::Header) == 1);
                        TEST_CHECK(trace.count(PacketKind::Chunk) == (with_payload ? 1 : 0));
                        TEST_CHECK(trace.count(PacketKind::Trailer) ==
                                   (point == PacketKind::Trailer ? 1 : 0));
                    }
                }
            }
        }
    }
}

void TestRoomFactories() {
    asio::io_context io;
    auto room = livekit::Room::Create(io.get_executor());
    std::weak_ptr<livekit::Room> weak_room = room;
    {
        auto text = room->CreateTextStreamWriter();
        auto bytes = room->CreateByteStreamWriter("empty.bin");
        TEST_CHECK(!text->stream_id().empty());
        TEST_CHECK(!bytes->stream_id().empty());
        TEST_CHECK(!text->is_closed());
        TEST_CHECK(!bytes->is_closed());
    }
    auto text = room->CreateTextStreamWriter();
    auto bytes = room->CreateByteStreamWriter("orphan.bin");
    room.reset();
    TEST_CHECK(weak_room.expired());
    text.reset();
    bytes.reset();
    TEST_CHECK(weak_room.expired());
    // Offline fallback and bool-false delivery policy belong to Phase 3A; this
    // smoke verifies only real factory/destructor lifetime and weak ownership.
}

} // namespace

int main(int argc, char** argv) {
    static_assert(std::is_nothrow_destructible_v<livekit::TextStreamWriter>);
    static_assert(std::is_nothrow_destructible_v<livekit::ByteStreamWriter>);
    _set_purecall_handler(&FailPurecall);
    std::set_terminate(&FailTerminate);

    const std::string selected = argc == 3 && std::string(argv[1]) == "--case"
                                     ? argv[2] : "all";
    TEST_CHECK(argc == 1 || (argc == 3 && std::string(argv[1]) == "--case"));
    struct TestCase { const char* name; std::function<void()> run; };
    const std::vector<TestCase> cases{
        {"empty-text", [] { TestEmpty(WriterKind::Text); }},
        {"empty-byte", [] { TestEmpty(WriterKind::Byte); }},
        {"explicit", TestExplicitCloseCancel},
        {"chunks", TestChunks},
        {"unwind", TestOuterUnwind},
        {"explicit-exceptions", TestExplicitPublisherFailures},
        {"destructor-exceptions", TestDestructorPublisherFailures},
        {"room-factory", TestRoomFactories},
    };
    int executed = 0;
    for (const auto& test : cases) {
        if (selected != "all" && selected != test.name) continue;
        std::printf("[RUN] stream writer lifetime: %s\n", test.name);
        std::fflush(stdout);
        test.run();
        ++executed;
        std::printf("[PASS] stream writer lifetime: %s\n", test.name);
    }
    TEST_CHECK(executed > 0);
    TEST_CHECK(selected != "all" || executed == 8);
    std::printf("[SUCCESS] stream writer lifetime groups executed=%d\n", executed);
    return 0;
}
