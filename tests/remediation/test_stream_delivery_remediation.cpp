#include <asio.hpp>
#include <openssl/sha.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "api/make_ref_counted.h"
#include "data_stream.h"
#include "operation.h"
#include "room.h"
#include "tests/support/test_check.h"

// The bundled headers expose the newer convenience overload while the bundled
// WebRTC library predates its default definition. Production does not call it;
// provide the header-specified forwarding behavior so the test double's base
// vtable remains linkable.
namespace webrtc {

void DataChannelInterface::SendAsync(
    DataBuffer buffer,
    absl::AnyInvocable<void(RTCError) &&> on_complete) {
    const bool accepted = Send(buffer);
    std::move(on_complete)(accepted
        ? RTCError()
        : RTCError(RTCErrorType::RESOURCE_EXHAUSTED));
}

} // namespace webrtc

namespace livekit {

class RoomStreamDeliveryTestAccess final {
public:
    static void AttachReliableChannel(
        Room& room,
        webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) {
        std::lock_guard lock(room.room_mutex_);
        room.reliable_dc_ = std::move(channel);
    }

    static uint64_t InstalledGeneration(const Room& room) {
        std::lock_guard lock(room.room_mutex_);
        return room.installed_session_generation_;
    }

    static void AttachObservedReliableChannel(
        Room& room,
        webrtc::scoped_refptr<webrtc::DataChannelInterface> channel,
        uint64_t generation) {
        auto observer = room.CreateDataChannelObserver(
            true, generation, channel.get());
        channel->RegisterObserver(observer.get());
        std::lock_guard lock(room.room_mutex_);
        TEST_CHECK(room.IsNativeGenerationCurrentLocked(generation));
        room.reliable_dc_ = std::move(channel);
        room.data_channel_observers_.push_back(std::move(observer));
    }

    static asio::awaitable<void> WaitForReliableDataChannel(
        Room& room,
        std::chrono::milliseconds timeout,
        uint64_t generation) {
        co_await room.WaitForReliableDataChannel(timeout, generation);
    }

    static std::size_t PendingReliableDataChannelWaitCount(const Room& room) {
        std::lock_guard lock(room.room_mutex_);
        return room.pending_reliable_dc_waits_.size();
    }

    static std::size_t StreamReaderCount(const Room& room) {
        std::lock_guard lock(room.room_mutex_);
        return room.active_text_readers_.size() + room.active_byte_readers_.size();
    }

    static void SetAdmissionHooks(Room& room,
                                  std::function<void()> before,
                                  std::function<void()> after) {
        std::lock_guard lock(room.room_mutex_);
        room.stream_delivery_test_hooks_ =
            std::make_shared<Room::StreamDeliveryTestHooks>();
        room.stream_delivery_test_hooks_->before_admission = std::move(before);
        room.stream_delivery_test_hooks_->after_admission = std::move(after);
    }

    static void ClearAdmissionHooks(Room& room) {
        std::lock_guard lock(room.room_mutex_);
        room.stream_delivery_test_hooks_.reset();
    }

    static void SetBeforeFullRestartDataChannelWait(
        Room& room,
        std::function<void(uint64_t)> hook) {
        std::lock_guard lock(room.room_mutex_);
        if (!room.stream_delivery_test_hooks_) {
            room.stream_delivery_test_hooks_ =
                std::make_shared<Room::StreamDeliveryTestHooks>();
        }
        room.stream_delivery_test_hooks_
            ->before_full_restart_data_channel_wait = std::move(hook);
    }

    static void ClearBeforeFullRestartDataChannelWait(Room& room) {
        std::lock_guard lock(room.room_mutex_);
        if (room.stream_delivery_test_hooks_) {
            room.stream_delivery_test_hooks_
                ->before_full_restart_data_channel_wait = {};
        }
    }

    static void SetFullRestartPublisherNegotiation(
        Room& room,
        std::function<asio::awaitable<void>(std::chrono::milliseconds, uint64_t)> hook) {
        std::lock_guard lock(room.room_mutex_);
        if (!room.stream_delivery_test_hooks_) {
            room.stream_delivery_test_hooks_ =
                std::make_shared<Room::StreamDeliveryTestHooks>();
        }
        room.stream_delivery_test_hooks_->negotiate_full_restart_publisher =
            std::move(hook);
    }
};

} // namespace livekit

namespace {

using livekit::OperationError;
using livekit::OperationErrorCode;
using livekit::OperationKind;

enum class WriterKind { Text, Byte };
enum class PacketKind { Header, Chunk, Trailer };
enum class FailureKind { ReturnFalse, StandardException, NonstandardException };

struct PublisherError final : std::runtime_error {
    PublisherError() : std::runtime_error("publisher sentinel detail") {}
};

constexpr int kNonstandardSentinel = 712;

PacketKind KindOf(const livekit::proto::DataPacket& packet) {
    if (packet.has_stream_header()) return PacketKind::Header;
    if (packet.has_stream_chunk()) return PacketKind::Chunk;
    TEST_CHECK(packet.has_stream_trailer());
    return PacketKind::Trailer;
}

const char* StageOf(PacketKind kind) {
    switch (kind) {
    case PacketKind::Header: return "header";
    case PacketKind::Chunk: return "chunk";
    case PacketKind::Trailer: return "trailer";
    }
    return "packet";
}

class PublisherTrace final {
public:
    PacketKind fail_on = PacketKind::Header;
    FailureKind failure = FailureKind::ReturnFalse;
    int failures_left = 1;
    std::vector<livekit::proto::DataPacket> attempts;
    std::vector<livekit::proto::DataPacket> accepted;

