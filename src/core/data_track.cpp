#include "data_track.h"

#include <algorithm>

namespace livekit {

// =========================================================================
// DataTrackSendQueue
// =========================================================================

DataTrackSendQueue::DataTrackSendQueue(size_t capacity)
    : capacity_(std::max<size_t>(1, capacity)) {}

std::optional<DataTrackSendQueue::FramePackets> DataTrackSendQueue::Push(FramePackets packets) {
    if (packets.empty()) {
        return std::nullopt;
    }
    std::lock_guard lock(mutex_);
    std::optional<FramePackets> evicted;
    if (queue_.size() >= capacity_) {
        evicted = std::move(queue_.front());
        queue_.pop_front();
    }
    queue_.push_back(std::move(packets));
    return evicted;
}

std::optional<DataTrackSendQueue::FramePackets> DataTrackSendQueue::TryPop() {
    std::lock_guard lock(mutex_);
    if (queue_.empty()) {
        return std::nullopt;
    }
    FramePackets front = std::move(queue_.front());
    queue_.pop_front();
    return front;
}

size_t DataTrackSendQueue::Size() const {
    std::lock_guard lock(mutex_);
    return queue_.size();
}

bool DataTrackSendQueue::Empty() const {
    std::lock_guard lock(mutex_);
    return queue_.empty();
}

void DataTrackSendQueue::Clear() {
    std::lock_guard lock(mutex_);
    queue_.clear();
}

// =========================================================================
// LocalDataTrack
// =========================================================================

LocalDataTrack::LocalDataTrack(DataTrackInfo info,
                               TryPushHandler try_push_handler,
                               UnpublishHandler unpublish_handler)
    : pub_handle_(info.pub_handle),
      name_(info.name),
      uses_e2ee_(info.uses_e2ee),
      info_(std::move(info)),
      try_push_handler_(std::move(try_push_handler)),
      unpublish_handler_(std::move(unpublish_handler)) {}

LocalDataTrack::~LocalDataTrack() {
    UnpublishDataTrack();
}

DataTrackInfo LocalDataTrack::info() const {
    std::lock_guard lock(state_mutex_);
    return info_;
}

std::string LocalDataTrack::sid() const {
    std::lock_guard lock(state_mutex_);
    return info_.sid;
}

bool LocalDataTrack::IsPublished() const noexcept {
    return published_.load(std::memory_order_acquire);
}

bool LocalDataTrack::IsRepublishing() const noexcept {
    return republishing_.load(std::memory_order_acquire);
}

void LocalDataTrack::SetRepublishing(bool republishing) noexcept {
    republishing_.store(republishing, std::memory_order_release);
}

void LocalDataTrack::UpdateSidFromReconnect(const std::string& new_sid) {
    std::lock_guard lock(state_mutex_);
    info_.sid = new_sid;
}

void LocalDataTrack::RebindHandlersInternal(TryPushHandler try_push_handler,
                                          UnpublishHandler unpublish_handler) {
    std::lock_guard lock(state_mutex_);
    if (!IsPublished()) {
        return;
    }
    try_push_handler_ = std::move(try_push_handler);
    unpublish_handler_ = std::move(unpublish_handler);
}

void LocalDataTrack::MarkUnpublishedInternal() noexcept {
    published_.store(false, std::memory_order_release);
    std::lock_guard lock(state_mutex_);
    try_push_handler_ = nullptr;
    unpublish_handler_ = nullptr;
}

DataTrackResult<void, LocalDataTrackTryPushError> LocalDataTrack::TryPush(
    const DataTrackFrame& frame) {
    if (!IsPublished()) {
        return DataTrackResult<void, LocalDataTrackTryPushError>::Failure({
            LocalDataTrackTryPushErrorCode::TrackUnpublished,
            "LocalDataTrack is no longer published",
        });
    }
    if (IsRepublishing()) {
        return DataTrackResult<void, LocalDataTrackTryPushError>::Failure({
            LocalDataTrackTryPushErrorCode::QueueFull,
            "LocalDataTrack is republishing during reconnect",
        });
    }
    if (frame.payload.empty()) {
        return DataTrackResult<void, LocalDataTrackTryPushError>::Success();
    }

    TryPushHandler handler;
    {
        std::lock_guard lock(state_mutex_);
        handler = try_push_handler_;
    }
    if (!handler) {
        return DataTrackResult<void, LocalDataTrackTryPushError>::Failure({
            LocalDataTrackTryPushErrorCode::TrackUnpublished,
            "LocalDataTrack is no longer published",
        });
    }
    return handler(frame);
}

DataTrackResult<void, LocalDataTrackTryPushError> LocalDataTrack::TryPush(
    std::vector<uint8_t>&& payload,
    std::optional<uint64_t> user_timestamp) {
    DataTrackFrame frame(std::move(payload), user_timestamp);
    return TryPush(frame);
}

DataTrackResult<void, LocalDataTrackTryPushError> LocalDataTrack::TryPush(
    const uint8_t* data,
    size_t size,
    std::optional<uint64_t> user_timestamp) {
    if (size == 0) {
        DataTrackFrame empty;
        empty.user_timestamp = user_timestamp;
        return TryPush(empty);
    }
    if (!data) {
        return DataTrackResult<void, LocalDataTrackTryPushError>::Failure({
            LocalDataTrackTryPushErrorCode::Internal,
            "LocalDataTrack::TryPush: payload pointer is null",
        });
    }
    DataTrackFrame frame;
    frame.payload.assign(data, data + size);
    frame.user_timestamp = user_timestamp;
    return TryPush(frame);
}

void LocalDataTrack::UnpublishDataTrack() {
    if (!published_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    UnpublishHandler handler;
    {
        std::lock_guard lock(state_mutex_);
        try_push_handler_ = nullptr;
        handler = std::move(unpublish_handler_);
    }
    if (handler) {
        handler();
    }
}

// =========================================================================
// DataTrackStream
// =========================================================================

DataTrackStream::DataTrackStream(size_t buffer_size, CloseHandler on_close)
    : buffer_size_(std::max<size_t>(1, buffer_size)),
      on_close_(std::move(on_close)) {}

DataTrackStream::~DataTrackStream() {
    Close();
}

bool DataTrackStream::Read(DataTrackFrame& out) {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this]() {
        return closed_ || !frames_.empty() || eof_;
    });
    if (closed_) {
        return false;
    }
    if (!frames_.empty()) {
        out = std::move(frames_.front());
        frames_.pop_front();
        return true;
    }
    return false;
}

