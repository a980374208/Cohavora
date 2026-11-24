#include "src/agent/room_agent_publisher.h"

#include "src/core/data_stream.h"
#include "src/net/openmeeting_http_client.h"
#include "src/telemetry/log_redaction.h"

#include <QtCore/QCoreApplication>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>

#include <map>

namespace OpenMeeting {

struct RoomAgentPublisher::AgentConnectionHolder {
	std::shared_ptr<asio::io_context> ioContext;
	std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> workGuard;
	std::thread ioThread;
	std::shared_ptr<livekit::Room> room;
	std::shared_ptr<livekit::RoomListener> listener;
};

class RoomAgentPublisher::AgentRoomListener final : public livekit::RoomListener {
public:
	AgentRoomListener(
		std::shared_ptr<QtCallbackGate<RoomAgentPublisher>> gate,
		quint64 opGen)
		: _gate(std::move(gate))
		, _opGen(opGen) {}

	void OnConnected() override {
		if (!_gate || !_gate->active()) return;
		const quint64 gen = _opGen;
		_gate->Post([gen](RoomAgentPublisher *self) {
			if (self->_opGeneration.load(std::memory_order_acquire) != gen) {
				return;
			}
			if (self->_activeAgentIdentity.isEmpty() && self->_connection && self->_connection->room) {
				if (auto local = self->_connection->room->local_participant()) {
					const QString localId = QString::fromStdString(local->identity()).trimmed();
					if (!localId.isEmpty()) {
						self->_activeAgentIdentity = localId;
						if (self->_meetingCoordinator) {
							self->_meetingCoordinator->setIgnoredTranscriptionSenderIdentity(self->_activeAgentIdentity);
						}
					}
				}
			}
			self->setState(RoomAgentRoleState::Active);
			self->setAgentActivityState(QStringLiteral("listening"));
		});
	}

	void OnReconnecting() override {
		if (!_gate || !_gate->active()) return;
		const quint64 gen = _opGen;
		_gate->Post([gen](RoomAgentPublisher *self) {
			if (self->_opGeneration.load(std::memory_order_acquire) != gen) {
				return;
			}
			self->setState(RoomAgentRoleState::Reconnecting);
		});
	}

	void OnReconnected() override {
		if (!_gate || !_gate->active()) return;
		const quint64 gen = _opGen;
		_gate->Post([gen](RoomAgentPublisher *self) {
			if (self->_opGeneration.load(std::memory_order_acquire) != gen) {
				return;
			}
			self->setState(RoomAgentRoleState::Active);
			self->setAgentActivityState(QStringLiteral("listening"));
		});
	}