    livekit::StreamPacketPublisher publisher() {
        return [this](const livekit::proto::DataPacket& packet, bool reliable) {
            TEST_CHECK(reliable);
            attempts.push_back(packet);
            if (KindOf(packet) == fail_on && failures_left-- > 0) {
                if (failure == FailureKind::ReturnFalse) return false;
                if (failure == FailureKind::StandardException) throw PublisherError();
                throw kNonstandardSentinel;
            }
            accepted.push_back(packet);
            return true;
        };
    }

    std::size_t count(PacketKind kind) const {
        std::size_t result = 0;
        for (const auto& packet : attempts) {
            if (KindOf(packet) == kind) ++result;
        }
        return result;
    }
};

std::unique_ptr<livekit::BaseStreamWriter> MakeWriter(
    WriterKind kind,
    livekit::StreamPacketPublisher publisher,
    const std::string& stream_id = "ST_delivery") {
    if (kind == WriterKind::Text) {
        return std::make_unique<livekit::TextStreamWriter>(
            std::move(publisher), "delivery", std::map<std::string, std::string>{},
            stream_id);
    }
    return std::make_unique<livekit::ByteStreamWriter>(
        std::move(publisher), "delivery.bin", "delivery",
        std::map<std::string, std::string>{}, stream_id);
}

void Write(WriterKind kind,
           livekit::BaseStreamWriter& writer,
           const std::string& payload) {
    if (kind == WriterKind::Text) {
        static_cast<livekit::TextStreamWriter&>(writer).Write(payload);
    } else {
        static_cast<livekit::ByteStreamWriter&>(writer).Write(
            std::vector<uint8_t>(payload.begin(), payload.end()));
    }
}

template <typename Action>
void ExpectFailure(FailureKind failure, PacketKind stage, Action&& action) {
    bool caught = false;
    try {
        action();
    } catch (const OperationError& error) {
        TEST_CHECK(failure == FailureKind::ReturnFalse);
        TEST_CHECK(error.operation() == OperationKind::SendData);
        TEST_CHECK(error.code() == OperationErrorCode::DataChannelRejected);
        TEST_CHECK(error.stage() == StageOf(stage));
        caught = true;
    } catch (const PublisherError& error) {
        TEST_CHECK(failure == FailureKind::StandardException);
        TEST_CHECK(std::string(error.what()) == "publisher sentinel detail");
        caught = true;
    } catch (int error) {
        TEST_CHECK(failure == FailureKind::NonstandardException);
        TEST_CHECK(error == kNonstandardSentinel);
        caught = true;
    }
    TEST_CHECK(caught);
}

void TriggerFailure(PacketKind stage,
                    WriterKind kind,
                    livekit::BaseStreamWriter& writer) {
    if (stage == PacketKind::Trailer) writer.Close();
    else Write(kind, writer, "payload");
}

void TestWriterFailureMatrix() {
    for (const auto kind : {WriterKind::Text, WriterKind::Byte}) {
        for (const auto stage : {PacketKind::Header, PacketKind::Chunk,
                                 PacketKind::Trailer}) {
            for (const auto failure : {FailureKind::ReturnFalse,
                                       FailureKind::StandardException,
                                       FailureKind::NonstandardException}) {
                PublisherTrace trace;
                trace.fail_on = stage;
                trace.failure = failure;
                auto writer = MakeWriter(kind, trace.publisher());
                ExpectFailure(failure, stage,
                              [&] { TriggerFailure(stage, kind, *writer); });
                TEST_CHECK(writer->is_closed());
                const auto attempts = trace.attempts.size();
                ExpectFailure(failure, stage,
                              [&] { Write(kind, *writer, "retry"); });
                ExpectFailure(failure, stage, [&] { writer->Close(); });
                ExpectFailure(failure, stage, [&] { writer->Cancel(); });
                writer.reset();
                TEST_CHECK(trace.attempts.size() == attempts);
                TEST_CHECK(trace.count(PacketKind::Trailer) ==
                           (stage == PacketKind::Trailer ? 1u : 0u));
            }
        }
    }
}

void TestMidWriteFailureStopsPrefix() {
    for (const auto kind : {WriterKind::Text, WriterKind::Byte}) {
        PublisherTrace trace;
        trace.fail_on = PacketKind::Chunk;
        trace.failures_left = 0;
        auto publisher = [&trace](const livekit::proto::DataPacket& packet,
                                  bool reliable) {
            TEST_CHECK(reliable);
            trace.attempts.push_back(packet);
            if (packet.has_stream_chunk() &&
                packet.stream_chunk().chunk_index() == 1) {
                return false;
            }
            trace.accepted.push_back(packet);
            return true;
        };
        auto writer = MakeWriter(kind, publisher, "ST_prefix");
        const std::string payload(livekit::kStreamChunkSize * 3 + 9, 'p');
        ExpectFailure(FailureKind::ReturnFalse, PacketKind::Chunk,
                      [&] { Write(kind, *writer, payload); });
        TEST_CHECK(trace.attempts.size() == 3);
        TEST_CHECK(trace.accepted.size() == 2);
        TEST_CHECK(trace.accepted[0].has_stream_header());
        TEST_CHECK(trace.accepted[1].stream_chunk().chunk_index() == 0);
        writer.reset();
        TEST_CHECK(trace.attempts.size() == 3);
        TEST_CHECK(trace.count(PacketKind::Trailer) == 0);
    }
}

void TestEmptyPublisher() {
    for (const auto kind : {WriterKind::Text, WriterKind::Byte}) {
        auto writer = MakeWriter(kind, {});
        bool caught = false;
        try {
            Write(kind, *writer, "payload");
        } catch (const OperationError& error) {
            caught = true;
            TEST_CHECK(error.operation() == OperationKind::SendData);
            TEST_CHECK(error.code() == OperationErrorCode::DataChannelUnavailable);
            TEST_CHECK(error.stage() == "header");
        }
        TEST_CHECK(caught);
        TEST_CHECK(writer->is_closed());
    }
}

class ControlledDataChannel : public webrtc::DataChannelInterface {
public:
    explicit ControlledDataChannel(std::string label)
        : label_(std::move(label)) {}

