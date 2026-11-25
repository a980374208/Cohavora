#include "src/net/user_event_stream_client.h"

#include "src/core/session_shutdown_service.h"
#include "src/net/session_manager.h"
#include "src/signal/websocket_client.h"
#include "src/telemetry/log_redaction.h"
#include "src/telemetry/safe_spawn.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QMetaObject>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>

#include <asio.hpp>
#include <asio/ssl.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <random>
#include <thread>

namespace OpenMeeting {
namespace {

constexpr uint16_t kWsCloseCodeSessionKicked = 4001;
constexpr auto kWsConnectTimeout = std::chrono::milliseconds(10000);
constexpr auto kHeartbeatInterval = std::chrono::seconds(20);
constexpr int kInitialBackoffMs = 500;
constexpr int kMaxBackoffMs = 30000;

qint64 readFrameInt64(const QJsonObject &obj, const QString &key) {
    const QJsonValue v = obj.value(key);
    if (v.isUndefined() || v.isNull()) return 0;
    if (v.isString()) {
        bool ok = false;
        const qint64 n = v.toString().toLongLong(&ok);
        return ok ? n : 0;
    }
    return static_cast<qint64>(v.toDouble(0.0));
}

} // namespace

std::optional<ParsedEventFrame> parseEventStreamFrame(const QString &jsonText) {
    const QJsonDocument doc = QJsonDocument::fromJson(jsonText.toUtf8());
    if (!doc.isObject()) return std::nullopt;
    const QJsonObject root = doc.object();
    const QString type = root.value(QStringLiteral("type")).toString().trimmed();
    if (type.isEmpty()) return std::nullopt;
    ParsedEventFrame frame;
    frame.type = type;
    frame.seq = readFrameInt64(root, QStringLiteral("seq"));
    frame.timestampMs = readFrameInt64(root, QStringLiteral("timestampMs"));
    if (root.value(QStringLiteral("payload")).isObject()) {
        frame.payload = root.value(QStringLiteral("payload")).toObject();
    }
    return frame;
}

EventStreamHello parseEventStreamHelloPayload(const QJsonObject &payload) {
    EventStreamHello hello;
    hello.serverTimeMs = readFrameInt64(payload, QStringLiteral("serverTimeMs"));
    hello.latestSeq = readFrameInt64(payload, QStringLiteral("latestSeq"));
    hello.syncResetRequired = payload.value(QStringLiteral("syncResetRequired")).toBool(false);
    for (const auto &v : payload.value(QStringLiteral("missedEvents")).toArray()) {
        if (v.isObject()) hello.missedEvents.push_back(v.toObject());
    }
    if (payload.value(QStringLiteral("activeIncomingCall")).isObject()) {
        hello.activeIncomingCall = parseCallSession(payload.value(QStringLiteral("activeIncomingCall")).toObject());
    }
    if (payload.value(QStringLiteral("selfPresence")).isObject()) {
        hello.selfPresence = parseUserPresence(payload.value(QStringLiteral("selfPresence")).toObject());
    }
    return hello;
}

QString buildUserEventsWebSocketUrl(const QString &httpBaseUrl, qint64 sinceSeq) {
    QUrl url(httpBaseUrl.trimmed());
    if (!url.isValid() || url.host().isEmpty()) return {};
    if (url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0) {
        url.setScheme(QStringLiteral("wss"));
    } else if (url.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) == 0) {
        url.setScheme(QStringLiteral("ws"));
    } else if (url.scheme().compare(QStringLiteral("ws"), Qt::CaseInsensitive) != 0 &&
               url.scheme().compare(QStringLiteral("wss"), Qt::CaseInsensitive) != 0) {
        return {};
    }
    QString path = url.path();
    while (path.endsWith(QLatin1Char('/'))) path.chop(1);
    url.setPath(path + QStringLiteral("/ws/events"));
    QUrlQuery query;
    if (sinceSeq > 0) {
        query.addQueryItem(QStringLiteral("since_seq"), QString::number(sinceSeq));
    }
    url.setQuery(query);
    return url.toString(QUrl::FullyEncoded);
}

