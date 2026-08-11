#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace codex_monitor {

constexpr std::uint64_t kPerformanceTrendHundredNanosecondsPerSecond =
    10'000'000ULL;
constexpr std::uint64_t kPerformanceTrendWindow100ns =
    600ULL * kPerformanceTrendHundredNanosecondsPerSecond;
constexpr std::uint64_t kPerformanceTrendMaximumGap100ns =
    30ULL * kPerformanceTrendHundredNanosecondsPerSecond;
constexpr std::size_t kPerformanceTrendBucketCount = 18;

struct PerformanceTrendPoint {
    std::uint64_t capturedAtUnbiased100ns = 0;
    std::optional<double> systemCpuPercent;
};

struct PerformanceTrendSummary {
    std::uint64_t coverage100ns = 0;
    std::size_t sampleCount = 0;
    std::size_t validSampleCount = 0;
    std::optional<double> averagePercent;
    std::optional<double> peakPercent;
    std::vector<std::optional<double>> buckets;
};

// Keeps only a short in-memory trend. Callers supply the Windows unbiased
// interrupt time so wall-clock changes and time spent asleep cannot fabricate
// CPU history.
class PerformanceTrend {
public:
    void Add(std::uint64_t capturedAtUnbiased100ns,
             std::optional<double> systemCpuPercent);
    void Reset() noexcept;

    [[nodiscard]] PerformanceTrendSummary Summarize() const;
    [[nodiscard]] bool empty() const noexcept { return points_.empty(); }

private:
    std::vector<PerformanceTrendPoint> points_;
};

}  // namespace codex_monitor