    void RegisterObserver(webrtc::DataChannelObserver* observer) override {
        std::lock_guard lock(mutex_);
        observer_ = observer;
    }

    void UnregisterObserver() override {
        std::lock_guard lock(mutex_);
        observer_ = nullptr;
    }

    std::string label() const override { return label_; }
    bool reliable() const override { return true; }
    int id() const override { return 1; }

    DataState state() const override {
        std::lock_guard lock(mutex_);
        return state_;
    }

    uint32_t messages_sent() const override {
        std::lock_guard lock(mutex_);
        return static_cast<uint32_t>(accepted_.size());
    }
    uint64_t bytes_sent() const override {
        std::lock_guard lock(mutex_);
        return bytes_sent_;
    }
    uint32_t messages_received() const override { return 0; }
    uint64_t bytes_received() const override { return 0; }
    uint64_t buffered_amount() const override { return 0; }

    void Close() override {
        webrtc::DataChannelObserver* observer = nullptr;
        {
            std::lock_guard lock(mutex_);
            state_ = kClosed;
            observer = observer_;
        }
        if (observer) observer->OnStateChange();
    }

    bool Send(const webrtc::DataBuffer& buffer) override {
        std::lock_guard lock(mutex_);
        ++attempts_;
        if (state_ != kOpen || !accept_) return false;
        livekit::proto::DataPacket packet;
        TEST_CHECK(packet.ParseFromArray(
            buffer.data.data(), static_cast<int>(buffer.data.size())));
        bytes_sent_ += buffer.data.size();
        accepted_.push_back(std::move(packet));
        return true;
    }

    void SendAsync(
        webrtc::DataBuffer buffer,
        absl::AnyInvocable<void(webrtc::RTCError) &&> on_complete) override {
        const bool accepted = Send(buffer);
        std::move(on_complete)(accepted
            ? webrtc::RTCError()
            : webrtc::RTCError(webrtc::RTCErrorType::RESOURCE_EXHAUSTED));
    }

    void SetState(DataState state) {
        webrtc::DataChannelObserver* observer = nullptr;
        {
            std::lock_guard lock(mutex_);
            state_ = state;
            observer = observer_;
        }
        if (observer) observer->OnStateChange();
    }

    void SetAccept(bool accept) {
        std::lock_guard lock(mutex_);
        accept_ = accept;
    }

    std::size_t attempts() const {
        std::lock_guard lock(mutex_);
        return attempts_;
    }

    std::vector<livekit::proto::DataPacket> accepted() const {
        std::lock_guard lock(mutex_);
        return accepted_;
    }

private:
    const std::string label_;
    mutable std::mutex mutex_;
    webrtc::DataChannelObserver* observer_ = nullptr;
    DataState state_ = kOpen;
    bool accept_ = true;
    std::size_t attempts_ = 0;
    uint64_t bytes_sent_ = 0;
    std::vector<livekit::proto::DataPacket> accepted_;
};

std::string Base64Encode(const unsigned char* data, std::size_t length) {
    static constexpr char chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    for (std::size_t i = 0; i < length; i += 3) {
        const uint32_t a = data[i];
        const bool has_b = i + 1 < length;
        const bool has_c = i + 2 < length;
        const uint32_t b = has_b ? data[i + 1] : 0;
        const uint32_t c = has_c ? data[i + 2] : 0;
        const uint32_t value = (a << 16) | (b << 8) | c;
        result.push_back(chars[(value >> 18) & 0x3f]);
        result.push_back(chars[(value >> 12) & 0x3f]);
        result.push_back(has_b ? chars[(value >> 6) & 0x3f] : '=');
        result.push_back(has_c ? chars[value & 0x3f] : '=');
    }
    return result;
}

class SignalTestServer final
    : public std::enable_shared_from_this<SignalTestServer> {
public:
    explicit SignalTestServer(asio::io_context& io)
        : io_(io),
          acceptor_(io, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0)) {}

    uint16_t port() const { return acceptor_.local_endpoint().port(); }

    void Start() { Accept(); }

    void Stop() {
        CloseConnections();
        std::error_code error;
        acceptor_.close(error);
    }

    void SetIdentity(std::string identity) { identity_ = std::move(identity); }
    void SetRejectResume(bool reject) { reject_resume_ = reject; }
    void SetPauseResume(bool pause) { pause_resume_ = pause; }
    void SetSubscriberPrimary(bool value) { subscriber_primary_ = value; }
    bool HasPendingResume() const { return pending_resume_ != nullptr; }

    void CloseConnections() {
        std::error_code error;
        for (const auto& socket : sockets_) {
            if (!socket || !socket->is_open()) continue;
            socket->shutdown(asio::ip::tcp::socket::shutdown_both, error);
            socket->close(error);
        }
        sockets_.clear();
        pending_resume_.reset();
    }

    void CompletePausedResume() {
        TEST_CHECK(pending_resume_ != nullptr);
        auto socket = std::move(pending_resume_);
        SendReconnect(socket);
        ReadFrame(socket);
    }

private:
    void Accept() {
        auto self = shared_from_this();
        auto socket = std::make_shared<asio::ip::tcp::socket>(io_);
        acceptor_.async_accept(*socket, [self, socket](std::error_code error) {
            if (!error) self->ReadHandshake(socket);
            if (self->acceptor_.is_open()) self->Accept();
        });
    }

