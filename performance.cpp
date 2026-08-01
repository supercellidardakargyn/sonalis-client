#include "performance.h"

#include <algorithm>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>

namespace ss {
namespace {

constexpr std::size_t metricCount = static_cast<std::size_t>(PerformanceMetric::Count);
constexpr std::size_t ringSize = 256;
std::array<std::array<std::atomic<std::uint32_t>, ringSize>, metricCount> rings{};
std::array<std::atomic<std::uint32_t>, metricCount> positions{};

}  // namespace

void RecordPerformance(const PerformanceMetric metric, const std::uint32_t value) noexcept {
    const std::size_t index = static_cast<std::size_t>(metric);
    if (index >= metricCount) return;
    const std::uint32_t position = positions[index].fetch_add(1, std::memory_order_relaxed);
    rings[index][position % ringSize].store(value, std::memory_order_relaxed);
}

PerformanceSnapshot ReadPerformanceSnapshot() noexcept {
    PerformanceSnapshot snapshot;
    for (std::size_t metric = 0; metric < metricCount; ++metric) {
        const std::uint32_t position = positions[metric].load(std::memory_order_relaxed);
        if (position == 0) continue;
        snapshot.latest[metric] = rings[metric][(position - 1) % ringSize].load(std::memory_order_relaxed);
        for (const auto& value : rings[metric]) snapshot.maximum[metric] = std::max(snapshot.maximum[metric], value.load(std::memory_order_relaxed));
    }
    return snapshot;
}

ProcessMemorySnapshot ReadProcessMemorySnapshot() noexcept {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters)) == FALSE) {
        return {};
    }
    return {
        static_cast<std::uint64_t>(counters.WorkingSetSize),
        static_cast<std::uint64_t>(counters.PrivateUsage),
        true,
    };
}

}  // namespace ss
