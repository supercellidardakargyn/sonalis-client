#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <utility>

namespace sonalis::core {

template <typename T, std::size_t Capacity>
class BoundedSpscQueue final {
    static_assert(Capacity >= 2);
public:
    bool TryPush(T value) noexcept {
        const auto write = write_.load(std::memory_order_relaxed);
        const auto next = (write + 1) % Capacity;
        if (next == read_.load(std::memory_order_acquire)) return false;
        slots_[write].emplace(std::move(value));
        write_.store(next, std::memory_order_release);
        return true;
    }

    std::optional<T> TryPop() noexcept {
        const auto read = read_.load(std::memory_order_relaxed);
        if (read == write_.load(std::memory_order_acquire)) return std::nullopt;
        std::optional<T> value(std::move(slots_[read]));
        slots_[read].reset();
        read_.store((read + 1) % Capacity, std::memory_order_release);
        return value;
    }

private:
    std::array<std::optional<T>, Capacity> slots_{};
    alignas(64) std::atomic<std::size_t> write_{0};
    alignas(64) std::atomic<std::size_t> read_{0};
};

}  // namespace sonalis::core

