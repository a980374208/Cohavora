#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class CameraOwnerTestAccess;

namespace MeetingUI {

class CameraSwitchCompletionOwner final {
private:
	struct State;

public:
	class Ticket final {
	public:
		Ticket() = default;
		bool isCurrent() const;
		explicit operator bool() const;

	private:
		friend class CameraSwitchCompletionOwner;
		friend class ::CameraOwnerTestAccess;
		Ticket(std::weak_ptr<State> state, uint64_t epoch);

		std::weak_ptr<State> _state;
		uint64_t _epoch = 0;
	};

	using ResultHandler = std::function<void(
		const Ticket &ticket,
		const QString &requestPath,
		bool success,
		const std::string &error)>;
	using NativeCallback = std::function<void(bool success, const std::string &error)>;

	CameraSwitchCompletionOwner(QObject *receiver, ResultHandler handler);
	~CameraSwitchCompletionOwner();

	CameraSwitchCompletionOwner(const CameraSwitchCompletionOwner &) = delete;
	CameraSwitchCompletionOwner &operator=(const CameraSwitchCompletionOwner &) = delete;

	Ticket beginRequest(const QString &requestPath);
	NativeCallback makeCallback(const Ticket &ticket) const;
	void invalidate();

private:
	friend class ::CameraOwnerTestAccess;
	static void deliver(
		const std::weak_ptr<State> &weakState,
		uint64_t epoch,
		bool success,
		std::string error);

	bool closedForTest() const;
	uint64_t enqueueCountForTest() const;
	uint64_t deliveryCountForTest() const;

	std::shared_ptr<State> _state;
};

} // namespace MeetingUI
