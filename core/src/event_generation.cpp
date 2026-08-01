#include "sonalis/core/event_generation.h"

namespace sonalis::core {

std::uint64_t EventGeneration::MarkDirty() noexcept {
    return generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
}

bool EventGeneration::TryBegin(std::uint64_t& token) noexcept {
    token = 0;
    if (inFlight_.load(std::memory_order_acquire) != 0) return false;
    const auto current = generation_.load(std::memory_order_acquire);
    if (current == 0 || current == completed_.load(std::memory_order_acquire)) return false;
    std::uint64_t expected = 0;
    if (!inFlight_.compare_exchange_strong(expected, current, std::memory_order_acq_rel)) return false;
    token = current;
    return true;
}

bool EventGeneration::Complete(const std::uint64_t token) noexcept {
    if (token == 0) return false;
    std::uint64_t expected = token;
    if (!inFlight_.compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) return false;
    completed_.store(token, std::memory_order_release);
    return generation_.load(std::memory_order_acquire) != token;
}

void EventGeneration::Reset() noexcept {
    inFlight_.store(0, std::memory_order_release);
    completed_.store(0, std::memory_order_release);
    generation_.store(0, std::memory_order_release);
}

std::uint64_t EventGeneration::Current() const noexcept {
    return generation_.load(std::memory_order_acquire);
}

bool EventGeneration::InFlight() const noexcept {
    return inFlight_.load(std::memory_order_acquire) != 0;
}

}  // namespace sonalis::core