    void ReadHandshake(const std::shared_ptr<asio::ip::tcp::socket>& socket) {
        sockets_.push_back(socket);
        auto self = shared_from_this();
        auto buffer = std::make_shared<asio::streambuf>();
        asio::async_read_until(*socket, *buffer, "\r\n\r\n",
            [self, socket, buffer](std::error_code error, std::size_t) {
                if (error) return;
                std::istream request(buffer.get());
                std::string request_line;
                std::getline(request, request_line);
                std::string header;
                std::string key;
                while (std::getline(request, header) && header != "\r") {
                    if (header.rfind("Sec-WebSocket-Key:", 0) != 0) continue;
                    key = header.substr(18);
                    while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) {
                        key.erase(key.begin());
                    }
                    if (!key.empty() && key.back() == '\r') key.pop_back();
                }
                self->WriteHandshake(socket, request_line, key);
            });
    }

    void WriteHandshake(const std::shared_ptr<asio::ip::tcp::socket>& socket,
                        const std::string& request_line,
                        const std::string& key) {
        const std::string source =
            key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        unsigned char hash[SHA_DIGEST_LENGTH];
        SHA1(reinterpret_cast<const unsigned char*>(source.data()),
             source.size(), hash);
        auto response = std::make_shared<std::string>(
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " +
            Base64Encode(hash, SHA_DIGEST_LENGTH) + "\r\n\r\n");
        auto self = shared_from_this();
        asio::async_write(*socket, asio::buffer(*response),
            [self, socket, response, request_line](std::error_code error,
                                                   std::size_t) {
                if (error) return;
                const bool resume = request_line.find("reconnect=1") !=
                                    std::string::npos;
                if (resume && self->reject_resume_) {
                    std::error_code ignored;
                    socket->close(ignored);
                    return;
                }
                if (resume && self->pause_resume_) {
                    self->pending_resume_ = socket;
                    return;
                }
                if (resume) self->SendReconnect(socket);
                else self->SendJoin(socket);
                self->ReadFrame(socket);
            });
    }

    void SendJoin(const std::shared_ptr<asio::ip::tcp::socket>& socket) {
        livekit::proto::SignalResponse response;
        auto* join = response.mutable_join();
        join->set_ping_interval(10);
        join->set_ping_timeout(20);
        join->set_subscriber_primary(subscriber_primary_);
        join->set_fast_publish(false);
        join->mutable_room()->set_sid("RM_STREAM_DELIVERY");
        join->mutable_room()->set_name("stream-delivery");
        join->mutable_participant()->set_sid("PA_" + identity_);
        join->mutable_participant()->set_identity(identity_);
        join->mutable_client_configuration()->set_resume_connection(
            livekit::proto::ClientConfigSetting::ENABLED);
        SendResponse(socket, response);
    }

    void SendReconnect(const std::shared_ptr<asio::ip::tcp::socket>& socket) {
        livekit::proto::SignalResponse response;
        response.mutable_reconnect()
            ->mutable_client_configuration()
            ->set_resume_connection(
                livekit::proto::ClientConfigSetting::ENABLED);
        SendResponse(socket, response);
    }

    void SendResponse(const std::shared_ptr<asio::ip::tcp::socket>& socket,
                      const livekit::proto::SignalResponse& response) {
        std::string payload;
        TEST_CHECK(response.SerializeToString(&payload));
        auto frame = std::make_shared<std::vector<uint8_t>>();
        frame->push_back(0x82);
        if (payload.size() < 126) {
            frame->push_back(static_cast<uint8_t>(payload.size()));
        } else {
            TEST_CHECK(payload.size() <= 0xffff);
            frame->push_back(126);
            frame->push_back(static_cast<uint8_t>(payload.size() >> 8));
            frame->push_back(static_cast<uint8_t>(payload.size()));
        }
        frame->insert(frame->end(), payload.begin(), payload.end());
        asio::async_write(*socket, asio::buffer(*frame),
                          [frame](std::error_code, std::size_t) {});
    }

    void ReadFrame(const std::shared_ptr<asio::ip::tcp::socket>& socket) {
        auto self = shared_from_this();
        auto header = std::make_shared<std::vector<uint8_t>>(2);
        asio::async_read(*socket, asio::buffer(*header),
            [self, socket, header](std::error_code error, std::size_t) {
                if (error) return;
                const uint8_t opcode = (*header)[0] & 0x0f;
                const bool masked = ((*header)[1] & 0x80) != 0;
                const uint8_t marker = (*header)[1] & 0x7f;
                if (marker < 126) {
                    self->ReadBody(socket, opcode, masked, marker);
                } else {
                    const std::size_t width = marker == 126 ? 2 : 8;
                    auto extended = std::make_shared<std::vector<uint8_t>>(width);
                    asio::async_read(*socket, asio::buffer(*extended),
                        [self, socket, opcode, masked, extended](
                            std::error_code extended_error, std::size_t) {
                            if (extended_error) return;
                            uint64_t size = 0;
                            for (uint8_t byte : *extended) size = (size << 8) | byte;
                            self->ReadBody(socket, opcode, masked, size);
                        });
                }
            });
    }

    void ReadBody(const std::shared_ptr<asio::ip::tcp::socket>& socket,
                  uint8_t opcode,
                  bool masked,
                  uint64_t payload_size) {
        auto self = shared_from_this();
        const std::size_t total = static_cast<std::size_t>(payload_size) +
                                  (masked ? 4u : 0u);
        auto body = std::make_shared<std::vector<uint8_t>>(total);
        asio::async_read(*socket, asio::buffer(*body),
            [self, socket, body, opcode, masked, payload_size](
                std::error_code error, std::size_t) {
                if (error) return;
                const uint8_t* mask = masked ? body->data() : nullptr;
                uint8_t* payload = body->data() + (masked ? 4 : 0);
                if (masked) {
                    for (std::size_t i = 0; i < payload_size; ++i) {
                        payload[i] ^= mask[i % 4];
                    }
                }
                if (opcode == 0x8) return;
                if (opcode == 0x9) {
                    auto pong = std::make_shared<std::vector<uint8_t>>();
                    pong->push_back(0x8a);
                    pong->push_back(static_cast<uint8_t>(payload_size));
                    pong->insert(pong->end(), payload, payload + payload_size);
                    asio::async_write(*socket, asio::buffer(*pong),
                                      [pong](std::error_code, std::size_t) {});
                }
                self->ReadFrame(socket);
            });
    }

    asio::io_context& io_;
    asio::ip::tcp::acceptor acceptor_;
    std::string identity_ = "same-user";
    bool reject_resume_ = false;
    bool pause_resume_ = false;
    bool subscriber_primary_ = false;
    std::vector<std::shared_ptr<asio::ip::tcp::socket>> sockets_;
    std::shared_ptr<asio::ip::tcp::socket> pending_resume_;
};

