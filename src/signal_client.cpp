#include "signal_client.h"
#include "region_provider.h"
#include "safe_spawn.h"
#include "livekit_rtc.pb.h"
#include "livekit_models.pb.h"
#include "logger/options.pb.h"
#include <sstream>
#include <algorithm>
#include <iostream>
#include <zlib.h>

namespace livekit {

// Base64 helper from websocket_client
static std::string Base64Encode(const unsigned char* buffer, size_t length) {
    static const char char_set[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((length + 2) / 3) * 4);
    size_t i = 0;
    while (i < length) {
        size_t count = length - i;
        uint32_t octet_a = buffer[i++];
        uint32_t octet_b = (count > 1) ? buffer[i++] : 0;
        uint32_t octet_c = (count > 2) ? buffer[i++] : 0;

        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        result.push_back(char_set[(triple >> 18) & 0x3F]);
        result.push_back(char_set[(triple >> 12) & 0x3F]);
        result.push_back((count > 1) ? char_set[(triple >> 6) & 0x3F] : '=');
        result.push_back((count > 2) ? char_set[triple & 0x3F] : '=');
    }
    return result;
}

static std::string UrlEncode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (char c : value) {
        if (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << '%' << std::setw(2) << std::uppercase << (static_cast<int>(static_cast<unsigned char>(c)) & 0xFF);
        }
    }
    return escaped.str();
}

static std::string Base64UrlEncode(const std::vector<uint8_t>& data) {
    std::string s = Base64Encode(data.data(), data.size());
    for (char& c : s) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    return s;
}

static std::vector<uint8_t> GzipCompress(const std::vector<uint8_t>& data) {
    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));

    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        throw std::runtime_error("deflateInit2 failed");
    }

    zs.next_in = const_cast<Bytef*>(data.data());
    zs.avail_in = static_cast<uInt>(data.size());

    int ret;
    std::vector<uint8_t> out;
    out.resize(32768);

    zs.next_out = out.data();
    zs.avail_out = static_cast<uInt>(out.size());

    do {
        if (zs.avail_out == 0) {
            size_t old_size = out.size();
            out.resize(old_size * 2);
            zs.next_out = out.data() + old_size;
            zs.avail_out = static_cast<uInt>(old_size);
        }
        ret = deflate(&zs, Z_FINISH);
    } while (ret == Z_OK);

    if (ret != Z_STREAM_END) {
        deflateEnd(&zs);
        throw std::runtime_error("deflate failed");
    }

    out.resize(zs.total_out);
    deflateEnd(&zs);
    return out;
}

static std::string CreateJoinRequestParam(const SignalOptions& options,
                                          bool reconnect,
                                          const std::string& participant_sid,
                                          const std::optional<std::vector<uint8_t>>& publisher_offer_sdp) {
    livekit::proto::JoinRequest join_req;
    join_req.set_reconnect(reconnect);
    if (!participant_sid.empty()) {
        join_req.set_participant_sid(participant_sid);
    }
    
    auto* conn_settings = join_req.mutable_connection_settings();
    conn_settings->set_auto_subscribe(options.auto_subscribe);
    conn_settings->set_adaptive_stream(options.adaptive_stream);
    
    auto* client_info = join_req.mutable_client_info();
    client_info->set_sdk(livekit::proto::ClientInfo::CPP);
    if (options.sdk_options.sdk_version) {
        client_info->set_version(*options.sdk_options.sdk_version);
    }
    client_info->set_protocol(14); 
    client_info->set_client_protocol(1); 
    client_info->add_capabilities(livekit::proto::ClientInfo::CAP_PACKET_TRAILER); 
    
#ifdef _WIN32
    client_info->set_os("windows");
    client_info->set_os_version("10.0");
#else
    client_info->set_os("unknown");
    client_info->set_os_version("unknown");
#endif
    client_info->set_device_model("PC");

    if (publisher_offer_sdp) {
        auto* desc = join_req.mutable_publisher_offer();
        desc->ParseFromArray(publisher_offer_sdp->data(), static_cast<int>(publisher_offer_sdp->size()));
    }

    std::vector<uint8_t> join_bytes(join_req.ByteSizeLong());
    join_req.SerializeToArray(join_bytes.data(), static_cast<int>(join_bytes.size()));

    std::vector<uint8_t> compressed_bytes = join_bytes;
    int compression_type = 0; 
    if (publisher_offer_sdp) {
        try {
            compressed_bytes = GzipCompress(join_bytes);
            compression_type = 1; 
        } catch(...) {
            compression_type = 0;
        }
    }

    livekit::proto::WrappedJoinRequest wrapped;
    wrapped.set_join_request(compressed_bytes.data(), compressed_bytes.size());
    wrapped.set_compression(static_cast<livekit::proto::WrappedJoinRequest_Compression>(compression_type));

    std::vector<uint8_t> wrapped_bytes(wrapped.ByteSizeLong());
    wrapped.SerializeToArray(wrapped_bytes.data(), static_cast<int>(wrapped_bytes.size()));

    return Base64UrlEncode(wrapped_bytes);
}

