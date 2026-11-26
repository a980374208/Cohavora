#include "detail/room_internal.h"
#include "livekit_rtc.pb.h"
#include "livekit_models.pb.h"

#include <algorithm>

namespace livekit {

void Room::BindLocalDataTrackHandlersLocked(uint64_t generation) {
    if (!local_participant_) {
        return;
    }
    std::weak_ptr<Room> weak_self = weak_from_this();
    local_participant_->SetAsyncPublishDataTrackHandler(
        [weak_self, generation](DataTrackPublishOptions options)
            -> asio::awaitable<DataTrackResult<std::shared_ptr<LocalDataTrack>, PublishDataTrackError>> {
            auto self = weak_self.lock();
            if (!self) {
                return []() -> asio::awaitable<DataTrackResult<std::shared_ptr<LocalDataTrack>, PublishDataTrackError>> {
                    co_return DataTrackResult<std::shared_ptr<LocalDataTrack>, PublishDataTrackError>::Failure({
                        PublishDataTrackErrorCode::Disconnected,
                        "Room is no longer active",
                    });
                }();
            }
            return self->PublishLocalDataTrackAsync(std::move(options), generation);
        });
    local_participant_->SetPublishDataTrackSyncHandler(
        [weak_self, generation](const DataTrackPublishOptions& options)
            -> DataTrackResult<std::shared_ptr<LocalDataTrack>, PublishDataTrackError> {
            auto self = weak_self.lock();
            if (!self) {
                return DataTrackResult<std::shared_ptr<LocalDataTrack>, PublishDataTrackError>::Failure({
                    PublishDataTrackErrorCode::Disconnected,
                    "Room is no longer active",
                });
            }
            return self->PublishLocalDataTrackSync(options, generation);
        });
}

std::optional<uint16_t> Room::AllocateDataTrackPubHandleLocked() {
    for (uint32_t attempts = 0; attempts < 65535; ++attempts) {
        if (next_data_track_pub_handle_ == 0) {
            next_data_track_pub_handle_ = 1;
        }
        const uint16_t candidate = next_data_track_pub_handle_++;
        if (local_data_tracks_.find(candidate) == local_data_tracks_.end() &&
            pending_data_track_publishes_.find(candidate) == pending_data_track_publishes_.end()) {
            return candidate;
        }
    }
    return std::nullopt;
}

void Room::SendDataTrackSignal(const proto::SignalRequest& req, uint64_t generation) {
    std::shared_ptr<SignalClient> signal;
    std::function<void(const proto::SignalRequest&)> signal_hook;
    {
        std::lock_guard lock(room_mutex_);
        if (!IsSignalGenerationCurrentLocked(generation)) {
            return;
        }
        signal = signal_client_;
        if (data_track_instrumentation_) {
            signal_hook = data_track_instrumentation_->on_signal_send;
        }
    }
    if (signal) {
        signal->Send(req);
    }
    if (signal_hook) {
        signal_hook(req);
    }
}

void Room::OnDataTrackChannelStateChanged(
    webrtc::DataChannelInterface::DataState state,
    uint64_t generation,
    const webrtc::DataChannelInterface* channel) {
    if (!channel) return;
    BeforeNativeEventCommit(generation);

    std::vector<std::shared_ptr<AwaitableState<void>>> success;
    std::vector<std::shared_ptr<AwaitableState<void>>> failure;
    {
        std::lock_guard lock(room_mutex_);
        if (!IsNativeGenerationCurrentLocked(generation) ||
            data_track_dc_.get() != channel) {
            return;
        }
        for (const auto& waiter : pending_data_track_dc_waits_) {
            if (waiter.generation != generation) continue;
            if (state == webrtc::DataChannelInterface::kOpen) {
                success.push_back(waiter.completion);
            } else if (state == webrtc::DataChannelInterface::kClosing ||
                       state == webrtc::DataChannelInterface::kClosed) {
                failure.push_back(waiter.completion);
            }
        }
        if (state == webrtc::DataChannelInterface::kOpen) {
            ScheduleDataTrackDrainLocked(generation);
        }
    }
    for (const auto& completion : success) {
        CompleteAwaitable(completion);
    }
    for (const auto& completion : failure) {
        FailAwaitable(completion, std::make_exception_ptr(OperationError(
            OperationKind::PublishTrack,
            OperationErrorCode::DataChannelUnavailable,
            "data_track_channel_state",
            "_data_track channel closed before becoming ready",
            true)));
    }
}

void Room::OnDataTrackChannelBufferedAmountLow(uint64_t generation) {
    BeforeNativeEventCommit(generation);
    std::lock_guard lock(room_mutex_);
    if (!IsNativeGenerationCurrentLocked(generation)) return;
    ScheduleDataTrackDrainLocked(generation);
}

asio::awaitable<void> Room::WaitForDataTrackChannelOpen(
    std::chrono::milliseconds timeout,
    uint64_t generation) {
    webrtc::scoped_refptr<webrtc::DataChannelInterface> channel;
    auto completion = std::make_shared<AwaitableState<void>>(executor_);
    {
        std::lock_guard lock(room_mutex_);
        if (!IsNativeGenerationCurrentLocked(generation)) {
            throw OperationError(OperationKind::PublishTrack,
                                 OperationErrorCode::Cancelled,
                                 "wait_data_track_channel",
                                 "session replaced while waiting for _data_track channel");
        }
        channel = data_track_dc_;
        if (!channel) {
            throw OperationError(OperationKind::PublishTrack,
                                 OperationErrorCode::DataChannelUnavailable,
                                 "wait_data_track_channel",
                                 "_data_track channel is missing",
                                 true);
        }
        pending_data_track_dc_waits_.push_back({generation, completion});
    }

    OnDataTrackChannelStateChanged(channel->state(), generation, channel.get());

    try {
        co_await WaitAwaitable<void>(completion, timeout,
                                     OperationKind::PublishTrack,
                                     OperationErrorCode::DataChannelUnavailable,
                                     "wait_data_track_channel");
    } catch (...) {
        std::lock_guard lock(room_mutex_);
        pending_data_track_dc_waits_.erase(
            std::remove_if(pending_data_track_dc_waits_.begin(),
                           pending_data_track_dc_waits_.end(),
                           [&](const PendingDataChannelWait& item) {
                               return item.completion == completion;
                           }),
            pending_data_track_dc_waits_.end());
        throw;
    }

    std::lock_guard lock(room_mutex_);
    pending_data_track_dc_waits_.erase(
        std::remove_if(pending_data_track_dc_waits_.begin(),
                       pending_data_track_dc_waits_.end(),
                       [&](const PendingDataChannelWait& item) {
                           return item.completion == completion;
                       }),
        pending_data_track_dc_waits_.end());
}

std::pair<LocalDataTrack::TryPushHandler, LocalDataTrack::UnpublishHandler>
Room::MakeLocalDataTrackHandlers(uint16_t pub_handle, uint64_t generation) {
    std::weak_ptr<Room> weak_self = weak_from_this();
    LocalDataTrack::TryPushHandler push_handler =
        [weak_self, pub_handle, generation](const DataTrackFrame& frame)
            -> DataTrackResult<void, LocalDataTrackTryPushError> {
            auto self = weak_self.lock();
            if (!self) {
                return DataTrackResult<void, LocalDataTrackTryPushError>::Failure({
                    LocalDataTrackTryPushErrorCode::TrackUnpublished,
                    "Room has been destroyed",
                });
            }
            return self->EnqueueLocalDataTrackFrame(pub_handle, frame, generation);
        };
    LocalDataTrack::UnpublishHandler unpub_handler =
        [weak_self, pub_handle, generation]() {
            if (auto self = weak_self.lock()) {
                self->UnpublishLocalDataTrack(pub_handle, generation);
            }
        };
    return {std::move(push_handler), std::move(unpub_handler)};
}

std::shared_ptr<LocalDataTrack> Room::BuildLocalDataTrackObjectLocked(
    const std::shared_ptr<LocalDataTrackEntry>& entry,
    uint64_t generation) {
    auto handlers = MakeLocalDataTrackHandlers(entry->pub_handle, generation);
    return std::make_shared<LocalDataTrack>(
        entry->info,
        std::move(handlers.first),
        std::move(handlers.second));
}

DataTrackResult<std::shared_ptr<LocalDataTrack>, PublishDataTrackError>
Room::PublishLocalDataTrackSync(const DataTrackPublishOptions& options, uint64_t generation) {
    using Res = DataTrackResult<std::shared_ptr<LocalDataTrack>, PublishDataTrackError>;
    if (retired()) {
        return Res::Failure({PublishDataTrackErrorCode::Disconnected, "Room is retired"});
    }
    auto promise = std::make_shared<std::promise<Res>>();
    auto future = promise->get_future();
    std::chrono::milliseconds wait_budget = std::chrono::seconds(11);
    {
        std::lock_guard lock(room_mutex_);
        wait_budget = operation_timeouts_.publish + std::chrono::seconds(1);
    }

    auto self = shared_from_this();
    livekit::safe_co_spawn(executor_, [self, options, generation, promise]() -> asio::awaitable<void> {
        try {
            auto result = co_await self->PublishLocalDataTrackAsync(options, generation);
            promise->set_value(std::move(result));
        } catch (const std::exception& e) {
            try {
                promise->set_value(Res::Failure({PublishDataTrackErrorCode::Internal, e.what()}));
            } catch (...) {}
        } catch (...) {
            try {
                promise->set_value(Res::Failure({PublishDataTrackErrorCode::Internal, "unknown error"}));
            } catch (...) {}
        }
    });

    if (future.wait_for(wait_budget) != std::future_status::ready) {
        return Res::Failure({PublishDataTrackErrorCode::Timeout, "PublishDataTrack timed out"});
    }
    return future.get();
}

asio::awaitable<DataTrackResult<std::shared_ptr<LocalDataTrack>, PublishDataTrackError>>
Room::PublishLocalDataTrackAsync(DataTrackPublishOptions options, uint64_t generation) {
    using Res = DataTrackResult<std::shared_ptr<LocalDataTrack>, PublishDataTrackError>;
    auto admission = callback_gate_->Enter();
    if (!admission) {
        co_return Res::Failure({PublishDataTrackErrorCode::Disconnected, "Room is retired"});
    }
    if (options.name.empty()) {
        co_return Res::Failure({PublishDataTrackErrorCode::InvalidName, "Data track name must not be empty"});
    }

    bool need_negotiate = false;
    bool need_dc_wait = false;
    std::chrono::milliseconds pc_timeout = std::chrono::seconds(10);
    std::chrono::milliseconds neg_timeout = std::chrono::seconds(10);
    std::chrono::milliseconds pub_timeout = std::chrono::seconds(10);
    bool uses_e2ee = false;
    uint16_t pub_handle = 0;

    {
        std::lock_guard lock(room_mutex_);
        if (!IsNativeGenerationCurrentLocked(generation) ||
            connection_state_ != ConnectionState::Connected) {
            co_return Res::Failure({
                PublishDataTrackErrorCode::Disconnected,
                "Room is not connected for this session generation",
            });
        }
        if (!local_participant_ || !local_participant_->permission().can_publish_data) {
            co_return Res::Failure({
                PublishDataTrackErrorCode::NotAllowed,
                "Participant does not have permission to publish data",
            });
        }
        pc_timeout = operation_timeouts_.peer_connection;
        neg_timeout = operation_timeouts_.negotiation;
        pub_timeout = operation_timeouts_.publish;
        const bool bypass_dc = data_track_instrumentation_ &&
            data_track_instrumentation_->bypass_webrtc_data_channel_wait;
        if (!bypass_dc && require_media_connection_) {
            if (!publisher_pc_ || !data_track_dc_) {
                co_return Res::Failure({
                    PublishDataTrackErrorCode::Disconnected,
                    "DataTrack transport is not available",
                });
            }
            if (data_track_dc_->state() != webrtc::DataChannelInterface::kOpen) {
                need_dc_wait = true;
                need_negotiate = !signal_client_ || !signal_client_->is_single_pc_mode_active();
            }
        }
    }

    if (need_dc_wait) {
        if (need_negotiate) {
            try {
                co_await NegotiatePublisherAsync(neg_timeout, generation);
            } catch (const OperationError& err) {
                co_return Res::Failure({
                    err.code() == OperationErrorCode::Cancelled
                        ? PublishDataTrackErrorCode::Disconnected
                        : PublishDataTrackErrorCode::Timeout,
                    err.what(),
                });
            } catch (const std::exception& err) {
                co_return Res::Failure({PublishDataTrackErrorCode::Disconnected, err.what()});
            }
        }
        try {
            co_await WaitForDataTrackChannelOpen(pc_timeout, generation);
        } catch (const OperationError& err) {
            co_return Res::Failure({
                err.code() == OperationErrorCode::Cancelled
                    ? PublishDataTrackErrorCode::Disconnected
                    : PublishDataTrackErrorCode::Timeout,
                err.what(),
            });
        } catch (const std::exception& err) {
            co_return Res::Failure({PublishDataTrackErrorCode::Disconnected, err.what()});
        }
    }

    auto state = std::make_shared<AwaitableState<Res>>(executor_);
    {
        std::lock_guard lock(room_mutex_);
        if (!IsNativeGenerationCurrentLocked(generation) ||
            connection_state_ != ConnectionState::Connected) {
            co_return Res::Failure({
                PublishDataTrackErrorCode::Disconnected,
                "Session closed before PublishDataTrack request",
            });
        }
        if (e2ee_manager_ && e2ee_manager_->enabled()) {
            if (e2ee_manager_->encryption_type() != EncryptionType::GCM) {
                co_return Res::Failure({
                    PublishDataTrackErrorCode::Internal,
                    "Unsupported encryption type for DataTrack",
                });
            }
            uses_e2ee = true;
        }
        const auto allocated = AllocateDataTrackPubHandleLocked();
        if (!allocated.has_value()) {
            co_return Res::Failure({
                PublishDataTrackErrorCode::LimitReached,
                "Maximum number of published data track handles exhausted",
            });
        }
        pub_handle = *allocated;
        pending_data_track_publishes_.emplace(pub_handle, state);
    }

    proto::SignalRequest req;
    auto* pub_req = req.mutable_publish_data_track_request();
    pub_req->set_pub_handle(pub_handle);
    pub_req->set_name(options.name);
    pub_req->set_encryption(uses_e2ee ? proto::Encryption::GCM : proto::Encryption::NONE);
    SendDataTrackSignal(req, generation);

    try {
        auto result = co_await WaitAwaitable<Res>(
            state,
            pub_timeout,
            OperationKind::PublishTrack,
            OperationErrorCode::TrackPublishTimeout,
            "publish_data_track");
        co_return result;
    } catch (const OperationError& err) {
        bool was_pending = false;
        bool still_connected = false;
        {
            std::lock_guard lock(room_mutex_);
            was_pending = pending_data_track_publishes_.erase(pub_handle) > 0;
            still_connected = IsSignalGenerationCurrentLocked(generation) &&
                connection_state_ == ConnectionState::Connected;
        }
        if (was_pending && still_connected) {
            // Cancel orphaned publish if SFU response arrives later.
            proto::SignalRequest unpub_req;
            unpub_req.mutable_unpublish_data_track_request()->set_pub_handle(pub_handle);
            SendDataTrackSignal(unpub_req, generation);
        }
        const auto code = err.code() == OperationErrorCode::Cancelled ||
                err.code() == OperationErrorCode::SessionClosed
            ? PublishDataTrackErrorCode::Disconnected
            : PublishDataTrackErrorCode::Timeout;
        co_return Res::Failure({code, err.what()});
    } catch (const std::exception& err) {
        std::lock_guard lock(room_mutex_);
        pending_data_track_publishes_.erase(pub_handle);
        co_return Res::Failure({PublishDataTrackErrorCode::Internal, err.what()});
    }
}

DataTrackResult<void, LocalDataTrackTryPushError> Room::EnqueueLocalDataTrackFrame(
    uint16_t pub_handle,
    const DataTrackFrame& frame,
    uint64_t expected_generation) {
    using Res = DataTrackResult<void, LocalDataTrackTryPushError>;
    if (frame.payload.empty()) {
        std::lock_guard lock(room_mutex_);
        if (!IsNativeGenerationCurrentLocked(expected_generation) ||
            connection_state_ != ConnectionState::Connected) {
            return Res::Failure({LocalDataTrackTryPushErrorCode::TrackUnpublished,
                                 "Room session is no longer active"});
        }
        const auto it = local_data_tracks_.find(pub_handle);
        if (it == local_data_tracks_.end() || !it->second || it->second->unpublished) {
            return Res::Failure({LocalDataTrackTryPushErrorCode::TrackUnpublished,
                                 "LocalDataTrack is unpublished"});
        }
        if (it->second->republishing) {
            return Res::Failure({LocalDataTrackTryPushErrorCode::QueueFull,
                                 "LocalDataTrack is republishing during reconnect"});
        }
        return Res::Success();
    }

    std::shared_ptr<LocalDataTrackEntry> entry;
    bool uses_e2ee = false;
    std::shared_ptr<E2eeManager> enc_owner;
    E2eeManager::DataPacketState enc_state{};
    std::string sender_identity;

    {
        std::lock_guard lock(room_mutex_);
        if (!IsNativeGenerationCurrentLocked(expected_generation) ||
            connection_state_ != ConnectionState::Connected) {
            return Res::Failure({LocalDataTrackTryPushErrorCode::TrackUnpublished,
                                 "Room session is no longer active"});
        }
        const auto it = local_data_tracks_.find(pub_handle);
        if (it == local_data_tracks_.end() || !it->second || it->second->unpublished) {
            return Res::Failure({LocalDataTrackTryPushErrorCode::TrackUnpublished,
                                 "LocalDataTrack is unpublished"});
        }
        entry = it->second;
        if (entry->republishing) {
            return Res::Failure({LocalDataTrackTryPushErrorCode::QueueFull,
                                 "LocalDataTrack is republishing during reconnect"});
        }
        uses_e2ee = entry->info.uses_e2ee;
        if (uses_e2ee) {
            enc_owner = e2ee_manager_;
            if (enc_owner) {
                enc_state = enc_owner->data_packet_state();
            }
            if (!local_participant_) {
                return Res::Failure({LocalDataTrackTryPushErrorCode::EncryptionFailed,
                                     "Missing local participant identity for E2EE"});
            }
            sender_identity = local_participant_->identity();
            if (!enc_owner || !enc_state.enabled ||
                enc_owner->encryption_type() != EncryptionType::GCM ||
                !enc_owner->data_packet_cryptor() || sender_identity.empty()) {
                return Res::Failure({LocalDataTrackTryPushErrorCode::EncryptionFailed,
                                     "E2EE key/cryptor unavailable for encrypted DataTrack"});
            }
        }
    }

    data_track::DtpExtensions extensions;
    extensions.user_timestamp = frame.user_timestamp;
    if (uses_e2ee) extensions.e2ee.emplace();
    const size_t max_wire_payload = data_track::DtpPacketizer::MaxFramePayloadBytes(extensions);
    const auto too_large = [] {
        return Res::Failure({LocalDataTrackTryPushErrorCode::PayloadTooLarge,
                             "DataTrack frame exceeds the DTP fragment limit"});
    };
    if (frame.payload.size() > max_wire_payload) return too_large();

    const auto* payload_bytes = &frame.payload;
    std::vector<uint8_t> ciphertext;
    if (uses_e2ee) {
        auto& ext = *extensions.e2ee;
        auto encrypted = enc_owner->data_packet_cryptor()->EncryptPacket(
            sender_identity,
            enc_state.key_index,
            frame.payload,
            max_wire_payload + ext.iv.size());
        const auto* error = std::get_if<PacketCryptoError>(&encrypted);
        if (error && *error == PacketCryptoError::SizeLimitExceeded) return too_large();
        auto* sealed = std::get_if<EncryptedDataPacket>(&encrypted);
        if (!sealed || sealed->iv.size() != ext.iv.size() || sealed->key_index > 0xFFu) {
            return Res::Failure({LocalDataTrackTryPushErrorCode::EncryptionFailed,
                                 "Failed to encrypt DataTrack frame payload"});
        }
        ext.key_index = static_cast<uint8_t>(sealed->key_index);
        std::copy_n(sealed->iv.begin(), ext.iv.size(), ext.iv.begin());
        ciphertext = std::move(sealed->ciphertext);
        payload_bytes = &ciphertext;
    }

    std::vector<data_track::DtpPacket> packets;
    {
        std::lock_guard pipe_lock(entry->pipeline_mutex);
        if (!entry->packetizer) {
            entry->packetizer = std::make_unique<data_track::DtpPacketizer>(pub_handle);
        }
        std::string pkt_err;
        if (!entry->packetizer->Packetize(*payload_bytes, extensions, packets, &pkt_err)) {
            return Res::Failure({LocalDataTrackTryPushErrorCode::Internal,
                                 pkt_err.empty() ? "Packetize failed" : pkt_err});
        }
    }

    DataTrackSendQueue::FramePackets serialized_batch;
    serialized_batch.reserve(packets.size());
    for (const auto& pkt : packets) {
        std::vector<uint8_t> encoded;
        if (!pkt.SerializeInto(encoded)) {
            return Res::Failure({LocalDataTrackTryPushErrorCode::Internal,
                                 "Failed to serialize DTP packet"});
        }
        serialized_batch.push_back(std::move(encoded));
    }
    if (serialized_batch.empty()) {
        return Res::Success();
    }

    {
        std::lock_guard lock(room_mutex_);
        if (!IsNativeGenerationCurrentLocked(expected_generation) ||
            connection_state_ != ConnectionState::Connected ||
            entry->unpublished || entry->republishing) {
            return Res::Failure({LocalDataTrackTryPushErrorCode::TrackUnpublished,
                                 "LocalDataTrack session state changed during push"});
        }
        if (uses_e2ee &&
            (e2ee_manager_ != enc_owner ||
             !enc_owner ||
             enc_owner->data_packet_state().policy_revision != enc_state.policy_revision)) {
            return Res::Failure({LocalDataTrackTryPushErrorCode::EncryptionFailed,
                                 "E2EE policy changed before admission"});
        }
        entry->send_queue.Push(std::move(serialized_batch));
        ScheduleDataTrackDrainLocked(expected_generation);
    }
    return Res::Success();
}

void Room::UnpublishLocalDataTrack(uint16_t pub_handle, uint64_t expected_generation) {
    bool send_unpublish = false;
    {
        std::lock_guard lock(room_mutex_);
        const auto it = local_data_tracks_.find(pub_handle);
        if (it != local_data_tracks_.end() && it->second) {
            it->second->unpublished = true;
            it->second->send_queue.Clear();
            it->second->in_flight_packets.clear();
            local_data_tracks_.erase(it);
        }
        // Keep handle tracked as unpublished across reconnect records so it isn't resurrected.
        reconnect_data_track_records_.erase(
            std::remove_if(reconnect_data_track_records_.begin(),
                           reconnect_data_track_records_.end(),
                           [&](const ReconnectLocalDataTrackRecord& rec) {
                               return rec.pub_handle == pub_handle;
                           }),
            reconnect_data_track_records_.end());
        send_unpublish = IsSignalGenerationCurrentLocked(expected_generation) &&
            (connection_state_ == ConnectionState::Connected ||
             connection_state_ == ConnectionState::Reconnecting);
    }
    if (send_unpublish) {
        proto::SignalRequest req;
        req.mutable_unpublish_data_track_request()->set_pub_handle(pub_handle);
        SendDataTrackSignal(req, expected_generation);
    }
}

void Room::ScheduleDataTrackDrainLocked(uint64_t generation) {
    if (data_track_drain_scheduled_) {
        return;
    }
    data_track_drain_scheduled_ = true;
    std::weak_ptr<Room> weak_self = weak_from_this();
    asio::post(executor_, [weak_self, generation]() {
        if (auto self = weak_self.lock()) {
            self->DrainDataTrackSendQueue(generation);
        }
    });
}

void Room::DrainDataTrackSendQueue(uint64_t generation) {
    auto admission = callback_gate_->Enter();
    if (!admission) return;

    while (true) {
        webrtc::scoped_refptr<webrtc::DataChannelInterface> dc;
        std::function<void(const std::vector<uint8_t>&)> wire_hook;
        {
            std::lock_guard lock(room_mutex_);
            data_track_drain_scheduled_ = false;
            if (!IsNativeGenerationCurrentLocked(generation) ||
                connection_state_ != ConnectionState::Connected) {
                return;
            }
            dc = data_track_dc_;
            if (data_track_instrumentation_) {
                wire_hook = data_track_instrumentation_->on_wire_packet_send;
            }
        }

        if (dc && dc->state() != webrtc::DataChannelInterface::kOpen && !wire_hook) {
            return;
        }
        if (!dc && !wire_hook) {
            return;
        }
        if (dc && !wire_hook && dc->buffered_amount() > kDataTrackLowBufferThresholdBytes) {
            return;
        }

        std::vector<uint8_t> packet_to_send;
        bool found_packet = false;
        {
            std::lock_guard lock(room_mutex_);
            if (!IsNativeGenerationCurrentLocked(generation) ||
                connection_state_ != ConnectionState::Connected) {
                return;
            }
            for (auto& [handle, entry] : local_data_tracks_) {
                (void)handle;
                if (!entry || entry->unpublished || entry->republishing) {
                    continue;
                }
                if (entry->in_flight_packets.empty()) {
                    if (auto batch = entry->send_queue.TryPop()) {
                        for (auto& p : *batch) {
                            entry->in_flight_packets.push_back(std::move(p));
                        }
                    }
                }
                if (!entry->in_flight_packets.empty()) {
                    packet_to_send = std::move(entry->in_flight_packets.front());
                    entry->in_flight_packets.pop_front();
                    found_packet = true;
                    break;
                }
            }
            if (!found_packet) {
                return;
            }
        }

        if (wire_hook) {
            wire_hook(packet_to_send);
        }
        if (dc && dc->state() == webrtc::DataChannelInterface::kOpen) {
            webrtc::DataBuffer buffer(
                webrtc::CopyOnWriteBuffer(packet_to_send.data(), packet_to_send.size()),
                /*binary=*/true);
            if (!dc->Send(buffer)) {
                Log("DATA_TRACK", "SEND_FAILED", "_data_track channel rejected outgoing DTP packet");
                return;
            }
        }
    }
}

std::shared_ptr<RemoteDataTrack> Room::BuildRemoteDataTrackObjectLocked(
    const std::shared_ptr<RemoteDataTrackEntry>& entry,
    uint64_t generation) {
    const std::string track_sid = entry->info.sid;
    std::weak_ptr<Room> weak_self = weak_from_this();
    std::weak_ptr<RemoteDataTrackEntry> weak_entry = entry;
    return std::make_shared<RemoteDataTrack>(
        entry->info,
        entry->publisher_identity,
        [weak_self, track_sid, generation](DataTrackSubscribeOptions opts)
            -> asio::awaitable<DataTrackResult<std::shared_ptr<DataTrackStream>, SubscribeDataTrackError>> {
            auto self = weak_self.lock();
            if (!self) {
                return []() -> asio::awaitable<DataTrackResult<std::shared_ptr<DataTrackStream>, SubscribeDataTrackError>> {
                    co_return DataTrackResult<std::shared_ptr<DataTrackStream>, SubscribeDataTrackError>::Failure({
                        SubscribeDataTrackErrorCode::Disconnected,
                        "Room has been destroyed",
                    });
                }();
            }
            return self->SubscribeRemoteDataTrackAsync(track_sid, std::move(opts), generation);
        },
        [weak_self, track_sid, generation](const DataTrackSubscribeOptions& opts)
            -> DataTrackResult<std::shared_ptr<DataTrackStream>, SubscribeDataTrackError> {
            auto self = weak_self.lock();
            if (!self) {
                return DataTrackResult<std::shared_ptr<DataTrackStream>, SubscribeDataTrackError>::Failure({
                    SubscribeDataTrackErrorCode::Disconnected,
                    "Room has been destroyed",
                });
            }
            return self->SubscribeRemoteDataTrackSync(track_sid, opts, generation);
        },
        [weak_entry](const DataTrackPipelineOptions& opts) {
            if (auto locked = weak_entry.lock()) {
                if (opts.max_partial_frames.has_value()) {
                    const size_t clamped = std::max<size_t>(1, *opts.max_partial_frames);
                    locked->max_partial_frames.store(clamped, std::memory_order_release);
                }
            }
        });
}

DataTrackResult<std::shared_ptr<DataTrackStream>, SubscribeDataTrackError>
Room::SubscribeRemoteDataTrackSync(const std::string& track_sid,
                                   const DataTrackSubscribeOptions& options,
                                   uint64_t generation) {
    using Res = DataTrackResult<std::shared_ptr<DataTrackStream>, SubscribeDataTrackError>;
    if (retired()) {
        return Res::Failure({SubscribeDataTrackErrorCode::Disconnected, "Room is retired"});
    }
    auto promise = std::make_shared<std::promise<Res>>();
    auto future = promise->get_future();
    std::chrono::milliseconds wait_budget = std::chrono::seconds(11);
    {
        std::lock_guard lock(room_mutex_);
        wait_budget = operation_timeouts_.publish + std::chrono::seconds(1);
    }

    auto self = shared_from_this();
    livekit::safe_co_spawn(executor_, [self, track_sid, options, generation, promise]() -> asio::awaitable<void> {
        try {
            auto result = co_await self->SubscribeRemoteDataTrackAsync(track_sid, options, generation);
            promise->set_value(std::move(result));
        } catch (const std::exception& e) {
            try {
                promise->set_value(Res::Failure({SubscribeDataTrackErrorCode::Internal, e.what()}));
            } catch (...) {}
        } catch (...) {
            try {
                promise->set_value(Res::Failure({SubscribeDataTrackErrorCode::Internal, "unknown error"}));
            } catch (...) {}
        }
    });

    if (future.wait_for(wait_budget) != std::future_status::ready) {
        return Res::Failure({SubscribeDataTrackErrorCode::Timeout, "RemoteDataTrack::Subscribe timed out"});
    }
    return future.get();
}

asio::awaitable<DataTrackResult<std::shared_ptr<DataTrackStream>, SubscribeDataTrackError>>
Room::SubscribeRemoteDataTrackAsync(std::string track_sid,
                                    DataTrackSubscribeOptions options,
                                    uint64_t generation) {
    using Res = DataTrackResult<std::shared_ptr<DataTrackStream>, SubscribeDataTrackError>;
    auto admission = callback_gate_->Enter();
    if (!admission) {
        co_return Res::Failure({SubscribeDataTrackErrorCode::Disconnected, "Room is retired"});
    }

    std::shared_ptr<AwaitableState<Res>> wait_state;
    std::shared_ptr<DataTrackStream> ready_stream;
    bool send_sub_request = false;
    std::chrono::milliseconds sub_timeout = std::chrono::seconds(10);

    {
        std::lock_guard lock(room_mutex_);
        if (!IsNativeGenerationCurrentLocked(generation) ||
            connection_state_ != ConnectionState::Connected) {
            co_return Res::Failure({
                SubscribeDataTrackErrorCode::Disconnected,
                "Room session is not connected",
            });
        }
        const auto it = remote_data_tracks_.find(track_sid);
        if (it == remote_data_tracks_.end() || !it->second || !it->second->published) {
            co_return Res::Failure({
                SubscribeDataTrackErrorCode::Unpublished,
                "RemoteDataTrack is not published",
            });
        }
        auto& entry = it->second;
        if (entry->info.uses_e2ee) {
            if (!e2ee_manager_ || !e2ee_manager_->enabled() ||
                e2ee_manager_->encryption_type() != EncryptionType::GCM ||
                !e2ee_manager_->data_packet_cryptor()) {
                co_return Res::Failure({
                    SubscribeDataTrackErrorCode::Internal,
                    "Encrypted remote DataTrack cannot be subscribed without active E2EE key",
                });
            }
        } else if (e2ee_manager_ && e2ee_manager_->enabled() &&
                   e2ee_manager_->encryption_type() != EncryptionType::NONE) {
            co_return Res::Failure({
                SubscribeDataTrackErrorCode::Internal,
                "Unencrypted remote DataTrack rejected under active E2EE room policy",
            });
        }

        sub_timeout = operation_timeouts_.publish;
        const size_t buf_size = options.buffer_size.has_value()
            ? std::max<size_t>(1, *options.buffer_size)
            : DataTrackStream::kDefaultBufferSize;

        if (entry->sub_state == RemoteDataTrackEntry::SubState::Active) {
            std::weak_ptr<Room> weak_self = weak_from_this();
            ready_stream = std::make_shared<DataTrackStream>(buf_size, [weak_self, track_sid, generation]() {
                if (auto self = weak_self.lock()) {
                    self->OnDataTrackStreamClosed(track_sid, generation);
                }
            });
            entry->active_streams.push_back(ready_stream);
        } else {
            wait_state = std::make_shared<AwaitableState<Res>>(executor_);
            entry->stream_buffer_size = buf_size;
            if (options.target_fps.has_value()) {
                entry->target_fps = options.target_fps;
            }
            entry->pending_subscribes.push_back(wait_state);
            if (entry->sub_state == RemoteDataTrackEntry::SubState::None) {
                entry->sub_state = RemoteDataTrackEntry::SubState::Pending;
                send_sub_request = true;
            }
        }
    }

    if (ready_stream) {
        co_return Res::Success(std::move(ready_stream));
    }

    if (send_sub_request) {
        proto::SignalRequest req;
        auto* sub = req.mutable_update_data_subscription();
        auto* upd = sub->add_updates();
        upd->set_track_sid(track_sid);
        upd->set_subscribe(true);
        if (options.target_fps.has_value()) {
            upd->mutable_options()->set_target_fps(*options.target_fps);
        }
        SendDataTrackSignal(req, generation);
    }

    try {
        auto res = co_await WaitAwaitable<Res>(
            wait_state,
            sub_timeout,
            OperationKind::SendData,
            OperationErrorCode::TrackPublishTimeout,
            "subscribe_data_track");
        co_return res;
    } catch (const OperationError& err) {
        bool send_unsub = false;
        {
            std::lock_guard lock(room_mutex_);
            if (const auto it = remote_data_tracks_.find(track_sid);
                it != remote_data_tracks_.end() && it->second) {
                auto& pending = it->second->pending_subscribes;
                pending.erase(std::remove(pending.begin(), pending.end(), wait_state), pending.end());
                it->second->active_streams.erase(
                    std::remove_if(it->second->active_streams.begin(),
                                   it->second->active_streams.end(),
                                   [](const std::weak_ptr<DataTrackStream>& w) { return w.expired(); }),
                    it->second->active_streams.end());
                if (pending.empty() && it->second->active_streams.empty() &&
                    it->second->sub_state == RemoteDataTrackEntry::SubState::Pending) {
                    it->second->sub_state = RemoteDataTrackEntry::SubState::None;
                    send_unsub = IsSignalGenerationCurrentLocked(generation) &&
                        connection_state_ == ConnectionState::Connected;
                }
            }
        }
        if (send_unsub) {
            proto::SignalRequest unsub_req;
            auto* upd = unsub_req.mutable_update_data_subscription()->add_updates();
            upd->set_track_sid(track_sid);
            upd->set_subscribe(false);
            SendDataTrackSignal(unsub_req, generation);
        }
        const auto code = err.code() == OperationErrorCode::Cancelled ||
                err.code() == OperationErrorCode::SessionClosed
            ? SubscribeDataTrackErrorCode::Disconnected
            : SubscribeDataTrackErrorCode::Timeout;
        co_return Res::Failure({code, err.what()});
    } catch (const std::exception& err) {
        co_return Res::Failure({SubscribeDataTrackErrorCode::Internal, err.what()});
    }
}

void Room::OnDataTrackStreamClosed(const std::string& track_sid, uint64_t generation) {
    bool send_unsub = false;
    {
        std::lock_guard lock(room_mutex_);
        // If session is currently in full_restart preserve records, detach stream cleanly
        // when the user drops it rather than resurrecting it later.
        for (auto it = reconnect_data_sub_records_.begin(); it != reconnect_data_sub_records_.end();) {
            it->streams.erase(
                std::remove_if(it->streams.begin(), it->streams.end(),
                               [](const std::weak_ptr<DataTrackStream>& w) {
                                   auto locked = w.lock();
                                   return !locked || locked->IsClosed();
                               }),
                it->streams.end());
            if (it->streams.empty()) {
                it = reconnect_data_sub_records_.erase(it);
            } else {
                ++it;
            }
        }

        const auto dt_it = remote_data_tracks_.find(track_sid);
        if (dt_it != remote_data_tracks_.end() && dt_it->second) {
            auto& entry = dt_it->second;
            entry->active_streams.erase(
                std::remove_if(entry->active_streams.begin(), entry->active_streams.end(),
                               [](const std::weak_ptr<DataTrackStream>& w) {
                                   auto locked = w.lock();
                                   return !locked || locked->IsClosed();
                               }),
                entry->active_streams.end());
            if (entry->active_streams.empty() && entry->pending_subscribes.empty() &&
                entry->sub_state != RemoteDataTrackEntry::SubState::None) {
                if (entry->assigned_sub_handle != 0) {
                    data_track_sub_handles_.erase(entry->assigned_sub_handle);
                    entry->assigned_sub_handle = 0;
                }
                entry->sub_state = RemoteDataTrackEntry::SubState::None;
                {
                    std::lock_guard pipe_lock(entry->depacketizer_mutex);
                    entry->depacketizer.Reset();
                }
                send_unsub = IsSignalGenerationCurrentLocked(generation) &&
                    connection_state_ == ConnectionState::Connected;
            }
        }
    }
    if (send_unsub) {
        proto::SignalRequest req;
        auto* upd = req.mutable_update_data_subscription()->add_updates();
        upd->set_track_sid(track_sid);
        upd->set_subscribe(false);
        SendDataTrackSignal(req, generation);
    }
}

void Room::HandlePublishDataTrackResponse(const proto::PublishDataTrackResponse& resp,
                                          uint64_t event_generation) {
    if (!resp.has_info()) {
        return;
    }
    const auto& pb_info = resp.info();
    if (pb_info.pub_handle() == 0 || pb_info.pub_handle() > 0xFFFFu) {
        return;
    }
    const uint16_t pub_handle = static_cast<uint16_t>(pb_info.pub_handle());
    if (pb_info.encryption() != proto::Encryption::NONE &&
        pb_info.encryption() != proto::Encryption::GCM) {
        std::shared_ptr<AwaitableState<DataTrackResult<std::shared_ptr<LocalDataTrack>, PublishDataTrackError>>>
            pending;
        {
            std::lock_guard lock(room_mutex_);
            if (!IsSignalGenerationCurrentLocked(event_generation)) return;
            const auto p_it = pending_data_track_publishes_.find(pub_handle);
            if (p_it != pending_data_track_publishes_.end()) {
                pending = std::move(p_it->second);
                pending_data_track_publishes_.erase(p_it);
            }
        }
        if (pending) {
            CompleteAwaitable(pending,
                DataTrackResult<std::shared_ptr<LocalDataTrack>, PublishDataTrackError>::Failure({
                    PublishDataTrackErrorCode::ProtocolError,
                    "Unsupported encryption type in PublishDataTrackResponse",
                }));
        }
        return;
    }

    std::shared_ptr<AwaitableState<DataTrackResult<std::shared_ptr<LocalDataTrack>, PublishDataTrackError>>>
        pending;
    std::shared_ptr<LocalDataTrack> created_track;
    std::shared_ptr<LocalDataTrack> repub_track;
    bool orphan_unpublish = false;
    uint64_t current_gen = 0;

    {
        std::lock_guard lock(room_mutex_);
        if (!IsSignalGenerationCurrentLocked(event_generation)) return;
        current_gen = session_generation_.load(std::memory_order_acquire);

        if (const auto p_it = pending_data_track_publishes_.find(pub_handle);
            p_it != pending_data_track_publishes_.end()) {
            pending = std::move(p_it->second);
            pending_data_track_publishes_.erase(p_it);

            auto entry = std::make_shared<LocalDataTrackEntry>();
            entry->pub_handle = pub_handle;
            entry->info.pub_handle = pub_handle;
            entry->info.sid = pb_info.sid();
            entry->info.name = pb_info.name();
            entry->info.uses_e2ee = (pb_info.encryption() == proto::Encryption::GCM);
            entry->packetizer = std::make_unique<data_track::DtpPacketizer>(pub_handle);

            created_track = BuildLocalDataTrackObjectLocked(entry, current_gen);
            entry->track = created_track;
            local_data_tracks_[pub_handle] = std::move(entry);
        } else if (const auto l_it = local_data_tracks_.find(pub_handle);
                   l_it != local_data_tracks_.end() && l_it->second && l_it->second->republishing) {
            l_it->second->info.sid = pb_info.sid();
            l_it->second->republishing = false;
            repub_track = l_it->second->track.lock();
        } else {
            orphan_unpublish = true;
        }
    }

    if (repub_track) {
        repub_track->UpdateSidFromReconnect(pb_info.sid());
        repub_track->SetRepublishing(false);
        std::lock_guard lock(room_mutex_);
        if (IsNativeGenerationCurrentLocked(current_gen)) {
            ScheduleDataTrackDrainLocked(current_gen);
        }
        return;
    }

    if (pending && created_track) {
        CompleteAwaitable(pending,
            DataTrackResult<std::shared_ptr<LocalDataTrack>, PublishDataTrackError>::Success(
                std::move(created_track)));
        return;
    }

    if (orphan_unpublish) {
        proto::SignalRequest unpub;
        unpub.mutable_unpublish_data_track_request()->set_pub_handle(pub_handle);
        SendDataTrackSignal(unpub, event_generation);
    }
}

void Room::HandleUnpublishDataTrackResponse(const proto::UnpublishDataTrackResponse& resp,
                                            uint64_t event_generation) {
    if (!resp.has_info() || resp.info().pub_handle() == 0 || resp.info().pub_handle() > 0xFFFFu) {
        return;
    }
    const uint16_t pub_handle = static_cast<uint16_t>(resp.info().pub_handle());
    std::shared_ptr<LocalDataTrack> track_to_retire;
    {
        std::lock_guard lock(room_mutex_);
        if (!IsSignalGenerationCurrentLocked(event_generation)) return;
        const auto it = local_data_tracks_.find(pub_handle);
        if (it != local_data_tracks_.end() && it->second) {
            it->second->unpublished = true;
            it->second->send_queue.Clear();
            it->second->in_flight_packets.clear();
            track_to_retire = it->second->track.lock();
            local_data_tracks_.erase(it);
        }
    }
    if (track_to_retire) {
        track_to_retire->MarkUnpublishedInternal();
    }
}

void Room::HandleDataTrackSubscriberHandles(const proto::DataTrackSubscriberHandles& handles,
                                            uint64_t event_generation) {
    using Res = DataTrackResult<std::shared_ptr<DataTrackStream>, SubscribeDataTrackError>;
    struct CompletedWait {
        std::shared_ptr<AwaitableState<Res>> state;
        std::shared_ptr<DataTrackStream> stream;
    };
    std::vector<CompletedWait> completed;

    {
        std::lock_guard lock(room_mutex_);
        if (!IsSignalGenerationCurrentLocked(event_generation)) return;
        const uint64_t current_gen = session_generation_.load(std::memory_order_acquire);

        for (const auto& [raw_handle, published] : handles.sub_handles()) {
            if (raw_handle == 0 || raw_handle > 0xFFFFu || published.track_sid().empty()) {
                continue;
            }
            const uint16_t sub_handle = static_cast<uint16_t>(raw_handle);
            const std::string& track_sid = published.track_sid();
            const auto dt_it = remote_data_tracks_.find(track_sid);
            if (dt_it == remote_data_tracks_.end() || !dt_it->second || !dt_it->second->published) {
                continue;
            }
            auto& entry = dt_it->second;
            if (entry->sub_state == RemoteDataTrackEntry::SubState::None) {
                continue;
            }
            if (entry->assigned_sub_handle != 0 && entry->assigned_sub_handle != sub_handle) {
                data_track_sub_handles_.erase(entry->assigned_sub_handle);
            }
            entry->assigned_sub_handle = sub_handle;
            data_track_sub_handles_[sub_handle] = track_sid;

            if (entry->sub_state == RemoteDataTrackEntry::SubState::Pending) {
                entry->sub_state = RemoteDataTrackEntry::SubState::Active;
                std::weak_ptr<Room> weak_self = weak_from_this();
                auto pending_list = std::move(entry->pending_subscribes);
                entry->pending_subscribes.clear();
                for (auto& wait_state : pending_list) {
                    if (!wait_state) continue;
                    auto stream = std::make_shared<DataTrackStream>(
                        entry->stream_buffer_size,
                        [weak_self, track_sid, current_gen]() {
                            if (auto self = weak_self.lock()) {
                                self->OnDataTrackStreamClosed(track_sid, current_gen);
                            }
                        });
                    entry->active_streams.push_back(stream);
                    completed.push_back({std::move(wait_state), std::move(stream)});
                }
            }
        }
    }

    for (auto& item : completed) {
        CompleteAwaitable(item.state, Res::Success(std::move(item.stream)));
    }
}

bool Room::HandleDataTrackRequestResponse(const proto::RequestResponse& resp,
                                          uint64_t event_generation) {
    if (!resp.has_publish_data_track()) {
        return false;
    }
    const uint32_t raw_handle = resp.publish_data_track().pub_handle();
    if (raw_handle == 0 || raw_handle > 0xFFFFu) {
        return true;
    }
    const uint16_t pub_handle = static_cast<uint16_t>(raw_handle);

    PublishDataTrackErrorCode code = PublishDataTrackErrorCode::Internal;
    switch (resp.reason()) {
        case proto::RequestResponse::NOT_ALLOWED:     code = PublishDataTrackErrorCode::NotAllowed; break;
        case proto::RequestResponse::DUPLICATE_NAME:  code = PublishDataTrackErrorCode::DuplicateName; break;
        case proto::RequestResponse::INVALID_NAME:    code = PublishDataTrackErrorCode::InvalidName; break;
        case proto::RequestResponse::INVALID_HANDLE:
        case proto::RequestResponse::DUPLICATE_HANDLE:code = PublishDataTrackErrorCode::InvalidHandle; break;
        case proto::RequestResponse::LIMIT_EXCEEDED:  code = PublishDataTrackErrorCode::LimitReached; break;
        default:                                      code = PublishDataTrackErrorCode::Internal; break;
    }
    PublishDataTrackError err{
        code,
        resp.message().empty() ? "SFU rejected PublishDataTrack request" : resp.message(),
    };

    std::shared_ptr<AwaitableState<DataTrackResult<std::shared_ptr<LocalDataTrack>, PublishDataTrackError>>>
        pending;
    std::shared_ptr<LocalDataTrack> repub_track_to_retire;
    {
        std::lock_guard lock(room_mutex_);
        if (!IsSignalGenerationCurrentLocked(event_generation)) return true;
        if (const auto it = pending_data_track_publishes_.find(pub_handle);
            it != pending_data_track_publishes_.end()) {
            pending = std::move(it->second);
            pending_data_track_publishes_.erase(it);
        } else if (const auto l_it = local_data_tracks_.find(pub_handle);
                   l_it != local_data_tracks_.end() && l_it->second && l_it->second->republishing) {
            l_it->second->unpublished = true;
            repub_track_to_retire = l_it->second->track.lock();
            local_data_tracks_.erase(l_it);
        }
    }

    if (pending) {
        CompleteAwaitable(pending,
            DataTrackResult<std::shared_ptr<LocalDataTrack>, PublishDataTrackError>::Failure(
                std::move(err)));
    }
    if (repub_track_to_retire) {
        repub_track_to_retire->MarkUnpublishedInternal();
    }
    return true;
}

void Room::OnIncomingDataTrackPacket(const std::vector<uint8_t>& payload, uint64_t generation) {
    if (payload.empty()) return;
    BeforeNativeEventCommit(generation);

    data_track::DtpPacket packet;
    data_track::DtpDeserializeError derr = data_track::DtpDeserializeError::None;
    if (!data_track::DtpPacket::Deserialize(payload, packet, &derr)) {
        Log("DATA_TRACK", "DESERIALIZE_ERR",
            std::string("Dropped malformed DTP packet: ") + data_track::ToString(derr));
        return;
    }

    std::shared_ptr<RemoteDataTrackEntry> entry;
    bool track_uses_e2ee = false;
    std::string publisher_identity;
    std::shared_ptr<E2eeManager> enc_owner;
    E2eeManager::DataPacketState enc_state{};
    std::vector<std::shared_ptr<DataTrackStream>> live_streams;
    size_t max_partials = 1;

    {
        std::lock_guard lock(room_mutex_);
        if (!IsNativeGenerationCurrentLocked(generation) ||
            connection_state_ != ConnectionState::Connected) {
            return;
        }
        const auto h_it = data_track_sub_handles_.find(packet.header.track_handle);
        if (h_it == data_track_sub_handles_.end()) {
            return;
        }
        const auto r_it = remote_data_tracks_.find(h_it->second);
        if (r_it == remote_data_tracks_.end() || !r_it->second || !r_it->second->published ||
            r_it->second->sub_state != RemoteDataTrackEntry::SubState::Active) {
            return;
        }
        entry = r_it->second;
        max_partials = entry->max_partial_frames.load(std::memory_order_acquire);
        track_uses_e2ee = entry->info.uses_e2ee;
        publisher_identity = entry->publisher_identity;
        enc_owner = e2ee_manager_;
        if (enc_owner) {
            enc_state = enc_owner->data_packet_state();
        }
        for (auto it = entry->active_streams.begin(); it != entry->active_streams.end();) {
            if (auto s = it->lock(); s && !s->IsClosed() && !s->IsEos()) {
                live_streams.push_back(std::move(s));
                ++it;
            } else {
                it = entry->active_streams.erase(it);
            }
        }
    }

    if (live_streams.empty()) {
        return;
    }

    // Fail-closed E2EE room gate before frame assembly: reject plaintext tracks
    // when room E2EE is enabled.
    if (!track_uses_e2ee && enc_owner && enc_state.enabled &&
        enc_owner->encryption_type() != EncryptionType::NONE) {
        Log("SECURITY", "DATA_TRACK_PLAINTEXT_REJECTED",
            "Rejected unencrypted remote DataTrack packet under active E2EE session");
        return;
    }

    data_track::DtpDepacketizerPushResult push_res;
    {
        std::lock_guard pipe_lock(entry->depacketizer_mutex);
        push_res = entry->depacketizer.Push(std::move(packet), {max_partials});
    }
    if (push_res.drop_error.has_value()) {
        Log("DATA_TRACK", "FRAME_DROPPED", push_res.drop_error->Describe());
    }
    if (!push_res.frame.has_value()) {
        return;
    }

    DataTrackFrame frame;
    frame.user_timestamp = push_res.frame->extensions.user_timestamp;
    frame.payload = std::move(push_res.frame->payload);

    if (track_uses_e2ee) {
        if (!enc_owner || !enc_state.enabled ||
            enc_owner->encryption_type() != EncryptionType::GCM ||
            !enc_owner->data_packet_cryptor()) {
            return;
        }
        if (!push_res.frame->extensions.e2ee.has_value()) {
            Log("SECURITY", "DATA_TRACK_E2EE_MISSING",
                "Dropped encrypted DataTrack frame missing E2EE header extension");
            return;
        }
        const auto& ext = *push_res.frame->extensions.e2ee;
        EncryptedDataPacket encrypted_pkt;
        encrypted_pkt.encryption_type = EncryptionType::GCM;
        encrypted_pkt.key_index = ext.key_index;
        encrypted_pkt.iv.assign(ext.iv.begin(), ext.iv.end());
        encrypted_pkt.ciphertext = std::move(frame.payload);

        auto decrypted = enc_owner->data_packet_cryptor()->DecryptPacket(
            publisher_identity,
            encrypted_pkt,
            DataPacketCryptor::kMaxDataTrackPayloadBytes);
        const auto* auth = std::get_if<AuthenticatedDataPayload>(&decrypted);
        if (!auth) {
            Log("SECURITY", "DATA_TRACK_DECRYPT_FAILED",
                "Dropped remote DataTrack frame failing E2EE authentication");
            return;
        }
        frame.payload = auth->bytes;
    }

    // Revalidate session generation and E2EE policy revision right before delivery.
    {
        std::lock_guard lock(room_mutex_);
        if (!IsNativeGenerationCurrentLocked(generation) ||
            connection_state_ != ConnectionState::Connected ||
            !entry->published ||
            entry->sub_state != RemoteDataTrackEntry::SubState::Active) {
            return;
        }
        if (track_uses_e2ee &&
            (e2ee_manager_ != enc_owner || !enc_owner ||
             !enc_owner->enabled() ||
             enc_owner->data_packet_state().policy_revision != enc_state.policy_revision)) {
            return;
        }
    }

    for (const auto& stream : live_streams) {
        stream->PushFrame(frame);
    }
}

void Room::PrepareDataTracksForFullRestartLocked() {
    reconnect_data_track_records_.clear();
    reconnect_data_sub_records_.clear();

    for (const auto& [pub_handle, entry] : local_data_tracks_) {
        if (!entry || entry->unpublished) continue;
        auto live = entry->track.lock();
        if (!live || !live->IsPublished()) continue;
        entry->republishing = true;
        live->SetRepublishing(true);
        reconnect_data_track_records_.push_back({
            pub_handle,
            entry->info.name,
            entry->info.uses_e2ee,
            live,
        });
    }

    for (auto& [sid, entry] : remote_data_tracks_) {
        (void)sid;
        if (!entry || !entry->published) continue;
        if (entry->sub_state == RemoteDataTrackEntry::SubState::None) continue;
        std::vector<std::weak_ptr<DataTrackStream>> live_streams;
        for (const auto& weak_s : entry->active_streams) {
            if (auto s = weak_s.lock(); s && !s->IsClosed() && !s->IsEos()) {
                live_streams.push_back(weak_s);
            }
        }
        if (!live_streams.empty()) {
            reconnect_data_sub_records_.push_back({
                entry->publisher_identity,
                entry->info.name,
                entry->info.sid,
                entry->target_fps,
                entry->max_partial_frames.load(std::memory_order_acquire),
                std::move(live_streams),
            });
            // Detach streams from the old session's entry so terminal teardown of
            // the old session does not spuriously push EOS before recovery tries.
            entry->active_streams.clear();
        }
    }
}

void Room::TakeDataTrackCleanupLocked(bool clear_reconnect_records,
                                      DataTrackCleanupBatch& batch) {
    for (const auto& waiter : pending_data_track_dc_waits_) {
        if (waiter.completion) batch.dc_waits.push_back(waiter.completion);
    }
    pending_data_track_dc_waits_.clear();

    for (auto& [handle, state] : pending_data_track_publishes_) {
        (void)handle;
        if (state) batch.publish_states.push_back(std::move(state));
    }
    pending_data_track_publishes_.clear();

    for (auto& [handle, entry] : local_data_tracks_) {
        (void)handle;
        if (!entry) continue;
        entry->unpublished = true;
        entry->send_queue.Clear();
        entry->in_flight_packets.clear();
        if (auto live = entry->track.lock()) {
            batch.local_tracks.push_back(std::move(live));
        }
    }
    local_data_tracks_.clear();

    for (auto& [sid, entry] : remote_data_tracks_) {
        (void)sid;
        if (!entry) continue;
        entry->published = false;
        if (auto live = entry->track.lock()) {
            batch.remote_tracks.push_back(std::move(live));
        }
        for (auto& sub_state : entry->pending_subscribes) {
            if (sub_state) batch.subscribe_states.push_back(std::move(sub_state));
        }
        entry->pending_subscribes.clear();
        for (const auto& weak_s : entry->active_streams) {
            if (auto s = weak_s.lock()) {
                batch.streams.push_back(std::move(s));
            }
        }
        entry->active_streams.clear();
    }
    remote_data_tracks_.clear();
    data_track_sub_handles_.clear();
    data_track_drain_scheduled_ = false;

    if (clear_reconnect_records) {
        for (auto& rec : reconnect_data_track_records_) {
            if (auto live = rec.track.lock()) {
                batch.local_tracks.push_back(std::move(live));
            }
        }
        reconnect_data_track_records_.clear();

        for (auto& sub_rec : reconnect_data_sub_records_) {
            for (const auto& weak_s : sub_rec.streams) {
                if (auto s = weak_s.lock()) {
                    batch.streams.push_back(std::move(s));
                }
            }
        }
        reconnect_data_sub_records_.clear();
    }
}

void Room::FinalizeDataTrackCleanup(DataTrackCleanupBatch batch,
                                    OperationErrorCode code,
                                    const std::string& message) {
    const auto pub_err = code == OperationErrorCode::ReconnectExhausted ||
            code == OperationErrorCode::SessionClosed ||
            code == OperationErrorCode::Cancelled
        ? PublishDataTrackErrorCode::Disconnected
        : PublishDataTrackErrorCode::Timeout;
    const auto sub_err = code == OperationErrorCode::ReconnectExhausted ||
            code == OperationErrorCode::SessionClosed ||
            code == OperationErrorCode::Cancelled
        ? SubscribeDataTrackErrorCode::Disconnected
        : SubscribeDataTrackErrorCode::Timeout;

    for (const auto& dc_wait : batch.dc_waits) {
        FailAwaitable(dc_wait, std::make_exception_ptr(OperationError(
            OperationKind::PublishTrack, code, "data_track_cleanup", message, true)));
    }
    for (const auto& pub_wait : batch.publish_states) {
        CompleteAwaitable(pub_wait,
            DataTrackResult<std::shared_ptr<LocalDataTrack>, PublishDataTrackError>::Failure({
                pub_err, message
            }));
    }
    for (const auto& sub_wait : batch.subscribe_states) {
        CompleteAwaitable(sub_wait,
            DataTrackResult<std::shared_ptr<DataTrackStream>, SubscribeDataTrackError>::Failure({
                sub_err, message
            }));
    }
    for (const auto& track : batch.local_tracks) {
        if (track) track->MarkUnpublishedInternal();
    }
    for (const auto& remote_track : batch.remote_tracks) {
        if (remote_track) remote_track->MarkUnpublishedInternal();
    }
    for (const auto& stream : batch.streams) {
        if (stream) {
            stream->PushEos(SubscribeDataTrackError{sub_err, message});
        }
    }
}

asio::awaitable<void> Room::RecoverDataTracksAfterFullRestartAsync(uint64_t generation) {
    std::vector<ReconnectLocalDataTrackRecord> local_records;
    std::vector<ReconnectRemoteDataSubRecord> remote_records;
    {
        std::lock_guard lock(room_mutex_);
        if (!IsNativeGenerationCurrentLocked(generation) ||
            connection_state_ != ConnectionState::Connected) {
            co_return;
        }
        local_records = std::move(reconnect_data_track_records_);
        remote_records = std::move(reconnect_data_sub_records_);
        reconnect_data_track_records_.clear();
        reconnect_data_sub_records_.clear();
    }

    // 1. Restore remote subscriptions against the newly joined participant tracks.
    std::vector<std::shared_ptr<DataTrackStream>> orphaned_streams;
    std::vector<proto::SignalRequest> resub_requests;
    {
        std::lock_guard lock(room_mutex_);
        if (!IsNativeGenerationCurrentLocked(generation) ||
            connection_state_ != ConnectionState::Connected) {
            for (auto& rec : local_records) {
                if (auto t = rec.track.lock()) t->MarkUnpublishedInternal();
            }
            for (auto& srec : remote_records) {
                for (const auto& w : srec.streams) {
                    if (auto s = w.lock()) orphaned_streams.push_back(std::move(s));
                }
            }
            co_return;
        }

        std::weak_ptr<Room> weak_self = weak_from_this();
        for (auto& rec : remote_records) {
            std::shared_ptr<RemoteDataTrackEntry> matched;
            for (const auto& [new_sid, entry] : remote_data_tracks_) {
                (void)new_sid;
                if (!entry || !entry->published) continue;
                if (entry->publisher_identity == rec.publisher_identity &&
                    entry->info.name == rec.track_name) {
                    matched = entry;
                    break;
                }
            }
            if (!matched) {
                for (const auto& w : rec.streams) {
                    if (auto s = w.lock()) orphaned_streams.push_back(std::move(s));
                }
                continue;
            }
            matched->max_partial_frames.store(
                std::max<size_t>(1, rec.max_partial_frames), std::memory_order_release);
            if (rec.target_fps.has_value()) {
                matched->target_fps = rec.target_fps;
            }
            const std::string new_sid = matched->info.sid;
            size_t attached = 0;
            for (const auto& w : rec.streams) {
                if (auto s = w.lock(); s && !s->IsClosed() && !s->IsEos()) {
                    s->SetCloseHandler([weak_self, new_sid, generation]() {
                        if (auto self = weak_self.lock()) {
                            self->OnDataTrackStreamClosed(new_sid, generation);
                        }
                    });
                    matched->active_streams.push_back(s);
                    ++attached;
                }
            }
            if (attached > 0 && matched->sub_state == RemoteDataTrackEntry::SubState::None) {
                matched->sub_state = RemoteDataTrackEntry::SubState::Pending;
                proto::SignalRequest req;
                auto* upd = req.mutable_update_data_subscription()->add_updates();
                upd->set_track_sid(new_sid);
                upd->set_subscribe(true);
                if (matched->target_fps.has_value()) {
                    upd->mutable_options()->set_target_fps(*matched->target_fps);
                }
                resub_requests.push_back(std::move(req));
            }
        }
    }

    for (const auto& s : orphaned_streams) {
        if (s) s->PushEos(std::nullopt);
    }
    for (const auto& req : resub_requests) {
        SendDataTrackSignal(req, generation);
    }

    // 2. Re-publish surviving local DataTracks preserving original pub_handle & object identity.
    std::vector<std::pair<std::shared_ptr<LocalDataTrack>,
        std::pair<LocalDataTrack::TryPushHandler, LocalDataTrack::UnpublishHandler>>>
        rebound_locals;
    std::vector<proto::SignalRequest> repub_requests;
    {
        std::lock_guard lock(room_mutex_);
        if (!IsNativeGenerationCurrentLocked(generation) ||
            connection_state_ != ConnectionState::Connected) {
            for (auto& rec : local_records) {
                if (auto t = rec.track.lock()) t->MarkUnpublishedInternal();
            }
            co_return;
        }
        for (const auto& rec : local_records) {
            auto live = rec.track.lock();
            if (!live || !live->IsPublished()) continue;
            auto entry = std::make_shared<LocalDataTrackEntry>();
            entry->pub_handle = rec.pub_handle;
            entry->info = live->info();
            entry->republishing = true;
            entry->track = live;
            entry->packetizer = std::make_unique<data_track::DtpPacketizer>(rec.pub_handle);
            // Rebind hooks to the new session generation while keeping original LocalDataTrack alive.
            rebound_locals.emplace_back(live, MakeLocalDataTrackHandlers(rec.pub_handle, generation));
            local_data_tracks_[rec.pub_handle] = std::move(entry);

            proto::SignalRequest req;
            auto* pub = req.mutable_publish_data_track_request();
            pub->set_pub_handle(rec.pub_handle);
            pub->set_name(rec.name);
            pub->set_encryption(rec.uses_e2ee ? proto::Encryption::GCM : proto::Encryption::NONE);
            repub_requests.push_back(std::move(req));
        }
    }

    for (auto& [live, handlers] : rebound_locals) {
        live->RebindHandlersInternal(std::move(handlers.first), std::move(handlers.second));
    }
    for (const auto& req : repub_requests) {
        SendDataTrackSignal(req, generation);
    }
    co_return;
}


} // namespace livekit