class RoomTrace final : public livekit::RoomListener {
public:
    void OnReconnecting() override {
        reconnecting.fetch_add(1, std::memory_order_release);
    }
    void OnReconnected() override {
        reconnected.fetch_add(1, std::memory_order_release);
    }
    void OnDataReceived(const std::vector<uint8_t>&,
                        std::shared_ptr<livekit::RemoteParticipant>,
                        const std::string&) override {
        incoming.fetch_add(1, std::memory_order_release);
    }
    void OnTextStreamOpened(
        std::shared_ptr<livekit::TextStreamReader>,
        std::shared_ptr<livekit::Participant>) override {
        incoming.fetch_add(1, std::memory_order_release);
    }
    void OnByteStreamOpened(
        std::shared_ptr<livekit::ByteStreamReader>,
        std::shared_ptr<livekit::Participant>) override {
        incoming.fetch_add(1, std::memory_order_release);
    }

    std::atomic<int> reconnecting{0};
    std::atomic<int> reconnected{0};
    std::atomic<int> incoming{0};
};

class Barrier final {
public:
    void ArriveAndWait() {
        std::unique_lock lock(mutex_);
        arrived_ = true;
        cv_.notify_all();
        cv_.wait(lock, [&] { return released_; });
    }

    void WaitUntilArrived() {
        std::unique_lock lock(mutex_);
        TEST_CHECK(cv_.wait_for(lock, std::chrono::seconds(5),
                                [&] { return arrived_; }));
    }

    void Release() {
        std::lock_guard lock(mutex_);
        released_ = true;
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool arrived_ = false;
    bool released_ = false;
};

template <typename Predicate>
asio::awaitable<void> WaitFor(asio::any_io_executor executor,
                              Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    asio::steady_timer timer(executor);
    while (!predicate()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        timer.expires_after(std::chrono::milliseconds(2));
        std::error_code error;
        co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, error));
    }
}

void ExpectRoomError(OperationErrorCode expected,
                     const char* stage,
                     const std::function<void()>& action) {
    bool caught = false;
    try {
        action();
    } catch (const OperationError& error) {
        caught = true;
        TEST_CHECK(error.operation() == OperationKind::SendData);
        TEST_CHECK(error.code() == expected);
        TEST_CHECK(error.stage() == stage);
    }
    TEST_CHECK(caught);
}

livekit::SignalOptions TestSignalOptions() {
    livekit::SignalOptions options;
    options.allow_insecure_transport = true;
    options.single_peer_connection = false;
    options.create_webrtc_pc = false;
    options.connect_timeout = std::chrono::seconds(2);
    options.timeouts.reconnect_attempt = std::chrono::milliseconds(200);
    options.timeouts.reconnect_total = std::chrono::seconds(5);
    return options;
}

asio::awaitable<uint64_t> FullRestart(
    const std::shared_ptr<livekit::Room>& room,
    const std::shared_ptr<SignalTestServer>& server,
    const std::shared_ptr<RoomTrace>& trace,
    std::string identity) {
    const auto old_generation =
        livekit::RoomStreamDeliveryTestAccess::InstalledGeneration(*room);
    const int old_reconnected = trace->reconnected.load(std::memory_order_acquire);
    server->SetIdentity(std::move(identity));
    server->SetPauseResume(false);
    server->SetRejectResume(true);
    server->CloseConnections();
    co_await WaitFor(room->executor(), [&] {
        return trace->reconnected.load(std::memory_order_acquire) > old_reconnected &&
               room->connection_state() == livekit::ConnectionState::Connected &&
               livekit::RoomStreamDeliveryTestAccess::InstalledGeneration(*room) !=
                   old_generation;
    });
    server->SetRejectResume(false);
    co_return livekit::RoomStreamDeliveryTestAccess::InstalledGeneration(*room);
}