EventStreamAuthFailureKind classifyEventStreamAuthFailure(
        unsigned int httpStatus, const std::string &handshakeDetail) {
    if (httpStatus != 401 && httpStatus != 403) {
        return EventStreamAuthFailureKind::None;
    }
    std::string lower = handshakeDetail;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (lower.find("duplicated_login") != std::string::npos ||
        lower.find("session_kicked") != std::string::npos ||
        lower.find("kickoffmeeting") != std::string::npos) {
        return EventStreamAuthFailureKind::DuplicatedLogin;
    }
    return EventStreamAuthFailureKind::TokenInvalid;
}

struct UserEventStreamClient::TransportImpl : public std::enable_shared_from_this<TransportImpl> {
    explicit TransportImpl(std::shared_ptr<QtCallbackGate<UserEventStreamClient>> gate)
        : uiGate(std::move(gate))
        , ioCtx(1)
        , sslCtx(asio::ssl::context::tls_client)
        , strand(asio::make_strand(ioCtx))
        , reconnectTimer(ioCtx)
        , heartbeatTimer(ioCtx) {
        sslCtx.set_default_verify_paths();
        sslCtx.set_verify_mode(asio::ssl::verify_peer);
    }

    ~TransportImpl() {
        shutdownThread();
    }

    void ensureThreadStarted() {
        if (threadStarted.exchange(true)) return;
        workGuard.emplace(asio::make_work_guard(ioCtx));
        worker = std::thread([this]() {
            try {
                ioCtx.run();
            } catch (...) {
                spdlog::warn("[EventStream] io thread exited {}",
                             livekit::secure_log::ExceptionSummary("event_stream_io"));
            }
        });
    }

    void shutdownThread() {
        stopping.store(true, std::memory_order_release);
        ++connectionSerial;
        if (threadStarted.load()) {
            asio::post(strand, [this]() {
                reconnectTimer.cancel();
                heartbeatTimer.cancel();
                if (ws) {
                    ws->Abort();
                    ws.reset();
                }
            });
            workGuard.reset();
            ioCtx.stop();
            if (worker.joinable()) worker.join();
            threadStarted.store(false);
        }
    }

    void startConnection(quint64 authGen, std::string httpBaseUrl, std::string jwtToken,
                         qint64 initialSeq, bool allowInsecure, std::string manualStatus) {
        ensureThreadStarted();
        auto self = shared_from_this();
        const uint64_t serial = ++connectionSerial;
        asio::post(strand, [self, serial, authGen, httpBaseUrl = std::move(httpBaseUrl),
                            jwtToken = std::move(jwtToken), initialSeq, allowInsecure,
                            manualStatus = std::move(manualStatus)]() mutable {
            if (self->stopping.load(std::memory_order_acquire) || serial != self->connectionSerial.load()) return;
            self->reconnectTimer.cancel();
            self->heartbeatTimer.cancel();
            if (self->ws) {
                self->ws->Abort();
                self->ws.reset();
            }
            self->activeAuthGen = authGen;
            self->baseUrl = std::move(httpBaseUrl);
            self->token = std::move(jwtToken);
            self->sinceSeq = initialSeq;
            self->allowInsecureWs = allowInsecure;
            self->manualStatusWire = std::move(manualStatus);
            self->backoffMs = kInitialBackoffMs;
            self->spawnConnectAttempt(serial);
        });
    }

    void stopConnection() {
        const uint64_t serial = ++connectionSerial;
        if (!threadStarted.load()) return;
        auto self = shared_from_this();
        asio::post(strand, [self, serial]() {
            if (serial != self->connectionSerial.load()) return;
            self->reconnectTimer.cancel();
            self->heartbeatTimer.cancel();
            if (self->ws) {
                self->ws->Abort();
                self->ws.reset();
            }
            self->token.clear();
            self->postState(EventStreamState::Disconnected);
        });
    }

    void updateSinceSeq(qint64 seq) {
        if (!threadStarted.load()) return;
        auto self = shared_from_this();
        asio::post(strand, [self, seq]() {
            if (seq > self->sinceSeq) self->sinceSeq = seq;
        });
    }

    void updateManualStatus(std::string wire) {
        if (!threadStarted.load()) return;
        auto self = shared_from_this();
        asio::post(strand, [self, wire = std::move(wire)]() mutable {
            self->manualStatusWire = std::move(wire);
        });
    }