bool DataTrackStream::TryRead(DataTrackFrame& out) {
    std::lock_guard lock(mutex_);
    if (closed_ || frames_.empty()) {
        return false;
    }
    out = std::move(frames_.front());
    frames_.pop_front();
    return true;
}

void DataTrackStream::SetFrameCallback(FrameCallback callback) {
    std::lock_guard lock(mutex_);
    frame_callback_ = std::move(callback);
}

std::optional<SubscribeDataTrackError> DataTrackStream::TerminalError() const {
    std::lock_guard lock(mutex_);
    return terminal_error_;
}

bool DataTrackStream::IsClosed() const noexcept {
    std::lock_guard lock(mutex_);
    return closed_;
}

bool DataTrackStream::IsEos() const noexcept {
    std::lock_guard lock(mutex_);
    return eof_;
}

size_t DataTrackStream::BufferedCount() const {
    std::lock_guard lock(mutex_);
    return frames_.size();
}

void DataTrackStream::Close() {
    CloseHandler close_handler;
    {
        std::lock_guard lock(mutex_);
        if (closed_) {
            return;
        }
        closed_ = true;
        if (!eof_) {
            terminal_error_.reset();
        }
        frames_.clear();
        frame_callback_ = nullptr;
        close_handler = std::move(on_close_);
    }
    cv_.notify_all();
    if (close_handler) {
        close_handler();
    }
}