asio::awaitable<void> TestLazyPublisherFullRestart(asio::any_io_executor executor) {
    auto server = std::make_shared<SignalTestServer>(
        static_cast<asio::io_context&>(executor.context()));
    server->SetSubscriberPrimary(true);
    server->Start();
    auto room = livekit::Room::Create(executor);
    auto trace = std::make_shared<RoomTrace>();
    room->AddListener(trace);
    auto options = TestSignalOptions();
    options.timeouts.reconnect_attempt = std::chrono::seconds(5);
    TEST_CHECK(co_await room->Connect(
        "ws://127.0.0.1:" + std::to_string(server->port()),
        "lazy-publisher-token", options));
    TEST_CHECK(room->join_response()->subscriber_primary());
    TEST_CHECK(!room->join_response()->fast_publish());
    TEST_CHECK(room->local_participant()->tracks().empty());
    const auto old_generation =
        livekit::RoomStreamDeliveryTestAccess::InstalledGeneration(*room);
    auto stale = room->CreateTextStreamWriter("before-lazy-restart");

    // Keep the real signaling/restart/republish/commit path. Substitute only
    // native transport boundaries so no device or ICE timing controls the test.
    auto channel = webrtc::make_ref_counted<ControlledDataChannel>("lazy-publisher");
    channel->SetState(webrtc::DataChannelInterface::kConnecting);
    auto negotiation_gate = std::make_shared<livekit::AwaitableState<void>>(executor);
    bool transport_installed = false;
    int negotiations = 0;
    livekit::RoomStreamDeliveryTestAccess::SetBeforeFullRestartDataChannelWait(
        *room, [&](uint64_t generation) {
            TEST_CHECK(generation != old_generation);
            TEST_CHECK(room->local_participant()->tracks().empty());
            livekit::RoomStreamDeliveryTestAccess::AttachObservedReliableChannel(
                *room, channel, generation);
            transport_installed = true;
        });
    livekit::RoomStreamDeliveryTestAccess::SetFullRestartPublisherNegotiation(
        *room, [&](std::chrono::milliseconds timeout,
                   uint64_t generation) -> asio::awaitable<void> {
            TEST_CHECK(generation ==
                livekit::RoomStreamDeliveryTestAccess::InstalledGeneration(*room));
            TEST_CHECK(timeout > std::chrono::milliseconds::zero());
            TEST_CHECK(timeout <= options.timeouts.reconnect_attempt);
            ++negotiations;
            co_await livekit::WaitAwaitable<void>(negotiation_gate, timeout,
                OperationKind::Negotiate, OperationErrorCode::NegotiationFailed,
                "test_lazy_publisher_negotiation");
        });
    server->SetRejectResume(true);
    server->CloseConnections();
    co_await WaitFor(executor, [&] { return transport_installed; });
    TEST_CHECK(negotiations == 1);
    TEST_CHECK(trace->reconnected.load(std::memory_order_acquire) == 0);
    TEST_CHECK(livekit::RoomStreamDeliveryTestAccess::
        PendingReliableDataChannelWaitCount(*room) == 0);

    livekit::CompleteAwaitable(negotiation_gate);
    co_await WaitFor(executor, [&] {
        return livekit::RoomStreamDeliveryTestAccess::
            PendingReliableDataChannelWaitCount(*room) == 1;
    });
    TEST_CHECK(trace->reconnected.load(std::memory_order_acquire) == 0);
    TEST_CHECK(channel->attempts() == 0);
    channel->SetState(webrtc::DataChannelInterface::kOpen);
    co_await WaitFor(executor, [&] {
        return trace->reconnected.load(std::memory_order_acquire) == 1;
    });
    TEST_CHECK(negotiations == 1);
    TEST_CHECK(room->connection_state() == livekit::ConnectionState::Connected);
    ExpectRoomError(OperationErrorCode::SessionInvalid, "header",
        [&] { stale->Write("must-not-enter-new-session"); });
    TEST_CHECK(channel->attempts() == 0);
    auto fresh = room->CreateTextStreamWriter("after-lazy-restart");
    fresh->Write("ready-after-negotiation-and-open");
    fresh->Close();
    TEST_CHECK(channel->accepted().size() == 3);
    livekit::RoomStreamDeliveryTestAccess::ClearAdmissionHooks(*room);
    room->Disconnect();
    server->Stop();
}