static std::string GetLivekitUrl(const std::string& base_url,
                                 const std::string& token,
                                 const SignalOptions& options,
                                 bool use_v1_path,
                                 bool reconnect,
                                 const std::string& participant_sid,
                                 const std::optional<std::vector<uint8_t>>& publisher_offer_sdp) {
    Url url = ParseUrl(base_url);
    std::string scheme = url.scheme;
    if (scheme == "https") scheme = "wss";
    else if (scheme == "http") scheme = "ws";
    
    std::string full_path = url.path;
    if (!full_path.empty() && full_path.back() == '/') {
        full_path.pop_back();
    }
    full_path += "/rtc";
    if (use_v1_path) {
        full_path += "/v1";
    }

    std::string query;
    if (!token.empty()) {
        query = "access_token=" + UrlEncode(token);
    }

    if (use_v1_path) {
        std::string join_req = CreateJoinRequestParam(options, reconnect, participant_sid, publisher_offer_sdp);
        if (!query.empty()) query += "&";
        query += "join_request=" + UrlEncode(join_req);
    } else {
        std::stringstream ss;
        if (!query.empty()) ss << "&";
        ss << "sdk=cpp"
           << "&os=windows"
           << "&os_version=10.0"
           << "&device_model=PC"
           << "&protocol=14"
           << "&client_protocol=1"
           << "&auto_subscribe=" << (options.auto_subscribe ? "1" : "0")
           << "&adaptive_stream=" << (options.adaptive_stream ? "1" : "0")
           << "&capabilities=CapPacketTrailer";
        if (options.sdk_options.sdk_version) {
            ss << "&version=" << *options.sdk_options.sdk_version;
        }
        if (reconnect) {
            ss << "&reconnect=1"
               << "&sid=" << participant_sid;
        }
        query += ss.str();
    }

    std::string result = scheme + "://" + url.host;
    if (url.port != "80" && url.port != "443" && (url.port != "80" || scheme != "ws") && (url.port != "443" || scheme != "wss")) {
        result += ":" + url.port;
    }
    result += full_path + "?" + query;
    return result;
}

