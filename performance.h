#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ss {

enum class PerformanceMetric : std::size_t {
    UiFrame,
    OpusEncode,
    RnNoise,
    WorkerQueue,
    WebSocketReconnect,
    Count,
};

struct PerformanceSnapshot {
    std::array<std::uint32_t, static_cast<std::size_t>(PerformanceMetric::Count)> latest{};
    std::array<std::uint32_t, static_cast<std::size_t>(PerformanceMetric::Count)> maximum{};
};

struct ProcessMemorySnapshot {
    std::uint64_t workingSetBytes{};
    std::uint64_t privateBytes{};
    bool available{};
};

void RecordPerformance(PerformanceMetric metric, std::uint32_t value) noexcept;
[[nodiscard]] PerformanceSnapshot ReadPerformanceSnapshot() noexcept;
[[nodiscard]] ProcessMemorySnapshot ReadProcessMemorySnapshot() noexcept;

}  // namespace ss