void DataTrackStream::PushFrame(DataTrackFrame frame) {
    FrameCallback callback;
    DataTrackFrame cb_copy;
    {
        std::unique_lock lock(mutex_);
        if (closed_ || eof_) {
            return;
        }
        callback = frame_callback_;
        if (frames_.size() >= buffer_size_) {
            frames_.pop_front();
        }
        frames_.push_back(std::move(frame));
        if (callback) {
            cb_copy = frames_.back();
        }
    }
    cv_.notify_one();
    if (callback) {
        callback(cb_copy);
    }
}

void DataTrackStream::PushEos(std::optional<SubscribeDataTrackError> error) {
    {
        std::lock_guard lock(mutex_);
        if (closed_ || eof_) {
            return;
        }
        eof_ = true;
        terminal_error_ = std::move(error);
    }
    cv_.notify_all();
}

void DataTrackStream::SetCloseHandler(CloseHandler on_close) {
    bool already_closed = false;
    {
        std::lock_guard lock(mutex_);
        already_closed = closed_;
        if (!already_closed) {
            on_close_ = std::move(on_close);
        }
    }
    if (already_closed && on_close) {
        on_close();
    }
}

// =========================================================================
// RemoteDataTrack
// =========================================================================

RemoteDataTrack::RemoteDataTrack(DataTrackInfo info,
                                 std::string publisher_identity,
                                 SubscribeAsyncHandler subscribe_async_handler,
                                 SubscribeSyncHandler subscribe_sync_handler,
                                 PipelineOptionsHandler pipeline_options_handler)
    : info_(std::move(info)),
      publisher_identity_(std::move(publisher_identity)),
      subscribe_async_handler_(std::move(subscribe_async_handler)),
      subscribe_sync_handler_(std::move(subscribe_sync_handler)),
      pipeline_options_handler_(std::move(pipeline_options_handler)) {}

void RemoteDataTrack::SetPipelineOptions(const DataTrackPipelineOptions& options) {
    PipelineOptionsHandler handler;
    {
        std::lock_guard lock(handlers_mutex_);
        handler = pipeline_options_handler_;
    }
    if (handler) {
        handler(options);
    }
}

asio::awaitable<DataTrackResult<std::shared_ptr<DataTrackStream>, SubscribeDataTrackError>>
RemoteDataTrack::SubscribeAsync(DataTrackSubscribeOptions options) {
    if (!IsPublished()) {
        co_return DataTrackResult<std::shared_ptr<DataTrackStream>, SubscribeDataTrackError>::Failure({
            SubscribeDataTrackErrorCode::Unpublished,
            "RemoteDataTrack is no longer published",
        });
    }
    SubscribeAsyncHandler handler;
    {
        std::lock_guard lock(handlers_mutex_);
        handler = subscribe_async_handler_;
    }
    if (!handler) {
        co_return DataTrackResult<std::shared_ptr<DataTrackStream>, SubscribeDataTrackError>::Failure({
            SubscribeDataTrackErrorCode::Disconnected,
            "RemoteDataTrack is detached from active session",
        });
    }
    co_return co_await handler(std::move(options));
}

DataTrackResult<std::shared_ptr<DataTrackStream>, SubscribeDataTrackError>
RemoteDataTrack::Subscribe(const DataTrackSubscribeOptions& options) {
    if (!IsPublished()) {
        return DataTrackResult<std::shared_ptr<DataTrackStream>, SubscribeDataTrackError>::Failure({
            SubscribeDataTrackErrorCode::Unpublished,
            "RemoteDataTrack is no longer published",
        });
    }
    SubscribeSyncHandler handler;
    {
        std::lock_guard lock(handlers_mutex_);
        handler = subscribe_sync_handler_;
    }
    if (!handler) {
        return DataTrackResult<std::shared_ptr<DataTrackStream>, SubscribeDataTrackError>::Failure({
            SubscribeDataTrackErrorCode::Disconnected,
            "RemoteDataTrack is detached from active session",
        });
    }
    return handler(options);
}

void RemoteDataTrack::MarkUnpublishedInternal() noexcept {
    published_.store(false, std::memory_order_release);
    std::lock_guard lock(handlers_mutex_);
    subscribe_async_handler_ = nullptr;
    subscribe_sync_handler_ = nullptr;
    pipeline_options_handler_ = nullptr;
}

} // namespace livekit
