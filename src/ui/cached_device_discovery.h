#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace MeetingUI {

// One native scan in flight; weak subscriptions do not retain closed windows.
// Delivery may run on a worker and must enter Qt through a callback gate.
template <typename DeviceList>
class CachedDeviceDiscovery {
public:
	using Clock = std::chrono::steady_clock;
	using Devices = DeviceList;
	struct Result {
		std::shared_ptr<const Devices> devices;
		bool succeeded = false;
		bool cacheHit = false;
		bool cancelled = false;
	};
	using Completion = std::function<void(const Result &)>;
	using Enumerate = std::function<Devices()>;
	using Submit = std::function<bool(std::function<void()>)>;
	using Now = std::function<Clock::time_point()>;
	using Report = std::function<void(const Result &, std::uint64_t)>;
	struct Subscription { Completion completed; };
	using Request = std::shared_ptr<Subscription>;

	CachedDeviceDiscovery(Enumerate enumerate, Submit submit, Now now = Clock::now,
			Report report = {}) : _state(std::make_shared<State>()) {
		_state->enumerate = std::move(enumerate);
		_state->submit = std::move(submit);
		_state->now = std::move(now);
		_state->report = std::move(report);
	}

	Request request(Completion completed, bool forceRefresh = false) {
		auto subscription = std::make_shared<Subscription>(Subscription{std::move(completed)});
		const auto state = _state;
		Result cached;
		{
			std::lock_guard lock(state->mutex);
			if (!forceRefresh && !state->scanning && state->snapshot
					&& state->now() - state->capturedAt < std::chrono::seconds(30)) {
				cached = {state->snapshot, true, true};
			} else {
				std::erase_if(state->subscribers, [](const auto &weak) { return weak.expired(); });
				state->subscribers.push_back(subscription);
				if (state->scanning) return subscription;
				state->scanning = true;
				state->snapshot.reset();
			}
		}
		if (cached.succeeded) {
			if (state->report) state->report(cached, 0);
			subscription->completed(cached);
			return subscription;
		}
		bool accepted = false;
		try {
			accepted = state->submit([state] {
				const auto started = Clock::now();
				Result result;
				try {
					result.devices = std::make_shared<const Devices>(state->enumerate());
					result.succeeded = true;
				} catch (...) {
					// A failure is distinct from an empty successful snapshot.
				}
				if (state->report) state->report(result,
					std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count());
				state->finish(result);
			});
		} catch (...) {
			// Executor rejection must release every subscriber and allow retry.
		}
		if (!accepted) {
			Result cancelled;
			cancelled.cancelled = true;
			if (state->report) state->report(cancelled, 0);
			state->finish(cancelled);
		}
		return subscription;
	}

private:
	struct State {
		Enumerate enumerate;
		Submit submit;
		Now now;
		Report report;
		std::mutex mutex;
		bool scanning = false;
		std::shared_ptr<const Devices> snapshot;
		Clock::time_point capturedAt{};
		std::vector<std::weak_ptr<Subscription>> subscribers;

		void finish(const Result &result) {
			std::vector<std::weak_ptr<Subscription>> pending;
			{
				std::lock_guard lock(mutex);
				scanning = false;
				if (result.succeeded) {
					snapshot = result.devices;
					capturedAt = now();
				}
				pending.swap(subscribers);
			}
			for (const auto &weak : pending) {
				if (const auto subscription = weak.lock()) subscription->completed(result);
			}
		}
	};
	std::shared_ptr<State> _state;
};

} // namespace MeetingUI
