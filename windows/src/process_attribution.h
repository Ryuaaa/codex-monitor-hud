#pragma once

#include "performance_snapshot.h"

#include <cstdint>
#include <optional>
#include <unordered_map>

namespace codex_monitor {

constexpr std::uint64_t kProcessAttributionHundredNanosecondsPerSecond =
    10'000'000ULL;
constexpr std::uint64_t kMaximumTargetIoInterval100ns =
    30ULL * kProcessAttributionHundredNanosecondsPerSecond;

// Tracks only bounded native counter baselines in memory. Target I/O is
// refreshed with the existing five-second sample. Whole-machine CPU ranking
// is refreshed only with the existing twenty-second slow sample.
class ProcessAttributionTracker {
public:
    void Apply(const RawPerformanceSnapshot& raw,
               bool updateWholeMachineCpuRanking,
               PerformanceSnapshot& destination);
    void Reset() noexcept;

private:
    struct CounterBaseline {
        std::uint64_t creationTime100ns = 0;
        std::uint64_t firstCounter = 0;
        std::uint64_t secondCounter = 0;
    };

    std::unordered_map<std::uint32_t, CounterBaseline> previousTargetIo_;
    std::optional<std::uint64_t> previousTargetIoTime100ns_;
    std::unordered_map<std::uint32_t, CounterBaseline> previousSlowCpu_;
    std::optional<CpuTimes> previousSlowSystemCpu_;
};

}  // namespace codex_monitor
