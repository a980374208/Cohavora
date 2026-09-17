#include "src/ui/camera_switch_completion_owner.h"

#include <QtCore/QMetaObject>
#include <QtCore/QThread>

#include <limits>
#include <mutex>
#include <utility>

namespace MeetingUI {

struct CameraSwitchCompletionOwner::State final {
	enum class Phase {
		NoRequest,
		Awaiting,
		Queued,
		Delivered,
		Closed,
	};

	mutable std::mutex deliveryMutex;
	QObject *receiver = nullptr;
	bool closed = false;
	uint64_t epoch = 0;
	Phase phase = Phase::NoRequest;
	QString requestPath;
	std::shared_ptr<const ResultHandler> handler;
	uint64_t enqueueCount = 0;
	uint64_t deliveryCount = 0;
};

CameraSwitchCompletionOwner::Ticket::Ticket(
		std::weak_ptr<State> state,
		uint64_t epoch)
:	_state(std::move(state)),
	_epoch(epoch) {
}

bool CameraSwitchCompletionOwner::Ticket::isCurrent() const {
	auto state = _state.lock();
	if (!state || !_epoch) {
		return false;
	}
	std::lock_guard<std::mutex> lock(state->deliveryMutex);
	return !state->closed && state->receiver && state->epoch == _epoch;
}

CameraSwitchCompletionOwner::Ticket::operator bool() const {
	return !_state.expired() && _epoch != 0;
}

CameraSwitchCompletionOwner::CameraSwitchCompletionOwner(
		QObject *receiver,
		ResultHandler handler)
:	_state(std::make_shared<State>()) {
	Q_ASSERT(receiver != nullptr);
	Q_ASSERT(receiver->thread() == QThread::currentThread());
	_state->receiver = receiver;
	_state->handler = std::make_shared<const ResultHandler>(std::move(handler));
}

CameraSwitchCompletionOwner::~CameraSwitchCompletionOwner() {
	invalidate();
}

CameraSwitchCompletionOwner::Ticket CameraSwitchCompletionOwner::beginRequest(
		const QString &requestPath) {
	Q_ASSERT(_state->receiver == nullptr
		|| _state->receiver->thread() == QThread::currentThread());
	std::lock_guard<std::mutex> lock(_state->deliveryMutex);
	if (_state->closed || !_state->receiver
		|| _state->epoch == std::numeric_limits<uint64_t>::max()) {
		return {};
	}
	++_state->epoch;
	_state->phase = State::Phase::Awaiting;
	_state->requestPath = requestPath;
	return Ticket(_state, _state->epoch);
}

CameraSwitchCompletionOwner::NativeCallback
CameraSwitchCompletionOwner::makeCallback(const Ticket &ticket) const {
	const auto weakState = ticket._state;
	const auto epoch = ticket._epoch;
	return [weakState, epoch](bool success, const std::string &error) {
		deliver(weakState, epoch, success, std::string(error));
	};
}

void CameraSwitchCompletionOwner::invalidate() {
	if (!_state) {
		return;
	}
	Q_ASSERT(_state->receiver == nullptr
		|| _state->receiver->thread() == QThread::currentThread());
	std::shared_ptr<const ResultHandler> releasedHandler;
	QString releasedPath;
	{
		std::lock_guard<std::mutex> lock(_state->deliveryMutex);
		if (_state->closed) {
			return;
		}
		_state->closed = true;
		_state->receiver = nullptr;
		if (_state->epoch != std::numeric_limits<uint64_t>::max()) {
			++_state->epoch;
		}
		_state->phase = State::Phase::Closed;
		releasedHandler = std::move(_state->handler);
		releasedPath = std::move(_state->requestPath);
	}
}

void CameraSwitchCompletionOwner::deliver(
		const std::weak_ptr<State> &weakState,
		uint64_t epoch,
		bool success,
		std::string error) {
	auto state = weakState.lock();
	if (!state || !epoch) {
		return;
	}

	std::unique_lock<std::mutex> lock(state->deliveryMutex);
	if (state->closed || !state->receiver || state->epoch != epoch
		|| state->phase != State::Phase::Awaiting) {
		return;
	}
	state->phase = State::Phase::Queued;
	const auto enqueued = QMetaObject::invokeMethod(
		state->receiver,
		[weakState, epoch, success, error = std::move(error)]() mutable {
			auto queuedState = weakState.lock();
			if (!queuedState) {
				return;
			}

			std::shared_ptr<const ResultHandler> handler;
			QString requestPath;
			{
				std::lock_guard<std::mutex> queuedLock(queuedState->deliveryMutex);
				if (queuedState->closed || !queuedState->receiver
					|| queuedState->epoch != epoch
					|| queuedState->phase != State::Phase::Queued) {
					return;
				}
				queuedState->phase = State::Phase::Delivered;
				++queuedState->deliveryCount;
				handler = queuedState->handler;
				requestPath = queuedState->requestPath;
			}

			if (handler && *handler) {
				(*handler)(Ticket(weakState, epoch), requestPath, success, error);
			}
		},
		Qt::QueuedConnection);
	if (enqueued) {
		++state->enqueueCount;
	} else {
		state->phase = State::Phase::Delivered;
	}
}

bool CameraSwitchCompletionOwner::closedForTest() const {
	std::lock_guard<std::mutex> lock(_state->deliveryMutex);
	return _state->closed;
}

uint64_t CameraSwitchCompletionOwner::enqueueCountForTest() const {
	std::lock_guard<std::mutex> lock(_state->deliveryMutex);
	return _state->enqueueCount;
}

uint64_t CameraSwitchCompletionOwner::deliveryCountForTest() const {
	std::lock_guard<std::mutex> lock(_state->deliveryMutex);
	return _state->deliveryCount;
}

} // namespace MeetingUI