    void sendText(std::string payload) {
        if (!threadStarted.load()) return;
        auto self = shared_from_this();
        asio::post(strand, [self, payload = std::move(payload)]() mutable {
            if (!self->ws || !self->ws->IsConnected()) return;
            auto wsRef = self->ws;
            livekit::safe_co_spawn(self->strand, [wsRef, payload = std::move(payload)]() -> asio::awaitable<void> {
                try {
                    co_await wsRef->SendText(payload);
                } catch (...) {}
            });
        });
    }

    void postState(EventStreamState state) {
        uiGate->Post([state](UserEventStreamClient *owner) {
            owner->setState(state);
        });
    }

    void spawnConnectAttempt(uint64_t serial) {
        if (stopping.load(std::memory_order_acquire) || serial != connectionSerial.load()) return;
        const QString wsUrlQ = buildUserEventsWebSocketUrl(QString::fromStdString(baseUrl), sinceSeq);
        if (wsUrlQ.isEmpty() || token.empty()) {
            postState(EventStreamState::Disconnected);
            return;
        }
        const std::string wsUrl = wsUrlQ.toStdString();
        postState(backoffMs > kInitialBackoffMs ? EventStreamState::Reconnecting : EventStreamState::Connecting);

        auto self = shared_from_this();
        livekit::safe_co_spawn(strand, [self, serial, wsUrl]() -> asio::awaitable<void> {
            if (self->stopping.load(std::memory_order_acquire) || serial != self->connectionSerial.load()) {
                co_return;
            }
            auto client = std::make_shared<livekit::WebSocketClient>(self->ioCtx, self->sslCtx);
            self->ws = client;

            const quint64 authGen = self->activeAuthGen;
            client->SetOnTextMessage([self, serial, authGen](const std::string &msg) {
                if (serial != self->connectionSerial.load()) return;
                const QString text = QString::fromUtf8(msg.data(), static_cast<int>(msg.size()));
                self->uiGate->Post([text, authGen](UserEventStreamClient *owner) {
                    owner->dispatchFrameOnUiThread(text, authGen);
                });
            });
            client->SetOnClose([self, serial, authGen](uint16_t code, const std::string &) {
                asio::post(self->strand, [self, serial, authGen, code]() {
                    if (serial != self->connectionSerial.load()) return;
                    self->heartbeatTimer.cancel();
                    if (code == kWsCloseCodeSessionKicked) {
                        self->uiGate->Post([authGen](UserEventStreamClient *owner) {
                            if (owner->_session.authGeneration() == authGen) {
                                emit owner->sessionKicked(QStringLiteral("duplicated_login"));
                            }
                        });
                        self->postState(EventStreamState::Disconnected);
                        return;
                    }
                    self->scheduleReconnect(serial);
                });
            });
            client->SetOnError([self, serial](const std::error_code &ec) {
                asio::post(self->strand, [self, serial, ec]() {
                    if (serial != self->connectionSerial.load()) return;
                    self->heartbeatTimer.cancel();
                    spdlog::warn("[EventStream] ws error {}",
                                 livekit::secure_log::ErrorCodeSummary("ws_read", ec.value(), ec.category().name()));
                    self->scheduleReconnect(serial);
                });
            });

            livekit::CredentialUrlPolicy policy;
            policy.allow_insecure = self->allowInsecureWs;
            const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            livekit::WebSocketClient::HeaderList headers;
            headers.emplace_back("token", self->token);
            headers.emplace_back("operationID", "ws-" + std::to_string(nowMs) + "-" + std::to_string(serial));

            // Pass empty LiveKit access_token so token travels ONLY via Header (never URL query).
            const auto ec = co_await client->Connect(wsUrl, std::string(), kWsConnectTimeout, policy, std::move(headers));
            if (self->stopping.load(std::memory_order_acquire) || serial != self->connectionSerial.load()) {
                client->Abort();
                co_return;
            }
            if (ec) {
                const auto httpStatus = livekit::WebSocketHttpStatus(ec);
                spdlog::warn("[EventStream] connect failed {}",
                             livekit::secure_log::ErrorCodeSummary("ws_connect", httpStatus.value_or(ec.value()), "ws"));
                if (httpStatus && (*httpStatus == 401 || *httpStatus == 403)) {
                    const auto failureKind = classifyEventStreamAuthFailure(
                        *httpStatus, client->LastHandshakeErrorDetail());
                    self->uiGate->Post([authGen, failureKind](UserEventStreamClient *owner) {
                        if (owner->_session.authGeneration() != authGen) return;
                        if (failureKind == EventStreamAuthFailureKind::DuplicatedLogin) {
                            emit owner->sessionKicked(QStringLiteral("duplicated_login"));
                        } else if (failureKind == EventStreamAuthFailureKind::TokenInvalid) {
                            emit owner->sessionAuthRejected(QStringLiteral("token_invalid"));
                        }
                    });
                    self->postState(EventStreamState::Disconnected);
                    co_return;
                }
                self->scheduleReconnect(serial);
                co_return;
            }

            self->backoffMs = kInitialBackoffMs;
            self->postState(EventStreamState::Connected);
            client->StartRead();
            self->startHeartbeat(serial);
        });
    }

