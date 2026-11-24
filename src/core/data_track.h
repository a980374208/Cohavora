#pragma once

#include <asio.hpp>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace livekit {

struct DataTrackInfo {
    uint16_t pub_handle = 0;
    std::string sid;
    std::string name;
    bool uses_e2ee = false;

    bool operator==(const DataTrackInfo& other) const = default;
};

struct DataTrackFrame {
    std::vector<uint8_t> payload;
    std::optional<uint64_t> user_timestamp;

    DataTrackFrame() = default;
    DataTrackFrame(const DataTrackFrame&) = default;
    DataTrackFrame(DataTrackFrame&&) noexcept = default;
    DataTrackFrame& operator=(const DataTrackFrame&) = default;
    DataTrackFrame& operator=(DataTrackFrame&&) noexcept = default;

    explicit DataTrackFrame(std::vector<uint8_t>&& p,
                            std::optional<uint64_t> ts = std::nullopt) noexcept
        : payload(std::move(p)), user_timestamp(ts) {}

    bool operator==(const DataTrackFrame& other) const = default;
};

struct DataTrackPublishOptions {
    std::string name;
};

struct DataTrackPipelineOptions {
    std::optional<uint32_t> max_partial_frames;
};

struct DataTrackSubscribeOptions {
    std::optional<uint32_t> buffer_size;
    std::optional<uint32_t> target_fps;
};

enum class PublishDataTrackErrorCode : uint32_t {
    Unknown = 0,
    InvalidHandle = 1,
    DuplicateName = 2,
    Timeout = 3,
    Disconnected = 4,
    NotAllowed = 5,
    InvalidName = 6,
    LimitReached = 7,
    ProtocolError = 8,
    Internal = 9,
};

struct PublishDataTrackError {
    PublishDataTrackErrorCode code = PublishDataTrackErrorCode::Unknown;
    std::string message;
};

enum class LocalDataTrackTryPushErrorCode : uint32_t {
    Unknown = 0,
    InvalidHandle = 1,
    TrackUnpublished = 2,
    QueueFull = 3,
    Internal = 4,
    EncryptionFailed = 5,
};

struct LocalDataTrackTryPushError {
    LocalDataTrackTryPushErrorCode code = LocalDataTrackTryPushErrorCode::Unknown;
    std::string message;
};

enum class SubscribeDataTrackErrorCode : uint32_t {
    Unknown = 0,
    InvalidHandle = 1,
    Unpublished = 2,
    Timeout = 3,
    Disconnected = 4,
    ProtocolError = 5,
    Internal = 6,
};

struct SubscribeDataTrackError {
    SubscribeDataTrackErrorCode code = SubscribeDataTrackErrorCode::Unknown;
    std::string message;
};

template <typename T, typename E>
class DataTrackResult {
public:
    static DataTrackResult Success(T value) {
        DataTrackResult res;
        res.value_ = std::move(value);
        res.ok_ = true;
        return res;
    }

    static DataTrackResult Failure(E error) {
        DataTrackResult res;
        res.error_ = std::move(error);
        res.ok_ = false;
        return res;
    }

    bool ok() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }

    const T& value() const& { return *value_; }
    T& value() & { return *value_; }
    T&& value() && { return std::move(*value_); }

    const E& error() const& { return *error_; }
    E& error() & { return *error_; }

private:
    bool ok_ = false;
    std::optional<T> value_;
    std::optional<E> error_;
};

template <typename E>
class DataTrackResult<void, E> {
public:
    static DataTrackResult Success() {
        DataTrackResult res;
        res.ok_ = true;
        return res;
    }

    static DataTrackResult Failure(E error) {
        DataTrackResult res;
        res.error_ = std::move(error);
        res.ok_ = false;
        return res;
    }

    bool ok() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }

    const E& error() const& { return *error_; }
    E& error() & { return *error_; }

private:
    bool ok_ = false;
    std::optional<E> error_;
};

/// Bounded, drop-oldest queue of whole serialized frame packet batches.
/// A multi-packet frame is always enqueued and evicted atomically so partial
/// frames never mix on the wire.
class DataTrackSendQueue {
public:
    using FramePackets = std::vector<std::vector<uint8_t>>;

    explicit DataTrackSendQueue(size_t capacity = 1);

    /// Pushes all serialized packets for one frame.
    /// Empty batches are a no-op returning std::nullopt.
    /// When full, evicts and returns the oldest queued whole frame.
    std::optional<FramePackets> Push(FramePackets packets);

    std::optional<FramePackets> TryPop();
    size_t Size() const;
    bool Empty() const;
    void Clear();

private:
    size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<FramePackets> queue_;
};

class Room;

class LocalDataTrack final : public std::enable_shared_from_this<LocalDataTrack> {
public:
    using TryPushHandler =
        std::function<DataTrackResult<void, LocalDataTrackTryPushError>(const DataTrackFrame&)>;
    using UnpublishHandler = std::function<void()>;

    LocalDataTrack(DataTrackInfo info,
                   TryPushHandler try_push_handler,
                   UnpublishHandler unpublish_handler);
    ~LocalDataTrack();

    LocalDataTrack(const LocalDataTrack&) = delete;
    LocalDataTrack& operator=(const LocalDataTrack&) = delete;

    DataTrackInfo info() const;
    uint16_t pub_handle() const noexcept { return pub_handle_; }
    std::string sid() const;
    const std::string& name() const noexcept { return name_; }
    bool uses_e2ee() const noexcept { return uses_e2ee_; }