asio::awaitable<ConnectResult> SignalClient::Connect(
                    const std::string& url_str,
                    const std::string& token,
                    const SignalOptions& options,
                    const std::optional<std::vector<uint8_t>>& publisher_offer_sdp,
                    SignalEventHandler event_handler) {
    std::cout << "SignalClient::Connect: Creating SignalClient instance" << std::endl;
    auto executor = co_await asio::this_coro::executor;
    auto client = std::make_shared<SignalClient>(url_str, token, options, options.single_peer_connection, nullptr, event_handler, executor);
    
    client->ssl_ctx_ = std::make_unique<asio::ssl::context>(asio::ssl::context::tls_client);
    client->ssl_ctx_->set_default_verify_paths();
    client->ssl_ctx_->set_verify_mode(asio::ssl::verify_none);
    
    try {
        std::cout << "SignalClient::Connect: executing ConnectInternal" << std::endl;
        std::shared_ptr<proto::JoinResponse> join_res = nullptr;
        std::error_code ec;
        try {
            join_res = co_await client->ConnectInternal(publisher_offer_sdp);
        } catch (const std::system_error& e) {
            ec = e.code();
        } catch (const std::exception&) {
            ec = std::make_error_code(std::errc::connection_aborted);
        } catch (...) {
            ec = std::make_error_code(std::errc::connection_aborted);
        }

        std::cout << "SignalClient::Connect: ConnectInternal completed, ec=" << ec.message() << std::endl;
        
        if (ec) {
            client->Close();
            co_return ConnectResult{nullptr, nullptr, ec};
        }
        
        client->join_response_ = join_res;
        client->StartHeartbeat();
        client->SetEventReady();
        co_return ConnectResult{client, join_res, {}};
    } catch (...) {
        std::cout << "SignalClient::Connect: Unknown exception" << std::endl;
        client->Close();
        co_return ConnectResult{nullptr, nullptr, std::make_error_code(std::errc::connection_aborted)};
    }
}

SignalClient::SignalClient(const std::string& url_str,
                           const std::string& token,
                           const SignalOptions& options,
                           bool single_pc_mode_active,
                           std::shared_ptr<proto::JoinResponse> join_response,
                           SignalEventHandler event_handler,
                           asio::any_io_executor executor)
    : url_(url_str), options_(options),
      single_pc_mode_active_(single_pc_mode_active),
      join_response_(join_response), event_handler_(event_handler),
      executor_(executor), token_(token) {
    last_received_time_.store(std::chrono::steady_clock::now());
}

SignalClient::~SignalClient() {
    Close();
}

asio::awaitable<RestartResult> SignalClient::Restart() {
    reconnecting_.store(true, std::memory_order_release);
    event_ready_.store(false, std::memory_order_release);
    StopHeartbeat();
    
    {
        std::unique_lock<std::shared_mutex> lock(stream_mutex_);
        if (stream_) {
            co_await stream_->Close(false);
            stream_.reset();
        }
    }
    
    try {
        auto reconnect_res = co_await ReconnectInternal();
        co_return RestartResult{reconnect_res, {}};
    } catch (const std::system_error& e) {
        reconnecting_.store(false, std::memory_order_release);
        co_return RestartResult{nullptr, e.code()};
    } catch (const std::exception&) {
        reconnecting_.store(false, std::memory_order_release);
        co_return RestartResult{nullptr, std::make_error_code(std::errc::connection_aborted)};
    } catch (...) {
        reconnecting_.store(false, std::memory_order_release);
        co_return RestartResult{nullptr, std::make_error_code(std::errc::connection_aborted)};
    }
}

void SignalClient::SetReconnected() {
    reconnecting_.store(false, std::memory_order_release);
    FlushQueue();
}

void SignalClient::SetEventReady() {
    std::vector<std::shared_ptr<proto::SignalResponse>> pending;
    {
        std::lock_guard<std::mutex> lock(event_buffer_mutex_);
        event_ready_.store(true, std::memory_order_release);
        pending.swap(buffered_events_);
    }

    for (const auto& msg : pending) {
        SignalEvent event;
        event.type = SignalEvent::Message;
        event.message = msg;
        if (event_handler_) {
            event_handler_(event);
        }
    }
}

static bool is_pass_through(const proto::SignalRequest& req) {
    return req.has_sync_state() ||
           req.has_trickle() ||
           req.has_offer() ||
           req.has_answer() ||
           req.has_simulate() ||
           req.has_leave() ||
           req.has_ping_req();
}

