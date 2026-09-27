#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace livekit::telemetry {

template <typename Signature, std::size_t Capacity = 256>
class BoundedCallback;

template <typename... Args, std::size_t Capacity>
class BoundedCallback<void(Args...), Capacity> final {
public:
    BoundedCallback() = default;
    BoundedCallback(std::nullptr_t) noexcept {}

    template <typename F>
        requires (!std::is_same_v<std::decay_t<F>, BoundedCallback> &&
                  std::is_invocable_r_v<void, std::decay_t<F>&, Args...>)
    BoundedCallback(F&& callback) {
        using Target = std::decay_t<F>;
        static_assert(sizeof(Target) <= Capacity,
            "callback captures exceed the telemetry queue budget");
        static_assert(alignof(Target) <= alignof(std::max_align_t));
        static_assert(std::is_copy_constructible_v<Target>);
        static_assert(!std::is_same_v<Target, std::function<void(Args...)>>,
            "std::function can hide unbounded callback allocation");
        std::construct_at(reinterpret_cast<Target*>(&storage_),
            std::forward<F>(callback));
        invoke_ = [](void* storage, Args... args) {
            (*reinterpret_cast<Target*>(storage))(
                std::forward<Args>(args)...);
        };
        copy_ = [](void* destination, const void* source) {
            std::construct_at(reinterpret_cast<Target*>(destination),
                *reinterpret_cast<const Target*>(source));
        };
        move_ = [](void* destination, void* source) {
            std::construct_at(reinterpret_cast<Target*>(destination),
                std::move(*reinterpret_cast<Target*>(source)));
            std::destroy_at(reinterpret_cast<Target*>(source));
        };
        destroy_ = [](void* storage) {
            std::destroy_at(reinterpret_cast<Target*>(storage));
        };
    }

    BoundedCallback(const BoundedCallback& other) {
        CopyFrom(other);
    }
    BoundedCallback(BoundedCallback&& other) {
        MoveFrom(other);
    }
    BoundedCallback& operator=(const BoundedCallback& other) {
        if (this != &other) {
            Reset();
            CopyFrom(other);
        }
        return *this;
    }
    BoundedCallback& operator=(BoundedCallback&& other) {
        if (this != &other) {
            Reset();
            MoveFrom(other);
        }
        return *this;
    }
    ~BoundedCallback() { Reset(); }

    explicit operator bool() const noexcept { return invoke_ != nullptr; }
    void operator()(Args... args) {
        invoke_(&storage_, std::forward<Args>(args)...);
    }

private:
    void Reset() noexcept {
        if (destroy_) destroy_(&storage_);
        invoke_ = nullptr;
        copy_ = nullptr;
        move_ = nullptr;
        destroy_ = nullptr;
    }
    void CopyFrom(const BoundedCallback& other) {
        if (!other) return;
        other.copy_(&storage_, &other.storage_);
        invoke_ = other.invoke_;
        copy_ = other.copy_;
        move_ = other.move_;
        destroy_ = other.destroy_;
    }
    void MoveFrom(BoundedCallback& other) {
        if (!other) return;
        other.move_(&storage_, &other.storage_);
        invoke_ = other.invoke_;
        copy_ = other.copy_;
        move_ = other.move_;
        destroy_ = other.destroy_;
        other.invoke_ = nullptr;
        other.copy_ = nullptr;
        other.move_ = nullptr;
        other.destroy_ = nullptr;
    }

    std::aligned_storage_t<Capacity, alignof(std::max_align_t)> storage_{};
    void (*invoke_)(void*, Args...) = nullptr;
    void (*copy_)(void*, const void*) = nullptr;
    void (*move_)(void*, void*) = nullptr;
    void (*destroy_)(void*) = nullptr;
};

} // namespace livekit::telemetry
