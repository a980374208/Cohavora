#pragma once

#include <windows.h>

#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

namespace livekit {

// DirectShow interfaces stay in one MTA from graph creation through release.
// Callers may wait for a graph operation; UI shutdown therefore retires the
// capture to SessionShutdownService before calling Stop or destroying it.
class DShowGraphOwner final {
public:
    explicit DShowGraphOwner(std::function<void()> finalize = {})
        : finalize_(std::move(finalize)) {
        std::promise<HRESULT> ready;
        auto result = ready.get_future();
        thread_ = std::thread([this, ready = std::move(ready)]() mutable {
            const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            ready.set_value(com);
            for (;;) {
                std::function<void()> job;
                {
                    std::unique_lock lock(mutex_);
                    wake_.wait(lock, [this] { return closing_ || !jobs_.empty(); });
                    if (jobs_.empty()) break;
                    job = std::move(jobs_.front());
                    jobs_.pop_front();
                }
                // packaged_task transports failures back to the waiting caller.
                job();
            }
            if (finalize_) finalize_();
            if (SUCCEEDED(com)) CoUninitialize();
        });
        com_result_ = result.get();
    }

    ~DShowGraphOwner() {
        {
            std::lock_guard lock(mutex_);
            closing_ = true;
        }
        wake_.notify_one();
        if (thread_.get_id() == std::this_thread::get_id()) std::terminate();
        thread_.join();
    }

    DShowGraphOwner(const DShowGraphOwner&) = delete;
    DShowGraphOwner& operator=(const DShowGraphOwner&) = delete;

    HRESULT comResult() const noexcept { return com_result_; }

    template <typename F>
    auto Invoke(F&& callback) -> std::invoke_result_t<F> {
        if (thread_.get_id() == std::this_thread::get_id()) {
            return std::forward<F>(callback)();
        }
        using Result = std::invoke_result_t<F>;
        auto task = std::make_shared<std::packaged_task<Result()>>(
            std::forward<F>(callback));
        auto result = task->get_future();
        {
            std::lock_guard lock(mutex_);
            // Ownership excludes racing destruction with a new operation.
            if (closing_) std::terminate();
            jobs_.push_back([task] { (*task)(); });
        }
        wake_.notify_one();
        return result.get();
    }

private:
    std::function<void()> finalize_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> jobs_;
    bool closing_ = false;
    HRESULT com_result_ = E_UNEXPECTED;
    std::thread thread_;
};

} // namespace livekit
