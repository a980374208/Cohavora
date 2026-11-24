#pragma once

#include "src/core/meeting_coordinator.h"
#include "src/core/room.h"
#include "src/core/session_shutdown_service.h"
#include "src/net/meeting_types.h"

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QTimer>

#include <asio.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>

namespace OpenMeeting {

class OpenMeetingHttpClient;

enum class RoomAgentRoleState {
	Inactive = 0,
	RequestingToken = 1,
	Connecting = 2,
	Active = 3,
	Reconnecting = 4,
	ConflictBlocked = 5,
	Disconnecting = 6,
	Failed = 7,
};

struct RoomAgentRoleConfig {
	QString meetingId;
	QString role = QStringLiteral("translator");
	QString displayName;
	QString sourceLanguage = QStringLiteral("auto");
	QString targetLanguage;
	bool allowInsecureTransport = false;
};

// Manages the secondary lightweight data-only LiveKit Room connection when
// the local client acts as the room's transcription/translation Agent.
// Can be enabled/disabled on the fly mid-meeting without interrupting media.
class RoomAgentPublisher final : public QObject {
	Q_OBJECT
public:
	using TokenRequester = std::function<void(
		const MeetingAgentTokenRequest &request,
		ResultCallback<MeetingAgentTokenResponse> callback)>;

	explicit RoomAgentPublisher(
		MeetingCoordinator *meetingCoordinator,
		OpenMeetingHttpClient *httpClient = nullptr,
		TokenRequester customTokenRequester = {},
		QObject *parent = nullptr);
	~RoomAgentPublisher() override;

	void enableAgentRole(const RoomAgentRoleConfig &config, bool forceTakeover = false);
	void disableAgentRole();

	void publishTranscript(const MeetingTranscriptItem &item);
	void setAgentActivityState(const QString &state);

	[[nodiscard]] RoomAgentRoleState state() const { return _state; }
	[[nodiscard]] bool isActive() const { return _state == RoomAgentRoleState::Active; }
	[[nodiscard]] bool isBusyOrActive() const {
		return _state == RoomAgentRoleState::RequestingToken ||
			_state == RoomAgentRoleState::Connecting ||
			_state == RoomAgentRoleState::Active ||
			_state == RoomAgentRoleState::Reconnecting;
	}
	[[nodiscard]] QString activeAgentIdentity() const { return _activeAgentIdentity; }
	[[nodiscard]] const ExistingRoomAgentInfo &conflictAgent() const { return _conflictAgent; }
	[[nodiscard]] QString lastError() const { return _lastError; }

signals:
	void stateChanged(OpenMeeting::RoomAgentRoleState state);
	void conflictDetected(const OpenMeeting::ExistingRoomAgentInfo &existingAgent);
	void takeoverRemoved(const QString &reason);
	void errorOccurred(const QString &safeError);

private:
	struct AgentConnectionHolder;
	class AgentRoomListener;

	void setState(RoomAgentRoleState next);
	void startAgentRoomConnection(
		quint64 opGen,
		const QString &liveKitUrl,
		const QString &token,
		const QString &agentIdentity,
		bool allowInsecureTransport);
	void teardownConnection(bool notifyDisconnecting);

	QPointer<MeetingCoordinator> _meetingCoordinator;
	QPointer<OpenMeetingHttpClient> _httpClient;
	TokenRequester _tokenRequester;
	std::shared_ptr<QtCallbackGate<RoomAgentPublisher>> _callbackGate;

	RoomAgentRoleState _state = RoomAgentRoleState::Inactive;
	RoomAgentRoleConfig _config;
	QString _activeAgentIdentity;
	QString _lastPublishedActivityState;
	ExistingRoomAgentInfo _conflictAgent;
	QString _lastError;
	std::atomic<quint64> _opGeneration{0};

	std::shared_ptr<AgentConnectionHolder> _connection;
	QTimer _stateResetTimer;
};

} // namespace OpenMeeting

Q_DECLARE_METATYPE(OpenMeeting::RoomAgentRoleState)