    DataTrackResult<void, LocalDataTrackTryPushError> TryPush(const DataTrackFrame& frame);
    DataTrackResult<void, LocalDataTrackTryPushError> TryPush(
        std::vector<uint8_t>&& payload,
        std::optional<uint64_t> user_timestamp = std::nullopt);
    DataTrackResult<void, LocalDataTrackTryPushError> TryPush(
        const uint8_t* data,
        size_t size,
        std::optional<uint64_t> user_timestamp = std::nullopt);

    bool IsPublished() const noexcept;
    void UnpublishDataTrack();

    // Internal lifecycle updates managed by Room:
    void UpdateSidFromReconnect(const std::string& new_sid);
    void SetRepublishing(bool republishing) noexcept;
    bool IsRepublishing() const noexcept;
    void RebindHandlersInternal(TryPushHandler try_push_handler,
                                UnpublishHandler unpublish_handler);
    void MarkUnpublishedInternal() noexcept;

private:
    const uint16_t pub_handle_;
    const std::string name_;
    const bool uses_e2ee_;

    mutable std::mutex state_mutex_;
    DataTrackInfo info_;
    TryPushHandler try_push_handler_;
    UnpublishHandler unpublish_handler_;

    std::atomic<bool> published_{true};
    std::atomic<bool> republishing_{false};
};

class DataTrackStream final : public std::enable_shared_from_this<DataTrackStream> {
public:
    static constexpr size_t kDefaultBufferSize = 16;

    using FrameCallback = std::function<void(const DataTrackFrame&)>;
    using CloseHandler = std::function<void()>;

    explicit DataTrackStream(size_t buffer_size = kDefaultBufferSize,
                             CloseHandler on_close = {});
    ~DataTrackStream();

    DataTrackStream(const DataTrackStream&) = delete;
    DataTrackStream& operator=(const DataTrackStream&) = delete;

    /// Blocking read: returns true with next frame, or false on EOS/Close.
    bool Read(DataTrackFrame& out);

    /// Non-blocking read: returns true if a buffered frame was available.
    bool TryRead(DataTrackFrame& out);

    /// Optional non-blocking push callback invoked outside internal lock.
    void SetFrameCallback(FrameCallback callback);

    std::optional<SubscribeDataTrackError> TerminalError() const;
    bool IsClosed() const noexcept;
    bool IsEos() const noexcept;
    size_t BufferedCount() const;

    void Close();

    // Internal feed APIs called by Room delivery path:
    void PushFrame(DataTrackFrame frame);
    void PushEos(std::optional<SubscribeDataTrackError> error = std::nullopt);
    void SetCloseHandler(CloseHandler on_close);

private:
    const size_t buffer_size_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<DataTrackFrame> frames_;
    bool eof_ = false;
    bool closed_ = false;
    std::optional<SubscribeDataTrackError> terminal_error_;
    FrameCallback frame_callback_;
    CloseHandler on_close_;
};

class RemoteDataTrack final : public std::enable_shared_from_this<RemoteDataTrack> {
public:
    using SubscribeAsyncHandler = std::function<
        asio::awaitable<DataTrackResult<std::shared_ptr<DataTrackStream>, SubscribeDataTrackError>>(
            DataTrackSubscribeOptions)>;
    using SubscribeSyncHandler =
        std::function<DataTrackResult<std::shared_ptr<DataTrackStream>, SubscribeDataTrackError>(
            const DataTrackSubscribeOptions&)>;
    using PipelineOptionsHandler = std::function<void(const DataTrackPipelineOptions&)>;

    RemoteDataTrack(DataTrackInfo info,
                    std::string publisher_identity,
                    SubscribeAsyncHandler subscribe_async_handler,
                    SubscribeSyncHandler subscribe_sync_handler,
                    PipelineOptionsHandler pipeline_options_handler);

    RemoteDataTrack(const RemoteDataTrack&) = delete;
    RemoteDataTrack& operator=(const RemoteDataTrack&) = delete;

    const DataTrackInfo& info() const noexcept { return info_; }
    const std::string& publisher_identity() const noexcept { return publisher_identity_; }
    const std::string& sid() const noexcept { return info_.sid; }
    const std::string& name() const noexcept { return info_.name; }
    bool uses_e2ee() const noexcept { return info_.uses_e2ee; }

    bool IsPublished() const noexcept { return published_.load(std::memory_order_acquire); }

    void SetPipelineOptions(const DataTrackPipelineOptions& options);

    asio::awaitable<DataTrackResult<std::shared_ptr<DataTrackStream>, SubscribeDataTrackError>>
    SubscribeAsync(DataTrackSubscribeOptions options = {});

    /// Synchronous convenience wrapper for non-executor threads.
    DataTrackResult<std::shared_ptr<DataTrackStream>, SubscribeDataTrackError>
    Subscribe(const DataTrackSubscribeOptions& options = {});

    // Internal lifecycle update from Room:
    void MarkUnpublishedInternal() noexcept;

private:
    DataTrackInfo info_;
    std::string publisher_identity_;
    std::atomic<bool> published_{true};

    mutable std::mutex handlers_mutex_;
    SubscribeAsyncHandler subscribe_async_handler_;
    SubscribeSyncHandler subscribe_sync_handler_;
    PipelineOptionsHandler pipeline_options_handler_;
};

} // namespace livekit
