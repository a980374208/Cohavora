#include "detail/room_internal.h"
#include "livekit_rtc.pb.h"
#include "livekit_models.pb.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

namespace livekit {

namespace {

std::optional<bool> ParseTranscriptionBoolAttr(const std::string& raw) {
    std::string lower;
    lower.reserve(raw.size());
    for (unsigned char ch : raw) {
        if (!std::isspace(ch)) {
            lower.push_back(static_cast<char>(std::tolower(ch)));
        }
    }
    if (lower == "true" || lower == "1") return true;
    if (lower == "false" || lower == "0") return false;
    return std::nullopt;
}

std::string MakeTranscriptionSegmentMapKey(
    const std::string& speaker_key,
    const std::string& segment_id) {
    std::string out;
    out.reserve(speaker_key.size() + segment_id.size() + 1);
    out.append(speaker_key);
    out.push_back('\0');
    out.append(segment_id);
    return out;
}

bool SenderMetadataMatchesBoundInstance(const SenderContext& sender) {
    return (sender.transport_sid.empty() || sender.transport_sid == sender.key.sid) &&
        (sender.transport_identity.empty() ||
         sender.transport_identity == sender.key.identity);
}

bool SameSenderInstance(const SenderContext& expected,
                        const SenderContext& actual) {
    if (expected.origin != actual.origin) return false;
    const bool expected_bound = expected.key.incarnation != 0;
    const bool actual_bound = actual.key.incarnation != 0;
    if (expected_bound || actual_bound) {
        return expected_bound && actual_bound && expected.key == actual.key &&
            SenderMetadataMatchesBoundInstance(expected) &&
            SenderMetadataMatchesBoundInstance(actual) &&
            IsParticipantTicketActive(expected.ticket, expected.key) &&
            IsParticipantTicketActive(actual.ticket, actual.key);
    }
    if (!expected.transport_sid.empty() || !actual.transport_sid.empty()) {
        return !expected.transport_sid.empty() && !actual.transport_sid.empty() &&
            expected.transport_sid == actual.transport_sid &&
            (expected.transport_identity.empty() || actual.transport_identity.empty() ||
             expected.transport_identity == actual.transport_identity);
    }
    return expected.transport_identity == actual.transport_identity;
}

bool IsSenderInstanceActive(const SenderContext& sender) {
    return sender.key.incarnation == 0 ||
        (SenderMetadataMatchesBoundInstance(sender) &&
         IsParticipantTicketActive(sender.ticket, sender.key));
}

} // namespace

bool Room::PublishData(const std::vector<uint8_t>& payload, bool reliable,
                       const std::vector<std::string>& destination_identities, const std::string& topic) {
    return PublishData(payload, reliable, destination_identities, topic, session_generation_.load(std::memory_order_acquire));
}

bool Room::PublishData(const std::vector<uint8_t>& payload, bool reliable,
                       const std::vector<std::string>& destination_identities, const std::string& topic, uint64_t generation) {
    static constexpr size_t kMaxChunkSize = 15000;
    static constexpr size_t kMaxDataStreamSize = 16 * 1024 * 1024;
    static std::atomic<uint64_t> stream_sequence{0};

    if (payload.size() > kMaxDataStreamSize) {
        Log("DATA", "PAYLOAD_TOO_LARGE", "Rejected DataStream larger than 16 MiB");
        return false;
    }

    webrtc::scoped_refptr<webrtc::DataChannelInterface> dc;
    std::shared_ptr<LocalParticipant> local_participant;
    OutgoingStreamContext send_context;
    {
        std::lock_guard lock(room_mutex_);
        if (!IsNativeGenerationCurrentLocked(generation)) return false;
        dc = reliable ? reliable_dc_ : lossy_dc_;
        local_participant = local_participant_;
        send_context.manager = e2ee_manager_;
        send_context.sender = local_participant_;
        if (send_context.manager)
            send_context.policy_revision = send_context.manager->data_packet_state().policy_revision;
    }

    const std::string local_identity = local_participant ? local_participant->identity() : std::string{};
    const std::string local_sid = local_participant ? local_participant->sid() : std::string{};
    auto send_packet = [&](const std::vector<uint8_t>& bytes) -> bool {
        if (send_context.manager || (dc && dc->state() == webrtc::DataChannelInterface::kOpen)) {
            proto::DataPacket packet;
            if (!packet.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) return false;
            packet.clear_destination_identities();
            for (const auto& destination : destination_identities)
                packet.add_destination_identities(destination);
            return PublishDataPacket(packet, reliable, generation, &send_context) == DataPacketSendResult::Accepted;
        } else {
            OnIncomingDataPacket(bytes, local_sid, topic, generation);
            return true;
        }
    };

    if (payload.size() > kMaxChunkSize) {
        // === DataStream Chunking 大包切片分发逻辑 ===
        std::string stream_id = "ds_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
            std::to_string(stream_sequence.fetch_add(1, std::memory_order_relaxed));

        // 1. 发送 Header 帧
        proto::DataPacket header_pkt;
        header_pkt.set_kind(reliable ? proto::DataPacket::RELIABLE : proto::DataPacket::LOSSY);
        if (local_participant) {
            header_pkt.set_participant_identity(local_identity);
            header_pkt.set_participant_sid(local_sid);
        }
        auto* header = header_pkt.mutable_stream_header();
        header->set_stream_id(stream_id);
        header->set_timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        header->set_topic(topic);
        header->set_total_length(payload.size());
        // A binary DataStream must carry its header oneof even when it has no
        // filename. The receiver (and official SDKs) reject untyped headers.
        header->set_mime_type("application/octet-stream");
        header->mutable_byte_header();

        std::vector<uint8_t> header_bytes(header_pkt.ByteSizeLong());
        header_pkt.SerializeToArray(header_bytes.data(), static_cast<int>(header_bytes.size()));

        if (!send_packet(header_bytes)) {
            return false;
        }

        // 2. 切片发送 Chunks
        size_t total_chunks = (payload.size() + kMaxChunkSize - 1) / kMaxChunkSize;
        for (size_t i = 0; i < total_chunks; ++i) {
            size_t offset = i * kMaxChunkSize;
            size_t chunk_len = std::min(kMaxChunkSize, payload.size() - offset);

            proto::DataPacket chunk_pkt;
            chunk_pkt.set_kind(reliable ? proto::DataPacket::RELIABLE : proto::DataPacket::LOSSY);
            if (local_participant) {
                chunk_pkt.set_participant_identity(local_identity);
                chunk_pkt.set_participant_sid(local_sid);
            }
            auto* chunk = chunk_pkt.mutable_stream_chunk();
            chunk->set_stream_id(stream_id);
            chunk->set_chunk_index(i);
            chunk->set_content(payload.data() + offset, chunk_len);

            std::vector<uint8_t> chunk_bytes(chunk_pkt.ByteSizeLong());
            chunk_pkt.SerializeToArray(chunk_bytes.data(), static_cast<int>(chunk_bytes.size()));

            if (!send_packet(chunk_bytes)) {
                return false;
            }
        }

        // 3. 发送 Trailer 结束帧
        proto::DataPacket trailer_pkt;
        trailer_pkt.set_kind(reliable ? proto::DataPacket::RELIABLE : proto::DataPacket::LOSSY);
        if (local_participant) {
            trailer_pkt.set_participant_identity(local_identity);
            trailer_pkt.set_participant_sid(local_sid);
        }
        auto* trailer = trailer_pkt.mutable_stream_trailer();
        trailer->set_stream_id(stream_id);

        std::vector<uint8_t> trailer_bytes(trailer_pkt.ByteSizeLong());
        trailer_pkt.SerializeToArray(trailer_bytes.data(), static_cast<int>(trailer_bytes.size()));

        if (!send_packet(trailer_bytes)) {
            return false;
        }
        return true;
    }

    proto::DataPacket packet;
    packet.set_kind(reliable ? proto::DataPacket::RELIABLE : proto::DataPacket::LOSSY);
    if (local_participant) {
        packet.set_participant_identity(local_identity);
        packet.set_participant_sid(local_sid);
    }

    auto* user_packet = packet.mutable_user();
    user_packet->set_topic(topic);
    user_packet->set_payload(payload.data(), payload.size());
    for (const auto& dest : destination_identities) {
        user_packet->add_destination_identities(dest);
        packet.add_destination_identities(dest);
    }

    std::vector<uint8_t> data(packet.ByteSizeLong());
    packet.SerializeToArray(data.data(), static_cast<int>(data.size()));

    return send_packet(data);
}

bool Room::PublishDataPacket(const proto::DataPacket& packet, bool reliable) {
    const auto result = PublishDataPacket(
        packet, reliable, session_generation_.load(std::memory_order_acquire));
    return result == DataPacketSendResult::Accepted;
}

Room::DataPacketSendResult Room::PublishDataPacket(
    const proto::DataPacket& packet,
    bool reliable,
    uint64_t expected_generation,
    const OutgoingStreamContext* stream_context) {
    webrtc::scoped_refptr<webrtc::DataChannelInterface> dc;
    const bool stream_packet = packet.has_stream_header() || packet.has_stream_chunk() ||
                               packet.has_stream_trailer();
    const bool protected_packet = stream_packet || packet.has_user() || packet.has_chat_message() ||
                                  packet.has_rpc_request() || packet.has_rpc_ack() || packet.has_rpc_response();
    std::shared_ptr<E2eeManager> encryption_owner;
    E2eeManager::DataPacketState encryption_state{};
    std::shared_ptr<LocalParticipant> sender_owner;
    std::string sender_identity;
    std::string sender_sid;
    std::function<void()> before_admission;
    std::function<void()> after_admission;
    std::function<void()> after_encrypt;
    {
        std::lock_guard lock(room_mutex_);
        if (stream_delivery_instrumentation_) {
            before_admission = stream_delivery_instrumentation_->before_admission;
        }
    }
    if (before_admission) before_admission();
    {
        std::lock_guard lock(room_mutex_);
        if (expected_generation == 0 ||
            session_generation_.load(std::memory_order_acquire) != expected_generation ||
            installed_session_generation_ != expected_generation ||
            connection_state_ != ConnectionState::Connected) {
            return DataPacketSendResult::SessionInvalid;
        }
        dc = reliable ? reliable_dc_ : lossy_dc_;
        if (!dc) return DataPacketSendResult::ChannelUnavailable;
        sender_owner = local_participant_;
        if (sender_owner) {
            sender_identity = sender_owner->identity();
            sender_sid = sender_owner->sid();
        }
        if (protected_packet) {
            encryption_owner = e2ee_manager_;
            if (encryption_owner) encryption_state = encryption_owner->data_packet_state();
            if (stream_context &&
                (stream_context->manager != encryption_owner ||
                 stream_context->policy_revision != encryption_state.policy_revision))
                return DataPacketSendResult::EncryptionContextChanged;
            if (stream_context && stream_context->sender.lock() != sender_owner)
                return DataPacketSendResult::SenderContextChanged;
        }
        if (stream_delivery_instrumentation_) {
            after_admission = stream_delivery_instrumentation_->after_admission;
            after_encrypt = stream_delivery_instrumentation_->after_encrypt_before_commit;
        }
    }
    if (after_admission) after_admission();

    proto::DataPacket final_pkt = packet;
    final_pkt.set_kind(reliable ? proto::DataPacket::RELIABLE : proto::DataPacket::LOSSY);
    if (!sender_identity.empty() || !sender_sid.empty()) {
        if (final_pkt.participant_identity().empty()) {
            final_pkt.set_participant_identity(sender_identity);
        }
        if (final_pkt.participant_sid().empty()) {
            final_pkt.set_participant_sid(sender_sid);
        }
    }

    if (protected_packet && encryption_owner && encryption_state.enabled &&
        encryption_owner->encryption_type() != EncryptionType::NONE) {
        if (encryption_owner->encryption_type() != EncryptionType::GCM ||
            sender_identity.empty() || final_pkt.participant_identity() != sender_identity)
            return DataPacketSendResult::EncryptionFailed;
        proto::EncryptedPacketPayload inner;
        if (packet.has_stream_header()) *inner.mutable_stream_header() = packet.stream_header();
        else if (packet.has_stream_chunk()) *inner.mutable_stream_chunk() = packet.stream_chunk();
        else if (packet.has_stream_trailer()) *inner.mutable_stream_trailer() = packet.stream_trailer();
        else if (packet.has_user()) *inner.mutable_user() = packet.user();
        else if (packet.has_chat_message()) *inner.mutable_chat_message() = packet.chat_message();
        else if (packet.has_rpc_request()) *inner.mutable_rpc_request() = packet.rpc_request();
        else if (packet.has_rpc_ack()) *inner.mutable_rpc_ack() = packet.rpc_ack();
        else if (packet.has_rpc_response()) *inner.mutable_rpc_response() = packet.rpc_response();
        const auto inner_size = inner.ByteSizeLong();
        if (inner_size > DataPacketCryptor::kMaxEncryptedPacketBytes - 12 - 16)
            return DataPacketSendResult::EncryptionFailed;
        std::vector<uint8_t> payload(inner_size);
        if (!inner.SerializeToArray(payload.data(), static_cast<int>(payload.size())))
            return DataPacketSendResult::SerializationFailed;
        auto result = encryption_owner->data_packet_cryptor()->EncryptPacket(
            sender_identity, encryption_state.key_index, payload);
        const auto* sealed = std::get_if<EncryptedDataPacket>(&result);
        if (!sealed) return DataPacketSendResult::EncryptionFailed;
        // Only replace the oneof: destinations, ordering and outer sender metadata
        // remain exactly as admitted. No deprecated Header/Chunk encryption fields.
        auto* envelope = final_pkt.mutable_encrypted_packet();
        envelope->set_encryption_type(proto::Encryption::GCM);
        envelope->set_key_index(sealed->key_index);
        envelope->set_iv(sealed->iv.data(), sealed->iv.size());
        envelope->set_encrypted_value(sealed->ciphertext.data(), sealed->ciphertext.size());
        if (after_encrypt) after_encrypt();
    }

    const auto byte_size = final_pkt.ByteSizeLong();
    if (byte_size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return DataPacketSendResult::SerializationFailed;
    }
    std::vector<uint8_t> bytes(byte_size);
    if (!final_pkt.SerializeToArray(bytes.data(), static_cast<int>(bytes.size()))) {
        return DataPacketSendResult::SerializationFailed;
    }

    if (protected_packet) {
        std::lock_guard lock(room_mutex_);
        if (!IsNativeGenerationCurrentLocked(expected_generation) ||
            connection_state_ != ConnectionState::Connected)
            return DataPacketSendResult::SessionInvalid;
        if (e2ee_manager_ != encryption_owner ||
            (encryption_owner && encryption_owner->data_packet_state().policy_revision !=
                                     encryption_state.policy_revision))
            return DataPacketSendResult::EncryptionContextChanged;
        if (local_participant_ != sender_owner ||
            (stream_context && stream_context->sender.lock() != sender_owner))
            return DataPacketSendResult::SenderContextChanged;
        if ((reliable ? reliable_dc_ : lossy_dc_) != dc)
            return DataPacketSendResult::ChannelUnavailable;
        // This is the final send admission. WebRTC calls use this captured channel
        // outside room_mutex_; a later replacement cannot redirect it to a new one.
    }
    if (dc->state() != webrtc::DataChannelInterface::kOpen) {
        return DataPacketSendResult::ChannelUnavailable;
    }
    webrtc::DataBuffer buffer(
        webrtc::CopyOnWriteBuffer(bytes.data(), bytes.size()),
        /*binary=*/true);
    if (!dc->Send(buffer)) {
        Log("DATA", "SEND_FAILED", "DataChannel rejected the packet");
        return DataPacketSendResult::ChannelRejected;
    }
    return DataPacketSendResult::Accepted;
}

std::shared_ptr<TextStreamWriter> Room::CreateTextStreamWriter(
    const std::string& topic,
    const std::map<std::string, std::string>& attributes,
    const std::string& stream_id,
    std::optional<std::size_t> total_size,
    const std::string& reply_to_id,
    const std::vector<std::string>& destination_identities) {

    std::string sender_id;
    uint64_t generation = 0;
    OutgoingStreamContext stream_context;
    {
        std::lock_guard lock(room_mutex_);
        generation = installed_session_generation_;
        stream_context.manager = e2ee_manager_;
        stream_context.sender = local_participant_;
        if (stream_context.manager)
            stream_context.policy_revision = stream_context.manager->data_packet_state().policy_revision;
        if (local_participant_) {
            sender_id = local_participant_->identity();
        }
    }

    auto weak_self = weak_from_this();
    auto publisher = [weak_self, generation, stream_context](const proto::DataPacket& packet,
                                              bool reliable) -> bool {
        if (auto self = weak_self.lock()) {
            const auto result = self->PublishDataPacket(packet, reliable, generation, &stream_context);
            if (result == DataPacketSendResult::Accepted) return true;
            OperationErrorCode code = OperationErrorCode::DataChannelRejected;
            const char* message = "data channel rejected stream packet";
            switch (result) {
            case DataPacketSendResult::SessionInvalid:
                code = OperationErrorCode::SessionInvalid;
                message = "stream session is no longer valid";
                break;
            case DataPacketSendResult::ChannelUnavailable:
                code = OperationErrorCode::DataChannelUnavailable;
                message = "stream data channel is unavailable";
                break;
            case DataPacketSendResult::SerializationFailed:
                code = OperationErrorCode::SerializationFailed;
                message = "stream packet serialization failed";
                break;
            case DataPacketSendResult::ChannelRejected:
                break;
            case DataPacketSendResult::EncryptionFailed:
                code = OperationErrorCode::EncryptionFailed;
                message = "stream packet encryption failed";
                break;
            case DataPacketSendResult::EncryptionContextChanged:
                code = OperationErrorCode::InvalidState;
                message = "stream encryption context changed";
                break;
            case DataPacketSendResult::SenderContextChanged:
                code = OperationErrorCode::InvalidState;
                message = "stream sender context changed";
                break;
            case DataPacketSendResult::Accepted:
                return true;
            }
            const char* stage = packet.has_stream_header() ? "header" :
                packet.has_stream_chunk() ? "chunk" :
                packet.has_stream_trailer() ? "trailer" : "packet";
            throw OperationError(OperationKind::SendData, code, stage, message);
        }
        const char* stage = packet.has_stream_header() ? "header" :
            packet.has_stream_chunk() ? "chunk" :
            packet.has_stream_trailer() ? "trailer" : "packet";
        throw OperationError(OperationKind::SendData,
                             OperationErrorCode::SessionInvalid,
                             stage,
                             "stream room is no longer available");
    };

    return std::make_shared<TextStreamWriter>(
        std::move(publisher), topic, attributes, stream_id,
        total_size, reply_to_id, destination_identities, sender_id);
}

std::shared_ptr<ByteStreamWriter> Room::CreateByteStreamWriter(
    const std::string& name,
    const std::string& topic,
    const std::map<std::string, std::string>& attributes,
    const std::string& stream_id,
    std::optional<std::size_t> total_size,
    const std::string& mime_type,
    const std::vector<std::string>& destination_identities) {

    std::string sender_id;
    uint64_t generation = 0;
    OutgoingStreamContext stream_context;
    {
        std::lock_guard lock(room_mutex_);
        generation = installed_session_generation_;
        stream_context.manager = e2ee_manager_;
        stream_context.sender = local_participant_;
        if (stream_context.manager)
            stream_context.policy_revision = stream_context.manager->data_packet_state().policy_revision;
        if (local_participant_) {
            sender_id = local_participant_->identity();
        }
    }

    auto weak_self = weak_from_this();
    auto publisher = [weak_self, generation, stream_context](const proto::DataPacket& packet,
                                              bool reliable) -> bool {
        if (auto self = weak_self.lock()) {
            const auto result = self->PublishDataPacket(packet, reliable, generation, &stream_context);
            if (result == DataPacketSendResult::Accepted) return true;
            OperationErrorCode code = OperationErrorCode::DataChannelRejected;
            const char* message = "data channel rejected stream packet";
            switch (result) {
            case DataPacketSendResult::SessionInvalid:
                code = OperationErrorCode::SessionInvalid;
                message = "stream session is no longer valid";
                break;
            case DataPacketSendResult::ChannelUnavailable:
                code = OperationErrorCode::DataChannelUnavailable;
                message = "stream data channel is unavailable";
                break;
            case DataPacketSendResult::SerializationFailed:
                code = OperationErrorCode::SerializationFailed;
                message = "stream packet serialization failed";
                break;
            case DataPacketSendResult::ChannelRejected:
                break;
            case DataPacketSendResult::EncryptionFailed:
                code = OperationErrorCode::EncryptionFailed;
                message = "stream packet encryption failed";
                break;
            case DataPacketSendResult::EncryptionContextChanged:
                code = OperationErrorCode::InvalidState;
                message = "stream encryption context changed";
                break;
            case DataPacketSendResult::SenderContextChanged:
                code = OperationErrorCode::InvalidState;
                message = "stream sender context changed";
                break;
            case DataPacketSendResult::Accepted:
                return true;
            }
            const char* stage = packet.has_stream_header() ? "header" :
                packet.has_stream_chunk() ? "chunk" :
                packet.has_stream_trailer() ? "trailer" : "packet";
            throw OperationError(OperationKind::SendData, code, stage, message);
        }
        const char* stage = packet.has_stream_header() ? "header" :
            packet.has_stream_chunk() ? "chunk" :
            packet.has_stream_trailer() ? "trailer" : "packet";
        throw OperationError(OperationKind::SendData,
                             OperationErrorCode::SessionInvalid,
                             stage,
                             "stream room is no longer available");
    };

    return std::make_shared<ByteStreamWriter>(
        std::move(publisher), name, topic, attributes, stream_id,
        total_size, mime_type, destination_identities, sender_id);
}

bool Room::PublishRpcPacket(const RpcPacket& packet, uint64_t generation,
    const OutgoingStreamContext& context) {
    if (packet.wire_format == RpcWireFormat::StreamV2 &&
        packet.type == RpcPacketType::Response && !packet.has_error) {
        try {
            TextStreamWriter writer(
                [this, generation, context](const proto::DataPacket& data, bool reliable) {
                    return PublishDataPacket(data, reliable, generation, &context) == DataPacketSendResult::Accepted;
                }, "lk.rpc_response", {{"lk.rpc_request_id", packet.request_id}},
                "", packet.payload.size(), "", {packet.destination_identity}, packet.caller_identity);
            writer.Write(packet.payload);
            writer.Close();
            return true;
        } catch (...) { return false; }
    }
    proto::DataPacket wire;
    wire.add_destination_identities(packet.destination_identity);
    if (packet.wire_format == RpcWireFormat::LegacyJson) {
        if (packet.type == RpcPacketType::Ack) return false;
        wire.mutable_user()->set_topic("lk.rpc");
        wire.mutable_user()->set_payload(packet.Encode());
    } else if (packet.type == RpcPacketType::Request) {
        auto* request = wire.mutable_rpc_request();
        request->set_id(packet.request_id); request->set_method(packet.method);
        request->set_payload(packet.payload); request->set_version(1);
        request->set_response_timeout_ms(static_cast<uint32_t>(std::max(1000.0, packet.timeout_sec * 1000.0 - 7000.0)));
    } else if (packet.type == RpcPacketType::Ack) {
        wire.mutable_rpc_ack()->set_request_id(packet.request_id);
    } else {
        auto* response = wire.mutable_rpc_response();
        response->set_request_id(packet.request_id);
        if (packet.has_error || packet.payload.size() > RpcPacket::kMaxPayloadBytes) {
            auto* error = response->mutable_error();
            error->set_code(RpcPacket::ToWireError(packet.has_error ? packet.error_code :
                static_cast<int>(RpcErrorCode::RESPONSE_PAYLOAD_TOO_LARGE)));
            error->set_message(packet.has_error ? packet.error_message.substr(0, 256) : "RPC response payload too large");
        } else response->set_payload(packet.payload);
    }
    return PublishDataPacket(wire, true, generation, &context) == DataPacketSendResult::Accepted;
}

asio::awaitable<std::string> Room::SendRpcRequest(const RpcPacket& packet) {
    auto operation_admission = AdmitOperation(OperationKind::SendData, "SendRpcRequest");
    auto self = shared_from_this();
    if (packet.wire_format == RpcWireFormat::StreamV2 || packet.request_id.empty() || packet.destination_identity.empty() || packet.method.empty() ||
        !std::isfinite(packet.timeout_sec) || packet.timeout_sec <= 0 || packet.timeout_sec > 300)
        throw RpcError(RpcErrorCode::REJECTED, "Invalid RPC request parameters");
    if (packet.payload.size() > RpcPacket::kMaxPayloadBytes)
        throw RpcError(RpcErrorCode::REQUEST_PAYLOAD_TOO_LARGE, "RPC request payload too large");
    auto pending = std::make_shared<PendingRpcCall>();
    pending->destination = packet.destination_identity; pending->wire_format = packet.wire_format;
    {
        std::lock_guard lock(room_mutex_);
        pending->generation = session_generation_.load(std::memory_order_acquire);
        pending->context.manager = e2ee_manager_; pending->context.sender = local_participant_;
        if (e2ee_manager_) pending->context.policy_revision = e2ee_manager_->data_packet_state().policy_revision;
    }
    if (packet.wire_format == RpcWireFormat::ProtobufV1)
        pending->ack_timer = std::make_shared<asio::steady_timer>(executor_, std::chrono::seconds(7));
    pending->timer = std::make_shared<asio::steady_timer>(
        executor_,
        std::chrono::milliseconds(static_cast<int64_t>(packet.timeout_sec * 1000.0))
    );

    {
        std::lock_guard<std::mutex> lock(pending_rpc_mutex_);
        if (pending_rpc_calls_.size() >= 128 || pending_rpc_calls_.contains(packet.request_id))
            throw RpcError(RpcErrorCode::REJECTED, "RPC pending limit or duplicate request id");
        pending_rpc_calls_[packet.request_id] = pending;
    }

    RpcPacket response_packet = co_await asio::async_initiate<decltype(asio::use_awaitable), void(RpcPacket)>(
        [self, pending, packet](auto handler) mutable {
            auto handler_ptr = std::make_shared<decltype(handler)>(std::move(handler));
            RpcPacket cancelled;
            cancelled.has_error = true;
            cancelled.error_code = static_cast<int>(RpcErrorCode::NETWORK_ERROR);
            cancelled.error_message = "Room session closed";
            using Completion = CancellableExecutorCallback<RpcPacket>;
            auto completion = Completion::Create(self->callback_gate_,
                [self, pending, handler_ptr](RpcPacket resp) {
                    {
                        std::lock_guard lock(self->pending_rpc_mutex_);
                        pending->finished = true;
                        pending->completion_cb = {};
                    }
                    std::error_code ec;
                    pending->timer->cancel(ec);
                    if (pending->ack_timer) pending->ack_timer->cancel(ec);
                    (*handler_ptr)(resp);
                }, std::move(cancelled));
            bool cancelled_before_registration = false;
            {
                std::lock_guard lock(self->pending_rpc_mutex_);
                cancelled_before_registration = pending->finished;
                if (!cancelled_before_registration)
                    pending->completion_cb = [weak = std::weak_ptr<Completion>(completion)](const RpcPacket& resp) {
                        if (auto live = weak.lock()) live->Complete(resp);
                    };
            }
            if (cancelled_before_registration) {
                RpcPacket failure; failure.has_error = true;
                failure.error_code = static_cast<int>(RpcErrorCode::NETWORK_ERROR);
                failure.error_message = "RPC session retired before send";
                completion->Complete(std::move(failure));
                return;
            }
            pending->timer->async_wait([completion](const std::error_code& ec) {
                if (!ec) {
                    RpcPacket timeout_resp;
                    timeout_resp.has_error = true;
                    timeout_resp.error_code = static_cast<int>(RpcErrorCode::TIMEOUT);
                    timeout_resp.error_message = "RPC call timed out";
                    completion->Complete(std::move(timeout_resp));
                }
            });

            if (pending->ack_timer) pending->ack_timer->async_wait([completion](const std::error_code& ec) {
                if (!ec) {
                    RpcPacket failure; failure.has_error = true;
                    failure.error_code = static_cast<int>(RpcErrorCode::CONNECTION_TIMEOUT);
                    failure.error_message = "RPC acknowledgement timed out";
                    completion->Complete(std::move(failure));
                }
            });
            if (!self->PublishRpcPacket(packet, pending->generation, pending->context)) {
                RpcPacket failure;
                failure.has_error = true;
                failure.error_code = static_cast<int>(RpcErrorCode::NETWORK_ERROR);
                failure.error_message = "RPC send was not admitted";
                completion->Complete(std::move(failure));
            }
        },
        asio::use_awaitable
    );

    {
        std::lock_guard<std::mutex> lock(pending_rpc_mutex_);
        const auto current = pending_rpc_calls_.find(packet.request_id);
        if (current != pending_rpc_calls_.end() && current->second == pending) pending_rpc_calls_.erase(current);
    }

    if (response_packet.has_error) {
        throw RpcError(static_cast<RpcErrorCode>(response_packet.error_code), response_packet.error_message);
    }

    co_return response_packet.payload;
}

void Room::OnIncomingRpcPacket(const RpcPacket& packet) {
    std::shared_ptr<E2eeManager> policy;
    uint64_t generation = 0, revision = 0;
    {
        std::lock_guard lock(room_mutex_);
        policy = e2ee_manager_;
        generation = session_generation_.load(std::memory_order_acquire);
        // This legacy entry has no authenticated envelope. Required callers
        // must enter through OnIncomingDataPacket instead.
        if (policy && policy->enabled() && policy->encryption_type() != EncryptionType::NONE) return;
        if (policy) revision = policy->data_packet_state().policy_revision;
    }
    OnIncomingRpcPacket(packet, generation, std::move(policy), std::nullopt, revision);
}

void Room::OnIncomingRpcPacket(const RpcPacket& packet, uint64_t generation,
    std::shared_ptr<E2eeManager> encryption_owner, std::optional<SenderContext> sender,
    uint64_t encryption_revision) {
    const auto admitted = [this, generation, encryption_owner, sender, encryption_revision] {
        return IsNativeGenerationCurrentLocked(generation) &&
            e2ee_manager_ == encryption_owner &&
            (!encryption_owner ||
                encryption_owner->data_packet_state().policy_revision == encryption_revision) &&
            (!sender || IsSenderInstanceActive(*sender)) &&
            (!(e2ee_manager_ && e2ee_manager_->enabled() &&
               e2ee_manager_->encryption_type() != EncryptionType::NONE) || encryption_owner);
    };
    if (packet.type == RpcPacketType::Response || packet.type == RpcPacketType::Ack) {
        std::shared_ptr<PendingRpcCall> pending;
        std::function<void(const RpcPacket&)> completion;
        {
            std::lock_guard room_lock(room_mutex_);
            if (!admitted()) return;
            std::lock_guard<std::mutex> lock(pending_rpc_mutex_);
            auto it = pending_rpc_calls_.find(packet.request_id);
            if (it != pending_rpc_calls_.end()) {
                pending = it->second;
                if (pending->destination != packet.caller_identity || pending->wire_format != packet.wire_format ||
                    pending->generation != generation || pending->context.manager != encryption_owner ||
                    pending->context.policy_revision != encryption_revision) return;
                if (packet.type == RpcPacketType::Ack) {
                    std::error_code ec;
                    if (pending->ack_timer) pending->ack_timer->cancel(ec);
                    return;
                }
                completion = pending->completion_cb;
                pending_rpc_calls_.erase(it);
            }
        }
        if (completion) completion(packet);
    } else if (packet.type == RpcPacketType::Request) {
        std::shared_ptr<LocalParticipant> local;
        {
            std::lock_guard lock(room_mutex_);
            if (!admitted()) return;
            local = local_participant_;
        }
        if (!local) return;
        const OutgoingStreamContext reply_context{encryption_owner, local, encryption_revision};
        if (packet.wire_format == RpcWireFormat::ProtobufV1) {
            RpcPacket ack; ack.type = RpcPacketType::Ack; ack.request_id = packet.request_id;
            ack.destination_identity = packet.caller_identity;
            if (!PublishRpcPacket(ack, generation, reply_context)) return;
        }
        auto handler = local->getRpcHandler(packet.method);
        auto self = shared_from_this();

        const bool invalid_version = packet.version !=
            (packet.wire_format == RpcWireFormat::StreamV2 ? 2u : 1u);
        const bool oversized = packet.wire_format != RpcWireFormat::StreamV2 &&
            packet.payload.size() > RpcPacket::kMaxPayloadBytes;
        if (!handler || invalid_version || packet.unsupported_encoding || oversized) {
            RpcPacket err_resp;
            err_resp.wire_format = packet.wire_format;
            err_resp.type = RpcPacketType::Response;
            err_resp.request_id = packet.request_id;
            err_resp.method = packet.method;
            err_resp.caller_identity = local->identity();
            err_resp.destination_identity = packet.caller_identity;
            err_resp.has_error = true;
            err_resp.error_code = static_cast<int>(invalid_version || packet.unsupported_encoding ? RpcErrorCode::UNSUPPORTED_VERSION :
                oversized ? RpcErrorCode::REQUEST_PAYLOAD_TOO_LARGE : RpcErrorCode::UNSUPPORTED_METHOD);
            err_resp.error_message = "RPC request is unsupported";
            PublishRpcPacket(err_resp, generation, reply_context);
            return;
        }

        RpcInvocationData inv_data;
        inv_data.request_id = packet.request_id;
        inv_data.caller_identity = packet.caller_identity;
        inv_data.payload = packet.payload;
        inv_data.response_timeout_sec = packet.timeout_sec;

        livekit::safe_co_spawn(executor_, [self, local, handler, inv_data, packet, generation, admitted, reply_context]() -> asio::awaitable<void> {
            {
                std::lock_guard lock(self->room_mutex_);
                if (!admitted() || self->local_participant_ != local) co_return;
            }
            RpcPacket resp;
            resp.wire_format = packet.wire_format;
            resp.type = RpcPacketType::Response;
            resp.request_id = packet.request_id;
            resp.method = packet.method;
            resp.caller_identity = local->identity();
            resp.destination_identity = packet.caller_identity;

            try {
                resp.payload = co_await handler(inv_data);
            } catch (const RpcError& e) {
                resp.has_error = true;
                resp.error_code = static_cast<int>(e.code());
                resp.error_message = e.message();
            } catch (const std::exception& e) {
                resp.has_error = true;
                resp.error_code = static_cast<int>(RpcErrorCode::APPLICATION_ERROR);
                resp.error_message = e.what();
            } catch (...) {
                resp.has_error = true;
                resp.error_code = static_cast<int>(RpcErrorCode::APPLICATION_ERROR);
                resp.error_message = "Unknown exception in RPC handler";
            }

            {
                std::lock_guard lock(self->room_mutex_);
                if (!admitted() || self->local_participant_ != local) co_return;
            }
            self->PublishRpcPacket(resp, generation, reply_context);
        });
    }
}

void Room::SetDataChannelBufferedAmountLowThreshold(uint64_t threshold, bool reliable) {
    std::lock_guard lock(room_mutex_);
    if (reliable) {
        reliable_buffered_low_threshold_ = threshold;
    } else {
        lossy_buffered_low_threshold_ = threshold;
    }
}

uint64_t Room::GetDataChannelBufferedAmount(bool reliable) const {
    std::lock_guard lock(room_mutex_);
    auto dc = reliable ? reliable_dc_ : lossy_dc_;
    if (dc) {
        return dc->buffered_amount();
    }
    return 0;
}

void Room::OnDataChannelBufferedAmountLow(uint64_t previous_amount, bool reliable) {
    OnDataChannelBufferedAmountLow(previous_amount, reliable, session_generation_.load(std::memory_order_acquire));
}

void Room::OnDataChannelBufferedAmountLow(uint64_t previous_amount, bool reliable, uint64_t generation) {
    BeforeNativeEventCommit(generation);
    std::vector<std::shared_ptr<RoomListener>> snapshot;
    webrtc::scoped_refptr<webrtc::DataChannelInterface> channel;
    {
        std::lock_guard lock(room_mutex_);
        if (!IsNativeGenerationCurrentLocked(generation)) return;
        snapshot = listeners_;
        channel = reliable ? reliable_dc_ : lossy_dc_;
    }
    const uint64_t current_amount = channel ? channel->buffered_amount() : 0;
    for (const auto& listener : snapshot) {
        DeliverListener({generation, {}, true}, listener, [&](RoomListener& target) {
            target.OnDataChannelBufferedAmountLowThresholdChanged(current_amount, reliable);
        });
    }
}

void Room::RetireIncomingReaderLocked(const std::string& stream_id,
                                      const std::string& reason) {
    if (auto it = active_text_readers_.find(stream_id);
        it != active_text_readers_.end()) {
        it->second.reader->OnStreamError(reason);
        active_text_readers_.erase(it);
    }
    if (auto it = active_byte_readers_.find(stream_id);
        it != active_byte_readers_.end()) {
        it->second.reader->OnStreamError(reason);
        active_byte_readers_.erase(it);
    }
    if (auto ts_it = active_transcription_streams_.find(stream_id);
        ts_it != active_transcription_streams_.end()) {
        const std::string seg_key = ts_it->second.segment_map_key;
        active_transcription_streams_.erase(ts_it);
        if (auto seg_it = active_transcription_segments_.find(seg_key);
            seg_it != active_transcription_segments_.end() &&
            seg_it->second.last_stream_id == stream_id) {
            active_transcription_segments_.erase(seg_it);
        }
    }
    incoming_stream_deadlines_.erase(stream_id);
}

size_t Room::RetireIncomingReadersForParticipantLocked(
    const std::shared_ptr<Participant>& participant,
    const std::string& reason) {
    const auto membership = FindMembershipLocked(participant);
    if (!membership) return 0;

    std::vector<std::string> stream_ids;
    const auto collect = [&](const auto& readers) {
        for (const auto& [stream_id, entry] : readers) {
            if (entry.sender.key == membership->key) {
                stream_ids.push_back(stream_id);
            }
        }
    };
    collect(active_text_readers_);
    collect(active_byte_readers_);
    std::sort(stream_ids.begin(), stream_ids.end());
    stream_ids.erase(std::unique(stream_ids.begin(), stream_ids.end()),
                     stream_ids.end());
    for (const auto& stream_id : stream_ids) {
        RetireIncomingReaderLocked(stream_id, reason);
        incoming_data_streams_->Discard(stream_id);
    }
    const std::string p_sid = participant ? participant->sid() : std::string{};
    const std::string p_identity = participant ? participant->identity() : std::string{};
    for (auto it = active_transcription_segments_.begin();
         it != active_transcription_segments_.end();) {
        if ((!p_sid.empty() && it->second.speaker_key == p_sid) ||
            (!p_identity.empty() && it->second.speaker_identity == p_identity)) {
            it = active_transcription_segments_.erase(it);
        } else {
            ++it;
        }
    }
    return stream_ids.size();
}

size_t Room::PurgeIncomingStreamsLocked(
    IncomingDataStreamAssembler::TimePoint now,
    uint64_t generation) {
    if (!IsNativeGenerationCurrentLocked(generation)) return 0;

    std::vector<std::string> expired;
    for (const auto& [stream_id, deadline] : incoming_stream_deadlines_) {
        if (now >= deadline) expired.push_back(stream_id);
    }
    for (const auto& stream_id : expired) {
        RetireIncomingReaderLocked(stream_id, kDataStreamExpired);
        incoming_data_streams_->Discard(stream_id);
    }
    return expired.size();
}

void Room::ScheduleIncomingStreamCleanupLocked(uint64_t generation) {
    if (incoming_stream_deadlines_.empty()) {
        if (incoming_stream_cleanup_timer_) {
            std::error_code ignored;
            incoming_stream_cleanup_timer_->cancel(ignored);
            incoming_stream_cleanup_timer_.reset();
        }
        return;
    }

    const auto next = std::min_element(
        incoming_stream_deadlines_.begin(),
        incoming_stream_deadlines_.end(),
        [](const auto& lhs, const auto& rhs) {
            return lhs.second < rhs.second;
        });
    if (!incoming_stream_cleanup_timer_) {
        incoming_stream_cleanup_timer_ =
            std::make_shared<asio::steady_timer>(executor_);
    }
    auto timer = incoming_stream_cleanup_timer_;
    timer->expires_at(next->second);
    timer->async_wait(
        [weak = weak_from_this(), timer, generation](const std::error_code& ec) {
            if (ec) return;
            if (auto room = weak.lock()) {
                std::lock_guard lock(room->room_mutex_);
                if (!room->IsNativeGenerationCurrentLocked(generation) ||
                    room->incoming_stream_cleanup_timer_ != timer) {
                    return;
                }
                room->PurgeIncomingStreamsLocked(
                    IncomingDataStreamAssembler::Clock::now(), generation);
                room->ScheduleIncomingStreamCleanupLocked(generation);
            }
        });
}

void Room::OnIncomingDataPacket(const std::vector<uint8_t>& payload, const std::string& participant_sid, const std::string& topic) {
    OnIncomingDataPacket(payload, participant_sid, topic, session_generation_.load(std::memory_order_acquire));
}

void Room::OnIncomingDataPacket(const std::vector<uint8_t>& payload, const std::string& participant_sid, const std::string& topic, uint64_t generation) {
    OnIncomingDataPacketAt(
        payload, participant_sid, topic, generation,
        IncomingDataStreamAssembler::Clock::now());
}

void Room::OnIncomingDataPacketAt(
    const std::vector<uint8_t>& payload,
    const std::string& participant_sid,
    const std::string& topic,
    uint64_t generation,
    IncomingDataStreamAssembler::TimePoint now) {
    BeforeNativeEventCommit(generation);
    {
        std::lock_guard lock(room_mutex_);
        if (!IsNativeGenerationCurrentLocked(generation)) return;
    }
    // Protobuf takes an int length. Never narrow an unbounded wire size.
    if (payload.size() > static_cast<size_t>(std::numeric_limits<int>::max())) return;
    std::vector<uint8_t> real_payload = payload;
    std::string real_topic = topic;
    std::string real_sender_sid = participant_sid;
    std::string sender_identity;
    bool is_structured_chat = false;
    EncryptionType encryption_type = EncryptionType::NONE;
    std::shared_ptr<E2eeManager> decrypt_owner;
    std::shared_ptr<E2eeManager> receive_policy;
    uint64_t receive_policy_revision = 0;
    {
        std::lock_guard lock(room_mutex_);
        receive_policy = e2ee_manager_;
        if (receive_policy) receive_policy_revision = receive_policy->data_packet_state().policy_revision;
    }
    SenderContext packet_sender;
    std::shared_ptr<Participant> packet_participant;
    bool sender_captured = false;
    // Called only with room_mutex_ held, at each state/event commit.
    const auto can_commit = [&] {
        return IsNativeGenerationCurrentLocked(generation) &&
            e2ee_manager_ == receive_policy &&
            (!receive_policy || receive_policy->data_packet_state().policy_revision == receive_policy_revision) &&
            (!decrypt_owner || (e2ee_manager_ == decrypt_owner && decrypt_owner->enabled())) &&
            (!sender_captured || IsSenderInstanceActive(packet_sender));
    };

    // 尝试反序列化 Protobuf DataPacket
    proto::DataPacket data_pkt;
    const bool parsed = data_pkt.ParseFromArray(payload.data(), static_cast<int>(payload.size()));
    {
        std::lock_guard lock(room_mutex_);
        if (!can_commit()) return;
        if (e2ee_manager_ && e2ee_manager_->enabled() &&
            e2ee_manager_->encryption_type() != EncryptionType::NONE &&
            (!parsed || !data_pkt.has_encrypted_packet())) return;
    }
    if (parsed) {
        if (!data_pkt.participant_identity().empty()) {
            sender_identity = data_pkt.participant_identity();
        }
        if (!data_pkt.participant_sid().empty()) {
            real_sender_sid = data_pkt.participant_sid();
        }
        {
            std::lock_guard lock(room_mutex_);
            if (!IsNativeGenerationCurrentLocked(generation)) return;
            packet_sender = ResolveSenderContextLocked(
                real_sender_sid, sender_identity, &packet_participant);
            sender_captured = true;
        }
        if (data_pkt.has_encrypted_packet()) {
            std::shared_ptr<StreamDeliveryInstrumentation> hooks;
            {
                std::lock_guard lock(room_mutex_);
                if (!IsNativeGenerationCurrentLocked(generation)) return;
                decrypt_owner = e2ee_manager_;
                if (!decrypt_owner || !decrypt_owner->enabled() ||
                    decrypt_owner->encryption_type() != EncryptionType::GCM) return;
                hooks = stream_delivery_instrumentation_;
            }
            const auto& envelope = data_pkt.encrypted_packet();
            // Validate before copying the ciphertext into the cryptor interface.
            if (envelope.encryption_type() != proto::Encryption::GCM ||
                envelope.iv().size() != 12 ||
                envelope.encrypted_value().size() >
                    DataPacketCryptor::kMaxEncryptedPacketBytes - 12) return;
            EncryptedDataPacket packet{
                EncryptionType::GCM, envelope.key_index(),
                {envelope.iv().begin(), envelope.iv().end()},
                {envelope.encrypted_value().begin(), envelope.encrypted_value().end()}};
            auto result = decrypt_owner->data_packet_cryptor()->DecryptPacket(
                data_pkt.participant_identity(), packet);
            auto* authenticated = std::get_if<AuthenticatedDataPayload>(&result);
            if (!authenticated) return; // An encrypted envelope never falls back to raw data.
            proto::EncryptedPacketPayload inner;
            if (!inner.ParseFromArray(authenticated->bytes.data(),
                                      static_cast<int>(authenticated->bytes.size()))) return;
            encryption_type = authenticated->encryption_type;
            // Preserve the outer sender metadata while replacing the envelope.
            if (inner.has_stream_header())
                *data_pkt.mutable_stream_header() = inner.stream_header();
            else if (inner.has_stream_chunk())
                *data_pkt.mutable_stream_chunk() = inner.stream_chunk();
            else if (inner.has_stream_trailer())
                *data_pkt.mutable_stream_trailer() = inner.stream_trailer();
            else if (inner.has_user())
                *data_pkt.mutable_user() = inner.user();
            else if (inner.has_chat_message())
                *data_pkt.mutable_chat_message() = inner.chat_message();
            else if (inner.has_rpc_request()) *data_pkt.mutable_rpc_request() = inner.rpc_request();
            else if (inner.has_rpc_ack()) *data_pkt.mutable_rpc_ack() = inner.rpc_ack();
            else if (inner.has_rpc_response()) *data_pkt.mutable_rpc_response() = inner.rpc_response();
            else
                return; // Unsupported/empty inner values are never application raw data.
            if (hooks && hooks->after_decrypt_before_commit)
                hooks->after_decrypt_before_commit();
        }
        if (data_pkt.has_rpc_request() || data_pkt.has_rpc_ack() || data_pkt.has_rpc_response()) {
            RpcPacket rpc;
            rpc.caller_identity = sender_identity;
            if (sender_identity.empty()) return;
            {
                std::lock_guard lock(room_mutex_);
                if (!can_commit()) return;
            }
            if (data_pkt.has_rpc_request()) {
                const auto& request = data_pkt.rpc_request();
                rpc.request_id = request.id(); rpc.method = request.method(); rpc.payload = request.payload();
                rpc.timeout_sec = request.response_timeout_ms() / 1000.0;
                rpc.version = request.version(); rpc.unsupported_encoding = !request.compressed_payload().empty();
            } else if (data_pkt.has_rpc_ack()) {
                rpc.type = RpcPacketType::Ack; rpc.request_id = data_pkt.rpc_ack().request_id();
            } else {
                const auto& response = data_pkt.rpc_response();
                rpc.type = RpcPacketType::Response; rpc.request_id = response.request_id(); rpc.payload = response.payload();
                rpc.has_error = response.has_error() || !response.compressed_payload().empty() || rpc.payload.size() > RpcPacket::kMaxPayloadBytes;
                rpc.error_code = response.has_error() ? RpcPacket::FromWireError(response.error().code()) : static_cast<int>(RpcErrorCode::RESPONSE_PAYLOAD_TOO_LARGE);
                rpc.error_message = response.has_error() ? response.error().message() : "Unsupported RPC response payload";
            }
            if (rpc.request_id.empty()) return;
            OnIncomingRpcPacket(rpc, generation, receive_policy, packet_sender, receive_policy_revision);
            return;
        }
        if (data_pkt.has_stream_header()) {
            const auto& header = data_pkt.stream_header();
            if (!header.has_text_header() && !header.has_byte_header()) {
                return;
            }
            std::shared_ptr<Participant> p;
            SenderContext sender;
            std::shared_ptr<TextStreamReader> text_reader;
            std::shared_ptr<ByteStreamReader> byte_reader;
            std::vector<std::shared_ptr<RoomListener>> listeners_snapshot;
            bool invalid_declared_length = false;
            const bool reserved_rpc = header.topic() == "lk.rpc_request" || header.topic() == "lk.rpc_response";
            std::optional<RpcPacket> stream_rpc;
            OutgoingStreamContext rpc_ack_context;
            if (reserved_rpc) {
                // This client originates v1 requests, so v2 responses are unsolicited.
                if (!header.has_text_header() || header.topic() != "lk.rpc_request") return;
                const auto& attrs = header.attributes();
                auto id = attrs.find("lk.rpc_request_id");
                auto method = attrs.find("lk.rpc_request_method");
                auto timeout = attrs.find("lk.rpc_request_response_timeout_ms");
                auto version = attrs.find("lk.rpc_request_version");
                if (id == attrs.end() || id->second.empty() || method == attrs.end() ||
                    method->second.empty() || timeout == attrs.end() || version == attrs.end() ||
                    version->second != "2") return;
                RpcPacket rpc;
                rpc.wire_format = RpcWireFormat::StreamV2; rpc.version = 2;
                rpc.request_id = id->second; rpc.method = method->second;
                rpc.caller_identity = sender_identity;
                try {
                    size_t consumed = 0;
                    const auto ms = std::stoull(timeout->second, &consumed);
                    if (consumed != timeout->second.size() || ms > 300000) return;
                    rpc.timeout_sec = ms / 1000.0;
                } catch (...) { return; }
                stream_rpc = std::move(rpc);
            }


            if (header.has_text_header()) {
                TextStreamInfo info;
                info.stream_id = header.stream_id();
                info.topic = header.topic();
                info.mime_type = header.mime_type();
                info.timestamp = header.timestamp();
                if (header.has_total_length()) {
                    if (header.total_length() <=
                        std::numeric_limits<size_t>::max()) {
                        info.total_length =
                            static_cast<size_t>(header.total_length());
                    } else {
                        invalid_declared_length = true;
                    }
                }
                for (const auto& [k, v] : header.attributes()) {
                    info.attributes[k] = v;
                }
                info.sender_identity = sender_identity;
                info.sender_sid = real_sender_sid;
                const auto& th = header.text_header();
                info.operation_type = th.operation_type();
                info.version = th.version();
                info.reply_to_stream_id = th.reply_to_stream_id();
                for (const auto& att : th.attached_stream_ids()) {
                    info.attached_stream_ids.push_back(att);
                }
                info.generated = th.generated();

                {
                    std::lock_guard lk(room_mutex_);
                    if (!can_commit()) return;
                    sender = packet_sender;
                    p = packet_participant;
                    const auto text = active_text_readers_.find(header.stream_id());
                    const auto bytes = active_byte_readers_.find(header.stream_id());
                    if ((text != active_text_readers_.end() &&
                         !SameSenderInstance(text->second.sender, sender)) ||
                        (bytes != active_byte_readers_.end() &&
                         !SameSenderInstance(bytes->second.sender, sender))) {
                        return;
                    }
                    RetireIncomingReaderLocked(
                        header.stream_id(), kDataStreamReplaced);
                    incoming_data_streams_->Discard(header.stream_id());
                    if (header.has_total_length()) {
                        incoming_data_streams_->Begin(
                            header.stream_id(), header.topic(),
                            header.total_length(), sender_identity,
                            real_sender_sid, now);
                    }
                    text_reader = std::make_shared<TextStreamReader>(
                        std::move(info), incoming_reader_budget_);
                    if (invalid_declared_length) {
                        text_reader->OnStreamError(
                            kDataStreamDeclaredLengthExceeded);
                    }
                    if (text_reader->admitted() &&
                        !text_reader->is_closed()) {
                        active_text_readers_[header.stream_id()] = {
                            text_reader, encryption_type, sender, stream_rpc, receive_policy, receive_policy_revision};
                        if (!reserved_rpc && header.topic() == "lk.transcription") {
                            const auto findAttr = [&](const char* key) -> std::string {
                                const auto it = header.attributes().find(key);
                                return (it != header.attributes().end()) ? it->second : std::string{};
                            };
                            std::string seg_id = findAttr("lk.segment_id");
                            if (seg_id.empty()) {
                                seg_id = header.stream_id();
                            }
                            const std::string track_sid = findAttr("lk.transcribed_track_id");
                            std::string lang = findAttr("lk.language");
                            if (lang.empty()) {
                                lang = findAttr("language");
                            }
                            const std::string source_text = findAttr("cohavora.source_text");
                            const std::string source_language = findAttr("cohavora.source_language");
                            const auto explicit_translated = ParseTranscriptionBoolAttr(findAttr("cohavora.translated"));
                            const bool is_translated = explicit_translated.value_or(!source_text.empty());
                            std::string fallback_identity = findAttr("lk.participant_identity");
                            if (fallback_identity.empty()) {
                                fallback_identity = sender_identity;
                            }
                            auto [tr_participant, tr_pub] = ResolveTranscriptionTargetLocked(track_sid, fallback_identity);
                            (void)tr_pub;
                            const std::string speaker_identity = tr_participant
                                ? tr_participant->identity()
                                : (!fallback_identity.empty() ? fallback_identity : sender_identity);
                            std::string speaker_key = (tr_participant && !tr_participant->sid().empty())
                                ? tr_participant->sid()
                                : speaker_identity;
                            if (speaker_key.empty()) {
                                speaker_key = header.stream_id();
                            }

                            for (auto seg_it = active_transcription_segments_.begin();
                                 seg_it != active_transcription_segments_.end();) {
                                if (seg_it->second.speaker_key == speaker_key &&
                                    seg_it->second.segment_id != seg_id) {
                                    seg_it = active_transcription_segments_.erase(seg_it);
                                } else {
                                    ++seg_it;
                                }
                            }
                            while (active_transcription_segments_.size() >= kMaxActiveTranscriptionSegments) {
                                auto oldest = std::min_element(
                                    active_transcription_segments_.begin(),
                                    active_transcription_segments_.end(),
                                    [](const auto& a, const auto& b) {
                                        return a.second.first_received_time_ms < b.second.first_received_time_ms;
                                    });
                                if (oldest == active_transcription_segments_.end()) break;
                                active_transcription_segments_.erase(oldest);
                            }

                            const std::string map_key = MakeTranscriptionSegmentMapKey(speaker_key, seg_id);
                            const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count();
                            const auto explicit_final = ParseTranscriptionBoolAttr(findAttr("lk.transcription_final"));
                            auto& seg_entry = active_transcription_segments_[map_key];
                            if (seg_entry.segment_id.empty()) {
                                seg_entry.map_key = map_key;
                                seg_entry.speaker_key = speaker_key;
                                seg_entry.speaker_identity = speaker_identity;
                                seg_entry.segment_id = seg_id;
                                seg_entry.last_stream_id = header.stream_id();
                                seg_entry.track_sid = track_sid;
                                seg_entry.language = lang;
                                seg_entry.source_text = source_text;
                                seg_entry.source_language = source_language;
                                seg_entry.is_translated = is_translated;
                                seg_entry.generated = header.text_header().generated();
                                seg_entry.first_received_time_ms = now_ms;
                                seg_entry.last_received_time_ms = now_ms;
                            } else {
                                if (seg_entry.last_stream_id != header.stream_id()) {
                                    if (!seg_entry.last_stream_id.empty()) {
                                        active_transcription_streams_.erase(seg_entry.last_stream_id);
                                    }
                                    seg_entry.last_stream_id = header.stream_id();
                                    seg_entry.pending_replace = true;
                                }
                                if (!track_sid.empty()) seg_entry.track_sid = track_sid;
                                if (!lang.empty()) seg_entry.language = lang;
                                if (!source_text.empty()) seg_entry.source_text = source_text;
                                if (!source_language.empty()) seg_entry.source_language = source_language;
                                if (is_translated) seg_entry.is_translated = true;
                                if (header.text_header().generated()) seg_entry.generated = true;
                                seg_entry.last_received_time_ms = now_ms;
                            }
                            if (explicit_final.has_value()) {
                                seg_entry.has_explicit_final = true;
                                seg_entry.explicit_final_value = *explicit_final;
                            }
                            active_transcription_streams_[header.stream_id()] = {
                                map_key,
                                speaker_identity,
                                sender_identity,
                                track_sid,
                                sender,
                                seg_entry.generated
                            };
                        }
                        if (reserved_rpc) rpc_ack_context = {receive_policy, local_participant_, receive_policy_revision};
                        incoming_stream_deadlines_[header.stream_id()] =
                            now + incoming_reader_budget_->limits()
                                      .stream_ttl;
                        ScheduleIncomingStreamCleanupLocked(generation);
                    } else {
                        incoming_data_streams_->Discard(header.stream_id());
                    }
                    if (!reserved_rpc) {
                    ParticipantEvent event = p
                        ? MakeParticipantEventLocked(
                            ParticipantEventKind::TextStreamOpened,
                            p,
                            p.get() == local_participant_.get())
                        : ParticipantEvent{};
                    event.kind = ParticipantEventKind::TextStreamOpened;
                    event.sender = sender;
                    event.text_reader = text_reader;
                    EnqueueParticipantEventLocked(std::move(event), receive_policy, receive_policy_revision);
                    listeners_snapshot = listeners_;
                    }
                }
                if (reserved_rpc && text_reader->admitted() && !text_reader->is_closed()) {
                    RpcPacket ack; ack.type = RpcPacketType::Ack;
                    ack.request_id = stream_rpc->request_id; ack.destination_identity = sender_identity;
                    if (!PublishRpcPacket(ack, generation, rpc_ack_context)) {
                        std::lock_guard lk(room_mutex_);
                        if (const auto it = active_text_readers_.find(header.stream_id());
                            it != active_text_readers_.end() && it->second.reader == text_reader)
                            RetireIncomingReaderLocked(header.stream_id(), kDataStreamAssemblyRejected);
                    }
                }
                for (const auto& listener : listeners_snapshot) {
                    DeliverListener({generation, {}, true, false, receive_policy, {}, receive_policy_revision}, listener, [&](RoomListener& target) {
                        target.OnTextStreamOpened(text_reader, p);
                    }, true);
                }
            } else {
                ByteStreamInfo info;
                info.stream_id = header.stream_id();
                info.topic = header.topic();
                info.mime_type = header.mime_type();
                info.timestamp = header.timestamp();
                if (header.has_total_length()) {
                    if (header.total_length() <=
                        std::numeric_limits<size_t>::max()) {
                        info.total_length =
                            static_cast<size_t>(header.total_length());
                    } else {
                        invalid_declared_length = true;
                    }
                }
                for (const auto& [k, v] : header.attributes()) {
                    info.attributes[k] = v;
                }
                info.sender_identity = sender_identity;
                info.sender_sid = real_sender_sid;
                if (header.has_byte_header()) {
                    info.name = header.byte_header().name();
                }

                {
                    std::lock_guard lk(room_mutex_);
                    if (!can_commit()) return;
                    sender = packet_sender;
                    p = packet_participant;
                    const auto text = active_text_readers_.find(header.stream_id());
                    const auto bytes = active_byte_readers_.find(header.stream_id());
                    if ((text != active_text_readers_.end() &&
                         !SameSenderInstance(text->second.sender, sender)) ||
                        (bytes != active_byte_readers_.end() &&
                         !SameSenderInstance(bytes->second.sender, sender))) {
                        return;
                    }
                    RetireIncomingReaderLocked(
                        header.stream_id(), kDataStreamReplaced);
                    incoming_data_streams_->Discard(header.stream_id());
                    if (header.has_total_length()) {
                        incoming_data_streams_->Begin(
                            header.stream_id(), header.topic(),
                            header.total_length(), sender_identity,
                            real_sender_sid, now);
                    }
                    byte_reader = std::make_shared<ByteStreamReader>(
                        std::move(info), incoming_reader_budget_);
                    if (invalid_declared_length) {
                        byte_reader->OnStreamError(
                            kDataStreamDeclaredLengthExceeded);
                    }
                    if (byte_reader->admitted() &&
                        !byte_reader->is_closed()) {
                        active_byte_readers_[header.stream_id()] = {
                            byte_reader, encryption_type, sender, {}, receive_policy, receive_policy_revision};
                        incoming_stream_deadlines_[header.stream_id()] =
                            now + incoming_reader_budget_->limits()
                                      .stream_ttl;
                        ScheduleIncomingStreamCleanupLocked(generation);
                    } else {
                        incoming_data_streams_->Discard(header.stream_id());
                    }
                    ParticipantEvent event = p
                        ? MakeParticipantEventLocked(
                            ParticipantEventKind::ByteStreamOpened,
                            p,
                            p.get() == local_participant_.get())
                        : ParticipantEvent{};
                    event.kind = ParticipantEventKind::ByteStreamOpened;
                    event.sender = sender;
                    event.byte_reader = byte_reader;
                    EnqueueParticipantEventLocked(std::move(event), receive_policy, receive_policy_revision);
                    listeners_snapshot = listeners_;
                }
                for (const auto& listener : listeners_snapshot) {
                    DeliverListener({generation, {}, true, false, receive_policy, {}, receive_policy_revision}, listener, [&](RoomListener& target) {
                        target.OnByteStreamOpened(byte_reader, p);
                    }, true);
                }
            }
            return;
        } else if (data_pkt.has_stream_chunk()) {
            const auto& chunk = data_pkt.stream_chunk();
            const auto& content = chunk.content();

            std::optional<AssembledDataStream> assembled;
            std::optional<TranscriptionEvent> chunk_transcription_ev;
            std::shared_ptr<Participant> chunk_transcription_participant;
            std::shared_ptr<TrackPublication> chunk_transcription_pub;
            std::vector<std::shared_ptr<RoomListener>> chunk_transcription_listeners;
            {
                std::lock_guard lk(room_mutex_);
                if (!can_commit()) return;
                const auto text = active_text_readers_.find(chunk.stream_id());
                const auto bytes = active_byte_readers_.find(chunk.stream_id());
                if ((text != active_text_readers_.end() &&
                     (text->second.policy != receive_policy || text->second.policy_revision != receive_policy_revision)) ||
                    (bytes != active_byte_readers_.end() &&
                     (bytes->second.policy != receive_policy || bytes->second.policy_revision != receive_policy_revision))) {
                    RetireIncomingReaderLocked(chunk.stream_id(), kDataStreamEncryptionTypeMismatch);
                    incoming_data_streams_->Discard(chunk.stream_id());
                    ScheduleIncomingStreamCleanupLocked(generation);
                    return;
                }
                if ((text != active_text_readers_.end() &&
                     !SameSenderInstance(text->second.sender, packet_sender)) ||
                    (bytes != active_byte_readers_.end() &&
                     !SameSenderInstance(bytes->second.sender, packet_sender))) {
                    return;
                }
                if (auto deadline = incoming_stream_deadlines_.find(
                        chunk.stream_id());
                    deadline != incoming_stream_deadlines_.end() &&
                    now >= deadline->second) {
                    RetireIncomingReaderLocked(
                        chunk.stream_id(), kDataStreamExpired);
                    incoming_data_streams_->Discard(chunk.stream_id());
                    ScheduleIncomingStreamCleanupLocked(generation);
                    return;
                }
                if ((text != active_text_readers_.end() &&
                     text->second.encryption_type != encryption_type) ||
                    (bytes != active_byte_readers_.end() &&
                     bytes->second.encryption_type != encryption_type)) {
                    RetireIncomingReaderLocked(chunk.stream_id(), kDataStreamEncryptionTypeMismatch);
                    incoming_data_streams_->Discard(chunk.stream_id());
                    ScheduleIncomingStreamCleanupLocked(generation);
                    return;
                }
                const bool assembler_was_active =
                    incoming_data_streams_->Contains(chunk.stream_id());
                assembled = incoming_data_streams_->AddChunk(
                    chunk.stream_id(),
                    chunk.chunk_index(),
                    std::span<const uint8_t>(
                        reinterpret_cast<const uint8_t*>(content.data()),
                        content.size()),
                    now);
                const bool assembler_rejected =
                    assembler_was_active && !assembled &&
                    !incoming_data_streams_->Contains(chunk.stream_id());
                if (assembler_rejected) {
                    RetireIncomingReaderLocked(
                        chunk.stream_id(), kDataStreamAssemblyRejected);
                    ScheduleIncomingStreamCleanupLocked(generation);
                    return;
                }

                bool reader_accepted = false;
                bool reader_terminated = false;
                if (auto it = active_text_readers_.find(chunk.stream_id()); it != active_text_readers_.end()) {
                    reader_accepted = it->second.reader->TryOnChunkUpdate(
                        chunk.chunk_index(), content);
                    if (!reader_accepted && it->second.reader->is_closed()) {
                        reader_terminated = true;
                    }
                }
                if (auto it = active_byte_readers_.find(chunk.stream_id()); it != active_byte_readers_.end()) {
                    reader_accepted = it->second.reader->TryOnChunkUpdate(
                        chunk.chunk_index(),
                        reinterpret_cast<const uint8_t*>(content.data()),
                        content.size());
                    if (!reader_accepted && it->second.reader->is_closed()) {
                        reader_terminated = true;
                    }
                }
                if (reader_terminated) {
                    RetireIncomingReaderLocked(
                        chunk.stream_id(), kDataStreamBufferLimitExceeded);
                    incoming_data_streams_->Discard(chunk.stream_id());
                    ScheduleIncomingStreamCleanupLocked(generation);
                    return;
                }
                if (text != active_text_readers_.end() && text->second.rpc) assembled.reset();
                if (reader_accepted) {
                    incoming_stream_deadlines_[chunk.stream_id()] =
                        now + incoming_reader_budget_->limits().stream_ttl;
                    ScheduleIncomingStreamCleanupLocked(generation);

                    if (!content.empty()) {
                        if (auto ts_it = active_transcription_streams_.find(chunk.stream_id());
                            ts_it != active_transcription_streams_.end()) {
                            if (auto seg_it = active_transcription_segments_.find(ts_it->second.segment_map_key);
                                seg_it != active_transcription_segments_.end()) {
                                auto& seg_entry = seg_it->second;
                                if (seg_entry.pending_replace) {
                                    seg_entry.text = content;
                                    seg_entry.pending_replace = false;
                                } else {
                                    seg_entry.text.append(content);
                                }
                                const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch()).count();
                                seg_entry.last_received_time_ms = now_ms;

                                TranscriptionEvent tr_ev;
                                tr_ev.sender_identity = ts_it->second.sender_identity;
                                tr_ev.transcribed_participant_identity = seg_entry.speaker_identity;
                                tr_ev.track_sid = seg_entry.track_sid;
                                tr_ev.generated = seg_entry.generated;
                                TranscriptionSegment seg_dto;
                                seg_dto.id = seg_entry.segment_id;
                                seg_dto.text = seg_entry.text;
                                seg_dto.source_text = seg_entry.source_text;
                                seg_dto.source_language = seg_entry.source_language;
                                seg_dto.is_translated = seg_entry.is_translated;
                                seg_dto.is_final = seg_entry.has_explicit_final && seg_entry.explicit_final_value;
                                seg_dto.language = seg_entry.language;
                                seg_dto.first_received_time_ms = seg_entry.first_received_time_ms;
                                seg_dto.last_received_time_ms = seg_entry.last_received_time_ms;
                                tr_ev.segments.push_back(std::move(seg_dto));

                                if (tr_ev.segments.front().is_final) {
                                    seg_entry.emitted_final = true;
                                    seg_entry.emitted_final_text = seg_entry.text;
                                }

                                std::tie(chunk_transcription_participant, chunk_transcription_pub) =
                                    ResolveTranscriptionTargetLocked(seg_entry.track_sid, seg_entry.speaker_identity);
                                ParticipantEvent p_ev = chunk_transcription_participant
                                    ? MakeParticipantEventLocked(
                                        ParticipantEventKind::TranscriptionReceived,
                                        chunk_transcription_participant,
                                        chunk_transcription_participant.get() == local_participant_.get())
                                    : ParticipantEvent{};
                                p_ev.kind = ParticipantEventKind::TranscriptionReceived;
                                p_ev.sender = packet_sender;
                                p_ev.transcription = tr_ev;
                                EnqueueParticipantEventLocked(
                                    std::move(p_ev), receive_policy, receive_policy_revision);
                                chunk_transcription_ev = std::move(tr_ev);
                                chunk_transcription_listeners = listeners_;
                            }
                        }
                    }
                }
            }
            if (chunk_transcription_ev) {
                for (const auto& listener : chunk_transcription_listeners) {
                    DeliverListener(
                        {generation, {}, true, false, receive_policy, {}, receive_policy_revision},
                        listener,
                        [&](RoomListener& target) {
                            target.OnTranscriptionReceived(
                                *chunk_transcription_ev,
                                chunk_transcription_participant,
                                chunk_transcription_pub);
                        },
                        true);
                }
            }
            if (!assembled) {
                return;
            }
            real_payload = std::move(assembled->payload);
            real_topic = std::move(assembled->topic);
            sender_identity = std::move(assembled->sender_identity);
            real_sender_sid = std::move(assembled->sender_sid);
        } else if (data_pkt.has_stream_trailer()) {
            const auto& trailer = data_pkt.stream_trailer();
            std::shared_ptr<TextStreamReader> rpc_reader;
            std::optional<RpcPacket> completed_rpc;

            std::optional<AssembledDataStream> assembled;
            std::optional<TranscriptionEvent> trailer_transcription_ev;
            std::shared_ptr<Participant> trailer_transcription_participant;
            std::shared_ptr<TrackPublication> trailer_transcription_pub;
            std::vector<std::shared_ptr<RoomListener>> trailer_transcription_listeners;
            {
                std::lock_guard lk(room_mutex_);
                if (!can_commit()) return;
                const auto text = active_text_readers_.find(trailer.stream_id());
                const auto bytes = active_byte_readers_.find(trailer.stream_id());
                if ((text != active_text_readers_.end() &&
                     (text->second.policy != receive_policy || text->second.policy_revision != receive_policy_revision)) ||
                    (bytes != active_byte_readers_.end() &&
                     (bytes->second.policy != receive_policy || bytes->second.policy_revision != receive_policy_revision))) {
                    RetireIncomingReaderLocked(trailer.stream_id(), kDataStreamEncryptionTypeMismatch);
                    incoming_data_streams_->Discard(trailer.stream_id());
                    ScheduleIncomingStreamCleanupLocked(generation);
                    return;
                }
                if ((text != active_text_readers_.end() &&
                     !SameSenderInstance(text->second.sender, packet_sender)) ||
                    (bytes != active_byte_readers_.end() &&
                     !SameSenderInstance(bytes->second.sender, packet_sender))) {
                    return;
                }
                if ((text != active_text_readers_.end() &&
                     text->second.encryption_type != encryption_type) ||
                    (bytes != active_byte_readers_.end() &&
                     bytes->second.encryption_type != encryption_type)) {
                    RetireIncomingReaderLocked(
                        trailer.stream_id(), kDataStreamEncryptionTypeMismatch);
                    incoming_data_streams_->Discard(trailer.stream_id());
                    ScheduleIncomingStreamCleanupLocked(generation);
                    return;
                }
                if (auto deadline = incoming_stream_deadlines_.find(
                        trailer.stream_id());
                    deadline != incoming_stream_deadlines_.end() &&
                    now >= deadline->second) {
                    RetireIncomingReaderLocked(
                        trailer.stream_id(), kDataStreamExpired);
                    incoming_data_streams_->Discard(trailer.stream_id());
                    ScheduleIncomingStreamCleanupLocked(generation);
                    return;
                }
                std::map<std::string, std::string> attrs(trailer.attributes().begin(), trailer.attributes().end());
                const bool assembler_was_active =
                    incoming_data_streams_->Contains(trailer.stream_id());
                if (trailer.reason().empty()) {
                    assembled = incoming_data_streams_->Finish(
                        trailer.stream_id(), now);
                } else {
                    incoming_data_streams_->Discard(trailer.stream_id());
                }
                const bool incomplete_normal_stream =
                    trailer.reason().empty() && assembler_was_active &&
                    !assembled;
                if (text != active_text_readers_.end()) {
                    if (text->second.rpc) {
                        completed_rpc = text->second.rpc;
                        rpc_reader = text->second.reader;
                    }
                    if (incomplete_normal_stream) {
                        text->second.reader->OnStreamError(kDataStreamAssemblyRejected);
                    } else {
                        text->second.reader->OnStreamClose(trailer.reason(), attrs);
                    }
                    active_text_readers_.erase(text);
                }
                if (bytes != active_byte_readers_.end()) {
                    if (incomplete_normal_stream) {
                        bytes->second.reader->OnStreamError(kDataStreamAssemblyRejected);
                    } else {
                        bytes->second.reader->OnStreamClose(trailer.reason(), attrs);
                    }
                    active_byte_readers_.erase(bytes);
                }
                if (auto ts_it = active_transcription_streams_.find(trailer.stream_id());
                    ts_it != active_transcription_streams_.end()) {
                    const std::string seg_key = ts_it->second.segment_map_key;
                    const std::string sender_id = ts_it->second.sender_identity;
                    active_transcription_streams_.erase(ts_it);

                    if (auto seg_it = active_transcription_segments_.find(seg_key);
                        seg_it != active_transcription_segments_.end()) {
                        auto& seg_entry = seg_it->second;
                        std::optional<bool> trailer_final;
                        if (auto tf = attrs.find("lk.transcription_final"); tf != attrs.end()) {
                            trailer_final = ParseTranscriptionBoolAttr(tf->second);
                        }
                        if (auto tl = attrs.find("lk.language"); tl != attrs.end() && !tl->second.empty()) {
                            seg_entry.language = tl->second;
                        } else if (auto tl2 = attrs.find("language"); tl2 != attrs.end() && !tl2->second.empty()) {
                            seg_entry.language = tl2->second;
                        }
                        const bool is_normal_close = trailer.reason().empty() && !incomplete_normal_stream;
                        const bool is_final = trailer_final.has_value()
                            ? *trailer_final
                            : (seg_entry.has_explicit_final ? seg_entry.explicit_final_value : is_normal_close);
                        if (is_normal_close && is_final && !seg_entry.text.empty() &&
                            (!seg_entry.emitted_final || seg_entry.emitted_final_text != seg_entry.text)) {
                            const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count();
                            seg_entry.last_received_time_ms = now_ms;
                            TranscriptionEvent tr_ev;
                            tr_ev.sender_identity = sender_id;
                            tr_ev.transcribed_participant_identity = seg_entry.speaker_identity;
                            tr_ev.track_sid = seg_entry.track_sid;
                            tr_ev.generated = seg_entry.generated;
                            TranscriptionSegment seg_dto;
                            seg_dto.id = seg_entry.segment_id;
                            seg_dto.text = seg_entry.text;
                            seg_dto.source_text = seg_entry.source_text;
                            seg_dto.source_language = seg_entry.source_language;
                            seg_dto.is_translated = seg_entry.is_translated;
                            seg_dto.is_final = true;
                            seg_dto.language = seg_entry.language;
                            seg_dto.first_received_time_ms = seg_entry.first_received_time_ms;
                            seg_dto.last_received_time_ms = seg_entry.last_received_time_ms;
                            tr_ev.segments.push_back(std::move(seg_dto));

                            std::tie(trailer_transcription_participant, trailer_transcription_pub) =
                                ResolveTranscriptionTargetLocked(seg_entry.track_sid, seg_entry.speaker_identity);
                            ParticipantEvent p_ev = trailer_transcription_participant
                                ? MakeParticipantEventLocked(
                                    ParticipantEventKind::TranscriptionReceived,
                                    trailer_transcription_participant,
                                    trailer_transcription_participant.get() == local_participant_.get())
                                : ParticipantEvent{};
                            p_ev.kind = ParticipantEventKind::TranscriptionReceived;
                            p_ev.sender = packet_sender;
                            p_ev.transcription = tr_ev;
                            EnqueueParticipantEventLocked(
                                std::move(p_ev), receive_policy, receive_policy_revision);
                            trailer_transcription_ev = std::move(tr_ev);
                            trailer_transcription_listeners = listeners_;
                        }
                        if (!is_normal_close || is_final) {
                            if (seg_entry.last_stream_id == trailer.stream_id()) {
                                active_transcription_segments_.erase(seg_it);
                            }
                        }
                    }
                }
                incoming_stream_deadlines_.erase(trailer.stream_id());
                ScheduleIncomingStreamCleanupLocked(generation);
            }
            if (trailer_transcription_ev) {
                for (const auto& listener : trailer_transcription_listeners) {
                    DeliverListener(
                        {generation, {}, true, false, receive_policy, {}, receive_policy_revision},
                        listener,
                        [&](RoomListener& target) {
                            target.OnTranscriptionReceived(
                                *trailer_transcription_ev,
                                trailer_transcription_participant,
                                trailer_transcription_pub);
                        },
                        true);
                }
            }
            if (completed_rpc) {
                if (rpc_reader->is_closed() && !rpc_reader->is_failed()) {
                    try { completed_rpc->payload = rpc_reader->ReadAll(); }
                    catch (...) { return; } // Never dispatch a failed or over-budget stream.
                    OnIncomingRpcPacket(*completed_rpc, generation, receive_policy, packet_sender, receive_policy_revision);
                }
                return;
            }
            if (!assembled) {
                return;
            }
            real_payload = std::move(assembled->payload);
            real_topic = std::move(assembled->topic);
            sender_identity = std::move(assembled->sender_identity);
            real_sender_sid = std::move(assembled->sender_sid);
        } else if (data_pkt.has_user()) {
            const auto& user_pkt = data_pkt.user();
            const std::string& p_bytes = user_pkt.payload();
            real_payload.assign(p_bytes.begin(), p_bytes.end());
            real_topic = user_pkt.topic();
        } else if (data_pkt.has_chat_message()) {
            const auto& pb_chat = data_pkt.chat_message();
            real_payload.assign(pb_chat.message().begin(), pb_chat.message().end());
            real_topic = "lk.chat";
            is_structured_chat = true;
        } else if (data_pkt.has_transcription()) {
            const auto& pb_tr = data_pkt.transcription();
            TranscriptionEvent ev;
            ev.sender_identity = sender_identity;
            ev.transcribed_participant_identity = pb_tr.transcribed_participant_identity();
            ev.track_sid = pb_tr.track_id();
            ev.generated = false;
            ev.segments.reserve(static_cast<size_t>(pb_tr.segments_size()));
            const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            for (const auto& s : pb_tr.segments()) {
                TranscriptionSegment seg;
                seg.id = s.id();
                seg.text = s.text();
                seg.start_time_ms = s.start_time();
                seg.end_time_ms = s.end_time();
                seg.is_final = s.final();
                seg.language = s.language();
                seg.first_received_time_ms = now_ms;
                seg.last_received_time_ms = now_ms;
                ev.segments.push_back(std::move(seg));
            }
            if (!ev.segments.empty()) {
                std::shared_ptr<Participant> target_participant;
                std::shared_ptr<TrackPublication> target_pub;
                std::vector<std::shared_ptr<RoomListener>> tr_listeners_snapshot;
                {
                    std::lock_guard lock(room_mutex_);
                    if (!can_commit()) return;
                    const std::string fallback_id = ev.transcribed_participant_identity.empty()
                        ? sender_identity
                        : ev.transcribed_participant_identity;
                    std::tie(target_participant, target_pub) =
                        ResolveTranscriptionTargetLocked(ev.track_sid, fallback_id);
                    if (ev.transcribed_participant_identity.empty() && target_participant) {
                        ev.transcribed_participant_identity = target_participant->identity();
                    }
                    ParticipantEvent p_ev = target_participant
                        ? MakeParticipantEventLocked(
                            ParticipantEventKind::TranscriptionReceived,
                            target_participant,
                            target_participant.get() == local_participant_.get())
                        : ParticipantEvent{};
                    p_ev.kind = ParticipantEventKind::TranscriptionReceived;
                    p_ev.sender = packet_sender;
                    p_ev.transcription = ev;
                    EnqueueParticipantEventLocked(
                        std::move(p_ev), receive_policy, receive_policy_revision);
                    tr_listeners_snapshot = listeners_;
                }
                for (const auto& listener : tr_listeners_snapshot) {
                    DeliverListener(
                        {generation, {}, true, false, receive_policy, {}, receive_policy_revision},
                        listener,
                        [&](RoomListener& target) {
                            target.OnTranscriptionReceived(ev, target_participant, target_pub);
                        },
                        true);
                }
            }
            return;
        }
    }