    void startHeartbeat(uint64_t serial) {
        if (stopping.load(std::memory_order_acquire) || serial != connectionSerial.load()) return;
        heartbeatTimer.expires_after(kHeartbeatInterval);
        auto self = shared_from_this();
        heartbeatTimer.async_wait(asio::bind_executor(strand, [self, serial](const std::error_code &ec) {
            if (ec || self->stopping.load(std::memory_order_acquire) || serial != self->connectionSerial.load()) return;
            if (!self->ws || !self->ws->IsConnected()) return;
            const std::string pingJson = "{\"type\":\"ping\",\"manualStatus\":\"" + self->manualStatusWire + "\"}";
            auto wsRef = self->ws;
            livekit::safe_co_spawn(self->strand, [wsRef, pingJson]() -> asio::awaitable<void> {
                try {
                    co_await wsRef->SendText(pingJson);
                } catch (...) {}
            });
            self->startHeartbeat(serial);
        }));
    }

    void scheduleReconnect(uint64_t serial) {
        if (stopping.load(std::memory_order_acquire) || serial != connectionSerial.load()) return;
        if (ws) {
            ws->Abort();
            ws.reset();
        }
        postState(EventStreamState::Reconnecting);
        static thread_local std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution<int> jitter(0, std::max(100, backoffMs / 4));
        const int delayMs = std::min(kMaxBackoffMs, backoffMs + jitter(rng));
        backoffMs = std::min(kMaxBackoffMs, backoffMs * 2);

        reconnectTimer.expires_after(std::chrono::milliseconds(delayMs));
        auto self = shared_from_this();
        reconnectTimer.async_wait(asio::bind_executor(strand, [self, serial](const std::error_code &ec) {
            if (ec || self->stopping.load(std::memory_order_acquire) || serial != self->connectionSerial.load()) return;
            self->spawnConnectAttempt(serial);
        }));
    }

    std::shared_ptr<QtCallbackGate<UserEventStreamClient>> uiGate;
    asio::io_context ioCtx;
    asio::ssl::context sslCtx;
    asio::strand<asio::io_context::executor_type> strand;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> workGuard;
    std::thread worker;
    std::atomic<bool> threadStarted{false};
    std::atomic<bool> stopping{false};
    std::atomic<uint64_t> connectionSerial{0};

    asio::steady_timer reconnectTimer;
    asio::steady_timer heartbeatTimer;
    std::shared_ptr<livekit::WebSocketClient> ws;

    quint64 activeAuthGen = 0;
    std::string baseUrl;
    std::string token;
    qint64 sinceSeq = 0;
    bool allowInsecureWs = false;
    std::string manualStatusWire = "online";
    int backoffMs = kInitialBackoffMs;
};

UserEventStreamClient::UserEventStreamClient(SessionManager &session, QObject *parent)
    : QObject(parent)
    , _session(session)
    , _transport(std::make_shared<TransportImpl>(QtCallbackGate<UserEventStreamClient>::Create(this))) {
    qRegisterMetaType<EventStreamState>();
    qRegisterMetaType<EventStreamHello>();
    qRegisterMetaType<std::vector<UserPresence>>();

    connect(&_session, &SessionManager::authenticationReset, this, [this](quint64) {
        handleSessionReset();
    });
    connect(&_session, &SessionManager::loggedOut, this, [this]() {
        handleSessionReset();
    });
    connect(&_session, &SessionManager::sessionInvalidated, this, [this](SessionInvalidationReason) {
        handleSessionReset();
    });
    connect(&_session, &SessionManager::loggedIn, this, [this](const UserInfo &) {
        handleSessionLoggedIn();
    });
}