void SignalClient::Send(const proto::SignalRequest& req) {
    bool pass = is_pass_through(req);
    bool reconnecting = reconnecting_.load(std::memory_order_acquire);

    if (reconnecting && !pass) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queued_requests_.push_back(req);
        return;
    }

    if (!reconnecting) {
        FlushQueue();
    }

    std::shared_ptr<SignalStream> stream;
    {
        std::shared_lock<std::shared_mutex> lock(stream_mutex_);
        stream = stream_;
    }

    if (stream && stream->IsConnected()) {
        auto self = shared_from_this();
        livekit::safe_co_spawn(executor_, [self, this, stream, req, pass]() -> asio::awaitable<void> {
            try {
                co_await stream->Send(req);
            } catch (...) {
                if (!pass) {
                    std::lock_guard<std::mutex> lock(queue_mutex_);
                    queued_requests_.push_back(req);
                }
            }
        });
    } else if (!pass) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queued_requests_.push_back(req);
    }
}

void SignalClient::FlushQueue() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (queued_requests_.empty()) return;

    std::shared_ptr<SignalStream> stream;
    {
        std::shared_lock<std::shared_mutex> lock(stream_mutex_);
        stream = stream_;
    }

    if (stream && stream->IsConnected()) {
        for (const auto& req : queued_requests_) {
            livekit::safe_co_spawn(executor_, [stream, req]() -> asio::awaitable<void> {
                try {
                    co_await stream->Send(req);
                } catch(...) {}
            });
        }
        queued_requests_.clear();
    }
}

void SignalClient::Close() {
    StopHeartbeat();
    
    std::shared_ptr<SignalStream> stream;
    {
        std::unique_lock<std::shared_mutex> lock(stream_mutex_);
        stream = stream_;
        stream_.reset();
    }
    
    stream.reset();
}

bool SignalClient::is_connected() const {
    std::shared_lock<std::shared_mutex> lock(stream_mutex_);
    return stream_ && stream_->IsConnected();
}

std::string SignalClient::token() const {
    std::lock_guard<std::mutex> lock(token_mutex_);
    return token_;
}

void SignalClient::StartHeartbeat() {
    StopHeartbeat();
    heartbeat_active_ = true;
    last_received_time_ = std::chrono::steady_clock::now();

    uint32_t interval_sec = join_response_->ping_interval();
    uint32_t timeout_sec = join_response_->ping_timeout();
    if (interval_sec == 0) interval_sec = 20; 
    if (timeout_sec == 0) timeout_sec = 29;

    auto self = shared_from_this();
    livekit::safe_co_spawn(executor_, [self, this, interval_sec, timeout_sec]() -> asio::awaitable<void> {
        co_await HeartbeatLoop(interval_sec, timeout_sec);
    });
}

void SignalClient::StopHeartbeat() {
    heartbeat_active_ = false;
    if (heartbeat_timer_) {
        std::error_code ec;
        heartbeat_timer_->cancel(ec);
    }
}

void SignalClient::HandleIncomingMessage(std::shared_ptr<proto::SignalResponse> msg) {
    last_received_time_.store(std::chrono::steady_clock::now(), std::memory_order_release);
    
    if (msg->has_refresh_token()) {
        std::lock_guard<std::mutex> lock(token_mutex_);
        token_ = msg->refresh_token();
    } else if (msg->has_pong_resp()) {
        int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        int64_t rtt = now_ms - msg->pong_resp().last_ping_timestamp();
        last_rtt_.store(rtt, std::memory_order_release);
    }

    if (!event_ready_.load(std::memory_order_acquire)) {
        if (!msg->has_refresh_token() && !msg->has_pong_resp()) {
            std::lock_guard<std::mutex> lock(event_buffer_mutex_);
            buffered_events_.push_back(msg);
            return;
        }
    }

    SignalEvent event;
    event.type = SignalEvent::Message;
    event.message = msg;
    if (event_handler_) {
        event_handler_(event);
    }
}

void SignalClient::HandleClose(const std::string& reason) {
    StopHeartbeat();
    
    SignalEvent event;
    event.type = SignalEvent::Close;
    event.close_reason = reason;
    if (event_handler_) {
        event_handler_(event);
    }
}