    // 1. 优先校验解包 LiveKit RPC 报文 (Topic 为 lk.rpc)
    if (real_topic == "lk.rpc") {

        std::string text_payload(real_payload.begin(), real_payload.end());
        auto rpc_pkt_opt = RpcPacket::Decode(text_payload);
        if (rpc_pkt_opt.has_value()) {
            if (decrypt_owner && rpc_pkt_opt->caller_identity != sender_identity) return;
            OnIncomingRpcPacket(rpc_pkt_opt.value(), generation, receive_policy, packet_sender, receive_policy_revision);
            return;
        }
    }

    auto listeners_snapshot = GetListenersSnapshot();
    std::shared_ptr<RemoteParticipant> remote_p;
    std::shared_ptr<LocalParticipant> local_p;
    std::shared_ptr<Participant> resolved_participant;
    SenderContext sender_context;
    {
        std::lock_guard lock(room_mutex_);
        if (!can_commit()) return;
        sender_context = ResolveSenderContextLocked(
            real_sender_sid, sender_identity, &resolved_participant);
        remote_p = std::dynamic_pointer_cast<RemoteParticipant>(resolved_participant);
        local_p = std::dynamic_pointer_cast<LocalParticipant>(resolved_participant);
    }

    // 2. 解析并派发结构化 Chat 消息
    bool chat_dispatched = false;
    if (is_structured_chat) {
        const auto& pb_chat = data_pkt.chat_message();
        ChatMessage chat;
        chat.id = pb_chat.id();
        chat.timestamp = pb_chat.timestamp();
        chat.edit_timestamp = pb_chat.edit_timestamp();
        chat.message = pb_chat.message();
        chat.sender_identity = !sender_identity.empty() ? sender_identity : (remote_p ? remote_p->identity() : real_sender_sid);
        std::shared_ptr<Participant> p = remote_p ? remote_p : (local_p && (local_p->sid() == real_sender_sid || local_p->identity() == sender_identity) ? std::static_pointer_cast<Participant>(local_p) : nullptr);
        for (const auto& listener : listeners_snapshot) {
            DeliverListener({generation, {}, true, false, receive_policy, {}, receive_policy_revision}, listener, [&](RoomListener& target) {
                target.OnChatMessage(chat, p);
            });
        }
        chat_dispatched = true;
    } else if (real_topic == "lk.chat" || real_topic == "lk-chat-topic" || real_topic.empty()) {
        std::string text_payload(real_payload.begin(), real_payload.end());
        auto chat_opt = ChatMessage::Decode(text_payload, !sender_identity.empty() ? sender_identity : real_sender_sid);
        if (chat_opt.has_value()) {
            std::shared_ptr<Participant> p = remote_p ? remote_p : (local_p && (local_p->sid() == real_sender_sid || local_p->identity() == sender_identity) ? std::static_pointer_cast<Participant>(local_p) : nullptr);
            for (const auto& listener : listeners_snapshot) {
                DeliverListener({generation, {}, true, false, receive_policy, {}, receive_policy_revision}, listener, [&](RoomListener& target) {
                    target.OnChatMessage(chat_opt.value(), p);
                });
            }
            chat_dispatched = true;
        }
    }

    // 3. 派发原始 OnDataReceived 事件给 RoomListener
    if (!chat_dispatched && real_topic != "lk.chat" && real_topic != "lk-chat-topic" && real_topic != "lk.rpc") {
        {
            std::lock_guard lock(room_mutex_);
            if (!can_commit()) return;
            ParticipantEvent event = resolved_participant
                ? MakeParticipantEventLocked(
                    ParticipantEventKind::DataReceived,
                    resolved_participant,
                    resolved_participant.get() == local_participant_.get())
                : ParticipantEvent{};
            event.kind = ParticipantEventKind::DataReceived;
            event.sender = sender_context;
            event.data = real_payload;
            event.topic = real_topic;
            EnqueueParticipantEventLocked(std::move(event), receive_policy, receive_policy_revision);
        }
        for (const auto& listener : listeners_snapshot) {
            DeliverListener({generation, {}, true, false, receive_policy, {}, receive_policy_revision}, listener, [&](RoomListener& target) {
                target.OnDataReceived(real_payload, remote_p, real_topic);
            }, true);
        }
    }
}


} // namespace livekit