UserEventStreamClient::~UserEventStreamClient() {
    _transport->uiGate->Revoke();
    _transport->shutdownThread();
}

void UserEventStreamClient::setInitialSinceSeq(qint64 seq) {
    if (seq >= 0) {
        _lastSeq = seq;
        _transport->updateSinceSeq(seq);
    }
}

void UserEventStreamClient::setManualPresenceStatus(PresenceStatus status) {
    _manualStatus = (status == PresenceStatus::Away) ? PresenceStatus::Away : PresenceStatus::Online;
    _transport->updateManualStatus(presenceStatusToWire(_manualStatus).toStdString());
    if (_state == EventStreamState::Connected) {
        QJsonObject ping;
        ping.insert(QStringLiteral("type"), QStringLiteral("ping"));
        ping.insert(QStringLiteral("manualStatus"), presenceStatusToWire(_manualStatus));
        sendJsonFrame(ping);
    }
}

void UserEventStreamClient::start() {
    _started = true;
    if (!_session.isLoggedIn() || _session.token().isEmpty() || _session.serverBaseUrl().isEmpty()) {
        return;
    }
    const auto authGen = _session.authGeneration();
    if (_startedAuthGen == authGen && _state != EventStreamState::Disconnected) {
        return;
    }
    _startedAuthGen = authGen;
    _transport->startConnection(
        authGen,
        _session.serverBaseUrl().toStdString(),
        _session.token().toStdString(),
        _lastSeq,
        _session.isDebugHttp(),
        presenceStatusToWire(_manualStatus).toStdString());
}

void UserEventStreamClient::stop() {
    _started = false;
    _startedAuthGen = 0;
    _transport->stopConnection();
    setState(EventStreamState::Disconnected);
}

void UserEventStreamClient::sendPresenceWatch(
        const std::optional<QStringList> &fixedUserIds,
        const std::optional<QStringList> &viewportUserIds) {
    if (fixedUserIds) {
        _cachedFixedWatch = *fixedUserIds;
        _hasFixedWatch = true;
    }
    if (viewportUserIds) {
        _cachedViewportWatch = *viewportUserIds;
        _hasViewportWatch = true;
    }
    if (_state != EventStreamState::Connected) return;

    QJsonObject frame;
    frame.insert(QStringLiteral("type"), QStringLiteral("presence.watch"));
    if (fixedUserIds) {
        QJsonArray arr;
        for (const auto &id : *fixedUserIds) arr.append(id);
        frame.insert(QStringLiteral("fixedUserIDs"), arr);
    }
    if (viewportUserIds) {
        QJsonArray arr;
        for (const auto &id : *viewportUserIds) arr.append(id);
        frame.insert(QStringLiteral("userIDs"), arr);
    }
    sendJsonFrame(frame);
}

void UserEventStreamClient::ingestRawTextFrameForTesting(const QString &text, quint64 expectedAuthGeneration) {
    dispatchFrameOnUiThread(text, expectedAuthGeneration);
}

void UserEventStreamClient::handleSessionReset() {
    _lastSeq = 0;
    _startedAuthGen = 0;
    _hasFixedWatch = false;
    _hasViewportWatch = false;
    _cachedFixedWatch.clear();
    _cachedViewportWatch.clear();
    _transport->stopConnection();
    setState(EventStreamState::Disconnected);
}

void UserEventStreamClient::handleSessionLoggedIn() {
    if (_started) {
        start();
    }
}

void UserEventStreamClient::setState(EventStreamState next) {
    if (_state == next) return;
    _state = next;
    emit stateChanged(_state);
    if (_state == EventStreamState::Connected && (_hasFixedWatch || _hasViewportWatch)) {
        sendPresenceWatch(
            _hasFixedWatch ? std::optional<QStringList>(_cachedFixedWatch) : std::nullopt,
            _hasViewportWatch ? std::optional<QStringList>(_cachedViewportWatch) : std::nullopt);
    }
}

void UserEventStreamClient::sendJsonFrame(const QJsonObject &frame) {
    const QByteArray bytes = QJsonDocument(frame).toJson(QJsonDocument::Compact);
    _transport->sendText(std::string(bytes.constData(), static_cast<size_t>(bytes.size())));
}