asio::awaitable<std::shared_ptr<proto::JoinResponse>> SignalClient::ConnectInternal(
    const std::optional<std::vector<uint8_t>>& publisher_offer_sdp) {
    bool try_v1 = options_.single_peer_connection;
    single_pc_mode_active_ = try_v1;
    std::string lk_url = GetLivekitUrl(url_, token_, options_, try_v1, false, "", publisher_offer_sdp);
    
    std::shared_ptr<proto::JoinResponse> join_res = nullptr;
    std::optional<std::error_code> first_err;
    
    try {
        join_res = co_await TryConnectInternal(lk_url);
    } catch (const std::system_error& ec) {
        first_err = ec.code();
    } catch (const std::exception&) {
        first_err = std::make_error_code(std::errc::connection_aborted);
    } catch (...) {
        first_err = std::make_error_code(std::errc::connection_aborted);
    }
    
    if (first_err) {
        if (try_v1) {
            single_pc_mode_active_ = false;
            std::string validate_url = GetLivekitUrl(url_, token_, options_, true, false, "", publisher_offer_sdp);
            
            // Validate (we can swallow exception)
            try {
                co_await ValidateInternal(validate_url, token_);
            } catch(...) {
            }
            
            std::string lk_url_v0 = GetLivekitUrl(url_, token_, options_, false, false, "", std::nullopt);
            std::optional<std::error_code> v0_err;
            try {
                join_res = co_await TryConnectInternal(lk_url_v0);
            } catch (const std::system_error& ec2) {
                v0_err = ec2.code();
            } catch (const std::exception&) {
                v0_err = std::make_error_code(std::errc::connection_aborted);
            } catch (...) {
                v0_err = std::make_error_code(std::errc::connection_aborted);
            }
            
            if (v0_err) {
                join_res = co_await FallbackRegionsInternal(*v0_err, publisher_offer_sdp);
            }
        } else {
            join_res = co_await FallbackRegionsInternal(*first_err, publisher_offer_sdp);
        }
    }
    
    co_return join_res;
}

asio::awaitable<std::shared_ptr<proto::ReconnectResponse>> SignalClient::ReconnectInternal() {
    std::string sid = join_response_->participant().sid();
    std::string tok = this->token();
    std::string reconnect_url = GetLivekitUrl(url_, tok, options_, single_pc_mode_active_, true, sid, std::nullopt);
    
    auto connect_res = co_await SignalStream::Connect(*ssl_ctx_, reconnect_url, tok, options_.connect_timeout);
    if (connect_res.error) {
        throw std::system_error(connect_res.error);
    }
    auto stream = connect_res.stream;
    
    auto executor = co_await asio::this_coro::executor;
    auto timer = std::make_shared<asio::steady_timer>(executor);
    timer->expires_after(options_.reconnect_timeout);
    
    std::shared_ptr<proto::ReconnectResponse> rec_res = nullptr;
    std::string close_reason;
    bool got_leave = false;
    
    stream->SetOnMessage([this, timer, &rec_res, &got_leave](std::shared_ptr<proto::SignalResponse> msg) {
        if (msg->has_reconnect()) {
            rec_res = std::make_shared<proto::ReconnectResponse>(msg->reconnect());
            auto n = timer->cancel();
            std::cout << "SignalClient::ReconnectInternal: timer->cancel() returned: " << n << std::endl;
        } else if (msg->has_leave()) {
            got_leave = true;
            auto n = timer->cancel();
            std::cout << "SignalClient::ReconnectInternal: leave timer->cancel() returned: " << n << std::endl;
        } else {
            if (!event_ready_.load(std::memory_order_acquire)) {
                if (!msg->has_refresh_token() && !msg->has_pong_resp()) {
                    std::lock_guard<std::mutex> lock(event_buffer_mutex_);
                    buffered_events_.push_back(msg);
                }
            }
        }
    });
    
    stream->SetOnClose([timer, &close_reason](const std::string& reason) {
        close_reason = reason;
        auto n = timer->cancel();
        std::cout << "SignalClient::ReconnectInternal: SetOnClose timer->cancel() returned: " << n << std::endl;
    });
    
    stream->StartRead();
    
    try {
        co_await timer->async_wait(asio::use_awaitable);
        stream->Close(false);
        throw std::system_error(std::make_error_code(std::errc::timed_out));
    } catch (const std::system_error& e) {
        if (e.code() == asio::error::operation_aborted) {
            if (rec_res) {
                {
                    std::unique_lock<std::shared_mutex> lock(stream_mutex_);
                    stream_ = stream;
                }
                stream_->SetOnMessage([c = weak_from_this()](std::shared_ptr<proto::SignalResponse> msg) {
                    if (auto client_ptr = c.lock()) {
                        client_ptr->HandleIncomingMessage(msg);
                    }
                });
                stream_->SetOnClose([c = weak_from_this()](const std::string& reason) {
                    if (auto client_ptr = c.lock()) {
                        client_ptr->HandleClose(reason);
                    }
                });
                
                StartHeartbeat();
                co_return rec_res;
            } else {
                stream->Close(false);
                throw std::system_error(std::make_error_code(std::errc::connection_aborted), got_leave ? "leave" : close_reason);
            }
        } else {
            throw;
        }
    }
}