asio::awaitable<void> TestRoomDeliveryAndSessions(asio::any_io_executor executor) {
    auto server = std::make_shared<SignalTestServer>(
        static_cast<asio::io_context&>(executor.context()));
    server->Start();
    auto room = livekit::Room::Create(executor);
    auto trace = std::make_shared<RoomTrace>();
    room->AddListener(trace);

    auto preconnect = room->CreateTextStreamWriter("preconnect");
    TEST_CHECK(co_await room->Connect(
        "ws://127.0.0.1:" + std::to_string(server->port()),
        "stream-delivery-token", TestSignalOptions()));
    const auto generation_a =
        livekit::RoomStreamDeliveryTestAccess::InstalledGeneration(*room);
    TEST_CHECK(generation_a != 0);
    auto channel_a = webrtc::make_ref_counted<ControlledDataChannel>("A");
    livekit::RoomStreamDeliveryTestAccess::AttachReliableChannel(*room, channel_a);

    ExpectRoomError(OperationErrorCode::SessionInvalid, "header",
                    [&] { preconnect->Write("must-not-bind-later"); });
    TEST_CHECK(channel_a->attempts() == 0);

    livekit::proto::DataPacket direct;
    direct.mutable_stream_header()->set_stream_id("direct");
    livekit::RoomStreamDeliveryTestAccess::AttachReliableChannel(*room, nullptr);
    TEST_CHECK(!room->PublishDataPacket(direct));
    TEST_CHECK(trace->incoming.load(std::memory_order_acquire) == 0);
    TEST_CHECK(livekit::RoomStreamDeliveryTestAccess::StreamReaderCount(*room) == 0);

    auto no_channel = room->CreateTextStreamWriter("no-channel");
    ExpectRoomError(OperationErrorCode::DataChannelUnavailable, "header",
                    [&] { no_channel->Write("offline"); });
    TEST_CHECK(trace->incoming.load(std::memory_order_acquire) == 0);

    auto non_open = webrtc::make_ref_counted<ControlledDataChannel>("non-open");
    non_open->SetState(webrtc::DataChannelInterface::kConnecting);
    livekit::RoomStreamDeliveryTestAccess::AttachReliableChannel(*room, non_open);
    auto closed_channel_writer = room->CreateByteStreamWriter("closed.bin");
    ExpectRoomError(OperationErrorCode::DataChannelUnavailable, "header", [&] {
        closed_channel_writer->Write(std::vector<uint8_t>{1, 2, 3});
    });
    TEST_CHECK(non_open->attempts() == 0);
    TEST_CHECK(trace->incoming.load(std::memory_order_acquire) == 0);

    auto rejecting = webrtc::make_ref_counted<ControlledDataChannel>("rejecting");
    rejecting->SetAccept(false);
    livekit::RoomStreamDeliveryTestAccess::AttachReliableChannel(*room, rejecting);
    auto rejected_writer = room->CreateTextStreamWriter("rejected");
    ExpectRoomError(OperationErrorCode::DataChannelRejected, "header",
                    [&] { rejected_writer->Write("backpressure"); });
    TEST_CHECK(rejecting->attempts() == 1);
    TEST_CHECK(trace->incoming.load(std::memory_order_acquire) == 0);

    livekit::RoomStreamDeliveryTestAccess::AttachReliableChannel(*room, channel_a);
    auto current_a = room->CreateTextStreamWriter("current-a");
    current_a->Write("accepted-a");
    current_a->Close();
    TEST_CHECK(channel_a->accepted().size() == 3);

    const auto routed_start = channel_a->accepted().size();
    std::vector<std::string> routed_destinations{"receiver-a", "receiver-b"};
    auto routed = room->CreateTextStreamWriter(
        "routed", {}, "routed-through-room", std::nullopt, "",
        routed_destinations);
    routed_destinations.assign({"mutated-after-create"});
    routed->Write(std::string(livekit::kStreamChunkSize + 1, 'r'));
    routed->Close();
    const auto routed_packets = channel_a->accepted();
    TEST_CHECK(routed_packets.size() == routed_start + 4);
    for (std::size_t index = routed_start; index < routed_packets.size(); ++index) {
        const auto& packet = routed_packets[index];
        TEST_CHECK(packet.destination_identities_size() == 2);
        TEST_CHECK(packet.destination_identities(0) == "receiver-a");
        TEST_CHECK(packet.destination_identities(1) == "receiver-b");
        TEST_CHECK(!packet.has_user());
        TEST_CHECK(packet.participant_identity() == "same-user");
    }
    TEST_CHECK(routed_packets[routed_start].has_stream_header());
    TEST_CHECK(routed_packets[routed_start + 1].has_stream_chunk());
    TEST_CHECK(routed_packets[routed_start + 2].has_stream_chunk());
    TEST_CHECK(routed_packets[routed_start + 3].has_stream_trailer());

    auto soft_survivor = room->CreateTextStreamWriter("soft-survivor");
    auto soft_failed = room->CreateTextStreamWriter("soft-failed");
    const int reconnecting_before =
        trace->reconnecting.load(std::memory_order_acquire);
    const int reconnected_before = trace->reconnected.load(std::memory_order_acquire);
    server->SetPauseResume(true);
    server->SetRejectResume(false);
    server->CloseConnections();
    co_await WaitFor(executor, [&] {
        return trace->reconnecting.load(std::memory_order_acquire) >
                   reconnecting_before &&
               room->connection_state() == livekit::ConnectionState::Reconnecting &&
               server->HasPendingResume();
    });
    ExpectRoomError(OperationErrorCode::SessionInvalid, "header",
                    [&] { soft_failed->Write("during-resume"); });
    server->CompletePausedResume();
    server->SetPauseResume(false);
    co_await WaitFor(executor, [&] {
        return trace->reconnected.load(std::memory_order_acquire) >
                   reconnected_before &&
               room->connection_state() == livekit::ConnectionState::Connected;
    });
    TEST_CHECK(livekit::RoomStreamDeliveryTestAccess::InstalledGeneration(*room) ==
               generation_a);
    soft_survivor->Write("after-resume");
    soft_survivor->Close();
    ExpectRoomError(OperationErrorCode::SessionInvalid, "header",
                    [&] { soft_failed->Close(); });

    auto stale_same_identity = room->CreateTextStreamWriter("stale-same");
    auto channel_b = webrtc::make_ref_counted<ControlledDataChannel>("B");
    channel_b->SetState(webrtc::DataChannelInterface::kConnecting);
    std::atomic<bool> full_restart_wait_started{false};
    livekit::RoomStreamDeliveryTestAccess::SetBeforeFullRestartDataChannelWait(
        *room, [&](uint64_t generation) {
            livekit::RoomStreamDeliveryTestAccess::AttachObservedReliableChannel(
                *room, channel_b, generation);
            full_restart_wait_started.store(true, std::memory_order_release);
        });
    const int reconnected_before_full_restart =
        trace->reconnected.load(std::memory_order_acquire);
    server->SetIdentity("same-user");
    server->SetPauseResume(false);
    server->SetRejectResume(true);
    server->CloseConnections();
    co_await WaitFor(executor, [&] {
        return full_restart_wait_started.load(std::memory_order_acquire) &&
               livekit::RoomStreamDeliveryTestAccess::
                   PendingReliableDataChannelWaitCount(*room) == 1;
    });
    TEST_CHECK(trace->reconnected.load(std::memory_order_acquire) ==
               reconnected_before_full_restart);
    channel_b->SetState(webrtc::DataChannelInterface::kOpen);
    co_await WaitFor(executor, [&] {
        return trace->reconnected.load(std::memory_order_acquire) >
               reconnected_before_full_restart;
    });
    server->SetRejectResume(false);
    livekit::RoomStreamDeliveryTestAccess::
        ClearBeforeFullRestartDataChannelWait(*room);
    const auto generation_b =
        livekit::RoomStreamDeliveryTestAccess::InstalledGeneration(*room);
    TEST_CHECK(generation_b != generation_a);
    ExpectRoomError(OperationErrorCode::SessionInvalid, "header",
                    [&] { stale_same_identity->Write("stale"); });
    ExpectRoomError(OperationErrorCode::SessionInvalid, "header",
                    [&] { stale_same_identity->Close(); });
    TEST_CHECK(channel_b->attempts() == 0);
    auto current_b = room->CreateTextStreamWriter("current-b");
    current_b->Write("accepted-b");
    current_b->Close();
    TEST_CHECK(channel_b->accepted().size() == 3);

    auto stale_different_identity =
        room->CreateByteStreamWriter("stale-different.bin");
    const auto generation_c = co_await FullRestart(
        room, server, trace, "replacement-user");
    TEST_CHECK(generation_c != generation_b);
    auto channel_c = webrtc::make_ref_counted<ControlledDataChannel>("C");
    livekit::RoomStreamDeliveryTestAccess::AttachReliableChannel(*room, channel_c);
    ExpectRoomError(OperationErrorCode::SessionInvalid, "header", [&] {
        stale_different_identity->Write(std::vector<uint8_t>{4, 5, 6});
    });
    TEST_CHECK(channel_c->attempts() == 0);
    auto current_c = room->CreateByteStreamWriter("current-c.bin");
    TEST_CHECK(current_c->info().sender_identity == "replacement-user");
    current_c->Write(std::vector<uint8_t>{7, 8, 9});
    current_c->Close();
    const auto packets_c = channel_c->accepted();
    TEST_CHECK(packets_c.size() == 3);
    for (const auto& packet : packets_c) {
        TEST_CHECK(packet.participant_identity() == "replacement-user");
    }

    auto before_writer = room->CreateTextStreamWriter("before-admission");
    auto before_barrier = std::make_shared<Barrier>();
    livekit::RoomStreamDeliveryTestAccess::SetAdmissionHooks(
        *room, [before_barrier] { before_barrier->ArriveAndWait(); }, {});
    std::exception_ptr before_error;
    std::thread before_thread([&] {
        try {
            before_writer->Write("before");
        } catch (...) {
            before_error = std::current_exception();
        }
    });
    before_barrier->WaitUntilArrived();
    const auto generation_d = co_await FullRestart(
        room, server, trace, "replacement-user");
    auto channel_d = webrtc::make_ref_counted<ControlledDataChannel>("D");
    livekit::RoomStreamDeliveryTestAccess::AttachReliableChannel(*room, channel_d);
    livekit::RoomStreamDeliveryTestAccess::ClearAdmissionHooks(*room);
    before_barrier->Release();
    before_thread.join();
    TEST_CHECK(before_error != nullptr);
    try {
        std::rethrow_exception(before_error);
    } catch (const OperationError& error) {
        TEST_CHECK(error.code() == OperationErrorCode::SessionInvalid);
    }
    TEST_CHECK(channel_d->attempts() == 0);
    TEST_CHECK(generation_d != generation_c);

    auto after_writer = room->CreateTextStreamWriter("after-admission");
    auto after_barrier = std::make_shared<Barrier>();
    livekit::RoomStreamDeliveryTestAccess::SetAdmissionHooks(
        *room, {}, [after_barrier] { after_barrier->ArriveAndWait(); });
    std::exception_ptr after_error;
    std::thread after_thread([&] {
        try {
            after_writer->Write("after");
        } catch (...) {
            after_error = std::current_exception();
        }
    });
    after_barrier->WaitUntilArrived();
    co_await FullRestart(room, server, trace, "replacement-user");
    auto channel_e = webrtc::make_ref_counted<ControlledDataChannel>("E");
    livekit::RoomStreamDeliveryTestAccess::AttachReliableChannel(*room, channel_e);
    livekit::RoomStreamDeliveryTestAccess::ClearAdmissionHooks(*room);
    after_barrier->Release();
    after_thread.join();
    TEST_CHECK(after_error != nullptr);
    TEST_CHECK(channel_e->attempts() == 0);

    auto pending_channel =
        webrtc::make_ref_counted<ControlledDataChannel>("pending-disconnect");
    pending_channel->SetState(webrtc::DataChannelInterface::kConnecting);
    const auto final_generation =
        livekit::RoomStreamDeliveryTestAccess::InstalledGeneration(*room);
    livekit::RoomStreamDeliveryTestAccess::AttachObservedReliableChannel(
        *room, pending_channel, final_generation);
    std::atomic<bool> pending_wait_done{false};
    std::exception_ptr pending_wait_error;
    asio::co_spawn(
        executor,
        [room, final_generation, &pending_wait_done,
         &pending_wait_error]() -> asio::awaitable<void> {
            try {
                co_await livekit::RoomStreamDeliveryTestAccess::
                    WaitForReliableDataChannel(
                        *room, std::chrono::seconds(5), final_generation);
            } catch (...) {
                pending_wait_error = std::current_exception();
            }
            pending_wait_done.store(true, std::memory_order_release);
        },
        asio::detached);
    co_await WaitFor(executor, [&] {
        return livekit::RoomStreamDeliveryTestAccess::
                   PendingReliableDataChannelWaitCount(*room) == 1;
    });

    auto orphan = room->CreateTextStreamWriter("orphan");
    std::weak_ptr<livekit::Room> weak_room = room;
    room->Disconnect();
    co_await WaitFor(executor, [&] {
        return pending_wait_done.load(std::memory_order_acquire);
    });
    TEST_CHECK(pending_wait_error != nullptr);
    try {
        std::rethrow_exception(pending_wait_error);
    } catch (const OperationError& error) {
        TEST_CHECK(error.code() == OperationErrorCode::Cancelled);
    }
    room.reset();
    TEST_CHECK(weak_room.expired());
    ExpectRoomError(OperationErrorCode::SessionInvalid, "header",
                    [&] { orphan->Write("after-room-destruction"); });

    server->Stop();
}

} // namespace

int main() {
    TestWriterFailureMatrix();
    TestMidWriteFailureStopsPrefix();
    TestEmptyPublisher();

    asio::io_context io;
    auto done = asio::co_spawn(
        io,
        TestRoomDeliveryAndSessions(io.get_executor()),
        asio::use_future);
    io.run();
    done.get();

    io.restart();
    auto lazy_done = asio::co_spawn(
        io, TestLazyPublisherFullRestart(io.get_executor()), asio::use_future);
    io.run();
    lazy_done.get();
    return 0;
}