void UserEventStreamClient::dispatchFrameOnUiThread(const QString &text, quint64 authGen) {
    if (!_session.isLoggedIn() || _session.authGeneration() != authGen) {
        return;
    }
    const auto parsed = parseEventStreamFrame(text);
    if (!parsed) return;
    dispatchSingleEvent(*parsed);
}

void UserEventStreamClient::dispatchSingleEvent(const ParsedEventFrame &frame) {
    if (frame.seq > _lastSeq) {
        _lastSeq = frame.seq;
        _transport->updateSinceSeq(_lastSeq);
        emit syncSeqAdvanced(_lastSeq);
    }

    const QString &t = frame.type;
    if (t == QStringLiteral("hello_ok")) {
        const auto hello = parseEventStreamHelloPayload(frame.payload);
        if (hello.latestSeq > _lastSeq) {
            _lastSeq = hello.latestSeq;
            _transport->updateSinceSeq(_lastSeq);
            emit syncSeqAdvanced(_lastSeq);
        }
        emit helloReceived(hello);
        if (hello.syncResetRequired) {
            emit syncResetRequired();
        }
        for (const auto &missedObj : hello.missedEvents) {
            ParsedEventFrame sub;
            sub.type = missedObj.value(QStringLiteral("type")).toString();
            sub.seq = readFrameInt64(missedObj, QStringLiteral("seq"));
            sub.timestampMs = readFrameInt64(missedObj, QStringLiteral("timestampMs"));
            if (missedObj.value(QStringLiteral("payload")).isObject()) {
                sub.payload = missedObj.value(QStringLiteral("payload")).toObject();
            }
            if (!sub.type.isEmpty() && sub.type != QStringLiteral("hello_ok")) {
                dispatchSingleEvent(sub);
            }
        }
        if (hello.selfPresence) {
            emit presenceBatchReceived({*hello.selfPresence});
        }
        if (hello.activeIncomingCall) {
            emit callIncoming(*hello.activeIncomingCall);
        }
        return;
    }
    if (t == QStringLiteral("pong")) {
        if (frame.payload.value(QStringLiteral("selfPresence")).isObject()) {
            emit presenceBatchReceived({parseUserPresence(frame.payload.value(QStringLiteral("selfPresence")).toObject())});
        }
        return;
    }
    if (t == QStringLiteral("session.kicked")) {
        const QString reason = frame.payload.value(QStringLiteral("reason")).toString(QStringLiteral("duplicated_login"));
        emit sessionKicked(reason);
        return;
    }
    if (t == QStringLiteral("contact.relation_updated")) {
        emit contactRelationUpdated(frame.payload);
        return;
    }
    if (t == QStringLiteral("contact.request_received")) {
        emit contactRequestReceived(frame.payload);
        return;
    }
    if (t == QStringLiteral("contact.request_handled")) {
        emit contactRequestHandled(frame.payload);
        return;
    }
    if (t == QStringLiteral("org.tree_updated")) {
        emit orgTreeUpdated(readFrameInt64(frame.payload, QStringLiteral("treeVersion")));
        return;
    }
    if (t == QStringLiteral("meeting.invited")) {
        emit meetingInvited(frame.payload);
        return;
    }
    if (t == QStringLiteral("presence.changed_batch")) {
        std::vector<UserPresence> items;
        for (const auto &v : frame.payload.value(QStringLiteral("items")).toArray()) {
            if (v.isObject()) items.push_back(parseUserPresence(v.toObject()));
        }
        if (!items.empty()) emit presenceBatchReceived(items);
        return;
    }
    if (t == QStringLiteral("call.incoming")) {
        emit callIncoming(parseCallSession(frame.payload));
        return;
    }
    if (t == QStringLiteral("call.accepted")) {
        emit callAccepted(parseCallAcceptedEvent(frame.payload));
        return;
    }
    if (t == QStringLiteral("call.rejected")) {
        emit callRejected(parseCallSession(frame.payload));
        return;
    }
    if (t == QStringLiteral("call.cancelled")) {
        emit callCancelled(parseCallSession(frame.payload));
        return;
    }
    if (t == QStringLiteral("call.timeout")) {
        emit callTimeout(parseCallSession(frame.payload));
        return;
    }
}

} // namespace OpenMeeting