asio::awaitable<std::shared_ptr<proto::JoinResponse>> SignalClient::TryConnectInternal(const std::string& connect_url) {
    std::cout << "SignalClient::TryConnectInternal: Connect URL: " << connect_url << std::endl;
    auto connect_res = co_await SignalStream::Connect(*ssl_ctx_, connect_url, token_, options_.connect_timeout);
    if (connect_res.error) {
        throw std::system_error(connect_res.error);
    }
    auto stream = connect_res.stream;
    std::cout << "SignalClient::TryConnectInternal: Connected stream, waiting for join" << std::endl;
    
    auto executor = co_await asio::this_coro::executor;
    auto timer = std::make_shared<asio::steady_timer>(executor);
    timer->expires_after(options_.connect_timeout);
    std::cout << "SignalClient::TryConnectInternal: Created timer address: " << timer.get() << std::endl;
    
    std::shared_ptr<proto::JoinResponse> join_res = nullptr;
    std::string close_reason;
    
    stream->SetOnMessage([this, timer, &join_res](std::shared_ptr<proto::SignalResponse> msg) {
        std::cout << "SignalClient::TryConnectInternal: SetOnMessage callback, has_join=" << msg->has_join() << " timer address: " << timer.get() << std::endl;
        if (msg->has_join()) {
            join_res = std::make_shared<proto::JoinResponse>(msg->join());
            auto n = timer->cancel();
            std::cout << "SignalClient::TryConnectInternal: timer->cancel() returned: " << n << std::endl;
        } else {
            if (!event_ready_.load(std::memory_order_acquire)) {
                if (!msg->has_refresh_token() && !msg->has_pong_resp()) {
                    std::lock_guard<std::mutex> lock(event_buffer_mutex_);
                    buffered_events_.push_back(msg);
                }
            }
        }
    });
    
    stream->SetOnClose([timer, &close_reason](const std::string& reason) {
        close_reason = reason;
        auto n = timer->cancel();
        std::cout << "SignalClient::TryConnectInternal: SetOnClose timer->cancel() returned: " << n << " timer address: " << timer.get() << std::endl;
    });
    
    stream->StartRead();
    
    try {
        std::cout << "SignalClient::TryConnectInternal: co_awaiting timer address: " << timer.get() << std::endl;
        co_await timer->async_wait(asio::use_awaitable);
        std::cout << "SignalClient::TryConnectInternal: timer wait ended normally (no exception)" << std::endl;
        stream->Close(false);
        throw std::system_error(std::make_error_code(std::errc::timed_out));
    } catch (const std::system_error& e) {
        std::cout << "SignalClient::TryConnectInternal: caught system_error: " << e.code().value() << " (" << e.code().message() << ")" << std::endl;
        if (e.code() == asio::error::operation_aborted) {
            if (join_res) {
                {
                    std::unique_lock<std::shared_mutex> lock(stream_mutex_);
                    stream_ = stream;
                }
                stream_->SetOnMessage([c = weak_from_this()](std::shared_ptr<proto::SignalResponse> msg) {
                    if (auto client_ptr = c.lock()) {
                        client_ptr->HandleIncomingMessage(msg);
                    }
                });
                stream_->SetOnClose([c = weak_from_this()](const std::string& reason) {
                    if (auto client_ptr = c.lock()) {
                        client_ptr->HandleClose(reason);
                    }
                });
                co_return join_res;
            } else {
                throw std::system_error(std::make_error_code(std::errc::connection_aborted), close_reason);
            }
        } else {
            throw;
        }
    }
}

