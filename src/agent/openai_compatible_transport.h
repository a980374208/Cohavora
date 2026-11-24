#pragma once

#include "src/agent/agent_transport.h"

#include <QtNetwork/QNetworkAccessManager>

#include <memory>

namespace OpenMeeting {

class OpenAiCompatibleTransport final : public IAgentTransport {
public:
	OpenAiCompatibleTransport();
	~OpenAiCompatibleTransport() override = default;

	AgentRequestHandle *sendChat(const AgentChatRequest &request, QObject *parent = nullptr) override;
	AgentRequestHandle *probe(
		const AgentProfile &profile,
		const QString &apiKey,
		QObject *parent = nullptr) override;
	AgentModelListHandle *listModels(
		const AgentProfile &profile,
		const QString &apiKey,
		QObject *parent = nullptr) override;

private:
	QNetworkAccessManager &networkManagerFor(bool loopback);

	std::unique_ptr<QNetworkAccessManager> _remoteNam;
	std::unique_ptr<QNetworkAccessManager> _loopbackNam;
};

} // namespace OpenMeeting