	void OnDisconnected(livekit::RoomDisconnectReason reason, const std::string &detail) override {
		(void)detail;
		if (!_gate || !_gate->active()) return;
		const quint64 gen = _opGen;
		_gate->Post([gen, reason](RoomAgentPublisher *self) {
			if (self->_opGeneration.load(std::memory_order_acquire) != gen) {
				return;
			}
			const bool removed = (reason == livekit::RoomDisconnectReason::ParticipantRemoved ||
				reason == livekit::RoomDisconnectReason::DuplicateIdentity);
			self->teardownConnection(false);
			if (removed) {
				const QString msg = QCoreApplication::translate(
					"MeetingUI",
					"Room agent role was taken over by the meeting host. Local translation remains active.");
				self->_lastError = msg;
				self->setState(RoomAgentRoleState::Inactive);
				emit self->takeoverRemoved(msg);
			} else if (reason != livekit::RoomDisconnectReason::UserLeave) {
				const QString msg = QCoreApplication::translate(
					"MeetingUI",
					"Room agent connection disconnected (%1).")
					.arg(QString::fromUtf8(livekit::ToString(reason)));
				self->_lastError = msg;
				self->setState(RoomAgentRoleState::Failed);
				emit self->errorOccurred(msg);
			} else {
				self->setState(RoomAgentRoleState::Inactive);
			}
		});
	}

private:
	std::shared_ptr<QtCallbackGate<RoomAgentPublisher>> _gate;
	quint64 _opGen = 0;
};

RoomAgentPublisher::RoomAgentPublisher(
		MeetingCoordinator *meetingCoordinator,
		OpenMeetingHttpClient *httpClient,
		TokenRequester customTokenRequester,
		QObject *parent)
	: QObject(parent)
	, _meetingCoordinator(meetingCoordinator)
	, _httpClient(httpClient ? httpClient : &OpenMeetingHttpClient::instance())
	, _tokenRequester(std::move(customTokenRequester))
	, _callbackGate(QtCallbackGate<RoomAgentPublisher>::Create(this)) {
	_stateResetTimer.setSingleShot(true);
	_stateResetTimer.setInterval(1200);
	connect(&_stateResetTimer, &QTimer::timeout, this, [this] {
		if (_state == RoomAgentRoleState::Active) {
			setAgentActivityState(QStringLiteral("listening"));
		}
	});
}

RoomAgentPublisher::~RoomAgentPublisher() {
	_stateResetTimer.stop();
	_opGeneration.fetch_add(1, std::memory_order_acq_rel);
	teardownConnection(false);
	if (_callbackGate) {
		_callbackGate->Revoke();
	}
}

void RoomAgentPublisher::setState(RoomAgentRoleState next) {
	if (_state == next) {
		return;
	}
	_state = next;
	emit stateChanged(_state);
}

void RoomAgentPublisher::enableAgentRole(const RoomAgentRoleConfig &config, bool forceTakeover) {
	_config = config;
	_lastError.clear();
	_conflictAgent = {};

	if (config.meetingId.trimmed().isEmpty()) {
		_lastError = QCoreApplication::translate("MeetingUI", "No active meeting ID to request an Agent token.");
		setState(RoomAgentRoleState::Failed);
		emit errorOccurred(_lastError);
		return;
	}

	teardownConnection(false);
	const quint64 opGen = _opGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
	setState(RoomAgentRoleState::RequestingToken);

	MeetingAgentTokenRequest req;
	req.meetingId = config.meetingId.trimmed();
	req.role = config.role.trimmed().isEmpty() ? QStringLiteral("translator") : config.role.trimmed();
	req.displayName = config.displayName.trimmed();
	req.sourceLanguage = config.sourceLanguage.trimmed().isEmpty()
		? QStringLiteral("auto") : config.sourceLanguage.trimmed();
	req.targetLanguage = config.targetLanguage.trimmed();
	req.forceTakeover = forceTakeover;

	const auto gate = _callbackGate;
	const bool allowInsecure = config.allowInsecureTransport;
	auto onTokenResult = [gate, opGen, allowInsecure](
			bool ok, const MeetingAgentTokenResponse &resp, const HttpError &err) {
		if (!gate || !gate->active()) {
			return;
		}
		gate->Post([opGen, allowInsecure, ok, resp, err](RoomAgentPublisher *self) {
			if (self->_opGeneration.load(std::memory_order_acquire) != opGen) {
				return;
			}
			if (!ok) {
				const QString msg = err.message.trimmed().isEmpty()
					? QCoreApplication::translate("MeetingUI", "Failed to request room Agent token.")
					: err.message.trimmed();
				self->_lastError = msg;
				self->setState(RoomAgentRoleState::Failed);
				emit self->errorOccurred(msg);
				return;
			}
			if (resp.conflict) {
				self->_conflictAgent = resp.existingAgent;
				self->_lastError = QCoreApplication::translate(
					"MeetingUI", "Another Agent is already active in this meeting room.");
				self->setState(RoomAgentRoleState::ConflictBlocked);
				emit self->conflictDetected(resp.existingAgent);
				return;
			}
			if (resp.liveKit.url.trimmed().isEmpty() || resp.liveKit.token.trimmed().isEmpty()) {
				self->_lastError = QCoreApplication::translate(
					"MeetingUI", "Server returned an incomplete LiveKit Agent credential.");
				self->setState(RoomAgentRoleState::Failed);
				emit self->errorOccurred(self->_lastError);
				return;
			}
			self->startAgentRoomConnection(
				opGen,
				resp.liveKit.url.trimmed(),
				resp.liveKit.token.trimmed(),
				resp.agentIdentity.trimmed(),
				allowInsecure);
		});
	};

	if (_tokenRequester) {
		_tokenRequester(req, std::move(onTokenResult));
	} else if (_httpClient) {
		_httpClient->requestMeetingAgentToken(req, std::move(onTokenResult));
	} else {
		_lastError = QCoreApplication::translate("MeetingUI", "OpenMeeting HTTP client is unavailable.");
		setState(RoomAgentRoleState::Failed);
		emit errorOccurred(_lastError);
	}
}

void RoomAgentPublisher::startAgentRoomConnection(
		quint64 opGen,
		const QString &liveKitUrl,
		const QString &token,
		const QString &agentIdentity,
		bool allowInsecureTransport) {
	if (_opGeneration.load(std::memory_order_acquire) != opGen) {
		return;
	}
	_activeAgentIdentity = agentIdentity;
	_lastPublishedActivityState.clear();
	if (_meetingCoordinator) {
		_meetingCoordinator->setIgnoredTranscriptionSenderIdentity(_activeAgentIdentity);
	}
	setState(RoomAgentRoleState::Connecting);

	auto holder = std::make_shared<AgentConnectionHolder>();
	holder->ioContext = std::make_shared<asio::io_context>();
	holder->workGuard = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(
		asio::make_work_guard(*holder->ioContext));
	holder->room = livekit::Room::Create(holder->ioContext->get_executor(), holder->ioContext);
	holder->listener = std::make_shared<AgentRoomListener>(_callbackGate, opGen);
	holder->room->AddListener(holder->listener);

	auto ioCtx = holder->ioContext;
	holder->ioThread = std::thread([ioCtx]() {
		ioCtx->run();
	});

	_connection = holder;

	livekit::SignalOptions opts;
	opts.auto_subscribe = false;
	opts.auto_subscribe_data_track = false;
	opts.single_peer_connection = true;
	opts.create_webrtc_pc = true;
	opts.eager_publish_data_channel = true;
	opts.allow_insecure_transport = allowInsecureTransport;

	const std::string urlStd = liveKitUrl.toStdString();
	const std::string tokenStd = token.toStdString();
	const auto gate = _callbackGate;
	auto room = holder->room;

	asio::co_spawn(
		*holder->ioContext,
		[room, urlStd, tokenStd, opts, gate, opGen]() -> asio::awaitable<void> {
			bool connected = false;
			std::string safeReason;
			try {
				connected = co_await room->Connect(urlStd, tokenStd, opts);
				if (!connected) {
					safeReason = "LiveKit agent room connection failed";
				}
			} catch (const std::exception &ex) {
				const auto sanitized = livekit::secure_log::SanitizeForOutput(ex.what());
				safeReason = sanitized.empty() ? "LiveKit agent room connection error" : sanitized;
				connected = false;
			} catch (...) {
				safeReason = "Unknown LiveKit agent room connection error";
				connected = false;
			}
			if (!connected && gate && gate->active()) {
				const QString qReason = QString::fromUtf8(safeReason.data(), static_cast<int>(safeReason.size()));
				gate->Post([opGen, qReason](RoomAgentPublisher *self) {
					if (self->_opGeneration.load(std::memory_order_acquire) != opGen) {
						return;
					}
					self->teardownConnection(false);
					self->_lastError = qReason;
					self->setState(RoomAgentRoleState::Failed);
					emit self->errorOccurred(qReason);
				});
			}
			co_return;
		},
		asio::detached);
}

void RoomAgentPublisher::disableAgentRole() {
	if (_state == RoomAgentRoleState::Inactive && !_connection) {
		return;
	}
	_opGeneration.fetch_add(1, std::memory_order_acq_rel);
	teardownConnection(true);
	setState(RoomAgentRoleState::Inactive);
}

void RoomAgentPublisher::teardownConnection(bool notifyDisconnecting) {
	_stateResetTimer.stop();
	if (notifyDisconnecting && _connection) {
		setState(RoomAgentRoleState::Disconnecting);
	}
	if (_meetingCoordinator) {
		_meetingCoordinator->setIgnoredTranscriptionSenderIdentity(QString());
	}
	_activeAgentIdentity.clear();
	_lastPublishedActivityState.clear();

	auto holder = std::move(_connection);
	if (!holder) {
		return;
	}

	SessionShutdownService::Instance().SubmitCleanup([holder = std::move(holder)]() mutable {
		if (holder->room) {
			if (holder->listener) {
				holder->room->RemoveListener(holder->listener);
			}
			holder->room->Retire();
			holder->room.reset();
		}
		if (holder->workGuard) {
			holder->workGuard.reset();
		}
		if (holder->ioThread.joinable()) {
			holder->ioThread.join();
		}
		holder->ioContext.reset();
	});
}

void RoomAgentPublisher::publishTranscript(const MeetingTranscriptItem &item) {
	if (_state != RoomAgentRoleState::Active || !_connection || !_connection->room || !_connection->ioContext) {
		return;
	}
	const QString cleanText = item.text.trimmed();
	if (cleanText.isEmpty()) {
		return;
	}

	std::map<std::string, std::string> attrs;
	if (!item.segmentId.isEmpty()) {
		attrs["lk.segment_id"] = item.segmentId.toStdString();
	}
	if (!item.trackSid.isEmpty()) {
		attrs["lk.transcribed_track_id"] = item.trackSid.toStdString();
	}
	if (!item.speakerIdentity.isEmpty()) {
		attrs["lk.participant_identity"] = item.speakerIdentity.toStdString();
	}
	attrs["lk.transcription_final"] = item.isFinal ? "true" : "false";
	if (!item.language.trimmed().isEmpty()) {
		attrs["lk.language"] = item.language.trimmed().toStdString();
	}
	attrs["cohavora.translated"] = item.isTranslated ? "true" : "false";
	if (item.isTranslated && !item.sourceText.trimmed().isEmpty()) {
		attrs["cohavora.source_text"] = item.sourceText.trimmed().left(512).toStdString();
	}
	if (!item.sourceLanguage.trimmed().isEmpty()) {
		attrs["cohavora.source_language"] = item.sourceLanguage.trimmed().toStdString();
	}

	const std::string utf8Text = cleanText.toUtf8().toStdString();
	auto room = _connection->room;
	asio::post(*_connection->ioContext, [room, utf8Text, attrs = std::move(attrs)]() {
		if (!room || room->retired() || room->connection_state() != livekit::ConnectionState::Connected) {
			return;
		}
		try {
			auto writer = room->CreateTextStreamWriter(
				"lk.transcription",
				attrs,
				"",
				utf8Text.size());
			if (writer) {
				writer->Write(utf8Text);
				writer->Close();
			}
		} catch (...) {
			// Drop transient DataChannel send errors cleanly without crashing media loop.
		}
	});

	setAgentActivityState(QStringLiteral("speaking"));
	_stateResetTimer.start();
}

void RoomAgentPublisher::setAgentActivityState(const QString &state) {
	if (_state != RoomAgentRoleState::Active || !_connection || !_connection->room || !_connection->ioContext) {
		return;
	}
	const QString clean = state.trimmed().toLower();
	if (clean.isEmpty() || clean == _lastPublishedActivityState) {
		return;
	}
	_lastPublishedActivityState = clean;
	const std::string stateStd = clean.toStdString();
	auto room = _connection->room;
	asio::post(*_connection->ioContext, [room, stateStd]() {
		if (!room || room->retired() || room->connection_state() != livekit::ConnectionState::Connected) {
			return;
		}
		if (auto local = room->local_participant()) {
			local->SetAttribute("lk.agent.state", stateStd);
		}
	});
}

} // namespace OpenMeeting