asio::awaitable<std::shared_ptr<proto::JoinResponse>> SignalClient::FallbackRegionsInternal(
    const std::error_code& last_error,
    const std::optional<std::vector<uint8_t>>& publisher_offer_sdp) {
    
    std::vector<std::string> fallback_urls = co_await RegionUrlProvider::FetchRegionUrls(*ssl_ctx_, url_, token_);
    if (fallback_urls.empty()) {
        throw std::system_error(last_error);
    }
    
    for (size_t i = 0; i < fallback_urls.size(); ++i) {
        std::string lk_url = GetLivekitUrl(fallback_urls[i], token_, options_, options_.single_peer_connection, false, "", publisher_offer_sdp);
        try {
            co_return co_await TryConnectInternal(lk_url);
        } catch (...) {
            if (i == fallback_urls.size() - 1) {
                throw;
            }
        }
    }
    throw std::system_error(last_error);
}

asio::awaitable<void> SignalClient::ValidateInternal(const std::string& url_str, const std::string& token) {
    Url url = ParseUrl(url_str);
    std::string scheme = (url.scheme == "wss" || url.scheme == "https") ? "https" : "http";
    std::string val_path = url.path;
    if (!val_path.empty() && val_path.back() == '/') val_path.pop_back();
    val_path += "/validate";
    
    std::string val_url = scheme + "://" + url.host;
    if (url.port != "80" && url.port != "443" && (url.port != "80" || scheme != "http") && (url.port != "443" || scheme != "https")) {
        val_url += ":" + url.port;
    }
    val_url += val_path;
    if (!url.query.empty()) {
        val_url += "?" + url.query;
    }

    HttpResponse res = co_await HttpClient::Get(*ssl_ctx_, val_url, token, std::chrono::seconds(3));
    if (res.status_code >= 400) {
        throw std::system_error(std::make_error_code(std::errc::permission_denied));
    }
}

asio::awaitable<void> SignalClient::HeartbeatLoop(uint32_t interval_sec, uint32_t timeout_sec) {
    auto executor = co_await asio::this_coro::executor;
    heartbeat_timer_ = std::make_shared<asio::steady_timer>(executor);
    
    try {
        while (heartbeat_active_) {
            heartbeat_timer_->expires_after(std::chrono::seconds(interval_sec));
            co_await heartbeat_timer_->async_wait(asio::use_awaitable);
            
            if (!heartbeat_active_) break;
            
            int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            
            proto::SignalRequest req;
            auto* ping = req.mutable_ping_req();
            ping->set_timestamp(now_ms);
            ping->set_rtt(last_rtt_.load(std::memory_order_acquire));
            
            Send(req);

            auto last_recv = last_received_time_.load(std::memory_order_acquire);
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - last_recv).count();
            
            if (elapsed > static_cast<int64_t>(timeout_sec)) {
                HandleClose("ping timeout");
                break;
            }
        }
    } catch (const std::system_error& e) {
        if (e.code() != asio::error::operation_aborted) {
            HandleClose("heartbeat timer error: " + e.code().message());
        }
    }
}

} // namespace livekit
