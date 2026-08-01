#pragma once

#include <atomic>
#include <cstdint>

namespace sonalis::core {

// Coalesces bursts without losing an event that arrives while a refresh is in
// flight. Network callbacks call MarkDirty; one platform worker owns TryBegin
// and Complete. No callback allocation or unbounded queue is required.
class EventGeneration final {
public:
    [[nodiscard]] std::uint64_t MarkDirty() noexcept;
    [[nodiscard]] bool TryBegin(std::uint64_t& token) noexcept;
    [[nodiscard]] bool Complete(std::uint64_t token) noexcept;
    void Reset() noexcept;
    [[nodiscard]] std::uint64_t Current() const noexcept;
    [[nodiscard]] bool InFlight() const noexcept;

private:
    std::atomic<std::uint64_t> generation_{};
    std::atomic<std::uint64_t> completed_{};
    std::atomic<std::uint64_t> inFlight_{};
};

}  // namespace sonalis::core
